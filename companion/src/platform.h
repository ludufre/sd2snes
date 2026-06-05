// platform.h - the only place ESP32 vs ESP8266 differ. One Arduino codebase,
// two targets (companion/platformio.ini: esp32dev / nodemcuv2).
//
//   - ESP32: 3 UARTs -> UART2 (GPIO16 RX / GPIO17 TX) is the MCU link, UART0/USB
//     stays the debug console. WiFi.h / WebServer.
//   - ESP8266: 1 full UART -> UART0 swapped to GPIO13/GPIO15 is the MCU link
//     (USB stays free for flashing), UART1 (GPIO2, TX-only) is the debug log.
//     ESP8266WiFi / ESP8266WebServer.
#pragma once
#include <Arduino.h>

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  typedef ESP8266WebServer WebServerT;
  #define LINK_SERIAL  Serial      // UART0, swapped to GPIO13/15 in link_init()
  #define DBG_SERIAL   Serial1     // GPIO2, TX-only
  #define ENC_IS_OPEN(t) ((t) == ENC_TYPE_NONE)
  #define CHIP_NAME    "ESP8266"
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  typedef WebServer WebServerT;
  #define LINK_SERIAL  Serial2     // UART2, GPIO16 RX / GPIO17 TX
  #define DBG_SERIAL   Serial      // UART0 over USB
  #define ENC_IS_OPEN(t) ((t) == WIFI_AUTH_OPEN)
  #define CHIP_NAME    "ESP32"
#else
  #error "unsupported platform (expected ESP8266 or ESP32)"
#endif
