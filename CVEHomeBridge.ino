/*
   Original Author: Klusjesman

   Tested with STK500 + ATMega328P
   GCC-AVR compiler

   Modified by supersjimmie:
   Code and libraries made compatible with Arduino and ESP8266
   Tested with Arduino IDE v1.6.5 and 1.6.9
   For ESP8266 tested with ESP8266 core for Arduino v 2.1.0 and 2.2.0 Stable
   (See https://github.com/esp8266/Arduino/ )

*/

/*
  CC11xx pins    ESP pins Arduino pins  Description
  1 - VCC        VCC      VCC           3v3
  2 - GND        GND      GND           Ground
  3 - MOSI       13=D7    Pin 11        Data input to CC11xx
  4 - SCK        14=D5    Pin 13        Clock pin
  5 - MISO/GDO1  12=D6    Pin 12        Data output from CC11xx / serial clock from CC11xx
  6 - GDO2       04=D2    Pin 2?        Serial data to CC11xx
  7 - GDO0       ?        Pin  ?        output as a symbol of receiving or sending data
  8 - CSN        15=D8    Pin 10        Chip select / (SPI_SS)
*/

#include <SPI.h>
#include "IthoCC1101.h"
#include "IthoPacket.h"
#include <Ticker.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

#define ITHO_IRQ_PIN D2

ESP8266WebServer server;
char* ssid = "IKHEBBEREIKHIER";
char* password = "00022711";

IthoCC1101 rf;
IthoPacket packet;
Ticker ITHOticker;

const uint8_t RFTid[] = {101, 89, 154, 153, 170, 105, 154, 86}; // my ID

bool ITHOhasPacket = false;
IthoCommand RFTcommand[3] = {IthoUnknown, IthoUnknown, IthoUnknown};
byte RFTRSSI[3] = {0, 0, 0};
byte RFTcommandpos = 0;
IthoCommand RFTlastCommand = IthoLow;
IthoCommand RFTstate = IthoUnknown;
IthoCommand savedRFTstate = IthoUnknown;
bool RFTidChk[3] = {false, false, false};

bool demoDone = false;
const int ledPin =  LED_BUILTIN;
int ledState = LOW;
unsigned long previousMillis = 0;
const long cycleInterval = 10000;

void setup(void) { 
  Serial.begin(115200);
  delay(500);
  Serial.println("######  setup begin  ######");
  rf.init();
  pinMode(ITHO_IRQ_PIN, INPUT);
  attachInterrupt(ITHO_IRQ_PIN, ITHOinterrupt, RISING);
  setupWiFi();
  setupServer();
  Serial.println("######  setup done   ######");
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
}

void loop(void) {
  // do whatever you want, check (and reset) the ITHOhasPacket flag whenever you like
  if (ITHOhasPacket) {
    showPacket();
  }

    server.handleClient();

  return;
  
  //set CC1101 registers
  rf.initReceive();
  Serial.print("start\n");
  sei();

  while (1==1) {
    if (rf.checkForNewPacket()) {
      packet = rf.getLastPacket();
      //show counter
      Serial.print("counter=");
      Serial.print(packet.counter);
      Serial.print(", ");
      //show command
      switch (packet.command) {
        case IthoUnknown:
          Serial.print("unknown\n");
          break;
        case IthoLow:
          Serial.print("low\n");
          break;
        case IthoMedium:
          Serial.print("medium\n");
          break;
        case IthoFull:
          Serial.print("full\n");
          break;
        case IthoTimer1:
          Serial.print("timer1\n");
          break;
        case IthoTimer2:
          Serial.print("timer2\n");
          break;
        case IthoTimer3:
          Serial.print("timer3\n");
          break;
        case IthoJoin:
          Serial.print("join\n");
          break;
        case IthoLeave:
          Serial.print("leave\n");
          break;
      } // switch (recv) command
    } // checkfornewpacket
  yield();
  } // while 1==1
  
}

void usage() {
  server.send(200, "text/plain", "/press?button=low\r\n");
}

void pressButton() {
   if (!server.hasArg("button")) {
       return returnFail("Please provide a button, e.g. ?button=low");
   }
   
   String button = server.arg("button");
   Serial.print("Pressing button: ");
   Serial.println(button);

   if (button == "low") { 
       sendLowSpeed();
   } else if (button == "medium") { 
       sendMediumSpeed();
   } else if (button == "high") { 
       sendHighSpeed();
   } else if (button == "timer") {     
       sendTimer();
   } else {
      return returnFail("Unknown button. Buttons are: low, medium, high, timer and register");
   }
  
   returnJson("\"button\": \"" + button + "\"");
}

void returnJson(String msg)
{
    server.send(200, "application/json", "{\"success\": true, "+  msg + "}\r\n");
}

void returnFail(String msg)
{
    server.sendHeader("Connection", "close");
    server.send(500, "application/json", "{\"success\": false, \"message\": \""+ msg + "\"}\r\n");
}

bool setupServer(){
    // server setup
  server.on("/", usage);
  server.on("/press", pressButton);
  server.begin();
  Serial.println("Server started");
}

bool setupWiFi(){
  // connect to wifi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.println("");
}

void flashLed(){
  for (int thisPin = 0; thisPin < 20; thisPin++) {
    digitalWrite(LED_BUILTIN, HIGH);   // turn the LED on (HIGH is the voltage level)
    delay(20);
    digitalWrite(LED_BUILTIN, LOW);    // turn the LED off by making the voltage LOW
    delay(20);
  }
}

void ITHOinterrupt() {
  ITHOticker.once_ms(10, ITHOcheck);
}

void ITHOcheck() {
  if (rf.checkForNewPacket()) {
    IthoCommand cmd = rf.getLastCommand();
    if (++RFTcommandpos > 2) RFTcommandpos = 0;  // store information in next entry of ringbuffers
    RFTcommand[RFTcommandpos] = cmd;
    RFTRSSI[RFTcommandpos]    = rf.ReadRSSI();
    bool chk = rf.checkID(RFTid);
    RFTidChk[RFTcommandpos]   = chk;
    if ((cmd != IthoUnknown) && chk) {  // only act on good cmd and correct id.
      ITHOhasPacket = true;
    }
  }
}

void showPacket() {
  ITHOhasPacket = false;
  uint8_t goodpos = findRFTlastCommand();
  if (goodpos != -1)  RFTlastCommand = RFTcommand[goodpos];
  else                RFTlastCommand = IthoUnknown;
  //show data
  Serial.print(F("RFT Current Pos: "));
  Serial.print(RFTcommandpos);
  Serial.print(F(", Good Pos: "));
  Serial.println(goodpos);
  Serial.print(F("Stored 3 commands: "));
  Serial.print(RFTcommand[0]);
  Serial.print(F(" "));
  Serial.print(RFTcommand[1]);
  Serial.print(F(" "));
  Serial.print(RFTcommand[2]);
  Serial.print(F(" / Stored 3 RSSI's:     "));
  Serial.print(RFTRSSI[0]);
  Serial.print(F(" "));
  Serial.print(RFTRSSI[1]);
  Serial.print(F(" "));
  Serial.print(RFTRSSI[2]);
  Serial.print(F(" / Stored 3 ID checks: "));
  Serial.print(RFTidChk[0]);
  Serial.print(F(" "));
  Serial.print(RFTidChk[1]);
  Serial.print(F(" "));
  Serial.print(RFTidChk[2]);
  Serial.print(F(" / Last ID: "));
  Serial.print(rf.getLastIDstr());

  Serial.print(F(" / Command = "));
  //show command
  switch (RFTlastCommand) {
    case IthoUnknown:
      Serial.print("unknown\n");
      break;
    case IthoLow:
      Serial.print("low\n");
      break;
    case IthoMedium:
      Serial.print("medium\n");
      break;
    case IthoHigh:
      Serial.print("high\n");
      break;
    case IthoFull:
      Serial.print("full\n");
      break;
    case IthoTimer1:
      Serial.print("timer1\n");
      break;
    case IthoTimer2:
      Serial.print("timer2\n");
      break;
    case IthoTimer3:
      Serial.print("timer3\n");
      break;
    case IthoJoin:
      Serial.print("join\n");
      break;
    case IthoLeave:
      Serial.print("leave\n");
      break;
  }
}

uint8_t findRFTlastCommand() {
  if (RFTcommand[RFTcommandpos] != IthoUnknown)               return RFTcommandpos;
  if ((RFTcommandpos == 0) && (RFTcommand[2] != IthoUnknown)) return 2;
  if ((RFTcommandpos == 0) && (RFTcommand[1] != IthoUnknown)) return 1;
  if ((RFTcommandpos == 1) && (RFTcommand[0] != IthoUnknown)) return 0;
  if ((RFTcommandpos == 1) && (RFTcommand[2] != IthoUnknown)) return 2;
  if ((RFTcommandpos == 2) && (RFTcommand[1] != IthoUnknown)) return 1;
  if ((RFTcommandpos == 2) && (RFTcommand[0] != IthoUnknown)) return 0;
  return -1;
}

void sendRegister() {
  Serial.println("sending join...");
  rf.sendCommand(IthoJoin);
  Serial.println("sending join done.");
}

void sendStandbySpeed() {
  Serial.println("sending standby...");
  rf.sendCommand(IthoStandby);
  Serial.println("sending standby done.");
}

void sendLowSpeed() {
  Serial.println("sending low...");
  rf.sendCommand(IthoLow);
  flashLed();
  Serial.println("sending low done.");
}

void sendMediumSpeed() {
  Serial.println("sending medium...");
  rf.sendCommand(IthoMedium);
  flashLed();
  Serial.println("sending medium done.");
}

void sendHighSpeed() {
  Serial.println("sending high...");
  rf.sendCommand(IthoHigh);
  flashLed();
  Serial.println("sending high done.");
}

void sendFullSpeed() {
  Serial.println("sending FullSpeed...");
  rf.sendCommand(IthoFull);
  Serial.println("sending FullSpeed done.");
}

void sendTimer() {
  Serial.println("sending timer...");
  rf.sendCommand(IthoTimer1);
  Serial.println("sending timer done.");
}
