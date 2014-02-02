#include <SimpleTimer.h>
#include <IRremote.h>

#include <SPI.h>
#include <Ethernet.h>

#include <CameraC328R.h>
#include <SoftwareSerial.h>

//#define DEBUG_MODE TRUE

#define USB_BAUD 115200
#define CAMERA_BAUD 14400

// HTTP server
#define COMMAND_TYPES 4
#define COMMAND_LENGTH 11

#define COMMAND_TAKE_PICTURE 0
#define COMMAND_SEND_IR_SIGNAL 1
#define COMMAND_GET_DISTANCE 2
#define COMMAND_SENSE_PROXIMITY 3

// IR remote
#define LED_PIN 3
#define IR_SIGNAL_TYPES 2
#define IR_SIGNAL_LENGTH 19

#define IR_SIGNAL_LIGHT_OFF 0
#define IR_SIGNAL_LIGHT_ON 1

// Distance threshold for turning on the light
#define DISTANCE_THRESHOLD 160

// Lighting duration after the distance exceeds the threshold
#define LIGHTING_DURATION 10000

// C328 JPEG camera
SoftwareSerial mySerial(4, 5);
CameraC328R camera(&mySerial);
uint16_t pictureSizeCount = 0;

// Ethernet shield
byte mac[] = { 
  0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip( 192, 168, 10, 7 );
EthernetServer server(80);
EthernetClient client;

// HTTP commands
char commands[COMMAND_TYPES][COMMAND_LENGTH] = {
  { 't', 'a', 'k', 'e', 'p', 'i', 'c', 't', 'u', 'r', 'e' },
  { 's', 'e', 'n', 'd', 'i', 'r', 's', 'i', 'g', 'n', 'l' },
  { 'g', 'e', 't', 'd', 'i', 's', 't', 'a', 'n', 'c', 'e' },
  { 's', 'e', 'n', 's', 'e', 'p', 'r', 'x', 'm', 't', 'y' } };

// IR signal data
IRsend irsend;
unsigned int irSignals[IR_SIGNAL_TYPES][IR_SIGNAL_LENGTH] = {
  /* OFF */ {2688, 2608,  900, 736,  948, 1828,  900, 736,  952, 736,  948, 1832,  892, 740,  948, 1828,  900, 1828,  896},
  /* O N */ {2688, 2584,  900, 760,  948, 740,  944, 1804,  904, 1824,  900, 1828,  896, 1824,  904, 760,  956, 732,  956} };

// Auto turn-on and delayed turn-off of the lighting
boolean senseProximity = true;
SimpleTimer timer;
int distance;
int timerId = -1;

void setup()
{
  #ifdef DEBUG_MODE
  Serial.begin( USB_BAUD );
  #endif

  // Start the Ethernet connection and the server
  Ethernet.begin( mac, ip );
  server.begin();
  #ifdef DEBUG_MODE
  Serial.print( "server is at " );
  Serial.println( Ethernet.localIP() );
  #endif

  // Disconnect the camera
  endSerial();

  // Turn off the light
  sendIRSignal( IR_SIGNAL_LIGHT_OFF );
}

void loop()
{
  client = server.available();
  if ( client ) {
    handleClientRequest();
  }

  getDistance();
  if ( senseProximity && distance > DISTANCE_THRESHOLD ) {
    turnOnLighting();
  }

  timer.run();
}

void getDistance() {
  int currentDistance = analogRead( 0 );
  distance = (distance * 9 + currentDistance) / 10;
}

void turnOnLighting() {

  // Skip if the light is already turned on
  if (timerId >= 0) {
    return;
  }

  #ifdef DEBUG_MODE
  Serial.print( "light turned on: " );
  Serial.println( distance );
  #endif

  // Turn on the light
  sendIRSignal( IR_SIGNAL_LIGHT_ON );

  // Set timer for turning off the light
  timerId = timer.setTimeout( LIGHTING_DURATION, turnOffLighting );
}

void turnOffLighting() {

  // Turn off the light
  if ( distance <= DISTANCE_THRESHOLD ) {
    sendIRSignal( IR_SIGNAL_LIGHT_OFF );
  }

  // Disable the timer
  if (timerId >= 0) {
    // timer.deleteTimer(timerId);
    timerId = -1;
  }

  // Re-enable the timer
  if ( distance > DISTANCE_THRESHOLD ) {
    turnOnLighting();
  }
}

void handleClientRequest() {
  #ifdef DEBUG_MODE
  Serial.println( "new client" );
  Serial.println( "---" );
  #endif

  // An http request ends with a blank line
  boolean currentLineIsBlank = true;

  // Clear flags for command string detection
  int commandIndex[COMMAND_TYPES];
  for (int i = 0; i < COMMAND_TYPES; i ++) {
    commandIndex[i] = 0;
  }
  int parameter = -1;

  // Read the request
  while ( client.connected() ) {
    if ( client.available() ) {
      char c = client.read();
      #ifdef DEBUG_MODE
      Serial.write( c );
      #endif

      // Check command string
      for (int i = 0; i < COMMAND_TYPES; i ++) {
        if ( commandIndex[i] == COMMAND_LENGTH) {
          if ( parameter < 0 ) {
            parameter = (int) (c - '0');
          }
        } else if ( c == commands[i][commandIndex[i]] ) {
          commandIndex[i] ++;
        } else {
          commandIndex[i] = 0;
        }
      }

      // if you've gotten to the end of the line (received a newline
      // character) and the line is blank, the http request has ended,
      // so you can send a reply
      if ( c == '\n' && currentLineIsBlank ) {
        break;
      }

      if ( c == '\n' ) {
        // you're starting a new line
        currentLineIsBlank = true;
      } 
      else if ( c != '\r' ) {
        // you've gotten a character on the current line
        currentLineIsBlank = false;
      }
    }
  }

  #ifdef DEBUG_MODE
  Serial.println( "---" );
  #endif

  // Take a picture?
  if ( commandIndex[COMMAND_TAKE_PICTURE] == COMMAND_LENGTH ) {
    getJPEG();

  // Send IR signal?
  } else if ( commandIndex[COMMAND_SEND_IR_SIGNAL] == COMMAND_LENGTH ) {
    if (sendIRSignal(parameter)) {
      client.println( "HTTP/1.1 200 OK" );
      client.println( "Content-Type: text/plain" );
      client.println( "Connnection: close" );
      client.println();
      client.print( "IR Command: OK (" );
      client.print( parameter );
      client.println( ")" );
    } else {
      sendNotFoundHeader();
      client.print( "IR Command: Failed (" );
      client.print( parameter );
      client.println( ")" );
    }

  // Get the distance?
  } else if ( commandIndex[COMMAND_GET_DISTANCE] == COMMAND_LENGTH ) {
    client.println( "HTTP/1.1 200 OK" );
    client.println( "Content-Type: text/plain" );
    client.println( "Connnection: close" );
    client.println();
    client.print( "Distance sensor: " );
    client.print(distance);

  // Sense proximity?
  } else if ( commandIndex[COMMAND_SENSE_PROXIMITY] == COMMAND_LENGTH ) {
    client.println( "HTTP/1.1 200 OK" );
    client.println( "Content-Type: text/plain" );
    client.println( "Connnection: close" );
    client.println();
    client.print( "Sense proximity: " );
    senseProximity = parameter != 0;
    if (senseProximity) {
      client.println( "yes" );
    } else {
      client.println( "no" );
    }

  // Not supported
  } else {
    sendNotFoundHeader();
    client.println( "Requested resource was not found." );
  }

  client.flush();
  delay( 1 );
  client.stop();

  #ifdef DEBUG_MODE
  Serial.println( "client disonnected" );
  #endif
}

boolean sendIRSignal(int parameter)
{
  #ifdef DEBUG_MODE
  Serial.print("IR command parameter: ");
  Serial.println(parameter);
  #endif
  if (parameter >= 0 && parameter < IR_SIGNAL_TYPES) {

    // Send 5 times to make sure :)
    for ( int i = 0; i < 5; i ++ ) {
      irsend.sendRaw(irSignals[parameter], IR_SIGNAL_LENGTH, 38);
    }
    return true;
  }
  return false;
}

void sendNotFoundHeader() {
  client.println( "HTTP/1.1 404 Not Found" );
  client.println( "Content-Type: text/plain" );
  client.println( "Connnection: close" );
  client.println();
}

void sendErrorHeader() {
  client.println( "HTTP/1.1 500 Internal Server Error" );
  client.println( "Content-Type: text/plain" );
  client.println( "Connnection: close" );
  client.println();
}

void beginSerial() {
  mySerial.begin( CAMERA_BAUD );
}

void endSerial() {
  mySerial.end();
  pinMode( 2, INPUT );
  pinMode( 3, OUTPUT );
  digitalWrite( 3, LOW );
}

void getJPEG()
{
  beginSerial();

  if( !camera.sync() )
  {
    #ifdef DEBUG_MODE
    Serial.println( "Sync failed." );
    #endif
    sendErrorHeader();
    client.println( "Camera: Sync failed." );
    endSerial();
    return;
  }

  if( !camera.initial( CameraC328R::CT_JPEG, CameraC328R::PR_160x120, CameraC328R::JR_640x480 ) )
  {
    #ifdef DEBUG_MODE
    Serial.println( "Initial failed." );
    #endif
    sendErrorHeader();
    client.println( "Camera: Initialization failed." );
    endSerial();
    turnOffLighting();
    return;
  }

  if( !camera.setPackageSize( 64 ) )
  {
    #ifdef DEBUG_MODE
    Serial.println( "Package size failed." );
    #endif
    sendErrorHeader();
    client.println( "Camera: Package size couldn't be configured." );
    endSerial();
    turnOffLighting();
    return;
  }

  if( !camera.setLightFrequency( CameraC328R::FT_50Hz ) )
  {
    #ifdef DEBUG_MODE
    Serial.println( "Light frequency failed." );
    #endif
    sendErrorHeader();
    client.println( "Camera: Light frequency couldn't be configured." );
    endSerial();
    turnOffLighting();
    return;
  }

  client.print( "HTTP/1.1 " );
  client.flush();

  if( !camera.snapshot( CameraC328R::ST_COMPRESSED, 0 ) )
  {
    #ifdef DEBUG_MODE
    Serial.println( "Snapshot failed." );
    #endif
    client.println( "500 Internal Server Error" );
    client.println( "Content-Type: text/plain" );
    client.println( "Connnection: close" );
    client.println();
    client.println( "Camera: snapshot failed." );
    endSerial();
    turnOffLighting();
    return;
  }

  client.println( "200 OK" );
  client.println( "Content-Type: image/jpeg" );
  client.println( "Connnection: close" );
  client.println();

  pictureSizeCount = 0;
  if( !camera.getJPEGPicture( CameraC328R::PT_JPEG, PROCESS_DELAY, &getJPEGPicture_callback ) )
  {
    #ifdef DEBUG_MODE
    Serial.println( "Get JPEG failed." );
    #endif
  }
  
  camera.powerOff();
  endSerial();
}

/**
 * This callback is called EVERY time a JPEG data packet is received.
 */
void getJPEGPicture_callback( uint16_t pictureSize, uint16_t packageSize, uint16_t packageCount, byte* package )
{
  // Update values from the distance sensor.
  getDistance();

  // Send data.
  pictureSizeCount += packageSize;
  client.write( package, packageSize );

  // Complete data transmission.
  if( pictureSizeCount >= pictureSize )
  { 
    #ifdef DEBUG_MODE
    Serial.println( "Get JPEG finished." );
    Serial.flush();
    #endif

    client.flush();
 }
}
