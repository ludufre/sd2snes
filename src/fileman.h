#ifndef _FILEMAN_H
#define _FILEMAN_H

#include <stdint.h>

/* --- File manager: copy / cut / paste / rename / view, from the browser ---
 *
 * Three parties share this header:
 *   - the firmware (menucmd.c, fileman.c): SNES_CMD_FM, the clipboard, bank $C3 and
 *     opening the card module;
 *   - the card module fileops.bin (src/modules/fileops/): the engine (copy trees, move,
 *     rename, conflicts, sidecars, list entries, the text viewer);
 *   - the menu (snes/fileman*.a65, bank $C3): selection mode, the clipboard icon, the
 *     progress / conflict / rename / view screens.
 * The byte offsets are the interface: mirrored as FM_* in snes/memmap.i65 and pinned by the
 * _Static_asserts below.
 *
 * WHERE THINGS LIVE (menu mode only; a game load drops all of it):
 *   FM_MARKS  BSRAM, SNES-written: one bit per entry of the directory table the browser
 *             shows (bit i = entry i in SORTED order, byte i>>3, bit i&7).  In $FF because
 *             the menu can only write $F0-$FF.
 *   FM_STAT   BSRAM, module-written status block (fm_stat_t), polled by the menu.  The menu
 *             zeroes the magic before every SNES_CMD_FM that opens the module.
 *   FM_NAME   BSRAM, SNES-written: the new leaf name for FM_OP_RENAME (NUL-terminated).
 *   FM_CLIPINFO BSRAM, firmware-written: what the clipboard icon shows.
 *   FM_CLIP   PSRAM bank $E1.., firmware-written by FM_OP_CLIP, read by the module.  Banks
 *             $E0-$EF are the game's save RAM in game mode and idle in menu mode.
 *   FM_VIEW   PSRAM bank $E4.., module-written text lines, read by the menu.
 *
 * THE CLIPBOARD IS FIRMWARE RAM.  Its authority is the firmware's own fm_clip_count (.bss,
 * zero at power-on, cleared by every game load), not anything in PSRAM: the PSRAM copy only
 * carries the names.  So "gone at power-off and at a game start" needs no code beyond that.
 */

#define FM_VERSION          1

/* SNES_CMD_FM (0x50): MCU_PARAM+8 = op.  MCU_PARAM+0..7 are the usual cwd pointer and
   selected entry (get_selected_name) where an op needs them. */
#define FM_OP_PROBE         0   /* publish FM_CLIPINFO (+ its magic: the capability) */
#define FM_OP_BANK          1   /* make sure the menu's bank $C3 is in PSRAM */
#define FM_OP_CLIP          2   /* +9 = FM_CLIP_COPY / FM_CLIP_CUT, +10..11 = entries in the
                                   directory table; the marked entries of FM_MARKS in the
                                   folder at MCU_PARAM+0..2 become the clipboard */
#define FM_OP_UNCLIP        3   /* empty the clipboard */
#define FM_OP_PASTE         4   /* open fileops.bin: paste the clipboard into the cwd */
#define FM_OP_RENAME        5   /* open fileops.bin: rename the selected entry to FM_NAME */
#define FM_OP_VIEW          6   /* open fileops.bin: view the selected entry as text */
#define FM_OP_MKDIR         7   /* open fileops.bin: make folder FM_NAME in the cwd */
#define FM_OP_RMDIR         8   /* open rmdir.bin: delete a folder and everything in it --
                                   the selected entry, or with +9 = 1 the folder the browser
                                   shows (an MSU-1 folder opened as its game).  It counts
                                   first and asks (FM_ASK_RMDIR); only the folder's own
                                   contents go: the files /sd2snes keeps by game (saves,
                                   cheats, info...) are keyed by stem and may belong to a
                                   same-named game elsewhere, so they stay */

#define FM_OP_RMSEL         9   /* open rmdir.bin: delete the entries of FM_MARKS in the folder the
                                   browser shows (MCU_PARAM+0..2), files and folders, the
                                   same way as FM_OP_RMDIR (count, FM_ASK_RMDIR, remove) */
#define FM_OP_NAMECHK      10   /* would the browser list a file named FM_NAME in the folder at
                                   MCU_PARAM+0..2?  The answer is FM_CLIPINFO+7: 1 = no (no
                                   extension, an unknown one, a leading dot), and the rename
                                   asks before using it.  An older firmware leaves 0 there */

#define FM_CLIP_NONE        0
#define FM_CLIP_COPY        1
#define FM_CLIP_CUT         2

/* SNES_CMD_MOD_CTL while fileops.bin is open: MCU_PARAM+0 = control, +1.. its argument */
#define FM_CTL_CANCEL       1   /* stop at the next block; a half-copied file is removed */
#define FM_CTL_ANSWER       2   /* +1 = FM_ANS_*, +2 = 1: the same answer for every later conflict */
#define FM_CTL_VIEW_WIN     3   /* +1..2 = window index (u16 LE): render that window */
#define FM_CTL_CLOSE        4

#define FM_ANS_REPLACE      1
#define FM_ANS_SKIP         2
#define FM_ANS_KEEPBOTH     3   /* paste as "name (2)", "name (3)"... */

/* fm_stat_t.state */
#define FM_ST_NONE          0
#define FM_ST_SCAN          1   /* counting what the paste will copy */
#define FM_ST_RUN           2   /* copying / moving */
#define FM_ST_ASK           3   /* waiting for FM_CTL_ANSWER about cur_name (ask = kind) */
#define FM_ST_DONE          4   /* finished: n_done / n_skipped / n_failed say how */
#define FM_ST_ERROR         5   /* stopped: err says why */
#define FM_ST_VIEW          6   /* a window of text lines is in FM_VIEW */
#define FM_ST_BUSY          7   /* rendering a window */

/* fm_stat_t.err */
#define FM_ERR_NONE         0
#define FM_ERR_NOSPACE      1   /* not enough free space for the copy (checked up front) */
#define FM_ERR_INTO_SELF    2   /* a folder pasted into itself or below itself */
#define FM_ERR_PATH         3   /* a path would not fit 255 characters */
#define FM_ERR_IO           4   /* the card refused a read / write */
#define FM_ERR_EXISTS       5   /* rename / mkdir: the name is taken */
#define FM_ERR_CANCELLED    6
#define FM_ERR_BADNAME      7   /* empty, or a character FAT does not take */
#define FM_ERR_NOCLIP       8   /* nothing on the clipboard */
#define FM_ERR_NOTFOUND     9   /* the source is gone (deleted since it was copied) */
#define FM_ERR_FULL         10  /* the clipboard cannot hold that many names */
#define FM_ERR_SYSTEM       11  /* delete: the card root, /sd2snes, or a folder of it the
                                   system needs (only its info / cheats / saves / states /
                                   patches trees may go) */
#define FM_ERR_RDONLY       12  /* delete: an entry is read-only (left, with its folders) */

/* fm_stat_t.ask: what collides with what */
#define FM_ASK_FILE         1   /* a file over a file */
#define FM_ASK_DIR          2   /* a folder over a folder: replace = merge into it */
#define FM_ASK_MIXED        3   /* a file over a folder or a folder over a file */
#define FM_ASK_RMDIR        4   /* delete the folder cur_name (rm_files, rm_dirs, bytes_total KB)?
                                   FM_ANS_REPLACE = yes, FM_CTL_CANCEL = no */

/* fm_stat_t.flags */
#define FM_FL_ROM_ASSETS    0x01 /* rename: the game's saves / covers / cheats went along */
#define FM_FL_LISTS         0x02 /* Recents / Favorites entries were rewritten */
#define FM_FL_ASSET_SKIP    0x04 /* a game file was left behind: the new name already had one */
#define FM_FL_UNCLIP        0x08 /* delete: everything on the clipboard was in the deleted
                                    folder; the menu empties the clipboard (FM_OP_UNCLIP) */

#define FM_MAGIC0           ('F')
#define FM_MAGIC1           ('S')

#define FM_NAME_LEN         128

typedef struct __attribute__((packed)) {
  uint8_t  magic[2];        /* +0  'F','S' */
  uint8_t  version;         /* +2  FM_VERSION */
  uint8_t  state;           /* +3  FM_ST_* */
  uint8_t  op;              /* +4  FM_OP_* being served */
  uint8_t  err;             /* +5  FM_ERR_* (state ERROR, or the last failure in DONE) */
  uint8_t  ask;             /* +6  FM_ASK_* while state == ASK */
  uint8_t  flags;           /* +7  FM_FL_* */
  uint32_t bytes_done;      /* +8 */
  uint32_t bytes_total;     /* +12 (a delete: the folder's size in KB, rounded up) */
  uint16_t items_done;      /* +16 files + folders handled */
  uint16_t items_total;     /* +18 */
  uint16_t n_done;          /* +20 copied / moved */
  uint16_t n_skipped;       /* +22 */
  uint16_t n_failed;        /* +24 */
  uint8_t  progress;        /* +26 0..255 of bytes (items when there are no bytes): the
                                   65816 has no divide, the bar is progress * w / 256 */
  uint8_t  view_pct;        /* +27 0..100: where the window starts in the file */
  uint16_t view_lines;      /* +28 lines in FM_VIEW for the current window */
  uint16_t view_win;        /* +30 the window index rendered */
  uint8_t  view_last;       /* +32 1 = this window reaches the end of the file */
  uint8_t  rsvd0;           /* +33 */
  uint16_t rm_files;        /* +34 delete: files in the folder (all levels) */
  uint16_t rm_dirs;         /* +36 delete: folders in it (all levels) */
  uint8_t  rsvd[2];         /* +38 */
  uint8_t  cur_name[FM_NAME_LEN]; /* +40 leaf being handled / asked about, font-encoded, NUL */
  uint8_t  rsvd2[88];       /* +168 .. 256 */
} fm_stat_t;

_Static_assert(sizeof(fm_stat_t) == 256, "fm_stat_t is the FM_STAT block");
_Static_assert(__builtin_offsetof(fm_stat_t, bytes_done) == 8, "FM_BYTES_DONE");
_Static_assert(__builtin_offsetof(fm_stat_t, items_done) == 16, "FM_ITEMS_DONE");
_Static_assert(__builtin_offsetof(fm_stat_t, progress) == 26, "FM_PROGRESS");
_Static_assert(__builtin_offsetof(fm_stat_t, view_lines) == 28, "FM_VIEW_LINES");
_Static_assert(__builtin_offsetof(fm_stat_t, view_last) == 32, "FM_VIEW_LAST");
_Static_assert(__builtin_offsetof(fm_stat_t, cur_name) == 40, "FM_CUR_NAME");
_Static_assert(__builtin_offsetof(fm_stat_t, rm_files) == 34, "FMS_RM_FILES");
_Static_assert(__builtin_offsetof(fm_stat_t, rm_dirs) == 36, "FMS_RM_DIRS");

/* FM_CLIPINFO: +0 'F' +1 'C' +2 FM_VERSION +3 mode (FM_CLIP_*) +4..5 count (u16 LE) +6 the
   last FM_OP_CLIP's FM_ERR_* +7 the FM_OP_NAMECHK answer (0 after any other op) */
#define FM_CLIPINFO_LEN     8

/* The clipboard in PSRAM (FM_CLIP):
     +0   mode (FM_CLIP_*)
     +1   reserved
     +2   count (u16 LE)
     +4   the source folder, NUL-terminated (256 bytes)
     +260 count records: one type byte (FM_ENT_FILE / FM_ENT_DIR), then the leaf name,
          NUL-terminated.  The records stop before FM_CLIP_END. */
#define FM_CLIP_HDR         260
#define FM_ENT_FILE         1
#define FM_ENT_DIR          2

/* FM_VIEW: line i of the window at FM_VIEW + i * FM_VIEW_STRIDE, font-encoded, NUL-terminated,
   at most FM_VIEW_COLS characters (a longer line wraps onto the next record).  Byte
   FM_VIEW_PCT of the record = where that line starts in the file, 0..100 (the viewer's
   position on its border; the 65816 has no divide). */
#define FM_VIEW_STRIDE      64
#define FM_VIEW_COLS        56
#define FM_VIEW_PCT         63
#define FM_VIEW_MAXLINES    2048
#define FM_VIEW_WINDOW      32768   /* bytes of the file one window covers, at most */

/* firmware side (fileman.c) */
void fileman_command(void);     /* SNES_CMD_FM */
void fileman_publish(void);     /* FM_CLIPINFO, on every menu load and after each change */
void fileman_forget(void);      /* empty the clipboard: every game load */

#endif
