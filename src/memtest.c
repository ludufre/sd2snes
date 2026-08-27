/* sd2snes+ -- RAM connection test, run from the menu.
 *
 * Port of test_memconn() from the standalone diagnostic firmware (src/tests/tests.c).
 * Same walks, same conclusions, but the findings are published as the binary block in
 * memtest.h instead of printed over a serial console the Mk.II does not have.
 *
 * WHAT IT ACTUALLY PROVES: this is a WIRING test, not a capacity test.  It writes one
 * word at a handful of addresses (the base, plus one address per address line) and checks
 * that every data line toggles independently, that every address line reaches a distinct
 * cell, and that no two address lines are shorted together.  It does NOT sweep the whole
 * 16 MB -- that is test_mem() in the diagnostic firmware, which needs several minutes and
 * would tell a user with a marginal cell nothing they can act on.  The failures users
 * actually hit (a cracked solder joint, a bridged pin, a dead chip) all show up here.
 *
 * WHY IT NEEDS THE fpga_test CORE: the runtime cores leave RAM1 (U511, the 4 Mbit SRAM)
 * completely undriven -- RAM_DATA/RAM_ADDR have no assignment in sd2snes_base/main.v --
 * so half the test is unreachable without reconfiguring.  fpga_test is also the core the
 * OFFICIAL diagnostic firmware uses, which is what makes a result from here comparable
 * with one from the official image: same core, same walks, same verdict.
 */

#include <string.h>

#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "memmap.h"
#include "memory.h"
#include "memtest.h"

/* Scratch word used to load the FPGA's internal data register WITHOUT writing the test
   location -- that is what makes the byte-select check below meaningful.  Any address
   outside the walked set works; this is the one the diagnostic firmware uses. */
#define MT_DUMMY_ADDR   (0x56789aL)

/* Highest address-line offset walked on RAM0.  16 MB PSRAM -> A23. */
#define MT_RAM0_TOP     (0x800000L)
/* ...and on RAM1: 4 Mbit = 512 KB -> A18 (the diagnostic firmware walks one past, to
   0x20000, which is what catches an address line that is open ABOVE the array). */
#define MT_RAM1_TOP     (0x20000L)

/* Distinct faults on ONE chip past which its own CE#/OE#/WE# becomes the better
   explanation than that many independent broken lines, and the same across all chips for
   the FPGA side.  Both thresholds are the diagnostic firmware's. */
#define MT_CTRL_THRESHOLD      (20)
#define MT_FPGACTRL_THRESHOLD  (40)

typedef struct {
  memtest_finding_t find[MEMTEST_MAX_FINDINGS];
  uint8_t  nfind;
  uint8_t  truncated;
  /* Distinct faults already recorded, per chip.  These double as the DEDUP for the
     repeated passes (the walks run MEMTEST_ITERATIONS times to catch intermittents, so
     the same broken line is seen hundreds of times) AND as the fault COUNT the CE#/OE#/WE#
     heuristics threshold on.  Bitmasks, not the diagnostic firmware's byte arrays: those
     are 1.5 KB of stack, and this MCU has ~3 KB of stack+heap headroom in total. */
  uint32_t addr_seen[MEMTEST_NUM_CHIPS];   /* bit N = address line A<N> already reported */
  uint16_t data_seen[MEMTEST_NUM_CHIPS];   /* bit N = data line D<N> already reported */
  uint8_t  bytesel_seen[MEMTEST_NUM_CHIPS];
  uint8_t  ctrl_seen[MEMTEST_NUM_CHIPS];
  uint8_t  fpgactrl_seen;
} mt_ctx_t;

/* Append one finding, unless the exact same (chip, kind, line1, line2) is already in the
   list.  Shorts dedup THROUGH THIS LIST rather than through a bitmask of their own: a
   short pair needs 24x24 bits per chip, and the list is capped at 12 entries anyway, so a
   linear scan over at most 12 four-byte records is both smaller and simpler. */
static void mt_add(mt_ctx_t *ctx, uint8_t chip, uint8_t kind, uint8_t l1, uint8_t l2) {
  uint8_t i;
  for(i = 0; i < ctx->nfind; i++) {
    if(ctx->find[i].chip == chip && ctx->find[i].kind == kind
       && ctx->find[i].line1 == l1 && ctx->find[i].line2 == l2) return;
  }
  if(ctx->nfind >= MEMTEST_MAX_FINDINGS) { ctx->truncated = 1; return; }
  ctx->find[ctx->nfind].chip  = chip;
  ctx->find[ctx->nfind].kind  = kind;
  ctx->find[ctx->nfind].line1 = l1;
  ctx->find[ctx->nfind].line2 = l2;
  ctx->nfind++;
}

static void mt_data_fault(mt_ctx_t *ctx, uint8_t chip, uint8_t line) {
  if(ctx->data_seen[chip] & (1u << line)) return;
  ctx->data_seen[chip] |= (1u << line);
  mt_add(ctx, chip, MEMTEST_KIND_DATA, line, 0);
}

static void mt_addr_fault(mt_ctx_t *ctx, uint8_t chip, uint8_t line) {
  if(ctx->addr_seen[chip] & (1ul << line)) return;
  ctx->addr_seen[chip] |= (1ul << line);
  mt_add(ctx, chip, MEMTEST_KIND_ADDR, line, 0);
}

static void mt_bytesel_fault(mt_ctx_t *ctx, uint8_t chip) {
  if(ctx->bytesel_seen[chip]) return;
  ctx->bytesel_seen[chip] = 1;
  mt_add(ctx, chip, MEMTEST_KIND_BYTESEL, 0, 0);
}

/* popcount without a table: the counts are tiny and this runs a handful of times. */
static uint8_t mt_bits(uint32_t v) {
  uint8_t n = 0;
  while(v) { n += (v & 1); v >>= 1; }
  return n;
}

static uint8_t mt_fault_count(const mt_ctx_t *ctx, uint8_t chip) {
  return mt_bits(ctx->addr_seen[chip]) + mt_bits(ctx->data_seen[chip])
       + ctx->bytesel_seen[chip];
}

/* "Too many independent lines broken" is almost never that -- it is one control signal
   (CE#/OE#/WE#) taking the whole chip out.  Say so, so the user checks the right pin. */
static void mt_check_ctrl(mt_ctx_t *ctx, uint8_t chip) {
  if(ctx->ctrl_seen[chip]) return;
  if(mt_fault_count(ctx, chip) <= MT_CTRL_THRESHOLD) return;
  ctx->ctrl_seen[chip] = 1;
  mt_add(ctx, chip, MEMTEST_KIND_CTRL, 0, 0);
}

static void mt_check_fpgactrl(mt_ctx_t *ctx) {
  uint8_t chip, sum = 0;
  if(ctx->fpgactrl_seen) return;
  for(chip = 0; chip < MEMTEST_NUM_CHIPS; chip++) sum += mt_fault_count(ctx, chip);
  if(sum <= MT_FPGACTRL_THRESHOLD) return;
  ctx->fpgactrl_seen = 1;
  mt_add(ctx, MEMTEST_CHIP_RAM0_1, MEMTEST_KIND_FPGACTRL, 0, 0);
}

/* ---- RAM0: the PSRAM (U501, plus U502 on Mk.III).  16 bits wide, so the byte enables
   (LB#/UB#) are part of the wiring and get their own check. ------------------------- */
static void mt_ram0(mt_ctx_t *ctx) {
  static const uint8_t ram0_bases[] = RAM0_BASES;
  uint8_t chip;

  fpga_select_mem(0);
  for(chip = 0; chip < RAM0_NUM_CHIPS; chip++) {
    uint32_t addr = ram0_bases[chip];
    int iter;

    for(iter = 0; iter < MEMTEST_ITERATIONS; iter++) {
      uint32_t offset, shortoffset, data16;
      uint8_t line, shortline, shortcount;
      int localerror, byte, i;

      /* Zero every cell the walks below read back -- INCLUDING the other chip's, because
         a short between the two chips' address lines would otherwise read as this chip's
         own leftover data. */
      for(i = 0; i < RAM0_NUM_CHIPS; i++) {
        sram_writeshort(0x0000, ram0_bases[i]);
        for(offset = RAM0_SECOND_ADDR; offset <= MT_RAM0_TOP; offset <<= 1)
          sram_writeshort(0x0000, offset + ram0_bases[i]);
      }

      /* --- byte selects: write ONE byte of a word and prove the other byte survived.
         The dummy write in between is what makes this a real test: it loads the FPGA's
         data register with a different value, so a byte enable stuck active writes 0x55
         or 0xaa over the neighbour instead of the 0x22 we asked for. */
      localerror = 0;
      for(byte = 0; byte < 2; byte++) {
        sram_writeshort(0x7777, addr);
        sram_writeshort(0x55aa, MT_DUMMY_ADDR);
        sram_writebyte(0x22, addr ^ byte);
        if(sram_readbyte((addr + 1) ^ byte) != 0x77) localerror = 1;
      }
      if(localerror) mt_bytesel_fault(ctx, chip);

      /* --- data lines: each bit must be settable AND clearable on its own. */
      sram_writeshort(0x0000, addr);
      line = 0;
      for(data16 = 1; data16 <= 0x8000; data16 <<= 1) {
        localerror = 0;
        sram_writeshort((uint16_t)data16, addr);
        if(!(sram_readshort(addr) & data16)) localerror = 1;
        sram_writeshort((uint16_t)~data16, addr);
        if(sram_readshort(addr) & data16) localerror = 1;
        /* ^8 maps the walk index back to the chip's own D numbering: the halves of the
           16-bit word are swapped on the way to the pins (the diagnostic firmware prints
           the same corrected number, so the two reports name the same pin). */
        if(localerror) mt_data_fault(ctx, chip, line ^ 8);
        line++;
      }

      /* --- address lines: set exactly one line and prove (a) the cell it selects is
         reachable, (b) the base cell did NOT change (line stuck high / decoded twice),
         and (c) no OTHER single-line address also changed (two lines shorted). */
      line = 0;
      for(offset = RAM0_SECOND_ADDR; offset <= MT_RAM0_TOP; offset <<= 1) {
        for(shortoffset = RAM0_SECOND_ADDR; shortoffset <= MT_RAM0_TOP; shortoffset <<= 1)
          sram_writeshort(0, addr + shortoffset);
        sram_writebyte(0x00, addr);
        sram_writebyte(0xff, addr + offset);
        if(!sram_readbyte(addr + offset) || sram_readbyte(addr))
          mt_addr_fault(ctx, chip, line);

        shortcount = 0;
        shortline = 0;
        for(shortoffset = RAM0_SECOND_ADDR; shortoffset <= MT_RAM0_TOP; shortoffset <<= 1) {
          if(sram_readbyte(addr + shortoffset)) shortcount++;
          if(shortcount > 1) {
            shortcount = 1;   /* one report per extra hit, not one per remaining line */
            mt_add(ctx, chip, MEMTEST_KIND_SHORT, line, shortline);
          }
          shortline++;
        }
        line++;
      }
    }
    mt_check_ctrl(ctx, chip);
  }
}

/* ---- RAM1: the 4 Mbit SRAM (U511).  8 bits wide, so no byte enables. -------------- */
static void mt_ram1(mt_ctx_t *ctx) {
  int iter;

  fpga_select_mem(1);
  for(iter = 0; iter < MEMTEST_ITERATIONS; iter++) {
    uint32_t offset, shortoffset;
    uint8_t data, line, shortline;
    int localerror;

    for(offset = 1; offset <= MT_RAM1_TOP; offset <<= 1) sram_writebyte(0x00, offset);

    sram_writebyte(0x00, 0);
    line = 0;
    for(data = 1; data; data <<= 1) {
      localerror = 0;
      sram_writebyte(data, 0);
      if(!(sram_readbyte(0) & data)) localerror = 1;
      sram_writebyte(~data, 0);
      if(sram_readbyte(0) & data) localerror = 1;
      if(localerror) mt_data_fault(ctx, MEMTEST_CHIP_RAM1, line);
      line++;
    }

    line = 0;
    for(offset = 1; offset <= MT_RAM1_TOP; offset <<= 1) {
      for(shortoffset = 1; shortoffset <= MT_RAM1_TOP; shortoffset <<= 1)
        sram_writebyte(0, shortoffset);
      sram_writebyte(0x00, 0);
      sram_writebyte(0xff, offset);
      if(!sram_readbyte(offset) || sram_readbyte(0))
        mt_addr_fault(ctx, MEMTEST_CHIP_RAM1, line);

      shortline = 0;
      for(shortoffset = 1; shortoffset <= MT_RAM1_TOP; shortoffset <<= 1) {
        /* Unlike RAM0 there is no base offset here, so the line under test reads back
           its OWN 0xff -- skip it or every line would report a short with itself. */
        if(line != shortline && sram_readbyte(shortoffset))
          mt_add(ctx, MEMTEST_CHIP_RAM1, MEMTEST_KIND_SHORT, line, shortline);
        shortline++;
      }
      line++;
    }
  }
  mt_check_ctrl(ctx, MEMTEST_CHIP_RAM1);
}

/* ---- publishing ------------------------------------------------------------------ */

static void mt_write_blk(const memtest_blk_t *blk) {
  sram_writeblock((void *)blk, SRAM_MEMTEST_ADDR, sizeof(memtest_blk_t));
}

static void mt_blk_init(memtest_blk_t *blk, uint8_t state) {
  memset(blk, 0, sizeof(*blk));
  blk->magic[0] = MEMTEST_MAGIC0;
  blk->magic[1] = MEMTEST_MAGIC1;
  blk->version  = MEMTEST_VERSION;
  blk->state    = state;
  blk->chips    = RAM0_NUM_CHIPS;
}

void memtest_clear(void) {
  memtest_blk_t blk;
  mt_blk_init(&blk, MEMTEST_STATE_NONE);
  mt_write_blk(&blk);
}

void memtest_publish_nocore(void) {
  memtest_blk_t blk;
  mt_blk_init(&blk, MEMTEST_STATE_NOCORE);
  mt_write_blk(&blk);
}

int memtest_available(void) {
  FILINFO fno;
  fno.lfname = NULL;
  return f_stat((TCHAR *)FPGA_MEMTEST, &fno) == FR_OK;
}

void memtest_run(void) {
  mt_ctx_t ctx;
  memtest_blk_t blk;
  uint8_t i;

  memset(&ctx, 0, sizeof(ctx));

  printf("memtest: configuring %s\n", (const char *)FPGA_MEMTEST);
  fpga_pgm((uint8_t *)FPGA_MEMTEST);
  /* fpga_pgm sets fpga_config ONLY on success and returns quietly when it cannot open the
     file, so this is the one check that separates "the test core is up" from "the BASE
     core is still up".  It matters more than it looks: without the test core, every
     sram_* call below still WORKS (base drives the PSRAM fine) but fpga_select_mem means
     something else entirely there -- 0xee is SET213F on every runtime core -- so the run
     would report confident nonsense about RAM1.  Bail out and let the reload say so. */
  if(fpga_config != FPGA_MEMTEST) {
    printf("memtest: %s failed to configure\n", (const char *)FPGA_MEMTEST);
    mt_blk_init(&blk, MEMTEST_STATE_COREFAIL);
    mt_write_blk(&blk);
    return;
  }

  mt_ram0(&ctx);
  mt_ram1(&ctx);
  mt_check_fpgactrl(&ctx);

  /* The block lives in PSRAM, and mt_ram1 left the MCU window pointed at RAM1. */
  fpga_select_mem(0);

  mt_blk_init(&blk, MEMTEST_STATE_DONE);
  blk.nfind = ctx.nfind;
  if(ctx.nfind) blk.flags |= MEMTEST_FLAG_FAIL;
  if(ctx.truncated) blk.flags |= MEMTEST_FLAG_TRUNCATED;
  for(i = 0; i < ctx.nfind; i++) blk.findings[i] = ctx.find[i];
  mt_write_blk(&blk);

  printf("memtest: %d finding(s)%s\n", ctx.nfind, ctx.truncated ? " (truncated)" : "");
}
