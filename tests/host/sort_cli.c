/* Conformance test for the browser sort order (src/sort.c), compiled against the REAL source.
 *
 * The directory table is laid out exactly as scan_dir writes it -- a 4-byte pointer table at
 * SRAM_DIR_ADDR ([string offset | type << 24]) and, per entry, a 6-byte size string followed
 * by the NUL-terminated leaf name -- in a fake PSRAM, then sort_dir() runs over it.
 *
 * Checked:
 *   1) a fixed MSU-1 folder lands in the exact expected order: ".." first, folders, every
 *      non-track file alphabetically, then the .pcm tracks in NUMERIC order;
 *   2) the same folder with hidden extensions (the '.' stored as byte 0x01);
 *   3) randomized tables are non-decreasing under an independent reference comparator.
 * run_sort.sh builds this twice, with QSORT_MAXELEM large and tiny, so both the qsort path
 * and the in-place heapsort path are held to the same order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "memory.h"
#include "filetypes.h"
#include "sort.h"

static uint8_t *psram;
static int fails;

/* The FPGA takes a 24-bit address, so the type byte a directory pointer carries in its top
   8 bits never reaches the bus: sort_getstring_for_dirent adds the whole pointer to the
   window base and relies on exactly that. Every accessor masks the same way. */
#define A24(a) ((a) & 0xffffff)

uint8_t sram_readbyte(uint32_t a) {
  return psram[A24(a)];
}

uint32_t sram_readlong(uint32_t a) {
  a = A24(a);
  return psram[a] | psram[a+1] << 8 | psram[a+2] << 16 | (uint32_t)psram[a+3] << 24;
}

void sram_writelong(uint32_t v, uint32_t a) {
  a = A24(a);
  psram[a] = v; psram[a+1] = v >> 8; psram[a+2] = v >> 16; psram[a+3] = v >> 24;
}

uint16_t sram_readblock(void *buf, uint32_t a, uint16_t n) {
  memcpy(buf, psram + A24(a), n);
  return n;
}

uint16_t sram_writeblock(void *buf, uint32_t a, uint16_t n) {
  memcpy(psram + A24(a), buf, n);
  return n;
}

/* Same contract as the firmware: stops after a NUL, and a run that fills the buffer gets
   its last byte overwritten with the terminator. */
uint16_t sram_readstrn(void *buf, uint32_t a, uint16_t n) {
  uint8_t *t = buf;
  uint16_t count = 0;
  while(n--) {
    if(!(*t++ = psram[A24(a++)])) break;
    count++;
  }
  t--;
  if(*t) *t = 0;
  return count;
}

typedef struct {
  const char *name;
  uint8_t type;
} ent_t;

/* scan_dir's layout, including the hidden-extension byte and the '/' on folder names. */
static unsigned build_table(const ent_t *e, unsigned n, int hide_ext) {
  uint32_t str = SRAM_DIR_ADDR + 0x10000;
  for(unsigned i = 0; i < n; i++) {
    char name[300];
    snprintf(name, sizeof(name), "%s", e[i].name);
    if(hide_ext && !(e[i].type & (TYPE_SUBDIR | TYPE_PARENT))) {
      char *dot = strrchr(name, '.');
      if(dot) *dot = 1;
    }
    size_t len = strlen(name);
    memcpy(psram + str, " 1234k", 6);
    memcpy(psram + str + 6, name, len + 1);
    sram_writelong((str - SRAM_MENU_ADDR) | (uint32_t)e[i].type << 24, SRAM_DIR_ADDR + 4 * i);
    str += len + 7;
  }
  sram_writelong(0, SRAM_DIR_ADDR + 4 * n);
  return n;
}

static const char *entry_name(unsigned i) {
  uint32_t p = sram_readlong(SRAM_DIR_ADDR + 4 * i);
  return (const char*)psram + SRAM_MENU_ADDR + (p & 0xffffff) + 6;
}

static uint8_t entry_type(unsigned i) {
  return sram_readlong(SRAM_DIR_ADDR + 4 * i) >> 24;
}

static void t_fixed(int hide_ext) {
  /* deliberately scrambled, the way FAT hands a copied MSU-1 pack back */
  static const ent_t in[] = {
    { "Game-10.pcm",  TYPE_PCM    },
    { "Game.sfc",     TYPE_ROM    },
    { "Zeta/",        TYPE_SUBDIR },
    { "Game-2.pcm",   TYPE_PCM    },
    { "Theme.thm",    TYPE_SKIN   },
    { "Game-100.pcm", TYPE_PCM    },
    { "../",          TYPE_PARENT },
    { "game-3.pcm",   TYPE_PCM    },
    { "Another.spc",  TYPE_SPC    },
    { "Game-1.pcm",   TYPE_PCM    },
    { "alpha/",       TYPE_SUBDIR },
    { "Game-010.pcm", TYPE_PCM    },
    { "Mario.nes",    TYPE_NES    },
    { "Game-9.pcm",   TYPE_PCM    },
    { "Boot.sfc",     TYPE_ROM    },
  };
  static const char *want[] = {
    "../", "alpha/", "Zeta/",
    "Another.spc", "Boot.sfc", "Game.sfc", "Mario.nes", "Theme.thm",
    "Game-1.pcm", "Game-2.pcm", "game-3.pcm", "Game-9.pcm", "Game-010.pcm", "Game-10.pcm",
    "Game-100.pcm",
  };
  unsigned n = build_table(in, sizeof(in) / sizeof(in[0]), hide_ext);
  sort_dir(SRAM_DIR_ADDR, n);
  for(unsigned i = 0; i < n; i++) {
    char got[300];
    snprintf(got, sizeof(got), "%s", entry_name(i));
    char *dot = strchr(got, 1);
    if(dot) *dot = '.';
    if(strcmp(got, want[i])) {
      printf("  FAIL fixed%s: position %u is \"%s\", want \"%s\"\n",
             hide_ext ? " (hidden ext)" : "", i, got, want[i]);
      fails++;
    }
  }
}

/* ---- independent reference order ---------------------------------------------------- */

static int ref_rank(uint8_t type) {
  if(type & TYPE_PARENT) return 0;
  if(type & TYPE_SUBDIR) return 1;
  return type == TYPE_PCM ? 3 : 2;
}

/* digit runs by value (strtoul; the generator keeps runs short), everything else by
   lowercase byte, and the plain case-insensitive compare breaks value ties */
static int ref_nat(const char *a, const char *b) {
  const char *pa = a, *pb = b;
  while(*pa || *pb) {
    if(*pa >= '0' && *pa <= '9' && *pb >= '0' && *pb <= '9') {
      char *ea, *eb;
      unsigned long va = strtoul(pa, &ea, 10), vb = strtoul(pb, &eb, 10);
      if(va != vb) return va < vb ? -1 : 1;
      pa = ea;
      pb = eb;
      continue;
    }
    int ca = (unsigned char)*pa, cb = (unsigned char)*pb;
    if(ca >= 'A' && ca <= 'Z') ca += 32;
    if(cb >= 'A' && cb <= 'Z') cb += 32;
    if(ca != cb) return ca < cb ? -1 : 1;
    pa++;
    pb++;
  }
  return strcasecmp(a, b);
}

static int ref_cmp(unsigned i, unsigned j) {
  int ri = ref_rank(entry_type(i)), rj = ref_rank(entry_type(j));
  if(ri != rj) return ri < rj ? -1 : 1;
  char a[300], b[300];
  snprintf(a, sizeof(a), "%s", entry_name(i));
  snprintf(b, sizeof(b), "%s", entry_name(j));
  if(ri == 1) {
    a[strlen(a) - 1] = 0;
    b[strlen(b) - 1] = 0;
  }
  return ri == 3 ? ref_nat(a, b) : strcasecmp(a, b);
}

static void t_random(unsigned seed, unsigned n) {
  static const uint8_t types[] = { TYPE_SUBDIR, TYPE_ROM, TYPE_SPC, TYPE_PCM, TYPE_NES, TYPE_SKIN };
  static const char alphabet[] = "aAbB-_ 0123456789";
  ent_t *e = calloc(n, sizeof(*e));
  char **names = calloc(n, sizeof(*names));
  srand(seed);
  for(unsigned i = 0; i < n; i++) {
    char base[40];
    unsigned len = 1 + rand() % 12;
    for(unsigned k = 0; k < len; k++) base[k] = alphabet[rand() % (sizeof(alphabet) - 1)];
    base[len] = 0;
    uint8_t type = i == 0 ? TYPE_PARENT : types[rand() % sizeof(types)];
    names[i] = malloc(64);
    if(type == TYPE_PARENT)      snprintf(names[i], 64, "../");
    else if(type == TYPE_SUBDIR) snprintf(names[i], 64, "%s~%u/", base, i);   /* unique, as FAT is */
    else                         snprintf(names[i], 64, "%s~%u.ext", base, i);
    e[i].name = names[i];
    e[i].type = type;
  }
  /* shuffle so ".." is not already first */
  for(unsigned i = n - 1; i > 0; i--) {
    unsigned j = rand() % (i + 1);
    ent_t t = e[i]; e[i] = e[j]; e[j] = t;
  }
  build_table(e, n, 0);
  sort_dir(SRAM_DIR_ADDR, n);
  for(unsigned i = 1; i < n; i++) {
    if(ref_cmp(i - 1, i) > 0) {
      printf("  FAIL random seed=%u n=%u: \"%s\" (type %u) sorted before \"%s\" (type %u)\n",
             seed, n, entry_name(i - 1), entry_type(i - 1), entry_name(i), entry_type(i));
      fails++;
      break;
    }
  }
  for(unsigned i = 0; i < n; i++) free(names[i]);
  free(names);
  free(e);
}

int main(void) {
  psram = calloc(1, 0x1000000);
  if(!psram) return 2;
  t_fixed(0);
  t_fixed(1);
  for(unsigned seed = 1; seed <= 40; seed++) t_random(seed, 2 + seed * 7);
  printf("sort (QSORT_MAXELEM=%d): %s\n", QSORT_MAXELEM, fails ? "FAIL" : "ok");
  free(psram);
  return fails ? 1 : 0;
}
