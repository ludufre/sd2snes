`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// nes_chrwin_capture -- per-frame CHR raster-split over the EIGHT 1KB WINDOW
// VECTOR (CMD_CHR_SPLITS8, opcode 0x15), protocol v2.5 / mapper 4 (MMC3).
//
// STRUCTURAL MIRROR of nes_chrsplit_capture.v (which is itself the mirror of
// nes_split_capture.v): same reset/frame_tick reseed, same entry-0 seed at the
// first rendering ce, same <=1-scanline coalescence CHAIN, same shortest-strip
// eviction with entry 0 NEVER evicted, same rendering gate, same K=4.  The ONLY
// payload difference is the width: 64 bits (8 windows x 8 bits) instead of one
// slot-0 bank byte.  Read that file first -- every rule below has its rationale
// there and the two are meant to stay line-for-line comparable.
//
// WHY A SECOND MODULE INSTEAD OF A MODE BIT
// -------------------------------------------------------------------------------
//   The 8-bit capture must keep behaving EXACTLY as it does today: it is what
//   makes every mapper 0/1/2/3/7/28 frame byte-identical, and its gate
//   (run_chrsplit_capture.sh) is auto-sensitizing against a very specific bug.
//   A width-parameterized single module would have put both payloads through one
//   set of arrays and one comparator, i.e. it would have made a mapper-4 change
//   able to perturb the legacy path.  Two instances cost the same LEs (the
//   arrays are disjoint either way) and cost ZERO risk.  Only ONE of them can
//   ever produce cnt>=2 in a given run: outside mapper 4 the window vector is a
//   CONSTANT 0 (MultiMapper publishes it only for mapper 4), and inside mapper 4
//   the legacy slot0/slot1 tap is a CONSTANT sentinel.  Both directions are
//   inert BY CONSTRUCTION, not by a gate.
//
// COMMAND FORMAT (serialized by nes_bridge.v, S_CWSP_*):
//   0x15 | hdr(1: bit7=overflow, bits[2:0]=cnt 2..4) | cnt x [scanline(1) win[0..7](8)]
//   Entry 0 is the window vector at DISPLAY START; every entry is valid until
//   the next entry's scanline (the last one until scanline 240).  Every entry is
//   an ABSOLUTE snapshot -- no deltas (protocol design SS3.3: the delta encoding
//   saves ~18 B/frame and breaks the "every entry is absolute" invariant the
//   renderer's resolve is built on).
//
// ADDITIVE GATE (byte-identical goldens): the bridge emits the command ONLY when
//   cnt>=2, and only in mapper 4 at all.  A mapper-4 frame with no mid-display
//   window change leaves cnt==1 and NOTHING is emitted.
//
// NO POISON (delta vs nes_chrsplit_capture) -- deliberate, do not "fix":
//   The 8-bit capture poisons the frame when s1_present flips mid-display,
//   because 8K<->4K changes the MEANING of "slot 0 bank" for the strips already
//   captured.  The window vector has no such mode: every composition of the 8
//   windows is directly interpretable, so there is no event that can invalidate
//   a captured entry.  The poison valve therefore has NO TRIGGER here and is
//   simply absent (protocol design SS3.3).  It stays alive on the $13 path.
//
// NO w-GATING, for the same reason the 8-bit capture has none, only stronger:
//   every MMC3 register is a SINGLE-WRITE latch (bank select $8000 / bank data
//   $8001 / $A000 -- see module MMC3 in mmu.v), so there is no multi-write
//   commit window to gate away; and the tap this module consumes is itself
//   REGISTERED under ce inside MultiMapper.  An MMC3 IRQ handler that rewrites
//   several registers in a row DOES walk through intermediate compositions --
//   and those are REAL: the PPU fetches with them.  When the burst fits inside
//   one scanline pair the coalescence chain folds it into one entry carrying the
//   FINAL vector; when it straddles with a gap >= 2 lines the intermediate
//   composition gets its own entry, which is what the hardware actually showed.
//
// PIPELINED COMPARATOR (the one microarchitectural decision here)
// -------------------------------------------------------------------------------
//   This module is instantiated in nes_wrap, i.e. OUTSIDE `NES:core`, so the
//   main.sdc multicycle `-from {*|NES:core|*} -to {*|NES:core|*}` does NOT cover
//   it and its logic must close at full CLK2 (84 MHz / 11.9 ns).  A 64-bit
//   inequality is a 64-wide XOR followed by a 3-level OR tree -- the only new
//   combinational cone in the whole Phase 2 RTL that lives outside that
//   umbrella.  It is therefore REGISTERED into cwin_chg_r.
//
//   That register is FREE-RUNNING (no ce gate) ON PURPOSE, and this is the
//   invariant to preserve:
//     * both of its inputs (`win` from the mmu tap, `cwin_last_win` here) only
//       ever change ON A ce EDGE;
//     * ce pulses are one PPU DOT apart = ~15 CLK2 cycles in hardware, and >= 2
//       posedges in every testbench driver;
//   so at every ce the flop already carries the combinational result of inputs
//   that have been stable since the previous ce.  The stage is free.
//   GATING IT UNDER ce WOULD BE A BUG, not a nicety: the change flag would then
//   arrive one full ce late, and the captured scanline (and, in a sparse-tick
//   testbench, the captured VECTOR) would shift by one tick.  The tb asserts
//   exact scanlines precisely so that mistake cannot pass.
//
// COST: 4x(8 scanline + 64 window) = 288 FF for the entry arrays, + 64+8 for the
//   "last change" pair, + cnt/ovf/frozen/chg = ~366 FF, ZERO M9K.  The change
//   test is ONE registered compare against a single register (never a scan over
//   the array); array writes are 4-deep decodes/shifts; the eviction cone is
//   consumed ONLY in the cnt==4 branch.  Mirror of the bridge_sim chr-window
//   split bookkeeping.
//////////////////////////////////////////////////////////////////////////////////

module nes_chrwin_capture(
  input          CLK,
  input          RST,
  input          ce,              // core tick (nes_wrap ce_pulse_r)
  input          frame_tick,      // frame close (nes_wrap frame_tick_r)
  input  [8:0]   scanline,        // core scanline
  input  [63:0]  win,             // chr_snap_win (registered under ce in mmu.v)
  input  [7:0]   ppumask,         // ppu_tap_ppumask
  output [2:0]   cwin_cnt_o,
  output         cwin_ovf_o,
  output [31:0]  cwin_sl_flat,    // 4 x scanline[7:0]  (window is 0..239 -> 8 bits)
  output [255:0] cwin_win_flat    // 4 x win[63:0]
);
  reg [7:0]  cwin_sl [0:3];
  reg [63:0] cwin_w  [0:3];
  reg [2:0]  cwin_cnt;
  reg        cwin_ovf;
  reg        cwin_frozen;
  reg [7:0]  cwin_last_sl;    // scanline of the LAST CHANGE (coalesce chain)
  reg [63:0] cwin_last_win;
  // PIPELINE STAGE -- see the header.  Free-running by design; ce pulses are
  // >= 2 CLK2 apart and both inputs only move on a ce, so this is transparent.
  reg        cwin_chg_r;
  always @(posedge CLK) begin
    if (RST) cwin_chg_r <= 1'b0;
    else     cwin_chg_r <= (win != cwin_last_win);
  end
  // RENDERING GATE (ppumask BG|OBJ): a window change while rendering is OFF is
  // not a visible CHR split (games re-bank freely in vblank / forced blank; the
  // frame-close CMD_CHR_STATE8 already carries that).  Keeps non-splitting
  // mapper-4 frames at cnt<=1 so they emit no $15 at all.
  wire cwin_render = ppumask[3] | ppumask[4];
  // eviction cones (consumed ONLY in the cnt==4 overflow branch; reg-to-reg,
  // 8-bit subtract + compare tree -- IDENTICAL to nes_chrsplit_capture, the
  // widened payload does not touch it)
  wire [7:0] cwin_sl_now = scanline[7:0];
  wire [7:0] cwin_d1 = cwin_sl[2] - cwin_sl[1];
  wire [7:0] cwin_d2 = cwin_sl[3] - cwin_sl[2];
  wire [7:0] cwin_d3 = cwin_sl_now - cwin_sl[3];
  wire [7:0] cwin_dn = 8'd240 - cwin_sl_now;
  wire cwin_ev1 = (cwin_d1 <= cwin_d2) && (cwin_d1 <= cwin_d3) && (cwin_d1 <= cwin_dn);
  wire cwin_ev2 = !cwin_ev1 && (cwin_d2 <= cwin_d3) && (cwin_d2 <= cwin_dn);
  wire cwin_ev3 = !cwin_ev1 && !cwin_ev2 && (cwin_d3 <= cwin_dn);
  always @(posedge CLK) begin
    if (RST) begin
      cwin_cnt<=3'd1; cwin_ovf<=1'b0; cwin_frozen<=1'b0;
      cwin_sl[0]<=8'd0; cwin_w[0]<=64'd0;
      cwin_last_sl<=8'd0; cwin_last_win<=64'd0;
    end else if (frame_tick) begin
      // re-seed entry0 (fallback = close-time vector) + re-arm for next frame
      cwin_frozen<=1'b0; cwin_ovf<=1'b0; cwin_cnt<=3'd1;
      cwin_sl[0]<=8'd0; cwin_w[0]<=win;
      cwin_last_sl<=8'd0; cwin_last_win<=win;
    end else if (ce && scanline <= 9'd239 && cwin_render) begin
      if (!cwin_frozen) begin
        // entry 0 = display-start state (chain anchored at scanline 0)
        cwin_sl[0]<=8'd0; cwin_w[0]<=win;
        cwin_cnt<=3'd1; cwin_frozen<=1'b1;
        cwin_last_sl<=8'd0; cwin_last_win<=win;
      end else if (cwin_chg_r) begin
        if ((cwin_sl_now - cwin_last_sl) <= 8'd1) begin
          // coalesce (same/adjacent scanline): entry keeps its ORIGINAL
          // scanline; the chain advances so a burst may continue next line.
          // This is what folds an MMC3 handler's 6-write register burst into a
          // single entry carrying the FINAL composition.
          cwin_w[cwin_cnt-3'd1]<=win;
          cwin_last_sl<=cwin_sl_now; cwin_last_win<=win;
        end else if (cwin_cnt >= 3'd4) begin
          cwin_ovf<=1'b1;
          if (cwin_ev1) begin       // evict e1: shift e2/e3 down, new at [3]
            cwin_sl[1]<=cwin_sl[2]; cwin_w[1]<=cwin_w[2];
            cwin_sl[2]<=cwin_sl[3]; cwin_w[2]<=cwin_w[3];
            cwin_sl[3]<=cwin_sl_now; cwin_w[3]<=win;
            cwin_last_sl<=cwin_sl_now; cwin_last_win<=win;
          end else if (cwin_ev2) begin
            cwin_sl[2]<=cwin_sl[3]; cwin_w[2]<=cwin_w[3];
            cwin_sl[3]<=cwin_sl_now; cwin_w[3]<=win;
            cwin_last_sl<=cwin_sl_now; cwin_last_win<=win;
          end else if (cwin_ev3) begin
            cwin_sl[3]<=cwin_sl_now; cwin_w[3]<=win;
            cwin_last_sl<=cwin_sl_now; cwin_last_win<=win;
          end
          // else: the new entry is the shortest strip -> dropped, chain frozen
        end else begin
          cwin_sl[cwin_cnt]<=cwin_sl_now;
          cwin_w[cwin_cnt]<=win;
          cwin_cnt<=cwin_cnt+3'd1;
          cwin_last_sl<=cwin_sl_now; cwin_last_win<=win;
        end
      end
    end
  end
  assign cwin_sl_flat  = {cwin_sl[3],cwin_sl[2],cwin_sl[1],cwin_sl[0]};
  assign cwin_win_flat = {cwin_w[3], cwin_w[2], cwin_w[1], cwin_w[0]};
  assign cwin_cnt_o    = cwin_cnt;
  assign cwin_ovf_o    = cwin_ovf;
endmodule
