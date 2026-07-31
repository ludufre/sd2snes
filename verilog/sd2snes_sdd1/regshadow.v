`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// Company: sd2snes
// Module Name: regshadow
// Description:
//   Small write-only register shadow for the in-game cheat overlay.  The overlay
//   restores the SNES PPU ($2100-$213F) and CPU ($4200-$421F) registers from a
//   shadow of the last value(s) written to each, read back through the NMI hook's
//   identity window as PSRAM:
//     $F90500-$F9057F : PPU regs, stride-2 words (low byte = 1st write,
//                       high byte = 2nd write -- see the double-write note below)
//     $F90700-$F9071F : CPU $42xx regs, stride-1 bytes
//   On the base/DSP/SA-1 mk3 cores that shadow is a by-product of the full ctx.v
//   PSRAM mirror.  The coprocessor cores that only run the overlay (no full
//   savestate) don't carry ctx.v, so this replaces just the one piece the overlay
//   needs with a single RAMB16 (block RAM is plentiful; LUTs are the scarce
//   resource, especially on the mk2 Spartan-3).
//
//   DOUBLE-WRITE registers: the scroll regs ($210D-$2114) and the mode-7 regs
//   ($211B-$2120) latch a 16-bit quantity from TWO consecutive 8-bit writes, so a
//   1-deep shadow cannot restore them -- replaying the last byte twice yields
//   (high,high) and wrecks the scroll (Star Fox loses its background).  Same
//   scheme as ctx.v on the base core: keep the previous byte written to ANY
//   BG-double reg (rBG) and to ANY M7-double reg (rM7), and store the PAIR
//   (1st write, 2nd write) on every PPU register write.  BG has priority over M7
//   because $210D/$210E belong to both sets (both trackers still update).
//   Non-double regs store (value, value) so the serve stays uniform.
//   The M7 half is compile-gated -- see REGSHADOW_NO_M7 at the trackers below.
//
//   Storage layout (256x8, one RAMB16):
//     mem[0x00-0x7F] : PPU reg pairs -- mem[{PA,1'b0}] = 1st write (prev byte),
//                      mem[{PA,1'b1}] = 2nd write (current byte)
//     mem[0x80-0x9F] : CPU reg       ($4200-$421F)
//   ...and under REGSHADOW_1DEEP (see below) the pre-fix layout instead:
//     mem[0x00-0x3F] : PPU reg, last byte only
//     mem[0x40-0x5F] : CPU reg     ($4200-$421F)
//   The serve side in main.v indexes the SAME two layouts and is gated on the same
//   macro -- they MUST be changed together or the overlay reads the wrong cells.
//
//   TWO compile gates, mutually exclusive (a guard below fails the build if both
//   are defined).  Both exist only because the mk2 Spartan-3 runs out of room:
//     REGSHADOW_NO_M7  -- drop the mode-7 tracker, keep the scroll pair.
//                         GSU mk2 only; fit by 1 slice, HW-validated on Star Fox.
//     REGSHADOW_1DEEP  -- drop the whole 2-deep scheme, back to the pre-fix shadow.
//                         CX4 mk2 only; see the note at the gate itself.
//
//   Write snoop uses the settled A-bus/PA/data taps + end strobes from main.v.
//   The two bus write sources are mutually exclusive within a SNES cycle (a CPU
//   access is either an A-bus write or a B-bus PA write); a PPU write additionally
//   defers the even (prev) byte of its pair to the next cycle, and the mux below
//   keeps a single write address/enable so XST infers a clean one-write/one-read
//   RAMB16.
//////////////////////////////////////////////////////////////////////////////////
module regshadow(
  input clk,
  // write snoop (settled taps + end strobes from main.v)
  input pawr_end,            // settled rising edge of /PAWR
  input wr_end,              // settled rising edge of /WR
  input [23:0] snes_addr,    // settled A-bus
  input [7:0] snes_pa,       // settled B-bus (PA)
  input [7:0] snes_data,     // settled data tap
  // read-serve side
  input [8:0] rd_addr,       // shadow read index (see mapping above)
  output reg [7:0] rd_data
);

// The two mk2 escape hatches are alternatives, not layers: NO_M7 thins the 2-deep
// scheme, 1DEEP removes it.  Defining both is always a mistake, and a silent one
// (1DEEP wins by construction below), so make it a parse-time error instead -- the
// bare identifier is illegal Verilog in both iverilog and XST, and its name is the
// diagnostic.
`ifdef REGSHADOW_1DEEP
 `ifdef REGSHADOW_NO_M7
   ERROR_REGSHADOW_1DEEP_and_REGSHADOW_NO_M7_are_mutually_exclusive
 `endif
`endif

// Force Block RAM so this costs no LUTs (the whole point on the full mk2 core).
(* ram_style = "block" *) reg [7:0] mem [0:255];

// XST block-RAM template requirement: ONE write enable and ONE write address
// expression inside the clocked process.  Two conditional writes to different
// computed addresses (the previous form) fall outside the template and XST
// silently builds the memory out of 2048 flip-flops + muxes (~400 LUTs --
// blows the mk2 budget).  So mux enable/address combinationally first, and
// serialize the two bytes of a PPU pair over two cycles (wr2_* below).
// `pawr_end` is wired to the registered ctx-style counter strobe rs_pawr_end_r
// (see main.v): the write byte is sampled at count==4 of the /PAWR-low window
// into rs_data_r, the same calibration the base core's ctx capture uses.  (An
// earlier PAWR_START+raw variant was calibrated against a floating bus and got
// replaced by this scheme after the OE/DIR snoop fix; do not reintroduce it.)
wire       ppu_wr = pawr_end & (snes_pa < 8'h40);
// $4200-$421F in ANY bank with ADDR[22]=0 ($00-$3F and $80-$BF) -- the CPU regs are
// mirrored there and games write them via FastROM banks ($80+); a bank-$00-only
// decode misses those (e.g. Metal Combat writes $4200 from $80 -> $4200 uncaptured).
wire       cpu_wr = wr_end & ~snes_addr[22]
                           & (snes_addr[15:5] == 11'b01000010000);

`ifdef REGSHADOW_1DEEP
// ---------------------------------------------------------------------------
// REGSHADOW_1DEEP: the pre-fix 1-deep shadow, verbatim.  Defined for the CX4 mk2
// build ONLY (see "Verilog Macros" in sd2snes_cx4.xise).
//
// Why: the 2-deep netlist does not make timing on the CX4 mk2 Spartan-3.  Placer
// cost table 3 closed at -2.863ns and parked the savestate load (CS_STATE=01, black
// screen, 100% reproducible); a sweep of 8 cost tables put the best at CT11
// (-2.096ns) and that parked too.  The old "-2.0 passes" threshold was a property of
// the OLD netlist, not a portable limit, so there is no table to chase.  This form
// is the one proven in silicon (CT3, -2.027ns, save+load validated).
//
// What it costs: nothing in practice on this core.  The CX4 library is exactly two
// games (Mega Man X2 / X3), and both rewrite scroll and mode-7 every frame, so a
// stale double-write pair is overwritten before it can be seen -- the fix has no
// beneficiary here.  CX4 mk3 and every other core keep the 2-deep scheme.
wire       wr_en  = ppu_wr | cpu_wr;
wire [7:0] wr_a   = ppu_wr ? {2'b00, snes_pa[5:0]}
                           : {3'b010, snes_addr[4:0]};

// Single write port + synchronous read = clean one-RAMB16 inference.
always @(posedge clk) begin
  if (wr_en)
    mem[wr_a] <= snes_data;
  rd_data <= mem[rd_addr[7:0]];
end
`else
// Previous-byte trackers -- faithful port of ctx.v's rBG/rM7 on the base core.
// Both are consumed before being updated (non-blocking, same cycle), exactly like
// ctx.v assembling DATA from rBG/rM7 while assigning rBG/rM7 <= SNES_DATA_IN.
//
// REGSHADOW_NO_M7 drops the mode-7 tracker (rM7) and keeps only the scroll one
// (rBG).  It is defined for the GSU mk2 build ONLY -- see the "Verilog Macros"
// property in sd2snes_gsu.xise -- where the Spartan-3 came out 9 slices OVERMAPPED
// (3593/3584) with the full 2-deep shadow, with Optimization Goal already at Area
// and the $E8 savestate window a live mk2 feature that cannot be cut.  Dropping rM7
// sends $211B-$2120 back to the old (v,v) 1-deep behaviour -- i.e. exactly what has
// shipped until now: the SuperFX library sets the mode-7 matrix once (if at all)
// instead of streaming it per frame, so a stale M7 pair costs nothing in practice.
// The pair Star Fox actually needs is SCROLL ($210D-$2114), and that stays.
// Undefined everywhere else (every mk3 core INCLUDING GSU mk3, and cx4/obc1/sdd1 on
// both targets), where the preprocessed source is identical to the ungated version.
reg [7:0]  prev_bg;   initial prev_bg = 0;
`ifndef REGSHADOW_NO_M7
reg [7:0]  prev_m7;   initial prev_m7 = 0;
`endif
wire is_bg_dbl = (snes_pa >= 8'h0D) && (snes_pa <= 8'h14);
`ifndef REGSHADOW_NO_M7
wire is_m7_dbl = ((snes_pa >= 8'h0D) && (snes_pa <= 8'h0E))
              || ((snes_pa >= 8'h1B) && (snes_pa <= 8'h20));
`endif

// Serialized pair write: the strobe cycle stores the CURRENT byte (odd offset) and
// the next cycle stores the PREV byte (even offset) out of these defer flops.
// wr2_pend x (ppu_wr|cpu_wr) can never collide: consecutive SNES bus write strobes
// are dozens of module clocks apart (CLK2 here, CLK96 on the CX4 core), far more
// than the 2 cycles this defer occupies.  Priority: wr2_pend > ppu_wr > cpu_wr.
reg        wr2_pend;  initial wr2_pend = 0;
reg [7:0]  wr2_a;     initial wr2_a = 0;
reg [7:0]  wr2_d;     initial wr2_d = 0;

wire       wr_en  = ppu_wr | cpu_wr | wr2_pend;
wire [7:0] wr_a   = wr2_pend ? wr2_a
                  : ppu_wr   ? {1'b0, snes_pa[5:0], 1'b1}
                             : {3'b100, snes_addr[4:0]};
wire [7:0] wr_d   = wr2_pend ? wr2_d : snes_data;

// Single write port + synchronous read = clean one-RAMB16 inference.
//
// STROBE-WIDTH CONTRACT: `pawr_end` MUST be a one-cycle pulse per PPU write.  All
// four cores build it that way (the ctx-style counter asserts `cnt==4` for exactly
// one cycle, then resets), but the arm below is edge-guarded so that invariant is
// enforced instead of merely assumed:
//   * `~wr2_pend` blocks re-arming while a defer is in flight.  Unguarded, cycle 2
//     of a 2-cycle strobe would re-arm with prev_bg/prev_m7 ALREADY updated to the
//     current byte, so the even slot would get (value) instead of (prev) -- silently
//     degrading the pair back to the old 1-deep (v,v) shadow and losing the scroll
//     high byte again (the exact bug this module was rewritten to fix).
//   * the clear waits for `~ppu_wr`, so wr2_pend stays asserted for as long as a
//     wide strobe lasts.  wr2_a/wr2_d are held, so the extra cycles just rewrite the
//     same even byte idempotently; the pair still ends up (prev, current).
// Net effect: a strobe of ANY width >= 1 arms exactly once and yields the correct
// pair, and for the actual 1-cycle strobe the behaviour is bit-identical to the
// unguarded form (arm on cycle 0, deferred write + clear on cycle 1, idle after).
always @(posedge clk) begin
  if (wr_en)
    mem[wr_a] <= wr_d;
  rd_data <= mem[rd_addr[7:0]];
  if (ppu_wr & ~wr2_pend) begin
    wr2_pend <= 1'b1;
    wr2_a    <= {1'b0, snes_pa[5:0], 1'b0};
`ifndef REGSHADOW_NO_M7
    wr2_d    <= is_bg_dbl ? prev_bg : is_m7_dbl ? prev_m7 : snes_data;
`else
    wr2_d    <= is_bg_dbl ? prev_bg : snes_data;
`endif
    if (is_bg_dbl) prev_bg <= snes_data;
`ifndef REGSHADOW_NO_M7
    if (is_m7_dbl) prev_m7 <= snes_data;
`endif
  end else if (~ppu_wr) begin
    wr2_pend <= 1'b0;
  end
end
`endif

endmodule
