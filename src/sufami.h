/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

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

   sufami.c: Sufami Turbo (Bandai) Slot B minicart -- battery SRAM autosave

   Each .st minicart carries its own battery-backed SRAM, and the linkable titles
   write into the OTHER slot's cart.  The SaveRAM scan in snes.c covers ONE region
   with ONE CRC, so a write landing only in Slot B is invisible to it: Slot B gets
   its own chunked scan, debounce and file.
*/

#ifndef SUFAMI_H
#define SUFAMI_H

#include "config.h"
#include <stdint.h>

/* Diagnostics, in the DBG_FS/DBG_YAML shape: on by default, dropped on the mk2 where the
   flash has no room.  Define DEBUG_SUFAMI to force them back on there. */
#if defined(CONFIG_MK2) && !defined(DEBUG_SUFAMI)
#define DBG_SUFAMI while(0)
#else
#define DBG_SUFAMI
#endif

/* The Slot B candidate list is published through the IPS patch contract (src/patch.h):
   identical shape, so the menu reuses the patch dialog instead of carrying a second one. */

/* sel value meaning "no selector ran -- fall back to the .stb sidecar".  Used by the
   Recents / Favorites / autoboot paths, which never open the selector. */
#define SUFAMI_SEL_SIDECAR  0xFF

/* Set at game load; cleared on the way back to the menu. */
extern uint32_t sufami_slotb_ramsize;  /* Slot B SAVEABLE bytes; 0 = no Slot B, or a
                                          cart with no battery -> never write a file */
extern uint32_t sufami_rom_mask_b;     /* power-of-two size mask of the Slot B ROM; 0 = empty */

/* Resolve which minicart goes into Slot B for this load and remember it.
   `rom_path` is the Slot A cart; a non-.st path makes this a no-op that just clears
   the state.  `sel` is 0 = explicitly none, 1..N = index into the list published by
   the patch query, or SUFAMI_SEL_SIDECAR to reuse whatever the sidecar holds.
   An explicit selection is persisted to the sidecar; MUST run before load_rom. */
void     sufami_stage_slotb(const uint8_t *rom_path, uint8_t sel);

/* Full SD path of the staged Slot B cart, or "" when there is none. */
extern char sufami_slotb_path[];

/* Present the Slot B window as an EMPTY slot: zero the masks and blank the ROM header
   area, so the ST BIOS cannot match the signature and offer a cart that is not there. */
void     sufami_slotb_empty(void);

/* Stage the Slot B minicart ROM into PSRAM and derive its masks.  A missing or
   unreadable cart degrades to an empty slot. */
void     sufami_stage_slotb_rom(void);

/* Scan the directory holding `rom_path` for other .st minicarts and publish them to
   the IPS patch contract (SRAM_IPS_LIST_ADDR / SRAM_IPS_TEXT_ADDR) with the list
   flagged IPS_DLGMODE_SLOTB, so the menu's existing patch dialog draws it.  Runs in
   place of ips_find_patches on the browser's CMD_QUERY_IPS_PATCHES round trip -- a
   .st never carries a patch, so the two never both apply.
   `rom_path` must NOT be file_lfn -- f_readdir overwrites that buffer. */
void     sufami_query_slotb(const uint8_t *rom_path);

/* Seed the CRC baseline right after the Slot B .srm has been staged, so an
   untouched cart is never rewritten.  Safe to call with no Slot B. */
void     sufami_slotb_crc_seed(void);

/* One slice of the chunked autosave scan.  Call from BOTH game loops (snes_main_loop
   and msu1_loop): the MSU loop is a parallel clone. */
void     sufami_slotb_autosave(void);

/* Final flush with the SNES already in reset (calc_sram_crc bails out there, so this
   reads raw).  Writes only when the cart actually changed. */
void     sufami_slotb_flush_inreset(void);

/* Drop all Slot B state.  Call AFTER the final flush. */
void     sufami_clear(void);

#endif
