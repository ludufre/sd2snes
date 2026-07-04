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
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

memory.c: RAM operations
*/


#include "config.h"
#include "uart.h"
#include "fpga.h"
#include "cfg.h"
#include "cic.h"
#include "crc.h"
#include "crc32.h"
#include "ff.h"
#include "fileops.h"
#include "spi.h"
#include "fpga_spi.h"
#include "led.h"
#include "smc.h"
#include "memory.h"
#include "snes.h"
#include "timer.h"
#include "rle.h"
#include "diskio.h"
#include "snesboot.h"
#include "msu1.h"
#include "cli.h"
#include "cheat.h"
#include "rtc.h"
#include "savestate.h"
#include "sgb.h"
#include "patch.h"
#include "patch_copier.h"

#include <string.h>
char* hex = "0123456789ABCDEF";

uint8_t current_ips_srm_source[256];

/* State for re-deriving the FPGA core from a PATCHED image.
   When a patch changes the cartridge type, load_rom() recurses once with
   ips_recore_active set and ips_recore_props holding the cartridge fields
   detected from the patched SDRAM image. */
static uint8_t ips_recore_active = 0;
static snes_romprops_t ips_recore_props;

extern snes_romprops_t romprops;
extern uint32_t saveram_crc_old, saveram_crc, saveram_offset;
extern uint32_t bs_pack_crc, bs_pack_crc_old, bs_pack_offset, bs_pack_diff, bs_pack_same,
                bs_pack_didnotsave, bs_pack_save_failed;
extern uint8_t bs_pack_erase_seq;
static uint8_t rom_scan_bs_vendor(uint32_t size); /* BS slot auto-detect (defined below) */
static uint8_t bs_pack_exists(uint8_t *filename);
extern sgb_romprops_t sgb_romprops;
extern uint32_t saveram_crc_old;
extern uint8_t sram_crc_valid;
extern uint8_t sram_crc_init;
extern uint32_t sram_crc_romsize;
extern cfg_t CFG;
extern snes_status_t STS;

void sram_hexdump(uint32_t addr, uint32_t len) {
  static uint8_t buf[16];
  uint32_t ptr;
  for(ptr=0; ptr < len; ptr += 16) {
    sram_readblock((void*)buf, ptr+addr, 16);
    uart_trace(buf-ptr-addr, ptr+addr, 16);
  }
}

void sram_writebyte(uint8_t val, uint32_t addr) {
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98); /* WRITE */
  FPGA_TX_BYTE(val);
  FPGA_WAIT_RDY();
  FPGA_DESELECT();
}

uint8_t sram_readbyte(uint32_t addr) {
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88); /* READ */
  FPGA_WAIT_RDY();
  uint8_t val = FPGA_RX_BYTE();
  FPGA_DESELECT();
  return val;
}

void sram_writeshort(uint16_t val, uint32_t addr) {
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98); /* WRITE */
  FPGA_TX_BYTE(val&0xff);
  FPGA_WAIT_RDY();
  FPGA_TX_BYTE((val>>8)&0xff);
  FPGA_WAIT_RDY();
  FPGA_DESELECT();
}

void sram_writelong(uint32_t val, uint32_t addr) {
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98); /* WRITE */
  FPGA_TX_BYTE(val&0xff);
  FPGA_WAIT_RDY();
  FPGA_TX_BYTE((val>>8)&0xff);
  FPGA_WAIT_RDY();
  FPGA_TX_BYTE((val>>16)&0xff);
  FPGA_WAIT_RDY();
  FPGA_TX_BYTE((val>>24)&0xff);
  FPGA_WAIT_RDY();
  FPGA_DESELECT();
}

uint16_t sram_readshort(uint32_t addr) {
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88);
  FPGA_WAIT_RDY();
  uint32_t val = FPGA_RX_BYTE();
  FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<8);
  FPGA_DESELECT();
  return val;
}

uint32_t sram_readlong(uint32_t addr) {
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88);
  FPGA_WAIT_RDY();
  uint32_t val = FPGA_RX_BYTE();
  FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<8);
  FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<16);
  FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<24);
  FPGA_DESELECT();
  return val;
}

void sram_readlongblock(uint32_t* buf, uint32_t addr, uint16_t count) {
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88);
  uint16_t i=0;
  while(i<count) {
    FPGA_WAIT_RDY();
    uint32_t val = (uint32_t)FPGA_RX_BYTE()<<24;
    FPGA_WAIT_RDY();
    val |= ((uint32_t)FPGA_RX_BYTE()<<16);
    FPGA_WAIT_RDY();
    val |= ((uint32_t)FPGA_RX_BYTE()<<8);
    FPGA_WAIT_RDY();
    val |= FPGA_RX_BYTE();
    buf[i++] = val;
  }
  FPGA_DESELECT();
}

uint16_t sram_readblock(void* buf, uint32_t addr, uint16_t size) {
  uint16_t count=size;
  uint8_t* tgt = buf;
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88);   /* READ */
  while(count--) {
    FPGA_WAIT_RDY();
    *(tgt++) = FPGA_RX_BYTE();
  }
  FPGA_DESELECT();
  return size;
}

uint16_t sram_readstrn(void* buf, uint32_t addr, uint16_t size) {
  uint16_t elemcount = 0;
  uint16_t count = size;
  uint8_t* tgt = buf;
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88);   /* READ */
  while(count--) {
    FPGA_WAIT_RDY();
    if(!(*(tgt++) = FPGA_RX_BYTE())) break;
    elemcount++;
  }
  tgt--;
  if(*tgt) *tgt = 0;
  FPGA_DESELECT();
  return elemcount;
}

uint16_t sram_writestrn(void* buf, uint32_t addr, uint16_t size) {
  uint16_t elemcount = 0;
  uint16_t count = size;
  uint8_t *src = buf;
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);   /* WRITE */
  if(*src) {
    while(count > 1) {
      FPGA_TX_BYTE(*src++);
      FPGA_WAIT_RDY();
      elemcount++;
      count--;
      if(!(*src)) break;
    }
  }
  FPGA_TX_BYTE(0);
  FPGA_WAIT_RDY();
  FPGA_DESELECT();
  return elemcount;
}

uint16_t sram_writeblock(void* buf, uint32_t addr, uint16_t size) {
  uint16_t count = size;
  uint8_t* src = buf;
  set_mcu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);   /* WRITE */
  while(count--) {
    FPGA_TX_BYTE(*src++);
    FPGA_WAIT_RDY();
  }
  FPGA_DESELECT();
  return size;
}

char current_filename[258];

/* Does a file exist on the SD card? (lfname=NULL like main.c:stage_patch_from_entry) */
static int file_exists(const char *path) {
  FILINFO fno;
  fno.lfname = NULL;
  return f_stat((TCHAR*)path, &fno) == FR_OK;
}

/* basename: last path component (drops "/sd2snes/"). Used so the on-screen error
   shows "dsp1b.bin", not the full path (which would overflow the popup). */
static const char *basename_of(const char *path) {
  const char *b = strrchr(path, '/');
  return b ? b + 1 : path;
}

/* Abort a game load because a required chip BIOS/firmware is missing: push the
   message to the menu's error region ($FF1000 code, $FF1001 string) and NACK the
   handshake so the menu falls into game_handshake_error and shows it WITHOUT a
   reset (it never left the menu). For a menu load (flags has no LOADROM_WAIT_SNES)
   this only clears the FS error. `name` must be a short basename/chip string.
   Always returns 0 -- call as `return load_abort_missing(...)`. */
static uint32_t load_abort_missing(uint8_t flags, int err, const char *name) {
  printf("load aborted: err=%d missing=%s\n", err, name ? name : "");
  if(flags & LOADROM_WAIT_SNES) {
    snes_menu_errmsg(err, (void*)(name ? name : ""));
    snes_set_snes_cmd(0xaa);
  }
  file_res = FR_OK;
  return 0;
}

uint32_t load_rom(uint8_t* filename, uint32_t base_addr, uint8_t flags) {
  UINT bytes_read;
  DWORD filesize;
  UINT count=0;
  uint8_t is_menu = (filename == (uint8_t*)MENU_FILENAME);
  tick_t ticksstart, ticks_total=0;
  ticksstart=getticks();

  /* NB: menu SFX teardown (menu_sfx_shutdown) is deferred to the commit point
     below -- AFTER the prerequisite check -- so that an aborted game load (missing
     chip BIOS -> NACK -> back to menu) leaves the menu sound intact. */

  // copy the full name and path
  strncpy(current_filename, (char *)filename, sizeof(current_filename)-1);

  printf("%s\n", filename);
  file_open(filename, FA_READ);
  if(file_res) {
    uart_putc('?');
    uart_putc(0x30+file_res);
    /* ROM vanished/SD glitch between selection and load: populate the error
       region so the menu's popup names this file instead of showing stale bytes
       from a previous abort (the main.c epilogue NACKs on a 0 return either way). */
    return load_abort_missing(flags, MENU_ERR_FS, basename_of((const char*)filename));
  }
  filesize = file_handle.fsize; // won't be correct for combo roms
  
  uint32_t file_offset = 0;
  if(flags & LOADROM_WITH_COMBO) {
    printf("Combo Header Check...");
    // seek to the proper slot.  slots are naturally aligned on 1MB boundaries.
    file_offset = 0x100000 * snescmd_readbyte(SNESCMD_MCU_CMD + 1);
    printf(" file_offset=0x%lx", file_offset);
    printf(" OK.\n");
  }

  /* SGB detect and file management */
  uint8_t *sgb_filename = filename;
  DWORD sgb_filesize = file_handle.fsize;
  sgb_id(&sgb_romprops, sgb_filename);
  /* SGB SNES BIOS (sgbN_snes.bin) missing -> message + NACK (else the menu would
     hang in game_handshake waiting for an ACK/NACK that never came). */
  if (!sgb_update_file(&filename)) {
    return load_abort_missing(flags, MENU_ERR_SUPPLFILE, basename_of(SGBSR));
  }

  filesize = file_handle.fsize;
  smc_id(&romprops, file_offset);
  /* On a recore reload, the file still holds the UNPATCHED header, so smc_id()
     picked the old core again.  Force the cartridge type detected from the
     patched image (ips_recore_props), but keep the file's own copier offset /
     load address / size. Those describe how to stream the file, not the
     patched cartridge type. */
  if(ips_recore_active) {
    uint32_t f_offset  = romprops.offset;
    uint32_t f_load    = romprops.load_address;
    uint32_t f_romsize = romprops.romsize_bytes;
    romprops = ips_recore_props;
    romprops.offset        = f_offset;
    romprops.load_address  = f_load;
    romprops.romsize_bytes = f_romsize;
  }
  file_close();

  if(flags & LOADROM_WITH_COMBO) {
    printf("Combo Transition...");
    uint32_t romslot = snescmd_readbyte(SNESCMD_MCU_CMD + 1);
    romprops.offset += romslot << 20;
    printf(" romslot=0x%lx", romslot);
    printf(" offset=0x%lx", romprops.offset);
    
    // force has_combo since only slot 00 has the matching carttype
    romprops.has_combo = 1;
    printf(" OK.\n");
  }

  /* SGB assign the SGB FPGA file and relocate the snes image to the 512KB RAM.
     A 0 here means the SGB SNES BIOS is PRESENT but fails the mapper/size/sram
     requirements (sgb_update_file already verified existence), so report a generic
     load error, not "file not found". */
  if (!sgb_update_romprops(&romprops, sgb_filename)) {
    return load_abort_missing(flags, MENU_ERR_FS, basename_of(SGBSR));
  }

  uint16_t fpga_features_preload = romprops.fpga_features | FEAT_CMD_UNLOCK | FEAT_2100_LIMIT_NONE;
  if(is_menu) {
    printf("Setting menu features...");
    fpga_set_features(fpga_features_preload);
    printf("OK.\n");
  }
  /* Prerequisite check BEFORE the ACK: if a required chip BIOS/firmware file is
     missing (or the chip is unimplemented), NACK the handshake so the menu shows
     the error and stays alive (no reset), instead of booting a broken game. Gated
     on LOADROM_WAIT_SNES (the SNES is parked in game_handshake able to take the
     NACK): a menu load (flags 0) never needs these files, and the IPS/BPS recore
     reload clears WAIT_SNES AFTER the SNES already ACKed/booted, so aborting there
     would only desync MCU and SNES -- let it fall through as before. */
  if(!is_menu && (flags & LOADROM_WAIT_SNES)) {
    /* unimplemented chip (ST0011/ST0018/SPC7110): smc_id already flagged it */
    if(romprops.error == MENU_ERR_NOIMPL) {
      return load_abort_missing(flags, MENU_ERR_NOIMPL, (char*)romprops.error_param);
    }
    /* FPGA core (.bit) for the enhancement chip: SA-1/GSU/CX4/OBC1/S-DD1/DSP/SGB.
       fpga_conf is NULL for plain LoROM/HiROM (those fall back to fpga_base, not
       gated here); when set it is always a real /sd2snes/fpga_*.bit path. Without
       it, fpga_pgm() fails silently and the game boots with no/wrong core. */
    if(romprops.fpga_conf && !file_exists((const char*)romprops.fpga_conf)) {
      return load_abort_missing(flags, MENU_ERR_SUPPLFILE,
                                basename_of((const char*)romprops.fpga_conf));
    }
    /* DSPx / ST0010 firmware. DSP1 may fall back to dsp1b.bin (see below). */
    if(romprops.has_dspx && romprops.dsp_fw) {
      if(!file_exists((const char*)romprops.dsp_fw)
         && !(romprops.dsp_fw == DSPFW_1 && file_exists((const char*)DSPFW_1B))) {
        return load_abort_missing(flags, MENU_ERR_SUPPLFILE,
                                  basename_of((const char*)romprops.dsp_fw));
      }
    }
    /* BS-X BIOS + data page (mapper_id 3 = BS-X Flash cart); both are loaded by
       the BS-X path below with their result ignored. */
    if(romprops.mapper_id == 3) {
      if(!file_exists("/sd2snes/bsxbios.bin")) {
        return load_abort_missing(flags, MENU_ERR_SUPPLFILE, "bsxbios.bin");
      }
      if(!file_exists("/sd2snes/bsxpage.bin")) {
        return load_abort_missing(flags, MENU_ERR_SUPPLFILE, "bsxpage.bin");
      }
    }
    /* SGB boot ROM (sgbN_boot.bin); the SNES BIOS sgbN_snes.bin was already
       checked by sgb_update_file above. */
    if(sgb_romprops.has_sgb && !file_exists(SGBFW)) {
      return load_abort_missing(flags, MENU_ERR_SUPPLFILE, basename_of(SGBFW));
    }
    /* A non-combo file too small to be a real ROM would still ACK the SNES out of
       game_handshake and then stream nothing, desyncing the handshake. Abort here
       (clean NACK while the SNES is still parked) instead of half-booting. */
    if(!(flags & LOADROM_WITH_COMBO) && filesize < 1024) {
      return load_abort_missing(flags, MENU_ERR_FS, basename_of((const char*)filename));
    }
    /* Prerequisites OK -> committed to the load: NOW tear down menu SFX so the
       DAC/SD/feature set are free for the game (deferred from the top of load_rom
       so an aborted load above keeps the menu sound alive). */
    menu_sfx_shutdown();
  }
  if(flags & LOADROM_WAIT_SNES) {
    printf("Setting cmd=0x55...");
    snes_set_snes_cmd(0x55);
    printf("OK.\n");
  }
  /* reconfigure FPGA if necessary */
  if(flags & LOADROM_WAIT_SNES) {
    printf("Checking if ok to reconfigure...");
    while(snes_get_mcu_cmd() != SNES_CMD_FPGA_RECONF);
    printf("OK.\n");
  }
  if(romprops.fpga_conf || (flags & LOADROM_WITH_FPGA)) {
    const uint8_t *fpga_conf = romprops.fpga_conf ? romprops.fpga_conf : FPGA_BASE;
    printf("reconfigure FPGA with %s...\n", fpga_conf);
    fpga_pgm((uint8_t*)fpga_conf);
    fpga_set_features(fpga_features_preload);
  }
  if(flags & LOADROM_WAIT_SNES) snes_set_snes_cmd(0x77);
  set_mcu_addr(base_addr + romprops.load_address);
  file_open(filename, FA_READ);
  ff_sd_offload=1;
  sd_offload_tgt=0;
  f_lseek(&file_handle, romprops.offset);
  uint32_t total_bytes_read = 0;
  for(;;) {
    ff_sd_offload=1;
    sd_offload_tgt=0;
    bytes_read = file_read();
    if (file_res || !bytes_read) break;
    if(!(count++ % 512)) {
      uart_putc('.');
    }
    total_bytes_read += bytes_read;
    // FIXME: can we do this in the general (non-combo) case?
    // FIXME: what does the condition below do that doubles romsize_bytes until it hits the file limit?  Do some games
    // misreport size?  Or is this for BSX?
    if((flags & LOADROM_WITH_COMBO) && (total_bytes_read >= romprops.romsize_bytes)) break;
  }
  uart_putc('\n');
  file_close();

  /* Single-pass recore (optimization): decide a cartridge-type change RIGHT AFTER
     the stream, BEFORE the expensive tail (BSX/features/SaveRAM CRC/init 196KB
     memset).  A chip-converting BPS (e.g. SMW->SA-1) does fpga_pgm under the new
     core, which WIPES the PSRAM -> all that tail work is thrown away and redone in
     pass 2.  Probing the patched header here lets us recore + apply the patch ONCE,
     skipping the whole wasted pass-1 tail.  The post-patch smc re-detect (further
     down) stays as the safety net, so a probe miss only costs time, never
     correctness. */
  if(ips_pending_index > 0 && !ips_recore_active) {
    uint32_t probe_scratch = 0;
    uint32_t probe_tgt = bps_probe_header(SRAM_IPS_LIST_ADDR, ips_pending_index,
                                          SRAM_ROM_ADDR + romprops.load_address,
                                          romprops.romsize_bytes,
                                          PATCH_PROBE_HEADER_LIMIT, &probe_scratch);
    if(probe_tgt) {
      smc_id_sdram_window(&ips_recore_props, probe_scratch, probe_tgt,
                          PATCH_PROBE_HEADER_LIMIT);
      const uint8_t* core_now = romprops.fpga_conf ? romprops.fpga_conf : FPGA_BASE;
      const uint8_t* core_new = ips_recore_props.fpga_conf ? ips_recore_props.fpga_conf
                                                           : FPGA_BASE;
      if(core_new != core_now) {
        printf("IPS: single-pass recore -> reload under correct core (skip wasted tail)\n");
        ips_recore_active = 1;
        uint32_t r = load_rom(filename, base_addr,
                              (flags & ~LOADROM_WAIT_SNES) | LOADROM_WITH_RESET);
        ips_recore_active = 0;
        if(!r) deassert_reset();
        return r;
      }
    }
  }

  printf("rom header map: %02x; mapper id: %d\n", romprops.header.map, romprops.mapper_id);
  ticks_total=getticks()-ticksstart;
  printf("%u ticks total\n", ticks_total);
  if(romprops.mapper_id==3) {
    printf("BSX Flash cart image\n");
    printf("attempting to load BSX BIOS /sd2snes/bsxbios.bin...\n");
    load_sram_offload((uint8_t*)"/sd2snes/bsxbios.bin", 0x800000, LOADRAM_AUTOSKIP_HEADER);
    printf("attempting to load BS data file /sd2snes/bsxpage.bin...\n");
    load_sram_offload((uint8_t*)"/sd2snes/bsxpage.bin", 0x900000, 0);
    printf("Type: %02x\n", romprops.header.destcode);
    set_bsx_regs(0xf6, 0x09);
    uint16_t rombase;
    if(romprops.header.ramsize & 1) {
      rombase = romprops.load_address + 0xff00;
// set_bsx_regs(0x36, 0xc9);
    } else {
      rombase = romprops.load_address + 0x7f00;
// set_bsx_regs(0x34, 0xcb);
    }
    sram_writebyte(0x33, rombase+0xda);
    sram_writebyte(0x00, rombase+0xd4);
    sram_writebyte(0x00, rombase+0xd5);
    if(CFG.bsx_use_usertime) {
      set_fpga_time(srtctime2bcdtime(CFG.bsx_time));
    } else {
      set_fpga_time(get_bcdtime());
    }
  }
  if(romprops.has_dspx) {
    printf("DSPx game. Loading firmware image %s...\n", romprops.dsp_fw);
    load_dspx(romprops.dsp_fw, romprops.fpga_features);
    /* fallback to DSP1B firmware if DSP1.bin is not present */
    if(file_res && romprops.dsp_fw == DSPFW_1) {
      load_dspx(DSPFW_1B, romprops.fpga_features);
    }
    if(file_res) {
      snes_menu_errmsg(MENU_ERR_SUPPLFILE, (void*)romprops.dsp_fw);
    }
  }
  uint32_t rammask;
  uint32_t rommask;

  while(!romprops.has_combo && romprops.romsize_bytes && filesize > (romprops.romsize_bytes + romprops.offset)) {
    romprops.romsize_bytes <<= 1;
  }

  if (romprops.has_sa1 && romprops.header.carttype == 0x36 && romprops.header.ramsize) {
    // move iram into saveram for special carts with no bwram
    romprops.header.ramsize = 1;
    romprops.ramsize_bytes = 0x800;
    // override any changes to this so we capture full sram
    romprops.srambase       = 0;
    romprops.sramsize_bytes = romprops.ramsize_bytes;
    rammask = 1;
  } else if(romprops.header.ramsize == 0) {
    rammask = 0;
  } else {
    rammask = romprops.ramsize_bytes - 1;
  }
  rommask = romprops.romsize_bytes - 1;
  
  uint8_t ramslot = 0;
  if (romprops.has_combo) {
    ramslot = sram_readbyte((romprops.mapper_id == 0 || romprops.mapper_id == 2) ? 0xFFDA : 0x7FDA);
  }
  
  printf("ramsize=%x ramslot=%hx rammask=%lx\nromsize=%x rommask=%lx\n", romprops.header.ramsize, ramslot, rammask, romprops.header.romsize, rommask);

  /* SGB setup romprops and load SRAM */
  sgb_load_sram(sgb_filename);

  /* SGB update local file properties */
  if (sgb_romprops.has_sgb) {
    /* reset the filename to match the GB file */
    filename = sgb_filename;
    filesize = sgb_filesize;

    /* update SaveRAM properties */
    romprops.ramsize_bytes = (CFG.sgb_enable_state && sgb_romprops.ramsize_bytes <= 64 * 1024) ? (128 * 1024) : sgb_romprops.ramsize_bytes;
    romprops.srambase = sgb_romprops.srambase;
    romprops.sramsize_bytes = (CFG.sgb_enable_state && sgb_romprops.ramsize_bytes <= 64 * 1024) ? (128 * 1024) : sgb_romprops.sramsize_bytes;

    rammask = sgb_romprops.ramsize_bytes ? (sgb_romprops.ramsize_bytes - 1) : 0;
    rommask = sgb_romprops.romsize_bytes ? (sgb_romprops.romsize_bytes - 1) : 0;
  }

  /* SGB load GB RTC */
  sgb_gtc_load(sgb_filename);

  printf("ramsize=%x rammask=%lx\nromsize=%x rommask=%lx\n", romprops.header.ramsize, rammask, romprops.header.romsize, rommask);
  set_saveram_mask(rammask);
  // don't set these for special chips as it may break from not supporting the feature
  if (  !romprops.fpga_conf
      || romprops.fpga_conf == FPGA_BASE
      || romprops.fpga_conf == FPGA_DSP) {
      set_saveram_base(ramslot);
  }
  set_rom_mask(rommask);
  readled(0);

  printf("gsu=%x sa1=%x srambase=%lx sramsize=%lx\n", romprops.has_gsu, romprops.has_sa1, romprops.srambase, romprops.sramsize_bytes);
  if(flags & LOADROM_WITH_SRAM) {
    if(romprops.ramsize_bytes) {
      // powerslide relies on the init value to be 00.
      sram_memset(SRAM_SAVE_ADDR, romprops.ramsize_bytes, romprops.has_gsu ? 0x00 : 0xFF);
      if (romprops.sramsize_bytes) migrate_and_load_srm(filename, SRAM_SAVE_ADDR);
      /* file not found error is ok (SRM file might not exist yet) */
      if(file_res == FR_NO_FILE) file_res = 0;
      saveram_crc_old = calc_sram_crc(SRAM_SAVE_ADDR + romprops.srambase, romprops.sramsize_bytes, 0);
      saveram_crc = 0;
      saveram_offset = 0;
    } else {
      printf("No SRAM\n");
    }
  }

  /* BS Memory Pack slot, auto-detected (no title list): needs a <rom>.mpk present.
     Two families:
       - base mapper (LoROM, or HiROM <=2MB; above that its own ROM reaches $E0+, where
         the HiROM pack maps): confirm with the pack-probe ROM scan (LDA $bb:FF00/$bb:FF02).
         The window follows the mapper (LoROM $C0-$DF / HiROM $E0-$EF).  FEAT_BSLOROM here
         for a >2MB LoROM cart with a pack; standalone (no pack) boot of Derby/SN is in smc.c.
       - SA-1 slotted (e.g. SD Gundam G Next): the game validates the pack on the S-CPU and
         reads it through SuperMMC block 4 (the SA-1 core redirects MMC block>=4 to the pack
         at PSRAM 0x900000).  It writes no flash registers, so there is no pack-probe
         signature to scan -- gate on has_sa1 + a present .mpk (an explicit user action). */
  uint8_t bs_slot = 0;
  if((flags & LOADROM_WITH_SRAM) && bs_pack_exists(filename)) {
    if(!romprops.fpga_conf
       && (romprops.mapper_id == 1
           || (romprops.mapper_id == 0 && romprops.romsize_bytes <= 0x200000))
       && rom_scan_bs_vendor(romprops.romsize_bytes)) {
      if(romprops.mapper_id == 1 && romprops.romsize_bytes > 0x200000) {
        romprops.fpga_features |= FEAT_BSLOROM;
      }
      bs_slot = 1;
    } else if(romprops.has_sa1) {
      bs_slot = 1;
    }
  }
  if(bs_slot && load_bs_pack(filename)) {
    printf("BS Memory Pack present\n");
    romprops.fpga_features |= FEAT_BSSLOT;
    /* seed the autosave baseline + reset scan state per game (globals, like
       saveram_offset) so an unchanged pack is not re-written */
    bs_pack_crc_old = calc_sram_crc(BS_PACK_ADDR, BS_PACK_SIZE, 0);
    bs_pack_crc = bs_pack_offset = bs_pack_diff = bs_pack_same = 0;
    bs_pack_didnotsave = bs_pack_save_failed = 0;
    /* sync the erase seq to the FPGA's current value so a stale seq from a previous
       game doesn't trigger a spurious erase on the first poll */
    bs_pack_erase_seq = (fpga_status() >> 11) & 0x3;
    /* RTC is the Satellaview base-unit clock (base core only); the SA-1 core has no
       BS-X base regs, so don't poke the FPGA time there. */
    if(!romprops.has_sa1) {
      if(CFG.bsx_use_usertime) {
        set_fpga_time(srtctime2bcdtime(CFG.bsx_time));
      } else {
        set_fpga_time(get_bcdtime());
      }
    }
  }

  printf("check MSU...");
  if(msu1_check(filename)) {
    romprops.fpga_features |= FEAT_MSU1;
    romprops.has_msu1 = 1;
  } else {
    romprops.has_msu1 = 0;
  }
  printf("done\n");

  printf("r213fen=%d is_u16=%d filename=%s\n", cfg_is_r213f_override_enabled(), STS.is_u16, filename);
  if(cfg_is_r213f_override_enabled() && !is_menu && !STS.is_u16) {
    romprops.fpga_features |= FEAT_213F; /* e.g. for general consoles */
  }
  fpga_set_213f(romprops.region);
//  fpga_set_features(romprops.fpga_features);
  fpga_set_chipfeat(sgb_romprops.has_sgb ? sgb_romprops.fpga_sgbfeat : romprops.fpga_dspfeat);
  fpga_set_dac_boost(CFG.msu_volume_boost);
  dac_pause();
  dac_reset(0);
/* fully enable pair mode again instead of just setting the video/d4 mode
   in case previous pair mode entry was skipped / pair mode undetected so far */
  if(get_cic_state() == CIC_PAIR) {
    if(!is_menu) {
      if(CFG.vidmode_game == VIDMODE_AUTO) {
        cic_pair(romprops.region, romprops.region);
      } else {
        cic_pair(CFG.vidmode_game, romprops.region);
      }
    }
  }

  if(cfg_is_onechip_transient_fixes() && !is_menu) {
    romprops.fpga_features |= FEAT_2100;
  }
  romprops.fpga_features |= FEAT_2100_LIMIT(cfg_get_brightness_limit());

  /* enable Satellaview Base emulation only if no physical Satellaview Base unit is present */
  if(!STS.has_satellaview) {
    romprops.fpga_features |= FEAT_SATELLABASE;
  }

  if(flags & LOADROM_WAIT_SNES) {
    while(snes_get_mcu_cmd() != SNES_CMD_RESET) cli_entrycheck();
  }

  set_mapper(sgb_romprops.has_sgb ? sgb_romprops.mapper_id : romprops.mapper_id);

  if (romprops.has_combo) {
    static uint32_t combo_srambase = 0;
    static uint32_t combo_sramsize_bytes = 0;
  
    // set version number
    snescmd_writebyte(COMBO_VERSION, SNESCMD_COMBO_VERSION);
  
    if (flags & LOADROM_WITH_COMBO) {
      // restore proper bounds
      romprops.srambase = combo_srambase;
      romprops.sramsize_bytes = combo_sramsize_bytes;
    } else {
      // base ROM.
      // set base unlock features.
      snescmd_writebyte(0x1, SNESCMD_MAP);
      // record the saveram properties
      combo_srambase = romprops.srambase;
      combo_sramsize_bytes = romprops.sramsize_bytes;
    }

    // enable use of the DMA unit
    romprops.fpga_features |= FEAT_DMA1;
  }

//printf("%04lx\n", romprops.header_address + ((void*)&romprops.header.vect_irq16 - (void*)&romprops.header));
  if(flags & (LOADROM_WITH_RESET|LOADROM_WAIT_SNES)) {
    assert_reset();
    init(filename);
    /* Apply IPS patch to the ROM in SRAM while the SNES is in hardware reset.
       ips_pending_index is set by the CMD_LOADROM handler in main.c before
       calling load_rom().  We consume+clear it here. */
    uint8_t saved_ips_idx = ips_pending_index; /* for recore reload */
    uint8_t patch_ok = 0;                      /* patch_apply succeeded */
    if(ips_pending_index > 0) {
      /* On a recore reload, fpga_pgm() above wiped all of SDRAM, including the
         patch list that ips_find_patches() staged once at SRAM_IPS_LIST_ADDR
         before LOADROM.  The patch's full path survives in MCU RAM as
         current_ips_srm_source, so re-stage it into SDRAM at the same slot
         patch_apply() reads, otherwise the re-patch would open an empty path
         and silently leave the ROM unpatched. */
      if(ips_recore_active && current_ips_srm_source[0]) {
        sram_writeblock(current_ips_srm_source,
                        SRAM_IPS_LIST_ADDR + IPS_PATH_BASE
                          + (uint32_t)(ips_pending_index - 1) * IPS_PATH_LEN,
                        (uint16_t)(strlen((char*)current_ips_srm_source) + 1));
      }

      /* (The chip-converting-BPS recore decision was MOVED earlier -- right after
         the stream, before the expensive tail -- so the wasted pass-1 tail is
         skipped; see "Single-pass recore" above.  The post-patch smc re-detect
         below stays as the safety net for a probe miss.) */

      /* Dispatch to ips_apply or bps_apply based on the patch file extension.
         For IPS: pass the copier-header size so offset correction works when
         the IPS was authored for a headered ROM.  For combo ROMs
         romprops.offset carries a slot shift in the upper bits; mask those
         off to get just the header size (0 or 0x200).
         BPS encodes exact sizes so no header correction is needed there. */
      uint32_t ips_header_size = romprops.offset & 0xFFFFF;
      /* Approach B: drive the FPGA copier from the MCU (the SNES is held in reset
         here) for a BPS when EnableBpsCopier is on AND the currently-loaded core
         carries the copier (probe).  IPS, the gate off, or a core without the copier
         fall through to the byte-by-byte apply.  A copier op failure returns 0 and is
         treated as a patch error (the probe makes a runtime failure unlikely). */
      uint32_t ips_end;
      uint8_t  copier_avail = CFG.enable_bps_copier && patch_copier_available();
      if(copier_avail) {
        extern uint32_t patch_targetread_bytes;
        printf("BPS copier path (MCU-driven, SNES in reset)\n");
        tick_t t_copier = getticks();
        ips_end = patch_apply_copier(SRAM_IPS_LIST_ADDR, ips_pending_index,
                                     SRAM_ROM_ADDR + romprops.load_address,
                                     romprops.romsize_bytes, ips_header_size);
        uint32_t patch_ms = (uint32_t)(getticks() - t_copier) * 10u; /* 1 tick = 10ms */
        /* DEBUG ($FF0724 marker, $FF0721 op count): B1 = copier applied OK,
           B2 = copier failed mid-apply (timeout). */
        sram_writebyte(ips_end ? 0xB1 : 0xB2, 0xFF0724L);
        uint32_t n = patch_copier_count();
        sram_writebyte((uint8_t)(n),       0xFF0721L);
        sram_writebyte((uint8_t)(n >> 8),  0xFF0722L);
        sram_writebyte((uint8_t)(n >> 16), 0xFF0723L);
        /* DEBUG breakdown: $FF0728 = patch time ms (16b), $FF072A = TargetRead
           byte-by-byte volume (24b) -> copier-op overhead vs literal volume. */
        sram_writebyte((uint8_t)(patch_ms),       0xFF0728L);
        sram_writebyte((uint8_t)(patch_ms >> 8),  0xFF0729L);
        sram_writebyte((uint8_t)(patch_targetread_bytes),       0xFF072AL);
        sram_writebyte((uint8_t)(patch_targetread_bytes >> 8),  0xFF072BL);
        sram_writebyte((uint8_t)(patch_targetread_bytes >> 16), 0xFF072CL);
      } else {
        ips_end = patch_apply(SRAM_IPS_LIST_ADDR, ips_pending_index,
                              SRAM_ROM_ADDR + romprops.load_address,
                              romprops.romsize_bytes, ips_header_size);
        /* B3 = gate on but copier not in this core (probe failed); B0 = gate off. */
        sram_writebyte(CFG.enable_bps_copier ? 0xB3 : 0xB0, 0xFF0724L);
      }
      ips_pending_index = 0;
      patch_ok = (ips_end > 0); /* 0 => patch error / FPGA stall */
      /* If the IPS patch wrote past the original ROM boundary (ROM expansion
         hack), expand the FPGA ROM mask so those new banks are accessible.
         romprops.romsize_bytes is always a power of 2, so a simple left-
         shift loop finds the next fitting power of 2. */
      if(ips_end > romprops.romsize_bytes) {
        /* IPS/BPS patches authored for a headered (512-byte copier prefix)
           ROM image sometimes have max_end that overshoots a clean power-of-2
           ROM boundary by exactly 512 bytes.  Those extra bytes are the
           copier header padding — not real ROM data — so they must not cause
           the mask to double.  Snap ips_end back to the clean boundary when
           the overshoot is ≤ 512 bytes. */
        if (ips_end > 512) {
          /* Largest power-of-2 that is ≤ ips_end */
          uint32_t p2 = ips_end;
          p2 |= p2 >> 1; p2 |= p2 >> 2; p2 |= p2 >> 4;
          p2 |= p2 >> 8; p2 |= p2 >> 16;
          p2 = (p2 + 1) >> 1;
          if (p2 < ips_end && ips_end - p2 <= 512) {
            printf("IPS/BPS: header padding trimmed 0x%lx -> 0x%lx\n",
                   (unsigned long)ips_end, (unsigned long)p2);
            ips_end = p2;
          }
        }
        uint32_t new_size = romprops.romsize_bytes;
        while(new_size < ips_end) new_size <<= 1;
        /* For LoROM (mapper_id 1) the FPGA address formula uses ~A23 to
           overlay the two SNES bank halves onto the same SRAM window.
           This only works correctly when the ROM mask has bit 22 clear
           (i.e. mask <= 0x3FFFFF, ROM <= 4 MB).  LoROM's addressing limit
           is 4 MB regardless of IPS expansion, so if the loop doubled past
           4 MB (typically due to a single stray IPS byte sitting just past
           the 4 MB boundary), cap new_size back to 4 MB. */
        if(romprops.mapper_id == 1 && new_size > 0x400000)
          new_size = 0x400000;
        printf("IPS ROM expansion: %lx -> %lx (mask %lx)\n",
               romprops.romsize_bytes, new_size, new_size - 1);
        romprops.romsize_bytes = new_size;
        set_rom_mask(new_size - 1);
      }
    }

    /* The FPGA core, mapper and masks were all selected from the PRE-patch
       header (smc_id ran on the unpatched file before fpga_pgm).  If the patch
       changed the cartridge type (e.g. an SA-1 / Super FX conversion hack), the
       wrong core is currently loaded and the game would boot broken.
       Re-derive the cartridge from the now-patched image in SDRAM; if it needs a
       different core, reload+repatch once under the correct core.  Reconfiguring
       the FPGA wipes SDRAM, so a full reload (re-stream + re-patch) is required
       rather than a bare fpga_pgm.  Guarded so the normal path (no patch, or a
       patch that keeps the same core) is completely unaffected. */
    if(saved_ips_idx && patch_ok && !ips_recore_active) {
      smc_id_sdram(&ips_recore_props, SRAM_ROM_ADDR + romprops.load_address,
                   romprops.romsize_bytes);
      const uint8_t* core_now = romprops.fpga_conf ? romprops.fpga_conf : FPGA_BASE;
      const uint8_t* core_new = ips_recore_props.fpga_conf ? ips_recore_props.fpga_conf
                                                           : FPGA_BASE;
      if(core_new != core_now) {
        printf("IPS: patch changed cartridge type -> reloading under correct core\n");
        ips_recore_active = 1;
        ips_pending_index = saved_ips_idx; /* re-apply the same patch on reload */
        /* Keep the SNES held in hardware reset across the reload (do NOT
           deassert here): the SNES handshake already completed on this pass, so
           we drop LOADROM_WAIT_SNES and let fpga_pgm reconfigure the FPGA while
           the SNES is safely in reset.  The recursive call ends with its own
           deassert_reset(), releasing the SNES into the correctly-cored game.
           NOTE: on the recursive pass the global (un-timeout'd) sram_* writes to
           SaveRAM/BWRAM (0xE00000) run under the chip core; they complete only
           because a fresh fpga_pgm powers up SNES_DEADr=1 and the SNES stays in
           reset the whole time, so the chip-core RAM arbiter grants the MCU
           every cycle.  Do not deassert before the reload or those would hang. */
        uint32_t r = load_rom(filename, base_addr,
                              (flags & ~LOADROM_WAIT_SNES) | LOADROM_WITH_RESET);
        ips_recore_active = 0;
        /* If the reload aborted early (before its own deassert_reset), the SNES
           is still held in reset from this pass — release it so the console is
           never left frozen with the MCU alive. */
        if(!r) deassert_reset();
        return r;
      }
      /* The patch may change only the MAPPER without changing the FPGA core —
         most commonly a HiROM -> ExHiROM promotion when an expansion grows the
         ROM past 4 MB (e.g. Bahamut Lagoon English: 3 MB HiROM -> 8 MB ExHiROM,
         Tales of Phantasia-style).  set_mapper() ran before the patch with the
         PRE-patch mapper, and the core-change branch above only fires on a core
         swap, so a same-core mapper change would otherwise leave an 8 MB ExHiROM
         image addressed as 4 MB HiROM -> the SNES reads garbage and black-
         screens.  The core is already correct and the ROM mask was expanded
         above, so just re-program the FPGA mapper register here (no reload). */
      if(ips_recore_props.mapper_id != romprops.mapper_id) {
        printf("IPS: patch changed mapper %d -> %d (same core) -> reprogramming FPGA mapper\n",
               romprops.mapper_id, ips_recore_props.mapper_id);
        romprops.mapper_id = ips_recore_props.mapper_id;
        set_mapper(romprops.mapper_id);
      }
    }
    deassert_reset();
  }
  // loading a new rom implies the previous crc is no longer valid
  sram_crc_valid = romprops.has_combo ? 1 : 0;
  sram_crc_init = 1;
  sram_crc_romsize = filesize - romprops.offset;

  return (uint32_t)filesize;
}

void assert_reset() {
  printf("resetting SNES\n");
  fpga_dspx_reset(1);
  snes_reset(1);
  if(STS.is_u16 && (STS.u16_cfg & 0x01)) {
    delay_ms(60*SNES_RESET_PULSELEN_MS);
  } else {
    delay_ms(SNES_RESET_PULSELEN_MS);
  }
}

void init(uint8_t *filename) {
  snescmd_prepare_nmihook();
  if (CFG.reset_patch) snescmd_writebyte(0, SNESCMD_RESET_HOOK+1);
  cheat_yaml_load(filename);
// XXX    cheat_yaml_save(filename);
  cheat_program();
  savestate_program();
  fpga_set_features(romprops.fpga_features);
  fpga_reset_srtc_state();
  snes_set_mcu_cmd(0);
  // init save state region - VRAM, APURAM, CGRAM, OAM only
  sram_memset(0xF70000, 0x30000, 0);
}

void deassert_reset() {
  snes_reset(0);
  fpga_dspx_reset(0);
  // handle reset loop from hook
  snes_reset_loop();
}

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
      FPGA_WAIT_RDY();
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
    FPGA_WAIT_RDY();
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
    FPGA_WAIT_RDY();
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

uint32_t load_sram_offload(uint8_t* filename, uint32_t base_addr, uint8_t flags) {
  set_mcu_addr(base_addr);
  UINT bytes_read;
  DWORD filesize;
  file_open(filename, FA_READ);
  filesize = file_handle.fsize;
  if(file_res) return 0;
  if(flags & LOADRAM_AUTOSKIP_HEADER) {
    if((filesize & 0xffff) == 0x200) {
      ff_sd_offload=1;
      f_lseek(&file_handle, 0x200L);
      printf("load_sram_offload: skipping 512b header\n");
    }
  }
  if(file_res) return 0;
  for(;;) {
    ff_sd_offload=1;
    sd_offload_tgt=0;
    bytes_read = file_read();
    if (file_res || !bytes_read) break;
  }
  file_close();
  return (uint32_t)filesize;
}

uint32_t migrate_and_load_srm(uint8_t* filename, uint32_t base_addr) {
  uint8_t srmfile[256] = SAVE_BASEDIR;
  /* When a patched load is active, derive the .srm name from the IPS file
     path instead of the ROM filename so each patch gets its own save. */
  const uint8_t *srm_src = current_ips_srm_source[0]
                            ? current_ips_srm_source
                            : filename;
  append_file_basename((char*)srmfile, (char*)srm_src, ".srm", sizeof(srmfile));
  printf("SRM file: %s\n", srmfile);

  uint32_t filesize;
  /* check for SRM file in new centralized sram folder */
  filesize = load_sram(srmfile, base_addr);
  if(file_res) {
    if(current_ips_srm_source[0]) {
      /* No old-style migration for patched ROMs; a missing save is fine. */
      return 0;
    }
    /* try to move SRM file from old place to new one and to load again */
    char *dot = strrchr((char*)filename, (int)'.');
    if(!dot) return 0;   /* ROM name has no extension: nothing to migrate (a missing save is fine) */
    strcpy(dot, ".srm");
    printf("%s not found, trying to load and migrate %s...\n", srmfile, filename);
    /* check if new sram folder exists, create it if it doesn't */
    check_or_create_folder(SAVE_BASEDIR);
    f_rename((TCHAR*)filename, (TCHAR*)srmfile);
    filesize = load_sram(srmfile, base_addr);
    if(file_res) {
      print_fresult(file_res, "migrate_and_load_sram: could not open %s\n", srmfile);
      return 0;
    }
  }
  return (uint32_t)filesize;
}

uint32_t load_sram(uint8_t* filename, uint32_t base_addr) {
  UINT bytes_read;
  DWORD filesize;

  set_mcu_addr(base_addr);
  file_open((uint8_t*)filename, FA_READ);
  if(file_res) {
    printf("load_sram: could not open %s, res=%d\n", filename, file_res);
    return 0;
  }
  for(;;) {
    bytes_read = file_read();
    if (file_res || !bytes_read) break;
    FPGA_SELECT();
    FPGA_TX_BYTE(0x98);
    for(int j=0; j<bytes_read; j++) {
      FPGA_TX_BYTE(file_buf[j]);
      FPGA_WAIT_RDY();
    }
    FPGA_DESELECT();
  }
  file_close();
  return (uint32_t)filesize;
}

uint32_t load_sram_rle(uint8_t* filename, uint32_t base_addr) {
  uint8_t data;
  set_mcu_addr(base_addr);
  DWORD filesize;
  file_open(filename, FA_READ);
  filesize = file_handle.fsize;
  if(file_res) return 0;
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);
  for(;;) {
    data = rle_file_getc();
    if (file_res || file_status) break;
    FPGA_TX_BYTE(data);
    FPGA_WAIT_RDY();
  }
  FPGA_DESELECT();
  file_close();
  return (uint32_t)filesize;
}

uint32_t load_bootrle(uint32_t base_addr) {
  uint8_t data;
  set_mcu_addr(base_addr);
  DWORD filesize = 0;
  rle_mem_init(bootrle, sizeof(bootrle));

  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);
  for(;;) {
    data = rle_mem_getc();
    if(rle_state) break;
    FPGA_TX_BYTE(data);
    FPGA_WAIT_RDY();
    filesize++;
  }
  FPGA_DESELECT();
  return (uint32_t)filesize;
}

/* build the SD save path (SAVE_BASEDIR + basename + ext) into buf, honoring an active
   IPS/BPS patch source -- shared by the .srm and .mpk paths so they can't drift */
static void append_save_basename(char *buf, size_t buflen, uint8_t *filename, const char *ext) {
  const uint8_t *src = current_ips_srm_source[0] ? current_ips_srm_source : filename;
  append_file_basename(buf, (char*)src, (char*)ext, buflen);
}

/* is there a <rom>.mpk?  cheap f_stat that gates the ROM scan below */
static uint8_t bs_pack_exists(uint8_t *filename) {
  uint8_t bsfile[256] = SAVE_BASEDIR;
  append_save_basename((char*)bsfile, sizeof(bsfile), filename, ".mpk");
  return file_exists((const char*)bsfile);
}

/* BS slot auto-detect: scan the staged ROM for the pack-probe vendor read
   (LDA $bb:FF00 then LDA $bb:FF02 within 24 bytes, bb>=$C0).  Returns the vendor bank
   ($C0/$C1 LoROM, $E0 HiROM) or 0.  SA-1 / normal carts have no such pattern.  SNES is
   in reset during load, so the raw PSRAM read is stable. */
static uint8_t rom_scan_bs_vendor(uint32_t size) {
  uint8_t w0=0, w1=0, w2=0, w3=0; /* sliding 4-byte window, w3 = newest */
  uint8_t pend=0; uint16_t cd=0;  /* pending AF 00 FF bb + countdown to its $FF02 */
  uint8_t found=0;
  set_mcu_addr(0);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_READMEM | FPGA_MEM_AUTOINC);
  for(uint32_t i=0; i<size; i++) {
    FPGA_WAIT_RDY();
    w0=w1; w1=w2; w2=w3; w3=FPGA_RX_BYTE();
    if(w0==0xAF && w2==0xFF && w3>=0xC0) {
      if(w1==0x00) { pend=w3; cd=24; }
      else if(w1==0x02 && cd && w3==pend) { found=w3; break; }
    }
    if(cd) cd--;
  }
  FPGA_DESELECT();
  return found;
}

void save_srm(uint8_t* filename, uint32_t sram_size, uint32_t base_addr) {
    char srmfile[256] = SAVE_BASEDIR;
    check_or_create_folder(SAVE_BASEDIR);
    append_save_basename(srmfile, sizeof(srmfile), filename, ".srm");
    save_sram((uint8_t*)srmfile, sram_size, base_addr);
}

/* stage <rom>.mpk into PSRAM at BS_PACK_ADDR.  returns 1 if a pack loaded, 0 = empty
   slot (no file -> nothing mapped, game boots standalone).  .mpk not .bs (.bs is a
   bootable BS-X ROM type in the browser). */
uint8_t load_bs_pack(uint8_t* filename) {
  uint8_t bsfile[256] = SAVE_BASEDIR;
  FILINFO fno;
  append_save_basename((char*)bsfile, sizeof(bsfile), filename, ".mpk");
  if(!file_exists((const char*)bsfile)) {
    printf("no pack (%s); empty slot\n", bsfile);
    return 0;
  }
  printf("BS pack file: %s\n", bsfile);
  load_sram(bsfile, BS_PACK_ADDR);
  /* clear only the tail past a short .mpk (a full 1MB one overwrites the window) */
  fno.lfname = NULL;
  if(f_stat((TCHAR*)bsfile, &fno) == FR_OK && fno.fsize < BS_PACK_SIZE) {
    sram_memset(BS_PACK_ADDR + fno.fsize, BS_PACK_SIZE - fno.fsize, 0x00);
  }
  file_res = 0;
  printf("pack loaded\n");
  return 1;
}

void save_bs_pack(uint8_t* filename) {
  char bsfile[256] = SAVE_BASEDIR;
  check_or_create_folder(SAVE_BASEDIR);
  append_save_basename(bsfile, sizeof(bsfile), filename, ".mpk");
  save_sram((uint8_t*)bsfile, BS_PACK_SIZE, BS_PACK_ADDR);
}

void save_sram(uint8_t* filename, uint32_t sram_size, uint32_t base_addr) {
  uint32_t count = 0;
  uint32_t remain = sram_size;
  size_t copy;
  FPGA_DESELECT();
  file_open(filename, FA_CREATE_ALWAYS | FA_WRITE);
  if(file_res) {
    uart_putc(0x30+file_res);
    return;
  }
  set_mcu_addr(base_addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88); /* read */
  while(remain) {
    copy = (remain > 512) ? 512 : remain;
    for(int j=0; j < copy; j++) {
      FPGA_WAIT_RDY();
      file_buf[j] = FPGA_RX_BYTE();
      count++;
    }
    file_write(copy);
    if(file_res) {
      uart_putc(0x30+file_res);
      return;
    }
    remain -= copy;
  }
  FPGA_DESELECT();
  file_close();
}

uint32_t calc_sram_crc(uint32_t base_addr, uint32_t size, uint32_t crc) {
  uint8_t data;
  uint32_t count;
  crc_valid=1;
  set_mcu_addr(base_addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_READMEM | FPGA_MEM_AUTOINC);
  for(count=0; count<size; count++) {
    FPGA_WAIT_RDY();
    data = FPGA_RX_BYTE();
    if(get_snes_reset()) {
      crc_valid = 0;
      sram_crc_valid = romprops.has_combo ? 1 : 0;
      sram_crc_init = 1;
      break;
    }
    crc = crc32_update(crc, data);
  }
  FPGA_DESELECT();
  return crc;
}

/* CRC the 1MB pack -- like calc_sram_crc but no get_snes_reset bail (prepare_reset
   holds the SNES in reset, so the read is stable) */
uint32_t calc_pack_crc_inreset(void) {
  uint32_t crc = 0;
  set_mcu_addr(BS_PACK_ADDR);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_READMEM | FPGA_MEM_AUTOINC);
  for(uint32_t i = 0; i < BS_PACK_SIZE; i++) {
    FPGA_WAIT_RDY();
    crc = crc32_update(crc, FPGA_RX_BYTE());
  }
  FPGA_DESELECT();
  return crc;
}

uint8_t sram_reliable() {
  uint16_t score=0;
  uint32_t val;
  uint8_t result = 0;
  for(uint16_t i = 0; i < SRAM_RELIABILITY_SCORE; i++) {
    val=sram_readlong(SRAM_SCRATCHPAD);
    if(val==0x12345678) {
      score++;
    } else {
      printf("i=%d val=%08lX\n", i, val);
    }
  }
  if(score<SRAM_RELIABILITY_SCORE) {
    result = 0;
/* dprintf("score=%d\n", score); */
  } else {
    result = 1;
  }
  rdyled(result);
  return result;
}

void sram_memset(uint32_t base_addr, uint32_t len, uint8_t val) {
  set_mcu_addr(base_addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);
  for(uint32_t i=0; i<len; i++) {
    FPGA_TX_BYTE(val);
    FPGA_WAIT_RDY();
  }
  FPGA_DESELECT();
}

void load_dspx(const uint8_t *filename, uint8_t coretype) {
  UINT bytes_read;
  uint16_t word_cnt;
  uint8_t wordsize_cnt = 0;
  uint16_t sector_remaining = 0;
  uint16_t sector_cnt = 0;
  uint16_t pgmsize = 0;
  uint16_t datsize = 0;
  uint32_t pgmdata = 0;
  uint16_t datdata = 0;

  if(coretype & FEAT_ST0010) {
    datsize = 1536;
    pgmsize = 2048;
  } else if (coretype & FEAT_DSPX) {
    datsize = 1024;
    pgmsize = 2048;
  } else {
    printf("load_dspx: unknown core (%02x)!\n", coretype);
  }

  file_open((uint8_t*)filename, FA_READ);
  if(file_res) {
    printf("Could not read %s: error %d\n", filename, file_res);
    return;
  }

  fpga_reset_dspx_addr();

  for(word_cnt = 0; word_cnt < pgmsize;) {
    if(!sector_remaining) {
      bytes_read = file_read();
      if(!bytes_read) break;   /* truncated firmware: stop before sector_remaining underflows to 0xffff */
      sector_remaining = bytes_read;
      sector_cnt = 0;
    }
    pgmdata = (pgmdata << 8) | file_buf[sector_cnt];
    sector_cnt++;
    wordsize_cnt++;
    sector_remaining--;
    if(wordsize_cnt == 3){
      wordsize_cnt = 0;
      word_cnt++;
      fpga_write_dspx_pgm(pgmdata);
    }
  }

  wordsize_cnt = 0;
  if(coretype & FEAT_ST0010) {
    file_seek(0xc000);
    sector_remaining = 0;
  }

  for(word_cnt = 0; word_cnt < datsize;) {
    if(!sector_remaining) {
      bytes_read = file_read();
      if(!bytes_read) break;   /* truncated firmware: stop before sector_remaining underflows to 0xffff */
      sector_remaining = bytes_read;
      sector_cnt = 0;
    }
    datdata = (datdata << 8) | file_buf[sector_cnt];
    sector_cnt++;
    wordsize_cnt++;
    sector_remaining--;
    if(wordsize_cnt == 2){
      wordsize_cnt = 0;
      word_cnt++;
      fpga_write_dspx_dat(datdata);
    }
  }

  fpga_reset_dspx_addr();

  file_close();

}
