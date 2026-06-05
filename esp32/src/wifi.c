// wifi.c - see wifi.h.
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "wifi.h"

static const char *TAG = "wifi";

#define AP_PASS    "sd2snes0"    // >= 8 chars (WPA2)
#define NVS_NS     "wifi"
#define MAX_RETRY  5
#define AP_OFF_US  (6 * 1000 * 1000)   // grace after GOT_IP so the WebUI can show the new IP

static volatile int s_connected;
static int  s_retry;
static char s_ip[16]   = "";
static char s_ssid[33] = "";     // STA target SSID
static esp_timer_handle_t s_ap_off;

// SoftAP only when NOT connected to a router: APSTA keeps the AP up, STA hides it.
static void ap_set(int on) {
    wifi_mode_t want = on ? WIFI_MODE_APSTA : WIFI_MODE_STA, cur;
    if (esp_wifi_get_mode(&cur) == ESP_OK && cur == want) return;
    esp_wifi_set_mode(want);
    ESP_LOGI(TAG, "SoftAP %s", on ? "on (not connected)" : "off (STA connected)");
}

static void ap_off_cb(void *arg) {
    if (s_connected) ap_set(0);      // still connected after the grace window -> drop the AP
}

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = 0; s_ip[0] = 0;
        esp_timer_stop(s_ap_off);    // cancel a pending AP-off
        ap_set(1);                   // bring the AP back so access survives the drop
        if (s_retry < MAX_RETRY) { s_retry++; esp_wifi_connect(); }
        else ESP_LOGW(TAG, "STA connect failed, staying AP-only");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = 1; s_retry = 0;
        ESP_LOGI(TAG, "STA \"%s\" got IP %s", s_ssid, s_ip);
        esp_timer_stop(s_ap_off);
        esp_timer_start_once(s_ap_off, AP_OFF_US);   // drop the AP after the grace window
    }
}

static void load_and_connect(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char ssid[33] = {0}, pass[65] = {0};
    size_t sl = sizeof(ssid), pl = sizeof(pass);
    esp_err_t e = nvs_get_str(h, "ssid", ssid, &sl);
    nvs_get_str(h, "pass", pass, &pl);
    nvs_close(h);
    if (e == ESP_OK && ssid[0]) wifi_connect(ssid, pass);
}

void wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL, NULL);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = {0};
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "sd2snes-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    strlcpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP \"%s\" pass \"%s\"  ->  http://192.168.4.1/", (char *)ap.ap.ssid, AP_PASS);

    const esp_timer_create_args_t targs = { .callback = ap_off_cb, .name = "ap_off" };
    esp_timer_create(&targs, &s_ap_off);

    load_and_connect();
}

int wifi_scan(wifi_ap_t *out, int max) {
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (!num) return 0;
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * num);
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&num, recs);

    int cnt = 0;
    for (int i = 0; i < num; i++) {
        if (recs[i].ssid[0] == 0) continue;
        int found = -1;
        for (int j = 0; j < cnt; j++)
            if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) { found = j; break; }
        if (found >= 0) {                                   // keep the strongest per SSID
            if (recs[i].rssi > out[found].rssi) {
                out[found].rssi = recs[i].rssi;
                out[found].enc = (recs[i].authmode != WIFI_AUTH_OPEN);
                out[found].channel = recs[i].primary;
            }
            continue;
        }
        if (cnt >= max) continue;
        strlcpy(out[cnt].ssid, (char *)recs[i].ssid, sizeof(out[cnt].ssid));
        out[cnt].rssi = recs[i].rssi;
        out[cnt].enc = (recs[i].authmode != WIFI_AUTH_OPEN);
        out[cnt].channel = recs[i].primary;
        cnt++;
    }
    free(recs);

    for (int i = 0; i < cnt; i++)                           // sort desc by RSSI
        for (int j = i + 1; j < cnt; j++)
            if (out[j].rssi > out[i].rssi) { wifi_ap_t t = out[i]; out[i] = out[j]; out[j] = t; }
    return cnt;
}

esp_err_t wifi_connect(const char *ssid, const char *pass) {
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (pass) strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) return e;

    strlcpy(s_ssid, ssid, sizeof(s_ssid));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass ? pass : "");
        nvs_commit(h);
        nvs_close(h);
    }

    s_connected = 0; s_retry = 0; s_ip[0] = 0;
    esp_timer_stop(s_ap_off);
    ap_set(1);                      // keep the AP up while we (re)connect
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return esp_wifi_connect();
}

void wifi_forget(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); nvs_commit(h); nvs_close(h);
    }
    s_ssid[0] = 0; s_ip[0] = 0; s_connected = 0;
    s_retry = MAX_RETRY;                                    // stop auto-retry
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "forgot saved network");
}

void wifi_get_status(wifi_status_t *st) {
    memset(st, 0, sizeof(*st));
    st->connected = s_connected;
    strlcpy(st->ssid, s_ssid, sizeof(st->ssid));
    strlcpy(st->ip, s_ip, sizeof(st->ip));
    if (s_connected) {
        wifi_ap_record_t ai;
        if (esp_wifi_sta_get_ap_info(&ai) == ESP_OK) st->rssi = ai.rssi;
    }
}
