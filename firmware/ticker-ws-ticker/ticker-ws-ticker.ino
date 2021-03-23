#include <esp8266_hw_spi_max7219_7seg.h>  //https://github.com/BugerDread/esp8266-hw-spi-max7219-7seg
#include <Ticker.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <WebSocketsClient.h>     //https://github.com/Links2004/arduinoWebSockets   
#include <EEPROM.h>

// configuration
const char COMPILE_DATE[] PROGMEM = __DATE__ " " __TIME__;

const uint32_t SPI_SPEED = 8000000;           //SPI@8MHZ
const uint8_t SPI_CSPIN = 15;                  //SPI CS - may vary in older versions
const uint8_t DISP_BRGTH = 8;                 //brightness of the display
const uint8_t DISP_AMOUNT = 1;                //number of max 7seg modules connected

const uint8_t CFGPORTAL_TIMEOUT = 120;        //timeout for config portal in seconds
const uint8_t CFGPORTAL_BUTTON = 0;                 //0 for default FLASH button on nodeMCU board
const uint8_t CFGPORTAL_BUTTON_TIME = 5;                   //time [s] to hold CFGPORTAL_BUTTON to activate cfg portal
const char * const CFGPORTAL_SSID = "Bgr ticker";
const char * const CFGPORTAL_PWD = "btcbtcbtc";

const char * const CFG_DEF_SYMBOLS = "BTCUSD";
const uint8_t CFG_DEF_BRIGHTNESS = 8;
const uint8_t CFG_DEF_CYCLE_TIME = 3;

const char * const APISRV = "api.bitfinex.com";
const uint16_t APIPORT = 443;
const char * const APIURL = "/ws/2";
const char * const REQ1 = "{\"event\":\"subscribe\",\"channel\":\"ticker\",\"symbol\":\"t";
const char * const REQ2 = "\"}";
const uint16_t WS_RECONNECT_INTERVAL = 5000;  // websocket reconnec interval
const uint8_t HB_TIMEOUT = 30;                //heartbeat interval in seconds

const size_t jcapacity = JSON_ARRAY_SIZE(2) + JSON_ARRAY_SIZE(10);   //size of json to parse ticker api (according to https://arduinojson.org/v6/assistant/)

float price = -1;
float prevval = -1;
//String pays = "";
bool clrflag = false;
bool shouldSaveConfig  = false; //flag for saving data
bool reconnflag = false;
bool dispchng = false;
int symidx, subsidx = 0;
int prevsymidx = -1;
int symnum = 0;

//array for ticker data
struct  symboldata_t {
  //String symbol;
  char symbol[7];
  long chanid;
  float price;
  float change;
  bool hb;
};

symboldata_t symarray[16];

StaticJsonDocument<jcapacity> jdoc;
Ticker symticker; //ticker to switch symbols
Ticker hbticker; 
Ticker rstticker;
WebSocketsClient webSocket;
BgrMax7seg ld = BgrMax7seg(SPI_SPEED, SPI_CSPIN, DISP_AMOUNT); //init display

struct cfg_t {        
  char symbols[130];
  uint8_t brightness;
  uint8_t cycle_time;
  uint8_t checksum;
} cfg;

uint8_t cfg_checksum(const cfg_t c) {
  uint8_t checksum = 42;    //init value, to make chcecksum of config full of 0s invalid
  for (uint8_t * i = (uint8_t *)&c; i < ((uint8_t *)&c + sizeof(c) - sizeof(c.checksum)); i++) {
    //i is pointer to uint_8, initial value addres of c, iterate over all bytes of c except last one which is the checksum itself
    checksum = (checksum + *i) & 0xff;
  }
  return checksum;
}

bool get_cfg_eeprom() {
  EEPROM.get(0, cfg);
  Serial.println(F("Loading config from EEPROM"));
  Serial.print(F("Checksum "));
  if (cfg.checksum == cfg_checksum(cfg)) {
    Serial.print(F("[VALID]: "));
    Serial.println(cfg.checksum);
    Serial.print(F("Symbols: "));
    Serial.println(cfg.symbols);
    Serial.print(F("Cycle time: "));
    Serial.println(cfg.cycle_time);
    Serial.print(F("Brightness: "));
    Serial.println(cfg.brightness);
    return true;
  } else {
    Serial.println(F("[INVALID] !"));
    return false;
  }
}

bool save_cfg_eeprom() {
  cfg.checksum = cfg_checksum(cfg);
  EEPROM.put(0, cfg);
  if (EEPROM.commit()) {
    Serial.println(F("Config saved to EEPROM"));
    return true;
  } else {
    Serial.println(F("EEPROM error, cant save config"));
    return false;
  }
}

void rstwmcfg() {
  if (digitalRead(CFGPORTAL_BUTTON) == LOW) {  //if still pressed
    clrflag = true;
  } else {                      //not pressed anymore
    rstticker.detach();
  }
}

void nextsymidx () {
  if (prevval != -200) {prevval = -201;} //force redraw if not in connecting mode
  if (dispchng == false) {
    dispchng = true;  //will display daily change
  } else {
    //move to next symbol
    dispchng = false;
    symidx++;
    if (symidx >= subsidx) {
      symidx = 0;
    }
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  if (DISP_AMOUNT == 2) {
    ld.print(F(" config "), 1);
  }
  ld.print(F("192.168.4.1"), DISP_AMOUNT);  //show on 1st display if we have only one, show on 2nd if we have two
}

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println(F("Should save config"));
  shouldSaveConfig = true;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t len) {
  switch (type) {
    case WStype_DISCONNECTED: {
        Serial.println("[WSc] Disconnected!");
        subsidx = 0;  //no symbols subscribed
      }
      break;
    case WStype_CONNECTED: {
        Serial.print("[WSc] Connected to url: ");
        Serial.println((char*)payload);
      }
      break;
    case WStype_TEXT: {
        Serial.print("[WSc] data: ");
        Serial.println((char*)payload);
        //pays = (char*)payload;
        parsepl((char*)payload, len);
      }
      break;
    case WStype_BIN: {
        Serial.print("[WSc] get binary length: ");
        Serial.println(len);
      }
      break;
  }
}

bool parsepl(const char * payload, const size_t len) {
  DeserializationError error = deserializeJson(jdoc, payload, len);   //deserialize
  //pays = "";
  // Test if parsing succeeds.
  if (!error) {
    //   Serial.println(F("[Prs] its an array"));
    //float tp = 0.0;
    bool newdata = false;
    bool temphb = false;
    if (jdoc[1] == "hb") {  //its a heartbeat
      //  Serial.println(F("[Prs] Heartbeat!"));
      temphb = true;
    } else if (jdoc[1][6] != nullptr) { // new prize
      newdata = true;
      //Serial.print("[Prs] Update, price: ");
      //tp = root[1][6];
      //Serial.print((float)jdoc[1][6]);
      //Serial.print(", change: ");
      //Serial.println(100*(float)jdoc[1][5]);
    } 

    //[CHANNEL_ID,[BID,BID_SIZE,ASK,ASK_SIZE,DAILY_CHANGE,DAILY_CHANGE_PERC,LAST_PRICE,VOLUME,HIGH,LOW]]
    
    //find the symbol in array and set the prize if we have some prizze
    if ((newdata == true) or (temphb == true)) {  //if we have prize or hb
      for (byte i = 0; i < subsidx; i++) { //symnum -> subsidx   iterate the array of subscribed
        if (symarray[i].chanid == jdoc[0]) {  //we found it
          if (newdata == true) {
            symarray[i].price = jdoc[1][6]; 
            symarray[i].change = (float)(jdoc[1][5]) * 100.0;
            //symarray[i].change *= 100;
            }
          //if (temphb == true) 
          symarray[i].hb = true; 
          // Serial.print(F("[Prs] array updated, i = "));
          // Serial.println(i);
          break;
        }
      }
      return true;
    } else {
      // its not HB or price update
      // check if its a subscribe event info
      if (jdoc["event"] == "info") {
        if (subsidx == 0) {
          Serial.print(F("[Prs] Got info, lets subscribe 1st ticker symbol: "));
          Serial.println(symarray[subsidx].symbol);
          char txbuff[strlen(REQ1) + 6 + strlen(REQ2) + 1];
          snprintf(txbuff, sizeof(txbuff), "%s%s%s", REQ1, symarray[subsidx].symbol, REQ2);
          //Serial.println(txbuff);
          webSocket.sendTXT(txbuff, strlen(txbuff));
        }
      } else if (jdoc["event"] == "subscribed")  {
        if (jdoc["chanId"] != false) {
          symarray[subsidx].chanid = jdoc["chanId"];
          Serial.print("[Prs] Ticker subscribe success, channel id: ");
          Serial.println(symarray[subsidx].chanid);
          subsidx++;  //move to next symbol in array
          if (subsidx < symnum) { //subscribe next
            Serial.print("[Prs] Lets subscribe next ticker symbol: ");
            //webSocket.sendTXT(REQ1 + symarray[subsidx].symbol + REQ2);
            char txbuff[strlen(REQ1) + 6 + strlen(REQ2) + 1];
            snprintf(txbuff, sizeof(txbuff), "%s%s%s", REQ1, symarray[subsidx].symbol, REQ2);
            webSocket.sendTXT(txbuff, strlen(txbuff));
          }
        } else {
          Serial.println(F("[Prs] Ticker subscribe failed"));
        }
      }
      return true;
    }
  } else {      //deserializing error 
    return false;
  }
}

void hbcheck() {
  bool ok = true;                       // for testing only
  for (byte i = 0; i < symnum; i++) {   //for all symbols
    if (symarray[i].hb != true) {
      ok = false;
      Serial.print("[HBC] hb check failed, symbol = ");
      Serial.println(symarray[i].symbol);
    }
    symarray[i].hb = false; //clear all HBs
  }
  if (ok) {Serial.println("[HBC] hb check OK");} else {
    //hbcheck failed
    Serial.println("[HBC] hb check FAILED, reconnect websocket");
    reconnflag = true;  //set the flag, will do the reconnect in main loop
  }
}

void parsesymbols(String s) {
  //lets count symbols and put them into symarray
  int last = 0;
  int pos = 0;
  symnum = 0;
  while ((pos != -1) and (symnum <= 16) and (s.length() > 0)) {
    pos = s.indexOf(' ', last);
    if (pos == -1) { //last symbol
      Serial.print("[Setup] last symbol: ");
      Serial.println(s.substring(last));
      //symarray[symnum].symbol = s.substring(last);
      snprintf(symarray[symnum].symbol, sizeof(symarray[symnum].symbol), "%s", s.substring(last).c_str());
    } else {
      Serial.print("[Setup] add symbol: ");
      Serial.println(s.substring(last, pos));
      //symarray[symnum].symbol = s.substring(last, pos);
      snprintf(symarray[symnum].symbol, sizeof(symarray[symnum].symbol), "%s", s.substring(last, pos).c_str());
      last = pos + 1;
    }
    //symarray[symnum].symbol.toUpperCase();
    strupr(symarray[symnum].symbol);
    symarray[symnum].hb = false;
    symnum++;
  }
}



void cfgbywm() {
  WiFiManager wifiManager;                                //Local intialization. Once its business is done, there is no need to keep it around

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_symbol("symbol", "bitfinex symbol(s)", cfg.symbols, 128);
  char sbr[4];
  itoa(cfg.brightness, sbr, 10);
  WiFiManagerParameter custom_sbrightness("sbrightness", "display brightness [0 - 15]", sbr, 2);
  char sctime[4];
  itoa(cfg.cycle_time, sctime, 10);
  WiFiManagerParameter custom_symtime("symtime", "time to cycle symbols", sctime, 3);

  wifiManager.setSaveConfigCallback(saveConfigCallback);  //set config save notify callback
  wifiManager.setAPCallback(configModeCallback);          //flash led if in config mode
  //add all your parameters here
  wifiManager.addParameter(&custom_symbol);
  wifiManager.addParameter(&custom_sbrightness);
  wifiManager.addParameter(&custom_symtime);
  //wifiManager.setMinimumSignalQuality();                //set minimu quality of signal so it ignores AP's under that quality, defaults to 8%
  wifiManager.setTimeout(CFGPORTAL_TIMEOUT);                          //sets timeout until configuration portal gets turned off useful to make it all retry or go to sleep in seconds

  if (!wifiManager.autoConnect(CFGPORTAL_SSID, CFGPORTAL_PWD)) {  //fetches ssid and pass and tries to connect if it does not connect it starts an access point with the specified name and goes into a blocking loop awaiting configuration
    Serial.println(F("Config portal timeout, trying to connect again"));
    ESP.reset();                                          //reset and try again, or maybe put it to deep sleep
  }

  Serial.println("WiFi connected...yeey :)");             //if you get here you have connected to the WiFi
  ld.print("  wifi  ", 1);
  if (DISP_AMOUNT == 2) {
      ld.print(" online ", 2);
    }
  
  parsesymbols(String(custom_symbol.getValue()));

  if  (((String(custom_sbrightness.getValue()).toInt()) <= 0) or ((String(custom_sbrightness.getValue()).toInt()) > 16) or
         ((String(custom_symtime.getValue()).toInt()) <= 0) or ((String(custom_symtime.getValue()).toInt()) > 999) or 
         (symnum == 0))
  {
    Serial.println(F("Parametters out of range, restart config portal"));
    WiFi.disconnect();
    ESP.reset(); 
  }

  strncpy(cfg.symbols, custom_symbol.getValue(), sizeof(cfg.symbols));               //read updated parameters
  cfg.brightness = atoi(custom_sbrightness.getValue());               //read updated parameters
  cfg.cycle_time = atoi(custom_symtime.getValue());

  //save the custom parameters to EEPROM
  if (shouldSaveConfig) {
    save_cfg_eeprom();
  }
}

void setup() {
  Serial.begin(115200);
  delay (1000);
  
  // initialize digital pin LED_BUILTIN as an output and turn off the LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println(F("[Setup] Boot!"));
  
  Serial.print(F("Compile date: "));
  Serial.println(FPSTR(COMPILE_DATE));

  if (WiFi.getMode() != WIFI_STA) {
    Serial.println(F("Set WiFi mode to STA"));
    WiFi.mode(WIFI_STA); // set STA mode, esp defaults to STA+AP
  } else {
    Serial.println(F("WiFi already in STA mode"));
  }

  /* init displays and set the brightness min:1, max:15 */
  ld.init();
  ld.setBright(DISP_BRGTH, ALL_MODULES);
  ld.print(F("dread.cz "), 1);
  if (DISP_AMOUNT == 2) {
    ld.print(F(" ticker "), 2);
  }

  //get configuration from eeprom
  EEPROM.begin(512);
  if (!get_cfg_eeprom()) {
    //configuration in eeprom is not valid - we need to create default one
    Serial.println(F("EEPROM checksum invalid, creating default configuration"));
    strncpy (cfg.symbols, CFG_DEF_SYMBOLS, sizeof(cfg.symbols));
    cfg.brightness = CFG_DEF_BRIGHTNESS;
    cfg.cycle_time = CFG_DEF_CYCLE_TIME;
    save_cfg_eeprom();
  }

  cfgbywm();

  //uint8_t i = 15 & String(sbrightness).toInt() ;  //max brightness is 15 so cap it to 15
  ld.setBright(cfg.brightness, ALL_MODULES);
  Serial.print(F("Setting display brightness to: "));
  Serial.println(cfg.brightness);
  Serial.print(F("[Setup] My IP: "));
  Serial.println(WiFi.localIP());
  Serial.print(F("[Setup] symnum = "));
  Serial.println(symnum);
  
  symticker.attach(cfg.cycle_time, nextsymidx);
  Serial.print("[Setup] symbol cycle time: ");
  Serial.println(cfg.cycle_time);

  //start the connection
  webSocket.beginSSL(APISRV, APIPORT, APIURL);  //, "#INSECURE#"
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
  hbticker.attach(HB_TIMEOUT, hbcheck);
  Serial.print("[Setup] started HB check ticker, time: ");
  Serial.println(HB_TIMEOUT);

  pinMode(CFGPORTAL_BUTTON, INPUT_PULLUP);  //button for reset of params
}

String temp;
byte needdeci;
float temppr;

char dbuff[10]; //8 chars + 1decimal dot + zero terminator

void loop() {
  webSocket.loop();
   
  if ((digitalRead(CFGPORTAL_BUTTON) == LOW) and (!rstticker.active())) {
    Serial.println(F("[Sys] clear settings button pressed, hold it for a while to reset Wifi settings"));
    rstticker.attach(CFGPORTAL_BUTTON_TIME, rstwmcfg);
  }
  
//  if (pays != "") {
//    parsepl();
//  //  Serial.println(F("parsing"));
//  }

  if (clrflag) {
    digitalWrite(LED_BUILTIN, LOW);
    ld.print(F("reconfig"), 1);
    if (DISP_AMOUNT == 2) {
      ld.print(F(" button "), 2);
    }
    webSocket.disconnect();
    WiFi.disconnect();
    //WiFiManager wifiManager;
    //wifiManager.resetSettings();
    delay(3000);
    digitalWrite(LED_BUILTIN, HIGH);
    ESP.restart();
  }

  if (reconnflag) {
    reconnflag = false;
    webSocket.disconnect();
    subsidx = 0;  //no symbols subscribed
    delay(1000);
    webSocket.beginSSL(APISRV, APIPORT, APIURL);  //, "#INSECURE#"
    webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
  }

  //display prizze
  if (subsidx > 0) {  //if there is sth subscribed
    if (dispchng == true) {
      if (prevval != symarray[symidx].change) {
        prevval = symarray[symidx].change;
       // Serial.println(F("[LED] showing change"));
        //String temp = String(symarray[symidx].change, 1);
        //while (temp.length() < 6) {
        //  temp = " " + temp;
        //}
        //temp = "C24" + temp;
        snprintf(dbuff, sizeof(dbuff), "C%#8.1f", symarray[symidx].change);
        ld.print(dbuff, DISP_AMOUNT);
      }
    } else if (symarray[symidx].price != prevval) {
      //  Serial.println(F("[LED] showing price"));
      prevval = symarray[symidx].price;

      //lets find how many decimal places we need
      needdeci = 0;
      temppr = prevval;
      while ((temppr < 100) and (needdeci < 7)) {       //we want to display decimals only for for numbers <100
        needdeci ++;                                    //and we want more decimals for small numbers
        temppr = temppr * 10;
      }
      //ld.print(String(prevval, needdeci), DISP_AMOUNT); //print needdeci decimal places
      snprintf(dbuff, sizeof(dbuff), "%#.*f", needdeci, symarray[symidx].price);
      ld.print(dbuff, DISP_AMOUNT);
      
    }
    if (symidx != prevsymidx) { //symbol changed, display it
      if (DISP_AMOUNT == 2) {
        //show the symbol only if we have two displays
        Serial.println(F("[LED] showing symbol"));
        //ld.print(' ' + symarray[symidx].symbol + ' ', 1); //print on 1st modules
        snprintf(dbuff, sizeof(dbuff), " %s ", symarray[symidx].symbol);
        ld.print(dbuff, 1);
      }
    prevsymidx = symidx;
    }
  } else { //nothing subscribed, display message that trying connect
    if (prevval != -200) { // send it to display only once, not everytime the loop passes
      prevval = -200;
      prevsymidx = -1;
      Serial.println("[LED] showing cnct");
      ld.print("cnct api", 1);
      if (DISP_AMOUNT == 2) {
        ld.print("bitfinex", 2);
      }
    }
  }
}
