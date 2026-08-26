/* Host CLI over the REAL src/cfg.c (+ src/yaml.c and the two S-RTC converters
 * extracted from the platform rtc.c).  Nothing about the config format is
 * transcribed here: run_cfg.sh drives this binary to write config.yml files and
 * to dump the resulting cfg_t, and compares both against goldens.  A diff means
 * cfg.c changed what it writes to the card or what it makes of what it reads.
 *
 * The firmware paths are absolute ("/sd2snes/config.yml") and hard-coded in
 * cfg.h, so the FatFs surface here re-roots them under build/ (host_path()).
 * The string functions have to be faithful, not merely plausible: ffconf.h sets
 * _USE_STRFUNC == 2, so f_putc/f_puts/f_printf translate '\n' to CRLF and
 * f_gets strips '\r'.  A config.yml on a card is CRLF; drop that here and every
 * golden is wrong by a byte per line.
 *
 * Usage:
 *   cfg_cli size                         print sizeof(cfg_t)
 *   cfg_cli save-default     <out.yml>   fresh-card defaults -> cfg_save()
 *   cfg_cli save-allchanged  <out.yml>   every serialized field off its default
 *   cfg_cli save-alternating <out.yml>   booleans alternate by table position
 *   cfg_cli save-altoffset <out.yml>     ...and by their offset in cfg_t
 *   cfg_cli load-dump <in.yml> <out.bin>  cfg_load() -> raw cfg_t image
 *   cfg_cli roundtrip <in.yml> <out.yml>  cfg_load() then cfg_save()
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"    /* shim_cfg: system headers, __attribute__ left ALONE */
#include "ff.h"        /* shim: the minimal FatFs surface */
#include "fileops.h"   /* shim: file_handle / file_res / file_status / file_path */
#include "memory.h"    /* shim: sram_* prototypes + the real SRAM_* addresses */

#include "cfg.h"       /* shim_cfg forwarder -> the REAL src/cfg.h */
#include "snes.h"      /* the real src/snes.h: SNES_BUTTON_*, mcu_status_t */

extern cfg_t CFG;
extern const cfg_t CFG_DEFAULT;

/* cfg.c declares this; nothing it does touches it, but the definition keeps
   the link honest if that ever changes. */
mcu_status_t STM;

/* ---- virtual SD card ----------------------------------------------------
 * Every firmware path is absolute; re-root it under build/ so "/sd2snes/x"
 * becomes "build/sd2snes/x".  Callers may hold two mapped paths at once
 * (f_rename), hence the small rotation instead of one static buffer. */
#define SD_BASE "build"

static const char *host_path(const char *p) {
  static char bufs[4][512];
  static int slot;
  char *b = bufs[slot];
  slot = (slot + 1) & 3;
  snprintf(b, sizeof bufs[0], "%s%s%s", SD_BASE, (p[0] == '/') ? "" : "/", p);
  return b;
}

/* mkdir -p of the directory portion of a mapped path (host side only). */
static void host_mkdir_for(const char *mapped) {
  char tmp[512];
  char *s;
  strncpy(tmp, mapped, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  for (s = tmp + 1; *s; s++) {
    if (*s != '/') continue;
    *s = 0;
    mkdir(tmp, 0777);
    *s = '/';
  }
}

/* ---- fileops / FatFs globals -------------------------------------------- */
BYTE    file_buf[512];
FIL     file_handle;
FRESULT file_res;
uint8_t file_lfn[258];
uint8_t file_path[256];
enum filestates file_status;

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode) {
  const char *hp = host_path(path);
  const char *fmode = (mode & (FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) ? "wb" : "rb";
  memset(fp, 0, sizeof(*fp));
  if (fmode[0] == 'w') host_mkdir_for(hp);
  fp->fp = fopen(hp, fmode);
  if (!fp->fp) return FR_NO_FILE;
  fseek(fp->fp, 0, SEEK_END);
  fp->fsize = (DWORD)ftell(fp->fp);
  fseek(fp->fp, 0, SEEK_SET);
  fp->fptr = 0;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  if (fp->fp) { fclose(fp->fp); fp->fp = NULL; }
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  UINT got;
  if (!fp->fp) { if (br) *br = 0; return FR_INT_ERR; }
  got = (UINT)fread(buff, 1, btr, fp->fp);
  fp->fptr += got;
  if (br) *br = got;
  return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
  UINT put;
  if (!fp->fp) { if (bw) *bw = 0; return FR_INT_ERR; }
  put = (UINT)fwrite(buff, 1, btw, fp->fp);
  fp->fptr += put;
  if (fp->fptr > fp->fsize) fp->fsize = fp->fptr;
  if (bw) *bw = put;
  return (put == btw) ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, DWORD ofs) {
  if (!fp->fp) return FR_INT_ERR;
  if (ofs > fp->fsize) ofs = fp->fsize;   /* FatFs clamps read-only files */
  if (fseek(fp->fp, (long)ofs, SEEK_SET) != 0) return FR_DISK_ERR;
  fp->fptr = ofs;
  return FR_OK;
}

FRESULT f_unlink(const TCHAR *path) {
  return remove(host_path(path)) == 0 ? FR_OK : FR_NO_FILE;
}

FRESULT f_rename(const TCHAR *path_old, const TCHAR *path_new) {
  const char *a = host_path(path_old);
  const char *b = host_path(path_new);
  return rename(a, b) == 0 ? FR_OK : FR_DENIED;
}

FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
  struct stat st;
  if (stat(host_path(path), &st) != 0) return FR_NO_FILE;
  if (fno) { memset(fno, 0, sizeof(*fno)); }
  return FR_OK;
}

FRESULT f_opendir(DIR *dp, const TCHAR *path) { (void)dp; (void)path; return FR_NO_PATH; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; fno->fname[0] = 0; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

void file_open(const uint8_t *filename, BYTE flags) {
  file_res = f_open(&file_handle, (const TCHAR *)filename, flags);
  file_status = file_res ? FILE_ERR : FILE_OK;
}

void file_close(void) {
  file_res = f_close(&file_handle);
}

/* ---- FatFs string functions (ffconf.h: _USE_STRFUNC == 2) ---------------
 * putc_bfd() in src/ff.c expands '\n' to CRLF on the way out, and f_gets()
 * drops '\r' and stops on '\n' OR on a NUL byte (that NUL stop is what makes
 * the NUL-separated recent/favorite lists readable with f_gets at all). */
static int host_putc_raw(FIL *fp, TCHAR c) {
  UINT bw;
  BYTE b = (BYTE)c;
  if (f_write(fp, &b, 1, &bw) != FR_OK || bw != 1) return EOF;
  return 1;
}

int f_putc(TCHAR c, FIL *fp) {
  int n = 0;
  if (c == '\n') {                      /* LF -> CRLF */
    if (host_putc_raw(fp, '\r') == EOF) return EOF;
    n++;
  }
  if (host_putc_raw(fp, c) == EOF) return EOF;
  return n + 1;
}

int f_puts(const TCHAR *str, FIL *fp) {
  int n = 0, r;
  while (*str) {
    r = f_putc(*str++, fp);
    if (r == EOF) return EOF;
    n += r;
  }
  return n;
}

/* f_printf: one conversion at a time through snprintf, with the 'l' length
 * modifier DROPPED.  FatFs reads a long-flagged argument as va_arg(arp, long)
 * and a long is 32 bits on the LPC175x -- exactly the uint32_t cfg.c pushes for
 * "%06lX%08lX" (BSXTime).  A 64-bit host long would consume eight bytes for a
 * four-byte argument, so stripping 'l' leaves 32 bits on both sides.
 *
 * Only the conversions FatFs implements are accepted, and only the ones cfg.c
 * uses are wired up; anything else aborts rather than guessing (run_cfg.sh
 * screens for new specifiers before the build). */
int f_printf(FIL *fp, const TCHAR *fmt, ...) {
  char out[2048];
  size_t n = 0;
  va_list ap;

  va_start(ap, fmt);
  while (*fmt) {
    char spec[16];
    size_t s = 0;
    int wrote = 0;

    if (*fmt != '%') {
      if (n >= sizeof out - 1) {
        fprintf(stderr, "cfg_cli: f_printf output overflow (raise `out`)\n");
        exit(99);
      }
      out[n++] = *fmt++;
      continue;
    }
    spec[s++] = *fmt++;                       /* '%' */
    while (*fmt && !strchr("diouxXcs%", *fmt) && s < sizeof spec - 2) {
      if (*fmt == 'l' || *fmt == 'L') { fmt++; continue; }   /* see above */
      spec[s++] = *fmt++;
    }
    if (!*fmt) break;
    spec[s++] = *fmt;
    spec[s] = 0;

    switch (*fmt++) {
      case 'd': case 'i':
        wrote = snprintf(out + n, sizeof out - n, spec, va_arg(ap, int));
        break;
      case 'u': case 'o': case 'x': case 'X':
        wrote = snprintf(out + n, sizeof out - n, spec, va_arg(ap, unsigned int));
        break;
      case 'c':
        wrote = snprintf(out + n, sizeof out - n, spec, va_arg(ap, int));
        break;
      case 's':
        wrote = snprintf(out + n, sizeof out - n, spec, va_arg(ap, char *));
        break;
      case '%':
        wrote = snprintf(out + n, sizeof out - n, "%%");
        break;
      default:
        fprintf(stderr, "cfg_cli: f_printf conversion '%s' not modelled\n", spec);
        exit(99);
    }
    if (wrote < 0 || (size_t)wrote >= sizeof out - n) {
      fprintf(stderr, "cfg_cli: f_printf output overflow on '%s'\n", spec);
      exit(99);
    }
    n += (size_t)wrote;
  }
  va_end(ap);
  out[n] = 0;
  return f_puts(out, fp);
}

TCHAR *f_gets(TCHAR *buff, int len, FIL *fp) {
  int n = 0;
  TCHAR c, *p = buff;
  BYTE s[1];
  UINT rc;

  while (n < len - 1) {
    f_read(fp, s, 1, &rc);
    if (rc != 1) break;
    c = (TCHAR)s[0];
    if (c == '\r') continue;      /* _USE_STRFUNC == 2: strip '\r' */
    *p++ = c;
    n++;
    if (c == '\n') break;         /* break on EOL */
    if (c == 0) break;            /* break on NUL */
  }
  *p = 0;
  return n ? buff : 0;
}

/* ---- sram_* accessors over a plain array -------------------------------
 * cfg.c only writes the CFG mirror and the two LAST_GAME strings; nothing
 * reads them back here, but sizing the array to the 16 MB window keeps the
 * shipping SRAM_* addresses usable as-is. */
#define SDRAM_SIZE 0x1000000u
#define SDRAM_MASK 0x0FFFFFFu
static uint8_t *host_sram;

static void host_sram_init(void) {
  host_sram = calloc(SDRAM_SIZE, 1);
  if (!host_sram) { fprintf(stderr, "cfg_cli: calloc failed\n"); exit(99); }
}

void sram_writebyte(uint8_t val, uint32_t addr) { host_sram[addr & SDRAM_MASK] = val; }
uint8_t sram_readbyte(uint32_t addr) { return host_sram[addr & SDRAM_MASK]; }

void sram_writeblock(const void *buf, uint32_t addr, uint16_t size) {
  for (uint16_t i = 0; i < size; i++)
    host_sram[(addr + i) & SDRAM_MASK] = ((const uint8_t *)buf)[i];
}

void sram_readblock(void *buf, uint32_t addr, uint16_t size) {
  for (uint16_t i = 0; i < size; i++)
    ((uint8_t *)buf)[i] = host_sram[(addr + i) & SDRAM_MASK];
}

void sram_readstrn(void *buf, uint32_t addr, uint16_t size) {
  uint16_t i = 0;
  if (!size) return;
  for (; i < (uint16_t)(size - 1); i++) {
    uint8_t c = host_sram[(addr + i) & SDRAM_MASK];
    ((uint8_t *)buf)[i] = c;
    if (!c) return;
  }
  ((uint8_t *)buf)[i] = 0;
}

void sram_writestrn(void *buf, uint32_t addr, uint16_t size) {
  const char *src = buf;
  uint16_t i = 0;
  while (i < size) {
    host_sram[(addr + i) & SDRAM_MASK] = (uint8_t)src[i];
    if (!src[i]) break;
    i++;
  }
}

int save_sram(uint8_t *filename, uint32_t sram_size, uint32_t base_addr) {
  (void)filename; (void)sram_size; (void)base_addr;
  return 0;
}

/* ---- host file plumbing ------------------------------------------------- */
static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb"), *out;
  char buf[4096];
  size_t n;
  if (!in) { perror(from); return -1; }
  host_mkdir_for(to);
  out = fopen(to, "wb");
  if (!out) { perror(to); fclose(in); return -1; }
  while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { perror(to); fclose(in); fclose(out); return -1; }
  }
  fclose(in);
  return fclose(out) == 0 ? 0 : -1;
}

/* ---- the all-changed config --------------------------------------------
 * Start from CFG_DEFAULT and move EVERY field cfg_save() serializes off its
 * default, so a golden captured from it pins each write site individually.
 * The values are a FIXED POINT of load->save: they survive the clamps in
 * cfg_load (brightness &0xf, led >15, language >5, the 0/1/2 multi-state
 * fields) and cfg_check_menu_combo.
 *
 * Four fields deliberately stay at their default, because cfg_save() does not
 * write them and cfg_load() does not read them:
 *   screensaver_timeout  both f_printf lines are commented out in cfg_save
 *   control_type         CFG_CONTROL_TYPE is defined in cfg.h and never used
 *   show_tribute         reserved padding, keeps the menu CFG offsets aligned
 *   enable_sram_slots    retired toggle, forced to 1
 * sgb_spr_increase is a fifth, conditional case: written and read only under
 * CONFIG_MK3, which is why this gate builds twice. */
static void cfg_make_allchanged(void) {
  memcpy(&CFG, &CFG_DEFAULT, sizeof(cfg_t));

  CFG.vidmode_menu = VIDMODE_50;                 /* 0 -> 1 */
  CFG.vidmode_game = VIDMODE_60;                 /* 2 -> 0 */
  CFG.pair_mode_allowed = 1;
  CFG.bsx_use_usertime = 1;
  /* 2015-12-25 09:41:07 in the S-RTC layout (see bcdtime2srtctime): least
     significant first -- [0..1] seconds, [2..3] minutes, [4..5] hours, [6..7]
     day, [8] month as a BINARY 1..12, [9..10] year, [11] century minus 10.
     Round-trips through the "%06lX%08lX" form cfg_save writes. */
  {
    const uint8_t t[12] = {7, 0, 1, 4, 9, 0, 5, 2, 12, 5, 1, 10};
    memcpy(CFG.bsx_time, t, sizeof t);
  }
  CFG.r213f_override = 0;
  CFG.enable_ingame_hook = 1;
  CFG.enable_ingame_buttons = 0;
  CFG.enable_hook_holdoff = 0;
  CFG.enable_screensaver = 0;
  CFG.sort_directories = 0;
  CFG.hide_extensions = 1;
  CFG.cx4_speed = 1;
  strcpy((char *)CFG.skin_name, "/themes/neon.thm");
  CFG.msu_volume_boost = 3;
  CFG.onechip_transient_fixes = 1;
  CFG.brightness_limit = 7;                      /* survives & 0xf */
  CFG.gsu_speed = 1;
  CFG.reset_to_menu = 2;                         /* survives the >3 clamp */
  CFG.led_brightness = 4;                        /* survives the >15 clamp */
  CFG.enable_cheats = 0;
  CFG.reset_patch = 0;
  CFG.enable_ingame_savestate = 1;
  CFG.loadstate_delay = 20;
  CFG.enable_savestate_slots = 0;
  CFG.ingame_buttons_savestate   = SNES_BUTTON_START | SNES_BUTTON_X;   /* "SX" */
  CFG.ingame_buttons_loadstate   = SNES_BUTTON_START | SNES_BUTTON_A;   /* "SA" */
  CFG.ingame_buttons_changestate = SNES_BUTTON_Y | SNES_BUTTON_SELECT;  /* "Ys" */
  CFG.sgb_enable_ingame_hook = 1;
  CFG.sgb_enable_state = 1;
  CFG.sgb_volume_boost = 4;
  CFG.sgb_enh_override = 1;
  CFG.sgb_spr_increase = 1;                      /* CONFIG_MK3 only, see above */
  CFG.sgb_clock_fix = 0;
  CFG.sgb_bios_version = 1;
  CFG.enable_autosave = 0;
  CFG.enable_autosave_msu1 = 0;
  CFG.show_covers = 2;                           /* small */
  CFG.language = 3;                              /* German; survives the >5 clamp */
  CFG.patch_verify_integrity = 1;
  CFG.enable_menu_music = 0;
  CFG.covers_in_lists = 0;
  CFG.enable_menu_sfx = 0;
  strcpy((char *)CFG.bgm_name, "/music/track01.spc");
  CFG.sort_favorites = 0;
  CFG.enable_cheat_overlay = 0;
  CFG.show_game_info = 2;                        /* context */
  CFG.enable_wifi = 1;
  CFG.game_info_video = 0;
  CFG.game_info_music = 0;
  CFG.enable_bps_copier = 0;
  CFG.clear_ppu_on_boot = 1;
  CFG.bus_compat = 1;
  CFG.enable_game_manual = 0;
  /* L+R+X+Right: 4 buttons, includes a shoulder, and no subset of any reserved
     gesture -- cfg_check_menu_combo() has to accept it verbatim ("rXLR"). */
  CFG.ingame_buttons_menu = SNES_BUTTON_L | SNES_BUTTON_R | SNES_BUTTON_X | SNES_BUTTON_RIGHT;
  CFG.a26_video_width = 1;                       /* 256 px stretched */
  CFG.cc_time_limit = 15;                        /* 18 minutes */
}

/* ---- the alternating-booleans config ------------------------------------
 * cfg_make_allchanged() moves every boolean to the SAME value, which hides one
 * class of mistake: swap the field names between two CFGI lines whose booleans
 * share a default and cfg_save/cfg_load swap with them, leaving file and image
 * byte-identical -- one table drives both directions, so it cannot catch
 * itself.
 *
 * Here each boolean alternates instead, so NEIGHBOURS hold different values and
 * swapping the lines that name them changes both what cfg_save writes and what
 * cfg_load stores.  "Neighbour" has two meanings and they do not coincide:
 *
 *   ALT_BY_INDEX    parity of the boolean's line in cfg_items[]: separates
 *                   every pair of ADJACENT TABLE LINES.
 *   ALT_BY_OFFSET   parity of its byte offset in cfg_t: separates every pair
 *                   of ADJACENT STRUCT FIELDS.
 *
 * One boolean carries one bit, so no single fixture can do both; run_cfg.sh
 * captures a golden along each axis and the pair of them is the gate.  Pairs
 * that are neighbours in NEITHER order can still alias.
 *
 * The offsets are DISCOVERED, not transcribed: cfg_items[] is static to cfg.c
 * and no header exposes it, so the CLI writes the defaults out, flips one
 * "true"/"false" line at a time, loads the result and sees which byte of cfg_t
 * moved.  The probe is itself a check: a boolean key that moves no byte, or
 * more than one, stops the run.
 */
enum alt_axis { ALT_BY_INDEX, ALT_BY_OFFSET };
#define ALT_MAX_LINES 128

struct alt_line {
  size_t off;    /* start of the line in alt_buf */
  size_t len;    /* its length, EOL included */
  size_t klen;   /* its length without the CR/LF */
  size_t vlen;   /* length of the "true"/"false" literal, 0 if not a boolean */
  int    val;
};

static char   alt_buf[8192];
static size_t alt_len;
static struct alt_line alt_lines[ALT_MAX_LINES];
static int    alt_nlines;

static int alt_slurp_cfgfile(void) {
  FILE *f = fopen(host_path(CFG_FILE), "rb");
  if (!f) { perror("config.yml"); return -1; }
  alt_len = fread(alt_buf, 1, sizeof alt_buf, f);
  fclose(f);
  if (alt_len == sizeof alt_buf) {
    fprintf(stderr, "cfg_cli: config.yml no longer fits alt_buf -- raise it\n");
    return -1;
  }
  return 0;
}

static void alt_split(void) {
  size_t i = 0;
  alt_nlines = 0;
  while (i < alt_len) {
    /* Truncating here would silently drop the tail of config.yml from the
       fixture -- the probe would still "succeed", just over fewer keys. */
    if (alt_nlines == ALT_MAX_LINES) {
      fprintf(stderr, "cfg_cli: config.yml has more than %d lines -- raise "
                      "ALT_MAX_LINES\n", ALT_MAX_LINES);
      exit(99);
    }
    struct alt_line *l = &alt_lines[alt_nlines];
    size_t s = i, k;
    while (i < alt_len && alt_buf[i] != '\n') i++;
    if (i < alt_len) i++;                       /* take the LF with the line */
    k = i - s;
    while (k && (alt_buf[s + k - 1] == '\n' || alt_buf[s + k - 1] == '\r')) k--;
    l->off = s; l->len = i - s; l->klen = k; l->vlen = 0; l->val = 0;
    if (k >= 6 && !memcmp(alt_buf + s + k - 6, ": true", 6)) { l->vlen = 4; l->val = 1; }
    else if (k >= 7 && !memcmp(alt_buf + s + k - 7, ": false", 7)) { l->vlen = 5; l->val = 0; }
    alt_nlines++;
  }
}

/* Write the slurped file back to the card, with the boolean on line `flip`
   inverted (-1 = verbatim).  CRLF, like everything cfg_save emits. */
static int alt_write(int flip) {
  const char *hp = host_path(CFG_FILE);
  FILE *f;
  int i;
  host_mkdir_for(hp);
  f = fopen(hp, "wb");
  if (!f) { perror(hp); return -1; }
  for (i = 0; i < alt_nlines; i++) {
    const char *p = alt_buf + alt_lines[i].off;
    if (i == flip) {
      fwrite(p, 1, alt_lines[i].klen - alt_lines[i].vlen, f);
      fputs(alt_lines[i].val ? "false" : "true", f);
      fputs("\r\n", f);
    } else {
      fwrite(p, 1, alt_lines[i].len, f);
    }
  }
  return fclose(f) == 0 ? 0 : -1;
}

static int cfg_make_alternating(enum alt_axis axis) {
  const uint8_t *dflt = (const uint8_t *)&CFG_DEFAULT;
  long off[ALT_MAX_LINES];
  int i, nbool = 0;

  memcpy(&CFG, &CFG_DEFAULT, sizeof(cfg_t));
  cfg_save();
  if (alt_slurp_cfgfile()) return 1;
  alt_split();

  for (i = 0; i < alt_nlines; i++) {
    size_t j;
    unsigned hits = 0;
    long where = -1;
    if (!alt_lines[i].vlen) continue;
    if (alt_write(i)) return 1;
    cfg_load();
    for (j = 0; j < sizeof(cfg_t); j++)
      if (((const uint8_t *)&CFG)[j] != dflt[j]) { hits++; where = (long)j; }
    if (hits != 1) {
      fprintf(stderr, "cfg_cli: flipping the boolean on line %d moved %u byte(s) of"
                      " cfg_t, expected exactly 1 -- a key whose CFGI line names the"
                      " wrong field, or a non-boolean whose value spells true/false\n",
              i, hits);
      return 1;
    }
    off[nbool++] = where;
  }
  if (!nbool) {
    fprintf(stderr, "cfg_cli: no boolean keys in config.yml -- the probe found nothing\n");
    return 1;
  }

  memcpy(&CFG, &CFG_DEFAULT, sizeof(cfg_t));
  for (i = 0; i < nbool; i++)
    ((uint8_t *)&CFG)[off[i]] =
        (uint8_t)(((axis == ALT_BY_INDEX ? (long)i : off[i])) & 1);
  return 0;
}

/* ---- modes -------------------------------------------------------------- */
static int mode_save_default(const char *out) {
  /* The fresh-card path verbatim: cfg_load() with no file on the card
     pre-loads CFG_DEFAULT, fails the open, and still runs the menu combo
     through cfg_check_menu_combo() on the way out. */
  remove(host_path(CFG_FILE));
  cfg_load();
  if (memcmp(&CFG, &CFG_DEFAULT, sizeof(cfg_t)) != 0) {
    fprintf(stderr, "cfg_cli: cfg_load() on a card with no config.yml did not land on"
                    " CFG_DEFAULT -- a default is being rejected by its own validator\n");
    return 1;
  }
  cfg_save();
  return copy_file(host_path(CFG_FILE), out) == 0 ? 0 : 1;
}

static int mode_save_allchanged(const char *out) {
  cfg_make_allchanged();
  cfg_save();
  return copy_file(host_path(CFG_FILE), out) == 0 ? 0 : 1;
}

static int mode_save_alternating(const char *out, enum alt_axis axis) {
  if (cfg_make_alternating(axis)) return 1;
  cfg_save();
  return copy_file(host_path(CFG_FILE), out) == 0 ? 0 : 1;
}

static int mode_load_dump(const char *in, const char *out) {
  FILE *f;
  if (copy_file(in, host_path(CFG_FILE)) != 0) return 1;
  cfg_load();
  f = fopen(out, "wb");
  if (!f) { perror(out); return 1; }
  if (fwrite(&CFG, 1, sizeof(cfg_t), f) != sizeof(cfg_t)) { perror(out); fclose(f); return 1; }
  return fclose(f) == 0 ? 0 : 1;
}

static int mode_roundtrip(const char *in, const char *out) {
  if (copy_file(in, host_path(CFG_FILE)) != 0) return 1;
  cfg_load();
  cfg_save();
  return copy_file(host_path(CFG_FILE), out) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  const char *mode;

  if (argc < 2) {
    fprintf(stderr, "usage: %s size|save-default <out.yml>|save-allchanged <out.yml>|"
                    "save-alternating <out.yml>|save-altoffset <out.yml>|"
                    "load-dump <in.yml> <out.bin>|roundtrip <in.yml> <out.yml>\n", argv[0]);
    return 2;
  }
  mode = argv[1];
  host_sram_init();

  if (!strcmp(mode, "size")) {
    printf("%zu\n", sizeof(cfg_t));
    return 0;
  }
  if (!strcmp(mode, "save-default") && argc == 3)    return mode_save_default(argv[2]);
  if (!strcmp(mode, "save-allchanged") && argc == 3) return mode_save_allchanged(argv[2]);
  if (!strcmp(mode, "save-alternating") && argc == 3)
    return mode_save_alternating(argv[2], ALT_BY_INDEX);
  if (!strcmp(mode, "save-altoffset") && argc == 3)
    return mode_save_alternating(argv[2], ALT_BY_OFFSET);
  if (!strcmp(mode, "load-dump") && argc == 4)       return mode_load_dump(argv[2], argv[3]);
  if (!strcmp(mode, "roundtrip") && argc == 4)       return mode_roundtrip(argv[2], argv[3]);

  fprintf(stderr, "%s: bad mode/arity\n", argv[0]);
  return 2;
}
