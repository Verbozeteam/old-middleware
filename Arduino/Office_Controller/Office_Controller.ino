#include <SPI.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <TimerOne.h>

/*
curtains   : 22+ (UP, DOWN, UP, DOWN, ...)
onoff      : 37-
fans       : 48-49
ACs        : 50-51
temp sensor: 53
dimmers    : 4-7 (analog PWM output)
central ACs: 8-9 (analog PWM output)
SYNC       : digital 3
hotel card : 44 (input 0v/5v)
hotel power: 42 (output 0v/5v)
*/

#define USE_ISR_DIMMERS

//changable
const int NUM_DIMMERS = 4;
const int NUM_LIGHTS = 10;
const int NUM_CURTAINS = 4;
// shouldn't change
const int NUM_ACS = 2;

// analog
const int BASE_DIMMER_PORT = 4;
const int BASE_CENTRAL_AC_PORT = 8;
// digital
const int BASE_CURTAIN_PORT = 22;
const int BASE_LIGHT_PORT = 37;
const int BASE_FAN_PORT = 48;
const int BASE_AC_PORT = 50;
const int BASE_TEMP_PORT = 53;

// hotel controls (set to -1 to disable)
const int HOTEL_CARD_INPUT_PORT = 44;
const int HOTEL_POWER_OUTPUT_PORT = 42;

unsigned char clock_tick = 0;

int dimmers[NUM_DIMMERS];
int effectiveLights[NUM_DIMMERS];
int onoffLights[NUM_LIGHTS];
int curtains[NUM_CURTAINS];
float ACSetPts[NUM_ACS];
int ACFanSpds[NUM_ACS];
long tempReadTmr;
long tempSendTmr;
boolean tempRising[NUM_ACS];
float fTemp[NUM_ACS];
float fAirflow = 0.0f;
long hotelControlTimer;

const float fHomeostasis = 0.49f;
const long iHotelTimeoutPeriod = 1 * 60 * 1000; // 1 minutes to turn off power
const float fHotelDefaultACTemp = 25.0f; // default temperature in hotel

OneWire oneWire(BASE_TEMP_PORT);
DallasTemperature sensors(&oneWire);

char buffer[64];
int pos;

void Wakeup() {
  SetLight(0, 1);
  SetLight(1, 1);
  SetLight(2, 1);
}

void SetCurtain ( int curtain, int action ) {
  curtains[curtain] = action;
  if ( action == 0 ) {
    digitalWrite ( BASE_CURTAIN_PORT + 2 * curtain, LOW );
    digitalWrite ( BASE_CURTAIN_PORT + 1 + 2 * curtain, LOW );
  } else if ( action == 1 ) {
    digitalWrite ( BASE_CURTAIN_PORT + 2 * curtain, LOW );
    digitalWrite ( BASE_CURTAIN_PORT + 1 + 2 * curtain, HIGH );
  } else if ( action == 2 ) {
    digitalWrite ( BASE_CURTAIN_PORT + 2 * curtain, HIGH );
    digitalWrite ( BASE_CURTAIN_PORT + 1 + 2 * curtain, LOW );
  }
}

void SetDimmer(int dimmer, int val) {
  dimmers[dimmer] = val;
#ifdef USE_ISR_DIMMERS
  if (val > 100)
    val = 100;
  if (val < 5)
    val = 0;
  val /= 2;
  effectiveLights[dimmer] = 50 + 50 - val;
#else
  analogWrite ( BASE_DIMMER_PORT + dimmer, (float)dimmers[dimmer] * 2.55 );
#endif
}

void SetLight(int light, int val) {
  onoffLights[light] = val;
  digitalWrite ( BASE_LIGHT_PORT - light, onoffLights[light] );
}

void SetAC(int ac, float val) {
  ACSetPts[ac] = val;
}

void SetFanSpeed(int fan, int val) {
  ACFanSpds[fan] = val;
  if ( ACFanSpds[fan] < 1 )
    digitalWrite ( BASE_FAN_PORT + fan, LOW );
  else
    digitalWrite ( BASE_FAN_PORT + fan, HIGH );
}

void SetAllOutputValues(boolean set_off = false) {
  for (int i = 0; i < NUM_LIGHTS; i++) {
    if (set_off)
      onoffLights[i] = 0;
    SetLight(i, onoffLights[i]);
  }
  for (int i = 0; i < NUM_DIMMERS; i++) {
    if (set_off)
      dimmers[i] = 0;
    SetDimmer(i, dimmers[i]);
  }
  for (int i = 0; i < NUM_CURTAINS; i++) {
    if (set_off)
      curtains[i] = 0;
    SetCurtain (i, curtains[i]);
  }
  for (int i = 0; i < NUM_ACS; i++) {
    if (set_off) {
      ACSetPts[i] = 25.0f;
      ACFanSpds[i] = 1;
    }
    SetAC(i, ACSetPts[i]);
    SetFanSpeed(i, ACFanSpds[i]);
  }
}

bool ExecuteCommand ( char buf[64] ) {
  int colon = -1;//buf.indexOf ( ':' );
  int len = 0;
  for (int i = 0; i < 64; i++, len++) {
    if (buf[i] == '\0')
      break;
    if (buf[i] == ':')
      colon = i;
  }
  if ( colon == -1 ) {
    return false;
  }
  char name[32], valStr[32];
  for (int i = 0; i < colon; i++)
    name[i] = buf[i];
  name[colon] = '\0';
  for (int i = colon+1; i < len; i++)
    valStr[i-colon-1] = buf[i];
  valStr[len-colon-1] = '\0';
  int val = atoi(valStr);
  if (val < 0)
    return false;
  if (strlen(name) == 2) {
    char p = name[0];
    int index = name[1] - '0';
    switch ( p ) {
      case 'l':
        if ( index < NUM_DIMMERS ) {
          SetDimmer(index, val);
        }
        break;
      case 't':
        if ( index < NUM_LIGHTS ) {
          SetLight(index, val);
        }
        break;
      case 'c':
        if ( index < NUM_CURTAINS ) {
          SetCurtain (index, val);
        }
        break;
      case 'a':
        if ( index < NUM_ACS ) {
          SetAC(index, ((float)val)/2.0f);
        }
        break;
      case 'f':
        if ( index < NUM_ACS ) {
          SetFanSpeed(index, val);
        }
        break;
    }
  }
  return true;
}

void timerIsr()
{
  if (effectiveLights[0]==clock_tick)
  {
    digitalWrite(BASE_DIMMER_PORT + 0, HIGH);   // triac firing
    delayMicroseconds(10);           // triac On propogation delay (for 60Hz use 8.33)
    digitalWrite(BASE_DIMMER_PORT + 0, LOW);    // triac Off
  }

  if (effectiveLights[1]==clock_tick)
  {
    digitalWrite(BASE_DIMMER_PORT + 1, HIGH);   // triac firing
    delayMicroseconds(10);           // triac On propogation delay (for 60Hz use 8.33)
    digitalWrite(BASE_DIMMER_PORT + 1, LOW);    // triac Off
  }

  if (effectiveLights[2]==clock_tick)
  {
    digitalWrite(BASE_DIMMER_PORT + 2, HIGH);   // triac firing
    delayMicroseconds(10);           // triac On propogation delay (for 60Hz use 8.33)
    digitalWrite(BASE_DIMMER_PORT + 2, LOW);    // triac Off
  }

  if (effectiveLights[3]==clock_tick)
  {
    digitalWrite(BASE_DIMMER_PORT + 3, HIGH);   // triac firing
    delayMicroseconds(10);           // triac On propogation delay (for 60Hz use 8.33)
    digitalWrite(BASE_DIMMER_PORT + 3, LOW);    // triac Off
  }
  clock_tick++;
}



void zero_crosss_int()  // function to be fired at the zero crossing to dim the light
{
  // Every zerocrossing interrupt: For 50Hz (1/2 Cycle) => 10ms  ; For 60Hz (1/2 Cycle) => 8.33ms
  // 10ms=10000us , 8.33ms=8330us
  clock_tick=0;
}

void setup() {
  Serial.begin(9600);

  //PWM output for dimmers
  for ( int i = BASE_DIMMER_PORT; i < BASE_DIMMER_PORT + NUM_DIMMERS; i++ )
    pinMode ( i, OUTPUT );
  //Digital output for curtains
  for (int i = BASE_CURTAIN_PORT; i < BASE_CURTAIN_PORT + NUM_CURTAINS*2; i++)
    pinMode ( i, OUTPUT );
  //Digital output for 2 compressors
  for ( int i = BASE_AC_PORT; i < BASE_AC_PORT + NUM_ACS; i++ )
    pinMode ( i, OUTPUT );
  //Digital output for 2 fan speeds
  for ( int i = BASE_FAN_PORT; i < BASE_FAN_PORT + NUM_ACS; i++ )
    pinMode ( i, OUTPUT );
  //Digital output for on-off lights
  for ( int i = BASE_LIGHT_PORT; i > BASE_LIGHT_PORT - NUM_LIGHTS; i-- )
    pinMode ( i, OUTPUT );
  //DIGITAL 46 FOR 1-WIRE BUS**********
  sensors.begin ( );
  discoverOneWireDevices ( );
  if (HOTEL_CARD_INPUT_PORT != -1)
    pinMode(HOTEL_CARD_INPUT_PORT, INPUT);
  if (HOTEL_POWER_OUTPUT_PORT != -1)
    pinMode(HOTEL_POWER_OUTPUT_PORT, OUTPUT);

  // setup interrupts for the dimmers
#ifdef USE_ISR_DIMMERS
  attachInterrupt(1, zero_crosss_int, RISING);
  Timer1.initialize(100); // set a timer of length 100 microseconds for 50Hz or 83 microseconds for 60Hz;
  Timer1.attachInterrupt( timerIsr ); // attach the service routine here
#endif

  SetAllOutputValues(true); //default
  for ( int i = 0; i < NUM_ACS; i++ ) {
    digitalWrite ( BASE_AC_PORT + i, HIGH );
    tempRising[i] = false;
  }

  tempSendTmr = millis ( );
  tempReadTmr = millis ( );
  hotelControlTimer = millis ( );

  pos = 0;
  buffer[0] = '\0';
}

void loop() {
  long curTime = millis ( );

  // time overflow?
  if (curTime < tempSendTmr - 1000) {
    tempSendTmr = curTime;
    tempReadTmr = curTime;
  }

  while (Serial.available() > 0) {
    // read the bytes incoming from the client:
    char c = Serial.read();
    //if ( DEBUGPRINT )
    //  Serial.write(c);
    if ( pos > 62 )
      pos = 0;
    buffer[pos++] = c;
    if ( c == '\n' ) {
      buffer[pos-1] = '\0';
      ExecuteCommand (buffer);
      pos = 0;
    }
  }

  if ( curTime > 1000 + tempSendTmr ) { //1000 milliseconds
    tempSendTmr = curTime;
    Serial.write ( -2 );
    Serial.write ( (int)NUM_ACS );
    Serial.write ( (int)ACFanSpds[0] );
    Serial.write ( (int)ACFanSpds[1] );
    Serial.write ( (int)(ACSetPts[0]*2.0f) );
    Serial.write ( (int)(ACSetPts[1]*2.0f) );
    Serial.write ( (int)fTemp[0] );
    Serial.write ( (int)fTemp[1] );

    Serial.write ( (int)NUM_DIMMERS );
    for ( int i = 0; i < NUM_DIMMERS; i++ )
      Serial.write ( (int)dimmers[i] );
    Serial.write ( (int)NUM_LIGHTS );
    for ( int i = 0; i < NUM_LIGHTS; i++ )
      Serial.write ( (int)onoffLights[i] );
    Serial.write ( (int)NUM_CURTAINS );
    Serial.write ( -1 );
  }

  if ( curTime > 1000 + tempReadTmr ) { //refresh every 1 second
    //read the temperatures
    sensors.requestTemperatures(); // Send the command to get temperatures
    for ( int i = 0; i < NUM_ACS; i++ )
      fTemp[i] = sensors.getTempCByIndex ( i );
    tempReadTmr = curTime;
  }
  for ( int i = 0; i < NUM_ACS; i++ ) {
    if ( tempRising[i] ) {
      if ( fTemp[i] > ((float)ACSetPts[i])+ fHomeostasis ) {
        digitalWrite ( BASE_AC_PORT + i, HIGH ); //was rising, got above the band, turn on compressor
        tempRising[i] = false;
      } else {
        digitalWrite ( BASE_AC_PORT + i, LOW );
      }
    }
    if ( !tempRising[i] ) {
      if ( fTemp[i] < ((float)ACSetPts[i]) - fHomeostasis ) {
        digitalWrite ( BASE_AC_PORT + i, LOW);
        tempRising[i] = true;
      } else {
        digitalWrite ( BASE_AC_PORT + i, HIGH );
      }
    }

    // central AC
    float fTempDiff = ACSetPts[i] - fTemp[i];
    float diff = fTemp[i] - ((float)ACSetPts[i]);
    float coeff = (min(max(diff, -10), 10)) / 10; // [-1, 1]
    fAirflow = min(max(fAirflow + fHomeostasis * coeff, 0), 255);
    analogWrite(BASE_CENTRAL_AC_PORT + i, (int)fAirflow);
  }

  if (HOTEL_CARD_INPUT_PORT != -1 && HOTEL_POWER_OUTPUT_PORT != -1) {
    if (digitalRead(HOTEL_CARD_INPUT_PORT) == HIGH) {
      if (curTime > hotelControlTimer + 2000) {
        Wakeup();
      }
      digitalWrite(HOTEL_POWER_OUTPUT_PORT, HIGH);
      hotelControlTimer = curTime;
    } else if (curTime > hotelControlTimer + iHotelTimeoutPeriod) {
      digitalWrite(HOTEL_POWER_OUTPUT_PORT, LOW);
      SetAllOutputValues(true); // turn off everything
    }
  }
}

int discoverOneWireDevices(void) {
  byte i;
  byte present = 0;
  byte data[12];
  byte addr[8];
  int count = 0;

  //Serial.println("Looking for 1-Wire devices...");
  while(oneWire.search(addr)) {
    /*Serial.print("Found \'1-Wire\' device with address: ");
    for( i = 0; i < 8; i++) {
      Serial.print("0x");
      if (addr[i] < 16) {
        Serial.print('0');
      }
      Serial.print(addr[i], HEX);
      if (i < 7) {
        Serial.print(", ");
      }
    }
    if ( OneWire::crc8( addr, 7) != addr[7]) {
        Serial.println("CRC is not valid!");
        return 0;
    }
    Serial.println();*/
    count++;
  }
  //Serial.println("That's it.");
  oneWire.reset_search();
  return count;
}