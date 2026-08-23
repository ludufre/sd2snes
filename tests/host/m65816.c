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
static uint8_t  ppu_reg[0x100];    /* mute mirror of unmodelled $21xx writes */
static uint8_t  cpu_reg[0x100];    /* $42xx */
static uint8_t  dma_reg[0x80];     /* $43xx */

/* VRAM/CGRAM/OAM port state */
static uint8_t  vmain;
static uint16_t vmadd;
static uint16_t cgadd;
static uint16_t oam_ba;
static uint8_t  oam_latch;
static int      hv_hi;             /* hi/lo flip-flop of $213C/$213D */

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
  memset(cpu_reg, 0, sizeof(cpu_reg));
  memset(dma_reg, 0, sizeof(dma_reg));
  vmain = 0; vmadd = 0; cgadd = 0; oam_ba = 0; oam_latch = 0; hv_hi = 0;
  m_forced_blank = 0;
  memset(&m_cpu, 0, sizeof(m_cpu));
  m_cpu.s = 0x1fff;
  m_cpu.p = M_M | M_I;   /* native, A 8-bit, X/Y 16-bit (rep #$10 at RESET) */
  m_cpu.e = 0;
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
  int mode = ctl & 7, step, i;
  if(cnt == 0) cnt = 0x10000;
  if(ctl & 0x80) M_DIE("DMA channel %d B->A not modelled (ctl=$%02X)\n", ch, ctl);
  step = (ctl & 0x08) ? 0 : ((ctl & 0x10) ? -1 : 1);
  for(i = 0; (uint32_t)i < cnt; i++) {
    uint8_t v = bus_read(((uint32_t)a1b << 16) | a1);
    reg_write((uint16_t)(0x2100 + ((bbad + pat[mode][i % patlen[mode]]) & 0xff)), v);
    a1 = (uint16_t)(a1 + step);
  }
  r[2] = (uint8_t)a1; r[3] = (uint8_t)(a1 >> 8);
  r[5] = 0; r[6] = 0;
}

/* ---------------- registers ---------------- */
static uint16_t vmain_step(void) {
  switch(vmain & 0x03) { case 0: return 1; case 1: return 32; default: return 128; }
}

static void reg_write(uint16_t off, uint8_t v) {
  if(off >= 0x2100 && off <= 0x21ff) {
    switch(off) {
      case 0x2100: m_forced_blank = (v & 0x80) ? 1 : 0; break;
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
      default:
        /* MUTE BY WHITELIST, never by 'default'.  These only shape what the
           SCREEN shows -- none of them moves a byte of VRAM/CGRAM/OAM:
             $2101       OBSEL
             $2105-$2114 BGMODE/MOSAIC/BGxSC/BGxNBA + the 8 scrolls
             $2123-$2133 windows, TM/TS/TMW/TSW, color math, SETINI
           Everything else ABORTS: APU ports ($2140-$2143), Mode 7
           ($211A-$2120, the renderer is Mode 0), read-only $2134+. */
        if(off == 0x2101 || (off >= 0x2105 && off <= 0x2114) ||
           (off >= 0x2123 && off <= 0x2133)) { ppu_reg[off & 0xff] = v; break; }
        M_DIE("write to unmodelled PPU register $%04X = $%02X\n", off, v);
    }
    return;
  }
  if(off >= 0x4300 && off <= 0x437f) { dma_reg[off - 0x4300] = v; return; }
  if(off == 0x420b) { int c; cpu_reg[0x0b] = v; for(c = 0; c < 8; c++) if(v & (1 << c)) dma_run(c); return; }
  /* $4200 NMITIMEN and $420C HDMAEN are mute: no interrupts and no HDMA here
     (the driver CALLS the NMI, HDMA only affects the screen).  $4202-$420A
     (mult/div, IRQ timers) and $420D (MEMSEL) stay out -- a multiplier that
     returns garbage is the silent error this file refuses. */
  if(off == 0x4200 || off == 0x420c) { cpu_reg[off & 0xff] = v; return; }
  M_DIE("write to unmodelled register $%04X = $%02X\n", off, v);
}

static uint8_t reg_read(uint16_t off) {
  switch(off) {
    case 0x2137: return 0;   /* SLHV: latches H/V.  Leaves the OPHCT/OPVCT
                                hi/lo flip-flop alone; $213F resets it. */
    case 0x213c: return 0;                                /* OPHCT: H unused */
    case 0x213d: {                                        /* OPVCT: V[7:0] / V[8] */
      uint8_t r = hv_hi ? (uint8_t)((m_vcounter >> 8) & 1) : (uint8_t)(m_vcounter & 0xff);
      hv_hi = !hv_hi; return r;
    }
    case 0x213f: hv_hi = 0; return 0x01;                  /* STAT78: NTSC, resets the ff */
    case 0x4210: return 0x42;                             /* RDNMI */
    case 0x4212: return 0x80;                             /* HVBJOY: in vblank */
  }
  if(off >= 0x4300 && off <= 0x437f) return dma_reg[off - 0x4300];
  M_DIE("read from unmodelled register $%04X\n", off);
  return 0;
}

static uint8_t bus_read(uint32_t a) {
  uint8_t *p = mem_ptr(a, 0);
  return p ? *p : reg_read((uint16_t)a);
}
static void bus_write(uint32_t a, uint8_t v) {
  uint8_t *p = mem_ptr(a, 1);
  if(p) *p = v; else reg_write((uint16_t)a, v);
}

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

static void push8(uint8_t v)  { bus_write(m_cpu.s, v); m_cpu.s = (uint16_t)(m_cpu.s - 1); }
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
static void branch(int take) { int8_t d = (int8_t)fetch8(); if(take) m_cpu.pc = (uint16_t)(m_cpu.pc + d); }

static void set_x(uint16_t v) { if(I8()) { m_cpu.x = (uint16_t)(v & 0xff); setnz8((uint8_t)v); } else { m_cpu.x = v; setnz16(v); } }

/* Called whenever P may have GAINED the X bit (SEP, PLP, RTI).  On the 65816
 * the high byte of X and Y is LOST the moment that flag goes up and a later
 * rep does not bring it back, so `sep #$10` then `abs,X` reads elsewhere. */
static void p_narrow_index(void) { if(m_cpu.p & M_X) { m_cpu.x &= 0x00ff; m_cpu.y &= 0x00ff; } }
static void set_y(uint16_t v) { if(I8()) { m_cpu.y = (uint16_t)(v & 0xff); setnz8((uint8_t)v); } else { m_cpu.y = v; setnz16(v); } }

/* One instruction. */
static void step(void) {
  uint8_t op = fetch8();
  int wa = !P8(), wi = !I8();
  uint32_t ea;
  m_cpu.instrs++;
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
    case 0x40: m_cpu.p = pull8(); p_narrow_index(); m_cpu.pc = pull16(); m_cpu.pbr = pull8(); break;
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
        m_cpu.x = (uint16_t)(m_cpu.x + dir);
        m_cpu.y = (uint16_t)(m_cpu.y + dir);
        if(m_cpu.a == 0) { m_cpu.a = 0xffff; break; }
        m_cpu.a = (uint16_t)(m_cpu.a - 1);
      }
      m_cpu.dbr = dst;
    } break;
    case 0xea: break;                                   /* NOP */
    case 0x42: (void)fetch8(); break;                   /* WDM */
    default: M_DIE("opcode not implemented $%02X\n", op);
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

  return st_fail;
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
