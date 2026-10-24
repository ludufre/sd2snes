#ifndef OVL_H
#define OVL_H
/* Card overlays: menu code that lives on the card (/sd2snes/ovl<N>.img|im3|stm) and is linked
   against this very firmware build -- fixed load address (ptrcache, the module block), direct
   calls into the firmware, no function table.

   The firmware keeps one resident veneer per entry point (ovl_stub.c): it loads the overlay
   if needed and calls the entry, or returns what the function already answered on failure
   (theme baked, no cover, no sidecar...), so a missing or stale file degrades, never hangs.

   An image is only valid for the build that produced it: its header carries the firmware
   image CRC that genhdr wrote into the firmware header (fwhdr + 12), and the loader refuses
   any other.  The rules the mechanism needs (enforced by the build, see ovl/ovlreach.py):
   - an overlay never calls another overlay, nor anything that reuses ptrcache (READDIR
     sort, module loader, the ZX0 window): the running code would be overwritten;
   - nothing reachable from game_cmd_serve (the in-game command server) is overlaid: in a
     game the block may hold a session module (rtc7110) that must not be dropped;
   and at run time ovl_ensure refuses while overlay code is running with another image
   wanted (ovl_depth) and while a module is live (a session module in a game, or a call
   made from inside a module's own code). */
#include <stdint.h>

/* "SOV1", "SOV2"...: an image renamed to another overlay's file is refused */
#define OVL_MAGIC(id) (0x30564f53UL + ((uint32_t)(id) << 24))

#define OVL_ID_MENU1   1           /* theme, sysinfo, cover, spcload, giload */
#define OVL_ID_MENU2   2           /* patchui, patchmeta, patchexp, savesop, bootleg */
#define OVL_COUNT      2

/* the loader's FIL sits at the end of the block (an overlay can be loaded while
   file_handle holds a ROM in load_rom); the build checks image + scratch against it */
#define OVL_FIL_RESERVE  576

/* each overlay's .bss slice inside the resident ovl_bss (ovl_stub.c), in bytes, overlay 2's
   right after overlay 1's; the Makefile reads these two lines (OVL_RULES) */
#define OVL1_BSS  48
#define OVL2_BSS  16

typedef struct {
  uint32_t magic;
  uint32_t build;           /* the firmware image CRC (fwhdr + 12) it was linked against */
  uint32_t size;            /* the whole image, header included */
  uint32_t crc;             /* CRC32 of the image with this field read as 0 */
  uint32_t entry[];         /* absolute Thumb addresses */
} ovl_hdr_t;

/* why the last load that reached the card failed (0 after a good load); the values are the
   menu's popup codes (MENU_ERR_SUPPLFILE / MENU_ERR_OVL in snes.h, checked in main.c) */
#define OVL_ERR_MISSING  2   /* no file */
#define OVL_ERR_REFUSED  7   /* present, but short, too big, corrupt or of another build */
extern uint8_t ovl_err;
extern const char ovl1_file[];     /* "/sd2snes/ovl1.<img|im3|stm>" */
extern const char ovl2_file[];     /* "/sd2snes/ovl2.<img|im3|stm>" */
extern uint8_t ovl_depth;          /* veneers running overlay code (nesting) */
int  ovl_ensure(uint8_t id);       /* 1 = loaded and valid; a refused file is not read
                                      again (ovl_err = the same reason) until ovl_recheck */
void ovl_forget(void);             /* ptrcache is about to be reused (sort, module, ZX0) */
void ovl_recheck(void);            /* menu load: forget the image and the refusals, re-read */
#ifdef OVL_TIMING
void ovl_timing_tick(void);        /* publish the load timing probe (ovl.c), once per loop pass */
#else
#define ovl_timing_tick() do {} while(0)
#endif
extern uint32_t ptrcache[];
#define OVL_HDR  ((const ovl_hdr_t *)(void *)ptrcache)
#endif
