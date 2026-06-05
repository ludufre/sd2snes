// net.h - WiFi for the ESP8266 companion: always-on SoftAP (so the WebUI is
// reachable with no router) + optional STA join (persisted by the SDK), with the
// AP dropped once a station connection is up (same behaviour as the ESP32 build).
#pragma once
#include <Arduino.h>
#include "link.h"   // ap_rec

struct net_status { bool connected; char ssid[33]; char ip[16]; int8_t rssi; };

void net_start(void);
void net_loop(void);                              // AP on/off + connection tracking
int  net_scan(ap_rec *out, int max);              // sync scan, strongest-per-SSID
bool net_connect(const char *ssid, const char *pass);
void net_forget(void);
void net_get_status(net_status *st);
const char *net_ap_ssid(void);
