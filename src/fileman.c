/* File manager, the firmware's part (see fileman.h): SNES_CMD_FM, the clipboard, the menu's
   bank $C3 and opening the card module fileops.bin, which does the actual work.  Kept to
   what has to be firmware: the Mk.II flash has a few KB left. */

#include <string.h>
#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "filetypes.h"
#include "memory.h"
#include "snes.h"
#include "psram_io.h"
#include "module.h"
#include "fileman.h"

/* The clipboard's authority (fileman.h): .bss, so zero at power-on, and fileman_forget
   runs on every game load. */
static uint16_t fm_clip_count;
static uint8_t  fm_clip_mode;
static uint8_t  fm_clip_err;

void fileman_publish(void) {
  uint8_t b[FM_CLIPINFO_LEN];
  b[0] = 'F';
  b[1] = 'C';
  b[2] = FM_VERSION;
  b[3] = fm_clip_count ? fm_clip_mode : FM_CLIP_NONE;
  b[4] = fm_clip_count & 0xff;
  b[5] = fm_clip_count >> 8;
  b[6] = fm_clip_err;
  b[7] = 0;
  sram_writeblock(b, SRAM_FM_CLIPINFO_ADDR, sizeof(b));
}

void fileman_forget(void) {
  fm_clip_count = 0;
  fm_clip_mode = FM_CLIP_NONE;
}

/* The menu's 4th bank lives at $C3, which the Ficha's guide viewer borrows for its tiles
   (SRAM_MANUAL_S1TILES_ADDR): the menu checks the bank's magic before calling into it and
   asks for it back with FM_OP_BANK.  Bounded: one 64 KB stream. */
static void fm_bank(void) {
  file_open((const uint8_t *)MENU_FILENAME, FA_READ);
  if(!file_res && f_size(&file_handle) >= 0x40000UL
     && f_lseek(&file_handle, 0x30000UL) == FR_OK)
    psram_stream(&file_handle, SRAM_MENU_ADDR + 0x30000UL, 0x10000UL, NULL);
  file_close();
  file_res = 0;
}

/* The marked entries of the folder the browser shows become the clipboard.  The names come
   from the directory table the last READDIR left in PSRAM, in the same sorted order the
   menu's bit numbers follow.  noinline: the two buffers belong to this command only. */
static __attribute__((noinline)) void fm_clip(void) {
  uint8_t marks[64], ent[4];
  char name[256];
  uint8_t mode = snescmd_readbyte(SNESCMD_MCU_PARAM + 9);
  uint16_t n = snescmd_readshort(SNESCMD_MCU_PARAM + 10);
  uint32_t out = SRAM_FM_CLIP_ADDR + FM_CLIP_HDR;
  uint16_t count = 0;
  uint32_t i;

  fileman_forget();
  fm_clip_err = FM_ERR_NONE;
  if(n > FM_MARKS_BYTES * 8) n = FM_MARKS_BYTES * 8;
  /* the source folder, as the menu keeps it (FILESEL_CWD) */
  sram_readstrn(file_lfn, snes_get_mcu_param() & 0xffffff, 256);
  file_lfn[255] = 0;
  sram_writeblock(file_lfn, SRAM_FM_CLIP_ADDR + 4, 256);
  for(i = 0; i < n; i++) {
    if(!(i & 511)) sram_readblock(marks, SRAM_FM_MARKS_ADDR + (i >> 3), sizeof(marks));
    if(!(marks[(i & 511) >> 3] & (1 << (i & 7)))) continue;
    sram_readblock(ent, SRAM_DIR_ADDR + i * 4, 4);
    if(ent[3] == TYPE_PARENT) continue;
    uint32_t p = ent[0] | ((uint32_t)ent[1] << 8) | ((uint32_t)ent[2] << 16);
    sram_read_fsname(name + 1, SRAM_MENU_ADDR + p + 6, sizeof(name) - 1);
    char *dot = strchr(name + 1, 1);          /* the hidden extension (get_selected_name) */
    if(dot) *dot = '.';
    uint16_t len = strlen(name + 1);
    if(!len) continue;
    name[0] = (ent[3] == TYPE_SUBDIR) ? FM_ENT_DIR : FM_ENT_FILE;
    if(out + len + 2 > SRAM_FM_CLIP_END) {
      fm_clip_err = FM_ERR_FULL;
      return;
    }
    sram_writeblock(name, out, len + 2);      /* type + name + NUL */
    out += len + 2;
    count++;
  }
  ent[0] = mode;
  ent[1] = 0;
  ent[2] = count & 0xff;
  ent[3] = count >> 8;
  sram_writeblock(ent, SRAM_FM_CLIP_ADDR, 4);
  if(count && (mode == FM_CLIP_COPY || mode == FM_CLIP_CUT)) {
    fm_clip_count = count;
    fm_clip_mode = mode;
  }
}

static void fm_stat_error(uint8_t op, uint8_t err) {
  uint8_t b[6] = { FM_MAGIC0, FM_MAGIC1, FM_VERSION, FM_ST_ERROR, op, err };
  sram_writeblock(b, SRAM_FM_STAT_ADDR, sizeof(b));
}

/* The module learns its op from FM_STAT+4, written before it starts; the path is the folder
   for a paste / new folder / a delete of the marked entries / a delete with MCU_PARAM+9 set,
   the selected entry otherwise. */
static void fm_open(uint8_t op) {
  sram_writebyte(op, SRAM_FM_STAT_ADDR + 4);
  if(op == FM_OP_PASTE || op == FM_OP_MKDIR || op == FM_OP_RMSEL
     || (op == FM_OP_RMDIR && snescmd_readbyte(SNESCMD_MCU_PARAM + 9))) {
    sram_readstrn(file_lfn, snes_get_mcu_param() & 0xffffff, 256);
    file_lfn[255] = 0;
  } else {
    get_selected_name(file_lfn);
  }
  /* a folder's directory entry ends in '/' (scan_dir); the module wants the bare path */
  size_t n = strlen((const char *)file_lfn);
  if(n > 1 && file_lfn[n - 1] == '/') file_lfn[n - 1] = 0;
  module_open(op >= FM_OP_RMDIR ? MODULE_ID_RMDIR : MODULE_ID_FILEOP, (const char *)file_lfn);
}

void fileman_command(void) {
  uint8_t op = snescmd_readbyte(SNESCMD_MCU_PARAM + 8);
  switch(op) {
    case FM_OP_BANK:
      fm_bank();
      break;
    case FM_OP_CLIP:
      fm_clip();
      break;
    case FM_OP_UNCLIP:
      fileman_forget();
      break;
    case FM_OP_PASTE:
      if(!fm_clip_count) {
        fm_stat_error(op, FM_ERR_NOCLIP);
        break;
      }
      /* a paste spends the clipboard, a copy's as well as a cut's: the
         module reads the entries from PSRAM, which keeps them for this paste */
      fileman_forget();
      fm_open(op);
      break;
    case FM_OP_RENAME:
    case FM_OP_VIEW:
    case FM_OP_MKDIR:
    case FM_OP_RMDIR:
    case FM_OP_RMSEL:
      fm_open(op);
      break;
    default:                                  /* FM_OP_PROBE */
      break;
  }
  fileman_publish();
}
