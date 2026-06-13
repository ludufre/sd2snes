// proto.h - UART link frame format + opcodes.
//
// MUST stay byte-identical to the MCU side (sd2snes-fixes/src/uart_proto.h).
//
// Frame (little-endian):
//   0   SOF    = 0x7E
//   1   TYPE   bit7=resp, bit6=final-chunk, bits5..0 reserved
//   2   OPCODE
//   3   SEQ    request id, echoed in the response
//   4-5 LEN    payload length (0..UP_MAX_PAYLOAD)
//   6.. PAYLOAD
//   +2  CRC16  over bytes [1 .. 6+LEN-1], poly 0x8005 reflected (CRC16/ARC), init 0
#pragma once

#define UP_SOF          0x7E
#define UP_PROTO_VER    1

#define UP_MAX_PAYLOAD  256
#define UP_CHUNK        250     // max file data bytes per GET/PUT frame
#define UP_LS_NAMEMAX   240

#define UP_TYPE_RESP    0x80
#define UP_TYPE_FINAL   0x40

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

// WiFi-in-menu bridge (0x10..0x1F). ESP=client / MCU=server: the ESP polls the
// MCU for a pending menu request and pushes status/scan back.
//   WIFI_POLL   req: -                resp: u8 enabled, u8 action(0=none,1=scan,2=connect,3=forget)
//                                           [+ ssid\0 pass\0 when action==connect]
//                                     (enabled = menu's EnableWifi; persistent, re-sent each poll)
//   WIFI_REPORT req: u8 connected, i8 rssi, ssid\0, ip\0   resp: u8 ok
//   WIFI_SCAN   req: u8 count, count*{i8 rssi, u8 enc, ssid\0}  resp: u8 ok
#define UP_OP_WIFI_POLL   0x10
#define UP_OP_WIFI_REPORT 0x11
#define UP_OP_WIFI_SCAN   0x12
#define UP_OP_ESP_INFO    0x13   // ESP->MCU: companion version string "x.y.z (CHIP)"

#define UP_WIFI_NONE      0
#define UP_WIFI_SCAN_REQ  1
#define UP_WIFI_CONNECT   2
#define UP_WIFI_FORGET    3

#define UP_WIFI_MAX_APS   8
#define UP_WIFI_SSID_MAX  32
#define UP_WIFI_PASS_MAX  63
