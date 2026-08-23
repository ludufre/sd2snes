/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

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

   sufami.c: Sufami Turbo Slot B minicart -- battery SRAM autosave

   See sufami.h for why this is a separate scan and not part of the SaveRAM one.
*/

#include "config.h"
#include "uart.h"
#include "cfg.h"
#include "crc32.h"
#include "string.h"
#include "ff.h"
#include "fileops.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "led.h"
#include "memory.h"
#include "patch.h"
#include "snes.h"
#include "sufami.h"

extern cfg_t CFG;
extern uint8_t crc_valid;

uint32_t sufami_slotb_ramsize;
uint32_t sufami_rom_mask_b;

/* .ahbram: written once per load, read only after, and every read is gated on
   sufami_slotb_ramsize (plain .bss, zeroed at boot). Keeps 256 B out of the main SRAM. */
char sufami_slotb_path[256] IN_AHBRAM;

/* Chunked CRC over the region, then five stable passes before writing. */
static uint32_t slotb_crc, slotb_crc_old, slotb_offset;
static uint32_t slotb_diff, slotb_same, slotb_didnotsave, slotb_save_failed;

/* The A+B pair sidecar: /sd2snes/saves/sft/<BB>/<stemA>.stb holds the full SD path of
   the Slot B cart.  Same root and the same one-shot shape as the .slot sidecar that
   carries the battery-SRAM slot selection.  A real Sufami Turbo keeps both carts
   plugged in, so remembering the pair is the behaviour that matches the hardware. */
static int slotb_sidecar_name(char *buf, int buflen, const uint8_t *rom_path) {
  return path_asset(buf, buflen, SAVE_BASEDIR, (const char*)rom_path, ".stb");
}

static void slotb_sidecar_load(const uint8_t *rom_path) {
  char sc[256];
  UINT br = 0;
  sufami_slotb_path[0] = 0;
  if(slotb_sidecar_name(sc, sizeof(sc), rom_path) < 0) return;
  file_open((uint8_t*)sc, FA_READ);
  if(!file_res) {
    f_read(&file_handle, sufami_slotb_path, sizeof(sufami_slotb_path) - 1, &br);
    sufami_slotb_path[br] = 0;
    /* written without a terminator by design; also tolerate a hand-edited file */
    while(br && (sufami_slotb_path[br - 1] == '\n' || sufami_slotb_path[br - 1] == '\r'))
      sufami_slotb_path[--br] = 0;
    if(sufami_slotb_path[0] != '/') sufami_slotb_path[0] = 0;   /* only absolute paths are usable */
  }
  file_close();
  file_res = 0;              /* an absent sidecar just means "no Slot B" */
}

static void slotb_sidecar_save(const uint8_t *rom_path) {
  char sc[256];
  UINT bw = 0;
  if(slotb_sidecar_name(sc, sizeof(sc), rom_path) < 0) return;
  if(!sufami_slotb_path[0]) {                      /* explicit "none" -> forget the pair */
    f_unlink(sc);
    file_res = 0;
    return;
  }
  path_asset_mkdir(sc);                     /* write path only, after the name exists */
  file_open((uint8_t*)sc, FA_CREATE_ALWAYS | FA_WRITE);
  if(!file_res) f_write(&file_handle, sufami_slotb_path, strlen(sufami_slotb_path), &bw);
  file_close();
  file_res = 0;
}

void sufami_slotb_empty(void) {
  sufami_rom_mask_b = 0;
  sufami_slotb_ramsize = 0;
  sufami_slotb_path[0] = 0;                 /* stops the autosave dead: nothing to name */
  /* The BIOS probes the slot by looking for the signature at offset 0.  With the
     ROM mask at 0 every Slot B read collapses onto this byte range, so blanking it
     is what makes the slot read as empty rather than as a corrupt cart. */
  sram_memset(SUFAMI_SLOTB_ROM_ADDR, 0x40, 0x00);
}

void sufami_query_slotb(const uint8_t *rom_path) {
  const char *path = (const char *)rom_path;
  const char *last_slash = NULL, *leaf;
  char dirpath[256], entry[IPS_PATH_LEN];
  DIR dir;
  FILINFO fno;
  uint8_t count = 0, preselect = 0;
  size_t dirlen = 0;

  sram_writebyte(0, SRAM_IPS_LIST_ADDR);
  sram_writebyte(IPS_DLGMODE_PATCH, SRAM_IPS_LIST_ADDR + IPS_DLGMODE_OFFSET);
  if(!path_is_st(path)) return;

  /* The pair remembered for THIS Slot A cart decides where the dialog opens. */
  slotb_sidecar_load(rom_path);

  for(const char *p = path; *p; p++) if(*p == '/') last_slash = p;
  leaf = last_slash ? last_slash + 1 : path;
  if(last_slash && last_slash != path) {
    dirlen = (size_t)(last_slash - path);
    if(dirlen >= sizeof(dirpath)) dirlen = sizeof(dirpath) - 1;
    memcpy(dirpath, path, dirlen);
    dirpath[dirlen] = 0;
  } else {
    dirpath[0] = '/';
    dirpath[1] = 0;
  }

  /* Same caution as patch_scan_dir: f_readdir writes through fno.lfname into the
     global file_lfn, so `path` must be a buffer of the caller's own. */
  fno.lfsize = 255;
  fno.lfname = (TCHAR *)file_lfn;
  if(f_opendir(&dir, dirpath) != FR_OK) {
    DBG_SUFAMI printf("sufami_query_slotb: opendir(%s) failed\n", dirpath);
    return;
  }

  for(;;) {
    const char *fn;
    size_t n;
    if(f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
    if(fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
    fn = fno.lfname[0] ? fno.lfname : fno.fname;
    if(fn[0] == '.') continue;
    if(!path_is_st(fn)) continue;
    /* The Slot A cart itself is not a candidate: a real Sufami Turbo has two
       physical slots, and the same cartridge cannot be in both. */
    if(!strcasecmp(fn, leaf)) continue;
    if(count >= IPS_MAX_PATCHES) break;

    n = strlen(fn);
    if(dirlen + 1 + n >= IPS_PATH_LEN) {
      DBG_SUFAMI printf("sufami_query_slotb: name too long, skipping %s\n", fn);
      continue;
    }
    memcpy(entry, dirpath, dirlen);
    entry[dirlen] = '/';
    memcpy(entry + dirlen + 1, fn, n + 1);
    if(sufami_slotb_path[0] && !strcasecmp(entry, sufami_slotb_path)) preselect = count + 1;
    sram_writeblock(entry, SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                           + (uint32_t)count * IPS_PATH_LEN, dirlen + 1 + n + 1);

    /* Display slot.  A patch entry reserves IPS_NAME_BADGE.. for the IPS/BPS badge,
       but every entry in THIS list is a minicart, so the badge would say nothing --
       the dialog skips that column for Slot B (dlg_badge) and the name gets the whole
       slot, IPS_NAME_LEN-1 characters instead of IPS_NAME_BADGE-1.
       The ".st" is dropped too: it is the same on every row, and those three columns
       are worth more to a No-Intro filename than to an extension nobody is comparing. */
    {
      size_t shown = n;
      if(shown > 3 && !strcasecmp(fn + shown - 3, ".st")) shown -= 3;
      if(shown > IPS_NAME_LEN - 1) shown = IPS_NAME_LEN - 1;
      memset(entry, 0, IPS_NAME_LEN);
      memcpy(entry, fn, shown);
    }
    sram_writeblock(entry, SRAM_IPS_TEXT_ADDR + IPS_NAME_BASE
                           + (uint32_t)count * IPS_NAME_LEN, IPS_NAME_LEN);
    /* PATCH_TYPE_BPS, deliberately: it is the type the dialog draws NO header-mode
       marker for (a .bps has no header convention), which is what a cart entry
       needs -- and the Y context menu is switched off for this list anyway. */
    sram_writebyte(PATCH_TYPE_BPS, SRAM_IPS_LIST_ADDR + IPS_FLAGS_BASE + count);
    count++;
  }
  f_closedir(&dir);

  sram_writebyte(count, SRAM_IPS_LIST_ADDR);
  sram_writebyte(IPS_DLGMODE_SLOTB, SRAM_IPS_LIST_ADDR + IPS_DLGMODE_OFFSET);
  /* The dialog opens on listsel_sel 0 ("no cart"); a remembered pair is reported
     so a future revision can pre-select it. */
  DBG_SUFAMI printf("Sufami Slot B candidates: %d (remembered #%d)\n", count, preselect);
}

void sufami_stage_slotb_rom(void) {
  uint8_t  hdr[0x38];
  uint32_t fsize, base, loaded, sz;

  sufami_rom_mask_b = 0;
  sufami_slotb_ramsize = 0;
  if(!sufami_slotb_path[0]) { sufami_slotb_empty(); return; }

  /* Peek at the header before staging: an .st that is not actually a minicart (or
     is the base BIOS) must leave the slot empty rather than be handed to the ST BIOS
     as a cart.  Bare dumps only, same as Slot A -- see smc_id. */
  file_open((uint8_t*)sufami_slotb_path, FA_READ);
  if(file_res) { file_close(); file_res = 0; sufami_slotb_empty(); return; }
  fsize = file_handle.fsize;
  base = ((fsize & 0xffff) == 0x200) ? 0x200 : 0;   /* same test load_sram_offload uses */
  file_readblock(hdr, base, sizeof(hdr));
  file_close();
  file_res = 0;

  if(!smc_is_sufami_minicart(hdr)) {
    DBG_SUFAMI printf("Sufami Slot B: %s is not a minicart, slot left empty\n", sufami_slotb_path);
    sufami_slotb_empty();
    return;
  }

  loaded = load_sram_offload((uint8_t*)sufami_slotb_path, SUFAMI_SLOTB_ROM_ADDR,
                             LOADRAM_AUTOSKIP_HEADER);
  if(file_res || !loaded) {
    DBG_SUFAMI printf("Sufami Slot B: load failed (%d), slot left empty\n", file_res);
    file_res = 0;
    sufami_slotb_empty();
    return;
  }

  sz = 1;
  while(sz < (fsize - base)) sz <<= 1;
  if(sz > SUFAMI_ROM_MASK_MAX + 1) sz = SUFAMI_ROM_MASK_MAX + 1;
  sufami_rom_mask_b = sz - 1;
  /* The Slot B cart's OWN header byte 0x37, read back from the staged image: its save
     size has nothing to do with the Slot A cart's, which is exactly why the FPGA
     carries a second SaveRAM mask. */
  sufami_slotb_ramsize = (uint32_t)sram_readbyte(SUFAMI_SLOTB_ROM_ADDR + 0x37) * 2048;
  DBG_SUFAMI printf("Sufami Slot B: ROM=%ldKB SRAM=%ldKB\n", sz >> 10, sufami_slotb_ramsize >> 10);
}

void sufami_stage_slotb(const uint8_t *rom_path, uint8_t sel) {
  sufami_clear();
  if(!rom_path || !path_is_st((const char*)rom_path)) return;

  if(sel == SUFAMI_SEL_SIDECAR) {
    /* Recents / Favorites / autoboot: no selector ran, reuse the remembered pair. */
    slotb_sidecar_load(rom_path);
  } else {
    if(sel > 0 && sel <= IPS_MAX_PATCHES) {
      sram_readstrn(sufami_slotb_path, SRAM_IPS_TEXT_ADDR + IPS_PATH_BASE
                                + (uint32_t)(sel - 1) * IPS_PATH_LEN, sizeof(sufami_slotb_path));
      if(sufami_slotb_path[0] != '/') sufami_slotb_path[0] = 0;
    } else {
      sufami_slotb_path[0] = 0;                    /* 0 = the user picked "none" */
    }
    slotb_sidecar_save(rom_path);
  }
  DBG_SUFAMI printf("Sufami Turbo: Slot B = %s\n", sufami_slotb_path[0] ? sufami_slotb_path : "(none)");
}

/* Named from the Slot B CART, never through save_srm(): that helper routes the name
   through current_ips_srm_source (a Slot A patch would rename Slot B's save) and
   through srm_slot (which belongs to the loaded game, not to its companion). */
static int slotb_srm_name(char *buf, int buflen) {
  if(!sufami_slotb_path[0]) { if(buflen > 0) buf[0] = 0; return -1; }
  return path_asset(buf, buflen, SAVE_BASEDIR, sufami_slotb_path, ".srm");
}

static void slotb_write(void) {
  char srmfile[256];
  if(slotb_srm_name(srmfile, sizeof(srmfile)) < 0) {
    DBG_SUFAMI printf("Sufami Slot B: save path too long, not saving\n");
    slotb_save_failed = 1;
    return;
  }
  path_asset_mkdir(srmfile);          /* write path only, and only after the name */
  save_sram((uint8_t*)srmfile, sufami_slotb_ramsize, SUFAMI_SLOTB_SAVE_ADDR);
  slotb_save_failed = file_res ? 1 : 0;
}

void sufami_slotb_crc_seed(void) {
  slotb_crc = slotb_offset = slotb_diff = slotb_same = 0;
  slotb_didnotsave = slotb_save_failed = 0;
  if(!sufami_slotb_ramsize) { slotb_crc_old = 0; return; }
  slotb_crc_old = calc_sram_crc(SUFAMI_SLOTB_SAVE_ADDR, sufami_slotb_ramsize, 0);
}

void sufami_slotb_autosave(void) {
  uint32_t crc_bytes;

  if(!sufami_slotb_ramsize || !CFG.enable_autosave) {
    /* keep slotb_crc_old: the reset flush compares against it, and clearing it
       would force a pointless full rewrite on every return to the menu */
    slotb_offset = 0;
    slotb_crc = 0;
    slotb_same = 0;
    slotb_didnotsave = 0;
    return;
  }

  crc_bytes = min(sufami_slotb_ramsize - slotb_offset, SRAM_REGION_SIZE);
  slotb_crc = calc_sram_crc(SUFAMI_SLOTB_SAVE_ADDR + slotb_offset, crc_bytes, slotb_crc);
  slotb_offset += crc_bytes;

  if(crc_valid && sram_reliable()) {
    if(slotb_offset >= sufami_slotb_ramsize) {
      if(slotb_save_failed) slotb_didnotsave++;
      if(slotb_crc != slotb_crc_old) {
        slotb_diff = 1;              /* dirty since the last write */
        slotb_same = 0;
        slotb_didnotsave++;
      } else if(slotb_diff) {
        slotb_same++;                /* dirty, but stable this pass */
      }
      if(slotb_diff && slotb_same >= 5) {
        DBG_SUFAMI printf("Sufami Slot B CRC: 0x%04lx; saving\n", slotb_crc);
        writeled(1);
        slotb_write();
        if(!slotb_save_failed) { slotb_diff = 0; slotb_same = 0; }
        slotb_didnotsave = slotb_save_failed ? 25 : 0;
        writeled(0);
      }
      if(slotb_didnotsave > 50) {
        DBG_SUFAMI printf("Sufami Slot B periodic save\n");
        writeled(1);
        slotb_write();
        if(!slotb_save_failed) slotb_diff = 0;
        slotb_didnotsave = slotb_save_failed ? 25 : 0;
        writeled(0);
      }
      slotb_offset = 0;
      slotb_crc_old = slotb_crc;
      slotb_crc = 0;
    }
  } else {
    slotb_offset = 0;
    slotb_crc = 0;
  }
}

void sufami_slotb_flush_inreset(void) {
  uint32_t crc;

  if(!sufami_slotb_ramsize) return;
  if(fpga_test() != FPGA_TEST_TOKEN) return;

  /* calc_sram_crc gives up as soon as get_snes_reset() is true, and we are called
     with the SNES already held in reset, so read raw here (same reason
     calc_pack_crc_inreset exists for the BS-X pack). */
  crc = calc_sram_crc_raw(SUFAMI_SLOTB_SAVE_ADDR, sufami_slotb_ramsize);

  if(crc != slotb_crc_old) {
    writeled(1);
    slotb_write();
    slotb_crc_old = crc;
    slotb_diff = 0;
    writeled(0);
  }
}

void sufami_clear(void) {
  sufami_slotb_ramsize = 0;
  sufami_rom_mask_b = 0;
  sufami_slotb_path[0] = 0;
  slotb_crc = slotb_crc_old = slotb_offset = 0;
  slotb_diff = slotb_same = slotb_didnotsave = slotb_save_failed = 0;
}
