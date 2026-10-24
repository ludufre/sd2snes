/* Card overlays -- see ovl.h */
#include "config.h"
#include "ff.h"
#include "crc32.h"
#include "module.h"
#include "ovl.h"

#ifdef CONFIG_MK3_STM32
#define OVL_SFX   "stm"
#elif defined(CONFIG_MK2)
#define OVL_SFX   "img"
#else
#define OVL_SFX   "im3"
#endif
/* the end of the module block (module.c MOD_END); a host test passes its own */
#ifndef OVL_END
#ifdef CONFIG_MK3_STM32
extern uint8_t __ahbram_end__[];
#define OVL_END   ((uintptr_t)__ahbram_end__)
#else
#define OVL_END   ((uintptr_t)0x20080000UL)
#endif
#endif

_Static_assert(sizeof(FIL) + 8 <= OVL_FIL_RESERVE, "ovl: the loader's FIL outgrew OVL_FIL_RESERVE");
#define OVL_FIL   ((FIL *)((OVL_END - sizeof(FIL)) & ~(uintptr_t)7))

/* genhdr writes the image CRC at fwhdr + 12 after the link: read it from flash, not
   from what the compiler saw (a zero-filled const) */
extern const char fwhdr[];
#define OVL_BUILD (*(volatile const uint32_t *)(void *)(fwhdr + 12))

static uint8_t ovl_cur;
/* why each overlay's file was last refused (0 = not refused): kept until ovl_recheck (menu
   load), so a missing or stale file is read once per menu load, not at every veneer call */
static uint8_t ovl_fail[OVL_COUNT];
uint8_t ovl_depth;
uint8_t ovl_err;
const char ovl1_file[] = "/sd2snes/ovl1." OVL_SFX;
const char ovl2_file[] = "/sd2snes/ovl2." OVL_SFX;

#ifdef OVL_TIMING
/* Load timing probe (measurement build, make OVL_TIMING=1): DWT cycles of the last load from
   the card, published at SRAM_OVL_TIMING_ADDR by ovl_timing_tick (module_idle, i.e. every pass
   of the menu loop and of both game loops) for USB.  Words: [0] f_open + f_read, [1] the CRC,
   [2] the whole load (ovl_ensure entry to its answer), [3] bits 0..15 loads read from the card,
   16..23 of them refused (missing, short, CRC, magic, build), 24..31 the id last asked for. */
#include "memory.h"
#define DEMCR    (*(volatile uint32_t *)0xe000edfcUL)
#define DWT_CTRL (*(volatile uint32_t *)0xe0001000UL)
#define DWT_CYC  (*(volatile uint32_t *)0xe0001004UL)
static uint32_t ovl_tm[4];
static uint8_t ovl_tm_dirty;
static int ovl_tm_end(int ok, uint32_t t0)
{
  ovl_tm[2] = DWT_CYC - t0;
  if(!ok) ovl_tm[3] += 1UL << 16;
  ovl_tm_dirty = 1;
  return ok;
}
#define TM_START()   uint32_t t0, t1; DEMCR |= 1UL << 24; DWT_CTRL |= 1; t0 = DWT_CYC; \
                     ovl_tm[3] = ((ovl_tm[3] + 1) & 0x00ffffffUL) | ((uint32_t)id << 24)
#define TM_RET(ok)   return ovl_tm_end((ok), t0)
#define TM_MARK(k)   do { t1 = DWT_CYC; ovl_tm[k] = t1 - t0; } while(0)
#define TM_SINCE(k)  do { ovl_tm[k] = DWT_CYC - t1; } while(0)

void ovl_timing_tick(void)
{
  if(!ovl_tm_dirty) return;
  ovl_tm_dirty = 0;
  sram_writeblock(ovl_tm, SRAM_OVL_TIMING_ADDR, sizeof(ovl_tm));
}
#else
#define TM_START()   do {} while(0)
#define TM_RET(ok)   return (ok)
#define TM_MARK(k)   do {} while(0)
#define TM_SINCE(k)  do {} while(0)
#endif

void ovl_forget(void)
{
  ovl_cur = 0;
}

void ovl_recheck(void)
{
  ovl_cur = 0;
  for(uint8_t i = 0; i < OVL_COUNT; i++) ovl_fail[i] = 0;
}

/* a refusal from the card: remembered with its reason (ovl_err) */
#define OVL_REFUSE() do { ovl_fail[id - 1] = ovl_err; TM_RET(0); } while(0)

/* noinline: the build finds every way into an overlay by the calls to this (ovlreach.py) */
int __attribute__((noinline)) ovl_ensure(uint8_t id)
{
  static const char *const file[OVL_COUNT] = { ovl1_file, ovl2_file };
  uint8_t *img = (uint8_t *)ptrcache;
  const ovl_hdr_t *h = (const ovl_hdr_t *)img;
  uint32_t crc = crc32_init(), size, i;
  UINT br = 0;
  if(!id || id > OVL_COUNT) return 0;   /* (0 is also ovl_cur's "nothing loaded") */
  if(ovl_cur == id) return 1;
  /* overlay code is running (it would be overwritten), or a module owns the block (a
     session module in a game, or this call comes from inside a module) */
  if(ovl_depth || module_live()) return 0;
  if(ovl_fail[id - 1]) {            /* refused since the menu load: not read again */
    ovl_err = ovl_fail[id - 1];
    return 0;
  }
  TM_START();
  module_gate(0);                   /* forget the module image: it is about to be overwritten */
  ovl_cur = 0;
  ovl_err = OVL_ERR_MISSING;
  if(f_open(OVL_FIL, file[id - 1], FA_READ) != FR_OK) OVL_REFUSE();
  ovl_err = OVL_ERR_REFUSED;
  size = f_size(OVL_FIL);
  if(size < sizeof(*h) || size > (uintptr_t)OVL_FIL - (uintptr_t)img
     || f_read(OVL_FIL, img, size, &br) != FR_OK || br != size) {
    f_close(OVL_FIL);
    OVL_REFUSE();
  }
  f_close(OVL_FIL);
  TM_MARK(0);
  for(i = 0; i < size; i++)
    crc = crc32_update(crc, (i >= 12 && i < 16) ? 0 : img[i]);   /* the crc field reads as 0 */
  TM_SINCE(1);
  if(h->magic != OVL_MAGIC(id) || h->build != OVL_BUILD || h->size != size
     || crc32_finalize(crc) != h->crc)
    OVL_REFUSE();
  ovl_cur = id;
  ovl_err = 0;
  TM_RET(1);
}
