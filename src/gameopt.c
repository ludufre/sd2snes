/* sd2snes - SD card based universal cartridge for the SNES

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   gameopt.c: per-game settings (the <stem>.yml in saves/, GAMEOPT_BLK, CFG while a game
   runs).  See gameopt.h.
*/

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "config.h"
#include "uart.h"
#include "cfg.h"
#include "ff.h"
#include "fileops.h"
#include "fpga_spi.h"
#include "memory.h"
#include "smc.h"
#include "util.h"
#include "gameopt.h"
#include "scratch.h"

extern cfg_t CFG;
extern snes_romprops_t romprops;
extern char current_filename[];

/* THE LIST.  One line per option a game can override: the cfg_t field (its config.yml key
   comes with it), GAMEOPT_F_* and the games it applies to (AP_*).  The index is the
   option's number in GAMEOPT_BLK; where the menus show it (and what they call it) is up to
   the Settings row whose value is that field.  GAMEOPT_F_LIVE only for what can change
   under a running game without harm: the in-game menu greys and refuses everything else.
   A field must be a byte option of cfg_items[] (true/false or a number): the row is left
   out otherwise.

   AP_*: the consoles the option means something for, and at most one thing the game has to
   have (a chip, a .msu).  An option that does not apply is published as GAMEOPT_V_NA, so
   the menus leave its row out and a group with no row left goes too.  What each console
   runs decides it: the SGB core runs the SGB BIOS (a SNES program, with the hook, the
   $2100 patcher and the reset hook of cheat.v), while the GBC, SMS and A26 cores run our
   players, under which no NMI hook ever runs (they match the reset/menu gestures
   themselves, unconditionally) and the overlay and savestates do not exist; the NES core
   has no cheat.v at all.  A Game Boy game shows what either of its two cores uses (a
   per-game GbcMode can move it): GAMEOPT_F_SGB / _F_GBC say which core an option is for,
   and the menu writes the core after the label (gox_core in snes/gameopt.a65).

   Left at AP_SNES on purpose: the controller-2 hooks (the patched stub calls hx_pad in
   bank $C1, which only the SNES cores map), the in-game menu and savestates (the handler
   is installed on the SNES cores only, savestate_program), the region override (no
   non-SNES program reads $213F bit 4 except the GBC player, which uses it only to keep a
   NTSC-timed IRQ off on PAL consoles) and bus timing compat (the base core's featurebit
   13). */
#define AP_SNES   0x01
#define AP_GB     0x02                       /* .gb / .gbc: SGB core, or the GBC one */
#define AP_A26    0x04
#define AP_NES    0x08
#define AP_SMS    0x10
#define AP_ALL    0x1f
#define AP_CX4    (1 << 5)                   /* ...and the game needs: */
#define AP_GSU    (2 << 5)
#define AP_CC     (3 << 5)                   /* a Competition Cart */
#define AP_MSU    (4 << 5)                   /* a <stem>.msu */
#define GO_CHIP(k) (0x80 << (k))             /* the game's class: AP_* type | GO_CHIP(id) */
/* the $2100 patcher (featurebit 6): base and chip cores, SGB, SMS, A26 -- not the GBC core
   (the player owns $2100) nor the NES one */
#define AP_2100   (AP_SNES | AP_GB | AP_SMS | AP_A26)
#ifdef CONFIG_MK2
#define AP_LIMIT  AP_SNES                    /* the Mk.II SGB core has no brightness limit */
#else
#define AP_LIMIT  AP_2100
#endif
#define F_LIVE    GAMEOPT_F_LIVE
#define F_SGB     GAMEOPT_F_SGB
#define F_GBC     GAMEOPT_F_GBC              /* unused: the GBC rows name the GBC already */
#define GO(field, fl, ap) { offsetof(cfg_t, field), fl, ap }
static const struct { uint16_t off; uint8_t fl, ap; } gameopt_list[] = {
  GO(onechip_transient_fixes, F_LIVE | F_SGB, AP_2100),   /* featurebit 6, rewritten live */
  GO(brightness_limit,        F_LIVE | F_SGB, AP_LIMIT),  /* featurebits 7-10, the same */
  GO(enable_autosave,         F_LIVE, AP_SNES | AP_GB | AP_NES),  /* SMS/A26: no save RAM */
  GO(enable_autosave_msu1,    F_LIVE, AP_SNES | AP_GB | AP_MSU),  /* msu1.c gates the GB's too */
  GO(reset_to_menu,           F_LIVE, AP_ALL),            /* read when reset is pressed */
  GO(msu_volume_boost,        F_LIVE, AP_SNES | AP_MSU),  /* the DAC's gain, at once */
  GO(enable_ingame_hook,      F_SGB, AP_SNES | AP_GB),    /* cheat_program arms it on SGB too */
  GO(enable_ingame_buttons,   F_SGB, AP_SNES | AP_GB),    /* the gestures of cheat.v */
  GO(enable_ingame_pad2,      0, AP_SNES),
  GO(enable_cheat_overlay,    0, AP_SNES),
  GO(enable_hook_holdoff,     F_SGB, AP_SNES | AP_GB),
  GO(r213f_override,          0, AP_SNES),
  GO(bus_compat,              0, AP_SNES),
  GO(reset_patch,             F_SGB, AP_2100),            /* cheat.v's reset hook; dead on the GBC core */
  GO(enable_ingame_savestate, 0, AP_SNES),
  GO(enable_savestate_slots,  0, AP_SNES),
  GO(cx4_speed,               0, AP_SNES | AP_CX4),
  GO(gsu_speed,               0, AP_SNES | AP_GSU),
  GO(a26_video_width,         GAMEOPT_F_MK3, AP_A26),
  GO(cc_time_limit,           0, AP_SNES | AP_CC),
  GO(sgb_enable_ingame_hook,  F_SGB, AP_GB),
  GO(sgb_enable_state,        F_SGB, AP_GB),
  GO(sgb_enh_override,        F_SGB, AP_GB),
  GO(sgb_spr_increase,        F_SGB, AP_GB),              /* config.yml has it on the Mk.III only */
  GO(sgb_clock_fix,           F_SGB, AP_GB),
  GO(sgb_volume_boost,        0, AP_GB),                  /* the GBC core's chipfeat has it too */
  GO(gbc_mode,                GAMEOPT_F_MK3, AP_GB),
  GO(gbc_sync,                GAMEOPT_F_MK3, AP_GB),      /* GBC core: its label says so */
  GO(gbc_stretch,             GAMEOPT_F_MK3, AP_GB),
  GO(gb_rotate_buttons,       0, AP_GB),                  /* SGB BIOS patch / GBC player block, at load */
  GO(vidmode_game,            0, AP_ALL),
};
#undef GO
#define GO_N (sizeof(gameopt_list) / sizeof(gameopt_list[0]))
#define GO_OFF(i) (gameopt_list[i].off)
#define GO_FL(i)  (gameopt_list[i].fl)
_Static_assert(GO_N <= GAMEOPT_MAX, "gameopt_list: GAMEOPT_BLK has room for GAMEOPT_MAX");
_Static_assert(GAMEOPT_SLOTS == SRM_SLOT_COUNT, "SaveGameSlot counts the battery-save slots");
_Static_assert(GAMEOPT_BLK_SIZE == 168, "memmap.h reserves 168 B for GAMEOPT_BLK");

static const char go_key_srm[] = "SaveGameSlot";
static const char go_key_ss[]  = "SaveStateSlot";

/* While a game runs CFG holds its values and go_glob the globals (gameopt_swap).  Both
   arrays are written by gameopt_apply before anything reads them. */
static uint8_t go_active;
static uint8_t go_val[GO_N];
static uint8_t go_glob[GO_N];
/* The lines of the file this firmware does not know, kept for the rewrite: the scratch
   LEAF, held from the read to the write by the two callers that write (go_keep_take).
   NULL = not held: the read keeps nothing and the write refuses.  Not a buffer of its
   own in AHB RAM: those bytes come out of the card modules' block (module.c), and the
   336 B these three arrays took there left fileops.bin without room for its buffers --
   every paste / rename / new folder failed as a card error on the LPC boards. */
#define GO_KEEP 256
typedef char go_keep_t[GO_KEEP];
SCRATCH_FITS(go_keep_t, SCRATCH_LEAF_BYTES);
static char *go_keep;
static uint16_t go_keep_len;
static uint8_t go_keep_full;
static uint16_t go_any;                      /* go_write: something went into the file */

int gameopt_count(void) { return GO_N; }

static uint8_t *go_cfg(unsigned i) { return (uint8_t *)&CFG + GO_OFF(i); }

/* not on this board: a Mk.III feature on a Mk.II, or a key this build's config.yml lacks */
static int go_board_na(unsigned i) {
#ifdef CONFIG_MK2
  if(GO_FL(i) & GAMEOPT_F_MK3) return 1;
#endif
  return !cfg_key_at(GO_OFF(i), NULL);
}

/* The game the screens show (AP_* type | GO_CHIP(id) of what it has), from gameopt_classify
   or the running game (gameopt_stage); every option applies until one is known. */
static uint16_t go_cls = 0xffff;

/* ...and not for this game either */
static int go_na(unsigned i) {
  uint8_t ap = gameopt_list[i].ap;
  if(!(ap & go_cls & AP_ALL)) return 1;
  if((ap >> 5) && !(go_cls & GO_CHIP(ap >> 5))) return 1;
  return go_board_na(i);
}

/* A game's console, by its name (what loads it decides the same way). */
static uint16_t go_type(const char *rom) {
  const char *x = strrchr(rom, '.');
  if(path_is_gb(rom)) return AP_GB;
  if(x && !strcasecmp(x, ".a26")) return AP_A26;
  if(x && !strcasecmp(x, ".nes")) return AP_NES;
  if(x && !strcasecmp(x, ".sms")) return AP_SMS;
  return AP_SNES;
}

static uint16_t go_chips_of(const snes_romprops_t *p) {
  return (p->has_cx4 ? GO_CHIP(1) : 0) | (p->has_gsu ? GO_CHIP(2) : 0)
       | ((p->fpga_features & (FEAT_CC92 | FEAT_PF94)) ? GO_CHIP(3) : 0);
}

/* The menus' screen for a game that is not running: its console, and for a SNES game what
   its header says (smc_id, the load's own identification, on the file) and whether a
   <stem>.msu sits next to it. */
void gameopt_classify(const char *rom) {
  uint16_t c = go_type(rom);
  if(c == AP_SNES) {
    snes_romprops_t p;
    file_open((uint8_t *)rom, FA_READ);
    if(!file_res) {
      smc_set_file_span(0);
      smc_id(&p, 0);
      c |= go_chips_of(&p);
    }
    file_close();
    file_res = 0;
  }
  if(c & (AP_SNES | AP_GB)) {               /* msu1_check pairs a .msu with either */
    char path[256], *x;
    strlcpy_nul(path, rom, sizeof(path) - 4);
    x = strrchr(path, '.');
    if(x && !strchr(x, '/')) {
      strcpy(x, ".msu");
      if(f_stat((TCHAR *)path, NULL) == FR_OK) c |= GO_CHIP(4);
    }
  }
  go_cls = c;
}

/* The file's scalars: true/false (what the firmware and the Web Manager write; the first
   letter decides) or a decimal.
   1 = *v is one, a boolean as 0/1; 0 = none, and -1 is none too: "follow the global". */
static __attribute__((noinline)) int go_scalar(const char *s, long *v) {
  char *e;
  if((*s | 0x20) == 't') { *v = 1; return 1; }
  if((*s | 0x20) == 'f') { *v = 0; return 1; }
  *v = strtol(s, &e, 10);
  return e != s && !*e && *v != -1;
}

/* Key i of the file: the options of gameopt_list, then the two slots (NULL: the option has
   no key on this build). */
static const char *go_key(unsigned i, uint8_t *isbool) {
  if(i < GO_N) return cfg_key_at(GO_OFF(i), isbool);
  if(isbool) *isbool = 0;
  return i == GO_N ? go_key_srm : go_key_ss;
}

/* One line of the file into f; a line that is not ours is kept for the rewrite.  Ours are
   "Key: value" as config.yml writes them (the key from the start of the line to the colon). */
void gameopt_line(gameopt_file_t *f, char *line) {
  size_t len = strlen(line);
  int fits = go_keep && go_keep_len + len + 2 <= GO_KEEP;   /* + the '\n' a last line lacks + NUL */
  char *c = strchr(line, ':'), *v, *e;
  if(fits) memcpy(go_keep + go_keep_len, line, len + 1);
  if((unsigned char)*line <= ' ' || *line == '-') return;     /* blank, or the "---" */
  if(c && *line != '#') {
    *c = 0;
    for(v = c + 1; *v == ' '; v++);
    for(e = v; (unsigned char)*e > ' '; e++);
    *e = 0;
    for(unsigned i = 0; i < GO_N + 2; i++) {
      const char *key = go_key(i, NULL);
      long n;
      int have;
      if(!key || strcmp(line, key)) continue;
      have = go_scalar(v, &n);
      if(i < GO_N) {
        uint8_t x = have ? cfg_scalar_at(GO_OFF(i), n) : GAMEOPT_V_DEFAULT;
        f->val[i] = x < GAMEOPT_V_NA ? x : GAMEOPT_V_DEFAULT;
      } else {
        *(i == GO_N ? &f->srm : &f->ss) = (have && n >= 1 && n <= GAMEOPT_SLOTS) ? (uint8_t)(n - 1) : 0xff;
      }
      return;
    }
  }
  if(!go_keep) return;                       /* a read that will not be written back */
  if(!fits) { go_keep_full = 1; return; }
  go_keep_len += len;
  if(go_keep[go_keep_len - 1] != '\n') go_keep[go_keep_len++] = '\n';
  go_keep[go_keep_len] = 0;
}

/* The game's file into f.  path = 256 bytes of the caller's, scratch. */
static __attribute__((noinline)) void go_read(const char *src, gameopt_file_t *f, char *path) {
  char line[128];
  memset(f, 0xff, sizeof(*f));
  f->legacy = 0;
  go_keep_len = 0;
  go_keep_full = 0;
  if(go_keep) go_keep[0] = 0;
  if(path_asset(path, 256, SAVE_BASEDIR, src, ".yml") >= 0) {
    file_open((uint8_t *)path, FA_READ);
    if(!file_res) while(f_gets(line, sizeof(line), &file_handle)) gameopt_line(f, line);
    file_close();
  }
  /* the .slot of before (byte 0 the battery save, byte 1 the savestate, '1'..'4'), for
     the slot keys the file does not have */
  if((f->srm == 0xff || f->ss == 0xff)
     && path_asset(path, 256, SAVE_BASEDIR, src, ".slot") >= 0) {
    uint8_t b[2];
    UINT br = 0;
    file_open((uint8_t *)path, FA_READ);
    if(!file_res) {
      f_read(&file_handle, b, 2, &br);
      f->legacy = 1;
    }
    file_close();
    if(br >= 1 && f->srm == 0xff && b[0] >= '1' && b[0] < '1' + GAMEOPT_SLOTS) f->srm = b[0] - '1';
    if(br >= 2 && f->ss == 0xff && b[1] >= '1' && b[1] < '1' + GAMEOPT_SLOTS) f->ss = b[1] - '1';
  }
  file_res = 0;
}

static void go_put(const char *key, int v, uint8_t isbool) {
  go_any = 1;
  if(isbool) f_printf(&file_handle, "%s: %s\n", key, v ? "true" : "false");
  else f_printf(&file_handle, "%s: %d\n", key, v);
}

/* f back to the game's file (after go_read of the same game: the kept lines go with it).
   Nothing left to say = no file.  0 = the card refused it. */
static __attribute__((noinline)) int go_write(const char *src, const gameopt_file_t *f, char *path) {
  int ok;
  if(!go_keep || go_keep_full) return 0;     /* a line we could not hold would be lost */
  if(path_asset(path, 256, SAVE_BASEDIR, src, ".yml") < 0) return 0;
  path_asset_mkdir(path);
  file_open((uint8_t *)path, FA_CREATE_ALWAYS | FA_WRITE);
  if(file_res) {
    file_res = 0;
    return 0;
  }
  go_keep[go_keep_len] = 0;                 /* past it: the tentative copy of a line of ours */
  go_any = go_keep_len;
  f_puts("---\n", &file_handle);
  f_puts(go_keep, &file_handle);
  for(unsigned i = 0; i < GO_N + 2; i++) {
    uint8_t isbool, v;
    const char *key = go_key(i, &isbool);
    if(i < GO_N) {
      v = f->val[i];
      if(v >= GAMEOPT_V_NA) continue;
    } else {
      v = i == GO_N ? f->srm : f->ss;             /* slot 1 is no line */
      if(!v || v >= GAMEOPT_SLOTS) continue;
      v++;
    }
    if(key) go_put(key, v, isbool);
  }
  file_close();                              /* a file this small reaches the card here */
  ok = !file_res;
  /* nothing left to say: no file at all */
  if(!go_any) ok = f_unlink((TCHAR *)path) == FR_OK;
  file_res = 0;
  /* the slots live in the .yml now: the .slot it was read from goes */
  if(ok && f->legacy && path_asset(path, 256, SAVE_BASEDIR, src, ".slot") >= 0)
    f_unlink((TCHAR *)path);
  return ok;
}

void gameopt_swap(void) {
  if(!go_active) return;
  for(unsigned i = 0; i < GO_N; i++) {
    uint8_t *p = go_cfg(i), t = *p;
    *p = go_glob[i];
    go_glob[i] = t;
  }
}

void gameopt_restore(void) {
  gameopt_swap();
  go_active = 0;
}

/* noinline: called once, from load_rom; inlined, its path buffer and file image would
   sit in load_rom's frame under every phase of the load */
void __attribute__((noinline)) gameopt_apply(const char *src) {
  gameopt_file_t f;
  char path[256];
  gameopt_restore();
  go_read(src, &f, path);
  for(unsigned i = 0; i < GO_N; i++) {
    uint8_t *p = go_cfg(i);
    go_glob[i] = *p;
    go_val[i] = go_board_na(i) ? GAMEOPT_V_NA : f.val[i];
    if(go_val[i] < GAMEOPT_V_NA) {
      printf("gameopt: %s=%d\n", cfg_key_at(GO_OFF(i), NULL), go_val[i]);
      *p = go_val[i];
    }
  }
  go_active = 1;
}

static void go_stage(const uint8_t *val, uint8_t result) {
  uint8_t b[GAMEOPT_BLK_SIZE];
  memset(b, 0, sizeof(b));
  b[GAMEOPT_H_MAGIC] = 'G';
  b[GAMEOPT_H_MAGIC + 1] = 'O';
  b[GAMEOPT_H_VERSION] = GAMEOPT_VERSION;
  b[GAMEOPT_H_COUNT] = GO_N;
  b[GAMEOPT_H_RESULT] = result;
  for(unsigned i = 0; i < GO_N; i++) {
    uint16_t off = GO_OFF(i) | ((GO_FL(i) & GAMEOPT_F_LIVE) ? 0x8000 : 0);
#ifndef CONFIG_MK2
    /* which Game Boy core: there is only the SGB one on the Mk.II */
    if(go_cls == AP_GB) off |= (GO_FL(i) & (F_SGB | F_GBC)) << 11;
#endif
    b[GAMEOPT_VAL + i] = go_na(i) ? GAMEOPT_V_NA : val[i];
    b[GAMEOPT_GLOBAL + i] = go_active ? go_glob[i] : *go_cfg(i);
    b[GAMEOPT_OFF + 2 * i] = off;
    b[GAMEOPT_OFF + 2 * i + 1] = off >> 8;
  }
  sram_writeblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
}

void gameopt_stage(uint8_t result) {
  go_cls = go_type(current_filename);
  if(go_cls == AP_SNES) go_cls |= go_chips_of(&romprops);
  if((go_cls & (AP_SNES | AP_GB)) && romprops.has_msu1) go_cls |= GO_CHIP(4);
  go_stage(go_val, result);
}

/* What the in-game menu changes takes effect at once: the $2100 patch and its brightness
   limit are feature bits every game core has, and the feature register can be rewritten
   with the game running (msu1.c does it on every track); the rest of GAMEOPT_F_LIVE is
   read from CFG when it is needed. */
static void go_live(void) {
  const uint16_t m = FEAT_2100 | FEAT_2100_LIMIT(15);
  uint16_t f = (current_features & ~m) | FEAT_2100_LIMIT(CFG.brightness_limit);
  if(CFG.onechip_transient_fixes) f |= FEAT_2100;
  romprops.fpga_features = (romprops.fpga_features & ~m) | (f & m);
  fpga_set_features(f);
  fpga_set_dac_boost(CFG.msu_volume_boost);
}

/* The scratch LEAF for the kept lines; 0 = someone holds it (the write then fails). */
static int go_keep_take(void) {
  if(!scratch_leaf_take(SCR_GAMEOPT)) return 0;
  go_keep = (char *)SCRATCH_LEAF(go_keep_t);
  return 1;
}

static void go_keep_drop(void) {
  if(!go_keep) return;
  go_keep = NULL;
  scratch_leaf_drop();
}

void gameopt_command(const char *src, uint8_t op, uint8_t idx, uint8_t val, int live) {
  gameopt_file_t f;
  char path[256];
  uint8_t result = GAMEOPT_RES_OK;
  printf("gameopt %s: op=%d idx=%d val=%d live=%d\n", src, op, idx, val, live);
  if(op == GAMEOPT_OP_SET) go_keep_take();
  go_read(src, &f, path);
  if(op == GAMEOPT_OP_SET) {
    if(idx >= GO_N || go_na(idx)
       || (live && !(go_active && (GO_FL(idx) & GAMEOPT_F_LIVE)))) {
      result = GAMEOPT_RES_FAIL;
    } else {
      if(val < GAMEOPT_V_NA) {
        val = cfg_scalar_at(GO_OFF(idx), val);
        if(val >= GAMEOPT_V_NA) val = GAMEOPT_V_DEFAULT;
      } else {
        val = GAMEOPT_V_DEFAULT;
      }
      f.val[idx] = val;
      if(!go_write(src, &f, path)) {
        result = GAMEOPT_RES_FAIL;
      } else if(live) {
        go_val[idx] = val;
        *go_cfg(idx) = val == GAMEOPT_V_DEFAULT ? go_glob[idx] : val;
        go_live();
      }
    }
  }
  go_keep_drop();
  go_stage(live ? go_val : f.val, result);
}

void gameopt_slots(const char *src, uint8_t *srm, uint8_t *ss) {
  gameopt_file_t f;
  char path[256];
  go_read(src, &f, path);
  *srm = f.srm;
  *ss = f.ss;
}

int gameopt_slots_set(const char *src, uint8_t srm, uint8_t ss) {
  gameopt_file_t f;
  char path[256];
  int ok;
  go_keep_take();
  go_read(src, &f, path);
  f.srm = srm < GAMEOPT_SLOTS ? srm : 0;
  f.ss = ss < GAMEOPT_SLOTS ? ss : 0;
  ok = go_write(src, &f, path);
  go_keep_drop();
  return ok;
}
