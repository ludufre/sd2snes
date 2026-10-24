/* The patched-ROM export (its .sfc and the sidecars that go with it) and the sidecar
   cleanup of a deleted ROM: menu-only, card overlay code (see ovl.h).  Moved out of
   patch.c to go to the card. */

#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "patch.h"
#include "gameinfo.h"
#include "cheat.h"
#include "savestate.h"
#include "patchmeta.h"   /* PATCH_BASEDIR */

#include <stddef.h>
#include <string.h>

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

/* --- Patched-ROM export ------------------------------------------------- */
/* Built for every config: the export behaves identically on both boards. */

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

/* noinline: under LTO this is inlined into its single caller and its path buffer
   would sit in that caller's frame for the whole menu loop, under every call chain. */
int __attribute__((noinline)) patch_export_write(const uint8_t *patch_path,
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
int patch_copy_to(const char *dst_path) {
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
    { GAMEINFO_DIR,  ".qoa"  },   /* clip audio, compressed (wins over .pcm) */
    { GAMEINFO_DIR,  ".yml"  },   /* metadata (carries the long description) */
    { GAMEINFO_DIR,  ".man"  },   /* guide 1                                 */
    { CHEAT_BASEDIR, ".yml"  },
};

static const struct patch_asset patch_assets_save[] = {
    { SAVE_BASEDIR,  ".srm"  },
    { SAVE_BASEDIR,  ".yml"  },   /* per-game settings and slots (gameopt.h) */
    { SAVE_BASEDIR,  ".slot" },   /* the slots of before                     */
    { SAVE_BASEDIR,  ".mpk"  },
};

/* Delete every PRESENTATION sidecar of `rom_path`.  Runs when the ROM itself is deleted
   from the menu: its box art, info screen, guides, clip and cheat file describe a game
   that is no longer there, and nothing else on the card ever collects them (the Web
   Manager cannot tell an orphan from an asset whose ROM is simply not inserted).

   Battery saves, memory packs and savestates are deliberately NOT touched: the context
   menu has its own "delete save" action, and progress lost to a mis-pressed delete does
   not come back.  That is why this walks patch_assets_rom and not patch_assets_save.

   Sharing patch_assets_rom with the export is the point -- the two can never disagree
   about what belongs to a ROM.  The patch sidecar (PATCH_BASEDIR) is the one addition:
   it is keyed by the ROM but stays out of the export table, because a freshly exported
   ROM starts with an empty patch list.

   Best effort, and an absent file is the NORMAL case (f_unlink fails, we move on), so the
   return value is informational only -- the caller's NACK belongs to the ROM itself. */
__attribute__((noinline))
int patch_unlink_rom_assets(const uint8_t *rom_path) {
    char path[256];
    char ext[12];
    unsigned i;
    int n = 0;

    for (i = 0; i < sizeof(patch_assets_rom) / sizeof(patch_assets_rom[0]); i++) {
        const char *aext = patch_assets_rom[i].ext;
        if (patch_assets_rom[i].root) {
            if (path_asset(path, sizeof(path), patch_assets_rom[i].root,
                           (const char *)rom_path, aext) < 0) continue;
        } else {
            /* sibling of the ROM: swap the extension, exactly like patch_copy_sibling */
            const char *dot = strrchr((const char *)rom_path, '.');
            size_t len = dot ? (size_t)(dot - (const char *)rom_path)
                             : strlen((const char *)rom_path);
            if (len + strlen(aext) >= sizeof(path)) continue;
            memcpy(path, rom_path, len);
            strcpy(path + len, aext);
        }
        if (f_unlink((TCHAR *)path) == FR_OK) { printf("deleted %s\n", path); n++; }
    }
    /* guides 2..8 ("<stem>.0N.man"); 8 = MAN_MAX_GUIDES, private to manual.c:116 */
    for (i = 2; i <= 8; i++) {
        patch_numext(ext, ".", (int)i, ".man");
        if (path_asset(path, sizeof(path), GAMEINFO_DIR, (const char *)rom_path, ext) >= 0
            && f_unlink((TCHAR *)path) == FR_OK) n++;
    }
    /* per-ROM patch metadata: header mode + display name of each of its patches */
    if (path_asset(path, sizeof(path), PATCH_BASEDIR, (const char *)rom_path, ".yml") >= 0
        && f_unlink((TCHAR *)path) == FR_OK) n++;
    return n;
}

/* noinline: under LTO this is inlined into its single caller and its path buffer
   would sit in that caller's frame for the whole menu loop, under every call chain. */
int __attribute__((noinline)) patch_export_copy_assets(const uint8_t *rom_path, const uint8_t *save_key) {
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
