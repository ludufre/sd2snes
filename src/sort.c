
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "uart.h"
#include "memory.h"
#include "filetypes.h"
#include "sort.h"

/*
   heap sort algorithm for data located outside RAM
   addr:     start address of pointer table
   i:        index (in 32-bit elements)
   heapsize: size of heap (in 32-bit elements)
*/

uint32_t stat_getstring = 0;
static char sort_str1[SORT_STRLEN+1], sort_str2[SORT_STRLEN+1];
uint32_t ptrcache[QSORT_MAXELEM] IN_AHBRAM;

/* get element from pointer table in external RAM*/
uint32_t sort_get_elem(uint32_t base, unsigned int index) {
  return sram_readlong(base+4*index);
}

/* put element from pointer table in external RAM */
void sort_put_elem(uint32_t base, unsigned int index, uint32_t elem) {
  sram_writelong(elem, base+4*index);
}

/* compare strings pointed to by elements of pointer table */
int sort_cmp_idx(uint32_t base, unsigned int index1, unsigned int index2) {
  uint32_t elem1, elem2;
  elem1 = sort_get_elem(base, index1);
  elem2 = sort_get_elem(base, index2);
  return sort_cmp_elem((void*)&elem1, (void*)&elem2);
}

/* Case-insensitive compare in which a run of digits compares by numeric value, so an
   MSU-1 folder lists track -2 before track -10. Runs of equal value (-01 vs -1) fall
   back to a plain compare to keep the order total. No <ctype.h>: this only has to know
   ASCII digits and letters, and the table would cost flash. */
static int sort_natcasecmp(const char *s1, const char *s2) {
  const unsigned char *a = (const unsigned char*)s1, *b = (const unsigned char*)s2;
  for(;;) {
    unsigned ca = *a, cb = *b;
    if(ca - '0' < 10u && cb - '0' < 10u) {
      while(*a == '0') a++;
      while(*b == '0') b++;
      const unsigned char *da = a, *db = b;
      while(*a - '0' < 10u) a++;
      while(*b - '0' < 10u) b++;
      if(a - da != b - db) return a - da < b - db ? -1 : 1;
      int r = memcmp(da, db, a - da);
      if(r) return r;
      continue;
    }
    if(ca - 'A' < 26u) ca += 'a' - 'A';
    if(cb - 'A' < 26u) cb += 'a' - 'A';
    if(ca != cb) return ca < cb ? -1 : 1;
    if(!ca) return strcasecmp(s1, s2);
    a++;
    b++;
  }
}

int sort_cmp_elem(const void* elem1, const void* elem2) {
  uint32_t el1 = *(uint32_t*)elem1;
  uint32_t el2 = *(uint32_t*)elem2;
  /* Order by type before reading any name: each name is a 256-byte PSRAM read over SPI,
     wasted on every pair the type alone already decides. */
  /* parent dir is always the first entry */
  if (el1 & 0x80000000) return -1;
  if (el2 & 0x80000000) return 1;

  int dir1 = (el1 & 0x40000000) != 0;
  int dir2 = (el2 & 0x40000000) != 0;
  if (dir1 != dir2) return dir1 ? -1 : 1;

  /* MSU-1 audio tracks go after every other file, so a game folder shows its ROM first
     instead of burying it among dozens of <stem>-N.pcm entries. */
  int pcm1 = (el1 >> 24) == TYPE_PCM;
  int pcm2 = (el2 >> 24) == TYPE_PCM;
  if (pcm1 != pcm2) return pcm1 ? 1 : -1;

  sort_getstring_for_dirent(sort_str1, el1);
  sort_getstring_for_dirent(sort_str2, el2);

  if (*sort_str1 == '.') return -1;
  if (*sort_str2 == '.') return 1;

  /* Do not compare trailing slashes of directory names */
  if (dir1) {
    char *str1_slash = strrchr(sort_str1, '/');
    char *str2_slash = strrchr(sort_str2, '/');
    if(str1_slash != NULL) *str1_slash = 0;
    if(str2_slash != NULL) *str2_slash = 0;
  }

  if (pcm1) return sort_natcasecmp(sort_str1, sort_str2);
  return strcasecmp(sort_str1, sort_str2);
}

/* get truncated string from database */
void sort_getstring_for_dirent(char *ptr, uint32_t addr) {
  sram_readstrn(ptr, addr + SRAM_MENU_ADDR + 6, SORT_STRLEN);
}

/* get truncated string from database */
void sort_getstring_for_dirent_old(char *ptr, uint32_t addr) {
  uint8_t leaf_offset;
  if(addr & 0xc0000000) {
    /* is directory link, name offset 4 */
    leaf_offset = sram_readbyte(addr + 4 + SRAM_MENU_ADDR);
    sram_readstrn(ptr, addr + 5 + leaf_offset + SRAM_MENU_ADDR, SORT_STRLEN);
  } else {
    /* is file link, name offset 6 */
    leaf_offset = sram_readbyte(addr + 6 + SRAM_MENU_ADDR);
    sram_readstrn(ptr, addr + 7 + leaf_offset + SRAM_MENU_ADDR, SORT_STRLEN);
  }
}

void sort_heapify(uint32_t addr, unsigned int i, unsigned int heapsize)
{
  while(1) {
    unsigned int l = 2*i+1;
    unsigned int r = 2*i+2;
    unsigned int largest = (l < heapsize && sort_cmp_idx(addr, i, l) < 0) ? l : i;

    if(r < heapsize && sort_cmp_idx(addr, largest, r) < 0)
      largest = r;

    if(largest != i) {
      uint32_t tmp = sort_get_elem(addr, i);
      sort_put_elem(addr, i, sort_get_elem(addr, largest));
      sort_put_elem(addr, largest, tmp);
      i = largest;
    }
    else break;
  }
}

void sort_dir(uint32_t addr, unsigned int size)
{
stat_getstring=0;
  if(size > QSORT_MAXELEM) {
    printf("more than %d dir entries, doing slower in-place sort\n", QSORT_MAXELEM);
    ext_heapsort(addr, size);
  } else {
    /* retrieve, sort, and store dir table */
    sram_readblock(ptrcache, addr, size*4);
    qsort((void*)ptrcache, size, 4, sort_cmp_elem);
    sram_writeblock(ptrcache, addr, size*4);
  }
}

void ext_heapsort(uint32_t addr, unsigned int size) {
  for(unsigned int i = size/2; i > 0;) sort_heapify(addr, --i, size);

  for(unsigned int i = size-1; i>0; --i) {
    uint32_t tmp = sort_get_elem(addr, 0);
    sort_put_elem(addr, 0, sort_get_elem(addr, i));
    sort_put_elem(addr, i, tmp);
    sort_heapify(addr, 0, i);
  }
}

