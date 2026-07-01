/*

   uart_proto.h: compact framed protocol + non-blocking SD file server over the
   ESP32 link.  Arch-neutral - uses the esplink.h byte transport (UART0 on LPC,
   USART6 on STM32).

   The MCU is a pure SERVER: it only ever answers requests sent by the ESP and
   never blocks the SNES.  uart_proto_poll() advances at most one bounded step
   per call (parse one frame / one f_read / one f_write / one readdir batch),
   modelled on the cover state machine (cover.c).  All timeouts/retries live on
   the ESP side.

   === Frame (little-endian) ===
     off size field
     0   1    SOF    = 0x7E
     1   1    TYPE   bit7=resp, bit6=final-chunk, bits5..0 reserved
     2   1    OPCODE
     3   1    SEQ    request id, echoed in the response
     4   2    LEN    payload length (0..UP_MAX_PAYLOAD)
     6   N    PAYLOAD
     6+N 2    CRC16  over bytes [1 .. 6+N-1] (TYPE..end of payload), poly 0x8005 reflected

   The MCU/ESP headers MUST stay byte-identical (see ESP src/proto.h).

*/

#ifndef UART_PROTO_H
#define UART_PROTO_H

#include <stdint.h>

#define UP_SOF          0x7E
#define UP_PROTO_VER    1

#define UP_MAX_PAYLOAD  256     /* max payload bytes per frame */
#define UP_CHUNK        250     /* max file data bytes per GET/PUT frame
                                   (GET resp = 3+250=253; PUT req = 6+250=256) */
#define UP_LS_NAMEMAX   240     /* directory names truncated to this in LS */

/* TYPE bits */
#define UP_TYPE_RESP    0x80
#define UP_TYPE_FINAL   0x40

/* Opcodes - 0x00..0x0F = v1 (link + file ops). 0x10+ reserved (see plan). */
#define UP_OP_PING      0x00
#define UP_OP_LS_OPEN   0x01
#define UP_OP_LS_NEXT   0x02
#define UP_OP_STAT      0x03
#define UP_OP_GET_OPEN  0x04
#define UP_OP_GET_DATA  0x05
#define UP_OP_PUT_OPEN  0x06
#define UP_OP_PUT_DATA  0x07
#define UP_OP_PUT_CLOSE 0x08
#define UP_OP_RM        0x09
#define UP_OP_MV        0x0A
#define UP_OP_MKDIR     0x0B
#define UP_OP_ABORT     0x0C

/* WiFi-in-menu bridge (0x10..0x1F). Direction is still ESP=client / MCU=server:
   the ESP polls the MCU for a pending menu request and pushes status/scan back.
     WIFI_POLL  req: -                resp: u8 enabled, u8 action(0=none,1=scan,2=connect,3=forget)
                                            [+ ssid\0 pass\0 when action==connect]
                                      (enabled = menu's EnableWifi; persistent, re-sent each poll)
     WIFI_REPORT req: u8 connected, i8 rssi, ssid\0, ip\0   resp: u8 ok
     WIFI_SCAN  req: u8 count, count*{i8 rssi, u8 enc, ssid\0}  resp: u8 ok */
#define UP_OP_WIFI_POLL   0x10
#define UP_OP_WIFI_REPORT 0x11
#define UP_OP_WIFI_SCAN   0x12
#define UP_OP_ESP_INFO    0x13   /* ESP->MCU: companion version string "x.y.z (CHIP)" */

/* Menu hot-reload (0x40 range): the WebUI wrote a new config.yml / theme / list and
   asks the menu to re-read it WITHOUT a power-cycle. The handler only sets a flag
   (O(1), no I/O); main.c consumes it and triggers the existing cold menu reload. */
#define UP_OP_HOT_RELOAD  0x40

/* menu->ESP actions carried by WIFI_POLL */
#define UP_WIFI_NONE      0
#define UP_WIFI_SCAN_REQ  1
#define UP_WIFI_CONNECT   2
#define UP_WIFI_FORGET    3

#define UP_WIFI_MAX_APS   8      /* scan APs surfaced to the menu (fits one frame) */
#define UP_WIFI_SSID_MAX  32
#define UP_WIFI_PASS_MAX  63

/* status byte: 0 = OK, otherwise the FatFs FRESULT value. */

void uart_proto_init(void);     /* call once after uart0_link_init() + file_init() */
void uart_proto_poll(void);     /* call every main-loop iteration (like usbint_handler) */
int  uart_proto_busy(void);     /* 1 while a file/dir handle is open or a reply is pending */
int  uart_proto_active(void);   /* 1 if a frame arrived recently (~100ms) -> a transfer is flowing */

/* ---- WiFi-in-menu bridge (talks to the ESP via WIFI_* opcodes) ---- */
typedef struct {
  uint8_t connected;       /* 1 = STA has an IP */
  int8_t  rssi;            /* current STA RSSI (dBm) */
  char    ssid[UP_WIFI_SSID_MAX + 1];
  char    ip[16];
  uint8_t scan_count;      /* APs in aps[] (0..UP_WIFI_MAX_APS) */
  uint8_t scan_seq;        /* increments on each fresh scan result */
  struct { int8_t rssi; uint8_t enc; char ssid[UP_WIFI_SSID_MAX + 1]; } aps[UP_WIFI_MAX_APS];
} uart_wifi_state_t;

const uart_wifi_state_t *uart_wifi_state(void);   /* status + last scan (read by main.c) */
void uart_wifi_request_scan(void);                /* queue a scan for the ESP to run */
void uart_wifi_request_connect(const char *ssid, const char *pass);
void uart_wifi_request_forget(void);

/* ---- companion (ESP) identity, reported via UP_OP_ESP_INFO ---- */
int  uart_esp_present(void);        /* 1 if the ESP reported in recently */
const char *uart_esp_string(void); /* "x.y.z (CHIP)" or "" if never seen */

/* ---- menu hot-reload request (set by UP_OP_HOT_RELOAD, consumed by main.c) ---- */
int  uart_take_menu_reload(void);  /* returns 1 once if a hot-reload was requested, then clears */

#endif
