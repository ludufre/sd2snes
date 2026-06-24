#ifndef BSX_DL_H
#define BSX_DL_H

#include <stdint.h>

/* BS-X over-the-air program download — MCU <-> host (USB "satellite") bridge.
 *
 * The FPGA receiver (verilog/sd2snes_base/bsx.v) serves the Town's $218A/$218B/$218C
 * queue/prefix/data stream from a ring in PSRAM.  The MCU is a thin, BOUNDED bridge:
 * it relays a descriptor from a PSRAM mailbox to the FPGA (opcode 0xf7) and publishes
 * the FPGA drain notify (opcode 0xf8) back to the host.  All the heavy lifting (the
 * stream FSM, the 22-byte framing) is in hardware; the program bytes are DMA'd into
 * the ring by the host over USB (PUT space=SNES) and read by the FPGA directly.
 *
 * Memory map (all inside the 1MB BS_PACK region 0x900000-0x9FFFFF, see memory.h):
 *   0x900000-0x97FFFF  broadcast page window (bs_page, the catalog: Dir/Town/ChMap)
 *   0x980000-0x99FFFF  the download RING (bsx.v reads 0x980000 + bs_dl_offset)  [128KB]
 *   0x9A0000           descriptor mailbox  (host -> MCU)   13 bytes
 *   0x9A0010           status mailbox      (MCU -> host)    6 bytes
 *   0x9A0020-0x9FFFFF  free
 *
 * Descriptor (0x9A0000): 'B' 'X' 'D' 'L' | seq | ctl | chan_lo chan_hi |
 *                        base_lo base_mid base_hi | frames_lo frames_hi
 *   seq    - host bumps it on every new descriptor; the MCU applies on change.
 *   ctl    - BS_DL_CTL_ARM | BS_DL_CTL_STAGE (apply when seq changes).
 *   chan   - the program's data-channel LCI (the page the Town tunes to download).
 *   base   - ring offset (0..0x1FFFF) of the staged fragment.
 *   frames - 22-byte frames in the fragment (a 32KB SatellaWave data group = 1490).
 *
 * Status (0x9A0010): 'B' 'X' 'D' 'S' | dl_seq | ack
 *   dl_seq - the FPGA's drain notify (bumps when the Town fully consumed a fragment).
 *   ack    - echo of the last descriptor seq the MCU applied.
 *
 * Lifecycle: the host MUST wipe the "BXDL" magic (4 zero bytes at 0x9A0000) at session
 * START (pausing >50ms so this bridge sees the magic absent and resets its active/last_seq
 * state) AND at session END, so a descriptor left by a crashed/interrupted run cannot
 * re-arm the FPGA receiver on the next game.  utils/bsx_download.py does this.  The
 * channel-match guard in bsx.v (bs_dl_armed needs bs_page0 == bs_dl_chan) keeps a stale
 * arm dormant in the meantime, so this is defense-in-depth, not a correctness crutch.
 */

#define BS_DL_RING_ADDR    0x980000UL
#define BS_DL_RING_SIZE    0x020000UL   /* 128KB */
#define BS_DL_MBOX_ADDR    0x9A0000UL   /* descriptor: host -> MCU */
#define BS_DL_STATUS_ADDR  0x9A0010UL   /* status:     MCU -> host */

#define BS_DL_CTL_ARM      0x01
#define BS_DL_CTL_STAGE    0x02

/* Poll the descriptor mailbox, bridge a new descriptor to the FPGA, and publish the
 * drain notify.  BOUNDED + fail-safe: a handful of SPI/PSRAM transactions; returns
 * immediately when no "BXDL" magic is present.  Call once per game-loop tick while a
 * BS-X base cart runs (gate: romprops.mapper_id == 3).  NEVER hangs the MCU. */
void bsx_dl_service(void);

#endif
