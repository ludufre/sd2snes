// link.h - MCU UART link + framed protocol client for the ESP8266 companion.
//
// ESP8266 has a single full UART (UART0). We move it to GPIO13(RX)/GPIO15(TX)
// with Serial.swap() so the USB serial stays free for flashing, and talk the
// same byte-identical protocol as the ESP32 build (proto.h). Single-threaded
// (Arduino loop), so no mutex is needed; handlers call these directly.
#pragma once
#include <Arduino.h>
#include <stddef.h>

typedef int lerr_t;
#define LOK 0
#define LERR_TIMEOUT 1
#define LERR_FAIL 2

void link_init(void);   // Serial @921600 swapped to GPIO13/15

lerr_t proto_ping(uint8_t *ver, uint8_t *lang, char *name, size_t ns);
lerr_t proto_ls_open(const char *path, uint8_t *status);
lerr_t proto_ls_next(uint8_t *buf, size_t bufsz, uint16_t *outlen, int *final);
lerr_t proto_get_open(const char *path, uint8_t *status, uint32_t *total);
lerr_t proto_get_data(uint32_t off, uint16_t want, uint8_t *status,
                      uint8_t *buf, uint16_t *got, int *final);
// Pipelined GET: download [0,total) by bursting requests so the MCU streams replies back
// to back. Calls sink(ctx,data,len) per chunk IN ORDER; sink returns nonzero to abort.
typedef int (*get_sink_fn)(void *ctx, const uint8_t *data, uint16_t len);
lerr_t proto_get_stream(uint32_t total, get_sink_fn sink, void *ctx);
lerr_t proto_put_open(const char *path, uint32_t total, uint8_t *status);
lerr_t proto_put_data(uint32_t off, const uint8_t *buf, uint16_t len,
                      uint8_t *status, uint16_t *credit);
lerr_t proto_put_close(uint8_t *status);
lerr_t proto_rm(const char *path, uint8_t *status);
lerr_t proto_mv(const char *from, const char *to, uint8_t *status);
lerr_t proto_mkdir(const char *path, uint8_t *status);
lerr_t proto_hot_reload(uint8_t *status);   // ask the menu to re-read config/theme/lists (no power-cycle)
lerr_t proto_abort(uint8_t *status);         // cancel an in-flight file/dir op (frees the MCU handle)

// WiFi-in-menu bridge (ESP is the client; the MCU buffers for the SNES menu).
struct ap_rec { char ssid[33]; int8_t rssi; uint8_t enc; };
lerr_t proto_wifi_report(uint8_t connected, int8_t rssi, const char *ssid, const char *ip);
lerr_t proto_wifi_poll(uint8_t *enabled, uint8_t *action, char *ssid, size_t ssz, char *pass, size_t psz);
lerr_t proto_wifi_scan_push(const ap_rec *aps, int n);
lerr_t proto_esp_info(const char *s);   // report the companion version "x.y.z (CHIP)"
