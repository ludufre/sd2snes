/* BS-X over-the-air program download — MCU bridge.  See bsx_dl.h for the protocol.
 *
 * SAFETY: every path here is bounded and fail-safe (mirrors load_cover / gameinfo).
 * It runs inside the in-game command loop, which also services USB-serial, so a hang
 * would wedge USB and deafen the firmware to reset commands.  The only loops are the
 * fixed-size sram_readblock/writeblock; the FPGA SPI helpers are single transactions.
 * No allocation, no unbounded search, no waiting on the SNES. */

#include "config.h"      /* defines CONFIG_MCU_H, required by memory.h */
#include "bsx_dl.h"
#include "memory.h"
#include "fpga_spi.h"

void bsx_dl_service(void) {
  static uint8_t last_seq = 0;
  static uint8_t active = 0;
  uint8_t hdr[13];

  /* read the descriptor mailbox (fixed 13 bytes — bounded) */
  sram_readblock(hdr, BS_DL_MBOX_ADDR, 13);

  if (hdr[0] != 'B' || hdr[1] != 'X' || hdr[2] != 'D' || hdr[3] != 'L') {
    active = 0;                 /* no active download -> stay inert */
    return;
  }

  if (!active) {
    /* first sight of a download this session -> (re)initialise the status mailbox
       and force the current descriptor to apply (seq != last_seq). */
    active = 1;
    last_seq = (uint8_t)(hdr[4] - 1);
    uint8_t st[6] = { 'B', 'X', 'D', 'S', 0, 0 };
    sram_writeblock(st, BS_DL_STATUS_ADDR, 6);
  }

  uint8_t seq = hdr[4];
  if (seq != last_seq) {
    last_seq = seq;
    uint8_t  ctl    = hdr[5];
    uint16_t chan   = (uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8);
    uint32_t base   = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8)
                                       | ((uint32_t)hdr[10] << 16);
    uint16_t frames = (uint16_t)hdr[11] | ((uint16_t)hdr[12] << 8);
    fpga_bs_dl_set(ctl, chan, base, frames);          /* opcode 0xf7 */
    sram_writebyte(seq, BS_DL_STATUS_ADDR + 5);        /* ack the applied descriptor */
    /* DBG: echo the frames the firmware actually applied (host reads +6/+7) */
    sram_writebyte((uint8_t)(frames & 0xff), BS_DL_STATUS_ADDR + 6);
    sram_writebyte((uint8_t)((frames >> 8) & 0xff), BS_DL_STATUS_ADDR + 7);
  }

  /* publish the FPGA drain notify + DEBUG live state so the host can read it */
  uint8_t dbg[6];
  fpga_bs_dl_dbg(dbg);                                       /* opcode 0xf8 (multi-byte) */
  sram_writebyte(dbg[0] & 0x03, BS_DL_STATUS_ADDR + 4);     /* dl_seq */
  sram_writeblock(dbg + 1, BS_DL_STATUS_ADDR + 8, 5);       /* DEBUG: dl_queue(2)+staged_frames(2)+flags(1) */
}
