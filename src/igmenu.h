/* sd2snes fork -- in-game TAB menu (igmenu.bin) loader.

   The in-game overlay tab shell (tab bar + tabs 2..5) lives in a SEPARATE SNES-side
   binary, snes/igmenu.a65 -> igmenu.bin, org'd for PSRAM bank $C8 and loaded here at
   every game-load into SRAM_IGMENU_ADDR (0xC80000 -- a bank that is idle in BOTH
   modes, so the 3-bank menu image at $C0-$C2 stays resident in-game). The $C0 cheat
   overlay dispatches into it (jsl $C80008) only after
   BOTH the IGMENU_GATE flag (armed here) and the bin's ABI version word validate;
   any failure falls through to the proven single-tab cheat overlay. igmenu_stage() is
   100% bounded and NEVER aborts the game-load (a bad/absent bin just leaves the gate 0).

   File layout (offset 0 = $C80000): "IGMN"(4) version(1) rsvd(1) crc16_le(2) body...
   The crc16 covers the body from offset 8 (jml + code); see build step in snes/Makefile.
   Lockstep: the gate byte SRAM_IGMENU_GATE_ADDR lives with the rest of the address map
   in src/memmap.h; IGMENU_* in snes/memmap.i65. */

#ifndef IGMENU_H
#define IGMENU_H

#define IGMENU_FILENAME   "/sd2snes/igmenu.bin"
#define IGMENU_MAX_BYTES  (0x10000L)   /* one PSRAM bank; a larger file is rejected */
#define IGMENU_ABI_VERSION           (4)         /* igmenu.bin ABI version, checked by igmenu_stage (MCU) AND the overlay ($C80004). Bump on ANY igmenu.bin layout/ABI change. v4 moved the shell from PSRAM $C2 to $C8 (the menu image grew to 3 banks and $C2 is now menu-resident in-game); a v3 bin on the card fails the version check and the overlay falls through to the single-tab fail-safe instead of jsl-ing into the old bank. Lockstep with IGMENU_ABI_VERSION in snes/memmap.i65. Common to all 3 configs (NOT in config-mk*). */

/* Stage + validate igmenu.bin into PSRAM $C80000 and arm SRAM_IGMENU_GATE_ADDR on
   success (0 on any failure). Bounded; safe to call every game-load. */
void igmenu_stage(void);

#endif
