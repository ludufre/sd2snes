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

/* Apply the theme whose full SD path is in CFG.skin_name onto the just-loaded
 * menu image in PSRAM (SRAM_MENU_ADDR). Call AFTER load_rom(MENU_FILENAME, ...)
 * and BEFORE the SNES is released. A missing/invalid theme leaves the baked
 * menu intact; never hangs the MCU. */
void theme_apply(void);

/* Persist the chosen theme. `name` is the full SD path of a .thm (as returned
 * by get_selected_name) or NULL/empty to clear back to the baked default.
 * Updates CFG.skin_name and saves config. */
void theme_select(const char *name);

#endif
