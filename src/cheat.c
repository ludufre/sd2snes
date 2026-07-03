#include "config.h"
#include "fileops.h"
#include "uart.h"
#include "memory.h"
#include "fpga_spi.h"
#include "snes.h"
#include "cheat.h"
#include "yaml.h"
#include "cfg.h"
#include "sgb.h"
#include "lang.h"

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
   otherwise show them verbatim. The output is always no longer than
   the input, so an in-place rewrite is safe. */
static void cheat_decode_html_entities(char *s) {
  if(!s) return;
  char *r = s;
  char *w = s;
  while(*r) {
    if(*r == '&') {
      if(!strncmp(r, "&quot;", 6)) { *w++ = '"';  r += 6; continue; }
      if(!strncmp(r, "&amp;",  5)) { *w++ = '&';  r += 5; continue; }
      if(!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; continue; }
      if(!strncmp(r, "&lt;",   4)) { *w++ = '<';  r += 4; continue; }
      if(!strncmp(r, "&gt;",   4)) { *w++ = '>';  r += 4; continue; }
    }
    *w++ = *r++;
  }
  *w = 0;
}

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
     The overlay reuses the savestate snapshot/restore machinery, which is NOT
     supported on enhancement-chip games (SA-1, SuperFX/GSU, S-DD1, SPC7110, and
     the DSP/CX4/OBC1 helpers) — freezing the SNES CPU mid-frame and snapshotting
     the PPU desyncs the asynchronous coprocessor. So force it off there regardless
     of the user toggle; on plain LoROM/HiROM it follows CFG.enable_cheat_overlay. */
  {
    uint8_t special_chip = romprops.has_dspx || romprops.has_cx4 || romprops.has_obc1
                        || romprops.has_gsu  || romprops.has_sa1 || romprops.has_sdd1
                        || romprops.has_spc7110;
    sram_writebyte((CFG.enable_cheat_overlay && !special_chip) ? 1 : 0, SRAM_CHEAT_OVL_GATE_ADDR);
  }

  sgb_cheat_program();
}

void cheat_program_single(cheat_patch_record_t *cheat) {
  uint8_t is_wram_cheat;
  /* determine ROM or WRAM cheat */
  is_wram_cheat = cheat_is_wram_cheat(cheat->code);
  /* apply cheat to FPGA / NMI hook */
  if(is_wram_cheat) {
    if(wram_index < CHEAT_WRAM_MAX) cheat_program_ram_cheat(wram_index++, cheat);
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

/* Build the "Cheats for <name>" window title into PSRAM and open the cheat YAML
   file.  Kept in a SEPARATE function from cheat_yaml_load on purpose: its
   title[64] + line[256] scratch (~320 B) is then popped before the per-cheat
   parse loop runs.  That loop drives the yaml parser, whose chain
   (cheat_record_t + yaml_token_t + candidate buffer) already nearly fills the
   LPC1756's ~1976-byte stack; keeping these buffers live across it overflowed
   into .bss and hung the cheat menu (esp. via the deeper recents/favorites
   path).  Sets the global file_res. */
static void cheat_yaml_title_and_open(uint8_t* romfilename) {
  char line[256] = CHEAT_BASEDIR;

/* Build "Cheats for <basename without extension>" in PSRAM at
  SRAM_CHEAT_TITLE_ADDR ($D80000) so the SNES menu can use it as the
  window title. Always written, even if the YAML is missing/empty.
  The address is well past any plausible cheat record (cheats live
  at $D00000+512*N for N up to 511, spanning banks D0..D3, and the
  per-code display strings start at $D40000), and is in the same
  PSRAM region as the cheat records, reachable from both MCU and
  SNES. */
  {
    char title[64];
    memset(title, 0, sizeof(title));
    const char *p = strrchr((const char*)romfilename, '/');
    p = p ? p + 1 : (const char*)romfilename;
    strncpy(title, cheatmenu_l[lang_idx()][CHEAT_FOR], sizeof(title));
    title[sizeof(title) - 1] = 0;
    int prefix_len = strlen(title);
    int copy_max = sizeof(title) - prefix_len - 1;
    int n = strlen(p);
    if (n > copy_max) n = copy_max;
    memcpy(title + prefix_len, p, n);
    title[prefix_len + n] = 0;
    /* strip trailing extension (.smc/.sfc/.fig/etc.) if any */
    char *dot = strrchr(title + prefix_len, '.');
    if (dot) *dot = 0;

    /* The cheat window is 52 chars wide. Cap the visible title length
       at 46 chars so it never collides with the window's right border.
       If the full string is longer, replace the last 3 chars with
       "..." so the user can see the title was clipped. */
    if (strlen(title) > 46) {
      title[43] = '.';
      title[44] = '.';
      title[45] = '.';
      title[46] = 0;
    }

    sram_writeblock(title, SRAM_CHEAT_TITLE_ADDR, sizeof(title));
  }

  append_file_basename(line, (char*)romfilename, ".yml", sizeof(line));
  check_or_create_folder(CHEAT_BASEDIR);
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
      cheat_decode_html_entities(cheat.description);
    }
    /* If the cheat's Name is empty after parsing + HTML decoding,
       substitute a visible placeholder so the row in the menu is not
       just a blank line. cheat_yaml_save reverses this substitution so
       the placeholder never ends up in the YAML on disk. */
    if(cheat.description[0] == 0) {
      strncpy(cheat.description, cheatmenu_l[lang_idx()][CHEAT_NO_NAME], 254);
      cheat.description[253] = 0;
    }
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
    /* Stage the first CHEAT_NAME_INGAME_MAX descriptions into the
       SNES-visible BSRAM window ($FF0800) so the in-game cheat overlay
       can display names: the canonical PSRAM record at $D00000 is the
       game's own ROM during gameplay, unreachable from the overlay. */
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
}

/* Inverse of cheat_decode_html_entities. Writes the supplied string to
   the given FatFs file handle, re-encoding the two characters that would
   otherwise break a load-save round trip:
     '"' -> &quot;  (mandatory: an unescaped quote inside a YAML
                    double-quoted scalar terminates the value early and
                    corrupts the Name field)
     '&' -> &amp;   (mandatory: a literal '&' followed by 'quot;', 'amp;',
                    'apos;', 'lt;' or 'gt;' would be re-decoded by the
                    next load and lose the original ampersand)
   '<', '>' and '\'' are left literal so YAML stays human-readable; the
   load side accepts both literal and entity forms for those. */
static void cheat_write_yaml_string(FIL *fp, const char *s) {
  if(!fp || !s) return;
  char one[2];
  one[1] = 0;
  while(*s) {
    if(*s == '"') {
      f_puts("&quot;", fp);
    } else if(*s == '&') {
      f_puts("&amp;", fp);
    } else {
      one[0] = *s;
      f_puts(one, fp);
    }
    s++;
  }
}

/* save cheats to YAML file from ROM/menu */
void cheat_yaml_save(uint8_t *romfilename) {
  cheat_record_t cheat;
  char line[256] = CHEAT_BASEDIR;
  int numcheats = sram_readshort(SRAM_NUM_CHEATS);

  append_file_basename(line, (char*)romfilename, ".yml", sizeof(line));
  printf("Cheat YAML file: %s\n", line);

  /* Mirror what save_sram and save_backup_state do. Any prior FPGA SPI
     transaction that did not clean up its chip-select would corrupt
     the SD card SPI traffic, so explicitly release before any FatFs
     call. Also make sure the directory exists. */
  FPGA_DESELECT();

  /* Make sure the cheat directory exists before writing into it. */
  check_or_create_folder(CHEAT_BASEDIR);

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
    /* Reverse the (empty) placeholder substitution from cheat_yaml_load
       so a YAML round trip (load then save) preserves cheats with
       intentionally empty Names. */
    const char *desc_for_yaml = cheat.description;
    if(!strcmp(cheat.description, cheatmenu_l[lang_idx()][CHEAT_NO_NAME])) {
      desc_for_yaml = "";
    }
    /* Emit the Name with HTML entity re-encoding so descriptions
       containing '"' or '&' survive the round trip. The previous code
       used f_printf with "%s" which would emit a literal quote into a
       YAML double-quoted scalar and silently corrupt the file. */
    f_puts("- Name: \"", &file_handle);
    cheat_write_yaml_string(&file_handle, desc_for_yaml);
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
