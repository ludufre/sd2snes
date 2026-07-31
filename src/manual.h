/* sd2snes+ - in-game manual/guides viewer (MCU side).
 *
 * A game may ship UP TO 8 guides ("<stem>.man" plus "<stem>.02.man".."<stem>.08.man", all
 * bucketed under /sd2snes/info like the game-info assets). Each .man holds a paletted 8bpp guide
 * rendered as fixed 57856-byte blocks (512B palette + 57344B tile-order tiles, sector-aligned),
 * a 40-byte header (magic/ver/bpp/npages/flags/dims/nblocks/rsvd/title[24]), plus an
 * optional scrollable 2x zoom section. At game load manual_stage_meta() PROBES the 8 candidates directly,
 * caches a compacted per-guide table, and publishes MANUAL_GUIDES ($FF9000, the guide list the
 * GUIDES tab reads) + MANUAL_META ($FF0760, present bit + guide-0 npages + meta_abi sanity byte).
 * The in-game tab then requests one block at a time via SNES_CMD_MANUAL_BLOCK ($43), whose
 * MCU_PARAM carries [0..1]=index-in-stream, [2]=guide (0..7), [3]=mode (bit0=zoom);
 * manual_stage_block() maps it to a global file index and streams 57856B into PSRAM at
 * SRAM_MANUAL_BLOCK_ADDR ($C30000) for the frozen SNES viewer to DMA.
 *
 * SAFETY (mirrors gameinfo.c's FMV streaming): both entry points are BOUNDED and fail-safe --
 * they must never hang the MCU command loop. Missing/bad/CFG-off guides just leave the meta
 * "not present"; a transient SD read error retries in place, then latches an error bit and
 * returns. See src/memory.h for the SRAM contract and snes/memmap.i65 for the lockstep. */

#ifndef MANUAL_H
#define MANUAL_H

#include <stdint.h>

/* Probe the <=8 guide candidates, cache the compacted guide table, and publish MANUAL_GUIDES +
 * MANUAL_META. Call at game load (after igmenu_stage/saveinfo_stage). No valid guide, or
 * EnableGameManual off -> the present bit stays 0 (the tab shows "not found"). Bounded + fail-safe. */
void manual_stage_meta(uint8_t *rom_path);

/* Stream the block at stream index `idx` of compacted `guide` (mode bit0=zoom -> the guide's zoom
 * stream) as 57856B into SRAM_MANUAL_BLOCK_ADDR for the frozen SNES viewer to DMA. guide/idx out
 * of range -> no-op (the ACK still clears). On a hard read failure the MANUAL_META error bit is
 * latched and it returns (the viewer shows an error). Bounded (retry in place + reopen by guide);
 * never hangs. */
void manual_stage_block(uint8_t guide, uint16_t idx, uint8_t mode);

/* Stage ONE WHOLE scrollable 2x zoom page (`page` == the 1x block index rendered at 2x) into PSRAM:
 * tile rows split into their BG1/BG2 halves ($C50000/$C60000), prebuilt tilemap words ($C5E000) and
 * the 128-colour palette ($C5FC00). Publishes MAN_META_FLAG_ZREADY + the page's nrows/pix_h.
 *
 * Staging the page WHOLE is deliberate: it is what lets the viewer pan with nothing but PSRAM->VRAM
 * DMA, so a scroll can never stall on the MCU, show a half-streamed row, or need a prefetch FIFO.
 * The cost lands once per page turn instead of once per nav step.
 *
 * Guide/page out of range, or a guide with no zoom section -> no-op with the ready flag CLEARED
 * (never leaves the previous page pannable). Hard read failure -> error latch. Bounded; never hangs. */
void manual_stage_zpage(uint8_t guide, uint16_t index, uint8_t mode);

/* manual_stage_zpage() modes. Without MAN_ZMODE_PAGE, `index` is a 1x BLOCK and the block map
   resolves it to (zoom page, entry Y) -- that is what makes pressing Y on the second band of a
   page land half-way down the SAME page instead of on a page of its own. */
#define MAN_ZMODE_PAGE  (0x01)   /* `index` is a zoom page directly (page turns) */

/* Stage the SCALE-1 (1x) scrollable page `page` into PSRAM: tiles at $C30000, prebuilt tilemap at
 * $C4B000, palette at $C4C000. Same machinery as the zoom page at 256px wide -- 32 tiles/row, one
 * BG layer, no split. It has its OWN PSRAM region, so the 1x and 2x pages are both resident and
 * toggling between them costs nothing. Publishes MANUAL_S1META. Bounded; never hangs. */
void manual_stage_s1page(uint8_t guide, uint16_t page);

#endif
