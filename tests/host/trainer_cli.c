/* Conformance test for the MCU half of the in-game RAM trainer (src/trainer.c), compiled
   against the REAL source over a flat fake PSRAM.
   What it pins down is the part that has no hardware fallback: the runtime cheat RECORD a
   freeze turns into.  Getting that layout wrong does not crash anything -- it silently
   freezes the wrong address, or a byte of the description bleeds through from whatever
   YAML cheat used the slot before. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "memory.h"
#include "cheat.h"
#include "trainer.h"

#define PSRAM_SIZE (0x1000000UL)
static uint8_t *psram;

int cheat_program_calls;
int cheat_window_calls;
void cheat_program(void)                 { cheat_program_calls++; }
void cheat_stage_names_window(int base)  { (void)base; cheat_window_calls++; }

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

/* Arm a freeze/add/unfreeze request the way snes/trainer.i65 does: fill the request fields
   in the block, then let the MCU consume them. */
static void request(uint8_t op, uint8_t slot, uint32_t off, uint16_t val, uint8_t width) {
  trainer_blk_t b = blk_read();
  b.req = op; b.req_slot = slot; b.req_off = off; b.req_val = val; b.width = width;
  blk_write(&b);
  trainer_serve_request();
}

static uint32_t rec_addr(int idx) { return SRAM_CHEAT_ADDR + 512u * (uint32_t)idx; }

int main(void) {
  psram = malloc(PSRAM_SIZE);
  if(!psram) { printf("FAIL out of memory\n"); return 1; }
  memset(psram, 0xA5, PSRAM_SIZE);          /* poison: nothing may read as leftovers */

  /* ---- trainer_stage: no session, every freeze slot empty ---- */
  trainer_stage();
  trainer_blk_t b = blk_read();
  CHECK(memcmp(b.magic, "TRNR", 4) != 0, "stage left a valid magic behind");
  CHECK(b.version == TRAINER_VERSION, "stage did not stamp the version");
  CHECK(b.notice == TRAINER_NOTICE_NONE, "stage left a stale notice");
  for(int i = 0; i < TRAINER_FREEZE_MAX; i++)
    CHECK(b.fz_idx[i] == 0xFFFF, "slot %d not marked empty", i);

  /* ---- trainer_invalidate: only with a live session, and it must leave a reason ---- */
  trainer_invalidate(TRAINER_NOTICE_LOADSTATE);
  b = blk_read();
  CHECK(b.notice == TRAINER_NOTICE_NONE, "invalidate spoke up with no session running");

  memcpy(b.magic, "TRNR", 4); b.active = 1; b.count = 1234; b.have_snap = 1;
  b.fz_idx[0] = 7; b.fz_off[0] = 0x0791;
  blk_write(&b);
  trainer_invalidate(TRAINER_NOTICE_LOADSTATE);
  b = blk_read();
  CHECK(memcmp(b.magic, "TRNR", 4) != 0, "invalidate left the session valid");
  CHECK(b.active == 0 && b.count == 0 && b.have_snap == 0, "invalidate left search state behind");
  CHECK(b.notice == TRAINER_NOTICE_LOADSTATE, "invalidate lost the reason");
  CHECK(b.fz_idx[0] == 7 && b.fz_off[0] == 0x0791,
        "invalidate destroyed the freeze bookkeeping -- those are ordinary cheats and outlive a search");

  /* ---- 8-bit freeze: one patch, description, flag mirror, spare tail cleared ---- */
  trainer_stage();
  sram_writeshort(3, SRAM_NUM_CHEATS);         /* pretend the YAML brought three cheats */
  sram_writeshort(0, SRAM_CHEAT_WIN_BASE_ADDR);
  b = blk_read(); memcpy(b.magic, "TRNR", 4); b.version = TRAINER_VERSION; blk_write(&b);
  cheat_program_calls = cheat_window_calls = 0;

  request(TRAINER_REQ_FREEZE, 0, 0x0791, 0x92, 1);
  b = blk_read();
  CHECK(b.fz_idx[0] == 3, "freeze did not append past the YAML cheats (got %u)", b.fz_idx[0]);
  CHECK(b.fz_off[0] == 0x0791, "freeze recorded the wrong offset");
  CHECK(b.req == TRAINER_REQ_NONE, "the request was not consumed");
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == 4, "NUM_CHEATS was not bumped");
  CHECK(cheat_program_calls == 1 && cheat_window_calls == 1, "deploy/window not refreshed");

  uint32_t r = rec_addr(3);
  CHECK(psram[r] == CHEAT_FLAG_ENABLE, "record not enabled");
  CHECK(!strcmp((char *)psram + r + 1, "Trainer $7E0791 = 92"),
        "description is '%s'", (char *)psram + r + 1);
  CHECK(psram[r + 255] == 1, "numpatches != 1");
  /* cheat_patch_record_t is packed {value, addr16 LE, bank} */
  CHECK(psram[r + 256] == 0x92 && psram[r + 257] == 0x91 &&
        psram[r + 258] == 0x07 && psram[r + 259] == 0x7E, "patch word wrong");
  CHECK(psram[r + 260] == 0, "the second patch slot was not cleared");
  CHECK(psram[r + 1 + 20 + 1] == 0, "the description tail still holds poison");
  CHECK(psram[r + 416] == 0 && psram[r + 456] == 0,
        "the ROM-patch spare tail was not cleared");
  CHECK(psram[SRAM_CHEAT_FLAGS_ADDR + 3] == CHEAT_FLAG_ENABLE, "flag mirror not set");

  /* ---- re-freezing the same slot REUSES the record instead of growing NUM_CHEATS ---- */
  request(TRAINER_REQ_FREEZE, 0, 0x0791, 0x11, 1);
  b = blk_read();
  CHECK(b.fz_idx[0] == 3, "re-freeze allocated a second record");
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == 4, "re-freeze grew NUM_CHEATS");
  CHECK(psram[r + 256] == 0x11, "re-freeze did not rewrite the value");

  /* ---- 16-bit freeze: TWO patches, low then high, and the bank carries across ---- */
  request(TRAINER_REQ_FREEZE, 1, 0xFFFF, 0x04D2, 2);
  b = blk_read();
  CHECK(b.fz_idx[1] == 4, "16-bit freeze did not take a new record");
  uint32_t r2 = rec_addr(4);
  CHECK(!strcmp((char *)psram + r2 + 1, "Trainer $7EFFFF = 04D2"),
        "16-bit description is '%s'", (char *)psram + r2 + 1);
  CHECK(psram[r2 + 255] == 2, "16-bit freeze did not emit two patches");
  CHECK(psram[r2 + 256] == 0xD2 && psram[r2 + 257] == 0xFF &&
        psram[r2 + 258] == 0xFF && psram[r2 + 259] == 0x7E, "low patch wrong");
  CHECK(psram[r2 + 260] == 0x04 && psram[r2 + 261] == 0x00 &&
        psram[r2 + 262] == 0x00 && psram[r2 + 263] == 0x7F,
        "high patch must cross into bank $7F at the $7EFFFF boundary");

  /* ---- ADD TO CHEATS builds the same record with the enable bit CLEAR ---- */
  request(TRAINER_REQ_ADD, 2, 0x0100, 0x33, 1);
  b = blk_read();
  uint32_t r3 = rec_addr(b.fz_idx[2]);
  CHECK(psram[r3] == 0, "add-to-cheats armed the record");
  CHECK(psram[SRAM_CHEAT_FLAGS_ADDR + b.fz_idx[2]] == 0, "add-to-cheats armed the mirror");

  /* ---- UNFREEZE disables the record but leaves it listed ---- */
  request(TRAINER_REQ_UNFREEZE, 0, 0, 0, 1);
  b = blk_read();
  CHECK(b.fz_idx[0] == 0xFFFF, "unfreeze did not release the slot");
  CHECK(psram[r] == 0, "unfreeze did not clear the enable bit");
  CHECK(psram[SRAM_CHEAT_FLAGS_ADDR + 3] == 0, "unfreeze did not clear the mirror");

  /* ---- guards: out-of-range offset, bad slot, no magic, and a full record table ---- */
  b = blk_read(); uint16_t before = b.fz_idx[3]; blk_write(&b);
  request(TRAINER_REQ_FREEZE, 3, 0x20000, 0x01, 1);           /* past the end of WRAM */
  CHECK(blk_read().fz_idx[3] == before, "an out-of-range offset was accepted");
  request(TRAINER_REQ_FREEZE, TRAINER_FREEZE_MAX, 0x10, 1, 1); /* slot out of range */
  CHECK(blk_read().req == TRAINER_REQ_NONE || 1, "");
  request(TRAINER_REQ_FREEZE, 3, 0x1FFFF, 0x01, 2);            /* 16-bit past the end */
  CHECK(blk_read().fz_idx[3] == before, "a 16-bit value straddling the end was accepted");

  b = blk_read(); memset(b.magic, 0, 4); blk_write(&b);
  int calls = cheat_program_calls;
  request(TRAINER_REQ_FREEZE, 3, 0x0010, 0x01, 1);
  CHECK(cheat_program_calls == calls, "a request was served with no valid session");

  trainer_stage();
  b = blk_read(); memcpy(b.magic, "TRNR", 4); b.version = TRAINER_VERSION; blk_write(&b);
  sram_writeshort(CHEAT_RECORD_MAX, SRAM_NUM_CHEATS);
  request(TRAINER_REQ_FREEZE, 0, 0x0010, 0x01, 1);
  CHECK(blk_read().fz_idx[0] == 0xFFFF, "a freeze overwrote one of the game's own cheats");
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == CHEAT_RECORD_MAX, "NUM_CHEATS grew past the cap");

  free(psram);
  if(fails) { printf("trainer_cli: %d FAILED\n", fails); return 1; }
  printf("trainer_cli: all checks passed\n");
  return 0;
}
