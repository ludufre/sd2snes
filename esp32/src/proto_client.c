// proto_client.c - see proto_client.h.
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "proto.h"
#include "proto_client.h"

#define EXT_UART        UART_NUM_2
#define EXT_TX_PIN      GPIO_NUM_17     // TX2 -> MCU RXD0 (P0.3)
#define EXT_RX_PIN      GPIO_NUM_16     // RX2 <- MCU TXD0 (P0.2)
#define EXT_BAUD        921600    // sweet spot: higher baud is round-trip-bound (no gain) or errors
#define EXT_RX_RING     4096
#define RESP_TIMEOUT_MS 2000
#define DATA_RETRIES    4      // GET_DATA/PUT_DATA are idempotent (offset-addressed) -> safe to retry

static const char *TAG = "proto";
static SemaphoreHandle_t s_lock;
static uint8_t s_seq;

// CRC16/ARC, reflected poly 0x8005 (0xA001), init 0, no final xor -
// identical to the MCU's crc16_update() per-byte step.
static uint16_t crc16_upd(uint16_t crc, uint8_t d) {
    crc ^= d;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    return crc;
}

void proto_init(void) {
    const uart_config_t cfg = {
        .baud_rate  = EXT_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(EXT_UART, EXT_RX_RING, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(EXT_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(EXT_UART, EXT_TX_PIN, EXT_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "UART2 link @ %d (GPIO%d RX / GPIO%d TX)", EXT_BAUD, EXT_RX_PIN, EXT_TX_PIN);
}

void proto_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
void proto_unlock(void) { xSemaphoreGive(s_lock); }

static void send_req(uint8_t op, const uint8_t *pl, uint16_t len, uint8_t *seq_out) {
    uint8_t h[6], c[2];
    uint16_t crc = 0;
    uint8_t s = s_seq++;
    h[0] = UP_SOF; h[1] = 0 /*req*/; h[2] = op; h[3] = s;
    h[4] = (uint8_t)(len & 0xff); h[5] = (uint8_t)(len >> 8);
    for (int i = 1; i < 6; i++) crc = crc16_upd(crc, h[i]);
    for (int i = 0; i < len; i++) crc = crc16_upd(crc, pl[i]);
    c[0] = (uint8_t)(crc & 0xff); c[1] = (uint8_t)(crc >> 8);
    uart_write_bytes(EXT_UART, (const char *)h, 6);
    if (len) uart_write_bytes(EXT_UART, (const char *)pl, len);
    uart_write_bytes(EXT_UART, (const char *)c, 2);
    *seq_out = s;
}

// Wait for a RESPONSE frame matching exp_seq. Copies up to rpl_max payload bytes.
static esp_err_t recv_resp(uint8_t exp_seq, uint8_t *rtype, uint8_t *rpl,
                           uint16_t rpl_max, uint16_t *rlen, int timeout_ms) {
    enum { S_SOF, S_HDR, S_PL, S_CRC } st = S_SOF;
    uint8_t hb[5]; int hn = 0;
    uint16_t need = 0, pn = 0;
    uint8_t cb[2]; int cn = 0;
    static uint8_t plbuf[UP_MAX_PAYLOAD];
    uint8_t tmp[128];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while ((int)(deadline - xTaskGetTickCount()) > 0) {
        /* Drain exactly what's buffered (non-blocking). Asking uart_read_bytes
           for a fixed count would block the full 10ms tick waiting for the tail
           of each response that never fills the buffer -> ~10ms wasted/chunk.
           When nothing is buffered, block for just the first byte (wakes on RX). */
        size_t avail = 0;
        uart_get_buffered_data_len(EXT_UART, &avail);
        int n;
        if (avail > 0) {
            n = uart_read_bytes(EXT_UART, tmp, avail < sizeof(tmp) ? avail : sizeof(tmp), 0);
        } else {
            n = uart_read_bytes(EXT_UART, tmp, 1, pdMS_TO_TICKS(10));
        }
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            uint8_t c = tmp[i];
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
                        uint16_t cp = need;
                        if (cp > rpl_max) cp = rpl_max;
                        if (rtype) *rtype = hb[0];
                        if (rpl && cp) memcpy(rpl, plbuf, cp);
                        if (rlen) *rlen = cp;
                        return ESP_OK;
                    }
                    // else: bad CRC or stale seq -> keep scanning
                }
                break;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

// Single-attempt transaction (link is verified-clean; the WebUI/browser retries
// whole operations on the rare timeout). Avoids non-idempotent LS_NEXT re-sends.
static esp_err_t txn(uint8_t op, const uint8_t *pl, uint16_t len,
                     uint8_t *rtype, uint8_t *rpl, uint16_t rpl_max, uint16_t *rlen) {
    uint8_t seq;
    send_req(op, pl, len, &seq);
    return recv_resp(seq, rtype, rpl, rpl_max, rlen, RESP_TIMEOUT_MS);
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

esp_err_t proto_ping(uint8_t *pv, uint8_t *lang, char *name, size_t ns) {
    /* response: [proto_ver(1)][lang(1)][device_name NUL-term] */
    uint8_t r[UP_MAX_PAYLOAD]; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_PING, NULL, 0, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (rl >= 1 && pv) *pv = r[0];
    if (rl >= 2 && lang) *lang = r[1];
    if (name && ns) {
        size_t i = 0;
        while (i + 1 < ns && (2 + i) < rl && r[2 + i]) { name[i] = (char)r[2 + i]; i++; }
        name[i] = 0;
    }
    return ESP_OK;
}

esp_err_t proto_ls_open(const char *path, uint8_t *status) {
    uint8_t r[4]; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_LS_OPEN, (const uint8_t *)path, strlen(path) + 1, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return ESP_OK;
}

esp_err_t proto_ls_next(uint8_t *buf, size_t bufsz, uint16_t *outlen, int *final) {
    uint8_t rt = 0; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_LS_NEXT, NULL, 0, &rt, buf, bufsz, &rl);
    if (e) return e;
    if (outlen) *outlen = rl;
    if (final) *final = (rt & UP_TYPE_FINAL) ? 1 : 0;
    return ESP_OK;
}

esp_err_t proto_stat(const char *path, uint8_t *status, uint8_t *is_dir, uint32_t *size) {
    uint8_t r[6]; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_STAT, (const uint8_t *)path, strlen(path) + 1, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (rl < 6) return ESP_FAIL;
    if (status) *status = r[0];
    if (is_dir) *is_dir = r[1];
    if (size)   *size = rd_u32(r + 2);
    return ESP_OK;
}

esp_err_t proto_get_open(const char *path, uint8_t *status, uint32_t *total) {
    uint8_t r[5]; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_GET_OPEN, (const uint8_t *)path, strlen(path) + 1, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (rl < 5) return ESP_FAIL;
    if (status) *status = r[0];
    if (total)  *total = rd_u32(r + 1);
    return ESP_OK;
}

esp_err_t proto_get_data(uint32_t off, uint16_t want, uint8_t *status,
                         uint8_t *buf, uint16_t *got, int *final) {
    uint8_t req[6]; uint8_t r[UP_MAX_PAYLOAD]; uint16_t rl = 0; uint8_t rt = 0;
    if (want > UP_CHUNK) want = UP_CHUNK;
    wr_u32(req, off); req[4] = (uint8_t)(want & 0xff); req[5] = (uint8_t)(want >> 8);
    esp_err_t e = ESP_ERR_TIMEOUT;
    for (int a = 0; a < DATA_RETRIES; a++) {     // idempotent - a dropped chunk must not truncate the file
        e = txn(UP_OP_GET_DATA, req, 6, &rt, r, sizeof(r), &rl);
        if (e == ESP_OK && rl >= 3) break;
    }
    if (e) return e;
    if (rl < 3) return ESP_FAIL;
    uint16_t g = (uint16_t)(r[1] | (r[2] << 8));
    if (g > rl - 3) g = rl - 3;
    if (status) *status = r[0];
    if (buf && g) memcpy(buf, r + 3, g);
    if (got) *got = g;
    if (final) *final = (rt & UP_TYPE_FINAL) ? 1 : 0;
    return ESP_OK;
}

esp_err_t proto_put_open(const char *path, uint32_t total, uint8_t *status) {
    uint8_t req[4 + 256]; uint16_t pl = strlen(path) + 1;
    if (pl > 256) return ESP_ERR_INVALID_ARG;
    wr_u32(req, total);
    memcpy(req + 4, path, pl);
    uint8_t r[1]; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_PUT_OPEN, req, 4 + pl, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return ESP_OK;
}

esp_err_t proto_put_data(uint32_t off, const uint8_t *buf, uint16_t len,
                         uint8_t *status, uint16_t *credit) {
    uint8_t req[6 + UP_CHUNK]; uint8_t r[3]; uint16_t rl = 0;
    if (len > UP_CHUNK) len = UP_CHUNK;
    wr_u32(req, off); req[4] = (uint8_t)(len & 0xff); req[5] = (uint8_t)(len >> 8);
    memcpy(req + 6, buf, len);
    esp_err_t e = ESP_ERR_TIMEOUT;
    for (int a = 0; a < DATA_RETRIES; a++) {     // idempotent - re-send the same offset on a hiccup
        e = txn(UP_OP_PUT_DATA, req, 6 + len, NULL, r, sizeof(r), &rl);
        if (e == ESP_OK && rl >= 3) break;
    }
    if (e) return e;
    if (rl < 3) return ESP_FAIL;
    if (status) *status = r[0];
    if (credit) *credit = (uint16_t)(r[1] | (r[2] << 8));
    return ESP_OK;
}

esp_err_t proto_put_close(uint8_t *status) {
    uint8_t r[1]; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_PUT_CLOSE, NULL, 0, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return ESP_OK;
}

static esp_err_t simple_path_op(uint8_t op, const char *path, uint8_t *status) {
    uint8_t r[1]; uint16_t rl = 0;
    esp_err_t e = txn(op, (const uint8_t *)path, strlen(path) + 1, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return ESP_OK;
}

esp_err_t proto_rm(const char *path, uint8_t *status)    { return simple_path_op(UP_OP_RM, path, status); }
esp_err_t proto_mkdir(const char *path, uint8_t *status) { return simple_path_op(UP_OP_MKDIR, path, status); }

esp_err_t proto_mv(const char *from, const char *to, uint8_t *status) {
    uint8_t req[UP_MAX_PAYLOAD]; uint16_t fl = strlen(from) + 1, tl = strlen(to) + 1;
    if (fl + tl > UP_MAX_PAYLOAD) return ESP_ERR_INVALID_ARG;
    memcpy(req, from, fl); memcpy(req + fl, to, tl);
    uint8_t r[1]; uint16_t rl = 0;
    esp_err_t e = txn(UP_OP_MV, req, fl + tl, NULL, r, sizeof(r), &rl);
    if (e) return e;
    if (status) *status = rl ? r[0] : 0xFF;
    return ESP_OK;
}

esp_err_t proto_abort(void) {
    uint8_t r[1]; uint16_t rl = 0;
    return txn(UP_OP_ABORT, NULL, 0, NULL, r, sizeof(r), &rl);
}
