/* sd2snes fork -- in-game RAM trainer: MCU side. See trainer.h. */

#include "config.h"
#include "uart.h"
#include "memory.h"
#include "cheat.h"
#include "snes.h"
#include "trainer.h"
#include "cheatedit.h"

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
_Static_assert(offsetof(trainer_blk_t, req)      == 0x38, "TR_REQ");
_Static_assert(offsetof(trainer_blk_t, req_off)  == 0x3A, "TR_REQ_OFF");
_Static_assert(offsetof(trainer_blk_t, req_val)  == 0x3E, "TR_REQ_VAL");
_Static_assert(sizeof(trainer_pin_t) == 8,               "trainer_pin_t must be 8 bytes (TR_PIN_SIZE)");
_Static_assert(offsetof(trainer_pin_t, val)   == 4,      "TR_PIN_VAL");
_Static_assert(offsetof(trainer_pin_t, flags) == 6,      "TR_PIN_FLAGS");
_Static_assert(sizeof(trainer_pin_t) * TRAINER_PIN_MAX == TRAINER_PINS_BYTES, "TRAINER_PINS_BYTES");

/* WRAM offsets the tab may hand us; anything else is a stale/corrupt block. */
#define TRAINER_WRAM_BYTES  (0x20000UL)

#define TR_CE(ofs)          (SRAM_CHEAT_EDIT_ADDR + (uint32_t)(ofs))

static const char tr_hex[] = "0123456789ABCDEF";   /* NUL included: sizing it [16] trips -Wunterminated-string-initialization on newer GCC */

static char *tr_put_hex(char *p, uint32_t v, int digits) {
  while(digits--) *p++ = tr_hex[(v >> (digits * 4)) & 0xf];
  return p;
}

/* "Trainer $7E1694 = 87" / "= 04D2" for 16-bit. Fits well inside the 63 visible bytes
   of the cheat editor's name field (CHEAT_EDIT_NAME_LEN). */
static int tr_make_desc(char *out, uint32_t addr, uint16_t val, uint8_t width) {
  char *p = out;
  memcpy(p, "Trainer $", 9); p += 9;
  p = tr_put_hex(p, addr, 6);
  memcpy(p, " = ", 3); p += 3;
  p = tr_put_hex(p, val, width == 2 ? 4 : 2);
  *p = 0;
  return (int)(p - out);
}

/* A pin the MCU may act on: in use, and inside WRAM (a 16-bit one needs room for both
   bytes). Anything else is a stale or corrupt entry and is ignored. */
static int tr_pin_valid(const trainer_pin_t *p) {
  if(p->off >= TRAINER_WRAM_BYTES) return 0;          /* also rejects TRAINER_PIN_EMPTY */
  if((p->flags & TRAINER_PIN_WIDE) && p->off + 1 >= TRAINER_WRAM_BYTES) return 0;
  return 1;
}

static int tr_session_version_ok(void) {
  return sram_readbyte(SRAM_TRAINER_META_ADDR + offsetof(trainer_blk_t, version)) == TRAINER_VERSION;
}

void trainer_stage(void) {
  trainer_blk_t blk;
  memset(&blk, 0, sizeof(blk));
  blk.version = TRAINER_VERSION;
  sram_writeblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
  sram_memset(SRAM_TRAINER_PINS_ADDR, TRAINER_PINS_BYTES, 0xFF);   /* every off = TRAINER_PIN_EMPTY */
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

/* Not gated on the magic on purpose: trainer_invalidate() drops the search but the
   pins are the user's and stay in effect. The version is what proves the table was
   written by a tab that speaks this layout (trainer_stage() stamps it every load). */
void __attribute__((noinline)) trainer_program_freezes(void) {
  trainer_pin_t pins[TRAINER_PIN_MAX];
  int frozen = 0;
  if(!tr_session_version_ok()) return;
  sram_readblock(pins, SRAM_TRAINER_PINS_ADDR, sizeof(pins));
  for(int i = 0; i < TRAINER_PIN_MAX && frozen < TRAINER_FREEZE_MAX; i++) {
    cheat_patch_record_t patch;
    uint32_t addr;
    if(!(pins[i].flags & TRAINER_PIN_FROZEN) || !tr_pin_valid(&pins[i])) continue;
    frozen++;
    addr = 0x7E0000UL + pins[i].off;              /* the +1 below may cross into $7F */
    patch.code = (addr << 8) | (pins[i].val & 0xff);
    cheat_program_single(&patch);
    if(pins[i].flags & TRAINER_PIN_WIDE) {
      patch.code = ((addr + 1) << 8) | ((pins[i].val >> 8) & 0xff);
      cheat_program_single(&patch);
    }
  }
}

/* SAVE: hand the address to the cheat editor as an ADD, exactly as if the user had
   typed it: name "Trainer $7E0DBF = 63", raw code(s) "7E0DBF63". Going through
   cheat_edit_serve is what gives the new cheat its display strings, its place at the
   top of the list and the .yml write -- none of it duplicated here. */
static void tr_stage_save(uint32_t off, uint16_t val, uint8_t width) {
  char buf[CHEAT_EDIT_NAME_LEN];
  uint32_t addr = 0x7E0000UL + off;
  memset(buf, 0, sizeof(buf));
  tr_make_desc(buf, addr, val, width);
  sram_writeblock(buf, TR_CE(CHEAT_EDIT_OFS_NAME), sizeof(buf));
  memset(buf, 0, CHEAT_EDIT_CODE_LEN);
  tr_put_hex(buf, (addr << 8) | (val & 0xff), 8);
  sram_writeblock(buf, TR_CE(CHEAT_EDIT_OFS_CODES), CHEAT_EDIT_CODE_LEN);
  if(width == 2) {
    tr_put_hex(buf, ((addr + 1) << 8) | ((val >> 8) & 0xff), 8);
    sram_writeblock(buf, TR_CE(CHEAT_EDIT_OFS_CODES + CHEAT_EDIT_CODE_LEN), CHEAT_EDIT_CODE_LEN);
  }
  sram_writeshort(0, TR_CE(CHEAT_EDIT_OFS_IDX));
  sram_writebyte(0, TR_CE(CHEAT_EDIT_OFS_FLAGS));
  sram_writebyte(width, TR_CE(CHEAT_EDIT_OFS_NUMCODES));
  sram_writebyte(CHEAT_EDIT_OP_ADD, TR_CE(CHEAT_EDIT_OFS_OP));
}

int trainer_serve_request(void) {
  trainer_blk_t blk;
  uint8_t req, width;

  sram_readblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
  if(memcmp(blk.magic, "TRNR", 4) || blk.version != TRAINER_VERSION) return 0;

  req = blk.req;
  width = (blk.width == 2) ? 2 : 1;
  if(req == TRAINER_REQ_NONE) return 0;
  sram_writebyte(TRAINER_REQ_NONE, SRAM_TRAINER_META_ADDR + offsetof(trainer_blk_t, req));

  if(req == TRAINER_REQ_APPLY) {
    cheat_program();
    return 0;
  }
  if(req != TRAINER_REQ_SAVE) return 0;
  if(blk.req_off >= TRAINER_WRAM_BYTES) return 0;          /* stale/corrupt block */
  if(width == 2 && blk.req_off + 1 >= TRAINER_WRAM_BYTES) return 0;
  tr_stage_save(blk.req_off, blk.req_val, width);
  return 1;
}

void trainer_save_done(void) {
  trainer_pin_t pin;
  uint32_t off = 0;
  sram_readblock(&off, SRAM_TRAINER_META_ADDR + offsetof(trainer_blk_t, req_off), 4);
  for(int i = 0; i < TRAINER_PIN_MAX; i++) {
    uint32_t a = SRAM_TRAINER_PINS_ADDR + (uint32_t)sizeof(pin) * (uint32_t)i;
    sram_readblock(&pin, a, sizeof(pin));
    if(pin.off != off || !(pin.flags & TRAINER_PIN_FROZEN)) continue;
    sram_writebyte(pin.flags & (uint8_t)~TRAINER_PIN_FROZEN, a + offsetof(trainer_pin_t, flags));
  }
  cheat_program();
}
