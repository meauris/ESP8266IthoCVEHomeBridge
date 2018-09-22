/*
   Original Author: Klusjesman

   Tested with STK500 + ATMega328P
   GCC-AVR compiler

   Modified by supersjimmie:
   Code and libraries made compatible with Arduino and ESP8266
   Tested with Arduino IDE v1.6.5 and 1.6.9
   For ESP8266 tested with ESP8266 core for Arduino v 2.1.0 and 2.2.0 Stable
   (See https://github.com/esp8266/Arduino/ )

  Modified by Meauris:
  Extra features added:
  * Connects to WiFi netwerk as a station using ESP8266WiFi
  * Prints the received IP address to serial at 115200 baud
  * Responds to http get commands using ESP8266WebServer and prints to serial at 115200 baud
  * Responds to rft packets, if the RFTid is the same as configured under RFTid[] the desired state is saved and printed to serial at 115200 baud.
  * The root path (http://xxx.xxx.xxx.xxx/) returns a json respons containing the current status of the CVE
  * This solution is also usable as a repeater, which results in an increased signal range of the RFT.
*/


#include <SPI.h>
#include "IthoCC1101.h"
#include "IthoPacket.h"
#include <Ticker.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

#define ITHO_IRQ_PIN D2

/*
 * The values of ssid and password are required for the webserver to work
 */
char* ssid = "IKHEBBEREIKHIER";
char* password = "00022711";

/*
 * change the value of repeater to true if repeater functionality is required, 
 * you can also change this value by navigating to http://xxx.xxx.xxx.xxx/repeater?value=true
 */
bool repeater = false;

 
ESP8266WebServer server;
IthoCC1101 rf;
IthoPacket packet;
Ticker ITHOticker;

// This constant is used to filter out other RFT device packets and to send packets with
// Use Serial.println("ID of sender: " + rf.getLastIDstr()); to find the sender ID of your RFT device
const uint8_t RFTid[] = {0x66, 0xa9, 0x6a, 0xa5, 0xa9, 0xa9, 0x9a, 0x56}; 

bool ITHOhasPacket = false;
bool RFTrepeater = false;
IthoCommand RFTcommand[3] = {IthoLow, IthoMedium, IthoHigh};
byte RFTRSSI[3] = {0, 0, 0};
byte RFTcommandpos = 0;
IthoCommand RFTlastCommand = IthoLow;
IthoCommand RFTstate = IthoUnknown;
IthoCommand savedRFTstate = IthoUnknown;
bool RFTidChk[3] = {false, false, false};

// variables for the led flashing
int LedFlashTimes = 0;
int LedState = HIGH; //means off
const int OFF = HIGH;
const int ON = LOW;
unsigned long previousMillisLedStateOn = 0;
const unsigned long flashIntervalMillis = 10;

void setup(void) { 
  Serial.begin(115200);
  delay(500);
  Serial.println("######  setup begin  ######");
  setupLED();
  setupRF();
  setupWiFi();
  setupServer();
  Serial.println("######  setup done   ######");
  delay(500);
}

void loop(void) {
  ledState();
  if (ITHOhasPacket) {
    showPacket();
    if(repeater) repeatReceivedPacketCommand();
  }
  server.handleClient();
  
  yield();
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

bool setupRF(){
  rf.init();
  rf.initReceive();
  pinMode(ITHO_IRQ_PIN, INPUT);
  attachInterrupt(ITHO_IRQ_PIN, ITHOinterrupt, RISING);
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

void setupLED() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LedState);
}

void flashLed(int times){
  LedFlashTimes += times;
}

void ledState() {
  unsigned long currentMillis = millis();
  if ((LedFlashTimes == 0) && (LedState == ON) && ((currentMillis - previousMillisLedStateOn) > flashIntervalMillis)) {
    LedState = OFF;
  } else if ((LedFlashTimes > 0) && (LedState == OFF)) {
    LedFlashTimes--;
    previousMillisLedStateOn = currentMillis;
    LedState = ON;
  } 
  else if ((LedFlashTimes > 0) && (LedState == ON) && ((currentMillis - previousMillisLedStateOn) > flashIntervalMillis)) {
    LedState = OFF;
  } 
  digitalWrite(LED_BUILTIN, LedState);
}

void ITHOinterrupt() {
  ITHOticker.once_ms(10, ITHOcheck);
}

void ITHOcheck() {
  flashLed(1);
  if (rf.checkForNewPacket()) {
    IthoCommand cmd = rf.getLastCommand();
    if (++RFTcommandpos > 2) RFTcommandpos = 0;  // store information in next entry of ringbuffers
    RFTcommand[RFTcommandpos] = cmd;
    RFTRSSI[RFTcommandpos]    = rf.ReadRSSI();
    bool chk = rf.checkID(RFTid);
    RFTidChk[RFTcommandpos]   = chk;
    if ((cmd != IthoUnknown) && chk) {  // only act on good cmd and correct RFTid.
      ITHOhasPacket = true;
    }
  }
}

void repeatReceivedPacketCommand() {
  ITHOhasPacket = false;
  uint8_t goodpos = findRFTlastCommand();
  if (goodpos != -1)  RFTlastCommand = RFTcommand[goodpos];
  else                RFTlastCommand = IthoUnknown;
  Serial.print("Repeating command: [");
  Serial.print(RFTlastCommand);
  rf.sendCommand(RFTlastCommand);
  Serial.println("]\n");
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
  flashLed(10);
  Serial.println("sending low done.");
}

void sendMediumSpeed() {
  Serial.println("sending medium...");
  rf.sendCommand(IthoMedium);
  flashLed(10);
  Serial.println("sending medium done.");
}

void sendHighSpeed() {
  Serial.println("sending high...");
  rf.sendCommand(IthoHigh);
  flashLed(10);
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
