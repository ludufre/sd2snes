// link.cpp - see link.h. Arduino protocol client, shared by ESP32 and ESP8266
// (the UART object/pins live in platform.h).
#include "proto.h"
#include "platform.h"
#include "link.h"

#define EXT_BAUD        921600
#define RESP_TIMEOUT_MS 2000
#define PUT_TIMEOUT_MS  6000      // a PUT_DATA/PUT_OPEN reply can lag while the MCU's SD
                                  // write allocates a cluster + flushes the FAT (seconds);
                                  // wait it out instead of firing a spurious retry that
                                  // would overrun the MCU's small UART RX ring.
#define WIFI_TO_MS      250       // short timeout for the frequent WiFi-bridge polls
#define DATA_RETRIES    4         // GET/PUT data are idempotent (offset-addressed)

static uint8_t s_seq;

// CRC16/ARC, reflected poly 0xA001, init 0 - identical to the MCU crc16_update.
static uint16_t crc16_upd(uint16_t crc, uint8_t d) {
    crc ^= d;
    for (int i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    return crc;
}

void link_init(void) {
    LINK_SERIAL.setRxBufferSize(1024);
#if defined(ESP8266)
    LINK_SERIAL.begin(EXT_BAUD);
    LINK_SERIAL.swap();            // UART0 -> GPIO13(RX)/GPIO15(TX); USB stays free
#else // ESP32
    LINK_SERIAL.begin(EXT_BAUD, SERIAL_8N1, 16, 17);   // UART2: GPIO16 RX / GPIO17 TX
#endif
    DBG_SERIAL.begin(115200);
    DBG_SERIAL.println(F("\n[sd2snes-companion] MCU link up @921600"));
}

static void send_req(uint8_t op, const uint8_t *pl, uint16_t len, uint8_t *seq_out) {
    uint8_t h[6], c[2];
    uint16_t crc = 0;
    uint8_t s = s_seq++;
    h[0] = UP_SOF; h[1] = 0; h[2] = op; h[3] = s;
    h[4] = (uint8_t)(len & 0xff); h[5] = (uint8_t)(len >> 8);
    for (int i = 1; i < 6; i++) crc = crc16_upd(crc, h[i]);
    for (int i = 0; i < len; i++) crc = crc16_upd(crc, pl[i]);
    c[0] = (uint8_t)(crc & 0xff); c[1] = (uint8_t)(crc >> 8);
    LINK_SERIAL.write(h, 6);
    if (len) LINK_SERIAL.write(pl, len);
    LINK_SERIAL.write(c, 2);
    *seq_out = s;
}

static lerr_t recv_resp(uint8_t exp_seq, uint8_t *rtype, uint8_t *rpl,
                        uint16_t rpl_max, uint16_t *rlen, uint32_t timeout_ms) {
    enum { S_SOF, S_HDR, S_PL, S_CRC } st = S_SOF;
    uint8_t hb[5]; int hn = 0;
    uint16_t need = 0, pn = 0;
    uint8_t cb[2]; int cn = 0;
    static uint8_t plbuf[UP_MAX_PAYLOAD];
    uint32_t deadline = millis() + timeout_ms;

    while ((int32_t)(deadline - millis()) > 0) {
        while (LINK_SERIAL.available()) {
            uint8_t c = (uint8_t)LINK_SERIAL.read();
            switch (st) {
            case S_SOF: if (c == UP_SOF) { st = S_HDR; hn = 0; } break;
            case S_HDR:
                hb[hn++] = c;
                if (hn == 5) {
                    need = (uint16_t)(hb[3] | (hb[4] << 8));
                    if (need > UP_MAX_PAYLOAD) { st = S_SOF; break; }
                    pn = 0; cn = 0; st = need ? S_PL : S_CRC;
                }
                break;
            case S_PL:
                plbuf[pn++] = c;
                if (pn == need) { st = S_CRC; cn = 0; }
                break;
            case S_CRC:
                cb[cn++] = c;
                if (cn == 2) {
                    uint16_t want = (uint16_t)(cb[0] | (cb[1] << 8));
                    uint16_t crc = 0;
                    for (int k = 0; k < 5; k++) crc = crc16_upd(crc, hb[k]);
                    for (int k = 0; k < need; k++) crc = crc16_upd(crc, plbuf[k]);
                    st = S_SOF;
                    if (crc == want && hb[2] == exp_seq && (hb[0] & UP_TYPE_RESP)) {
                        uint16_t cp = need < rpl_max ? need : rpl_max;
                        if (rtype) *rtype = hb[0];
                        if (rpl && cp) memcpy(rpl, plbuf, cp);
                        if (rlen) *rlen = cp;
                        return LOK;
                    }
                }
                break;
            }
        }
        yield();   // feed the soft WDT while waiting for the MCU
    }
    return LERR_TIMEOUT;
}

static lerr_t txn_to(uint8_t op, const uint8_t *pl, uint16_t len,
                     uint8_t *rtype, uint8_t *rpl, uint16_t rpl_max, uint16_t *rlen,
                     uint32_t to) {
    uint8_t seq;
    send_req(op, pl, len, &seq);
    return recv_resp(seq, rtype, rpl, rpl_max, rlen, to);
}
static lerr_t txn(uint8_t op, const uint8_t *pl, uint16_t len,
                  uint8_t *rtype, uint8_t *rpl, uint16_t rpl_max, uint16_t *rlen) {
    return txn_to(op, pl, len, rtype, rpl, rpl_max, rlen, RESP_TIMEOUT_MS);
}

// Discard any bytes sitting in the RX buffer (a late/duplicate reply, or partial
// garbage from an overrun) so the next request's reply parses from a clean state.
static void link_drain(void) {
    int guard = 4096;
    while (guard-- > 0 && LINK_SERIAL.read() >= 0) { /* drop */ }
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

lerr_t proto_ping(uint8_t *ver, uint8_t *lang, char *name, size_t ns) {
    uint8_t r[UP_MAX_PAYLOAD]; uint16_t rl = 0;
    lerr_t e = txn(UP_OP_PING, NULL, 0, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (rl >= 1 && ver) *ver = r[0];
    if (rl >= 2 && lang) *lang = r[1];
    if (name && ns) {
        size_t i = 0;
        while (i + 1 < ns && (2 + i) < rl && r[2 + i]) { name[i] = (char)r[2 + i]; i++; }
        name[i] = 0;
    }
    return LOK;
}

lerr_t proto_ls_open(const char *path, uint8_t *status) {
    uint8_t r[4]; uint16_t rl = 0;
    lerr_t e = txn(UP_OP_LS_OPEN, (const uint8_t *)path, strlen(path) + 1, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return LOK;
}

lerr_t proto_ls_next(uint8_t *buf, size_t bufsz, uint16_t *outlen, int *final) {
    uint8_t rt = 0; uint16_t rl = 0;
    lerr_t e = txn(UP_OP_LS_NEXT, NULL, 0, &rt, buf, bufsz, &rl);
    if (e) return e;
    if (outlen) *outlen = rl;
    if (final) *final = (rt & UP_TYPE_FINAL) ? 1 : 0;
    return LOK;
}

lerr_t proto_get_open(const char *path, uint8_t *status, uint32_t *total) {
    uint8_t r[5]; uint16_t rl = 0;
    lerr_t e = txn(UP_OP_GET_OPEN, (const uint8_t *)path, strlen(path) + 1, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (rl < 5) return LERR_FAIL;
    if (status) *status = r[0];
    if (total)  *total = rd_u32(r + 1);
    return LOK;
}

lerr_t proto_get_data(uint32_t off, uint16_t want, uint8_t *status,
                      uint8_t *buf, uint16_t *got, int *final) {
    uint8_t req[6]; uint8_t r[UP_MAX_PAYLOAD]; uint16_t rl = 0; uint8_t rt = 0;
    if (want > UP_CHUNK) want = UP_CHUNK;
    wr_u32(req, off); req[4] = (uint8_t)(want & 0xff); req[5] = (uint8_t)(want >> 8);
    lerr_t e = LERR_TIMEOUT;
    for (int a = 0; a < DATA_RETRIES; a++) {
        e = txn(UP_OP_GET_DATA, req, 6, &rt, r, sizeof(r), &rl);
        if (e == LOK && rl >= 3) break;
    }
    if (e) return e;
    if (rl < 3) return LERR_FAIL;
    uint16_t g = (uint16_t)(r[1] | (r[2] << 8));
    if (g > rl - 3) g = rl - 3;
    if (status) *status = r[0];
    if (buf && g) memcpy(buf, r + 3, g);
    if (got) *got = g;
    if (final) *final = (rt & UP_TYPE_FINAL) ? 1 : 0;
    return LOK;
}

lerr_t proto_put_open(const char *path, uint32_t total, uint8_t *status) {
    uint8_t req[4 + 256]; uint16_t pl = strlen(path) + 1;
    if (pl > 256) return LERR_FAIL;
    wr_u32(req, total);
    memcpy(req + 4, path, pl);
    uint8_t r[1]; uint16_t rl = 0;
    lerr_t e = LERR_TIMEOUT;
    for (int a = 0; a < DATA_RETRIES; a++) {
        if (a) { delay(40); link_drain(); }   // CREATE_ALWAYS may stall on FAT I/O
        e = txn_to(UP_OP_PUT_OPEN, req, 4 + pl, NULL, r, sizeof(r), &rl, PUT_TIMEOUT_MS);
        if (e == LOK && rl) break;
    }
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return LOK;
}

lerr_t proto_put_data(uint32_t off, const uint8_t *buf, uint16_t len,
                      uint8_t *status, uint16_t *credit) {
    uint8_t req[6 + UP_CHUNK]; uint8_t r[3]; uint16_t rl = 0;
    if (len > UP_CHUNK) len = UP_CHUNK;
    wr_u32(req, off); req[4] = (uint8_t)(len & 0xff); req[5] = (uint8_t)(len >> 8);
    memcpy(req + 6, buf, len);
    lerr_t e = LERR_TIMEOUT;
    for (int a = 0; a < DATA_RETRIES; a++) {
        // before re-sending, let the MCU finish the slow write and clear its RX
        // ring, then drop its (late) reply -> the retry lands in a clean ring.
        if (a) { delay(40); link_drain(); }
        e = txn_to(UP_OP_PUT_DATA, req, 6 + len, NULL, r, sizeof(r), &rl, PUT_TIMEOUT_MS);
        if (e == LOK && rl >= 3) break;
    }
    if (e) return e;
    if (rl < 3) return LERR_FAIL;
    if (status) *status = r[0];
    if (credit) *credit = (uint16_t)(r[1] | (r[2] << 8));
    return LOK;
}

lerr_t proto_put_close(uint8_t *status) {
    uint8_t r[1]; uint16_t rl = 0;
    lerr_t e = txn(UP_OP_PUT_CLOSE, NULL, 0, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return LOK;
}

static lerr_t simple_path_op(uint8_t op, const char *path, uint8_t *status) {
    uint8_t r[1]; uint16_t rl = 0;
    lerr_t e = txn(op, (const uint8_t *)path, strlen(path) + 1, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return LOK;
}
lerr_t proto_rm(const char *path, uint8_t *status)    { return simple_path_op(UP_OP_RM, path, status); }
lerr_t proto_mkdir(const char *path, uint8_t *status) { return simple_path_op(UP_OP_MKDIR, path, status); }

lerr_t proto_mv(const char *from, const char *to, uint8_t *status) {
    uint8_t req[UP_MAX_PAYLOAD]; uint16_t fl = strlen(from) + 1, tl = strlen(to) + 1;
    if (fl + tl > UP_MAX_PAYLOAD) return LERR_FAIL;
    memcpy(req, from, fl); memcpy(req + fl, to, tl);
    uint8_t r[1]; uint16_t rl = 0;
    lerr_t e = txn(UP_OP_MV, req, fl + tl, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return LOK;
}

// ---- WiFi bridge ----
static uint16_t put_cstr(uint8_t *p, const char *s, uint16_t max) {
    uint16_t i = 0;
    while (s[i] && i < max) { p[i] = (uint8_t)s[i]; i++; }
    p[i++] = 0;
    return i;
}

lerr_t proto_wifi_report(uint8_t connected, int8_t rssi, const char *ssid, const char *ip) {
    uint8_t req[2 + UP_WIFI_SSID_MAX + 1 + 16]; uint16_t n = 0;
    req[n++] = connected ? 1 : 0;
    req[n++] = (uint8_t)rssi;
    n += put_cstr(req + n, ssid ? ssid : "", UP_WIFI_SSID_MAX);
    n += put_cstr(req + n, ip ? ip : "", 15);
    uint8_t r[1]; uint16_t rl = 0;
    return txn_to(UP_OP_WIFI_REPORT, req, n, NULL, r, sizeof(r), &rl, WIFI_TO_MS);
}

lerr_t proto_wifi_poll(uint8_t *action, char *ssid, size_t ssz, char *pass, size_t psz) {
    uint8_t r[UP_MAX_PAYLOAD]; uint16_t rl = 0;
    if (ssid && ssz) ssid[0] = 0;
    if (pass && psz) pass[0] = 0;
    lerr_t e = txn_to(UP_OP_WIFI_POLL, NULL, 0, NULL, r, sizeof(r), &rl, WIFI_TO_MS);
    if (e) return e;
    if (rl < 1) { if (action) *action = UP_WIFI_NONE; return LOK; }
    if (action) *action = r[0];
    if (r[0] == UP_WIFI_CONNECT) {
        uint16_t i = 1, j = 0;
        while (i < rl && r[i] && ssid && j + 1 < ssz) ssid[j++] = (char)r[i++];
        if (ssid && ssz) ssid[j] = 0;
        while (i < rl && r[i]) i++;
        if (i < rl) i++;
        j = 0;
        while (i < rl && r[i] && pass && j + 1 < psz) pass[j++] = (char)r[i++];
        if (pass && psz) pass[j] = 0;
    }
    return LOK;
}

lerr_t proto_wifi_scan_push(const ap_rec *aps, int n) {
    uint8_t req[UP_MAX_PAYLOAD]; uint16_t o = 1;
    if (n > UP_WIFI_MAX_APS) n = UP_WIFI_MAX_APS;
    int put = 0;
    for (int i = 0; i < n; i++) {
        uint16_t sl = strlen(aps[i].ssid);
        if (sl > UP_WIFI_SSID_MAX) sl = UP_WIFI_SSID_MAX;
        if (o + 2 + sl + 1 > UP_MAX_PAYLOAD) break;
        req[o++] = (uint8_t)aps[i].rssi;
        req[o++] = aps[i].enc ? 1 : 0;
        memcpy(req + o, aps[i].ssid, sl); o += sl; req[o++] = 0;
        put++;
    }
    req[0] = (uint8_t)put;
    uint8_t r[1]; uint16_t rl = 0;
    return txn(UP_OP_WIFI_SCAN, req, o, NULL, r, sizeof(r), &rl);
}

lerr_t proto_esp_info(const char *s) {
    uint16_t n = strlen(s);
    if (n > 38) n = 38;
    uint8_t r[1]; uint16_t rl = 0;
    return txn_to(UP_OP_ESP_INFO, (const uint8_t *)s, n + 1, NULL, r, sizeof(r), &rl, WIFI_TO_MS);
}
