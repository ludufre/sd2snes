/* Conformance test for yaml_open_write()/yaml_put_quoted() (src/yamlw.c) and
 * the escaping half of src/yaml.c, compiled against the REAL sources.
 *
 * These write the per-ROM cheat and patch-metadata sidecars.  The values are
 * filenames and hand-typed labels, so a stray '"' ends the scalar early and a
 * stray '&' becomes whatever entity name follows it on the next load -- and
 * the next save rewrites the file from what was parsed.  What matters is that
 * the parser gives the ORIGINAL string back, so the cases end in a real round
 * trip through yaml_get_itemvalue + yaml_decode_entities rather than in a
 * comparison against an expected spelling.
 *
 * The FatFs surface is modelled here (paths re-rooted under build/, '\n' ->
 * CRLF on the way out and '\r' dropped on the way in, as _USE_STRFUNC == 2
 * does on the card), plus FPGA_DESELECT and path_asset_mkdir.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "ff.h"
#include "fileops.h"
#include "yaml.h"
#include "yamlw.h"

/* ---- virtual SD card ---------------------------------------------------- */
#define SD_BASE "build/yamlw"

static const char *host_path(const char *p) {
  static char bufs[4][512];
  static int slot;
  char *b = bufs[slot];
  slot = (slot + 1) & 3;
  snprintf(b, sizeof bufs[0], "%s%s%s", SD_BASE, (p[0] == '/') ? "" : "/", p);
  return b;
}

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

/* Counters + injectable failures: the housekeeping yaml_open_write does
   before the first byte is observable rather than assumed. */
static unsigned n_deselect, n_mkdir, n_chmod, n_unlink, n_truncate;
static int      open_write_fails;   /* FA_CREATE_ALWAYS and FA_OPEN_ALWAYS both fail */
static int      create_always_fails; /* only FA_CREATE_ALWAYS fails -> truncate fallback */
static int      null_fp_write;      /* set if anything tried to write to a NULL FIL */

void host_fpga_deselect(void) { n_deselect++; }
/* Declared by shim/fpga_spi.h for the patch harness; unused here. */
void host_fpga_select(void) { }
void host_fpga_tx(uint8_t b) { (void)b; }
uint8_t host_fpga_rx(void) { return 0; }

FRESULT path_asset_mkdir(char *path) {
  n_mkdir++;
  host_mkdir_for(host_path(path));
  return FR_OK;
}

FRESULT f_chmod(const TCHAR *path, BYTE value, BYTE mask) {
  (void)path; (void)value; (void)mask;
  n_chmod++;
  return FR_OK;
}

FRESULT f_unlink(const TCHAR *path) {
  n_unlink++;
  remove(host_path(path));
  return FR_OK;
}

FRESULT f_truncate(FIL *fp) {
  n_truncate++;
  if (!fp || !fp->fp) return FR_INT_ERR;
  if (ftruncate(fileno(fp->fp), (off_t)fp->fptr) != 0) return FR_DISK_ERR;
  fp->fsize = fp->fptr;
  return FR_OK;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode) {
  const char *hp = host_path(path);
  const char *fmode;
  memset(fp, 0, sizeof(*fp));
  if (open_write_fails) return FR_DENIED;
  if (create_always_fails && (mode & FA_CREATE_ALWAYS)) return FR_DENIED;
  fmode = (mode & (FA_WRITE | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS))
          ? ((mode & FA_CREATE_ALWAYS) ? "wb" : "r+b") : "rb";
  if (fmode[0] != 'r' || fmode[1] == '+') host_mkdir_for(hp);
  fp->fp = fopen(hp, fmode);
  if (!fp->fp && fmode[0] == 'r' && fmode[1] == '+') fp->fp = fopen(hp, "w+b");
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
  if (!fp || !fp->fp) { if (bw) *bw = 0; return FR_INT_ERR; }
  put = (UINT)fwrite(buff, 1, btw, fp->fp);
  fp->fptr += put;
  if (fp->fptr > fp->fsize) fp->fsize = fp->fptr;
  if (bw) *bw = put;
  return (put == btw) ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, DWORD ofs) {
  if (!fp->fp) return FR_INT_ERR;
  if (fseek(fp->fp, (long)ofs, SEEK_SET) != 0) return FR_DISK_ERR;
  fp->fptr = ofs;
  return FR_OK;
}

FRESULT f_stat(const TCHAR *path, FILINFO *fno) {
  struct stat st;
  if (stat(host_path(path), &st) != 0) return FR_NO_FILE;
  if (fno) memset(fno, 0, sizeof(*fno));
  return FR_OK;
}

FRESULT f_opendir(DIR *dp, const TCHAR *path) { (void)dp; (void)path; return FR_NO_PATH; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; fno->fname[0] = 0; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

void file_open(const uint8_t *filename, BYTE flags) {
  file_res = f_open(&file_handle, (const TCHAR *)filename, flags);
  file_status = file_res ? FILE_ERR : FILE_OK;
}

void file_close(void) { file_res = f_close(&file_handle); }

/* _USE_STRFUNC == 2: LF becomes CRLF on the way out, CR is dropped on the way
   in and f_gets stops on LF or on a NUL. */
static int host_putc_raw(FIL *fp, TCHAR c) {
  UINT bw;
  BYTE b = (BYTE)c;
  if (f_write(fp, &b, 1, &bw) != FR_OK || bw != 1) return EOF;
  return 1;
}

int f_putc(TCHAR c, FIL *fp) {
  int n = 0;
  if (!fp) { null_fp_write = 1; return EOF; }
  if (c == '\n') {
    if (host_putc_raw(fp, '\r') == EOF) return EOF;
    n++;
  }
  if (host_putc_raw(fp, c) == EOF) return EOF;
  return n + 1;
}

int f_puts(const TCHAR *str, FIL *fp) {
  int n = 0, r;
  if (!fp) { null_fp_write = 1; return EOF; }
  while (*str) {
    r = f_putc(*str++, fp);
    if (r == EOF) return EOF;
    n += r;
  }
  return n;
}

int f_printf(FIL *fp, const TCHAR *fmt, ...) {
  /* yamlw.c never calls it; the round-trip fixture writes the "  Header:"
     line by hand instead of pulling patchmeta.c in. */
  (void)fp; (void)fmt;
  fprintf(stderr, "yamlw_cli: f_printf is not modelled\n");
  exit(99);
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
    if (c == '\r') continue;
    *p++ = c;
    n++;
    if (c == '\n') break;
    if (c == 0) break;
  }
  *p = 0;
  return n ? buff : 0;
}

/* ---- harness ------------------------------------------------------------ */
#define YML "/sd2snes/patches/AA/probe.yml"

static int fails;

static void fail(const char *name, const char *fmt, ...) {
  va_list ap;
  printf("FAIL  %s: ", name);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  fails++;
}

static void pass(const char *name) { printf("PASS  %s\n", name); }

static void counters_reset(void) {
  n_deselect = n_mkdir = n_chmod = n_unlink = n_truncate = 0;
  open_write_fails = create_always_fails = 0;
  null_fp_write = 0;
  file_res = FR_OK;
}

/* Read the whole sidecar back as raw bytes (CRLF included). */
static size_t slurp(char *out, size_t cap) {
  FILE *f = fopen(host_path(YML), "rb");
  size_t n;
  if (!f) return 0;
  n = fread(out, 1, cap - 1, f);
  out[n] = 0;
  fclose(f);
  return n;
}

static void expect_file(const char *name, const char *want) {
  char got[1024];
  slurp(got, sizeof got);
  if (!strcmp(got, want)) { pass(name); return; }
  fail(name, "file body\n        got  |%s|\n        want |%s|", got, want);
}

/* Write one sidecar holding a single item whose Patch: and Name: both carry s. */
static void write_one(const char *s) {
  char path[256];
  strcpy(path, YML);
  if (yaml_open_write(path)) {
    fail("write_one", "yaml_open_write failed (%d)", file_res);
    return;
  }
  yaml_put_quoted(&file_handle, "- Patch: \"", s);
  yaml_put_quoted(&file_handle, "  Name: \"", s);
  f_puts("  Header: \"auto\"\n", &file_handle);
  file_close();
}

/* Read it back the way patchmeta_apply does. */
static int read_one(char *patch_out, char *name_out, size_t cap) {
  yaml_token_t tok;
  int ok = 0;
  char path[256];
  strcpy(path, YML);
  patch_out[0] = name_out[0] = 0;
  yaml_file_open(path, FA_READ);
  if (file_res) return 0;
  while (yaml_next_item()) {
    if (!yaml_get_itemvalue("Patch", &tok)) continue;
    yaml_decode_entities(tok.stringvalue);
    strncpy(patch_out, tok.stringvalue, cap - 1);
    patch_out[cap - 1] = 0;
    if (yaml_get_itemvalue("Name", &tok)) {
      yaml_decode_entities(tok.stringvalue);
      strncpy(name_out, tok.stringvalue, cap - 1);
      name_out[cap - 1] = 0;
    }
    ok = 1;
    break;
  }
  yaml_file_close();
  return ok;
}

static void roundtrip(const char *label, const char *s) {
  char got_patch[512], got_name[512];
  counters_reset();
  write_one(s);
  if (!read_one(got_patch, got_name, sizeof got_patch)) {
    fail(label, "the parser found no item at all");
    return;
  }
  if (strcmp(got_patch, s)) {
    fail(label, "Patch came back as |%s|, wrote |%s|", got_patch, s);
    return;
  }
  if (strcmp(got_name, s)) {
    fail(label, "Name came back as |%s|, wrote |%s|", got_name, s);
    return;
  }
  pass(label);
}

int main(void) {
  char path[256];

  mkdir("build", 0777);
  mkdir(SD_BASE, 0777);

  /* --- yaml_puts_escaped: the two characters that MUST be escaped, and the
     three deliberately left alone (yaml_decode_entities takes either form for
     those). ------------------------------------------------------------- */
  counters_reset();
  strcpy(path, YML);
  if (yaml_open_write(path)) { fail("open-write", "failed (%d)", file_res); return 1; }
  yaml_put_quoted(&file_handle, "K: \"", "a\"b&c<d>e'f");
  file_close();
  expect_file("escape-set",
              "---\r\n# Generated by sd2snes\r\nK: \"a&quot;b&amp;c<d>e'f\"\r\n");

  /* Input that ALREADY looks like an entity: the '&' has to be escaped too,
     or the next load decodes a quote the user typed as six literal chars. */
  counters_reset();
  strcpy(path, YML);
  if (yaml_open_write(path)) { fail("open-write", "failed (%d)", file_res); return 1; }
  yaml_put_quoted(&file_handle, "K: \"", "&quot;x&quot;");
  file_close();
  expect_file("escape-existing-entity",
              "---\r\n# Generated by sd2snes\r\nK: \"&amp;quot;x&amp;quot;\"\r\n");

  /* Empty scalar: prefix, closing quote, newline -- nothing between. */
  counters_reset();
  strcpy(path, YML);
  if (yaml_open_write(path)) { fail("open-write", "failed (%d)", file_res); return 1; }
  yaml_put_quoted(&file_handle, "K: \"", "");
  file_close();
  expect_file("empty-value", "---\r\n# Generated by sd2snes\r\nK: \"\"\r\n");

  /* THE PREFIX CONTRACT: yaml_put_quoted writes the CLOSING quote and nothing
     else, so the prefix has to carry the opening one; run_yamlw.sh greps the
     call sites to keep it that way.  This pins what a prefix without it
     produces. */
  counters_reset();
  strcpy(path, YML);
  if (yaml_open_write(path)) { fail("open-write", "failed (%d)", file_res); return 1; }
  yaml_put_quoted(&file_handle, "K: ", "abc");
  file_close();
  expect_file("prefix-without-open-quote",
              "---\r\n# Generated by sd2snes\r\nK: abc\"\r\n");

  /* --- the NULL guards ------------------------------------------------- */
  /* yaml_puts_escaped guards BOTH arguments; yaml_put_quoted has no guard at
     all -- its own f_puts calls would go straight to a NULL FIL, so a caller
     may not hand it one.  Hence the guard is exercised through
     yaml_puts_escaped. */
  counters_reset();
  strcpy(path, YML);
  if (yaml_open_write(path)) { fail("open-write", "failed (%d)", file_res); return 1; }
  yaml_puts_escaped(&file_handle, NULL);          /* must write nothing */
  yaml_puts_escaped(NULL, "should not be written"); /* must not touch any file */
  file_close();
  if (null_fp_write) fail("null-guards", "yaml_puts_escaped wrote through a NULL FIL");
  else expect_file("null-guards", "---\r\n# Generated by sd2snes\r\n");

  /* --- yaml_open_write housekeeping ------------------------------------ */
  /* Release the FPGA chip select, create the bucket directory (only now that
     the name is built), clear the attributes, unlink, then open truncating. */
  counters_reset();
  strcpy(path, YML);
  if (yaml_open_write(path)) fail("open-write-housekeeping", "failed (%d)", file_res);
  file_close();
  if (n_deselect != 1) fail("open-write-housekeeping", "FPGA_DESELECT called %u times, want 1", n_deselect);
  else if (n_mkdir != 1) fail("open-write-housekeeping", "path_asset_mkdir called %u times, want 1", n_mkdir);
  else if (n_chmod != 1) fail("open-write-housekeeping", "f_chmod called %u times, want 1", n_chmod);
  else if (n_unlink != 1) fail("open-write-housekeeping", "f_unlink called %u times, want 1", n_unlink);
  else if (n_truncate != 0) fail("open-write-housekeeping", "took the truncate fallback on a healthy volume");
  else pass("open-write-housekeeping");

  /* A volume that refuses truncate-on-open falls back to open-always +
     f_truncate, and the header still comes out exactly once. */
  counters_reset();
  create_always_fails = 1;
  strcpy(path, YML);
  if (yaml_open_write(path)) fail("open-write-truncate-fallback", "failed (%d)", file_res);
  file_close();
  create_always_fails = 0;
  if (n_truncate != 1) fail("open-write-truncate-fallback", "f_truncate called %u times, want 1", n_truncate);
  else expect_file("open-write-truncate-fallback", "---\r\n# Generated by sd2snes\r\n");

  /* Nothing opens: the caller MUST see non-zero and write nothing. */
  counters_reset();
  open_write_fails = 1;
  strcpy(path, YML);
  if (!yaml_open_write(path)) fail("open-write-failure", "returned 0 with no file open");
  else pass("open-write-failure");
  open_write_fails = 0;

  /* --- round trips: whatever went in comes back out of the real parser, byte
   * for byte, after yaml_decode_entities -- the way cheat.c and patchmeta.c
   * read these files. --- */
  roundtrip("rt-plain",        "Chrono Trigger (USA) - [BR]");
  roundtrip("rt-quote",        "say \"hello\"");
  roundtrip("rt-amp",          "Rock & Roll");
  roundtrip("rt-entity-text",  "&quot;literal&quot; &amp; more");
  roundtrip("rt-angles",       "<tag> 'single' \"double\" & <end>");
  roundtrip("rt-empty",        "");
  roundtrip("rt-only-quote",   "\"");
  roundtrip("rt-only-amp",     "&");
  roundtrip("rt-trailing-amp", "ends with &");

  if (fails) {
    printf("== summary: %d failure(s) ==\n", fails);
    return 1;
  }
  printf("== summary: all yamlw cases pass ==\n");
  return 0;
}
