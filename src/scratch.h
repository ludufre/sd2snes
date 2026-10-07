/* sd2snes - shared scratch for the main loop's large per-call buffers.
 *
 * Several subsystems kept a private `static` buffer of a few hundred bytes only to keep
 * it off the tight LPC175x stack.  Each one sat in RAM for the whole session although it
 * is live only while its own function runs, and they never run at the same time: the
 * firmware is a single-threaded main loop and none of these buffers is touched from an
 * IRQ.  They now share two regions in AHB SRAM:
 *
 *   FRAME  the working set of ONE top-level operation.  Two owners, which are separate
 *          menu commands and so can never overlap:
 *            - gameinfo_load (gameinfo.c): the meta struct + the info path buffers;
 *            - a game load (load_rom): the .sms / .a26 ROM path, written by sms_id /
 *              a26_id and read back by sms_load_rom / a26_load_rom later in the same load.
 *          Nothing that runs inside either owner may use the FRAME.
 *
 *   LEAF   a self-contained helper that does not call another LEAF user: load_cover,
 *          gi_cov_to_gcv, gi_value_scan, the manual stagers, igmenu_stage, a26_id,
 *          scan_dir (the path of the MSU-1 folder probe).
 *          A LEAF user may run inside a FRAME owner (gameinfo_load calls three of them).
 *          Every LEAF user takes the region with scratch_leaf_take() and drops it on the
 *          way out.  A second taker is REFUSED and fails the way its own error path
 *          already does (no cover, no text, no guide page, default A26 scheme, inert
 *          in-game menu): a future nested call degrades one feature and says so in the
 *          log instead of silently corrupting another one's buffer.
 *
 * Both regions are .ahbram, which is NOLOAD: every user writes before it reads, exactly
 * as the private buffers they replace already had to.  Each user overlays a struct and
 * pins its size with SCRATCH_FITS, so growing a user past its region fails the build.
 */
#ifndef SCRATCH_H
#define SCRATCH_H

#include <stdint.h>

#define SCRATCH_FRAME_BYTES  1044
#define SCRATCH_LEAF_BYTES   564

/* words, not bytes: the overlaid structs need 4-byte alignment on every build */
extern uint32_t scratch_frame[SCRATCH_FRAME_BYTES / 4];
extern uint32_t scratch_leaf[SCRATCH_LEAF_BYTES / 4];

#define SCRATCH_FRAME(type)  ((type *)(void *)scratch_frame)
#define SCRATCH_LEAF(type)   ((type *)(void *)scratch_leaf)
#define SCRATCH_FITS(type, bytes) \
  _Static_assert(sizeof(type) <= (bytes), #type " outgrew its scratch region")

/* LEAF owners, for the log line of a refused take */
enum {
  SCR_COVER = 1,
  SCR_GI_COV,
  SCR_GI_SCAN,
  SCR_MANUAL,
  SCR_IGMENU,
  SCR_A26,
  SCR_GI_CJK
};

/* 1 = taken; 0 = someone else holds it (logged), caller takes its failure path */
int  scratch_leaf_take(uint8_t who);
void scratch_leaf_drop(void);

#endif
