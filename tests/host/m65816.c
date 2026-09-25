/* m65816.c -- 65816 core plus the modelled SNES bus.  See m65816.h for the
 * scope.  One rule here: anything not modelled aborts loudly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m65816.h"

m_cpu_t  m_cpu;
uint8_t  m_vram[0x10000];
uint16_t m_vcounter = 225;
int      m_forced_blank;
uint64_t m_instr_budget = 400000000ull;

static uint8_t  wram[0x20000];     /* $7E:0000 .. $7F:FFFF */
static uint8_t  rom[0x8000];      /* ONE LoROM page -- see m_load_rom */
static size_t   rom_len;
static uint8_t  cgram[0x200];
static uint8_t  oam[0x400];        /* 512 low + 32 high, byte-addressed 0..$3FF */
/* Low-water mark of the stack pointer.  The player's working set sits right
 * under the stack in bank $7E, so 'how deep did the deepest call chain go'
 * is the difference between a measurement and a guess. */
static uint16_t stack_low = 0xffff;
static uint8_t  ppu_reg[0x100];    /* mute mirror of unmodelled $21xx writes */
static uint32_t ppu_wr[0x100];     /* how many times each of them was written */
static uint8_t  cpu_reg[0x100];    /* $42xx */
static uint8_t  dma_reg[0x80];     /* $43xx */
/* GBC mode: the player's HDMA ch0 is a DECOY (gbc_snes.asm, "THE 5A22 TAKES
 * THE LOWEST HDMA CHANNEL") and its logical channel 0 lives on ch6.  dma_reg
 * and cpu_reg[0x0C] keep the LOGICAL layout every check above was written
 * against; the decoy's registers and the physical $420C live here, and a
 * physical mask without the decoy (or with ch7, the general DMA channel)
 * is counted. */
static uint8_t decoy_reg[16];
static uint8_t phys_420c;
static unsigned decoy_bad;
static int decoy_on;

/* VRAM/CGRAM/OAM port state */
static uint8_t  vmain;
static uint16_t vmadd;
static uint16_t cgadd;
static uint16_t oam_ba;
static uint8_t  oam_latch;
static int      hv_hi;             /* hi/lo flip-flop of $213C/$213D */

/* --- GBC player extensions (m65816.h); every one of them is inert at 0 --- */
static int       gbc;              /* master switch, m_gbc_mode() */
static uint8_t  *view_win;         /* 320 KB, banks $E0-$E4 (wire $03: $E4 = the FB) */
#define RW_MAX 4
static uint32_t  rw_lo[RW_MAX], rw_hi[RW_MAX];   /* m_set_read_watch */
static int       rw_n;
static void    (*rw_fn)(uint32_t a24);
static uint32_t  ef_base, ef_len;  /* the declared $EF write window */
static void    (*ef_fn)(uint32_t off, uint8_t v);
static uint16_t  pad_joy1;
static void    (*cmd_fn)(uint8_t v);
static unsigned  cmd_n;
static void    (*dma_fn)(int ch, uint8_t bbad, uint32_t src24, uint32_t bytes,
                         uint32_t bdest);
static uint32_t  wmadd;            /* $2181-$2183, 17 bits */
static uint16_t  mulres;           /* $4216/$4217 */
/* Write-twice scroll, decoded through the SHARED BGOFS latch the real PPU
 * has: BGnHOFS = (v<<8) | (latch & ~7) | (hlatch & 7), BGnVOFS = (v<<8) |
 * latch.  Two back-to-back writes of lo then hi to the SAME register land on
 * (hi<<8)|lo either way, which is the pattern GbcScrollW emits -- but the
 * latch is what makes an INTERLEAVED pair come out wrong, and coming out
 * wrong is exactly what the model has to reproduce (contract sec. 11.3 makes
 * "each register written low-then-high back to back" an invariant). */
static uint8_t   bgofs_latch, bghofs_latch;
static uint16_t  bg_hofs[4], bg_vofs[4];
static uint8_t   coldata[3];       /* $2132: the fixed colour's R, G, B */

/* --- the CLOCK (m_clock_start).  Off by default, and with it off nothing
 * below runs: every gate that predates it sees the cycle-less model it was
 * written against.  With it on, time is master cycles (21.477 MHz): every CPU
 * bus access costs its region's speed, every internal cycle 6, a general DMA
 * 8 per byte, and each scanline start charges the DRAM refresh (40) and, on
 * lines 0..224 with HDMA enabled, the contract's sec. 11.1 per-line tax
 * 18 + 8*C + 8*B.  V/H, $4210/$4211/$4212, NMI at V=225 and the V-IRQ at VTIME
 * come out of that clock, and WAI waits for them. */
static int       clk_on;
static uint64_t  clk_stall_pend;   /* m_clock_stall, applied in clk_advance */
static uint64_t  clk_mc, clk_line_start, clk_pend, clk_depth0;
static int       clk_line, clk_lines = 262;
static int       clk_in_dma, clk_io, clk_waiting, clk_depth;
static int       nmi_pend, rdnmi_flag, irq_timeup;
static uint16_t  v_latch;
static int       cpu_rev = 2, cpu_pal;
static uint64_t  dma_start_mc, dma_end_mc;
static void    (*line_fn)(int v);
static void    (*int_fn)(int kind, int v, int h);
static void    (*wwatch_fn)(uint32_t a24);
static void    (*pc_fn)(uint32_t pc24);   /* profiler: every instruction */

#define M_DIE(...) do { fprintf(stderr, "m65816: " __VA_ARGS__); \
                        fprintf(stderr, "  (PC=%02X:%04X instr=%llu)\n", \
                                m_cpu.pbr, m_cpu.pc, (unsigned long long)m_cpu.instrs); \
                        abort(); } while(0)

void m_reset_memory(void) {
  /* WRAM comes up as $55, the way the fork's clear_wram leaves it: a gate the
     boot forgets to clear then shows up as garbage instead of as a zero. */
  memset(wram, 0x55, sizeof(wram));
  /* VRAM POISONED, not zeroed: nes_boot_init is what clears it (by DMA), so a
     memset here would credit the harness with the renderer's work and the
     post-boot snapshot would describe the model.  $A5 is never a plausible
     conversion result. */
  memset(m_vram, 0xa5, sizeof(m_vram));
  memset(cgram, 0, sizeof(cgram));
  memset(oam, 0, sizeof(oam));
  memset(ppu_reg, 0, sizeof(ppu_reg));
  memset(ppu_wr, 0, sizeof(ppu_wr));
  memset(cpu_reg, 0, sizeof(cpu_reg));
  memset(dma_reg, 0, sizeof(dma_reg));
  memset(decoy_reg, 0, sizeof(decoy_reg)); phys_420c = 0; decoy_bad = 0; decoy_on = 0;
  vmain = 0; vmadd = 0; cgadd = 0; oam_ba = 0; oam_latch = 0; hv_hi = 0;
  wmadd = 0; mulres = 0; cmd_n = 0;
  /* The GBC wiring is reset too: a driver configures it AFTER m_reset_memory,
     so a stale view window or EF sink can never survive into a run that did
     not ask for one. */
  gbc = 0; view_win = NULL; ef_base = 0; ef_len = 0; ef_fn = NULL;
  rw_n = 0; rw_fn = NULL;
  pad_joy1 = 0; cmd_fn = NULL; dma_fn = NULL;
  bgofs_latch = 0; bghofs_latch = 0;
  clk_on = 0; clk_mc = 0; clk_line_start = 0; clk_pend = 0; clk_depth0 = 0;
  clk_line = 0; clk_lines = 262; clk_in_dma = 0; clk_io = 0; clk_waiting = 0;
  clk_depth = 0; nmi_pend = 0; rdnmi_flag = 0; irq_timeup = 0; v_latch = 0;
  clk_stall_pend = 0;
  cpu_rev = 2; cpu_pal = 0; dma_start_mc = dma_end_mc = 0;
  line_fn = NULL; int_fn = NULL; wwatch_fn = NULL;
  memset(bg_hofs, 0, sizeof(bg_hofs));
  memset(bg_vofs, 0, sizeof(bg_vofs));
  m_forced_blank = 0;
  memset(&m_cpu, 0, sizeof(m_cpu));
  m_cpu.s = 0x1fff;
  m_cpu.p = M_M | M_I;   /* native, A 8-bit, X/Y 16-bit (rep #$10 at RESET) */
  m_cpu.e = 0;
  memset(coldata, 0, sizeof coldata);
}

void m_load_rom(const uint8_t *src, size_t len) {
  /* The decode below is `(off - 0x8000) % rom_len`, which only describes a
     ONE-page LoROM (32KB mirrored over $00:8000-$FFFF).  A bigger ROM has
     distinct banks and would fold into bank $00 -- silently wrong. */
  if(len == 0 || len > sizeof(rom))
    M_DIE("ROM de %zu B fora de 1..%zu -- este modelo so' cobre UMA pagina LoROM\n",
          len, sizeof(rom));
  memcpy(rom, src, len);
  rom_len = len;
}

/* ---------------- address decode ---------------- */
/* Pointer into dumb memory (WRAM/ROM), or NULL when it is a register. */
static uint8_t *mem_ptr(uint32_t a, int write) {
  uint8_t bank = (uint8_t)(a >> 16);
  uint16_t off = (uint16_t)a;
  if(bank == 0x7e) return &wram[off];
  if(bank == 0x7f) return &wram[0x10000 + off];
  if(bank <= 0x3f || (bank >= 0x80 && bank <= 0xbf)) {
    if(off < 0x2000) return &wram[off];              /* LowRAM mirror */
    if(off >= 0x8000) {
      if(write) M_DIE("escrita em ROM $%06X\n", a);
      return &rom[(off - 0x8000) % rom_len];         /* LoROM, one page */
    }
    return NULL;                                     /* register */
  }
  M_DIE("access to unmodelled bank $%06X\n", a);
  return NULL;
}

/* ---------------- the clock ---------------- */
/* Master cycles one CPU access to `a` costs (fullsnes' memory map).  MEMSEL
 * ($420D) is not modelled: every ROM access is the slow 8. */
static int acc_cost(uint32_t a) {
  uint8_t bank = (uint8_t)(a >> 16);
  uint16_t off = (uint16_t)a;
  if(!(bank & 0x40)) {
    if(off < 0x2000) return 8;
    if(off < 0x4000) return 6;
    if(off < 0x4200) return 12;
    if(off < 0x6000) return 6;
  }
  return 8;
}

/* The per-line HDMA tax of contract sec. 11.1: 18 + 8C + 8B for C enabled
 * channels moving B bytes a line.  It is the contract's (pessimistic) model on
 * purpose -- B is charged on every line, hold entries included -- so the clock
 * and the player's budget arithmetic disagree only where the PLAYER's own
 * bookkeeping does, which is what this harness is for. */
static uint64_t hdma_tax(void) {
  static const int unit[8] = {1, 2, 2, 4, 4, 4, 2, 4};
  uint8_t en = decoy_on ? phys_420c : cpu_reg[0x0c];
  int ch, c = 0, b = 0;
  if(!en) return 0;
  for(ch = 0; ch < 8; ch++)
    if(en & (1 << ch)) {
      const uint8_t *r = !decoy_on ? &dma_reg[ch * 0x10]
                       : ch == 0 ? decoy_reg : ch == 6 ? &dma_reg[0] : &dma_reg[ch * 0x10];
      c++; b += unit[r[0] & 7];
    }
  return (uint64_t)(18 + 8 * c + 8 * b);
}

/* M65816_CPU_SCALE (env, read by m_clock_start): multiplies every CPU cost --
 * bus accesses and internal cycles of instructions and interrupt entries, NOT
 * DMA, refresh or the HDMA tax -- to ask "does anything that passes at the
 * modelled speed fail on a slower CPU?".  The model runs ~26 mc/instruction on
 * the player's code, the bsnes-plus harness measured 29.5 (+13 %).  Default 1:
 * byte-identical to the unscaled clock. */
/* m_clock_stall: master cycles the CPU loses at the end of the current line
 * hook (a test's way to make one stretch of code "slower than the model"
 * without touching the rest of the frame).  Applied inside clk_advance, after
 * the hook returns, so a hook never recurses into the clock. */
static uint32_t  clk_scale_q = 1024;
static uint64_t  clk_scale_rem;
static uint64_t cpu_cost(uint64_t c) {
  uint64_t t;
  if(clk_scale_q == 1024) return c;
  t = c * clk_scale_q + clk_scale_rem;
  clk_scale_rem = t & 1023;
  return t >> 10;
}

static void clk_advance(uint64_t d) {
  clk_mc += d;
  if(clk_depth == 0) clk_depth0 += d;
  while(clk_mc >= clk_line_start + 1364) {
    uint64_t stall = 40;                       /* DRAM refresh */
    clk_line_start += 1364;
    clk_line = (clk_line + 1) % clk_lines;
    if(clk_line <= 224) stall += hdma_tax();
    clk_mc += stall;
    if(clk_line == 225) {
      rdnmi_flag = 1;
      if(cpu_reg[0x00] & 0x80) nmi_pend = 1;
    }
    /* V-IRQ only (H-IRQ off): fires at the start of line VTIME. */
    if((cpu_reg[0x00] & 0x30) == 0x20 &&
       clk_line == (((cpu_reg[0x0a] & 1) << 8) | cpu_reg[0x09]))
      irq_timeup = 1;
    if(line_fn) line_fn(clk_line);
    if(clk_stall_pend) { clk_mc += clk_stall_pend; clk_stall_pend = 0; }
  }
}

/* ---------------- general-purpose DMA ---------------- */
static void reg_write(uint16_t off, uint8_t v);
static uint8_t reg_read(uint16_t off);
static uint8_t bus_read(uint32_t a);

static void dma_run(int ch) {
  static const uint8_t pat[8][4] = {
    {0,0,0,0}, {0,1,0,0}, {0,0,0,0}, {0,0,1,1},
    {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1}
  };
  static const int patlen[8] = {1,2,2,4,4,4,2,4};
  uint8_t *r = &dma_reg[ch * 0x10];
  uint8_t ctl = r[0], bbad = r[1], a1b = r[4];
  uint16_t a1 = (uint16_t)(r[2] | (r[3] << 8));
  uint32_t cnt = (uint32_t)(r[5] | (r[6] << 8));
  uint16_t a1_start = a1;
  uint32_t bdest;
  int mode = ctl & 7, step, i;
  if(cnt == 0) cnt = 0x10000;
  if(ctl & 0x80) M_DIE("DMA channel %d B->A not modelled (ctl=$%02X)\n", ch, ctl);
  /* Where the block lands, sampled BEFORE the transfer counts the port up. */
  bdest = (bbad == 0x18 || bbad == 0x19) ? (uint32_t)vmadd * 2 :
          (bbad == 0x22) ? cgadd :
          (bbad == 0x04) ? oam_ba :
          (bbad == 0x80) ? wmadd : 0;
  step = (ctl & 0x08) ? 0 : ((ctl & 0x10) ? -1 : 1);
  if(clk_on) {                  /* the instruction so far, then the transfer */
    clk_advance(cpu_cost(clk_pend)); clk_pend = 0;
    dma_start_mc = clk_mc;
  }
  clk_in_dma = 1;
  for(i = 0; (uint32_t)i < cnt; i++) {
    uint8_t v = bus_read(((uint32_t)a1b << 16) | a1);
    reg_write((uint16_t)(0x2100 + ((bbad + pat[mode][i % patlen[mode]]) & 0xff)), v);
    a1 = (uint16_t)(a1 + step);
  }
  r[2] = (uint8_t)a1; r[3] = (uint8_t)(a1 >> 8);
  r[5] = 0; r[6] = 0;
  clk_in_dma = 0;
  if(clk_on) {                  /* 8 mc a byte, 8 a channel, ~16 of start-up */
    clk_advance((uint64_t)cnt * 8 + 8 + 16);
    dma_end_mc = clk_mc;
  }
  if(dma_fn) dma_fn(ch, bbad, ((uint32_t)a1b << 16) | a1_start, cnt, bdest);
}

/* ---------------- registers ---------------- */
static uint16_t vmain_step(void) {
  switch(vmain & 0x03) { case 0: return 1; case 1: return 32; default: return 128; }
}

/* Write-twice scroll registers, through the shared BGOFS latch.  Called from
 * inside the $21xx whitelist, so it never widens what aborts. */
static void bgofs_write(uint16_t off, uint8_t v) {
  int n = (off - 0x210d) >> 1;          /* 0..3 = BG1..BG4 */
  if(off & 1) {                          /* $210D/$210F/$2111/$2113 = HOFS */
    bg_hofs[n] = (uint16_t)(((v << 8) | (bgofs_latch & ~7) | (bghofs_latch & 7)) & 0x3ff);
    bgofs_latch = v; bghofs_latch = v;
  } else {                               /* $210E/$2110/$2112/$2114 = VOFS */
    bg_vofs[n] = (uint16_t)(((v << 8) | bgofs_latch) & 0x3ff);
    bgofs_latch = v;
  }
}

static void reg_write(uint16_t off, uint8_t v) {
  if(off >= 0x2100 && off <= 0x21ff) {
    /* Shadow of the whole block, for a driver that wants to compare the
       register state against a model.  Purely additive: nothing reads it
       inside the interpreter, and the data ports below overwrite their own
       slot with the last byte pushed, which is what "last write" means. */
    ppu_reg[off & 0xff] = v;
    ppu_wr[off & 0xff]++;
    switch(off) {
      case 0x2100: m_forced_blank = (v & 0x80) ? 1 : 0; break;
      case 0x2132: if(v & 0x20) coldata[0] = v & 31;   /* each component is  */
                   if(v & 0x40) coldata[1] = v & 31;   /* selected by its bit */
                   if(v & 0x80) coldata[2] = v & 31;
                   break;
      case 0x2102: oam_ba = (uint16_t)(((oam_ba & 0x200) | (v << 1)) & 0x3ff); oam_latch = 0; break;
      case 0x2103: oam_ba = (uint16_t)((oam_ba & 0x1ff) | ((v & 1) << 9)); oam_latch = 0; break;
      case 0x2104:
        if(oam_ba < 0x200) {
          if(!(oam_ba & 1)) oam_latch = v;
          else { oam[oam_ba - 1] = oam_latch; oam[oam_ba] = v; }
        } else oam[oam_ba] = v;
        oam_ba = (uint16_t)((oam_ba + 1) & 0x3ff);
        break;
      case 0x2115:
        if(v & 0x0c) M_DIE("VMAIN address remap ($%02X) not modelled\n", v);
        vmain = v; break;
      case 0x2116: vmadd = (uint16_t)((vmadd & 0xff00) | v); break;
      case 0x2117: vmadd = (uint16_t)((vmadd & 0x00ff) | (v << 8)); break;
      case 0x2118:
        m_vram[(uint16_t)(vmadd * 2)] = v;
        if(!(vmain & 0x80)) vmadd = (uint16_t)(vmadd + vmain_step());
        break;
      case 0x2119:
        m_vram[(uint16_t)(vmadd * 2 + 1)] = v;
        if(vmain & 0x80) vmadd = (uint16_t)(vmadd + vmain_step());
        break;
      case 0x2121: cgadd = (uint16_t)(v << 1); break;
      case 0x2122: cgram[cgadd & 0x1ff] = v; cgadd = (uint16_t)((cgadd + 1) & 0x1ff); break;
      /* --- GBC: the WRAM port.  The player fetches its 64-byte status block
         by DMA with BBAD = $80, so this is reached both by a plain store and
         from inside dma_run(); modelling only the store would leave the one
         path that matters unmodelled. */
      case 0x2180:
        if(!gbc) M_DIE("WRAM port $2180 requires m_gbc_mode()\n");
        wram[wmadd & 0x1ffff] = v; wmadd = (wmadd + 1) & 0x1ffff; break;
      case 0x2181:
        if(!gbc) M_DIE("WRAM port $2181 requires m_gbc_mode()\n");
        wmadd = (wmadd & 0x1ff00) | v; break;
      case 0x2182:
        if(!gbc) M_DIE("WRAM port $2182 requires m_gbc_mode()\n");
        wmadd = (wmadd & 0x100ff) | ((uint32_t)v << 8); break;
      case 0x2183:
        if(!gbc) M_DIE("WRAM port $2183 requires m_gbc_mode()\n");
        wmadd = (wmadd & 0x0ffff) | ((uint32_t)(v & 1) << 16); break;
      default:
        /* MUTE BY WHITELIST, never by 'default'.  These only shape what the
           SCREEN shows -- none of them moves a byte of VRAM/CGRAM/OAM:
             $2101       OBSEL
             $2105-$2114 BGMODE/MOSAIC/BGxSC/BGxNBA + the 8 scrolls
             $2123-$2133 windows, TM/TS/TMW/TSW, color math, SETINI
           Everything else ABORTS: APU ports ($2140-$2143), Mode 7
           ($211A-$2120, the renderer is Mode 0), read-only $2134+. */
        if(off >= 0x210d && off <= 0x2114) { bgofs_write(off, v); break; }
        if(off == 0x2101 || (off >= 0x2105 && off <= 0x2114) ||
           (off >= 0x2123 && off <= 0x2133)) break;
        /* --- GBC: the APU ports.  There is no S-SMP here, so GbcApuUnmute's
           handshake times out and the boot carries on silent -- the same
           outcome a console with a dead APU would give, and nothing the
           VRAM/CGRAM/OAM gate depends on.  Sunk rather than aborted so the
           REAL GbcInit (of which the unmute is the last step) can run. */
        if(gbc && off >= 0x2140 && off <= 0x2143) break;
        /* --- GBC: Mode 7 ($211A-$2120).  The player runs mode 0 and never
           reads $2134-$2136, but GbcInit SWEEPS the whole $2101-$2133 block
           with stz -- "the menu hands this block over as it last used it and
           the console reset does not clear it" (contract sec. 11.3), so a
           stale mosaic or colour window would land here as a broken picture.
           Refusing the sweep would mean not running the real init. */
        if(gbc && off >= 0x211a && off <= 0x2120) break;
        M_DIE("write to unmodelled PPU register $%04X = $%02X\n", off, v);
    }
    return;
  }
  if(off >= 0x4300 && off <= 0x437f) {
    if(decoy_on && (off & 0x70) == 0x00) { decoy_reg[off & 0x0f] = v; return; }
    if(decoy_on && (off & 0x70) == 0x60) { dma_reg[off & 0x0f] = v; return; }
    dma_reg[off - 0x4300] = v; return;
  }
  if(off == 0x420b && decoy_on && (v & 0x7f))
    M_DIE("general DMA on $420B = $%02X: the GBC player runs every general DMA "
          "on ch7 (ch0 is the HDMA decoy, ch6 its logical channel 0)\n", v);
  if(off == 0x420b) { int c; cpu_reg[0x0b] = v; for(c = 0; c < 8; c++) if(v & (1 << c)) dma_run(c); return; }
  /* $4200 NMITIMEN and $420C HDMAEN are mute: no interrupts and no HDMA here
     (the driver CALLS the NMI, HDMA only affects the screen).  $420D (MEMSEL)
     and the IRQ timers stay out -- a register that returns garbage is the
     silent error this file refuses. */
  if(off == 0x4200) {
    uint8_t old = cpu_reg[0x00];
    cpu_reg[0x00] = v;
    /* Enabling the NMI inside a vblank whose flag is still up fires it at
       once (the 5A22 does); taking both timer IRQs away acknowledges one. */
    if(clk_on && !(old & 0x80) && (v & 0x80) && rdnmi_flag) nmi_pend = 1;
    if(!(v & 0x30)) irq_timeup = 0;
    return;
  }
  if(off == 0x420c) {
    if(decoy_on) {
      /* the lowest armed channel takes the 5A22's hit: it has to be the
         decoy (ch0) or the FB's constant window (ch4); ch7 is general DMA */
      phys_420c = v;
      if(v && (!(v & 0x11) || ((v & 0x01) == 0 && (v & 0x0f)) || (v & 0x80))) decoy_bad++;
      v = (uint8_t)((v & 0x3e) | ((v >> 6) & 1));
    }
    cpu_reg[0x0c] = v; return;
  }
  if(gbc) {
    /* $4201 WRIO: bit 7 high is what ARMS the H/V latch $2137 reads, and the
       whole transfer guard is a function of that latch.  Mute here (the latch
       is modelled as always available), but never silently: a player that
       stopped writing it would be a silicon bug this model cannot see. */
    if(off == 0x4201) { cpu_reg[0x01] = v; return; }
    /* $4207-$420A HTIME/VTIME.  Modelled only as the V-IRQ the clock raises
       (H-IRQ, $4200 b4, is not); without the clock they are just stored. */
    if(off >= 0x4207 && off <= 0x420a) { cpu_reg[off & 0xff] = v; return; }
    /* The 8x8 -> 16 unsigned multiplier.  GbcCapacity turns "scanlines left"
       into "byte-equivalents left" with it, so a stub returning 0 would hand
       the guard a capacity of zero and make every scenario look like the
       V-guard test.  Result latches on the $4203 write; the four NOPs the
       player spends waiting for it are free in a cycle-less model. */
    if(off == 0x4202) { cpu_reg[0x02] = v; return; }
    if(off == 0x4203) { cpu_reg[0x03] = v; mulres = (uint16_t)(cpu_reg[0x02] * v); return; }
    /* $2A00: the fork's MCU_CMD mailbox (snescmd BRAM).  The player writes one
       byte there when an IGR combo fires; it is a mailbox, not code. */
    if(off == 0x2a00) { cmd_n++; if(cmd_fn) cmd_fn(v); return; }
  }
  M_DIE("write to unmodelled register $%04X = $%02X\n", off, v);
}

static uint8_t reg_read(uint16_t off) {
  switch(off) {
    case 0x2137:             /* SLHV: latches H/V.  Leaves the OPHCT/OPVCT
                                hi/lo flip-flop alone; $213F resets it. */
      v_latch = clk_on ? (uint16_t)clk_line : m_vcounter; return 0;
    case 0x213c: return 0;                                /* OPHCT: H unused */
    case 0x213d: {                                        /* OPVCT: V[7:0] / V[8] */
      uint16_t vv = clk_on ? v_latch : m_vcounter;
      uint8_t r = hv_hi ? (uint8_t)((vv >> 8) & 1) : (uint8_t)(vv & 0xff);
      hv_hi = !hv_hi; return r;
    }
    case 0x213f: hv_hi = 0;                               /* STAT78: b4 = PAL, resets the ff */
      return (uint8_t)(0x01 | (cpu_pal ? 0x10 : 0));
    case 0x4210: {                                        /* RDNMI: b7 flag, b3-0 CPU rev */
      uint8_t r = (uint8_t)((rdnmi_flag ? 0x80 : 0) | 0x40 | (cpu_rev & 0x0f));
      rdnmi_flag = 0; return r;                           /* no clock: $42, as always */
    }
    case 0x4212: {                                        /* HVBJOY */
      uint64_t h; uint8_t r = 0;
      if(!clk_on) return 0x80;                            /* no clock: in vblank,
                                                             auto-joypad idle */
      h = clk_mc - clk_line_start;
      if(clk_line >= 225) r |= 0x80;
      if(h >= 1096) r |= 0x40;
      /* the auto-joypad read: from V=225 H~130, ~4224 mc */
      if((cpu_reg[0x00] & 1) &&
         ((clk_line == 225 && h >= 130) || clk_line == 226 || clk_line == 227 ||
          (clk_line == 228 && h < 262))) r |= 0x01;
      return r;
    }
  }
  if(off >= 0x4300 && off <= 0x437f) {
    if(decoy_on && (off & 0x70) == 0x00) return decoy_reg[off & 0x0f];
    if(decoy_on && (off & 0x70) == 0x60) return dma_reg[off & 0x0f];
    return dma_reg[off - 0x4300];
  }
  if(gbc) {
    if(off == 0x2180) { uint8_t r = wram[wmadd & 0x1ffff]; wmadd = (wmadd + 1) & 0x1ffff; return r; }
    if(off >= 0x2140 && off <= 0x2143) return 0x00;       /* no S-SMP: see reg_write */
    if(off == 0x4216) return (uint8_t)mulres;
    if(off == 0x4211) { uint8_t r = irq_timeup ? 0x80 : 0x00; irq_timeup = 0; return r; }
    if(off == 0x4217) return (uint8_t)(mulres >> 8);
    if(off == 0x4218) return (uint8_t)pad_joy1;
    if(off == 0x4219) return (uint8_t)(pad_joy1 >> 8);
  }
  M_DIE("read from unmodelled register $%04X\n", off);
  return 0;
}

/* The GBC address window sits in FRONT of mem_ptr: banks $E0-$E3 and $EF are
 * not memory in this chassis, they are the FPGA's view/write ports, and
 * mem_ptr would abort on them (correctly, for the NES gate). */
static uint8_t bus_read(uint32_t a) {
  if(clk_on && !clk_in_dma) clk_pend += (uint64_t)acc_cost(a);
  if(rw_fn) {                 /* m_set_read_watch: the bridge model catches up */
    int i;
    for(i = 0; i < rw_n; i++) if(a >= rw_lo[i] && a <= rw_hi[i]) { rw_fn(a); break; }
  }
  if(gbc) {
    uint8_t bank = (uint8_t)(a >> 16);
    if(bank >= 0xe0 && bank <= 0xe4) {
      if(!view_win) M_DIE("leitura de view $%06X sem m_set_view_window()\n", a);
      return view_win[(((uint32_t)(bank - 0xe0)) << 16) | (uint16_t)a];
    }
    if(bank == 0xef) return 0x00;   /* contract sec. 3: write-only, reads $00 */
  }
  { uint8_t *p = mem_ptr(a, 0);
    return p ? *p : reg_read((uint16_t)a); }
}
static void bus_write(uint32_t a, uint8_t v) {
  if(clk_on && !clk_in_dma) clk_pend += (uint64_t)acc_cost(a);
  if(wwatch_fn && !clk_in_dma) wwatch_fn(a);
  if(gbc) {
    uint8_t bank = (uint8_t)(a >> 16);
    if(ef_len && a >= ef_base && a < ef_base + ef_len) {
      /* In the harness build the window is a WRAM page, so the store has to
         LAND as well as be reported; on $EF nothing backs it. */
      if(bank != 0xef) { uint8_t *p = mem_ptr(a, 1); if(p) *p = v; }
      if(ef_fn) ef_fn(a - ef_base, v);
      return;
    }
    if(bank == 0xef)
      M_DIE("escrita em $%06X fora da janela $EF declarada\n", a);
    if(bank >= 0xe0 && bank <= 0xe4)
      M_DIE("escrita na view READ-ONLY $%06X = $%02X\n", a, v);
  }
  { uint8_t *p = mem_ptr(a, 1);
    if(p) *p = v; else reg_write((uint16_t)a, v); }
}

/* --- GBC wiring (see m65816.h) --- */
void m_gbc_mode(int on) { gbc = on ? 1 : 0; }
void m_hdma_decoy(int on) { decoy_on = on ? 1 : 0; }
void m_set_view_window(uint8_t *buf) { view_win = buf; }
void m_set_read_watch(int n, const uint32_t *lo, const uint32_t *hi, void (*fn)(uint32_t)) {
  int i;
  if(n < 0 || n > RW_MAX) M_DIE("m_set_read_watch: %d faixas (max %d)\n", n, RW_MAX);
  for(i = 0; i < n; i++) { rw_lo[i] = lo[i]; rw_hi[i] = hi[i]; }
  rw_n = n; rw_fn = n ? fn : NULL;
}
uint64_t m_clock_now(void) { return clk_mc + (clk_in_dma ? 0 : clk_pend); }
void m_set_ef_sink(uint32_t base24, uint32_t len, void (*fn)(uint32_t, uint8_t)) {
  ef_base = base24; ef_len = len; ef_fn = fn;
}
void m_set_pad(uint16_t joy1) { pad_joy1 = joy1; }
void m_set_cmd_sink(void (*fn)(uint8_t)) { cmd_fn = fn; }
void m_set_v(int v) {
  if(v < 0 || v > 511) M_DIE("m_set_v(%d): V so' tem 9 bits\n", v);
  m_vcounter = (uint16_t)v;
}
void m_set_dma_hook(void (*fn)(int, uint8_t, uint32_t, uint32_t, uint32_t)) { dma_fn = fn; }
const uint8_t *m_ppu_regs(void) { return ppu_reg; }
const uint32_t *m_ppu_reg_writes(void) { return ppu_wr; }
const uint8_t *m_cpu_regs(void) { return cpu_reg; }
const uint8_t *m_dma_regs(void) { return dma_reg; }
const uint8_t *m_decoy_regs(void) { return decoy_reg; }
uint8_t m_phys_420c(void) { return phys_420c; }
unsigned m_decoy_bad(void) { return decoy_bad; }
uint16_t m_bg_hofs(int n) { if(n < 1 || n > 4) M_DIE("m_bg_hofs(%d)\n", n); return bg_hofs[n - 1]; }
uint8_t m_coldata(int c) { if(c < 0 || c > 2) M_DIE("m_coldata(%d)\n", c); return coldata[c]; }
uint16_t m_bg_vofs(int n) { if(n < 1 || n > 4) M_DIE("m_bg_vofs(%d)\n", n); return bg_vofs[n - 1]; }
unsigned m_cmd_writes(void) { return cmd_n; }
uint16_t m_stack_low(void) { return stack_low; }
void m_stack_low_reset(void) { stack_low = m_cpu.s; }
uint8_t *m_cgram(void) { return cgram; }
uint8_t *m_oam(void)   { return oam; }

/* ---------------- host access ---------------- */
uint8_t m_peek(uint32_t a)             { uint8_t *p = mem_ptr(a, 0); if(!p) M_DIE("peek em registrador $%06X\n", a); return *p; }
void    m_poke(uint32_t a, uint8_t v)  { uint8_t *p = mem_ptr(a, 1); if(!p) M_DIE("poke em registrador $%06X\n", a); *p = v; }
uint16_t m_peek16(uint32_t a)          { return (uint16_t)(m_peek(a) | (m_peek(a + 1) << 8)); }
void    m_poke16(uint32_t a, uint16_t v) { m_poke(a, (uint8_t)v); m_poke(a + 1, (uint8_t)(v >> 8)); }
void    m_poke_block(uint32_t a, const void *src, size_t n) {
  const uint8_t *s = (const uint8_t*)src; size_t i;
  for(i = 0; i < n; i++) m_poke(a + i, s[i]);
}

/* ---------------- CPU ---------------- */
#define P8()  (m_cpu.p & M_M)     /* 8-bit accumulator */
#define I8()  (m_cpu.p & M_X)     /* 8-bit indices */

static uint8_t  fetch8(void)  { uint8_t v = bus_read(((uint32_t)m_cpu.pbr << 16) | m_cpu.pc); m_cpu.pc = (uint16_t)(m_cpu.pc + 1); return v; }
static uint16_t fetch16(void) { uint16_t l = fetch8(); return (uint16_t)(l | (fetch8() << 8)); }
static uint32_t fetch24(void) { uint32_t l = fetch16(); return l | ((uint32_t)fetch8() << 16); }

static uint16_t rd16(uint32_t a) { return (uint16_t)(bus_read(a) | (bus_read((a + 1) & 0xffffff) << 8)); }
static void     wr16(uint32_t a, uint16_t v) { bus_write(a, (uint8_t)v); bus_write((a + 1) & 0xffffff, (uint8_t)(v >> 8)); }

static void push8(uint8_t v)  { bus_write(m_cpu.s, v); m_cpu.s = (uint16_t)(m_cpu.s - 1);
                                if(m_cpu.s < stack_low) stack_low = m_cpu.s; }
static uint8_t pull8(void)    { m_cpu.s = (uint16_t)(m_cpu.s + 1); return bus_read(m_cpu.s); }
static void push16(uint16_t v){ push8((uint8_t)(v >> 8)); push8((uint8_t)v); }
static uint16_t pull16(void)  { uint16_t l = pull8(); return (uint16_t)(l | (pull8() << 8)); }

static void setnz8(uint8_t v)  { m_cpu.p &= (uint8_t)~(M_N | M_Z); if(!v) m_cpu.p |= M_Z; if(v & 0x80) m_cpu.p |= M_N; }
static void setnz16(uint16_t v){ m_cpu.p &= (uint8_t)~(M_N | M_Z); if(!v) m_cpu.p |= M_Z; if(v & 0x8000) m_cpu.p |= M_N; }

/* --- addressing modes (return a 24-bit address) --- */
static uint32_t am_dp(void)    { return (uint16_t)(m_cpu.d + fetch8()); }
static uint32_t am_dpx(void)   { return (uint16_t)(m_cpu.d + fetch8() + m_cpu.x); }
static uint32_t am_dpy(void)   { return (uint16_t)(m_cpu.d + fetch8() + m_cpu.y); }
static uint32_t am_idl(void)   { uint16_t b = (uint16_t)(m_cpu.d + fetch8());
                                 return (uint32_t)bus_read(b) | ((uint32_t)bus_read((uint16_t)(b + 1)) << 8)
                                        | ((uint32_t)bus_read((uint16_t)(b + 2)) << 16); }
static uint32_t am_idly(void)  { return (am_idl() + m_cpu.y) & 0xffffff; }
static uint32_t am_idp(void)   { uint16_t b = (uint16_t)(m_cpu.d + fetch8());
                                 uint16_t a = (uint16_t)(bus_read(b) | (bus_read((uint16_t)(b + 1)) << 8));
                                 return ((uint32_t)m_cpu.dbr << 16) | a; }
static uint32_t am_idpy(void)  { return (am_idp() + m_cpu.y) & 0xffffff; }
static uint32_t am_idpx(void)  { uint16_t b = (uint16_t)(m_cpu.d + fetch8() + m_cpu.x);
                                 uint16_t a = (uint16_t)(bus_read(b) | (bus_read((uint16_t)(b + 1)) << 8));
                                 return ((uint32_t)m_cpu.dbr << 16) | a; }
static uint32_t am_abs(void)   { return ((uint32_t)m_cpu.dbr << 16) | fetch16(); }
static uint32_t am_absx(void)  { return (am_abs() + m_cpu.x) & 0xffffff; }
static uint32_t am_absy(void)  { return (am_abs() + m_cpu.y) & 0xffffff; }
static uint32_t am_long(void)  { return fetch24(); }
static uint32_t am_longx(void) { return (fetch24() + m_cpu.x) & 0xffffff; }
static uint32_t am_sr(void)    { return (uint16_t)(m_cpu.s + fetch8()); }
static uint32_t am_sry(void)   { uint16_t b = (uint16_t)(m_cpu.s + fetch8());
                                 uint16_t a = (uint16_t)(bus_read(b) | (bus_read((uint16_t)(b + 1)) << 8));
                                 return (((uint32_t)m_cpu.dbr << 16) + a + m_cpu.y) & 0xffffff; }

/* --- load/store at the current width --- */
static uint16_t ld(uint32_t a, int wide) { return wide ? rd16(a) : bus_read(a); }
static void     st(uint32_t a, uint16_t v, int wide) { if(wide) wr16(a, v); else bus_write(a, (uint8_t)v); }

static void set_a(uint16_t v) { if(P8()) { m_cpu.a = (uint16_t)((m_cpu.a & 0xff00) | (v & 0xff)); setnz8((uint8_t)v); } else { m_cpu.a = v; setnz16(v); } }

static void op_adc(uint16_t v) {
  if(m_cpu.p & M_D) M_DIE("ADC in decimal mode not modelled\n");
  if(P8()) {
    unsigned a = m_cpu.a & 0xff, r = a + (v & 0xff) + (m_cpu.p & M_C ? 1 : 0);
    m_cpu.p &= (uint8_t)~(M_C | M_V);
    if(r > 0xff) m_cpu.p |= M_C;
    if(~(a ^ (v & 0xff)) & (a ^ r) & 0x80) m_cpu.p |= M_V;
    m_cpu.a = (uint16_t)((m_cpu.a & 0xff00) | (r & 0xff)); setnz8((uint8_t)r);
  } else {
    unsigned a = m_cpu.a, r = a + v + (m_cpu.p & M_C ? 1 : 0);
    m_cpu.p &= (uint8_t)~(M_C | M_V);
    if(r > 0xffff) m_cpu.p |= M_C;
    if(~(a ^ v) & (a ^ r) & 0x8000) m_cpu.p |= M_V;
    m_cpu.a = (uint16_t)r; setnz16((uint16_t)r);
  }
}
static void op_sbc(uint16_t v) {
  if(m_cpu.p & M_D) M_DIE("SBC in decimal mode not modelled\n");
  op_adc(P8() ? (uint16_t)(~v & 0xff) : (uint16_t)~v);
}
static void op_cmp(uint16_t reg, uint16_t v, int wide) {
  if(wide) { unsigned r = (unsigned)reg - v; m_cpu.p &= (uint8_t)~M_C; if(reg >= v) m_cpu.p |= M_C; setnz16((uint16_t)r); }
  else { unsigned a = reg & 0xff, b = v & 0xff, r = a - b; m_cpu.p &= (uint8_t)~M_C; if(a >= b) m_cpu.p |= M_C; setnz8((uint8_t)r); }
}
static uint16_t op_asl(uint16_t v, int wide) {
  m_cpu.p &= (uint8_t)~M_C;
  if(wide) { if(v & 0x8000) m_cpu.p |= M_C; v = (uint16_t)(v << 1); setnz16(v); }
  else { if(v & 0x80) m_cpu.p |= M_C; v = (uint8_t)(v << 1); setnz8((uint8_t)v); }
  return v;
}
static uint16_t op_lsr(uint16_t v, int wide) {
  m_cpu.p &= (uint8_t)~M_C; if(v & 1) m_cpu.p |= M_C;
  if(wide) { v = (uint16_t)(v >> 1); setnz16(v); } else { v = (uint8_t)((v & 0xff) >> 1); setnz8((uint8_t)v); }
  return v;
}
static uint16_t op_rol(uint16_t v, int wide) {
  int c = (m_cpu.p & M_C) ? 1 : 0;
  m_cpu.p &= (uint8_t)~M_C;
  if(wide) { if(v & 0x8000) m_cpu.p |= M_C; v = (uint16_t)((v << 1) | c); setnz16(v); }
  else { if(v & 0x80) m_cpu.p |= M_C; v = (uint8_t)((v << 1) | c); setnz8((uint8_t)v); }
  return v;
}
static uint16_t op_ror(uint16_t v, int wide) {
  int c = (m_cpu.p & M_C) ? 1 : 0;
  m_cpu.p &= (uint8_t)~M_C; if(v & 1) m_cpu.p |= M_C;
  if(wide) { v = (uint16_t)((v >> 1) | (c << 15)); setnz16(v); }
  else { v = (uint8_t)(((v & 0xff) >> 1) | (c << 7)); setnz8((uint8_t)v); }
  return v;
}
static uint16_t op_inc(uint16_t v, int wide) { if(wide) { v = (uint16_t)(v + 1); setnz16(v); } else { v = (uint8_t)(v + 1); setnz8((uint8_t)v); } return v; }
static uint16_t op_dec(uint16_t v, int wide) { if(wide) { v = (uint16_t)(v - 1); setnz16(v); } else { v = (uint8_t)(v - 1); setnz8((uint8_t)v); } return v; }
static void op_bit(uint16_t v, int wide) {
  uint16_t a = P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a;
  m_cpu.p &= (uint8_t)~(M_N | M_V | M_Z);
  if(!(a & v)) m_cpu.p |= M_Z;
  if(wide) { if(v & 0x8000) m_cpu.p |= M_N; if(v & 0x4000) m_cpu.p |= M_V; }
  else     { if(v & 0x80)   m_cpu.p |= M_N; if(v & 0x40)   m_cpu.p |= M_V; }
}
static void branch(int take) { int8_t d = (int8_t)fetch8(); if(take) { clk_io++; m_cpu.pc = (uint16_t)(m_cpu.pc + d); } }

static void set_x(uint16_t v) { if(I8()) { m_cpu.x = (uint16_t)(v & 0xff); setnz8((uint8_t)v); } else { m_cpu.x = v; setnz16(v); } }

/* Called whenever P may have GAINED the X bit (SEP, PLP, RTI).  On the 65816
 * the high byte of X and Y is LOST the moment that flag goes up and a later
 * rep does not bring it back, so `sep #$10` then `abs,X` reads elsewhere. */
static void p_narrow_index(void) { if(m_cpu.p & M_X) { m_cpu.x &= 0x00ff; m_cpu.y &= 0x00ff; } }
static void set_y(uint16_t v) { if(I8()) { m_cpu.y = (uint16_t)(v & 0xff); setnz8((uint8_t)v); } else { m_cpu.y = v; setnz16(v); } }

/* Internal (I/O) cycles of each opcode, 6 mc each, on top of its bus
 * accesses -- which bus_read/bus_write already charge.  The native-mode
 * figures of the 65816 data sheet, simplified where the player does not care:
 * every indexed absolute / (dp),Y is charged its +1 (16-bit index or page
 * cross), and the "DL != 0" +1 never applies (the player runs with D = 0).
 * A taken branch adds its +1 in branch(), MVN its +2 per byte in the case. */
static int io_cycles(uint8_t op) {
  switch(op) {
    /* implied, 2 cycles */
    case 0x18: case 0x38: case 0x58: case 0x78: case 0xd8: case 0xf8: case 0xb8:
    case 0xaa: case 0xa8: case 0x8a: case 0x98: case 0x9b: case 0xbb: case 0xba:
    case 0x9a: case 0x5b: case 0x7b: case 0x1b: case 0x3b: case 0xe8: case 0xc8:
    case 0xca: case 0x88: case 0x1a: case 0x3a: case 0x0a: case 0x4a: case 0x2a:
    case 0x6a: case 0xea: case 0xfb: case 0xc2: case 0xe2:
    /* pushes, jsr/jsl, per, brl, (abs,X) jumps */
    case 0x48: case 0xda: case 0x5a: case 0x08: case 0x8b: case 0x0b: case 0x4b:
    case 0x20: case 0x22: case 0x62: case 0x82: case 0x7c: case 0xfc:
    /* dp,X / dp,Y */
    case 0x15: case 0x35: case 0x55: case 0x75: case 0x95: case 0xb5: case 0xd5:
    case 0xf5: case 0x94: case 0xb4: case 0x34: case 0x74: case 0x96: case 0xb6:
    /* abs,X / abs,Y / (dp),Y / (dp,X) / sr */
    case 0x1d: case 0x3d: case 0x5d: case 0x7d: case 0x9d: case 0xbd: case 0xdd:
    case 0xfd: case 0x19: case 0x39: case 0x59: case 0x79: case 0x99: case 0xb9:
    case 0xd9: case 0xf9: case 0xbc: case 0xbe: case 0x3c: case 0x9e:
    case 0x11: case 0x31: case 0x51: case 0x71: case 0x91: case 0xb1: case 0xd1:
    case 0xf1: case 0x01: case 0x21: case 0x41: case 0x61: case 0x81: case 0xa1:
    case 0xc1: case 0xe1: case 0x03: case 0x23: case 0x43: case 0x63: case 0x83:
    case 0xa3: case 0xc3: case 0xe3:
    /* read-modify-write on memory */
    case 0x06: case 0x0e: case 0x46: case 0x4e: case 0x26: case 0x2e: case 0x66:
    case 0x6e: case 0xe6: case 0xee: case 0xc6: case 0xce: case 0x04: case 0x0c:
    case 0x14: case 0x1c:
      return 1;
    /* rtl: op, IO, IO, 3 pulls */
    case 0xeb: case 0x68: case 0xfa: case 0x7a: case 0x28: case 0xab: case 0x2b:
    case 0x6b: case 0x40: case 0xcb: case 0x13: case 0x33: case 0x53: case 0x73:
    case 0x93: case 0xb3: case 0xd3: case 0xf3:
    /* indexed read-modify-write */
    case 0x16: case 0x1e: case 0x56: case 0x5e: case 0x36: case 0x3e: case 0x76:
    case 0x7e: case 0xf6: case 0xfe: case 0xd6: case 0xde:
      return 2;
    /* rts: op, IO, IO, PCL, PCH, IO -- three internal cycles, the datasheet's
       6-cycle count (it used to be charged 2, like rtl was charged 1: ~6 mc
       short per return, and the player is subroutine-heavy) */
    case 0x60:
      return 3;
  }
  return 0;
}

/* One instruction. */
static void exec_one(void) {
  uint8_t op;
  if(pc_fn) pc_fn(((uint32_t)m_cpu.pbr << 16) | m_cpu.pc);
  op = fetch8();
  int wa = !P8(), wi = !I8();
  uint32_t ea;
  m_cpu.instrs++;
  clk_io += io_cycles(op);
  switch(op) {
    /* --- LDA --- */
    case 0xa9: set_a(wa ? fetch16() : fetch8()); break;
    case 0xa5: set_a(ld(am_dp(),   wa)); break;
    case 0xb5: set_a(ld(am_dpx(),  wa)); break;
    case 0xad: set_a(ld(am_abs(),  wa)); break;
    case 0xbd: set_a(ld(am_absx(), wa)); break;
    case 0xb9: set_a(ld(am_absy(), wa)); break;
    case 0xaf: set_a(ld(am_long(), wa)); break;
    case 0xbf: set_a(ld(am_longx(),wa)); break;
    case 0xa7: set_a(ld(am_idl(),  wa)); break;
    case 0xb7: set_a(ld(am_idly(), wa)); break;
    case 0xb2: set_a(ld(am_idp(),  wa)); break;
    case 0xb1: set_a(ld(am_idpy(), wa)); break;
    case 0xa1: set_a(ld(am_idpx(), wa)); break;
    case 0xa3: set_a(ld(am_sr(),   wa)); break;
    case 0xb3: set_a(ld(am_sry(),  wa)); break;
    /* --- LDX/LDY --- */
    case 0xa2: set_x(wi ? fetch16() : fetch8()); break;
    case 0xa6: set_x(ld(am_dp(),  wi)); break;
    case 0xb6: set_x(ld(am_dpy(), wi)); break;
    case 0xae: set_x(ld(am_abs(), wi)); break;
    case 0xbe: set_x(ld(am_absy(),wi)); break;
    case 0xa0: set_y(wi ? fetch16() : fetch8()); break;
    case 0xa4: set_y(ld(am_dp(),  wi)); break;
    case 0xb4: set_y(ld(am_dpx(), wi)); break;
    case 0xac: set_y(ld(am_abs(), wi)); break;
    case 0xbc: set_y(ld(am_absx(),wi)); break;
    /* --- STA --- */
    case 0x85: st(am_dp(),   m_cpu.a, wa); break;
    case 0x95: st(am_dpx(),  m_cpu.a, wa); break;
    case 0x8d: st(am_abs(),  m_cpu.a, wa); break;
    case 0x9d: st(am_absx(), m_cpu.a, wa); break;
    case 0x99: st(am_absy(), m_cpu.a, wa); break;
    case 0x8f: st(am_long(), m_cpu.a, wa); break;
    case 0x9f: st(am_longx(),m_cpu.a, wa); break;
    case 0x87: st(am_idl(),  m_cpu.a, wa); break;
    case 0x97: st(am_idly(), m_cpu.a, wa); break;
    case 0x92: st(am_idp(),  m_cpu.a, wa); break;
    case 0x91: st(am_idpy(), m_cpu.a, wa); break;
    case 0x81: st(am_idpx(), m_cpu.a, wa); break;
    case 0x83: st(am_sr(),   m_cpu.a, wa); break;
    case 0x93: st(am_sry(),  m_cpu.a, wa); break;
    /* --- STX/STY/STZ --- */
    case 0x86: st(am_dp(),  m_cpu.x, wi); break;
    case 0x96: st(am_dpy(), m_cpu.x, wi); break;
    case 0x8e: st(am_abs(), m_cpu.x, wi); break;
    case 0x84: st(am_dp(),  m_cpu.y, wi); break;
    case 0x94: st(am_dpx(), m_cpu.y, wi); break;
    case 0x8c: st(am_abs(), m_cpu.y, wi); break;
    case 0x64: st(am_dp(),   0, wa); break;
    case 0x74: st(am_dpx(),  0, wa); break;
    case 0x9c: st(am_abs(),  0, wa); break;
    case 0x9e: st(am_absx(), 0, wa); break;
    /* --- ADC/SBC --- */
    case 0x69: op_adc(wa ? fetch16() : fetch8()); break;
    case 0x65: op_adc(ld(am_dp(),   wa)); break;
    case 0x75: op_adc(ld(am_dpx(),  wa)); break;
    case 0x6d: op_adc(ld(am_abs(),  wa)); break;
    case 0x7d: op_adc(ld(am_absx(), wa)); break;
    case 0x79: op_adc(ld(am_absy(), wa)); break;
    case 0x6f: op_adc(ld(am_long(), wa)); break;
    case 0x7f: op_adc(ld(am_longx(),wa)); break;
    case 0x67: op_adc(ld(am_idl(),  wa)); break;
    case 0x77: op_adc(ld(am_idly(), wa)); break;
    case 0x72: op_adc(ld(am_idp(),  wa)); break;
    case 0x71: op_adc(ld(am_idpy(), wa)); break;
    case 0x61: op_adc(ld(am_idpx(), wa)); break;
    case 0x63: op_adc(ld(am_sr(),   wa)); break;
    case 0x73: op_adc(ld(am_sry(),  wa)); break;
    case 0xe9: op_sbc(wa ? fetch16() : fetch8()); break;
    case 0xe5: op_sbc(ld(am_dp(),   wa)); break;
    case 0xf5: op_sbc(ld(am_dpx(),  wa)); break;
    case 0xed: op_sbc(ld(am_abs(),  wa)); break;
    case 0xfd: op_sbc(ld(am_absx(), wa)); break;
    case 0xf9: op_sbc(ld(am_absy(), wa)); break;
    case 0xef: op_sbc(ld(am_long(), wa)); break;
    case 0xff: op_sbc(ld(am_longx(),wa)); break;
    case 0xe7: op_sbc(ld(am_idl(),  wa)); break;
    case 0xf7: op_sbc(ld(am_idly(), wa)); break;
    case 0xf2: op_sbc(ld(am_idp(),  wa)); break;
    case 0xf1: op_sbc(ld(am_idpy(), wa)); break;
    case 0xe1: op_sbc(ld(am_idpx(), wa)); break;
    case 0xe3: op_sbc(ld(am_sr(),   wa)); break;
    case 0xf3: op_sbc(ld(am_sry(),  wa)); break;
    /* --- CMP/CPX/CPY --- */
    case 0xc9: op_cmp(m_cpu.a, wa ? fetch16() : fetch8(), wa); break;
    case 0xc5: op_cmp(m_cpu.a, ld(am_dp(),   wa), wa); break;
    case 0xd5: op_cmp(m_cpu.a, ld(am_dpx(),  wa), wa); break;
    case 0xcd: op_cmp(m_cpu.a, ld(am_abs(),  wa), wa); break;
    case 0xdd: op_cmp(m_cpu.a, ld(am_absx(), wa), wa); break;
    case 0xd9: op_cmp(m_cpu.a, ld(am_absy(), wa), wa); break;
    case 0xcf: op_cmp(m_cpu.a, ld(am_long(), wa), wa); break;
    case 0xdf: op_cmp(m_cpu.a, ld(am_longx(),wa), wa); break;
    case 0xc7: op_cmp(m_cpu.a, ld(am_idl(),  wa), wa); break;
    case 0xd7: op_cmp(m_cpu.a, ld(am_idly(), wa), wa); break;
    case 0xd2: op_cmp(m_cpu.a, ld(am_idp(),  wa), wa); break;
    case 0xd1: op_cmp(m_cpu.a, ld(am_idpy(), wa), wa); break;
    case 0xc1: op_cmp(m_cpu.a, ld(am_idpx(), wa), wa); break;
    case 0xc3: op_cmp(m_cpu.a, ld(am_sr(),   wa), wa); break;
    case 0xd3: op_cmp(m_cpu.a, ld(am_sry(),  wa), wa); break;
    case 0xe0: op_cmp(m_cpu.x, wi ? fetch16() : fetch8(), wi); break;
    case 0xe4: op_cmp(m_cpu.x, ld(am_dp(),  wi), wi); break;
    case 0xec: op_cmp(m_cpu.x, ld(am_abs(), wi), wi); break;
    case 0xc0: op_cmp(m_cpu.y, wi ? fetch16() : fetch8(), wi); break;
    case 0xc4: op_cmp(m_cpu.y, ld(am_dp(),  wi), wi); break;
    case 0xcc: op_cmp(m_cpu.y, ld(am_abs(), wi), wi); break;
    /* --- AND/ORA/EOR --- */
#define LOGIC(OPC, EXPR) case OPC: { uint16_t v; v = EXPR; \
      if(wa) { m_cpu.a = (uint16_t)(m_cpu.a OPSYM v); setnz16(m_cpu.a); } \
      else { m_cpu.a = (uint16_t)((m_cpu.a & 0xff00) | ((m_cpu.a OPSYM v) & 0xff)); setnz8((uint8_t)m_cpu.a); } } break;
#define OPSYM &
    LOGIC(0x29, wa ? fetch16() : fetch8())
    LOGIC(0x25, ld(am_dp(),   wa)) LOGIC(0x35, ld(am_dpx(),  wa))
    LOGIC(0x2d, ld(am_abs(),  wa)) LOGIC(0x3d, ld(am_absx(), wa))
    LOGIC(0x39, ld(am_absy(), wa)) LOGIC(0x2f, ld(am_long(), wa))
    LOGIC(0x3f, ld(am_longx(),wa)) LOGIC(0x27, ld(am_idl(),  wa))
    LOGIC(0x37, ld(am_idly(), wa)) LOGIC(0x32, ld(am_idp(),  wa))
    LOGIC(0x31, ld(am_idpy(), wa)) LOGIC(0x21, ld(am_idpx(), wa))
    LOGIC(0x23, ld(am_sr(),   wa)) LOGIC(0x33, ld(am_sry(),  wa))
#undef OPSYM
#define OPSYM |
    LOGIC(0x09, wa ? fetch16() : fetch8())
    LOGIC(0x05, ld(am_dp(),   wa)) LOGIC(0x15, ld(am_dpx(),  wa))
    LOGIC(0x0d, ld(am_abs(),  wa)) LOGIC(0x1d, ld(am_absx(), wa))
    LOGIC(0x19, ld(am_absy(), wa)) LOGIC(0x0f, ld(am_long(), wa))
    LOGIC(0x1f, ld(am_longx(),wa)) LOGIC(0x07, ld(am_idl(),  wa))
    LOGIC(0x17, ld(am_idly(), wa)) LOGIC(0x12, ld(am_idp(),  wa))
    LOGIC(0x11, ld(am_idpy(), wa)) LOGIC(0x01, ld(am_idpx(), wa))
    LOGIC(0x03, ld(am_sr(),   wa)) LOGIC(0x13, ld(am_sry(),  wa))
#undef OPSYM
#define OPSYM ^
    LOGIC(0x49, wa ? fetch16() : fetch8())
    LOGIC(0x45, ld(am_dp(),   wa)) LOGIC(0x55, ld(am_dpx(),  wa))
    LOGIC(0x4d, ld(am_abs(),  wa)) LOGIC(0x5d, ld(am_absx(), wa))
    LOGIC(0x59, ld(am_absy(), wa)) LOGIC(0x4f, ld(am_long(), wa))
    LOGIC(0x5f, ld(am_longx(),wa)) LOGIC(0x47, ld(am_idl(),  wa))
    LOGIC(0x57, ld(am_idly(), wa)) LOGIC(0x52, ld(am_idp(),  wa))
    LOGIC(0x51, ld(am_idpy(), wa)) LOGIC(0x41, ld(am_idpx(), wa))
    LOGIC(0x43, ld(am_sr(),   wa)) LOGIC(0x53, ld(am_sry(),  wa))
#undef OPSYM
#undef LOGIC
    /* --- BIT/TRB/TSB --- */
    case 0x89: { uint16_t v = wa ? fetch16() : fetch8(); uint16_t a = P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a;
                 m_cpu.p &= (uint8_t)~M_Z; if(!(a & v)) m_cpu.p |= M_Z; } break;
    case 0x24: op_bit(ld(am_dp(),   wa), wa); break;
    case 0x34: op_bit(ld(am_dpx(),  wa), wa); break;
    case 0x2c: op_bit(ld(am_abs(),  wa), wa); break;
    case 0x3c: op_bit(ld(am_absx(), wa), wa); break;
    case 0x14: case 0x1c: case 0x04: case 0x0c: {
      int trb = (op == 0x14 || op == 0x1c);
      uint16_t a = P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a;
      ea = (op == 0x14 || op == 0x04) ? am_dp() : am_abs();
      { uint16_t v = ld(ea, wa);
        m_cpu.p &= (uint8_t)~M_Z; if(!(a & v)) m_cpu.p |= M_Z;
        st(ea, trb ? (uint16_t)(v & ~a) : (uint16_t)(v | a), wa); }
    } break;
    /* --- shifts --- */
    case 0x0a: set_a(op_asl(P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a, wa)); break;
    case 0x4a: set_a(op_lsr(P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a, wa)); break;
    case 0x2a: set_a(op_rol(P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a, wa)); break;
    case 0x6a: set_a(op_ror(P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a, wa)); break;
    case 0x06: ea = am_dp();   st(ea, op_asl(ld(ea, wa), wa), wa); break;
    case 0x16: ea = am_dpx();  st(ea, op_asl(ld(ea, wa), wa), wa); break;
    case 0x0e: ea = am_abs();  st(ea, op_asl(ld(ea, wa), wa), wa); break;
    case 0x1e: ea = am_absx(); st(ea, op_asl(ld(ea, wa), wa), wa); break;
    case 0x46: ea = am_dp();   st(ea, op_lsr(ld(ea, wa), wa), wa); break;
    case 0x56: ea = am_dpx();  st(ea, op_lsr(ld(ea, wa), wa), wa); break;
    case 0x4e: ea = am_abs();  st(ea, op_lsr(ld(ea, wa), wa), wa); break;
    case 0x5e: ea = am_absx(); st(ea, op_lsr(ld(ea, wa), wa), wa); break;
    case 0x26: ea = am_dp();   st(ea, op_rol(ld(ea, wa), wa), wa); break;
    case 0x36: ea = am_dpx();  st(ea, op_rol(ld(ea, wa), wa), wa); break;
    case 0x2e: ea = am_abs();  st(ea, op_rol(ld(ea, wa), wa), wa); break;
    case 0x3e: ea = am_absx(); st(ea, op_rol(ld(ea, wa), wa), wa); break;
    case 0x66: ea = am_dp();   st(ea, op_ror(ld(ea, wa), wa), wa); break;
    case 0x76: ea = am_dpx();  st(ea, op_ror(ld(ea, wa), wa), wa); break;
    case 0x6e: ea = am_abs();  st(ea, op_ror(ld(ea, wa), wa), wa); break;
    case 0x7e: ea = am_absx(); st(ea, op_ror(ld(ea, wa), wa), wa); break;
    /* --- INC/DEC --- */
    case 0x1a: set_a(op_inc(P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a, wa)); break;
    case 0x3a: set_a(op_dec(P8() ? (uint16_t)(m_cpu.a & 0xff) : m_cpu.a, wa)); break;
    case 0xe6: ea = am_dp();   st(ea, op_inc(ld(ea, wa), wa), wa); break;
    case 0xf6: ea = am_dpx();  st(ea, op_inc(ld(ea, wa), wa), wa); break;
    case 0xee: ea = am_abs();  st(ea, op_inc(ld(ea, wa), wa), wa); break;
    case 0xfe: ea = am_absx(); st(ea, op_inc(ld(ea, wa), wa), wa); break;
    case 0xc6: ea = am_dp();   st(ea, op_dec(ld(ea, wa), wa), wa); break;
    case 0xd6: ea = am_dpx();  st(ea, op_dec(ld(ea, wa), wa), wa); break;
    case 0xce: ea = am_abs();  st(ea, op_dec(ld(ea, wa), wa), wa); break;
    case 0xde: ea = am_absx(); st(ea, op_dec(ld(ea, wa), wa), wa); break;
    case 0xe8: set_x((uint16_t)(m_cpu.x + 1)); break;
    case 0xc8: set_y((uint16_t)(m_cpu.y + 1)); break;
    case 0xca: set_x((uint16_t)(m_cpu.x - 1)); break;
    case 0x88: set_y((uint16_t)(m_cpu.y - 1)); break;
    /* --- transfers --- */
    case 0xaa: set_x(m_cpu.a); break;
    case 0xa8: set_y(m_cpu.a); break;
    case 0x8a: set_a(m_cpu.x); break;
    case 0x98: set_a(m_cpu.y); break;
    case 0x9b: set_y(m_cpu.x); break;
    case 0xbb: set_x(m_cpu.y); break;
    case 0xba: set_x(m_cpu.s); break;
    case 0x9a: m_cpu.s = m_cpu.x; break;
    case 0x5b: m_cpu.d = m_cpu.a; setnz16(m_cpu.d); break;
    case 0x7b: m_cpu.a = m_cpu.d; setnz16(m_cpu.a); break;
    case 0x1b: m_cpu.s = m_cpu.a; break;
    case 0x3b: m_cpu.a = m_cpu.s; setnz16(m_cpu.a); break;
    case 0xeb: m_cpu.a = (uint16_t)((m_cpu.a >> 8) | (m_cpu.a << 8)); setnz8((uint8_t)m_cpu.a); break;
    /* --- flags --- */
    case 0x18: m_cpu.p &= (uint8_t)~M_C; break;
    case 0x38: m_cpu.p |= M_C; break;
    case 0x58: m_cpu.p &= (uint8_t)~M_I; break;
    case 0x78: m_cpu.p |= M_I; break;
    case 0xd8: m_cpu.p &= (uint8_t)~M_D; break;
    case 0xf8: m_cpu.p |= M_D; break;
    case 0xb8: m_cpu.p &= (uint8_t)~M_V; break;
    case 0xc2: m_cpu.p &= (uint8_t)~fetch8(); break;
    case 0xe2: m_cpu.p |= fetch8(); p_narrow_index(); break;
    case 0xfb: { int c = (m_cpu.p & M_C) ? 1 : 0; m_cpu.p &= (uint8_t)~M_C; if(m_cpu.e) m_cpu.p |= M_C;
                 m_cpu.e = c; if(m_cpu.e) M_DIE("xce -> emulation mode not modelled\n"); } break;
    /* --- stack --- */
    case 0x48: if(wa) push16(m_cpu.a); else push8((uint8_t)m_cpu.a); break;
    case 0x68: set_a(wa ? pull16() : pull8()); break;
    case 0xda: if(wi) push16(m_cpu.x); else push8((uint8_t)m_cpu.x); break;
    case 0xfa: set_x(wi ? pull16() : pull8()); break;
    case 0x5a: if(wi) push16(m_cpu.y); else push8((uint8_t)m_cpu.y); break;
    case 0x7a: set_y(wi ? pull16() : pull8()); break;
    case 0x08: push8(m_cpu.p); break;
    case 0x28: m_cpu.p = pull8(); p_narrow_index(); break;
    case 0x8b: push8(m_cpu.dbr); break;
    case 0xab: m_cpu.dbr = pull8(); setnz8(m_cpu.dbr); break;
    case 0x0b: push16(m_cpu.d); break;
    case 0x2b: m_cpu.d = pull16(); setnz16(m_cpu.d); break;
    case 0x4b: push8(m_cpu.pbr); break;
    case 0xf4: push16(fetch16()); break;
    case 0xd4: push16(rd16(am_dp())); break;
    case 0x62: { uint16_t r = fetch16(); push16((uint16_t)(m_cpu.pc + r)); } break;
    /* --- jumps and branches --- */
    case 0x4c: m_cpu.pc = fetch16(); break;
    case 0x5c: { uint32_t t = fetch24(); m_cpu.pbr = (uint8_t)(t >> 16); m_cpu.pc = (uint16_t)t; } break;
    case 0x6c: { uint16_t a = fetch16(); m_cpu.pc = (uint16_t)(bus_read(a) | (bus_read((uint16_t)(a + 1)) << 8)); } break;
    case 0x7c: { uint16_t a = (uint16_t)(fetch16() + m_cpu.x); uint32_t b = (uint32_t)m_cpu.pbr << 16;
                 m_cpu.pc = (uint16_t)(bus_read(b | a) | (bus_read(b | (uint16_t)(a + 1)) << 8)); } break;
    case 0xdc: { uint16_t a = fetch16(); m_cpu.pc = (uint16_t)(bus_read(a) | (bus_read((uint16_t)(a + 1)) << 8));
                 m_cpu.pbr = bus_read((uint16_t)(a + 2)); } break;
    case 0x20: { uint16_t t = fetch16(); push16((uint16_t)(m_cpu.pc - 1)); m_cpu.pc = t; } break;
    case 0xfc: { uint16_t a = (uint16_t)(fetch16() + m_cpu.x); uint32_t b = (uint32_t)m_cpu.pbr << 16;
                 push16((uint16_t)(m_cpu.pc - 1));
                 m_cpu.pc = (uint16_t)(bus_read(b | a) | (bus_read(b | (uint16_t)(a + 1)) << 8)); } break;
    case 0x22: { uint32_t t = fetch24(); push8(m_cpu.pbr); push16((uint16_t)(m_cpu.pc - 1));
                 m_cpu.pbr = (uint8_t)(t >> 16); m_cpu.pc = (uint16_t)t; } break;
    case 0x60: m_cpu.pc = (uint16_t)(pull16() + 1); break;
    case 0x6b: m_cpu.pc = (uint16_t)(pull16() + 1); m_cpu.pbr = pull8(); break;
    case 0x40: m_cpu.p = pull8(); p_narrow_index(); m_cpu.pc = pull16(); m_cpu.pbr = pull8();
      if(clk_on && clk_depth > 0) {
        clk_depth--;
        if(int_fn) int_fn(2, clk_line, (int)(clk_mc - clk_line_start));
      }
      break;
    case 0x80: branch(1); break;
    case 0x82: { uint16_t r = fetch16(); m_cpu.pc = (uint16_t)(m_cpu.pc + r); } break;
    case 0x10: branch(!(m_cpu.p & M_N)); break;
    case 0x30: branch( (m_cpu.p & M_N)); break;
    case 0x50: branch(!(m_cpu.p & M_V)); break;
    case 0x70: branch( (m_cpu.p & M_V)); break;
    case 0x90: branch(!(m_cpu.p & M_C)); break;
    case 0xb0: branch( (m_cpu.p & M_C)); break;
    case 0xd0: branch(!(m_cpu.p & M_Z)); break;
    case 0xf0: branch( (m_cpu.p & M_Z)); break;
    /* --- block move --- */
    case 0x54: case 0x44: {
      uint8_t dst = fetch8(), src = fetch8();
      int dir = (op == 0x54) ? 1 : -1;
      if(I8()) M_DIE("MVN/MVP with 8-bit indexes not modelled\n");
      for(;;) {
        bus_write(((uint32_t)dst << 16) | m_cpu.y, bus_read(((uint32_t)src << 16) | m_cpu.x));
        clk_io += 2;
        if(clk_on) clk_pend += 3 * 8;         /* the opcode is refetched per byte */
        m_cpu.x = (uint16_t)(m_cpu.x + dir);
        m_cpu.y = (uint16_t)(m_cpu.y + dir);
        if(m_cpu.a == 0) { m_cpu.a = 0xffff; break; }
        m_cpu.a = (uint16_t)(m_cpu.a - 1);
      }
      m_cpu.dbr = dst;
    } break;
    case 0xea: break;                                   /* NOP */
    case 0xcb:                                          /* WAI */
      if(!clk_on) M_DIE("WAI sem relogio: nada acordaria a CPU (m_clock_start)\n");
      clk_waiting = 1; break;
    case 0x42: (void)fetch8(); break;                   /* WDM */
    default: M_DIE("opcode not implemented $%02X\n", op);
  }
}

/* Take an interrupt: native-mode push of PBR, PC, P; I set, D clear, bank 0,
 * PC from the vector.  kind 0 = NMI, 1 = IRQ. */
static void take_int(uint16_t vec, int kind) {
  push8(m_cpu.pbr);
  push16(m_cpu.pc);
  push8(m_cpu.p);
  m_cpu.p = (uint8_t)((m_cpu.p | M_I) & ~M_D);
  m_cpu.pbr = 0;
  m_cpu.pc = (uint16_t)(bus_read(vec) | (bus_read((uint16_t)(vec + 1)) << 8));
  clk_io += 2;
  clk_depth++;
  if(int_fn) int_fn(kind, clk_line, (int)(clk_mc - clk_line_start));
}

/* One instruction, or -- with the clock on -- one interrupt entry or one line
 * of WAI.  Without the clock this is exactly exec_one(), as it always was. */
static void step(void) {
  if(!clk_on) { exec_one(); return; }
  clk_pend = 0; clk_io = 0;
  if(nmi_pend) {
    nmi_pend = 0; clk_waiting = 0;
    take_int(0xffea, 0);
  } else if(irq_timeup && (cpu_reg[0x00] & 0x20)) {
    if(!(m_cpu.p & M_I)) { clk_waiting = 0; take_int(0xffee, 1); }
    else clk_waiting = 0;                 /* WAI wakes on a masked IRQ too */
  }
  if(clk_pend || clk_io) {                /* an entry was taken: charge it */
    clk_advance(cpu_cost(clk_pend + 6u * (uint64_t)clk_io));
    return;
  }
  if(clk_waiting) {                       /* sleep to the next line start */
    clk_advance(clk_line_start + 1364 - clk_mc);
    return;
  }
  exec_one();
  clk_advance(cpu_cost(clk_pend + 6u * (uint64_t)clk_io));
}

/* --- clock API (m65816.h) --- */
void m_clock_start(int line, int lines) {
  if(lines != 262 && lines != 312) M_DIE("m_clock_start: %d linhas\n", lines);
  if(line < 0 || line >= lines) M_DIE("m_clock_start: linha %d\n", line);
  clk_on = 1; clk_lines = lines; clk_line = line;
  clk_mc = 0; clk_line_start = 0; clk_pend = 0; clk_depth0 = 0; clk_depth = 0;
  clk_waiting = 0; nmi_pend = 0; rdnmi_flag = 0; irq_timeup = 0;
  { const char *e = getenv("M65816_CPU_SCALE");
    double f = e ? atof(e) : 1.0;
    if(f < 0.5 || f > 4.0) f = 1.0;
    clk_scale_q = (uint32_t)(f * 1024.0 + 0.5); clk_scale_rem = 0; }
}
void m_clock_stop(void) { clk_on = 0; }
uint64_t m_clock_mc(void) { return clk_mc; }
int m_clock_line(void) { return clk_line; }
int m_clock_h(void) { return (int)(clk_mc - clk_line_start); }
uint64_t m_clock_line_start(void) { return clk_line_start; }
uint64_t m_mainloop_mc(void) { return clk_depth0; }
int m_int_depth(void) { return clk_depth; }
void m_set_line_hook(void (*fn)(int v)) { line_fn = fn; }
void m_clock_stall(uint64_t mc) { clk_stall_pend += mc; }
void m_set_int_hook(void (*fn)(int kind, int v, int h)) { int_fn = fn; }
void m_set_write_watch(void (*fn)(uint32_t a24)) { wwatch_fn = fn; }
void m_set_pc_hook(void (*fn)(uint32_t pc24)) { pc_fn = fn; }
uint64_t m_dma_start_mc(void) { return dma_start_mc; }
uint64_t m_dma_end_mc(void) { return dma_end_mc; }
void m_set_cpurev(int rev) { cpu_rev = rev & 0x0f; }
void m_set_pal(int pal) { cpu_pal = pal ? 1 : 0; }
void m_run_clocked(uint64_t until) {
  uint64_t start = m_cpu.instrs;
  if(!clk_on) M_DIE("m_run_clocked sem m_clock_start\n");
  while(clk_mc < until) {
    step();
    if(m_cpu.instrs - start > m_instr_budget)
      M_DIE("orcamento de %llu instrucoes estourado no modo com relogio\n",
            (unsigned long long)m_instr_budget);
  }
}

/* ============================================================
 * m_selftest -- micro-tests of the interpreter itself, which is the premise
 * of the whole gate: an opcode understood wrong makes the renderer run on
 * hardware that does not exist.  Each case assembles a small program in WRAM
 * (bank $00) and EXECUTES it -- nothing is asserted about code that did not
 * run.  Cases 1-3 pin the index truncation the renderer does not use yet.
 * ============================================================ */
static int st_fail;
static uint32_t st_pc;

static void e8(uint8_t b)   { m_poke(st_pc++, b); }
static void e16(uint16_t w) { e8((uint8_t)w); e8((uint8_t)(w >> 8)); }
static void st_begin(void)  { m_reset_memory(); st_pc = 0x000400; }

/* Witness for the $EF write-window test below. */
static unsigned ef_st_n, ef_st_last_off, ef_st_last_v;
static void ef_st_hook(uint32_t off, uint8_t v) {
  ef_st_n++; ef_st_last_off = off; ef_st_last_v = v;
}
static void st_run(void)    { m_call(0x000400, 0); }

#define ST_CHECK(cond, ...) do { if(!(cond)) { \
    fprintf(stderr, "m65816 selftest FAIL: "); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); st_fail++; } } while(0)

int m_selftest(void) {
  st_fail = 0;

  /* 1) sep #$10 TRUNCATES X and Y, it does not just narrow the operation. */
  st_begin();
  e8(0xc2); e8(0x30);                 /* rep #$30      */
  e8(0xa2); e16(0x1234);              /* ldx #$1234    */
  e8(0xa0); e16(0x5678);              /* ldy #$5678    */
  e8(0xe2); e8(0x10);                 /* sep #$10      */
  e8(0x60);                           /* rts           */
  st_run();
  ST_CHECK(m_cpu.x == 0x0034, "sep #$10: X=$%04X, want $0034", m_cpu.x);
  ST_CHECK(m_cpu.y == 0x0078, "sep #$10: Y=$%04X, want $0078", m_cpu.y);

  /* 2) a PLP that RAISES the X bit truncates the same way. */
  st_begin();
  e8(0xc2); e8(0x30);                 /* rep #$30      */
  e8(0xa2); e16(0x1234);              /* ldx #$1234    */
  e8(0xa0); e16(0x5678);              /* ldy #$5678    */
  e8(0xe2); e8(0x20);                 /* sep #$20      */
  e8(0xa9); e8(0x30);                 /* lda #$30 (M|X)*/
  e8(0x48);                           /* pha           */
  e8(0x28);                           /* plp           */
  e8(0x60);                           /* rts           */
  st_run();
  ST_CHECK(m_cpu.x == 0x0034, "plp: X=$%04X, want $0034", m_cpu.x);
  ST_CHECK(m_cpu.y == 0x0078, "plp: Y=$%04X, want $0078", m_cpu.y);

  /* 3) RTI likewise, and it also pins the pull order (P, PC, PBR). */
  st_begin();
  m_poke(0x000500, 0x60);             /* RTI target: rts */
  e8(0xc2); e8(0x30);                 /* rep #$30      */
  e8(0xa2); e16(0x1234);              /* ldx #$1234    */
  e8(0xa0); e16(0x5678);              /* ldy #$5678    */
  e8(0xe2); e8(0x20);                 /* sep #$20      */
  e8(0xa9); e8(0x00); e8(0x48);       /* lda #$00 : pha   (PBR) */
  e8(0xc2); e8(0x20);                 /* rep #$20      */
  e8(0xa9); e16(0x0500); e8(0x48);    /* lda #$0500 : pha (PC)  */
  e8(0xe2); e8(0x20);                 /* sep #$20      */
  e8(0xa9); e8(0x30); e8(0x48);       /* lda #$30 : pha   (P)   */
  e8(0x40);                           /* rti           */
  st_run();
  ST_CHECK(m_cpu.x == 0x0034, "rti: X=$%04X, want $0034", m_cpu.x);
  ST_CHECK(m_cpu.y == 0x0078, "rti: Y=$%04X, want $0078", m_cpu.y);

  /* 4) MVN moves A+1 bytes, advances X/Y and leaves DBR = DESTINATION bank.
   *    Every $41 payload reaches the shadow through it. */
  st_begin();
  { static const uint8_t pat[5] = { 0xde, 0xad, 0xbe, 0xef, 0x42 };
    int i;
    for(i = 0; i < 5; i++) m_poke(0x7e1000 + i, pat[i]);
    e8(0xc2); e8(0x30);               /* rep #$30      */
    e8(0xa2); e16(0x1000);            /* ldx #$1000    */
    e8(0xa0); e16(0x2000);            /* ldy #$2000    */
    e8(0xa9); e16(0x0004);            /* lda #4  (= 5 bytes) */
    e8(0x54); e8(0x7f); e8(0x7e);     /* mvn $7f,$7e   */
    e8(0xe2); e8(0x20);               /* sep #$20      */
    e8(0x60);                         /* rts           */
    st_run();
    for(i = 0; i < 5; i++)
      ST_CHECK(m_peek(0x7f2000 + i) == pat[i], "mvn: dst[%d]=$%02X, want $%02X",
               i, m_peek(0x7f2000 + i), pat[i]);
    ST_CHECK(m_peek(0x7f2005) == 0x55, "mvn: wrote one byte too many");
    ST_CHECK(m_cpu.dbr == 0x7f, "mvn: DBR=$%02X, want $7F", m_cpu.dbr);
    ST_CHECK(m_cpu.x == 0x1005 && m_cpu.y == 0x2005, "mvn: X=$%04X Y=$%04X", m_cpu.x, m_cpu.y);
  }

  /* 5) VMAIN $80 + mode-1 DMA ($2118/$2119): the byte pair becomes one VRAM
   *    word and the address only advances AFTER the $2119 write, exactly what
   *    nes_chrq_dma_init/nes_chrq_dma_dsc program. */
  st_begin();
  { static const uint8_t src[4] = { 0x11, 0x22, 0x33, 0x44 };
    int i;
    for(i = 0; i < 4; i++) m_poke(0x7e1100 + i, src[i]);
    e8(0xe2); e8(0x20);                          /* sep #$20            */
    e8(0xa9); e8(0x80); e8(0x8d); e16(0x2115);   /* lda #$80 : sta $2115 */
    e8(0xc2); e8(0x20);                          /* rep #$20            */
    e8(0xa9); e16(0x0100); e8(0x8d); e16(0x2116);/* lda #$0100 : sta $2116 */
    e8(0xe2); e8(0x20);                          /* sep #$20            */
    e8(0xa9); e8(0x01); e8(0x8d); e16(0x4300);   /* mode 1              */
    e8(0xa9); e8(0x18); e8(0x8d); e16(0x4301);   /* B-bus $2118/$2119   */
    e8(0xa9); e8(0x7e); e8(0x8d); e16(0x4304);   /* source bank         */
    e8(0xc2); e8(0x20);                          /* rep #$20            */
    e8(0xa9); e16(0x1100); e8(0x8d); e16(0x4302);/* source address      */
    e8(0xa9); e16(0x0004); e8(0x8d); e16(0x4305);/* 4 bytes             */
    e8(0xe2); e8(0x20);                          /* sep #$20            */
    e8(0xa9); e8(0x01); e8(0x8d); e16(0x420b);   /* trigger             */
    e8(0x60);                                    /* rts                 */
    st_run();
    for(i = 0; i < 4; i++)
      ST_CHECK(m_vram[0x200 + i] == src[i], "dma mode 1: vram[$%03X]=$%02X, want $%02X",
               0x200 + i, m_vram[0x200 + i], src[i]);
    ST_CHECK(m_vram[0x204] == 0xa5, "dma mode 1: wrote past the length");
  }

  /* 6) m_call through JSL: 3-byte return, RTL, trap does not fire early. */
  st_begin();
  m_poke(0x000600, 0xa9); m_poke(0x000601, 0x5a);   /* lda #$5a */
  m_poke(0x000602, 0x6b);                            /* rtl      */
  m_call(0x000600, 1);
  ST_CHECK((m_cpu.a & 0xff) == 0x5a, "jsl/rtl: A=$%04X", m_cpu.a);

  /* ---------------- GBC extensions (m_gbc_mode) ---------------- */

  /* 7) banks $E0-$E3 are READ-ONLY views served from the caller's buffer, and
   *    the bank selects which 64 KB slice.  A run whose view window was never
   *    installed must not silently read WRAM. */
  st_begin();
  { static uint8_t win[5 * 0x10000];
    memset(win, 0, sizeof(win));
    win[0x40000 + 0x8765] = 0x44;      /* $E4:8765 (wire $03: the FB view) */
    win[0x00000 + 0x0123] = 0x11;      /* $E0:0123 */
    win[0x14000]          = 0x22;      /* $E1:4000 */
    win[0x30000 + 0xffff] = 0x33;      /* $E3:FFFF */
    m_gbc_mode(1);
    m_set_view_window(win);
    e8(0xe2); e8(0x20);                             /* sep #$20             */
    e8(0xaf); e8(0x23); e8(0x01); e8(0xe0);         /* lda $E00123          */
    e8(0x8f); e8(0x00); e8(0x30); e8(0x7e);         /* sta $7E3000          */
    e8(0xaf); e8(0x00); e8(0x40); e8(0xe1);         /* lda $E14000          */
    e8(0x8f); e8(0x01); e8(0x30); e8(0x7e);         /* sta $7E3001          */
    e8(0xaf); e8(0xff); e8(0xff); e8(0xe3);         /* lda $E3FFFF          */
    e8(0x8f); e8(0x02); e8(0x30); e8(0x7e);         /* sta $7E3002          */
    e8(0xaf); e8(0x65); e8(0x87); e8(0xe4);         /* lda $E48765          */
    e8(0x8f); e8(0x03); e8(0x30); e8(0x7e);         /* sta $7E3003          */
    e8(0x60);                                       /* rts                  */
    st_run();
    ST_CHECK(m_peek(0x7e3003) == 0x44, "view $E4:8765 = $%02X, want $44", m_peek(0x7e3003));
    ST_CHECK(m_peek(0x7e3000) == 0x11, "view $E0:0123 = $%02X, want $11", m_peek(0x7e3000));
    ST_CHECK(m_peek(0x7e3001) == 0x22, "view $E1:4000 = $%02X, want $22", m_peek(0x7e3001));
    ST_CHECK(m_peek(0x7e3002) == 0x33, "view $E3:FFFF = $%02X, want $33", m_peek(0x7e3002));
  }

  /* 8) the WRAM port ($2181-$2183 + $2180), reached BY DMA with BBAD = $80 --
   *    which is how the player fetches its status block.  The address the
   *    port counts up is its own, not the DMA's, so a model that only wrote
   *    the first byte would still look right at offset 0. */
  st_begin();
  { static const uint8_t src[4] = { 0xa1, 0xb2, 0xc3, 0xd4 };
    int i;
    m_gbc_mode(1);
    for(i = 0; i < 4; i++) m_poke(0x7f1234 + i, src[i]);
    e8(0xe2); e8(0x20);                          /* sep #$20             */
    e8(0xa9); e8(0x00); e8(0x8d); e16(0x2181);   /* WMADD = $01:0F00     */
    e8(0xa9); e8(0x0f); e8(0x8d); e16(0x2182);
    e8(0xa9); e8(0x01); e8(0x8d); e16(0x2183);
    e8(0xa9); e8(0x00); e8(0x8d); e16(0x4360);   /* ch6 mode 0, A->B, inc */
    e8(0xa9); e8(0x80); e8(0x8d); e16(0x4361);   /* BBAD = $2180          */
    e8(0xa9); e8(0x7f); e8(0x8d); e16(0x4364);   /* source bank $7F       */
    e8(0xc2); e8(0x20);                          /* rep #$20             */
    e8(0xa9); e16(0x1234); e8(0x8d); e16(0x4362);
    e8(0xa9); e16(0x0004); e8(0x8d); e16(0x4365);
    e8(0xe2); e8(0x20);                          /* sep #$20             */
    e8(0xa9); e8(0x40); e8(0x8d); e16(0x420b);   /* fire ch6             */
    e8(0x60);
    st_run();
    for(i = 0; i < 4; i++)
      ST_CHECK(m_peek(0x7f0f00 + i) == src[i], "wram port: $7F:0F0%d = $%02X, want $%02X",
               i, m_peek(0x7f0f00 + i), src[i]);
    ST_CHECK(m_peek(0x7f0f04) == 0x55, "wram port: escreveu um byte a mais");
  }

  /* 9) m_set_v drives the 9-bit V the transfer guard is a function of.  The
   *    second $213D read is bit 8 alone, and $213F resets the flip-flop --
   *    an 8-bit-only model would fold V=300 onto V=44 and hand a mid-screen
   *    transfer a full window's worth of capacity. */
  st_begin();
  { m_gbc_mode(1);
    m_set_v(300);
    e8(0xe2); e8(0x20);                          /* sep #$20             */
    e8(0xad); e16(0x213f);                       /* lda $213F (reset ff) */
    e8(0xad); e16(0x2137);                       /* lda $2137 (latch)    */
    e8(0xad); e16(0x213d); e8(0x8f); e8(0x10); e8(0x30); e8(0x7e);  /* V[7:0] */
    e8(0xad); e16(0x213d); e8(0x8f); e8(0x11); e8(0x30); e8(0x7e);  /* V[8]   */
    e8(0x60);
    st_run();
    ST_CHECK(m_peek(0x7e3010) == (300 & 0xff), "V=300 low = $%02X, want $%02X",
             m_peek(0x7e3010), 300 & 0xff);
    ST_CHECK((m_peek(0x7e3011) & 1) == 1, "V=300 bit 8 = %d, want 1", m_peek(0x7e3011) & 1);
  }
  st_begin();
  { m_gbc_mode(1);
    m_set_v(100);
    e8(0xe2); e8(0x20);
    e8(0xad); e16(0x213f);
    e8(0xad); e16(0x2137);
    e8(0xad); e16(0x213d); e8(0x8f); e8(0x10); e8(0x30); e8(0x7e);
    e8(0xad); e16(0x213d); e8(0x8f); e8(0x11); e8(0x30); e8(0x7e);
    e8(0x60);
    st_run();
    ST_CHECK(m_peek(0x7e3010) == 100, "V=100 low = $%02X, want $64", m_peek(0x7e3010));
    ST_CHECK((m_peek(0x7e3011) & 1) == 0, "V=100 bit 8 = %d, want 0", m_peek(0x7e3011) & 1);
  }

  /* 10) the 8x8 multiplier: GbcCapacity turns lines into byte-equivalents
   *     with it, so a stub returning zero would make every scenario look
   *     like the V-guard one. */
  st_begin();
  { m_gbc_mode(1);
    e8(0xe2); e8(0x20);
    e8(0xa9); e8(41);  e8(0x8d); e16(0x4202);
    e8(0xa9); e8(163); e8(0x8d); e16(0x4203);
    e8(0xea); e8(0xea); e8(0xea); e8(0xea);
    e8(0xc2); e8(0x20);
    e8(0xad); e16(0x4216); e8(0x8f); e8(0x20); e8(0x30); e8(0x7e);
    e8(0xe2); e8(0x20);
    e8(0x60);
    st_run();
    ST_CHECK(m_peek16(0x7e3020) == 41 * 163, "mult 41*163 = %u, want %u",
             m_peek16(0x7e3020), 41 * 163);
  }

  /* 11) the $EF write window reports every store, and in the harness build
   *     (window inside WRAM) the store LANDS as well -- the strobes have to
   *     stay observable in memory for a USB read on silicon to work the same
   *     way. */
  st_begin();
  { m_gbc_mode(1);
    m_set_ef_sink(0x7e1000, 6, ef_st_hook);
    ef_st_n = 0; ef_st_last_off = 0xffff; ef_st_last_v = 0;
    e8(0xe2); e8(0x20);
    e8(0xa9); e8(0x5a); e8(0x8f); e8(0x02); e8(0x10); e8(0x7e);  /* sta $7E1002 */
    e8(0xa9); e8(0xa5); e8(0x8f); e8(0x05); e8(0x10); e8(0x7e);  /* sta $7E1005 */
    e8(0x60);
    st_run();
    ST_CHECK(ef_st_n == 2, "$EF sink: %u escritas, want 2", ef_st_n);
    ST_CHECK(ef_st_last_off == 5 && ef_st_last_v == 0xa5,
             "$EF sink: ultima = off %u val $%02X", ef_st_last_off, ef_st_last_v);
    ST_CHECK(m_peek(0x7e1002) == 0x5a, "$EF harness: o store nao pousou na WRAM");
  }

  /* 12) THE CLOCK: a V-IRQ at VTIME acknowledged through $4211, the NMI at
   *     V=225 through $4210, both waking a WAI loop, and the V latch read in
   *     each handler.  Three frames from V=100; each handler counts itself and
   *     stores the V it saw.  A handler that did NOT read $4211 would be
   *     re-entered on every instruction boundary -- the count is what says the
   *     acknowledge is modelled. */
  { static uint8_t vrom[0x8000];
    memset(vrom, 0, sizeof(vrom));
    vrom[0x7fea] = 0x20; vrom[0x7feb] = 0x05;      /* NMI -> $00:0520 */
    vrom[0x7fee] = 0x00; vrom[0x7fef] = 0x05;      /* IRQ -> $00:0500 */
    st_begin();
    m_load_rom(vrom, sizeof(vrom));
    m_gbc_mode(1);
    e8(0xe2); e8(0x20);                            /* sep #$20            */
    e8(0xa9); e8(185); e8(0x8d); e16(0x4209);      /* VTIME = 185         */
    e8(0x9c); e16(0x420a);                         /* stz $420A           */
    e8(0xad); e16(0x4211);                         /* lda $4211           */
    e8(0xa9); e8(0xa0); e8(0x8d); e16(0x4200);     /* NMI + V-IRQ         */
    e8(0x58);                                      /* cli                 */
    e8(0xcb);                                      /* loop: wai           */
    e8(0x80); e8(0xfd);                            /*       bra loop      */
    st_pc = 0x000500;                              /* IRQ                 */
    e8(0xad); e16(0x4211);                         /* lda $4211 (ack)     */
    e8(0xad); e16(0x213f);                         /* reset the ff        */
    e8(0xad); e16(0x2137);                         /* latch               */
    e8(0xad); e16(0x213d); e8(0x8f); e8(0x41); e8(0x30); e8(0x7e);
    e8(0xaf); e8(0x40); e8(0x30); e8(0x7e); e8(0x1a); e8(0x8f); e8(0x40); e8(0x30); e8(0x7e);
    e8(0x40);                                      /* rti                 */
    st_pc = 0x000520;                              /* NMI                 */
    e8(0xad); e16(0x4210);
    e8(0xad); e16(0x213f);
    e8(0xad); e16(0x2137);
    e8(0xad); e16(0x213d); e8(0x8f); e8(0x43); e8(0x30); e8(0x7e);
    e8(0xaf); e8(0x42); e8(0x30); e8(0x7e); e8(0x1a); e8(0x8f); e8(0x42); e8(0x30); e8(0x7e);
    e8(0x40);
    m_poke(0x7e3040, 0); m_poke(0x7e3042, 0);
    m_cpu.pbr = 0; m_cpu.pc = 0x0400;
    m_clock_start(100, 262);
    m_run_clocked(3ull * 262 * 1364);
    ST_CHECK(m_peek(0x7e3040) == 3, "clock: %u V-IRQs em 3 frames, want 3", m_peek(0x7e3040));
    ST_CHECK(m_peek(0x7e3042) == 3, "clock: %u NMIs em 3 frames, want 3", m_peek(0x7e3042));
    ST_CHECK(m_peek(0x7e3041) == 185, "clock: IRQ viu V=%u, want 185", m_peek(0x7e3041));
    ST_CHECK(m_peek(0x7e3043) == 225, "clock: NMI viu V=%u, want 225", m_peek(0x7e3043));
    ST_CHECK(m_int_depth() == 0, "clock: profundidade %d depois dos RTIs", m_int_depth());
    m_clock_stop();
  }

  m_gbc_mode(0);
  m_set_view_window(NULL);
  m_set_ef_sink(0, 0, NULL);

  return st_fail;
}

void m_run_to(uint32_t from, uint32_t stop) {
  uint64_t start = m_cpu.instrs;
  m_cpu.pbr = (uint8_t)(from >> 16);
  m_cpu.pc  = (uint16_t)from;
  for(;;) {
    if(((uint32_t)m_cpu.pbr << 16 | m_cpu.pc) == stop) return;
    step();
    if(m_cpu.instrs - start > m_instr_budget)
      M_DIE("orcamento de %llu instrucoes estourado indo de $%06X a $%06X\n",
            (unsigned long long)m_instr_budget, from, stop);
  }
}

void m_call(uint32_t addr24, int is_long) {
  uint16_t s0 = m_cpu.s;
  const uint16_t trap = 0xfff0;
  uint64_t start = m_cpu.instrs;
  if(is_long) push8(0x00);
  push16((uint16_t)(trap - 1));
  m_cpu.pbr = (uint8_t)(addr24 >> 16);
  m_cpu.pc  = (uint16_t)addr24;
  for(;;) {
    step();
    if(m_cpu.pc == trap && m_cpu.s == s0) return;
    if(m_cpu.instrs - start > m_instr_budget)
      M_DIE("orcamento de %llu instrucoes estourado em $%06X\n",
            (unsigned long long)m_instr_budget, addr24);
  }
}

/* --- a call that can be SUSPENDED ----------------------------------------
 * Same contract as m_call, except that it gives up after `max` instructions
 * and leaves the CPU exactly where it stopped, so the caller can run OTHER
 * code (an NMI body) on the same stack and then resume.  There is no
 * interrupt model here and there does not need to be one: on silicon an NMI
 * is precisely "push the registers, run a routine at this stack depth, pull
 * them back", which is what a driver does around m_step_resume.
 *
 * Only one suspended call at a time.  A nested m_call (the injected body) is
 * free to run meanwhile: it picks its own s0, and the trap address is only
 * recognised at ITS stack depth.
 */
static int      step_live;
static uint16_t step_s0;
static uint32_t step_addr;

static int step_run(uint64_t max) {
  const uint16_t trap = 0xfff0;
  uint64_t start = m_cpu.instrs;
  for(;;) {
    step();
    if(m_cpu.pc == trap && m_cpu.s == step_s0) { step_live = 0; return 0; }
    if(m_cpu.instrs - start >= max) return 1;
    if(m_cpu.instrs - start > m_instr_budget)
      M_DIE("orcamento de %llu instrucoes estourado em $%06X\n",
            (unsigned long long)m_instr_budget, step_addr);
  }
}

int m_call_stepped(uint32_t addr24, int is_long, uint64_t max) {
  if(step_live) M_DIE("m_call_stepped com uma chamada ja' suspensa em $%06X\n",
                      step_addr);
  step_s0 = m_cpu.s;
  step_addr = addr24;
  step_live = 1;
  if(is_long) push8(0x00);
  push16((uint16_t)(0xfff0 - 1));
  m_cpu.pbr = (uint8_t)(addr24 >> 16);
  m_cpu.pc  = (uint16_t)addr24;
  return step_run(max);
}

int m_step_resume(uint64_t max) {
  if(!step_live) M_DIE("m_step_resume sem chamada suspensa\n");
  return step_run(max);
}

uint16_t m_step_sp(void) { return step_s0; }
