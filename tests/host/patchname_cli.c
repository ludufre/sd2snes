/* Conformance test for the patch-selector display name, compiled against the
 * REAL patch_display_name() in src/patch.c.
 *
 * The rule: show what is left of the patch's basename AFTER the ROM stem, with
 * leading separators stripped, so a folder full of "<ROM>_PT-BR.ips" reads as a
 * list of suffixes instead of the ROM name repeated N times.  When nothing is
 * left (the patch is exactly "<ROM stem>.ips") fall back to the whole
 * extension-less basename rather than an empty row.
 *
 * This is pure string code with no I/O, which is exactly why it is worth pinning
 * here: it decides what the user reads on the one screen where they choose
 * between patches that may differ by a single character. */
#include <stdio.h>
#include <string.h>

int patch_display_name(char *out, int outlen, const char *patch_basename,
                       unsigned stem_len);

/* {ROM stem, patch basename, expected display name} */
static const char *CASES[][3] = {
  /* the ordinary shape: strip the stem and the separator that follows it */
  {"Zelda",              "Zelda_PT-BR.ips",          "PT-BR"},
  {"Zelda",              "Zelda-PT-BR.ips",          "PT-BR"},
  {"Zelda",              "Zelda PT-BR.ips",          "PT-BR"},
  {"Zelda",              "Zelda.PT-BR.ips",          "PT-BR"},
  {"Zelda",              "Zelda (v1.2).ips",         "v1.2)"},
  /* only LEADING separators go; the ones inside the suffix are the user's */
  {"Zelda",              "Zelda - Hack (v1.2).bps",  "Hack (v1.2)"},
  {"Zelda",              "Zelda___msu1.ips",         "msu1"},
  /* patch named exactly like the ROM -> no suffix -> show the whole basename */
  {"Zelda",              "Zelda.ips",                "Zelda"},
  {"Zelda",              "Zelda.bps",                "Zelda"},
  /* a suffix made entirely of separators is not a name either */
  {"Zelda",              "Zelda__.ips",              "Zelda__"},
  /* real-world stems with spaces and parentheses */
  {"Super Mario World (USA)", "Super Mario World (USA)_hardtype.ips", "hardtype"},
  {"Super Mario World (USA)", "Super Mario World (USA).ips",          "Super Mario World (USA)"},
  /* multiple dots: only the LAST one is the extension */
  {"Rom.v2",             "Rom.v2_fix.ips",           "fix"},
  /* no extension at all: nothing to strip, the whole tail is the suffix */
  {"Zelda",              "Zelda_raw",                "raw"},
};

int main(void) {
  int n = (int)(sizeof(CASES) / sizeof(CASES[0])), bad = 0, i;
  char out[42];   /* IPS_NAME_BADGE: the real display field width */

  for(i = 0; i < n; i++) {
    int len = patch_display_name(out, sizeof out, CASES[i][1],
                                 (unsigned)strlen(CASES[i][0]));
    if(strcmp(out, CASES[i][2])) {
      printf("FAIL %-38s got \"%s\" want \"%s\"\n", CASES[i][1], out, CASES[i][2]);
      bad++;
    } else if(len != (int)strlen(out)) {
      printf("FAIL %-38s returned %d, wrote %d chars\n", CASES[i][1], len, (int)strlen(out));
      bad++;
    }
  }

  /* A name longer than the display field must be truncated, never overrun: the
     caller stages exactly len+1 bytes into a 42-byte SRAM slot. */
  {
    char big[200];
    memset(big, 'x', 190);
    strcpy(big + 190, ".ips");
    int len = patch_display_name(out, sizeof out, big, 0);
    if(len != (int)sizeof(out) - 1 || out[sizeof(out) - 1] != 0) {
      printf("FAIL long name: len %d, terminator %d\n", len, out[sizeof(out) - 1]);
      bad++;
    }
    n++;
  }
  /* Degenerate buffers must not write past the end. */
  {
    char tiny[1] = { 'Z' };
    if(patch_display_name(tiny, 1, "Zelda_x.ips", 5) != 0 || tiny[0] != 0) {
      printf("FAIL 1-byte buffer\n");
      bad++;
    }
    n++;
  }

  printf("%d/%d patch-name cases OK%s\n", n - bad, n, bad ? " (WITH FAILURES)" : "");
  return bad ? 1 : 0;
}
