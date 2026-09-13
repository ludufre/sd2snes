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

   filetypes.h: directory scanning and file type detection
*/

#ifndef FILETYPES_H
#define FILETYPES_H

#ifdef DEBUG_FS
#define DBG_FS
#else
#define DBG_FS while(0)
#endif

#include "ff.h"

#define FS_MAX_DEPTH	(10)
/* rely on optimization to reduce every occurrence to the same location. */
#define SYS_DIR_NAME	((const char*)"sd2snes")
typedef enum {
  TYPE_UNKNOWN =   0,
  TYPE_ROM     =   1,
  TYPE_SRM     =   2,
  TYPE_SPC     =   3,
  TYPE_IPS     =   4,
  TYPE_CHT     =   5,
  TYPE_SKIN    =   6,
  TYPE_NES     =   7,   /* .nes (iNES) -- core NES mk3-only; lockstep com TYPE_NES em snes/memmap.i65 */
  TYPE_PCM     =   8,   /* .pcm (MSU-1 audio track) -- played by the menu PCM player; lockstep com TYPE_PCM em snes/memmap.i65 */
  TYPE_FILE    =   9,   /* any other file INSIDE /sd2snes (see scan_dir): listed with its size for
                           inspection, NOT actionable and NOT deletable -- this is what keeps
                           firmware.im3 / m3nu.bin / fpga_*.bi3 out of reach of the context menu.
                           Lockstep com TYPE_FILE em snes/memmap.i65 */
  TYPE_DATA    =  10,   /* a card-data file the user owns (see is_card_data_ext): saves, savestates,
                           sidecars, .msu. Listed like TYPE_FILE but the Y context menu offers
                           Delete. Lockstep com TYPE_DATA em snes/memmap.i65 */
  TYPE_SUBDIR  =  64,
  TYPE_PARENT  = 128
} SNES_FTYPE;


SNES_FTYPE determine_filetype(FILINFO fno);
/* Extension-only classification of a leaf or a full path (never a directory). THE list of
   known extensions lives in filetype_by_ext; determine_filetype feeds it the directory
   entry's name, and the delete path asks it whether what it removed was a ROM. */
SNES_FTYPE filetype_by_ext(const char *name);
/* *msu_rom: index, in the sorted table, of the ROM this folder opens as (CFG.open_msu_folders),
   or DIR_NO_MSU_ROM */
#define DIR_NO_MSU_ROM (0xffff)
uint16_t scan_dir(const uint8_t *path, uint32_t base_addr, const SNES_FTYPE *filetypes, uint16_t *msu_rom);
uint8_t dir_may_open_as_msu(const uint8_t *path);
int get_num_dirent(uint32_t addr);
void sort_all_dir(uint32_t endaddr);
void make_filesize_string(char *buf, uint32_t size);
int is_requested_filetype(SNES_FTYPE type, const SNES_FTYPE *filetypes);
#endif
