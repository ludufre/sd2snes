`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// nes_split_capture -- per-frame multi-scroll (CMD_SPLITS) capture (protocol
// v1.3.1 + v1.4b w-gating).  Extracted VERBATIM from nes_wrap.v's spl_* block so
// it is independently testable (tb/tb_split_capture.v feeds it RAW $2005/$2006
// pairs); nes_wrap now instantiates it.  All state registered; the change test
// compares the live tap against a single "last change" register set, never a
// scan; array writes are 4-deep decodes/shifts.  Mirror of bridge_sim
// ppustate.{note_scanline entry-0 seed, note_split_change}.
//
// v1.4b FIX (SINTOMA 2 -- scanline-straddle residue):  a split entry is now
// finalized only on a COMPLETE $2005/$2006 write pair (`w`==0).  While a pair is
// mid-flight (`w`==1) loopy_t holds an INTERMEDIATE value; sampling it made the
// pair-straddling case emit a phantom intermediate-T entry for ~1 line (~7%/
// split, the device flicker on Excitebike's billboard line 57-58).  Gating the
// change detection on `w`==0 is "coalescence by EVENT", matching the sim, which
// processes writes discretely.  The entry-0 display-start anchor is NOT gated
// (it fires with w==0 at scanline 0 in practice) -- only the change path is.
//////////////////////////////////////////////////////////////////////////////////

module nes_split_capture(
  input         CLK,
  input         RST,
  input         ce,              // core tick (nes_wrap ce_pulse_r)
  input         frame_tick,      // frame close (nes_wrap frame_tick_r)
  input  [8:0]  scanline,        // core scanline
  input  [14:0] loopy_t,         // ppu_tap_loopy_t (scroll intent)
  input  [2:0]  fine_x,          // ppu_tap_fine_x
  input  [7:0]  ppumask,         // ppu_tap_ppumask
  input         w,               // ppu_tap_loopy_w ($2005/$2006 toggle; 0 = pair done)
  output [2:0]  spl_cnt_o,
  output        spl_ovf_o,
  output [31:0] spl_sl_flat,
  output [59:0] spl_t_flat,
  output [11:0] spl_fx_flat
);
  reg [7:0]  spl_sl [0:3];
  reg [14:0] spl_t  [0:3];
  reg [2:0]  spl_fx [0:3];
  reg [2:0]  spl_cnt;
  reg        spl_ovf;
  reg        spl_frozen;
  reg [7:0]  spl_last_sl;     // scanline of the LAST CHANGE (coalesce chain)
  reg [14:0] spl_last_t;
  reg [2:0]  spl_last_fx;
  wire spl_changed = (loopy_t != spl_last_t) | (fine_x != spl_last_fx);
  // RENDERING GATE (ppumask BG|OBJ): a T/fine_x change while rendering is OFF is
  // not a visible scroll split (e.g. DK streams a nametable via $2006 at the top
  // of the frame with ppumask=0x06).  Keeps split-less games at cnt<=1.
  wire spl_render = ppumask[3] | ppumask[4];
  // v1.4b: only finalize a change on a COMPLETE write pair (w==0) -- the
  // intermediate-T of a mid-flight $2005/$2006 pair (w==1) is never an entry.
  wire spl_do_change = spl_changed & ~w;
  // eviction cones (consumed ONLY in the cnt==4 overflow branch; reg-to-reg,
  // 8-bit subtract + compare tree, single-cycle at CLK2 with room to spare)
  wire [7:0] spl_sl_now = scanline[7:0];
  wire [7:0] spl_d1 = spl_sl[2] - spl_sl[1];
  wire [7:0] spl_d2 = spl_sl[3] - spl_sl[2];
  wire [7:0] spl_d3 = spl_sl_now - spl_sl[3];
  wire [7:0] spl_dn = 8'd240 - spl_sl_now;
  wire spl_ev1 = (spl_d1 <= spl_d2) && (spl_d1 <= spl_d3) && (spl_d1 <= spl_dn);
  wire spl_ev2 = !spl_ev1 && (spl_d2 <= spl_d3) && (spl_d2 <= spl_dn);
  wire spl_ev3 = !spl_ev1 && !spl_ev2 && (spl_d3 <= spl_dn);
  always @(posedge CLK) begin
    if (RST) begin
      spl_cnt<=3'd1; spl_ovf<=1'b0; spl_frozen<=1'b0;
      spl_sl[0]<=8'd0; spl_t[0]<=15'd0; spl_fx[0]<=3'd0;
      spl_last_sl<=8'd0; spl_last_t<=15'd0; spl_last_fx<=3'd0;
    end else if (frame_tick) begin
      // re-seed entry0 (fallback = close-time T) + re-arm capture for next frame
      spl_frozen<=1'b0; spl_ovf<=1'b0; spl_cnt<=3'd1;
      spl_sl[0]<=8'd0; spl_t[0]<=loopy_t; spl_fx[0]<=fine_x;
      spl_last_sl<=8'd0; spl_last_t<=loopy_t; spl_last_fx<=fine_x;
    end else if (ce && scanline <= 9'd239 && spl_render) begin
      if (!spl_frozen) begin
        // entry 0 = display-start state (chain anchored at scanline 0)
        spl_sl[0]<=8'd0; spl_t[0]<=loopy_t; spl_fx[0]<=fine_x;
        spl_cnt<=3'd1; spl_frozen<=1'b1;
        spl_last_sl<=8'd0; spl_last_t<=loopy_t; spl_last_fx<=fine_x;
      end else if (spl_do_change) begin
        if ((spl_sl_now - spl_last_sl) <= 8'd1) begin
          // coalesce (same/adjacent scanline): entry keeps its ORIGINAL
          // scanline; the chain advances so a pair may continue next line
          spl_t[spl_cnt-3'd1]<=loopy_t; spl_fx[spl_cnt-3'd1]<=fine_x;
          spl_last_sl<=spl_sl_now;
          spl_last_t<=loopy_t; spl_last_fx<=fine_x;
        end else if (spl_cnt >= 3'd4) begin
          spl_ovf<=1'b1;
          if (spl_ev1) begin        // evict e1: shift e2/e3 down, new at [3]
            spl_sl[1]<=spl_sl[2]; spl_t[1]<=spl_t[2]; spl_fx[1]<=spl_fx[2];
            spl_sl[2]<=spl_sl[3]; spl_t[2]<=spl_t[3]; spl_fx[2]<=spl_fx[3];
            spl_sl[3]<=spl_sl_now; spl_t[3]<=loopy_t; spl_fx[3]<=fine_x;
            spl_last_sl<=spl_sl_now; spl_last_t<=loopy_t; spl_last_fx<=fine_x;
          end else if (spl_ev2) begin
            spl_sl[2]<=spl_sl[3]; spl_t[2]<=spl_t[3]; spl_fx[2]<=spl_fx[3];
            spl_sl[3]<=spl_sl_now; spl_t[3]<=loopy_t; spl_fx[3]<=fine_x;
            spl_last_sl<=spl_sl_now; spl_last_t<=loopy_t; spl_last_fx<=fine_x;
          end else if (spl_ev3) begin
            spl_sl[3]<=spl_sl_now; spl_t[3]<=loopy_t; spl_fx[3]<=fine_x;
            spl_last_sl<=spl_sl_now; spl_last_t<=loopy_t; spl_last_fx<=fine_x;
          end
          // else: the new entry is the shortest strip -> dropped, chain frozen
        end else begin
          spl_sl[spl_cnt]<=spl_sl_now;
          spl_t[spl_cnt] <=loopy_t;
          spl_fx[spl_cnt]<=fine_x;
          spl_cnt<=spl_cnt+3'd1;
          spl_last_sl<=spl_sl_now; spl_last_t<=loopy_t; spl_last_fx<=fine_x;
        end
      end
    end
  end
  assign spl_sl_flat = {spl_sl[3],spl_sl[2],spl_sl[1],spl_sl[0]};
  assign spl_t_flat  = {spl_t[3], spl_t[2], spl_t[1], spl_t[0]};
  assign spl_fx_flat = {spl_fx[3],spl_fx[2],spl_fx[1],spl_fx[0]};
  assign spl_cnt_o   = spl_cnt;
  assign spl_ovf_o   = spl_ovf;
endmodule
