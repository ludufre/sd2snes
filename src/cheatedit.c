/* sd2snes fork -- cheat EDITOR: MCU side. See cheatedit.h. */

#include "config.h"
#include "uart.h"
#include "memory.h"
#include "cheat.h"
#include "cheatcode.h"
#include "trainer.h"
#include "cheatedit.h"

#include <string.h>

#define NO_INLINE __attribute__((noinline))

/* Record slot geometry (cheat.h/cheat.c): 416-byte struct in a 512-byte stride, the
   96-byte tail is the PSRAM-patch bookkeeping (original byte + applied flag / code). */
#define CE_REC_STRIDE      (512)
#define CE_REC_DESC_OFS    (1)
#define CE_REC_DESC_LEN    (254)
#define CE_REC_NPATCH_OFS  (255)
#define CE_REC_PATCH_OFS   (256)
#define CE_REC_PATCH_LEN   (CHEAT_NUM_CODES_PER_CHEAT * 4)
#define CE_STR_STRIDE      (512)
#define CE_STR_SLOT        (12)

#define CE_BLK             (SRAM_CHEAT_EDIT_ADDR)
#define CE_REC(i)          (SRAM_CHEAT_ADDR + (uint32_t)CE_REC_STRIDE * (uint32_t)(i))
#define CE_STR(i)          (SRAM_CHEAT_CODE_STRINGS_ADDR + (uint32_t)CE_STR_STRIDE * (uint32_t)(i))
#define CE_CODE(c)         (CE_BLK + CHEAT_EDIT_OFS_CODES + (uint32_t)CHEAT_EDIT_CODE_LEN * (uint32_t)(c))

static int ce_count(void) {
  int c = sram_readshort(SRAM_NUM_CHEATS);
  if(c < 0 || c > CHEAT_RECORD_MAX) c = 0;
  return c;
}

/* One 512-byte slot, 128 bytes at a time: the stack is the scarce resource here
   (LPC175x, a few KB of headroom), the SPI is not. */
static void NO_INLINE ce_copy_slot(uint32_t dst, uint32_t src) {
  uint8_t buf[128];
  for(int o = 0; o < CE_REC_STRIDE; o += (int)sizeof(buf)) {
    sram_readblock(buf, src + (uint32_t)o, sizeof(buf));
    sram_writeblock(buf, dst + (uint32_t)o, sizeof(buf));
  }
}

/* Move cheat `src` into slot `dst`: record, code strings and flag-mirror byte. The
   three arrays index the same cheat, so they never move separately. */
static void ce_move(int dst, int src) {
  ce_copy_slot(CE_REC(dst), CE_REC(src));
  ce_copy_slot(CE_STR(dst), CE_STR(src));
  sram_writebyte(sram_readbyte(SRAM_CHEAT_FLAGS_ADDR + (uint32_t)src),
                 SRAM_CHEAT_FLAGS_ADDR + (uint32_t)dst);
}

/* The trainer's freeze slots hold ABSOLUTE record indices (TR_FZ_IDX). Free the
   slot that pointed at `freed` (-1 = none) and shift every index >= `from` by
   `delta`.  Not gated on the block magic on purpose: trainer_invalidate() drops the
   session but keeps fz_idx meaningful (the freezes are ordinary cheats). */
static void NO_INLINE ce_trainer_rebase(int freed, int from, int delta) {
  trainer_blk_t blk;
  int dirty = 0;
  sram_readblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
  for(int i = 0; i < TRAINER_FREEZE_MAX; i++) {
    uint16_t v = blk.fz_idx[i];
    if(v == 0xFFFF) continue;
    if((int)v == freed) {
      blk.fz_idx[i] = 0xFFFF;
      blk.fz_off[i] = 0;
      dirty = 1;
    } else if((int)v >= from) {
      blk.fz_idx[i] = (uint16_t)((int)v + delta);
      dirty = 1;
    }
  }
  if(dirty) sram_writeblock(&blk, SRAM_TRAINER_META_ADDR, sizeof(blk));
}

/* PSRAM-patch ROM mode only (Mk.II SA-1/GSU/CX4): a record's ROM codes may be
   applied INTO the image, with the original bytes in the record's tail.  Before the
   record is rewritten or moved, put the image back.  Returns the record's flag byte
   (so REPLACE can put the enable bit back once the new codes are in). */
static uint8_t ce_unapply(int idx) {
  uint8_t flag = sram_readbyte(CE_REC(idx));
  if(cheat_rom_psram_mode() && (flag & CHEAT_FLAG_ENABLE)) {
    sram_writebyte(flag & (uint8_t)~CHEAT_FLAG_ENABLE, CE_REC(idx));
    cheat_rom_psram_apply();
  }
  return flag;
}

/* Every code string in the block must parse strictly; the lenient cheat_str2bin
   would turn a typo into a silent zero code. */
static int ce_codes_valid(int n) {
  char s[CHEAT_EDIT_CODE_LEN];
  for(int c = 0; c < n; c++) {
    sram_readblock(s, CE_CODE(c), sizeof(s));
    s[sizeof(s) - 1] = 0;
    if(!cheat_code_valid(s)) return 0;
  }
  return 1;
}

/* block name -> record description (zero-filled to its full 254 bytes) */
static void NO_INLINE ce_write_name(int idx) {
  char name[CHEAT_EDIT_NAME_LEN];
  sram_readblock(name, CE_BLK + CHEAT_EDIT_OFS_NAME, sizeof(name));
  name[sizeof(name) - 1] = 0;
  sram_writeblock(name, CE_REC(idx) + CE_REC_DESC_OFS, sizeof(name));
  sram_memset(CE_REC(idx) + CE_REC_DESC_OFS + sizeof(name), CE_REC_DESC_LEN - sizeof(name), 0);
}

/* block codes -> record patches + display strings; unused slots zeroed, the
   PSRAM-patch tail cleared (nothing of the new codes is applied yet). */
static void NO_INLINE ce_write_codes(int idx, int n) {
  char s[CHEAT_EDIT_CODE_LEN];
  for(int c = 0; c < n; c++) {
    uint32_t code;
    sram_readblock(s, CE_CODE(c), sizeof(s));
    s[sizeof(s) - 1] = 0;
    code = cheat_str2bin(s);               /* LE in PSRAM == cheat_patch_record_t.code */
    sram_writeblock(&code, CE_REC(idx) + CE_REC_PATCH_OFS + 4u * (uint32_t)c, 4);
    cheat_write_code_string(idx, c, s);
  }
  sram_writebyte((uint8_t)n, CE_REC(idx) + CE_REC_NPATCH_OFS);
  if(n < CHEAT_NUM_CODES_PER_CHEAT) {
    sram_memset(CE_REC(idx) + CE_REC_PATCH_OFS + 4u * (uint32_t)n,
                (uint32_t)(CHEAT_NUM_CODES_PER_CHEAT - n) * 4u, 0);
    sram_memset(CE_STR(idx) + (uint32_t)CE_STR_SLOT * (uint32_t)n,
                (uint32_t)(CHEAT_NUM_CODES_PER_CHEAT - n) * CE_STR_SLOT, 0);
  }
  sram_memset(CE_REC(idx) + CE_REC_PATCH_OFS + CE_REC_PATCH_LEN,
              CE_REC_STRIDE - (CE_REC_PATCH_OFS + CE_REC_PATCH_LEN), 0);
}

static const char ce_hex[] = "0123456789ABCDEF";

/* record idx -> block (name, numcodes, code strings) */
static void NO_INLINE ce_fetch(int idx) {
  char s[CE_STR_SLOT];
  uint8_t n;
  {
    char name[CHEAT_EDIT_NAME_LEN];
    sram_readblock(name, CE_REC(idx) + CE_REC_DESC_OFS, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    sram_writeblock(name, CE_BLK + CHEAT_EDIT_OFS_NAME, sizeof(name));
  }
  n = sram_readbyte(CE_REC(idx) + CE_REC_NPATCH_OFS);
  if(n > CHEAT_NUM_CODES_PER_CHEAT) n = CHEAT_NUM_CODES_PER_CHEAT;
  for(int c = 0; c < n; c++) {
    if(cheat_read_code_string(idx, c, s) == 0) {
      /* never populated (a trainer record): show the raw form, like the writer */
      uint32_t code;
      sram_readblock(&code, CE_REC(idx) + CE_REC_PATCH_OFS + 4u * (uint32_t)c, 4);
      for(int d = 0; d < 8; d++) s[d] = ce_hex[(code >> (28 - 4 * d)) & 0xf];
      s[8] = 0;
    }
    s[CHEAT_EDIT_CODE_LEN - 1] = 0;
    sram_writeblock(s, CE_CODE(c), CHEAT_EDIT_CODE_LEN);
  }
  sram_writebyte(n, CE_BLK + CHEAT_EDIT_OFS_NUMCODES);
  sram_writebyte(0, CE_BLK + CHEAT_EDIT_OFS_FLAGS);
}

int cheat_edit_serve(int in_game) {
  uint8_t op    = sram_readbyte(CE_BLK + CHEAT_EDIT_OFS_OP);
  uint8_t flags = sram_readbyte(CE_BLK + CHEAT_EDIT_OFS_FLAGS);
  int idx = sram_readshort(CE_BLK + CHEAT_EDIT_OFS_IDX);
  int n   = sram_readbyte(CE_BLK + CHEAT_EDIT_OFS_NUMCODES);
  int count = ce_count();
  int changed = 0;
  uint8_t res = CHEAT_EDIT_RES_BADREQ;

  printf("cheat_edit: op %d idx %d n %d (%d cheats)\n", op, idx, n, count);

  /* In-game the toggles live only in the $FF0500 mirror until the overlay closes;
     fold them in first so the file we are about to write reflects what the user
     sees.  In the menu the two are already in sync (CMD_TOGGLE_CHT) = no-op. */
  cheat_sync_flags_from_mirror();

  switch(op) {
    case CHEAT_EDIT_OP_FETCH:
      if(idx < 0 || idx >= count) break;
      ce_fetch(idx);
      res = CHEAT_EDIT_RES_OK;
      break;

    case CHEAT_EDIT_OP_ADD:
      if(n < 1 || n > CHEAT_EDIT_MAX_CODES) break;
      if(!ce_codes_valid(n)) { res = CHEAT_EDIT_RES_BADCODE; break; }
      if(count >= CHEAT_RECORD_MAX) { res = CHEAT_EDIT_RES_FULL; break; }
      /* top of the file == index 0: everything else moves up one slot, top down */
      for(int i = count - 1; i >= 0; i--) ce_move(i + 1, i);
      if(in_game) ce_trainer_rebase(-1, 0, +1);
      sram_writebyte(CHEAT_FLAG_ENABLE, CE_REC(0));
      ce_write_name(0);
      ce_write_codes(0, n);
      sram_writebyte(CHEAT_FLAG_ENABLE, SRAM_CHEAT_FLAGS_ADDR);
      sram_writeshort((uint16_t)(count + 1), SRAM_NUM_CHEATS);
      res = CHEAT_EDIT_RES_OK;
      changed = 1;
      break;

    case CHEAT_EDIT_OP_REPLACE: {
      uint8_t f;
      if(idx < 0 || idx >= count || n < 1 || n > CHEAT_EDIT_MAX_CODES) break;
      if(!ce_codes_valid(n)) { res = CHEAT_EDIT_RES_BADCODE; break; }
      f = ce_unapply(idx) & (uint8_t)~CHEAT_FLAG_RUNTIME;   /* edited by the user: it is theirs now */
      if(flags & CHEAT_EDIT_FLAG_NAME) ce_write_name(idx);
      ce_write_codes(idx, n);
      sram_writebyte(f, CE_REC(idx));
      res = CHEAT_EDIT_RES_OK;
      changed = 1;
      break;
    }

    case CHEAT_EDIT_OP_DELETE:
      if(idx < 0 || idx >= count) break;
      ce_unapply(idx);
      for(int i = idx + 1; i < count; i++) ce_move(i - 1, i);
      sram_writebyte(0, SRAM_CHEAT_FLAGS_ADDR + (uint32_t)(count - 1));
      if(in_game) ce_trainer_rebase(idx, idx + 1, -1);
      sram_writeshort((uint16_t)(count - 1), SRAM_NUM_CHEATS);
      res = CHEAT_EDIT_RES_OK;
      changed = 1;
      break;

    default:
      break;
  }

  if(changed) {
    /* the in-game list reads names from the resident 64-name window: restage it at
       the base the list is showing (slots past the new count come out empty) */
    int base = sram_readshort(SRAM_CHEAT_WIN_BASE_ADDR);
    if(base < 0 || base >= ce_count()) base = 0;
    cheat_stage_names_window(base);
    if(in_game) {
      cheat_program();
      cheat_rom_psram_apply();
    }
  }

  sram_writebyte(res, CE_BLK + CHEAT_EDIT_OFS_RESULT);
  sram_writebyte(CHEAT_EDIT_OP_NONE, CE_BLK + CHEAT_EDIT_OFS_OP);
  return changed;
}
