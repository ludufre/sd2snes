/* The regions of a .thm, resident: the menu's effects are preloaded from the idle loop, which can
 * run while READDIR is using ptrcache, so this path must never load an overlay (ovl.h). The format
 * is in theme.c, the overlay that reads the rest. */
#include "config.h"
#include <string.h>
#include "ff.h"
#include "cfg.h"
#include "theme.h"

extern cfg_t CFG;

uint32_t theme_toc_len(const uint8_t *e) {
  return ((uint32_t)e[1] << 16) | ((uint32_t)e[3] << 8) | e[2];
}

/* Leave `f` open at the payload of `slot` in the .thm at `path`. 0 (and closed) when the
   file is not a theme or does not carry that slot. Takes the FIL because the effects
   preload runs on its own while file_handle may be in use. */
int theme_region_open(FIL *f, const char *path, uint8_t slot, uint32_t *len) {
  uint8_t e[16];
  UINT got;
  if(f_open(f, (const TCHAR*)path, FA_READ) != FR_OK) return 0;
  if(f_read(f, e, 16, &got) == FR_OK && got == 16 && !memcmp(e, "FXTHEME1", 8)
     && e[8] && e[8] <= THEME_VERSION && e[9] <= THEME_NREGIONS) {
    uint32_t off = 16 + (uint32_t)e[9] * 4;
    for(uint8_t n = e[9]; n; n--) {
      if(f_read(f, e, 4, &got) != FR_OK || got != 4) break;
      uint32_t l = theme_toc_len(e);
      if(e[0] == slot) {
        if(off + l > f->fsize || f_lseek(f, off) != FR_OK) break;
        *len = l;
        return 1;
      }
      off += l;
    }
  }
  f_close(f);
  return 0;
}

/* What the chosen theme brings, for those who read it at run time. The music (slot 14) and the
   effects (15-18) answer to separate switches: picking a .spc gives up only the theme's music. */
int theme_sound_open(FIL *f, uint8_t slot, uint32_t *len) {
  return (slot == THEME_SLOT_MUSIC ? CFG.theme_use_music : CFG.theme_use_sound)
         && CFG.skin_name[0] == '/'
         && theme_region_open(f, (const char*)CFG.skin_name, slot, len);
}

/* An effect of the chosen theme instead of the .pcm of that name. */
int theme_sfx_open(FIL *f, const char *name, uint32_t *len) {
  static const char *const sfx[4] = { "cursor", "confirm", "back", "error" };
  for(uint8_t i = 0; i < 4; i++) {
    if(strstr(name, sfx[i])) return theme_sound_open(f, THEME_SLOT_SFX + i, len);
  }
  return 0;
}

