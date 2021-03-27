#include <esp8266_hw_spi_max7219_7seg.h>  //https://github.com/BugerDread/esp8266-hw-spi-max7219-7seg
#include <Ticker.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <ESP8266WiFi.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <WebSocketsClient.h>     //https://github.com/Links2004/arduinoWebSockets   
#include <EEPROM.h>

// configuration
static const char COMPILE_DATE[] PROGMEM = __DATE__ " " __TIME__;

static const uint32_t SPI_SPEED = 8000000;           //SPI@8MHZ
static const uint8_t SPI_CSPIN = 15;                  //SPI CS - may vary in older versions
static const uint8_t DISP_AMOUNT = 1;                //number of max 7seg modules connected

static const uint8_t CFGPORTAL_TIMEOUT = 120;        //timeout for config portal in seconds
static const uint8_t CFGPORTAL_BUTTON = 0;                 //0 for default FLASH button on nodeMCU board
static const uint8_t CFGPORTAL_BUTTON_TIME = 5;                   //time [s] to hold CFGPORTAL_BUTTON to activate cfg portal
static const char CFGPORTAL_SSID[] = "Bgr ticker";
static const char CFGPORTAL_PWD[] = "btcbtcbtc";

static const char CFG_DEF_SYMBOLS[] = "BTCUSD";
static const uint8_t CFG_SYMBOLS_LEN = 111;          //length of all symbols to show incl spaces = 16symbols = (15*7)+6 = 111characters
static const uint8_t CFG_DEF_BRIGHTNESS = 8;
static const uint8_t CFG_DEF_CYCLE_TIME = 3;

static const char APISRV[] = "api.bitfinex.com";
static const uint16_t APIPORT = 443;
static const char APIURL[] = "/ws/2";
static const char REQ1[] PROGMEM = "{\"event\":\"subscribe\",\"channel\":\"ticker\",\"symbol\":\"t%s\"}";
static const uint16_t WS_RECONNECT_INTERVAL = 5000;  // websocket reconnec interval
static const uint8_t HB_TIMEOUT = 30;                //heartbeat interval in seconds

static const uint8_t SYMBOL_LEN = 6;                 //length of each symbol (like BTCUSD) in characters

static const size_t jcapacity = JSON_ARRAY_SIZE(2) + JSON_ARRAY_SIZE(10);   //size of json to parse ticker api (according to https://arduinojson.org/v6/assistant/)

static const float INSANE_PREVVAL = -1e999; //number which we mos tlikely never reach, used invalidate prevval and redraw display
static float prevval = INSANE_PREVVAL;
static bool clrflag = false;
static bool shouldSaveConfig  = false; //flag for saving data
static bool reconnflag = false;
static bool dispchng = false;
static int symidx, subsidx = 0;
static int prevsymidx = -1;
static const int PREVSYMIDX_NOREDRAW = -100;  //do not refresh display every time the loop pases, nothing subscribed and prevsimidx set to this value
static int symnum = 0;

//array for ticker data
struct  symboldata_t 
{
    char symbol[SYMBOL_LEN + 1];
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

struct cfg_t 
{        
    char symbols[CFG_SYMBOLS_LEN + 1];
    int8_t brightness;
    int8_t cycle_time;
    uint8_t checksum;
} cfg;

uint8_t cfg_checksum()                                                                  //PROTOZE structure padding ! nepocitej to jako kokot :D proc nefunguje kdyz se do cfg_t pridat int16 ?
{
    uint8_t checksum = 42;                                                              //init (magic) value, to make chcecksum of config full of 0s invalid
    for (size_t i = 0; i <= sizeof(cfg.symbols); i++) {                                //sum all chars in cfg.symbols
      checksum += cfg.symbols[i];
    }
    checksum += cfg.brightness;                                                         //add both integers
    checksum += cfg.cycle_time;
    return checksum;                                                                    //and here is the checksum
}

bool get_cfg_eeprom()
{
    EEPROM.get(0, cfg);
    Serial.println(F("Loading config from EEPROM"));
    Serial.print(F("Checksum "));
    if (cfg.checksum != cfg_checksum()) {
        Serial.println(F("[INVALID] !"));
        return false;
    }
    Serial.printf_P( PSTR("[VALID]: %u\n"), cfg.checksum);
    Serial.printf_P( PSTR("Symbols: %s\n"), cfg.symbols);
    Serial.printf_P( PSTR("Cycle time: %u\n"), cfg.cycle_time);
    Serial.printf_P( PSTR("Brightness: %u\n"), cfg.brightness); 
    return true;
}

bool save_cfg_eeprom()
{
    cfg.checksum = cfg_checksum();
    EEPROM.put(0, cfg);
    if (!EEPROM.commit()) {
        Serial.println(F("EEPROM error, cant save config"));
        return false;
    }
    Serial.println(F("Config saved to EEPROM"));
    return true;   
}

void rstwmcfg() 
{
    if (digitalRead(CFGPORTAL_BUTTON) == LOW) {  //if still pressed
        clrflag = true;
        return;
    } 
    //not pressed anymore
    rstticker.detach();
}

void nextsymidx() 
{
    prevval = INSANE_PREVVAL;         //change prevval to insane value to ensure it will redraw display
    
    if (dispchng == false) {          //are we ahowing 24h change?
        dispchng = true;              //no, so lets set flag to display it
        return;                       //we are done
    } 
    
    //move to next symbol if we already show change
    dispchng = false;                 //want to show the prizzeee first
    symidx++;                         //move to next symbol 
    if (symidx >= subsidx) {          //go to the 1st symbol when we are out of range
        symidx = 0;
    }
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
        break;
    case WStype_TEXT: 
        Serial.printf("[WSc] data: %s\n", (char*)payload);  //not using PROGMEM here - this occurs every price update
        //pays = (char*)payload;
        parsepl((char*)payload, len);
        break;
    case WStype_BIN:
        Serial.printf_P( PSTR("[WSc] get binary length: %u\n"), len);
        break;
    }
}

void subscribe_symbol(const char * const s)
{
    char txbuff[strlen(REQ1) + 6 + 1];
    snprintf_P(txbuff, sizeof(txbuff), REQ1, s);
    //Serial.println(txbuff);
    webSocket.sendTXT(txbuff, strlen(txbuff));
}

bool parsepl(const char * payload, const size_t len)
{
    //[CHANNEL_ID,[BID,BID_SIZE,ASK,ASK_SIZE,DAILY_CHANGE,DAILY_CHANGE_PERC,LAST_PRICE,VOLUME,HIGH,LOW]] - price update
    //[CHANNEL_ID, "hb"] - for heartbeat
    //=> if (jdoc[1] == "hb") {       //its a heartbeat
    //=> if (jdoc[1][6] != nullptr) { // new prize
    
    DeserializationError error = deserializeJson(jdoc, payload, len);   //deserialize
    // Test if parsing succeeds
    if (error) {
        Serial.println(F("[Prs] deserialization error"));
        return false;
    }
  
    //check if its a json with price update or heartbeat
    if ((jdoc[0] != nullptr) and (((jdoc[1][6] != nullptr) and (jdoc[1][5] != nullptr)) or (jdoc[1] == "hb"))) {                 //if we have prize or hb
        for (byte i = 0; i < subsidx; i++) {                              //iterate the array of subscribed
            if (symarray[i].chanid == jdoc[0]) {                            //we found it
                if (((jdoc[1][5]) != nullptr) and (jdoc[1][6] != nullptr)) {  //its prize so update it incl 24hr change
                    symarray[i].price = jdoc[1][6]; 
                    symarray[i].change = (float)(jdoc[1][5]) * 100.0;
                } 
                symarray[i].hb = true;                                  //we need to set this flag in case of price update and also hb
                return true;                                            //were done
            }
        }
        Serial.println(F("[Prs] ERROR - we most probaly got prize update for symbol we never subscribed"));
        return false;                                               //we are also done but not found the ID
    }
  
    //check if its initial server info
    if ((jdoc["event"] == F("info")) and (subsidx == 0)) {
        //yes it is initial server subscribe info and we have nothing subscribed
        Serial.printf_P( PSTR("[Prs] Got info, lets subscribe 1st ticker symbol: %s\n"), symarray[subsidx].symbol);
        subscribe_symbol(symarray[subsidx].symbol);
        return true;                                              //we are done
    } 
    
    if ((jdoc["event"] == F("subscribed")) and (jdoc["chanId"] != false)) {
        //ist a channel subscribe message
        symarray[subsidx].chanid = jdoc["chanId"];                                                              //remember it chanId for last subscribed symbol
        Serial.printf_P( PSTR("[Prs] Ticker subscribe success, channel id: %u\n"), symarray[subsidx].chanid);
        subsidx++;                                                                                              //move to next symbol
        if (subsidx < symnum) {                                                                                 //subscribe next if there is some more to subscribe
            Serial.printf_P( PSTR("[Prs] Lets subscribe next ticker symbol: %s\n"), symarray[subsidx].symbol);
            subscribe_symbol(symarray[subsidx].symbol);
        }
        return true;                                            //we are done
    }
    Serial.println(F("[Prs] ERROR, unknown data"));
    return false;
}

void hbcheck()
{
    for (byte i = 0; i < symnum; i++) {   //for all symbols check hb flag
        if (symarray[i].hb != true) {
            Serial.printf_P( PSTR("[HBC] HB check failed, symbol = %s, reconnect websocket\n"), symarray[i].symbol);
            reconnflag = true;  //set the flag, will do the reconnect in main loop
            return; //makes no sense to continue if one failed found
        }
        symarray[i].hb = false; //and clear HBs so we can check next time that we got update / hb for each
    }
    Serial.println(F("[HBC] HB check OK"));
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
        
        if (((pos_p != NULL) and ((pos_p - last_p) == SYMBOL_LEN))                              //found symbol of correct length
                or ((pos_p == NULL) and (strlen(last_p) == SYMBOL_LEN))) {                      //found last symbol of correct length
            memset(symarray[symnum].symbol, '\0', sizeof(symarray[symnum].symbol));             //.symbol defined as [SYMBOL_LEN + 1]
            strncpy(symarray[symnum].symbol, last_p, sizeof(symarray[symnum].symbol) - 1);
            symarray[symnum].hb = false;
            symnum++;    
        } 
        
        if (pos_p != NULL) {
            last_p = pos_p + 1; //set laspt_p behind the space found
        }
    } while (pos_p != NULL);
}

void cfgbywm()
{
    char sbr[3];
    char sctime[3];
    itoa(cfg.brightness + 1, sbr, 10);    //internally is 0-15, wee need to make it 1-16 during setup
    itoa(cfg.cycle_time, sctime, 10);
    WiFiManager wifiManager;                                //Local intialization. Once its business is done, there is no need to keep it around
  
    // The extra parameters to be configured (can be either global or just in the setup). After connecting, parameter.getValue() will get you the configured value, id/name placeholder/prompt default length
    WiFiManagerParameter custom_symbol("symbol", "bitfinex symbol(s)", cfg.symbols, CFG_SYMBOLS_LEN);
    WiFiManagerParameter custom_sbrightness("sbrightness", "display brightness [1 - 16]", sbr, 2);
    WiFiManagerParameter custom_symtime("symtime", "time to cycle symbols", sctime, 2);
  
    wifiManager.setSaveConfigCallback(saveConfigCallback);  //set config save notify callback
    wifiManager.setAPCallback(configModeCallback);          //flash led if in config mode
    wifiManager.addParameter(&custom_symbol);
    wifiManager.addParameter(&custom_sbrightness);
    wifiManager.addParameter(&custom_symtime);
    //wifiManager.setMinimumSignalQuality();                //set minimu quality of signal so it ignores AP's under that quality, defaults to 8%
    wifiManager.setTimeout(CFGPORTAL_TIMEOUT);                          //sets timeout until configuration portal gets turned off useful to make it all retry or go to sleep in seconds
  
    if (!wifiManager.autoConnect(CFGPORTAL_SSID, CFGPORTAL_PWD)) {  //fetches ssid and pass and tries to connect if it does not connect it starts an access point with the specified name and goes into a blocking loop awaiting configuration
        Serial.println(F("Config portal timeout, trying to connect again"));
        ESP.reset();                                          //reset and try again, or maybe put it to deep sleep
    }
  
    Serial.println(F("WiFi connected... :)"));             //if you get here you have connected to the WiFi
    ld.print(F("  wifi  "), 1);
    if (DISP_AMOUNT == 2) {
        ld.print(F(" online "), 2);
    }
    
    //params modified using WM, process them and save to EEPROM
    if (shouldSaveConfig) {
        memset(cfg.symbols, '\0', sizeof(cfg.symbols));
        strncpy(cfg.symbols, custom_symbol.getValue(), sizeof(cfg.symbols) - 1);              
        strupr(cfg.symbols);    //uppercase symbols
        cfg.brightness = atoi(custom_sbrightness.getValue());               
        cfg.cycle_time = atoi(custom_symtime.getValue());
        parsesymbols(cfg.symbols);    //needs to be also here because we are checking that symnum > 0 (that we have some valid symbols)
  
        //check that everything is valid
        if  ((cfg.brightness <= 0) or (cfg.brightness > 16)
                or (cfg.cycle_time <= 0) or (cfg.cycle_time > 99) or (symnum == 0)) {
            Serial.println(F("Parametters out of range, restart config portal"));
            WiFi.disconnect();
            ESP.reset(); 
        }
        cfg.brightness = cfg.brightness - 1;  //brightness range 0-15, but atoi return 0 when error so we made it 1-16 and here is the correction
        save_cfg_eeprom();
    }
}

void check_cfg_reset_button() 
{
    //cfg reset button 
    if ((digitalRead(CFGPORTAL_BUTTON) == LOW) and (!rstticker.active())) {
        Serial.println(F("[Sys] clear settings button pressed, hold it for a while to reset Wifi settings"));
        rstticker.attach(CFGPORTAL_BUTTON_TIME, rstwmcfg);
    }
    
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
}

void check_reconnect_flag()
{
    if (reconnflag) {
        //reconnect ws now
        reconnflag = false;
        webSocket.disconnect();
        subsidx = 0;  //no symbols subscribed
        delay(1000);
        webSocket.beginSSL(APISRV, APIPORT, APIURL);
        webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
    }
}

void show_cnct_api()
{
    //nothing subscribed, display message that trying connect
    if (prevsymidx != PREVSYMIDX_NOREDRAW) { // send it to display only once, not everytime the loop passes
        prevsymidx = PREVSYMIDX_NOREDRAW;
        Serial.println(F("[LED] showing cnct"));
        ld.print(F("cnct api"), 1);
        if (DISP_AMOUNT == 2) {
            ld.print(F("bitfinex"), 2);
        }
    }
}

void setup() 
{
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
    ld.setBright(CFG_DEF_BRIGHTNESS, ALL_MODULES);
    ld.print(F("dread.cz "), 1);
    if (DISP_AMOUNT == 2) {
        ld.print(F(" ticker "), 2);
    }
  
    //get configuration from eeprom
    EEPROM.begin(512);
    if (!get_cfg_eeprom()) {
        //configuration in eeprom is not valid - we need to create default one
        Serial.println(F("EEPROM checksum invalid, creating default configuration"));
        memset(cfg.symbols, '\0', sizeof(cfg.symbols));
        strncpy (cfg.symbols, CFG_DEF_SYMBOLS, sizeof(cfg.symbols) - 1);
        cfg.brightness = CFG_DEF_BRIGHTNESS;
        cfg.cycle_time = CFG_DEF_CYCLE_TIME;
        save_cfg_eeprom();
    }
  
    cfgbywm();
  
    parsesymbols(cfg.symbols);
  
    ld.setBright(cfg.brightness, ALL_MODULES);
    Serial.printf_P(PSTR("Setting display brightness to: %u\n"), cfg.brightness);
    Serial.print(F("[Setup] My IP: "));
    Serial.println(WiFi.localIP());
    Serial.printf_P(PSTR("[Setup] symnum = %u\n"), symnum);
    
    symticker.attach(cfg.cycle_time, nextsymidx);
    Serial.printf_P(PSTR("[Setup] symbol cycle time: %u\n"), cfg.cycle_time);
  
    //start the connection
    webSocket.beginSSL(APISRV, APIPORT, APIURL);  //, "#INSECURE#"
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL);
    hbticker.attach(HB_TIMEOUT, hbcheck);
    Serial.printf_P(PSTR("[Setup] started HB check ticker, interval: %u\n"), HB_TIMEOUT);
  
    pinMode(CFGPORTAL_BUTTON, INPUT_PULLUP);  //button for reset of params
}

void loop()
{
    webSocket.loop();
    check_cfg_reset_button();
    check_reconnect_flag();
  
    //display
    static char dbuff[10]; //8 chars + 1decimal dot + zero terminator = thats our buffer
    
    if (subsidx > 0) {                                                            //if there is sth subscribed
        if ((dispchng == true) and (prevval != symarray[symidx].change)) {        //if we should display 24h change and the value changed
            prevval = symarray[symidx].change;                                    //remember that value
            // Serial.println(F("[LED] showing change"));
            snprintf(dbuff, sizeof(dbuff), "C%#8.1f", symarray[symidx].change);
            ld.print(dbuff, DISP_AMOUNT);
            
        } else if ((dispchng == false) and (symarray[symidx].price != prevval)) { //if we should display prizzee and prizzee changed
            //  Serial.println(F("[LED] showing price"));
            int8_t decimals = 3 - log10(symarray[symidx].price);                  //3 valid decimals
            if (decimals < 0) decimals = 0;                                       //but limit - we cant have negative decimal count
            if (decimals > 7) decimals = 7;                                       //and we have only 8 digit display = max 7 decimals
            snprintf(dbuff, sizeof(dbuff), "%#.*f", decimals, symarray[symidx].price);
            ld.print(dbuff, DISP_AMOUNT);
            prevval = symarray[symidx].price;                                     //remember that value
        }
        
        if (symidx != prevsymidx) {                                               //symbol changed, display it
            prevsymidx = symidx;
            if (DISP_AMOUNT == 2) {                                               //show it on 1st display but only if we have two displays
                Serial.println(F("[LED] showing symbol"));
                snprintf(dbuff, sizeof(dbuff), " %s ", symarray[symidx].symbol);
                ld.print(dbuff, 1);
            }
        }
    } else { 
        //nothing subscibed - show cnct api;
        show_cnct_api();
    }
}
