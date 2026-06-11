// platform.h - the only place the chips differ. One Arduino codebase, three
// targets (companion/platformio.ini: esp32dev / nodemcuv2 / esp32-c3-supermini).
//
//   - ESP32 (WROOM-32): 3 UARTs -> UART2 (GPIO16 RX / GPIO17 TX) is the MCU link,
//     UART0/USB stays the debug console. WiFi.h / WebServer.
//   - ESP32-C3 (Super Mini): only 2 UARTs (no Serial2) -> UART1 (GPIO20 RX /
//     GPIO21 TX) is the MCU link, and Serial is the native USB-CDC debug console
//     (-DARDUINO_USB_CDC_ON_BOOT=1). Same WiFi.h / WebServer as the ESP32.
//   - ESP8266: 1 full UART -> UART0 swapped to GPIO13/GPIO15 is the MCU link
//     (USB stays free for flashing), UART1 (GPIO2, TX-only) is the debug log.
//     ESP8266WiFi / ESP8266WebServer.
//
// Per-chip knobs each branch defines:
//   LINK_SERIAL / LINK_RX_PIN / LINK_TX_PIN  - the MCU UART link (see link.cpp)
//   DBG_SERIAL / CHIP_NAME / WebServerT / ENC_IS_OPEN
//   OTA_BASENAME                             - /sd2snes/<name> self-update image (ota.cpp)
//   TFT_HWSPI + TFT_SCLK/MOSI/DC/RST/CS/BL   - ST7789 1.54" display wiring (display.cpp)
//
// IMPORTANT: the Arduino-ESP32 core defines the bare `ESP32` macro for the C3 too,
// so the C3 branch MUST come BEFORE `#elif defined(ESP32)`. The unambiguous
// discriminator is CONFIG_IDF_TARGET_ESP32C3 (from sdkconfig.h via Arduino.h).
#pragma once
#include <Arduino.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  typedef ESP8266WebServer WebServerT;
  #define LINK_SERIAL  Serial      // UART0, swapped to GPIO13/15 in link_init()
  #define LINK_RX_PIN  13          // informational: fixed by Serial.swap() (D7)
  #define LINK_TX_PIN  15          // informational: fixed by Serial.swap() (D8)
  #define DBG_SERIAL   Serial1     // GPIO2, TX-only
  #define ENC_IS_OPEN(t) ((t) == ENC_TYPE_NONE)
  #define CHIP_NAME    "ESP8266"
  #define OTA_BASENAME "esp8266.bin"
  // ST7789 over SOFTWARE SPI: hardware HSPI (MOSI=GPIO13 / SS=GPIO15) collides with
  // the swapped UART0 link, so bit-bang on the only free pins. CS->GND, BL->3V3.
  #define TFT_HWSPI    0
  #define TFT_SCLK     4           // D2
  #define TFT_MOSI     5           // D1
  #define TFT_DC       0           // D3 - strapping pin, must NOT be pulled low at boot
  #define TFT_RST      16          // D0
  #define TFT_CS       (-1)        // tied to GND
  #define TFT_BL       (-1)        // tied to 3V3 (no GPIO)
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #include <WiFi.h>
  #include <WebServer.h>
  typedef WebServer WebServerT;
  #define LINK_SERIAL  Serial1     // UART1 (the C3 has no Serial2)
  #define LINK_RX_PIN  20
  #define LINK_TX_PIN  21
  #define DBG_SERIAL   Serial      // native USB-CDC (USB-Serial-JTAG)
  #define ENC_IS_OPEN(t) ((t) == WIFI_AUTH_OPEN)
  #define CHIP_NAME    "ESP32-C3"
  #define OTA_BASENAME "esp32c3.bin"
  // ST7789 over hardware FSPI (routed via the GPIO matrix). Avoids strapping
  // GPIO2/8/9 and USB GPIO18/19. DC=GPIO5 doubles as the default MISO, harmless
  // since the panel is write-only.
  #define TFT_HWSPI    1
  #define TFT_SCLK     4
  #define TFT_MOSI     6
  #define TFT_DC       5
  #define TFT_RST      7
  #define TFT_CS       10
  #define TFT_BL       3
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  typedef WebServer WebServerT;
  #define LINK_SERIAL  Serial2     // UART2, GPIO16 RX / GPIO17 TX
  #define LINK_RX_PIN  16
  #define LINK_TX_PIN  17
  #define DBG_SERIAL   Serial      // UART0 over USB
  #define ENC_IS_OPEN(t) ((t) == WIFI_AUTH_OPEN)
  #define CHIP_NAME    "ESP32"
  #define OTA_BASENAME "esp32.bin"
  // ST7789 over hardware VSPI. Avoids the link (GPIO16/17) and input-only 34-39.
  #define TFT_HWSPI    1
  #define TFT_SCLK     18
  #define TFT_MOSI     23
  #define TFT_DC       4
  #define TFT_RST      22
  #define TFT_CS       5           // boot strap, but ST7789 CS idles high - safe
  #define TFT_BL       21
#else
  #error "unsupported platform (expected ESP8266, ESP32 or ESP32-C3)"
#endif
