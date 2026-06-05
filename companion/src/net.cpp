// net.cpp - see net.h. WiFi is API-compatible across Arduino-ESP32/ESP8266; the
// only difference is the "open network" auth constant (platform.h::ENC_IS_OPEN).
#include "platform.h"
#include "net.h"
#if defined(ESP32)
  #include "esp_wifi.h"   // esp_wifi_get_config (read the saved STA SSID)
#endif

#define AP_PASS    "sd2snes0"   // >= 8 chars (WPA2)
#define AP_OFF_MS  8000         // grace after connecting before dropping the AP

static char  s_ap[20];
static bool  s_connected = false;
static bool  s_ap_up     = true;
static unsigned long s_ap_off_at = 0;   // 0 = none scheduled

static void ap_bring_up(void) {
    if (s_ap_up) return;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(s_ap, AP_PASS, 1);
    s_ap_up = true;
}

const char *net_ap_ssid(void) { return s_ap; }

// True if the SDK has a STA network saved. With none saved we leave the STA idle
// instead of looping on an empty connect, which keeps the radio free for scans.
static bool sta_has_creds(void) {
#if defined(ESP8266)
    struct station_config c = {0};
    wifi_station_get_config(&c);
    return c.ssid[0] != 0;
#else // ESP32
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) != ESP_OK) return false;
    return c.sta.ssid[0] != 0;
#endif
}

void net_start(void) {
    WiFi.persistent(true);          // SDK stores/auto-reconnects the last STA creds
    WiFi.mode(WIFI_AP_STA);
    uint8_t mac[6]; WiFi.softAPmacAddress(mac);
    snprintf(s_ap, sizeof(s_ap), "sd2snes-%02X%02X", mac[4], mac[5]);
    WiFi.softAP(s_ap, AP_PASS, 1);
    s_ap_up = true;
    if (sta_has_creds()) WiFi.begin();   // reconnect only if a network is saved
}

void net_loop(void) {
    bool now = (WiFi.status() == WL_CONNECTED);
    if (now && !s_connected) {
        s_connected = true;
        s_ap_off_at = millis() + AP_OFF_MS;   // drop the AP after a short grace
    } else if (!now && s_connected) {
        s_connected = false;
        s_ap_off_at = 0;
        ap_bring_up();                          // lost the link -> AP back for access
    }
    if (s_ap_off_at && s_connected && (int32_t)(millis() - s_ap_off_at) >= 0) {
        WiFi.mode(WIFI_STA);                    // connected and settled -> AP off
        s_ap_up = false;
        s_ap_off_at = 0;
    }
}

int net_scan(ap_rec *out, int max) {
    bool connected = (WiFi.status() == WL_CONNECTED);
    WiFi.scanDelete();                              // drop any stale async result
    int n = WiFi.scanNetworks(false /*sync*/, false /*hidden*/);

    // An in-flight (auto-)reconnect keeps the radio busy, so a sync scan bails out
    // at once with WIFI_SCAN_RUNNING(-1)/WIFI_SCAN_FAILED(-2) instead of actually
    // sweeping the channels (returns fast with nothing). When we're not connected,
    // free the STA, scan, then let the SDK resume reconnecting to the saved AP.
    if (n < 0 && !connected) {
        WiFi.persistent(false);                    // ESP8266: keep disconnect from wiping saved creds
        WiFi.disconnect(false /*keep WiFi on*/);
        WiFi.persistent(true);
        delay(120);
        WiFi.scanDelete();
        n = WiFi.scanNetworks(false, false);
        if (sta_has_creds()) WiFi.begin();         // resume reconnect only if a network is saved
    }
    for (int tries = 0; n < 0 && tries < 4; tries++) {   // transient SDK hiccup
        delay(200);
        WiFi.scanDelete();
        n = WiFi.scanNetworks(false, false);
    }
    DBG_SERIAL.printf("[net] scanNetworks -> %d (%sconnected)\n", n, connected ? "" : "not ");
    if (n <= 0) return 0;
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        int8_t rssi = WiFi.RSSI(i);
        uint8_t enc = ENC_IS_OPEN(WiFi.encryptionType(i)) ? 0 : 1;
        int found = -1;
        for (int j = 0; j < cnt; j++) if (s == out[j].ssid) { found = j; break; }
        if (found >= 0) {
            if (rssi > out[found].rssi) { out[found].rssi = rssi; out[found].enc = enc; }
            continue;
        }
        strncpy(out[cnt].ssid, s.c_str(), 32); out[cnt].ssid[32] = 0;
        out[cnt].rssi = rssi; out[cnt].enc = enc;
        cnt++;
    }
    for (int i = 0; i < cnt; i++)
        for (int j = i + 1; j < cnt; j++)
            if (out[j].rssi > out[i].rssi) { ap_rec t = out[i]; out[i] = out[j]; out[j] = t; }
    WiFi.scanDelete();
    return cnt;
}

bool net_connect(const char *ssid, const char *pass) {
    ap_bring_up();                  // keep the AP up while (re)connecting
    s_ap_off_at = 0;
    WiFi.begin(ssid, pass);         // persists creds, async connect
    return true;
}

void net_forget(void) {
    WiFi.disconnect(true);          // erase stored creds
    s_connected = false;
    ap_bring_up();
}

void net_get_status(net_status *st) {
    memset(st, 0, sizeof(*st));
    if (WiFi.status() == WL_CONNECTED) {
        st->connected = true;
        strncpy(st->ssid, WiFi.SSID().c_str(), 32);
        strncpy(st->ip, WiFi.localIP().toString().c_str(), 15);
        st->rssi = WiFi.RSSI();
    } else {
        strncpy(st->ssid, WiFi.SSID().c_str(), 32);   // target ssid even if down
    }
}
