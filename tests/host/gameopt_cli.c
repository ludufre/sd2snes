/* Host test of the per-game settings file (src/gameopt.c) over the REAL src/cfg.c (its keys,
 * kinds and clamps) and the FatFs string functions of cfg_cli.c (CRLF out, '\r' stripped in,
 * the card re-rooted under build/).  Every case checks itself; exit 0 = all passed.
 *
 * What is pinned: the file is read with config.yml's keys and values, -1 and a missing key
 * both mean "the global", the two slot keys, the .slot of before as a fallback (and removed
 * on the first write), lines this firmware does not know kept through a rewrite (and a
 * rewrite refused rather than one of them lost), a file with nothing left deleted, CFG
 * overlaid on apply / swapped for cfg_save / restored, a live change reaching the feature
 * word, and GAMEOPT_BLK's layout.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "ff.h"
#include "fileops.h"
#include "scratch.h"
#include "memory.h"
#include "cfg.h"
#include "smc.h"
#include "fpga_spi.h"
#include "gameopt.h"

extern cfg_t CFG;
void cfg_defaults(cfg_t *c);
extern cfg_t CFG_DEFAULT_img;
#define CFG_DEFAULT CFG_DEFAULT_img
int cfg_cli_main(int argc, char **argv);   /* cfg_cli.c's own main, renamed: unused */
void cfg_cli_sram_init(void);

snes_romprops_t romprops;
char current_filename[258] = "/roms/Game.sfc";
static snes_romprops_t next_id;              /* what smc_id "finds" in the next file */
void smc_set_file_span(uint32_t rom_size) { (void)rom_size; }
void smc_id(snes_romprops_t *props, uint32_t file_offset) { (void)file_offset; *props = next_id; }
int path_is_gb(const char *name) {
  const char *x = strrchr(name, '.');
  return x && (x[1] | 0x20) == 'g' && (x[2] | 0x20) == 'b';
}
uint16_t current_features;
static uint16_t last_features = 0xffff;
static int last_boost = -1;
void fpga_set_features(uint16_t feat) { last_features = feat; current_features = feat; }
void fpga_set_dac_boost(uint8_t boost) { last_boost = boost; }

/* The bucket/namespace logic is run_bucket.sh's; here a name is root + stem + ext. */
int path_asset(char *buf, int buflen, const char *root, const char *src, const char *ext) {
  const char *leaf = strrchr(src, '/');
  const char *dot;
  int n;
  leaf = leaf ? leaf + 1 : src;
  dot = strrchr(leaf, '.');
  n = snprintf(buf, buflen, "%s%.*s%s", root, dot ? (int)(dot - leaf) : (int)strlen(leaf), leaf, ext);
  if(n < 0 || n >= buflen) { buf[0] = 0; return -1; }
  return 0;
}
FRESULT path_asset_mkdir(char *path) { (void)path; return FR_OK; }

static int fails;
#define CHECK(c) do { if(!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while(0)

#define HOST_SAVES "build/sd2snes/saves/"

static void put(const char *name, const char *text) {   /* a card file, CRLF like FatFs */
  char p[256];
  FILE *f;
  snprintf(p, sizeof p, HOST_SAVES "%s", name);
  mkdir("build/sd2snes", 0777);
  mkdir("build/sd2snes/saves", 0777);
  f = fopen(p, "wb");
  if(!f) { perror(p); exit(99); }
  for(; *text; text++) {
    if(*text == '\n') fputc('\r', f);
    fputc(*text, f);
  }
  fclose(f);
}

static char *slurp(const char *name) {   /* NULL = no such file */
  static char buf[4096];
  char p[256];
  FILE *f;
  size_t n;
  snprintf(p, sizeof p, HOST_SAVES "%s", name);
  f = fopen(p, "rb");
  if(!f) return NULL;
  n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);
  return buf;
}

static int has(const char *name, const char *line) {   /* the file holds this exact line */
  char want[256];
  const char *s = slurp(name);
  snprintf(want, sizeof want, "%s\r\n", line);
  return s && strstr(s, want) != NULL;
}

static int idx_of(size_t off) {   /* gameopt_list's index of a field, from GAMEOPT_BLK */
  uint8_t b[GAMEOPT_BLK_SIZE];
  gameopt_stage(GAMEOPT_RES_OK);
  sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
  for(int i = 0; i < b[GAMEOPT_H_COUNT]; i++)
    if(((b[GAMEOPT_OFF + 2 * i] | b[GAMEOPT_OFF + 2 * i + 1] << 8) & 0x7fff) == off) return i;
  printf("FAIL: offset %zu is not in the list\n", off);
  fails++;
  return 0;
}

int main(void) {
  cfg_defaults(&CFG_DEFAULT_img);
  uint8_t b[GAMEOPT_BLK_SIZE], srm, ss;
  int i1chip, ihook, ibright;
  const char *g = "/roms/Game.sfc";

  cfg_cli_sram_init();
  memcpy(&CFG, &CFG_DEFAULT, sizeof(cfg_t));
  remove(HOST_SAVES "Game.yml");

  /* ---- reading ---- */
  put("Game.yml",
      "---\n"
      "1CHIPTransientFixes: true\n"
      "BrightnessLimit: 8\n"
      "EnableAutoSave: -1\n"
      "ShortReset2Menu: 9\n"
      "GbcMode: 2\n"
      "SaveGameSlot: 3\n"
      "SaveStateSlot: 7\n"
      "FutureKey: hello\n"
      "# a note\n"
      "EnableIngameHook: false\n"
      "EnableIngameSavestate: banana\n");
  gameopt_apply(g);
  CHECK(CFG.onechip_transient_fixes == 1);
  CHECK(CFG.brightness_limit == 8);
  CHECK(CFG.enable_autosave == CFG_DEFAULT.enable_autosave);           /* -1 = the global */
  CHECK(CFG.reset_to_menu == 1);                                       /* clamp 0x41 */
#ifdef CONFIG_MK2
  CHECK(CFG.gbc_mode == CFG_DEFAULT.gbc_mode);                         /* Mk.III only */
#else
  CHECK(CFG.gbc_mode == 2);
#endif
  CHECK(CFG.enable_ingame_hook == 0);
  CHECK(CFG.enable_ingame_savestate == CFG_DEFAULT.enable_ingame_savestate);  /* not a scalar */
  gameopt_slots(g, &srm, &ss);
  CHECK(srm == 2);
  CHECK(ss == 0xff);                                                   /* 7 is no slot */

  /* ---- cfg_save writes the globals, and CFG keeps the game's ---- */
  cfg_save();
  {
    FILE *f = fopen("build/sd2snes/config.yml", "rb");
    static char cfgtext[8192];
    size_t n = f ? fread(cfgtext, 1, sizeof cfgtext - 1, f) : 0;
    cfgtext[n] = 0;
    if(f) fclose(f);
    CHECK(strstr(cfgtext, "1CHIPTransientFixes: false\r\n") != NULL);
    CHECK(strstr(cfgtext, "BrightnessLimit: 15\r\n") != NULL);
    CHECK(strstr(cfgtext, "EnableIngameHook: true\r\n") != NULL);
  }
  CHECK(CFG.onechip_transient_fixes == 1 && CFG.brightness_limit == 8);

  /* ---- GAMEOPT_BLK ---- */
  i1chip = idx_of(offsetof(cfg_t, onechip_transient_fixes));
  ihook = idx_of(offsetof(cfg_t, enable_ingame_hook));
  ibright = idx_of(offsetof(cfg_t, brightness_limit));
  gameopt_stage(GAMEOPT_RES_OK);
  sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
  CHECK(b[0] == 'G' && b[1] == 'O' && b[GAMEOPT_H_VERSION] == GAMEOPT_VERSION);
  CHECK(b[GAMEOPT_H_COUNT] == gameopt_count());
  CHECK(b[GAMEOPT_VAL + i1chip] == 1 && b[GAMEOPT_GLOBAL + i1chip] == 0);
  CHECK(b[GAMEOPT_VAL + ihook] == 0 && b[GAMEOPT_GLOBAL + ihook] == 1);
  CHECK(b[GAMEOPT_OFF + 2 * i1chip + 1] & 0x80);                       /* live */
  CHECK(!(b[GAMEOPT_OFF + 2 * ihook + 1] & 0x80));                     /* load only */
  CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, enable_autosave))] == GAMEOPT_V_DEFAULT);

  /* ---- the in-game menu: a live change, a default, and a refused one ---- */
  current_features = FEAT_2100 | FEAT_2100_LIMIT(8) | 0x0001;
  gameopt_command(g, GAMEOPT_OP_SET, (uint8_t)i1chip, 0, 1);
  sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
  CHECK(b[GAMEOPT_H_RESULT] == GAMEOPT_RES_OK);
  CHECK(CFG.onechip_transient_fixes == 0);
  CHECK(!(last_features & FEAT_2100) && (last_features & 0x0001));
  CHECK(has("Game.yml", "1CHIPTransientFixes: false"));
  CHECK(has("Game.yml", "FutureKey: hello"));                          /* kept */
  CHECK(has("Game.yml", "# a note"));
  CHECK(has("Game.yml", "SaveGameSlot: 3"));
  CHECK(!strstr(slurp("Game.yml"), "EnableIngameSavestate"));         /* ours: unreadable = the global */
  if(fails) printf("Game.yml now:\n%s---\n", slurp("Game.yml"));
  CHECK(!strstr(slurp("Game.yml"), "SaveStateSlot"));                  /* it said nothing */
  CHECK(!strstr(slurp("Game.yml"), "EnableAutoSave"));                 /* -1 is not written */
  /* the kept lines live in the scratch LEAF: held by someone else, the rewrite is refused
     and the file stays as it was; after a write the LEAF is free again */
  {
    char before[4096];
    snprintf(before, sizeof(before), "%s", slurp("Game.yml"));
    CHECK(scratch_leaf_take(SCR_COVER));
    gameopt_command(g, GAMEOPT_OP_SET, (uint8_t)ibright, 5, 1);
    sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
    CHECK(b[GAMEOPT_H_RESULT] == GAMEOPT_RES_FAIL);
    CHECK(!strcmp(before, slurp("Game.yml")));
    CHECK(!gameopt_slots_set(g, 1, 1));
    CHECK(!strcmp(before, slurp("Game.yml")));
    scratch_leaf_drop();
    CHECK(gameopt_slots_set(g, 2, 0));
    CHECK(scratch_leaf_take(SCR_COVER));
    scratch_leaf_drop();
    CHECK(has("Game.yml", "FutureKey: hello") && has("Game.yml", "SaveGameSlot: 3"));
  }
  gameopt_command(g, GAMEOPT_OP_SET, (uint8_t)ibright, 3, 1);
  CHECK(CFG.brightness_limit == 3);
  CHECK((last_features & FEAT_2100_LIMIT(15)) == FEAT_2100_LIMIT(3));
  CHECK(last_boost == CFG.msu_volume_boost);
  gameopt_command(g, GAMEOPT_OP_SET, (uint8_t)i1chip, GAMEOPT_V_DEFAULT, 1);
  CHECK(CFG.onechip_transient_fixes == 0);                             /* the global */
  CHECK(!strstr(slurp("Game.yml"), "1CHIPTransientFixes"));
  gameopt_command(g, GAMEOPT_OP_SET, (uint8_t)ihook, 1, 1);
  sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
  CHECK(b[GAMEOPT_H_RESULT] == GAMEOPT_RES_FAIL);                      /* not while it runs */
  CHECK(CFG.enable_ingame_hook == 0 && has("Game.yml", "EnableIngameHook: false"));

  /* ---- back to the menu ---- */
  gameopt_restore();
  CHECK(!memcmp(&CFG, &CFG_DEFAULT, sizeof(cfg_t)));
  gameopt_restore();                                                   /* twice is harmless */
  CHECK(!memcmp(&CFG, &CFG_DEFAULT, sizeof(cfg_t)));

  /* ---- the menu's screen: another game, from nothing and back to nothing ---- */
  remove(HOST_SAVES "Other.yml");
  gameopt_command("/roms/Other.smc", GAMEOPT_OP_INFO, 0, 0, 0);
  sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
  CHECK(b[GAMEOPT_VAL + ihook] == GAMEOPT_V_DEFAULT && b[GAMEOPT_GLOBAL + ihook] == 1);
  CHECK(slurp("Other.yml") == NULL);
  gameopt_command("/roms/Other.smc", GAMEOPT_OP_SET, (uint8_t)ihook, 0, 0);
  CHECK(has("Other.yml", "EnableIngameHook: false"));
  CHECK(!memcmp(&CFG, &CFG_DEFAULT, sizeof(cfg_t)));                    /* the menu's CFG */
  gameopt_command("/roms/Other.smc", GAMEOPT_OP_SET, (uint8_t)ihook, GAMEOPT_V_DEFAULT, 0);
  CHECK(slurp("Other.yml") == NULL);                                   /* nothing left */
  gameopt_command("/roms/Other.smc", GAMEOPT_OP_SET, (uint8_t)ibright, 77, 0);
  CHECK(has("Other.yml", "BrightnessLimit: 13"));                      /* a nibble, masked */
  remove(HOST_SAVES "Other.yml");

  /* ---- the .slot of before ---- */
  remove(HOST_SAVES "Old.yml");
  put("Old.slot", "24");
  gameopt_slots("/roms/Old.sfc", &srm, &ss);
  CHECK(srm == 1 && ss == 3);
  put("Old.yml", "SaveStateSlot: 2\n");                                /* the file wins */
  gameopt_slots("/roms/Old.sfc", &srm, &ss);
  CHECK(srm == 1 && ss == 1);
  CHECK(gameopt_slots_set("/roms/Old.sfc", srm, 0));
  CHECK(has("Old.yml", "SaveGameSlot: 2") && !strstr(slurp("Old.yml"), "SaveStateSlot"));
  CHECK(slurp("Old.slot") == NULL);                                    /* migrated */
  CHECK(gameopt_slots_set("/roms/Old.sfc", 0, 0));
  CHECK(slurp("Old.yml") == NULL);

  /* ---- a file this firmware cannot hold whole is not rewritten ---- */
  {
    char big[1024], *p = big;
    for(int i = 0; i < 20; i++) p += snprintf(p, big + sizeof(big) - p, "FutureSetting%02d: something long\n", i);
    put("Big.yml", big);
    gameopt_command("/roms/Big.sfc", GAMEOPT_OP_SET, (uint8_t)ihook, 0, 0);
    sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
    CHECK(b[GAMEOPT_H_RESULT] == GAMEOPT_RES_FAIL);
    CHECK(has("Big.yml", "FutureSetting19: something long"));
    CHECK(!strstr(slurp("Big.yml"), "EnableIngameHook"));
    remove(HOST_SAVES "Big.yml");
  }

  /* ---- which options apply: by console, by chip, by a .msu next to the ROM ---- */
  {
    int icx4 = idx_of(offsetof(cfg_t, cx4_speed)), igb = idx_of(offsetof(cfg_t, sgb_enable_state));
    int imsu = idx_of(offsetof(cfg_t, msu_volume_boost)), iall = idx_of(offsetof(cfg_t, reset_to_menu));
    FILE *rom;
    mkdir("build/roms", 0777);
    remove("build/roms/Chip.msu");
    rom = fopen("build/roms/Chip.sfc", "wb"); fputs("x", rom); fclose(rom);
    rom = fopen("build/roms/Pokemon.gb", "wb"); fputs("x", rom); fclose(rom);
    memset(&next_id, 0, sizeof(next_id));
    gameopt_classify("/roms/Chip.sfc");
    gameopt_command("/roms/Chip.sfc", GAMEOPT_OP_INFO, 0, 0, 0);
    sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
    CHECK(b[GAMEOPT_VAL + icx4] == GAMEOPT_V_NA && b[GAMEOPT_VAL + imsu] == GAMEOPT_V_NA);
    CHECK(b[GAMEOPT_VAL + ihook] == GAMEOPT_V_DEFAULT && b[GAMEOPT_VAL + igb] == GAMEOPT_V_NA);
    next_id.has_cx4 = 1;
    rom = fopen("build/roms/Chip.msu", "wb"); fclose(rom);
    gameopt_classify("/roms/Chip.sfc");
    gameopt_command("/roms/Chip.sfc", GAMEOPT_OP_INFO, 0, 0, 0);
    sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
    CHECK(b[GAMEOPT_VAL + icx4] == GAMEOPT_V_DEFAULT && b[GAMEOPT_VAL + imsu] == GAMEOPT_V_DEFAULT);
    gameopt_command("/roms/Chip.sfc", GAMEOPT_OP_SET, (uint8_t)igb, 1, 0);   /* not this game's */
    sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
    CHECK(b[GAMEOPT_H_RESULT] == GAMEOPT_RES_FAIL && slurp("Chip.yml") == NULL);
    gameopt_classify("/roms/Pokemon.gb");
    gameopt_command("/roms/Pokemon.gb", GAMEOPT_OP_INFO, 0, 0, 0);
    sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
    /* the SGB core runs the SGB BIOS: the hook, the gestures and the $2100 patcher work there */
    CHECK(b[GAMEOPT_VAL + igb] == GAMEOPT_V_DEFAULT && b[GAMEOPT_VAL + ihook] == GAMEOPT_V_DEFAULT);
    CHECK(b[GAMEOPT_VAL + i1chip] == GAMEOPT_V_DEFAULT && b[GAMEOPT_VAL + iall] == GAMEOPT_V_DEFAULT);
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, enable_ingame_buttons))] == GAMEOPT_V_DEFAULT);
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, enable_hook_holdoff))] == GAMEOPT_V_DEFAULT);
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, reset_patch))] == GAMEOPT_V_DEFAULT);
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, enable_autosave))] == GAMEOPT_V_DEFAULT);
    /* ...but not the SNES cores' own machinery */
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, enable_ingame_pad2))] == GAMEOPT_V_NA);
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, enable_cheat_overlay))] == GAMEOPT_V_NA);
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, enable_ingame_savestate))] == GAMEOPT_V_NA);
    CHECK(b[GAMEOPT_VAL + idx_of(offsetof(cfg_t, r213f_override))] == GAMEOPT_V_NA);
    {
      int ibri = idx_of(offsetof(cfg_t, brightness_limit)), isync = idx_of(offsetof(cfg_t, gbc_sync));
      int ivol = idx_of(offsetof(cfg_t, sgb_volume_boost));
#ifdef CONFIG_MK2
      CHECK(b[GAMEOPT_VAL + ibri] == GAMEOPT_V_NA);             /* no limit in the Mk.II SGB core */
      CHECK(b[GAMEOPT_VAL + isync] == GAMEOPT_V_NA);
      CHECK(!(b[GAMEOPT_OFF + 2 * ihook + 1] & 0x60));          /* one Game Boy core: no names */
#else
      CHECK(b[GAMEOPT_VAL + ibri] == GAMEOPT_V_DEFAULT);
      CHECK(b[GAMEOPT_VAL + isync] == GAMEOPT_V_DEFAULT);
      CHECK((b[GAMEOPT_OFF + 2 * ihook + 1] & 0x60) == 0x20);   /* the SGB core's */
      CHECK((b[GAMEOPT_OFF + 2 * i1chip + 1] & 0xe0) == 0xa0);  /* live, the SGB core's */
      CHECK(!(b[GAMEOPT_OFF + 2 * isync + 1] & 0x60));          /* GBC core: its label says so */
      CHECK(!(b[GAMEOPT_OFF + 2 * ivol + 1] & 0x60));           /* both */
#endif
    }
    /* the players: no hook under them; SMS/A26 have no save RAM, NES no cheat.v */
    {
      static const struct { const char *rom; uint8_t hook, onechip, autosave, rpatch; } p[] = {
        { "/roms/Alex.sms", GAMEOPT_V_NA, GAMEOPT_V_DEFAULT, GAMEOPT_V_NA,      GAMEOPT_V_DEFAULT },
#ifdef CONFIG_MK2
        /* the Mk.II Atari core stubs cheat.v out: no reset hook */
        { "/roms/Pitfall.a26", GAMEOPT_V_NA, GAMEOPT_V_DEFAULT, GAMEOPT_V_NA,      GAMEOPT_V_NA },
#else
        { "/roms/Pitfall.a26", GAMEOPT_V_NA, GAMEOPT_V_DEFAULT, GAMEOPT_V_NA,      GAMEOPT_V_DEFAULT },
#endif
        { "/roms/Zelda.nes", GAMEOPT_V_NA, GAMEOPT_V_NA,      GAMEOPT_V_DEFAULT, GAMEOPT_V_NA },
      };
      int iauto = idx_of(offsetof(cfg_t, enable_autosave)), irp = idx_of(offsetof(cfg_t, reset_patch));
      for(unsigned k = 0; k < sizeof(p) / sizeof(p[0]); k++) {
        gameopt_classify(p[k].rom);
        gameopt_command(p[k].rom, GAMEOPT_OP_INFO, 0, 0, 0);
        sram_readblock(b, SRAM_GAMEOPT_ADDR, sizeof(b));
        CHECK(b[GAMEOPT_VAL + ihook] == p[k].hook && b[GAMEOPT_VAL + i1chip] == p[k].onechip);
        CHECK(b[GAMEOPT_VAL + iauto] == p[k].autosave && b[GAMEOPT_VAL + irp] == p[k].rpatch);
        CHECK(b[GAMEOPT_VAL + iall] == GAMEOPT_V_DEFAULT && b[GAMEOPT_VAL + igb] == GAMEOPT_V_NA);
        CHECK(!(b[GAMEOPT_OFF + 2 * ihook + 1] & 0x60));
      }
    }
  }

  remove(HOST_SAVES "Game.yml");
  if(fails) { printf("%d check(s) failed\n", fails); return 1; }
  printf("PASS gameopt (%d options)\n", gameopt_count());
  return 0;
}
