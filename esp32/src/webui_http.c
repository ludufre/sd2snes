// webui_http.c - see webui_http.h.
//
// Tiny REST surface translating HTTP -> UART file-server requests:
//   GET  /                 embedded WebUI (gzip)
//   GET  /api/ping         {ver,name}
//   GET  /api/ls?path=     {path,entries:[{name,dir,size}]}
//   GET  /api/dl?path=     file download (octet-stream)
//   POST /api/up?path=     raw body -> file
//   GET  /api/rm?path=     delete
//   GET  /api/mv?from=&to= rename/move
//   GET  /api/mkdir?path=  create directory
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "proto.h"
#include "proto_client.h"
#include "wifi.h"
#include "webui_http.h"

static const char *TAG = "webui";

// WebUI assets, gzipped, embedded as C arrays (see src/www_assets.c, generated
// from www/{index.html,style.css,app.js} by tools/gen_www.sh).
extern const unsigned char index_html_gz[]; extern const unsigned int index_html_gz_len;
extern const unsigned char style_css_gz[];  extern const unsigned int style_css_gz_len;
extern const unsigned char app_js_gz[];     extern const unsigned int app_js_gz_len;

// ---- helpers ----------------------------------------------------------------

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// decode %XX in place (browser uses encodeURIComponent: space=%20, '+' stays '+')
static void url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            int h = hexval(s[1]), l = hexval(s[2]);
            if (h >= 0 && l >= 0) { *d++ = (char)((h << 4) | l); s += 3; continue; }
        }
        *d++ = *s++;
    }
    *d = 0;
}

// fetch a query param, URL-decoded. Returns 1 on success.
static int get_query(httpd_req_t *req, const char *key, char *out, size_t outsz) {
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1 || qlen > 1024) return 0;
    char *q = malloc(qlen);
    if (!q) return 0;
    int ok = 0;
    if (httpd_req_get_url_query_str(req, q, qlen) == ESP_OK &&
        httpd_query_key_value(q, key, out, outsz) == ESP_OK) {
        url_decode(out);
        ok = 1;
    }
    free(q);
    return ok;
}

// send a JSON-escaped string as a response chunk
static void send_json_str(httpd_req_t *req, const char *s, int len) {
    char esc[256]; int o = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (o > (int)sizeof(esc) - 8) { httpd_resp_send_chunk(req, esc, o); o = 0; }
        if (c == '"' || c == '\\') { esc[o++] = '\\'; esc[o++] = c; }
        else if (c < 0x20) { o += snprintf(esc + o, 7, "\\u%04x", c); }
        else esc[o++] = (char)c;
    }
    if (o) httpd_resp_send_chunk(req, esc, o);
}

static esp_err_t reply_status_json(httpd_req_t *req, esp_err_t e, uint8_t st) {
    char buf[64];
    int ok = (e == ESP_OK && st == 0);
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"err\":%d,\"status\":%d}",
             ok ? "true" : "false", (int)e, (int)st);
    if (!ok) httpd_resp_set_status(req, "500 Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

// ---- handlers ---------------------------------------------------------------

static esp_err_t send_gz(httpd_req_t *req, const char *ctype,
                         const unsigned char *data, unsigned int len) {
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");   // avoid stale assets after a reflash
    httpd_resp_send(req, (const char *)data, len);
    return ESP_OK;
}

static esp_err_t h_index(httpd_req_t *req) {
    return send_gz(req, "text/html", index_html_gz, index_html_gz_len);
}
static esp_err_t h_css(httpd_req_t *req) {
    return send_gz(req, "text/css", style_css_gz, style_css_gz_len);
}
static esp_err_t h_js(httpd_req_t *req) {
    return send_gz(req, "application/javascript", app_js_gz, app_js_gz_len);
}

static esp_err_t h_ping(httpd_req_t *req) {
    uint8_t ver = 0, lang = 0; char name[48] = {0};
    proto_lock();
    esp_err_t e = proto_ping(&ver, &lang, name, sizeof(name));
    proto_unlock();
    if (e) { httpd_resp_set_status(req, "504 Gateway Timeout"); httpd_resp_sendstr(req, "{\"ok\":false}"); return ESP_OK; }
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"ver\":%d,\"lang\":%d,\"name\":\"", ver, lang);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, buf);
    send_json_str(req, name, strlen(name));
    httpd_resp_sendstr_chunk(req, "\"}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t h_ls(httpd_req_t *req) {
    char path[260];
    if (!get_query(req, "path", path, sizeof(path))) strcpy(path, "/");

    proto_lock();
    uint8_t st = 0xFF;
    esp_err_t e = proto_ls_open(path, &st);
    if (e || st) { proto_unlock(); return reply_status_json(req, e, st); }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"path\":\"");
    send_json_str(req, path, strlen(path));
    httpd_resp_sendstr_chunk(req, "\",\"entries\":[");

    int first = 1;
    for (;;) {
        uint8_t buf[UP_MAX_PAYLOAD]; uint16_t len = 0; int final = 0;
        if (proto_ls_next(buf, sizeof(buf), &len, &final) != ESP_OK) break;
        int i = 0;
        while (i < len) {
            uint8_t type = buf[i++];
            if (type == 0xFF) { final = 1; break; }
            if (i + 4 > len) break;
            uint32_t size = rd_u32(buf + i); i += 4;
            char *name = (char *)(buf + i);
            int nl = strnlen(name, len - i);
            i += nl + 1;
            char head[24];
            snprintf(head, sizeof(head), "%s{\"name\":\"", first ? "" : ",");
            first = 0;
            httpd_resp_sendstr_chunk(req, head);
            send_json_str(req, name, nl);
            char tail[48];
            snprintf(tail, sizeof(tail), "\",\"dir\":%s,\"size\":%lu}",
                     type == 0 ? "true" : "false", (unsigned long)size);
            httpd_resp_sendstr_chunk(req, tail);
        }
        if (final) break;
    }
    proto_unlock();
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t h_dl(httpd_req_t *req) {
    char path[260];
    if (!get_query(req, "path", path, sizeof(path))) { httpd_resp_set_status(req, "400 Bad Request"); httpd_resp_sendstr(req, "no path"); return ESP_OK; }

    proto_lock();
    uint8_t st = 0xFF; uint32_t total = 0;
    esp_err_t e = proto_get_open(path, &st, &total);
    if (e || st) { proto_unlock(); httpd_resp_set_status(req, "404 Not Found"); httpd_resp_sendstr(req, "open failed"); return ESP_OK; }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char disp[300];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    /* Accumulate many small UART chunks into one big buffer and push them to the
       browser in large HTTP writes - sending each 250B UART chunk as its own
       TCP/WiFi chunk was dominating the per-chunk time. (static: h_dl holds
       proto_lock for the whole download, so it's serialized.) */
    static uint8_t agg[8192];
    uint32_t off = 0; int an = 0;
    while (off < total) {
        uint16_t got = 0; int final = 0;
        if (proto_get_data(off, UP_CHUNK, &st, agg + an, &got, &final) != ESP_OK || st) break;
        an += got; off += got;
        if (an > (int)sizeof(agg) - UP_CHUNK || final || got == 0) {
            if (an) { httpd_resp_send_chunk(req, (const char *)agg, an); an = 0; }
        }
        if (final || got == 0) break;
    }
    proto_unlock();
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_up(httpd_req_t *req) {
    char path[260];
    if (!get_query(req, "path", path, sizeof(path))) { httpd_resp_set_status(req, "400 Bad Request"); httpd_resp_sendstr(req, "no path"); return ESP_OK; }

    int remaining = req->content_len;
    proto_lock();
    uint8_t st = 0xFF;
    esp_err_t e = proto_put_open(path, (uint32_t)remaining, &st);
    if (e || st) { proto_unlock(); return reply_status_json(req, e, st); }

    uint32_t off = 0;
    uint8_t buf[UP_CHUNK];
    int failed = 0;
    while (remaining > 0) {
        int want = remaining > UP_CHUNK ? UP_CHUNK : remaining;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; failed = 1; break; }
        uint16_t credit = 0;
        if (proto_put_data(off, buf, r, &st, &credit) != ESP_OK || st) { failed = 1; break; }
        off += r; remaining -= r;
    }
    uint8_t cst = 0;
    proto_put_close(&cst);
    proto_unlock();
    return reply_status_json(req, failed ? ESP_FAIL : ESP_OK, st);
}

static esp_err_t h_rm(httpd_req_t *req) {
    char path[260];
    if (!get_query(req, "path", path, sizeof(path))) return reply_status_json(req, ESP_ERR_INVALID_ARG, 0xFF);
    proto_lock(); uint8_t st = 0xFF; esp_err_t e = proto_rm(path, &st); proto_unlock();
    return reply_status_json(req, e, st);
}

static esp_err_t h_mkdir(httpd_req_t *req) {
    char path[260];
    if (!get_query(req, "path", path, sizeof(path))) return reply_status_json(req, ESP_ERR_INVALID_ARG, 0xFF);
    proto_lock(); uint8_t st = 0xFF; esp_err_t e = proto_mkdir(path, &st); proto_unlock();
    return reply_status_json(req, e, st);
}

static esp_err_t h_mv(httpd_req_t *req) {
    char from[260], to[260];
    if (!get_query(req, "from", from, sizeof(from)) || !get_query(req, "to", to, sizeof(to)))
        return reply_status_json(req, ESP_ERR_INVALID_ARG, 0xFF);
    proto_lock(); uint8_t st = 0xFF; esp_err_t e = proto_mv(from, to, &st); proto_unlock();
    return reply_status_json(req, e, st);
}

/* ---- WiFi config ---- */

static esp_err_t h_wifi_scan(httpd_req_t *req) {
    static wifi_ap_t aps[24];
    int n = wifi_scan(aps, 24);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < n; i++) {
        char head[16]; snprintf(head, sizeof(head), "%s{\"ssid\":\"", i ? "," : "");
        httpd_resp_sendstr_chunk(req, head);
        send_json_str(req, aps[i].ssid, strlen(aps[i].ssid));
        char tail[48];
        snprintf(tail, sizeof(tail), "\",\"rssi\":%d,\"enc\":%d,\"ch\":%d}",
                 aps[i].rssi, aps[i].enc, aps[i].channel);
        httpd_resp_sendstr_chunk(req, tail);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t h_wifi_status(httpd_req_t *req) {
    wifi_status_t s; wifi_get_status(&s);
    char buf[32];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"connected\":");
    httpd_resp_sendstr_chunk(req, s.connected ? "true" : "false");
    httpd_resp_sendstr_chunk(req, ",\"ssid\":\"");
    send_json_str(req, s.ssid, strlen(s.ssid));
    httpd_resp_sendstr_chunk(req, "\",\"ip\":\"");
    send_json_str(req, s.ip, strlen(s.ip));
    snprintf(buf, sizeof(buf), "\",\"rssi\":%d}", s.rssi);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t h_wifi_connect(httpd_req_t *req) {
    char ssid[64], pass[80];
    if (!get_query(req, "ssid", ssid, sizeof(ssid)))
        return reply_status_json(req, ESP_ERR_INVALID_ARG, 0xFF);
    if (!get_query(req, "pass", pass, sizeof(pass))) pass[0] = 0;
    esp_err_t e = wifi_connect(ssid, pass);
    return reply_status_json(req, e, 0);
}

static esp_err_t h_wifi_forget(httpd_req_t *req) {
    wifi_forget();
    return reply_status_json(req, ESP_OK, 0);
}

void webui_http_start(void) {
    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }

    httpd_uri_t u;
    #define REG(uri_, method_, fn_) do { \
        u.uri = uri_; u.method = method_; u.handler = fn_; u.user_ctx = NULL; \
        httpd_register_uri_handler(srv, &u); } while (0)

    REG("/",          HTTP_GET,  h_index);
    REG("/style.css", HTTP_GET,  h_css);
    REG("/app.js",    HTTP_GET,  h_js);
    REG("/api/ping",  HTTP_GET,  h_ping);
    REG("/api/ls",    HTTP_GET,  h_ls);
    REG("/api/dl",    HTTP_GET,  h_dl);
    REG("/api/up",    HTTP_POST, h_up);
    REG("/api/rm",    HTTP_GET,  h_rm);
    REG("/api/mv",    HTTP_GET,  h_mv);
    REG("/api/mkdir", HTTP_GET,  h_mkdir);
    REG("/api/wifi/scan",    HTTP_GET, h_wifi_scan);
    REG("/api/wifi/status",  HTTP_GET, h_wifi_status);
    REG("/api/wifi/connect", HTTP_GET, h_wifi_connect);
    REG("/api/wifi/forget",  HTTP_GET, h_wifi_forget);
    #undef REG

    ESP_LOGI(TAG, "httpd started");
}
