#ifndef MODULE_H
#define MODULE_H

/* Card modules: menu features whose code lives on the card instead of in the firmware.
 *
 * A module is a stand-alone, position-independent image (/sd2snes/<name>.bin, built from
 * src/modules/) that the firmware loads into the directory-sort cache (ptrcache, 8 KB of
 * AHB SRAM only a READDIR uses) when the menu opens the feature, and that only lives while
 * the feature's screen is up: every menu command that is not one of the modules' own
 * (module_gate) unloads it first, which is what hands ptrcache back before anything sorts.
 * Its state lives in the scratch FRAME (scratch.h: a module is one more FRAME owner, like
 * gameinfo_load and a game load, and never runs inside them).
 *
 * A SESSION module (module_session) is the other kind: opened by load_rom for the game
 * being loaded, alive for the whole game, served from both game loops by module_idle, and
 * dropped by the next load_rom (the menu's included).  It fits because a running game
 * never sorts a directory nor uses the FRAME.  Its stop() must not touch the FPGA: the
 * core may already be another one.  It reaches the firmware only
 * through the module_api_t table below, and runs the same image on every board: anything
 * board-specific (SPI registers, RAM layout) is behind the table.
 *
 * Image layout: a module_hdr_t first; entry points are offsets from the image start (Thumb
 * bit set), linked at 0 and built -fpic.  No writable data in the image.  The firmware
 * checks magic, ABI, size, CRC and that the state fits the FRAME before calling anything.
 *
 * Bump MODULE_ABI on any change to this file (the table, the header, the entry points) or
 * to a structure a module shares with the firmware (FIL through ff.h's options, the status
 * blocks): the firmware refuses an image built for another ABI. */

#include <stdint.h>
#include "ff.h"

#define MODULE_MAGIC   0x444f4d53UL   /* "SMOD" */
#define MODULE_ABI     4

/* the modules, and the file each one is */
#define MODULE_ID_NSF      1          /* nsfplay.bin: the NSF player (SNES_CMD_MOD_OPEN) */
#define MODULE_ID_MEMTEST  2          /* memtest.bin: the RAM test (SNES_CMD_MEMTEST) */
#define MODULE_ID_PCM      3          /* pcmplay.bin: the .pcm player (SNES_CMD_PLAY_PCM) */
#define MODULE_ID_RTC7110  4          /* rtc7110.bin: the SPC7110 RTC battery (session) */
#define MODULE_ID_FILEOP   5          /* fileops.bin: the browser's file manager (SNES_CMD_FM) */
#define MODULE_ID_RMDIR    6          /* rmdir.bin: its "Delete folder" (FM_OP_RMDIR) */
#define MODULE_COUNT       6

/* What start() returns when the module could not be loaded at all */
#define MODULE_MISSING     (-1)

/* SRAM_MODULE_STATUS: what the last load did, for the menu to tell "no module file" from
   "a firmware without modules" */
#define MODULE_ST_OK       0
#define MODULE_ST_MISSING  1          /* the file is missing, damaged or of another ABI */

/* An endless music generator (music_play_gen): fills frames stereo s16 frames into pcm and
   returns how many.  The frames are mono, L = R: the firmware may keep L only. */
typedef unsigned (*module_gen_fn)(void *ctx, int16_t *pcm, unsigned frames);

/* The memory test's hot loops stay in the firmware: they are SPI bursts over all 16 MB,
   through board-specific registers.  memtest.h has the struct. */
struct memtest_cell;

typedef struct {
  /* PSRAM and the BSRAM window */
  uint16_t (*sram_read)(void *buf, uint32_t addr, uint16_t size);
  uint16_t (*sram_write)(void *buf, uint32_t addr, uint16_t size);
  void     (*sram_memset)(uint32_t addr, uint32_t len, uint8_t val);
  uint8_t  (*sram_readbyte)(uint32_t addr);
  void     (*sram_writebyte)(uint8_t val, uint32_t addr);
  uint16_t (*sram_readshort)(uint32_t addr);
  void     (*sram_writeshort)(uint16_t val, uint32_t addr);
  /* files, on the firmware's file_handle (free while a module runs) */
  FRESULT  (*f_open)(FIL *fp, const TCHAR *path, BYTE mode);
  FRESULT  (*f_read)(FIL *fp, void *buf, UINT len, UINT *br);
  FRESULT  (*f_close)(FIL *fp);
  int      (*psram_stream)(FIL *fp, uint32_t addr, uint32_t size, void (*pump)(void));
  FIL      *file;
  /* the menu music engine (msu1.c) */
  int      (*music_play_gen)(module_gen_fn gen, void *ctx, uint32_t ring);
  int      (*music_play)(const char *path);
  void     (*music_stop)(void);
  void     (*music_pause)(int paused);
  void     (*music_lock)(int locked);
  void     (*music_noloop)(int on);
  int      (*music_seek)(uint32_t pos);
  int      (*music_active)(void);
  int      (*music_ended)(void);
  uint32_t (*music_tell)(void);
  uint32_t (*music_size)(void);
  uint32_t (*music_played)(void);
  /* the memory test (memtest.c) */
  int      (*mt_core)(void);          /* fpga_test up: 1; not on the card: 0; failed: -1 */
  void     (*mt_select)(uint8_t unit);
  void     (*mt_pass)(struct memtest_cell *cc, uint32_t len, uint8_t verify);
  void     (*led)(unsigned int on);
  uint32_t (*ticks)(void);
  void     (*sfx_forget)(void);
  /* AHB SRAM nobody else uses (the NSF player's cache): *size bytes at the returned address */
  uint8_t *(*spare)(uint32_t *size);
  /* files on the card by game (fileops.c) and writing one */
  int      (*path_asset)(char *buf, int buflen, const char *root, const char *src, const char *ext);
  FRESULT  (*path_asset_mkdir)(char *path);
  FRESULT  (*f_write)(FIL *fp, const void *buf, UINT len, UINT *bw);
  /* the clocks: the console's own (packed BCD, get_bcdtime) and the core's (set_fpga_time),
     and the SPC7110 core's battery handover (FPGA_SPC7110_RTC_LEN bytes each way) */
  uint64_t (*bcdtime)(void);
  void     (*fpga_time)(uint64_t time);
  void     (*s7rtc_get)(uint8_t *state);
  void     (*s7rtc_set)(const uint8_t *state);
  /* the rest of the file system, for the file manager (any FIL / DIR the module owns) */
  FRESULT  (*f_lseek)(FIL *fp, DWORD ofs);
  FRESULT  (*f_unlink)(const TCHAR *path);
  FRESULT  (*f_rename)(const TCHAR *from, const TCHAR *to);
  FRESULT  (*f_mkdir)(const TCHAR *path);
  FRESULT  (*f_opendir)(DIR *dp, const TCHAR *path);
  FRESULT  (*f_readdir)(DIR *dp, FILINFO *fno);
  FRESULT  (*f_closedir)(DIR *dp);
  FRESULT  (*f_stat)(const TCHAR *path, FILINFO *fno);
  FRESULT  (*f_getfree)(const TCHAR *path, DWORD *nclst, FATFS **fs);
  /* one Unicode code point (>= 0x80) -> 1..3 menu font bytes ('?' when there is no glyph
     nor a stand-in); the Ficha's table (gameinfo.c), without its CJK glyphs */
  int      (*font_cp)(uint32_t cp, uint8_t *out);
  /* Recents / Favorites: drop dead entries and republish both lists to the menu, after
     the module rewrote lastgame.cfg / favorites.cfg */
  void     (*lists_refresh)(void);
  /* the module has work queued: the menu loop runs the next idle() right away instead of
     sleeping its 20 ms first (call it from idle(), every pass that leaves work) */
  void     (*hurry)(void);
  /* the board */
  uint32_t cpu_hz;
  const uint8_t *ram0_base;           /* the first cell of each PSRAM chip... */
  uint8_t  ram0_chips;                /* ...of this many */
  uint8_t  ram0_second;               /* stride between a chip's own cells */
  uint16_t mt_iterations;
} module_api_t;

typedef struct {
  uint32_t magic;
  uint16_t abi;
  uint16_t state_size;      /* bytes of scratch FRAME the module needs */
  uint32_t size;            /* the whole image, this header included */
  uint32_t crc;             /* CRC32 of the image with this field read as 0 */
  uint32_t start;           /* int start(void *st, const module_api_t *api, const char *path) */
  uint32_t ctl;             /* void ctl(void *st, uint32_t param): MCU_PARAM+0..3, LE */
  uint32_t idle;            /* void idle(void *st): once per menu-loop pass */
  uint32_t stop;            /* void stop(void *st): release everything (the DAC first) */
} module_hdr_t;

typedef struct {
  int  (*start)(void *st, const module_api_t *api, const char *path);
  void (*ctl)(void *st, uint32_t param);
  void (*idle)(void *st);
  void (*stop)(void *st);
} module_fns_t;

/* Module side: one MODULE_DEFINE(prefix, state type) per module, after prefix_start,
   prefix_ctl, prefix_idle and prefix_stop.  On the card it is the image header; in host
   builds of the firmware (the Ciclone), which link the modules in, the function table. */
#ifdef MODULE_HOST_LINKED
#define MODULE_DEFINE(p, st) \
  const module_fns_t p##_fns = { p##_start, p##_ctl, p##_idle, p##_stop };
#else
#define MODULE_DEFINE(p, st) \
  extern const uint8_t __mod_end[]; \
  __attribute__((section(".modhdr"), used)) const module_hdr_t p##_hdr = { \
    MODULE_MAGIC, MODULE_ABI, sizeof(st), (uint32_t)__mod_end, 0, \
    (uint32_t)p##_start, (uint32_t)p##_ctl, (uint32_t)p##_idle, (uint32_t)p##_stop };
#endif

/* Firmware side (module.c) */
int  module_open(uint8_t id, const char *path);   /* start()'s answer, or MODULE_MISSING */
int  module_session(uint8_t id, const char *path);/* the same, for a module that lives with the game */
void module_drop(void);                           /* stop and forget whatever is open */
void module_ctl(uint32_t param);
int  module_live(void);                           /* a module is started (its code may be running) */
void module_gate(uint8_t cmd);                    /* before every menu command */
int  module_idle(void);                           /* once per menu- or game-loop pass; 1 = the
                                                     module asked to be called again at once */

#endif
