/* Card modules -- see module.h. */

#include "config.h"
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "msu1.h"
#include "crc32.h"
#include "psram_io.h"
#include "scratch.h"
#include "snes.h"
#include "fpga_spi.h"
#include "timer.h"
#include "led.h"
#include "memtest.h"
#include "rtc.h"
#include "module.h"
#include "gameinfo.h"
#include "menucmd.h"
#include "ovl.h"

/* sort.c: the READDIR sort cache, idle while a module's screen is open (module_gate) */
extern uint32_t ptrcache[QSORT_MAXELEM];

#ifdef MODULE_HOST_LINKED
/* the host build links the modules in: no image, the spare is plain .bss */
static uint32_t mod_spare_buf[3072 / 4];
static uint8_t *module_spare(uint32_t *size)
{
  *size = sizeof(mod_spare_buf);
  return (uint8_t *)mod_spare_buf;
}
#else
/* A module's image is loaded at ptrcache, the last thing in the AHB-side RAM (IN_AHBMOD),
   and what follows it up to MOD_END is free: an image may run past ptrcache into it, and
   the rest is the module's spare.  LPC175x: the unused end of the 16 KB AHB SRAM; STM32:
   3 KB reserved there by the linker script. */
#ifdef CONFIG_MK3_STM32
extern uint8_t __ahbram_end__[];
#define MOD_END   ((uint32_t)__ahbram_end__)
#else
#define MOD_END   0x20080000UL
#endif
static uint8_t *mod_img_end;        /* the loaded image's end, 4-aligned */
static uint8_t *module_spare(uint32_t *size)
{
  *size = MOD_END - (uint32_t)mod_img_end;
  return mod_img_end;
}
#endif

static uint32_t module_ticks(void)
{
  return getticks();
}

static uint8_t mod_hurry;           /* the module asked for its next idle() at once */
static void module_hurry(void)
{
  mod_hurry = 1;
}

static const uint8_t ram0_bases[] = RAM0_BASES;

static const module_api_t mod_api = {
  sram_readblock, sram_writeblock, sram_memset,
  sram_readbyte, sram_writebyte, sram_readshort, sram_writeshort,
  f_open, f_read, f_close, psram_stream, &file_handle,
  menu_music_play_gen, menu_music_play, menu_music_stop, menu_music_pause, menu_music_lock,
  menu_music_noloop, menu_music_seek, menu_music_active, menu_music_ended,
  menu_music_tell, menu_music_size, menu_music_gen_played,
  memtest_core, fpga_select_mem, memtest_pass, writeled, module_ticks, menu_sfx_forget,
  module_spare,
  path_asset, path_asset_mkdir, f_write,
  get_bcdtime, set_fpga_time, get_spc7110_rtc, set_spc7110_rtc,
  f_lseek, f_unlink, f_rename, f_mkdir, f_opendir, f_readdir, f_closedir, f_stat, f_getfree,
  gameinfo_font_cp, revalidate_game_lists, module_hurry,
  CONFIG_CPU_FREQUENCY, ram0_bases, RAM0_NUM_CHIPS, RAM0_SECOND_ADDR, MEMTEST_ITERATIONS
};

static module_fns_t mod;
static uint8_t mod_live;            /* started, not stopped yet */
static uint8_t mod_id;              /* the module in ptrcache, 0 = none (or gone) */

#ifdef MODULE_HOST_LINKED
/* Host builds of the firmware (the Ciclone) cannot run an ARM image: there the modules are
   linked in and nothing is loaded. */
extern const module_fns_t nsfmod_fns, mtmod_fns, pcmmod_fns, rtcmod_fns, fopmod_fns;
static int module_load(uint8_t id)
{
  /* rmdir.bin is fopmod.c too: the host build links one module with both */
  static const module_fns_t *const fns[MODULE_COUNT] = { &nsfmod_fns, &mtmod_fns, &pcmmod_fns, &rtcmod_fns, &fopmod_fns,
                                                         &fopmod_fns };
  if(!id || id > MODULE_COUNT) return 0;
  mod = *fns[id - 1];
  return 1;
}
#else
static int module_load(uint8_t id)
{
  static const char *const file[MODULE_COUNT] = {
    "/sd2snes/nsfplay.bin", "/sd2snes/memtest.bin", "/sd2snes/pcmplay.bin",
    "/sd2snes/rtc7110.bin", "/sd2snes/fileops.bin", "/sd2snes/rmdir.bin"
  };
  uint8_t *img = (uint8_t *)ptrcache;
  const module_hdr_t *h = (const module_hdr_t *)img;
  uint32_t crc = crc32_init(), size, i;
  UINT br = 0;
  if(!id || id > MODULE_COUNT) return 0;
  ovl_forget();                     /* the image goes over any card overlay */
  if(f_open(&file_handle, file[id - 1], FA_READ) != FR_OK) return 0;
  size = f_size(&file_handle);
  if(size < sizeof(*h) || size > MOD_END - (uint32_t)img
     || f_read(&file_handle, img, size, &br) != FR_OK || br != size) {
    f_close(&file_handle);
    return 0;
  }
  f_close(&file_handle);
  for(i = 0; i < size; i++)
    crc = crc32_update(crc, (i >= 12 && i < 16) ? 0 : img[i]);   /* the crc field reads as 0 */
  if(h->magic != MODULE_MAGIC || h->abi != MODULE_ABI || h->size != size
     || h->state_size > sizeof(scratch_frame) || crc32_finalize(crc) != h->crc)
    return 0;
  mod_img_end = img + ((size + 3) & ~3UL);
  mod.start = (void *)((uint32_t)img + h->start);
  mod.ctl   = (void *)((uint32_t)img + h->ctl);
  mod.idle  = (void *)((uint32_t)img + h->idle);
  mod.stop  = (void *)((uint32_t)img + h->stop);
  return 1;
}
#endif

/* status: publish SRAM_MODULE_STATUS for the menu (a menu module only; a session module
   opens inside a game load, with no menu to read it) */
static int module_start(uint8_t id, const char *path, uint8_t status)
{
  module_drop();
  /* the same module again (the next track, the next file): its image is still in place,
     nothing has used ptrcache since -- every foreign command goes through module_gate */
  if(id != mod_id) {
    mod_id = 0;
    if(!module_load(id)) {
      if(status) sram_writebyte(MODULE_ST_MISSING, SRAM_MODULE_STATUS);
      return MODULE_MISSING;
    }
    mod_id = id;
  }
  if(status) sram_writebyte(MODULE_ST_OK, SRAM_MODULE_STATUS);
  mod_live = 1;
  return mod.start(scratch_frame, &mod_api, path);
}

int module_open(uint8_t id, const char *path)
{
  return module_start(id, path, 1);
}

int module_session(uint8_t id, const char *path)
{
  return module_start(id, path, 0);
}

void module_drop(void)
{
  if(mod_live) mod.stop(scratch_frame);
  mod_live = 0;
}

int module_live(void)
{
  return mod_live;
}

void module_ctl(uint32_t param)
{
  if(mod_live) mod.ctl(scratch_frame, param);
}

void module_gate(uint8_t cmd)
{
  if(cmd == SNES_CMD_MOD_OPEN || cmd == SNES_CMD_MOD_CTL
     || cmd == SNES_CMD_PLAY_PCM || cmd == SNES_CMD_PCM_CTL) return;
  module_drop();
  mod_id = 0;                       /* ptrcache and the FRAME are someone else's from here */
}

int module_idle(void)
{
  ovl_timing_tick();                /* measurement builds only (OVL_TIMING) */
  mod_hurry = 0;
  if(mod_live) mod.idle(scratch_frame);
  return mod_hurry;
}
