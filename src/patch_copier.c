/* sd2snes - SD card based universal cartridge for the SNES
   patch_copier.c: firmware side of BPS-via-FPGA-copier acceleration.

   Approach B drives the FPGA copier one op at a time (fpga_copier_op: one SPI
   trigger + busy-poll per copy) but uses the FPGA's SAFE 1-trigger queue (#3) to
   fire small ops WITHOUT polling busy -- the FPGA latches a trigger that arrives
   while it is still copying, so small ops stream at SPI speed with a periodic drain
   to keep the 1-deep queue from overrunning.

   Ordering (proven byte-identical by the host test): the source backup is done
   IMMEDIATELY (patch_copier_op_now) before run_actions, so SourceCopy reads the
   pristine source; SourceCopy/TargetCopy are replayed in order; TargetRead literals
   stay inline.

   (A descriptor list-mode "Approach B+" -- the copier fetching a whole PSRAM
   descriptor batch autonomously -- was tried and REMOVED (C side and dma.v): it
   hangs on real hardware because the autonomous fetch runs away on a mis-sampled
   word.  Do not reintroduce it; a burst-trigger over the existing queue is the
   safe way to batch, should it ever be needed.)
*/

#include "config.h"
#include "memory.h"
#include "fpga_spi.h"
#include "patch_copier.h"

/* #3: ops up to this size are fired WITHOUT polling busy (the FPGA's 1-trigger
   queue absorbs the next op fired while this one runs, and the FPGA keeps up
   because a small copy is faster than the SPI trigger).  Bigger ops are serialized
   (poll) so the 1-deep queue can't overrun.  The FPGA's overrun bit is the
   backstop, checked at finish. */
#define QUEUE_NOPOLL_MAX 128u

/* Drain (poll busy=0) every N no-poll ops so the 1-deep queue can't overflow: a
   sustained no-poll burst outpaces the copier (each op's copy is slower than the
   next trigger's SPI), so the queue must be drained.  N=2 keeps the queue at <=1. */
#define QUEUE_DRAIN_EVERY 2u
static uint8_t nopoll_cnt;

static uint32_t bps_op_count;           /* number of descriptors emitted */
static uint8_t  bps_op_err;             /* sticky failure -> bps_apply aborts */

void patch_copier_reset(void) {
    bps_op_count   = 0;
    bps_op_err     = 0;
    nopoll_cnt     = 0;
}

uint32_t patch_copier_count(void) {
    return bps_op_count;
}

/* Immediate single copier op (used for the source backup, which must run before
   run_actions so it reads the pristine source).  Returns 0 ok, 1 on failure. */
int patch_copier_op_now(uint32_t src, uint32_t dst, uint32_t len) {
    if (bps_op_err) return 1;
    if (fpga_copier_op(src, dst, len)) { bps_op_err = 1; return 1; }
    return 0;
}

int patch_copier_emit(uint32_t src, uint32_t dst, uint32_t len) {
    if (bps_op_err) return 1;
    /* #3 1-trigger queue: fire WITHOUT polling, but DRAIN every QUEUE_DRAIN_EVERY
       ops -- a sustained no-poll burst outpaces the copier (the FPGA copy is
       slower than the next trigger's SPI), so the 1-deep queue must be drained or
       it overruns.  Big ops are fully serialized. */
    /* & 0x03 covers BOTH the overrun bit (0x02) and a stuck busy bit (0x01):
       fpga_copier_wait() returns bit0=1 only when the copier never went idle
       before the timeout (wedged).  On a normal drain bit0=0, so & 0x03 == the
       overrun test in the good case, but also fails a wedge (which & 0x02 would
       silently pass -> a half-patched image could boot). */
    if (len > QUEUE_NOPOLL_MAX) {
        if (fpga_copier_wait() & 0x03) { bps_op_err = 1; return 1; }  /* drain */
        fpga_copier_op_nopoll(src, dst, len);
        if (fpga_copier_wait() & 0x03) { bps_op_err = 1; return 1; }  /* wait big op (wedge or overrun) */
        nopoll_cnt = 0;
    } else {
        fpga_copier_op_nopoll(src, dst, len);     /* fire and move on -- no poll */
        if (++nopoll_cnt >= QUEUE_DRAIN_EVERY) {
            if (fpga_copier_wait() & 0x03) { bps_op_err = 1; return 1; }  /* wedge or overrun */
            nopoll_cnt = 0;
        }
    }
    bps_op_count++;
    return 0;
}

/* Wait for the last queued op to drain, then check the FPGA overrun bit -- if set,
   the queue dropped an op (the MCU outran it) and the image is incomplete -> fail
   (the CRC verify would also catch it).  Returns 0 ok, 1 on failure. */
int patch_copier_finish(void) {
    if (bps_op_err) return 1;
    uint8_t st = fpga_copier_wait();
    /* & 0x03: fail on overrun (0x02) OR a stuck busy bit (0x01 = copier wedged,
       never went idle before the timeout) -- either means the image is incomplete. */
    if (st & 0x03) { bps_op_err = 1; return 1; }
    return 0;
}

/* Probe whether the currently-loaded FPGA core carries the MCU copier: write a
   marker to a scratch slot, copier-copy it to another, read it back.  A core
   WITHOUT the copier leaves the destination untouched (or the op times out).
   Must be called with the SNES held in reset.  Returns 1 if the copier works. */
int patch_copier_available(void) {
    uint8_t marker[4] = { 0xC0, 0x91, 0xE7, 0x5A };
    uint8_t got[4]    = { 0, 0, 0, 0 };
    uint8_t save_a[4], save_b[4];
    uint32_t a = SRAM_SCRATCHPAD;          /* 0xFFFF00 */
    uint32_t b = SRAM_SCRATCHPAD + 0x40;
    int ok;
    /* $FFFF00 is NOT free scratch: its first 4 bytes are the 0x12345678 sentinel
       (written at menu-load, main.c) that sram_reliable() polls to gate the in-game
       autosave.  Clobbering it here (every patched game-load) would leave a marker
       there for the whole session -> sram_reliable()==0 -> autosave silently off
       (save loss) + a 256-line log spew.  So save both scratch words and restore
       them unconditionally -- leave memory as we found it. */
    sram_readblock(save_a, a, 4);
    sram_readblock(save_b, b, 4);
    sram_writeblock(marker, a, 4);
    sram_writeblock(got, b, 4);            /* zero the destination first */
    /* Use the SHORT-timeout probe: a core without the copier does not know the
       0xd5 (DMA_BUSY) command and can report a phantom busy bit, so the normal
       60M-iteration timeout would stall for seconds.  The probe fails fast. */
    if (fpga_copier_op_probe(a, b, 4)) {   /* timeout -> no copier */
        ok = 0;
    } else {
        sram_readblock(got, b, 4);
        ok = (got[0] == marker[0] && got[1] == marker[1]
           && got[2] == marker[2] && got[3] == marker[3]) ? 1 : 0;
    }
    sram_writeblock(save_a, a, 4);         /* restore: sentinel + neighbour */
    sram_writeblock(save_b, b, 4);
    return ok;
}
