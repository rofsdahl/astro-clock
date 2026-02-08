# astro-clock

ESP32 based Astro clock with Nexa transmitter, astro calculations and weather info.
The project is built using an ESP32 microcontroller from Espressif and a FS1000A/XD-FST RF Radio module.
The code is written in Arduino-style C/C++.

* The system connects to WiFi and retrieves correct local time using NTP
* Local times for sunrise and sunset are calculated using simplified NOAA formulas for solar geometry
* Weather forecast is retrieved from met.no
* Nexa power plugs are switched using RF 433 MHz, between dusk and dawn (like a twilight switch)

# Hardware

## LilyGO TTGO T-display ESP32

![LilyGO TTGO T-display ESP32](images/LilyGO-TTGO-T-display-ESP32.jpg)

[LilyGO TTGO T-display ESP32](https://github.com/Xinyuan-LilyGO/TTGO-T-Display)

## FS1000A/XD-FST RF Radio module

![FS1000A](images/FS1000A.jpg)

It might be better to use a straight solid core wire antenna (quarter wave-length, 17.3 cm), rather than the antenna coil shown on the picture.

# Software installation

## Board
* Install/update the [Arduino IDE](https://www.arduino.cc/en/software)
* In **Preferences** set **Additional Board Manager URLs** to `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
* Go to **Tools > Board > Boards Manager** and install **esp32** by Espressif Systems
* Go to **Tools > Board > ESP32 Arduino** and select the board **ESP32 Dev Module**
* The board shall be configured like this:
  * **Board:** ESP32 Dev Module
  * **Upload Speed:** 921600
  * **CPU Frequency:** 240MHz (WiFi/BT)
  * **Flash Frequency:** 80MHz
  * **Flash Mode:** QIO
  * **Flash Size:** 4MB (32Mb)
  * **Partition Scheme:** Default 4MB with spiffs (1.2MB APP/1.5 SPIFFS)
  * **Core Debug Level:** None
  * **PSRAM:** Disabled
  * **Arduino runs on:** Core 1
  * **Events runs on:** Core 1
  * **Port:** (the COM port your board has connected to, see below)
* To determine on which COM port your board connects, do the following steps:
  * If required, install [UART driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
  * Unplug the board
  * Have a look at **Tools > Port** and remember which ports you see
  * Plug in the board to the USB port
  * Look again at **Tools > Port**
  * The newly added COM port is the COM port of your board and the one you can select

## Libraries
Go to **Tools > Manage libraries** and install the following libraries:
* **TFT_eSPI** by Bodmer
* **ArduinoJson** by Benoit

## Custom setup file for TTGO T-Display ST7789V
* Edit the file `C:/Users/%USERNAME%/Documents/Arduino/libraries/TFT_eSPI/User_Setup_Select.h`
 and enable the custom setup file for the display:
    ```
    //#include <User_Setup.h>
    #include <User_Setups/Setup25_TTGO_T_Display.h>
    ```
* [Setup file, alt. 1](https://github.com/Xinyuan-LilyGO/TTGO-T-Display/blob/master/TFT_eSPI/User_Setups/Setup25_TTGO_T_Display.h)
* [Setup file, alt. 2](https://github.com/ubcenvcom/TTGO-EkoTuki-Display/blob/master/TTGO_T_Display.h)
