/* Conformance test for the MCU half of the cheat editor (src/cheatedit.c), compiled
   against the REAL source over a flat fake PSRAM.  See run_cheatedit.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "memory.h"
#include "cheat.h"
#include "cheatcode.h"
#include "cheatedit.h"

#define PSRAM_SIZE (0x1000000UL)
static uint8_t *psram;

/* ---- stubs the harness counts ---- */
static int program_calls, window_calls, apply_calls, last_window_base;
static uint8_t psram_mode;
void cheat_program(void)                 { program_calls++; }
void cheat_stage_names_window(int base)  { window_calls++; last_window_base = base; }
uint8_t cheat_rom_psram_mode(void)       { return psram_mode; }
void cheat_rom_psram_apply(void)         { apply_calls++; }

/* ---- fake PSRAM ---- */
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

/* ---- the three cheat.c helpers cheatedit.c leans on, restated (their byte
   contracts are what the checks below pin) ---- */
void cheat_sync_flags_from_mirror(void) {
  int count = sram_readshort(SRAM_NUM_CHEATS);
  if(count < 0 || count > CHEAT_RECORD_MAX) count = CHEAT_RECORD_MAX;
  for(int i = 0; i < count; i++) {
    uint8_t mirror = sram_readbyte(SRAM_CHEAT_FLAGS_ADDR + i);
    uint32_t rec = SRAM_CHEAT_ADDR + 512u * (uint32_t)i;
    uint8_t flag = sram_readbyte(rec);
    uint8_t want = (flag & ~CHEAT_FLAG_ENABLE) | (mirror & CHEAT_FLAG_ENABLE);
    if(want != flag) sram_writebyte(want, rec);
  }
}
void cheat_write_code_string(int cheat_idx, int code_idx, const char *s) {
  char buf[12]; int len = 0;
  memset(buf, 0, sizeof(buf));
  if(s) {
    while(s[len] && s[len] != '#' && len < 11) len++;
    while(len > 0 && (s[len-1] == ' ' || s[len-1] == '\t')) len--;
    if(len > 9) len = 9;
    memcpy(buf, s, len);
  }
  for(int j = len; j < 9; j++) buf[j] = ' ';
  sram_writeblock(buf, SRAM_CHEAT_CODE_STRINGS_ADDR + 512u * (uint32_t)cheat_idx + 12u * (uint32_t)code_idx, 12);
}
int cheat_read_code_string(int cheat_idx, int code_idx, char *out) {
  sram_readblock(out, SRAM_CHEAT_CODE_STRINGS_ADDR + 512u * (uint32_t)cheat_idx + 12u * (uint32_t)code_idx, 12);
  int len = 0;
  while(len < 9 && out[len] != 0) len++;
  while(len > 0 && out[len-1] == ' ') len--;
  out[len] = 0;
  return len;
}

static int fails;
#define CHECK(cond, ...) do { if(!(cond)) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while(0)

#define BLK   SRAM_CHEAT_EDIT_ADDR
#define REC(i) (SRAM_CHEAT_ADDR + 512u * (uint32_t)(i))
#define STR(i) (SRAM_CHEAT_CODE_STRINGS_ADDR + 512u * (uint32_t)(i))

/* seed a record the way cheat_yaml_load would have */
static void seed(int idx, const char *name, uint8_t flags, const char **codes, int n) {
  memset(psram + REC(idx), 0, 512);
  memset(psram + STR(idx), 0, 512);
  psram[REC(idx)] = flags;
  strncpy((char *)psram + REC(idx) + 1, name, 253);
  psram[REC(idx) + 255] = (uint8_t)n;
  for(int c = 0; c < n; c++) {
    uint32_t code = cheat_str2bin((char *)codes[c]);
    memcpy(psram + REC(idx) + 256 + 4 * c, &code, 4);
    cheat_write_code_string(idx, c, codes[c]);
  }
  psram[SRAM_CHEAT_FLAGS_ADDR + idx] = flags & CHEAT_FLAG_ENABLE;
}

static void blk_reset(void) { memset(psram + BLK, 0, 512); }
static void blk_name(const char *s) { memset(psram + BLK + CHEAT_EDIT_OFS_NAME, 0, 64); strncpy((char *)psram + BLK + CHEAT_EDIT_OFS_NAME, s, 63); }
static void blk_code(int c, const char *s) { memset(psram + BLK + CHEAT_EDIT_OFS_CODES + 10 * c, 0, 10); strncpy((char *)psram + BLK + CHEAT_EDIT_OFS_CODES + 10 * c, s, 9); }
static int serve(uint8_t op, int idx, int n, uint8_t flags, int in_game) {
  psram[BLK + CHEAT_EDIT_OFS_OP] = op;
  psram[BLK + CHEAT_EDIT_OFS_RESULT] = CHEAT_EDIT_RES_PENDING;
  sram_writeshort((uint16_t)idx, BLK + CHEAT_EDIT_OFS_IDX);
  psram[BLK + CHEAT_EDIT_OFS_FLAGS] = flags;
  psram[BLK + CHEAT_EDIT_OFS_NUMCODES] = (uint8_t)n;
  return cheat_edit_serve(in_game);
}
static uint8_t result(void) { return psram[BLK + CHEAT_EDIT_OFS_RESULT]; }
static uint32_t patch(int idx, int c) { uint32_t v; memcpy(&v, psram + REC(idx) + 256 + 4 * c, 4); return v; }
static const char *desc(int idx) { return (const char *)psram + REC(idx) + 1; }
static const char *blk_codestr(int c) { return (const char *)psram + BLK + CHEAT_EDIT_OFS_CODES + 10 * c; }


int main(void) {
  psram = malloc(PSRAM_SIZE);
  if(!psram) { printf("FAIL out of memory\n"); return 1; }
  memset(psram, 0xA5, PSRAM_SIZE);          /* poison: nothing may read as leftovers */

  /* ---- cheat_code_valid: the strict alphabet the keyboards enforce ---- */
  CHECK(cheat_code_valid("7E0DBF63"), "raw code refused");
  CHECK(cheat_code_valid("7e0dbf63"), "lower-case raw refused");
  CHECK(cheat_code_valid("DD62-3B1F"), "GG code refused");
  CHECK(!cheat_code_valid("DD623B1F0"), "9 chars without the dash accepted");
  CHECK(!cheat_code_valid("DD6-23B1F"), "dash in the wrong place accepted");
  CHECK(!cheat_code_valid("7E0DBF6"), "7 chars accepted");
  CHECK(!cheat_code_valid("7E0DBF6G"), "non-hex accepted");
  CHECK(!cheat_code_valid(""), "empty accepted");
  CHECK(cheat_str2bin((char*)"DD62-3B1F") == cheat_gg2raw(0xDD623B1F), "GG decode path drifted");
  CHECK(cheat_raw2gg(cheat_gg2raw(0x12345678)) == 0x12345678, "GG round trip");

  /* ---- a list of three cheats, the last one with blank code-string slots ---- */
  const char *c0[] = { "7E0DBF63", "DD62-3B1F" };
  const char *c1[] = { "C2BF6C00" };
  sram_writeshort(3, SRAM_NUM_CHEATS);
  sram_writeshort(0, SRAM_CHEAT_WIN_BASE_ADDR);
  seed(0, "Infinite Lives", 0x80, c0, 2);
  seed(1, "Moon Jump", 0x00, c1, 1);
  memset(psram + REC(2), 0, 512); memset(psram + STR(2), 0, 512);   /* no code strings */
  psram[REC(2)] = CHEAT_FLAG_ENABLE;
  strcpy((char *)psram + REC(2) + 1, "Trainer $7E0791 = 92");
  psram[REC(2) + 255] = 1;
  { uint32_t w = 0x7E079192; memcpy(psram + REC(2) + 256, &w, 4); }
  psram[SRAM_CHEAT_FLAGS_ADDR + 2] = 0x80;
  /* the editor must never touch the trainer's block or pin table (freezes are not records) */
  memset(psram + SRAM_TRAINER_META_ADDR, 0x5A, 64);
  memset(psram + SRAM_TRAINER_PINS_ADDR, 0x5A, 64);

  /* ---- FETCH: strings come back as typed, a string-less record is synthesised ---- */
  blk_reset();
  CHECK(serve(CHEAT_EDIT_OP_FETCH, 0, 0, 0, 0) == 0, "FETCH reported a change");
  CHECK(result() == CHEAT_EDIT_RES_OK, "FETCH result %d", result());
  CHECK(!strcmp((char *)psram + BLK + CHEAT_EDIT_OFS_NAME, "Infinite Lives"), "FETCH name");
  CHECK(psram[BLK + CHEAT_EDIT_OFS_NUMCODES] == 2, "FETCH numcodes");
  CHECK(!strcmp(blk_codestr(0), "7E0DBF63") && !strcmp(blk_codestr(1), "DD62-3B1F"), "FETCH codes '%s' '%s'", blk_codestr(0), blk_codestr(1));
  CHECK(psram[BLK + CHEAT_EDIT_OFS_OP] == CHEAT_EDIT_OP_NONE, "op not consumed");
  blk_reset();
  serve(CHEAT_EDIT_OP_FETCH, 2, 0, 0, 0);
  CHECK(!strcmp(blk_codestr(0), "7E079192"), "FETCH must synthesise the raw form for a string-less slot (got '%s')", blk_codestr(0));
  blk_reset();
  serve(CHEAT_EDIT_OP_FETCH, 3, 0, 0, 0);
  CHECK(result() == CHEAT_EDIT_RES_BADREQ, "FETCH past the end accepted");

  /* ---- ADD: a bad code changes nothing ---- */
  uint8_t snap[3 * 512], snaps[3 * 512], snapf[3];
  memcpy(snap, psram + REC(0), sizeof(snap)); memcpy(snaps, psram + STR(0), sizeof(snaps)); memcpy(snapf, psram + SRAM_CHEAT_FLAGS_ADDR, 3);
  blk_reset(); blk_name("Bad"); blk_code(0, "7E0DBF6G");
  CHECK(serve(CHEAT_EDIT_OP_ADD, 0, 1, 0, 1) == 0, "bad ADD reported a change");
  CHECK(result() == CHEAT_EDIT_RES_BADCODE, "bad ADD result %d", result());
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == 3, "bad ADD grew the count");
  CHECK(!memcmp(snap, psram + REC(0), sizeof(snap)) && !memcmp(snaps, psram + STR(0), sizeof(snaps)) && !memcmp(snapf, psram + SRAM_CHEAT_FLAGS_ADDR, 3), "bad ADD moved something");
  CHECK(program_calls == 0, "bad ADD redeployed");
  blk_reset();
  CHECK(serve(CHEAT_EDIT_OP_ADD, 0, 0, 0, 1) == 0 && result() == CHEAT_EDIT_RES_BADREQ, "ADD with 0 codes accepted");
  CHECK(serve(CHEAT_EDIT_OP_NONE, 0, 0, 0, 1) == 0 && result() == CHEAT_EDIT_RES_BADREQ, "op NONE left result %d", result());

  /* ---- ADD in-game: index 0, everything shifts up ---- */
  blk_reset(); blk_name("99 Coins"); blk_code(0, "DD62-3B1F"); blk_code(1, "7E0DBE63");
  program_calls = window_calls = 0;
  CHECK(serve(CHEAT_EDIT_OP_ADD, 0, 2, 0, 1) == 1, "ADD did not report a change");
  CHECK(result() == CHEAT_EDIT_RES_OK, "ADD result %d", result());
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == 4, "count after ADD");
  CHECK(psram[REC(0)] == CHEAT_FLAG_ENABLE, "new cheat must start enabled (flag %02x)", psram[REC(0)]);
  CHECK(!strcmp(desc(0), "99 Coins"), "new name '%s'", desc(0));
  CHECK(psram[REC(0) + 1 + 63] == 0 && psram[REC(0) + 254] == 0, "description tail not zeroed");
  CHECK(psram[REC(0) + 255] == 2, "new numpatches");
  CHECK(patch(0, 0) == cheat_gg2raw(0xDD623B1F), "GG code not decoded into the record");
  CHECK(patch(0, 1) == 0x7E0DBE63, "raw code word %08X", patch(0, 1));
  CHECK(patch(0, 2) == 0 && psram[REC(0) + 416] == 0 && psram[REC(0) + 511] == 0, "unused patches / tail not zeroed");
  { char s[12]; CHECK(cheat_read_code_string(0, 0, s) == 9 && !strcmp(s, "DD62-3B1F"), "new code string 0 '%s'", s);
                CHECK(cheat_read_code_string(0, 2, s) == 0, "unused string slot not empty"); }
  CHECK(psram[SRAM_CHEAT_FLAGS_ADDR] == 0x80, "new mirror byte");
  /* the old three moved up intact, strings and mirror included */
  CHECK(!memcmp(snap, psram + REC(1), sizeof(snap)), "records did not shift up byte-for-byte");
  CHECK(!memcmp(snaps, psram + STR(1), sizeof(snaps)), "code strings did not shift up");
  CHECK(!memcmp(snapf, psram + SRAM_CHEAT_FLAGS_ADDR + 1, 3), "flag mirror did not shift up");
  CHECK(!strcmp(desc(1), "Infinite Lives") && !strcmp(desc(3), "Trainer $7E0791 = 92"), "shifted names");
  CHECK(program_calls == 1 && window_calls == 1 && last_window_base == 0, "in-game ADD must redeploy + restage the window");

  /* ---- ADD in the menu ---- */
  blk_reset(); blk_name(""); blk_code(0, "7E0DC2FF");
  program_calls = 0;
  serve(CHEAT_EDIT_OP_ADD, 0, 1, 0, 0);
  CHECK(result() == CHEAT_EDIT_RES_OK && sram_readshort(SRAM_NUM_CHEATS) == 5, "menu ADD");
  CHECK(desc(0)[0] == 0, "empty name must stay empty (the UIs draw the placeholder)");
  CHECK(program_calls == 0, "menu-mode ADD redeployed (nothing is running)");
  /* list is now: [0] "", [1] 99 Coins, [2] Infinite Lives, [3] Moon Jump, [4] Trainer */

  /* ---- REPLACE without the name flag keeps a long description and the enable state ---- */
  char longname[200]; memset(longname, 'x', 199); longname[199] = 0;
  strcpy((char *)psram + REC(3) + 1, longname);
  psram[REC(3)] = 0;                                  /* disabled */
  psram[SRAM_CHEAT_FLAGS_ADDR + 3] = 0;
  blk_reset(); blk_name("short"); blk_code(0, "C2BF6C01");
  CHECK(serve(CHEAT_EDIT_OP_REPLACE, 3, 1, 0, 0) == 1 && result() == CHEAT_EDIT_RES_OK, "REPLACE");
  CHECK(!strcmp(desc(3), longname), "REPLACE without bit0 rewrote the description");
  CHECK(patch(3, 0) == 0xC2BF6C01 && psram[REC(3) + 255] == 1, "REPLACE codes");
  CHECK(psram[REC(3)] == 0, "REPLACE must keep the enable state (flag %02x)", psram[REC(3)]);
  /* REPLACE with the name flag, shrinking 2 codes -> 1 zeroes the leftovers */
  blk_reset(); blk_name("Lives"); blk_code(0, "7E0DBF09");
  psram[SRAM_CHEAT_FLAGS_ADDR + 2] = 0x80;
  serve(CHEAT_EDIT_OP_REPLACE, 2, 1, CHEAT_EDIT_FLAG_NAME, 1);
  CHECK(result() == CHEAT_EDIT_RES_OK && !strcmp(desc(2), "Lives"), "REPLACE with bit0 '%s'", desc(2));
  CHECK(patch(2, 1) == 0, "old second patch survived");
  { char s[12]; CHECK(cheat_read_code_string(2, 1, s) == 0, "old second code string survived"); }
  CHECK(psram[REC(2)] == 0x80, "REPLACE lost the enable bit");
  /* the in-game mirror is authoritative: toggled OFF in the mirror, REPLACE keeps OFF */
  psram[SRAM_CHEAT_FLAGS_ADDR + 2] = 0;
  blk_reset(); blk_code(0, "7E0DBF09");
  serve(CHEAT_EDIT_OP_REPLACE, 2, 1, 0, 1);
  CHECK(psram[REC(2)] == 0, "REPLACE ignored the live mirror toggle");
  psram[SRAM_CHEAT_FLAGS_ADDR + 2] = 0x80;

  /* ---- PSRAM-patch mode: an enabled ROM record is un-applied before its codes change ---- */
  psram_mode = 1; apply_calls = 0;
  blk_reset(); blk_code(0, "C2BF6C02");
  psram[REC(3)] = 0x80; psram[SRAM_CHEAT_FLAGS_ADDR + 3] = 0x80;
  serve(CHEAT_EDIT_OP_REPLACE, 3, 1, 0, 1);
  CHECK(apply_calls >= 2, "REPLACE in PSRAM-patch mode must restore the image first (apply calls %d)", apply_calls);
  CHECK(psram[REC(3)] == 0x80, "REPLACE in PSRAM-patch mode lost the enable bit");
  psram_mode = 0;

  /* ---- DELETE the middle one in-game: shift down ---- */
  /* list: [0] "", [1] 99 Coins, [2] Lives, [3] xxx.., [4] Trainer */
  memcpy(snap, psram + REC(4), 512);
  blk_reset();
  CHECK(serve(CHEAT_EDIT_OP_DELETE, 3, 0, 0, 1) == 1 && result() == CHEAT_EDIT_RES_OK, "DELETE");
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == 4, "count after DELETE");
  CHECK(!memcmp(snap, psram + REC(3), 512), "record above the hole did not move down");
  CHECK(!strcmp(desc(2), "Lives") && !strcmp(desc(3), "Trainer $7E0791 = 92"), "names after DELETE");
  CHECK(psram[SRAM_CHEAT_FLAGS_ADDR + 4] == 0, "stale mirror byte past the new count");
  /* DELETE the first one from the menu */
  blk_reset();
  serve(CHEAT_EDIT_OP_DELETE, 0, 0, 0, 0);
  CHECK(result() == CHEAT_EDIT_RES_OK && sram_readshort(SRAM_NUM_CHEATS) == 3 && !strcmp(desc(0), "99 Coins"), "menu DELETE of index 0");
  blk_reset();
  serve(CHEAT_EDIT_OP_DELETE, 3, 0, 0, 0);
  CHECK(result() == CHEAT_EDIT_RES_BADREQ && sram_readshort(SRAM_NUM_CHEATS) == 3, "DELETE past the end");

  /* ---- a full table refuses ADD ---- */
  sram_writeshort(CHEAT_RECORD_MAX, SRAM_NUM_CHEATS);
  blk_reset(); blk_code(0, "7E0DBF63");
  CHECK(serve(CHEAT_EDIT_OP_ADD, 0, 1, 0, 0) == 0 && result() == CHEAT_EDIT_RES_FULL, "ADD at 512 accepted");
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == CHEAT_RECORD_MAX, "count changed on a refused ADD");

  /* ---- ADD into an EMPTY list ---- */
  sram_writeshort(0, SRAM_NUM_CHEATS);
  blk_reset(); blk_name("First"); blk_code(0, "7E0DBF63");
  window_calls = 0;
  CHECK(serve(CHEAT_EDIT_OP_ADD, 0, 1, 0, 1) == 1 && result() == CHEAT_EDIT_RES_OK, "ADD into empty");
  CHECK(sram_readshort(SRAM_NUM_CHEATS) == 1 && !strcmp(desc(0), "First"), "empty-list ADD");
  CHECK(window_calls == 1 && last_window_base == 0, "window not restaged at base 0");

  for(int i = 0; i < 64; i++)
    if(psram[SRAM_TRAINER_META_ADDR + i] != 0x5A || psram[SRAM_TRAINER_PINS_ADDR + i] != 0x5A) {
      CHECK(0, "the editor wrote into the trainer block/pins (+%d)", i); break;
    }

  free(psram);
  if(fails) { printf("cheatedit_cli: %d FAILED\n", fails); return 1; }
  printf("cheatedit_cli: all checks passed\n");
  return 0;
}
