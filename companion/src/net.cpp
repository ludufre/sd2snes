// net.cpp - see net.h. WiFi is API-compatible across Arduino-ESP32/ESP8266; the
// only difference is the "open network" auth constant (platform.h::ENC_IS_OPEN).
#include "platform.h"
#include "net.h"
#if defined(ESP32)
  #include "esp_wifi.h"   // esp_wifi_get_config (read the saved STA SSID)
#endif

#define AP_PASS         "sd2snes0"   // >= 8 chars (WPA2)
#define CONNECT_TO_MS   15000        // give a STA join this long before falling back to the portal

static char  s_ap[20];
static bool  s_connected = false;
static bool  s_ap_up     = true;
static bool  s_wifi_on   = false;       // master: radio up at all (menu's EnableWifi)
static bool  s_connecting = false;      // a STA join is in flight (STA-only window)
static unsigned long s_connect_deadline = 0;

// Operational WiFi logging on the debug console (DBG_SERIAL = the C3's native
// USB-CDC; only seen when someone runs `pio ... -t monitor`). Fires on state
// changes only, so no spam. The disconnect-reason decode makes field diagnosis
// (wrong key vs out-of-range vs auth/PMF reject) obvious without a lookup table.
#if defined(ESP32)
static const char *disc_reason_str(uint8_t r) {
    switch (r) {
        case 1:   return "UNSPECIFIED";
        case 2:   return "AUTH_EXPIRE";
        case 4:   return "ASSOC_EXPIRE";
        case 15:  return "4WAY_HANDSHAKE_TIMEOUT -> wrong password (most common)";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND -> SSID not in range / wrong name";
        case 202: return "AUTH_FAIL -> password/auth rejected by the AP";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default:  return "see esp_wifi_types.h";
    }
}

static void on_wifi_event(WiFiEvent_t ev, WiFiEventInfo_t info) {
    switch (ev) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            DBG_SERIAL.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint8_t r = info.wifi_sta_disconnected.reason;
            DBG_SERIAL.printf("[wifi] STA disconnected, reason=%u (%s)\n", r, disc_reason_str(r));
            break;
        }
        default: break;
    }
}
#endif

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

// Associate to a network in STA-ONLY mode. The C3 has a single radio: an AP+STA
// SoftAP pinned to channel 1 cannot coexist with a STA whose AP lives on another
// channel (the common mesh / multi-AP case - same SSID on ch 1/6/11), so the join
// keeps failing with reason 2. Drop the SoftAP for the attempt -> the radio is free
// to follow the target's channel. The portal is restored from net_loop if the join
// doesn't settle within CONNECT_TO_MS. ssid==NULL -> use the saved credentials.
// Join the saved SSID's STRONGEST AP. WiFi.begin() (and even WIFI_CONNECT_AP_BY_SIGNAL)
// grabbed a -69 ch6 sibling while a -53 ch11 one existed in the mesh -> slow. So scan all
// channels ourselves, find the best-RSSI matching BSSID, and connect to THAT exact AP.
static void sta_join_strongest(void) {
#if defined(ESP32)
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0]) {
        int n = WiFi.scanNetworks(false /*sync*/, false /*hidden*/);   // ~2-3s, all channels
        int best = -1; int8_t br = -127;
        for (int i = 0; i < n; i++)
            if (WiFi.RSSI(i) > br && WiFi.SSID(i) == (const char *)cfg.sta.ssid) { best = i; br = WiFi.RSSI(i); }
        if (best >= 0) {
            DBG_SERIAL.printf("[wifi] join strongest %s ch=%d rssi=%d\n",
                              WiFi.BSSIDstr(best).c_str(), WiFi.channel(best), (int)br);
            WiFi.begin((const char *)cfg.sta.ssid, (const char *)cfg.sta.password,
                       WiFi.channel(best), WiFi.BSSID(best));
            WiFi.scanDelete();
            return;
        }
        WiFi.scanDelete();
    }
#endif
    WiFi.begin();                   // fallback: saved creds, default selection
}

static void sta_join(const char *ssid, const char *pass) {
    WiFi.softAPdisconnect(true);    // portal down for the duration of the attempt
    s_ap_up = false;
    WiFi.persistent(true);          // keep creds across the on/off cycle
    WiFi.mode(WIFI_STA);            // STA-only: no channel pinned by the SoftAP
#if defined(ESP32)
    WiFi.setSleep(false);           // keep the radio awake during the handshake
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    // C3 SuperMini: weak antenna/power path -> at full TX power the auth frames come
    // out distorted and the AP can't decode them -> reason 2 (AUTH_EXPIRE) even with a
    // correct WPA2 key and -50 dBm RX. Lower TX power = cleaner signal (plenty close).
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
    WiFi.disconnect(false);         // clear any half-open attempt, keep the radio on
    delay(50);
    if (ssid) WiFi.begin(ssid, pass);   // explicit network from the menu
    else      sta_join_strongest();     // saved network: pick the strongest mesh AP
    s_connecting = true;
    s_connect_deadline = millis() + CONNECT_TO_MS;
}

void net_start(void) {
    WiFi.persistent(true);          // SDK stores the last STA creds
#if defined(ESP32)
    WiFi.setAutoReconnect(false);   // we drive (re)connects ourselves (STA-only); the
                                    // core's auto-reconnect storms reason 2 in AP+STA
#endif
    WiFi.mode(WIFI_AP_STA);         // a mode must be set to read the AP MAC for the name
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    WiFi.setTxPower(WIFI_POWER_8_5dBm);   // C3 SuperMini: tame TX so auth frames aren't distorted
#endif
    uint8_t mac[6]; WiFi.softAPmacAddress(mac);
    snprintf(s_ap, sizeof(s_ap), "sd2snes-%02X%02X", mac[4], mac[5]);
    s_connecting = false;
    s_connected  = false;
    if (sta_has_creds()) {
        DBG_SERIAL.println(F("[wifi] saved creds -> STA-only auto-join"));
        sta_join(NULL, NULL);       // try the saved network first (portal returns on timeout)
    } else {
        DBG_SERIAL.println(F("[wifi] no saved creds -> portal (SoftAP) only"));
        WiFi.softAP(s_ap, AP_PASS, 1);
        s_ap_up = true;
    }
}

void net_loop(void) {
    net_scan_pump();            // harvest a finished async scan (non-blocking)
    if (!s_wifi_on) return;     // WiFi master-off: nothing to track
    bool now = (WiFi.status() == WL_CONNECTED);
    if (now && !s_connected) {
        s_connected = true;
        s_connecting = false;   // joined; we are STA-only (AP was dropped for the attempt)
        DBG_SERIAL.printf("[wifi] CONNECTED to \"%s\" ch=%d ip=%s (STA-only, portal off)\n",
                          WiFi.SSID().c_str(), WiFi.channel(), WiFi.localIP().toString().c_str());
    } else if (!now && s_connected) {
        s_connected = false;
        DBG_SERIAL.println(F("[wifi] link lost -> portal back"));
        ap_bring_up();          // lost the link -> AP back for access
    }
    // join didn't settle in time: stop trying and bring the portal back so the user
    // still has the SoftAP/WebUI to retry from.
    if (s_connecting && (int32_t)(millis() - s_connect_deadline) >= 0) {
        s_connecting = false;
        DBG_SERIAL.println(F("[wifi] join timed out -> portal back (SoftAP)"));
        WiFi.disconnect(false);
        ap_bring_up();
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
static bool    s_scan_was_connected = false;   // restore the STA link after the scan
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

// After a scan, restore the STA link via sta_join (which now re-scans + picks the STRONGEST
// mesh AP) - so a scan also upgrades us to the best BSSID, not back to the weak one we were
// on. autoReconnect is off, so sta_join here is one-shot (no reason-2 storm).
static void scan_resume_sta(void) {
    if (s_scan_freed_sta && s_scan_was_connected) sta_join(NULL, NULL);
    s_scan_freed_sta = false;
}

void net_scan_start(void) {
    if (!s_wifi_on) return;                 // radio off: no scan
    if (s_scan_state == 1 || s_scan_state == 3) return;   // already in progress -> coalesce
    WiFi.scanDelete();                      // drop any stale result
    // ALWAYS free the STA before scanning so the sweep covers ALL channels. A *connected*
    // STA parks the radio on its home channel, so a scan would only return that channel's
    // APs -> the user can't see networks on other channels (1/11/...). Disconnect (keep
    // creds) for the sweep; scan_resume_sta() reconnects afterwards if we were connected.
    s_scan_was_connected = (WiFi.status() == WL_CONNECTED);
    WiFi.persistent(false);                 // keep the ESP8266 from wiping saved creds
    WiFi.disconnect(false /*keep WiFi on*/);
    WiFi.persistent(true);
    s_scan_freed_sta = true;
    s_scan_at = millis();
    s_scan_state = 3;   // PENDING: let the disconnect settle before scanning. Scanning a
                        // still-busy radio (mid-disconnect) makes scanNetworks bail out
                        // instantly -> "no networks" when scanning while connected.
}

void net_scan_pump(void) {
    if (s_scan_state == 3) {                 // pending: start the real scan once settled
        if ((int32_t)(millis() - (s_scan_at + 250)) >= 0) {
            WiFi.scanNetworks(true /*async*/, false /*hidden*/);
            s_scan_state = 1;
            s_scan_at = millis();
        }
        return;
    }
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
    unsigned long deadline = millis() + SCAN_MAX_MS + 250;   // +settle window
    while ((s_scan_state == 1 || s_scan_state == 3) && (int32_t)(deadline - millis()) > 0) {
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
#if defined(ESP32)
    WiFi.onEvent(on_wifi_event);    // DEBUG: connect/disconnect-reason trace
#endif
    WiFi.persistent(true);          // keep saved STA creds across the on/off cycle
    WiFi.mode(WIFI_OFF);
    s_wifi_on = false;
    s_ap_up = false;
    s_connected = false;
    s_connecting = false;
}

void net_set_enabled(bool on) {
    if (on == s_wifi_on) return;    // no transition
    DBG_SERIAL.printf("[wifi] radio %s by menu (EnableWifi=%d)\n", on ? "ENABLED" : "DISABLED", on);
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
        s_connecting = false;
    }
}

bool net_connect(const char *ssid, const char *pass) {
    DBG_SERIAL.printf("[wifi] connect request: ssid=\"%s\" (pass %u chars)\n",
                      ssid, (unsigned)strlen(pass));   // password length only, never the value
    sta_join(ssid, pass);           // STA-only join (frees the radio channel; portal back on timeout)
    return true;
}

void net_forget(void) {
    WiFi.disconnect(true);          // erase stored creds
    s_connected = false;
    s_connecting = false;
    s_ap_up = false;                // force ap_bring_up to re-create the portal
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
