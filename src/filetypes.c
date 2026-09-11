/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   filetypes.c: directory scanning and file type detection
*/

#include <string.h>
#include "config.h"
#include "uart.h"
#include "filetypes.h"
#include "ff.h"
#include "smc.h"
#include "fileops.h"
#include "crc.h"
#include "memory.h"
#include "led.h"
#include "sort.h"
#include "cfg.h"
#include "msu1.h"   /* menu_sfx_pump: keep a playing effect fed during dir scans */

#include "timer.h"

extern cfg_t CFG;

/*
 * directory format:
 *  I. Pointer tables
 *      3 bytes   pointer to file entry
 *      1 byte    type of entry
 *                (see enum SNES_FTYPE in filetypes.h)
 *
 * II. File entries
 *      6 bytes   size string (e.g. " 1024k")
 *      n bytes   file/dir name
 */

/* Index of the ROM a folder opens as (CFG.open_msu_folders), or DIR_NO_MSU_ROM. The caller
   only asks for a folder whose single listed ROM sits next to at least one .msu, so the cost
   is finding that entry plus one f_stat of <path>/<rom stem>.msu. The path is built in
   file_lfn: the scan that used it as the LFN buffer is over, and like get_selected_name (which
   builds the same cwd + leaf there) a path that does not fit is one no ROM load could use. */
static uint16_t scan_dir_msu_rom(const uint8_t *path, uint32_t base_addr, uint16_t numentries) {
  char *buf = (char*)file_lfn;
  for(uint16_t i = 0; i < numentries; i++) {
    uint32_t ent = sram_readlong(base_addr + 4 * i);
    if((ent >> 24) != TYPE_ROM) continue;
    size_t len = strlen((const char*)path);
    if(len > 250) break;
    memcpy(buf, path, len);
    if(!len || buf[len-1] != '/') buf[len++] = '/';
    sram_readstrn(buf + len, (ent & 0xffffff) + SRAM_MENU_ADDR + 6, 256 - len);
    char *dot = strchr(buf + len, 1);   /* hidden extension */
    if(dot) *dot = '.';
    dot = strrchr(buf + len, '.');
    if(!dot || dot - buf + 5 > (int)sizeof(file_lfn)) break;
    strcpy(dot, ".msu");
    if(f_stat(buf, NULL) == FR_OK) return i;
    break;
  }
  return DIR_NO_MSU_ROM;
}

uint16_t scan_dir(const uint8_t *path, const uint32_t base_addr, const SNES_FTYPE *filetypes, uint16_t *msu_rom) {
  DIR dir;
  FRESULT res;
  FILINFO fno;
  TCHAR *fn;
  uint32_t ptr_tbl_off = base_addr;
  uint32_t file_tbl_off = base_addr + 0x10000;
  char buf[7];
  size_t fnlen;
  uint16_t rom_seen = 0;
  uint8_t msu_seen = 0;

  fno.lfsize = 255;
  fno.lfname = (TCHAR*)file_lfn;
  res = f_opendir(&dir, (TCHAR*)path);
printf("opendir res=%d\n", res);
  uint16_t numentries = 0;
  int ticks=getticks();
  SNES_FTYPE type;
printf("start\n");
  if (res == FR_OK) {
    for (;;) {
      menu_sfx_pump();  /* keep a playing menu effect fed while scanning a big dir */
      res = f_readdir(&dir, &fno);
      if(res != FR_OK || fno.fname[0] == 0 || numentries >= 16000)break;
      fn = *fno.lfname ? fno.lfname : fno.fname;
      type = determine_filetype(fno);
      /* .msu is never listed, but a folder holding one may open as its MSU-1 game */
      if(type == TYPE_UNKNOWN && !(fno.fattrib & (AM_DIR | AM_HID | AM_SYS))) {
        const char *ext = strrchr(fno.fname, '.');
        if(ext && !strcasecmp(ext+1, "MSU")) msu_seen = 1;
      }
      if(is_requested_filetype(type, filetypes)) {
        switch(type) {
          case TYPE_ROM:
          case TYPE_SPC:
          case TYPE_PCM:    /* .pcm (MSU-1 track) -- listed like a .spc, played by the menu PCM player */
          case TYPE_SKIN:   /* theme files (.thm/.skin) are listed like ROMs */
          case TYPE_NES:    /* .nes (core NES, mk3-only) -- listado como ROM */
          case TYPE_SUBDIR:
          case TYPE_PARENT:
            /* omit entries with hidden or system attribute -- but NEVER the
               parent "..": dirs created/copied on other OSes (e.g. macOS) can
               mark their "." and ".." entries hidden, and we still need ".."
               for navigation back up.  The sd2snes directory is exempt while
               CFG.show_sd2snes_folder lists it: it is hidden by NAME below and
               usually carries the system attribute on top of that, so lifting
               only one of the two would leave it invisible on most cards. No
               other hidden/system entry is affected. */
            if(type != TYPE_PARENT && (fno.fattrib & (AM_HID | AM_SYS))
               && !(CFG.show_sd2snes_folder && type == TYPE_SUBDIR && strstr(fn, "sd2snes"))) continue;
            if(fno.fattrib & AM_DIR) {
              /* omit dot directories except '..' */
              if(fn[0]=='.' && fn[1]!='.') continue;
              /* omit sd2snes directory specifically, unless the user asked for it */
              if(!CFG.show_sd2snes_folder && strstr(fn, "sd2snes")) continue;
              snprintf(buf, sizeof(buf), " <dir>");
            } else {
              if(fn[0]=='.') continue; /* omit dot files */
              make_filesize_string(buf, fno.fsize);
              if(CFG.hide_extensions) {
                char *dot = strrchr(fn, '.');
                if(dot) *dot = 1;
              }
            }
            fnlen = strlen(fn);
            if(fno.fattrib & AM_DIR) {
              fn[fnlen] = '/';
              fn[fnlen+1] = 0;
              fnlen++;
            }
            /* The file-string table grows from base_addr+0x10000 ($DC0000) up
               toward SRAM_DIR_STRINGS_END ($E00000, the battery SaveRAM). The
               region moved twice as the menu image grew: $C2.. -> $C3..$C7 (128K
               menu) -> $DC..$DF (192K menu; SRAM_DIR_ADDR now $DB0000). Stop
               before the next entry would spill into the SaveRAM. goto, not
               break: break would only exit the switch, not this scan loop. */
            if(file_tbl_off + fnlen + 7 >= SRAM_DIR_STRINGS_END) goto dir_full;
            /* write file size string */
            sram_writeblock(buf, file_tbl_off, 6);
            /* write file name string (leaf) */
            sram_writeblock(fn, file_tbl_off+6, fnlen+1);
            /* link file string entry in directory table */
            sram_writelong((file_tbl_off-SRAM_MENU_ADDR) | ((uint32_t)type << 24), ptr_tbl_off);
            file_tbl_off += fnlen+7;
            ptr_tbl_off += 4;
            numentries++;
            if(type == TYPE_ROM) rom_seen++;
            break;
          case TYPE_UNKNOWN:
          default:
            break;
        }
      }
    }
  }
dir_full:
  /* write directory termination */
  sram_writelong(0, ptr_tbl_off);
  if(CFG.sort_directories) {
    sort_dir(SRAM_DIR_ADDR, numentries);
  }
printf("end\n");
printf("%d entries, time: %d\n", numentries, getticks()-ticks);
  f_closedir(&dir);
  *msu_rom = (CFG.open_msu_folders && rom_seen == 1 && msu_seen)
           ? scan_dir_msu_rom(path, base_addr, numentries) : DIR_NO_MSU_ROM;
  return numentries;
}

SNES_FTYPE determine_filetype(FILINFO fno) {
  if(fno.fattrib & AM_DIR) {
    if(!strcmp(fno.fname, "..")) {
      return TYPE_PARENT;
    }
    return TYPE_SUBDIR;
  }
  return filetype_by_ext(fno.fname);
}

/* Extension-only classification, for a leaf OR a full path -- the one place that knows which
   extension means what. determine_filetype above hands it a directory entry's name; the
   browser delete (menucmd.c) hands it the path it just unlinked, to decide whether that file
   was a ROM and therefore owns the sidecars under /sd2snes. Only the LEAF is searched for the
   '.', so a dot in a parent directory can never be mistaken for an extension. */
SNES_FTYPE filetype_by_ext(const char *name) {
  const char *leaf = strrchr(name, '/');
  const char *ext;
  leaf = leaf ? leaf + 1 : name;
  ext = strrchr(leaf, '.');
  if(ext == NULL)
    return TYPE_UNKNOWN;
  if(  (!strcasecmp(ext+1, "SMC"))
     ||(!strcasecmp(ext+1, "SFC"))
     ||(!strcasecmp(ext+1, "FIG"))
     ||(!strcasecmp(ext+1, "SWC"))
     ||(!strcasecmp(ext+1, "BS"))
     ||(!strcasecmp(ext+1, "GB"))
     ||(!strcasecmp(ext+1, "GBC"))
     ||(!strcasecmp(ext+1, "SGB"))
     ||(!strcasecmp(ext+1, "SMS"))   /* Sega Master System (experimental SMS core) */
     ||(!strcasecmp(ext+1, "A26"))   /* Atari 2600 (experimental A26 core) */
     ||(!strcasecmp(ext+1, "ST"))    /* Sufami Turbo minicart (smc_id detects it by signature) */
    ) {
    return TYPE_ROM;
  }
/*  if(  (!strcasecmp(ext+1, "IPS"))
     ||(!strcasecmp(ext+1, "UPS"))
    ) {
    return TYPE_IPS;
  }*/
  if(!strcasecmp(ext+1, "SPC")) {
    return TYPE_SPC;
  }
  /* .pcm -- an MSU-1 audio track. Listed so the menu PCM player can play it
     straight from the browser (src/pcmplay.c). */
  if(!strcasecmp(ext+1, "PCM")) {
    return TYPE_PCM;
  }
  if(!strcasecmp(ext+1, "CHT")) {
    return TYPE_CHT;
  }
  if(!strcasecmp(ext+1, "SKIN") || !strcasecmp(ext+1, "THM")) {
    return TYPE_SKIN;   /* menu theme file (see theme.c) */
  }
  /* .nes (iNES) -- core NES, mk3-only. Listed on mk2 too: load_rom() aborts
     there with a clear MENU_ERR_NOHW popup instead of the file silently not
     appearing in the browser (looked like a scan bug to users). */
  if(!strcasecmp(ext+1, "NES")) {
    return TYPE_NES;
  }
  return TYPE_UNKNOWN;
}

int get_num_dirent(uint32_t addr) {
  int result = 0;
  while(sram_readlong(addr+result*4)) {
    result++;
  }
  return result;
}

void sort_all_dir(uint32_t endaddr) {
  uint32_t entries = 0;
  uint32_t current_base = SRAM_DIR_ADDR;
  while(current_base<(endaddr)) {
    while(sram_readlong(current_base+entries*4)) {
      entries++;
    }
    int ticks=getticks();
    printf("sorting dir @%lx, entries: %ld, time: ", current_base, entries);
    sort_dir(current_base, entries);
    printf("%d\n", getticks()-ticks);
    current_base += 4*entries + 4;
    entries = 0;
  }
}

void make_filesize_string(char *buf, uint32_t size) {
  static const char *const size_units[3] = {" ", "k", "M"};
  uint32_t fsize = size;
  uint8_t unit_idx = 0;
  while(fsize > 9999) {
    fsize >>= 10;
    unit_idx++;
  }
  snprintf(buf, 6, "% 5ld", fsize);
  strncat(buf, size_units[unit_idx], 1);
}

int is_requested_filetype(SNES_FTYPE type, const SNES_FTYPE *filetypes) {
  return strchr((const char*)filetypes, (int)type) != NULL;
}