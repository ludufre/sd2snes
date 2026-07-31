#include "config.h"
#include "fileops.h"
#include "uart.h"
#include "memory.h"
#include "fpga_spi.h"
#include "snes.h"
#include "savestate.h"
#include "yaml.h"
#include "cfg.h"
#include "cheat.h"
#include "fpga.h"

#include <string.h>
#include <stdlib.h>

extern cfg_t CFG;
extern snes_romprops_t romprops;

/* Scan the SD for the 4 possible savestate slot files of the game being loaded and
   publish an occupancy bitmask (bit N-1 = slot N has <rom>0N.state) to
   SRAM_SS_SLOT_STATUS_ADDR for the in-game STATES tab.  Names are built EXACTLY as
   load/save_backup_state build them (same patch-aware ssbase, same SS_BASEDIR, same
   "%02d.state" format) so the mask matches the real files.  Bounded (4 cheap f_stats,
   no FILINFO fetched) and runs at game-load, never during a freeze.  ALWAYS writes the
   byte -- even 0 -- so a previous game's occupancy is never left stale.  The states dir
   not existing simply yields mask 0 (every f_stat fails); no folder is created here. */
void savestate_slot_status_stage(void) {
  char line[256];
  char extend[10];
  uint8_t mask = 0;
  int slot;
  cfg_get_listed_game(LAST_FILE, file_lfn, 0);
  char *ssbase = current_ips_srm_source[0] ? (char*)current_ips_srm_source : (char*)file_lfn;
  for(slot = 1; slot <= 4; slot++) {
    strcpy(line, SS_BASEDIR);
    snprintf(extend, sizeof(extend), "%02d.state", slot);
    append_file_basename(line, ssbase, extend, sizeof(line));
    if(f_stat((const TCHAR*)line, NULL) == FR_OK) {
      mask |= (uint8_t)(1 << (slot - 1));
    }
  }
  sram_writebyte(mask, SRAM_SS_SLOT_STATUS_ADDR);
  file_res = FR_OK;
}

void savestate_program() {
  /* Publish savestate slot occupancy for the STATES tab on EVERY game load, BEFORE the
     core gate below.  Placing it here (not after the early return) means occupancy stays
     fresh even on overlay-only / unsupported cores, and it never goes stale because
     savestate_program runs on every load. */
  savestate_slot_status_stage();
  /* The cheat overlay needs, on the FPGA core: the NMI/IRQ savestate hook + the $C0-FF
     IS_PATCH identity window (so the handler executes from menu PSRAM) + a shadow of the
     write-only $21xx/$42xx registers read back at $F90500/$F90700.  VRAM/CGRAM it reads
     back directly via $2139/$213B, so it does NOT need the full ctx.v mirror or the
     $2020 copier.  BASE/DSP/SA-1 carry that shadow inside ctx.v; the coprocessor cores
     OBC1, S-DD1, CX4 and GSU instead get a small standalone regshadow.v BRAM (see their
     verilog/), which is all the overlay needs -- so the overlay runs there too.  On the
     CX4 core the $FFE0-$FFFF vector override (cx4_active) is suppressed while the hook
     owns the vectors; on SA-1 the autonomous CPU is halted (snapshot_pause / $202C); on
     GSU the coprocessor is auto-paused and the ROM arbiter yields to the SNES for the
     duration of the hook (the handler runs from PSRAM, which the GSU would otherwise
     starve under RON).
     Full in-game SAVESTATES still require the base ctx copier (kept base-only below); the
     coprocessor cores are overlay-only.  Cores still WITHOUT the overlay machinery:
     SPC7110, SGB.  Key the gate on the core, not the chip flags. (Pointer
     compare against the FPGA_* path literals -- same idiom as FPGA_BASE.) */
  int core_has_snapshot = (romprops.fpga_conf == NULL)
                       || (romprops.fpga_conf == FPGA_BASE)
                       || (romprops.fpga_conf == FPGA_DSP)
                       || (romprops.fpga_conf == FPGA_SA1)
                       || (romprops.fpga_conf == FPGA_OBC1)
                       || (romprops.fpga_conf == FPGA_SDD1)
                       || (romprops.fpga_conf == FPGA_CX4)
                       || (romprops.fpga_conf == FPGA_GSU);
  if(!core_has_snapshot) {
    savestate_enable_handler(0);
    return;
  }

/*
 * savestate code is run from bank C0 directly
 * 2C00 "EXE" hook is now left alone so it doesn't clash with USB hook features
 */

  /* In-game savestates stay OFF on the DSP and SA-1 cores: a save/load resumes later with
     the coprocessor's internal state unrestored. The overlay resumes immediately and never
     restores the coprocessor, so it is fine there. Hence savestates require a plain base
     core, while the overlay just needs the in-game hook (matches the menu greying via
     mfunc_isenabled_hooks) plus its own toggle. The overlay probe (L+R+Y+Left) lives inside
     this handler, so the handler must be installed whenever the overlay is usable -- even
     with savestates OFF. */
  int savestate_ok = CFG.enable_ingame_savestate
                  && (romprops.fpga_conf == NULL || romprops.fpga_conf == FPGA_BASE);
  int overlay_only = !savestate_ok
                  && CFG.enable_ingame_hook && CFG.enable_cheat_overlay;

  savestate_enable_handler(savestate_ok || overlay_only);
  if(savestate_ok) {
    sram_writeshort(0x0101, SS_REQ_ADDR);
    sram_writebyte(CFG.loadstate_delay, SS_DELAY_ADDR);
    sram_writebyte(CFG.enable_savestate_slots, SS_SLOTS_ADDR);
    sram_writebyte(CFG.enable_ingame_savestate, SS_CTRL_ADDR);
    savestate_set_inputs();
    savestate_set_fixes();
    load_backup_state();
  } else if(overlay_only) {
    /* Overlay-only: the handler is installed purely so the cheat-overlay probe can run.
       CS_CTRL=0 makes the handler skip ALL save/load/slot handling below the probe (the
       save/load inputs are unconfigured here and would otherwise match every frame) --
       see snes/savestate.a65 at ss_probe_done.  The probe stays gated by CHEAT_OVL_GATE. */
    sram_writebyte(0, SS_CTRL_ADDR);
  }
}

void savestate_set_inputs() {
  int err = 0;
  char buf[5];
  char * str;
  uint16_t input;
  snprintf(buf, 5, "%04X", romprops.header.chk);

  input = CFG.ingame_buttons_savestate;
  sram_writeshort(input, SS_SAVE_INPUT_ADDR);

  input = CFG.ingame_buttons_loadstate;
  sram_writeshort(input, SS_LOAD_INPUT_ADDR);

  input = CFG.ingame_buttons_changestate;
  sram_writeshort(input, SS_SLOTS_INPUT_ADDR);

  yaml_file_open(SS_INPUTFILE, FA_READ);
  if(file_res) {
    err = file_res;
  }
  if(!err) {
    yaml_token_t tok;
    if(yaml_get_itemvalue(buf, &tok)) { 
      str = strtok(tok.stringvalue, ";, \t");
      input = cfg_buttons_string2bits(str);
      if(input > 0) sram_writeshort(input, SS_SAVE_INPUT_ADDR);
      str = strtok(NULL, ";, \t");
      input = cfg_buttons_string2bits(str);
      if(input > 0) sram_writeshort(input, SS_LOAD_INPUT_ADDR);
    }
  }
  yaml_file_close();
}

/* convert a YAML record into binary fix data for the savestate handler.
   XXX Also patches the ROM directly when ROM patch directive found
*/
int savestate_parse_yaml_fix(ssfix_record_t *fix, yaml_token_t *tok) {
  fix->operator = SS_OP_NONE;
  fix->operand = 0;
  uint32_t dst;
  char *pos;

  //dst address
  pos = tok->stringvalue;
  fix->dst = strtol(pos, &pos, 16);

  //src offset
  if(*pos != ',') {
    /* invalid record */
    return 0;
  }
  pos++; /* skip comma */
  fix->src = strtol(pos, &pos, 16);

  //operation
  if(*pos && *pos != ';' && *pos != ' ') {
    switch(*pos) {
      case '^': // EOR
        fix->operator = SS_OP_EOR;
        break;
      case '&': // AND
        fix->operator = SS_OP_AND;
        break;
      case '|': // OR
        fix->operator = SS_OP_OR;
        break;
      default:
        /* invalid record */
        return 0;
    }
    //operand
    pos++;
    fix->operand = strtol(pos, &pos, 16);
  }

  //rompatch
  if(*pos) {
    pos++;
    if(*pos == ';') {
      dst = strtol(pos, &pos, 16);
      if(*pos == ',') {
        pos++;
        uint8_t byte = strtol(pos, &pos, 16);
        if(dst > 0){
          sram_writebyte(byte, dst);
        }
      }
    }
  }
  return 1;
}

/*
  convert savestate fix record into executable code and deploy at addr.
  Returns: number of bytes written
*/
int savestate_write_fix_code(ssfix_record_t *fix, uint32_t addr) {
  int count = 0;
  uint8_t fixcode[10];
  memset(fixcode, 0, sizeof(fixcode));
  if(fix->src >= 0x2140 && fix->src <= 0x2143){
    fixcode[count++] = ASM_LDA_ABSLONG;
    fixcode[count++] = (fix->src >> 0) & 0xff;
    fixcode[count++] = (fix->src >> 8) & 0xff;
    fixcode[count++] = 0;
  } else {
    fixcode[count++] = ASM_LDA_IMM;
    fixcode[count++] = fix->src & 0xff;
  }
  if(fix->operator) {
    fixcode[count++] = fix->operator;
    fixcode[count++] = fix->operand;
  }
  fixcode[count++] = ASM_STA_ABSLONG;
  fixcode[count++] = (fix->dst >>  0) & 0xff;
  fixcode[count++] = (fix->dst >>  8) & 0xff;
  fixcode[count++] = (fix->dst >> 16) & 0xff;

  sram_writeblock(fixcode, addr, count);
  return count;
}

/*
  convert literal savestate code string str into binary and deploy at addr
  Returns: number of bytes written
*/
int savestate_write_fix_literal(char *str, uint32_t addr) {
  int count = 0;
  uint8_t fixcode[64];
  char c, d;
  while((c = *str++) && (count < sizeof(fixcode))) {
    /* skip prefix */
    if(c == '@') continue;
    /* stop on incomplete hex tuple */
    if (!(d = *str++)) break;
    c = (c & 0x40) ? (c & 0x7) + 9 : c & 0xf;
    d = (d & 0x40) ? (d & 0x7) + 9 : d & 0xf;
    fixcode[count++] = (c << 4) | d;
  }
  sram_writeblock(fixcode, addr, count);
  return count;
}

void savestate_set_fixes() {
  int err = 0;
  char chksum[5];
  uint32_t addr = SS_FIXES_ADDR;
  yaml_token_t tok;
  tok.type = YAML_KEY;
  snprintf(chksum, 5, "%04X", romprops.header.chk);
  yaml_file_open(SS_FIXESFILE, FA_READ);
  if(file_res) {
    err = file_res;
  }
  if(!err) {
    while(yaml_get_value(chksum, &tok, YAML_SCOPE_GLOBAL)) {
      ssfix_record_t fix;
      if(tok.type == YAML_LIST_START) {
        while(yaml_get_next(&tok)) {
          if(tok.type == YAML_LIST_END) break;
          if(tok.stringvalue[0] == '@') {
            /* code literal */
            // printf("Fix record (list/literal): %s\n", tok.stringvalue);
            addr += savestate_write_fix_literal(tok.stringvalue, addr);
          } else {
            if(savestate_parse_yaml_fix(&fix, &tok)) {
              // printf("Fix record (list/std): tgt=%06lx src=%04x operator=%02x operand=%02x\n", fix.dst, fix.src, fix.operator, fix.operand);
              addr += savestate_write_fix_code(&fix, addr);
            }
          }
        }
      } else {
        if(tok.stringvalue[0] == '@') {
          /* code literal */
          // printf("Fix record (single/literal): %s\n", tok.stringvalue);
          addr += savestate_write_fix_literal(tok.stringvalue, addr);
        } else {
          if(savestate_parse_yaml_fix(&fix, &tok)) {
            // printf("Fix record (single/std): tgt=%06lx src=%04x operator=%02x operand=%02x\n", fix.dst, fix.src, fix.operator, fix.operand);
            addr += savestate_write_fix_code(&fix, addr);
          }
        }
      }

    }
  }
  sram_writebyte(ASM_RTL, addr);
  yaml_file_close();
}

void savestate_enable_handler(int enable) {
  uint16_t flags;
  printf("savestate_enable_handler->%d\n", enable);
  flags = (enable ? 0x0040 : 0x4000);
  fpga_write_cheat(7, flags);
}

void load_backup_state() {
  uint8_t slot = CFG.enable_savestate_slots ? sram_readbyte(SS_SLOTS_ADDR) : 1;
  slot &= 0x7F;
  char line[256] = SS_BASEDIR;
  char extend[10];
  check_or_create_folder(SS_BASEDIR);
  cfg_get_listed_game(LAST_FILE, file_lfn, 0);
  /* A patched game keys its savestate off the PATCH name (mirrors the .srm path
     chosen via current_ips_srm_source in memory.c) so the .state matches the
     .srm; a plain game keys off the base ROM at recents index 0. */
  char *ssbase = current_ips_srm_source[0] ? (char*)current_ips_srm_source : (char*)file_lfn;
  snprintf(extend, sizeof(extend), "%02d.state", slot);
  append_file_basename(line, ssbase, extend, sizeof(line));

  load_sram((uint8_t*) line, 0xF00000L);
  file_res = FR_OK;
  // clear the busy bit in the slot
  sram_writebyte(slot, SS_SLOTS_ADDR);
}

void save_backup_state() {
  uint8_t slot = CFG.enable_savestate_slots ? sram_readbyte(SS_SLOTS_ADDR) : 1;
  slot &= 0x7F;
  char line[256] = SS_BASEDIR;
  char extend[10];
  check_or_create_folder(SS_BASEDIR);
  cfg_get_listed_game(LAST_FILE, file_lfn, 0);
  /* A patched game keys its savestate off the PATCH name (mirrors the .srm path
     chosen via current_ips_srm_source in memory.c) so the .state matches the
     .srm; a plain game keys off the base ROM at recents index 0. */
  char *ssbase = current_ips_srm_source[0] ? (char*)current_ips_srm_source : (char*)file_lfn;
  snprintf(extend, sizeof(extend), "%02d.state", slot);
  append_file_basename(line, ssbase, extend, sizeof(line));

  save_sram((uint8_t*) line, 0x50000L, 0xF00000L);
  /* Reflect fresh occupancy for the STATES tab without a re-scan: OR this slot's bit
     into the published mask. */
  if(slot >= 1 && slot <= 4) {
    uint8_t st = sram_readbyte(SRAM_SS_SLOT_STATUS_ADDR);
    st |= (uint8_t)(1 << (slot - 1));
    sram_writebyte(st, SRAM_SS_SLOT_STATUS_ADDR);
  }
  // clear the busy bit in the slot
  sram_writebyte(slot, SS_SLOTS_ADDR);
}