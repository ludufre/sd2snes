// sd2snes-companion - companion firmware for the sd2snes/FXPAK PRO. One Arduino
// codebase, two targets (ESP32 / ESP8266 - see platform.h and platformio.ini).
//
//   [MCU P0.2/P0.3] <--921600--> ESP UART  (ESP32: UART2 GPIO16/17;
//                                           ESP8266: UART0 swapped to GPIO13/15)
//   [browser] <--WiFi SoftAP/STA HTTP--> file manager + WiFi config + OTA
//
// Single-threaded (Arduino loop) - no mutex needed.
#include <Arduino.h>
#include "platform.h"
#include "proto.h"
#include "link.h"
#include "ota.h"
#include "net.h"
#include "web.h"
#include "display.h"
#include "version.h"

#define BRIDGE_MS 400

static unsigned long s_bridge_at = 0;

// Pump the SNES menu's WiFi requests to the WiFi stack and report status back
// (the ESP8266 equivalent of the ESP32 wifi_bridge task).
static void bridge_once(void) {
    net_status st; net_get_status(&st);
    if (proto_wifi_report(st.connected, st.rssi, st.ssid, st.ip) != LOK) return;  // MCU silent
    proto_esp_info(FW_VERSION_STR " (" CHIP_NAME ")");   // identity for System Information
    uint8_t action = UP_WIFI_NONE;
    char ssid[UP_WIFI_SSID_MAX + 1] = {0}, pass[UP_WIFI_PASS_MAX + 1] = {0};
    if (proto_wifi_poll(&action, ssid, sizeof(ssid), pass, sizeof(pass)) != LOK) return;
    if (action == UP_WIFI_SCAN_REQ) {
        static ap_rec aps[UP_WIFI_MAX_APS];
        int n = net_scan(aps, UP_WIFI_MAX_APS);
        proto_wifi_scan_push(aps, n);
    } else if (action == UP_WIFI_CONNECT) {
        net_connect(ssid, pass);
    } else if (action == UP_WIFI_FORGET) {
        net_forget();
    }
}

void setup() {
    link_init();              // MCU UART @921600
    ota_check_and_apply();    // self-update from /sd2snes/espXX.bin if a newer .ver is on the SD
    net_start();              // SoftAP (+ STA auto-reconnect)
    display_init();           // ST7789 status panel (no-op wiring still boots fine)
    web_start();              // HTTP file manager + WiFi config + OTA
}

void loop() {
    web_loop();       // serve HTTP
    net_loop();       // AP on/off + connection tracking
    display_loop();   // ST7789 status panel (throttled, redraw-on-change)
    if ((int32_t)(millis() - s_bridge_at) >= 0) {
        s_bridge_at = millis() + BRIDGE_MS;
        bridge_once();
    }
    yield();
}
