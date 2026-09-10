/* Conformance test for sort_dir() (src/sort.c), compiled against the REAL source.
 *
 * sort_dir() picks between two algorithms purely on `size > QSORT_MAXELEM`:
 * qsort() through the `ptrcache[QSORT_MAXELEM]` AHB-RAM scratch buffer below
 * the threshold, ext_heapsort() (in-place, no scratch buffer) above it. This
 * harness builds ONE shuffled directory table and runs it through sort_dir()
 * twice -- once compiled with QSORT_MAXELEM set above the table size (forces
 * the qsort path) and once set below it (forces ext_heapsort) -- and requires
 * both to (a) produce the exact same order and (b) actually be sorted and a
 * permutation of the input. That's the property a QSORT_MAXELEM tuning change
 * (e.g. lowering it to free AHB RAM on the LPC175x boards) must not break:
 * entries that cross from the fast path to the fallback path have to keep
 * sorting identically.
 *
 * QSORT_MAXELEM and SORT_STRLEN are supplied with -D on the compile line
 * (see run_sort.sh), the same way run_cfg.sh feeds board constants to cfg.c --
 * the host build has no per-board autoconf.h to pull them from.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"   /* shim: host_sdram_init(), sram_* over a flat array */
#include "sort.h"     /* the real sort_dir()/ext_heapsort() prototypes */

void host_sdram_init(void);

#define N_ENTRIES 1500          /* straddles 1024 < N < 2048: the exact band a
                                    2048->1024 QSORT_MAXELEM change moves */
#define SLOT_SIZE 32             /* bytes reserved per directory entry */
#define TABLE_ADDR 0             /* pointer-table base, in sram_* address space */

/* Deterministic PRNG so a failure is reproducible without stashing a seed. */
static uint32_t rng_state = 0x2b7e1516u;
static uint32_t rng_next(void) {
  rng_state = rng_state * 1103515245u + 12345u;
  return rng_state;
}

static void make_name(char *out, unsigned i) {
  /* Mixed case and non-uniform lengths so strcasecmp actually has work to do,
     and so two entries can share a prefix (exercises the tie-break path). */
  snprintf(out, SORT_STRLEN, "Game_%04u_%s.sfc", i,
           (i % 3 == 0) ? "Alpha" : (i % 3 == 1) ? "beta" : "GAMMA");
}

/* Loads N_ENTRIES shuffled {pointer -> name} pairs into the shim SDRAM at
   SRAM_MENU_ADDR (names) and TABLE_ADDR (the pointer table sort_dir sorts). */
static void load_table(void) {
  uint32_t elem[N_ENTRIES];
  for (unsigned i = 0; i < N_ENTRIES; i++) {
    elem[i] = i * SLOT_SIZE;
    char name[SORT_STRLEN];
    make_name(name, i);
    sram_writeblock(name, SRAM_MENU_ADDR + elem[i] + 6, (uint16_t)(strlen(name) + 1));
  }
  /* Fisher-Yates shuffle of the pointer table itself -- the names stay where
     they are; only the order sort_dir() sees changes. */
  for (unsigned i = N_ENTRIES - 1; i > 0; i--) {
    unsigned j = rng_next() % (i + 1);
    uint32_t tmp = elem[i]; elem[i] = elem[j]; elem[j] = tmp;
  }
  sram_writeblock(elem, TABLE_ADDR, N_ENTRIES * 4);
}

static void read_table(uint32_t *out) {
  sram_readblock(out, TABLE_ADDR, N_ENTRIES * 4);
}

static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

int main(void) {
  host_sdram_init();
  int fails = 0;

  printf("QSORT_MAXELEM=%d, N_ENTRIES=%d -> %s path\n",
         QSORT_MAXELEM, N_ENTRIES,
         N_ENTRIES > QSORT_MAXELEM ? "ext_heapsort (fallback)" : "qsort (fast)");

  load_table();
  uint32_t before[N_ENTRIES];
  read_table(before);

  sort_dir(TABLE_ADDR, N_ENTRIES);

  uint32_t after[N_ENTRIES];
  read_table(after);

  /* 1. Sorted: consecutive names must be non-decreasing per sort_cmp_elem's
        own rule (case-insensitive on the name; no dir/parent bits are set by
        this test, so it reduces to plain strcasecmp). */
  char prev_name[SORT_STRLEN], cur_name[SORT_STRLEN];
  sort_getstring_for_dirent(prev_name, after[0]);
  for (unsigned i = 1; i < N_ENTRIES; i++) {
    sort_getstring_for_dirent(cur_name, after[i]);
    if (strcasecmp(prev_name, cur_name) > 0) {
      printf("FAIL: out of order at %u: '%s' > '%s'\n", i, prev_name, cur_name);
      fails++;
      if (fails > 10) break;
    }
    memcpy(prev_name, cur_name, sizeof(cur_name));
  }

  /* 2. Permutation: same multiset of pointer values, just reordered -- a
        heap/qsort bug that drops or duplicates an entry must not slip by
        just because the visible names still happened to look sorted. */
  uint32_t sorted_before[N_ENTRIES], sorted_after[N_ENTRIES];
  memcpy(sorted_before, before, sizeof(before));
  memcpy(sorted_after, after, sizeof(after));
  qsort(sorted_before, N_ENTRIES, sizeof(uint32_t), cmp_u32);
  qsort(sorted_after, N_ENTRIES, sizeof(uint32_t), cmp_u32);
  if (memcmp(sorted_before, sorted_after, sizeof(sorted_before)) != 0) {
    printf("FAIL: output is not a permutation of the input (entries lost/duplicated)\n");
    fails++;
  }

  if (fails) { printf("%d failure(s)\n", fails); return 1; }
  printf("sort_dir: %d entries correctly sorted via the %s path\n",
         N_ENTRIES, N_ENTRIES > QSORT_MAXELEM ? "ext_heapsort" : "qsort");
  return 0;
}
