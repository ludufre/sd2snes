/* Conformance test for THE bucket rule, compiled against the REAL src/fileops.c.
 * The rule lives in three places (firmware C, the Manager's sd-layout.ts) and a drift means the
 * device looks in a different directory than the Manager wrote to -- i.e. saves "disappear".
 * This pins the C side; the Manager's spec uses the SAME table. */
#include <stdio.h>
#include <string.h>
void path_bucket2(const char *path, char *out);
int  path_asset(char *buf, int buflen, const char *root, const char *src, const char *ext);

static const char *CASES[][2] = {
  {"Super Mario World (USA).sfc","SU"}, {"/S/Super Mario World (USA).sfc","SU"},
  {"/sd2snes/info/S/Super Mario World.yml","SU"}, {"super mario world.sfc","SU"},
  {"sUpEr.sfc","SU"}, {"A.sfc","A_"}, {"A","A_"}, {"AB","AB"}, {"","__"}, {"/","__"},
  {"2020 Super Baseball (Japan).sfc","20"}, {"96 Zenkoku.sfc","96"},
  {" Leading space.sfc","_L"}, {"-Dash.sfc","_D"}, {"._Super Mario World.yml","__"},
  {".DS_Store","_D"}, {"'Tis a name.sfc","_T"}, {"[BIOS] Thing.sfc","_B"},
  {"(Proto) Thing.sfc","_P"}, {"Ys III.sfc","YS"}, {"F-Zero (USA).sfc","F_"},
  {"Pokemon.sfc","PO"}, {"Super Mario World (USA)01.state","SU"},
  {"Super Mario World (USA).02.man","SU"}, {"Super Mario World (USA).03.srm","SU"},
  {"1.sfc","1_"}, {"__weird__.sfc","__"},
  /* the bucket itself is namespace-agnostic -- a GB ROM buckets like any other, just inside sgb/ */
  {"Tetris.gb","TE"},
};

/* The Game Boy namespace. path_asset mirrors sgb.c:66-71 (extension STARTS WITH "gb"), which is
 * the only GB detection the firmware has. The .sgb rows sit next to the .gb ones on purpose: it
 * is the trap in this rule -- ".sgb" starts with 's', so the device loads it as a plain SNES ROM,
 * even though the Manager's SYSTEM_BY_EXT calls that extension's system 'SGB'. */
static const char *SGB_CASES[][2] = {
  {"Tetris.gb",   "/sd2snes/saves/sgb/TE/Tetris.srm"},
  {"Tetris.GB",   "/sd2snes/saves/sgb/TE/Tetris.srm"},
  {"Tetris.gbc",  "/sd2snes/saves/sgb/TE/Tetris.srm"},
  {"Tetris.GBC",  "/sd2snes/saves/sgb/TE/Tetris.srm"},
  {"Tetris.sgb",  "/sd2snes/saves/TE/Tetris.srm"},      /* NOT Game Boy -- see above */
  {"Tetris.SGB",  "/sd2snes/saves/TE/Tetris.srm"},
  {"Tetris.sfc",  "/sd2snes/saves/TE/Tetris.srm"},
  {"Tetris",      "/sd2snes/saves/TE/Tetris.srm"},      /* no dot at all */
  {"foo.gb.sfc",  "/sd2snes/saves/FO/foo.gb.srm"},      /* last extension wins (strrchr) */
  /* The Sufami Turbo namespace.  path_asset mirrors path_is_st (EXACT ".st"), which is
   * also what decides where the Slot B companion cart's own .srm is written -- both
   * slots name their save from their own cart path, through this one function. */
  {"Poi Poi.st",  "/sd2snes/saves/sft/PO/Poi Poi.srm"},
  {"Poi Poi.ST",  "/sd2snes/saves/sft/PO/Poi Poi.srm"},
  {"Poi Poi.stx", "/sd2snes/saves/PO/Poi Poi.srm"},     /* prefix is NOT enough, unlike gb */
  {"Poi Poi.s",   "/sd2snes/saves/PO/Poi Poi.srm"},
  {"Tetris.st",   "/sd2snes/saves/sft/TE/Tetris.srm"}, /* never collides with Tetris.sfc */
  /* Three letters on purpose: FAT is case-insensitive, so a "st/" namespace would BE
     the "ST" bucket -- the one that holds Star Ocean and the ST010 carts. */
  {"Star Ocean.sfc", "/sd2snes/saves/ST/Star Ocean.srm"},
  {"Star Ocean.st",  "/sd2snes/saves/sft/ST/Star Ocean.srm"},

  /* The mk3-only consoles.  Same reason as sgb/ and sft/: the stem drops the extension, so
   * without these "Tetris (USA).nes" and "Tetris (USA).sfc" would share one cover, one .yml
   * and one cheat file -- and cross-platform stems are the rule on these systems, not the
   * exception (Tetris, Double Dragon, Mega Man, Battletoads...). */
  {"Tetris (USA).nes",  "/sd2snes/saves/nes/TE/Tetris (USA).srm"},
  {"Tetris (USA).sfc",  "/sd2snes/saves/TE/Tetris (USA).srm"},
  {"Tetris (USA).NES",  "/sd2snes/saves/nes/TE/Tetris (USA).srm"},
  {"Sonic.sms",         "/sd2snes/saves/sms/SO/Sonic.srm"},
  {"Sonic.SMS",         "/sd2snes/saves/sms/SO/Sonic.srm"},
  {"Pitfall!.a26",      "/sd2snes/saves/a26/PI/Pitfall!.srm"},
  {"Pitfall!.A26",      "/sd2snes/saves/a26/PI/Pitfall!.srm"},
  /* EXACT extensions, and this is the pair that makes it non-negotiable: ".smc" is a SNES ROM
   * and must never be caught by the ".sms" rule. */
  {"Sonic.smc",         "/sd2snes/saves/SO/Sonic.srm"},
  {"Sonic.sm",          "/sd2snes/saves/SO/Sonic.srm"},
  {"Sonic.smsx",        "/sd2snes/saves/SO/Sonic.srm"},
  {"Doom.nesx",         "/sd2snes/saves/DO/Doom.srm"},
  {"Doom.ne",           "/sd2snes/saves/DO/Doom.srm"},
  {"Combat.a2",         "/sd2snes/saves/CO/Combat.srm"},
  {"Combat.a260",       "/sd2snes/saves/CO/Combat.srm"},
  /* Every namespace is three characters, so none of them can ever be a two-letter bucket. */
  {"Nesting.sfc",       "/sd2snes/saves/NE/Nesting.srm"},
  {"Smash.sfc",         "/sd2snes/saves/SM/Smash.srm"},
};
int main(void) {
  int bad = 0, i, n = (int)(sizeof(CASES)/sizeof(CASES[0]));
  char b[4];
  for(i = 0; i < n; i++) {
    path_bucket2(CASES[i][0], b);
    if(strcmp(b, CASES[i][1])) { printf("FAIL %-40s got %-3s want %s\n", CASES[i][0], b, CASES[i][1]); bad++; }
  }
  /* path_asset: full paths, extension handling, and the -1 truncation guard */
  { char p[256]; int off;
    off = path_asset(p, sizeof p, "/sd2snes/saves/", "/S/Super Mario World (USA).sfc", ".srm");
    if(strcmp(p, "/sd2snes/saves/SU/Super Mario World (USA).srm")) { printf("FAIL asset: %s\n", p); bad++; }
    if(strcmp(p + off, "Super Mario World (USA).srm")) { printf("FAIL stem_off: %s\n", p + off); bad++; }
    path_asset(p, sizeof p, "/sd2snes/info/", "Super Mario World (USA).sfc", "");
    if(strcmp(p, "/sd2snes/info/SU/Super Mario World (USA)")) { printf("FAIL bare: %s\n", p); bad++; }
    path_asset(p, sizeof p, "/sd2snes/states/", "A.sfc", "01.state");
    if(strcmp(p, "/sd2snes/states/A_/A01.state")) { printf("FAIL state: %s\n", p); bad++; }
    { char big[300]; memset(big, 'x', 290); big[290] = 0;
      if(path_asset(p, sizeof p, "/sd2snes/saves/", big, ".srm") != -1) { printf("FAIL: truncation not caught\n"); bad++; } }
    /* A stem sized to fit WITHOUT sgb/ and NOT with it, so this actually exercises the extra
       level's guard: 15 root + 3 bucket + 231 + 4 ext + NUL = 254 fits; +4 for "sgb/" does not.
       Truncating instead of failing here would make two games share one save file. */
    { char big[300]; memset(big, 'x', 231); strcpy(big + 231, ".sfc");
      if(path_asset(p, sizeof p, "/sd2snes/saves/", big, ".srm") == -1) { printf("FAIL: 231-char SNES stem should still fit\n"); bad++; }
      memcpy(big + 231, ".gb", 4);
      if(path_asset(p, sizeof p, "/sd2snes/saves/", big, ".srm") != -1) { printf("FAIL: sgb truncation not caught\n"); bad++; } }
  }
  /* the sgb/ namespace table */
  { char p[256]; int i, m = (int)(sizeof(SGB_CASES)/sizeof(SGB_CASES[0]));
    for(i = 0; i < m; i++) {
      path_asset(p, sizeof p, "/sd2snes/saves/", SGB_CASES[i][0], ".srm");
      if(strcmp(p, SGB_CASES[i][1])) { printf("FAIL sgb %-14s got %s want %s\n", SGB_CASES[i][0], p, SGB_CASES[i][1]); bad++; }
    }
    /* the bucket still applies inside sgb/, padding a one-char stem exactly as outside */
    path_asset(p, sizeof p, "/sd2snes/states/", "A.gb", "01.state");
    if(strcmp(p, "/sd2snes/states/sgb/A_/A01.state")) { printf("FAIL sgb pad: %s\n", p); bad++; }
  }
  n += (int)(sizeof(SGB_CASES)/sizeof(SGB_CASES[0]));   /* namespace rows count too */
  printf("%d/%d bucket cases OK%s\n", n - bad, n, bad ? " (WITH FAILURES)" : "");
  return bad ? 1 : 0;
}
