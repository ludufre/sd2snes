#include "config.h"
#include "fileops.h"
#include "uart.h"
#include "memory.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "snes.h"
#include "cheat.h"
#include "yaml.h"
#include "cfg.h"
#include "sgb.h"

#include <string.h>
#include <stdlib.h>

extern cfg_t CFG;
extern sgb_romprops_t sgb_romprops;
extern snes_romprops_t romprops;

uint8_t rom_index;
uint8_t wram_index;
uint8_t enable_mask;

uint8_t cheat_is_wram_cheat(uint32_t code) {
  return ((code & 0xfe000000) == 0x7e000000)
        || (!(code & 0x40000000)
            && ((code & 0xffff00) < 0x200000));
}

/* Write a code's display string into the PSRAM string region.
   Slot: SRAM_CHEAT_CODE_STRINGS_ADDR + cheat_idx*512 + code_idx*12.
   Each slot is 12 bytes: 9 visible chars (padded with spaces),
   then 3 trailing nulls. The SNES editor menu prints the first 9
   chars; the trailing nulls let save logic tell empty slots
   (buf[0]==0) apart from real ones. */
static void cheat_write_code_string(int cheat_idx, int code_idx, const char *s) {
  if(cheat_idx < 0 || cheat_idx >= 512) return;
  if(code_idx < 0 || code_idx >= CHEAT_NUM_CODES_PER_CHEAT) return;

  char buf[12];
  int len = 0;
  memset(buf, 0, sizeof(buf));
  if(s) {
    /* stop at null, '#' (yaml inline comment), or whitespace at end */
    while(s[len] && s[len] != '#' && len < 11) len++;
    while(len > 0 && (s[len-1] == ' ' || s[len-1] == '\t')) len--;
    if(len > 9) len = 9;
    memcpy(buf, s, len);
  }
  /* pad to 9 visible chars */
  for(int j = len; j < 9; j++) buf[j] = ' ';

  uint32_t slot = SRAM_CHEAT_CODE_STRINGS_ADDR
                + 512u * (uint32_t)cheat_idx
                + 12u  * (uint32_t)code_idx;
  sram_writeblock(buf, slot, sizeof(buf));
}

/* Read a code's display string into the supplied 12-byte buffer.
   Trims trailing spaces and null-terminates. Returns the trimmed
   length; 0 means the slot is empty (never populated). */
static int cheat_read_code_string(int cheat_idx, int code_idx, char *out) {
  uint32_t slot = SRAM_CHEAT_CODE_STRINGS_ADDR
                + 512u * (uint32_t)cheat_idx
                + 12u  * (uint32_t)code_idx;
  sram_readblock(out, slot, 12);
  int len = 0;
  while(len < 9 && out[len] != 0) len++;
  while(len > 0 && out[len-1] == ' ') len--;
  out[len] = 0;
  return len;
}

/* Decode common HTML character entity references in place. Some
   community cheat YAML files (gamehacking.org dumps in particular)
   contain entities like &quot; in description strings; the menu would
   otherwise show them verbatim.  The implementation lives in yaml.c and is
   shared with the patch metadata reader, so both sides accept the same forms. */

void cheat_init(void) {
  rom_index = 0;
  wram_index = 0;
  enable_mask = 0;
  snescmd_writebyte(ASM_RTS, SNESCMD_WRAM_CHEATS);
}

void cheat_program() {
  cheat_record_t cheat;
  uint32_t cheat_record_addr = SRAM_CHEAT_ADDR;
  int cheat_count;
  int cheat_index;

  cheat_count = sram_readshort(SRAM_NUM_CHEATS);
  if(cheat_count > CHEAT_RECORD_MAX) cheat_count = CHEAT_RECORD_MAX;

  printf("cheat_program: %d cheats present\n", cheat_count);
  /* get list of activated cheats from menu */
  cheat_init(); /* reset counters and state */
  for(cheat_index = 0; cheat_index < cheat_count; cheat_index++) {
    sram_readblock(&cheat, cheat_record_addr, sizeof(cheat_record_t));
    if(cheat.flags & CHEAT_FLAG_ENABLE) {
      int np = cheat.numpatches;
      if(np > CHEAT_NUM_CODES_PER_CHEAT) np = CHEAT_NUM_CODES_PER_CHEAT;   /* patches[] is fixed-size */
      for(int patch_index = 0; patch_index < np; patch_index++) {
        cheat_program_single(cheat.patches+patch_index);
      }
    }
    cheat_record_addr += 512;
  }
  /* put number of WRAM cheats + enable flag */
  snescmd_writebyte(wram_index, SNESCMD_NMI_WRAM_PATCH_COUNT);
  printf("enable mask=%02x\n", enable_mask);
  fpga_write_cheat(6, enable_mask);
  cheat_enable(CFG.enable_cheats);
  cheat_nmi_enable(CFG.enable_ingame_hook);
  cheat_irq_enable((romprops.has_gsu && !strncmp((char *)romprops.header.name, "DOOM", strlen("DOOM"))) ? 0 : CFG.enable_ingame_hook);
  cheat_holdoff_enable(CFG.enable_hook_holdoff);
  cheat_buttons_enable(CFG.enable_ingame_buttons);
  cheat_wram_present(wram_index);

  /* Arm the in-game cheat overlay (snes/savestate.a65 probe reads this byte).
     This byte carries ONLY the user toggle. The chip/core gating lives in
     savestate.c (savestate_program): it installs the handler that runs this probe
     only on FPGA cores that actually have the snapshot machinery the overlay reuses
     (the base and DSP cores -- ctx.v copier + PPU/VRAM/CGRAM mirrors). On the
     SA-1/GSU/CX4/OBC1/S-DD1/SPC7110/SGB cores the handler is not installed, so this
     byte is never read there. Unlike an in-game savestate, the overlay never
     snapshots/restores the coprocessor's internal state -- it only freezes the CPU
     and saves/restores the PPU it took over -- which is why it is safe on the DSP
     core (e.g. Super Mario Kart) even though savestates are not. */
  sram_writebyte(CFG.enable_cheat_overlay ? 1 : 0, SRAM_CHEAT_OVL_GATE_ADDR);

  /* Combo plus its complement: a new m3nu.bin can run against an old MCU that never writes
     here, so ss_init needs a way to tell a real publication from leftovers. */
  sram_writeshort(CFG.ingame_buttons_menu, SRAM_MENU_COMBO_ADDR);
  sram_writeshort((uint16_t)~CFG.ingame_buttons_menu, SRAM_MENU_COMBO_INV_ADDR);

  /* The SA-1 core arms its IRQ redirect on this; other cores ignore the command. */
  fpga_set_ovl_combo(CFG.ingame_buttons_menu);

  /* Mirror of the master cheat switch for the in-game CHEATS tab (X). The switch
     itself lives in the FPGA (cheat_enable, set above); this byte only lets the
     shell draw the current state and toggle from it. Re-published here so a
     CMD_CHEAT_REPROGRAM (overlay close) can never leave the UI out of sync with
     CFG.enable_cheats. */
  sram_writebyte(CFG.enable_cheats ? 1 : 0, SRAM_CHEAT_MASTER_ADDR);

  sgb_cheat_program();
}

void cheat_program_single(cheat_patch_record_t *cheat) {
  uint8_t is_wram_cheat;
  /* determine ROM or WRAM cheat */
  is_wram_cheat = cheat_is_wram_cheat(cheat->code);
  /* apply cheat to FPGA / NMI hook */
  if(is_wram_cheat) {
    if(wram_index < CHEAT_WRAM_MAX) cheat_program_ram_cheat(wram_index++, cheat);
  } else if(cheat_rom_psram_mode()) {
    /* ROM codes are patched straight into the image (no comparator slots);
       applied at deassert_reset / in-game reprogram via cheat_rom_psram_apply */
  } else if(rom_index < 6) {
    enable_mask |= (1 << rom_index);
    cheat_program_rom_cheat(rom_index++, cheat);
  }
}

void cheat_program_rom_cheat(int index, cheat_patch_record_t *cheat) {
  uint32_t code = cheat->code;
  printf("ROM cheat #%d: %04lx\n", index, cheat->code);
  fpga_write_cheat(index, code);
}

void cheat_program_ram_cheat(int index, cheat_patch_record_t *cheat) {
  uint16_t address = SNESCMD_WRAM_CHEATS + 6 * index;
  fpga_set_snescmd_addr(address);
  fpga_write_snescmd(ASM_LDA_IMM);
  fpga_write_snescmd(cheat->fields.patchvalue);
  fpga_write_snescmd(ASM_STA_ABSLONG);
  fpga_write_snescmd(cheat->fields.patchaddr & 0xff);
  fpga_write_snescmd(cheat->fields.patchaddr >> 8);
  fpga_write_snescmd(cheat->fields.patchbank);
  fpga_write_snescmd(ASM_RTS);
  printf("RAM cheat #%d: %02x%04x %02x\n", index, cheat->fields.patchbank, cheat->fields.patchaddr, cheat->fields.patchvalue);
}

/* ---- ROM cheats without FPGA comparators: patch the loaded image ---------
   On the mk2 SA-1 core the six bus comparators were removed to make room for
   the in-game cheat overlay, so ROM codes are applied by writing the byte
   straight into the ROM image in PSRAM (original byte stashed in the record's
   spare tail for restore-on-disable). This is stronger than the comparators
   on SA-1: it covers address mirrors and is visible to the SA-1 coprocessor's
   own fetches, which never went through the comparators at all. Trade-off:
   the bus->offset translation uses the SuperMMC reset-default banking (same
   mapping code databases assume); a game that rebanks a patched region at
   runtime diverges from strict bus semantics (rare, documented). */

uint8_t cheat_rom_psram_mode(void) {
#if defined(CONFIG_MK2)
  /* Mk.II SA-1, GSU and CX4 cut their FPGA ROM-cheat comparators to fit (SA-1 for
     the overlay, GSU and CX4 for the full savestate), so ROM cheats are applied to
     the PSRAM image here instead. */
  return romprops.fpga_conf == FPGA_SA1
      || romprops.fpga_conf == FPGA_GSU
      || romprops.fpga_conf == FPGA_CX4;
#elif defined(CHEAT_PSRAM_FORCE_SA1)
  /* mk3 validation build: exercise the PSRAM path with the comparators idle */
  return romprops.fpga_conf == FPGA_SA1;
#else
  return 0;
#endif
}

/* Translate a ROM-code bus address into an offset inside the loaded image,
   or -1 when the address is not ROM-backed. Mirrors collapse through the
   ROM size mask exactly like the FPGA's ROM_MASK. */
static int32_t cheat_rom_code_offset(uint32_t addr) {
  uint32_t bank = (addr >> 16) & 0xff;
  uint32_t ofs  = addr & 0xffff;
  uint32_t off;
  uint32_t mask = romprops.romsize_bytes ? (romprops.romsize_bytes - 1) : 0x3fffff;
  if(romprops.has_sa1) {
    if(bank >= 0xc0) {
      /* $C0-$FF:0000-FFFF, SuperMMC defaults xxb={0,1,2,3} = linear 4MB */
      off = ((bank - 0xc0) << 16) | ofs;
    } else if(!(bank & 0x40) && (ofs & 0x8000)) {
      /* $00-3F/$80-BF:8000-FFFF, default block = {A23,A21} */
      uint32_t blk = ((bank & 0x80) >> 6) | ((bank & 0x20) >> 5);
      off = (blk << 20) | ((bank & 0x1f) << 15) | (ofs & 0x7fff);
    } else return -1;
  } else if(romprops.fpga_conf == FPGA_GSU) {   /* GSU hybrid Lo/Hi map (address.v) */
    if(bank & 0x40) {
      /* $40-5F/$C0-DF:0000-FFFF -> SNES_ADDR[21:0]; $60-7D/$E0-FF are SAVERAM */
      if((bank & 0x60) == 0x60) return -1;
      off = addr & 0x3fffff;
    } else {
      /* $00-3F/$80-BF -> SNES_ADDR[14:0] (both halves); $6000-7FFF is SAVERAM */
      if((ofs & 0xe000) == 0x6000) return -1;
      off = ((bank & 0x7f) << 15) | (ofs & 0x7fff);
    }
  } else if(romprops.fpga_conf == FPGA_CX4) {   /* CX4: plain LoROM (cx4/address.v:79-80) */
    /* offsets < $8000 are never ROM in this map: $6000-$7FFF is the CX4 MMIO
       (address.v:91) and $70-$77:0000-$7FFF is SAVERAM (address.v:65). */
    if(!(ofs & 0x8000)) return -1;
    off = ((bank & 0x7f) << 15) | (ofs & 0x7fff);
  } else if(romprops.mapper_id == 1) {          /* LoROM (smc.c sets mapper_id=1 for (Ex)LoROM) */
    if(!(ofs & 0x8000)) return -1;
    off = ((bank & 0x7f) << 15) | (ofs & 0x7fff);
  } else if(romprops.mapper_id == 0) {          /* HiROM (smc.c sets mapper_id=0 for HiROM) */
    if(bank >= 0x40 && bank <= 0x7d) off = addr & 0x3fffff;
    else if(bank >= 0xc0)            off = addr & 0x3fffff;
    else if(!(bank & 0x40) && (ofs & 0x8000)) off = ((bank & 0x3f) << 16) | ofs;
    else return -1;
  } else return -1;                             /* ExHiROM/BSX: unsupported */
  return (int32_t)(off & mask);
}

void cheat_rom_psram_apply(void) {
  if(!cheat_rom_psram_mode()) return;
  int count = sram_readshort(SRAM_NUM_CHEATS);
  if(count < 0) count = 0;
  if(count > CHEAT_RECORD_MAX) count = CHEAT_RECORD_MAX;
  for(int i = 0; i < count; i++) {
    uint32_t rec = SRAM_CHEAT_ADDR + 512u * (uint32_t)i;
    uint8_t flags = sram_readbyte(rec);
    uint8_t np = sram_readbyte(rec + 255);
    if(np > CHEAT_NUM_CODES_PER_CHEAT) np = CHEAT_NUM_CODES_PER_CHEAT;
    uint8_t want = CFG.enable_cheats && (flags & CHEAT_FLAG_ENABLE);
    for(uint8_t c = 0; c < np; c++) {
      uint32_t code;
      sram_readblock(&code, rec + 256 + 4u * c, 4);
      if(cheat_is_wram_cheat(code)) continue;   /* WRAM codes stay hook-based */
      int32_t off = cheat_rom_code_offset(code >> 8);
      if(off < 0) continue;
      uint32_t tgt = (uint32_t)off;             /* image loads at PSRAM 0 */
      uint8_t applied = sram_readbyte(rec + CHEAT_REC_APPLIED_OFS + c);
      if(want && applied != 1) {
        sram_writebyte(sram_readbyte(tgt), rec + CHEAT_REC_ORIG_OFS + c);
        sram_writebyte(code & 0xff, tgt);
        sram_writebyte(1, rec + CHEAT_REC_APPLIED_OFS + c);
      } else if(!want && applied == 1) {
        sram_writebyte(sram_readbyte(rec + CHEAT_REC_ORIG_OFS + c), tgt);
        sram_writebyte(0, rec + CHEAT_REC_APPLIED_OFS + c);
      }
    }
  }
}

void cheat_load_to_menu(int index, cheat_record_t *cheat) {
  uint32_t offset = SRAM_CHEAT_ADDR + 512 * index;
  sram_writeblock(cheat, offset, sizeof(cheat_record_t));
  sram_writeblock(cheat->patches, offset+256, cheat->numpatches*4);
}

void cheat_save_from_menu(int index, cheat_record_t *cheat) {
  uint32_t offset = SRAM_CHEAT_ADDR + 512 * index;
  sram_readblock(cheat, offset, sizeof(cheat_record_t)-4);
  if(cheat->numpatches > CHEAT_NUM_CODES_PER_CHEAT) cheat->numpatches = CHEAT_NUM_CODES_PER_CHEAT;   /* patches[] is fixed-size */
  sram_readblock(cheat->patches, offset+256, cheat->numpatches*4);
}

void cheat_enable(int enable) {
  uint16_t flags;
  /* switch ROM cheats */
  printf("cheat_enable->%d\n", enable);
  flags = (enable ? 0x0001 : 0x0100);
  fpga_write_cheat(7, flags);
  /* switch WRAM cheats */
  snescmd_writebyte(enable ? 0 : 1, SNESCMD_NMI_DISABLE_WRAM);
}

void cheat_nmi_enable(int enable) {
  uint16_t flags;
  printf("nmi_enable->%d\n", enable);
  flags = (enable ? 0x0002 : 0x0200);
  fpga_write_cheat(7, flags);
}

void cheat_irq_enable(int enable) {
  uint16_t flags;
  printf("irq_enable->%d\n", enable);
  flags = (enable ? 0x0004 : 0x0400);
  fpga_write_cheat(7, flags);
}

void cheat_holdoff_enable(int enable) {
  uint16_t flags;
  printf("holdoff_enable->%d\n", enable);
  flags = (enable ? 0x0008 : 0x0800);
  fpga_write_cheat(7, flags);
}

void cheat_buttons_enable(int enable) {
  uint16_t flags;
  printf("buttons_enable->%d\n", enable);
  flags = (enable ? 0x0010 : 0x1000);
  fpga_write_cheat(7, flags);
}

void cheat_wram_present(int enable) {
  uint16_t flags;
  printf("wram_present->%d\n", enable);
  flags = (enable ? 0x0020 : 0x2000);
  fpga_write_cheat(7, flags);
}

/* Build the cheat window title into PSRAM and open the cheat YAML
   file.  Kept in a SEPARATE function from cheat_yaml_load on purpose: its
   title[64] + line[256] scratch (~320 B) is then popped before the per-cheat
   parse loop runs.  That loop drives the yaml parser, whose chain
   (cheat_record_t + yaml_token_t + candidate buffer) already nearly fills the
   LPC1756's ~1976-byte stack; keeping these buffers live across it overflowed
   into .bss and hung the cheat menu (esp. via the deeper recents/favorites
   path).  Sets the global file_res. */
static void cheat_yaml_title_and_open(uint8_t* romfilename) {
  char line[256];

/* Build the title source at SRAM_CHEAT_TITLE_ADDR ($D80000) so the SNES
  menu can compose its window title from it. Always written, even if the
  YAML is missing/empty. The address is well past any plausible cheat
  record (cheats live at $D00000+512*N for N up to 511, spanning banks
  D0..D3, and the per-code display strings start at $D40000), and is in
  the same PSRAM region as the cheat records, reachable from both MCU and
  SNES.
  v2 layout = marker byte, then the bare basename with its extension
  stripped. The "Cheats for " prefix and the clipping to the window width
  belong to the menu: it owns the localized text and knows the geometry,
  so neither is baked into a firmware string any more. */
  {
    char title[64];
    memset(title, 0, sizeof(title));
    const char *p = strrchr((const char*)romfilename, '/');
    p = p ? p + 1 : (const char*)romfilename;
    title[0] = SRAM_CHEAT_TITLE_MARKER;
    int copy_max = sizeof(title) - 2;    /* marker + terminator */
    int n = strlen(p);
    if (n > copy_max) n = copy_max;
    memcpy(title + 1, p, n);
    title[1 + n] = 0;
    /* strip trailing extension (.smc/.sfc/.fig/etc.) if any */
    char *dot = strrchr(title + 1, '.');
    if (dot) *dot = 0;

    sram_writeblock(title, SRAM_CHEAT_TITLE_ADDR, sizeof(title));
  }

  /* READ path: no directory creation. Under the flat layout this harmlessly re-created
     /sd2snes/cheats; with buckets it would create an empty <BB>/ on every game load and litter
     the card with hundreds of empty directories. */
  path_asset(line, sizeof(line), CHEAT_BASEDIR, (const char*)romfilename, ".yml");
  printf("Cheat YAML file: %s\n", line);
  yaml_file_open(line, FA_READ);
}

/* read cheats from YAML file to ROM for menu usage */
void cheat_yaml_load(uint8_t* romfilename) {
  yaml_token_t token;
  cheat_record_t cheat;

  cheat_yaml_title_and_open(romfilename);
  if(file_res) {
    printf("no cheat list YML found\n");
    sram_writeshort(0, SRAM_NUM_CHEATS);
    sram_writeshort(0, SRAM_CHEAT_WIN_BASE_ADDR); /* no cheats -> resident window base 0 (never stale) */
    file_res = 0; /* soft fail, suppress LED blink */
    return;
  }
  /* read cheat entries */
  int cheat_idx = 0;
  while(yaml_next_item()) {
    if(cheat_idx >= CHEAT_RECORD_MAX) break;   /* records region holds 512; matches menu cap */
    int i=0;
    /* Defensive: zero the local cheat record at the start of each
       iteration so a parse failure on any field cannot leak data from
       the previous iteration. */
    memset(&cheat, 0, sizeof(cheat));
    if(yaml_get_itemvalue("Name", &token)) {
      strncpy(cheat.description, token.stringvalue, 254);
      cheat.description[253] = 0;
      /* Some YAML sources (gamehacking.org in particular) contain HTML
         character entity references in cheat descriptions, e.g. &quot;
         instead of a literal " character. Decode the common ones in
         place so the menu shows the text the way a human would expect. */
      yaml_decode_entities(cheat.description);
    }
    /* An empty Name stays empty all the way to the menu, which substitutes its own
       localized placeholder when it draws the row. Nothing here may invent text: the
       firmware would have to know the language, and cheat_yaml_save would then have to
       recognise the placeholder again to keep a load/save round trip lossless. */
    printf("%s\n", token.stringvalue);
    yaml_get_itemvalue("Enabled", &token);
    cheat.flags = (token.boolvalue ? 0x80 : 0x00);
    printf("  enabled: %d\n", token.boolvalue);
    yaml_get_itemvalue("Code", &token);
    if(token.type == YAML_LIST_START) {
      for(i=0; i < CHEAT_NUM_CODES_PER_CHEAT; i++) {
        if(!yaml_get_next(&token)) break;
        if(token.type == YAML_LIST_END) break;
        cheat.patches[i].code = cheat_str2bin(token.stringvalue);
        cheat_write_code_string(cheat_idx, i, token.stringvalue);
      }
      cheat.numpatches = i;
    } else if (token.type != YAML_NONE) {
      cheat.patches[0].code = cheat_str2bin(token.stringvalue);
      cheat_write_code_string(cheat_idx, 0, token.stringvalue);
      cheat.numpatches = 1;
    } else {
      /* empty list */
      cheat.numpatches = 0;
    }
    printf("  num codes: %d\n", cheat.numpatches);
    for(i=0; i<cheat.numpatches; i++) {
      printf("  - %08lX\n", cheat.patches[i].code);
    }
    /* a single cheat + codes have been read, put in RAM */
    cheat_load_to_menu(cheat_idx, &cheat);
    /* Mirror the flag byte to BSRAM so the SNES side has a writable
       memory it can XOR for the visual toggle without an MCU round
       trip. The PSRAM record at $D00000+512*idx remains the canonical
       state that save reads. */
    sram_writebyte(cheat.flags, SRAM_CHEAT_FLAGS_ADDR + cheat_idx);
    /* Stage the base-0 name window (first CHEAT_NAME_INGAME_MAX descriptions) into the
       SNES-visible BSRAM window ($FF8000) so the in-game cheat overlay shows the first page
       instantly: the canonical PSRAM record at $D00000 is the game's own ROM during gameplay,
       unreachable from the overlay. Scrolling past this window slides it via
       CMD_CHEAT_NAMES_WINDOW (cheat_stage_names_window) -> this stays base-0 only, so game-load
       cost is unchanged even though the overlay can list ALL cheats. */
    if(cheat_idx < CHEAT_NAME_INGAME_MAX) {
      char nbuf[CHEAT_NAME_INGAME_LEN];
      memset(nbuf, 0, sizeof(nbuf));
      strncpy(nbuf, cheat.description, CHEAT_NAME_INGAME_LEN - 1);
      nbuf[CHEAT_NAME_INGAME_LEN - 1] = 0;
      sram_writeblock(nbuf, SRAM_CHEAT_NAMES_ADDR + (uint32_t)cheat_idx * CHEAT_NAME_INGAME_LEN, CHEAT_NAME_INGAME_LEN);
    }
    cheat_idx++;
  }
  sram_writeshort((uint16_t)cheat_idx, SRAM_NUM_CHEATS);
  /* The resident in-game name window starts at base 0 (the first-64 stage above) for every game. */
  sram_writeshort(0, SRAM_CHEAT_WIN_BASE_ADDR);
  /* PSRAM-patch mode: the image was just streamed fresh, so no code is
     applied yet -- clear the per-record applied flags the apply engine keys
     on (the spare tail carries whatever the previous game left there). */
  if(cheat_rom_psram_mode()) {
    uint8_t zeros[CHEAT_NUM_CODES_PER_CHEAT];
    memset(zeros, 0, sizeof(zeros));
    for(int i = 0; i < cheat_idx; i++)
      sram_writeblock(zeros, SRAM_CHEAT_ADDR + 512u * (uint32_t)i + CHEAT_REC_APPLIED_OFS,
                      sizeof(zeros));
  }
  yaml_file_close();
  file_res = 0; /* soft fail, suppress LED blink */
  printf("Total number of cheats: %d\n", cheat_idx);
}

/* Toggle bit 7 of the flag byte in the PSRAM cheat record at the given
   index. Called from the SNES menu via CMD_TOGGLE_CHT. The flag byte
   lives at offset 0 of the 512-byte cheat record. */
void cheat_toggle_flag(int index) {
  uint32_t addr = SRAM_CHEAT_ADDR + 512 * index;
  uint8_t flag = sram_readbyte(addr);
  sram_writebyte(flag ^ CHEAT_FLAG_ENABLE, addr);
}

/* In-game live re-program (CMD_CHEAT_REPROGRAM). The in-game cheat overlay
   edits the BSRAM flag mirror ($FF0500, one byte per cheat) directly for
   instant visual feedback without an MCU round trip. This reconciles that
   mirror's enable bit back into the canonical PSRAM records ($D00000 +
   512*i, byte 0) and re-runs cheat_program() so the FPGA ROM-cheat enable
   mask and the injected WRAM-cheat block reflect the new state without a
   reboot. Bounded by the cheat count, so it can never hang. */
void cheat_reprogram_from_mirror(void) {
  int count = sram_readshort(SRAM_NUM_CHEATS);
  if(count < 0) count = 0;
  if(count > 512) count = 512;
  for(int i = 0; i < count; i++) {
    uint8_t mirror = sram_readbyte(SRAM_CHEAT_FLAGS_ADDR + i);
    uint32_t rec = SRAM_CHEAT_ADDR + 512u * (uint32_t)i;
    uint8_t flag = sram_readbyte(rec);
    flag = (flag & ~CHEAT_FLAG_ENABLE) | (mirror & CHEAT_FLAG_ENABLE);
    sram_writebyte(flag, rec);
  }
  cheat_program();
  cheat_rom_psram_apply(); /* PSRAM-patch mode: apply/restore toggled ROM codes */
}

/* Re-stage the sliding 64-name window for absolute cheat indices [base, base+64) from the
   canonical $D00000 records into the SNES-visible BSRAM window, so the in-game overlay can list
   ALL cheats without a bigger game-load stage. Reads the description field straight from PSRAM
   ($D00000+512*i+1) -- the SAME frozen-SNES $D0 read cheat_reprogram_from_mirror does -- so there
   is no SD access and no YAML re-parse. Bounded (64 fixed reads). Served on CMD_CHEAT_NAMES_WINDOW
   while the SNES is frozen in the overlay; slots past the cheat count are staged empty. */
void cheat_stage_names_window(int base) {
  int count = sram_readshort(SRAM_NUM_CHEATS);
  if(count < 0) count = 0;
  if(count > CHEAT_RECORD_MAX) count = CHEAT_RECORD_MAX;
  if(base < 0) base = 0;
  for(int s = 0; s < CHEAT_NAME_INGAME_MAX; s++) {
    char nbuf[CHEAT_NAME_INGAME_LEN];
    int i = base + s;
    memset(nbuf, 0, sizeof(nbuf));
    if(i < count) {
      /* record layout: flags(1) + description[254] + ... -> the name is at record offset +1 */
      sram_readblock(nbuf, SRAM_CHEAT_ADDR + 512u * (uint32_t)i + 1, CHEAT_NAME_INGAME_LEN - 1);
      nbuf[CHEAT_NAME_INGAME_LEN - 1] = 0;
    }
    sram_writeblock(nbuf, SRAM_CHEAT_NAMES_ADDR + (uint32_t)s * CHEAT_NAME_INGAME_LEN,
                    CHEAT_NAME_INGAME_LEN);
  }
  sram_writeshort((uint16_t)base, SRAM_CHEAT_WIN_BASE_ADDR);
}

/* The writer counterpart, yaml_puts_escaped (yaml.c), is the inverse of
   yaml_decode_entities and is shared with the patch metadata writer, so both emit
   exactly the escapes the loader understands. */

/* save cheats to YAML file from ROM/menu */
void cheat_yaml_save(uint8_t *romfilename) {
  cheat_record_t cheat;
  char line[256];
  int numcheats = sram_readshort(SRAM_NUM_CHEATS);

  if(path_asset(line, sizeof(line), CHEAT_BASEDIR, (const char*)romfilename, ".yml") < 0) return;
  printf("Cheat YAML file: %s\n", line);

  /* Mirror what save_sram and save_backup_state do. Any prior FPGA SPI
     transaction that did not clean up its chip-select would corrupt
     the SD card SPI traffic, so explicitly release before any FatFs
     call. Also make sure the directory exists. */
  FPGA_DESELECT();

  /* Make sure the bucket directory exists before writing into it -- AFTER the name is built,
     never from the bare root (which would not create the <BB>/ level at all). */
  path_asset_mkdir(line);

  /* Clear any read-only / hidden / system attribute on the existing
     YAML file before unlinking. If the file does not exist this fails
     silently, which is OK. */
  f_chmod((TCHAR*)line, 0, AM_RDO | AM_HID | AM_SYS);

  /* If the YAML already exists with a read-only attribute, or any
     other state that makes FA_CREATE_ALWAYS return FR_DENIED, an
     unlink first lets us recreate it cleanly. */
  f_unlink((TCHAR*)line);

  file_open((uint8_t*)line, FA_WRITE | FA_CREATE_ALWAYS);

  /* If FA_CREATE_ALWAYS still failed, retry with FA_OPEN_ALWAYS plus
     manual truncation. This handles the case where the directory or
     volume rejects truncate-on-open semantics. */
  if(file_res) {
    file_open((uint8_t*)line, FA_WRITE | FA_OPEN_ALWAYS);
    if(!file_res) {
      f_lseek(&file_handle, 0);
      f_truncate(&file_handle);
    }
  }
  f_puts("---\n# Generated by sd2snes\n", &file_handle);
  for(int cheat_idx = 0; cheat_idx < numcheats; cheat_idx++) {
    cheat_save_from_menu(cheat_idx, &cheat);
    /* Emit the Name with HTML entity re-encoding so descriptions
       containing '"' or '&' survive the round trip. The previous code
       used f_printf with "%s" which would emit a literal quote into a
       YAML double-quoted scalar and silently corrupt the file.
       A description that is empty stays empty (Name: ""): no placeholder
       was ever substituted on the way in, so none has to be undone here. */
    f_puts("- Name: \"", &file_handle);
    yaml_puts_escaped(&file_handle, cheat.description);
    f_puts("\"\n", &file_handle);
    f_printf(&file_handle, "  Enabled: %s\n", cheat.flags & CHEAT_FLAG_ENABLE ? "true" : "false");
    f_printf(&file_handle, "  Code:\n");
    for(int i = 0; i < cheat.numpatches; i++) {
      uint32_t gg_code = cheat_raw2gg(cheat.patches[i].code);
      char str_buf[12];
      int slen = cheat_read_code_string(cheat_idx, i, str_buf);
      if(slen == 0) {
        /* fallback: slot was never populated, write raw form */
        f_printf(&file_handle, "  - \"%08lX\"    ", cheat.patches[i].code);
      } else {
        /* keep the column for the trailing GG comment aligned: pad
           the field to 9 visible chars regardless of slen */
        f_printf(&file_handle, "  - \"%-9s\"   ", str_buf);
      }
      if(cheat_is_wram_cheat(cheat.patches[i].code)) {
        f_printf(&file_handle, "# GG code: N/A (WRAM cheat)\n");
      } else {
        f_printf(&file_handle, "# GG code: %04lX-%04lX\n", gg_code >> 16, gg_code & 0xffff);
      }
    }
  }
  file_close();
}

uint32_t cheat_str2bin(char *string) {
  char code[9];
  uint32_t patch;
  if(strlen(string) >= 9 && string[4] == '-') {
    /* GG code */
    printf("GG code: %s\n", string);
    memcpy(code, string, 4);
    strncpy(code+4, string+5, 4);
    code[8] = 0;
    patch = (uint32_t)strtoul(code, NULL, 16);
    patch = cheat_gg2raw(patch);
  } else {
    /* PAR/RAW code */
    patch = (uint32_t)strtoul(string, NULL, 16);
    printf("PAR code: %08lX\n", patch);
  }
  return patch;
}

uint32_t cheat_gg2raw(uint32_t patch) {
  uint8_t gg2raw_tab[16] = {
    0x4, 0x6, 0xd, 0xe,
    0x2, 0x7, 0x8, 0x3,
    0xb, 0x5, 0xc, 0x9,
    0xa, 0x0, 0xf, 0x1
  };
  uint32_t decrypt = 0;
  /* translate nibbles */
  for(int i=0; i<8; i++) {
    decrypt = ((decrypt >> 4) & 0x0fffffff)
            | ((uint32_t)(gg2raw_tab[patch & 0xf]) << 28);
    patch >>= 4;
  }
  /* remap bits: VVVVVVVVAAAABBBBCCDDDDEEEEFFFFGG
              => DDDDFFFFAAAAGGCCBBBBEEEEVVVVVVVV */
  decrypt = ((decrypt & 0xff000000) >> 24)
          |  (decrypt & 0x00f00000)
          | ((decrypt & 0x000f0000) >> 4)
          | ((decrypt & 0x0000c000) << 2)
          | ((decrypt & 0x00003c00) << 18)
          | ((decrypt & 0x000003c0) << 2)
          | ((decrypt & 0x0000003c) << 22)
          | ((decrypt & 0x00000003) << 18);
  return decrypt;
}

uint32_t cheat_raw2gg(uint32_t patch) {
  uint8_t raw2gg_tab[16] = {
    0xd, 0xf, 0x4, 0x7,
    0x0, 0x9, 0x1, 0x5,
    0x6, 0xb, 0xc, 0x8,
    0xa, 0x2, 0x3, 0xe
  };
  uint32_t encrypt = 0;
  /* remap bits: AAAABBBBCCCCDDEEFFFFGGGGVVVVVVVV
              => VVVVVVVVCCCCFFFFEEAAAAGGGGBBBBDD */
  patch = ((patch & 0xf0000000) >> 18)
        | ((patch & 0x0f000000) >> 22)
        |  (patch & 0x00f00000)
        | ((patch & 0x000c0000) >> 18)
        | ((patch & 0x00030000) >> 2)
        | ((patch & 0x0000f000) << 4)
        | ((patch & 0x00000f00) >> 2)
        | ((patch & 0x000000ff) << 24);
  /* translate nibbles */
  for(int i=0; i<8; i++) {
    encrypt = ((encrypt >> 4) & 0x0fffffff)
            | ((uint32_t)(raw2gg_tab[patch & 0xf]) << 28);
    patch >>= 4;
  }
  return encrypt;
}
