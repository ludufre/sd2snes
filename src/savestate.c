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

/* The savestate base name: a patched game keys off the PATCH name (mirroring the .srm
   path via current_ips_srm_source in memory.c) so the .state matches the .srm; a plain
   game keys off the base ROM at recents index 0.  Uses file_lfn as the scratch the
   recents lookup fills, so the pointer stays valid until the next
   cfg_get_listed_game. */
static const char *ss_base(void) {
  cfg_get_listed_game(LAST_FILE, file_lfn, 0);
  return current_ips_srm_source[0] ? (const char *)current_ips_srm_source
                                   : (const char *)file_lfn;
}

/* Build "<SS_BASEDIR>/<BB>/<base>NN.state" for slot NN.  Passes path_asset's return
   through: -1 means the name did not fit, and `line` is left an empty string. */
static int ss_slot_path(char *line, int n, const char *base, uint8_t slot) {
  char extend[10];
  snprintf(extend, sizeof(extend), "%02d.state", slot);
  return path_asset(line, n, SS_BASEDIR, base, extend);
}

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
  uint8_t mask = 0;
  int slot;
  const char *ssbase = ss_base();
  for(slot = 1; slot <= 4; slot++) {
    ss_slot_path(line, sizeof(line), ssbase, (uint8_t)slot);
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
  /* Zero the handler's CS_STATE ($FE100C) on EVERY game load: it lives in PSRAM,
     which survives resets and short power-cycles, and a stale nonzero value
     (the resume cooldown of a previous session) silently swallows every
     save/load combo until it happens to clear.  Pre-existing on all cores;
     first observed on CX4 after a mid-session power-cycle. */
  sram_writebyte(0, 0xFE100CL);
  /* Overlay scene gate.  The NMI/IRQ hook fires from the vector, which some engines
     take even when their frame loop is NOT running -- Super Mario RPG parks the S-CPU
     mid-RPC across a scene transition and runs a stub vblank handler there.  The
     software probe in snes/savestate.a65 had no scene gate on ANY platform (cheat.v's
     overlay_combo only gates the IRQ redirect), so opening the overlay in that window
     hung the game on resume (proven in hardware on Mk.II).  For the games that need it
     the probe additionally requires the FPGA's scene_fresh bit at $F90720 (cheat.v:
     pad forwarded to SA-1 IRAM $3010/$3011, or $4218/$4219 polled, within ~49-73ms).
     Keyed by core + header checksum, not by chip flags: an engine that does NEITHER of
     those two accesses (its own pad path, its own registers) would look permanently
     dead to the FPGA and lose the overlay entirely, so the gate stays opt-in per game
     rather than defaulting on for the whole SA-1 library.
     Written on EVERY load -- even 0, even on cores that never install the handler --
     for the same reason as the two writes above: nothing here may go stale. */
  uint8_t scene_gate = 0;
  if(romprops.fpga_conf == FPGA_SA1) {
    switch(romprops.header.chk) {
      case 0x3BB4: /* Super Mario RPG (US) */ scene_gate = 1; break;
      default: break;
    }
  }
  sram_writebyte(scene_gate, SS_SCENE_GATE_ADDR);
  /* Overlay close-time APU resync.  savestate_fixes.yml entries re-synchronise a WRAM
     shadow of the APU handshake with the live $214x port, and so far they only ran on
     savestate save/load (audio_fix in snes/savestate.a65).  The cheat overlay freezes
     the S-CPU for seconds too, and a game whose interrupt handler waits on that shadow
     with an UNBOUNDED spin deadlocks on resume if it drifted.  Star Ocean is exactly
     that shape: its V-IRQ handler ($C0:0221) reaches three copies of
       LDA $2140 : CMP $2140 : BNE * : EOR $4A : BPL *
     with no timeout, and $4A is the shadow its 13B8 entry rewrites.  The reported
     symptom matches: closing the overlay in a field scene leaves the restored picture
     on screen with every sprite gone (OAM is deliberately not restored -- the game is
     supposed to repopulate it) and the game never runs again.
     Keyed on the header checksum ALONE, deliberately: the yml itself is keyed that way,
     so gate and blob can never disagree about which game they mean.  Everything else
     gets 0 and behaves exactly as before -- opt-in per game because a blob is free-form
     code (several entries write immediates and one writes $2140 itself), so running it
     on every overlay close library-wide would be a much wider behaviour change.
     Cleared HERE on every load so it can never go stale, and only raised further down,
     AFTER savestate_set_fixes() has actually deployed this game's code at CS_FIXES --
     in overlay-only mode the fix area is never written, so the gate must stay 0 there
     rather than point the overlay at the previous game's blob. */
  sram_writebyte(0, SS_OVL_APUFIX_GATE_ADDR);
  /* The cheat overlay needs, on the FPGA core: the NMI/IRQ savestate hook + the $C0-FF
     IS_PATCH identity window (so the handler executes from menu PSRAM) + a shadow of the
     write-only $21xx/$42xx registers read back at $F90500/$F90700.  VRAM/CGRAM it reads
     back directly via $2139/$213B, so it does NOT need the full ctx.v mirror or the
     $2020 copier.  BASE/DSP/SA-1 carry that shadow inside ctx.v; the coprocessor cores
     OBC1, S-DD1, CX4, GSU and SPC7110 instead get a small standalone regshadow.v BRAM
     (see their verilog/), which is all the overlay needs -- so the overlay runs there
     too.  On the CX4 core the $FFE0-$FFFF vector override (cx4_active) is suppressed
     while the hook owns the vectors; on SA-1 the autonomous CPU is halted
     (snapshot_pause / $202C); on GSU the coprocessor is auto-paused and the ROM arbiter
     yields to the SNES for the duration of the hook (the handler runs from PSRAM, which
     the GSU would otherwise starve under RON).  The SPC7110 needs neither: like OBC1/
     S-DD1 it is reactive on this side (the overlay never touches $4800-$484F, and the
     data-ROM fetch engine only arms in free slots, so the handler's PSRAM fetches are
     never starved).
     Full in-game SAVESTATES work on base, DSP1-4, (Mk.III only) SA-1, GSU, OBC1,
     S-DD1 and CX4.  SPC7110 is overlay-only: its chip state (decompressor position,
     data-port pointer, ALU in flight) has no halt/scan window yet, so it never enters
     savestate_ok below.  Cores still WITHOUT the overlay machinery: SGB.  Key
     the gate on the core, not the chip flags. (Pointer compare against the FPGA_*
     path literals -- same idiom as FPGA_BASE.) */
  int core_has_snapshot = (romprops.fpga_conf == NULL)
                       || (romprops.fpga_conf == FPGA_BASE)
                       || (romprops.fpga_conf == FPGA_DSP)
                       || (romprops.fpga_conf == FPGA_SA1)
                       || (romprops.fpga_conf == FPGA_OBC1)
                       || (romprops.fpga_conf == FPGA_SDD1)
                       || (romprops.fpga_conf == FPGA_CX4)
                       || (romprops.fpga_conf == FPGA_GSU)
                       || (romprops.fpga_conf == FPGA_SPC7110);
  if(!core_has_snapshot) {
    savestate_enable_handler(0);
    return;
  }

/*
 * savestate code is run from bank C0 directly
 * 2C00 "EXE" hook is now left alone so it doesn't clash with USB hook features
 */

  /* In-game savestates need the coprocessor's state restored on load, so they stay OFF
     on cores where that state is not reachable.
     DSP1-4 (uPD7725), SA-1, GSU and CX4 expose a halt + scan window ($E8 bank) over their
     internal state, so the handler can capture and restore the chip -- full savestates
     work there. OBC1 needs no window at all: it is purely reactive, so the handler just
     snapshots/restores its SNES-visible $7800-$7FFF window over the bus (see obc1_ok).
     S-DD1 likewise needs no window: its decompressor FSM is never mid-transfer at an
     NMI boundary (the GP-DMA that drives it is atomic), so the handler just snapshots/
     restores the bus-visible config block $4800-$4807 (see sdd1_ok; $4801 is left alone
     on restore -- rewriting it would re-arm the FSM).
     For SA-1 the handler additionally reads back IRAM
     ($00:3000) and BW-RAM ($40:0000) through the SNES bus. ST0010 (uPD96050, 2KB data RAM)
     is not covered yet (its RAM collides with the DSP scan register-file gap), so it stays
     overlay-only. The SA-1 window only exists on Mk.III: it was MEASURED to be impossible
     on Mk.II's Spartan-3, not just assumed -- synthesizing the full machinery (gated
     SA1_SS_MK2 in the sa1 core) overmaps the xc3s400 by ~2,200 logic LUTs (8,842/7,168
     = 123%), with no recoverable lever (MSU-1 was already cut from the mk2 sa1 build by
     upstream in 2019). So sa1_ok stays gated under !CONFIG_MK2.
     The in-game menu probe (default combo L+R+Y+Left; armed per game load from
     CFG.ingame_buttons_menu) lives inside this handler, so the handler must be
     installed whenever the overlay is usable -- even with savestates OFF. */
  int dsp_ok = (romprops.fpga_conf == FPGA_DSP) && !romprops.has_st0010;
#ifndef CONFIG_MK2
  int sa1_ok = (romprops.fpga_conf == FPGA_SA1);
#else
  int sa1_ok = 0;
#endif
  /* Unlike SA-1, the GSU savestate window works on Mk.II too: its freeze FSM,
     $E8 scan window and PIXBUF/CBR restore were un-gated from ifdef MK3 (with an
     mk2-only single-driver write restructure so XST accepts the shared array
     write ports), and the ROM-cheat comparators were cut to make room -- so ROM
     cheats move to patch-PSRAM on this core (see cheat_rom_psram_mode). */
  int gsu_ok = (romprops.fpga_conf == FPGA_GSU);
  /* The OBC1 is purely reactive (no autonomous FSM to halt or scan): the handler
     captures/restores the SNES-visible $7800-$7FFF window directly over the bus, so
     this works on Mk.II and Mk.III alike (cheat.v only gained flip-flops). */
  int obc1_ok = (romprops.fpga_conf == FPGA_OBC1);
  /* The S-DD1 decompressor FSM is never mid-transfer at an NMI boundary (the GP-DMA
     that feeds it is atomic), so the handler captures/restores the bus-visible config
     block ($4800-$4807) directly over the bus -- no chip halt/scan window.  Works on
     Mk.II and Mk.III alike (cheat.v only gained flip-flops). */
  int sdd1_ok = (romprops.fpga_conf == FPGA_SDD1);
  /* The CX4 is the coprocessor with the most state already bus-visible (3 KB data RAM,
     the $7F4x MMIO, vectors and GPR all live in $00:6000-$7FFF), so its $E8 window only
     has to cover the CPU core that survives between programs plus the freeze protocol.
     Halt is requested EARLY and the core runs to its next clean boundary (run-to-stop,
     saturating timeout), then a $F0 normalize parks it at a canonical boundary before
     anything is read.  The program cache is not captured at all: on restore it is
     REPLAYED through the native $7F48-$7F4E MMIO, whose FSM is not pause-gated and so
     runs under the freeze.  Works on Mk.II and Mk.III alike (the ROM-cheat comparators
     were cut on Mk.II to make room -- see cheat_rom_psram_mode). */
  int cx4_ok = (romprops.fpga_conf == FPGA_CX4);
  /* Sufami Turbo runs on the base core, so the core test below would pass -- but the
     snapshot machinery assumes the ordinary single-cart map: under mapper 5 the ROM is
     at 0x100000/0x700000 instead of 0 and there are TWO SaveRAM windows, so a restore
     lands on the wrong regions and the game comes back black.  Overlay-only still
     applies below, so the in-game menu and cheats keep working. */
  int savestate_ok = CFG.enable_ingame_savestate
                  && !romprops.has_sufami
                  && (romprops.fpga_conf == NULL || romprops.fpga_conf == FPGA_BASE
                      || dsp_ok || sa1_ok || gsu_ok || obc1_ok || sdd1_ok || cx4_ok);
  int overlay_only = !savestate_ok
                  && CFG.enable_ingame_hook && CFG.enable_cheat_overlay;

  savestate_enable_handler(savestate_ok || overlay_only);
  if(savestate_ok) {
    sram_writeshort(0x0101, SS_REQ_ADDR);
    sram_writebyte(CFG.loadstate_delay, SS_DELAY_ADDR);
    sram_writebyte(CFG.enable_savestate_slots, SS_SLOTS_ADDR);
    sram_writebyte(CFG.enable_ingame_savestate, SS_CTRL_ADDR);
    sram_writebyte(dsp_ok ? 1 : 0, SS_DSP_GATE_ADDR);
    sram_writebyte(sa1_ok ? 1 : 0, SS_SA1_GATE_ADDR);
    sram_writebyte(gsu_ok ? 1 : 0, SS_GSU_GATE_ADDR);
    sram_writebyte(obc1_ok ? 1 : 0, SS_OBC1_GATE_ADDR);
    sram_writebyte(sdd1_ok ? 1 : 0, SS_SDD1_GATE_ADDR);
    sram_writebyte(cx4_ok ? 1 : 0, SS_CX4_GATE_ADDR);
    savestate_set_inputs();
    savestate_set_fixes();
    load_backup_state();
  } else if(overlay_only) {
    /* Overlay-only: the handler is installed purely so the cheat-overlay probe can run.
       CS_CTRL=0 makes the handler skip ALL save/load/slot handling below the probe (the
       save/load inputs are unconfigured here and would otherwise match every frame) --
       see snes/savestate.a65 at ss_probe_done.  The probe stays gated by CHEAT_OVL_GATE. */
    sram_writebyte(0, SS_CTRL_ADDR);
    sram_writebyte(0, SS_DSP_GATE_ADDR);
    sram_writebyte(0, SS_SA1_GATE_ADDR);
    sram_writebyte(0, SS_GSU_GATE_ADDR);
    sram_writebyte(0, SS_OBC1_GATE_ADDR);
    sram_writebyte(0, SS_SDD1_GATE_ADDR);
    sram_writebyte(0, SS_CX4_GATE_ADDR);
    /* Deploy the per-game fix blob here too: the overlay-close APU resync below
       needs it, and the user running overlay-only (savestates off) is exactly the
       audience of the Star Ocean close-freeze. Cheap -- with no yml match it just
       writes the terminating rtl. */
    savestate_set_fixes();
  }
  /* Now that this game's fix code really is deployed at CS_FIXES (both branches),
     allow the overlay to re-run it on close for the games that need it (see the
     comment at the early clear above). Checksum-keyed exactly like
     savestate_fixes.yml; both the stock JP ROM and the DeJap-patched dump carry
     chk 13B8 in the header field (read from both files), so one key covers both. */
  if((savestate_ok || overlay_only) && romprops.header.chk == 0x13B8) {
    sram_writebyte(1, SS_OVL_APUFIX_GATE_ADDR);
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
static int savestate_parse_yaml_fix(ssfix_record_t *fix, yaml_token_t *tok) {
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
static int savestate_write_fix_code(ssfix_record_t *fix, uint32_t addr) {
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
static int savestate_write_fix_literal(char *str, uint32_t addr) {
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
  char line[256];
  ss_slot_path(line, sizeof(line), ss_base(), slot);

  /* Publish which slot's image is resident ONLY when the file is really there. A
     failed load leaves the PREVIOUS slot's image in PSRAM, and claiming it as this
     slot would make the next in-game load replay the wrong state under the right
     name -- worse than not loading at all. */
  int staged = (f_stat(line, NULL) == FR_OK);
  if(staged) {
    load_sram((uint8_t*) line, 0xF00000L);
    sram_writebyte(slot, SRAM_SS_STAGED_SLOT_ADDR);
  } else {
    printf("load_backup_state: %s missing, keeping resident image\n", line);
  }
  file_res = FR_OK;
  // clear the busy bit in the slot
  sram_writebyte(slot, SS_SLOTS_ADDR);
}

void save_backup_state() {
  uint8_t slot = CFG.enable_savestate_slots ? sram_readbyte(SS_SLOTS_ADDR) : 1;
  slot &= 0x7F;
  char line[256];
  if(ss_slot_path(line, sizeof(line), ss_base(), slot) < 0) return;
  path_asset_mkdir(line);                     /* create only AFTER the name exists */

  save_sram((uint8_t*) line, 0x50000L, 0xF00000L);
  /* Reflect fresh occupancy for the STATES tab without a re-scan: OR this slot's bit
     into the published mask. */
  if(slot >= 1 && slot <= 4) {
    uint8_t st = sram_readbyte(SRAM_SS_SLOT_STATUS_ADDR);
    st |= (uint8_t)(1 << (slot - 1));
    sram_writebyte(st, SRAM_SS_SLOT_STATUS_ADDR);
  }
  /* The image we just wrote out IS this slot's, and it is still resident -- record
     that so an immediate load of the same slot skips a pointless 320KB re-read. */
  sram_writebyte(slot, SRAM_SS_STAGED_SLOT_ADDR);
  // clear the busy bit in the slot
  sram_writebyte(slot, SS_SLOTS_ADDR);
}