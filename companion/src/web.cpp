// web.cpp - see web.h. WebServer (ESP32) / ESP8266WebServer share the same API;
// platform.h typedefs the right class as WebServerT.
#include "platform.h"
#include "proto.h"
#include "link.h"
#include "net.h"
#include "assets.h"
#include "version.h"
#include "web.h"

static WebServerT server(80);
static bool s_up = false;        // listener begun (tracks the WiFi radio state)

// upload state (single in-flight upload; single-threaded loop)
static uint32_t g_up_off;
static bool     g_up_failed;
static uint8_t  g_up_status;

static String jsonesc(const String &s) {
    String o; o.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

static void reply_status(lerr_t e, uint8_t st) {
    bool ok = (e == LOK && st == 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"err\":%d,\"status\":%d}",
             ok ? "true" : "false", (int)e, (int)st);
    server.send(ok ? 200 : 500, "application/json", buf);
}

static void send_gz(const char *ctype, const unsigned char *data, unsigned int len) {
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-cache");
    server.send_P(200, ctype, (PGM_P)data, len);
}

static void h_index() { send_gz("text/html", index_html_gz, index_html_gz_len); }
static void h_css()   { send_gz("text/css", style_css_gz, style_css_gz_len); }
static void h_js()    { send_gz("application/javascript", app_js_gz, app_js_gz_len); }

static void h_ping() {
    uint8_t ver = 0, lang = 0; char name[48] = {0};
    if (proto_ping(&ver, &lang, name, sizeof(name)) != LOK) {
        server.send(504, "application/json", "{\"ok\":false}");
        return;
    }
    String j = "{\"ok\":true,\"ver\":" + String(ver) + ",\"lang\":" + String(lang) +
               ",\"name\":\"" + jsonesc(name) + "\",\"fw\":\"" + FW_VERSION_STR + "\"}";
    server.send(200, "application/json", j);
}

static void h_ls() {
    String path = server.hasArg("path") ? server.arg("path") : String("/");
    uint8_t st = 0xFF;
    if (proto_ls_open(path.c_str(), &st) != LOK || st) { reply_status(LOK, st); return; }
    // Stream the body in chunks: build into a String and flush (sendContent) at ~3KB so the
    // heap stays bounded for ANY directory size. Building the WHOLE body in one String OOMs
    // on a big dir -> the heap edge appends the 1-byte comma but drops the ~60-byte entry
    // -> orphan commas -> invalid JSON. (The setRxFIFOFull fix already tames the 3M
    // UART-vs-WiFi race, so the occasional flush between LS_NEXT reads is safe.)
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);   // chunked
    server.send(200, "application/json", "");
    server.client().setNoDelay(true);
    String body; body.reserve(4096);
    body = "{\"path\":\"" + jsonesc(path) + "\",\"entries\":[";
    bool first = true;
    for (;;) {
        uint8_t buf[UP_MAX_PAYLOAD]; uint16_t len = 0; int fin = 0;
        if (proto_ls_next(buf, sizeof(buf), &len, &fin) != LOK) break;
        int i = 0;
        while (i < len) {
            uint8_t type = buf[i++];
            if (type == 0xFF) { fin = 1; break; }
            if (i + 4 > len) break;
            uint32_t size = (uint32_t)buf[i] | ((uint32_t)buf[i+1] << 8) |
                            ((uint32_t)buf[i+2] << 16) | ((uint32_t)buf[i+3] << 24);
            i += 4;
            int nl = 0; while (i + nl < len && buf[i + nl]) nl++;
            String name; for (int k = 0; k < nl; k++) name += (char)buf[i + k];
            i += nl + 1;
            body += first ? "" : ",";
            first = false;
            body += "{\"name\":\"" + jsonesc(name) + "\",\"dir\":" +
                    (type == 0 ? "true" : "false") + ",\"size\":" + String(size) + "}";
        }
        if (body.length() >= 3072) { server.sendContent(body); body = ""; }   // bound heap
        if (fin) break;
    }
    body += "]}";
    server.sendContent(body);
    server.sendContent("");   // end chunked
}

// download sink for proto_get_stream: batch the pipelined-GET chunks into 8KB TCP writes
// (setNoDelay + big writes beat the per-chunk Nagle stall). client.write() blocks for TCP
// flow control (resumes as the receiver ACKs) and returns on a dropped socket - that's the
// reliable path. (availableForWrite() is unreliable on the ESP32 WiFiClient: it returns 0,
// so a non-blocking gate never writes.) Single download at a time (the loop is busy here).
static WiFiClient *s_dl_client = nullptr;
static uint8_t s_dlbuf[8192]; static uint16_t s_dlfill = 0;
static int dl_sink(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx;
    while (len) {
        uint16_t space = (uint16_t)sizeof(s_dlbuf) - s_dlfill;
        uint16_t n = len < space ? len : space;
        memcpy(s_dlbuf + s_dlfill, data, n); s_dlfill += n; data += n; len -= n;
        if (s_dlfill == sizeof(s_dlbuf)) {
            if (!s_dl_client->connected()) return 1;
            // client.write() is bounded by SO_SNDTIMEO (h_dl set client.setTimeout) so a
            // stalled WiFi TX returns a SHORT write instead of blocking the loop forever
            // (the wedge that left the WROOM unreachable). Short write -> abort cleanly.
            size_t w = s_dl_client->write(s_dlbuf, s_dlfill);
            if (w < s_dlfill) return 1;
            s_dlfill = 0;
        }
    }
    return s_dl_client->connected() ? 0 : 1;
}

static void h_dl() {
    if (!server.hasArg("path")) { server.send(400, "text/plain", "no path"); return; }
    String path = server.arg("path");
    uint8_t st = 0xFF; uint32_t total = 0;
    if (proto_get_open(path.c_str(), &st, &total) != LOK || st) {
        server.send(404, "text/plain", "open failed"); return;
    }
    const char *base = strrchr(path.c_str(), '/');
    base = base ? base + 1 : path.c_str();
    server.sendHeader("Content-Disposition", String("attachment; filename=\"") + base + "\"");
    server.setContentLength(total);
    server.send(200, "application/octet-stream", "");
    WiFiClient client = server.client();
    client.setNoDelay(true);             // no Nagle: don't wait for an ACK per small write
    client.setTimeout(8);                // 8s SO_SNDTIMEO: a stalled TX returns short, never
                                         // blocks the loop forever (no more WROOM wedge)
    s_dl_client = &client; s_dlfill = 0;
    proto_get_stream(total, dl_sink, nullptr);                 // pipelined GET -> batched sink
    if (s_dlfill && client.connected()) client.write(s_dlbuf, s_dlfill);   // flush tail
}

static void h_up_done() {
    reply_status(g_up_failed ? LERR_FAIL : LOK, g_up_status);
}
static void h_up_data() {
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        g_up_off = 0; g_up_failed = false; g_up_status = 0;
        String path = server.arg("path");
        uint8_t st = 0xFF;
        if (proto_put_open(path.c_str(), 0, &st) != LOK || st) { g_up_failed = true; g_up_status = st; }
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (!g_up_failed) {
            uint32_t left = u.currentSize; uint8_t *p = u.buf;
            while (left) {
                uint16_t n = left > UP_CHUNK ? UP_CHUNK : left;
                uint8_t st = 0xFF; uint16_t cr = 0;
                if (proto_put_data(g_up_off, p, n, &st, &cr) != LOK || st) { g_up_failed = true; g_up_status = st; break; }
                g_up_off += n; p += n; left -= n; yield();
            }
        }
    } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
        uint8_t cst = 0; proto_put_close(&cst);
    }
}

static void h_rm() {
    uint8_t st = 0xFF; lerr_t e = proto_rm(server.arg("path").c_str(), &st);
    reply_status(e, st);
}
static void h_mkdir() {
    uint8_t st = 0xFF; lerr_t e = proto_mkdir(server.arg("path").c_str(), &st);
    reply_status(e, st);
}
static void h_mv() {
    uint8_t st = 0xFF; lerr_t e = proto_mv(server.arg("from").c_str(), server.arg("to").c_str(), &st);
    reply_status(e, st);
}
static void h_apply() {   // WebUI wrote config/theme/lists -> ask the menu to hot-reload (no power-cycle)
    uint8_t st = 0xFF; lerr_t e = proto_hot_reload(&st);
    reply_status(e, st);
}
static void h_abort() {   // cancel an in-flight transfer -> free the MCU file/dir handle
    uint8_t st = 0xFF; lerr_t e = proto_abort(&st);
    reply_status(e, st);
}

static void h_wifi_scan() {
    ap_rec aps[16]; int n = net_scan(aps, 16);
    String j = "[";
    for (int i = 0; i < n; i++) {
        if (i) j += ",";
        j += "{\"ssid\":\"" + jsonesc(aps[i].ssid) + "\",\"rssi\":" + String(aps[i].rssi) +
             ",\"enc\":" + String(aps[i].enc) + ",\"ch\":0}";
    }
    j += "]";
    server.send(200, "application/json", j);
}
static void h_wifi_status() {
    net_status s; net_get_status(&s);
    String j = "{\"connected\":";
    j += s.connected ? "true" : "false";
    j += ",\"ssid\":\"" + jsonesc(s.ssid) + "\",\"ip\":\"" + jsonesc(s.ip) +
         "\",\"rssi\":" + String(s.rssi) + "}";
    server.send(200, "application/json", j);
}
static void h_wifi_connect() {
    if (!server.hasArg("ssid")) { reply_status(LERR_FAIL, 0xFF); return; }
    net_connect(server.arg("ssid").c_str(), server.hasArg("pass") ? server.arg("pass").c_str() : "");
    reply_status(LOK, 0);
}
static void h_wifi_forget() { net_forget(); reply_status(LOK, 0); }

// Register the routes ONCE at boot. We deliberately do NOT call server.begin()
// here: at boot the radio (and thus the lwIP tcpip stack) is OFF by design
// (net_init -> WiFi.mode(WIFI_OFF)), and begin() binds a TCP socket through the
// lwIP tcpip thread. With no stack up that asserts ("tcpip_send_msg_wait_sem ...
// Invalid mbox") and the C3 boot-loops. The listener follows the radio instead
// (web_set_enabled), so it only ever begins once net_set_enabled(true) has
// brought lwIP up.
void web_init(void) {
    server.on("/", HTTP_GET, h_index);
    server.on("/style.css", HTTP_GET, h_css);
    server.on("/app.js", HTTP_GET, h_js);
    server.on("/api/ping", HTTP_GET, h_ping);
    server.on("/api/ls", HTTP_GET, h_ls);
    server.on("/api/dl", HTTP_GET, h_dl);
    server.on("/api/up", HTTP_POST, h_up_done, h_up_data);
    /* mutating endpoints are POST so a cross-origin <img>/prefetch can't fire
       them (no auth exists, so GET side-effects would be trivially CSRF-able) */
    server.on("/api/rm", HTTP_POST, h_rm);
    server.on("/api/mv", HTTP_POST, h_mv);
    server.on("/api/mkdir", HTTP_POST, h_mkdir);
    server.on("/api/apply", HTTP_POST, h_apply);
    server.on("/api/abort", HTTP_POST, h_abort);
    server.on("/api/wifi/scan", HTTP_GET, h_wifi_scan);
    server.on("/api/wifi/status", HTTP_GET, h_wifi_status);
    server.on("/api/wifi/connect", HTTP_POST, h_wifi_connect);
    server.on("/api/wifi/forget", HTTP_POST, h_wifi_forget);
}

// Begin/stop the HTTP listener to match the WiFi master switch. Idempotent: the
// caller (the MCU WiFi bridge) drives this every poll with the current state.
// MUST be called only AFTER net_set_enabled(on) so the lwIP stack is already up
// when on==true (see web_init for why begin() can't run at boot).
void web_set_enabled(bool on) {
    if (on == s_up) return;
    if (on) server.begin();    // lwIP is up (radio just came on) -> safe to listen
    else    server.stop();     // radio going down -> release the socket
    s_up = on;
}

void web_loop(void) { if (s_up) server.handleClient(); }   // no-op until the radio is up
