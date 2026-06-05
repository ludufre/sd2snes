/*

   uart_proto.c: framed protocol + non-blocking SD file server over UART0.

   See uart_proto.h.  Bounded, fail-safe, never blocks the SNES - the contract
   of cover.c.  Uses its OWN FatFs FIL/DIR handles (never the menu's
   file_handle/file_buf), which is safe with _FS_LOCK=0/_FS_REENTRANT=0 because
   the MCU is single-threaded cooperative (one bounded step per poll, no
   reentrancy) and each FIL keeps its own sector buffer (_FS_TINY=0) - exactly
   how usbinterface.c already runs a private FIL alongside the menu.

*/

#include <string.h>

#include "config.h"
#include "fileops.h"     /* pulls in ff.h (FIL/DIR/FILINFO/FRESULT/f_*) */
#include "crc16.h"
#include "timer.h"       /* getticks()/tick_t for the activity tracker */
#include "lang.h"        /* lang_idx() - configured menu language for the WebUI */
#include "esplink.h"
#include "uart_proto.h"

#define UP_ACTIVE_TICKS 10   /* ~100ms @ HZ=100: "a transfer is flowing" window */

/* Big buffers: AHB SRAM on LPC (tiny 16KB main SRAM), plain .bss on STM32
   (~64KB RAM, and its linker has no .ahbram). */
#ifdef CONFIG_MK3_STM32
#  define ESPLINK_BUF
#else
#  define ESPLINK_BUF IN_AHBRAM
#endif

/* Large buffers live in AHB SRAM (IN_AHBRAM, see config.h) so they don't eat
   the tiny 16KB main SRAM / stack.  AHB is NOT zeroed at startup, so nothing
   here may rely on zero-init: every buffer is filled before use, and all state
   scalars (in .bss, zeroed) are reset in uart_proto_init(). */

/* ---- frame parser state ---- */
enum { PS_SOF = 0, PS_HDR, PS_PL, PS_CRC };
static uint8_t  ps;
static uint8_t  hdr[5];        /* TYPE, OPCODE, SEQ, LEN_lo, LEN_hi */
static uint8_t  hdr_n;
static uint16_t plen;
static ESPLINK_BUF uint8_t pl[UP_MAX_PAYLOAD + 1];  /* +1: always NUL-terminate for string ops */
static uint16_t pl_n;
static uint8_t  crcb[2];
static uint8_t  crc_n;

/* ---- response assembly ---- */
static ESPLINK_BUF uint8_t txframe[6 + UP_MAX_PAYLOAD + 2];
static uint16_t out_len;
static uint8_t  out_ready;
static tick_t   up_last_active;   /* getticks() of the last complete frame */
static ESPLINK_BUF uint8_t resp[UP_MAX_PAYLOAD];    /* scratch for big response payloads */

/* ---- file server context (private handles) ---- */
static ESPLINK_BUF FIL      up_fh;
static ESPLINK_BUF DIR      up_dir;
static uint8_t  up_fh_mode;    /* 0 none, 1 read (GET), 2 write (PUT) */
static uint8_t  up_dir_open;
static ESPLINK_BUF FILINFO  up_fno;
static ESPLINK_BUF char     up_lfn[256];   /* LFN buffer (>= _MAX_LFN+1) */
static ESPLINK_BUF uint8_t  ls_carry[1 + 4 + UP_LS_NAMEMAX + 1];
static uint16_t ls_carry_len;

void uart_proto_init(void) {
  ps = PS_SOF;
  out_ready = 0;
  up_fh_mode = 0;
  up_dir_open = 0;
  ls_carry_len = 0;
  up_last_active = 0;
}

/* assemble a frame into txframe[]; the poll loop flushes it to UART0 */
static void build_frame(uint8_t type, uint8_t op, uint8_t seq,
                        const uint8_t *p, uint16_t len) {
  uint16_t i, crc = 0;
  txframe[0] = UP_SOF;
  txframe[1] = type;
  txframe[2] = op;
  txframe[3] = seq;
  txframe[4] = (uint8_t)(len & 0xff);
  txframe[5] = (uint8_t)(len >> 8);
  if (len && p) memcpy(txframe + 6, p, len);
  for (i = 1; i < (uint16_t)(6 + len); i++) crc = crc16_update(crc, txframe[i]);
  txframe[6 + len]     = (uint8_t)(crc & 0xff);
  txframe[6 + len + 1] = (uint8_t)(crc >> 8);
  out_len = 6 + len + 2;
  out_ready = 1;
}

static void reply_status(uint8_t op, uint8_t seq, uint8_t status) {
  build_frame(UP_TYPE_RESP, op, seq, &status, 1);
}

static void proto_close_file(void) {
  if (up_fh_mode) { f_close(&up_fh); up_fh_mode = 0; }
}
static void proto_close_dir(void) {
  if (up_dir_open) { f_closedir(&up_dir); up_dir_open = 0; }
  ls_carry_len = 0;
}

static uint32_t rd_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* format the current up_fno into out[]; returns bytes written */
static int ls_format(uint8_t *out) {
  const char *name = up_lfn[0] ? up_lfn : up_fno.fname;
  int nl = (int)strlen(name);
  int o = 0;
  if (nl > UP_LS_NAMEMAX) nl = UP_LS_NAMEMAX;
  out[o++] = (up_fno.fattrib & AM_DIR) ? 0 : 1;
  wr_u32(out + o, (uint32_t)up_fno.fsize); o += 4;
  memcpy(out + o, name, nl); o += nl;
  out[o++] = 0;
  return o;
}

static void handle_frame(void) {
  uint8_t op  = hdr[1];
  uint8_t seq = hdr[2];

  if (hdr[0] & UP_TYPE_RESP) return;   /* ignore stray responses */

  pl[plen] = 0;                        /* NUL-terminate for string payloads */

  switch (op) {

  case UP_OP_PING: {
    int i = 0;
    const char *nm = DEVICE_NAME;
    resp[0] = UP_PROTO_VER;
    resp[1] = lang_idx();   /* configured menu language: 0=EN, 1=PT-BR, 2=ES */
    while (nm[i] && i < (int)sizeof(resp) - 3) { resp[2 + i] = (uint8_t)nm[i]; i++; }
    resp[2 + i] = 0;
    build_frame(UP_TYPE_RESP, op, seq, resp, (uint16_t)(2 + i + 1));
    break;
  }

  case UP_OP_LS_OPEN: {
    proto_close_dir();
    FRESULT r = f_opendir(&up_dir, (TCHAR *)pl);
    if (r == FR_OK) up_dir_open = 1;
    reply_status(op, seq, (uint8_t)r);
    break;
  }

  case UP_OP_LS_NEXT: {
    uint16_t n = 0;
    uint8_t final = 0;
    if (ls_carry_len) { memcpy(resp, ls_carry, ls_carry_len); n = ls_carry_len; ls_carry_len = 0; }
    while (up_dir_open) {
      up_fno.lfname = up_lfn;
      up_fno.lfsize = sizeof(up_lfn);
      if (f_readdir(&up_dir, &up_fno) != FR_OK || up_fno.fname[0] == 0) {
        proto_close_dir();
        final = 1;
        break;
      }
      int el = ls_format(ls_carry);
      if (n + el + 1 <= UP_MAX_PAYLOAD) { memcpy(resp + n, ls_carry, el); n += el; }
      else { ls_carry_len = (uint16_t)el; break; }   /* carry to next frame */
    }
    if (final) resp[n++] = 0xFF;
    build_frame(UP_TYPE_RESP | (final ? UP_TYPE_FINAL : 0), op, seq, resp, n);
    break;
  }

  case UP_OP_STAT: {
    FRESULT r = f_stat((TCHAR *)pl, &up_fno);
    resp[0] = (uint8_t)r;
    resp[1] = (r == FR_OK && (up_fno.fattrib & AM_DIR)) ? 1 : 0;
    wr_u32(resp + 2, (r == FR_OK) ? (uint32_t)up_fno.fsize : 0);
    build_frame(UP_TYPE_RESP, op, seq, resp, 6);
    break;
  }

  case UP_OP_GET_OPEN: {
    proto_close_file();
    FRESULT r = f_open(&up_fh, (TCHAR *)pl, FA_READ);
    if (r == FR_OK) up_fh_mode = 1;
    resp[0] = (uint8_t)r;
    wr_u32(resp + 1, (r == FR_OK) ? (uint32_t)f_size(&up_fh) : 0);
    build_frame(UP_TYPE_RESP, op, seq, resp, 5);
    break;
  }

  case UP_OP_GET_DATA: {
    uint32_t off  = rd_u32(pl);
    uint16_t want = (uint16_t)(pl[4] | (pl[5] << 8));
    if (want > UP_CHUNK) want = UP_CHUNK;
    if (up_fh_mode != 1) {
      resp[0] = FR_INVALID_OBJECT; resp[1] = 0; resp[2] = 0;
      build_frame(UP_TYPE_RESP | UP_TYPE_FINAL, op, seq, resp, 3);
      break;
    }
    if (f_tell(&up_fh) != off) f_lseek(&up_fh, off);
    UINT got = 0;
    FRESULT r = f_read(&up_fh, resp + 3, want, &got);
    resp[0] = (uint8_t)r;
    resp[1] = (uint8_t)(got & 0xff);
    resp[2] = (uint8_t)(got >> 8);
    uint8_t final = (r != FR_OK || got < want || f_eof(&up_fh)) ? UP_TYPE_FINAL : 0;
    build_frame(UP_TYPE_RESP | final, op, seq, resp, (uint16_t)(3 + got));
    break;
  }

  case UP_OP_PUT_OPEN: {
    proto_close_file();
    /* payload: u32 total (advisory, unused) + path */
    FRESULT r = f_open(&up_fh, (TCHAR *)(pl + 4), FA_WRITE | FA_CREATE_ALWAYS);
    if (r == FR_OK) up_fh_mode = 2;
    reply_status(op, seq, (uint8_t)r);
    break;
  }

  case UP_OP_PUT_DATA: {
    uint32_t off = rd_u32(pl);
    uint16_t len = (uint16_t)(pl[4] | (pl[5] << 8));
    uint8_t out3[3];
    if (up_fh_mode != 2) {
      out3[0] = FR_INVALID_OBJECT;
    } else {
      if (f_tell(&up_fh) != off) f_lseek(&up_fh, off);
      UINT wr = 0;
      FRESULT r = f_write(&up_fh, pl + 6, len, &wr);
      out3[0] = (uint8_t)r;
    }
    out3[1] = (uint8_t)(UP_CHUNK & 0xff);   /* credit: one chunk at a time */
    out3[2] = (uint8_t)(UP_CHUNK >> 8);
    build_frame(UP_TYPE_RESP, op, seq, out3, 3);
    break;
  }

  case UP_OP_PUT_CLOSE: {
    FRESULT r = FR_OK;
    if (up_fh_mode == 2) { r = f_close(&up_fh); up_fh_mode = 0; }
    reply_status(op, seq, (uint8_t)r);
    break;
  }

  case UP_OP_RM:
    reply_status(op, seq, (uint8_t)f_unlink((TCHAR *)pl));
    break;

  case UP_OP_MV: {
    /* payload: from\0 to\0 */
    const char *from = (const char *)pl;
    const char *to   = from + strlen(from) + 1;
    reply_status(op, seq, (uint8_t)f_rename((TCHAR *)from, (TCHAR *)to));
    break;
  }

  case UP_OP_MKDIR:
    reply_status(op, seq, (uint8_t)f_mkdir((TCHAR *)pl));
    break;

  case UP_OP_ABORT:
    proto_close_file();
    proto_close_dir();
    reply_status(op, seq, 0);
    break;

  default:
    reply_status(op, seq, 0xFF);   /* unknown opcode */
    break;
  }
}

static int parse_byte(uint8_t c) {
  switch (ps) {
  case PS_SOF:
    if (c == UP_SOF) { ps = PS_HDR; hdr_n = 0; }
    break;
  case PS_HDR:
    hdr[hdr_n++] = c;
    if (hdr_n == 5) {
      plen = (uint16_t)(hdr[3] | (hdr[4] << 8));
      if (plen > UP_MAX_PAYLOAD) { ps = PS_SOF; break; }  /* bad length: resync */
      pl_n = 0; crc_n = 0;
      ps = plen ? PS_PL : PS_CRC;
    }
    break;
  case PS_PL:
    pl[pl_n++] = c;
    if (pl_n == plen) { ps = PS_CRC; crc_n = 0; }
    break;
  case PS_CRC:
    crcb[crc_n++] = c;
    if (crc_n == 2) {
      uint16_t want = (uint16_t)(crcb[0] | (crcb[1] << 8));
      uint16_t crc = 0, i;
      for (i = 0; i < 5; i++) crc = crc16_update(crc, hdr[i]);
      for (i = 0; i < plen; i++) crc = crc16_update(crc, pl[i]);
      ps = PS_SOF;
      if (crc == want) return 1;   /* a complete, valid frame is ready */
    }
    break;
  }
  return 0;
}

void uart_proto_poll(void) {
  /* 1. flush a pending response first (built once; only the send is retried) */
  if (out_ready) {
    if (esplink_tx_space() < (int)out_len) return;
    esplink_write(txframe, (int)out_len);
    out_ready = 0;
  }

  /* 2. consume RX; handle at most one complete frame per poll (one bounded step) */
  int c;
  while ((c = esplink_getbyte()) >= 0) {
    if (parse_byte((uint8_t)c)) {
      up_last_active = getticks();            /* mark the link as actively transferring */
      handle_frame();                         /* executes the op, builds the reply */
      if (out_ready && esplink_tx_space() >= (int)out_len) {
        esplink_write(txframe, (int)out_len);
        out_ready = 0;
      }
      return;
    }
  }
}

int uart_proto_busy(void) {
  return (up_fh_mode || up_dir_open || out_ready);
}

int uart_proto_active(void) {
  /* a frame completed within the last ~100ms -> a transfer is actively flowing,
     so the caller should service the link continuously instead of sleeping. */
  return (tick_t)(getticks() - up_last_active) < UP_ACTIVE_TICKS;
}
