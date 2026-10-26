// SPDX-License-Identifier: GPL-2.0-or-later
//
// tia_hc.v -- horizontal timing of the TIA as the original chip builds it: a 6-bit polynomial counter with a two-phase
// clock, and the 4-bit "motion" counter that drives the HMOVE.  Passo 33, replaces the binary hcount logic of tia.v.
//
// Written from public documentation only (docs/tia_spec.md, sections 1, 2 and 5); every rule carries its source.
//
// Time base: `cc_ce` is one colour clock (CLK).  A "count" of the polynomial counter is 4 CLK.
//
//   ph    0..3: CLK inside the current count.  hcount (as numbered by tia.v) = 4 * count + ph.
//   cnt   polynomial counter state during the current count.
//   hb    HBLANK latch (1 = blank).  hb = 1 for hcount 0..67, or 0..75 when the HMOVE latch is set.
//
`timescale 1ns / 1ps

`ifndef TIA_PD
`define TIA_PD 3
`endif
module tia_hc #(
  // HMOVE strobed on the last count of the line (CPU cycle 74).  FONTE: medida, out/passo33.txt (decision of the project owner, 2026-10-20):
  // the three reference models of the bench (ALE, current Stella, MiSTer TIA as black box) and the game Daring Do versus the Everfree Forest
  // agree that the object moves by its nominal HMxx value.  HM74 = 1 (default) does this: the HBLANK seen by the object counters is
  // extended by 8 CLK on the next line, without the visible comb bar.  HM74 = 0 is the literal reading of the Towers sentence "(8 + value)
  // pixels to the left" ("Playing with the HMOVE registers", quoted in docs/tia_spec.md sec. 5); kept only to reproduce old bench runs.
  parameter HM74 = 1,
  // HMVIS = 1: every HMOVE strobed after the HBLANK has ended (hb = 0) also extends, for the object counters only, the HBLANK of the next
  // line by 8 CLK (see tia_pos: the stuffed clocks that fell in the visible part are replayed there), so a late HMOVE still moves by its
  // nominal value, as in two of the three reference models of the bench.  [ABERTO: real console]
  parameter HMVIS = 0,
  // HMIDLE = 1: when the motion counter has run down to 0 it goes back to 15 and stays there, and an object whose "more motion required" latch
  // was never cleared (a value written to HMxx, or HMCLR, while the counter was running can bypass the stop condition) keeps getting an extra
  // clock every 4 CLK until the next HMOVE.  FONTE: Towers, "Playing with the HMOVE registers" (the Cosmic Ark and Rabbit Transit cases).
  // HMIDLE = 0: the extra clocks stop when the counter reaches 0 (first model).
  parameter HMIDLE = 1
) (
  input            clk,
  input            rst,
  input            cc_ce,
  input            hmove_wr,        // a write to HMOVE lands on this clock (coincides with cc_ce)
  output reg [1:0] ph,
  output reg [5:0] cnt,
  output reg       hb,
  output           hb_nx,           // value the object clock gate (hb, plus the HM74 = 1 extension) takes on the next colour clock
  output           hb_nx2,          // same, two colour clocks from now (lets the object counters start early)
  output reg       hmove_lat,
  // HMOVE motion sequencer (see "HMOVE" in docs/tia_spec.md)
  output reg       hm_tick,         // 1 clock pulse, once every 4 CLK while the motion counter runs
  output reg [3:0] hm_v,            // motion counter value that this tick compares against
  output reg       hm_start         // 1 clock pulse on the HMOVE write: objects set their "more motion" latch
);

  // ---- polynomial counter ---------------------------------------------------------------------------------------
  // FONTE: Towers, "Polynomial Counters, what the heck is this?" and the state table of "Horizontal Sync Counter".
  // HW: forma imposta pelo chip original: o polinomio de realimentacao do contador e' fixo no silicio (novo bit da
  //     esquerda = XNOR dos dois bits menos significativos, deslocamento a direita); a tabela de estados do Towers e'
  //     reproduzida por essa regra, e o estado 111111 e' ilegal e forca o reset.
  function [5:0] pnext;
    input [5:0] s;
    begin
      pnext = {~(s[0] ^ s[1]), s[5:1]};
    end
  endfunction

  // decoded states of the horizontal counter
  // FONTE: Towers, "Horizontal Sync Counter" (decode table: RHB, LRHB, SHB)
  localparam [5:0] ST_RHB  = 6'b011100;   // count 16: reset HBLANK
  localparam [5:0] ST_LRHB = 6'b010111;   // count 18: late reset HBLANK (used when the HMOVE latch is set)
  localparam [5:0] ST_SHB  = 6'b010100;   // count 56: start HBLANK, counter back to 000000
  localparam [5:0] ST_ERR  = 6'b111111;   // illegal state: reset to 000000

  wire end_of_count = (ph == 2'd3);
  // FONTE: medida, out/passo33.txt (Fase 3): the position counters of the objects start counting one CLK before the first visible pixel
  wire hb_bd = (cnt == ST_SHB) ? 1'b1 :
               (cnt == ST_RHB  && !hmove_lat) ? 1'b0 :
               (cnt == ST_LRHB &&  hmove_lat) ? 1'b0 : hb;     // value hb takes at the end of this count
  // HM74 = 1: a HMOVE strobed in the last count of the line extends only the blank seen by the object counters (hbm), not the picture
  reg hmove_lat2;                                               // the late HMOVE is active for the whole next line
  reg late_pend;                                                // the late HMOVE was written before the wrap of this line
  reg hbm;
  wire lat_m  = hmove_lat | hmove_lat2;
  wire hbm_bd = (cnt == ST_SHB) ? 1'b1 :
                (cnt == ST_RHB  && !lat_m) ? 1'b0 :
                (cnt == ST_LRHB &&  lat_m) ? 1'b0 : hbm;
  assign hb_nx  = end_of_count ? hbm_bd : hbm;                  // next colour clock
  assign hb_nx2 = (ph >= 2'd2) ? hbm_bd : hbm;                  // two colour clocks ahead

  always @(posedge clk) begin
    if (rst) begin
      ph <= 2'd0; cnt <= 6'd0; hb <= 1'b1; hmove_lat <= 1'b0; hmove_lat2 <= 1'b0; late_pend <= 1'b0; hbm <= 1'b1;
    end else begin
      if (cc_ce) begin
        ph <= ph + 2'd1;
        if (end_of_count) begin
          // the decode of the state of the count that ends here is latched into the control lines "4 CLK later",
          // i.e. it takes effect on the first CLK of the next count
          // FONTE: Towers, "Horizontal Sync Counter" (decodes are clocked into other logic during the next H@1-H@2 cycle)
          if (cnt == ST_SHB || cnt == ST_ERR) cnt <= 6'd0; else cnt <= pnext(cnt);
          hbm <= hbm_bd;
          if (cnt == ST_SHB) begin
            hmove_lat2 <= late_pend; late_pend <= 1'b0;
            hb        <= 1'b1;            // FONTE: Towers, "Horizontal Sync Counter" (SHB: turn HBLANK on, reset the counter)
            hmove_lat <= 1'b0;            // FONTE: Towers, "Playing with the HMOVE registers" (the latch is cleared when the HSync counter wraps)
          end
          // FONTE: Towers, "Playing with the HMOVE registers": the HBLANK latch is reset by RHB, or by the late decode LRHB when [HMOVE] is set
          if (cnt == ST_RHB  && !hmove_lat) hb <= 1'b0;
          if (cnt == ST_LRHB &&  hmove_lat) hb <= 1'b0;
        end
      end
      // a write to HMOVE on this clock sets the latch for the NEXT clock; set after the wrap clear above (same order as tia.v)
      if (hmove_wr) hmove_lat <= 1'b1;
      if (hmove_wr && ((HM74 != 0 && cnt == ST_SHB) || (HMVIS != 0 && !hb))) begin
        if (cc_ce && end_of_count && cnt == ST_SHB) hmove_lat2 <= 1'b1; else late_pend <= 1'b1;
      end
    end
  end

  // ---- HMOVE motion counter ---------------------------------------------------------------------------------------
  // FONTE: Towers, "Playing with the HMOVE registers": a 4-bit binary counter that starts at 15 and decrements every
  // 4 CLK during the HMOVE; the first comparison (against 15) is sampled 15 CLK after the STA HMOVE begins and every
  // 4 CLK after that; the first decrement is at CLK 17.  The write lands on the last CLK of the 3-cycle STA, i.e. 8 CLK
  // after it begins, so the first comparison is 7 CLK after the write.
  parameter [3:0] HM_FIRST = 4'd8 - `TIA_PD;        // CLK from the HMOVE write to the first comparison (calibrated, see the test bench)

  reg [3:0] hm_cd;                        // CLK to the next comparison
  reg       hm_run;
  reg       hm_idle;                      // the counter has run down and sits at 15 (HMIDLE = 1)
  always @(posedge clk) begin
    if (rst) begin
      hm_tick <= 1'b0; hm_v <= 4'd15; hm_start <= 1'b0; hm_cd <= 4'd0; hm_run <= 1'b0; hm_idle <= 1'b0;
    end else begin
      if (cc_ce) hm_start <= 1'b0;        // stays high until the next colour clock, where the objects read it
      if (hmove_wr) begin
        hm_start <= 1'b1; hm_run <= 1'b1; hm_idle <= 1'b0; hm_v <= 4'd15; hm_cd <= HM_FIRST - 4'd1; hm_tick <= 1'b0;
      end else if (cc_ce) begin
        hm_tick <= 1'b0;
        if (hm_tick) begin                // the comparison of the previous tick is done: next value
          if (hm_idle) ;
          else if (hm_v == 4'd0) begin
            if (HMIDLE != 0) begin hm_v <= 4'd15; hm_idle <= 1'b1; end else hm_run <= 1'b0;
          end else hm_v <= hm_v - 4'd1;
        end
        if (hm_run) begin
          if (hm_cd == 4'd0) begin hm_tick <= 1'b1; hm_cd <= 4'd3; end
          else hm_cd <= hm_cd - 4'd1;
        end
      end
    end
  end

endmodule
