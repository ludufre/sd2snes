// Copyright (c) 2012-2013 Ludvig Strigeus
// This program is GPL Licensed. See COPYING for the full license.
//
// ---------------------------------------------------------------------------
// PRUNED for sd2snes_nes (started at Phase -1, nestest gate only; MMC3 added
// back in Phase 2.1).
//
// Vendored from strigeus/fpganes `src/mmu.v` (1757 lines). This file originally
// contained the `MultiMapper` dispatcher plus EVERY supported mapper (MMC0..MMC5,
// Rambo1, Mapper13/15/28/34/41/66/68/69/71/79/228/234, NesEvent). Per
// NES-FPGANES-ANALYSIS.md #5 and NES-CORE-CONTRACT.md #9, Phase -1 only needed to
// exercise CPU+DMA+DMC+mapper 0 (nestest.nes is an NROM/mapper-0 image), so this
// file keeps ONLY:
//   - MMC0     (mmu.v:5-21 upstream)   -- trivial NROM fallback, kept as `default`
//   - MMC1     (mmu.v:25-133 upstream) -- mapper 1 (not exercised by nestest, but
//                                          cheap and explicitly requested to stay)
//   - Mapper28 (mmu.v:946-1036 upstream) -- mappers 0, 2, 3, 7, 28 (nestest.nes is
//                                          mapper 0, dispatched here)
//   - MMC3     (mmu.v:251-395 upstream) -- mapper 4 ONLY (Phase 2.1; the 47/118/119
//                                          variants that share the upstream module
//                                          are pruned -- see that module's header)
//   - MultiMapper, reduced to a 4-way case (1 / 4 / {0,2,3,7,28} / default) plus the
//     mapper-agnostic prg_mask/chr_mask + CHR-VRAM/CPU-RAM remap tail, which is
//     unchanged from upstream (mmu.v:1670-1749).
//
// REMOVED entirely (modules + their instantiation + case arms in MultiMapper):
//   MMC2 (mapper 9), MMC5 (5, incl. its 1KB ExRAM + mul8x8 +
//   vsplit + scanline IRQ), Rambo1 (64/158), Mapper13 (13), Mapper15 (15),
//   Mapper34 (34), Mapper41 (41), Mapper66 (11/66), Mapper68 (68), Mapper69 (69),
//   Mapper71 (71/232), Mapper79 (79/113), Mapper228 (228), Mapper234 (234),
//   NesEvent (105, which upstream reaches INTO MMC1's internals -- mmc1_chr/
//   mmc1_aout -- so it could not have been pruned independently of MMC1 anyway).
//
// Consequences of the prune:
//   - `irq` is driven ONLY by MMC3 (mapper 4). For every other mapper here the
//     upstream `irq=0` default in the combinational block is never overridden,
//     exactly as before Phase 2.1.
//   - `has_chr_dout`/`chr_dout`/`prg_dout` (used upstream only by MMC5's ExRAM
//     readback) are always the inert defaults (0 / 0 / 8'hff).
//   - `ppu_ce` (2nd clock-enable input, used upstream by MMC2/MMC3/MMC5/Rambo1 to
//     react every PPU dot instead of only on cart_ce) is consumed by MMC3 since
//     Phase 2.1; nes.v has always fed it the core `ce` (nes.v: `MultiMapper
//     multi_mapper(clk, cart_ce, ce, ...)`), so nothing outside this file changed.
//
// FUNCTIONAL DEVIATIONS from upstream (besides the prune):
//   - Mapper28 maps 8KB of PRG-RAM at $6000-$7FFF (upstream had none there) --
//     see the comment block at the end of module Mapper28 for the full rationale
//     (blargg test harness / Family-Basic-style NROM / standard emulator behavior).
//   - MMC3 forces the PRG-RAM enable bit on (`ram_enable_eff`) for the SAME
//     reason -- see the DEVIATION block in module MMC3.
// ---------------------------------------------------------------------------

// No mapper chip
module MMC0(input clk, input ce,
            input [31:0] flags,
            input [15:0] prg_ain, output [21:0] prg_aout,
            input prg_read, prg_write,                   // Read / write signals
            input [7:0] prg_din,
            output prg_allow,                            // Enable access to memory for the specified operation.
            input [13:0] chr_ain, output [21:0] chr_aout,
            output chr_allow,                      // Allow write
            output vram_a10,                             // Value for A10 address line
            output vram_ce);                             // True if the address should be routed to the internal 2kB VRAM.
  assign prg_aout = {7'b00_0000_0, prg_ain[14:0]};
  assign prg_allow = prg_ain[15] && !prg_write;
  assign chr_allow = flags[15];
  assign chr_aout = {9'b10_0000_000, chr_ain[12:0]};
  assign vram_ce = chr_ain[13];
  assign vram_a10 = flags[14] ? chr_ain[10] : chr_ain[11];
endmodule

// MMC1 mapper chip. Maps prg or chr addresses into a linear address.
// If vram_ce is set, {vram_a10, chr_aout[9:0]} are used to access the NES internal VRAM instead.
module MMC1(input clk, input ce, input reset,
            input [31:0] flags,
            input [15:0] prg_ain, output [21:0] prg_aout,
            input prg_read, prg_write,                   // Read / write signals
            input [7:0] prg_din,
            output prg_allow,                            // Enable access to memory for the specified operation.
            input [13:0] chr_ain, output [21:0] chr_aout,
            output chr_allow,                      // Allow write
            output vram_a10,                             // Value for A10 address line
            output vram_ce,                              // True if the address should be routed to the internal 2kB VRAM.
            // sd2snes video-bridge CHR-bank tap (raw register state; MultiMapper
            // derives the per-slot snapshot -- see chr_snap_* there)
            output [4:0] chr_snap_bank0,
            output [4:0] chr_snap_bank1,
            output chr_snap_4k,
            // sd2snes video-bridge NT-arrangement tap (v2.0a): raw MMC1 mirror
            // control control[1:0] (00=1-screen lower, 01=1-screen upper,
            // 10=vertical, 11=horizontal -- same field vram_a10_t decodes).
            // MultiMapper maps it to the protocol NTARR code (see ntarr_of).
            output [1:0] snap_mirror);
  reg [4:0] shift;

// CPPMM
// |||||
// |||++- Mirroring (0: one-screen, lower bank; 1: one-screen, upper bank;
// |||               2: vertical; 3: horizontal)
// |++--- PRG ROM bank mode (0, 1: switch 32 KB at $8000, ignoring low bit of bank number;
// |                         2: fix first bank at $8000 and switch 16 KB bank at $C000;
// |                         3: fix last bank at $C000 and switch 16 KB bank at $8000)
// +----- CHR ROM bank mode (0: switch 8 KB at a time; 1: switch two separate 4 KB banks)
  reg [4:0] control;

// CCCCC
// |||||
// +++++- Select 4 KB or 8 KB CHR bank at PPU $0000 (low bit ignored in 8 KB mode)
  reg [4:0] chr_bank_0;

// CCCCC
// |||||
// +++++- Select 4 KB CHR bank at PPU $1000 (ignored in 8 KB mode)
  reg [4:0] chr_bank_1;

// RPPPP
// |||||
// |++++- Select 16 KB PRG ROM bank (low bit ignored in 32 KB mode)
// +----- PRG RAM chip enable (0: enabled; 1: disabled; ignored on MMC1A)
  reg [4:0] prg_bank;

  // Update shift register
  always @(posedge clk) if (reset) begin
    shift <= 1;
    control <= 'hC;
    // sd2snes_nes: chr_bank_0/1 had NO reset upstream (X in simulation until
    // first written; 0 in hardware).  Reset added for sim determinism -- the
    // bridge CHR-bank tap reads them every frame; same precedent as Mapper28's
    // a53chr.  Hardware behavior unchanged (registers power up 0), and the
    // golden simulator's power-on state is bank 0 too (mappers.py).
    chr_bank_0 <= 0;
    chr_bank_1 <= 0;
  end else if (ce) begin
    if (prg_write && prg_ain[15]) begin
      if (prg_din[7]) begin
        shift <= 5'b10000;
        control <= control | 'hC;
      end else begin
        if (shift[0]) begin
          casez(prg_ain[14:13])
          0: control    <= {prg_din[0], shift[4:1]};
          1: chr_bank_0 <= {prg_din[0], shift[4:1]};
          2: chr_bank_1 <= {prg_din[0], shift[4:1]};
          3: prg_bank   <= {prg_din[0], shift[4:1]};
          endcase
          shift <= 5'b10000;
        end else begin
          shift <= {prg_din[0], shift[4:1]};
        end
      end
    end
  end

  // The PRG bank to load. Each increment here is 16kb. So valid values are 0..15.
  reg [3:0] prgsel;
  always @* begin
    casez({control[3:2], prg_ain[14]})
    3'b0?_?: prgsel = {prg_bank[3:1], prg_ain[14]};
    3'b10_0: prgsel = 4'b0000;
    3'b10_1: prgsel = prg_bank[3:0];
    3'b11_0: prgsel = prg_bank[3:0];
    3'b11_1: prgsel = 4'b1111;
    endcase
  end
  wire [21:0] prg_aout_tmp = {4'b00_00,  prgsel, prg_ain[13:0]};

  // The CHR bank to load. Each increment here is 4 kb. So valid values are 0..31.
  reg [4:0] chrsel;
  always @* begin
    casez({control[4], chr_ain[12]})
    2'b0_?: chrsel = {chr_bank_0[4:1], chr_ain[12]};
    2'b1_0: chrsel = chr_bank_0;
    2'b1_1: chrsel = chr_bank_1;
    endcase
  end
  assign chr_aout = {5'b100_00, chrsel, chr_ain[11:0]};

  // The a10 VRAM address line. (Used for mirroring)
  reg vram_a10_t;
  always @* begin
    casez(control[1:0])
    2'b00: vram_a10_t = 0;             // One screen, lower bank
    2'b01: vram_a10_t = 1;             // One screen, upper bank
    2'b10: vram_a10_t = chr_ain[10];   // One screen, vertical
    2'b11: vram_a10_t = chr_ain[11];   // One screen, horizontal
    endcase
  end
  assign vram_a10 = vram_a10_t;
  assign vram_ce = chr_ain[13];

  wire prg_is_ram = prg_ain >= 'h6000 && prg_ain < 'h8000;
  assign prg_allow = prg_ain[15] && !prg_write || prg_is_ram;
  wire [21:0] prg_ram = {9'b11_1100_000, prg_ain[12:0]};

  assign prg_aout = prg_is_ram ? prg_ram : prg_aout_tmp;
  assign chr_allow = flags[15];

  // sd2snes bridge tap: raw CHR-bank register state (register outputs only)
  assign chr_snap_bank0 = chr_bank_0;
  assign chr_snap_bank1 = chr_bank_1;
  assign chr_snap_4k    = control[4];
  // sd2snes bridge tap (v2.0a): raw MMC1 mirror control (dynamic; power-on 0x0C
  // -> control[1:0]=00 = 1-screen lower, matching the golden mappers.py MMC1
  // reset state and the vram_a10_t decode above).
  assign snap_mirror    = control[1:0];
endmodule

// Mapper28 -- covers mappers 0 (NROM), 2 (UNROM), 3 (CNROM), 7 (AxROM) and 28
// (homebrew "mapper 28" multi-discrete). nestest.nes is mapper 0, dispatched here.
module Mapper28(input clk, input ce, input reset,
                input [31:0] flags,
                input [15:0] prg_ain, output [21:0] prg_aout,
                input prg_read, prg_write,                   // Read / write signals
                input [7:0] prg_din,
                output prg_allow,                            // Enable access to memory for the specified operation.
                input [13:0] chr_ain, output [21:0] chr_aout,
                output chr_allow,                      // Allow write
                output reg vram_a10,                         // Value for A10 address line
                output vram_ce,                              // True if the address should be routed to the internal 2kB VRAM.
                // sd2snes video-bridge CHR-bank tap (raw CNROM latch)
                output [1:0] chr_snap_a53chr,
                // sd2snes video-bridge NT-arrangement tap (v2.0a): raw mode[1:0]
                // (00=1-screen lower, 01=1-screen upper, 10=vertical,
                // 11=horizontal -- same field vram_a10 decodes; covers AxROM's
                // dynamic single-screen page select via mode[0]).
                output [1:0] snap_mirror);
    reg [6:0] a53prg;    // output PRG ROM (A14-A20 on ROM)
    reg [1:0] a53chr;    // output CHR RAM (A13-A14 on RAM)

    reg [3:0] inner;    // "inner" bank at 01h
    reg [5:0] mode;     // mode register at 80h
    reg [5:0] outer;    // "outer" bank at 81h
    reg [1:0] selreg;   // selector register

    // Allow writes to 0x5000 only when launching through the proper mapper ID.
    wire [7:0] mapper = flags[7:0];
    wire allow_select = (mapper == 8'd28);

    always @(posedge clk) if (reset) begin
      mode[5:2] <= 0;         // NROM mode, 32K mode
      outer[5:0] <= 6'h3f;    // last bank
      inner <= 0;
      selreg <= 1;
      // sd2snes: a53chr had NO reset upstream (relied on FPGA power-up-0;
      // X in simulation for mappers that never write it -- 0/2/7).  Reset it
      // for sim determinism (the bridge CHR-bank tap reads it); hardware
      // behavior unchanged (registers power up 0 anyway), and CNROM's
      // power-on latch state is bank 0 by the same convention the golden
      // simulator uses (mappers.py chr_bank=0 initial).
      a53chr <= 0;

      // Set value for mirroring
      if (mapper == 2 || mapper == 0 || mapper == 3)
        mode[1:0] <= flags[14] ? 2'b10 : 2'b11;

      // UNROM #2 - Current bank in $8000-$BFFF and fixed top half of outer bank in $C000-$FFFF
      if (mapper == 2)
        mode[5:2] <= 4'b1111;

      // CNROM #3 - Fixed PRG bank, switchable CHR bank.
      if (mapper == 3)
        selreg <= 0;

      // AxROM #7 - Switch 32kb rom bank + switchable nametables
      if (mapper == 7) begin
        mode[1:0] <= 2'b00;   // Switchable VRAM page.
        mode[5:2] <= 4'b1100; // 256K banks, (B)NROM mode
      end
    end else if (ce) begin
      if ((prg_ain[15:12] == 4'h5) & prg_write && allow_select) selreg <= {prg_din[7], prg_din[0]};        // select register
      if (prg_ain[15] & prg_write) begin
        case (selreg)
        2'h0:  {mode[0], a53chr}  <= {(mode[1] ? mode[0] : prg_din[4]), prg_din[1:0]};  // CHR RAM bank
        2'h1:  {mode[0], inner}   <= {(mode[1] ? mode[0] : prg_din[4]), prg_din[3:0]};  // "inner" bank
        2'h2:  {mode}             <= {prg_din[5:0]};                                    // mode register
        2'h3:  {outer}            <= {prg_din[5:0]};                                    // "outer" bank
        endcase
      end
    end

    always @* begin
      // mirroring mode
      casez(mode[1:0])
      2'b0?   :   vram_a10 = {mode[0]};        // 1 screen lower
      2'b10   :   vram_a10 = {chr_ain[10]};    // vertical
      2'b11   :   vram_a10 = {chr_ain[11]};    // horizontal
      endcase

      // PRG ROM bank size select
      casez({mode[5:2], prg_ain[14]})
      5'b00_0?_?  :  a53prg = {outer[5:0],             prg_ain[14]};  // 32K banks, (B)NROM mode
      5'b01_0?_?  :  a53prg = {outer[5:1], inner[0],   prg_ain[14]};  // 64K banks, (B)NROM mode
      5'b10_0?_?  :  a53prg = {outer[5:2], inner[1:0], prg_ain[14]};  // 128K banks, (B)NROM mode
      5'b11_0?_?  :  a53prg = {outer[5:3], inner[2:0], prg_ain[14]};  // 256K banks, (B)NROM mode

      5'b00_10_1,
      5'b00_11_0  :  a53prg = {outer[5:0], inner[0]};             // 32K banks, UNROM mode
      5'b01_10_1,
      5'b01_11_0  :  a53prg = {outer[5:1], inner[1:0]};           // 64K banks, UNROM mode
      5'b10_10_1,
      5'b10_11_0  :  a53prg = {outer[5:2], inner[2:0]};           // 128K banks, UNROM mode
      5'b11_10_1,
      5'b11_11_0  :  a53prg = {outer[5:3], inner[3:0]};           // 256K banks, UNROM mode

      default     :  a53prg = {outer[5:0],             prg_ain[14]};  // 16K fixed bank
      endcase
    end

  assign vram_ce = chr_ain[13];
  // DEVIATION from upstream fpganes (documented; found by the blargg
  // interrupt-suite bring-up): upstream Mapper28 exposes NO PRG-RAM -- reads
  // and writes at $6000-$7FFF are dead (prg_allow=0, writes dropped, reads
  // return the mapper's 8'hff). But blargg's test harness (and Family-Basic-
  // style NROM boards, and effectively every emulator's mapper-0 behavior)
  // expects 8KB of PRG-RAM at $6000-$7FFF: the tests write their status byte
  // to $6000, the DE B0 61 magic to $6001-6003 and result text to $6004+, and
  // the harness reads the region back too. Mirror MMC1's prg_is_ram mapping
  // exactly (CART-RAM window at PSRAM 0x3C0000, ANALYSIS SS2.4) for all
  // mappers this module serves (0/2/3/7/28). Providing RAM where a real board
  // had none is harmless for well-behaved ROMs (they never touch it); a ROM
  // relying on open-bus reads at $6000 would now see RAM instead --
  // acceptable, matches common emulator behavior.
  wire prg_is_ram = prg_ain >= 'h6000 && prg_ain < 'h8000;
  wire [21:0] prg_ram = {9'b11_1100_000, prg_ain[12:0]};
  assign prg_aout = prg_is_ram ? prg_ram : {1'b0, (a53prg & 7'b0011111), prg_ain[13:0]};
  assign prg_allow = (prg_ain[15] && !prg_write) || prg_is_ram;
  assign chr_allow = flags[15];
  assign chr_aout = {7'b10_0000_0, a53chr, chr_ain[12:0]};

  // sd2snes bridge tap: raw CNROM CHR latch (register output only)
  assign chr_snap_a53chr = a53chr;
  // sd2snes bridge tap (v2.0a): raw mirror mode (dynamic for AxROM/#7; static
  // header value for 0/2/3 -- set from flags[14] at reset).
  assign snap_mirror     = mode[1:0];
endmodule

// MMC3 mapper chip -- mapper 4 (TxROM boards: TSROM/TLROM/TKROM/TGROM/...).
// Ported for Phase 2.1 from strigeus/fpganes `src/mmu.v:251-395`, which served
// mappers 4/47/118/119 from this one module. Only mapper 4 survives here.
//
// CLOCKING: unlike MMC1/Mapper28 (clocked by cart_ce = 1 tick every 3 dots),
// MMC3 is clocked by `ppu_ce` = ONE PPU DOT, exactly as upstream instantiates it
// (fpganes/src/mmu.v:1592 `MMC3 mmc3(clk, ppu_ce, ...)`). The A12 scanline
// counter needs dot resolution: its 15-dot hysteresis filter (a12_ctr) is what
// makes a PPU fetch pattern look like one scanline tick. MultiMapper already
// carried ppu_ce in its signature and nes.v has always fed it the core `ce`
// (one PPU dot), so this port changes nothing outside this file. The pacer
// stretches WALL TIME, not the dot count, so the filter keeps its hardware
// meaning (15 dots = 5 M2 cycles).
//
// PRUNED (all three were served by the same upstream module; none is reachable
// here, and each one costs area or actively conflicts):
//   - mapper 47 (multicart): its block-select register is written by ANY write
//     to $6000-$7FFF, which collides with the PRG-RAM window, and it is the only
//     reason upstream's prg_allow/prg_aout carry `&& !mapper47`.
//   - TQROM (mapper 119): 8KB of CHR-RAM at a SECOND address space
//     (9'b11_1111_111); this core's CHR-RAM path is the 0x200000 one, and a
//     second one with no target game is pure debt.
//   - TxSROM (mapper 118): drives CIRAM A10 from chrsel[7] (a form of
//     four-screen), which the loader already rejects (src/nes.c) and which
//     nt_snap_arr cannot encode.
//   - `mmc3_alt_behavior`: upstream declares `wire mmc3_alt_behavior = 0`
//     (mmu.v:277) = MMC3C / Sharp MMC3B "normal" IRQ semantics. Kept at 0, so
//     the guard it controls is constant-true and is pruned to the constant (see
//     the IRQ block). CONSEQUENCE, by design: the "alternate"/rev-A IRQ test
//     ROMs (mmc3_test_2/6-MMC3_alt, mmc3_irq_tests/5.MMC3_rev_A) FAIL -- they
//     are declared expected-fails in run_mmc3.sh / run_mmc3_irq.sh. mapper_flags
//     bit [14] (the iNES mirroring bit, which MMC3 ignores because $A000 owns
//     mirroring) is the natural place to expose the variant one day -- RESERVED,
//     not used.
//
// TAPS: MMC3's CHR view is a VECTOR OF EIGHT 1KB WINDOWS, which the slot0/slot1
// pair of the legacy tap (chr_snap_*, CMD_CHR_STATE $12) cannot represent, so
// mapper 4 gets its OWN tap here (snap_chr_win, 8x8 bits) feeding the new
// CMD_CHR_STATE8 $14 / CMD_CHR_SPLITS8 $15 opcodes. The legacy pair stays at a
// CONSTANT sentinel for mapper 4 -- see the snapshot block in MultiMapper for
// why that constant IS the mechanism that suppresses $13. The mirror tap IS
// wired, because MMC3 owns mirroring dynamically via $A000 and the static
// header bit is meaningless for it.
module MMC3(input clk, input ce, input reset,
            input [31:0] flags,
            input [15:0] prg_ain, output [21:0] prg_aout,
            input prg_read, prg_write,                   // Read / write signals
            input [7:0] prg_din,
            output prg_allow,                            // Enable access to memory for the specified operation.
            input [13:0] chr_ain, output [21:0] chr_aout,
            output chr_allow,                            // Allow write
            output vram_a10,                             // Value for A10 address line
            output vram_ce,                              // True if the address should be routed to the internal 2kB VRAM.
            output reg irq,
            // sd2snes video-bridge NT-arrangement tap (v2.0a molde): raw mirror
            // control in the SAME encoding MMC1/Mapper28 export
            // (00=1scr-lo, 01=1scr-hi, 10=vertical, 11=horizontal). MMC3 only
            // does V/H, so the high bit is tied 1 -- see the assign at the end.
            output [1:0] snap_mirror,
            // sd2snes video-bridge CHR WINDOW VECTOR tap (protocol v2.5): the
            // eight 1KB windows the PPU currently sees, window k in [k*8 +: 8].
            // UNMASKED (the size mask is applied where every other tap masks --
            // in MultiMapper); combinational here, registered under ce there.
            output [63:0] snap_chr_win);
  reg [2:0] bank_select;             // Register to write to next
  reg prg_rom_bank_mode;             // Mode for PRG banking
  reg chr_a12_invert;                // Mode for CHR banking
  reg mirroring;                     // 0 = vertical, 1 = horizontal
  reg irq_enable, irq_reload;        // IRQ enabled, and IRQ reload requested
  reg [7:0] irq_latch, counter;      // IRQ latch value and current counter
  reg ram_enable, ram_protect;       // RAM protection bits
  reg [6:0] chr_bank_0, chr_bank_1;  // Selected CHR banks
  reg [7:0] chr_bank_2, chr_bank_3, chr_bank_4, chr_bank_5;
  reg [5:0] prg_bank_0, prg_bank_1;  // Selected PRG banks
  wire prg_is_ram;

  // DEVIATION from upstream fpganes (documented; same class and same cause as
  // the Mapper28 PRG-RAM deviation at the end of that module): the PRG-RAM
  // enable bit is FORCED ON. Upstream honors `ram_enable` ($A001 bit 7), which
  // resets to 0, so $6000-$7FFF is dead until the ROM programs it.
  // Measured in the fixtures: mmc3_test/5-MMC3.nes and
  // mmc3_test_2/rom_singles/1-clocking.nes each contain FIVE `STA $6000`-family
  // opcodes and ZERO `STA $A001` (8D 01 A0) -- i.e. the blargg harness writes
  // its status byte/magic/result text to the PRG-RAM window but never enables
  // it. With upstream semantics every one of those writes is dropped and the
  // testbench is blind (it polls CART-RAM at PSRAM 0x3C0000). Real TxROM boards
  // (TSROM/TLROM/TKROM) DO have the RAM chip -- only the enable is a register --
  // so handing a well-behaved ROM an already-enabled RAM is inert (it programs
  // $A001 before using it anyway). `ram_protect` ($A001 bit 6) stays FULLY
  // FUNCTIONAL, so write-protection semantics are unchanged. `ram_enable`
  // itself is still latched (kept for fidelity and for a future savestate of
  // the mapper state); it simply has no consumer while this deviation stands.
  wire ram_enable_eff = 1'b1;

  wire [7:0] new_counter = (counter == 0 || irq_reload) ? irq_latch : counter - 1;
  reg [3:0] a12_ctr;

  always @(posedge clk) if (reset) begin
    irq <= 0;
    bank_select <= 0;
    prg_rom_bank_mode <= 0;
    chr_a12_invert <= 0;
    mirroring <= 0;
    {irq_enable, irq_reload} <= 0;
    {irq_latch, counter} <= 0;
    {ram_enable, ram_protect} <= 0;
    {chr_bank_0, chr_bank_1} <= 0;
    {chr_bank_2, chr_bank_3, chr_bank_4, chr_bank_5} <= 0;
    {prg_bank_0, prg_bank_1} <= 0;
    a12_ctr <= 0;
  end else if (ce) begin
    if (prg_write && prg_ain[15]) begin
      case({prg_ain[14], prg_ain[13], prg_ain[0]})
      3'b00_0: {chr_a12_invert, prg_rom_bank_mode, bank_select} <= {prg_din[7], prg_din[6], prg_din[2:0]}; // Bank select ($8000-$9FFE, even)
      3'b00_1: begin // Bank data ($8001-$9FFF, odd)
        case (bank_select)
        0: chr_bank_0 <= prg_din[7:1];  // Select 2 KB CHR bank at PPU $0000-$07FF (or $1000-$17FF);
        1: chr_bank_1 <= prg_din[7:1];  // Select 2 KB CHR bank at PPU $0800-$0FFF (or $1800-$1FFF);
        2: chr_bank_2 <= prg_din;       // Select 1 KB CHR bank at PPU $1000-$13FF (or $0000-$03FF);
        3: chr_bank_3 <= prg_din;       // Select 1 KB CHR bank at PPU $1400-$17FF (or $0400-$07FF);
        4: chr_bank_4 <= prg_din;       // Select 1 KB CHR bank at PPU $1800-$1BFF (or $0800-$0BFF);
        5: chr_bank_5 <= prg_din;       // Select 1 KB CHR bank at PPU $1C00-$1FFF (or $0C00-$0FFF);
        6: prg_bank_0 <= prg_din[5:0];  // Select 8 KB PRG ROM bank at $8000-$9FFF (or $C000-$DFFF);
        7: prg_bank_1 <= prg_din[5:0];  // Select 8 KB PRG ROM bank at $A000-$BFFF
        endcase
      end
      3'b01_0: mirroring <= prg_din[0];                   // Mirroring ($A000-$BFFE, even)
      3'b01_1: {ram_enable, ram_protect} <= prg_din[7:6]; // PRG RAM protect ($A001-$BFFF, odd)
      3'b10_0: irq_latch <= prg_din;                      // IRQ latch ($C000-$DFFE, even)
      3'b10_1: irq_reload <= 1;                           // IRQ reload ($C001-$DFFF, odd)
      3'b11_0: begin irq_enable <= 0; irq <= 0; end       // IRQ disable ($E000-$FFFE, even)
      3'b11_1: irq_enable <= 1;                           // IRQ enable ($E001-$FFFF, odd)
      endcase
    end

    // Trigger IRQ counter on rising edge of chr_ain[12]
    // All MMC3A's and non-Sharp MMC3B's will generate only a single IRQ when $C000 is $00.
    // This is because this version of the MMC3 generates IRQs when the scanline counter is decremented to 0.
    // In addition, writing to $C001 with $C000 still at $00 will result in another single IRQ being generated.
    // In the community, this is known as the "alternate" or "old" behavior.
    // All MMC3C's and Sharp MMC3B's will generate an IRQ on each scanline while $C000 is $00.
    // This is because this version of the MMC3 generates IRQs when the scanline counter is equal to 0.
    // In the community, this is known as the "normal" or "new" behavior.
    if (chr_ain[12] && a12_ctr == 0) begin
      counter <= new_counter;
      // sd2snes_nes: upstream guards this with
      //   (!mmc3_alt_behavior || counter != 0 || irq_reload)
      // and ties mmc3_alt_behavior to 0, making the guard constant-true. Pruned
      // to the constant (we are MMC3C / Sharp MMC3B, "normal" behavior); see the
      // module header for the expected-fail consequence on the rev-A test ROMs.
      if (new_counter == 0 && irq_enable) begin
        irq <= 1;
      end
      irq_reload <= 0;
    end
    a12_ctr <= chr_ain[12] ? 4'b1111 : (a12_ctr != 0) ? a12_ctr - 4'b0001 : a12_ctr;
  end

  // The PRG bank to load. Each increment here is 8kb. So valid values are 0..63.
  reg [5:0] prgsel;
  always @* begin
    casez({prg_ain[14:13], prg_rom_bank_mode})
    3'b00_0: prgsel = prg_bank_0;  // $8000 mode 0
    3'b00_1: prgsel = 6'b111110;   // $8000 fixed to second last bank
    3'b01_?: prgsel = prg_bank_1;  // $A000 mode 0,1
    3'b10_0: prgsel = 6'b111110;   // $C000 fixed to second last bank
    3'b10_1: prgsel = prg_bank_0;  // $C000 mode 1
    3'b11_?: prgsel = 6'b111111;   // $E000 fixed to last bank
    endcase
  end

  // The CHR bank to load. Each increment here is 1kb. So valid values are 0..255.
  //
  // SINGLE IMPLEMENTATION (do not fork this): upstream writes the selection as a
  // casez over {chr_ain[12]^inv, chr_ain[11], chr_ain[10]} (fpganes mmu.v:370-377).
  // The sd2snes bridge needs the SAME function evaluated for all eight windows at
  // once (snap_chr_win, below), so the body moved into `chr_win_of` and both the
  // address path and the tap call it. Forking them would let the tap drift from
  // what the PPU actually fetches -- the one failure mode this arrangement makes
  // impossible.
  //
  //   j = {k[2] ^ chr_a12_invert, k[1], k[0]}     (k = the PPU's chr_ain[12:10])
  //   j[2]==0 : win = {(j[1] ? chr_bank_1 : chr_bank_0), j[0]}   (2KB pair)
  //   j[2]==1 : win = chr_bank_{2 + j[1:0]}                      (1KB slot)
  //
  // j[0]==k[0] (the XOR only touches bit 2), which is why the low bit of a 2KB
  // pair is the raw PPU address bit exactly as in the upstream casez. 8 bits =
  // 256 x 1KB = 256KB = the full width of chrsel, so the vector is loss-less.
  function [7:0] chr_win_of;
    input [2:0] k;
    reg [2:0] j;
    begin
      j = {k[2] ^ chr_a12_invert, k[1], k[0]};
      if (!j[2]) chr_win_of = {(j[1] ? chr_bank_1 : chr_bank_0), j[0]};
      else case (j[1:0])
        2'd0: chr_win_of = chr_bank_2;
        2'd1: chr_win_of = chr_bank_3;
        2'd2: chr_win_of = chr_bank_4;
        2'd3: chr_win_of = chr_bank_5;
      endcase
    end
  endfunction

  reg [7:0]  chrsel;
  reg [63:0] snap_win_r;
  // EXPLICIT sensitivity list, not @*: the right-hand sides are FUNCTION CALLS,
  // and whether @* looks through a function body at the variables it reads is
  // exactly the kind of tool-dependent corner that costs a debugging session
  // (Icarus left the vector at X). Listing the six bank registers + the invert
  // bit + chr_ain is complete by construction -- chr_win_of reads nothing else.
  always @(chr_ain or chr_a12_invert or chr_bank_0 or chr_bank_1 or chr_bank_2
           or chr_bank_3 or chr_bank_4 or chr_bank_5) begin
    chrsel     = chr_win_of(chr_ain[12:10]);
    // sd2snes bridge tap: the whole window vector (see the port comment).
    snap_win_r = {chr_win_of(3'd7), chr_win_of(3'd6),
                  chr_win_of(3'd5), chr_win_of(3'd4),
                  chr_win_of(3'd3), chr_win_of(3'd2),
                  chr_win_of(3'd1), chr_win_of(3'd0)};
  end
  assign snap_chr_win = snap_win_r;

  wire [21:0] prg_aout_tmp = {3'b00_0,  prgsel, prg_ain[12:0]};

  assign {chr_allow, chr_aout} = {flags[15], 4'b10_00, chrsel, chr_ain[9:0]};

  // PRG-RAM window: same CART-RAM mapping MMC1/Mapper28 use (PSRAM 0x3C0000).
  // `ram_enable_eff` is the deviation documented at the top of this module;
  // `ram_protect` is honored as upstream does.
  assign prg_is_ram = prg_ain >= 'h6000 && prg_ain < 'h8000 && ram_enable_eff && !(ram_protect && prg_write);
  assign prg_allow = prg_ain[15] && !prg_write || prg_is_ram;
  wire [21:0] prg_ram = {9'b11_1100_000, prg_ain[12:0]};
  assign prg_aout = prg_is_ram ? prg_ram : prg_aout_tmp;
  assign vram_a10 = mirroring ? chr_ain[11] : chr_ain[10];
  assign vram_ce = chr_ain[13];

  // sd2snes bridge tap (v2.0a molde): MMC3 mirroring is DYNAMIC ($A000 bit 0)
  // and the iNES header bit does not reflect it, so nt_snap_arr must come from
  // here. Encoding: {1'b1, mirroring} -> 2'b10 = vertical, 2'b11 = horizontal,
  // which is exactly what vram_a10 decodes above and what ntarr_of() expects.
  assign snap_mirror = {1'b1, mirroring};
endmodule

module MultiMapper(input clk, input ce, input ppu_ce, input reset,
                   input [19:0] ppuflags,                           // Misc flags from PPU for MMC5 cheating (unused, kept for interface compat)
                   input [31:0] flags,                              // Misc flags from ines header {prg_size(3), chr_size(3), mapper(8)}
                   input [15:0] prg_ain, output reg [21:0] prg_aout,// PRG Input / Output Address Lines
                   input prg_read, prg_write,                       // PRG Read / write signals
                   input [7:0] prg_din, output reg [7:0] prg_dout,  // PRG Data
                   input [7:0] prg_from_ram,                        // PRG Data from RAM (unused by MMC1/Mapper28/MMC0)
                   output reg prg_allow,                            // PRG Allow write access
                   input chr_read,                                  // Read from CHR
                   input [13:0] chr_ain, output reg [21:0] chr_aout,// CHR Input / Output Address Lines
                   output reg [7:0] chr_dout,                       // Value to override CHR data with
                   output reg has_chr_dout,                         // True if CHR data should be overridden
                   output reg chr_allow,                            // CHR Allow write
                   output reg vram_a10,                             // CHR Value for A10 address line
                   output reg vram_ce,                              // CHR True if the address should be routed to the internal 2kB VRAM.
                   output reg irq,
                   // sd2snes video-bridge CHR-bank snapshot (molde dbg_cpu:
                   // registered in-core, threaded out through nes.v).  Encodes
                   // the SAME per-slot state bridge_sim/mappers.py::chr_slots()
                   // reports (the golden's CMD_CHR_BANK source):
                   //   MMC1 8KB mode: slot0 = chr_bank_0>>1, no slot1;
                   //   MMC1 4KB mode: slot0 = chr_bank_0, slot1 = chr_bank_1;
                   //   Mapper28 family (0/2/3/7/28): slot0 = a53chr masked by
                   //     the CHR size (the simulator masks in cpu_write --
                   //     chr_mask[1:0] is that same mask), no slot1.
                   // slot0 is ALWAYS present (every mapper reports it): the
                   // consumer ties s0_present=1.  Registered under ce (cart
                   // cycle) -> NES:core multicycle covers the shallow cones.
                   output reg        chr_snap_s1_present,
                   output reg [7:0]  chr_snap_s0_bank,
                   output reg [7:0]  chr_snap_s1_bank,
                   // sd2snes video-bridge CHR WINDOW VECTOR snapshot (protocol
                   // v2.5, CMD_CHR_STATE8 $14) -- the MMC3 successor to the
                   // slot0/slot1 pair above.  Window k (= the 1KB slice of the
                   // PPU's 8KB CHR view starting at k*1024) in [k*8 +: 8], SIZE
                   // MASKED here exactly like every other tap masks (the
                   // Mapper28 arm's `& chr_mask[1:0]` is the precedent), so the
                   // byte is the physical 1KB bank index the PPU really fetches.
                   // Lockstep with bridge_sim/mappers.py::Mapper4.chr_windows().
                   //   chr_snap_win_en    = this mapper publishes a window
                   //                        vector (mapper 4 ONLY -- the bridge
                   //                        uses it as the emission gate for
                   //                        $14/$15)
                   //   chr_snap_win_flags = the $14 flags BYTE, verbatim.
                   //     bit0     = CHR-RAM (mapper_flags[15]): the renderer
                   //                picks the window SOURCE from it (CHR-ROM
                   //                pre-converted in PSRAM vs the converted
                   //                CHR-RAM mirror in WRAM, design SS5.2).
                   //     bits 7:1 = RESERVED, must stay 0.
                   //   LOCKSTEP with bridge_sim/mailbox.py CHR8_FLAG_CHR_RAM --
                   //   the design fixes the SIZE of this byte, not its bits, so
                   //   this is the one field where RTL and golden had to agree
                   //   by hand.  It is a BYTE, not a bit, precisely so a new
                   //   flag never costs a port again.
                   // Registered under ce (cart cycle), same shallow-cone contract
                   // as chr_snap_* -> NES:core multicycle covers it.
                   output reg [63:0] chr_snap_win,
                   output reg [7:0]  chr_snap_win_flags,
                   output reg        chr_snap_win_en,
                   // sd2snes video-bridge NT-arrangement snapshot (v2.0a).
                   // Registered (molde chr_snap_*): the protocol NTARR code
                   // (FRAME_HDR.flags[5:4]) derived from the ACTIVE mapper's
                   // DYNAMIC mirror control.  Replaces the static
                   // mapper_flags[14] the nes_wrap used to feed snap_ntarr --
                   // MMC1/AxROM change mirroring at runtime and the iNES flag
                   // does not reflect it (proven: metroid golden = 1794 V + 6
                   // 1A frames on a header-`h` cart).  Encodes exactly what
                   // bridge_sim/mailbox.py NTARR_BY_MODE[mapper.mirror_mode]
                   // reports (the golden's FRAME_HDR ntarr source).
                   output reg [1:0]  nt_snap_arr);
  // Raw mirror control (00=1scr-lo,01=1scr-hi,10=V,11=H) -> protocol NTARR
  // (H=0,V=1,1A/lo=2,1B/hi=3); lockstep with mappers.py MIRROR_TABLE ->
  // NTARR_BY_MODE.  Same field vram_a10 decodes in each mapper.
  function [1:0] ntarr_of;
    input [1:0] mc;
    case (mc)
      2'b00: ntarr_of = 2'd2;   // one-screen lower  -> 1A
      2'b01: ntarr_of = 2'd3;   // one-screen upper  -> 1B
      2'b10: ntarr_of = 2'd1;   // vertical          -> V
      2'b11: ntarr_of = 2'd0;   // horizontal        -> H
    endcase
  endfunction

  wire mmc0_prg_allow, mmc0_vram_a10, mmc0_vram_ce, mmc0_chr_allow;
  wire [21:0] mmc0_prg_addr, mmc0_chr_addr;
  MMC0 mmc0(clk, ce, flags, prg_ain, mmc0_prg_addr, prg_read, prg_write, prg_din, mmc0_prg_allow,
                            chr_ain, mmc0_chr_addr, mmc0_chr_allow, mmc0_vram_a10, mmc0_vram_ce);

  wire mmc1_prg_allow, mmc1_vram_a10, mmc1_vram_ce, mmc1_chr_allow;
  wire [21:0] mmc1_prg_addr, mmc1_chr_addr;
  wire [4:0] mmc1_snap_bank0, mmc1_snap_bank1;
  wire mmc1_snap_4k;
  wire [1:0] mmc1_snap_mirror;
  MMC1 mmc1(clk, ce, reset, flags, prg_ain, mmc1_prg_addr, prg_read, prg_write, prg_din, mmc1_prg_allow,
                                   chr_ain, mmc1_chr_addr, mmc1_chr_allow, mmc1_vram_a10, mmc1_vram_ce,
                                   mmc1_snap_bank0, mmc1_snap_bank1, mmc1_snap_4k, mmc1_snap_mirror);

  wire map28_prg_allow, map28_vram_a10, map28_vram_ce, map28_chr_allow;
  wire [21:0] map28_prg_addr, map28_chr_addr;
  wire [1:0] map28_snap_a53chr;
  wire [1:0] map28_snap_mirror;
  Mapper28 map28(clk, ce, reset, flags, prg_ain, map28_prg_addr, prg_read, prg_write, prg_din, map28_prg_allow,
                                        chr_ain, map28_chr_addr, map28_chr_allow, map28_vram_a10, map28_vram_ce,
                                        map28_snap_a53chr, map28_snap_mirror);

  // MMC3 (mapper 4) -- NOTE the 2nd port: `ppu_ce`, not `ce`. This is the only
  // mapper here clocked per PPU dot (its A12 scanline counter needs it); see the
  // CLOCKING note in module MMC3. Same instantiation upstream uses
  // (fpganes/src/mmu.v:1592).
  wire mmc3_prg_allow, mmc3_vram_a10, mmc3_vram_ce, mmc3_chr_allow, mmc3_irq;
  wire [21:0] mmc3_prg_addr, mmc3_chr_addr;
  wire [1:0] mmc3_snap_mirror;
  wire [63:0] mmc3_snap_win;
  MMC3 mmc3(clk, ppu_ce, reset, flags, prg_ain, mmc3_prg_addr, prg_read, prg_write, prg_din, mmc3_prg_allow,
                                       chr_ain, mmc3_chr_addr, mmc3_chr_allow, mmc3_vram_a10, mmc3_vram_ce,
                                       mmc3_irq, mmc3_snap_mirror, mmc3_snap_win);

  // Mask
  reg [5:0] prg_mask;
  reg [6:0] chr_mask;
  integer wi;   // loop var of the CHR window-vector mask (elaboration-time)

  always @* begin
    case(flags[10:8])
    0: prg_mask = 6'b000000;
    1: prg_mask = 6'b000001;
    2: prg_mask = 6'b000011;
    3: prg_mask = 6'b000111;
    4: prg_mask = 6'b001111;
    5: prg_mask = 6'b011111;
    default: prg_mask = 6'b111111;
    endcase

    case(flags[13:11])
    0: chr_mask = 7'b0000000;
    1: chr_mask = 7'b0000001;
    2: chr_mask = 7'b0000011;
    3: chr_mask = 7'b0000111;
    4: chr_mask = 7'b0001111;
    5: chr_mask = 7'b0011111;
    6: chr_mask = 7'b0111111;
    7: chr_mask = 7'b1111111;
    endcase

    irq = 0;
    prg_dout = 8'hff;
    has_chr_dout = 0;
    chr_dout = 8'h00;

    case(flags[7:0])
    1:  {prg_aout, prg_allow, chr_aout, vram_a10, vram_ce, chr_allow}      = {mmc1_prg_addr, mmc1_prg_allow, mmc1_chr_addr, mmc1_vram_a10, mmc1_vram_ce, mmc1_chr_allow};

    // MMC3 is the only arm that also drives `irq` (upstream mmu.v:1706 does the
    // same, sharing the arm with 47/118/119 -- pruned here). Every other arm
    // leaves the `irq = 0` default above untouched.
    4:  {prg_aout, prg_allow, chr_aout, vram_a10, vram_ce, chr_allow, irq}  = {mmc3_prg_addr, mmc3_prg_allow, mmc3_chr_addr, mmc3_vram_a10, mmc3_vram_ce, mmc3_chr_allow, mmc3_irq};

    0,
    2,
    3,
    7,
    28: {prg_aout, prg_allow, chr_aout, vram_a10, vram_ce, chr_allow}      = {map28_prg_addr, map28_prg_allow, map28_chr_addr, map28_vram_a10, map28_vram_ce, map28_chr_allow};

    default: {prg_aout, prg_allow, chr_aout, vram_a10, vram_ce, chr_allow} = {mmc0_prg_addr, mmc0_prg_allow, mmc0_chr_addr, mmc0_vram_a10, mmc0_vram_ce, mmc0_chr_allow};
    endcase
    if (prg_aout[21:20] == 2'b00)
      prg_aout[19:0] = {prg_aout[19:14] & prg_mask, prg_aout[13:0]};
    if (chr_aout[21:20] == 2'b10)
      chr_aout[19:0] = {chr_aout[19:13] & chr_mask, chr_aout[12:0]};
    // Remap the CHR address into VRAM, if needed.
    chr_aout = vram_ce ? {11'b11_0000_0000_0, vram_a10, chr_ain[9:0]} : chr_aout;
    prg_aout = (prg_ain < 'h2000) ? {11'b11_1000_0000_0, prg_ain[10:0]} : prg_aout;
    prg_allow = prg_allow || (prg_ain < 'h2000);
  end

  // sd2snes video-bridge CHR-bank snapshot (see port comment).  Registered
  // output stage: source cones are pure mapper registers + the quasi-static
  // chr_mask decode of flags -- shallow, and the destinations are ce-gated
  // (NES:core multicycle umbrella).
  always @(posedge clk) begin
    if (ce) begin
      if (flags[7:0] == 8'd1) begin  // MMC1
        chr_snap_s1_present <= mmc1_snap_4k;
        chr_snap_s0_bank    <= mmc1_snap_4k ? {3'b000, mmc1_snap_bank0}
                                            : {4'b0000, mmc1_snap_bank0[4:1]};
        chr_snap_s1_bank    <= {3'b000, mmc1_snap_bank1};
      end else if (flags[7:0] == 8'd4) begin  // MMC3 -- CONSTANT sentinel, ON PURPOSE
        // MMC3's CHR view is a vector of EIGHT 1KB windows, which the slot0/slot1
        // pair this tap encodes (CMD_CHR_STATE $12) cannot represent. Mapper 4
        // publishes it through chr_snap_win / CMD_CHR_STATE8 $14 instead, and the
        // legacy pair stays a CONSTANT, DEFINED sentinel. That constant is not a
        // placeholder -- it is the MECHANISM behind two protocol rules:
        //   1. the per-frame $12 carries an inert value, which is exactly what
        //      the contract asks for ("the renderer ignores the $12 when it saw
        //      the $14"; $12 only keeps its fixed offset 14 so the parser does
        //      not break);
        //   2. because the tap NEVER changes, nes_chrsplit_capture can never see
        //      a mid-display bank change, so its cnt stays 1 and CMD_CHR_SPLITS
        //      $13 is STRUCTURALLY unreachable in mapper 4 -- the hard rule "$13
        //      must not appear in mapper 4", enforced by construction rather than
        //      by a gate that could be wired wrong.  (nes_bridge ALSO orders its
        //      state chain so the $13 branch is not reachable when the window
        //      vector is enabled: belt and braces, zero extra logic.)
        // Written out explicitly instead of falling through to the Mapper28 arm:
        // a53chr does happen to stay 0 for mapper 4 (Mapper28's selreg resets to
        // 1, so only `inner`/`mode` ever get written), but relying on another
        // mapper's incidental state would be a trap.
        chr_snap_s1_present <= 1'b0;
        chr_snap_s0_bank    <= 8'd0;
        chr_snap_s1_bank    <= 8'd0;
      end else begin                 // Mapper28 family (0/2/3/7/28) / MMC0
        chr_snap_s1_present <= 1'b0;
        chr_snap_s0_bank    <= {6'b000000, map28_snap_a53chr & chr_mask[1:0]};
        chr_snap_s1_bank    <= 8'd0;
      end
      // v2.5 CHR window vector (see the port comment).  SIZE MASK applied here,
      // where every other tap masks: the address path does
      // `chr_aout[19:13] & chr_mask` with chr_aout[19:18]=2'b00 and
      // chr_aout[17:10]=chrsel, so the surviving bits of an 8-bit window are
      // {chrsel[7:3] & chr_mask[4:0], chrsel[2:0]} -- e.g. CHR-RAM (class 0,
      // chr_mask=0) confines all eight windows to 0..7 = the single 8KB page,
      // exactly what the PPU really fetches.  The mask AND and the mapper-4
      // gate collapse into ONE LUT level per bit, so publishing a defined zero
      // outside mapper 4 is free (and keeps the tap from carrying MMC3
      // registers that some OTHER mapper's writes happened to move).
      if (flags[7:0] == 8'd4) begin
        for (wi=0; wi<8; wi=wi+1)
          chr_snap_win[wi*8 +: 8] <= {mmc3_snap_win[wi*8+3 +: 5] & chr_mask[4:0],
                                      mmc3_snap_win[wi*8 +: 3]};
        chr_snap_win_flags <= {7'd0, flags[15]};   // bit0 = CHR-RAM
        chr_snap_win_en    <= 1'b1;
      end else begin
        chr_snap_win       <= 64'd0;
        chr_snap_win_flags <= 8'd0;
        chr_snap_win_en    <= 1'b0;
      end
      // NT arrangement (v2.0a): from the ACTIVE mapper's dynamic mirror control.
      // Same ce-registration + shallow-cone contract as chr_snap_* (main.sdc
      // {*|NES:core|*} multicycle covers it).  True default (mappers outside the
      // v0 set) is dead -- those are rejected NOIMPL at load; fall back to the
      // static header mirror there.
      case (flags[7:0])
        8'd1:                       nt_snap_arr <= ntarr_of(mmc1_snap_mirror);
        // MMC3 owns mirroring dynamically through $A000 and IGNORES the iNES
        // header bit, so the static fallback below would be wrong for it.
        8'd4:                       nt_snap_arr <= ntarr_of(mmc3_snap_mirror);
        8'd0, 8'd2, 8'd3, 8'd7, 8'd28: nt_snap_arr <= ntarr_of(map28_snap_mirror);
        default:                    nt_snap_arr <= flags[14] ? 2'd1 : 2'd0;
      endcase
    end
  end
endmodule

// PRG       = 0....
// CHR       = 10...
// CHR-VRAM  = 1100
// CPU-RAM   = 1110
// CARTRAM   = 1111
