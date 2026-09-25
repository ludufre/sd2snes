/* gbc_c6.h -- the C6 half of the bridge (wire $03, contract sec. 14) as the
 * offline harness plays it: the row machine, the dirty compare, the CL vote,
 * the live status bytes +58..+91 and the $E4 view.  See gbc_c6.c. */
#ifndef GBC_C6_H
#define GBC_C6_H
#include <stdint.h>

/* Turns the model on.  `scene` is the per-Game-Boy-frame script (see
 * gbc_c6.c), `stat` the STAT view's address in the harness build, `e4` the
 * 64 KB the m65816 serves as bank $E4, `a_fb` the player's FB state block
 * (GbcHarnessMap), `vram_ok_*` the VRAM byte range no DMA may touch. */
void c6_setup(const char *scene, uint32_t stat, uint8_t *e4, uint32_t a_fb);
int  c6_enabled(void);
/* The driver's LY=0 scheduler, run before the model catches up on a read or a
 * store (a LY=0 a fraction of a line before them). */
void c6_set_catchup(void (*fn)(uint64_t now));
/* The clock: every event of the bridge up to `now` (master cycles). */
void c6_advance(uint64_t now);
/* A Game Boy frame's LY=0 at master cycle `t` (its events are scheduled). */
void c6_ly0(uint64_t t);
/* A snapshot the bridge took (not skipped): COLW_N and the scene's flags0
 * bits are published with it.  Returns the flags0 to store. */
uint8_t c6_snapshot(uint8_t flags0);
/* $EF0006 / $EF0007. */
void c6_ef(uint32_t off, uint8_t v);
/* Every general DMA (after it ran). */
void c6_dma(uint8_t bbad, uint32_t src24, uint32_t bytes, uint32_t dest, uint64_t s, uint64_t e);
/* Line start (V) -- the picture sample at V=41, the SNES frame count at V=0. */
void c6_line(int v);
/* Every instruction (CPU cost of the drain), NULL-safe. */
void c6_pc(uint32_t pc24, int depth, uint16_t sp, uint64_t now);
void c6_set_syms(uint32_t a_drain, uint32_t a_row);
void c6_set_tables(uint32_t a_inidisp, uint32_t a_w1);
void c6_set_tables2(uint32_t a_inidisp16, uint32_t a_vofs);
int  c6_check_regs(void);
unsigned c6_redundant(void);
int c6_ever_stretched(void);
/* End of run: prints the report, writes the dump, returns failures. */
int  c6_finish(const char *dump_path, int report);
/* VRAM byte ranges a DMA may never write (default: the solid tiles). */
void c6_forbid(uint32_t lo, uint32_t hi);

#endif
