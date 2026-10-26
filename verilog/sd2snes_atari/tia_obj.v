// SPDX-License-Identifier: GPL-2.0-or-later
//
// tia_obj.v -- movable objects of the TIA built the way the original chip does it: each object has its own horizontal
// position counter (a 6-bit polynomial counter plus a 2-bit phase, 40 counts x 4 CLK = 160 CLK per turn) that is
// clocked only while the line is visible, started by a START decode, and moved by extra clock pulses during HMOVE.
// Passo 33.  tia_pos (position counter + HMOVE latch + START decodes), tia_mis (missile and ball) and tia_ply (player);
// docs/tia_spec.md sections 3 to 5.
//
// Written from public documentation only; every rule carries its source (see fpga/PROVENIENCIA_TIA.md).
//
`timescale 1ns / 1ps

// -----------------------------------------------------------------------------------------------------------------
// position counter of one object: 6-bit polynomial counter + 2-bit phase, HMOVE "more motion" latch, START decodes
// -----------------------------------------------------------------------------------------------------------------
`ifndef TIA_PD
`define TIA_PD 3
`endif
module tia_pos #(
  parameter [1:0] RPH = 2'd0,            // phase the 4-CLK clock has right after a reset (calibrated by measurement, see out/passo33.txt)
  // PUPH: phase of the 4-CLK clock after power-up, for an object that is never reset.  The state of a real counter at power-up is arbitrary
  // (no source defines it); the value is chosen so that such an object appears where the reference model of the bench puts it (pixel 0), which
  // is what a game that enables an object it never placed (Mortal Kurling! and the ball) was written and tested against.
  // FONTE: medida, out/passo33.txt (bench powerup.s: missile and ball 2 CLK, player 3 CLK later than pixel 0 with PUPH = 0).
  parameter [1:0] PUPH = 2'd0,
  // HMVIS: what happens to a stuffed HMOVE clock that arrives while the line is visible.
  // 0: it coincides with a normal clock and moves nothing (OR).  FONTE: Towers, "HMOVE during the visible scanline" ("should have no
  //    effect"); the reference models of the bench do the same.
  // 1: it is not lost: it is applied at the next HBLANK, so a late HMOVE still moves the object by its nominal value (two of the three
  //    reference models of the bench do this).  [ABERTO: real console, see out/passo33.txt]
  parameter       HMVIS = 0
) (
  input            clk,
  input            rst,
  input            cc_ce,
  input            motck,                // the position counter is clocked: visible part of the line
  input            reset_wr,             // RESxx written since the last colour clock (acts on this cc_ce)
  input            force_start,          // RESBL: the reset itself sends a START
  input      [2:0] nus,                  // NUSIZ[2:0]: which copies are decoded
  input            hm_start,
  input            hm_tick,
  input      [3:0] hm_v,
  input      [3:0] hmv,                  // HMxx register of this object (bits 7:4 of the written byte)
  output reg [1:0] ph,                   // phase of the two-phase clock
  output reg       st1,                  // START (4 CLK wide)
  output reg       st2,                  // START delayed by 4 CLK
  output           step,                 // clock of everything that hangs on this counter (normal + stuffed pulses)
  output           st_ev,                // pulse: START rises on the next clock
  output           st_main               // same, but only for the main copy (decode 101101)
);

  // FONTE: Towers, "Polynomial Counters, what the heck is this?" and "Horizontal Sync Counter".
  // HW: forma imposta pelo chip original: o polinomio de realimentacao do contador e' fixo no silicio (novo bit da
  //     esquerda = XNOR dos dois bits menos significativos, deslocamento a direita).
  function [5:0] pnext;
    input [5:0] s;
    begin
      pnext = {~(s[0] ^ s[1]), s[5:1]};
    end
  endfunction

  reg [5:0] cnt;
  reg       more;                        // "more motion required" latch of the HMOVE

  // ---- HMOVE: extra clock pulses ("clock stuffing") ----------------------------------------------------------------
  // FONTE: Towers, "Playing with the HMOVE registers": a latch per object is set at the start of the HMOVE; while it is
  // set, every H@1 sends one extra clock to the position counter; the 4-bit comparator clears the latch when the motion
  // counter reaches the complement of the stored value; D7 of HMxx is compared inverted, so the number of extra clocks
  // is {~hmv[3], hmv[2:0]} (0..15).  Equivalent form used here: stop when the counter == ~m.
  wire [3:0] m     = {~hmv[3], hmv[2:0]};
  wire       match = (hm_v == ~m);
  wire       stuff = hm_tick && more && !match;

  // the stuffed clock reaches the position counter PD colour clocks after the comparison that decided it; tia_hc takes the same PD
  // out of the time of the first comparison, so the pulses themselves do not move.  FONTE: medida, out/passo33.txt (Fase 3, bench
  // hmclr_mid: the window of HMCLR / HMxx writes that bypass the stop condition is the same for two of the three reference models and this model
  // only with PD = 2 to 4; 5 and 6 move the window one test (3 CLK) earlier).
  localparam PD = `TIA_PD;
  reg [7:0] stq;
  always @(posedge clk) if (rst) stq <= 8'd0; else if (cc_ce) stq <= {stq[6:0], stuff};
  wire stuff_d = (PD == 0) ? stuff : stq[PD - 1];
  // HMVIS = 1: backlog of the stuffed clocks that fell in the visible part, replayed one per CLK in the next HBLANK
  reg [3:0] bk;
  wire      rep  = (HMVIS != 0) && !motck && !stuff_d && (bk != 4'd0);
  // FONTE: medida, out/passo33.txt (Fase 3, hmove_m0 contra os modelos de referencia): um pulso extra que cai na parte visivel
  // coincide com um pulso normal e nao move nada (OR); so' os pulsos extras do HBLANK deslocam o objeto.  Towers, "HMOVE during the
  // visible scanline", descreve o caso como nao universal.
  assign step = motck | stuff_d | rep;

  // ---- START decodes -----------------------------------------------------------------------------------------------
  // FONTE: Towers, "Player 0 and Player 1 Horizontal Position Counters" (decode table) and "Missile 0 and Missile 1 ...":
  // missiles and ball use the same decodes as players and the same NUSIZ[2:0] flags.
  // HW: forma imposta pelo chip original: a decodificacao 'start' e' um unico estado do contador (101101 = volta
  //     principal; 111000, 101111, 111001 = copias "close", "medium", "far"); a tabela de NUSIZ tem 8 valores
  //     definidos pelo hardware.
  wire d_main  = (cnt == 6'b101101);
  wire d_close = (cnt == 6'b111000) && (nus == 3'b001 || nus == 3'b011);
  wire d_med   = (cnt == 6'b101111) && (nus == 3'b011 || nus == 3'b010 || nus == 3'b110);
  wire d_far   = (cnt == 6'b111001) && (nus == 3'b100 || nus == 3'b110);
  wire d_any   = d_main | d_close | d_med | d_far;

  assign st_ev = cc_ce && !reset_wr && step && ph == 2'd3 && d_any || cc_ce && reset_wr && force_start;

  assign st_main = cc_ce && !reset_wr && step && ph == 2'd3 && d_main;

  always @(posedge clk) begin
    if (rst) begin
      ph <= PUPH; cnt <= 6'd0; st1 <= 1'b0; st2 <= 1'b0; more <= 1'b0; bk <= 4'd0;
    end else if (cc_ce) begin
      if (HMVIS != 0) begin
        if (stuff_d && motck) begin if (bk != 4'd15) bk <= bk + 4'd1; end
        else if (rep) bk <= bk - 4'd1;
      end
      if (hm_start) more <= 1'b1;
      else if (hm_tick && more && match) more <= 1'b0;

      if (reset_wr) begin
        // FONTE: Towers, "Re-triggering after exactly 18, 33, 66 or 162 cycles": a reset of the position counter also
        // resets the two-phase clock attached to it.  FONTE: Stella PG §4.A: RESxx zeroes the counter at any time.
        ph  <= RPH;
        cnt <= 6'd0;
        // FONTE: Towers, "Ball Horizontal Position Counter": RESBL sends a START; RESMx / RESPx do not.
        if (force_start) st1 <= 1'b1;
        // FONTE: medida, out/passo33.txt (Fase 3, resp_double_n3): a START that is at the end of its count when the reset lands still
        // reaches the graphics (the reset acts on the position counter, not on the START latch path); it is consumed here.
        else if (step && ph == 2'd3) begin st2 <= st1; st1 <= 1'b0; end
      end else if (step) begin
        ph <= ph + 2'd1;
        if (ph == 2'd3) begin
          st2 <= st1;
          st1 <= d_any;
          // FONTE: Towers, "Polynomial Counters": the illegal state 111111 resets to 000000; the "wrap-around" decode
          // makes the counter reset itself to 000000 on the next phase-2 clock.
          if (d_main || cnt == 6'b111111) cnt <= 6'd0; else cnt <= pnext(cnt);
        end
      end
    end
  end

endmodule

// -----------------------------------------------------------------------------------------------------------------
// missile / ball: position counter + width logic + output delay
// -----------------------------------------------------------------------------------------------------------------
module tia_mis #(
  parameter       HMVIS = 0,
  parameter       BALL = 1'b0,        // 1: ball (RESBL sends a START); 0: missile (RESMx does not)
  parameter [1:0] RPH  = 2'd0,
  parameter [1:0] PUPH = 2'd2,        // power-up phase of the position counter (see tia_pos)
  parameter [1:0] PW   = 2'd0,        // phase, inside the START window, of the single pixel of a width-1 object (calibrated)
  parameter [2:0] DLY  = 3'd3         // CLK between the START window and the pixel (decode + latch delay, calibrated)
) (
  input            clk,
  input            rst,
  input            cc_ce,
  input            motck,
  input            reset_wr,
  input            lock_reset,        // RESMPx: the player's scan counter is at the centre of its main copy (missiles only)
  input            en,                // object enabled (ENAMx and not RESMPx; ENABL after VDELBL)
  input      [2:0] nus,               // NUSIZ[2:0] (missiles); 000 for the ball
  input      [1:0] size,              // width 1, 2, 4, 8 (NUSIZ[5:4] or CTRLPF[5:4])
  input            hm_start,
  input            hm_tick,
  input      [3:0] hm_v,
  input      [3:0] hmv,
  output           pix
);
  wire [1:0] ph; wire st1, st2, step, st_ev, st_main;
  tia_pos #(.RPH(RPH), .PUPH(PUPH), .HMVIS(HMVIS)) pos (.clk(clk), .rst(rst), .cc_ce(cc_ce), .motck(motck), .reset_wr(reset_wr | lock_reset), .force_start(BALL), .nus(nus),
                            .hm_start(hm_start), .hm_tick(hm_tick), .hm_v(hm_v), .hmv(hmv), .ph(ph), .st1(st1), .st2(st2), .step(step), .st_ev(st_ev), .st_main(st_main));

  // ---- width -------------------------------------------------------------------------------------------------------
  // FONTE: Towers, "Notes on the Ball/Missile width enclockifier": START is 4 CLK wide; width 1 = one pulse of the
  // two-phase clock, 2 = a line active 2 CLK in 4, 4 = the START itself, 8 = START plus a copy delayed by 4 CLK.
  // FONTE: Stella PG §6.B: widths 1, 2, 4 or 8 clocks.
  wire wsel = size[1] | (size[0] ? (ph == PW || ph == PW + 2'd1) : (ph == PW));
  wire raw = (st1 & wsel) | (st2 & (size == 2'b11));
  // FONTE: Towers, "Player 0 and Player 1 Horizontal Position Counters": each START decode is delayed by 4 CLK plus 1 CLK to
  // latch it; modelled as a delay line on the pixel, clocked like the position counter (the START latches use the same two-phase
  // clock, so they pause in HBLANK together with the counter and a pending pixel survives the horizontal blank and a RESxx)
  reg [7:0] dl;
  always @(posedge clk) if (rst) dl <= 8'd0; else if (cc_ce && step) dl <= {dl[6:0], raw};
  // FONTE: Stella PG §4.D: ENAxx acts on the output (it enables the object, it does not stop its counter)
  assign pix = en & ((DLY == 3'd0) ? raw : dl[DLY - 3'd1]);

endmodule

// -----------------------------------------------------------------------------------------------------------------
// player: position counter + 3-bit graphics scan counter
// -----------------------------------------------------------------------------------------------------------------
module tia_ply #(
  parameter       HMVIS = 0,
  // ALN = 1: the START reaches the scan counter at the end of the 4-CLK count in which it rose (see below).  ALN = 0: the START reaches
  // it a fixed 4 CLK after it rose, whatever the phase of the two-phase clock (the first model of this file; kept only for the bench).
  parameter       ALN = 1,
  parameter [1:0] RPH = 2'd0,
  parameter [1:0] PUPH = 2'd3,        // power-up phase of the position counter (see tia_pos); players show 1 CLK later than missiles
  parameter [2:0] DLY = 3'd0,         // extra CLK after the count-aligned START (sweep parameter of the bench; 0 in the design)
  parameter [1:0] SPH = 2'd0,         // phase of the 4-CLK clock on which the double / quad scan clock ticks (calibrated)
  parameter [1:0] CD  = 2'd0,         // delay, in colour clocks, of the RESMPx centre pulse (calibrated)
  parameter [1:0] EXD = 2'd1,         // extra START delay of the double-width player (calibrated: the three reference models agree on +1 CLK)
  parameter [1:0] EXQ = 2'd1,         // extra START delay of the quad-width player (same)
  parameter [3:0] CTD = 4'd6,         // RESMPx centre pulse, in CLK after the scan counter starts, for the double-width player (calibrated)
  parameter [3:0] CTQ = 4'd10         // same for the quad-width player (calibrated)
) (
  input            clk,
  input            rst,
  input            cc_ce,
  input            motck,
  input            reset_wr,
  input      [2:0] nus,               // NUSIZ0/1[2:0]: copies and width
  input            refl,              // REFPx[3]
  input      [7:0] grp,               // graphics register already selected by VDELPx (new or old)
  input            hm_start,
  input            hm_tick,
  input      [3:0] hm_v,
  input      [3:0] hmv,
  output           pix,
  output           centre             // RESMPx: pulse while the scan counter is at %100 during the main copy
);
  wire [1:0] ph; wire st1, st2, step, st_ev, st_main;
  tia_pos #(.RPH(RPH), .PUPH(PUPH), .HMVIS(HMVIS)) pos (.clk(clk), .rst(rst), .cc_ce(cc_ce), .motck(motck), .reset_wr(reset_wr), .force_start(1'b0), .nus(nus),
                            .hm_start(hm_start), .hm_tick(hm_tick), .hm_v(hm_v), .hmv(hmv), .ph(ph), .st1(st1), .st2(st2), .step(step), .st_ev(st_ev), .st_main(st_main));

  // START -> graphics scan counter.  FONTE: Towers, "Player 0 and Player 1 Horizontal Position Counters" (the START decode is latched by
  // the two-phase clock): the START that rises at the beginning of a count reaches the scan counter at the end of that count (4 CLK later
  // when nothing intervenes); a RESPx in between re-phases the two-phase clock and the START waits for the new end of count.
  // FONTE: medida, out/passo33.txt (Fase 3, resp_double_n3: the three reference models agree).
  wire       dbl  = (nus == 3'b101);
  wire       quad = (nus == 3'b111);
  wire       go_al = step && ph == 2'd3 && st1;
  // the extra CLK of the double / quad player and the sweep parameter DLY come after the count alignment
  wire [3:0] dsel = DLY + (dbl ? EXD : quad ? EXQ : 2'd0);
  reg [10:0] gd;
  always @(posedge clk) if (rst) gd <= 11'd0; else if (cc_ce && step) gd <= {gd[9:0], go_al};
  wire go_a = (dsel == 4'd0) ? go_al : gd[dsel - 4'd1];
  // ALN = 0: fixed delay of 4 CLK + extras after the pulse that announces the START
  reg [13:0] od;
  always @(posedge clk) if (rst) od <= 14'd0; else if (cc_ce && step) od <= {od[12:0], st_ev};
  wire [4:0] osel = 5'd4 + {2'b00, DLY} + (dbl ? {3'b000, EXD} : quad ? {3'b000, EXQ} : 5'd0);
  wire go_o = od[osel - 5'd1];
  wire go = (ALN != 0) ? go_a : go_o;
  // START as the position counter sees it (used by RESMPx below), delayed only by the extra CLK of the wide players
  reg [2:0] sd;
  wire      stp = st_ev;
  always @(posedge clk) if (rst) sd <= 3'd0; else if (cc_ce && step) sd <= {sd[1:0], stp};
  reg [2:0] sm;
  always @(posedge clk) if (rst) sm <= 3'd0; else if (cc_ce && step) sm <= {sm[1:0], st_main};

  // ---- graphics scan counter -----------------------------------------------------------------------------------------
  // FONTE: Towers, "Player 0 and Player 1 Graphics Scan Counters": a 3-bit binary counter that START resets and that counts
  // 0..7 and stops; it counts only while MOTCK runs (so a sprite that ends beyond the right edge continues on the next line)
  // and its clock is masked by NUSIZ: every CLK (normal), every 2 CLK (double, NUSIZ 101), every 4 CLK (quad, NUSIZ 111).
  // The NUSIZ and REFP bits are read at every pixel.
  reg       act;
  reg [2:0] sc;
  wire      tick = dbl ? (ph[0] == SPH[0]) : quad ? (ph == SPH) : 1'b1;
  always @(posedge clk) begin
    if (rst) begin act <= 1'b0; sc <= 3'd0; end
    else if (cc_ce && step) begin
      if (go) begin act <= 1'b1; sc <= 3'd0; end
      else if (act && tick) begin
        if (sc == 3'd7) act <= 1'b0; else sc <= sc + 3'd1;
      end
    end
  end
  // FONTE: Towers (same section): REFPx inverts the address bits of the source bit
  // FONTE: medida, out/passo33.txt (Fase 3, pl_mid_0: the three reference models agree): a write to REFPx reaches the address
  // inversion one colour clock later than a write to GRPx or VDELPx.
  reg refl_q;
  always @(posedge clk) if (rst) refl_q <= 1'b0; else if (cc_ce) refl_q <= refl;
  assign pix = act & grp[refl_q ? sc : ~sc];

  // ---- RESMPx -------------------------------------------------------------------------------------------------------
  // FONTE: Towers, "Missile 0 and Missile 1 Horizontal Position Counters": the missile counter is reset when the player's scan
  // counter is at %100 and the main copy is being drawn (latch [FSTOB], cleared by the wrap-around START and set by the START of a
  // "close", "medium" or "far" copy).  The scan counter runs ahead of the pixel that reaches the screen by the START latch/decode
  // delay (the screen pixel is the output of the graphics latches), so the centre is taken from a copy of the counter that
  // starts at the START itself, without the count alignment of `go` above.  FONTE: medida, out/passo33.txt (Fase 3, resmp_basic:
  // the three reference models agree for the single and the "close" copy player).
  wire [1:0] ex   = dbl ? EXD : quad ? EXQ : 2'd0;
  wire       go0  = (ex == 2'd0) ? stp : sd[ex - 2'd1];
  wire       gm0  = (ex == 2'd0) ? st_main : sm[ex - 2'd1];
  reg        act0, fstob;
  reg  [2:0] sc0;
  reg  [3:0] tcn;                       // CLK since the scan counter started (saturates)
  always @(posedge clk) begin
    if (rst) begin act0 <= 1'b0; sc0 <= 3'd0; fstob <= 1'b0; tcn <= 4'd0; end
    else if (cc_ce && step) begin
      if (go0) begin act0 <= 1'b1; sc0 <= 3'd0; fstob <= ~gm0; tcn <= 4'd0; end
      else if (act0) begin
        if (tcn != 4'd15) tcn <= tcn + 4'd1;
        if (tick) begin
          if (sc0 == 3'd7) act0 <= 1'b0; else sc0 <= sc0 + 3'd1;
        end
      end
    end
  end
  // FONTE: medida, out/passo33.txt (Fase 3, resmp_basic NUSIZ 5 e 7): with a stretched player the missile lands at the player's first pixel
  // + 6 (double) and + 10 (quad) instead of + 4 (single); two of the three reference models agree, the third says + 7 and + 15
  // [ABERTO: console].  The source only says "scan counter at %100".
  wire [3:0] tc_c = dbl ? CTD : quad ? CTQ : 4'd4;
  wire c_raw = act0 && !fstob && tcn == tc_c && cc_ce && step;
  reg [3:0] cs;
  always @(posedge clk) if (rst) cs <= 4'd0; else if (cc_ce) cs <= {cs[2:0], c_raw};
  assign centre = (CD == 2'd0) ? c_raw : cs[CD - 2'd1];

endmodule
