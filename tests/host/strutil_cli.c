/* Conformance + bounds test for src/strutil.c (strlcpy_nul, path_leaf).
 *
 * Two things are checked on every case:
 *   1) the RESULT matches an independent reference model (bytes written + the NUL + the
 *      strlen(src) return, the BSD contract callers may test for truncation);
 *   2) it writes NOTHING outside [dst, dst + dstsz). dst is an EXACT-SIZE heap allocation, so
 *      ASan traps a one-past-the-end store.
 * dstsz == 0 has to touch nothing at all: cfg_get_stringvalue() can be called with a zero cap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

static int fails;

static void fail(const char *what, const char *detail) {
  printf("  FAIL %s: %s\n", what, detail);
  fails++;
}

/* Reference model, written straight from the contract rather than from strutil.c. */
static size_t ref_strlcpy_nul(char *dst, const char *src, size_t dstsz) {
  size_t len = strlen(src);
  if(dstsz) {
    size_t n = len < dstsz - 1 ? len : dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
  }
  return len;
}

static void t_copy(const char *src, size_t dstsz) {
  char detail[256];
  /* exact-size allocations: ASan owns the bounds check on both sides */
  char *got = malloc(dstsz ? dstsz : 1);
  char *want = malloc(dstsz ? dstsz : 1);
  size_t rgot, rwant;

  memset(got, '#', dstsz);
  memset(want, '#', dstsz);
  rgot  = strlcpy_nul(got, src, dstsz);
  rwant = ref_strlcpy_nul(want, src, dstsz);

  if(rgot != rwant) {
    snprintf(detail, sizeof(detail), "src=\"%s\" dstsz=%zu return %zu, want %zu",
             src, dstsz, rgot, rwant);
    fail("return", detail);
  }
  if(dstsz && memcmp(got, want, dstsz)) {
    snprintf(detail, sizeof(detail), "src=\"%s\" dstsz=%zu -> \"%s\", want \"%s\"",
             src, dstsz, got, want);
    fail("buffer", detail);
  }
  /* a NUL always lands inside the buffer; the tail past it keeps the '#' filler, since
     strlcpy_nul does NOT zero-pad (the CFG sites that blit their struct to BSRAM need that). */
  if(dstsz && !memchr(got, 0, dstsz)) {
    snprintf(detail, sizeof(detail), "src=\"%s\" dstsz=%zu not terminated", src, dstsz);
    fail("nul", detail);
  }
  free(got);
  free(want);
}

static void t_leaf(const char *path, const char *want) {
  const char *got = path_leaf(path);
  char detail[256];
  if(strcmp(got, want)) {
    snprintf(detail, sizeof(detail), "\"%s\" -> \"%s\", want \"%s\"", path, got, want);
    fail("path_leaf", detail);
  }
  /* the result must point INTO the input, never at a copy: callers keep using it after that
     buffer is edited in place (cfg_parse_patch_entry cuts at the tab). */
  if(got < path || got > path + strlen(path)) fail("path_leaf", "result outside the input");
}

int main(void) {
  static const char *srcs[] = { "", "a", "ab", "abc", "abcd", "abcde",
                                "/sd2snes/saves/SU/Super Metroid (USA).srm" };
  size_t i, n;

  printf("strlcpy_nul: dstsz 0..8 x %zu sources\n", sizeof(srcs) / sizeof(srcs[0]));
  for(i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++)
    for(n = 0; n <= 8; n++) t_copy(srcs[i], n);

  /* exact fits on both sides of the boundary for a realistic path */
  {
    const char *p = srcs[6];
    size_t len = strlen(p);
    t_copy(p, len);        /* one short: truncates */
    t_copy(p, len + 1);    /* exact fit */
    t_copy(p, len + 2);    /* room to spare */
  }
  t_copy("", 1);
  t_copy("", 0);
  t_copy("x", 1);          /* only the NUL fits */

  printf("path_leaf\n");
  t_leaf("", "");
  t_leaf("a", "a");
  t_leaf("abc", "abc");
  t_leaf("/", "");
  t_leaf("/a", "a");
  t_leaf("a/b", "b");
  t_leaf("a/", "");
  t_leaf("/sd2snes/saves/SU/Super Metroid (USA).srm", "Super Metroid (USA).srm");
  t_leaf("/a/b/c", "c");
  t_leaf("no/slash/at/end/x", "x");
  t_leaf(".hidden", ".hidden");

  if(fails) { printf("!! %d failure(s)\n", fails); return 1; }
  printf("OK\n");
  return 0;
}
