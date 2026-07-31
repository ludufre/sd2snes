/* Conformance test for the patch-selector MATCH rule, compiled against the REAL
 * patch_belongs_to_rom() in src/patch.c.
 *
 * This decides which .ips/.bps files a ROM is offered on the patch screen.  The
 * subtle rule -- and the reason this test exists -- is the last one: a patch whose
 * name is the ROM stem plus nothing but an extension is REFUSED, because that is
 * precisely what "create patched ROM" leaves behind.  The export writes
 * <patch stem>.sfc, so the patch that produced a ROM always ends up sharing that
 * ROM's stem; offering it again would re-apply an IPS over an already-patched
 * image, which corrupts it silently (IPS carries no checksum or size).
 *
 * The cut has to be at the LAST dot, not the first: get that wrong and
 * "Foo.v2.ips" quietly stops being offered.  That is a one-character mistake with
 * no visible symptom, which is exactly what a pinned table is for.
 */
#include <stdio.h>
#include <string.h>

#define PATCH_TYPE_IPS 0
#define PATCH_TYPE_BPS 1

int patch_belongs_to_rom(const char *fn, const char *romfile, unsigned stem_len);

/* {ROM basename, patch basename, expected: "ips" / "bps" / "no"} */
static const char *CASES[][3] = {
  /* ---- the ordinary shapes: a suffix after the ROM stem ---- */
  {"Foo.sfc",  "Foo - [BR].ips",  "ips"},
  {"Foo.sfc",  "Foo_PTBR.ips",    "ips"},
  {"Foo.sfc",  "Foo hardtype.bps","bps"},
  {"Foo.sfc",  "FOO - BR.IPS",    "ips"},   /* matching is case-insensitive */

  /* ---- THE RULE: stem + extension only is refused ---- */
  {"Foo.sfc",  "Foo.ips",         "no"},
  {"Foo.sfc",  "Foo.bps",         "no"},

  /* ---- ...but the cut is at the LAST dot, so an inner dot still counts ---- */
  {"Foo.sfc",  "Foo.v2.ips",      "ips"},
  {"Foo.sfc",  "Foo..ips",        "ips"},   /* stem "Foo." (4) != "Foo" (3) */

  /* ---- the case that motivated all of this ---------------------------------
     One patch file, two ROMs.  It belongs to the ORIGINAL and must keep being
     offered there; it must NOT be offered on the ROM it produced. */
  {"Chrono Trigger (USA).sfc",
   "Chrono Trigger (USA) - [BR].ips", "ips"},
  {"Chrono Trigger (USA) - [BR].sfc",
   "Chrono Trigger (USA) - [BR].ips", "no"},

  /* ---- prefix must match ---- */
  {"Foo.sfc",  "Bar - [BR].ips",  "no"},
  {"Foo.sfc",  "Fo - [BR].ips",   "no"},

  /* ---- extension must be exactly three chars ---- */
  {"Foo.sfc",  "Foo - BR.txt",    "no"},
  {"Foo.sfc",  "Foo - BR.ipsx",   "no"},
  {"Foo.sfc",  "Foo - BR.ip",     "no"},
  {"Foo.sfc",  "Foo - BR",        "no"},

  /* ---- ROM stems that themselves contain dots and spaces ---- */
  {"Rom.v2.sfc",   "Rom.v2 - BR.ips", "ips"},
  {"Rom.v2.sfc",   "Rom.v2.ips",      "no"},
  {"Game (U) [!].smc", "Game (U) [!] - fix.bps", "bps"},
};

/* Same derivation patch_scan_dir uses: everything before the LAST dot. */
static unsigned stem_of(const char *romfile) {
  const char *last_dot = NULL, *p;
  for(p = romfile; *p; p++) if(*p == '.') last_dot = p;
  return last_dot ? (unsigned)(last_dot - romfile) : (unsigned)strlen(romfile);
}

int main(void) {
  int n = (int)(sizeof(CASES) / sizeof(CASES[0])), bad = 0, i;

  for(i = 0; i < n; i++) {
    int got = patch_belongs_to_rom(CASES[i][1], CASES[i][0], stem_of(CASES[i][0]));
    const char *name = got == PATCH_TYPE_IPS ? "ips"
                     : got == PATCH_TYPE_BPS ? "bps" : "no";
    if(strcmp(name, CASES[i][2])) {
      printf("FAIL %-34s on %-32s got %s want %s\n",
             CASES[i][1], CASES[i][0], name, CASES[i][2]);
      bad++;
    }
  }

  /* A ROM with no stem at all must never claim a patch. */
  if(patch_belongs_to_rom("x.ips", ".sfc", 0) != -1) {
    printf("FAIL empty ROM stem\n");
    bad++;
  }
  n++;

  printf("%d/%d patch-match cases OK%s\n", n - bad, n, bad ? " (WITH FAILURES)" : "");
  return bad ? 1 : 0;
}
