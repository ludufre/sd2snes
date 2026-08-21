/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

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

   memory.h: RAM operations
*/

#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include CONFIG_MCU_H
#include "smc.h"

extern char current_filename[];

#define MENU_ADDR_BRAM_SRC           (0xFF00)
/* 4-byte capability marker the menu image carries inside its own header area: 'S','2',
   capability version, reserved. Read ONCE per SNES_CMD_SYSINFO entry (sysinfo.c). A menu
   built before the v2 sysinfo block has plain code bytes there, so its absence means "this
   menu still expects 13 pre-formatted text lines" and the firmware writes the legacy
   update-your-menu line instead. Lockstep with MENU_CAP_MARKER in snes/memmap.i65. */
#define MENU_ADDR_SYSINFO_CAPS       (0xFF02)
#define MENU_CAP_MAGIC0              ('S')
#define MENU_CAP_MAGIC1              ('2')
#define MENU_CAP_SYSINFO_VER         (1)  /* lowest marker version that consumes sysinfo_blk_t */

#define SRAM_ROM_ADDR                (0x000000L)
#define SRAM_SAVE_ADDR               (0xE00000L)
/* Sufami Turbo Slot B battery SRAM: 512 KB above the Slot A window (SRAM_SAVE_ADDR)
   so the two minicarts can never alias whatever sizes they declare.  address.v
   derives the same base by ORing bit 19 into SAVERAM_ADDR, which has it hardwired
   to 0.  Lockstep with ST_SAVERAM_B in verilog/sd2snes_base/address.v. */
#define SUFAMI_SLOTB_SAVE_ADDR       (0xE80000L)
/* Minicart ROM windows, one megabyte each, on boundaries the FPGA can OR in rather
   than add.  The BIOS sits at 0 masked to its own 256 KB.
   Lockstep with the 24'h100000 / 24'h700000 constants in address.v. */
#define SUFAMI_SLOTA_ROM_ADDR        (0x100000L)
#define SUFAMI_SLOTA_BIOS_ADDR       (0x000000L)
#define SUFAMI_SLOTB_ROM_ADDR        (0x700000L)
/* A minicart is at most 1 MB (header byte 0x36 counts 128 KB units); the FPGA ORs
   the window base in, so a bigger mask would alias into it. */
#define SUFAMI_ROM_MASK_MAX          (0x0FFFFFL)
/* Even a minicart that declares no battery gets 8 KB mapped: the ST BIOS dispatches
   INTO Slot A SaveRAM (below $8000), so an unmapped window hangs the boot.  Mapped is
   not saved -- romprops.sramsize_bytes stays 0 and no file is made. */
#define SUFAMI_SLOTA_SCRATCH_SIZE    (0x2000L)

#define SRAM_MENU_ADDR               (0xC00000L)
#define SRAM_DIR_ADDR                (0xC20000L)
#define SRAM_DB_ADDR                 (0xC80000L)
#define SRAM_COVER_ADDR              (0xC90000L) /* bank C9: per-ROM cover preview staging */
#define SRAM_GAMEINFO_TILES_ADDR     (0xCA0000L) /* bank CA: game-info DirectColor 8bpp tiles (up to ~48KB) */
#define SRAM_GAMEINFO_TMAP_ADDR      (0xCB0000L) /* bank CB: game-info 16-bit BG tilemap */
#define SRAM_MENU_SFX_ADDR           (0xCC0000L) /* banks CC..CF (256 KB, free during menu, below cheats @D0):
                                                    4x 64KB slots holding the preloaded nav-SFX PCM bodies the
                                                    FPGA sfxdma engine streams into the DAC (msu1.c menusfx). */

/* 0xFF0C00-0xFF0C07: savestate diagnostics (reject counter + watchdog breadcrumb +
   partial-sample gate breadcrumbs + the fill-wait throttle counter), owned and zeroed
   by the menu handler (ss_init, snes/savestate.i65). Reserved here so MCU-side
   allocations skip the range. */
#define SRAM_NUM_CHEATS              (0xFF0700L)
#define SS_SCENE_GATE_ADDR           (0xFF0702L) /* 1 byte armed on EVERY game load (savestate.c, before the core gate, so it is never stale) = "this game needs the overlay probe gated on scene liveness". Keyed by core+checksum; today only Super Mario RPG (US) on the SA-1 core. When 1, the probe in snes/savestate.a65 additionally requires the FPGA's scene_fresh bit at $F90720 (cheat.v: the S-CPU wrote the pad to SA-1 IRAM $3010/$3011 or polled $4218/$4219 in the last ~49-73ms) before it opens the overlay -- the NMI hook also fires during scene transitions where the frame loop is parked mid-RPC, and opening there hangs the game on resume (proven in hardware on Mk.II). Takes the first byte of the $FF0702..$FF0707 gap (NUM_CHEATS is a short at $FF0700-01; CHEAT_WIN_BASE follows at $FF0708); $FF0704..$FF0707 now hold the in-game menu combo pair, so the gap is FULL. Lockstep with SS_SCENE_GATE in snes/memmap.i65. */
#define SS_OVL_APUFIX_GATE_ADDR      (0xFF0703L) /* 1 byte armed on EVERY game load (savestate.c) = "re-run this game's savestate_fixes.yml code when the in-game overlay CLOSES". Keyed by header checksum only, like the yml itself; today only Star Ocean (13B8). The fix blobs re-sync a WRAM shadow of the APU handshake with the live $214x port, but until now they only ran on savestate save/load (audio_fix in snes/savestate.a65). Star Ocean spins on that shadow with an UNBOUNDED loop inside its V-IRQ handler (LDA $2140 : CMP $2140 : BNE : EOR $4A : BPL, three sites reached from $C0:0221), so any drift across an overlay open/close deadlocks the handler on resume -- the picture stays exactly as the overlay restored it and the game never repopulates OAM (its sprites vanish), which is the reported field-scene freeze. Running the blob once more on close makes the spin pass. Opt-in per game on purpose: a blob is free-form code (some entries write immediates, one writes $2140 itself), so firing it on every overlay close for the whole library would be a far wider behaviour change. MERGE HAZARD: the v2.16 branch uses 0xFF0703 as the savestate-thumbnail validity mask -- whichever side merges second must move one of the two. Lockstep with SS_OVL_APUFIX_GATE in snes/memmap.i65. */
#define SRAM_MENU_COMBO_ADDR         (0xFF0704L) /* 2 bytes armed on every game load (cheat.c) = the pad combo that opens the in-game menu (CFG.ingame_buttons_menu, default $4230). ss_init patches it into the two probe operands; the mid-frame IRQ pre-check reads this address directly, since it can run before ss_init does. Never 0 (cfg_load rejects it): a zero mask matches every entry and kills the anti-freeze fast exit. Lockstep with MENU_COMBO in snes/memmap.i65. */
#define SRAM_MENU_COMBO_INV_ADDR     (0xFF0706L) /* 2 bytes = ~SRAM_MENU_COMBO_ADDR. Stale-publication check: a new m3nu.bin can run against an old MCU that never writes here, and this region survives resets, so a nonzero word alone proves nothing. ss_init requires (combo ^ inv) == $FFFF and uses $4230 otherwise. Fills the $FF0704..$FF0707 gap -- next allocation goes at $FF0718 or above. Lockstep with MENU_COMBO_INV in snes/memmap.i65. */
#define SRAM_CHEAT_WIN_BASE_ADDR     (0xFF0708L) /* in-game cheat overlay: absolute base index of the 64-name window RESIDENT in SRAM_CHEAT_NAMES_ADDR ($FF8000). The MCU is the sole writer (base 0 at game load, the requested base on each CMD_CHEAT_NAMES_WINDOW); the overlay reads it to map an absolute cheat index to its window slot ((idx - base)*CHEAT_NAME_LEN). In the $FF0702..$FF070F gap (SS_SCENE_GATE takes $FF0702, SS_OVL_APUFIX_GATE $FF0703, the in-game menu combo pair $FF0704..$FF0707 -- that gap is now full). Lockstep with CHEAT_WIN_BASE in snes/memmap.i65. */
#define SRAM_CHEAT_OVL_GATE_ADDR     (0xFF0710L) /* 1 byte the firmware arms at game load = CFG.enable_cheat_overlay (user toggle only -- the per-core gate is core_has_snapshot in savestate.c, which decides whether the handler is installed at all). The in-game overlay probe (snes/savestate.a65) reads it; 0 => don't open. Lives in the free $FF0701..$FF07FF gap between NUM_CHEATS and CHEAT_NAMES. */
#define SRAM_PPU_CLEAR_GATE_ADDR     (0xFF0711L) /* 1 byte the firmware arms in load_rom (before releasing the SNES) = CFG.clear_ppu_on_boot && ips_pending_index>0. game_handshake (snes/main.a65) reads it before boot; 1 => clear VRAM/CGRAM/OAM for a patched romhack that skips PPU init. Lockstep with PPU_CLEAR_GATE in snes/memmap.i65; same free $FF0701..$FF07FF gap. */
#define SS_DSP_GATE_ADDR             (0xFF0712L) /* 1 byte armed at game load = (fpga_conf==FPGA_DSP && !has_st0010). The savestate handler reads it to decide whether to capture/restore the DSP1-4 internal state. Lockstep with SS_DSP_GATE in snes/memmap.i65; same free $FF0701..$FF07FF gap. */
#define SS_SA1_GATE_ADDR             (0xFF0713L) /* 1 byte armed at game load = (Mk.III && fpga_conf==FPGA_SA1). The savestate handler reads it to decide whether to capture/restore the SA-1 internal state (IRAM/BW-RAM/register-file). 0 on Mk.II (the SA-1 FPGA savestate window constant-folds away there). Lockstep with SS_SA1_GATE in snes/memmap.i65; same free $FF0701..$FF07FF gap. */
#define SS_GSU_GATE_ADDR             (0xFF0714L) /* 1 byte armed at game load = (fpga_conf==FPGA_GSU), on Mk.II AND Mk.III -- the GSU savestate window is un-gated from ifdef MK3, unlike SA-1. Lockstep with SS_GSU_GATE in snes/memmap.i65; same free $FF0701..$FF07FF gap. */
#define SRAM_IGMENU_GATE_ADDR        (0xFF0715L) /* 1 byte the firmware arms in igmenu_stage() at game load when /sd2snes/igmenu.bin is present AND validates (magic "IGMN" + version == IGMENU_ABI_VERSION + crc16). The in-game overlay hook ($C0) reads it; 0 => single-tab fail-safe. Free $FF0701..$FF07FF gap, after SS_GSU_GATE. Lockstep with IGMENU_GATE in snes/memmap.i65. */
#define SRAM_SS_SLOT_STATUS_ADDR     (0xFF0716L) /* 1 byte: bitmask of existing savestate files, bit N-1 = slot N (1..4) has <rom>0N.state on SD. Staged by the firmware at game load (savestate.c), read by the in-game STATES tab (igmenu.bin). Lockstep with SS_SLOT_STATUS in snes/memmap.i65. */
#define SRAM_SRM_SLOT_STATUS_ADDR    (0xFF0717L) /* 1 byte: bitmask of existing battery-SRAM slot files. When EnableSramSlots is ON, bit i = slot i (i=0..3: <stem>.srm, <stem>.02/03/04.srm) exists on SD; when OFF, bit0 = the legacy <stem>.srm exists (bits1-3 clear). Staged by saveinfo_stage() at game load and refreshed after autosave; read by the in-game SAVES tab (igmenu.bin). Free $FF0701..$FF07FF gap, after SS_SLOT_STATUS. Lockstep with SRM_SLOT_STATUS in snes/memmap.i65. */
#define SRAM_SAVEINFO_ADDR           (0xFF0730L) /* in-game SAVES tab (igmenu.bin) staging block, 48 bytes ($FF0730-$FF075F -- ABOVE the BPS/copier breadcrumbs $FF0720-$FF072E, below the free tail of the $FF0701..$FF07FF gap). Layout: +0 flags (bit0 = game has SRAM, bit1 = .srm exists on SD, bit2 = autosave enabled), +1 size string (16B ASCII NUL-term, e.g. "8 KB"), +17 datetime string (24B ASCII NUL-term, e.g. "2026-07-16 11:30"), +42 = selected/next SRAM slot (0..3; sidecar value, 0 when EnableSramSlots OFF), +41/+43..47 reserved. The slot occupancy bitmask lives separately at SRAM_SRM_SLOT_STATUS_ADDR $FF0717. Strings pre-formatted by the MCU (ASCII == font codes for digits/letters). Staged at game load; refreshed after a successful in-game autosave. Lockstep with SAVEINFO in snes/memmap.i65. */
#define IGMENU_ABI_VERSION           (3)         /* igmenu.bin ABI version, checked by igmenu_stage (MCU) AND the overlay ($C20004). Bump on ANY igmenu.bin layout/ABI change. v3 requires the theme block the $C1 overlay stages into PSRAM $F4 (IGM_THM_* in snes/memmap.i65) before dispatching the shell. Lockstep with IGMENU_ABI_VERSION in snes/memmap.i65. Common to all 3 configs (NOT in config-mk*). */
/* --- in-game manual viewer, SCROLLABLE 1x page (scale-1 twin of the 2x page). 256px wide = 32
 * tiles = 1024B per row, so a 29-row ring is 928 tiles and fits ONE BG layer -- no split, no
 * window, unlike the 2x. A whole page (<=MAN_S1_MAX_ROWS=64 rows = 512px at 1x) fits in bank C3,
 * which is exactly the space the retired 8bpp block used to occupy; 1024 divides 65536, so no row
 * ever straddles a bank. 1x and 2x are BOTH resident, so toggling between them is instant. */
#define SRAM_MANUAL_S1TILES_ADDR     (0xC30000L) /* scale-1 tile row r at +r*1024. Bank C3 is menu dir-cache territory, dormant in-game (same rationale as igmenu.bin @ C2). Lockstep with MANUAL_S1TILES in snes/memmap.i65. */
#define SRAM_MANUAL_S1TMAP_ADDR      (0xC4B000L) /* prebuilt tilemap, row r at +r*64 (32 entries) */
#define SRAM_MANUAL_S1PAL_ADDR       (0xC4C000L) /* 8 palettes x 16 BGR555 -> CGRAM 0..127 */
#define SRAM_MANUAL_SHELLSAVE_ADDR   (0xC40000L) /* in-game manual viewer: the $C2 shell saves its mode-5 state here before taking the PPU ($2139/$213B readback: VRAM $0000-$5FFF words + CGRAM 512B) and restores from it on viewer exit. Bank C4, dormant in-game. Lockstep with MANUAL_SHELLSAVE in snes/memmap.i65. */
/* --- in-game manual viewer, SCROLLABLE 2x zoom page (banks C5/C6, dormant in-game like C2/C3/C4).
 * ONE whole 2x page (512 x up-to-448, 4bpp, <=56 tile rows) is staged here by manual_stage_zpage on
 * SNES_CMD_MANUAL_ZPAGE, so panning over it costs the SNES nothing but PSRAM->VRAM DMA -- there is
 * NO per-row MCU traffic, which is what makes the pan smooth and unable to stall.
 * The 512px width does not fit one BG layer (a tilemap entry's character field is 10 bits = 1024
 * tiles max), so each tile row is split: cols 0-31 on BG1, cols 32-63 on BG2, joined by a window.
 * No DMA ever crosses a bank boundary (the A-bus does not increment the bank). Lockstep with the
 * MANUAL_Z* defines in snes/memmap.i65. */
/* A zoom page is a WHOLE PDF PAGE at 2x (up to MAN_Z_MAX_ROWS=96 tile rows = 768px), NOT a 1x
 * band -- cutting it at band boundaries made the view JUMP half-way down every page. The two
 * tile halves are sized so they exactly fill $C50000..$C7FFFF, and 1024 divides 65536, so no
 * row ever straddles a bank (the DMA A-bus does not increment the bank). The tilemap and
 * palette live in the $C46200..$C4FFFF hole above the shellsave. */
#define SRAM_MANUAL_ZTILES_A_ADDR    (0xC50000L) /* BG1 half (cols 0-31) of tile row r at +r*1024; 96 rows -> $C50000-$C67FFF (bank break at row 64, exact) */
#define SRAM_MANUAL_ZTILES_B_ADDR    (0xC68000L) /* BG2 half (cols 32-63) of tile row r at +r*1024; 96 rows -> $C68000-$C7FFFF (bank break at row 32, exact) */
#define SRAM_MANUAL_ZTMAP_ADDR       (0xC47000L) /* prebuilt tilemap entries, row r at +r*128 = 32 words BG1 then 32 words BG2 (entry = tile | pal<<10, tile = MAN_Z_TILE0 + (r % MAN_Z_RING_ROWS)*32 + col). The MCU bakes these so the SNES never builds map words on the CPU mid-scroll. 96 rows = 12288B. */
#define SRAM_MANUAL_ZPAL_ADDR        (0xC4A000L) /* 8 palettes x 16 BGR555 = 256B -> CGRAM 0..127 */
#define SRAM_MANUAL_META_ADDR        (0xFF0760L) /* in-game guides viewer meta block, 16B staged at game load by manual.c (after SAVEINFO $FF0730-5F): +0 flags (bit0 = >=1 valid guide present, bit1 = transient read-error latch read by the viewer, bit2 = a zoom page is staged and ready in $C5/$C6), +1 npages of guide 0, +2 meta_abi (=2, firmware<->igmenu.bin data-contract sanity; the shell degrades to "no guides" if != this), +3 staged 1x page, +4 block-in-page, +5 content_rows, +6 zoom nrows (tile rows in the staged zoom page, 1..56), +7..8 zoom pix_h u16 LE (2x pixel height -- the viewer clamps vertical scroll to pix_h-224 so padding never shows), +9 staged zoom page, +10 staged zoom guide, +11..15 reserved. Per-guide metadata (nblocks/zoom/titles) lives in MANUAL_GUIDES $FF9000. Read by the GUIDES tab. Lockstep with MANUAL_META in snes/memmap.i65. */
#define SS_OBC1_GATE_ADDR            (0xFF0718L) /* 1 byte armed at game load = (fpga_conf==FPGA_OBC1), on Mk.II AND Mk.III -- the OBC1 is purely reactive, so the handler captures/restores the SNES-visible $7800-$7FFF window directly over the bus (no chip halt/scan window). Free $FF0701..$FF07FF gap, after SRM_SLOT_STATUS. Lockstep with SS_OBC1_GATE in snes/memmap.i65. */
#define SS_SDD1_GATE_ADDR            (0xFF0719L) /* 1 byte armed at game load = (fpga_conf==FPGA_SDD1), on Mk.II AND Mk.III -- the S-DD1 decompressor FSM is never mid-transfer at an NMI boundary, so the handler captures/restores the bus-visible config block ($4800-$4807) directly over the bus (no chip halt/scan window). Free $FF0701..$FF07FF gap, after SS_OBC1_GATE. Lockstep with SS_SDD1_GATE in snes/memmap.i65. */
#define SS_CX4_GATE_ADDR             (0xFF071FL) /* 1 byte armed at game load = (fpga_conf==FPGA_CX4), on Mk.II AND Mk.III. The savestate handler reads it to decide whether to capture/restore the CX4: the core is asked to halt early and runs to its next clean boundary (run-to-stop), then the handler reaches its state through the $E8:00xx scan window plus the native $00:6000-$7FFF space, and REPLAYS the program cache instead of capturing it. Free $FF0701..$FF07FF gap, after EXPORT_RESULT -- the last byte before the BPS breadcrumbs at $FF0720. Lockstep with SS_CX4_GATE in snes/memmap.i65. */
#define SRAM_LOAD_NACK_ADDR          (0xFF071DL) /* 1 = this game load was refused (missing chip BIOS / unimplemented chip). Set by load_abort_missing BEFORE the 0xaa it writes to SNES_CMD, and never cleared by the MCU: that 0xaa lasts only until the menu loop re-arms MCU_CMD_RDY (0x55 == the ACK) on its next iteration, which is far too short a window for the SNES to catch now that game_handshake runs the iris before waiting for an answer. The SNES clears this byte just before sending the command and tests it afterwards. Free $FF0701..$FF07FF gap, after PATCH_HDR_SEL. Lockstep with LOAD_NACK in snes/memmap.i65. */
#define SRAM_EXPORT_PATH_ADDR        (0xFF4D00L) /* full SD path of the ROM the last "create patched ROM" wrote, 256 B. Parked in the free gap between SRAM_LASTGAME_FILE_ADDR's 256 bytes and IPS_LIST at 0xFF5000, because the menu reload rewrites SRAM_LASTGAME_DIR/FILE from the recents list before the SNES boots -- so the export cannot just fill those in directly; the reload path copies from here afterwards. Was 0xFF4B00 until the browser-restore feature landed on that same "free" address from another branch; it now owns 0xFF4B00/0xFF4C00, so this moved up to 0xFF4D00 (0xFF4E00..0xFF4FFF still free). */
#define SRAM_EXPORT_RESULT_ADDR      (0xFF071EL) /* result of the last "create patched ROM" export, reported to the user AFTER the menu reload (the export resets the SNES, so nothing can be shown while it runs): 0 = nothing to report, 1 = written, 2 = failed, 3 = ROM written but a sidecar that existed did not copy, 4 = refused because the .sfc already exists (patch_export_exists, checked before the SNES is halted). Values are the PATCH_EXPORT_* constants in src/patch.h -- change them there, not here. The reloaded browser pops a modal and clears it. Free $FF0701..$FF07FF gap, after LOAD_NACK. Lockstep with EXPORT_RESULT in snes/memmap.i65. */
#define SRAM_PATCH_HDR_SEL_ADDR      (0xFF071CL) /* 1 byte: scratch the patch selector's context menu uses to edit one patch's copier-header mode (0 auto, 1 headered, 2 headerless). The menu entry is a plain MTYPE_VALUE over this byte; patchsel_contextmenu seeds it from the patch's flags byte and folds the result back afterwards, which keeps the menu machinery unaware of the packed flags layout. Free $FF0701..$FF07FF gap, after SS_STAGED_SLOT and below the BPS breadcrumbs at $FF0720. Lockstep with PATCH_HDR_SEL in snes/memmap.i65. */
#define SRAM_SS_STAGED_SLOT_ADDR     (0xFF071BL) /* 1 byte: savestate slot whose image is RESIDENT in PSRAM 0xF00000 (1..4; 0 = none). The in-game load is a pure PSRAM->console replay (snes/savestate.a65 load_state -> run_vm), so selecting a slot only changes CS_SLOT -- the handler compares it with this byte and requests a CMD_LOADSTATE when they differ. Written by load_backup_state (on a successful stage) and by save_backup_state (a fresh save leaves that slot's image resident). Lockstep with SS_STAGED_SLOT in snes/memmap.i65. */
#define SRAM_CHEAT_MASTER_ADDR       (0xFF071AL) /* 1 byte: master cheat switch mirror (1 = cheats globally enabled). Published in cheat_program (= CFG.enable_cheats) and re-published when the L+R+Start+A / L+R+Start+B combos are served (main.c). The in-game CHEATS tab reads it for its status line and writes it when X toggles the master; the ACTUAL switch is the FPGA cheat_enable register, which the SNES flips by writing $82/$83 to MCU_CMD (cheat.v decodes that write directly) -- this byte is only the UI mirror. Free $FF0701..$FF07FF gap, after SS_SDD1_GATE. Lockstep with CHEAT_MASTER in snes/memmap.i65. */
#define SRAM_CHEAT_ADDR              (0xD00000L) /* up to 512 cheat records (512 bytes each), spans banks D0..D3 */
#define SRAM_CHEAT_CODE_STRINGS_ADDR (0xD40000L) /* per-code display strings, 12 bytes each. cheat_idx*512 + code_idx*12. Spans D4..D7, leaving D0..D3 free for up to 512 cheat records. */

#define SRAM_CHEAT_TITLE_ADDR        (0xD80000L) /* 64 bytes: cheat window title source, in the cheat region past any plausible cheat count. v2 layout = SRAM_CHEAT_TITLE_MARKER, then the ROM basename WITHOUT its extension, NUL-terminated (62 chars max). No prefix and no clipping: the menu prepends its own localized "Cheats for " and does the width clipping, so neither the language nor the window geometry is baked in here. */
#define SRAM_CHEAT_TITLE_MARKER      (0x02)      /* first byte of the v2 title. Non-printable on purpose: an older firmware wrote a printable "Cheats for ..." string, so the menu can tell the two layouts apart by looking at byte 0 alone. Lockstep with snes/cheatmenu.a65. */
#define SRAM_CHEAT_FLAGS_ADDR        (0xFF0500L) /* 512 bytes BSRAM mirror of cheat flag byte 0 (cheats 0..511). SNES reads/writes here for instant visual toggle. */
#define SRAM_CHEAT_NAMES_ADDR        (0xFF8000L) /* in-game cheat overlay: first CHEAT_NAME_INGAME_MAX names, CHEAT_NAME_INGAME_LEN bytes each (63 visible + NUL), staged at game load. 64*64 = 4 KB -> FF8000..FF8FFF (free area above SRAM_GAMEINFO_DESCEXT $FF7600, below scratchpad $FFFF00). Lockstep with CHEAT_NAMES in snes/memmap.i65. */
#define SRAM_MANUAL_S1META_ADDR      (0xFF0770L) /* scale-1 (1x) scrollable page meta, 16B right after MANUAL_META: +0 flags (bit0 = a 1x page is staged and ready), +1 nrows, +2..3 pix_h u16 LE (the viewer clamps vertical scroll to pix_h-224), +4 staged page, +5 staged guide, +6..7 scale-1 page count u16 LE. Lockstep with MANUAL_S1META in snes/memmap.i65. */
#define SRAM_MANUAL_GUIDES_ADDR      (0xFF9000L) /* in-game guides list, 260B ($FF9000..$FF9103) staged at game load by manual.c (free area above CHEAT_NAMES $FF8000-$FF8FFF, below scratchpad $FFFF00). Layout: +0 count (0..8), +1 selected (active guide; SNES writes it; default 0), +2..3 rsvd, then record[8] of 32B each at +4: +0 present (1=valid; the list is compacted so 0..count-1 are present), +1 nn (0=".man", 2..8=".0N.man"), +2 flags (raw .man header flags: bit0 = LEGACY quadrant zoom -- IGNORED, never produced any more; bit1 = scrollable zoom section present), +3 npages, +4 nblocks u16 LE, +6 zoom_pages u16 LE (= nblocks when bit1 is set, else 0; a zoom page IS a 1x block rendered at 2x), +8 title[24] font-encoded NUL-term (copied raw from the .man header). Read by the GUIDES tab. Lockstep with MANUAL_GUIDES in snes/memmap.i65. */
#define IGMENU_PERSIST_MAGIC_ADDR    (0xF4819EL) /* in-game menu session gate = the SNES-side man_pos_magic (PSRAM bank $F4), which keeps the remembered manual reading position AND the last-open tab across overlay close/reopen. Cleared to 0 here on every game load (manual_stage_meta) so position/tab never LEAK across games -- PSRAM $F4 survives a short power-cycle, so relying on stale-PSRAM alone was not enough. Lockstep with man_pos_magic / MN_POS_MAGIC in snes/igmenu.a65. */

#define SRAM_SKIN_ADDR               (0xF00000L)

#define SRAM_SPC_DATA_ADDR           (0xFD0000L)
#define SRAM_SPC_HEADER_ADDR         (0xFE0000L)
#define SRAM_SAVESTATE_HANDLER_ADDR  (0xFE1000L)

#define SRAM_MENU_FILEPATH_ADDR      (0xFF0000L)
#define SRAM_MENU_CFG_ADDR           (0xFF0100L)
#define SRAM_CMD_ADDR                (0xFF1000L)
#define SRAM_PARAM_ADDR              (0xFF1004L)
#define SRAM_MCU_STATUS_ADDR         (0xFF1100L)
#define SRAM_SNES_STATUS_ADDR        (0xFF1110L)
#define SRAM_SYSINFO_ADDR            (0xFF1200L) /* System Information screen: the packed sysinfo_blk_t defined below (128 B of the 544 B free up to LAST_GAME $FF1420). Lockstep with SYSINFO_BLK / SI2_* in snes/memmap.i65. */
#define SRAM_LASTGAME_ADDR           (0xFF1420L)
#define SRAM_LASTGAME_DIR_ADDR       (0xFF1F00L)
/* WiFi status/scan + connect block, RESERVED for the Companion port. Its OWN dedicated
   437-byte block (0xFF4000..0xFF41B5) in the free gap left when favorites moved off the
   old 0xFF4000 slot to 0xFF6000 -- no longer aliases SRAM_SYSINFO_ADDR, so the WiFi and
   sysinfo screens can coexist. Layout = WIFI_OFF_* in src/snes.h; lockstep with WIFI_BLK
   in snes/memmap.i65. LAST_GAME_FILE (0xFF4A00) stays clear above it. */
#define SRAM_WIFI_ADDR               (0xFF4000L)
/* Favorites mirror, 20*256 = 0x1400 bytes -> 0xFF6000..0xFF73FF.  Relocated out of
   the old 0xFF4000 slot (which only fit 10 entries before LAST_GAME_FILE) into the
   free gap past IPS_LIST so growing to 20 needed only this one address (kept in
   lockstep with FAVORITE_GAMES in snes/memmap.i65).  The old 0xFF4000 slot now hosts
   SRAM_WIFI_ADDR (437 B); the rest of 0xFF4000..0xFF49FF stays free.  MAX_FAVORITE_GAMES
   (cfg.h) sizes this; nothing else lives up to SCRATCHPAD. */
#define SRAM_FAVORITEGAMES_ADDR      (0xFF6000L)
/* base ROM basename of the most recent game, for reset_to_menu==3 (Rom) pre-select.
   Distinct from SRAM_LASTGAME_ADDR[0] (the recents *display* name, which for a
   patch-aware "<rom>\t<patch>" entry is the patch name and would never match a
   TYPE_ROM entry in the folder). Lives in the (now fully) free gap before
   IPS_LIST (0xFF5000); the favorites list was moved off 0xFF4000 to 0xFF6000. */
#define SRAM_LASTGAME_FILE_ADDR      (0xFF4A00L)
/* Browser position to restore on the NEXT menu boot, staged by browser_pos_save() when a
   theme/BGM action forces a menu reload (the reload cold-boots the SNES, and clear_wram
   fills $7E/$7F with $55, so dirlog/filesel_sel can't survive -- only strings in BSRAM do).
   DIR is the folder to reopen, FILE the basename to put the cursor on ("" = folder only).
   Consumed by filesel_nav_restore (snes/filesel.a65) gated on ST_RESTORE_BROWSER.
   Deliberately NOT reusing SRAM_LASTGAME_DIR/FILE: those are rewritten every menu boot by
   cfg_dump_listed_games_for_snes(). Sits in the verified-free gap 0xFF4B00..0xFF4FFF,
   clear of the ESP's WiFi block. Lockstep with BROWSER_DIR/BROWSER_FILE in snes/memmap.i65.
   NOTE: SRAM_EXPORT_PATH_ADDR shares this gap at 0xFF4D00 -- the two features were built in
   parallel and both originally claimed 0xFF4B00. Anything else taking a slice of
   0xFF4B00..0xFF4FFF has to check all three. */
#define SRAM_BROWSER_DIR_ADDR        (0xFF4B00L)
#define SRAM_BROWSER_FILE_ADDR       (0xFF4C00L)
/* IPS/BPS patch list staged by ips_find_patches(), SNES-WRITABLE half: +0 num_patches,
   +1..15 reserved, +IPS_FLAGS_BASE(16) one flags byte per patch (PATCH_FLAG_* -- type
   and the user's header-mode override), 254 of them -> 0xFF5000..0xFF510D. The rest of
   0xFF5000..0xFF5FFF is free (this used to be the WHOLE list, and its 4096-byte budget
   under the favorites mirror at 0xFF6000 is exactly what capped it at 16 patches).
   THE FLAGS CANNOT MOVE OUT OF 0xF0-0xFF. In menu mode (mapper 7) the FPGA only makes
   banks $F0-$FF writable by the SNES -- address.v, IS_SAVERAM = &SNES_ADDR[23:20], and
   IS_PATCH needs an unlock that only the in-game hooks raise. The menu's Y context menu
   WRITES this byte (sta @IPS_FLAGS,x in snes/patch.a65), so anywhere else the store is
   swallowed and the header-mode override silently never persists. Same reason
   SRAM_CHEAT_FLAGS_ADDR 0xFF0500 exists instead of the SNES poking 0xD00000 directly.
   Lockstep with IPS_LIST in snes/memmap.i65 and the layout comment in src/patch.h. */
#define SRAM_IPS_LIST_ADDR           (0xFF5000L)
/* The read-only half of the same list: +IPS_NAME_BASE(0) display slots
   (254*IPS_NAME_LEN=12192; each slot is a 42-byte name plus an "IPS"/"BPS" badge at
   +IPS_NAME_BADGE), +IPS_PATH_BASE(0x3000) full SD paths (254*IPS_PATH_LEN=48768 -> ends
   at 61056, inside the bank). Names are 12 KB and could never have fitted the 4096 bytes
   at 0xFF5000; banks $D9..$DF are free (cheat records D0..D3, code strings D4..D7,
   CHEAT_TITLE 64B at D80000, save at 0xE00000) and the whole 0xC00000..0xFFFFFF PSRAM
   window maps 1:1 onto SNES banks $C0..$FF, which the menu already READS from ($C90000
   covers, $D00000 cheats, $D80000 cheat title). One bank also keeps every 16-bit offset
   the 65816 computes (patchsel_slot_addr does idx*48 + !IPS_NAMES) in range. The paths
   are MCU-only. Lockstep with IPS_NAMES in snes/memmap.i65. */
#define SRAM_IPS_TEXT_ADDR           (0xD90000L)
/* Scan scratch for ips_find_patches(): IPS_MAX_PATCHES patch_entry_t in DIRECTORY
   ARRIVAL order, read back through patch_order[] (patch.c) to get alphabetical order.
   MCU-only, the menu never reads it. This is where the old ips_entries[] AHB array
   went: at 129 bytes an entry it could not hold 254 of them (the AHB region is 16 KB
   with a few hundred bytes to spare). Valid until the next scan, same as ips_scan_count.
   Bank $DA, right above the text half. */
#define SRAM_IPS_SCRATCH_ADDR        (0xDA0000L)
/* Ceiling for every write the patcher makes into the staged ROM image. It used to be
   SRAM_SAVE_ADDR, which let a malformed IPS offset or an absurd BPS target_size scribble
   over the cheat records at 0xD00000 -- and, once the patch list moved down here, over
   the list itself while the very same patch is being applied. Nothing legitimate comes
   close: the largest SNES ROM is 8 MB, and the BPS source backup lands at
   rom_base + target_size. */
#define SRAM_PATCH_TOP               (SRAM_CHEAT_ADDR)
/* packed gameinfo_meta_t for the pre-boot info screen (see gameinfo.h). Sits AFTER the
   favorites mirror (0xFF6000..0xFF73FF) -- it must NOT overlap it, or opening a favorite's
   info panel clobbers the favorites list (lockstep with GAMEINFO in snes/memmap.i65). */
#define SRAM_GAMEINFO_ADDR           (0xFF7400L)
/* full (untruncated) info-screen description, char[2048] NUL-terminated, staged on demand
   by SNES_CMD_GI_DESC_FULL (the "full description" Y mode). The struct's description[256]
   above is capped by the YAML parser (YAML_BUFLEN); this region carries the complete text,
   read with a streaming line scanner outside the YAML parser. A 1st byte of 0 = invalid ->
   the menu falls back to the struct's description[256]. Sits after the struct's end
   ($FF759D) and before SRAM_SCRATCHPAD ($FFFF00); lockstep with GI_DESC_EXT in
   snes/memmap.i65. */
#define SRAM_GAMEINFO_DESCEXT_ADDR   (0xFF7600L)
#define SRAM_SCRATCHPAD              (0xFFFF00L)
#define SRAM_DIRID                   (0xFFFFF0L)
#define SRAM_RELIABILITY_SCORE       (0x100)

/* --- System Information block, v2: the MCU publishes VALUES, the menu formats the screen.
 *
 * The firmware used to write 13 ready-made 40-char text lines to SRAM_SYSINFO_ADDR, which
 * meant one localized template set per language had to live in MCU flash. It now publishes
 * this packed struct and the menu builds every line from its own string pool, so a new
 * language costs the firmware nothing.
 *
 * THE BYTE OFFSETS ARE THE INTERFACE. They are mirrored as SI2_* in snes/memmap.i65 and must
 * never be re-derived on the SNES side; the _Static_asserts below pin the three string
 * offsets and the total size so a field inserted in the middle breaks the build here rather
 * than silently shifting the menu's reads.
 *
 * The menu re-checks magic[] before reading any field, which is what makes "new menu + old
 * firmware" safe: an older firmware leaves plain text at this address, and no legacy line 0,
 * in any of the 5 languages, ever started with 'S', so the check cannot false-positive on
 * leftover text.
 *
 * Anything that would cost the 65816 arithmetic is pre-split here: the SD revision nibbles,
 * the access time integer and fractional parts, and the manufacturing year already offset by
 * 2000. Strings are plain NUL-terminated ASCII (the font maps ASCII 1:1 for these).
 */
#define SYSINFO_MAGIC0               ('S')
#define SYSINFO_MAGIC1               ('I')
#define SYSINFO_VERSION              (1)

#define SYSINFO_FLAG_SD_GONE         (0x01)  /* card removed or the USB server owns the SD: the SD/card/access-time fields are all zero */
#define SYSINFO_FLAG_FS_BUSY         (0x02)  /* partial block: NONE of the card fields is valid yet (maker, oem, product, revision, serial, manufacturing date, tacc_*, card_used_mb, card_total_mb are all still zero while the free-space count runs). The menu must blank the card lines for as long as this is set -- printing the zeros would be plausible-looking nonsense, not an obvious placeholder */
#define SYSINFO_FLAG_ACC_MEASURING   (0x04)  /* access time not sampled yet; the tacc_* fields are zero */
#define SYSINFO_FLAG_CLK_MEASURING   (0x08)  /* SNES master clock not sampled yet; snes_clk_hz is zero */
#define SYSINFO_FLAG_U16             (0x10)  /* an Ultra16 was detected; u16_serial is valid */
#define SYSINFO_FLAG_U16_AUTOBOOT    (0x20)  /* Ultra16 autoboot is on (only meaningful with SYSINFO_FLAG_U16) */

/* Canonical SGB BIOS state, ordered by severity so the menu can index its word table
   directly. Deliberately NOT the SGB_BIOS_* order of sgb.h (CHECK=0 OK=1 MISMATCH=2
   MISSING=3) -- sysinfo.c maps between the two. */
#define SYSINFO_SGB_CHECKING         (0)
#define SYSINFO_SGB_MISSING          (1)
#define SYSINFO_SGB_MISMATCH         (2)
#define SYSINFO_SGB_OK               (3)

typedef struct __attribute__((__packed__)) _sysinfo_blk {
  uint8_t  magic[2];        /* +0   ($00) 'S','I' */
  uint8_t  version;         /* +2   ($02) SYSINFO_VERSION */
  uint8_t  flags;           /* +3   ($03) SYSINFO_FLAG_* */
  uint8_t  sgb_ver;         /* +4   ($04) CFG.sgb_bios_version, the N in sgbN_boot.bin */
  uint8_t  sgb_state;       /* +5   ($05) SYSINFO_SGB_* */
  uint8_t  cic_state;       /* +6   ($06) raw enum cicstates; cic_str carries the text */
  uint8_t  sd_maker;        /* +7   ($07) CID manufacturer ID, shown as hex */
  char     sd_oem[2];       /* +8   ($08) CID OEM/application ID, not NUL-terminated */
  char     sd_product[5];   /* +10  ($0A) CID product name, not NUL-terminated */
  uint8_t  sd_rev_maj;      /* +15  ($0F) CID revision, high nibble already split out */
  uint8_t  sd_rev_min;      /* +16  ($10) CID revision, low nibble */
  uint8_t  sd_serial[4];    /* +17  ($11) CID serial number, shown as hex */
  uint8_t  pad0;            /* +21  ($15) */
  uint16_t sd_mfd_year;     /* +22  ($16) manufacturing year, 2000 already added */
  uint8_t  sd_mfd_month;    /* +24  ($18) manufacturing month, 1..12 */
  uint8_t  pad1;            /* +25  ($19) */
  uint16_t tacc_avg_int;    /* +26  ($1A) average access time, whole milliseconds */
  uint16_t tacc_avg_frac;   /* +28  ($1C) average access time, thousandths (0..999) */
  uint16_t tacc_max_int;    /* +30  ($1E) worst access time, whole milliseconds */
  uint16_t tacc_max_frac;   /* +32  ($20) worst access time, thousandths (0..999) */
  uint32_t card_used_mb;    /* +34  ($22) */
  uint32_t card_total_mb;   /* +38  ($26) */
  uint32_t snes_clk_hz;     /* +42  ($2A) measured SNES master clock */
  uint16_t u16_serial;      /* +46  ($2E) Ultra16 serial number */
  char     fw_str[24];      /* +48  ($30) CONFIG_VERSION, NUL-terminated */
  char     cic_str[28];     /* +72  ($48) CIC state text, NUL-terminated. 28 because the
                                    longest English name, "SuperCIC detected, not used",
                                    is 27 chars (cic.c owns that table) */
  char     model_str[28];   /* +100 ($64) DEVICE_NAME, NUL-terminated */
} sysinfo_blk_t;            /* 128 bytes */

_Static_assert(offsetof(sysinfo_blk_t, fw_str) == 48, "sysinfo_blk_t.fw_str must stay at SI2_FW_STR (+48)");
_Static_assert(offsetof(sysinfo_blk_t, cic_str) == 72, "sysinfo_blk_t.cic_str must stay at SI2_CIC_STR (+72)");
_Static_assert(offsetof(sysinfo_blk_t, model_str) == 100, "sysinfo_blk_t.model_str must stay at SI2_MODEL_STR (+100)");
_Static_assert(sizeof(sysinfo_blk_t) == 128, "sysinfo_blk_t must stay 128 bytes (SI2_SIZE in snes/memmap.i65)");
/* the block replaces the 13x40 text layout in place, so it can never outgrow it */
_Static_assert(sizeof(sysinfo_blk_t) <= 13 * 40, "sysinfo_blk_t must fit the old 13-line text region");

#define LOADROM_WITH_SRAM   (1)
#define LOADROM_WITH_RESET  (2)
#define LOADROM_WAIT_SNES   (4)
#define LOADROM_WITH_FPGA   (8)
#define LOADROM_WITH_COMBO  (16)

#define LOADRAM_AUTOSKIP_HEADER (1)

#define SAVE_BASEDIR    ("/sd2snes/saves/")

#define min(a,b) \
 ({ __typeof__ (a) _a = (a); \
 __typeof__ (b) _b = (b); \
 _a < _b ? _a : _b; })

uint32_t load_rom(uint8_t* filename, uint32_t base_addr, uint8_t flags);
void assert_reset(void);
void init(uint8_t *filename);
void deassert_reset(void);
uint32_t load_spc(uint8_t* filename, uint32_t spc_data_addr, uint32_t spc_header_addr);
/* Multi-slot battery SRAM (CICLO 2). SRM_SLOT_COUNT slots: internal index 0..3,
   UI "SLOT 1..4". Slot 0 = the legacy <stem>.srm (byte-identical); slots 1..3 =
   <stem>.02/03/04.srm. Gated by CFG.enable_sram_slots (OFF -> srm_slot forced 0). */
#define SRM_SLOT_COUNT 4
extern uint8_t srm_slot;      /* LIVE session slot used for save/load naming. Set once in
                                 migrate_and_load_srm (game load), IMMUTABLE for the session so
                                 an in-game slot switch can never misroute an autosave. */
extern uint8_t srm_slot_sel;  /* selected/next slot (== sidecar). == srm_slot at load; the in-game
                                 SET command writes the sidecar + updates this; applies next load. */
/* Build the slot's file extension (".srm" for slot 0, ".0N.srm" N=slot+1 for 1..3) into ext. */
void srm_slot_ext(char *ext, size_t extlen, uint8_t slot);
/* Read the /sd2snes/saves/<stem>.slot sidecar into srm_slot/srm_slot_sel (both 0 when
   CFG off / absent / invalid). Called at game load. Bounded (one f_read). */
void srm_slot_load(uint8_t *filename);
/* Write the sidecar (in-game SET). Updates srm_slot_sel; NEVER touches the live srm_slot.
   Bounded (one f_write). */
void srm_slot_save(uint8_t *filename, uint8_t slot);

uint32_t migrate_and_load_srm(uint8_t *filename, uint32_t base_addr);
uint32_t load_sram(uint8_t* filename, uint32_t base_addr);
uint32_t load_sram_offload(uint8_t* filename, uint32_t base_addr, uint8_t flags);
uint32_t load_sram_rle(uint8_t* filename, uint32_t base_addr);
uint32_t load_bootrle(uint32_t base_addr);
void load_dspx(const uint8_t* filename, uint8_t st0010);
void sram_hexdump(uint32_t addr, uint32_t len);
uint8_t sram_readbyte(uint32_t addr);
uint16_t sram_readshort(uint32_t addr);
uint32_t sram_readlong(uint32_t addr);
void sram_writebyte(uint8_t val, uint32_t addr);
void sram_writeshort(uint16_t val, uint32_t addr);
void sram_writelong(uint32_t val, uint32_t addr);
uint16_t sram_readblock(void* buf, uint32_t addr, uint16_t size);
uint16_t sram_readstrn(void* buf, uint32_t addr, uint16_t size);
uint16_t sram_writestrn(void* buf, uint32_t addr, uint16_t size);
void sram_readlongblock(uint32_t* buf, uint32_t addr, uint16_t count);
uint16_t sram_writeblock(void* buf, uint32_t addr, uint16_t size);
void save_srm(uint8_t* filename, uint32_t sram_size, uint32_t base_addr);
void saveinfo_stage(uint8_t *filename);
extern uint8_t current_ips_srm_source[256];
extern uint8_t current_ips_flags;
extern uint8_t rom_export_active;
extern uint32_t patch_export_size;
extern uint8_t patch_last_ok;
int  save_sram(uint8_t* filename, uint32_t sram_size, uint32_t base_addr);
uint32_t calc_sram_crc(uint32_t base_addr, uint32_t size, uint32_t crc);
uint16_t calc_sram_sum(uint32_t base_addr, uint32_t size);
uint8_t sram_reliable(void);
void sram_memset(uint32_t base_addr, uint32_t len, uint8_t val);

/* BS 8M Memory Pack: 1MB in PSRAM at 0x900000, mapped LoROM $C0-$DF / HiROM $E0-$EF
   when FEAT_BSSLOT is set (0x900000 must match BS_PACK_HIT in address.v).  SD: .mpk */
#define BS_PACK_ADDR  0x900000
#define BS_PACK_SIZE  0x100000
uint8_t load_bs_pack(uint8_t* filename);  /* returns 1 if a real pack was loaded */
void save_bs_pack(uint8_t* filename);
uint32_t calc_pack_crc_inreset(void);     /* reset-tolerant pack CRC for prepare_reset */

#endif
