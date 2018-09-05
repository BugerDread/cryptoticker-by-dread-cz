#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <esp8266-hw-spi-max7219-7seg.h>
#include <Ticker.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <Hash.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager

// configuration
#define SPI_SPEED   8000000 //SPI@8MHZ
#define SPI_CSPIN   5       //SPI CS=GPIO5
#define USE_SERIAL  Serial
#define LED_BLINKMS 1       //led blink duration [ms]
#define DISP_BRGTH  8      //brightness of the display
#define DISP_AMOUNT 2       //number of max 7seg modules connected
#define WS_RECONNECT_INTERVAL 5000  // websocket reconnec interval

const char REQ1[] PROGMEM = "{\"event\":\"subscribe\",\"channel\":\"ticker\",\"symbol\":\"t";
const char REQ2[] PROGMEM = "\"}";

//define your default values here, if there are different values in config.json, they are overwritten.
char symbol[66] = "BTCUSD";
char sbrightness[4] = "8";
char symtime[4] = "3";

// ESP8266WiFiMulti WiFiMulti;
WebSocketsClient webSocket;
BgrMax7seg ld = BgrMax7seg(SPI_SPEED, SPI_CSPIN, DISP_AMOUNT); //init display
// word chid = 0; //ws channel id
float price = -1;
float prevprice = -1;
String pays = "";

//flag for saving data
bool shouldSaveConfig = false;

//array for ticker data
struct symboldata_t {
  String symbol;
  long chanid;
  float price;
};

int symnum = 0;
symboldata_t symarray[16];

int symidx, subsidx = 0;
int prevsymidx = -1;
Ticker symticker; //ticker to switch symbols

void nextsymidx () {
  //move to next symbol
  symidx++;
  if (symidx >= subsidx) {
    symidx = 0;
  }
}

void configModeCallback(WiFiManager *myWiFiManager) {
  ld.print(" config ", 1);
  ld.print("192.168.4.1", 2);
}

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println(F("Should save config"));
  shouldSaveConfig = true;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED: {
        USE_SERIAL.println(F("[WSc] Disconnected!"));
        subsidx = 0;  //no symbols subscribed
      }
      break;
    case WStype_CONNECTED: {
        USE_SERIAL.print(F("[WSc] Connected to url: "));
        USE_SERIAL.println((char*)payload);
      }
      break;
    case WStype_TEXT: {
        USE_SERIAL.print(F("[WSc] data: "));
        USE_SERIAL.println((char*)payload);
        pays = (char*)payload;
      }
      break;
    case WStype_BIN: {
        USE_SERIAL.print(F("[WSc] get binary length: "));
        USE_SERIAL.println(length);
      }
      break;
  }
}

void parsepl() {
  //USE_SERIAL.println(pays);
//  if (pays.indexOf("],[") != -1) {
//    //     USE_SERIAL.println(F("[Prs] probably snaphost, lets short&fix it"));
//    pays = pays.substring(0, pays.indexOf("],["));
//    pays += "]]]";
//    // USE_SERIAL.println(pays);
//  }

  //StaticJsonBuffer<512> jsonBuffer;
  DynamicJsonBuffer jsonBuffer(512);
  //check if input is array or object
  JsonObject& root = jsonBuffer.parseObject(pays);
  // Test if parsing succeeds.
  if (root.success()) {
    //   USE_SERIAL.println(F("[Prs] its json object"));
    if (root["event"] == "info") {
      if (subsidx == 0) {
        USE_SERIAL.print(F("[Prs] Got info, lets subscribe 1st ticker symbol: "));
        String request = FPSTR(REQ1);
        request += symarray[subsidx].symbol;
        request += FPSTR(REQ2);
        webSocket.sendTXT(request);
      }
    } else if (root["event"] == "subscribed")  {
      if (root["chanId"] != false) {
        symarray[subsidx].chanid = root["chanId"];
        USE_SERIAL.print(F("[Prs] Ticker subscribe success, channel id: "));
        USE_SERIAL.println(symarray[subsidx].chanid);
        subsidx++;  //move to next symbol in array
        if (subsidx < symnum) { //subscribe next
          USE_SERIAL.print(F("[Prs] Lets subscribe next ticker symbol: "));
          String request = FPSTR(REQ1);
          request += symarray[subsidx].symbol;
          request += FPSTR(REQ2);
          webSocket.sendTXT(request);
        }
      } else {
        USE_SERIAL.println(F("[Prs] Ticker subscribe failed"));
      }
    }
  } else {
    JsonArray& root = jsonBuffer.parseArray(pays);
    // Test if parsing succeeds.
    if (root.success()) {
      //   USE_SERIAL.println(F("[Prs] its an array"));
      float tp = 0.0;
      if (root[1] == "hb") {  //its a heartbeat
        //  USE_SERIAL.println(F("[Prs] Heartbeat!"));
      } else if (root[1][6] > 0.0) { // new prize
        USE_SERIAL.print(F("[Prs] Update, new price: "));
        tp = root[1][6];
        USE_SERIAL.println(tp);
      } 

      //root[0] contains chanid of received message
      //find the symbol in array and set the prize if we have some prizze
      if (tp != 0.0) {  //if we have prize
        for (byte i = 0; i < symnum; i++) { //iterate the array
          if (symarray[i].chanid == root[0]) {  //we found it
            symarray[i].price = tp;
            // USE_SERIAL.print(F("[Prs] array updated, i = "));
            // USE_SERIAL.println(i);
            break;
          }
        }
      }
    }
  }
  pays = "";
  //blinkled();
}

void parsesymbols(String s) {
  //lets count symbols and put them into symarray
  int last = 0;
  int pos = 0;
  symnum = 0;
  while ((pos != -1) and (symnum <= 16) and (s.length() > 0)) {
    pos = s.indexOf(' ', last);
    if (pos == -1) { //last symbol
      USE_SERIAL.print(F("[Setup] last symbol: "));
      USE_SERIAL.println(s.substring(last));
      symarray[symnum].symbol = s.substring(last);
    } else {
      USE_SERIAL.print(F("[Setup] add symbol: "));
      USE_SERIAL.println(s.substring(last, pos));
      symarray[symnum].symbol = s.substring(last, pos);
      last = pos + 1;
    }
    symarray[symnum].symbol.toUpperCase();
    symnum++;
  }
}

void setup() {
  // USE_SERIAL.begin(921600);
  USE_SERIAL.begin(115200);

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  USE_SERIAL.setDebugOutput(true);
  USE_SERIAL.println(F("[Setup] Boot!"));

  /* init displays and set the brightness min:1, max:15 */
  ld.init();
  ld.setBright(DISP_BRGTH, ALL_MODULES);
  ld.print("boot v14", 1);
  ld.print("connect ", 2);

  //WiFi.setPhyMode(WIFI_PHY_MODE_11G);

  // wifi manager = config save / load
  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          strcpy(symbol, json["symbol"]);
          strcpy(sbrightness, json["sbrightness"]);
          strcpy(symtime, json["symtime"]);
        } else {
          Serial.println("failed to load json config");
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_symbol("symbol", "bitfinex symbol", symbol, 64);
  WiFiManagerParameter custom_sbrightness("sbrightness", "display brightness [1 - 16]", sbrightness, 2);
  WiFiManagerParameter custom_symtime("symtime", "time to cycle symbols", symtime, 3);

  WiFiManager wifiManager;                                //Local intialization. Once its business is done, there is no need to keep it around
  wifiManager.setSaveConfigCallback(saveConfigCallback);  //set config save notify callback
  wifiManager.setAPCallback(configModeCallback);          //flash led if in config mode
  //add all your parameters here
  wifiManager.addParameter(&custom_symbol);
  wifiManager.addParameter(&custom_sbrightness);
  wifiManager.addParameter(&custom_symtime);
  //wifiManager.setMinimumSignalQuality();                //set minimu quality of signal so it ignores AP's under that quality, defaults to 8%
  //wifiManager.setTimeout(120);                          //sets timeout until configuration portal gets turned off useful to make it all retry or go to sleep in seconds
  if (!wifiManager.autoConnect("Bgr ticker", "btcbtcbtc")) {  //fetches ssid and pass and tries to connect if it does not connect it starts an access point with the specified name and goes into a blocking loop awaiting configuration
    Serial.println("failed to connect and hit timeout");
    delay(1000);
    digitalWrite(LED_BUILTIN, HIGH);
    ESP.reset();                                          //reset and try again, or maybe put it to deep sleep
  }

  Serial.println("WiFi connected...yeey :)");             //if you get here you have connected to the WiFi

  ld.print("  wifi  ", 1);
  ld.print(" online ", 2);

  parsesymbols(String(custom_symbol.getValue()));

  while (((String(custom_sbrightness.getValue()).toInt()) <= 0) or ((String(custom_sbrightness.getValue()).toInt()) > 16) or
         ((String(custom_symtime.getValue()).toInt()) <= 0) or ((String(custom_symtime.getValue()).toInt()) > 999) or 
         (symnum == 0))
  {
    Serial.println(F("Parametters out of range, restart config portal"));
    ld.print("wifi ok ", 1);
    ld.print("cfg err ", 2);
    wifiManager.startConfigPortal("Bgr ticker", "btcbtcbtc");
    parsesymbols(String(custom_symbol.getValue()));
  }

  strcpy(symbol, custom_symbol.getValue());               //read updated parameters
  strcpy(sbrightness, custom_sbrightness.getValue());               //read updated parameters
  strcpy(symtime, custom_symtime.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    String upsym = symbol;
    upsym.toUpperCase();
    json["symbol"] = upsym;
    json["sbrightness"] = sbrightness;
    json["symtime"] = symtime;
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }
    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }
  long i = String(sbrightness).toInt();
  if (( i >= 0 ) and (i <= 15)) {
    ld.setBright(i - 1, ALL_MODULES);
    Serial.print("Setting display brightness to: ");
    Serial.println(i);
  }

  Serial.println("local ip");
  Serial.println(WiFi.localIP());

  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(0, INPUT_PULLUP);  //button for reset of params
  //ld.clear(ALL_MODULES);

  
  USE_SERIAL.print(F("[Setup] symnum = "));
  USE_SERIAL.println(symnum);

  USE_SERIAL.print(F("[Setup] symbol cycle time: "));
  USE_SERIAL.println(String(symtime).toInt());
  symticker.attach(String(symtime).toInt(), nextsymidx);

  //start the connection
  webSocket.beginSSL("api.bitfinex.com", 443, "/ws/2");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
}

void loop() {
  webSocket.loop();
  
  if (digitalRead(0) == LOW) {
    Serial.println(F("[Sys] clear settings button pressed"));
    digitalWrite(LED_BUILTIN, LOW);
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    delay(2000);
    digitalWrite(LED_BUILTIN, HIGH);
    ESP.reset();
  }
  
  if (pays != "") {
    parsepl();
  }

  //display prizze
  if (subsidx > 0) {  //if there is sth subscribed
    if (symarray[symidx].price != prevprice) {
      prevprice = symarray[symidx].price;
      if (prevprice >= 1000000) {
        ld.print(String(prevprice, 0), 2); //print no decimal places
      } else if (prevprice >= 10) {
        ld.print(String(prevprice, 2), 2); //print 2 decimal places
      } else {
        //its smaller than 10, lets count how many decimals we need to display
        byte needdeci = 2;  //lets start with 2
        float temppr = prevprice;
        while ((temppr < 10) and (needdeci < 7)) {
          needdeci++;
          temppr = temppr * 10;
        }
        ld.print(String(prevprice, needdeci), 2); //print needdeci decimal places
      }
    }
    if (symidx != prevsymidx) { //symbol changed, display it
      prevsymidx = symidx;
      ld.print(' ' + symarray[prevsymidx].symbol + ' ', 1); //print on 1st module
    }
  } else { //nothing subscribed, display message that trying connect
    if (prevprice != -2) { // send it to display only once, not everytime the loop passes
      prevprice = -2;
      prevsymidx = -1;
      ld.print("cnct to ", 1);
      ld.print("bitfinex", 2);
    }
  }
}


