/* sd2snes fork -- in-game TAB menu (igmenu.bin) loader.

   The in-game overlay tab shell (tab bar + tabs 2..5) lives in a SEPARATE SNES-side
   binary, snes/igmenu.a65 -> igmenu.bin, org'd for PSRAM bank $C2 and loaded here at
   every game-load into SRAM_DIR_ADDR (0xC20000, the menu's directory buffer -- dormant
   during a game). The $C0 cheat overlay dispatches into it (jsl $C20008) only after
   BOTH the IGMENU_GATE flag (armed here) and the bin's ABI version word validate;
   any failure falls through to the proven single-tab cheat overlay. igmenu_stage() is
   100% bounded and NEVER aborts the game-load (a bad/absent bin just leaves the gate 0).

   File layout (offset 0 = $C20000): "IGMN"(4) version(1) rsvd(1) crc16_le(2) body...
   The crc16 covers the body from offset 8 (jml + code); see build step in snes/Makefile.
   Lockstep: SRAM_IGMENU_GATE_ADDR + IGMENU_ABI_VERSION in memory.h; IGMENU_* in memmap.i65. */

#ifndef IGMENU_H
#define IGMENU_H

#define IGMENU_FILENAME   "/sd2snes/igmenu.bin"
#define IGMENU_MAX_BYTES  (0x10000L)   /* one PSRAM bank; a larger file is rejected */

/* Stage + validate igmenu.bin into PSRAM $C20000 and arm SRAM_IGMENU_GATE_ADDR on
   success (0 on any failure). Bounded; safe to call every game-load. */
void igmenu_stage(void);

#endif
