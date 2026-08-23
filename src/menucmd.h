/* sd2snes - menu command handlers (MCU side)
 *
 * Handlers whose body this fork rewrote live here; main.c keeps the ones still in
 * upstream's form, plus any that need one of main()'s locals (SAVE_CFG needs
 * cic_state, SETRTC needs btime).  The split keeps src/main.c close to upstream.
 */

#ifndef MENUCMD_H
#define MENUCMD_H

#include <stdint.h>

/* Runs BEFORE the menu switch, for EVERY command: stops a running info-screen
   FMV unless the command is one the info screen itself issues. */
void menucmd_fmv_gate(uint8_t cmd);

/* LOADROM / LOADLAST / LOADFAVORITE / LOAD_AUTOBOOT.  Returns the loaded size
   (the game booted -> leave the menu loop) or 0 when the load was aborted, in
   which case the NACK has already been sent and the caller stays in the menu. */
uint32_t menucmd_launch_rom(uint8_t cmd);

/* Every other command this fork added.  Returns 0 to stay in the menu loop,
   or `cmd` to leave it -- which only the handlers that ask for a menu reload
   do (*menu_reload = 1).  An unknown command is reported and returns 0. */
uint8_t menucmd_dispatch(uint8_t cmd, uint8_t *menu_reload);

/* Boot-time epilogue of "create patched ROM": park the browser on the file the
   export just wrote.  Runs once per menu boot, after the game lists are dumped. */
void menucmd_export_boot_nav(uint8_t firstboot);

#endif
