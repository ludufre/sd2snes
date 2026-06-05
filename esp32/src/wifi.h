// wifi.h - WiFi for the companion: always-on SoftAP (so the WebUI is reachable
// with no router) + optional STA join to a home network, configured from the
// WebUI. STA credentials persist in NVS and auto-connect on boot.
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char    ssid[33];
    int8_t  rssi;       // dBm
    uint8_t enc;        // 1 = encrypted, 0 = open
    uint8_t channel;
} wifi_ap_t;

typedef struct {
    int     connected;  // STA got an IP
    char    ssid[33];   // STA target SSID
    char    ip[16];     // STA IP (empty if not connected)
    int8_t  rssi;
} wifi_status_t;

void      wifi_start(void);                              // AP+STA up, NVS auto-connect
int       wifi_scan(wifi_ap_t *out, int max);           // sync scan, strongest-per-SSID, sorted desc; returns count
esp_err_t wifi_connect(const char *ssid, const char *pass); // set STA, persist, connect (async)
void      wifi_forget(void);                             // clear NVS creds + disconnect
void      wifi_get_status(wifi_status_t *st);
