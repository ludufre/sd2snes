/* Header of overlay 1 (theme.c, sysinfo.c, cover.c, spcload.c, giload.c and the System
   Information helpers), linked at ptrcache. The entry order is the veneers' in ovl_stub.c.
   ovlpack.py fills in the build id, the size and the CRC. */
#include <stdint.h>
#include "../ovl.h"
void OVL_theme_apply(void); void OVL_theme_font_edges_stale(void);
void OVL_theme_font_edges(void); void OVL_theme_font_file(void); void OVL_font_select(void);
void OVL_theme_clear(void); void OVL_theme_pick(void); void OVL_theme_select(void);
void OVL_sysinfo_open(void); void OVL_load_cover(void); void OVL_load_cover_path(void);
void OVL_sysinfo_pass(void); void OVL_load_spc(void); void OVL_menu_spc_pick_random(void);
void OVL_gameinfo_load(void); void OVL_gameinfo_desc_full(void);
__attribute__((section(".ovlhdr"), used)) const struct { ovl_hdr_t h; uint32_t e[16]; } ovl_hdr1 = {
  { OVL_MAGIC(1), 0, 0, 0 },
  { (uint32_t)OVL_theme_apply, (uint32_t)OVL_theme_font_edges_stale,
    (uint32_t)OVL_theme_font_edges, (uint32_t)OVL_theme_font_file, (uint32_t)OVL_font_select,
    (uint32_t)OVL_theme_clear, (uint32_t)OVL_theme_pick, (uint32_t)OVL_theme_select,
    (uint32_t)OVL_sysinfo_open, (uint32_t)OVL_load_cover, (uint32_t)OVL_load_cover_path,
    (uint32_t)OVL_sysinfo_pass, (uint32_t)OVL_load_spc, (uint32_t)OVL_menu_spc_pick_random,
    (uint32_t)OVL_gameinfo_load, (uint32_t)OVL_gameinfo_desc_full } };
