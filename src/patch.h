/* sd2snes - SD card based universal cartridge for the SNES
   patch.h: ROM patch support (IPS and BPS)
*/

#ifndef IPS_H
#define IPS_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Layout of the patch list the MCU stages for the menu.  It lives in TWO
 * regions, and the split is forced by hardware, not taste:
 *
 *   SRAM_IPS_LIST_ADDR ($FF5000, 4096 B) -- the SNES-WRITABLE half
 *     +0                                 u8   num_patches (0..IPS_MAX_PATCHES)
 *     +1 .. +15                               reserved
 *     +IPS_FLAGS_BASE + N                u8   per-patch flags (PATCH_FLAG_*)
 *
 *   SRAM_IPS_TEXT_ADDR ($D90000, 64 KB)  -- read-only for the SNES
 *     +IPS_NAME_BASE  + N*IPS_NAME_LEN   64 B per-patch display slot:
 *                        +0 .. +41            display name, NUL-terminated
 *                        +IPS_NAME_BADGE(58)  "IPS"/"BPS" badge, NUL-terminated
 *     +IPS_PATH_BASE  + N*IPS_PATH_LEN  192 B full SD path, NUL-terminated
 *                                             (MCU-only, the menu never reads it)
 *
 * In menu mode the FPGA only lets the SNES WRITE banks $F0-$FF (address.v:
 * IS_SAVERAM = &SNES_ADDR[23:20] for mapper 7; IS_PATCH needs an unlock that only
 * the in-game hooks raise).  The flags byte is the one field the menu also writes
 * -- the Y context menu cycles the header mode -- so it has to stay at $FF5000
 * even though the names no longer fit there.  Do NOT "simplify" this back into
 * one region: the store would be swallowed and the override would silently never
 * persist.  SRAM_CHEAT_FLAGS_ADDR $FF0500 exists for exactly the same reason.
 *
 * Everything is in DISPLAY order (alphabetical), which is also the order the
 * 1-based index in MCU_PARAM+7 refers to.
 *
 * LOCKSTEP with the IPS_* defines in snes/memmap.i65 and with the layout comments
 * on SRAM_IPS_LIST_ADDR / SRAM_IPS_TEXT_ADDR (src/memory.h).  Changing any offset
 * here without the mirror makes the menu render garbage and the MCU open a junk
 * path.
 *
 * The display name is resolved by the MCU (yml Name: override, else the cleaned
 * suffix after the ROM stem) so the 65816 side only ever renders.  The badge
 * lives inside the name slot on purpose: "IPS"/"BPS" are language-neutral, so a
 * menu-side string would land in bank $C0, which is ~90% full.
 * ------------------------------------------------------------------------- */

/* Maximum number of patches listed for one ROM.  254, not 255: the menu builds
   listsel_max = num_patches + 1 (row 0 is "[No patch]") in an 8-bit register, and
   the selected index travels back in the single byte at MCU_PARAM+7.  In practice
   that is no limit at all -- the old ceiling was 16, and it was pure arithmetic:
   48 name + 1 flags + 192 path = 241 bytes each against the 4096-byte budget the
   whole list used to have at $FF5000. */
#define IPS_MAX_PATCHES  254
/* Bytes available at SRAM_IPS_LIST_ADDR before the favorites mirror ($FF6000) */
#define IPS_LIST_SIZE    4096
/* Bytes the text half owns at SRAM_IPS_TEXT_ADDR (one full PSRAM/SNES bank) */
#define IPS_TEXT_SIZE    0x10000
/* Byte offset of the per-patch flag bytes within the list (past the count byte) */
/* Byte +1 of the list header: which job this list is for.  The pre-boot dialog is
   shared between picking a patch and picking the Sufami Turbo Slot B companion cart
   (identical list shape), and this tells the menu which wording to use.
   Lockstep with IPS_DLGMODE_* in snes/memmap.i65. */
#define IPS_DLGMODE_OFFSET 1
#define IPS_DLGMODE_PATCH  0
#define IPS_DLGMODE_SLOTB  1

#define IPS_FLAGS_BASE   16
/* Byte offset of the first display slot within the text half */
#define IPS_NAME_BASE    0
/* Bytes reserved per display slot (name + badge) */
#define IPS_NAME_LEN     64
/* Offset of the "IPS"/"BPS" badge inside a display slot */
#define IPS_NAME_BADGE   58
/* Byte offset of the full-path slots within the text half.  Rounded up from the
   end of the name slots (254*64 = 16256) to a tidy 0x4000.  Paths then end at
   0x4000 + 254*192 = 0xFE80, still inside the bank. */
#define IPS_PATH_BASE    0x4000
/* Bytes reserved per full-path slot (191 usable chars + NUL).  A patch whose
   full path does not fit is SKIPPED by the scan rather than truncated -- a
   truncated path would make f_open() hit a different (or missing) file. */
#define IPS_PATH_LEN     192

/* Per-patch flags byte at IPS_FLAGS_BASE + N. */
#define PATCH_FLAG_TYPE_MASK   0x03  /* patch file format */
#define PATCH_TYPE_IPS         0
#define PATCH_TYPE_BPS         1
#define PATCH_FLAG_HDR_MASK    0x0c  /* IPS copier-header convention */
#define PATCH_FLAG_HDR_SHIFT   2
#define PATCH_HDR_AUTO         0     /* detect (default; the historic behaviour) */
#define PATCH_HDR_HEADERED     1     /* offsets authored against a headered ROM */
#define PATCH_HDR_HEADERLESS   2     /* offsets authored against a headerless ROM */

#define PATCH_HDR_MODE(flags)  (((flags) & PATCH_FLAG_HDR_MASK) >> PATCH_FLAG_HDR_SHIFT)

/* Bytes reserved for a patch file name (no directory part) in the scan scratch */
#define PATCH_BASENAME_LEN     128

/* One scanned patch.  Deliberately holds the BASENAME and not the full path: the
   scan keeps IPS_MAX_PATCHES of these and patch_publish reassembles
   "<dirpath>/<basename>" straight into the write buffer.
   The array of these lives in PSRAM at SRAM_IPS_SCRATCH_ADDR, not in MCU RAM --
   see the comment on patch_order[] in patch.c. */
typedef struct {
    char    basename[PATCH_BASENAME_LEN];
    uint8_t flags;
} patch_entry_t;

/* Bytes of the patched image the BPS probe materializes to read the SNES header.
   Covers both the LoROM-position header (0x7FC0, used by SA-1) and the
   HiROM-position header (0xFFC0) plus the full snes_header_t. */
#define PATCH_PROBE_HEADER_LIMIT  0x10000

/*
 * ips_pending_index: non-zero when a patch should be applied inside load_rom.
 * Set by the CMD_LOADROM handler in main.c before calling load_rom();
 * consumed (and cleared to 0) inside load_rom() after assert_reset/init.
 */
extern uint8_t ips_pending_index;

/*
 * ips_find_patches
 *   Scan the directory of rom_path for *.ips / *.bps files whose name starts
 *   with the ROM stem (case-insensitive), sort them alphabetically and stage up
 *   to IPS_MAX_PATCHES of them at sram_addr in the layout documented above.
 *
 *   The directory scan is the SOLE source of truth for which patches exist; the
 *   per-ROM /sd2snes/patches/<BB>/<stem>.yml (patchmeta.h) is only a metadata
 *   overlay applied on top, so an entry left in the yml for a deleted patch is
 *   simply ignored (and pruned on the next write).
 *
 *   rom_path MUST NOT be the global file_lfn: the scan hands that buffer to
 *   f_readdir as the long-name destination, so it is overwritten on the first
 *   directory entry.
 *
 *   Returns num_patches.
 */
uint8_t ips_find_patches(const uint8_t *rom_path, uint32_t sram_addr);

/* How many entries the most recent ips_find_patches() staged, so
   CMD_PATCH_META_SAVE can write the sidecar back without touching the card
   again.  Valid only until the next scan. */
extern uint8_t ips_scan_count;

/*
 * patch_basename_at
 *   Basename of entry <i> (0-based) of the most recent scan, in DISPLAY order,
 *   read out of the PSRAM scan scratch through patch_order[].  This is what
 *   patchmeta needs to pair a staged flags byte with the patch file it belongs
 *   to, and it replaces the ips_entries[] array that used to be handed around
 *   (2064 bytes of AHB that could never have grown past 16 patches).
 *   The live flags byte is NOT returned here -- read it straight from
 *   SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE + i, which is the copy the menu edits.
 *   Writes an empty string and returns 0 when i is out of range.
 */
int patch_basename_at(uint8_t i, char *out, uint16_t len);

/*
 * patch_display_name
 *   Derive the menu display name for a patch: the part of its basename after
 *   the ROM stem, with leading separators ('_', '-', '.', '(', ' ') stripped and
 *   the extension removed.  When nothing is left (the patch is exactly
 *   "<ROM stem>.ips") the whole extension-less basename is used instead.
 *   Pure string code, no I/O -- covered by the host tests.
 *   Returns the number of characters written (excluding the NUL).
 */
int patch_display_name(char *out, int outlen, const char *patch_basename,
                       unsigned stem_len);

/*
 * patch_ext_type
 *   Classify a filename by extension: PATCH_TYPE_IPS, PATCH_TYPE_BPS, or -1 when it
 *   is neither.  Case-insensitive; the extension must be EXACTLY three characters,
 *   so ".ipsx" and ".bak" are both rejected.
 *   The single implementation for every site that has to tell IPS from BPS -- the
 *   directory scan, patch_apply_impl's dispatch to bps_apply, and the Recents/
 *   Favorites restage in main.c.  They used to each roll their own and disagree.
 *   Pure string code, no I/O.
 */
int patch_ext_type(const char *name);

/*
 * patch_belongs_to_rom
 *   Does patch file `fn` belong to the ROM basename `romfile`, whose stem is
 *   `stem_len` characters?  Returns PATCH_TYPE_IPS/PATCH_TYPE_BPS, or -1.
 *   The whole match rule of the directory scan, in one pure function so it can be
 *   host-tested (tests/host/run_patchmatch.sh).
 *   NOTE the deliberate exclusion: a patch named "<ROM stem>.<ext>" -- nothing between
 *   the stem and the extension -- is REFUSED, because that is exactly what "create
 *   patched ROM" leaves behind next to the file it wrote, and re-applying it would
 *   corrupt the image.  Full reasoning at the definition in patch.c.
 */
int patch_belongs_to_rom(const char *fn, const char *romfile, unsigned stem_len);

/*
 * ips_apply
 *   Read the IPS full path for patch <index> (1-based) from SRAM at
 *   sram_addr + IPS_PATH_BASE + (index-1)*IPS_PATH_LEN, open it and apply the patch
 *   over the ROM already loaded in SRAM at rom_base_addr.
 *   Must be called while the SNES is held in hardware reset.
 *   original_rom_size is the byte length of the unpatched ROM image already
 *   in SRAM (romprops.romsize_bytes).  If the patch writes beyond this size
 *   the function zero-fills the gap first so that areas between IPS records
 *   contain 0x00 rather than leftover data from a previously loaded ROM.
 *
 *   rom_header_size is the number of bytes that were skipped at the start of
 *   the ROM file when loading into SRAM (i.e. the copier-header size, typically
 *   0 or 512).  Records are applied at offset - rom_header_size, exactly like a
 *   PC tool (Lunar IPS / Floating IPS / RomPatcher.js) applies them to the same
 *   header-stripped file form.  The function does NOT try to guess a header from
 *   the record offsets: that heuristic was inverted and corrupted legit
 *   unheadered patches that touch the start of the ROM (e.g. Zelda Parallel
 *   Worlds).  Matching the patch's header convention to the ROM is the caller's
 *   job, the same as on a PC.
 *
 *   Returns the highest (offset + size) seen across all records — adjusted for
 *   the header offset — on success, or 0 on error.  If the returned value
 *   exceeds original_rom_size the caller must update the FPGA ROM mask.
 */
/*   header_addr: SNES internal header location in the staged ROM
 *   (romprops.header_address, e.g. 0x7FB0 LoROM / 0xFFB0 HiROM).  When
 *   rom_header_size == 0 and header_addr != 0, ips_apply auto-detects a headered
 *   patch (offsets authored for a 512-byte copier ROM) and shifts by 512; pass
 *   header_addr == 0 to disable detection (host tests / opt-out).
 *
 *   The user can override the detection per patch: the PATCH_FLAG_HDR_MASK field
 *   of the patch's flags byte (IPS_FLAGS_BASE + index-1) selects AUTO (detect,
 *   the default), HEADERED (adj = 512) or HEADERLESS (adj = 0).  The image staged
 *   in PSRAM is ALWAYS header-stripped, so HEADERED/HEADERLESS are absolute and
 *   do not depend on rom_header_size -- which is exactly what makes HEADERLESS
 *   meaningful for a ROM file that does carry a copier header.
 */
uint32_t ips_apply(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                   uint32_t original_rom_size, uint32_t rom_header_size,
                   uint32_t header_addr);

/* Set by ips_apply() for the USB debug breadcrumb at $FF072E (0xD0 + this):
   0 = offsets applied literally, 1 = auto-detection shifted them by 512,
   2 = user forced HEADERED, 3 = user forced HEADERLESS. */
extern uint8_t ips_header_adj_used;

/*
 * bps_apply
 *   Apply a BPS patch at <index> (1-based) from the list stored at sram_addr.
 *   target_size is known from the BPS header so no two-pass scan is needed.
 *   Returns target_size on success, 0 on error.
 *
 *   use_copier: 0 = legacy byte-by-byte apply (the whole image is finalized in
 *   SDRAM when this returns).  1 = copier mode: SourceCopy/TargetCopy are EMITTED
 *   as FPGA copier descriptors (patch_copier_emit) instead of moved here, and the
 *   CRC re-read is skipped; the image at rom_base is only PARTIAL on return (the
 *   live menu must drain the descriptor list to finish it).  The pristine source
 *   backup and TargetRead literals are still written inline either way.
 */
uint32_t bps_apply(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                   uint32_t original_rom_size, uint8_t use_copier);

/*
 * bps_probe_header  (load-time optimization)
 *   Cheaply materialize ONLY the first out_limit bytes of a BPS target image
 *   into a scratch region (rom_base + max(target_size, original_rom_size)),
 *   so the caller can run smc on the patched SNES header and decide whether the
 *   patch changes the cartridge type / required FPGA core BEFORE applying the
 *   full (slow) patch.  Reads source from the pristine original at rom_base; the
 *   real ROM image at rom_base is never modified.  Returns 0 for a non-BPS file
 *   (magic mismatch) / error, so the caller can fall back to the legacy
 *   apply-then-detect path.  On success returns target_size and sets
 *   *out_scratch_base to the materialized header window's base address.
 */
uint32_t bps_probe_header(uint32_t sram_addr, uint8_t index,
                          uint32_t rom_base_addr, uint32_t original_rom_size,
                          uint32_t out_limit, uint32_t *out_scratch_base);

/*
 * patch_apply
 *   Dispatcher: reads the stored SD path for <index>, detects the format
 *   from the file extension (.ips → ips_apply, .bps → bps_apply), and
 *   calls the appropriate apply function.
 *   rom_header_size is forwarded to ips_apply for copier-header correction
 *   (BPS encodes sizes explicitly so it is not needed there).
 *   Returns the required ROM size after patching (adj_max_end for IPS,
 *   target_size for BPS), or 0 on error.
 */
uint32_t patch_apply(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                     uint32_t original_rom_size, uint32_t rom_header_size,
                     uint32_t header_addr);

/*
 * patch_apply_copier
 *   Like patch_apply but, for a .bps, emits FPGA copier descriptors for the
 *   SourceCopy/TargetCopy bulk (see bps_apply use_copier=1).  On success the ROM
 *   at rom_base is only partially materialized: the caller must run the emitted
 *   descriptors via the live menu copier (patch_copier.h) before booting.  IPS
 *   patches are applied the legacy way.  Returns target_size / adj_max_end on
 *   success, 0 on error or descriptor-list-full (caller falls back to patch_apply).
 */
uint32_t patch_apply_copier(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                            uint32_t original_rom_size, uint32_t rom_header_size,
                            uint32_t header_addr);

/* Outcome of a "create patched ROM" run, as parked in SRAM_EXPORT_RESULT_ADDR for
 * the reloaded browser to report.  These ARE the on-the-wire values: keep them in
 * lockstep with EXPORT_RESULT in snes/memmap.i65 and with export_result_check in
 * snes/patch.a65.  PARTIAL is deliberately distinct from OK -- reporting success
 * for a half-copied set is how a user ends up with a ROM whose saves quietly did
 * not come along. */
#define PATCH_EXPORT_NONE     0   /* nothing to report */
#define PATCH_EXPORT_OK       1   /* ROM written and every sidecar that existed copied */
#define PATCH_EXPORT_FAILED   2   /* no ROM written */
#define PATCH_EXPORT_PARTIAL  3   /* ROM written, but a sidecar that existed did not copy */
#define PATCH_EXPORT_EXISTS   4   /* refused: the .sfc this export would write already exists
                                     (checked before anything ran -- re-running an export is
                                     almost always an accident; delete the old file first) */

/*
 * patch_export_write  (CMD_EXPORT_PATCHED_ROM)
 *   Stream the patched image that load_rom() just staged in PSRAM out to a .sfc
 *   next to the ROM, named after the PATCH FILE (see below).  Must be called
 *   with the SNES still held in reset; rom_base_addr/size come from the caller
 *   (romprops has no header of its own, and keeping it out of here also keeps
 *   patch.c compilable by the host test harness).
 *   The output is named after the PATCH FILE with a .sfc extension
 *   ("Foo (USA) - [BR].ips" -> "Foo (USA) - [BR].sfc"), which reads naturally next
 *   to the original and needs no sanitising.  A patch sharing the ROM's exact stem
 *   is never listed in the first place (patch_belongs_to_rom), so the name cannot
 *   silently derive the ROM's own.  Always writes the headerless form -- that is
 *   what is in PSRAM.  A taken name is REFUSED, never renamed around
 *   (see patch_export_exists).  Returns 1 on success.
 */
int patch_export_write(const uint8_t *patch_path,
                       uint32_t rom_base_addr, uint32_t size);

/*
 * patch_export_exists
 *   Would patch_export_write collide with a file already on the card?  Called by
 *   patch_export_command BEFORE the SNES is halted and the whole load_rom+patch
 *   runs, so an accidental re-export costs a menu reload and a
 *   "Patched ROM already exists" modal (PATCH_EXPORT_EXISTS) instead of minutes
 *   of work and a silent "<...> 2.sfc" duplicate with its saves split off.
 *   To regenerate a patched ROM on purpose, delete the old .sfc first (browser Y).
 */
int patch_export_exists(const uint8_t *patch_path);

/*
 * patch_export_copy_assets
 *   Copy every sidecar of the original ROM across to the freshly exported one, so
 *   the new entry arrives with its cover, info screen, guides, cheats and saves
 *   already in place.  Destination name is read back from SRAM_EXPORT_PATH_ADDR.
 *   rom_path keys the presentation assets (cover/info/guides/cheats); save_key
 *   keys the saves and states, which for a patched session are filed under the
 *   PATCH's name -- pass current_ips_srm_source before it is cleared.
 *   Missing assets are the normal case and are skipped silently; a source that
 *   exists but cannot be copied (card full, write error) is NOT -- the caller has
 *   to tell the user, or the export reports success with the saves missing.
 *   Returns 1 if everything that existed was copied, 0 if anything failed.
 */
int patch_export_copy_assets(const uint8_t *rom_path, const uint8_t *save_key);

#endif /* IPS_H */
