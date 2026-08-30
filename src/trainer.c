/* sd2snes fork -- in-game RAM trainer: MCU side. See trainer.h. */

#include "config.h"
#include "uart.h"
#include "memory.h"
#include "cheat.h"
#include "snes.h"
#include "trainer.h"

#include <stddef.h>
#include <string.h>

/* Lockstep proof against the TR_* offsets in snes/memmap.i65. */
_Static_assert(sizeof(trainer_blk_t) == 64,           "trainer_blk_t must be 64 bytes");
_Static_assert(offsetof(trainer_blk_t, version)  == 0x04, "TR_VERSION");
_Static_assert(offsetof(trainer_blk_t, active)   == 0x05, "TR_ACTIVE");
_Static_assert(offsetof(trainer_blk_t, mode)     == 0x06, "TR_MODE");
_Static_assert(offsetof(trainer_blk_t, width)    == 0x07, "TR_WIDTH");
_Static_assert(offsetof(trainer_blk_t, count)    == 0x08, "TR_COUNT");
_Static_assert(offsetof(trainer_blk_t, value)    == 0x0C, "TR_VALUE");
_Static_assert(offsetof(trainer_blk_t, have_snap)== 0x0E, "TR_HAVE_SNAP");
_Static_assert(offsetof(trainer_blk_t, notice)   == 0x0F, "TR_NOTICE");
_Static_assert(offsetof(trainer_blk_t, cursor)   == 0x10, "TR_CURSOR");
_Static_assert(offsetof(trainer_blk_t, top)      == 0x14, "TR_TOP");
_Static_assert(offsetof(trainer_blk_t, sel_off)  == 0x18, "TR_SEL_OFF");
_Static_assert(offsetof(trainer_blk_t, ui)       == 0x1C, "TR_UI");
_Static_assert(offsetof(trainer_blk_t, fz_idx)   == 0x20, "TR_FZ_IDX");
_Static_assert(offsetof(trainer_blk_t, fz_off)   == 0x28, "TR_FZ_OFF");
_Static_assert(offsetof(trainer_blk_t, req)      == 0x38, "TR_REQ");
_Static_assert(offsetof(trainer_blk_t, req_slot) == 0x39, "TR_REQ_SLOT");
_Static_assert(offsetof(trainer_blk_t, req_off)  == 0x3A, "TR_REQ_OFF");
_Static_assert(offsetof(trainer_blk_t, req_val)  == 0x3E, "TR_REQ_VAL");

/* WRAM offsets the tab may hand us; anything else is a stale/corrupt block. */
#define TRAINER_WRAM_BYTES  (0x20000UL)

/* Record slot geometry, from cheat.h/cheat.c: the struct is 416 B but the PSRAM slot
   stride is 512, and the 96-byte tail carries the PSRAM-patch bookkeeping. */
#define TR_REC_STRIDE       (512)
#define TR_REC_DESC_OFS     (1)
#define TR_REC_DESC_LEN     (254)
#define TR_REC_NPATCH_OFS   (255)
#define TR_REC_PATCH_OFS    (256)
#define TR_REC_PATCH_LEN    (CHEAT_NUM_CODES_PER_CHEAT * 4)

static const char tr_hex[] = "0123456789ABCDEF";   /* NUL included: sizing it [16] trips -Wunterminated-string-initialization on newer GCC */

static char *tr_put_hex(char *p, uint32_t v, int digits) {
  while(digits--) *p++ = tr_hex[(v >> (digits * 4)) & 0xf];
  return p;
}

/* "Trainer $7E1694 = 87" / "= 04D2" for 16-bit. Fits well inside the 254-byte
   description field and inside the 63 visible bytes of the in-game name window. */
static int tr_make_desc(char *out, uint32_t addr, uint16_t val, uint8_t width) {
  char *p = out;
  memcpy(p, "Trainer $", 9); p += 9;
  p = tr_put_hex(p, addr, 6);
  memcpy(p, " = ", 3); p += 3;
  p = tr_put_hex(p, val, width == 2 ? 4 : 2);
  *p = 0;
  return (int)(p - out);
}

/* Write ONE runtime cheat record straight into its PSRAM slot.
   noinline + piecewise on purpose: a cheat_record_t on the stack is 416 bytes, and the
   MCU has only a few KB of stack+heap headroom (the .bss/stack gotcha), so we never
   materialise one -- and never hold it across the cheat_program() call either. */
static void __attribute__((noinline))
tr_write_record(int idx, uint32_t addr, uint16_t val, uint8_t width, uint8_t enable) {
  uint32_t base = SRAM_CHEAT_ADDR + (uint32_t)TR_REC_STRIDE * (uint32_t)idx;
  char desc[40];
  uint8_t patch[8];
  int len = tr_make_desc(desc, addr, val, width);
  uint32_t a1 = addr + 1;                 /* may cross $7EFFFF -> $7F0000 */

  sram_writebyte(enable ? CHEAT_FLAG_ENABLE : 0, base);

  /* description: the generated text, then zero-fill the rest of the field so a slot
     recycled from a previous YAML load cannot show through. */
  sram_writeblock(desc, base + TR_REC_DESC_OFS, (uint16_t)(len + 1));
  sram_memset(base + TR_REC_DESC_OFS + len + 1, TR_REC_DESC_LEN - len - 1, 0);

  sram_writebyte(width == 2 ? 2 : 1, base + TR_REC_NPATCH_OFS);

  /* cheat_patch_record_t is packed {value, addr16 LE, bank} = the raw/PAR word
     bank<<24 | addr<<8 | value. A 16-bit freeze is two byte patches. */
  patch[0] = (uint8_t)(val & 0xff);
  patch[1] = (uint8_t)(addr & 0xff);
  patch[2] = (uint8_t)((addr >> 8) & 0xff);
  patch[3] = (uint8_t)((addr >> 16) & 0xff);
  patch[4] = (uint8_t)((val >> 8) & 0xff);
  patch[5] = (uint8_t)(a1 & 0xff);
  patch[6] = (uint8_t)((a1 >> 8) & 0xff);
  patch[7] = (uint8_t)((a1 >> 16) & 0xff);
  sram_writeblock(patch, base + TR_REC_PATCH_OFS, width == 2 ? 8 : 4);
  sram_memset(base + TR_REC_PATCH_OFS + (width == 2 ? 8 : 4),
              TR_REC_PATCH_LEN - (width == 2 ? 8 : 4), 0);

  /* spare tail: the ROM-code original-byte/applied flags. Zero so cheat_rom_psram_apply
     can never think this WRAM record has an image byte stashed. */
  sram_memset(base + TR_REC_PATCH_OFS + TR_REC_PATCH_LEN,
              TR_REC_STRIDE - (TR_REC_PATCH_OFS + TR_REC_PATCH_LEN), 0);

  /* the BSRAM flag mirror the in-game CHEATS tab reads and writes */
  sram_writebyte(enable ? CHEAT_FLAG_ENABLE : 0, SRAM_CHEAT_FLAGS_ADDR + idx);
}

void trainer_stage(void) {
  trainer_blk_t blk;
  memset(&blk, 0, sizeof(blk));
  for(int i = 0; i < TRAINER_FREEZE_MAX; i++) blk.fz_idx[i] = 0xFFFF;
  blk.version = TRAINER_VERSION;
  sram_writeblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
}

void trainer_invalidate(uint8_t reason) {
  trainer_blk_t blk;
  sram_readblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
  if(memcmp(blk.magic, "TRNR", 4)) return;    /* no session -> nothing to say */
  memset(blk.magic, 0, 4);
  blk.active = 0;
  blk.have_snap = 0;
  blk.count = 0;
  blk.notice = reason;
  sram_writeblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
}

void trainer_serve_request(void) {
  trainer_blk_t blk;
  uint32_t addr;
  int idx;
  int count;
  uint8_t slot, req, width;

  sram_readblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
  if(memcmp(blk.magic, "TRNR", 4) || blk.version != TRAINER_VERSION) return;

  req  = blk.req;
  slot = blk.req_slot;
  width = (blk.width == 2) ? 2 : 1;
  if(req > TRAINER_REQ_ADD || req == TRAINER_REQ_NONE || slot >= TRAINER_FREEZE_MAX) return;

  blk.req = TRAINER_REQ_NONE;                /* consumed, whatever happens below */

  if(req == TRAINER_REQ_UNFREEZE) {
    idx = (int)blk.fz_idx[slot];
    if(idx >= 0 && idx < CHEAT_RECORD_MAX) {
      sram_writebyte(0, SRAM_CHEAT_ADDR + (uint32_t)TR_REC_STRIDE * (uint32_t)idx);
      sram_writebyte(0, SRAM_CHEAT_FLAGS_ADDR + idx);
    }
    blk.fz_idx[slot] = 0xFFFF;
    blk.fz_off[slot] = 0;
    sram_writeblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
    cheat_program();
    return;
  }

  if(blk.req_off >= TRAINER_WRAM_BYTES) return;      /* stale/corrupt block */
  if(width == 2 && blk.req_off + 1 >= TRAINER_WRAM_BYTES) return;
  addr = 0x7E0000UL + blk.req_off;

  /* Reuse the record this slot already owns; otherwise append one past the .yml cheats.
     Reusing is what keeps NUM_CHEATS from growing every time the user re-freezes. */
  idx = (int)blk.fz_idx[slot];
  count = (int)sram_readshort(SRAM_NUM_CHEATS);
  if(count < 0 || count > CHEAT_RECORD_MAX) count = 0;
  if(idx < 0 || idx >= count) {
    if(count >= CHEAT_RECORD_MAX) {
      /* Every record slot is taken by the game's own cheats: refuse loudly (the tab
         sees fz_idx still empty and reports it) instead of overwriting one of them. */
      sram_writeblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
      printf("trainer: no free cheat record (%d used)\n", count);
      return;
    }
    idx = count;
    sram_writeshort((uint16_t)(count + 1), SRAM_NUM_CHEATS);
  }

  /* ADD TO CHEATS is the same record with its enable bit clear: the address is listed in
     the CHEATS tab where the user can arm it later, without freezing anything now. */
  tr_write_record(idx, addr, blk.req_val, width, req == TRAINER_REQ_FREEZE);

  blk.fz_idx[slot] = (uint16_t)idx;
  blk.fz_off[slot] = blk.req_off;
  sram_writeblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));

  /* Refresh the resident 64-name window so the new entry is readable from the CHEATS
     tab (in-game the $D00000 records ARE the game's ROM), then redeploy. */
  cheat_stage_names_window((int)sram_readshort(SRAM_CHEAT_WIN_BASE_ADDR));
  cheat_program();
}
