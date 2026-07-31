/* sd2snes fork -- in-game TAB menu (igmenu.bin) loader. See igmenu.h. */

#include "config.h"
#include "fileops.h"
#include "uart.h"
#include "memory.h"
#include "crc16.h"
#include "igmenu.h"

#include <string.h>

/* CRC16 convention (must match the build step in snes/Makefile): running crc16_update
   over the body bytes, init 0xFFFF, final XOR 0xFFFF. crc16_update is the standard
   reflected-0xA001 table step (src/crc16.c). */

void igmenu_stage(void) {
  /* Read buffer lives in AHB SRAM (NOT the tight main .bss -- see the .bss/AHB gotcha):
     it is written by f_read before it is read, so .ahbram's NOLOAD/no-zero-init is fine. */
  static uint8_t buf[512] IN_AHBRAM;
  uint32_t off = 0;
  uint16_t n;
  uint16_t crc = 0xFFFF;
  uint8_t  magic0 = 0, magic1 = 0, magic2 = 0, magic3 = 0;
  uint8_t  ver = 0xFF;
  uint16_t hdr_crc = 0xFFFF;
  uint8_t  got_header = 0;
  uint8_t  ok = 0;

  /* Invalidate the header + gate FIRST: a missing/short/corrupt bin must leave no
     stale magic behind (the $C0 hook then falls through to the single-tab fail-safe). */
  sram_memset(SRAM_DIR_ADDR, 8, 0);          /* $C20000..$C20007 */
  sram_writebyte(0, SRAM_IGMENU_GATE_ADDR);

  file_open((const uint8_t *)IGMENU_FILENAME, FA_READ);
  if (file_res) { file_res = 0; return; }    /* absent -> gate stays 0 (soft fail) */

  for (;;) {
    n = file_readblock(buf, off, sizeof(buf));
    if (file_res) { file_close(); file_res = 0; return; }
    if (n == 0) break;
    if (!got_header) {
      if (n < 12) { file_close(); file_res = 0; return; }   /* too small for a header */
      magic0 = buf[0]; magic1 = buf[1]; magic2 = buf[2]; magic3 = buf[3];
      ver = buf[4];
      hdr_crc = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
      got_header = 1;
    }
    sram_writeblock(buf, SRAM_DIR_ADDR + off, n);
    for (uint16_t i = 0; i < n; i++) {
      if (off + i >= 8) crc = crc16_update(crc, buf[i]);
    }
    off += n;
    if (off > IGMENU_MAX_BYTES) { file_close(); file_res = 0; return; }  /* bound */
    if (n < sizeof(buf)) break;              /* short read = EOF */
  }
  file_close();
  file_res = 0;

  crc ^= 0xFFFF;

  ok = got_header
       && magic0 == 'I' && magic1 == 'G' && magic2 == 'M' && magic3 == 'N'
       && ver == IGMENU_ABI_VERSION
       && crc == hdr_crc;

  if (ok) {
    sram_writebyte(1, SRAM_IGMENU_GATE_ADDR);
    printf("igmenu.bin OK (%lu bytes, ver %d)\n", (unsigned long)off, ver);
  } else {
    /* Wipe the streamed header so BOTH the gate (0) and the overlay's own version
       re-check ($C20004) fail -> guaranteed fall-through to the fail-safe. */
    sram_memset(SRAM_DIR_ADDR, 8, 0);
    printf("igmenu.bin rejected (hdr=%d ver=%d crc=%04x/%04x)\n",
           got_header, ver, crc, hdr_crc);
  }
}
