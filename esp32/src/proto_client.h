// proto_client.h - synchronous client for the MCU UART file-server protocol.
//
// Runs on the ESP32: builds request frames on UART2 and waits (with timeout)
// for the matching response.  The MCU is the server.  Blocking here is fine -
// it runs in the httpd worker task and never affects the SNES.
//
// All calls are serialized by proto_lock()/proto_unlock(): a multi-frame
// operation (LS_OPEN+LS_NEXT*, GET_OPEN+GET_DATA*, PUT_OPEN+PUT_DATA*+CLOSE)
// MUST be wrapped in a single lock/unlock pair by the caller, because the MCU
// keeps one file/dir handle at a time.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "wifi.h"                     // wifi_ap_t for the WiFi bridge push

void      proto_init(void);          // UART2 @921600 (GPIO16 RX / GPIO17 TX) + mutex
void      proto_lock(void);
void      proto_unlock(void);

esp_err_t proto_ping(uint8_t *proto_ver, uint8_t *lang, char *name, size_t name_sz);
esp_err_t proto_ls_open(const char *path, uint8_t *status);
esp_err_t proto_ls_next(uint8_t *buf, size_t bufsz, uint16_t *outlen, int *final);
esp_err_t proto_stat(const char *path, uint8_t *status, uint8_t *is_dir, uint32_t *size);
esp_err_t proto_get_open(const char *path, uint8_t *status, uint32_t *total);
esp_err_t proto_get_data(uint32_t off, uint16_t want, uint8_t *status,
                         uint8_t *buf, uint16_t *got, int *final);
esp_err_t proto_put_open(const char *path, uint32_t total, uint8_t *status);
esp_err_t proto_put_data(uint32_t off, const uint8_t *buf, uint16_t len,
                         uint8_t *status, uint16_t *credit);
esp_err_t proto_put_close(uint8_t *status);
esp_err_t proto_rm(const char *path, uint8_t *status);
esp_err_t proto_mv(const char *from, const char *to, uint8_t *status);
esp_err_t proto_mkdir(const char *path, uint8_t *status);
esp_err_t proto_abort(void);

// WiFi-in-menu bridge (ESP is the client here too; the MCU buffers for the menu).
esp_err_t proto_wifi_report(uint8_t connected, int8_t rssi, const char *ssid, const char *ip);
esp_err_t proto_wifi_poll(uint8_t *action, char *ssid, size_t ssz, char *pass, size_t psz);
esp_err_t proto_wifi_scan_push(const wifi_ap_t *aps, int n);
