/* The SPC player's file load and the random menu-music draw: menu-only, card overlay code
   (see ovl.h).  Moved out of memory.c and menucmd.c to go to the card. */
#include <string.h>
#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "fpga_spi.h"
#include "memory.h"
#include "snes.h"
#include "cfg.h"
#include "timer.h"    /* getticks: seed for the random menu-music draw */
#include "rtc.h"      /* get_bcdtime: mixed into that seed when the RTC is valid */

extern cfg_t CFG;
extern volatile mcu_status_t STM;

uint32_t load_spc(uint8_t* filename, uint32_t spc_data_addr, uint32_t spc_header_addr) {
  DWORD filesize;
  UINT bytes_read;
  uint8_t data;
  UINT j;

  printf("%s\n", filename);

  file_open(filename, FA_READ); /* Open SPC file */
  if(file_res) return 0;
  filesize = file_handle.fsize;
  if (filesize < 65920) { /* At this point, we care about filesize only */
    file_close(); /* since SNES decides if it is an SPC file */
    sram_writebyte(0, spc_header_addr); /* If file is too small, destroy previous SPC header */
    return 0;
  }

  set_mcu_addr(spc_data_addr);
  f_lseek(&file_handle, 0x100L); /* Load 64K data segment */

  for(;;) {
    bytes_read = file_read();
    if (file_res || !bytes_read) break;
    FPGA_SELECT();
    FPGA_TX_BYTE(0x98);
    for(j=0; j<bytes_read; j++) {
      FPGA_TX_BYTE(file_buf[j]);
      FPGA_WAIT_RDY_INLINE();
    }
    FPGA_DESELECT();
  }

  file_close();
  file_open(filename, FA_READ); /* Reopen SPC file to reset file_getc state*/

  set_mcu_addr(spc_header_addr);
  f_lseek(&file_handle, 0x0L); /* Load 256 bytes header */

  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);
  for (j = 0; j < 256; j++) {
    data = file_getc();
    FPGA_TX_BYTE(data);
    FPGA_WAIT_RDY_INLINE();
  }
  FPGA_DESELECT();

  file_close();
  file_open(filename, FA_READ); /* Reopen SPC file to reset file_getc state*/

  set_mcu_addr(spc_header_addr+0x100);
  f_lseek(&file_handle, 0x10100L); /* Load 128 DSP registers */

  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);
  for (j = 0; j < 128; j++) {
    data = file_getc();
    FPGA_TX_BYTE(data);
    FPGA_WAIT_RDY_INLINE();
  }
  FPGA_DESELECT();
  file_close(); /* Done ! */

  /* clear echo buffer to avoid artifacts */
  uint8_t esa = sram_readbyte(spc_header_addr+0x100+0x6d);
  uint8_t edl = sram_readbyte(spc_header_addr+0x100+0x7d);
  uint8_t flg = sram_readbyte(spc_header_addr+0x100+0x6c);
  if(!(flg & 0x20) && (edl & 0x0f)) {
    int echo_start = esa << 8;
    int echo_length = (edl & 0x0f) << 11;
    printf("clearing echo buffer %04x-%04x...\n", echo_start, echo_start+echo_length-1);
    sram_memset(spc_data_addr+echo_start, echo_length, 0);
  }

  return (uint32_t)filesize;
}

/* Random menu music (CFG.menu_music_random + CFG.menu_music_folder).
   There is no rand() in this firmware and pulling one in would drag stdlib's
   reentrancy state along, which is flash the mk2 does not have; xorshift32 costs a
   handful of instructions and is far beyond what "pick a file" needs.
   Seeded lazily, once: the tick counter alone starts from about the same value on
   every power-on, so the first draw of a session would keep landing on the same
   track -- the wall clock is mixed in whenever the RTC has one (STM.rtc_valid == 0
   means valid, 0xff means it is not; see main.c). */
static uint32_t menu_spc_rnd_state = 0;

static uint32_t menu_spc_rnd(void) {
  uint32_t x = menu_spc_rnd_state;
  if(!x) {
    x = (uint32_t)getticks() * 2654435761u;
    if(!STM.rtc_valid) {
      uint64_t bcd = get_bcdtime();
      x ^= (uint32_t)bcd ^ (uint32_t)(bcd >> 32);
    }
    if(!x) x = 0x1a2b3c4du;   /* zero is xorshift32's fixed point: it never leaves it */
  }
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  menu_spc_rnd_state = x;
  return x;
}

/* Fingerprint of the previous draw, so two BGM loads in a row (a boot and the return
   from the first game, say) do not land on the same track.  A HASH and not the name:
   it only has to answer "the same file as last time?", a collision costs at worst one
   redundant re-draw, and the .bss on this MCU is the same pool the stack eats into --
   4 bytes instead of a name buffer is the whole reason this is not a strncmp. */
static uint32_t menu_spc_prev_hash;

/* FNV-1a: a couple of instructions per byte and no table. */
static uint32_t menu_spc_hash(const char *s) {
  uint32_t h = 2166136261u;
  while(*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

/* Draw a random .spc from CFG.menu_music_folder and write its full SD path to `out`.
   Returns 1 when it wrote one and 0 when there is nothing to draw from -- folder not
   configured, missing, unreadable, or holding no .spc -- so the caller falls back to
   its fixed choice and the feature can never leave the menu silent.

   ONE f_opendir/f_readdir pass, with a reservoir of a single element: keeping the
   n-th candidate with probability 1/n leaves every file equally likely without ever
   building a list, so the folder may hold any number of tracks and this still costs
   one name's worth of RAM.

   `out` doubles as the FatFs long-name buffer for the scan.  That is what keeps this
   down to a single path buffer, and it works precisely BECAUSE f_readdir overwrites
   it on every entry: nothing may survive there across the loop, which is why the
   winner is copied out to `pick` and the path is only assembled at the very end.
   (Same trap patch_scan_dir documents, from the other side.)

   (Card overlay code: its frame is never part of menucmd_dispatch's.) */
int menu_spc_pick_random(uint8_t *out, int outsize) {
  /* CFG is volatile only because it is the shared BSRAM mirror; reading the folder
     name as a plain string is what every other CFG string consumer does. */
  const char *folder = (const char*)CFG.menu_music_folder;
  char pick[256];
  DIR dir;
  FILINFO fno;
  size_t flen;
  int attempt;

  if(folder[0] != '/') return 0;                   /* not configured -> caller falls back */
  flen = strlen(folder);
  while(flen && folder[flen - 1] == '/') flen--;   /* the join below adds exactly one */
  if(flen + 2 >= (size_t)outsize) return 0;        /* no room for even a 1-char name */

  for(attempt = 0; attempt < 2; attempt++) {
    unsigned n = 0;
    pick[0] = 0;
    fno.lfsize = (UINT)((outsize > 256) ? 255 : outsize - 1);
    fno.lfname = (TCHAR*)out;
    if(f_opendir(&dir, (const TCHAR*)folder) != FR_OK) return 0;
    for(;;) {
      const char *fn, *ext;
      size_t len;
      if(f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
      if(fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
      fn = fno.lfname[0] ? fno.lfname : fno.fname;
      /* macOS leaves a "._<name>.spc" beside every file it copies; those pass the
         extension test below and would be drawn as if they were tracks. */
      if(fn[0] == '.') continue;
      ext = strrchr(fn, '.');
      if(!ext || strcasecmp(ext + 1, "SPC")) continue;
      len = strlen(fn);
      /* Never draw a name the join could not hold: a truncated path opens some OTHER
         file, which is why patch_scan_dir refuses over-long names too. */
      if(len >= sizeof(pick) || flen + 1 + len >= (size_t)outsize) continue;
      if((menu_spc_rnd() % ++n) == 0) memcpy(pick, fn, len + 1);
    }
    f_closedir(&dir);
    if(!n) return 0;                               /* no .spc in there */
    /* Do not repeat the previous track -- but draw again AT MOST once.  Retrying
       until it differs is an unbounded coin flip as soon as the folder holds only
       two files, and this runs while the SNES waits on the command. */
    if(n < 2 || menu_spc_hash(pick) != menu_spc_prev_hash) break;
  }

  menu_spc_prev_hash = menu_spc_hash(pick);
  memcpy(out, folder, flen);
  out[flen] = '/';
  memcpy(out + flen + 1, pick, strlen(pick) + 1);
  printf("Random menu music: %s\n", out);
  return 1;
}
