/* Conformance test for the MCU half of the in-game RAM trainer (src/trainer.c), compiled
   against the REAL source over a flat fake PSRAM.
   What it pins down has no hardware fallback: which WRAM patches a frozen pin turns into
   (a wrong word freezes the wrong address and nothing crashes), that a freeze never
   becomes a cheat record, and what SAVE CHEAT hands to the cheat editor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "memory.h"
#include "cheat.h"
#include "cheatedit.h"
#include "trainer.h"

#define PSRAM_SIZE (0x1000000UL)
static uint8_t *psram;

int cheat_program_calls;
static uint32_t emitted[64];
static int n_emitted;
void cheat_program(void) {
  cheat_program_calls++;
  n_emitted = 0;
  trainer_program_freezes();                 /* what the real cheat_program does first */
}
void cheat_program_single(cheat_patch_record_t *cheat) {
  if(n_emitted < 64) emitted[n_emitted++] = cheat->code;
}

void sram_writebyte(uint8_t val, uint32_t addr) { psram[addr] = val; }
uint8_t sram_readbyte(uint32_t addr)            { return psram[addr]; }
void sram_writeshort(uint16_t val, uint32_t addr) {
  psram[addr] = val & 0xff; psram[addr + 1] = val >> 8;
}
uint16_t sram_readshort(uint32_t addr) {
  return (uint16_t)psram[addr] | ((uint16_t)psram[addr + 1] << 8);
}
uint16_t sram_writeblock(void *buf, uint32_t addr, uint16_t size) {
  memcpy(psram + addr, buf, size); return size;
}
uint16_t sram_readblock(void *buf, uint32_t addr, uint16_t size) {
  memcpy(buf, psram + addr, size); return size;
}
void sram_memset(uint32_t base_addr, uint32_t len, uint8_t val) {
  memset(psram + base_addr, val, len);
}

static int fails;
#define CHECK(cond, ...) do { if(!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while(0)

static trainer_blk_t blk_read(void) {
  trainer_blk_t b; sram_readblock(&b, SRAM_TRAINER_META_ADDR, sizeof(b)); return b;
}
static void blk_write(const trainer_blk_t *b) {
  trainer_blk_t t = *b; sram_writeblock(&t, SRAM_TRAINER_META_ADDR, sizeof(t));
}
static trainer_pin_t pin_read(int i) {
  trainer_pin_t p; sram_readblock(&p, SRAM_TRAINER_PINS_ADDR + 8u * (uint32_t)i, sizeof(p)); return p;
}
/* the tab writes the pin table itself; this mirrors what snes/trainer.i65 stores */
static void pin_write(int i, uint32_t off, uint16_t val, uint8_t flags) {
  trainer_pin_t p; p.off = off; p.val = val; p.flags = flags; p.rsvd = 0;
  sram_writeblock(&p, SRAM_TRAINER_PINS_ADDR + 8u * (uint32_t)i, sizeof(p));
}
static void session_open(void) {
  trainer_blk_t b = blk_read(); memcpy(b.magic, "TRNR", 4); b.version = TRAINER_VERSION; blk_write(&b);
}
/* Arm a request the way the tab does (fields first, TR_REQ last) and serve it. */
static int request(uint8_t op, uint32_t off, uint16_t val, uint8_t width) {
  trainer_blk_t b = blk_read();
  b.req = op; b.req_off = off; b.req_val = val; b.width = width;
  blk_write(&b);
  return trainer_serve_request();
}
static int emitted_has(uint32_t code) {
  for(int i = 0; i < n_emitted; i++) if(emitted[i] == code) return 1;
  return 0;
}
#define CE(ofs) (psram + SRAM_CHEAT_EDIT_ADDR + (ofs))

int main(void) {
  psram = malloc(PSRAM_SIZE);
  if(!psram) { printf("FAIL out of memory\n"); return 1; }
  memset(psram, 0xA5, PSRAM_SIZE);          /* poison: nothing may read as leftovers */

  /* ---- trainer_stage: no session, every pin empty, version stamped ---- */
  trainer_stage();
  trainer_blk_t b = blk_read();
  CHECK(memcmp(b.magic, "TRNR", 4) != 0, "stage left a valid magic behind");
  CHECK(b.version == TRAINER_VERSION, "stage did not stamp the version");
  CHECK(b.notice == TRAINER_NOTICE_NONE, "stage left a stale notice");
  for(int i = 0; i < TRAINER_PIN_MAX; i++)
    CHECK(pin_read(i).off == TRAINER_PIN_EMPTY, "pin %d not emptied", i);
  cheat_program();
  CHECK(n_emitted == 0, "an empty pin table emitted %d patches", n_emitted);

  /* ---- trainer_invalidate: only with a live session; the pins stay ---- */
  trainer_invalidate(TRAINER_NOTICE_RESET);
  CHECK(blk_read().notice == TRAINER_NOTICE_NONE, "invalidate spoke up with no session running");
  session_open();
  b = blk_read(); b.active = 1; b.count = 1234; b.have_snap = 1; blk_write(&b);
  pin_write(0, 0x0791, 0x92, TRAINER_PIN_FROZEN);
  trainer_invalidate(TRAINER_NOTICE_RESET);
  b = blk_read();
  CHECK(memcmp(b.magic, "TRNR", 4) != 0, "invalidate left the session valid");
  CHECK(b.active == 0 && b.count == 0 && b.have_snap == 0, "invalidate left search state behind");
  CHECK(b.notice == TRAINER_NOTICE_RESET, "invalidate lost the reason");
  CHECK(pin_read(0).off == 0x0791 && pin_read(0).flags == TRAINER_PIN_FROZEN,
        "invalidate touched the pins -- they are the user's and outlive a search");
  /* ... and a dropped search still programs them: the gate is the version, not the magic */
  cheat_program();
  CHECK(n_emitted == 1 && emitted[0] == 0x7E079192, "frozen pin after a dropped search (n=%d)", n_emitted);

  /* ---- trainer_program_freezes: frozen only, 16-bit = two patches across $7EFFFF ---- */
  trainer_stage();
  session_open();
  sram_writeshort(3, SRAM_NUM_CHEATS);
  memset(psram + SRAM_CHEAT_ADDR, 0x3C, 4 * 512);          /* the game's records: must stay untouched */
  pin_write(0, 0x0791, 0x0092, TRAINER_PIN_FROZEN);
  pin_write(1, 0xFFFF, 0x04D2, TRAINER_PIN_FROZEN | TRAINER_PIN_WIDE);
  pin_write(2, 0x0100, 0x0033, 0);                          /* set, not frozen */
  pin_write(3, 0x20000, 0x0001, TRAINER_PIN_FROZEN);        /* out of WRAM: stale */
  pin_write(4, 0x1FFFF, 0x0001, TRAINER_PIN_FROZEN | TRAINER_PIN_WIDE);  /* no room for byte 2 */
  cheat_program_calls = 0;
  CHECK(request(TRAINER_REQ_APPLY, 0, 0, 1) == 0, "APPLY asked for an editor pass");
  CHECK(cheat_program_calls == 1, "APPLY did not redeploy");
  CHECK(blk_read().req == TRAINER_REQ_NONE, "the request was not consumed");
  CHECK(n_emitted == 3, "expected 3 patches, got %d", n_emitted);
  CHECK(emitted_has(0x7E079192), "8-bit freeze word missing");
  CHECK(emitted_has(0x7EFFFFD2), "16-bit low word missing");
  CHECK(emitted_has(0x7F000004), "16-bit high byte must cross into bank $7F");
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == 3, "a freeze touched NUM_CHEATS");
  for(uint32_t o = 0; o < 4 * 512; o++)
    if(psram[SRAM_CHEAT_ADDR + o] != 0x3C) { CHECK(0, "a freeze wrote cheat record byte %u", o); break; }

  /* never more than TRAINER_FREEZE_MAX frozen pins, whatever the table says */
  for(int i = 0; i < TRAINER_PIN_MAX; i++) pin_write(i, 0x10 + i, 0x40 + i, TRAINER_PIN_FROZEN);
  cheat_program();
  CHECK(n_emitted == TRAINER_FREEZE_MAX, "%d frozen pins programmed, cap is %d", n_emitted, TRAINER_FREEZE_MAX);

  /* a table from another layout version emits nothing */
  b = blk_read(); b.version = TRAINER_VERSION + 1; blk_write(&b);
  cheat_program();
  CHECK(n_emitted == 0, "a foreign-version table was programmed");
  CHECK(request(TRAINER_REQ_APPLY, 0, 0, 1) == 0 && blk_read().req == TRAINER_REQ_APPLY,
        "a foreign-version request was served");

  /* ---- SAVE: staged into the CHEAT_EDIT block as an ADD ---- */
  trainer_stage();
  session_open();
  memset(CE(0), 0xEE, CHEAT_EDIT_BYTES);
  CHECK(request(TRAINER_REQ_SAVE, 0x0DBF, 0x63, 1) == 1, "SAVE did not ask for the editor pass");
  CHECK(*CE(CHEAT_EDIT_OFS_OP) == CHEAT_EDIT_OP_ADD, "op is %d", *CE(CHEAT_EDIT_OFS_OP));
  CHECK(*CE(CHEAT_EDIT_OFS_NUMCODES) == 1, "numcodes");
  CHECK(*CE(CHEAT_EDIT_OFS_FLAGS) == 0 && sram_readshort(SRAM_CHEAT_EDIT_ADDR + CHEAT_EDIT_OFS_IDX) == 0, "flags/idx");
  CHECK(!strcmp((char *)CE(CHEAT_EDIT_OFS_NAME), "Trainer $7E0DBF = 63"), "name '%s'", (char *)CE(CHEAT_EDIT_OFS_NAME));
  CHECK(CE(CHEAT_EDIT_OFS_NAME)[CHEAT_EDIT_NAME_LEN - 1] == 0, "name tail not zero-filled");
  CHECK(!memcmp(CE(CHEAT_EDIT_OFS_CODES), "7E0DBF63\0\0", 10), "code '%.10s'", (char *)CE(CHEAT_EDIT_OFS_CODES));
  CHECK(*CE(CHEAT_EDIT_OFS_RESULT) == 0xEE, "SAVE must leave the result byte to the editor/SNES");

  CHECK(request(TRAINER_REQ_SAVE, 0xFFFF, 0x04D2, 2) == 1, "16-bit SAVE");
  CHECK(*CE(CHEAT_EDIT_OFS_NUMCODES) == 2, "16-bit numcodes");
  CHECK(!strcmp((char *)CE(CHEAT_EDIT_OFS_NAME), "Trainer $7EFFFF = 04D2"), "16-bit name '%s'", (char *)CE(CHEAT_EDIT_OFS_NAME));
  CHECK(!memcmp(CE(CHEAT_EDIT_OFS_CODES), "7EFFFFD2\0\0", 10), "low code '%.10s'", (char *)CE(CHEAT_EDIT_OFS_CODES));
  CHECK(!memcmp(CE(CHEAT_EDIT_OFS_CODES + 10), "7F000004\0\0", 10), "high code '%.10s'", (char *)CE(CHEAT_EDIT_OFS_CODES + 10));

  CHECK(request(TRAINER_REQ_SAVE, 0x20000, 1, 1) == 0, "SAVE past WRAM accepted");
  CHECK(request(TRAINER_REQ_SAVE, 0x1FFFF, 1, 2) == 0, "16-bit SAVE straddling the end accepted");
  b = blk_read(); memset(b.magic, 0, 4); blk_write(&b);
  CHECK(request(TRAINER_REQ_SAVE, 0x10, 1, 1) == 0, "SAVE served with no session");
  session_open();

  /* ---- trainer_save_done: the cheat takes over, the pin stays listed ---- */
  pin_write(0, 0x0100, 0x0033, 0);
  pin_write(1, 0x0DBF, 0x0063, TRAINER_PIN_FROZEN);
  pin_write(2, 0x0200, 0x0011, TRAINER_PIN_FROZEN);
  request(TRAINER_REQ_SAVE, 0x0DBF, 0x63, 1);
  cheat_program_calls = 0;
  trainer_save_done();
  CHECK(pin_read(1).off == 0x0DBF && pin_read(1).flags == 0, "saved pin: flags %02x (must stay, unfrozen)", pin_read(1).flags);
  CHECK(pin_read(2).flags == TRAINER_PIN_FROZEN, "save_done unfroze a different pin");
  CHECK(pin_read(0).off == 0x0100, "save_done touched an unrelated pin");
  CHECK(cheat_program_calls == 1 && n_emitted == 1 && emitted[0] == 0x7E020011, "redeploy after save_done");

  free(psram);
  if(fails) { printf("trainer_cli: %d FAILED\n", fails); return 1; }
  printf("trainer_cli: all checks passed\n");
  return 0;
}
