/* Resident veneers of the card overlays (see ovl.h): overlay 1 = theme.c, sysinfo.c,
   cover.c, spcload.c, giload.c; overlay 2 = patchui.c, patchmeta.c, patchexp.c, savesop.c,
   bootleg.c.
   Each one loads its overlay and calls the entry, or answers what the function itself
   answers when it cannot do its job. */
#include "config.h"
#include "ovl.h"
#include "theme.h"
#include "sysinfo.h"
#include "cover.h"
#include "patchmeta.h"
#include "bootleg.h"
#include "memory.h"
#include "patch.h"
#include "gameinfo.h"
#include <string.h>

/* overlay `id` is loaded and ovl_depth counts the running call */
static int ovl_enter(uint8_t id)
{
  if(!ovl_ensure(id)) return 0;
  ovl_depth++;
  return 1;
}

#define E(i) (OVL_HDR->entry[i])
#define V(o, i, T, ...) do { if(ovl_enter(o)) { ((T)E(i))(__VA_ARGS__); ovl_depth--; } } while(0)
#define R(o, i, T, fail, ...) do { if(!ovl_enter(o)) return (fail); r = ((T)E(i))(__VA_ARGS__); ovl_depth--; return r; } while(0)
#define M1 OVL_ID_MENU1
#define M2 OVL_ID_MENU2

typedef void (*fv_t)(void);
typedef void (*fs_t)(const char *);

/* ---- overlay 1 ------------------------------------------------------------------- */
void theme_apply(void)           { V(M1, 0, fv_t); }
int  theme_font_edges_stale(void){ int r; R(M1, 1, int (*)(void), 0); }
void theme_font_edges(void)      { V(M1, 2, fv_t); }
void theme_font_file(void)       { V(M1, 3, fv_t); }
void font_select(const char *n)  { V(M1, 4, fs_t, n); }
void theme_clear(int f)          { V(M1, 5, void (*)(int), f); }
void theme_pick(const char *n)   { V(M1, 6, fs_t, n); }
void theme_select(const char *n) { V(M1, 7, fs_t, n); }
/* no overlay: a block that only says so (every other field zero, the header written
   last), so the menu names ovl1 instead of asking for a firmware update; 0 = no passes */
int sysinfo_open(void)
{
  static const uint8_t hdr[4] = { SYSINFO_MAGIC0, SYSINFO_MAGIC1, SYSINFO_VERSION,
                                  SYSINFO_FLAG_NOOVL };
  int r;
  if(ovl_enter(M1)) { r = ((int (*)(void))E(8))(); ovl_depth--; return r; }
  sram_memset(SRAM_SYSINFO_ADDR, sizeof(sysinfo_blk_t), 0);
  sram_writeblock((void *)hdr, SRAM_SYSINFO_ADDR, sizeof(hdr));
  return 0;
}
/* no overlay: "no cover", what the real ones mark before anything else (left alone, the
   menu would show the previous game's cover) */
static int cover_none(uint32_t a)
{
  sram_writebyte(COVER_STATUS_NONE, a + COVER_OFF_STATUS);
  return 0;
}
int load_cover(const uint8_t *p, uint32_t a)
{ int r; R(M1, 9, int (*)(const uint8_t *, uint32_t), cover_none(a), p, a); }
int load_cover_path(const char *p, uint32_t a)
{ int r; R(M1, 10, int (*)(const char *, uint32_t), cover_none(a), p, a); }
int  sysinfo_pass(int m)         { int r; R(M1, 11, int (*)(int), m, m); }
uint32_t load_spc(uint8_t *f, uint32_t d, uint32_t h)
{ uint32_t r; R(M1, 12, uint32_t (*)(uint8_t *, uint32_t, uint32_t), 0, f, d, h); }
int menu_spc_pick_random(uint8_t *o, int n)
{ int r; R(M1, 13, int (*)(uint8_t *, int), 0, o, n); }
/* no overlay: the info screen is skipped (status NONE), the game boots straight away */
void gameinfo_load(uint8_t *p)
{
  uint8_t none[3] = { GAMEINFO_MAGIC0, GAMEINFO_MAGIC1, GAMEINFO_STATUS_NONE };
  if(ovl_enter(M1)) { ((void (*)(uint8_t *))E(14))(p); ovl_depth--; return; }
  gameinfo_fmv_stop();
  sram_writeblock(none, SRAM_GAMEINFO_ADDR, sizeof(none));
}
/* no overlay: the full text stays invalid, the screen keeps its 256-character copy */
void gameinfo_desc_full(void)
{
  if(ovl_enter(M1)) { ((fv_t)E(15))(); ovl_depth--; return; }
  sram_writebyte(0, SRAM_GAMEINFO_DESCEXT_ADDR);
}
/* no overlay: the menu's music, and a box with nothing to offer */
int theme_music_load(void)       { int r; R(M1, 16, int (*)(void), 0); }
void theme_info(const char *p)
{
  if(ovl_enter(M1)) { ((fs_t)E(17))(p); ovl_depth--; return; }
  sram_writebyte(0, SRAM_COVER_ADDR + THEME_INFO_OFF);
  sram_writebyte(0, SRAM_COVER_ADDR + COVER_OFF_STATUS);
}

/* ---- overlay 2 ------------------------------------------------------------------- */
/* no overlay: an empty list, so the dialog opens on "no patch" */
void query_ips_patches(void)
{
  if(ovl_enter(M2)) { ((fv_t)E(0))(); ovl_depth--; return; }
  current_ips_srm_source[0] = 0;
  current_ips_flags = 0;
  ips_scan_count = 0;
  sram_writebyte(0, SRAM_IPS_LIST_ADDR);
  sram_writebyte(IPS_DLGMODE_PATCH, SRAM_IPS_LIST_ADDR + IPS_DLGMODE_OFFSET);
}
/* no overlay: the entry boots vanilla, cut to the base ROM path as load_rom wants it (a
   patched list launch never gets here without it: menucmd_launch_rom refuses it first) */
void stage_patch_from_entry(char *e)
{
  /* an entry with no "\t<patch>" tag (most of them) needs nothing from the overlay */
  char *t = strchr(e, '\t');
  if(t && ovl_enter(M2)) { ((void (*)(char *))E(1))(e); ovl_depth--; return; }
  ips_pending_index = 0;
  current_ips_srm_source[0] = 0;
  current_ips_flags = 0;
  if(t) *t = 0;
}
void patchmeta_save(const uint8_t *p, uint32_t a, uint8_t n)
{ V(M2, 2, void (*)(const uint8_t *, uint32_t, uint8_t), p, a, n); }
/* no overlay: the export reports a failure, a deleted ROM leaves its sidecars behind */
int patch_export_exists(const uint8_t *p)
{ int r; R(M2, 3, int (*)(const uint8_t *), 0, p); }
int patch_export_write(const uint8_t *p, uint32_t a, uint32_t n)
{ int r; R(M2, 4, int (*)(const uint8_t *, uint32_t, uint32_t), 0, p, a, n); }
int patch_export_copy_assets(const uint8_t *s, const uint8_t *k)
{ int r; R(M2, 5, int (*)(const uint8_t *, const uint8_t *), 0, s, k); }
int patch_unlink_rom_assets(const uint8_t *p)
{ int r; R(M2, 6, int (*)(const uint8_t *), 0, p); }
/* no overlay: the screen still lists the saves, every action fails */
void saves_command(const char *s, uint8_t op, uint8_t kind, uint8_t slot, uint8_t dst)
{
  if(ovl_enter(M2)) {
    ((void (*)(const char *, uint8_t, uint8_t, uint8_t, uint8_t))E(7))(s, op, kind, slot, dst);
    ovl_depth--;
    return;
  }
  saves_stage(s, 1, op == SAVES_OP_INFO ? SAVES_RES_OK : SAVES_RES_FAIL);
}
uint8_t bootleg_identify_file(uint32_t o, uint32_t n)
{ uint8_t r; R(M2, 8, uint8_t (*)(uint32_t, uint32_t), BOOTLEG_NONE, o, n); }
/* BOOTLEG_NONE (every smc_id of a normal game) does nothing: answered here, no load */
void bootleg_apply(snes_romprops_t *p, uint8_t v, uint32_t n)
{ if(v != BOOTLEG_NONE) V(M2, 9, void (*)(snes_romprops_t *, uint8_t, uint32_t), p, v, n); }

/* globals of the overlaid files that resident code uses stay resident */
uint8_t bootleg_scan = 0;

/* the overlays' .bss lives here (zeroed at reset, kept across reloads): OVL1_BSS bytes of
   overlay 1, then OVL2_BSS of overlay 2 (ovl.h; the Makefile links each into its slice) */
_Static_assert(OVL1_BSS % 4 == 0 && OVL2_BSS % 4 == 0, "ovl: .bss slices must be word-sized");
uint32_t ovl_bss[(OVL1_BSS + OVL2_BSS) / 4];
