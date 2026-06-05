// sd2snes-esp - companion firmware for the sd2snes/FXPAK PRO.
//
// Talks to the MCU (LPC1756 UART0, pins 79/80) over UART2 @921600 using a
// compact framed protocol (proto.h), and exposes the SD card through a WebUI
// served on its own always-on SoftAP.
//
//   [MCU UART0 P0.2/P0.3] <--921600--> GPIO16/GPIO17 (UART2) [ESP32]
//   [browser] <--WiFi SoftAP/HTTP--> 192.168.4.1
//
// The USB serial (UART0, 115200) stays a debug log (ESP_LOGx).
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "proto.h"
#include "proto_client.h"
#include "wifi.h"
#include "wifi_bridge.h"
#include "webui_http.h"

static const char *TAG = "sd2snes-esp";

// Boot self-test: list /sd2snes over the UART link and log a few entries.
// Read-only - validates LS_OPEN/LS_NEXT/framing without needing the WebUI.
static void ls_selftest(const char *path) {
    proto_lock();
    uint8_t st = 0xFF;
    if (proto_ls_open(path, &st) != ESP_OK || st) {
        proto_unlock();
        ESP_LOGW(TAG, "selftest LS %s failed (status %d)", path, st);
        return;
    }
    int count = 0, final = 0;
    while (!final) {
        uint8_t buf[256]; uint16_t len = 0;
        if (proto_ls_next(buf, sizeof(buf), &len, &final) != ESP_OK) break;
        int i = 0;
        while (i < len) {
            uint8_t type = buf[i++];
            if (type == 0xFF) { final = 1; break; }
            if (i + 4 > len) break;
            i += 4;                                  // skip size
            const char *name = (const char *)(buf + i);
            int nl = strnlen(name, len - i);
            i += nl + 1;
            if (count < 12)
                ESP_LOGI(TAG, "  [%s] %.*s", type == 0 ? "dir" : "fil", nl, name);
            count++;
        }
    }
    proto_unlock();
    ESP_LOGI(TAG, "selftest LS %s: %d entries", path, count);
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    proto_init();          // UART2 link to the MCU
    wifi_start();          // WiFi AP (+ STA auto-connect from NVS)
    wifi_bridge_start();   // bridge the SNES menu's WiFi requests to the WiFi stack
    webui_http_start();    // HTTP file manager + WiFi config

    // Probe the MCU link once so the log shows whether it's wired up.
    uint8_t ver = 0, lang = 0; char name[48] = {0};
    if (proto_ping(&ver, &lang, name, sizeof(name)) == ESP_OK) {
        ESP_LOGI(TAG, "MCU link OK: \"%s\" (proto v%d, lang %d)", name, ver, lang);
        ls_selftest("/sd2snes");
    } else {
        ESP_LOGW(TAG, "MCU did not answer PING (check wiring / power / baud)");
    }

    ESP_LOGI(TAG, "ready");
}
