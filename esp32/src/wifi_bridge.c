// wifi_bridge.c - see wifi_bridge.h.
//
// The menu (SNES) can't talk to the ESP directly, and the MCU is a pure UART
// server, so the ESP drives this: every ~300ms it pushes its current WiFi
// status to the MCU (WIFI_REPORT) and asks for a pending menu request
// (WIFI_POLL). A scan request runs wifi_scan() and pushes the list back
// (WIFI_SCAN); connect/forget call straight into the WiFi stack. The menu reads
// the buffered status/scan via its CMD_WIFI_GET SRAM block.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "proto.h"
#include "proto_client.h"
#include "wifi.h"
#include "wifi_bridge.h"

static const char *TAG = "wifibr";

#define POLL_MS  400     // normal cadence
#define BACKOFF_MS 3000  // when the MCU isn't answering (old firmware / link down)

static void wifi_bridge_task(void *arg) {
    static wifi_ap_t aps[UP_WIFI_MAX_APS];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        // 1. report current status to the MCU (so the menu can show it)
        wifi_status_t st;
        wifi_get_status(&st);
        proto_lock();
        esp_err_t er = proto_wifi_report(st.connected, st.rssi, st.ssid, st.ip);
        proto_unlock();
        if (er) { vTaskDelay(pdMS_TO_TICKS(BACKOFF_MS)); continue; }  // MCU silent -> ease off

        // 2. pick up a pending menu request
        uint8_t action = UP_WIFI_NONE;
        char ssid[UP_WIFI_SSID_MAX + 1] = {0}, pass[UP_WIFI_PASS_MAX + 1] = {0};
        proto_lock();
        esp_err_t e = proto_wifi_poll(&action, ssid, sizeof(ssid), pass, sizeof(pass));
        proto_unlock();
        if (e || action == UP_WIFI_NONE) continue;

        if (action == UP_WIFI_SCAN_REQ) {
            int n = wifi_scan(aps, UP_WIFI_MAX_APS);   // blocking (~2s), lock not held
            ESP_LOGI(TAG, "menu scan -> %d APs", n);
            proto_lock();
            proto_wifi_scan_push(aps, n);
            proto_unlock();
        } else if (action == UP_WIFI_CONNECT) {
            ESP_LOGI(TAG, "menu connect -> \"%s\"", ssid);
            wifi_connect(ssid, pass);
        } else if (action == UP_WIFI_FORGET) {
            ESP_LOGI(TAG, "menu forget");
            wifi_forget();
        }
    }
}

void wifi_bridge_start(void) {
    xTaskCreate(wifi_bridge_task, "wifi_bridge", 4096, NULL, 4, NULL);
}
