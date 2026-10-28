/* The browser's file manager, the card module fileops.bin -- see ../../fileman.h for the
 * contract (ops, the FM_STAT block, the clipboard, the viewer lines) and ../../fileman.c for
 * the firmware's part (the clipboard, opening this module).
 *
 * WHAT RUNS WHERE:
 *   start()  RENAME and MKDIR are done there (one f_rename / f_mkdir and the bookkeeping),
 *            VIEW renders its first window there, PASTE only sets up: it checks the
 *            clipboard, refuses a folder pasted into itself and publishes SCAN or RUN.
 *   idle()   the paste, a slice at a time: every menu-loop pass does about one tick (10 ms)
 *            of work -- a few directory entries, a few blocks of a file -- and returns, so
 *            the menu keeps polling and FM_CTL_CANCEL lands within a block.
 *   ctl()    the menu's answers: cancel, the answer to a conflict, another viewer window.
 *
 * THE PASTE: a COPY first scans what it will write (files, folders, bytes) and checks the
 * free space against it, rounding every file up to whole clusters and giving every folder
 * one: nothing is written when it does not fit.  Then it copies, file by file.  A CUT is
 * f_rename only, the card never copies a byte for it; a folder that meets a folder of the
 * same name (answer "replace") is merged, its entries moved one by one and the emptied
 * source removed.  Pasting into the folder the clipboard came from keeps both (a copy) or
 * does nothing (a cut).
 *
 * FOLDER WALKS use an explicit stack in PSRAM (SRAM_FM_WORK_ADDR): one frame per level, the
 * open DIR object and the two path lengths.  Never C recursion: a level would cost the
 * paths' 512 bytes and the MCU stack is a few KB.  The two working paths (source P,
 * destination Q) live in the spare AHB RAM behind the image and only grow and shrink at
 * their ends: P always starts with the clipboard's folder, Q with the destination.
 *
 * GAMES: a game's saves, states, cheats, info and patch choices live under /sd2snes keyed by
 * the LEAF stem (path_asset), and a cover / MSU-1 pack sits next to it under the same stem.
 * When a game's name changes (rename, a pasted "keep both") the /sd2snes files follow it; a
 * cut to another folder takes the cover and the MSU-1 pack along.  A copy never duplicates
 * saves or states: two games must not share progress.  Recents / Favorites entries follow
 * every rename and cut.
 *
 * DELETE (FM_OP_RMDIR: one folder; FM_OP_RMSEL: the entries marked in the selection mode,
 * files and folders) walks twice with the same stack: a count (files, folders, size, for the
 * question the menu asks) and then the removal, children first, every folder unlinked when
 * its walk pops.  Only the folder's own tree goes: a game's files under
 * /sd2snes are keyed by its stem alone, and another game of the same name in another folder
 * owns them too.  The card root, /sd2snes and its system folders are refused (rm_allowed).
 * A read-only entry is left (with the folders above it) and the rest goes on; a card that
 * stops answering ends the op.
 *
 * TWO CARD IMAGES FROM THIS FILE.  fileops.bin (everything but the delete) sits right under
 * its memory ceiling: the image and the spare it needs (FOP_NBUF paths + one sector) share
 * the firmware's module block, so the delete is its own image, rmdir.bin
 * (../rmdir/rmmod.c: FOP_RMDIR_ONLY, nothing but the delete).  Host builds
 * (MODULE_HOST_LINKED) link one module with both.
 *
 * No globals and no library: the state is the fopmod_t below (in the FRAME the firmware
 * lends), the buffers are the spare RAM, and every string helper is local.
 */

#include "module.h"
#include "memmap.h"
#include "fileman.h"
#include "scratch.h"
#include "filetypes.h"

#if defined(MODULE_HOST_LINKED) || defined(FOP_RMDIR_ONLY)
#define FOP_RMDIR                 /* the delete is in this build */
#endif
#ifdef FOP_RMDIR_ONLY
/* rmdir.bin: the delete is the only op, so every other path is dead code the compiler drops
   (with the functions only they call) */
#define FOP_IS_RMDIR(m) 1
#pragma GCC diagnostic ignored "-Wunused-function"
#else
#define FOP_FILEOPS               /* the rest is in this build */
#define FOP_IS_RMDIR(m) ((m)->h.op == FM_OP_RMDIR || (m)->h.op == FM_OP_RMSEL)
#endif

#define FOP_PATH        256       /* every path buffer: 255 characters and the NUL */
#define FOP_MAXPATH     255
#define FOP_NBUF        5         /* P, Q, L (the LFN buffer), A, B */
#define FOP_BUF_MIN     512
#define FOP_BUF_MAX     4096
#ifdef FOP_SPARE_MIN   /* the build's module-block check (src/Makefile modcheck) */
_Static_assert(FOP_NBUF * FOP_PATH + FOP_BUF_MIN == FOP_SPARE_MIN, "FOP_SPARE_MIN in src/Makefile");
#endif
#define FOP_RD          512       /* a sector: the copy buffer is a whole number of them */
#define FOP_SLICE       1         /* ticks (10 ms) of work per idle() */
/* SRAM_FM_WORK_ADDR..END (128 KB) is ours, split by what runs at the same time:
     PASTE  +0x00000..+0x0FFFF  the folder-walk stack (one fop_frame_t per level; a path of
                                255 characters has at most 128 levels)
            +0x10000..+0x1FFFF  the copy staging: a file goes source -> PSRAM -> destination
                                in runs of up to 64 KB (see xfer_piece)
     VIEW   +0x00000..+0x1FFFF  the window start table (a viewer never pastes) */
#define FOP_STACK       SRAM_FM_WORK_ADDR
#define FOP_STAGE       (SRAM_FM_WORK_ADDR + 0x10000UL)
#define FOP_STAGE_SIZE  (0x10000UL)
#define FOP_WOFF        SRAM_FM_WORK_ADDR
#define FOP_WOFF_MAX    ((SRAM_FM_WORK_END - SRAM_FM_WORK_ADDR) / 4)
#define FOP_KEEP_MAX    9999      /* "name (N)": the highest N tried */

/* engine phases (internal; the menu sees fm_stat_t.state) */
enum { PH_NONE, PH_FREE, PH_REC, PH_WALK, PH_COPY, PH_ASK, PH_END };

/* the FM_STAT header: fm_stat_t up to cur_name, kept here and republished on change */
typedef struct __attribute__((packed)) {
  uint8_t  magic[2];
  uint8_t  version;
  uint8_t  state;
  uint8_t  op;
  uint8_t  err;
  uint8_t  ask;
  uint8_t  flags;
  uint32_t bytes_done;
  uint32_t bytes_total;
  uint16_t items_done;
  uint16_t items_total;
  uint16_t n_done;
  uint16_t n_skipped;
  uint16_t n_failed;
  uint8_t  progress;
  uint8_t  view_pct;
  uint16_t view_lines;
  uint16_t view_win;
  uint8_t  view_last;
  uint8_t  rsvd0;
  uint16_t rm_files;
  uint16_t rm_dirs;
  uint8_t  rsvd[2];
} fop_hdr_t;

_Static_assert(sizeof(fop_hdr_t) == __builtin_offsetof(fm_stat_t, cur_name), "header = fm_stat_t up to cur_name");
_Static_assert(__builtin_offsetof(fop_hdr_t, items_done) == __builtin_offsetof(fm_stat_t, items_done), "items_done");
_Static_assert(__builtin_offsetof(fop_hdr_t, progress) == __builtin_offsetof(fm_stat_t, progress), "progress");
_Static_assert(__builtin_offsetof(fop_hdr_t, view_last) == __builtin_offsetof(fm_stat_t, view_last), "view_last");
_Static_assert(__builtin_offsetof(fop_hdr_t, rm_files) == __builtin_offsetof(fm_stat_t, rm_files), "rm_files");
_Static_assert(__builtin_offsetof(fop_hdr_t, rm_dirs) == __builtin_offsetof(fm_stat_t, rm_dirs), "rm_dirs");

/* one level of a folder walk; the levels above the current one wait in PSRAM */
typedef struct {
  DIR      dir;
  uint16_t plen, qlen;            /* the folder's own path lengths in P and Q */
  uint8_t  flags;
} fop_frame_t;
#define FRM_MERGECUT    0x01      /* a cut merged into an existing folder: remove the source if it ended empty */
#define FRM_RMDIR       0x02      /* delete: remove the folder when its walk is done */

typedef struct {
  const module_api_t *api;
  fop_hdr_t h, ph;                /* live header, and the one last written */
  uint8_t  ph_ok;
  uint8_t  phase;
  uint8_t  mode;                  /* FM_CLIP_COPY / FM_CLIP_CUT */
  uint8_t  scan;                  /* the COPY's counting pass */
  uint8_t  same;                  /* pasting into the folder the clipboard came from */
  uint8_t  top;                   /* the item at hand is a clipboard entry, not a walk child */
  uint8_t  isdir;                 /* ...and is a folder */
  uint8_t  copying;               /* api->file and fo are open on P and Q */
  uint8_t  draining;              /* the staging is being written out (else filled) */
  uint8_t  src_eof;               /* the source is read to its end */
  uint8_t  cancel;
  uint8_t  answer, answer_all;    /* FM_CTL_ANSWER, waiting for idle() */
  uint8_t  all[3];                /* the answer given "for all" per FM_ASK_* kind, 0 = none */
  uint8_t  lists;                 /* a list file was rewritten: refresh at the end */
  uint8_t  fstage;                /* COPY, a game's own files: 0 none, 1 its .cov next, 2 /sd2snes */
  uint8_t  froots;                /* ...and the /sd2snes ones are wanted (it gets a new stem) */
  uint8_t  froot, fdir, fmade;    /* the root at hand, d2 open on it, its bucket created */
  uint8_t  fsub;                  /* the open copy is one of them (A -> B), not P -> Q */
  uint16_t fsa, fblen;            /* the name's offset in A, the destination stem's end in B */
  uint16_t slen, dlen;            /* the clipboard folder in P, the destination in Q */
  uint16_t depth;                 /* open walk levels; 0 = at the clipboard entries */
  uint16_t rec_i, rec_n;
  uint32_t rec_off;
  uint32_t need;                  /* COPY: clusters it will take */
  uint32_t clus;                  /* bytes per cluster */
  uint32_t nfree;                 /* free clusters */
  uint32_t v_size, v_known;       /* VIEW: the file size, window starts known */
#ifdef FOP_RMDIR
  uint32_t rm_rest;               /* RMDIR: bytes under a KB not yet in bytes_total (KB) */
#endif
  char    *p, *q, *l, *a, *b;     /* path buffers in the spare RAM */
  uint8_t *buf;                   /* the copy / read buffer, after them */
  uint32_t bufsz;
  uint32_t st_fill, st_drain;     /* bytes in the staging, bytes of them written out */
  fop_frame_t cur;                /* the walk level being read */
  DIR      d2;                    /* bucket and sibling scans */
  FILINFO  fi;
  FIL      fo;                    /* the destination file, the viewer's file, the list temp */
} fopmod_t;

_Static_assert(sizeof(fopmod_t) <= SCRATCH_FRAME_BYTES, "fileops state must fit the FRAME");
_Static_assert(FOP_STACK + 129 * sizeof(fop_frame_t) <= FOP_STAGE, "the walk stack runs into the copy staging");
_Static_assert(FOP_STAGE + FOP_STAGE_SIZE <= SRAM_FM_WORK_END, "the copy staging must stay in SRAM_FM_WORK");
_Static_assert(FOP_BUF_MAX <= FOP_STAGE_SIZE && FOP_BUF_MAX <= 0xffff, "a piece is one sram_read / sram_write");

int  fopmod_start(void *st, const module_api_t *api, const char *path);
void fopmod_ctl(void *st, uint32_t param);
void fopmod_idle(void *st);
void fopmod_stop(void *st);

/* --- small helpers (no library) ------------------------------------------------------- */

static uint32_t slen(const char *s) {
  uint32_t n = 0;
  while(s[n]) n++;
  return n;
}

static uint32_t scpy(char *d, const char *s) {
  uint32_t n = 0;
  while((d[n] = s[n])) n++;
  return n;
}

static void bzero8(void *p, uint32_t n) {
  uint8_t *b = p;
  while(n--) *b++ = 0;
}

/* FAT names compare without case: ASCII and the Latin-1 letters of CP1252 */
static uint8_t lc(uint8_t c) {
  if((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) c += 32;
  return c;
}

static int eqin(const char *a, const char *b, uint32_t n) {
  while(n--) {
    if(lc(*a) != lc(*b)) return 0;
    if(!*a) return 1;
    a++; b++;
  }
  return 1;
}

static int eqi(const char *a, const char *b) {
  return eqin(a, b, 0xffffffffUL);
}

static int eqx(const char *a, const char *b) {
  while(*a && *a == *b) { a++; b++; }
  return *a == *b;
}

/* path == dir, or path is somewhere below dir (dir "" is the root: everything) */
static int under(const char *path, const char *dir) {
  uint32_t n = slen(dir);
  return eqin(path, dir, n) && (path[n] == 0 || path[n] == '/');
}

/* offset of the last '/' (the leaf follows it); paths here always start with one */
static uint32_t lastsl(const char *s) {
  uint32_t i = 0, r = 0;
  for(; s[i]; i++) if(s[i] == '/') r = i;
  return r;
}

static const char *leaf(const char *s) {
  return s + lastsl(s) + 1;
}

/* length of a leaf's stem: up to its last '.', unless that is its first character (the
   rule path_asset uses) */
static uint32_t stemlen(const char *lf) {
  uint32_t i, n = slen(lf), d = n;
  for(i = 1; i < n; i++) if(lf[i] == '.') d = i;
  return d;
}

/* drop trailing '/' (the browser's folder names carry one); the root becomes "" */
static uint32_t pnorm(char *s) {
  uint32_t n = slen(s);
  while(n && s[n - 1] == '/') s[--n] = 0;
  return n;
}

/* buf holds a path of len characters: append "/" + lf.  -1 when it would pass 255. */
static int pjoin(char *buf, uint32_t len, const char *lf) {
  uint32_t n = slen(lf);
  if(len + 1 + n > FOP_MAXPATH) return -1;
  buf[len] = '/';
  scpy(buf + len + 1, lf);
  return (int)(len + 1 + n);
}

static uint32_t utoa10(char *d, uint32_t v) {
  char t[10];
  uint32_t n = 0, i = 0;
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while(v);
  while(n) d[i++] = t[--n];
  d[i] = 0;
  return i;
}

static int isdig(uint8_t c) {
  return c >= '0' && c <= '9';
}

static int isalnum8(uint8_t c) {
  c = lc(c);
  return isdig(c) || (c >= 'a' && c <= 'z');
}

/* the game files whose /sd2snes files follow them (path_asset's world) */
static int is_game(const char *lf) {
  static const char exts[] = "sfc\0smc\0swc\0fig\0bs\0st\0sfrom\0gb\0gbc\0sgb\0nes\0sms\0a26\0";
  uint32_t n = slen(lf), d = stemlen(lf);
  const char *e;
  if(d >= n) return 0;
  for(e = exts; *e; e += slen(e) + 1)
    if(eqi(lf + d + 1, e)) return 1;
  return 0;
}

/* CP1252 0x80-0x9F to Unicode (0 = undefined) */
static const uint16_t cp1252_hi[32] = {
  0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
  0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
  0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
  0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
};

static uint32_t cp1252(uint8_t b) {
  return (b >= 0x80 && b < 0xa0) ? cp1252_hi[b - 0x80] : b;
}

/* one code point to menu font bytes; never 0 / 1 (they end a menu string) */
static int font(fopmod_t *m, uint32_t cp, uint8_t *out) {
  int n, i;
  if(cp < 0x80) {
    out[0] = (cp < 0x20 || cp == 0x7f) ? '?' : (uint8_t)cp;
    return 1;
  }
  if(cp < 0xa0) { out[0] = '?'; return 1; }     /* C1 controls, undefined CP1252 */
  n = m->api->font_cp(cp, out);
  if(n < 1 || n > 3) { out[0] = '?'; return 1; }
  for(i = 0; i < n; i++) if(out[i] < 2) out[i] = '?';
  return n;
}

/* --- FM_STAT ---------------------------------------------------------------------------- */

static void progress(fopmod_t *m) {
  uint32_t num, den;
#ifdef FOP_RMDIR
  if(m->h.bytes_total && !FOP_IS_RMDIR(m)) { num = m->h.bytes_done; den = m->h.bytes_total; }
#else
  if(m->h.bytes_total) { num = m->h.bytes_done; den = m->h.bytes_total; }
#endif
  else { num = m->h.items_done; den = m->h.items_total; }
  if(!den) return;
  if(num > den) num = den;
  while(den > 0x00ffffffUL) { den >>= 8; num >>= 8; }   /* num * 255 must not wrap */
  if(!den) return;
  m->h.progress = (uint8_t)((num * 255) / den);
}

/* every write is SPI traffic: only when the header changed */
static void publish(fopmod_t *m) {
  const uint8_t *a = (const uint8_t *)&m->h;
  uint8_t *b = (uint8_t *)&m->ph;
  uint32_t i;
  if(m->ph_ok) {
    for(i = 0; i < sizeof(fop_hdr_t) && a[i] == b[i]; i++);
    if(i == sizeof(fop_hdr_t)) return;
  }
  for(i = 0; i < sizeof(fop_hdr_t); i++) b[i] = a[i];
  m->ph_ok = 1;
  m->api->sram_write((void *)&m->h, SRAM_FM_STAT_ADDR, sizeof(fop_hdr_t));
}

/* cur_name: a FAT leaf (CP1252) in font bytes */
static void set_name(fopmod_t *m, const char *lf) {
  uint8_t nm[FM_NAME_LEN + 3], fb[3];
  uint32_t n = 0;
  int k, i;
  while(*lf && n < FM_NAME_LEN - 1) {
    k = font(m, cp1252((uint8_t)*lf++), fb);
    if(n + k > FM_NAME_LEN - 1) break;
    for(i = 0; i < k; i++) nm[n++] = fb[i];
  }
  nm[n] = 0;
  m->api->sram_write(nm, SRAM_FM_STAT_ADDR + __builtin_offsetof(fm_stat_t, cur_name), (uint16_t)(n + 1));
}

static void fail_op(fopmod_t *m, uint8_t err) {
  m->h.state = FM_ST_ERROR;
  m->h.err = err;
  m->phase = PH_END;
}

static uint8_t fr_err(FRESULT r) {
  switch(r) {
    case FR_NO_FILE:
    case FR_NO_PATH:      return FM_ERR_NOTFOUND;
    case FR_EXIST:        return FM_ERR_EXISTS;
    case FR_INVALID_NAME: return FM_ERR_BADNAME;
    default:              return FM_ERR_IO;
  }
}

/* --- files -------------------------------------------------------------------------------- */

/* f_stat without touching the walk's FILINFO; *isdir when wanted */
static FRESULT fstat(fopmod_t *m, const char *path, uint8_t *isdir) {
  FILINFO f;
  FRESULT r;
  f.lfname = 0;
  f.lfsize = 0;
  r = m->api->f_stat(path, &f);
  if(isdir) *isdir = (r == FR_OK) && (f.fattrib & AM_DIR);
  return r;
}

static FRESULT opendir_at(fopmod_t *m, DIR *d, const char *path) {
  return m->api->f_opendir(d, path[0] ? path : "/");
}

/* One piece of a file copy from api->file to fo, through the PSRAM staging.
 * WHY THE STAGING: the card driver keeps ONE multi-block transfer open (a read stream or a
 * write stream) and continues it only while the next sector follows the last one; every
 * switch between reading and writing stops the stream and starts the other (and a stopped
 * write waits for the card).  Copying a buffer at a time alternated every couple of sectors,
 * and a real Mk.II copied at ~55 KB/s.  So the file goes in runs: FILL reads up to 64 KB into
 * the staging, DRAIN writes it all out, then the next run.  Each phase is one stream; only the
 * FAT lookups of a new cluster step outside it.
 * A piece is one f_read or one f_write of up to bufsz (whole sectors go straight between the
 * card and buf).  Measured on a Mk.III, per 512-byte sector: card -> RAM 0.28 ms, RAM -> PSRAM
 * 0.48, PSRAM -> RAM 0.78, RAM -> card 0.76, so ~220 KB/s.  NOT the FPGA's SD DMA into PSRAM
 * (load_stream's path), tried and measured: it takes PSRAM away from the menu, which runs from
 * PSRAM and draws this copy's progress, and the SNES hung for the whole copy (it is only used
 * with the SNES parked in WRAM: a game load, the tour's clip).
 * Bytes count as done when WRITTEN.
 * Returns 0 = more to do, 1 = the source is copied (both files still open), or -FM_ERR_IO /
 * -FM_ERR_NOSPACE (FatFs: a short write is a full card). */
static int xfer_piece(fopmod_t *m) {
  const module_api_t *api = m->api;
  uint32_t n;
  UINT br = 0, bw = 0;
  if(!m->draining) {
    if(api->f_read(api->file, m->buf, m->bufsz, &br) != FR_OK) return -FM_ERR_IO;
    if(br) api->sram_write(m->buf, FOP_STAGE + m->st_fill, (uint16_t)br);
    m->st_fill += br;
    if(br < m->bufsz) m->src_eof = 1;
    if(m->src_eof || m->st_fill + m->bufsz > FOP_STAGE_SIZE) {
      m->draining = 1;
      m->st_drain = 0;
    }
    return 0;
  }
  n = m->st_fill - m->st_drain;
  if(n > m->bufsz) n = m->bufsz;
  if(n) {
    api->sram_read(m->buf, FOP_STAGE + m->st_drain, (uint16_t)n);
    FRESULT r = api->f_write(&m->fo, m->buf, n, &bw);
    if(r != FR_OK) return -FM_ERR_IO;
    if(bw != n) return -FM_ERR_NOSPACE;
    m->st_drain += n;
    m->h.bytes_done += n;
  }
  if(m->st_drain < m->st_fill) return 0;
  m->draining = 0;
  m->st_fill = 0;
  return m->src_eof;
}

static void xfer_init(fopmod_t *m) {
  m->draining = 0;
  m->src_eof = 0;
  m->st_fill = 0;
  m->st_drain = 0;
}

/* --- the clipboard ---------------------------------------------------------------------- */

/* the record at *off: its type, the leaf (trailing '/' dropped) in out; out = "" when the
   name does not fit a path */
static uint8_t rec_read(fopmod_t *m, uint32_t *off, char *out) {
  const module_api_t *api = m->api;
  uint8_t type = api->sram_readbyte(*off);
  uint32_t n;
  api->sram_read(out, *off + 1, FOP_PATH);
  for(n = 0; n < FOP_PATH && out[n]; n++);
  if(n == FOP_PATH) {                             /* longer than any FAT name: skip it */
    while(n < FOP_PATH + 4 && api->sram_readbyte(*off + 1 + n)) n++;
    out[0] = 0;
  }
  *off += n + 2;
  if(out[0]) pnorm(out);
  return type;
}

/* is lf one of the clipboard's own entries (it will be handled as such)? Uses L. */
static int clip_has(fopmod_t *m, const char *lf) {
  uint32_t off = SRAM_FM_CLIP_ADDR + FM_CLIP_HDR;
  uint16_t i;
  if(m->h.op != FM_OP_PASTE) return 0;
  for(i = 0; i < m->rec_n; i++) {
    rec_read(m, &off, m->l);
    if(eqi(m->l, lf)) return 1;
  }
  return 0;
}

/* --- Recents / Favorites ---------------------------------------------------------------- */

/* P moved to Q (a file, or a folder and everything below it).  An entry is "<rom>" or
   "<rom>\t<patch leaf>", the patch living next to the rom (cfg.c).  Writes the entry from A,
   mapped, into B; 1 = it changed. */
static int entry_map(fopmod_t *m, int isdir) {
  char *a = m->a, *b = m->b, *p = m->p, *q = m->q;
  uint32_t tab, n, plen = slen(p), qlen = slen(q), bl;
  const char *patch = 0;
  int ch;
  for(tab = 0; a[tab] && a[tab] != '\t'; tab++);
  if(a[tab]) { a[tab] = 0; patch = a + tab + 1; }
  if(eqi(a, p) || (isdir && under(a, p))) {
    n = slen(a);
    if(qlen + (n - plen) > FOP_MAXPATH) bl = scpy(b, a);
    else { scpy(b, q); bl = qlen + scpy(b + qlen, a + plen); }
  } else {
    bl = scpy(b, a);
  }
  ch = !eqx(a, b);
  if(patch) {
    const char *np = patch;
    uint32_t ad = lastsl(a), pd = lastsl(p), bd = lastsl(b), qd = lastsl(q);
    /* the patch file itself moved: follow it while it stays next to the rom */
    if(!isdir && ad == pd && eqin(a, p, ad) && eqi(patch, p + pd + 1)
       && bd == qd && eqin(b, q, bd)) np = q + qd + 1;
    if(!eqx(np, patch)) ch = 1;
    if(bl + 1 + slen(np) <= FOP_MAXPATH) {
      b[bl++] = '\t';
      scpy(b + bl, np);
    }
    a[tab] = '\t';
  }
  return ch;
}

typedef struct {
  uint32_t n, i;
  int eof;
} fop_rd_t;

/* next entry of api->file into A (entries end in NUL; CR / LF taken as ends too) */
static int entry_next(fopmod_t *m, fop_rd_t *r) {
  uint32_t n = 0;
  UINT br;
  for(;;) {
    if(r->i == r->n) {
      if(r->eof || m->api->f_read(m->api->file, m->buf, m->bufsz, &br) != FR_OK || !br) {
        r->eof = 1;
        m->a[n] = 0;
        return n != 0;
      }
      r->n = br;
      r->i = 0;
    }
    uint8_t c = m->buf[r->i++];
    if(!c || c == '\n' || c == '\r') {
      if(n) { m->a[n] = 0; return 1; }
      continue;
    }
    if(n < FOP_MAXPATH - 1) m->a[n++] = (char)c;
  }
}

/* one list: read-only when !write (1 = an entry would change); else rewrite it through
   "<list>.tmp" (one entry at a time, never the whole list in RAM) and swap it in */
static int list_pass(fopmod_t *m, const char *list, int isdir, int write) {
  const module_api_t *api = m->api;
  char tmp[40];
  fop_rd_t rd = { 0, 0, 0 };
  int hit = 0, ok = 1;
  UINT bw;
  if(write) {
    scpy(tmp + scpy(tmp, list), ".tmp");
    if(api->f_open(&m->fo, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return 0;
  }
  if(api->f_open(api->file, list, FA_READ) != FR_OK) {
    if(write) { api->f_close(&m->fo); api->f_unlink(tmp); }
    return 0;
  }
  while(entry_next(m, &rd)) {
    if(m->a[0] != '/') continue;                  /* garbage: dropped, as cfg.c does */
    if(entry_map(m, isdir)) hit = 1;
    if(!write) { if(hit) break; continue; }
    uint32_t n = slen(m->b) + 1;
    if(api->f_write(&m->fo, m->b, n, &bw) != FR_OK || bw != n) { ok = 0; break; }
  }
  api->f_close(api->file);
  if(!write) return hit;
  if(api->f_close(&m->fo) != FR_OK || !ok) {
    api->f_unlink(tmp);
    return 0;
  }
  api->f_unlink(list);
  return api->f_rename(tmp, list) == FR_OK;
}

/* after P moved to Q */
static void lists_map(fopmod_t *m, int isdir) {
  static const char lists[] = "/sd2snes/lastgame.cfg\0/sd2snes/favorites.cfg\0";
  const char *l;
  for(l = lists; *l; l += slen(l) + 1) {
    if(list_pass(m, l, isdir, 0) && list_pass(m, l, isdir, 1)) {
      m->lists = 1;
      m->h.flags |= FM_FL_LISTS;
    }
  }
}

/* --- a game's files ----------------------------------------------------------------------- */

/* what follows <stem> in a /sd2snes file of that game: ".srm" ".slot" ".yml" ... (a dot and
   1..5 letters / digits), ".NN.srm" / ".NN.man", or "NN.state" */
static int sidecar_tail(const char *r) {
  uint32_t n = 0;
  if(r[0] == '.') {
    if(isdig(r[1]) && isdig(r[2]) && r[3] == '.' && (eqi(r + 4, "srm") || eqi(r + 4, "man")))
      return 1;
    while(isalnum8(r[1 + n])) n++;
    return n >= 1 && n <= 5 && !r[1 + n];
  }
  return isdig(r[0]) && isdig(r[1]) && eqi(r + 2, ".state");
}

/* A and B hold one sibling's old and new path: move it */
static void sibling_one(fopmod_t *m) {
  if(eqi(m->a, m->b) || fstat(m, m->a, 0) != FR_OK || clip_has(m, leaf(m->a))) return;
  if(fstat(m, m->b, 0) == FR_OK) {
    m->h.flags |= FM_FL_ASSET_SKIP;
    return;
  }
  if(m->api->f_rename(m->a, m->b) == FR_OK)
    m->h.flags |= FM_FL_ROM_ASSETS;
}

/* CUT: game P's MSU-1 pack next to it -- <stem>.msu and its <stem>-<n>.pcm tracks -- goes to
   Q's folder under Q's stem.  The tracks are looked for only when a .msu is there: without
   one they are not an MSU-1 pack, and scanning a big folder for every game moved would be
   slow.  The .cov goes with the rest of the game's files (follow_find). */
static void siblings(fopmod_t *m) {
  const module_api_t *api = m->api;
  const char *lp = leaf(m->p), *lq = leaf(m->q), *name;
  uint32_t ps = stemlen(lp), qs = stemlen(lq), pd = lp - m->p, qd = lq - m->q, al, bl, i, nl;
  if(pd + ps + 8 > FOP_MAXPATH || qd + qs + 8 > FOP_MAXPATH) return;
  for(i = 0; i < pd + ps; i++) m->a[i] = m->p[i];
  for(i = 0; i < qd + qs; i++) m->b[i] = m->q[i];
  al = pd + ps;
  bl = qd + qs;
  {
    scpy(m->a + al, ".msu");
    if(fstat(m, m->a, 0) == FR_OK) {
      m->a[pd - 1] = 0;
      if(opendir_at(m, &m->d2, m->a) == FR_OK) {
        m->a[pd - 1] = '/';
        for(;;) {
          m->fi.lfname = m->l;
          m->fi.lfsize = FOP_PATH;
          if(api->f_readdir(&m->d2, &m->fi) != FR_OK || !m->fi.fname[0]) break;
          name = m->l[0] ? m->l : m->fi.fname;
          if(!eqin(name, lp, ps) || name[ps] != '-' || !isdig(name[ps + 1])) continue;
          for(i = ps + 1; isdig(name[i]); i++);
          if(!eqi(name + i, ".pcm")) continue;
          nl = slen(name + ps);
          if(al + nl > FOP_MAXPATH || bl + nl > FOP_MAXPATH) continue;
          scpy(m->a + al, name + ps);
          scpy(m->b + bl, name + ps);
          sibling_one(m);
        }
        api->f_closedir(&m->d2);
      }
      m->a[pd - 1] = '/';
    }
    scpy(m->a + al, ".msu");
    scpy(m->b + bl, ".msu");
    sibling_one(m);
  }
}

/* --- a game's own files ------------------------------------------------------------------
 * Its <stem>.cov next to it (a clipboard entry only) and, when it gets a new stem, its files
 * under /sd2snes: a CUT (and a rename) moves those of all five roots, at once (renames); a
 * COPY copies those of info and cheats.  The scan counts them into bytes_total and the
 * clusters the paste needs; the paste copies each one through PH_COPY like any other file,
 * its name in the window.  A game's clip, soundtrack and guides are megabytes (11 MB for a
 * real 2 MB game), and they used to be copied in one go after the ROM: outside the bar, the
 * menu loop stopped (no clock, no B) for most of a minute. */

static void item_ok(fopmod_t *m);

static void follow_begin(fopmod_t *m) {
  m->fstage = m->top ? 1 : 2;
  m->froot = 0;
  m->fdir = 0;
  m->froots = m->scan ? (m->top && m->same) : !eqi(leaf(m->p), leaf(m->q));
}

/* the copy may go ahead: B is free (else noted and passed over).  The scan takes them all. */
static int follow_free(fopmod_t *m) {
  if(m->scan) return 1;
  if(fstat(m, m->b, 0) == FR_OK) {
    m->h.flags |= FM_FL_ASSET_SKIP;
    return 0;
  }
  return 1;
}

/* the next one: A = it, B = where it goes, fi.fsize its size.  0 = none left. */
static __attribute__((noinline)) int follow_find(fopmod_t *m) {
  static const char roots[] = "info\0cheats\0saves\0states\0patches\0";
  const module_api_t *api = m->api;
  const char *lp = leaf(m->p), *name, *r;
  char root[24];
  uint32_t sl = stemlen(lp), nl, i, k;
  int sa, sb;
  if(m->fstage == 1) {
    const char *lq = leaf(m->q);
    uint32_t pd = lp - m->p, qd = lq - m->q, qs = stemlen(lq);
    m->fstage = 2;
    if(pd + sl + 5 <= FOP_MAXPATH && qd + qs + 5 <= FOP_MAXPATH) {
      for(i = 0; i < pd + sl; i++) m->a[i] = m->p[i];
      for(i = 0; i < qd + qs; i++) m->b[i] = m->q[i];
      scpy(m->a + pd + sl, ".cov");
      scpy(m->b + qd + qs, ".cov");
      m->fi.lfname = 0;
      m->fi.lfsize = 0;
      if((m->scan || !eqi(m->a, m->b)) && api->f_stat(m->a, &m->fi) == FR_OK
         && !(m->fi.fattrib & AM_DIR) && follow_free(m)) return 1;
    }
  }
  if(m->fstage != 2 || !m->froots) return 0;
  for(;;) {
    if(!m->fdir) {
      if(m->froot >= (m->mode == FM_CLIP_COPY ? 2 : 5)) return 0;
      for(r = roots, i = 0; i < m->froot; i++) r += slen(r) + 1;
      m->froot++;
      k = scpy(root, "/sd2snes/");
      k += scpy(root + k, r);
      root[k++] = '/';
      root[k] = 0;
      sa = api->path_asset(m->a, FOP_PATH, root, m->p, "");
      sb = api->path_asset(m->b, FOP_PATH, root, m->q, "");
      if(sa < 1 || sb < 1 || (!m->scan && eqi(m->a, m->b))) continue;
      m->fsa = (uint16_t)sa;
      m->fblen = (uint16_t)slen(m->b);
      m->a[sa - 1] = 0;
      k = api->f_opendir(&m->d2, m->a) == FR_OK;
      m->a[sa - 1] = '/';
      if(!k) continue;
      m->fdir = 1;
      m->fmade = 0;
    }
    m->b[m->fblen] = 0;
    m->fi.lfname = m->l;
    m->fi.lfsize = FOP_PATH;
    if(api->f_readdir(&m->d2, &m->fi) != FR_OK || !m->fi.fname[0]) {
      api->f_closedir(&m->d2);
      m->fdir = 0;
      continue;
    }
    if(m->fi.fattrib & AM_DIR) continue;
    name = m->l[0] ? m->l : m->fi.fname;
    if(!eqin(name, lp, sl) || !sidecar_tail(name + sl)) continue;
    nl = slen(name);
    if(m->fsa + nl > FOP_MAXPATH || m->fblen + nl - sl > FOP_MAXPATH) continue;
    scpy(m->a + m->fsa, name);
    scpy(m->b + m->fblen, name + sl);
    if(follow_free(m)) return 1;
  }
}

/* B's bucket, once per root: 0 = it cannot be made (the root is given up) */
static int follow_mkdir(fopmod_t *m) {
  if(m->fdir && !m->fmade) {
    if(m->api->path_asset_mkdir(m->b) != FR_OK) {
      m->api->f_closedir(&m->d2);
      m->fdir = 0;
      return 0;
    }
    m->fmade = 1;
  }
  return 1;
}

/* CUT / rename: a file P just became Q -- its game files, if it is a game */
static void game_follow(fopmod_t *m) {
  if(!is_game(leaf(m->p))) return;
  if(m->top) siblings(m);
  follow_begin(m);
  while(follow_find(m))
    if(follow_mkdir(m) && m->api->f_rename(m->a, m->b) == FR_OK) m->h.flags |= FM_FL_ROM_ASSETS;
  m->fstage = 0;
}

/* COPY: start copying the next one, or the item is done */
static void follow_next(fopmod_t *m) {
  const module_api_t *api = m->api;
  while(follow_find(m)) {
    if(!follow_mkdir(m)) continue;
    if(api->f_open(api->file, m->a, FA_READ) != FR_OK) continue;
    if(api->f_open(&m->fo, m->b, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
      api->f_close(api->file);
      continue;
    }
    set_name(m, leaf(m->a));
    xfer_init(m);
    m->fsub = 1;
    m->copying = 1;
    m->phase = PH_COPY;
    return;
  }
  m->fstage = 0;
  item_ok(m);
}

/* --- the paste: items --------------------------------------------------------------------- */

static void item_next(fopmod_t *m) {
  if(m->depth) {
    m->p[m->cur.plen] = 0;
    m->q[m->cur.qlen] = 0;
    m->phase = PH_WALK;
  } else {
    m->phase = PH_REC;
  }
}

static void item_ok(fopmod_t *m) {
  m->h.items_done++;
  m->h.n_done++;
  item_next(m);
}

static void item_skip(fopmod_t *m) {
  m->h.items_done++;
  m->h.n_skipped++;
  item_next(m);
}

static void item_fail(fopmod_t *m, uint8_t err) {
  m->h.items_done++;
  m->h.n_failed++;
  m->h.err = err;
  item_next(m);
}

/* Q's leaf becomes the first free "stem (N).ext" ("name (N)" for a folder).  0 = none fits. */
static int keep_both(fopmod_t *m) {
  char *b = m->b, *q = m->q;
  uint32_t qd = lastsl(q) + 1, n = scpy(b, q + qd), st = m->isdir ? n : stemlen(b), k, num;
  for(num = 2; num <= FOP_KEEP_MAX; num++) {
    char nb[8];
    uint32_t nl = utoa10(nb, num);
    if(qd + n + nl + 3 > FOP_MAXPATH) return 0;
    for(k = 0; k < st; k++) q[qd + k] = b[k];
    k = qd + st;
    q[k++] = ' ';
    q[k++] = '(';
    k += scpy(q + k, nb);
    q[k++] = ')';
    scpy(q + k, b + st);
    if(fstat(m, q, 0) == FR_NO_FILE) return 1;
  }
  return 0;
}

/* enter folder P (Q its destination) */
static int walk_push(fopmod_t *m, uint8_t flags) {
  const module_api_t *api = m->api;
  uint32_t at = FOP_STACK + (uint32_t)m->depth * sizeof(fop_frame_t);
  if(at + 2 * sizeof(fop_frame_t) > FOP_STAGE) return 0;
  if(m->depth) api->sram_write(&m->cur, at - sizeof(fop_frame_t), sizeof(fop_frame_t));
  if(opendir_at(m, &m->cur.dir, m->p) != FR_OK) {
    if(m->depth) api->sram_read(&m->cur, at - sizeof(fop_frame_t), sizeof(fop_frame_t));
    return 0;
  }
  m->cur.plen = (uint16_t)slen(m->p);
  m->cur.qlen = (uint16_t)slen(m->q);
  m->cur.flags = flags;
  m->depth++;
  m->phase = PH_WALK;
  return 1;
}

/* the folder at hand is finished */
#ifdef FOP_RMDIR
static void rm_res(fopmod_t *m, FRESULT r, int isdir);

static void walk_pop(fopmod_t *m) {
  const module_api_t *api = m->api;
  uint8_t fl = m->cur.flags;
  FRESULT r = FR_OK;
  m->p[m->cur.plen] = 0;
  m->q[m->cur.qlen] = 0;
  api->f_closedir(&m->cur.dir);
  if(fl & (FRM_MERGECUT | FRM_RMDIR)) r = api->f_unlink(m->p);   /* refused while anything is left */
  m->depth--;
  if(m->depth)
    api->sram_read(&m->cur, FOP_STACK + (uint32_t)(m->depth - 1) * sizeof(fop_frame_t), sizeof(fop_frame_t));
  if(fl & FRM_RMDIR) rm_res(m, r, 1);
  else item_next(m);
}
#else
static void walk_pop(fopmod_t *m) {
  const module_api_t *api = m->api;
  m->p[m->cur.plen] = 0;
  m->q[m->cur.qlen] = 0;
  api->f_closedir(&m->cur.dir);
  if(m->cur.flags & FRM_MERGECUT) api->f_unlink(m->p);   /* refused while anything is left */
  m->depth--;
  if(m->depth)
    api->sram_read(&m->cur, FOP_STACK + (uint32_t)(m->depth - 1) * sizeof(fop_frame_t), sizeof(fop_frame_t));
  item_next(m);
}
#endif

static void copy_abort(fopmod_t *m) {
  m->api->f_close(&m->fo);
  m->api->f_close(m->api->file);
  m->api->f_unlink(m->fsub ? m->b : m->q);
  m->copying = 0;
}

/* P goes to Q, which is free (replace = 0) or answered "replace" (1) */
static void item_go(fopmod_t *m, int replace) {
  const module_api_t *api = m->api;
  FRESULT r;
  if(m->mode == FM_CLIP_COPY) {
    if(m->isdir) {
      if(!replace && (r = api->f_mkdir(m->q)) != FR_OK) { item_fail(m, fr_err(r) == FM_ERR_NOTFOUND ? FM_ERR_IO : fr_err(r)); return; }
      m->h.items_done++;
      m->h.n_done++;
      if(!walk_push(m, 0)) { m->h.n_done--; m->h.n_failed++; m->h.err = FM_ERR_IO; item_next(m); }
      return;
    }
    if((r = api->f_open(api->file, m->p, FA_READ)) != FR_OK) { item_fail(m, fr_err(r)); return; }
    if((r = api->f_open(&m->fo, m->q, FA_CREATE_ALWAYS | FA_WRITE)) != FR_OK) {
      api->f_close(api->file);
      item_fail(m, FM_ERR_IO);
      return;
    }
    xfer_init(m);
    m->copying = 1;
    m->phase = PH_COPY;
    return;
  }
  /* CUT */
  if(m->isdir && replace) {
    m->h.items_done++;
    m->h.n_done++;
    if(!walk_push(m, FRM_MERGECUT)) { m->h.n_done--; m->h.n_failed++; m->h.err = FM_ERR_IO; item_next(m); }
    return;
  }
  if(replace && (r = api->f_unlink(m->q)) != FR_OK) { item_fail(m, fr_err(r)); return; }
  if((r = api->f_rename(m->p, m->q)) != FR_OK) { item_fail(m, fr_err(r)); return; }
  lists_map(m, m->isdir);
  if(!m->isdir) game_follow(m);
  item_ok(m);
}

/* a conflict answered (kind = FM_ASK_*) */
#ifdef FOP_RMDIR
static void rm_go(fopmod_t *m);
#endif

static void item_answer(fopmod_t *m, uint8_t kind, uint8_t ans) {
#ifdef FOP_RMDIR
  if(kind == FM_ASK_RMDIR) {
    if(ans == FM_ANS_REPLACE) rm_go(m);
    else fail_op(m, FM_ERR_CANCELLED);
    return;
  }
#endif
#ifdef FOP_FILEOPS
  if(ans == FM_ANS_KEEPBOTH) {
    if(keep_both(m)) item_go(m, 0);
    else item_fail(m, FM_ERR_PATH);
  } else if(ans == FM_ANS_REPLACE && kind != FM_ASK_MIXED) {
    item_go(m, 1);
  } else {
    item_skip(m);
  }
#endif
}

/* P (a folder when isdir) is to go to Q */
static void item_start(fopmod_t *m) {
  uint8_t qdir, kind, ans;
  FRESULT r;
  set_name(m, leaf(m->p));
  if(m->top && m->same) {
    if(m->mode == FM_CLIP_CUT) { item_ok(m); return; }   /* already where it is pasted */
    if(!keep_both(m)) { item_fail(m, FM_ERR_PATH); return; }
    item_go(m, 0);
    return;
  }
  r = fstat(m, m->q, &qdir);
  if(r == FR_NO_FILE) { item_go(m, 0); return; }
  if(r != FR_OK) { item_fail(m, fr_err(r) == FM_ERR_NOTFOUND ? FM_ERR_IO : fr_err(r)); return; }
  if(eqi(m->p, m->q)) { item_skip(m); return; }           /* never onto itself */
  kind = (qdir != m->isdir) ? FM_ASK_MIXED : (m->isdir ? FM_ASK_DIR : FM_ASK_FILE);
  ans = m->all[kind - 1];
  if(!ans) {
    m->h.state = FM_ST_ASK;
    m->h.ask = kind;
    m->phase = PH_ASK;
    return;
  }
  item_answer(m, kind, ans);
}

/* one piece of the file being copied (xfer_piece) */
static void copy_step(fopmod_t *m) {
  const module_api_t *api = m->api;
  int r = xfer_piece(m);
  if(!r) return;
  if(r == 1) {
    m->copying = 0;
    api->f_close(api->file);
    if(api->f_close(&m->fo) != FR_OK) {
      api->f_unlink(m->fsub ? m->b : m->q);
      r = -FM_ERR_IO;
    }
  } else {
    copy_abort(m);
  }
  if(m->fsub) {                       /* one of the game's own files: on to the next */
    m->fsub = 0;
    if(r == 1) m->h.flags |= FM_FL_ROM_ASSETS;
    follow_next(m);
    return;
  }
  if(r != 1) {
    item_fail(m, (uint8_t)-r);
    return;
  }
  if(is_game(leaf(m->p))) {
    follow_begin(m);
    follow_next(m);
    return;
  }
  item_ok(m);
}

/* COPY's counting pass: the bytes and clusters of one more file */
static void scan_bytes(fopmod_t *m, uint32_t size) {
  uint32_t t = m->h.bytes_total + size;
  m->h.bytes_total = t < size ? 0xffffffffUL : t;
  m->need += size / m->clus + ((size % m->clus) ? 1 : 0);
}

/* ...one more file or folder */
static void scan_count(fopmod_t *m, uint8_t isdir, uint32_t size) {
  m->h.items_total++;
#ifdef FOP_RMDIR
  if(FOP_IS_RMDIR(m)) {
    if(isdir) m->h.rm_dirs++;
    else m->h.rm_files++;
    m->rm_rest += size & 1023;      /* bytes_total counts KB here: no 4 GB ceiling */
    m->h.bytes_total += (size >> 10) + (m->rm_rest >> 10);
    m->rm_rest &= 1023;
    return;                         /* no clusters to find: m->clus is a paste's */
  }
#endif
  if(isdir) { m->need++; return; }
  scan_bytes(m, size);
}

/* the next clipboard entry */
#ifdef FOP_RMDIR
static void rm_end(fopmod_t *m);
static void rm_rec(fopmod_t *m);
#endif

static void rec_step(fopmod_t *m) {
  FRESULT r;
  uint8_t type;
#ifdef FOP_RMDIR
  if(FOP_IS_RMDIR(m)) {
    if(m->h.op == FM_OP_RMSEL) rm_rec(m);       /* the next marked entry */
    else rm_end(m);                              /* the folder's walk is over */
    return;
  }
#endif
  if(m->rec_i >= m->rec_n) {
    if(m->scan) {
      if(m->need > m->nfree) { fail_op(m, FM_ERR_NOSPACE); return; }
      m->scan = 0;
      m->rec_i = 0;
      m->rec_off = SRAM_FM_CLIP_ADDR + FM_CLIP_HDR;
      m->h.state = FM_ST_RUN;
      return;
    }
    m->h.state = FM_ST_DONE;
    m->h.progress = 255;
    m->phase = PH_END;
    return;
  }
  type = rec_read(m, &m->rec_off, m->l);
  (void)type;                     /* the card says what it is now: f_stat below */
  m->rec_i++;
  m->p[m->slen] = 0;
  m->q[m->dlen] = 0;
  m->top = 1;
  if(!m->l[0] || pjoin(m->p, m->slen, m->l) < 0 || pjoin(m->q, m->dlen, m->l) < 0) {
    if(!m->scan) item_fail(m, FM_ERR_PATH);
    return;
  }
  m->fi.lfname = 0;
  m->fi.lfsize = 0;
  r = m->api->f_stat(m->p, &m->fi);
  if(r != FR_OK) {
    if(!m->scan) item_fail(m, fr_err(r) == FM_ERR_BADNAME ? FM_ERR_NOTFOUND : fr_err(r));
    return;
  }
  m->isdir = (m->fi.fattrib & AM_DIR) != 0;
  if(m->scan) {
    scan_count(m, m->isdir, m->fi.fsize);
    if(m->isdir) walk_push(m, 0);
    else if(is_game(leaf(m->p))) {     /* the game's own files go with it: in the bar too */
      follow_begin(m);
      while(follow_find(m)) scan_bytes(m, m->fi.fsize);
      m->fstage = 0;
    }
    return;
  }
  item_start(m);
}

/* the next entry of the folder at hand */
static void walk_step(fopmod_t *m) {
  const char *name;
  int pl, ql;
  m->fi.lfname = m->l;
  m->fi.lfsize = FOP_PATH;
  if(m->api->f_readdir(&m->cur.dir, &m->fi) != FR_OK) {
    if(!m->scan) { m->h.n_failed++; m->h.err = FM_ERR_IO; }
    walk_pop(m);
    return;
  }
  if(!m->fi.fname[0]) { walk_pop(m); return; }
  name = m->l[0] ? m->l : m->fi.fname;
  if(name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) return;
  m->isdir = (m->fi.fattrib & AM_DIR) != 0;
  m->top = 0;
  pl = pjoin(m->p, m->cur.plen, name);
  ql = pjoin(m->q, m->cur.qlen, name);
  if(m->scan) {
    scan_count(m, m->isdir, m->fi.fsize);
    if(m->isdir && pl >= 0) walk_push(m, 0);
    m->p[m->cur.plen] = 0;
    return;
  }
  if(pl < 0 || ql < 0) { item_fail(m, FM_ERR_PATH); return; }
#ifdef FOP_RMDIR
  if(FOP_IS_RMDIR(m)) {
    set_name(m, name);
    if(!m->isdir) rm_res(m, m->api->f_unlink(m->p), 0);
    else if(!walk_push(m, FRM_RMDIR)) item_fail(m, FM_ERR_IO);
    return;
  }
#endif
  item_start(m);
}

/* --- PASTE / RENAME / MKDIR -------------------------------------------------------------- */

static void op_paste(fopmod_t *m, const char *path) {
  const module_api_t *api = m->api;
  uint32_t off, i;
  m->mode = api->sram_readbyte(SRAM_FM_CLIP_ADDR);
  m->rec_n = api->sram_readshort(SRAM_FM_CLIP_ADDR + 2);
  if(!m->rec_n || (m->mode != FM_CLIP_COPY && m->mode != FM_CLIP_CUT)) { fail_op(m, FM_ERR_NOCLIP); return; }
  api->sram_read(m->p, SRAM_FM_CLIP_ADDR + 4, FOP_PATH);
  m->p[FOP_MAXPATH] = 0;
  m->slen = (uint16_t)pnorm(m->p);
  for(i = 0; i < FOP_MAXPATH && path[i]; i++) m->q[i] = path[i];
  m->q[i] = 0;
  m->dlen = (uint16_t)pnorm(m->q);
  m->same = eqi(m->p, m->q);
  /* a folder into itself or below itself: refused before anything is touched */
  off = SRAM_FM_CLIP_ADDR + FM_CLIP_HDR;
  for(i = 0; i < m->rec_n; i++) {
    rec_read(m, &off, m->l);
    if(m->l[0] && pjoin(m->p, m->slen, m->l) >= 0 && under(m->q, m->p)) {
      m->p[m->slen] = 0;
      fail_op(m, FM_ERR_INTO_SELF);
      return;
    }
    m->p[m->slen] = 0;
  }
  m->rec_off = SRAM_FM_CLIP_ADDR + FM_CLIP_HDR;
  if(m->mode == FM_CLIP_COPY) {
    m->scan = 1;
    m->h.state = FM_ST_SCAN;
    m->phase = PH_FREE;          /* f_getfree first: it may walk the whole FAT once */
  } else {
    m->h.items_total = m->rec_n;
    m->h.state = FM_ST_RUN;
    m->phase = PH_REC;
  }
}

/* a new leaf from the menu (FM_NAME) into L; 0 = not a name FAT takes */
static int name_get(fopmod_t *m) {
  static const char bad[] = "\\/:*?\"<>|";
  char *l = m->l;
  uint32_t i, j, real = 0;
  m->api->sram_read(l, SRAM_FM_NAME_ADDR, FOP_PATH);
  l[FOP_MAXPATH] = 0;
  for(i = 0; l[i]; i++) {
    uint8_t c = (uint8_t)l[i];
    if(c < 0x20 || c == 0x7f) return 0;
    for(j = 0; bad[j]; j++) if(c == (uint8_t)bad[j]) return 0;
    if(c != '.' && c != ' ') real = 1;
  }
  return real;
}

static void op_done(fopmod_t *m) {
  m->h.state = FM_ST_DONE;
  m->h.items_done = m->h.items_total = 1;
  m->h.n_done = 1;
  m->h.progress = 255;
  m->phase = PH_END;
}

static void op_rename(fopmod_t *m, const char *path) {
  const module_api_t *api = m->api;
  uint32_t i, d, n;
  uint8_t isdir;
  FRESULT r;
  for(i = 0; i < FOP_MAXPATH && path[i]; i++) m->p[i] = path[i];
  m->p[i] = 0;
  if(!pnorm(m->p)) { fail_op(m, FM_ERR_NOTFOUND); return; }
  if(!name_get(m)) { fail_op(m, FM_ERR_BADNAME); return; }
  set_name(m, m->l);
  d = lastsl(m->p);
  for(i = 0; i < d; i++) m->q[i] = m->p[i];
  m->q[d] = 0;
  if(pjoin(m->q, d, m->l) < 0) { fail_op(m, FM_ERR_PATH); return; }
  if((r = fstat(m, m->p, &isdir)) != FR_OK) { fail_op(m, fr_err(r)); return; }
  if(eqx(m->p, m->q)) { op_done(m); return; }
  if(eqi(m->p, m->q)) {
    /* only the case changes: FatFs finds the new name taken (it is the same entry), so the
       entry goes through a free temporary name */
    for(n = 1; ; n++) {
      if(n > 99) { fail_op(m, FM_ERR_IO); return; }
      for(i = 0; i <= d; i++) m->b[i] = m->p[i];
      i = d + 1 + scpy(m->b + d + 1, "~fmren");
      utoa10(m->b + i, n);
      if(fstat(m, m->b, 0) == FR_NO_FILE) break;
    }
    if((r = api->f_rename(m->p, m->b)) != FR_OK) { fail_op(m, fr_err(r)); return; }
    if((r = api->f_rename(m->b, m->q)) != FR_OK) {
      api->f_rename(m->b, m->p);
      fail_op(m, fr_err(r));
      return;
    }
  } else {
    if(fstat(m, m->q, 0) == FR_OK) { fail_op(m, FM_ERR_EXISTS); return; }
    if((r = api->f_rename(m->p, m->q)) != FR_OK) { fail_op(m, fr_err(r)); return; }
  }
  lists_map(m, isdir);
  if(!isdir && is_game(leaf(m->p))) {
    m->top = 1;
    game_follow(m);
  }
  op_done(m);
  if(m->lists) {
    m->lists = 0;
    api->lists_refresh();
  }
}

static void op_mkdir(fopmod_t *m, const char *path) {
  uint32_t i;
  FRESULT r;
  for(i = 0; i < FOP_MAXPATH && path[i]; i++) m->p[i] = path[i];
  m->p[i] = 0;
  i = pnorm(m->p);
  if(!name_get(m)) { fail_op(m, FM_ERR_BADNAME); return; }
  set_name(m, m->l);
  if(pjoin(m->p, i, m->l) < 0) { fail_op(m, FM_ERR_PATH); return; }
  if(fstat(m, m->p, 0) == FR_OK) { fail_op(m, FM_ERR_EXISTS); return; }
  if((r = m->api->f_mkdir(m->p)) != FR_OK) { fail_op(m, fr_err(r)); return; }
  op_done(m);
}

/* --- DELETE (FM_OP_RMDIR) ---------------------------------------------------------------- */
#ifdef FOP_RMDIR

/* path_asset's five roots under /sd2snes: a game's own files */
static const char rm_roots[] = "info\0cheats\0saves\0states\0patches\0";

/* May folder p go?  Never the root nor /sd2snes; inside /sd2snes only the trees of a game's
   own files (path_asset's five roots), which the firmware remakes as it needs them --
   everything else there (the language files, what a later firmware adds) the system reads. */
static int rm_allowed(const char *p) {
  const char *r;
  if(!p[0]) return 0;
  if(!under(p, "/sd2snes")) return 1;
  if(p[8])                                  /* '/': under() leaves '\0' or '/' there */
    for(r = rm_roots; *r; r += slen(r) + 1)
      if(under(p + 9, r)) return 1;
  return 0;
}

/* P (a folder when isdir) was unlinked, or refused with r.  A card that stopped answering
   ends the op; anything else (a read-only entry, a folder left not empty) is counted and the
   rest goes on. */
static void rm_res(fopmod_t *m, FRESULT r, int isdir) {
  if(r == FR_DISK_ERR || r == FR_INT_ERR || r == FR_NOT_READY || r == FR_WRITE_PROTECTED) {
    fail_op(m, FM_ERR_IO);
    return;
  }
  if(r) {
    /* a folder refused as not empty: what was left in it already said why */
    if(!isdir || !m->h.err) m->h.err = (r == FR_DENIED) ? FM_ERR_RDONLY : fr_err(r);
    m->h.n_failed++;
  } else {
    m->h.n_done++;
    if(!isdir) m->lists = 1;        /* a game in it leaves Recents / Favorites (at any end) */
  }
  m->h.items_done++;
  item_next(m);
}

/* After a delete: was everything on the clipboard inside the deleted folder P?  Then the menu
   empties it.  A clipboard that only lost some of its entries stays: the paste reports those
   as not found.  Uses L, A. */
/* FM_OP_RMSEL: the next entry marked in the selection mode, from *i on (FM_MARKS: bit i =
   entry i of the directory table the last READDIR left in PSRAM, the order the menu shows):
   its name into out (a hidden extension's dot put back, a folder's '/' dropped), *i past it.
   Its type, or -1 when no marked entry is left.  ".." never counts. */
static int rm_mark(fopmod_t *m, uint32_t *i, char *out) {
  const module_api_t *api = m->api;
  uint8_t ent[4], b;
  uint32_t k;
  while(*i < FM_MARKS_BYTES * 8) {
    b = api->sram_readbyte(SRAM_FM_MARKS_ADDR + (*i >> 3)) >> (*i & 7);
    if(!b) { *i = (*i | 7) + 1; continue; }
    if(!(b & 1)) { (*i)++; continue; }
    api->sram_read(ent, SRAM_DIR_ADDR + *i * 4, 4);
    (*i)++;
    if(ent[3] == TYPE_PARENT) continue;
    api->sram_read(out, SRAM_MENU_ADDR + (ent[0] | ((uint32_t)ent[1] << 8) | ((uint32_t)ent[2] << 16)) + 6, FOP_PATH);
    out[FOP_MAXPATH] = 0;
    /* display\0 filesystem\0 (filetypes.c): delete/rename must use the UTF-8 name */
    for(k = 0; out[k]; k++) ;
    if(k + 1 < FOP_MAXPATH && out[k + 1]) {
      uint32_t j = 0;
      k++;
      while(out[k] && j < FOP_MAXPATH) out[j++] = out[k++];
      out[j] = 0;
    }
    for(k = 0; out[k]; k++) if(out[k] == 1) out[k] = 0x2e;     /* '.' */
    pnorm(out);
    if(out[0]) return ent[3];
  }
  return -1;
}

/* is L inside what was deleted (the folder P, or one of the marked entries of folder P)? */
static int rm_covered(fopmod_t *m) {
  uint32_t i = 0;
  int r = 0;
  if(m->h.op != FM_OP_RMSEL) return under(m->l, m->p);
  while(!r && rm_mark(m, &i, m->b) >= 0) {
    if(pjoin(m->p, m->slen, m->b) >= 0) r = under(m->l, m->p);
    m->p[m->slen] = 0;
  }
  return r;
}

/* After a delete: was everything on the clipboard inside what was deleted?  Then the menu
   empties it.  A clipboard that only lost some of its entries stays: the paste reports those
   as not found.  Uses L, A (and B for the marked entries). */
static void rm_clip(fopmod_t *m) {
  const module_api_t *api = m->api;
  uint32_t off = SRAM_FM_CLIP_ADDR + FM_CLIP_HDR, n = api->sram_readshort(SRAM_FM_CLIP_ADDR + 2), k;
  if(!n) return;
  api->sram_read(m->l, SRAM_FM_CLIP_ADDR + 4, FOP_PATH);
  m->l[FOP_MAXPATH] = 0;
  k = pnorm(m->l);
  while(n--) {
    rec_read(m, &off, m->a);
    if(pjoin(m->l, k, m->a) < 0 || !rm_covered(m)) return;
  }
  m->h.flags |= FM_FL_UNCLIP;
}

/* FM_OP_RMSEL, one marked entry of folder P (its walk, when a folder, comes back here) */
static void rm_rec(fopmod_t *m) {
  const module_api_t *api = m->api;
  uint32_t i = m->rec_i;
  int t;
  FRESULT r;
  m->p[m->slen] = 0;
  t = rm_mark(m, &i, m->l);
  m->rec_i = (uint16_t)i;
  if(t < 0) { rm_end(m); return; }
  if(pjoin(m->p, m->slen, m->l) < 0) {
    if(!m->scan) item_fail(m, FM_ERR_PATH);
    return;
  }
  scpy(m->q, m->p);
  /* the same rule as one folder: nothing the system reads (a card file of /sd2snes is
     TYPE_FILE; the path rule covers it too).  Checked in the count, before anything goes. */
  if(m->scan && (t == TYPE_FILE || !rm_allowed(m->p))) {
    m->p[m->slen] = 0;
    fail_op(m, FM_ERR_SYSTEM);
    return;
  }
  m->fi.lfname = 0;
  m->fi.lfsize = 0;
  r = api->f_stat(m->p, &m->fi);
  if(r != FR_OK) {
    if(!m->scan) item_fail(m, FM_ERR_NOTFOUND);
    return;
  }
  m->isdir = (m->fi.fattrib & AM_DIR) != 0;
  if(m->scan) {
    scan_count(m, m->isdir, m->fi.fsize);
    if(m->isdir) walk_push(m, 0);
    return;
  }
  set_name(m, m->l);
  if(!m->isdir) rm_res(m, api->f_unlink(m->p), 0);
  else if(!walk_push(m, FRM_RMDIR)) item_fail(m, FM_ERR_IO);
}

/* the folder's walk is over: after the count, ask; after the removal, done */
static void rm_end(fopmod_t *m) {
  if(m->h.op == FM_OP_RMSEL) m->p[m->slen] = 0;   /* P = the folder the entries are in */
  if(m->scan) {
    m->scan = 0;
    if(m->rm_rest) m->h.bytes_total++;   /* rounded up: "0 KB" is an empty folder */
    if(m->h.op == FM_OP_RMSEL && !m->h.items_total) { fail_op(m, FM_ERR_NOTFOUND); return; }
    set_name(m, m->h.op == FM_OP_RMSEL ? "" : leaf(m->p));
    m->h.state = FM_ST_ASK;
    m->h.ask = FM_ASK_RMDIR;
    m->phase = PH_ASK;
    return;
  }
  rm_clip(m);
  m->h.state = FM_ST_DONE;
  m->h.progress = 255;
  m->phase = PH_END;
}

/* the answer was yes: the same walk again, removing */
static void rm_go(fopmod_t *m) {
  m->h.state = FM_ST_RUN;
  m->h.items_done = 0;
  if(m->h.op == FM_OP_RMSEL) {
    m->rec_i = 0;
    m->phase = PH_REC;
  } else if(!walk_push(m, FRM_RMDIR)) fail_op(m, FM_ERR_IO);
}

static void op_rmsel(fopmod_t *m, const char *path) {
  uint32_t i;
  for(i = 0; i < FOP_MAXPATH && path[i]; i++) m->p[i] = path[i];
  m->p[i] = 0;
  m->slen = (uint16_t)pnorm(m->p);
  m->scan = 1;
  m->h.state = FM_ST_SCAN;
  m->phase = PH_REC;
}

static void op_rmdir(fopmod_t *m, const char *path) {
  const char *lf;
  uint32_t i;
  for(i = 0; i < FOP_MAXPATH && path[i]; i++) m->p[i] = path[i];
  m->p[i] = 0;
  pnorm(m->p);
  scpy(m->q, m->p);                 /* the walk keeps a destination too: unused here */
  if(!rm_allowed(m->p)) { fail_op(m, FM_ERR_SYSTEM); return; }
  lf = leaf(m->p);
  set_name(m, lf);
  m->scan = 1;
  m->h.state = FM_ST_SCAN;
  m->h.items_total = 1;             /* the folder itself */
  /* "." / ".." would open the folder itself or its parent; a file does not open */
  if((lf[0] == '.' && (!lf[1] || (lf[1] == '.' && !lf[2]))) || !walk_push(m, 0))
    fail_op(m, FM_ERR_NOTFOUND);
}
#endif

/* --- VIEW ---------------------------------------------------------------------------------
 * Window k shows the file from woff[k] (kept in SRAM_FM_WORK_ADDR) up to the last line break
 * that fits both FM_VIEW_WINDOW bytes and FM_VIEW_MAXLINES screen lines; a line with no
 * break in a whole window is cut where the window ends.  Two passes over the bytes: the
 * first decides UTF-8 (the window decodes as UTF-8 throughout) or CP1252, the second
 * writes the lines. */

typedef struct {
  uint32_t pos, n, i;
} fop_vr_t;

static int vpeek(fopmod_t *m, fop_vr_t *r) {
  UINT br;
  if(r->i == r->n) {
    if(m->api->f_read(&m->fo, m->buf, m->bufsz, &br) != FR_OK || !br) return -1;
    r->n = br;
    r->i = 0;
  }
  return m->buf[r->i];
}

static void vadv(fop_vr_t *r) {
  r->i++;
  r->pos++;
}

/* bytes in the UTF-8 sequence that b leads, 0 when b cannot lead one */
static uint32_t u8len(uint8_t b) {
  if(b < 0x80) return 1;
  if(b >= 0xc2 && b <= 0xdf) return 2;
  if(b >= 0xe0 && b <= 0xef) return 3;
  if(b >= 0xf0 && b <= 0xf4) return 4;
  return 0;
}

/* decode one sequence of len bytes at the reader; 0xffffffff when it is not a valid one */
static uint32_t u8get(fopmod_t *m, fop_vr_t *r, uint32_t len) {
  int c = vpeek(m, r);
  uint32_t cp, k;
  vadv(r);
  cp = (uint32_t)c & (len == 2 ? 0x1f : len == 3 ? 0x0f : 0x07);
  for(k = 1; k < len; k++) {
    c = vpeek(m, r);
    if(c < 0 || (c & 0xc0) != 0x80) return 0xffffffffUL;
    vadv(r);
    cp = (cp << 6) | (c & 0x3f);
  }
  if((len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (len == 4 && (cp < 0x10000 || cp > 0x10ffff)))
    return 0xffffffffUL;
  return cp;
}

/* is [start, limit) UTF-8 (a sequence cut by the end counts as fine)? */
static int view_utf8(fopmod_t *m, uint32_t start, uint32_t limit) {
  fop_vr_t r = { start, 0, 0 };
  int c;
  uint32_t len;
  while(r.pos < limit) {
    c = vpeek(m, &r);
    if(c < 0) return 1;
    len = u8len((uint8_t)c);
    if(!len) return 0;
    if(len == 1) { vadv(&r); continue; }
    if(r.pos + len > limit) return 1;
    if(u8get(m, &r, len) == 0xffffffffUL) return 0;
  }
  return 1;
}

typedef struct {
  uint8_t  lb[FM_VIEW_COLS + 4];
  uint32_t col, lines;
  int32_t  sp;                    /* column of the last space in lb, -1 none */
  uint32_t sp_next;               /* file position after that space */
  uint32_t lbpos;                 /* file position where lb's text begins */
} fop_vl_t;

static int vemit(fopmod_t *m, fop_vl_t *v, uint32_t n) {
  uint32_t num = v->lbpos, den = m->v_size, rec;
  if(v->lines >= FM_VIEW_MAXLINES) return 0;
  v->lb[n] = 0;
  rec = SRAM_FM_VIEW_ADDR + v->lines * FM_VIEW_STRIDE;
  m->api->sram_write(v->lb, rec, (uint16_t)(n + 1));
  while(den > 0x00ffffffUL) { den >>= 8; num >>= 8; }
  m->api->sram_writebyte(den ? (uint8_t)((num * 100) / den) : 0, rec + FM_VIEW_PCT);
  v->lines++;
  return 1;
}

/* one font byte of the character at cpos (after = the position past it) */
static int vput(fopmod_t *m, fop_vl_t *v, uint8_t b, uint32_t cpos, uint32_t after) {
  uint32_t k, s;
  if(v->col == FM_VIEW_COLS) {
    if(v->sp >= FM_VIEW_COLS - 16) {
      s = (uint32_t)v->sp;
      if(!vemit(m, v, s)) return 0;
      for(k = s + 1; k < v->col; k++) v->lb[k - s - 1] = v->lb[k];
      v->col -= s + 1;
      v->lbpos = v->sp_next;
    } else {
      if(!vemit(m, v, v->col)) return 0;
      v->col = 0;
      v->lbpos = cpos;
    }
    v->sp = -1;
  }
  if(b == ' ') { v->sp = (int32_t)v->col; v->sp_next = after; }
  v->lb[v->col++] = b;
  return 1;
}

static void view_render(fopmod_t *m, uint32_t k) {
  const module_api_t *api = m->api;
  fop_vr_t r;
  fop_vl_t v;
  uint32_t start, limit, base, brk, brk_lines, end, cpos, cp, len, n;
  uint8_t fb[4];
  int utf, c, last = 0, i, nf;
  if(k >= m->v_known) return;                     /* its start is not known yet */
  start = api->sram_readshort(FOP_WOFF + 4 * k) | ((uint32_t)api->sram_readshort(FOP_WOFF + 4 * k + 2) << 16);
  limit = (m->v_size - start > FM_VIEW_WINDOW) ? start + FM_VIEW_WINDOW : m->v_size;
  if(api->f_open(&m->fo, m->p, FA_READ) != FR_OK || api->f_lseek(&m->fo, start) != FR_OK) {
    api->f_close(&m->fo);
    fail_op(m, FM_ERR_IO);
    return;
  }
  utf = view_utf8(m, start, limit);
  api->f_lseek(&m->fo, start);
  r.pos = start; r.n = 0; r.i = 0;
  v.col = 0; v.lines = 0; v.sp = -1; v.sp_next = 0;
  if(utf && !start && m->v_size >= 3 && vpeek(m, &r) == 0xef) {   /* a BOM is not text */
    fop_vr_t s = r;
    vadv(&s);
    if(vpeek(m, &s) == 0xbb) { vadv(&s); if(vpeek(m, &s) == 0xbf) { vadv(&s); r = s; } }
  }
  base = brk = v.lbpos = r.pos;
  brk_lines = 0;
  for(;;) {
    cpos = r.pos;
    c = (cpos < limit) ? vpeek(m, &r) : -1;
    if(c < 0) {
      if(limit == m->v_size || cpos < limit) {    /* the end of the file */
        if(v.col && !vemit(m, &v, v.col)) goto lines_full;
        end = cpos; last = 1;
        break;
      }
      goto bytes_full;
    }
    if(c == '\r' || c == '\n') {
      if(c == '\r' && cpos + 1 == limit && limit < m->v_size) goto bytes_full;  /* a CRLF cut in two */
      vadv(&r);
      if(c == '\r' && r.pos < limit && vpeek(m, &r) == '\n') vadv(&r);
      if(!vemit(m, &v, v.col)) goto lines_full;
      v.col = 0; v.sp = -1;
      brk = v.lbpos = r.pos;
      brk_lines = v.lines;
      continue;
    }
    if(utf && c >= 0x80) {
      len = u8len((uint8_t)c);
      if(len && cpos + len > limit && limit < m->v_size) goto bytes_full;
      if(!len || cpos + len > limit) { vadv(&r); cp = 0xfffd; }
      else if((cp = u8get(m, &r, len)) == 0xffffffffUL) cp = 0xfffd;
    } else {
      vadv(&r);
      cp = utf ? (uint32_t)c : cp1252((uint8_t)c);
    }
    if(cp == '\t') {
      for(n = 4 - (v.col & 3); n; n--) if(!vput(m, &v, ' ', cpos, r.pos)) goto lines_full;
      continue;
    }
    if(cp == 0xfffd) { fb[0] = '?'; nf = 1; }
    else nf = font(m, cp, fb);
    for(i = 0; i < nf; i++) if(!vput(m, &v, fb[i], cpos, r.pos)) goto lines_full;
  }
  goto done;
bytes_full:                         /* the window's bytes are used up */
  if(brk > base) { end = brk; v.lines = brk_lines; goto done; }
  if(v.col && !vemit(m, &v, v.col)) goto lines_full;
  end = cpos;
  goto done;
lines_full:                         /* the screen lines are used up */
  if(brk > base) { end = brk; v.lines = brk_lines; }
  else { end = v.lbpos; v.lines = FM_VIEW_MAXLINES; }
done:
  api->f_close(&m->fo);
  if(!last && end <= start) end = limit;          /* never a window that does not move */
  if(!last && k + 1 == m->v_known && m->v_known < FOP_WOFF_MAX) {
    api->sram_writeshort((uint16_t)end, FOP_WOFF + 4 * (k + 1));
    api->sram_writeshort((uint16_t)(end >> 16), FOP_WOFF + 4 * (k + 1) + 2);
    m->v_known++;
  }
  m->h.state = FM_ST_VIEW;
  m->h.view_win = (uint16_t)k;
  m->h.view_lines = (uint16_t)v.lines;
  m->h.view_last = (uint8_t)last;
  {
    uint32_t num = start, den = m->v_size;
    while(den > 0x00ffffffUL) { den >>= 8; num >>= 8; }
    m->h.view_pct = den ? (uint8_t)((num * 100) / den) : 0;
  }
}

static void op_view(fopmod_t *m, const char *path) {
  const module_api_t *api = m->api;
  uint32_t i;
  FRESULT r;
  for(i = 0; i < FOP_MAXPATH && path[i]; i++) m->p[i] = path[i];
  m->p[i] = 0;
  pnorm(m->p);
  set_name(m, leaf(m->p));
  if((r = api->f_open(&m->fo, m->p, FA_READ)) != FR_OK) { fail_op(m, fr_err(r)); return; }
  m->v_size = m->fo.fsize;
  api->f_close(&m->fo);
  api->sram_writeshort(0, FOP_WOFF);
  api->sram_writeshort(0, FOP_WOFF + 2);
  m->v_known = 1;
  view_render(m, 0);
}

/* --- the module --------------------------------------------------------------------------- */

int fopmod_start(void *st, const module_api_t *api, const char *path) {
  fopmod_t *m = st;
  uint32_t size;
  uint8_t *sp;
  bzero8(m, sizeof(*m));
  m->api = api;
  m->h.magic[0] = FM_MAGIC0;
  m->h.magic[1] = FM_MAGIC1;
  m->h.version = FM_VERSION;
  m->h.op = api->sram_readbyte(SRAM_FM_STAT_ADDR + 4);
  set_name(m, "");
  sp = api->spare(&size);
  if(size >= FOP_NBUF * FOP_PATH + FOP_BUF_MIN) {
    m->p = (char *)sp;
    m->q = m->p + FOP_PATH;
    m->l = m->q + FOP_PATH;
    m->a = m->l + FOP_PATH;
    m->b = m->a + FOP_PATH;
    m->buf = sp + FOP_NBUF * FOP_PATH;
    m->bufsz = (size - FOP_NBUF * FOP_PATH) & ~(uint32_t)(FOP_RD - 1);
    if(m->bufsz > FOP_BUF_MAX) m->bufsz = FOP_BUF_MAX;
    switch(m->h.op) {
#ifdef FOP_FILEOPS
      case FM_OP_PASTE:  op_paste(m, path); break;
      case FM_OP_RENAME: op_rename(m, path); break;
      case FM_OP_MKDIR:  op_mkdir(m, path); break;
      case FM_OP_VIEW:   op_view(m, path); break;
#endif
#ifdef FOP_RMDIR
      case FM_OP_RMDIR:  op_rmdir(m, path); break;
      case FM_OP_RMSEL:  op_rmsel(m, path); break;
#endif
      default:           fail_op(m, FM_ERR_IO); break;
    }
  } else {
    fail_op(m, FM_ERR_IO);
  }
  publish(m);
  return 0;
}

static int running(fopmod_t *m) {
  return m->phase >= PH_FREE && m->phase <= PH_ASK;
}

void fopmod_ctl(void *st, uint32_t param) {
  fopmod_t *m = st;
  uint8_t a;
  switch(param & 0xff) {
    case FM_CTL_CANCEL:
    case FM_CTL_CLOSE:
      if(running(m)) m->cancel = 1;
      break;
    case FM_CTL_ANSWER:
      a = (param >> 8) & 0xff;
      if(m->phase == PH_ASK && a >= FM_ANS_REPLACE && a <= FM_ANS_KEEPBOTH) {
        m->answer = a;
        m->answer_all = (param >> 16) & 0xff;
      }
      break;
#ifdef FOP_FILEOPS
    case FM_CTL_VIEW_WIN:
      if(m->h.op == FM_OP_VIEW && m->h.state == FM_ST_VIEW) {
        view_render(m, (param >> 8) & 0xffff);
        publish(m);
      }
      break;
#endif
  }
}

void fopmod_idle(void *st) {
  fopmod_t *m = st;
  const module_api_t *api = m->api;
  uint32_t t0;
#ifdef FOP_FILEOPS
  uint32_t nclst;
  FATFS *fs;
#endif
  if(!running(m)) return;
  t0 = api->ticks();
  do {
    if(m->cancel) {
      if(m->copying) copy_abort(m);
      fail_op(m, FM_ERR_CANCELLED);
      break;
    }
    switch(m->phase) {
#ifdef FOP_FILEOPS
      case PH_FREE:
        if(api->f_getfree("/", &nclst, &fs) != FR_OK) { fail_op(m, FM_ERR_IO); break; }
        m->nfree = nclst;
        m->clus = (uint32_t)fs->csize * 512;
        m->phase = PH_REC;
        break;
      case PH_REC:  rec_step(m); break;
      case PH_WALK: walk_step(m); break;
      case PH_COPY: copy_step(m); break;
#else
      case PH_REC:  rec_step(m); break;
      case PH_WALK: walk_step(m); break;
#endif
      case PH_ASK:
        if(m->answer) {
          uint8_t kind = m->h.ask, a = m->answer;
#ifdef FOP_RMDIR
          if(m->answer_all && kind <= 3) m->all[kind - 1] = a;   /* FM_ASK_RMDIR has no "all" */
#else
          if(m->answer_all) m->all[kind - 1] = a;
#endif
          m->answer = 0;
          m->h.ask = 0;
          m->h.state = FM_ST_RUN;
          m->phase = m->depth ? PH_WALK : PH_REC;
          item_answer(m, kind, a);
        }
        break;
    }
  } while(running(m) && m->phase != PH_ASK && api->ticks() - t0 < FOP_SLICE);
  if(m->phase == PH_END && m->lists) {
    m->lists = 0;
    api->lists_refresh();
  }
  if(m->h.state != FM_ST_DONE) progress(m);
  publish(m);
  if(running(m) && m->phase != PH_ASK) api->hurry();
}

void fopmod_stop(void *st) {
  fopmod_t *m = st;
  if(m->copying) copy_abort(m);
  if(m->lists) {
    m->lists = 0;
    m->api->lists_refresh();
  }
  m->phase = PH_NONE;
}

MODULE_DEFINE(fopmod, fopmod_t)
