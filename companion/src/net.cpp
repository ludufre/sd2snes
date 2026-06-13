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
static bool  s_wifi_on   = false;       // master: radio up at all (menu's EnableWifi)
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
    net_scan_pump();            // harvest a finished async scan (non-blocking)
    if (!s_wifi_on) return;     // WiFi master-off: nothing to track
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

// ---- async AP scan ----
// A synchronous WiFi.scanNetworks() blocks the single Arduino loop() for ~1.5-2.5s
// (worse with retries), freezing the WebUI, the ST7789 panel and the MCU bridge.
// Instead the scan runs asynchronously: net_scan_start() kicks it off, net_scan_pump()
// (driven from net_loop every iteration) harvests the result when the radio finishes,
// and the bridge pushes it to the menu then (net_scan_take). net_scan() stays as a
// bounded, WDT-fed blocking wrapper for the WebUI's one-shot request, but drives the
// same machine - one async sweep, never the old 6x sync-retry amplification.
#define SCAN_CACHE  16          // strongest-per-SSID cache (the WebUI asks for up to 16)
#define SCAN_MAX_MS 8000        // abort if the radio never reports the scan complete

static ap_rec  s_scan[SCAN_CACHE];
static int     s_scan_n = 0;
static uint8_t s_scan_state = 0;     // 0 idle, 1 running, 2 done (fresh, unconsumed)
static bool    s_scan_freed_sta = false;
static unsigned long s_scan_at = 0;  // millis() when the running scan started

// fold the SDK's raw scan list into s_scan[]: strongest entry per SSID, RSSI-sorted.
static void scan_harvest(int n) {
    int cnt = 0;
    for (int i = 0; i < n && cnt < SCAN_CACHE; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        int8_t rssi = WiFi.RSSI(i);
        uint8_t enc = ENC_IS_OPEN(WiFi.encryptionType(i)) ? 0 : 1;
        int found = -1;
        for (int j = 0; j < cnt; j++) if (s == s_scan[j].ssid) { found = j; break; }
        if (found >= 0) {
            if (rssi > s_scan[found].rssi) { s_scan[found].rssi = rssi; s_scan[found].enc = enc; }
            continue;
        }
        strncpy(s_scan[cnt].ssid, s.c_str(), 32); s_scan[cnt].ssid[32] = 0;
        s_scan[cnt].rssi = rssi; s_scan[cnt].enc = enc;
        cnt++;
    }
    for (int i = 0; i < cnt; i++)
        for (int j = i + 1; j < cnt; j++)
            if (s_scan[j].rssi > s_scan[i].rssi) { ap_rec t = s_scan[i]; s_scan[i] = s_scan[j]; s_scan[j] = t; }
    s_scan_n = cnt;
}

// resume the STA reconnect we paused for the scan (only if we actually paused it)
static void scan_resume_sta(void) {
    if (!s_scan_freed_sta) return;
    s_scan_freed_sta = false;
    if (sta_has_creds()) WiFi.begin();
}

void net_scan_start(void) {
    if (!s_wifi_on) return;                 // radio off: no scan
    if (s_scan_state == 1) return;          // already running -> coalesce
    WiFi.scanDelete();                      // drop any stale result
    if (WiFi.status() != WL_CONNECTED) {
        // an in-flight (auto-)reconnect keeps the radio busy and makes the scan bail
        // out instantly, so free the idle STA first. persistent(false) keeps the
        // ESP8266 disconnect from wiping the saved creds.
        WiFi.persistent(false);
        WiFi.disconnect(false /*keep WiFi on*/);
        WiFi.persistent(true);
        s_scan_freed_sta = true;
    }
    WiFi.scanNetworks(true /*async*/, false /*hidden*/);
    s_scan_state = 1;
    s_scan_at = millis();
}

void net_scan_pump(void) {
    if (s_scan_state != 1) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        if ((int32_t)(millis() - (s_scan_at + SCAN_MAX_MS)) >= 0) {   // stuck -> give up
            WiFi.scanDelete();
            s_scan_n = 0;
            scan_resume_sta();
            s_scan_state = 2;               // publish an (empty) fresh result
        }
        return;
    }
    scan_harvest(n < 0 ? 0 : n);            // n<0 (FAILED) -> empty list
    WiFi.scanDelete();
    scan_resume_sta();
    s_scan_state = 2;
    DBG_SERIAL.printf("[net] async scan -> %d\n", s_scan_n);
}

// non-destructive snapshot of the latest result; AP count, or -1 while a scan is
// still running and nothing is cached yet.
static int scan_snapshot(ap_rec *out, int max) {
    if (s_scan_state == 1 && s_scan_n == 0) return -1;
    int n = s_scan_n < max ? s_scan_n : max;
    for (int i = 0; i < n; i++) out[i] = s_scan[i];
    return n;
}

// take a freshly-completed result exactly once (clears the fresh flag); -1 if none
// new. The bridge calls this each cycle to push to the menu the moment a scan ends.
int net_scan_take(ap_rec *out, int max) {
    if (s_scan_state != 2) return -1;
    s_scan_state = 0;
    return scan_snapshot(out, max);
}

// blocking one-shot wrapper for the WebUI: drive the async scan to completion with a
// bounded, WDT-fed wait. Leaves the fresh flag set so the bridge still pushes the
// same result to the menu.
int net_scan(ap_rec *out, int max) {
    net_scan_start();
    unsigned long deadline = millis() + SCAN_MAX_MS;
    while (s_scan_state == 1 && (int32_t)(deadline - millis()) > 0) {
        net_scan_pump();
        delay(50);                          // feed the WDT, let the SDK advance the scan
    }
    int n = scan_snapshot(out, max);
    return n < 0 ? 0 : n;
}

// ---- master enable (menu's EnableWifi) ----
void net_init(void) {
    // boot with the radio OFF; net_set_enabled(true) brings it up once the menu's
    // EnableWifi says so (secure-by-default: no AP/STA/WebUI until explicitly on).
    WiFi.persistent(true);          // keep saved STA creds across the on/off cycle
    WiFi.mode(WIFI_OFF);
    s_wifi_on = false;
    s_ap_up = false;
    s_connected = false;
    s_ap_off_at = 0;
}

void net_set_enabled(bool on) {
    if (on == s_wifi_on) return;    // no transition
    if (on) {
        net_start();                // SoftAP (+ STA auto-reconnect)
        s_wifi_on = true;
    } else {
        s_wifi_on = false;
        s_scan_state = 0;           // abandon any in-flight scan bookkeeping
        WiFi.scanDelete();
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(false /*keep creds*/);
        WiFi.mode(WIFI_OFF);        // radio fully off -> no AP, no WebUI
        s_connected = false;
        s_ap_up = false;
        s_ap_off_at = 0;
    }
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
