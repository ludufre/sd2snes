/* sd2snes (ludufre fork) - menu THEME loader (MCU side).
 *
 * A theme (/sd2snes/theme/<name>.thm) overrides the menu's gfxptr-marked
 * regions (palette, background HDMA, logo, ...). It is applied by locating the
 * loaded menu's `_GFXPTR_` table in PSRAM and overwriting each referenced
 * region in place, right after the menu is loaded and before the SNES runs it
 * (the menu's own setup_gfx then DMAs the patched palette/logo). See theme.c
 * for the .thm format and the hard safety contract (bounded, fail-safe). */
#ifndef _THEME_H
#define _THEME_H

/* CFG.skin_name sentinel meaning "no theme / baked-in default look". Any value
 * that is not an absolute path (does not start with '/') is treated as "no
 * theme", so themes may live in ANY visible SD folder (the hidden /sd2snes
 * folder is not browsable, so themes go e.g. in /Themes at the card root). */
#define THEME_DEFAULT    "sd2snes.skin"

/* The classic (pre-sd2snes+) look. It used to BE the baked default; now that the
 * baked regions carry the sd2snes+ theme it ships as a regular theme file, applied
 * by the "Restore classic theme" menu entry (SNES_CMD_RESTORE_CLASSIC). Lives in
 * the hidden /sd2snes folder -- not browsable, but the MCU opens it by full path,
 * and theme_apply only requires skin_name to start with '/'. Shipped by build.sh
 * (mcu_artifact_list) like menu.spc and the sfx_*.pcm. */
#define THEME_CLASSIC    "/sd2snes/classic.thm"

/* Apply the theme whose full SD path is in CFG.skin_name onto the just-loaded
 * menu image in PSRAM (SRAM_MENU_ADDR). Call AFTER load_rom(MENU_FILENAME, ...)
 * and BEFORE the SNES is released. A missing/invalid theme leaves the baked
 * menu intact; never hangs the MCU. */
void theme_apply(void);

/* Apply the menu font's edge remaps (outline ring / anti-alias step) to the
 * just-loaded menu image in PSRAM. Call right AFTER theme_apply, from the same
 * spot: it ORs the .thm font flags theme_apply published with the user options
 * CFG.text_outline / CFG.text_antialias, so the toggles work with AND without a
 * theme. A no-op (not even a _GFXPTR_ scan) when neither source asks for a
 * remap, which is the default. Bounded and fail-safe like theme_apply. */
void theme_font_edges(void);

/* 1 when the effective font-edge state (theme flags OR the user toggles) no
 * longer matches what theme_font_edges last wrote into the PSRAM font. The
 * remap is destructive -- turning an edge back ON needs a fresh menu image --
 * so the SAVE_CFG handler uses this to ask for a menu reload, which is what
 * makes the two toggles take effect as soon as the options screen is left. */
int theme_font_edges_stale(void);

/* Values of CFG.text_outline_mode / CFG.text_antialias_mode.  THEME is 0 so that a
 * config.yml written before the option existed -- and any value out of range, which
 * cfg.c clamps to 0 -- lands on the historical behaviour. */
#define TEXT_EDGE_THEME  0
#define TEXT_EDGE_ON     1
#define TEXT_EDGE_OFF    2

#include <stdint.h>
#include "ff.h"

/* What the catalog appends to a .thm (format in theme.c). */
#define THEME_SLOT_FONT     13
#define THEME_SLOT_MUSIC    14
#define THEME_SLOT_SFX      15   /* cursor, confirm, back, error */
#define THEME_SLOT_PREVIEW  19
#define THEME_SLOT_GAME_GRAD 20  /* gradient of the game info screen and the in-game menu */

/* theme_info's answer, at SRAM_COVER_ADDR + THEME_INFO_OFF (past the cover's 8-byte meta). */
#define THEME_INFO_OFF      0x3A10
#define THEME_HAS_FONT      0x01
#define THEME_HAS_SOUND     0x02
#define THEME_HAS_PICTURE   0x04
/* ...followed by "TI": the menu cannot clear bank $C9, so an older firmware (which leaves
   whatever was there) is told apart by these two bytes, and the menu then applies directly. */
#define THEME_INFO_MAGIC    "TI"

/* The answers of the menu's box, MCU_PARAM+8 of SNES_CMD_SET_THEME. */
#define THEME_ANSWER_FONT   0x01
#define THEME_ANSWER_SOUND  0x02   /* music and effects: sets theme_use_music and theme_use_sound */

#define THEME_VERSION   2       /* highest version read; 1 = no background */
#define THEME_NREGIONS  21      /* slots a .thm may carry: the 13 of the menu + the catalog's 7 + slot 20 */

/* themereg.c, resident */
uint32_t theme_toc_len(const uint8_t *e);
int theme_region_open(FIL *f, const char *path, uint8_t slot, uint32_t *len);
int theme_sound_open(FIL *f, uint8_t slot, uint32_t *len);
int theme_sfx_open(FIL *f, const char *name, uint32_t *len);
int theme_music_load(void);
void theme_info(const char *path);

/* Persist the chosen theme. `name` is the full SD path of a .thm (as returned
 * by get_selected_name) or NULL/empty to clear back to the baked default.
 * Updates CFG.skin_name and saves config. */
void theme_select(const char *name);

/* Overwrite the font of the just-loaded menu image in PSRAM with the .sfn whose
 * full SD path is in CFG.font_name (format in theme.c). Call after theme_apply
 * and before theme_font_edges, which then takes the new font as the pristine
 * one. Not an absolute path, a missing or a bad file = the menu's own font. */
void theme_font_file(void);

/* Persist the chosen .sfn (full SD path, or NULL/empty for the menu's own font)
 * in CFG.font_name and save config. */
void font_select(const char *name);

/* The browser's A on a TYPE_SKIN entry: a .sfn goes to font_select, anything
 * else (.thm/.skin) to theme_select. */
void theme_pick(const char *name);

/* theme_select(NULL), or with `font` set font_select(NULL): the two "Restore"
 * entries of the browser settings. */
void theme_clear(int font);

#endif
