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
#include "crc32.h"
#include "cfg.h"

#include <string.h>

extern cfg_t CFG;

/* Whether to re-read the whole patched image back from SDRAM and verify it
   against the BPS-embedded CRC32 after applying.  This is a pure sanity check:
   if the per-byte writes completed without patch_io_err the image is already
   correct.  The re-read walks target_size bytes byte-by-byte over the slow
   MCU<->SDRAM link (~8 s extra for a 4 MB BPS), so it is exposed as the runtime
   menu option "Verify Integrity" (Configuracao > Patch Options), default ON. */
#define BPS_VERIFY_CRC (CFG.patch_verify_integrity)

uint8_t ips_pending_index = 0;

/* The staged list must fit between SRAM_IPS_LIST_ADDR ($FF5000) and the
   favorites mirror ($FF6000).  These fire at compile time so a bumped
   IPS_MAX_PATCHES can never silently start clobbering the favorites. */
_Static_assert(IPS_NAME_BASE + IPS_MAX_PATCHES * IPS_NAME_LEN <= IPS_FLAGS_BASE,
               "IPS display slots overlap the flags region");
_Static_assert(IPS_FLAGS_BASE + IPS_MAX_PATCHES <= IPS_PATH_BASE,
               "IPS flags region overlaps the path slots");
_Static_assert(IPS_PATH_BASE + IPS_MAX_PATCHES * IPS_PATH_LEN <= 4096,
               "IPS list overflows $FF5000..$FF5FFF into the favorites mirror");
_Static_assert(IPS_NAME_BADGE + 4 <= IPS_NAME_LEN,
               "IPS badge does not fit in the display slot");

/* Case-insensitive prefix check: does str start with the first prefix_len
   characters of prefix?  Returns 1 on match, 0 otherwise. */
static int istartswith(const char *str, const char *prefix, size_t prefix_len) {
    for (size_t i = 0; i < prefix_len; i++) {
        char a = str[i], b = prefix[i];
        if (a == 0) return 0;   /* str shorter than prefix: no match, and don't read past its NUL */
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Case-insensitive string compare for sort (returns <0, 0, >0) */
static int istrcmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    if (*a == 0 && *b == 0) return 0;
    return *a ? 1 : -1;
}

/* Scratch storage for up to IPS_MAX_PATCHES entries.  Placed in AHB RAM to avoid
   overflowing the small 16 KB main RAM.  AHB RAM is not zero-initialised;
   entries are fully written before being read.
   Only the BASENAME is kept here, never the full path: the patches always live
   next to the ROM (cfg_parse_patch_entry already relies on that), so the
   directory is recomputed once at publish time.  At IPS_MAX_PATCHES=16 an entry
   array holding full paths would be ~3 KB and blow the AHB region, which has
   roughly a hundred bytes of slack left (see the manual.c note). */
patch_entry_t ips_entries[IPS_MAX_PATCHES] __attribute__((section(".ahbram")));

/* How many entries the last scan staged; CMD_PATCH_META_SAVE writes the yml
   back from these without touching the card again. */
uint8_t ips_scan_count = 0;

int patch_display_name(char *out, int outlen, const char *patch_basename,
                       unsigned stem_len) {
    const char *dot = NULL, *p;
    unsigned len;
    int n = 0;

    if (outlen <= 0) return 0;
    out[0] = 0;
    if (!patch_basename) return 0;

    for (p = patch_basename; *p; p++)
        if (*p == '.') dot = p;
    len = dot ? (unsigned)(dot - patch_basename) : (unsigned)strlen(patch_basename);

    /* Everything after the ROM stem is what actually distinguishes this patch.
       When the patch is exactly "<stem>.ips" there is no suffix, so fall back to
       the whole (extension-less) basename rather than showing an empty row. */
    if (stem_len < len) {
        const char *sfx = patch_basename + stem_len;
        unsigned slen = len - stem_len;
        while (slen && (*sfx == '_' || *sfx == '-' || *sfx == '.' ||
                        *sfx == '(' || *sfx == ' ')) { sfx++; slen--; }
        if (slen) { patch_basename = sfx; len = slen; }
    }

    while (n < outlen - 1 && (unsigned)n < len) { out[n] = patch_basename[n]; n++; }
    out[n] = 0;
    return n;
}

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

/* Does the patch file `fn` belong to the ROM whose basename is `romfile`, whose stem
   (everything before its last '.') is `stem_len` characters long?  Returns the
   PATCH_TYPE_* on a match, -1 otherwise.

   Pure string code, no I/O, so the host tests can cover it -- which matters because
   the last rule below is subtle enough to get wrong silently (cut at the FIRST dot
   instead of the last and "Foo.v2.ips" stops being offered).

   In order:
     1. the extension is exactly "ips"/"bps"                     (patch_ext_type)
     2. the name starts with the ROM stem, case-insensitively    (istartswith)
     3. something follows that stem
     4. the patch's OWN stem is longer than the ROM's.

   Rule 4 is the one that needs explaining.  A patch whose name is the ROM stem plus
   nothing but an extension is INDISTINGUISHABLE from the patch that produced this ROM:
   "create patched ROM" writes <patch stem>.sfc, so the .ips that made it ends up
   sitting next to it under exactly that stem, and it would be offered again on every
   launch.  Re-applying an IPS over an already-patched image corrupts it -- IPS carries
   no checksum, source size or target size to defend itself (unlike BPS, whose CRCs are
   only read when CFG.patch_verify_integrity is on, and it is off by default).
   The legitimate "Foo.sfc + Foo.ips" convention loses out here; the Web Manager renames
   those to "Foo - Patch 1.ips" during its 2.15 card migration so nothing is lost.

   The test is EXACT, not a heuristic: patch_ext_type has already guaranteed a last dot
   with a 3-character extension, so the patch's stem length is strlen(fn)-4, and rule 2
   has already established that the first stem_len characters match.  Equal prefix plus
   equal length means the two stems are the same string. */
int patch_belongs_to_rom(const char *fn, const char *romfile, unsigned stem_len) {
    size_t len;
    int ptype = patch_ext_type(fn);

    if (ptype < 0 || !stem_len) return -1;
    if (!istartswith(fn, romfile, stem_len)) return -1;
    len = strlen(fn);
    if (len <= stem_len) return -1;         /* nothing after the stem */
    if (len - 4 == stem_len) return -1;     /* stem + extension only: see above */
    return ptype;
}

/* Scan the ROM's directory for matching patches into ips_entries[].  Kept in its
   own frame (dirpath[256] + DIR + FILINFO) so it never coexists on the stack
   with the YAML parser's ~272-byte token -- the LPC175x has ~2.5 KB of stack
   headroom and that pair has overflowed into .bss before (cheat menu hang). */
static uint8_t patch_scan_dir(const uint8_t *rom_path) {
    const char *path = (const char *)rom_path;

    /* Find last '/' to split directory from filename */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    const char *filename = last_slash ? last_slash + 1 : path;

    /* Compute stem length (everything before the last '.', or whole name) */
    const char *last_dot = NULL;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') last_dot = p;
    }
    size_t stem_len = last_dot ? (size_t)(last_dot - filename) : strlen(filename);
    if (stem_len == 0) return 0;

    /* Build directory path string.  dirlen is the length WITHOUT a trailing
       slash (0 for the card root), matching how patch_publish reassembles
       "<dirlen bytes>/<basename>" -- keep the two in step. */
    char dirpath[256];
    size_t dirlen = 0;
    if (last_slash && last_slash != path) {
        dirlen = (size_t)(last_slash - path);
        if (dirlen >= sizeof(dirpath)) dirlen = sizeof(dirpath) - 1;
        memcpy(dirpath, path, dirlen);
        dirpath[dirlen] = '\0';
    } else {
        dirpath[0] = '/';
        dirpath[1] = '\0';
    }

    DIR dir;
    FILINFO fno;
    /* Re-use the global LFN buffer (same as scan_dir in filetypes.c).
       CAUTION: every f_readdir below OVERWRITES file_lfn, so rom_path must NOT
       be file_lfn -- `filename`/`stem_len` point into it and would turn to
       garbage on the first directory entry, matching nothing and reporting zero
       patches (a silently missing dialog).  The QUERY_IPS_PATCHES handler keeps
       its own buffer for exactly this reason; don't "optimize" it away. */
    fno.lfsize = 255;
    fno.lfname = (TCHAR *)file_lfn;

    FRESULT res = f_opendir(&dir, dirpath);
    if (res != FR_OK) {
        printf("patch_scan_dir: opendir(%s) failed: %d\n", dirpath, res);
        return 0;
    }

    uint8_t count = 0;

    for (;;) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;
        /* Skip directories, hidden and system entries */
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;

        const char *fn = fno.lfname[0] ? fno.lfname : fno.fname;
        if (fn[0] == '.') continue;

        /* Extension, ROM-stem prefix, and the same-stem rule -- one pure predicate,
           so the host tests can exercise it without FatFs (see patch_belongs_to_rom). */
        int ptype = patch_belongs_to_rom(fn, filename, (unsigned)stem_len);
        if (ptype < 0) continue;

        if (count >= IPS_MAX_PATCHES) break;

        /* Drop anything that would not survive staging intact.  The old code
           truncated silently, which aims f_open() at a different (or missing)
           file; leaving the patch out of the list is the honest failure. */
        if (strlen(fn) >= PATCH_BASENAME_LEN
                || dirlen + 1 + strlen(fn) >= IPS_PATH_LEN) {
            printf("patch_scan_dir: name too long, skipping %s\n", fn);
            continue;
        }

        strcpy(ips_entries[count].basename, fn);
        ips_entries[count].flags = (uint8_t)ptype;
        count++;
    }
    f_closedir(&dir);

    /* Insertion sort by basename, ascending, case-insensitive */
    for (uint8_t i = 1; i < count; i++) {
        patch_entry_t tmp;
        memcpy(&tmp, &ips_entries[i], sizeof(tmp));
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && istrcmp(ips_entries[j].basename, tmp.basename) > 0) {
            memcpy(&ips_entries[j + 1], &ips_entries[j], sizeof(patch_entry_t));
            j--;
        }
        memcpy(&ips_entries[j + 1], &tmp, sizeof(tmp));
    }

    return count;
}

/* Stage the scanned entries into the SNES-visible list: display name, badge,
   flags byte and full path per slot.  The path is assembled straight into the
   write buffer instead of being cached per entry, which is what keeps
   ips_entries[] small enough for the AHB region.  noinline is load-bearing: the
   whole point of the three-frame split is that this 192-byte buffer is NOT live
   while patch_scan_dir's ~490-byte frame or the YAML token are, and inlining it
   into ips_find_patches puts it back on the same frame (measured: 776 bytes of
   peak instead of ~500, on a stack with about 2.4 KB to spare). */
__attribute__((noinline))
static void patch_publish(const uint8_t *rom_path, uint32_t sram_addr,
                          uint8_t count) {
    const char *path = (const char *)rom_path;
    const char *last_slash = NULL, *filename, *last_dot = NULL, *p;
    char buf[IPS_PATH_LEN];
    unsigned stem_len, dirlen;

    for (p = path; *p; p++) if (*p == '/') last_slash = p;
    filename = last_slash ? last_slash + 1 : path;
    for (p = filename; *p; p++) if (*p == '.') last_dot = p;
    stem_len = last_dot ? (unsigned)(last_dot - filename) : (unsigned)strlen(filename);

    if (last_slash && last_slash != path) {
        dirlen = (unsigned)(last_slash - path);
        memcpy(buf, path, dirlen);
    } else {
        dirlen = 0;
    }
    buf[dirlen] = '/';

    for (uint8_t i = 0; i < count; i++) {
        uint32_t slot = sram_addr + IPS_NAME_BASE + (uint32_t)i * IPS_NAME_LEN;
        char name[IPS_NAME_BADGE];
        char badge[4];
        int n;

        n = patch_display_name(name, sizeof(name), ips_entries[i].basename, stem_len);
        sram_writeblock(name, slot, (uint16_t)(n + 1));

        memcpy(badge, (ips_entries[i].flags & PATCH_FLAG_TYPE_MASK) == PATCH_TYPE_BPS
                          ? "BPS" : "IPS", 4);
        sram_writeblock(badge, slot + IPS_NAME_BADGE, sizeof(badge));
        sram_writebyte(ips_entries[i].flags, sram_addr + IPS_FLAGS_BASE + i);

        /* dirlen + 1 + strlen(basename) < IPS_PATH_LEN was checked by the scan */
        strcpy(buf + dirlen + 1, ips_entries[i].basename);
        sram_writeblock(buf, sram_addr + IPS_PATH_BASE + (uint32_t)i * IPS_PATH_LEN,
                        (uint16_t)(strlen(buf) + 1));
    }
}

uint8_t ips_find_patches(const uint8_t *rom_path, uint32_t sram_addr) {
    uint8_t count;

    /* Zero the count byte up front so callers always see a valid value */
    sram_writebyte(0, sram_addr);
    ips_scan_count = 0;

    /* Three SIBLING frames, never nested: the directory scan (dirpath[256] +
       DIR + FILINFO), the publish step (one 192-byte path buffer) and the YAML
       overlay (a ~272-byte yaml_token_t).  Nesting them is what overflowed the
       LPC175x stack into .bss the last time this pattern was written by hand. */
    count = patch_scan_dir(rom_path);
    if (!count) return 0;
    patch_publish(rom_path, sram_addr, count);
    patchmeta_apply(rom_path, ips_entries, count, sram_addr);

    sram_writebyte(count, sram_addr);
    ips_scan_count = count;
    printf("ips_find_patches: %d patch(es) for %s\n", count, rom_path);
    return count;
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
        FPGA_WAIT_RDY_TO(patch_io_err);
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
        FPGA_WAIT_RDY_TO(patch_io_err);
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
        FPGA_WAIT_RDY_TO(patch_io_err);
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
        FPGA_WAIT_RDY_TO(patch_io_err);
        if (patch_io_err) break;
    }
    FPGA_DESELECT();
    return size;
}

/* Read a NUL-terminated string from SRAM (READ + auto-increment), bounded. */
static uint16_t psram_readstrn(void *buf, uint32_t addr, uint16_t size) {
    uint8_t *tgt = buf;
    uint16_t count = size;
    uint16_t elemcount = 0;
    psram_set_addr(addr);
    if (patch_io_err) { *tgt = 0; return 0; }
    FPGA_SELECT();
    FPGA_TX_BYTE(0x88); /* READ */
    while (count--) {
        FPGA_WAIT_RDY_TO(patch_io_err);
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
                  sram_addr + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
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
       staging banks above the ROM area (24-bit offsets reach the whole map). */
    if (adj_max_end > SRAM_SAVE_ADDR - rom_base_addr) {
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
                  sram_addr + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
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
     * (rom_base_addr + target_size, safely below SRAM_SAVE_ADDR) and use
     * that read-only backup for all SourceCopy reads. */
    uint32_t source_base_addr = rom_base_addr + target_size;
    /* Safety bound (mirrors bps_probe_header): never let the backup window
       reach the SaveRAM region. */
    if (source_base_addr + original_rom_size > SRAM_SAVE_ADDR) {
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

    /* Read the BPS-embedded target CRC32 (at fsize-8..fsize-5) before closing,
       only when integrity verification is enabled. */
    uint32_t bps_target_crc32 = 0;
    if (BPS_VERIFY_CRC) {
        uint8_t crc_bytes[4];
        UINT br2;
        f_lseek(&file_handle, file_handle.fsize - 8);
        f_read(&file_handle, crc_bytes, 4, &br2);
        if (br2 == 4) {
            bps_target_crc32 = (uint32_t)crc_bytes[0]
                             | ((uint32_t)crc_bytes[1] << 8)
                             | ((uint32_t)crc_bytes[2] << 16)
                             | ((uint32_t)crc_bytes[3] << 24);
        }
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
      if (BPS_VERIFY_CRC) {
        /* Verify CRC32 of the patched SRAM against the BPS-embedded expected value.
         * Re-reads the whole target image byte-by-byte (~8 s for 4 MB) — gated by
         * the "Verify Integrity" menu option (BPS_VERIFY_CRC).  In copier mode the
         * image is fully patched here (the copier ops are synchronous), so this is
         * a real end-to-end check of the copier path: on MISMATCH we FAIL the apply
         * (err=1 -> load aborts with the error popup) rather than boot a bad ROM. */
        uint32_t crc = crc32_init();
        uint32_t remaining = target_size, addr_off = 0;
        while (remaining > 0) {
            uint16_t chunk = (remaining > (uint32_t)sizeof(file_buf))
                             ? (uint16_t)sizeof(file_buf) : (uint16_t)remaining;
            psram_readblock(file_buf, rom_base_addr + addr_off, chunk);
            for (uint16_t i = 0; i < chunk; i++)
                crc = crc32_update(crc, file_buf[i]);
            addr_off  += chunk;
            remaining -= chunk;
        }
        crc = crc32_finalize(crc);
        printf("bps CRC32: expected=%08lx got=%08lx %s\n",
               (unsigned long)bps_target_crc32,
               (unsigned long)crc,
               crc == bps_target_crc32 ? "OK" : "MISMATCH");
        if (use_copier && bps_target_crc32 && crc != bps_target_crc32) err = 1;
      }
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
                  sram_addr + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
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

    /* Safety bound: never let the probe window reach the SaveRAM region.  For a
     * pathologically large image (or an unusual combo load_address) just bail —
     * the caller's post-patch smc re-detection still handles it correctly. */
    if (scratch_base + out_limit > SRAM_SAVE_ADDR) { file_close(); return 0; }

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
                  sram_addr + IPS_PATH_BASE + (uint32_t)(index - 1) * IPS_PATH_LEN,
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

/* --- Patched-ROM export ------------------------------------------------- */
/* The whole export is Mk.III-only: the mk2's LPC1754 has 122624 bytes of flash and
   this firmware already fills it.  The patch-selector context menu still OPENS on the
   mk2 (its header-mode entry is built for every config); only the "create patched ROM"
   entry is gated there -- greyed, and refused at the top of patch_create_rom, since
   greying alone is cosmetic in that menu -- so nothing can reach these.  Gating a heavy
   feature per config is the established escape hatch when the mk2 flash runs out. */
#ifndef CONFIG_MK2

/* Name the exported ROM after the PATCH FILE, not after a composed label:
   "<dir>/Chrono Trigger (USA) - [BR].ips" -> "<dir>/Chrono Trigger (USA) - [BR].sfc".
   The patch already carries the ROM stem plus whatever separator its author chose,
   so this reads naturally next to the original and needs no sanitising -- it IS a
   filename that exists on this card, so every character in it is already FAT-legal.
   The old scheme wrapped the derived suffix in parentheses and produced things like
   "Chrono Trigger (USA) ([BR]).sfc".
   The yml "Name:" deliberately does NOT feed this: it is a human label for the menu
   row (it may be long, repeated across patches, or contain characters FAT rejects),
   and a display string has no business deciding a filename.
   A patch sharing the ROM's exact stem could derive the ROM's own name, but such a
   patch is never listed in the first place (patch_belongs_to_rom refuses the shape)
   -- and any name collision, the original ROM included, is refused by the f_stat
   check anyway.  Returns 0 if it does not fit. */
static int patch_export_name(char *out, int outlen, const char *patch_path) {
    const char *dot = strrchr(patch_path, '.');
    int n = dot ? (int)(dot - patch_path) : (int)strlen(patch_path);

    if (n + 5 > outlen)
        return 0;
    memcpy(out, patch_path, (size_t)n);
    memcpy(out + n, ".sfc", 5);
    return 1;
}

int patch_export_exists(const uint8_t *patch_path) {
    char out[IPS_PATH_LEN];
    FILINFO fno;

    if (!patch_export_name(out, sizeof(out), (const char *)patch_path))
        return 0;   /* name would not fit -- let the write path report that */
    fno.lfname = NULL;
    if (f_stat((TCHAR *)out, &fno) != FR_OK) return 0;
    /* Stage WHERE the collision is: both refusal paths want the browser to end
       up ON the existing file rather than wherever it happens to be -- the
       CMD_EXPORT_CHECK refusal stays in place (path unread, harmless), and the
       belt-and-braces refusal inside the export reloads the menu, which then
       reads this back the same way a successful export's landing path is read. */
    sram_writestrn((uint8_t *)out, SRAM_EXPORT_PATH_ADDR, sizeof(out));
    return 1;
}

int patch_export_write(const uint8_t *patch_path,
                       uint32_t rom_base_addr, uint32_t size) {
    char out[IPS_PATH_LEN];
    FILINFO fno;

    if (!size || !patch_path[0]) return 0;

    if (!patch_export_name(out, sizeof(out), (const char *)patch_path))
        return 0;

    /* An existing .sfc under this name is REFUSED, never renamed around: the old
       dedup ("<...> 2.sfc".."<...> 9.sfc") silently duplicated a multi-megabyte
       ROM whenever the user re-ran an export by accident, and split the saves
       between two stems.  patch_export_command already checked this BEFORE the
       load and reported PATCH_EXPORT_EXISTS; this re-check is belt and braces. */
    fno.lfname = NULL;
    if (f_stat((TCHAR *)out, &fno) == FR_OK) return 0;

    /* Always headerless: that is the form staged in PSRAM (load_rom seeks past
       any copier header), and it is what every modern tool expects. */
    printf("patch_export: writing %s (%lu bytes)\n", out, (unsigned long)size);
    if (!save_sram((uint8_t *)out, size, rom_base_addr)) return 0;
    /* Publish where it landed so the reloaded browser can walk to it and put the
       cursor on it -- finding a freshly created file by hand in a folder of
       hundreds is exactly the chore this feature should not create. */
    sram_writestrn((uint8_t *)out, SRAM_EXPORT_PATH_ADDR, sizeof(out));
    return 1;
}

/* --- Sidecar asset copy for the patched-ROM export ----------------------- */


/* Stream the ALREADY-OPEN source (global file_handle) into dst_path.  Splitting it
   this way is what lets the caller keep a SINGLE 256-byte path buffer: the source
   name is dead the moment f_open succeeds, so the destination is built over it.
   Returns 1 on success, 0 on failure. */
static int patch_copy_to(const char *dst_path) {
    FIL dst;
    UINT br, bw;
    int ok = 1;

    if (f_open(&dst, (TCHAR *)dst_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        file_close();
        file_res = 0;
        return 0;
    }
    for (;;) {
        if (f_read(&file_handle, file_buf, 512, &br) != FR_OK) { ok = 0; break; }
        if (!br) break;
        if (f_write(&dst, file_buf, br, &bw) != FR_OK || bw != br) { ok = 0; break; }
    }
    /* f_close flushes the last dirty sector AND the directory entry -- if it fails
       the destination is short, so it has to count as a failure or the truncated
       file would survive the f_unlink below. */
    if (f_close(&dst) != FR_OK) ok = 0;
    file_close();
    file_res = 0;
    /* A half-written save or manual is worse than none: it would load as garbage
       instead of being silently absent. */
    if (!ok) f_unlink((TCHAR *)dst_path);
    return ok;
}

/* Do src_key and dst_key name the SAME asset file?  path_asset derives bucket and
   stem from the LEAF of its key, so the built paths are identical exactly when the
   leaf stems match (case-insensitively -- FAT).  This is the NORMAL case for every
   save-keyed sidecar: the saves of a patched session are filed under the PATCH's
   stem, and the export itself is NAMED after the patch -- a colliding name is
   refused outright (PATCH_EXPORT_EXISTS), never renamed around -- so the
   destination stem IS the source stem.
   Streaming a file onto itself is not a copy -- patch_copy_to's FA_CREATE_ALWAYS
   truncates the source mid-read (multi-cluster files then walk a freed FAT chain
   and fail), and its error path f_unlinks the USER'S ORIGINAL save.  Skipping is
   the correct outcome, not a workaround: the asset already sits exactly where the
   exported ROM (same stem) will look for it.  (The sgb/ namespace cannot make two
   equal stems diverge here: the export refuses .gb outright.) */
static int patch_same_asset(const uint8_t *src_key, const uint8_t *dst_key) {
    const char *a = (const char *)src_key, *b = (const char *)dst_key;
    const char *la = a, *lb = b, *da = NULL, *db = NULL, *p;
    unsigned na, nb;

    for (p = a; *p; p++) if (*p == '/') la = p + 1;
    for (p = b; *p; p++) if (*p == '/') lb = p + 1;
    for (p = la; *p; p++) if (*p == '.') da = p;
    for (p = lb; *p; p++) if (*p == '.') db = p;
    na = da ? (unsigned)(da - la) : (unsigned)strlen(la);
    nb = db ? (unsigned)(db - lb) : (unsigned)strlen(lb);
    return na == nb && istartswith(lb, la, na);
}

/* One bucketed sidecar: <root>[sgb/]<BB>/<stem><ext>, src_key -> dst_key.
   Returns 1 copied, 0 nothing to copy (or already in place), -1 tried and failed. */
static int patch_copy_asset(const char *root, const uint8_t *src_key,
                            const uint8_t *dst_key, const char *ext) {
    char path[256];

    if (patch_same_asset(src_key, dst_key)) return 0;   /* self-copy: see above */
    if (path_asset(path, sizeof(path), root, (const char *)src_key, ext) < 0) return 0;
    file_open((uint8_t *)path, FA_READ);
    if (file_res) { file_res = 0; return 0; }   /* asset simply does not exist */

    /* Source open -> its name is dead; reuse the buffer.  Everything below only runs
       when there IS something to copy, which also keeps path_asset_mkdir (and its
       312-byte check_or_create_folder frame) off the common path and stops it from
       littering the card with empty <BB>/ directories for the ~20 absent sidecars. */
    if (path_asset(path, sizeof(path), root, (const char *)dst_key, ext) < 0) {
        file_close();
        file_res = 0;
        return -1;
    }
    path_asset_mkdir(path);
    return patch_copy_to(path) ? 1 : -1;
}

/* Sidecars that live NEXT TO the rom instead of under /sd2snes (only .cov).
   noinline is load-bearing, same as patch_publish: inlined, its 256-byte buffer
   stays live in patch_export_copy_assets across ALL ~22 assets instead of just
   this one, and this chain already sits near the stack ceiling for minutes. */
__attribute__((noinline))
static int patch_copy_sibling(const uint8_t *src_rom, const uint8_t *dst_rom,
                              const char *ext) {
    char path[256];
    const char *dot;
    size_t n;

    /* Unreachable today (patch_belongs_to_rom guarantees the export's stem is longer
       than the ROM's), but self-copy destroys the source -- cheap to rule out. */
    if (patch_same_asset(src_rom, dst_rom)) return 0;

    dot = strrchr((const char *)src_rom, '.');
    n = dot ? (size_t)(dot - (const char *)src_rom) : strlen((const char *)src_rom);
    if (n + strlen(ext) >= sizeof(path)) return 0;
    memcpy(path, src_rom, n); strcpy(path + n, ext);

    file_open((uint8_t *)path, FA_READ);
    if (file_res) { file_res = 0; return 0; }

    dot = strrchr((const char *)dst_rom, '.');
    n = dot ? (size_t)(dot - (const char *)dst_rom) : strlen((const char *)dst_rom);
    if (n + strlen(ext) >= sizeof(path)) { file_close(); file_res = 0; return -1; }
    memcpy(path, dst_rom, n); strcpy(path + n, ext);

    return patch_copy_to(path) ? 1 : -1;
}

/* Build "<pre>NN<post>" by hand.  NOT sprintf: pulling in <stdio.h>'s printf family
   drags ~24 KB of newlib and overflows the mk2 flash outright (this firmware ships a
   custom printf for exactly that reason). */
static void patch_numext(char *out, const char *pre, int n, const char *post) {
    while (*pre) *out++ = *pre++;
    *out++ = (char)('0' + (n / 10));
    *out++ = (char)('0' + (n % 10));
    while (*post) *out++ = *post++;
    *out = 0;
}

/* The sidecars, as (root, ext) pairs.  A table + one loop instead of twenty call
   sites: each inlined call costs more than its table row.  root == NULL means
   "next to the ROM".  Order is cosmetic; a missing file costs one failed f_open. */
struct patch_asset { const char *root; const char *ext; };

static const struct patch_asset patch_assets_rom[] = {
    { NULL,          ".cov"  },   /* browser box art, sibling of the ROM     */
    { GAMEINFO_DIR,  ".gcv"  },   /* info-screen cover                       */
    { GAMEINFO_DIR,  ".gss"  },   /* screenshot                              */
    { GAMEINFO_DIR,  ".fmv"  },   /* clip                                    */
    { GAMEINFO_DIR,  ".pcm"  },   /* clip audio                              */
    { GAMEINFO_DIR,  ".yml"  },   /* metadata (carries the long description) */
    { GAMEINFO_DIR,  ".man"  },   /* guide 1                                 */
    { CHEAT_BASEDIR, ".yml"  },
};

static const struct patch_asset patch_assets_save[] = {
    { SAVE_BASEDIR,  ".srm"  },
    { SAVE_BASEDIR,  ".slot" },
    { SAVE_BASEDIR,  ".mpk"  },
};

int patch_export_copy_assets(const uint8_t *rom_path, const uint8_t *save_key) {
    uint8_t dst[IPS_PATH_LEN];
    char ext[12];
    unsigned i;
    int bad = 0;

    psram_readstrn(dst, SRAM_EXPORT_PATH_ADDR, sizeof(dst));
    if (!dst[0]) return 1;

    /* Presentation assets are keyed off the BASE ROM name. */
    for (i = 0; i < sizeof(patch_assets_rom) / sizeof(patch_assets_rom[0]); i++) {
        int r;
        /* Braces are load-bearing: without them the `else` binds to the INNER if
           and the sibling copy would only run when patch_copy_asset failed. */
        if (patch_assets_rom[i].root) {
            r = patch_copy_asset(patch_assets_rom[i].root, rom_path, dst,
                                 patch_assets_rom[i].ext);
        } else {
            r = patch_copy_sibling(rom_path, dst, patch_assets_rom[i].ext);
        }
        if (r < 0) bad++;
    }
    /* 8 = MAN_MAX_GUIDES, private to manual.c:116 with no header to include. */
    for (i = 2; i <= 8; i++) {
        patch_numext(ext, ".", (int)i, ".man");
        if (patch_copy_asset(GAMEINFO_DIR, rom_path, dst, ext) < 0) bad++;
    }

    /* Saves and states are keyed off the PATCH while a patched game is loaded
       (append_save_basename / savestate.c both prefer current_ips_srm_source), so
       the progress the user actually made with this patch lives under the PATCH's
       name -- copying from the base ROM's name would hand over the WRONG save, or
       none.  save_key is that name; the caller passes it before it is cleared. */
    for (i = 0; i < sizeof(patch_assets_save) / sizeof(patch_assets_save[0]); i++)
        if (patch_copy_asset(patch_assets_save[i].root, save_key, dst,
                         patch_assets_save[i].ext) < 0) bad++;
    for (i = 2; i <= SRM_SLOT_COUNT; i++) {
        patch_numext(ext, ".", (int)i, ".srm");
        if (patch_copy_asset(SAVE_BASEDIR, save_key, dst, ext) < 0) bad++;
    }
    for (i = 1; i <= 4; i++) {
        /* NO dot: savestate.c:34 builds "%02d.state" glued straight onto the stem,
           so the file is <stem>01.state.  Spelling it ".01.state" by analogy with
           the SRAM slots would quietly copy nothing. */
        patch_numext(ext, "", (int)i, ".state");
        if (patch_copy_asset(SS_BASEDIR, save_key, dst, ext) < 0) bad++;
    }

    /* Deliberately NOT copied: MSU-1 (.msu + -N.pcm) is tens of megabytes per track
       and dozens of tracks; /sd2snes/patches/<stem>.yml describes patches the new ROM
       does not have; .gtc is Game Boy only and .gb never reaches the export. */
    return bad == 0;
}

#endif /* !CONFIG_MK2 -- patched-ROM export */
