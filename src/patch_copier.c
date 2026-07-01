/* sd2snes - SD card based universal cartridge for the SNES
   patch_copier.c: firmware side of BPS-via-FPGA-copier acceleration.

   Approach B drove the FPGA copier one op at a time (fpga_copier_op: one SPI
   trigger + busy-poll per copy).  For a big chip-converting BPS that is tens of
   thousands of tiny relocations, the per-op SPI round-trip dominates.  Approach B+
   batches them: SourceCopy/TargetCopy are accumulated as 10-byte descriptors into a
   PSRAM list (written in big bursts), then the copier reads + runs the WHOLE list
   autonomously (fpga_copier_list / CMD 0xd6) with a single busy-poll.

   Ordering (proven byte-identical by the host test): the source backup is done
   IMMEDIATELY (patch_copier_op_now) before run_actions, so SourceCopy reads the
   pristine source; SourceCopy/TargetCopy are DEFERRED into the list and replayed in
   order at finish; TargetRead literals stay inline.  A deferred TargetCopy reads
   already-finalized output because earlier list ops are applied first, in order.

   See CLAUDE.md (subsistema BPS-copier) for the data flow + core list.
*/

#include "config.h"
#include "memory.h"
#include "fpga_spi.h"
#include "patch_copier.h"

/* List mode (Approach B+) hangs the copier on real hardware (the descriptor-fetch
   path -- single-op copy works), pending a hardware-accurate testbench + FPGA state
   diagnostic.  ABANDONED (hangs on real hardware -- the autonomous descriptor fetch
   runs away on a mis-sampled word in the tight SA-1 core).  Kept at 0; the per-op
   path below now uses the SAFE 1-trigger queue (#3) instead. */
#define USE_LIST_MODE 0

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

/* Descriptor accumulation buffer (.bss, NOT stack): flushed to the PSRAM list in
   bursts so the per-descriptor cost is a memcpy, not an SPI round-trip. */
#define DESC_BUF_BYTES 2000u            /* 200 descriptors per burst */
static uint8_t  desc_buf[DESC_BUF_BYTES];
static uint16_t desc_buf_pos;
static uint32_t list_ptr;               /* next PSRAM write address for a flush */
static uint32_t list_base_addr;         /* start of the list (passed to 0xd6) */
static uint32_t bps_op_count;           /* number of descriptors emitted */
static uint8_t  bps_op_err;             /* sticky failure -> bps_apply aborts */

void patch_copier_reset(uint32_t list_base) {
    desc_buf_pos   = 0;
    list_ptr       = list_base;
    list_base_addr = list_base;
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

static void desc_flush(void) {
    if (desc_buf_pos) {
        sram_writeblock(desc_buf, list_ptr, desc_buf_pos);
        list_ptr += desc_buf_pos;
        desc_buf_pos = 0;
    }
}

int patch_copier_emit(uint32_t src, uint32_t dst, uint32_t len) {
    if (bps_op_err) return 1;
    if (!USE_LIST_MODE) {
        /* #3 1-trigger queue: fire WITHOUT polling, but DRAIN every QUEUE_DRAIN_EVERY
           ops -- a sustained no-poll burst outpaces the copier (the FPGA copy is
           slower than the next trigger's SPI), so the 1-deep queue must be drained or
           it overruns.  Big ops are fully serialized. */
        if (len > QUEUE_NOPOLL_MAX) {
            if (fpga_copier_wait() & 0x02) { bps_op_err = 1; return 1; }  /* drain */
            fpga_copier_op_nopoll(src, dst, len);
            if (fpga_copier_wait() & 0x02) { bps_op_err = 1; return 1; }  /* wait big op */
            nopoll_cnt = 0;
        } else {
            fpga_copier_op_nopoll(src, dst, len);     /* fire and move on -- no poll */
            if (++nopoll_cnt >= QUEUE_DRAIN_EVERY) {
                if (fpga_copier_wait() & 0x02) { bps_op_err = 1; return 1; }
                nopoll_cnt = 0;
            }
        }
        bps_op_count++;
        return 0;
    }
    /* the list must stay below the SaveRAM region */
    if (list_ptr + desc_buf_pos + 10u > SRAM_SAVE_ADDR) { bps_op_err = 1; return 1; }
    if (desc_buf_pos + 10u > DESC_BUF_BYTES) desc_flush();
    uint8_t *d = &desc_buf[desc_buf_pos];
    d[0] = (uint8_t)(dst >> 16);   /* dstBank */
    d[1] = (uint8_t)(src >> 16);   /* srcBank */
    d[2] = (uint8_t)(src);         /* src[7:0]  */
    d[3] = (uint8_t)(src >> 8);    /* src[15:8] */
    d[4] = (uint8_t)(dst);         /* dst[7:0]  */
    d[5] = (uint8_t)(dst >> 8);    /* dst[15:8] */
    d[6] = (uint8_t)(len);         /* len[7:0]  */
    d[7] = (uint8_t)(len >> 8);    /* len[15:8] */
    d[8] = (uint8_t)(len >> 16);   /* len[23:16] */
    d[9] = 0x00;                   /* opcode COPY */
    desc_buf_pos += 10;
    bps_op_count++;
    return 0;
}

/* Flush the buffer and run the whole list on the FPGA copier (one trigger + poll).
   Returns 0 ok, 1 on failure (overflow latched earlier, or copier timeout). */
int patch_copier_finish(void) {
    if (bps_op_err) return 1;
    if (!USE_LIST_MODE) {
        /* wait for the last queued op to drain, then check the FPGA overrun bit --
           if set, the queue dropped an op (the MCU outran it) and the image is
           incomplete -> fail (the CRC verify would also catch it). */
        uint8_t st = fpga_copier_wait();
        if (st & 0x02) { bps_op_err = 1; return 1; }
        return 0;
    }
    desc_flush();
    if (bps_op_count == 0) return 0;          /* nothing to do */
    if (bps_op_count > 0xFFFFu) { bps_op_err = 1; return 1; } /* count is 16-bit on the FPGA */

    /* DIAGNOSTIC: dump list base/count + the first two descriptors (read back from
       the PSRAM list) to $FF0760, so a USB read shows whether the MCU wrote them
       correctly (vs an FPGA fetch hang). */
    sram_writebyte((uint8_t)(list_base_addr),       0xFF0760L);
    sram_writebyte((uint8_t)(list_base_addr >> 8),  0xFF0761L);
    sram_writebyte((uint8_t)(list_base_addr >> 16), 0xFF0762L);
    sram_writebyte((uint8_t)(bps_op_count),         0xFF0763L);
    sram_writebyte((uint8_t)(bps_op_count >> 8),    0xFF0764L);
    { uint8_t dsc[20];
      sram_readblock(dsc, list_base_addr, 20);
      sram_writeblock(dsc, 0xFF0768L, 20); }   /* descriptor 0 (10) + descriptor 1 (10) */

    if (fpga_copier_list(list_base_addr, (uint16_t)bps_op_count)) { bps_op_err = 1; return 1; }
    return 0;
}

/* Probe whether the currently-loaded FPGA core carries the MCU copier: write a
   marker to a scratch slot, copier-copy it to another, read it back.  A core
   WITHOUT the copier leaves the destination untouched (or the op times out).
   Must be called with the SNES held in reset.  Returns 1 if the copier works. */
int patch_copier_available(void) {
    uint8_t marker[4] = { 0xC0, 0x91, 0xE7, 0x5A };
    uint8_t got[4]    = { 0, 0, 0, 0 };
    uint32_t a = SRAM_SCRATCHPAD;          /* 0xFFFF00: free during a game load */
    uint32_t b = SRAM_SCRATCHPAD + 0x40;
    sram_writeblock(marker, a, 4);
    sram_writeblock(got, b, 4);            /* zero the destination first */
    if (fpga_copier_op(a, b, 4)) return 0; /* timeout -> no copier */
    sram_readblock(got, b, 4);
    return (got[0] == marker[0] && got[1] == marker[1]
         && got[2] == marker[2] && got[3] == marker[3]) ? 1 : 0;
}
