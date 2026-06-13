// net.h - WiFi for the ESP8266 companion: always-on SoftAP (so the WebUI is
// reachable with no router) + optional STA join (persisted by the SDK), with the
// AP dropped once a station connection is up (same behaviour as the ESP32 build).
#pragma once
#include <Arduino.h>
#include "link.h"   // ap_rec

struct net_status { bool connected; char ssid[33]; char ip[16]; int8_t rssi; };

void net_init(void);                              // boot with the radio OFF (secure default)
void net_set_enabled(bool on);                    // menu's EnableWifi: bring the radio up/down
void net_start(void);
void net_loop(void);                              // AP on/off + tracking + async-scan pump
int  net_scan(ap_rec *out, int max);              // blocking one-shot (WebUI); strongest-per-SSID
void net_scan_start(void);                        // begin an async AP scan (non-blocking; coalesces)
void net_scan_pump(void);                         // advance/harvest the async scan (called from net_loop)
int  net_scan_take(ap_rec *out, int max);         // freshly-completed scan result once, else -1
bool net_connect(const char *ssid, const char *pass);
void net_forget(void);
void net_get_status(net_status *st);
const char *net_ap_ssid(void);
