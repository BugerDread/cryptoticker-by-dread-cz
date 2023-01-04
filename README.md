# Cryptoticker-by-dread-cz
Cryptoticker for your table

## What does it do?
Ticker shows the price(s) of chosen cryptocurrencies. Data comes from Binance websocket API / WiFi. It aslo serves as a NTP controlled clock.

![ticker-and-ntpclock](ticker-and-ntpclock.jpg)

## How to make my own?
1. You will need:
* ESP8266 or ESP32-C3 (RISC-V) board (not tested if it works with non-C3 version of ESP32)
* 8-digit LED display driven by MAX7219 (or pair of them)
* Arduino with ESP8266 / ESP32 support installed
* MAX7219 HW SPI library - [https://github.com/BugerDread/esp8266-hw-spi-max7219-7seg](https://github.com/BugerDread/esp8266-hw-spi-max7219-7seg)
* ArduinoJson library (by Benoit Blanchon) - install from Arduino Library Manager
* WebSockets library (by Markus Sattler) - also from lib manager
* WiFiManager library (by tzapu) - again lib mngr
* Clone of [this repo](https://github.com/BugerDread/cryptoticker-by-dread-cz)
2. Hook up the display to the ESP
* for ESP8266 connect:
  * Vcc to 3V3
  * GND to GND
  * DIN to GPIO13
  * CS to GPIO15
  * CLK to GPIO14
* for ESP32-C3 connect:
  * Vcc to 3V3
  * GND to GND
  * DIN to GPIO7
  * CS3 to GPIO5
  * CLK to GPIO6
3. If you are going to use pair of displays hook up the 2nd to the output of the 1st one.
4. Hook up the ESP to the computer, launch Arduino, open the ticker-ws-ticker.ino 
5. Configure the project
* if you are using pair of displays change "static const uint8_t DISP_AMOUNT = 1;" at the top of the code to "static const uint8_t DISP_AMOUNT = 2;"
* adjust "static const char ourtimezone[] PROGMEM = "Europe/Prague";" to fit your timezone
6. Upload code to the board.
7. Wait until the display reads "config 192.168.4.1" (it may take about 15seconds).
8. Connect to the "Bgr ticker" WiFi network (via phone or computer - password is "btcbtcbtc") and open http://192.168.4.1 in your web browser it if doesnt do so automatically.
9. Click "Configure WiFi", then fill in (you can also click / tap it in the list above the form) name (SSID) and password for your WiFi (leave password blank for open WiFi).
10. Fill in list of Binance symbols separated by spaces.
11. You can also change brightness of the display (range 1-16) and interval how long is each value shown on the display.
12. Click save, wait few seconds, display should show "WiFi online" and then the ticker should start to show the prices of configured symbols. If it doesnt, please try to repeat the configuration and make sure that the configuration is correct.
13. If you want to configure the ticker again hold the "Flash" button on the ESP board for about 5 seconds to switch into configuration mode and repeat the steps above from #5.


