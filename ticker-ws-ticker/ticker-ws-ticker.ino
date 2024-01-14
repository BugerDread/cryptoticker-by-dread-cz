//binance version
//https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-streams.md
//maybe better to use candlestick data?? bcs of low volume pairs, miniticker updates only if there is a change

//#define EZTIME_CACHE_NVS needs to be enabled in libraries/ezTime/src/ezTime.h for esp32
//#define EZTIME_CACHE_EEPROM needs to be enabled in libraries/ezTime/src/ezTime.h for esp8266


#include <esp8266_hw_spi_max7219_7seg.h>  //https://github.com/BugerDread/esp8266-hw-spi-max7219-7seg
#include <Ticker.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <WebSocketsClient.h>     //https://github.com/Links2004/arduinoWebSockets   
#include <EEPROM.h>
#include <ezTime.h>               //https://github.com/ropg/ezTime

// configuration
static const uint8_t DISP_AMOUNT = 1;                       //number of max 7seg modules connected
static const char ourtimezone[] PROGMEM = "Europe/Prague";  //official timezone names https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
//static const char NTP_SRV[] PROGMEM = "pool.ntp.org";     //eg pool.ntp.org
static const uint32_t NTP_SYNC_INTERVAL = 39600;   //base ntp interval
static const uint32_t NTP_SYNC_INTERVAL_RND = 3600;    //random ntp interval - ntp update after NTP_SYNC_INTERVAL+random(NTP_SYNC_INTERVAL_RND)
static const uint32_t SPI_SPEED = 2000000;                  //SPI speed in Hz (8MHZ may cause problems when usb voltage lower)
#ifdef ESP8266
  static const uint8_t SPI_CSPIN = 15;  //SPI CS for display
#else
  static const uint8_t SPI_CSPIN = 5;
  //these pins could be changed for ESP32 only, for ESP8266 these are: MOSI = GPIO13, SCLK = GPIO14
  static const uint8_t ESP32_SPI_CLKPIN = 6;
  static const uint8_t ESP32_SPI_MOSIPIN = 7;
  static const uint8_t ESP32_SPI_MISOPIN = 10;                    //not used but needs to be defined
#endif

static const uint8_t SYMBOL_MAX_LEN = 10;                   //max length of each symbol (like BTCUSD) in characters
static const uint8_t SYMBOL_MAX_COUNT = 16; 

static const uint8_t CFGPORTAL_TIMEOUT = 300;               //timeout for config portal in seconds
#ifdef ESP8266
  static const uint8_t CFGPORTAL_BUTTON = 0;                 //flash button on nodemcu boards (ESP8266)
#else
  static const uint8_t CFGPORTAL_BUTTON = 9;                 //flash button on ESP32C3 devkit
#endif
static const uint16_t CFGPORTAL_BUTTON_TIME = 3000;             //time [s] to hold CFGPORTAL_BUTTON to activate cfg portal
static const char CFGPORTAL_SSID[] = "Bgr ticker";
static const char CFGPORTAL_PWD[] = "btcbtcbtc";
static const char CFGPORTAL_CH24CHBX_VALUE[] = "X";         //value returned by the checkbox when checked
static const char HTML_CHECKBOX[] PROGMEM = "type=\"checkbox\"";
static const char HTML_CHECKBOX_CHECKED[] PROGMEM = "type=\"checkbox\" checked=\"true\"";

static const char CFG_DEF_SYMBOLS[] = "btcusdt ethusdt";
static const uint8_t CFG_SYMBOLS_LEN = ((SYMBOL_MAX_LEN + 1) * (SYMBOL_MAX_COUNT - 1)) + SYMBOL_MAX_LEN + 1;          //length of all symbols to show incl spaces = 16symbols = (15*7)+6 = 111characters
static const uint8_t CFG_DEF_BRIGHTNESS = 8;
static const uint8_t CFG_DEF_CYCLE_TIME = 3;
static const bool CFG_DEF_SHOW_CH24 = true;
static const uint8_t CFG_CHECKCUM_MAGIC = 42;               //some magic constant to make checksum invalif if data full of 0s

static const char APISRV[] = "stream.binance.com";
static const uint16_t APIPORT = 9443;
static const char APIURL[] PROGMEM = "/ws/%s@miniTicker";
static const char BINANCE_SUBSCRIBE[] PROGMEM = "{\"method\": \"SUBSCRIBE\",\"params\":[\"%s@miniTicker\"],\"id\": %u}";
static const char REQ1[] PROGMEM = "{\"event\":\"subscribe\",\"channel\":\"ticker\",\"symbol\":\"t%s\"}";
static const uint16_t WS_RECONNECT_INTERVAL = 5000;  // websocket reconnec interval
static const uint8_t HB_TIMEOUT = 30;                //heartbeat interval in seconds

static const size_t jcapacity = 256;   //size of json to parse ticker api (according to https://arduinojson.org/v6/assistant/ we need 192)
static bool clrflag = false;
static bool shouldSaveConfig  = false; //flag for saving data
static bool do_subscribe = false;
static int subsidx = 0;
static int symnum = 0;
static bool tzoneok = false;   //got valid tz data
static bool disptimenominus = false;
static uint8_t disppage = 0;    //controls what is on the display, 0 = time, 1 = 1st symbol price, 2 = 1st symbol change, 3 = 2nd symbol price, 4 = 2nd change etc..
static bool dispupd = true;
static const char COMPILE_DATE[] PROGMEM = __DATE__ " " __TIME__;

//array for ticker data
struct  symboldata_t 
{
    char symbol[SYMBOL_MAX_LEN + 1];
    double price;
    float change;
    bool updated;
};

symboldata_t symarray[SYMBOL_MAX_COUNT];

StaticJsonDocument<jcapacity> jdoc;
Ticker blinkticker;
WebSocketsClient webSocket;
Timezone myTZ;

#ifdef ESP8266
//ESP8266 SPI pins are: SCLK = GPIO14, MISO = GPIO12, MOSI = GPIO13
BgrMax7seg ld = BgrMax7seg(SPI_SPEED, SPI_CSPIN, DISP_AMOUNT); //init display
#else
//ESP32 alows you to redefine SPI pinout
BgrMax7seg ld = BgrMax7seg(SPI_SPEED, SPI_CSPIN, DISP_AMOUNT, ESP32_SPI_CLKPIN, ESP32_SPI_MOSIPIN, ESP32_SPI_MISOPIN); //init display
#endif

struct cfg_t {        
    char symbols[CFG_SYMBOLS_LEN + 1];
    int8_t brightness;
    int8_t cycle_time;
    bool show_ch24;
    uint8_t checksum;
} cfg;

uint8_t cfg_checksum() {                                                                 //PROTOZE structure padding ! nepocitej to jako kokot :D proc nefunguje kdyz se do cfg_t pridat int16 ?
    uint8_t checksum = CFG_CHECKCUM_MAGIC;                                             //init (magic) value, to make chcecksum of config full of 0s invalid
    for (size_t i = 0; i <= sizeof(cfg.symbols); i++) {                                //sum all chars in cfg.symbols
      checksum += cfg.symbols[i];
    }
    checksum += cfg.brightness;                                                         //add both integers
    checksum += cfg.cycle_time;
    if (cfg.show_ch24) {
      checksum++;
    }
    return checksum;                                                                    //and here is the checksum
}

bool show_cfg_check_checksum()
{
    if (cfg.checksum != cfg_checksum()) {
        Serial.println(F("Checksum [INVALID] !"));
        return false;
    }
    
    Serial.printf_P( PSTR("Checksum [VALID]: %u\nSymbols: %s\nCycle time: %u\nBrightness: %u\nShow 24hr change: %d\n"), \
      cfg.checksum, cfg.symbols, cfg.cycle_time, cfg.brightness, cfg.show_ch24);
    return true; 
}

bool get_cfg_eeprom()
{
    EEPROM.begin(sizeof(cfg));
    Serial.println(F("Loading cfg from EEPROM"));
    EEPROM.get(0, cfg);
    return show_cfg_check_checksum();
    EEPROM.end();
}

bool save_cfg_eeprom()
{
    EEPROM.begin(sizeof(cfg));
    Serial.println(F("Saving cfg to EEPROM"));
    cfg.checksum = cfg_checksum();
    show_cfg_check_checksum();
    EEPROM.put(0, cfg);
    if (!EEPROM.commit()) {
        Serial.println(F("EEPROM error, cant save cfg"));
        EEPROM.end();
        return false;
    }
    Serial.println(F("Cfg saved to EEPROM"));
    EEPROM.end();
    return true;   
}

void reboot()
{
  #ifdef ESP8266
    ESP.reset();
  #else
    ESP.restart();
  #endif
}

void nextsymidx() 
{
    uint32_t free_heap = ESP.getFreeHeap();
    Serial.printf_P(PSTR("[ESP] free memory: %dB %.1fkB\n"), free_heap, (float)free_heap / 1024);  //show free heap
    if ((cfg.show_ch24 == false) and (disppage != 0)) disppage++; //skip 24hr change if we dont want to see it
    if (++disppage > (symnum * 2)) disppage = 0;
    dispupd = true;
}

void configModeCallback(WiFiManager *myWiFiManager)
{
    if (DISP_AMOUNT == 2) {
        ld.print(F(" config "), 1);
    }
    ld.print(F("192.168.4.1"), DISP_AMOUNT);  //show IP of captive portal on 1st display if we have only one, show on 2nd if we have two
}

//callback notifying us of the need to save config
void saveConfigCallback()
{
    Serial.println(F("Should save config"));
    shouldSaveConfig = true;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t len) 
{
    switch (type) {
    case WStype_DISCONNECTED:
        Serial.println(F("[WSc] Disconnected!"));
        subsidx = 0;  //no symbols subscribed
        break;
    case WStype_CONNECTED: 
        Serial.printf_P( PSTR("[WSc] Connected to url: %s\n"), (char*)payload);
        //Serial.println((char*)payload);
        subsidx = 1;  //1st symbol (with index 0) was subscribed in the connection request
        do_subscribe = true;
        break;
    case WStype_TEXT: 
        //Serial.printf("[WSc] data: %s\n", (char*)payload);  //not using PROGMEM here - this occurs every price update
        parsepl((char*)payload, len);
        break;
    case WStype_BIN:
        Serial.printf_P( PSTR("[WSc] get binary length: %u\n"), len);
        break;
    case WStype_PING:
        // pong will be send automatically
        Serial.printf_P( PSTR("[WSc] get ping\n"));
        break;
    case WStype_PONG:
        // answer to a ping we send
        Serial.printf_P( PSTR("[WSc] got pong\n"));
        break;
    }
}

void subscribe_symbol(const char * const s, const uint16_t id)
{
    char txbuff[strlen_P(BINANCE_SUBSCRIBE) + SYMBOL_MAX_LEN + 1]; 
    snprintf_P(txbuff, sizeof(txbuff), BINANCE_SUBSCRIBE, s, id);
    //Serial.println(txbuff);
    webSocket.sendTXT(txbuff, strlen(txbuff));
}

bool parsepl(char * payload, const size_t len)
{
    DeserializationError error = deserializeJson(jdoc, payload, len);   //deserialize
    // Test if parsing succeeds
    if (error) {
        Serial.println(F("[Prs] deserialization error"));
        return false;
    }

//    const char* e = jdoc["e"]; // "24hrMiniTicker"
//    long long E = jdoc["E"]; // 1619987134136
    const char* s = jdoc["s"]; // "BTCUSDT"
    const char* c = jdoc["c"]; // "56809.88000000"
    const char* o = jdoc["o"]; // "57722.38000000"
//    const char* h = jdoc["h"]; // "57939.00000000"
//    const char* l = jdoc["l"]; // "56035.25000000"
//    const char* v = jdoc["v"]; // "36335.03358400"
//    const char* q = jdoc["q"]; // "2066963833.58576210"

    if ((s == nullptr) or (c == nullptr) or (o == nullptr)) {
        Serial.println(F("[Prs] s, c or o parametter not found"));
        return false;
    }

    Serial.printf("[Prs] %s: c = %s, o = %s\n", s, c, o);   //not using progmem bcs this is called often

    for (byte i = 0; i < subsidx; i++) {                              //iterate the array of subscribed to find it
        if (strcmp(strlwr((char*)s), symarray[i].symbol) == 0) {
            double d_c = atof(c);
            double d_o = atof(o);
            symarray[i].price = d_c;
            symarray[i].change = (d_c - d_o) / d_o * 100;
            symarray[i].updated = true;    //use incomming data as HB
            return true;
        }
    }

    Serial.print(F("[Prs] ERROR, unknown data: "));
    Serial.println(payload);
    return false;
}

void parsesymbols(const char * const s)
{
    //parse s to find all symbols separated by spaces
    //results:
    //symnum = count of symbols 
    //put each symbol into symarray[symnum].symbol
    //set for each symarray[symnum].hb = false
    symnum = 0;
    const char * pos_p = NULL;
    const char * last_p = s;
  
    do {                                                                                        
        pos_p = strchr(last_p, ' ');    //try to find ' ' (=space)
        
        if (((pos_p != NULL) and ((pos_p - last_p) <= SYMBOL_MAX_LEN) and ((pos_p - last_p) > 0))                              //found symbol of correct length
                or ((pos_p == NULL) and (strlen(last_p) <= SYMBOL_MAX_LEN) and (strlen(last_p) > 0))) {                      //found last symbol of correct length
            memset(symarray[symnum].symbol, '\0', sizeof(symarray[symnum].symbol));             //.symbol defined as [SYMBOL_MAX_LEN + 1]
            //strncpy(symarray[symnum].symbol, last_p, sizeof(symarray[symnum].symbol) - 1);
            if (pos_p != NULL) {
                strncpy(symarray[symnum].symbol, last_p, pos_p - last_p);
            } else {
                strncpy(symarray[symnum].symbol, last_p, strlen(last_p));
            }
            symarray[symnum].price = 0;
            symarray[symnum].change = 0;
            symarray[symnum].updated = false;
            symnum++;    
        } 
        
        if (pos_p != NULL) {
            last_p = pos_p + 1; //set laspt_p behind the space found
        }
    } while ((pos_p != NULL) and (symnum < SYMBOL_MAX_COUNT));
}

void cfgbywm(bool ondemand)
{
    char sbr[3];
    char sctime[3];
    size_t checkbox_len;

    checkbox_len = cfg.show_ch24 ? strlen_P(HTML_CHECKBOX_CHECKED) : strlen_P(HTML_CHECKBOX);
    char checkbox_buf[checkbox_len + 1];
    cfg.show_ch24 ? strcpy_P(checkbox_buf, HTML_CHECKBOX_CHECKED) : strcpy_P(checkbox_buf, HTML_CHECKBOX);

    itoa(cfg.brightness + 1, sbr, 10);    //internally is 0-15, wee need to make it 1-16 during setup
    itoa(cfg.cycle_time, sctime, 10);
    WiFiManager wifiManager;                                //Local intialization. Once its business is done, there is no need to keep it around
  
    // The extra parameters to be configured (can be either global or just in the setup). After connecting, parameter.getValue() will get you the configured value, id/name placeholder/prompt default length
    WiFiManagerParameter custom_symbol("symbol", "Binance symbol(s)", cfg.symbols, CFG_SYMBOLS_LEN);
    WiFiManagerParameter custom_sbrightness("sbrightness", "Display brightness [1 - 16]", sbr, 2);
    WiFiManagerParameter custom_symtime("symtime", "Interval to cycle symbols", sctime, 2);
     
    WiFiManagerParameter custom_ch24_checkbox("ch24_chbx", "Show 24hr change", CFGPORTAL_CH24CHBX_VALUE, strlen(CFGPORTAL_CH24CHBX_VALUE), checkbox_buf, WFM_LABEL_BEFORE);
  
    shouldSaveConfig = false;
    
    //wifiManager.setSaveConfigCallback(saveConfigCallback);  //set config save notify callback
    wifiManager.setSaveParamsCallback(saveConfigCallback);  //set config save notify callback
    wifiManager.setAPCallback(configModeCallback);          //flash led if in config mode
    wifiManager.addParameter(&custom_symbol);
    wifiManager.addParameter(&custom_sbrightness);
    wifiManager.addParameter(&custom_symtime);
    wifiManager.addParameter(&custom_ch24_checkbox);
    //wifiManager.setMinimumSignalQuality();                //set minimum quality of signal so it ignores AP's under that quality, defaults to 8%
    wifiManager.setTimeout(CFGPORTAL_TIMEOUT);                          //sets timeout until configuration portal gets turned off useful to make it all retry or go to sleep in seconds

    if (ondemand) {
        wifiManager.startConfigPortal(CFGPORTAL_SSID, CFGPORTAL_PWD); //start cfg ap to be able to reconfigure
        Serial.println(F("Ondemand config portal exited or timeout"));
    } else {
        if (!wifiManager.autoConnect(CFGPORTAL_SSID, CFGPORTAL_PWD)) {  //fetches ssid and pass and tries to connect if it does not connect it starts an access point with the specified name and goes into a blocking loop awaiting configuration
            Serial.println(F("Autoconnect config portal timeout, trying to connect again"));
            reboot();                                          //reset and try again, or maybe put it to deep sleep
        }
    }
  
    //params modified using WM, process them and save to EEPROM
    if (shouldSaveConfig) {
        memset(cfg.symbols, '\0', sizeof(cfg.symbols));
        strncpy(cfg.symbols, custom_symbol.getValue(), sizeof(cfg.symbols) - 1);              
        strlwr(cfg.symbols);    //uppercase symbols
        cfg.brightness = atoi(custom_sbrightness.getValue());               
        cfg.cycle_time = atoi(custom_symtime.getValue());
        parsesymbols(cfg.symbols);    //needs to be also here because we are checking that symnum > 0 (that we have some valid symbols)
        cfg.show_ch24 = (strcmp(custom_ch24_checkbox.getValue(), CFGPORTAL_CH24CHBX_VALUE) == 0);
  
        //check that everything is valid and save
        if  (!((cfg.brightness <= 0) or (cfg.brightness > 16)
                or (cfg.cycle_time <= 0) or (cfg.cycle_time > 99) or (symnum == 0))) {
            Serial.println(F("Parametters OK, saving"));
            cfg.brightness = cfg.brightness - 1;  //brightness range 0-15, but atoi return 0 when error so we made it 1-16 and here is the correction
            save_cfg_eeprom();
            //WiFi.disconnect();
            //reboot(); 
        } else {
          Serial.println(F("Parametters out of range, NOT saving"));
        }
    }

    if (ondemand) {
      reboot();  //reboot to apply changes
    }

    Serial.println(F("WiFi connected... :)"));             //if you get here you have connected to the WiFi
    ld.print(F("  wifi  "), 1);
    if (DISP_AMOUNT == 2) {
        ld.print(F(" online "), 2);
    }
}

static unsigned long btn_pressed_millis = 0;
void check_cfg_reset_button() 
{
//#ifdef ESP8266
    //cfg reset button 
    if (digitalRead(CFGPORTAL_BUTTON) == HIGH) {
      //btn not pressed
      btn_pressed_millis = 0;
      return;
    }

    if (btn_pressed_millis == 0) {
      //btn pressed for first time
      btn_pressed_millis = millis();
      return;
    }

    if ((millis() - btn_pressed_millis) < CFGPORTAL_BUTTON_TIME) {
      //btn needs to be pressed longer
      return;
    }

#ifdef ESP8266
    digitalWrite(LED_BUILTIN, LOW);
#endif        
    ld.print(F("reconfig"), 1);
    if (DISP_AMOUNT == 2) {
      ld.print(F(" button "), 2);
    }
    webSocket.disconnect();
    cfgbywm(true);
    Serial.println(F("[Sys] Ondemand reconfig success, restart to apply changes"));
    reboot();   //restart - quick but little bit dirty way to apply changes, may be improved in future
}

void show_cnct_api() {
  Serial.println(F("[LED] showing cnct"));
  ld.print(F("cnct api"), 1);
  if (DISP_AMOUNT == 2) {
    ld.print(F("binance "), 2);
  }
}

void check_subscribe() {
  //subscribe all symbols but wait 1 second between each
  static unsigned long subs_prev_millis = 0;    //static variable that survives all the time
  
  if (!(do_subscribe and ((millis() - subs_prev_millis) > 1000))) {
      return;
  }

  if (subsidx == symnum) {
      Serial.println("No more symbols to subscribe");
      do_subscribe = false;
      dispupd = true;
      return;      
  }

  subs_prev_millis = millis();
  Serial.printf("Lets subscribe %u. symbol: %s\n", subsidx + 1, symarray[subsidx].symbol);

  //here call the subscribe
  subscribe_symbol(symarray[subsidx].symbol, subsidx);
  subsidx++;
}

void connect_binance() {
  //start the connection
  char api_url_buff[strlen_P(APIURL) + SYMBOL_MAX_LEN + 1];
  snprintf_P(api_url_buff, sizeof(api_url_buff), APIURL, symarray[0].symbol); //subscribe 1st symbol
  Serial.printf("API url: %s\n", api_url_buff);
  webSocket.beginSSL(APISRV, APIPORT, api_url_buff); 
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
  //webSocket.enableHeartbeat(uint32_t pingInterval, uint32_t pongTimeout, uint8_t disconnectTimeoutCount);
  //pingInterval uint32_t how often ping will be sent
  //pongTimeout uint32_t millis after which pong should timout if not received
  //disconnectTimeoutCount uint8_t how many timeouts before disconnect, 0=> do not disconnect
  webSocket.enableHeartbeat(10000, 9000, 3); //maybe this can save us the HB timer and troubles 
}

void ntp_init() {
  Serial.print(F("[Setup] Syncing time with NTP servers"));

  setDebug(INFO);
  //setServer(NTP_SRV);
  //sync ntp clock
  while (!waitForSync(3)) {
    Serial.print(F("."));
  }
  Serial.println(F("."));
  
  if (!myTZ.setCache(sizeof(cfg) + 1)) {
    Serial.println(F("[Setup] Getting timezone info from server"));
    if (myTZ.setLocation(FPSTR(ourtimezone))) {
      Serial.println(F("[Setup] Timezone data OK"));
      tzoneok = true;
    } else {
      Serial.println(F("[Setup] Timezone data ERROR"));
      ld.print(F("time err"), 1);
    }
  } else {
    Serial.println(F("[Setup] Reusing timezone info from eeprom cache"));
    tzoneok = true;
  }
  //query ntp server every ~18hours (default is 30minutes)
  uint16_t ntpi = NTP_SYNC_INTERVAL + random(NTP_SYNC_INTERVAL_RND);
  Serial.println((String)F("[Setup] setting NTP interval to ") + String(ntpi) + (String)F(" seconds"));
  setInterval(ntpi);
}

void setup() 
{
    Serial.begin(115200);
    Serial.println(F("[Setup] Boot!"));
    Serial.print(F("Compile date: "));
    Serial.println(FPSTR(COMPILE_DATE));
    
#ifdef ESP8266
    // initialize digital pin LED_BUILTIN as an output and turn off the LED
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
#endif    

    pinMode(CFGPORTAL_BUTTON, INPUT_PULLUP);  //button for reset of params
      
    if (WiFi.getMode() != WIFI_STA) {
        Serial.println(F("Set WiFi mode to STA"));
        WiFi.mode(WIFI_STA); // set STA mode, esp defaults to STA+AP
    } else {
        Serial.println(F("WiFi already in STA mode"));
    }
  
    /* init displays and set the brightness min:1, max:15 */
    ld.init();
    ld.setBright(CFG_DEF_BRIGHTNESS, ALL_MODULES);
    ld.print(F("dread.cz "), 1);
    if (DISP_AMOUNT == 2) {
        ld.print(F(" ticker "), 2);
    }
    delay(1000);
    
    //get configuration from eeprom
    //EEPROM.begin(512);
    if (!get_cfg_eeprom()) {
        //configuration in eeprom is not valid - we need to create default one
        Serial.println(F("EEPROM checksum invalid, creating default configuration"));
        memset(cfg.symbols, '\0', sizeof(cfg.symbols));
        strncpy (cfg.symbols, CFG_DEF_SYMBOLS, sizeof(cfg.symbols) - 1);
        cfg.brightness = CFG_DEF_BRIGHTNESS;
        cfg.cycle_time = CFG_DEF_CYCLE_TIME;
        cfg.show_ch24 = CFG_DEF_SHOW_CH24;
        save_cfg_eeprom();
    }
  
    cfgbywm(false);
  
    parsesymbols(cfg.symbols);
  
    ld.setBright(cfg.brightness, ALL_MODULES);
    Serial.printf_P(PSTR("Setting display brightness to: %u\n"), cfg.brightness);
    Serial.print(F("[Setup] My IP: "));
    Serial.println(WiFi.localIP());
    Serial.printf_P(PSTR("[Setup] symnum = %u\n"), symnum);

    ntp_init();
    connect_binance();    //connect ws api server
    Serial.printf_P(PSTR("[Setup] symbol cycle time: %u\n"), cfg.cycle_time);
}

void blinkf() {
  disptimenominus = true;
  blinkticker.detach();
}

uint32_t cycletime;
static uint8_t symi;
static uint16_t timcounter = 0;
static char dbuff[8+8+1]; //8 chars + 8 decimal dots + zero terminator = thats our display buffer
void loop()
{
    webSocket.loop();
    check_cfg_reset_button();
    check_subscribe();  
    events();

    if (secondChanged()) {
      myTZ.dateTime();    //needs to be here otherwise secondChange is not cleared
      //disppage == 0 ? cycletime = cfg.cycle_time * 2 : cycletime = cfg.cycle_time;
      //if (++timcounter >= cycletime) {
      if (++timcounter >= cfg.cycle_time) {
        timcounter = 0;
        nextsymidx();
      }
      if (disppage == 0) {  //show time
        dispupd = false;  //update display if showing time
        //Serial.println(F("[LED] showing time"));
        ld.print(myTZ.dateTime("H-i-s"), 1);
        if (DISP_AMOUNT >= 2) {
          ld.print("        ", 2);
          ld.print(myTZ.dateTime("j.n.Y"), 2);
        }
        disptimenominus = false;
        blinkticker.attach(0.5, blinkf);
        return;
      }
    }

    if (disppage == 0) {  //show time without minuses
      if (disptimenominus) {
        disptimenominus = false;
        ld.print(myTZ.dateTime("H i s"), 1);
      }
      return;
    }  

    if (subsidx == 0) {
      //not subscribed / no connection etc
      if (dispupd) {
        dispupd = false;
        show_cnct_api();
      }
      return;
    }

    symi = (disppage - 1) / 2;    //index of symbol in symarray

    if (!((symarray[symi].updated) or dispupd)) {
      return; //do not update display if nothing changed
    }
    
    if ((disppage % 2) == 1) {
      //show price
      //but show symbol name on 1st display but if we have two displays and diplay is for update first
      if ((DISP_AMOUNT == 2) and dispupd) {
        //Serial.println(F("[LED] showing symbol name"));
        Serial.println(symarray[symi].symbol);
        snprintf(dbuff, sizeof(dbuff), "%-8s", symarray[symi].symbol);
        ld.print(dbuff, 1);
      }
      
      //Serial.println(F("[LED] showing price"));
      int8_t decimals = 3 - log10(symarray[symi].price);                  //3 valid decimals
      if (decimals < 0) decimals = 0;                                       //but limit - we cant have negative decimal count
      if (decimals > 7) decimals = 7;                                       //and we have only 8 digit display = max 7 decimals
      snprintf(dbuff, sizeof(dbuff), "%.*f", decimals, symarray[symi].price);
      ld.print(dbuff, DISP_AMOUNT);
      
    } else {
      //show change
      //Serial.println(F("[LED] showing change"));
      snprintf(dbuff, sizeof(dbuff), "C%#8.1f", symarray[symi].change);
      ld.print(dbuff, DISP_AMOUNT);
    }

    dispupd = false;
    symarray[symi].updated = false;
}
