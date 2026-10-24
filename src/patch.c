/* sd2snes - SD card based universal cartridge for the SNES
   patch.c: ROM patch support (IPS and BPS)
*/

#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "fpga_spi.h"
#include "patch.h"
#include "patchmeta.h"
#include "patch_copier.h"
#include "gameinfo.h"
#include "cheat.h"
#include "savestate.h"
#include "timer.h"

#include <stddef.h>
#include <string.h>

uint8_t ips_pending_index = 0;

/* Both halves of the staged list have to stay inside the region they were given:
   the flags below the favorites mirror at $FF6000, the names and paths inside bank
   $D9, the scan scratch inside bank $DA.  These fire at compile time so a bumped
   IPS_MAX_PATCHES can never silently start clobbering a neighbour. */
_Static_assert(IPS_FLAGS_BASE + IPS_MAX_PATCHES <= IPS_LIST_SIZE,
               "IPS flags overflow $FF5000..$FF5FFF into the favorites mirror");
_Static_assert(IPS_NAME_BASE + IPS_MAX_PATCHES * IPS_NAME_LEN <= IPS_PATH_BASE,
               "IPS display slots overlap the path slots");
_Static_assert(IPS_PATH_BASE + IPS_MAX_PATCHES * IPS_PATH_LEN <= IPS_TEXT_SIZE,
               "IPS paths overflow the bank at SRAM_IPS_TEXT_ADDR");
/* The name slots are addressed by the 65816 as a 16-bit idx*IPS_NAME_LEN plus
   !IPS_NAMES with the bank fixed at ^IPS_NAMES (patchsel_slot_addr), so they may
   not cross a bank; same for the flags, indexed as @IPS_FLAGS,x. */
_Static_assert(IPS_NAME_BASE + IPS_MAX_PATCHES * IPS_NAME_LEN <= 0x10000,
               "IPS display slots cross a bank boundary");
/* num_patches + 1 (the "[No patch]" row) is computed in an 8-bit register on the
   menu side, and the selected index travels back in the single MCU_PARAM+7 byte. */
_Static_assert(IPS_MAX_PATCHES <= 254,
               "IPS_MAX_PATCHES + 1 must fit in a byte for the menu side");
_Static_assert(IPS_NAME_BADGE + 4 <= IPS_NAME_LEN,
               "IPS badge does not fit in the display slot");
/* The scan scratch layout is an ABI written into PSRAM, not just a local array:
   sizeof/offsetof drive the stride and the field offsets read back below. */
_Static_assert(sizeof(patch_entry_t) == PATCH_BASENAME_LEN + 1,
               "patch_entry_t grew padding; the PSRAM scratch stride assumes none");
_Static_assert(IPS_MAX_PATCHES * (PATCH_BASENAME_LEN + 1) <= 0x10000,
               "scan scratch overflows the bank at SRAM_IPS_SCRATCH_ADDR");

/* Alphabetical order of the scan, as INDICES into the PSRAM scratch (which the
   directory walk fills in FAT arrival order).  Sorting index bytes instead of
   129-byte entries is what makes 254 patches affordable: the entries themselves
   live in PSRAM at SRAM_IPS_SCRATCH_ADDR, so the only MCU RAM this costs is one
   byte per patch.  The array it replaces -- patch_entry_t ips_entries[16], 2064
   bytes -- sat in a 16 KB AHB region with a few hundred bytes to spare and could
   never have held 254 of anything.
   AHB RAM is not zero-initialised, which is fine: entries 0..count-1 are fully
   written by the scan before anything reads them, and it is never a DMA target. */
uint8_t patch_order[IPS_MAX_PATCHES] __attribute__((section(".ahbram")));

/* How many entries the last scan staged; CMD_PATCH_META_SAVE writes the yml
   back from these without touching the card again. */
uint8_t ips_scan_count = 0;

/* Classify a filename by its extension: PATCH_TYPE_IPS, PATCH_TYPE_BPS, or -1 when
   it is neither.  ONE implementation for all three sites that need this (the
   directory scan, patch_apply_impl's dispatch to bps_apply, and the Recents/
   Favorites restage in main.c), because they disagreed: the restage tested only
   `(dot[1] | 32) == 'b'`, which also claims ".bak", ".bin" and ".bmp".
   Case-insensitive, and the extension must be EXACTLY three characters. */
int patch_ext_type(const char *name) {
    const char *dot = NULL;
    char e[3];
    int i;

    if (!name) return -1;
    for (const char *p = name; *p; p++)
        if (*p == '.') dot = p;
    if (!dot) return -1;

    for (i = 0; i < 3; i++) {
        char c = dot[1 + i];
        if (!c) return -1;                      /* extension shorter than 3 */
        if (c >= 'a' && c <= 'z') c -= 32;
        e[i] = c;
    }
    if (dot[4]) return -1;                      /* extension longer than 3 */

    if (e[0] == 'I' && e[1] == 'P' && e[2] == 'S') return PATCH_TYPE_IPS;
    if (e[0] == 'B' && e[1] == 'P' && e[2] == 'S') return PATCH_TYPE_BPS;
    return -1;
}

/* === PR#292 fix #1: timeout-bounded SDRAM helpers (patcher only) ===
   The patcher writes ROM bytes through the MCU memory window while the SNES is
   held in reset.  Under an enhancement-chip FPGA core the MCU_RDY line can stay
   deasserted, which hangs the unbounded FPGA_WAIT_RDY forever (the reported
   stall).  These helpers bound every MCU_RDY wait with FPGA_WAIT_RDY_TO and
   latch patch_io_err on timeout; the apply loops poll patch_io_err and abort
   the patch cleanly (returning 0 -> the load fails) instead of wedging the MCU.
   Kept local to patch.c so the timing-critical global sram_* paths used by DMA,
   savestate and normal loading stay unchanged. */
static volatile uint8_t patch_io_err = 0;

/* set_mcu_addr with a bounded ready-wait (mirrors fpga_spi.c set_mcu_addr).
   Once patch_io_err is latched, every helper short-circuits here so a stalled
   load aborts in O(records) rather than re-spending the full timeout per op. */
static void psram_set_addr(uint32_t addr) {
    if (patch_io_err) return;
    FPGA_SELECT();
    FPGA_WAIT_RDY_TO(patch_io_err);
    FPGA_TX_BYTE(FPGA_CMD_SETADDR | FPGA_TGT_MEM);
    FPGA_TX_BYTE((addr >> 16) & 0xff);
    FPGA_TX_BYTE((addr >> 8) & 0xff);
    FPGA_TX_BYTE((addr) & 0xff);
    FPGA_DESELECT();
}

/* Write len bytes from buf to SRAM at addr (WRITE + auto-increment), bounded. */
static void sram_write_from_buf(uint32_t addr, const uint8_t *buf, uint16_t len) {
    psram_set_addr(addr);
    if (patch_io_err) return;
    FPGA_SELECT();
    FPGA_TX_BYTE(0x98); /* WRITE, address auto-increment */
    for (uint16_t i = 0; i < len; i++) {
        FPGA_TX_BYTE(buf[i]);
        FPGA_WAIT_RDY_TO_INLINE(patch_io_err);   /* per-byte loop: keep the wait inline */
        if (patch_io_err) break;
    }
    FPGA_DESELECT();
}

/* memset over SRAM (WRITE + auto-increment), bounded. */
static void psram_memset(uint32_t addr, uint32_t len, uint8_t val) {
    psram_set_addr(addr);
    if (patch_io_err) return;
    FPGA_SELECT();
    FPGA_TX_BYTE(0x98);
    for (uint32_t i = 0; i < len; i++) {
        FPGA_TX_BYTE(val);
        FPGA_WAIT_RDY_TO_INLINE(patch_io_err);
        if (patch_io_err) break;
    }
    FPGA_DESELECT();
}

/* Read size bytes from SRAM into buf (READ + auto-increment), bounded. */
static uint16_t psram_readblock(void *buf, uint32_t addr, uint16_t size) {
    uint8_t *tgt = buf;
    uint16_t count = size;
    psram_set_addr(addr);
    if (patch_io_err) return 0;
    FPGA_SELECT();
    FPGA_TX_BYTE(0x88); /* READ */
    while (count--) {
        FPGA_WAIT_RDY_TO_INLINE(patch_io_err);
        if (patch_io_err) break;
        *(tgt++) = FPGA_RX_BYTE();
    }
    FPGA_DESELECT();
    return size;
}

/* Write size bytes from buf to SRAM (WRITE + auto-increment), bounded. */
static uint16_t psram_writeblock(void *buf, uint32_t addr, uint16_t size) {
    uint8_t *src = buf;
    uint16_t count = size;
    psram_set_addr(addr);
    if (patch_io_err) return 0;
    FPGA_SELECT();
    FPGA_TX_BYTE(0x98); /* WRITE */
    while (count--) {
        FPGA_TX_BYTE(*src++);
        FPGA_WAIT_RDY_TO_INLINE(patch_io_err);
        if (patch_io_err) break;
    }
    FPGA_DESELECT();
    return size;
}

/* Read a NUL-terminated string from SRAM (READ + auto-increment), bounded. */
uint16_t psram_readstrn(void *buf, uint32_t addr, uint16_t size) {
    uint8_t *tgt = buf;
    uint16_t count = size;
    uint16_t elemcount = 0;
    psram_set_addr(addr);
    if (patch_io_err) { *tgt = 0; return 0; }
    FPGA_SELECT();
    FPGA_TX_BYTE(0x88); /* READ */
    while (count--) {
        FPGA_WAIT_RDY_TO_INLINE(patch_io_err);
        if (patch_io_err) break;
        if (!(*(tgt++) = FPGA_RX_BYTE())) break;
        elemcount++;
    }
    /* Step back onto the last byte written to force a terminator -- but ONLY if
       something was written.  When the very first FPGA_WAIT_RDY_TO stalls (exactly
       the case these helpers exist to survive) the loop breaks before tgt++ ever
       runs, and an unconditional tgt-- would read and zero buf[-1]: one byte BEFORE
       the caller's buffer, which in patch_apply_impl is a stack array.  A size of 0
       lands here the same way. */
    if (tgt == (uint8_t *)buf) {
        *tgt = 0;
        FPGA_DESELECT();
        return 0;
    }
    tgt--;
    if (*tgt) *tgt = 0;
    FPGA_DESELECT();
    return elemcount;
}

/* --- IPS copier-header auto-detection ---------------------------------------
 * An IPS carries no metadata about its base ROM, so a patch authored against a
 * headered (512-byte copier prefix) ROM is byte-for-byte indistinguishable from
 * a headerless one by its record offsets alone -- guessing from offsets is
 * exactly the inverted heuristic that used to corrupt unheadered patches (see
 * the long comment in ips_apply below).  Instead of guessing we VALIDATE:
 * materialize the SNES internal header the final image would carry under each
 * hypothesis (literal vs shifted +512) from the base ROM already staged in
 * PSRAM plus the patch's own header-region records, and pick the coherent one.
 * Conservative: shift by 512 ONLY when the headered hypothesis yields a coherent
 * header AND the literal one does not, so a working (matched-convention) patch
 * -- e.g. Zelda: Parallel Worlds, which coheres literal -- is never touched.
 * The detection is FOLDED into ips_apply()'s pass-1 scan (below) so the IPS is
 * read only once; the helpers ips_hdr_overlay()/ips_hdr_coherent() do the work. */
#define IPS_HDR_WIN  0x30u   /* header window [header_addr .. +0x30): covers the
                                map/type/size bytes (+0x25..+0x27) and the
                                checksum-complement/checksum pair (+0x2C/+0x2E) */

static uint8_t ips_hdr_base[IPS_HDR_WIN] __attribute__((section(".ahbram")));
static uint8_t ips_hdr_lit[IPS_HDR_WIN]  __attribute__((section(".ahbram")));
static uint8_t ips_hdr_hdr[IPS_HDR_WIN]  __attribute__((section(".ahbram")));
static uint8_t ips_hdr_tmp[IPS_HDR_WIN]  __attribute__((section(".ahbram")));

/* Overlay one patch record onto a candidate header window, for the bytes that
   land inside [header_addr+adj .. +IPS_HDR_WIN).  A data record (is_rle==0)
   reads the intersecting bytes from the open file at data_start+(p-P); an RLE
   record fills them with rle_val.  Returns the number of bytes written (0 if the
   record misses the window) so the caller can tell whether the patch actually
   authored this header. */
static uint32_t ips_hdr_overlay(uint8_t *win, uint32_t header_addr, uint32_t adj,
                                uint32_t P, uint32_t L, uint32_t data_start,
                                int is_rle, uint8_t rle_val) {
    uint32_t wstart = header_addr + adj;
    uint32_t wend   = wstart + IPS_HDR_WIN;
    uint32_t a = (P > wstart) ? P : wstart;
    uint32_t b = ((P + L) < wend) ? (P + L) : wend;
    if (a >= b) return 0;               /* record misses this window entirely */
    uint32_t n = b - a;
    if (is_rle) {
        for (uint32_t i = 0; i < n; i++) win[(a - wstart) + i] = rle_val;
    } else {
        UINT br;
        f_lseek(&file_handle, data_start + (a - P));
        f_read(&file_handle, ips_hdr_tmp, n, &br);
        for (UINT i = 0; i < br; i++) win[(a - wstart) + i] = ips_hdr_tmp[i];
    }
    return n;
}

/* Is the window a coherent SNES internal header for an image of image_size
   bytes?  Requires the checksum/complement pair to XOR to 0xFFFF and the
   declared ROM-size byte to be consistent with the actual image size. */
static int ips_hdr_coherent(const uint8_t *win, uint32_t image_size) {
    uint16_t comp = (uint16_t)win[0x2C] | ((uint16_t)win[0x2D] << 8);
    uint16_t chk  = (uint16_t)win[0x2E] | ((uint16_t)win[0x2F] << 8);
    if ((uint16_t)(comp ^ chk) != 0xFFFF) return 0;
    uint32_t nominal = 1024u << (win[0x27] & 0x0F);   /* declared ROM size */
    return image_size && (image_size <= nominal) && (image_size > (nominal >> 2));
}

/* How ips_apply() resolved the copier-header convention, for the USB debug
   breadcrumb memory.c writes to $FF072E as 0xD0 + this value:
     0 (D0) offsets applied literally      2 (D2) user forced headered
     1 (D1) auto-detection shifted by 512  3 (D3) user forced headerless */
uint8_t ips_header_adj_used = 0;

uint32_t ips_apply(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                   uint32_t original_rom_size, uint32_t rom_header_size,
                   uint32_t header_addr) {
    if (index < 1 || index > IPS_MAX_PATCHES) return 0;

    patch_io_err = 0; /* PR#292 fix #1: clear stall latch for this apply */

    /* Read the full IPS file path from SRAM */
    uint8_t ips_path[IPS_PATH_LEN];
    psram_readstrn(ips_path,
                  SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
                  sizeof(ips_path));

    /* Header convention for this patch: AUTO (detect) unless the user pinned it
       on the patch screen.  Staged alongside the path so every launch path
       (browser, recents, favorites, autoboot) gets it without a wider ABI. */
    uint8_t pflags = 0;
    psram_readblock(&pflags, sram_addr + IPS_FLAGS_BASE + (uint32_t)(index - 1), 1);
    uint8_t hmode = PATCH_HDR_MODE(pflags);

    printf("Applying IPS: %s\n", ips_path);

    file_open(ips_path, FA_READ);
    if (file_res != FR_OK) {
        printf("ips_apply: open failed (%d)\n", file_res);
        return 0;
    }

    /* Read and verify the 5-byte "PATCH" header */
    uint8_t hdr[5];
    UINT br;
    f_read(&file_handle, hdr, 5, &br);
    if (br != 5 || memcmp(hdr, "PATCH", 5) != 0) {
        printf("ips_apply: bad header\n");
        file_close();
        return 0;
    }

    /* ------------------------------------------------------------------
     * Pass 1: scan all record headers (skipping data bytes with f_lseek)
     * to determine max_end.  If the patch expands the ROM beyond
     * original_rom_size we must zero-fill the new area first — the SRAM
     * may contain old data from a previously loaded larger ROM.
     * ------------------------------------------------------------------ */
    uint32_t max_end = 0;
    uint32_t adj = 0;
    uint32_t adj_max_end = 0;
    uint8_t  rec[3];

    /* Header auto-detection, FOLDED into pass 1 so the IPS is scanned only once
       (a separate detection scan doubled the SD read time on big patches).
       Enabled only for a headerless-convention base (rom_header_size == 0) with a
       known header location; header_addr == 0 disables it (host tests / opt-out).
       We build the SNES internal header the final image would carry under each
       hypothesis -- literal (adj 0) and headered (adj 512) -- from the base ROM
       already in PSRAM plus the patch's own header-region records, then pick the
       coherent one after the scan. */
    int detect = (hmode == PATCH_HDR_AUTO) && (rom_header_size == 0) && (header_addr != 0);
    uint32_t hdr_touched = 0;
    if (detect) {
        psram_readblock(ips_hdr_base, rom_base_addr + header_addr, IPS_HDR_WIN);
        memcpy(ips_hdr_lit, ips_hdr_base, IPS_HDR_WIN);
        memcpy(ips_hdr_hdr, ips_hdr_base, IPS_HDR_WIN);
    }

    for (;;) {
        f_read(&file_handle, rec, 3, &br);
        if (br != 3) break;
        if (rec[0] == 0x45 && rec[1] == 0x4F && rec[2] == 0x46) break; /* EOF */

        uint8_t sz[2];
        f_read(&file_handle, sz, 2, &br);
        if (br != 2) break;
        uint16_t hunk_size = ((uint16_t)sz[0] << 8) | sz[1];

        if (hunk_size == 0) {
            /* RLE: 2-byte count, 1-byte value */
            uint8_t rle[3];
            f_read(&file_handle, rle, 3, &br);
            if (br != 3) break;
            uint32_t offset = ((uint32_t)rec[0] << 16) | ((uint32_t)rec[1] << 8) | rec[2];
            uint32_t rle_count = ((uint16_t)rle[0] << 8) | rle[1];
            if (offset + rle_count > max_end) max_end = offset + rle_count;
            if (detect) {
                uint8_t rle_val = rle[2];
                ips_hdr_overlay(ips_hdr_lit, header_addr, 0, offset, rle_count, 0, 1, rle_val);
                hdr_touched |= ips_hdr_overlay(ips_hdr_hdr, header_addr, 0x200u, offset, rle_count, 0, 1, rle_val);
            }
        } else {
            uint32_t offset = ((uint32_t)rec[0] << 16) | ((uint32_t)rec[1] << 8) | rec[2];
            if (offset + (uint32_t)hunk_size > max_end) max_end = offset + (uint32_t)hunk_size;
            uint32_t data_start = file_handle.fptr;
            if (detect) {
                ips_hdr_overlay(ips_hdr_lit, header_addr, 0, offset, hunk_size, data_start, 0, 0);
                hdr_touched |= ips_hdr_overlay(ips_hdr_hdr, header_addr, 0x200u, offset, hunk_size, data_start, 0, 0);
            }
            /* Skip data bytes (from the captured start; an overlay above may have
               moved the file pointer) */
            f_lseek(&file_handle, data_start + hunk_size);
        }
    }

    /* If the patch writes beyond the original ROM, zero-fill the extension
     * so that gaps between IPS records contain 0x00 as expected by the hack. */
    /* Header-offset correction.  The device loads the ROM with its copier header
     * (rom_header_size bytes, 0 or 512) stripped, so an IPS authored against that
     * same file form lands correctly when we shift record offsets down by exactly
     * the stripped header.
     *
     * We deliberately do NOT try to GUESS a 512-byte header from the record
     * offsets.  The old heuristic (adj=512 when the lowest record offset was
     * < 512, or when max_end-512 was a power of two) was both unreliable AND
     * inverted: a low offset means the patch writes the early ROM region, i.e. it
     * is an UNHEADERED patch that must NOT be shifted.  It silently corrupted
     * every legit unheadered patch that touches the start of the ROM -- e.g.
     * Zelda: Parallel Worlds (lowest offset 22) came out shifted by 512 and
     * failed its CRC.  Standard tools (Lunar IPS / Floating IPS / RomPatcher.js)
     * apply records at their literal offsets; matching the patch's header
     * convention to the ROM is the user's responsibility, exactly as on a PC. */
    /* A user override wins outright, and is ABSOLUTE rather than relative to
       rom_header_size: the image staged in PSRAM is always header-stripped, so
       "headered" means the offsets count the 512 bytes and "headerless" means
       they do not, whatever form the ROM file on the card happened to be in.
       That is what makes HEADERLESS useful on a ROM that does carry a copier
       header, where the default would otherwise shift by 512. */
    ips_header_adj_used = 0;
    if (hmode == PATCH_HDR_HEADERED) {
        adj = 0x200u;
        ips_header_adj_used = 2;
        printf("IPS: header mode forced to headered -> shift offsets by 512\n");
    } else if (hmode == PATCH_HDR_HEADERLESS) {
        adj = 0;
        ips_header_adj_used = 3;
        printf("IPS: header mode forced to headerless -> literal offsets\n");
    } else {
        adj = rom_header_size;
        if (detect) {
            /* Shift +512 ONLY when the headered image coheres AND the literal one does
               not, AND the patch actually authored the headered header (hdr_touched) --
               conservative, so a working matched-convention patch (Zelda: Parallel
               Worlds coheres literal) is never flipped. */
            uint32_t size_lit = max_end;
            uint32_t size_hdr = (max_end > 0x200u) ? (max_end - 0x200u) : 0;
            if (hdr_touched && ips_hdr_coherent(ips_hdr_hdr, size_hdr)
                            && !ips_hdr_coherent(ips_hdr_lit, size_lit)) {
                adj = 0x200u;
                ips_header_adj_used = 1;
                printf("IPS: headered patch auto-detected -> shift offsets by 512\n");
            }
        }
    }
    if (adj > 0)
        printf("IPS: header offset correction: %lu bytes\n", (unsigned long)adj);

    adj_max_end = (max_end > adj) ? (max_end - adj) : 0;

    /* Bound every write to the ROM region.  max_end is the maximum offset+len
       over ALL records (pass 1 scans the same sequence pass 2 applies), so
       this one check covers the zero-fill below and every pass-2 record.  A
       corrupt IPS could otherwise write over the menu image / SaveRAM /
       staging banks above the ROM area (24-bit offsets reach the whole map).
       The ceiling is SRAM_PATCH_TOP, not SRAM_SAVE_ADDR: the cheat records and
       the patch list itself live below the SaveRAM region. */
    if (adj_max_end > SRAM_PATCH_TOP - rom_base_addr) {
        printf("ips_apply: patch exceeds ROM region (end 0x%lx)\n",
               (unsigned long)adj_max_end);
        file_close();
        return 0;
    }

    if (adj_max_end > original_rom_size) {
        uint32_t fill_len = adj_max_end - original_rom_size;
        printf("IPS: zeroing 0x%lx bytes from 0x%lx\n", (unsigned long)fill_len,
               (unsigned long)(rom_base_addr + original_rom_size));
        psram_memset(rom_base_addr + original_rom_size, fill_len, 0x00);
    }

    /* ------------------------------------------------------------------
     * Pass 2: seek back to the start of records and apply the patch.
     * ------------------------------------------------------------------ */
    f_lseek(&file_handle, 5); /* rewind to just after "PATCH" header */

    int err = 0;

    for (;;) {
        if (patch_io_err) { err = 1; break; } /* PR#292 fix #1: SDRAM write stalled */
        f_read(&file_handle, rec, 3, &br);
        if (br != 3) {
            /* Ran out of file without ever seeing the "EOF" marker: the patch
               is truncated and only partially applied — report failure, don't
               boot a half-patched ROM as if it were fine. */
            err = 1;
            break;
        }

        /* IPS EOF marker: bytes 0x45 0x4F 0x46 ('E','O','F') */
        if (rec[0] == 0x45 && rec[1] == 0x4F && rec[2] == 0x46) break;

        /* 24-bit big-endian patch offset */
        uint32_t offset = ((uint32_t)rec[0] << 16)
                        | ((uint32_t)rec[1] <<  8)
                        |  (uint32_t)rec[2];

        /* 16-bit big-endian hunk size */
        uint8_t sz[2];
        f_read(&file_handle, sz, 2, &br);
        if (br != 2) { err = 1; break; }
        uint16_t hunk_size = ((uint16_t)sz[0] << 8) | sz[1];

        if (hunk_size == 0) {
            /* RLE record: 2-byte count + 1-byte fill value */
            uint8_t rle[3];
            f_read(&file_handle, rle, 3, &br);
            if (br != 3) { err = 1; break; }
            uint16_t rle_count = ((uint16_t)rle[0] << 8) | rle[1];
            uint8_t  rle_val   = rle[2];

            /* Skip records entirely within the header region. */
            if (offset + (uint32_t)rle_count <= adj) continue;
            /* Trim leading bytes that fall within the header region. */
            uint32_t rle_skip = (offset < adj) ? (adj - offset) : 0;
            uint32_t sram_off = (offset < adj) ? 0 : (offset - adj);
            uint16_t rle_write = (uint16_t)(rle_count - rle_skip);

            psram_memset(rom_base_addr + sram_off, rle_write, rle_val);
        } else {
            /* Data record: hunk_size bytes of replacement data. */
            /* Skip records entirely within the header region. */
            if (offset + (uint32_t)hunk_size <= adj) {
                f_lseek(&file_handle, file_handle.fptr + hunk_size);
                continue;
            }
            /* Seek past any leading bytes that fall within the header region. */
            uint32_t file_skip = (offset < adj) ? (adj - offset) : 0;
            if (file_skip > 0)
                f_lseek(&file_handle, file_handle.fptr + file_skip);
            uint32_t remain  = hunk_size - file_skip;
            uint32_t cur_off = (offset < adj) ? 0 : (offset - adj);
            while (remain > 0) {
                UINT to_read = (remain > sizeof(file_buf))
                               ? (UINT)sizeof(file_buf)
                               : (UINT)remain;
                f_read(&file_handle, file_buf, to_read, &br);
                if (br == 0) { err = 1; goto ips_apply_done; }
                sram_write_from_buf(rom_base_addr + cur_off, file_buf, (uint16_t)br);
                cur_off += br;
                remain  -= br;
            }
        }
    }

ips_apply_done:
    file_close();
    if (patch_io_err) err = 1; /* PR#292 fix #1: treat a stalled write as failure */
    if (err) printf("ips_apply: error during patching%s\n",
                    patch_io_err ? " (FPGA MCU_RDY timeout - enhancement chip?)" : "");
    else     printf("ips_apply: done, adj=%lu adj_max_end=0x%lx\n",
                    (unsigned long)adj, (unsigned long)adj_max_end);
    return err ? 0 : adj_max_end;
}

/* ------------------------------------------------------------------
 * BPS patch support
 * ------------------------------------------------------------------ */

/* Read-ahead buffer for the BPS action stream.  Reading the patch file
 * one byte at a time through FatFS incurs per-call overhead on every VLI
 * byte; buffering ~256 bytes at once reduces that by ~256x. */
static uint8_t  bps_sb[256] __attribute__((section(".ahbram")));
static uint16_t bps_sb_pos;   /* next byte to consume */
static uint16_t bps_sb_len;   /* valid bytes in buffer */
static uint8_t  bps_eof;      /* latched at EOF; checked by bps_decode_vli */
uint32_t patch_targetread_bytes; /* DEBUG: bytes written byte-by-byte by TargetRead */

/* Minimum well-formed BPS: "BPS1" + 3 one-byte header VLIs + 12-byte CRC
   footer.  Anything smaller would underflow `action_end = fsize - 12` and/or
   run the VLI decoder straight into EOF. */
#define BPS_MIN_FILE_SIZE 19

/* Logical file position = fptr - (bps_sb_len - bps_sb_pos) */
#define BPS_LOGICAL_POS() \
    (file_handle.fptr - (uint32_t)(bps_sb_len - bps_sb_pos))

static uint8_t bps_read_byte(void) {
    if (bps_sb_pos >= bps_sb_len) {
        UINT br;
        f_read(&file_handle, bps_sb, sizeof(bps_sb), &br);
        bps_sb_len = (uint16_t)br;
        bps_sb_pos = 0;
        if (!br) { bps_eof = 1; return 0; } /* latch: a truncated VLI would
                                               otherwise spin forever on the
                                               endless 0x00 stream (no bit 7) */
    }
    return bps_sb[bps_sb_pos++];
}

static uint32_t bps_decode_vli(void) {
    uint32_t data = 0, shift = 1;
    uint8_t x;
    for (;;) {
        x = bps_read_byte();
        if (bps_eof) return 0;  /* caller must check bps_eof */
        data += (uint32_t)(x & 0x7f) * shift;
        if (x & 0x80) break;
        shift <<= 7;
        data += shift;
    }
    return data;
}

/* ------------------------------------------------------------------
 * Shared BPS action-stream decoder, used by both bps_apply (full apply,
 * in-place at rom_base) and bps_probe_header (header-window prefix into
 * a scratch region).
 *
 * Decodes actions from the buffered patch stream until action_end
 * (logical file position) or until output_offset reaches out_limit.
 * Every output write lands inside [out_base, out_base+out_limit) and
 * every Source* read inside [src_base, src_base+source_size), so a
 * malformed/corrupt patch can never read or write outside the caller's
 * windows.  Returns 0 on success, nonzero on error (including a latched
 * patch_io_err FPGA stall).
 * ------------------------------------------------------------------ */
struct bps_actions {
    uint32_t out_base;      /* target image base (written) */
    uint32_t src_base;      /* source image base (read by Source* actions) */
    uint32_t source_size;   /* bounds SourceRead/SourceCopy references */
    uint32_t out_limit;     /* output window size; see strict_limit */
    uint8_t  in_place;      /* out IS the (still-pristine) source image:
                               SourceRead is a no-op (bps_apply) */
    uint8_t  strict_limit;  /* 1: writing past out_limit is an error (apply —
                               a valid BPS writes exactly target_size bytes);
                               0: clamp the final action to the window and
                               stop (probe prefix semantics) */
    uint8_t  emit_copier;   /* 1: SourceCopy/TargetCopy emit an FPGA copier
                               descriptor (patch_copier_emit) instead of moving
                               bytes over the slow SPI window.  SourceRead stays
                               the in_place no-op and TargetRead stays inline
                               (small literals), so the menu only needs to drain
                               the descriptor list afterwards.  See patch_copier.h. */
    uint32_t output_offset; /* out: bytes produced */
    uint32_t n_actions;     /* out: actions decoded (stats) */
};

static int bps_run_actions(struct bps_actions *c, uint32_t action_end) {
    uint32_t source_rel = 0;
    uint32_t target_rel = 0;
    UINT br;
    int err = 0;

    while (BPS_LOGICAL_POS() < action_end && !err
           && c->output_offset < c->out_limit) {
        if (patch_io_err) { err = 1; break; } /* PR#292 fix #1: SDRAM stalled */
        uint32_t d      = bps_decode_vli();
        if (bps_eof) { err = 1; break; }      /* action VLI truncated at EOF */
        uint8_t  action = (uint8_t)(d & 3);
        uint32_t length = (d >> 2) + 1;
        c->n_actions++;

        /* Bound the output window (loop guarantees output_offset < out_limit,
           so the subtraction cannot underflow). */
        if (length > c->out_limit - c->output_offset) {
            if (c->strict_limit) { err = 1; break; }
            length = c->out_limit - c->output_offset;
        }

        switch (action) {
            case 0: /* SourceRead: source[output_offset..] -> target.
                       A valid BPS only SourceReads while output_offset <
                       source_size; bail on a malformed offset rather than
                       pull stale SDRAM into the output. */
                if (c->output_offset >= c->source_size
                        || length > c->source_size - c->output_offset) {
                    err = 1; break;
                }
                if (c->in_place) {
                    /* output IS the source image, and output_offset only
                       moves forward, so those bytes are still pristine —
                       nothing to copy. */
                    c->output_offset += length;
                    break;
                }
                while (length > 0) {
                    uint16_t chunk = (length > (uint32_t)sizeof(file_buf))
                                     ? (uint16_t)sizeof(file_buf) : (uint16_t)length;
                    psram_readblock(file_buf, c->src_base + c->output_offset, chunk);
                    sram_write_from_buf(c->out_base + c->output_offset,
                                        file_buf, chunk);
                    c->output_offset += chunk;
                    length           -= chunk;
                }
                break;

            case 1: { /* TargetRead: literal bytes from the patch file.
                       * Drain the read-ahead buffer first, then bulk-read
                       * the remainder directly into file_buf. */
                patch_targetread_bytes += length; /* DEBUG: measure byte-by-byte literal volume */
                uint16_t avail = bps_sb_len - bps_sb_pos;
                /* clamp avail to length WITHOUT truncating length to 16 bits:
                   (uint16_t)length is 0 when length is a multiple of 0x10000,
                   which would wrongly drop the buffered read-ahead bytes.  avail
                   is <= sizeof(bps_sb) (256), so only length < avail matters. */
                if (length < avail) avail = (uint16_t)length;
                if (avail > 0) {
                    sram_write_from_buf(c->out_base + c->output_offset,
                                        bps_sb + bps_sb_pos, avail);
                    bps_sb_pos       += avail;
                    c->output_offset += avail;
                    length           -= avail;
                }
                while (length > 0) {
                    UINT to_read = (length > (uint32_t)sizeof(file_buf))
                                   ? (UINT)sizeof(file_buf) : (UINT)length;
                    f_read(&file_handle, file_buf, to_read, &br);
                    bps_sb_pos = 0;   /* fptr moved past our buffer */
                    bps_sb_len = 0;
                    if (br == 0) { err = 1; break; }
                    sram_write_from_buf(c->out_base + c->output_offset,
                                        file_buf, (uint16_t)br);
                    c->output_offset += br;
                    length           -= br;
                }
                break;
            }

            case 2: { /* SourceCopy: source[source_rel..] -> target. */
                uint32_t d2    = bps_decode_vli();
                if (bps_eof) { err = 1; break; }
                int32_t  delta = (d2 & 1) ? -(int32_t)(d2 >> 1) : (int32_t)(d2 >> 1);
                source_rel = (uint32_t)((int32_t)source_rel + delta);
                /* A valid BPS only references source bytes within source_size;
                   bail on a malformed patch rather than read foreign SDRAM. */
                if (source_rel >= c->source_size
                        || length > c->source_size - source_rel) {
                    err = 1; break;
                }
                if (c->emit_copier) {
                    /* One copier op: pristine source backup -> target. The whole
                       (possibly multi-MB) relocation moves at PSRAM bandwidth. */
                    if (patch_copier_emit(c->src_base + source_rel,
                                          c->out_base + c->output_offset, length)) {
                        err = 1; break; /* list full -> caller falls back */
                    }
                    c->output_offset += length;
                    source_rel       += length;
                    break;
                }
                while (length > 0) {
                    uint16_t chunk = (length > (uint32_t)sizeof(file_buf))
                                     ? (uint16_t)sizeof(file_buf) : (uint16_t)length;
                    psram_readblock(file_buf, c->src_base + source_rel, chunk);
                    sram_write_from_buf(c->out_base + c->output_offset,
                                        file_buf, chunk);
                    c->output_offset += chunk;
                    source_rel       += chunk;
                    length           -= chunk;
                }
                break;
            }

            case 3: { /* TargetCopy: copy from already-output data.
                       * src and dst offsets may overlap (RLE-style inflate).
                       *
                       * Fast path: when dist==1 the source byte is always the
                       * same value (a true RLE fill).  Read it once and stream
                       * the fill value with psram_memset.
                       *
                       * General path: copy in chunks of min(length, dist, 512)
                       * so we never read ahead of the write cursor. */
                uint32_t d2    = bps_decode_vli();
                if (bps_eof) { err = 1; break; }
                int32_t  delta = (d2 & 1) ? -(int32_t)(d2 >> 1) : (int32_t)(d2 >> 1);
                target_rel = (uint32_t)((int32_t)target_rel + delta);
                /* Spec: TargetCopy may only reference already-written output. */
                if (target_rel >= c->output_offset) { err = 1; break; }
                /* dist is invariant below: target_rel and output_offset
                   advance in lockstep. */
                uint32_t dist = c->output_offset - target_rel;
                if (c->emit_copier) {
                    /* One copier op with overlapping src<dst: the FPGA copies
                       element-by-element forward (read then write, both ++), so
                       any dist (incl. 1) reproduces the RLE inflate natively —
                       no dist==1 / chunk<=dist special-casing needed. */
                    if (patch_copier_emit(c->out_base + target_rel,
                                          c->out_base + c->output_offset, length)) {
                        err = 1; break; /* list full -> caller falls back */
                    }
                    target_rel       += length;
                    c->output_offset += length;
                    break;
                }
                if (dist == 1) {
                    uint8_t fill;
                    psram_readblock(&fill, c->out_base + target_rel, 1);
                    psram_memset(c->out_base + c->output_offset, length, fill);
                    target_rel       += length;
                    c->output_offset += length;
                } else {
                    while (length > 0) {
                        uint32_t chunk = length;
                        if (chunk > sizeof(file_buf))
                            chunk = sizeof(file_buf);
                        if (chunk > dist) chunk = dist;
                        psram_readblock(file_buf, c->out_base + target_rel,
                                       (uint16_t)chunk);
                        sram_write_from_buf(c->out_base + c->output_offset,
                                            file_buf, (uint16_t)chunk);
                        target_rel       += chunk;
                        c->output_offset += chunk;
                        length           -= chunk;
                    }
                }
                break;
            }
        }
    }
    return (err || patch_io_err) ? 1 : 0;
}

uint32_t bps_apply(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                   uint32_t original_rom_size, uint8_t use_copier) {
    if (index < 1 || index > IPS_MAX_PATCHES) return 0;

    patch_io_err = 0; /* PR#292 fix #1: clear stall latch for this apply */

    uint8_t bps_path[IPS_PATH_LEN];
    psram_readstrn(bps_path,
                  SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
                  sizeof(bps_path));

    printf("Applying BPS: %s\n", bps_path);

    file_open(bps_path, FA_READ);
    if (file_res != FR_OK) {
        printf("bps_apply: open failed (%d)\n", file_res);
        return 0;
    }

    /* Verify "BPS1" magic — read directly, buffer not yet active */
    uint8_t magic[4];
    UINT br;
    f_read(&file_handle, magic, 4, &br);
    if (br != 4 || memcmp(magic, "BPS1", 4) != 0) {
        printf("bps_apply: bad magic\n");
        file_close();
        return 0;
    }

    if (file_handle.fsize < BPS_MIN_FILE_SIZE) {
        printf("bps_apply: file too small (%lu)\n",
               (unsigned long)file_handle.fsize);
        file_close();
        return 0;  /* also guards the action_end = fsize - 12 underflow */
    }

    /* Activate buffered stream */
    bps_sb_pos = 0;
    bps_sb_len = 0;
    bps_eof    = 0;

    bps_decode_vli();                        /* source_size — unused */
    uint32_t target_size   = bps_decode_vli();
    uint32_t metadata_size = bps_decode_vli();
    if (bps_eof) {
        printf("bps_apply: truncated header\n");
        file_close();
        return 0;
    }

    /* target_size is an unbounded file VLI.  Reject anything larger than the
       biggest real SNES ROM (8 MB) so a hostile/corrupt header can never push
       the source backup (rom_base + target_size, below) into the SaveRAM/menu/
       cover/cheat staging banks above the ROM region. */
    if (target_size > 0x800000) {
        printf("bps_apply: bad target_size 0x%lx\n", (unsigned long)target_size);
        file_close();
        return 0;
    }

    if (metadata_size > 0) {
        /* Skip metadata: seek the real file cursor forward, then flush buffer.
           Reject a metadata_size that exceeds the file: the VLI is unbounded and
           logical+metadata_size could wrap uint32, silently defeating the skip. */
        uint32_t logical = BPS_LOGICAL_POS();
        if (metadata_size > file_handle.fsize) { file_close(); return 0; }
        f_lseek(&file_handle, logical + metadata_size);
        bps_sb_pos = 0;
        bps_sb_len = 0;
    }

    /* Zero-fill any expansion area.
     * Skip for BPS: the BPS spec guarantees SourceRead only references
     * bytes within source_size, so the expansion region is fully written
     * by TargetRead / TargetCopy actions before it is ever read back. */
    tick_t t_bps_start = getticks();
    (void)t_bps_start; /* used below even when no memset */
    if (target_size > original_rom_size) {
        printf("BPS ROM expansion: 0x%lx -> 0x%lx\n",
               (unsigned long)original_rom_size, (unsigned long)target_size);
        /* No memset needed — expansion area is written before read by BPS actions */
    }

    /* In-place BPS patching overwrites SRAM as target bytes are written.
     * SourceCopy reads from the original source ROM, but those SRAM bytes
     * may have already been overwritten by a previous TargetRead or TargetCopy.
     * Fix: copy the source ROM to a scratch area above the target region
     * (rom_base_addr + target_size, safely below SRAM_PATCH_TOP) and use
     * that read-only backup for all SourceCopy reads. */
    uint32_t source_base_addr = rom_base_addr + target_size;
    /* Safety bound (mirrors bps_probe_header): never let the backup window
       reach the staging banks -- cheat records, the patch list, SaveRAM. */
    if (source_base_addr + original_rom_size > SRAM_PATCH_TOP) {
        printf("bps_apply: backup window exceeds ROM region\n");
        file_close();
        return 0;
    }
    /* Copier mode (Approach B): reset the op counters before the source backup. */
    patch_targetread_bytes = 0;
    if (use_copier) patch_copier_reset();
    if (use_copier && source_base_addr >= rom_base_addr + original_rom_size) {
        /* No overlap (target >= orig: the common chip-converting case, e.g.
           SMW->4MB SA-1) -> the source backup is ONE forward copier op.  It runs
           IMMEDIATELY (not deferred into the list) because it must read the
           pristine source before run_actions overwrites the output. */
        if (patch_copier_op_now(rom_base_addr, source_base_addr, original_rom_size)) {
            file_close();
            return 0;   /* copier failed -> caller falls back */
        }
    } else {
        /* Byte-by-byte: copier OFF, or target < orig so [rom_base,+orig) ->
           [rom_base+target,...) OVERLAPS with dst>src (a forward copier op would
           corrupt it; a backward/DIR op is a future optimization). */
        uint32_t bak_off = 0;
        while (bak_off < original_rom_size) {
            if (patch_io_err) break; /* PR#292 fix #1: SDRAM stalled during backup */
            uint16_t chunk = (original_rom_size - bak_off > (uint32_t)sizeof(file_buf))
                             ? (uint16_t)sizeof(file_buf)
                             : (uint16_t)(original_rom_size - bak_off);
            psram_readblock(file_buf, rom_base_addr + bak_off, chunk);
            psram_writeblock(file_buf, source_base_addr + bak_off, chunk);
            bak_off += chunk;
        }
    }

    /* Action data ends 12 bytes before EOF (3 x CRC32) */
    uint32_t action_end = file_handle.fsize - 12;

    /* Full in-place apply: output goes to rom_base (SourceRead is a no-op),
       SourceCopy reads the pristine backup, and a valid BPS writes exactly
       target_size bytes (strict window).  Copier mode (Approach B): SourceCopy/
       TargetCopy fire the FPGA copier synchronously (the SNES is held in reset);
       TargetRead literals are still byte-by-byte (next optimization candidate). */

    struct bps_actions act = {
        .out_base     = rom_base_addr,
        .src_base     = source_base_addr,
        .source_size  = original_rom_size,
        .out_limit    = target_size,
        .in_place     = 1,
        .strict_limit = 1,
        .emit_copier  = use_copier,
    };
    tick_t t_actions = getticks();
    int err = bps_run_actions(&act, action_end);

    /* Copier mode (Approach B+): run_actions only RECORDED the SourceCopy/TargetCopy
       descriptors into the PSRAM list; now run the whole batch on the copier (one
       trigger + poll).  The deferred ops replay in order, so a TargetCopy reads the
       already-finalized output and SourceCopy reads the pristine backup. */
    if (use_copier && !err && patch_copier_finish()) err = 1;

    /* A valid BPS writes exactly target_size bytes.  Coming up short means a
       truncated action stream or a metadata_size that lseek'd past the data
       (FatFs clamps the seek) — either way the image is incomplete. */
    if (!err && act.output_offset != target_size) {
        printf("bps_apply: incomplete target (0x%lx of 0x%lx)\n",
               (unsigned long)act.output_offset, (unsigned long)target_size);
        err = 1;
    }

    file_close();
    if (patch_io_err) err = 1; /* PR#292 fix #1: treat a stalled write as failure */
    tick_t t_act_elapsed   = getticks() - t_actions;
    tick_t t_total_elapsed = getticks() - t_bps_start;
    printf("bps: %lu actions in %u ticks (%u ms), total %u ticks (%u ms)\n",
           (unsigned long)act.n_actions,
           (unsigned)t_act_elapsed,
           (unsigned)t_act_elapsed * 10u,
           (unsigned)t_total_elapsed,
           (unsigned)t_total_elapsed * 10u);
    if (err) {
        printf("bps_apply: error during patching%s\n",
               patch_io_err ? " (FPGA MCU_RDY timeout - enhancement chip?)" : "");
    } else {
        printf("bps_apply: done, target_size=0x%lx\n", (unsigned long)target_size);
    }
    return err ? 0 : target_size;
}

/* ------------------------------------------------------------------
 * bps_probe_header  (load-time optimization)
 *
 * A chip-converting BPS (e.g. SMW -> SA-1) is otherwise applied TWICE: once
 * under the wrong (pre-patch) core just to discover the new cartridge type, then
 * again under the correct core.  Applying a 4 MB BPS costs ~9 s, so the wasted
 * first pass roughly doubles the load time.
 *
 * This probe materializes ONLY the first `out_limit` bytes of the BPS target
 * image (enough to cover the SNES internal header at 0x7FC0 / 0xFFC0) into a
 * scratch region, so the caller can run smc on the patched header and decide
 * whether a core change is needed BEFORE committing the full, slow patch.
 *
 * It writes output to a scratch base ABOVE both the source ROM and the eventual
 * target image (rom_base + max(target_size, original_rom_size)); SourceRead /
 * SourceCopy therefore read the still-pristine original at rom_base directly, so
 * NO source backup and NO CRC pass are needed, and the real ROM image at
 * rom_base is never touched.  That makes the probe purely advisory: if it is
 * wrong or bails, the caller's post-patch smc re-detection still guarantees
 * correctness (at worst the old, slower behavior).
 *
 * Returns target_size on success (with *out_scratch_base set to where the header
 * window was materialized), or 0 for a non-BPS file / error / bail.
 * ------------------------------------------------------------------ */
uint32_t bps_probe_header(uint32_t sram_addr, uint8_t index,
                          uint32_t rom_base_addr, uint32_t original_rom_size,
                          uint32_t out_limit, uint32_t *out_scratch_base) {
    if (index < 1 || index > IPS_MAX_PATCHES) return 0;

    patch_io_err = 0;

    uint8_t bps_path[IPS_PATH_LEN];
    psram_readstrn(bps_path,
                  SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
                  sizeof(bps_path));
    if (patch_io_err) return 0;

    file_open(bps_path, FA_READ);
    if (file_res != FR_OK) return 0;

    uint8_t magic[4];
    UINT br;
    f_read(&file_handle, magic, 4, &br);
    if (br != 4 || memcmp(magic, "BPS1", 4) != 0) {
        /* Not a BPS (e.g. an IPS) — let the caller fall back to the legacy path */
        file_close();
        return 0;
    }

    if (file_handle.fsize < BPS_MIN_FILE_SIZE) {
        file_close();
        return 0;  /* also guards the action_end = fsize - 12 underflow */
    }

    bps_sb_pos = 0;
    bps_sb_len = 0;
    bps_eof    = 0;

    bps_decode_vli();                        /* source_size — unused */
    uint32_t target_size   = bps_decode_vli();
    uint32_t metadata_size = bps_decode_vli();
    if (bps_eof) { file_close(); return 0; } /* truncated header */

    /* Header must lie within the target image for the probe to be meaningful. */
    if (target_size < out_limit) { file_close(); return 0; }
    /* target_size is an unbounded file VLI.  Reject anything larger than the
       biggest real SNES ROM (8 MB) so a hostile/corrupt header can never push
       the scratch window into the SaveRAM/menu/cover/cheat staging banks above
       the ROM region; the post-patch smc safety net still handles such loads. */
    if (target_size > 0x800000) { file_close(); return 0; }

    if (metadata_size > 0) {
        uint32_t logical = BPS_LOGICAL_POS();
        if (metadata_size > file_handle.fsize) { file_close(); return 0; }
        f_lseek(&file_handle, logical + metadata_size);
        bps_sb_pos = 0;
        bps_sb_len = 0;
    }

    /* Scratch sits above both the source ROM and the eventual target image, so
     * it overlaps neither — SourceCopy can read rom_base directly, unbacked. */
    uint32_t scratch_base = rom_base_addr
        + ((target_size > original_rom_size) ? target_size : original_rom_size);

    /* Safety bound: never let the probe window reach the staging banks.  For a
     * pathologically large image (or an unusual combo load_address) just bail —
     * the caller's post-patch smc re-detection still handles it correctly. */
    if (scratch_base + out_limit > SRAM_PATCH_TOP) { file_close(); return 0; }

    uint32_t action_end = file_handle.fsize - 12;

    /* Probe: materialize only the header window into scratch.  SourceRead is
       a real copy here (scratch != source); Source* actions read the pristine
       ROM at rom_base directly (no backup needed); the final action is
       clamped to the window (non-strict) and we stop right after. */
    struct bps_actions act = {
        .out_base     = scratch_base,
        .src_base     = rom_base_addr,
        .source_size  = original_rom_size,
        .out_limit    = out_limit,
        .in_place     = 0,
        .strict_limit = 0,
    };
    int err = bps_run_actions(&act, action_end);

    file_close();
    if (err) return 0;
    /* Action stream ended before filling the header window -> the probe
       didn't materialize the full header; bail to the safe apply-then-detect
       path instead of advising from a partial image. */
    if (act.output_offset < out_limit) return 0;
    if (out_scratch_base) *out_scratch_base = scratch_base;
    return target_size;
}

static uint32_t patch_apply_impl(uint32_t sram_addr, uint8_t index,
                                 uint32_t rom_base_addr, uint32_t original_rom_size,
                                 uint32_t rom_header_size, uint32_t header_addr,
                                 uint8_t use_copier) {
    if (index < 1 || index > IPS_MAX_PATCHES) return 0;

    /* Read the stored SD path to determine the patch format from its extension */
    patch_io_err = 0; /* PR#292 fix #1: clear before the first SDRAM access */
    uint8_t path[IPS_PATH_LEN];
    psram_readstrn(path,
                  SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
                  sizeof(path));
    if (patch_io_err) { /* SDRAM stalled before we could even read the path */
        printf("patch_apply: FPGA MCU_RDY timeout reading patch path\n");
        return 0;
    }

    if (patch_ext_type((const char *)path) == PATCH_TYPE_BPS)
        return bps_apply(sram_addr, index, rom_base_addr, original_rom_size,
                         use_copier);
    /* IPS is not copier-accelerated (it is already fast vs a big BPS); always the
       legacy in-place apply regardless of use_copier. */
    return ips_apply(sram_addr, index, rom_base_addr, original_rom_size,
                     rom_header_size, header_addr);
}

uint32_t patch_apply(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                     uint32_t original_rom_size, uint32_t rom_header_size,
                     uint32_t header_addr) {
    return patch_apply_impl(sram_addr, index, rom_base_addr, original_rom_size,
                            rom_header_size, header_addr, 0);
}

/* Copier-accelerated apply: for a .bps, decode the action stream and EMIT copier
   descriptors (patch_copier_emit) for the SourceCopy/TargetCopy bulk instead of
   moving the bytes over the SPI window.  The ROM image at rom_base is only PARTIAL
   when this returns (source backup + TargetRead literals are inline; the
   SourceCopy/TargetCopy bytes are produced when the live menu drains the
   descriptor list).  Returns target_size on success (the caller must then run the
   descriptors via the menu copier before booting), or 0 on error / list-full ->
   caller should fall back to patch_apply (byte-by-byte). */
uint32_t patch_apply_copier(uint32_t sram_addr, uint8_t index, uint32_t rom_base_addr,
                            uint32_t original_rom_size, uint32_t rom_header_size,
                            uint32_t header_addr) {
    return patch_apply_impl(sram_addr, index, rom_base_addr, original_rom_size,
                            rom_header_size, header_addr, 1);
}
