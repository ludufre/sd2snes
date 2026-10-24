#ifndef GAMEINFO_INT_H
#define GAMEINFO_INT_H
/* Between the two halves of the info screen: the resident one (gameinfo.c: glyph tables,
   clip pump, ROM CRC job) and the loader on the card (giload.c, see ovl.h). */
#include <stdint.h>
#include "gameinfo.h"

#define GI_CJK_MAX   128
extern uint16_t gi_cjk_cp[GI_CJK_MAX];  /* this game's ideographs, by index */
extern uint8_t  gi_cjk_n;
extern const char *gi_cjk_code;         /* "ja"/"zh" while the menu language is CJK, else NULL */
extern char     gi_fmv_path[300];       /* the open clip, for a reopen ("" = none) */
extern char     gi_yml_path[300];       /* the last-loaded .yml, for the full description */

int  gi_cp_emit(uint32_t cp, uint8_t *out);           /* codepoint -> 0..3 font bytes */
void gi_fmv_close(void);
int  gi_fmv_begin(const char *fmvpath, gameinfo_meta_t *meta);
void gi_crc_start(const uint8_t *rom_path);           /* takes the FRAME over */
#endif
