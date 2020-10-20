#include <FS.h>
#include <esp8266_hw_spi_max7219_7seg.h>  //https://github.com/BugerDread/esp8266-hw-spi-max7219-7seg
#include <EEPROM.h>
#include <Ticker.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <WebSocketsClient.h>     //https://github.com/Links2004/arduinoWebSockets   

// configuration
const char COMPILE_DATE[] PROGMEM = __DATE__ " " __TIME__;

const uint32_t SPI_SPEED = 8000000;           //SPI@8MHZ
const uint8_t SPI_CSPIN = 5;                  //SPI CS - may vary in older versions
const uint8_t DISP_BRGTH = 8;                 //brightness of the display
const uint8_t DISP_AMOUNT = 2;                //number of max 7seg modules connected

const uint8_t CFGPORTAL_TIMEOUT = 120;        //timeout for config portal in seconds
const uint8_t CFG_BUTTON = 0;                 //0 for default FLASH button on nodeMCU board
const uint8_t CFG_TIME = 5;                   //time [s] to hold CFG_BUTTON to activate cfg portal

const char APISRV[] = "api.bitfinex.com";
const uint16_t APIPORT = 443;
const char APIURL[] = "/ws/2";
const char REQ1[] = "{\"event\":\"subscribe\",\"channel\":\"ticker\",\"symbol\":\"t";
const char REQ2[] = "\"}";
const uint16_t WS_RECONNECT_INTERVAL = 5000;  // websocket reconnec interval
const uint8_t HB_TIMEOUT = 30;                //heartbeat interval in seconds

const size_t jcapacity = JSON_ARRAY_SIZE(2) + JSON_ARRAY_SIZE(10);   //jargest json

//define your default values here, if there are different values in config.json, they are overwritten.
char symbol[130] = "BTCUSD";
char sbrightness[4] = "8";
char symtime[4] = "3";
float price = -1;
float prevval = -1;
String pays = "";
bool clrflag = false;
bool shouldSaveConfig  = false; //flag for saving data
bool reconnflag = false;
bool dispchng = false;
int symidx, subsidx = 0;
int prevsymidx = -1;
int symnum = 0;

//array for ticker data
struct  symboldata_t {
  String symbol;
  long chanid;
  float price;
  float change;
  bool hb;
};

symboldata_t symarray[16];

Ticker symticker; //ticker to switch symbols
Ticker hbticker;
Ticker rstticker;
WebSocketsClient webSocket;
BgrMax7seg ld = BgrMax7seg(SPI_SPEED, SPI_CSPIN, DISP_AMOUNT); //init display

void rstwmcfg() {
  if (digitalRead(CFG_BUTTON) == LOW) {  //if still pressed
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
  ld.print(F(" config "), 1);
  ld.print(F("192.168.4.1"), 2);
}

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println(F("Should save config"));
  shouldSaveConfig = true;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
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
        pays = (char*)payload;
      }
      break;
    case WStype_BIN: {
        Serial.print("[WSc] get binary length: ");
        Serial.println(length);
      }
      break;
  }
}

bool parsepl() {
  DynamicJsonDocument jdoc(jcapacity);
  auto error = deserializeJson(jdoc, pays);   //deserialize
  pays = "";
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
      Serial.print("[Prs] Update, price: ");
      //tp = root[1][6];
      Serial.print((float)jdoc[1][6]);
      Serial.print(", change: ");
      Serial.println(100*(float)jdoc[1][5]);
    } 

    //[CHANNEL_ID,[BID,BID_SIZE,ASK,ASK_SIZE,DAILY_CHANGE,DAILY_CHANGE_PERC,LAST_PRICE,VOLUME,HIGH,LOW]]
    
    //find the symbol in array and set the prize if we have some prizze
    if ((newdata == true) or (temphb == true)) {  //if we have prize or hb
      for (byte i = 0; i < subsidx; i++) { //symnum -> subsidx   iterate the array of subscribed
        if (symarray[i].chanid == jdoc[0]) {  //we found it
          if (newdata == true) {
            symarray[i].price = jdoc[1][6]; 
            symarray[i].change = jdoc[1][5];
            symarray[i].change *= 100;
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
          Serial.println("[Prs] Got info, lets subscribe 1st ticker symbol: " + symarray[subsidx].symbol);
            webSocket.sendTXT(REQ1 + symarray[subsidx].symbol + REQ2);
        }
      } else if (jdoc["event"] == "subscribed")  {
        if (jdoc["chanId"] != false) {
          symarray[subsidx].chanid = jdoc["chanId"];
          Serial.print("[Prs] Ticker subscribe success, channel id: ");
          Serial.println(symarray[subsidx].chanid);
          subsidx++;  //move to next symbol in array
          if (subsidx < symnum) { //subscribe next
            Serial.print("[Prs] Lets subscribe next ticker symbol: ");
              webSocket.sendTXT(REQ1 + symarray[subsidx].symbol + REQ2);
          }
        } else {
          Serial.println("[Prs] Ticker subscribe failed");
        }
      }
      return true;
    }
  } else {      //deserializing error 
    return false;
  }
}

void hbcheck() {
  bool ok = true; // false; // for testing only
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
      symarray[symnum].symbol = s.substring(last);
    } else {
      Serial.print("[Setup] add symbol: ");
      Serial.println(s.substring(last, pos));
      symarray[symnum].symbol = s.substring(last, pos);
      last = pos + 1;
    }
    symarray[symnum].symbol.toUpperCase();
    symarray[symnum].hb = false;
    symnum++;
  }
}

bool initspiffs() {
  Serial.println(F("[SPIFFS] init started"));
  
  SPIFFSConfig cfg;
  cfg.setAutoFormat(false);   //disable atuformat on spiffs begin so we can detect and display info
  SPIFFS.setConfig(cfg);
  
  if (SPIFFS.begin()) {
    //SPIFFS ok
    Serial.println(F("[SPIFFS] ready"));
    return true;
  } else {
    //SPIFFS not ok, try to format it
    Serial.println(F("[SPIFFS] Formatting file system"));
    ld.print(F(" please "), 1);
    ld.print(F("  wait  "), 2);
    if (SPIFFS.format()) {
      //SPIFFS format OK
      Serial.println(F("[SPIFFS] format OK, FS ready"));
      return true;
    } else {
      //SPIFFS format failed
      Serial.println(F("[SPIFFS] format FAILED"));
      return false;
    }
  }
}

void cfgbywm() {
  // wifi manager = config save / load
  //read configuration from FS json

  if (initspiffs()) {
    Serial.println(F("mounted file system"));
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println(F("reading config file"));
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println(F("opened config file"));
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        DynamicJsonDocument json(jcapacity);
        auto error = deserializeJson(json, buf.get());   //deserialize
        if (!error) {
          Serial.println(F("\nparsed json"));
          strcpy(symbol, json["symbol"]);
          strcpy(sbrightness, json["sbrightness"]);
          strcpy(symtime, json["symtime"]);
        } else {
          Serial.println(F("failed to load json config"));
        }
        configFile.close();
      }
    }
  } else {
    Serial.println(F("failed to mount FS"));
  }
  //end read

  WiFiManager wifiManager;                                //Local intialization. Once its business is done, there is no need to keep it around

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_symbol("symbol", "bitfinex symbol(s)", symbol, 128);
  WiFiManagerParameter custom_sbrightness("sbrightness", "display brightness [1 - 16]", sbrightness, 2);
  WiFiManagerParameter custom_symtime("symtime", "time to cycle symbols", symtime, 3);

  wifiManager.setSaveConfigCallback(saveConfigCallback);  //set config save notify callback
  wifiManager.setAPCallback(configModeCallback);          //flash led if in config mode
  //add all your parameters here
  wifiManager.addParameter(&custom_symbol);
  wifiManager.addParameter(&custom_sbrightness);
  wifiManager.addParameter(&custom_symtime);
  //wifiManager.setMinimumSignalQuality();                //set minimu quality of signal so it ignores AP's under that quality, defaults to 8%
  wifiManager.setTimeout(CFGPORTAL_TIMEOUT);                          //sets timeout until configuration portal gets turned off useful to make it all retry or go to sleep in seconds

  if (!wifiManager.autoConnect("Bgr ticker", "btcbtcbtc")) {  //fetches ssid and pass and tries to connect if it does not connect it starts an access point with the specified name and goes into a blocking loop awaiting configuration
    Serial.println(F("Config portal timeout, trying to connect again"));
    ESP.reset();                                          //reset and try again, or maybe put it to deep sleep
  }

  Serial.println("WiFi connected...yeey :)");             //if you get here you have connected to the WiFi
  ld.print("  wifi  ", 1);
  ld.print(" online ", 2);

  parsesymbols(String(custom_symbol.getValue()));

  if  (((String(custom_sbrightness.getValue()).toInt()) <= 0) or ((String(custom_sbrightness.getValue()).toInt()) > 16) or
         ((String(custom_symtime.getValue()).toInt()) <= 0) or ((String(custom_symtime.getValue()).toInt()) > 999) or 
         (symnum == 0))
  {
    Serial.println(F("Parametters out of range, restart config portal"));
    WiFi.disconnect();
    ESP.reset(); 
  }

  strcpy(symbol, custom_symbol.getValue());               //read updated parameters
  strcpy(sbrightness, custom_sbrightness.getValue());               //read updated parameters
  strcpy(symtime, custom_symtime.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println(F("saving config"));
    DynamicJsonDocument json(jcapacity);
    //JsonObject& json = jsonBuffer.createObject();
    String upsym = symbol;
    upsym.toUpperCase();
    json["symbol"] = upsym;
    json["sbrightness"] = sbrightness;
    json["symtime"] = symtime;
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println(F("failed to open config file for writing"));
    }
    //json.printTo(Serial);
    serializeJson(json, Serial);
    serializeJson(json, configFile);
    //json.printTo(configFile);
    configFile.close();
    //end save
  }

  SPIFFS.end();
  Serial.println(F("[SPIFFS] end"));
}

void setup() {
  Serial.begin(115200);

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  //Serial.setDebugOutput(true);
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
  ld.print(F(" ticker "), 2);

  cfgbywm();

  long i = String(sbrightness).toInt();
  if (( i >= 1 ) and (i <= 16)) {
    ld.setBright(i - 1, ALL_MODULES);
    Serial.print(F("Setting display brightness to: "));
    Serial.println(i);
  }

  Serial.print("[Setup] My IP: ");
  Serial.println(WiFi.localIP());

  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(CFG_BUTTON, INPUT_PULLUP);  //button for reset of params
  //ld.clear(ALL_MODULES);

  Serial.print("[Setup] symnum = ");
  Serial.println(symnum);
  
  symticker.attach(String(symtime).toInt(), nextsymidx);
  Serial.print("[Setup] symbol cycle time: ");
  Serial.println(String(symtime));

  //start the connection
  webSocket.beginSSL(APISRV, APIPORT, APIURL);  //, "#INSECURE#"
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
  hbticker.attach(HB_TIMEOUT, hbcheck);
  Serial.print("[Setup] started HB check ticker, time: ");
  Serial.println(HB_TIMEOUT);
}

String temp;

void loop() {
  webSocket.loop();
   
  if ((digitalRead(CFG_BUTTON) == LOW) and (!rstticker.active())) {
    Serial.println(F("[Sys] clear settings button pressed, hold it for a while to reset Wifi settings"));
    rstticker.attach(CFG_TIME, rstwmcfg);
  }
  
  if (pays != "") {
    parsepl();
  //  Serial.println(F("parsing"));
  }

  if (clrflag) {
    digitalWrite(LED_BUILTIN, LOW);
    ld.print(F("reconfig"), 1);
    ld.print(F(" button "), 2);
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
        String temp = String(symarray[symidx].change, 1);
        while (temp.length() < 7) {
          temp = " " + temp;
        }
        temp = "CH" + temp;
        ld.print(temp, 2);
      }
    } else if (symarray[symidx].price != prevval) {
    //  Serial.println(F("[LED] showing price"));
      prevval = symarray[symidx].price;
      if (prevval >= 1000000) {
        ld.print(String(prevval, 0), 2); //print no decimal places
      } else if (prevval >= 10) {
        ld.print(String(prevval, 2), 2); //print 2 decimal places
      } else {
        //its smaller than 10, lets count how many decimals we need to display
        byte needdeci = 2;  //lets start with 2
        float temppr = prevval;
        while ((temppr < 10) and (needdeci < 7)) {
          needdeci++;
          temppr = temppr * 10;
        }
        ld.print(String(prevval, needdeci), 2); //print needdeci decimal places
      }
    }
    if (symidx != prevsymidx) { //symbol changed, display it
   //   Serial.println(F("[LED] showing symbol"));
      prevsymidx = symidx;
      ld.print(' ' + symarray[prevsymidx].symbol + ' ', 1); //print on 1st module
    }
  } else { //nothing subscribed, display message that trying connect
    if (prevval != -200) { // send it to display only once, not everytime the loop passes
      prevval = -200;
      prevsymidx = -1;
      Serial.println("[LED] showing cnct");
      ld.print("cnct to ", 1);
      ld.print("bitfinex", 2);
    }
  }
}
