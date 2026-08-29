/* sd2snes+ -- RAM connection test, run from the menu.
 *
 * Port of test_memconn() from the standalone diagnostic firmware (src/tests/tests.c).
 * Same walks, same conclusions, but the findings are published as the binary block in
 * memtest.h instead of printed over a serial console the Mk.II does not have.
 *
 * WHAT THE WIRING WALK PROVES: it writes one word at a handful of addresses (the base,
 * plus one address per address line) and checks that every data line toggles
 * independently, that every address line reaches a distinct cell, and that no two address
 * lines are shorted together.  The failures users hit most (a cracked solder joint, a
 * bridged pin, a dead chip) all show up here, in ~10 seconds.
 *
 * WHAT IT DOES NOT PROVE, and why the cell sweep below exists: a single weak cell in the
 * middle of the array passes every walk above.  That is not a hypothetical gap -- the
 * official diagnostic has the same one.  Its test_mem() sweeps all 16 MB but has NO
 * CALLER, and the SNES-side memtest: in snes/tests/tests.a65 is commented out, so the only
 * cells it ever verifies are the first 1 MiB of 16, incidentally, inside test_sddma().
 * MEMTEST_MODE_FULL closes that: one write pass and one verify pass over both arrays,
 * ~20 seconds on top of the walk, on its own button so nobody pays for it by default.
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
#include "led.h"
#include "memmap.h"
#include "memory.h"
#include "memtest.h"
#include "timer.h"

/* Scratch word used to load the FPGA's internal data register WITHOUT writing the test
   location -- that is what makes the byte-select check below meaningful.  Any address
   outside the walked set works; this is the one the diagnostic firmware uses. */
#define MT_DUMMY_ADDR   (0x56789aL)

/* Highest address-line offset walked on RAM0.  16 MB PSRAM -> A23. */
#define MT_RAM0_TOP     (0x800000L)
/* ...and on RAM1: up to A17, which is the diagnostic firmware's own bound and the one
   that matches what the MCU can actually reach on this array (A0..A17 => 0x40000 bytes).
   Do NOT "fix" this to A18 by reasoning from the "4 Mbit" on the schematic: the sweep
   below derives its extent from this constant, and everything past 0x40000 measures as
   garbage on known-good hardware. */
#define MT_RAM1_TOP     (0x20000L)

/* Full extent of each array, for the cell sweep.  Not to be confused with the *_TOP
   address-line offsets above: those are the highest BIT walked, these are the byte count.
   RAM0 is 16 MB on both boards (Mk.II reaches it with one 128 Mbit chip, Mk.III with two
   64 Mbit ones interleaved), so the sweep is board-independent.

   MT_RAM1_SIZE IS DERIVED FROM MT_RAM1_TOP, NOT FROM "4 Mbit".  The walk above covers
   A0..A17, i.e. 2^18 == 0x40000 addressable bytes, and that is the extent the official
   diagnostic trusts on this array too.  Sweeping the 0x80000 that "4 Mbit = 512 KB"
   suggests reads past the end: measured on a healthy Mk.III, every byte below 0x40000
   verifies clean while everything above it comes back unrelated to the pattern, with a
   different error count on every run (2406 / 2439 / 4082) -- the signature of reading
   nothing, not of a bad cell.  Keep the two in step: a board with more RAM1 has to raise
   MT_RAM1_TOP, and this follows from it. */
#define MT_RAM0_SIZE    (0x1000000UL)
#define MT_RAM1_SIZE    (MT_RAM1_TOP * 2)

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

/* ---- RAM0 and RAM1: the cell sweep (MEMTEST_MODE_FULL) --------------------------- */

/* The diagnostic firmware's pattern (test_mem, src/tests/tests.c).  Every byte differs
   from its neighbours AND from the same offset one 64 KB page up, so an address line that
   aliases reads back as mismatched DATA rather than as a plausible value.  Kept
   byte-identical to that one on purpose: it is what makes a result from here comparable
   with a result from the official diagnostic image. */
#define MT_PATTERN(a)   ((uint8_t)((a) + ((a) >> 8) + ((a) >> 16)))

/* Ticks between LED flips (10 ms each), and how often the sweep bothers to look.  The
   screen is black and the SNES is in reset for half a minute, so with no LED the console
   is indistinguishable from a hung one.  Checking the clock every 64 KB costs 256 reads
   per pass instead of one per byte, and 64 KB is ~40 ms of sweeping, finer than the blink
   rate either way. */
#define MT_LED_TICKS    (25)
#define MT_LED_MASK     (0xFFFFUL)

typedef struct {
  uint32_t bad;     /* bad bytes seen, saturating at MEMTEST_CELL_BAD_MAX */
  uint32_t addr;    /* address of the first one */
  uint8_t  got;     /* what was actually read there */
  uint8_t  info;    /* MEMTEST_CELL_* */
  uint8_t  led;     /* current LED state, so the flip does not have to read it back */
  tick_t   led_at;  /* tick of the last flip */
} mt_cell_t;

/* Straight writeled(), never toggle_write_led(): that one passes ~state, and in PWM mode
   -- which is how the menu runs, see led_pwm() in main() -- ~1 is still non-zero, so the
   LED would come on and never go dark again. */
static void mt_cell_led(mt_cell_t *cc) {
  tick_t now = getticks();
  if(now - cc->led_at < MT_LED_TICKS) return;
  cc->led_at = now;
  cc->led ^= 1;
  writeled(cc->led);
}

/* One write pass: the burst shape of sram_memset (memory.c) with the pattern in place of
   a constant.  The bounded wait is the difference that matters at this length -- 16 M
   unbounded FPGA_WAIT_RDYs wedge the MCU forever if the core ever stops asserting ready,
   and there is no SNES left running to notice. */
static void mt_cell_write(mt_cell_t *cc, uint32_t len) {
  uint32_t i;
  uint8_t timeout = 0;

  set_mcu_addr(0);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_WRITEMEM | FPGA_MEM_AUTOINC);
  for(i = 0; i < len; i++) {
    if(!(i & MT_LED_MASK)) mt_cell_led(cc);
    FPGA_TX_BYTE(MT_PATTERN(i));
    FPGA_WAIT_RDY_TO_INLINE(timeout);
    if(timeout) break;
  }
  FPGA_DESELECT();
  if(timeout) cc->info |= MEMTEST_CELL_TIMEOUT;
}

static void mt_cell_verify(mt_cell_t *cc, uint32_t len, uint8_t ram1) {
  uint32_t i;
  uint8_t timeout = 0, data;

  set_mcu_addr(0);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_READMEM | FPGA_MEM_AUTOINC);
  for(i = 0; i < len; i++) {
    if(!(i & MT_LED_MASK)) mt_cell_led(cc);
    FPGA_WAIT_RDY_TO_INLINE(timeout);
    if(timeout) break;
    data = FPGA_RX_BYTE();
    if(data == MT_PATTERN(i)) continue;
    /* Only the FIRST bad byte is located.  One address is what a user can act on; a list
       of millions would not fit the block and would not say anything the count does not. */
    if(!cc->bad) {
      cc->addr = i;
      cc->got  = data;
      if(ram1) cc->info |= MEMTEST_CELL_RAM1;
    }
    if(cc->bad < MEMTEST_CELL_BAD_MAX) cc->bad++;
  }
  FPGA_DESELECT();
  if(timeout) cc->info |= MEMTEST_CELL_TIMEOUT;
}

/* RAM1 is swept ONE BYTE AT A TIME, not in a burst like RAM0, which is what the diagnostic
   firmware's test_mem does and is worth keeping.  The burst version of this reported
   thousands of scattered bad bytes on a board whose wiring walk passes clean and whose
   RAM0 sweep is spotless -- and since no runtime core even drives RAM1 (RAM_DATA/RAM_ADDR
   have no assignment outside fpga_test), there is no independent evidence to check that
   against.  Per-byte accesses are the same path test_memconn uses on this array and the
   same one the official image trusts, so a fault reported here is a fault two different
   methods agree on.  512 KB at ~4 us/byte is ~4 s; that is the price of an answer worth
   printing on screen. */
static void mt_cell_ram1(mt_cell_t *cc) {
  uint32_t i;
  uint8_t data;

  for(i = 0; i < MT_RAM1_SIZE; i++) {
    if(!(i & MT_LED_MASK)) mt_cell_led(cc);
    sram_writebyte(MT_PATTERN(i), i);
  }
  for(i = 0; i < MT_RAM1_SIZE; i++) {
    if(!(i & MT_LED_MASK)) mt_cell_led(cc);
    data = sram_readbyte(i);
    if(data == MT_PATTERN(i)) continue;
    if(!cc->bad) {
      cc->addr = i;
      cc->got  = data;
      cc->info |= MEMTEST_CELL_RAM1;
    }
    if(cc->bad < MEMTEST_CELL_BAD_MAX) cc->bad++;
  }
}

/* The sweep writes its pattern over ALL of RAM0, and the top page of that is the block the
   menu and the firmware talk through: SRAM_MENU_CFG_ADDR, every in-game gate, SAVEINFO,
   MANUAL_META, EXPORT_RESULT, and this test's own result block.  Leaving the pattern there
   is not "scribbled over the PSRAM, the reload fixes it" -- the reload only rebuilds the
   MENU IMAGE.  Nobody clears this page outside first boot, so a pattern byte in
   SRAM_EXPORT_RESULT_ADDR comes back as a "ROM export failed" modal on the very next
   browser repaint (MT_PATTERN(0xFF071E) == 0x24, which is not any PATCH_EXPORT_* value but
   is not zero either).  Zero the page and let each subsystem's boot path refill what it
   owns -- which is exactly what a cold start would hand them anyway.
   Called BEFORE the result is published, since MEMTEST_BLK lives in this page too. */
#define MT_STATE_PAGE_ADDR   (0xFF0000L)
#define MT_STATE_PAGE_SIZE   (0x800L)

static void mt_cells(mt_cell_t *cc) {
  fpga_select_mem(0);
  mt_cell_write(cc, MT_RAM0_SIZE);
#ifdef MEMTEST_SELFTEST
  /* Mutation gate: corrupt one byte at a known address between the passes, so the failure
     path can be exercised on healthy hardware.  MEMTEST_SELFTEST is deliberately absent
     from every config-mk*, i.e. this is never in a shipped image -- define it on the
     command line for one build, check that the screen names exactly this address and that
     cell_bad reads 1, then rebuild without it. */
  sram_writebyte((uint8_t)~MT_PATTERN(MEMTEST_SELFTEST), MEMTEST_SELFTEST);
#endif
  if(!(cc->info & MEMTEST_CELL_TIMEOUT)) mt_cell_verify(cc, MT_RAM0_SIZE, 0);

  /* RAM1 only if RAM0 finished: a timeout means the ready line is gone, and hammering a
     second array through the same dead handshake just costs another wait bound. */
  if(!(cc->info & MEMTEST_CELL_TIMEOUT)) {
    fpga_select_mem(1);
    mt_cell_ram1(cc);
  }

  /* Back to RAM0 for the cleanup above -- mt_cell_verify left the window on RAM1. */
  fpga_select_mem(0);
  sram_memset(MT_STATE_PAGE_ADDR, MT_STATE_PAGE_SIZE, 0);

  writeled(0);
  cc->info |= MEMTEST_CELL_RAN;
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

void memtest_run(uint8_t mode) {
  mt_ctx_t ctx;
  mt_cell_t cc;
  memtest_blk_t blk;
  uint8_t i;

  memset(&ctx, 0, sizeof(ctx));
  memset(&cc, 0, sizeof(cc));

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

  /* The sweep only says something over wiring that already checked out: one open address
     line aliases half the array onto the other half, so every cell in it reads back wrong
     and the real answer disappears under millions of "bad cells".  Say it was skipped
     rather than reporting a number that means nothing. */
  if(mode == MEMTEST_MODE_FULL) {
    if(ctx.nfind) cc.info = MEMTEST_CELL_RAN | MEMTEST_CELL_SKIPPED;
    else mt_cells(&cc);
  }

  /* The block lives in PSRAM, and both mt_ram1 and mt_cells leave the MCU window on RAM1. */
  fpga_select_mem(0);

  mt_blk_init(&blk, MEMTEST_STATE_DONE);
  blk.nfind = ctx.nfind;
  if(ctx.nfind || cc.bad) blk.flags |= MEMTEST_FLAG_FAIL;
  if(ctx.truncated) blk.flags |= MEMTEST_FLAG_TRUNCATED;
  for(i = 0; i < ctx.nfind; i++) blk.findings[i] = ctx.find[i];
  blk.cell_bad[0]  = cc.bad & 0xff;
  blk.cell_bad[1]  = (cc.bad >> 8) & 0xff;
  blk.cell_bad[2]  = (cc.bad >> 16) & 0xff;
  blk.cell_got     = cc.got;
  blk.cell_addr[0] = cc.addr & 0xff;
  blk.cell_addr[1] = (cc.addr >> 8) & 0xff;
  blk.cell_addr[2] = (cc.addr >> 16) & 0xff;
  blk.cell_info    = cc.info;
  mt_write_blk(&blk);

  printf("memtest: %d finding(s)%s, %lu bad cell(s)\n", ctx.nfind,
         ctx.truncated ? " (truncated)" : "", (unsigned long)cc.bad);
}
