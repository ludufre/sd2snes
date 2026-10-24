/* The menu half of the patch support: the pre-boot patch dialog (scan of the ROM's
   folder, the staged list, the Sufami Turbo Slot B picker that shares the dialog) and
   the patch restage of a Recents/Favorites launch.  Menu-only, card overlay code (see
   ovl.h); moved out of patch.c, menucmd.c and sufami.c to go to the card. */

#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "snes.h"
#include "cfg.h"
#include "util.h"
#include "patch.h"
#include "patchmeta.h"
#include "sufami.h"

#include <stddef.h>
#include <string.h>

/* the scan's display order: resident AHB (patch.c), it outlives an overlay reload */
extern uint8_t patch_order[IPS_MAX_PATCHES];

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

/* The scan scratch, addressed in ARRIVAL order (patch_order[] maps display index
   -> arrival index).  Interleaving these FPGA/PSRAM transfers with an open FatFs
   handle is established practice -- cheat_yaml_load does exactly the same while
   its yml is open -- because the sram_* helpers own the chip-select discipline. */
static void scan_entry_write(uint8_t i, patch_entry_t *e) {
    sram_writeblock(e, SRAM_IPS_SCRATCH_ADDR + (uint32_t)i * sizeof(patch_entry_t),
                    (uint16_t)sizeof(patch_entry_t));
}

/* Basename only, and with readstrn rather than a fixed 129-byte block read: a
   basename is typically ~40 characters, and this runs once per comparison of the
   sort (about 1770 of them at 254 patches). */
static void scan_name_read(uint8_t i, char *out, uint16_t len) {
    sram_readstrn(out, SRAM_IPS_SCRATCH_ADDR + (uint32_t)i * sizeof(patch_entry_t)
                       + offsetof(patch_entry_t, basename), len);
}

int patch_basename_at(uint8_t i, char *out, uint16_t len) {
    if (i >= ips_scan_count) { if (len) out[0] = 0; return 0; }
    scan_name_read(patch_order[i], out, len);
    return 1;
}

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
   no checksum, source size or target size to defend itself (BPS has CRCs, but the
   firmware never checks them).
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

/* Scan the ROM's directory for matching patches into the PSRAM scratch, building
   patch_order[] as it goes.  Kept in its own frame (dirpath[256] + DIR + FILINFO
   + the one patch_entry_t it stages and compares through) so it never coexists on
   the stack with the YAML parser's ~272-byte token -- the LPC175x has ~2.5 KB of
   stack headroom and that pair has overflowed into .bss before (cheat menu hang).
   noinline pins that: under LTO the menu-side caller otherwise absorbs this frame
   next to its own 256-byte path buffer. */
__attribute__((noinline))
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

        /* Stage the entry in PSRAM at its arrival index and slot that index into
           patch_order[] so the published list comes out alphabetical -- an
           insertion sort where only INDEX BYTES move.  The comparisons read the
           already-staged basenames back over the FPGA link, which is
           O(count*log count) short reads; sorting the 129-byte entries in place
           would instead be O(count^2) block moves through PSRAM, several
           megabytes of traffic at the top of the range. */
        patch_entry_t ent;
        uint8_t lo = 0, hi = count;

        memset(&ent, 0, sizeof(ent));
        strcpy(ent.basename, fn);
        ent.flags = (uint8_t)ptype;
        scan_entry_write(count, &ent);

        while (lo < hi) {
            uint8_t mid = (uint8_t)(lo + (hi - lo) / 2);
            /* ent.basename is free again -- the entry it held is already staged */
            scan_name_read(patch_order[mid], ent.basename, sizeof(ent.basename));
            if (istrcmp(ent.basename, fn) > 0) hi = mid;
            else                               lo = (uint8_t)(mid + 1);
        }
        memmove(&patch_order[lo + 1], &patch_order[lo], (size_t)(count - lo));
        patch_order[lo] = count;
        count++;
    }
    f_closedir(&dir);

    return count;
}

/* Stage the scanned entries into the SNES-visible list, in DISPLAY order: display
   name, badge, flags byte and full path per slot.  The path is assembled straight
   into the write buffer instead of being cached per entry, which is what keeps the
   scan scratch down to a basename each.  noinline is load-bearing: the whole point
   of the three-frame split is that this 192-byte buffer is NOT live while
   patch_scan_dir's ~490-byte frame or the YAML token are, and inlining it into
   ips_find_patches puts it back on the same frame (measured: 776 bytes of peak
   instead of ~500, on a stack with about 2.4 KB to spare). */
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

    /* patch_order[] turns the arrival order the scan wrote into display order, so
       everything published here -- names, flags AND paths -- is already sorted and
       no consumer needs an indirection to find patch N. */
    for (uint8_t i = 0; i < count; i++) {
        uint32_t slot = SRAM_IPS_TEXT_ADDR + IPS_NAME_BASE + (uint32_t)i * IPS_NAME_LEN;
        uint32_t ent  = SRAM_IPS_SCRATCH_ADDR
                        + (uint32_t)patch_order[i] * sizeof(patch_entry_t);
        char name[IPS_NAME_BADGE];
        char badge[4];
        uint8_t flags;
        int n;

        /* Read the basename back straight into the path buffer behind
           "<dirpath>/", so this loop needs no patch_entry_t of its own -- the
           scan already proved dirlen + 1 + strlen(basename) < IPS_PATH_LEN. */
        sram_readstrn(buf + dirlen + 1, ent + offsetof(patch_entry_t, basename),
                      (uint16_t)(IPS_PATH_LEN - dirlen - 1));
        flags = sram_readbyte(ent + offsetof(patch_entry_t, flags));

        n = patch_display_name(name, sizeof(name), buf + dirlen + 1, stem_len);
        sram_writeblock(name, slot, (uint16_t)(n + 1));

        memcpy(badge, (flags & PATCH_FLAG_TYPE_MASK) == PATCH_TYPE_BPS
                          ? "BPS" : "IPS", 4);
        sram_writeblock(badge, slot + IPS_NAME_BADGE, sizeof(badge));
        sram_writebyte(flags, sram_addr + IPS_FLAGS_BASE + i);

        sram_writeblock(buf, SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE + (uint32_t)i * IPS_PATH_LEN,
                        (uint16_t)(strlen(buf) + 1));
    }
}

uint8_t ips_find_patches(const uint8_t *rom_path, uint32_t sram_addr) {
    uint8_t count;

    /* Zero the count byte up front so callers always see a valid value, and stamp
       the dialog mode in the same breath -- the pre-boot dialog is shared with the
       Sufami Turbo Slot B picker, and a stale IPS_DLGMODE_SLOTB from a previous load
       would word this list as one.  Both have to happen BEFORE the early return
       below, which is the path a ROM with no patches takes. */
    sram_writebyte(0, sram_addr);
    sram_writebyte(IPS_DLGMODE_PATCH, sram_addr + IPS_DLGMODE_OFFSET);
    ips_scan_count = 0;

    /* Three SIBLING frames, never nested: the directory scan (dirpath[256] +
       DIR + FILINFO), the publish step (one 192-byte path buffer) and the YAML
       overlay (a ~272-byte yaml_token_t).  Nesting them is what overflowed the
       LPC175x stack into .bss the last time this pattern was written by hand. */
    count = patch_scan_dir(rom_path);
    if (!count) return 0;
    patch_publish(rom_path, sram_addr, count);
    /* ips_scan_count has to be live BEFORE the overlay runs: patchmeta_apply
       reaches the entries through patch_basename_at, which range-checks against it. */
    ips_scan_count = count;
    patchmeta_apply(rom_path, sram_addr, count);

    sram_writebyte(count, sram_addr);
    printf("ips_find_patches: %d patch(es) for %s\n", count, rom_path);
    return count;
}

/* Patch-aware Recents/Favorites: a list entry may be "<rom>\t<patch_basename>".
   Given such a raw entry, recover the patch, stage it where load_rom() expects
   it and arm ips_pending_index so the relaunch re-applies the patch (reusing the
   same path as a normal LOADROM after ips_find_patches).  `entry` is truncated to
   the bare base ROM path on return (ready to hand to load_rom).  A patch deleted
   since launch degrades to a clean vanilla boot instead of failing. */
void stage_patch_from_entry(char *entry) {
  char patchpath[256];
  const char *pbase;
  ips_pending_index = 0;
  current_ips_srm_source[0] = '\0';
  current_ips_flags = 0;
  if(!cfg_parse_patch_entry(entry, patchpath, sizeof(patchpath))) {
    return; /* no patch tag -> plain base ROM */
  }
  FILINFO fno;
  fno.lfname = NULL;
  if(f_stat((TCHAR*)patchpath, &fno) != FR_OK) {
    printf("stage_patch: patch gone (%s) -> vanilla\n", patchpath);
    return; /* patch deleted -> boot base vanilla */
  }
  /* A path that does not fit the staging slot would be truncated and then opened
     as some OTHER file, so refuse it the same way the directory scan does. */
  if(strlen(patchpath) >= IPS_PATH_LEN) {
    printf("stage_patch: path too long (%s) -> vanilla\n", patchpath);
    return;
  }
  strlcpy_nul((char*)current_ips_srm_source, patchpath, sizeof(current_ips_srm_source));
  /* Stage the patch path where patch_apply()/bps_probe_header() read it, exactly
     as ips_find_patches() would have for a normal LOADROM (slot index 1).  No
     directory scan runs on this path, so drop whatever scan was live: its scratch
     describes some other ROM, and CMD_PATCH_META_SAVE reads basenames from it. */
  ips_scan_count = 0;
  sram_writeblock(current_ips_srm_source, SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE,
                  (uint16_t)(strlen((char*)current_ips_srm_source) + 1));
  /* No directory scan happens on this path, so the header-mode override has to
     come straight from the sidecar -- `entry` is the base ROM path by now, which
     is exactly the key the sidecar is filed under. */
  pbase = path_leaf(patchpath);
  current_ips_flags = patchmeta_flags_for((const uint8_t*)entry, pbase,
                                          patch_ext_type(pbase) == PATCH_TYPE_BPS
                                            ? PATCH_TYPE_BPS : PATCH_TYPE_IPS);
  sram_writebyte(current_ips_flags, SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE);
  ips_pending_index = 1;
}

/* SNES_CMD_QUERY_IPS_PATCHES: list the patches (or, for a .st, the Slot B carts) that
   go with the selected ROM.  Its own function so the 256-byte path below never lands in
   menucmd_dispatch's frame.
   Under -flto this is a tempting inline target, and LTO answers by pulling
   ips_find_patches AND patch_scan_dir in -- merging the scan's frame with qpath and
   keeping both live under patch_publish, the nesting patch.c's "three SIBLING frames"
   note warns about.  patch_scan_dir wants noinline for the same reason patch_publish
   has it; until then this is the deepest path in the file. */
void query_ips_patches(void) {
  /* MUST be its own buffer, never file_lfn: patch_scan_dir points the
     FatFs long-name buffer AT file_lfn (fno.lfname), so the first
     f_readdir would overwrite the ROM path we are scanning for -- the
     stem stops matching, the scan returns 0 patches, and the dialog
     silently never opens.  patch_publish/patchmeta_apply read the same
     pointer afterwards and would key the sidecar off whatever directory
     entry happened to be read last. */
  uint8_t qpath[256];
  get_selected_name(qpath);
  current_ips_srm_source[0] = '\0';
  current_ips_flags = 0;
  /* A Sufami Turbo minicart takes over this query: the very same dialog
     picks its Slot B companion cart, so the firmware publishes the carts
     through the patch contract and flags the list as IPS_DLGMODE_SLOTB.
     A .st never carries a patch, so nothing is lost by not scanning. */
  if(path_is_st((const char*)qpath)) {
    sufami_query_slotb(qpath);
  } else {
    ips_find_patches(qpath, SRAM_IPS_LIST_ADDR);
  }
}

void sufami_query_slotb(const uint8_t *rom_path) {
  const char *path = (const char *)rom_path;
  const char *last_slash = NULL, *leaf;
  char dirpath[256], entry[IPS_PATH_LEN];
  DIR dir;
  FILINFO fno;
  uint8_t count = 0, preselect = 0;
  size_t dirlen = 0;

  sram_writebyte(0, SRAM_IPS_LIST_ADDR);
  sram_writebyte(IPS_DLGMODE_PATCH, SRAM_IPS_LIST_ADDR + IPS_DLGMODE_OFFSET);
  if(!path_is_st(path)) return;

  /* The pair remembered for THIS Slot A cart decides where the dialog opens. */
  slotb_sidecar_load(rom_path);

  for(const char *p = path; *p; p++) if(*p == '/') last_slash = p;
  leaf = last_slash ? last_slash + 1 : path;
  if(last_slash && last_slash != path) {
    dirlen = (size_t)(last_slash - path);
    if(dirlen >= sizeof(dirpath)) dirlen = sizeof(dirpath) - 1;
    memcpy(dirpath, path, dirlen);
    dirpath[dirlen] = 0;
  } else {
    dirpath[0] = '/';
    dirpath[1] = 0;
  }

  /* Same caution as patch_scan_dir: f_readdir writes through fno.lfname into the
     global file_lfn, so `path` must be a buffer of the caller's own. */
  fno.lfsize = 255;
  fno.lfname = (TCHAR *)file_lfn;
  if(f_opendir(&dir, dirpath) != FR_OK) {
    DBG_SUFAMI printf("sufami_query_slotb: opendir(%s) failed\n", dirpath);
    return;
  }

  for(;;) {
    const char *fn;
    size_t n;
    if(f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
    if(fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
    fn = fno.lfname[0] ? fno.lfname : fno.fname;
    if(fn[0] == '.') continue;
    if(!path_is_st(fn)) continue;
    /* The Slot A cart itself is not a candidate: a real Sufami Turbo has two
       physical slots, and the same cartridge cannot be in both. */
    if(!strcasecmp(fn, leaf)) continue;
    if(count >= IPS_MAX_PATCHES) break;

    n = strlen(fn);
    if(dirlen + 1 + n >= IPS_PATH_LEN) {
      DBG_SUFAMI printf("sufami_query_slotb: name too long, skipping %s\n", fn);
      continue;
    }
    memcpy(entry, dirpath, dirlen);
    entry[dirlen] = '/';
    memcpy(entry + dirlen + 1, fn, n + 1);
    if(sufami_slotb_path[0] && !strcasecmp(entry, sufami_slotb_path)) preselect = count + 1;
    sram_writeblock(entry, SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                           + (uint32_t)count * IPS_PATH_LEN, dirlen + 1 + n + 1);

    /* Display slot.  A patch entry reserves IPS_NAME_BADGE.. for the IPS/BPS badge,
       but every entry in THIS list is a minicart, so the badge would say nothing --
       the dialog skips that column for Slot B (dlg_badge) and the name gets the whole
       slot, IPS_NAME_LEN-1 characters instead of IPS_NAME_BADGE-1.
       The ".st" is dropped too: it is the same on every row, and those three columns
       are worth more to a No-Intro filename than to an extension nobody is comparing. */
    {
      size_t shown = n;
      if(shown > 3 && !strcasecmp(fn + shown - 3, ".st")) shown -= 3;
      if(shown > IPS_NAME_LEN - 1) shown = IPS_NAME_LEN - 1;
      memset(entry, 0, IPS_NAME_LEN);
      memcpy(entry, fn, shown);
    }
    sram_writeblock(entry, SRAM_IPS_TEXT_ADDR + IPS_NAME_BASE
                           + (uint32_t)count * IPS_NAME_LEN, IPS_NAME_LEN);
    /* PATCH_TYPE_BPS, deliberately: it is the type the dialog draws NO header-mode
       marker for (a .bps has no header convention), which is what a cart entry
       needs -- and the Y context menu is switched off for this list anyway. */
    sram_writebyte(PATCH_TYPE_BPS, SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE + count);
    count++;
  }
  f_closedir(&dir);

  sram_writebyte(count, SRAM_IPS_LIST_ADDR);
  sram_writebyte(IPS_DLGMODE_SLOTB, SRAM_IPS_LIST_ADDR + IPS_DLGMODE_OFFSET);
  /* The dialog opens on listsel_sel 0 ("no cart"); a remembered pair is reported
     so a future revision can pre-select it. */
  DBG_SUFAMI printf("Sufami Slot B candidates: %d (remembered #%d)\n", count, preselect);
}
