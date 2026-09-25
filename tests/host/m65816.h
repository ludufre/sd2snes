/* m65816.h -- 65816 interpreter plus enough of the SNES bus to EXECUTE the
 * real NES renderer (misc/nes_snes.bin) on the host.
 *
 * Not an emulator: no PPU rendering, no APU, and -- unless the optional CLOCK
 * at the bottom of this file is started -- no cycles and no IRQ/NMI.  What is
 * modelled is what the renderer's CHR-RAM path touches -- WRAM $7E/$7F, the
 * LoROM page in bank $00, the VRAM ports ($2115-$2119), the V counter
 * ($2137/$213D/$213F) and general-purpose DMA ($420B/$43xx).  Anything else
 * ABORTS with address and opcode; ignoring an unknown access would hand back
 * a worthless PASS.  Registers that only shape the SCREEN are mute BY
 * WHITELIST (the list is in reg_write()), never by 'default'.
 *
 * Memory comes up like the device: WRAM filled with $55 (the fork's
 * clear_wram), VRAM POISONED with $A5 -- nes_boot_init is what clears it.
 */
#ifndef M65816_H
#define M65816_H

#include <stddef.h>
#include <stdint.h>

/* --- P register flags --- */
#define M_C 0x01
#define M_Z 0x02
#define M_I 0x04
#define M_D 0x08
#define M_X 0x10
#define M_M 0x20
#define M_V 0x40
#define M_N 0x80

typedef struct {
  uint16_t a, x, y, s, d, pc;
  uint8_t  pbr, dbr, p;
  int      e;             /* emulation (0 = native; the renderer runs native) */
  uint64_t instrs;        /* instructions executed (anti-hang budget) */
} m_cpu_t;

extern m_cpu_t  m_cpu;
extern uint8_t  m_vram[0x10000];
extern uint16_t m_vcounter;      /* V returned by $213D (set by the driver) */
extern int      m_forced_blank;  /* last $2100 bit7 seen */
extern uint64_t m_instr_budget;  /* instruction ceiling per m_call() */

/* Reset the model: WRAM = $55, VRAM = $A5 (poisoned), CGRAM/OAM/registers = 0. */
void m_reset_memory(void);
/* Load the LoROM image (32KB = upper half of bank $00). */
void m_load_rom(const uint8_t *rom, size_t len);

/* HOST access: raw WRAM/ROM, no register decode. */
uint8_t m_peek(uint32_t addr24);
void    m_poke(uint32_t addr24, uint8_t v);
uint16_t m_peek16(uint32_t addr24);
void    m_poke16(uint32_t addr24, uint16_t v);
void    m_poke_block(uint32_t addr24, const void *src, size_t n);

/* Micro-tests of the model itself (index truncation via SEP/PLP/RTI, MVN,
 * VMAIN + mode-1 DMA, m_call through JSL).  Returns the failure count and
 * prints each one.  Leaves the model RESET -- call it before staging. */
int m_selftest(void);

/* ===========================================================================
 * GBC PLAYER EXTENSIONS (tests/host/gbc_render_cli.c)
 *
 * The NES renderer and the GBC player are two different programs sharing one
 * interpreter, and they touch DIFFERENT parts of the bus: the player fetches
 * its status block through the WRAM port, divides its transfer budget with the
 * hardware multiplier, drives the pads and needs the read-only view banks.
 * Modelling any of that unconditionally would WIDEN the surface the NES gate
 * accepts in silence -- an unmodelled access that used to abort would start
 * returning a plausible value.  So every one of them is behind m_gbc_mode(),
 * off by default, and run_nes_chr.sh never turns it on.
 *
 * What the flag opens (nothing else changes):
 *   banks $E0-$E4   read-only views, served from the caller's 320 KB buffer;
 *                   a WRITE there aborts (the FPGA would drop it silently)
 *   bank  $EF       the write window: a declared window (device: $EF00xx;
 *                   harness: a WRAM page) notifies a callback on every store
 *   $2180-$2183     WMDATA/WMADD, including as a DMA destination
 *   $2140-$2143     APU ports: read $00, writes sunk -- there is no S-SMP
 *   $2A00           MCU_CMD mailbox (the IGR combo), sunk and counted
 *   $4201           WRIO (the H/V latch enable the V guard depends on)
 *   $4202/3, $4216/7  the 8x8 multiplier GbcCapacity divides the window with
 *   $4218/$4219     the joypad the driver scripts
 * =========================================================================== */
void m_gbc_mode(int on);
/* The GBC player's channel layout: ch0 = HDMA decoy, ch6 = logical channel 0,
 * ch7 = every general DMA (see m65816.c, decoy_reg). */
void m_hdma_decoy(int on);

/* 320 KB, banks $E0-$E4 in order (wire $03 added $E4, the framebuffer view:
 * the player computes the A-bus of every C6 transfer from that bank, so the
 * harness serves it where the cartridge does instead of relocating it).  The
 * buffer stays the caller's; NULL (the default) makes any read abort. */
void m_set_view_window(uint8_t *buf320k);

/* Up to four address ranges [lo, hi] whose every READ (CPU or DMA source)
 * first calls `fn` with the address -- how a bridge model that runs in time
 * (the C6 row machine of wire $03) catches up to "now" before the player sees
 * a live status byte or a view byte.  n = 0 turns it off. */
void m_set_read_watch(int n, const uint32_t *lo, const uint32_t *hi, void (*fn)(uint32_t a24));
/* The master-cycle clock INCLUDING the cycles of the instruction in flight
 * (m_clock_mc() charges them only when the instruction ends). */
uint64_t m_clock_now(void);

/* Declare the $EF write window and who watches it.  `base24` may be the real
 * $EF00xx (nothing backs it: reads return $00, writes only reach `fn`) or the
 * WRAM page the harness build relocates it to (the store still lands in WRAM
 * AND reaches `fn`).  `fn` gets the OFFSET inside the window. */
void m_set_ef_sink(uint32_t base24, uint32_t len, void (*fn)(uint32_t off, uint8_t v));

/* $4218/$4219 (the auto-joypad result).  $4212 already reads as "vblank, not
 * busy", so the player's wait falls straight through. */
void m_set_pad(uint16_t joy1);

/* $2A00, the fork's MCU_CMD mailbox: the player writes it once when an IGR
 * combo fires.  Sunk, never executed. */
void m_set_cmd_sink(void (*fn)(uint8_t v));

/* The scanline $213D reports (9 bits).  The whole transfer engine is gated on
 * it: inside [41,225) the letterbox has handed the screen back and every DMA
 * must be refused. */
void m_set_v(int v);

/* Called after each general-purpose DMA completes.  `src24` is the A-bus
 * address and `bdest` the B-bus destination (VRAM byte address, CGRAM byte,
 * OAM byte or WRAM-port address) the transfer STARTED at -- both registers
 * have counted up by then, and the START is what says where the block LANDED. */
void m_set_dma_hook(void (*fn)(int ch, uint8_t bbad, uint32_t src24,
                               uint32_t bytes, uint32_t bdest));

/* Last byte written to each $2100-$21FF register (the whole block, including
 * the ones that are mute), and the write-twice scroll pairs decoded through
 * the shared BGOFS latch.  n = 1..4. */
const uint8_t *m_ppu_regs(void);
/* How many times each of them was written, same indexing.  A gate that has to
 * prove a routine did NOT touch a register needs counts: the LAST VALUE is the
 * same whether it was written once with the value already there or never. */
const uint32_t *m_ppu_reg_writes(void);
uint16_t m_bg_hofs(int n);
uint16_t m_bg_vofs(int n);
/* $2132 decoded: the fixed colour's R (0), G (1), B (2), 5 bits each. */
uint8_t  m_coldata(int c);

/* $2A00 traffic, for the driver to assert on. */
unsigned m_cmd_writes(void);

/* Lowest value the stack pointer ever reached, and a way to re-arm it after
 * the boot.  The player's working set lives right under the stack in bank
 * $7E, so a gate that has to say 'the deepest call chain stayed clear of the
 * counters' needs the measurement, not an estimate. */
uint16_t m_stack_low(void);
void m_stack_low_reset(void);

/* The CPU ($4200-$42FF) and DMA ($4300-$437F) register files as the model
 * holds them.  $420C and the $43xx block are write-only on real silicon, so
 * there is no read path to go through m_peek: a gate that has to assert which
 * HDMA channels the player armed, and at what table, needs the shadow. */
const uint8_t *m_cpu_regs(void);
const uint8_t *m_dma_regs(void);
/* GBC mode: the HDMA decoy on ch0 (its registers), the physical $420C, and how
 * many $420C writes armed something without it (or armed ch7). */
const uint8_t *m_decoy_regs(void);
uint8_t m_phys_420c(void);
unsigned m_decoy_bad(void);

/* CGRAM (512 B) and OAM (512 low + 32 high, byte-addressed) as m_vram is:
 * mutable, so a driver can poison them and read the result back.  The write
 * ports maintain them; there is no read port modelled. */
uint8_t *m_cgram(void);
uint8_t *m_oam(void);

/* Run from `from` until PBR:PC reaches `stop` at an instruction boundary.
 * m_call cannot express a boot: the player's Reset resets the stack (so the
 * pushed return address is gone) and ends in `wai / bra *`, which never
 * returns.  Executing the REAL boot is the only way to get the direct-page
 * gates, the budget seed and the HDMA tables set up without copying the
 * numbers into the driver.  Aborts on m_instr_budget. */
void m_run_to(uint32_t from, uint32_t stop);

/* Run a routine until its matching RTS/RTL.  `is_long` = called through JSL
 * (3-byte return).  Aborts if m_instr_budget is exceeded. */
void m_call(uint32_t addr24, int is_long);

/* The same call, SUSPENDABLE: it gives up after `max` instructions and leaves
 * the CPU where it stopped.  Returns 0 when the routine returned and 1 when it
 * is suspended; m_step_resume() carries on with a fresh allowance.
 *
 * This is what lets a driver interleave two entry points without an interrupt
 * model.  On silicon an NMI is exactly "push the registers, run a routine at
 * this stack depth, pull them back", so a driver that saves m_cpu, m_call()s
 * the NMI body and restores everything but the stack pointer is running the
 * real thing -- and m_step_sp() is the stack depth the suspended call was
 * entered at, so the body can be checked for leaving the stack as it found it.
 * One suspended call at a time; a nested m_call meanwhile is fine. */
int m_call_stepped(uint32_t addr24, int is_long, uint64_t max);
int m_step_resume(uint64_t max);
uint16_t m_step_sp(void);

/* ===========================================================================
 * THE CLOCK (GBC player, phase 5).  Off by default; nothing above changes
 * while it is off, so every gate written for the cycle-less model still runs
 * against exactly that model.
 *
 * m_clock_start(line, lines) turns it on at the START of scanline `line` of a
 * `lines`-line frame (262 NTSC, 312 PAL).  From then on:
 *   * every CPU bus access costs its region's speed (8/6/12 mc, slow ROM),
 *     every internal cycle 6 mc, an interrupt entry its pushes + 2;
 *   * a general DMA costs 8 mc a byte + 24, and its [start, end) is readable
 *     from the DMA hook through m_dma_start_mc()/m_dma_end_mc();
 *   * each scanline start charges 40 mc of DRAM refresh and, on lines 0..224
 *     with $420C != 0, the contract's HDMA tax 18 + 8C + 8B (sec. 11.1);
 *   * $213D reads the V latched by $2137, $4212 has vblank/hblank/auto-joypad,
 *     $4210 the NMI flag, $4211 TIMEUP; the NMI fires at V=225 when $4200 b7,
 *     the V-IRQ at V=VTIME ($4209/$420A) when $4200 b5 (H-IRQ not modelled);
 *   * WAI sleeps to the next interrupt.
 * m_run_clocked(mc) executes from the current PC until the clock reaches `mc`.
 * The line hook runs at every line start (the bridge's LY=0, statistics);
 * the interrupt hook on every entry (kind 0 NMI, 1 IRQ) and RTI (kind 2).
 * m_mainloop_mc() is the time spent outside any handler.
 * =========================================================================== */
void     m_clock_start(int line, int lines);
void     m_clock_stop(void);
uint64_t m_clock_mc(void);
int      m_clock_line(void);
int      m_clock_h(void);
uint64_t m_clock_line_start(void);
uint64_t m_mainloop_mc(void);
int      m_int_depth(void);
void     m_set_line_hook(void (*fn)(int v));
/* From inside the line hook: the CPU stalls `mc` master cycles right after it
 * (V and every interrupt keep running; the code simply gets there later). */
void     m_clock_stall(uint64_t mc);
void     m_set_int_hook(void (*fn)(int kind, int v, int h));
/* Every CPU store (not the DMA's), with its 24-bit address as the CPU issued
 * it -- a direct-page or bank-$00 store is NOT folded onto $7E here. */
void     m_set_write_watch(void (*fn)(uint32_t a24));
/* Every instruction, with the PBR:PC it is about to execute (NULL = off, the
 * default).  The raster compiler's profiler (gbc_render_cli --compile-profile)
 * attributes instructions and master cycles to the code they ran in. */
void     m_set_pc_hook(void (*fn)(uint32_t pc24));
uint64_t m_dma_start_mc(void);
uint64_t m_dma_end_mc(void);
void     m_run_clocked(uint64_t until_mc);
/* $4210 bits 3:0 (default 2) and $213F bit 4 (default 0 = NTSC).  Both are
 * read by the player's boot, so they are set BEFORE it runs, clock or not. */
void     m_set_cpurev(int rev);
void     m_set_pal(int pal);

#endif /* M65816_H */
