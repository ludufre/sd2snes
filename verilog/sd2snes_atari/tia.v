// SPDX-License-Identifier: GPL-2.0-or-later
//
// tia.v -- Atari 2600 TIA (video side) written from scratch.  Milestone 3.
//
// Passo 33: the five objects (P0, P1, M0, M1, ball) now come from tia_hc.v + tia_obj.v (counters of the original chip); compile with
// `define TIA_P31 to get the Passo 31 objects back (then tia_hc.v and tia_obj.v are not needed).
//
// WHAT IT IS
//   A colour-clock-accurate pixel generator.  One cc_ce pulse = one colour
//   clock (3.579545 MHz NTSC).  228 colour clocks per line: 68 HBLANK
//   (hcount 0..67) then 160 visible pixels (hcount 68..227).
//
//   Objects are modelled by a start position (0..159) per object.  Each
//   pixel is computed from the CURRENT register values, so mid-line writes
//   behave naturally.  The 4 colour sources that the SNES-side player needs
//   come out as pix_src:  0 = COLUBK, 1 = COLUPF, 2 = COLUP0, 3 = COLUP1.
//
// CPU INTERFACE
//   phi2_ce  : pulses once per CPU cycle (every 3rd colour clock, on the
//              last colour clock of the cycle).  The shell must build
//                 riot_tick = phi2_ce
//                 cpu_ce    = phi2_ce & rdy
//                 wr        = cpu_ce & cpu_we & tia_selected
//              so that wr always coincides with phi2_ce.
//   rdy      : 0 while WSYNC is holding the CPU.
//   a[5:0]   : CPU A5..A0 for writes; reads decode a[3:0].
//   dout     : read data (collision bits in D7/D6, INPTx in D7).
//
// POSITIONING (assumptions taken from the publicly documented behaviour,
// to be validated against real hardware / test ROMs)
//   hs = colour clock on which the strobe takes effect: the write lands on the
//   last clock of the CPU cycle (hcount % 3 == 2), so hs = hcount + 1.
//   RESP0/1 : pos = (hs < 68) ? 3 : (hs - 68 + 5) mod 160
//   RESM0/1 and RESBL : pos = (hs < 68) ? 2 : (hs - 68 + 4) mod 160
//   HMOVE   : every object moves LEFT by its signed HMxx nibble.
//             If strobed with hs < 68 the first 8 pixels of the line are
//             blanked (the "HMOVE comb").  Late HMOVE tricks are NOT modelled.
//   RESMPx  : on 1->0 the missile snaps to the player centre
//             (+4, or +8 for NUSIZ=5, +16 for NUSIZ=7).
//
// WRITE LATENCY (step 24, measured against the ALE in diagnosis 3)
//   PF0/PF1/PF2 : the playfield only changes at the start of a 4-pixel cell.  A write that
//             lands on colour clock h (the last clock of the CPU cycle) takes effect on the
//             first clock that is a multiple of 4 and >= h + 3, i.e. 2..5 clocks later than
//             the other registers.  One pending value per register (pfw_*), so writes on
//             consecutive CPU cycles to different PF registers do not disturb each other.
//   GRP0/GRP1 : one colour clock later than the other registers (gw_*); the VDEL copies
//             (dgrp0/dgrp1/denabl) are taken at that same moment.
//
// NOT IMPLEMENTED YET: RSYNC, paddle dump timing, audio generation (the AUD
// registers are exported), latency of the other writes (NUSIZ, REFP, ...), HMOVE as
// timed extra clocks, the floating data bus in bits 5..0 of the reads.
//
// STATUS: untested when written.  Verify with tb_tia.v.
//
`timescale 1ns / 1ps

module tia #(
  parameter HM74  = 1,       // HMOVE no ultimo ciclo da linha: valor nominal (ver tia_hc.v)
  parameter HMVIS = 0
) (
  input             clk,
  input             rst,
  input             cc_ce,
  input      [5:0]  a,
  input             wr,
  input      [7:0]  din,
  output     [7:0]  dout,
  input      [3:0]  inpt,
  input             inpt4,
  input             inpt5,
  output            rdy,
  output            phi2_ce,
  output     [7:0]  hcount_o,
  output            vsync,
  output            vblank,
  output reg        pix_stb,
  output reg [7:0]  pix_x,
  output reg        pix_vis,
  output reg [1:0]  pix_src,
  output reg [6:0]  pix_col,
  output     [6:0]  colubk,
  output     [6:0]  colupf,
  output     [6:0]  colup0,
  output     [6:0]  colup1,
  output     [3:0]  audc0,
  output     [3:0]  audc1,
  output     [4:0]  audf0,
  output     [4:0]  audf1,
  output     [3:0]  audv0,
  output     [3:0]  audv1
);

  // ------------------------------------------------------------------
  // state
  // ------------------------------------------------------------------
  reg [7:0] hcount;
  reg [1:0] ph;
  reg       wsync, hmove_blank;
  reg       vsync_b, vblank_b, vb_latch, vb_dump, l4, l5;

  reg [5:0] nusiz0, nusiz1, ctrlpf;
  reg [7:0] col_p0, col_p1, col_pf, col_bk;
  reg       refp0, refp1;
  reg [7:0] pf0, pf1, pf2;
  reg [7:0] grp0, grp1, dgrp0, dgrp1;
  reg       enam0, enam1, enabl, denabl;
  reg [3:0] hmp0, hmp1, hmm0, hmm1, hmbl;
  reg       vdelp0, vdelp1, vdelbl, resmp0, resmp1;
`ifdef TIA_P31
  reg [7:0] pos_p0, pos_p1, pos_m0, pos_m1, pos_bl;
`endif

  reg c_m0p1, c_m0p0, c_m1p0, c_m1p1, c_p0pf, c_p0bl, c_p1pf, c_p1bl;
  reg c_m0pf, c_m0bl, c_m1pf, c_m1bl, c_blpf, c_p0p1, c_m0m1;

  reg [3:0] r_audc0, r_audc1, r_audv0, r_audv1;
  reg [4:0] r_audf0, r_audf1;

  // pending writes (see WRITE LATENCY): bit 0 = PF0 / GRP0, bit 1 = PF1 / GRP1, bit 2 = PF2
  reg [2:0] pfw_pend, pfw_skip;
  reg [7:0] pfw_v0, pfw_v1, pfw_v2;
  reg [1:0] gw_pend;
  reg [7:0] gw_v0, gw_v1;

  assign rdy      = ~wsync;
  assign phi2_ce  = cc_ce & (ph == 2'd2);
  assign hcount_o = hcount;
  assign vsync    = vsync_b;
  assign vblank   = vblank_b;
  assign colubk = col_bk[7:1];  assign colupf = col_pf[7:1];
  assign colup0 = col_p0[7:1];  assign colup1 = col_p1[7:1];
  assign audc0 = r_audc0; assign audc1 = r_audc1;
  assign audf0 = r_audf0; assign audf1 = r_audf1;
  assign audv0 = r_audv0; assign audv1 = r_audv1;

  // ------------------------------------------------------------------
  // helper functions
  // ------------------------------------------------------------------
  // distance from an object's start to the current pixel, modulo 160
  function [7:0] rel;
    input [7:0] xx;
    input [7:0] pp;
    begin
      rel = (xx >= pp) ? (xx - pp) : (xx + 8'd160 - pp);
    end
  endfunction

  // player pixel (copies, size and reflection from NUSIZ / REFP)
  function pl_on;
    input [7:0] dx;
    input [2:0] ns;
    input       refp;
    input [7:0] g;
    reg         hit;
    reg [2:0]   pi;
    reg [7:0]   d1;
    begin
      hit = 1'b0;
      pi  = dx[2:0];
      // a stretched player (double or quad size) starts one pixel later than a
      // normal-width one at the same position, as on the real TIA
      d1  = (dx == 8'd0) ? 8'd159 : dx - 8'd1;
      case (ns)
        3'd5: begin hit = (d1 < 8'd16); pi = d1[3:1]; end      // double size
        3'd7: begin hit = (d1 < 8'd32); pi = d1[4:2]; end      // quad size
        default: begin
          hit = (dx < 8'd8);
          if (dx >= 8'd16 && dx < 8'd24 && (ns == 3'd1 || ns == 3'd3))                 hit = 1'b1;
          if (dx >= 8'd32 && dx < 8'd40 && (ns == 3'd2 || ns == 3'd3 || ns == 3'd6))   hit = 1'b1;
          if (dx >= 8'd64 && dx < 8'd72 && (ns == 3'd4 || ns == 3'd6))                 hit = 1'b1;
        end
      endcase
      pl_on = hit & (refp ? g[pi] : g[~pi]);
    end
  endfunction

  // missile pixel (copies follow NUSIZ[2:0], width from NUSIZ[5:4])
  function ms_on;
    input [7:0] dx;
    input [2:0] ns;
    input [1:0] sz;
    input       en;
    reg [7:0]   w;
    reg         hit;
    begin
      w   = 8'd1 << sz;
      hit = (dx < w);
      if (dx >= 8'd16 && dx < (8'd16 + w) && (ns == 3'd1 || ns == 3'd3))                hit = 1'b1;
      if (dx >= 8'd32 && dx < (8'd32 + w) && (ns == 3'd2 || ns == 3'd3 || ns == 3'd6))  hit = 1'b1;
      if (dx >= 8'd64 && dx < (8'd64 + w) && (ns == 3'd4 || ns == 3'd6))                hit = 1'b1;
      ms_on = en & hit;
    end
  endfunction

  // start position produced by a RESxx strobe outside HBLANK
  function [7:0] rpos;
    input [7:0] hs_i;
    input [2:0] off;
    reg   [8:0] t;
    begin
      t = {1'b0, hs_i} - 9'd68 + {6'b0, off};
      if (t >= 9'd160) t = t - 9'd160;
      rpos = t[7:0];
    end
  endfunction

  // HMOVE: pos - signed nibble, modulo 160
  function [7:0] mv;
    input [7:0] pos;
    input [3:0] hm;
    reg   [8:0] t;
    begin
      t = {1'b0, pos} - {{5{hm[3]}}, hm};
      if (t[8])               t = t + 9'd160;
      else if (t >= 9'd160)   t = t - 9'd160;
      mv = t[7:0];
    end
  endfunction

  // missile snapped to the player centre
  function [7:0] addmid;
    input [7:0] p;
    input [2:0] ns;
    reg   [8:0] t;
    begin
      t = {1'b0, p} + ((ns == 3'd5) ? 9'd8 : (ns == 3'd7) ? 9'd16 : 9'd4);
      if (t >= 9'd160) t = t - 9'd160;
      addmid = t[7:0];
    end
  endfunction

  // ------------------------------------------------------------------
  // pixel pipeline (combinational from current registers)
  // ------------------------------------------------------------------
  wire       vis_area = (hcount >= 8'd68);
  wire [7:0] x        = hcount - 8'd68;
  // colour clock on which a strobe written NOW takes effect: the write lands on the
  // last clock of the CPU cycle, so that is the next one (227 wraps to 0 = HBLANK)
  wire [7:0] hs       = (hcount == 8'd227) ? 8'd0 : hcount + 8'd1;

`ifdef TIA_P31
  wire [7:0] dxp0 = rel(x, pos_p0);
  wire [7:0] dxp1 = rel(x, pos_p1);
  wire [7:0] dxm0 = rel(x, pos_m0);
  wire [7:0] dxm1 = rel(x, pos_m1);
  wire [7:0] dxbl = rel(x, pos_bl);

  wire p0_on = pl_on(dxp0, nusiz0[2:0], refp0, vdelp0 ? dgrp0 : grp0);
  wire p1_on = pl_on(dxp1, nusiz1[2:0], refp1, vdelp1 ? dgrp1 : grp1);
  wire m0_on = ms_on(dxm0, nusiz0[2:0], nusiz0[5:4], enam0 & ~resmp0);
  wire m1_on = ms_on(dxm1, nusiz1[2:0], nusiz1[5:4], enam1 & ~resmp1);
  wire bl_on = (vdelbl ? denabl : enabl) & (dxbl < (8'd1 << ctrlpf[5:4]));
`else
  // ---- Passo 33: objetos do chip original (tia_hc.v, tia_obj.v); `define TIA_P31 volta aos objetos do Passo 31 ---------
  wire       hm_wr = wr && a == 6'h2A;
  reg  [4:0] pend;                                   // RESP0, RESP1, RESM0, RESM1, RESBL escritos desde o ultimo colour clock
  always @(posedge clk) begin
    if (rst) pend <= 5'd0;
    else begin
      if (cc_ce) pend <= 5'd0;
      if (wr && a >= 6'h10 && a <= 6'h14) pend[a - 6'h10] <= 1'b1;
    end
  end
  wire [1:0] n_ph; wire [5:0] n_cnt; wire n_hb, n_hb1, n_hb2, n_hml, n_hmt, n_hms; wire [3:0] n_hmv;
  tia_hc #(.HM74(HM74), .HMVIS(HMVIS)) n_hc (.clk(clk), .rst(rst), .cc_ce(cc_ce), .hmove_wr(hm_wr), .ph(n_ph), .cnt(n_cnt), .hb(n_hb),
               .hb_nx(n_hb1), .hb_nx2(n_hb2), .hmove_lat(n_hml), .hm_tick(n_hmt), .hm_v(n_hmv), .hm_start(n_hms));
  wire [7:0] p0g = vdelp0 ? dgrp0 : grp0;
  wire [7:0] p1g = vdelp1 ? dgrp1 : grp1;
  wire       blen = vdelbl ? denabl : enabl;
  wire       p0_on, p1_on, m0_on, m1_on, bl_on, p0cen, p1cen;
  tia_ply #(.HMVIS(HMVIS)) n_p0 (.clk(clk), .rst(rst), .cc_ce(cc_ce), .motck(~n_hb1), .reset_wr(pend[0]),
     .nus(nusiz0[2:0]), .refl(refp0), .grp(p0g),
     .hm_start(n_hms), .hm_tick(n_hmt), .hm_v(n_hmv), .hmv(hmp0), .pix(p0_on), .centre(p0cen));
  tia_ply #(.HMVIS(HMVIS)) n_p1 (.clk(clk), .rst(rst), .cc_ce(cc_ce), .motck(~n_hb1), .reset_wr(pend[1]),
     .nus(nusiz1[2:0]), .refl(refp1), .grp(p1g),
     .hm_start(n_hms), .hm_tick(n_hmt), .hm_v(n_hmv), .hmv(hmp1), .pix(p1_on), .centre(p1cen));
  tia_mis #(.HMVIS(HMVIS), .BALL(1'b0)) n_m0 (.clk(clk), .rst(rst), .cc_ce(cc_ce), .motck(~n_hb1), .reset_wr(pend[2]), .lock_reset(resmp0 & p0cen),
     .en(enam0 & ~resmp0), .nus(nusiz0[2:0]), .size(nusiz0[5:4]),
     .hm_start(n_hms), .hm_tick(n_hmt), .hm_v(n_hmv), .hmv(hmm0), .pix(m0_on));
  tia_mis #(.HMVIS(HMVIS), .BALL(1'b0)) n_m1 (.clk(clk), .rst(rst), .cc_ce(cc_ce), .motck(~n_hb1), .reset_wr(pend[3]), .lock_reset(resmp1 & p1cen),
     .en(enam1 & ~resmp1), .nus(nusiz1[2:0]), .size(nusiz1[5:4]),
     .hm_start(n_hms), .hm_tick(n_hmt), .hm_v(n_hmv), .hmv(hmm1), .pix(m1_on));
  tia_mis #(.HMVIS(HMVIS), .BALL(1'b1)) n_bl (.clk(clk), .rst(rst), .cc_ce(cc_ce), .motck(~n_hb1), .reset_wr(pend[4]), .lock_reset(1'b0),
     .en(blen), .nus(3'b000), .size(ctrlpf[5:4]),
     .hm_start(n_hms), .hm_tick(n_hmt), .hm_v(n_hmv), .hmv(hmbl), .pix(bl_on));
`endif

  // playfield: 20 bits, 4 pixels each, right half repeats or mirrors
  wire [5:0] pfx  = x[7:2];                                  // 0..39
  wire       left = (x < 8'd80);
  wire [5:0] pfr  = ctrlpf[0] ? (6'd39 - pfx) : (pfx - 6'd20);
  wire [4:0] pfi  = left ? pfx[4:0] : pfr[4:0];              // 0..19
  wire [4:0] pfi1 = pfi - 5'd4;
  wire [4:0] pfi2 = pfi - 5'd12;
  wire pf_on = (pfi < 5'd4)  ? pf0[{1'b1, pfi[1:0]}] :
               (pfi < 5'd12) ? pf1[~pfi1[2:0]]       :
                               pf2[pfi2[2:0]];

  // FONTE (V1, Passo 34): score mode (CTRLPF D1) only recolors the playfield while priority (D2) is off; with D2 = 1 the playfield and the ball
  //   are COLUPF.  Stella Programmer's Guide gives D1 and D2 separately and does not describe the combination; the combination is taken from the
  //   two witnesses: Stella 2c3d702 emucore/tia/Playfield.cxx:107 (score only if (CTRLPF & 6) == 2) and TIA.cxx:1936 ("CTRLPF D2=1, D1=ignored"),
  //   and Atari2600_MiSTer TIA.vhd (pf_priority tested before pf_score).  [ABERTO para o console: a fonte do chip e' omissa]
  wire [1:0] pf_src = (ctrlpf[1] & ~ctrlpf[2]) ? (left ? 2'd2 : 2'd3) : 2'd1;   // score mode
  wire       pfbl_on  = pf_on | bl_on;
  wire [1:0] pfbl_src = bl_on ? 2'd1 : pf_src;                   // ball is COLUPF
  wire       p0m0_on  = p0_on | m0_on;
  wire       p1m1_on  = p1_on | m1_on;

  reg [1:0] src;
  always @* begin
    if (ctrlpf[2]) begin
      if (pfbl_on)       src = pfbl_src;
      else if (p0m0_on)  src = 2'd2;
      else if (p1m1_on)  src = 2'd3;
      else               src = 2'd0;
    end else begin
      if (p0m0_on)       src = 2'd2;
      else if (p1m1_on)  src = 2'd3;
      else if (pfbl_on)  src = pfbl_src;
      else               src = 2'd0;
    end
  end

  reg [7:0] csel;
  always @* begin
    case (src)
      2'd0:    csel = col_bk;
      2'd1:    csel = col_pf;
      2'd2:    csel = col_p0;
      default: csel = col_p1;
    endcase
  end

  wire vis_ok = vis_area & ~vblank_b & ~(hmove_blank & (x < 8'd8));

  // ------------------------------------------------------------------
  // register reads
  // ------------------------------------------------------------------
  reg [7:0] rd8;
  always @* begin
    case (a[3:0])
      4'h0: rd8 = {c_m0p1, c_m0p0, 6'b000000};
      4'h1: rd8 = {c_m1p0, c_m1p1, 6'b000000};
      4'h2: rd8 = {c_p0pf, c_p0bl, 6'b000000};
      4'h3: rd8 = {c_p1pf, c_p1bl, 6'b000000};
      4'h4: rd8 = {c_m0pf, c_m0bl, 6'b000000};
      4'h5: rd8 = {c_m1pf, c_m1bl, 6'b000000};
      4'h6: rd8 = {c_blpf, 7'b0000000};
      4'h7: rd8 = {c_p0p1, c_m0m1, 6'b000000};
      4'h8, 4'h9, 4'hA, 4'hB: rd8 = {inpt[a[1:0]], 7'b0000000};
      4'hC: rd8 = {(vb_latch ? l4 : inpt4), 7'b0000000};
      4'hD: rd8 = {(vb_latch ? l5 : inpt5), 7'b0000000};
      default: rd8 = 8'h00;
    endcase
  end
  assign dout = rd8;

  // ------------------------------------------------------------------
  // sequencer
  // ------------------------------------------------------------------
  always @(posedge clk) begin
    if (rst) begin
      hcount <= 8'd0; ph <= 2'd0; wsync <= 1'b0; hmove_blank <= 1'b0;
      vsync_b <= 1'b0; vblank_b <= 1'b0; vb_latch <= 1'b0; vb_dump <= 1'b0;
      l4 <= 1'b1; l5 <= 1'b1;
      nusiz0 <= 6'd0; nusiz1 <= 6'd0; ctrlpf <= 6'd0;
      col_p0 <= 8'd0; col_p1 <= 8'd0; col_pf <= 8'd0; col_bk <= 8'd0;
      refp0 <= 1'b0; refp1 <= 1'b0; pf0 <= 8'd0; pf1 <= 8'd0; pf2 <= 8'd0;
      grp0 <= 8'd0; grp1 <= 8'd0; dgrp0 <= 8'd0; dgrp1 <= 8'd0;
      enam0 <= 1'b0; enam1 <= 1'b0; enabl <= 1'b0; denabl <= 1'b0;
      hmp0 <= 4'd0; hmp1 <= 4'd0; hmm0 <= 4'd0; hmm1 <= 4'd0; hmbl <= 4'd0;
      vdelp0 <= 1'b0; vdelp1 <= 1'b0; vdelbl <= 1'b0; resmp0 <= 1'b0; resmp1 <= 1'b0;
`ifdef TIA_P31
      pos_p0 <= 8'd0; pos_p1 <= 8'd0; pos_m0 <= 8'd0; pos_m1 <= 8'd0; pos_bl <= 8'd0;
`endif
      {c_m0p1, c_m0p0, c_m1p0, c_m1p1, c_p0pf, c_p0bl, c_p1pf, c_p1bl} <= 8'd0;
      {c_m0pf, c_m0bl, c_m1pf, c_m1bl, c_blpf, c_p0p1, c_m0m1} <= 7'd0;
      r_audc0 <= 4'd0; r_audc1 <= 4'd0; r_audv0 <= 4'd0; r_audv1 <= 4'd0;
      r_audf0 <= 5'd0; r_audf1 <= 5'd0;
      pix_stb <= 1'b0; pix_x <= 8'd0; pix_vis <= 1'b0; pix_src <= 2'd0; pix_col <= 7'd0;
      pfw_pend <= 3'd0; pfw_skip <= 3'd0; pfw_v0 <= 8'd0; pfw_v1 <= 8'd0; pfw_v2 <= 8'd0;
      gw_pend <= 2'd0; gw_v0 <= 8'd0; gw_v1 <= 8'd0;
    end else begin
      // fire-button latches (VBLANK D6)
      if (!vb_latch) begin l4 <= 1'b1; l5 <= 1'b1; end
      else begin l4 <= l4 & inpt4; l5 <= l5 & inpt5; end

      // ---- one colour clock ------------------------------------------
      // pix_stb is a ONE-clock pulse: without this default it would stay high
      // until the next cc_ce (~27 clk at 96 MHz) and every consumer would see
      // the same pixel dozens of times.
      pix_stb <= 1'b0;
      if (cc_ce) begin
        pix_stb <= vis_area;
        pix_x   <= x;
        pix_vis <= vis_ok;
        pix_src <= src;
        pix_col <= csel[7:1];

        if (vis_area) begin
          if (m0_on & p1_on) c_m0p1 <= 1'b1;
          if (m0_on & p0_on) c_m0p0 <= 1'b1;
          if (m1_on & p0_on) c_m1p0 <= 1'b1;
          if (m1_on & p1_on) c_m1p1 <= 1'b1;
          if (p0_on & pf_on) c_p0pf <= 1'b1;
          if (p0_on & bl_on) c_p0bl <= 1'b1;
          if (p1_on & pf_on) c_p1pf <= 1'b1;
          if (p1_on & bl_on) c_p1bl <= 1'b1;
          if (m0_on & pf_on) c_m0pf <= 1'b1;
          if (m0_on & bl_on) c_m0bl <= 1'b1;
          if (m1_on & pf_on) c_m1pf <= 1'b1;
          if (m1_on & bl_on) c_m1bl <= 1'b1;
          if (bl_on & pf_on) c_blpf <= 1'b1;
          if (p0_on & p1_on) c_p0p1 <= 1'b1;
          if (m0_on & m1_on) c_m0m1 <= 1'b1;
        end

        if (hcount == 8'd227) begin
          hcount <= 8'd0; ph <= 2'd0; wsync <= 1'b0; hmove_blank <= 1'b0;
        end else begin
          hcount <= hcount + 8'd1;
          ph     <= (ph == 2'd2) ? 2'd0 : ph + 2'd1;
        end
      end

      // ---- pending PFx / GRPx writes ------------------------------------
      // This comes before the write decode, so a new write to the same register on this
      // very clock replaces the pending one.  A PFx write made on clock h skips the clock
      // h + 1 (pfw_skip) and is then applied on the first clock with hcount % 4 == 3: the
      // pixel of the next clock, a multiple of 4 (68 is one, so that is a cell start), is
      // the first one drawn with the new value.
      if (cc_ce) begin
        if (pfw_pend[0]) begin
          if (pfw_skip[0]) pfw_skip[0] <= 1'b0;
          else if (hcount[1:0] == 2'b11) begin pf0 <= pfw_v0; pfw_pend[0] <= 1'b0; end
        end
        if (pfw_pend[1]) begin
          if (pfw_skip[1]) pfw_skip[1] <= 1'b0;
          else if (hcount[1:0] == 2'b11) begin pf1 <= pfw_v1; pfw_pend[1] <= 1'b0; end
        end
        if (pfw_pend[2]) begin
          if (pfw_skip[2]) pfw_skip[2] <= 1'b0;
          else if (hcount[1:0] == 2'b11) begin pf2 <= pfw_v2; pfw_pend[2] <= 1'b0; end
        end
        if (gw_pend[0]) begin gw_pend[0] <= 1'b0; grp0 <= gw_v0; dgrp1 <= grp1; end
        if (gw_pend[1]) begin gw_pend[1] <= 1'b0; grp1 <= gw_v1; dgrp0 <= grp0; denabl <= enabl; end
      end

      // ---- CPU register writes (coincide with phi2_ce) ----------------
      if (wr) begin
        case (a)
          6'h00: vsync_b <= din[1];
          6'h01: begin vblank_b <= din[1]; vb_latch <= din[6]; vb_dump <= din[7]; end
          6'h02: if (!(cc_ce && hcount == 8'd227)) wsync <= 1'b1;   // WSYNC
          6'h03: ;                                                   // RSYNC (todo)
          6'h04: nusiz0 <= din[5:0];
          6'h05: nusiz1 <= din[5:0];
          6'h06: col_p0 <= din;
          6'h07: col_p1 <= din;
          6'h08: col_pf <= din;
          6'h09: col_bk <= din;
          6'h0A: ctrlpf <= din[5:0];
          6'h0B: refp0 <= din[3];
          6'h0C: refp1 <= din[3];
          6'h0D: begin pfw_v0 <= din; pfw_pend[0] <= 1'b1; pfw_skip[0] <= 1'b1; end   // PF0
          6'h0E: begin pfw_v1 <= din; pfw_pend[1] <= 1'b1; pfw_skip[1] <= 1'b1; end   // PF1
          6'h0F: begin pfw_v2 <= din; pfw_pend[2] <= 1'b1; pfw_skip[2] <= 1'b1; end   // PF2
`ifdef TIA_P31
          6'h10: pos_p0 <= (hs < 8'd68) ? 8'd3 : rpos(hs, 3'd5);     // RESP0
          6'h11: pos_p1 <= (hs < 8'd68) ? 8'd3 : rpos(hs, 3'd5);     // RESP1
          6'h12: pos_m0 <= (hs < 8'd68) ? 8'd2 : rpos(hs, 3'd4);     // RESM0
          6'h13: pos_m1 <= (hs < 8'd68) ? 8'd2 : rpos(hs, 3'd4);     // RESM1
          // RESBL in the horizontal blank: position 2, or 3 when HMOVE was strobed earlier in this
          // blank (as the ALE / Stella; Crackpots and Decathlon do RESBL 9 clocks after HMOVE)
          6'h14: pos_bl <= (hs < 8'd68) ? (hmove_blank ? 8'd3 : 8'd2) : rpos(hs, 3'd4);     // RESBL
`endif
          6'h15: r_audc0 <= din[3:0];
          6'h16: r_audc1 <= din[3:0];
          6'h17: r_audf0 <= din[4:0];
          6'h18: r_audf1 <= din[4:0];
          6'h19: r_audv0 <= din[3:0];
          6'h1A: r_audv1 <= din[3:0];
          6'h1B: begin gw_v0 <= din; gw_pend[0] <= 1'b1; end                           // GRP0
          6'h1C: begin gw_v1 <= din; gw_pend[1] <= 1'b1; end                           // GRP1
          6'h1D: enam0 <= din[1];
          6'h1E: enam1 <= din[1];
          6'h1F: enabl <= din[1];
          6'h20: hmp0 <= din[7:4];
          6'h21: hmp1 <= din[7:4];
          6'h22: hmm0 <= din[7:4];
          6'h23: hmm1 <= din[7:4];
          6'h24: hmbl <= din[7:4];
          6'h25: vdelp0 <= din[0];
          6'h26: vdelp1 <= din[0];
          6'h27: vdelbl <= din[0];
          6'h28: begin
            resmp0 <= din[1];
`ifdef TIA_P31
            if (resmp0 && !din[1]) pos_m0 <= addmid(pos_p0, nusiz0[2:0]);
`endif
          end
          6'h29: begin
            resmp1 <= din[1];
`ifdef TIA_P31
            if (resmp1 && !din[1]) pos_m1 <= addmid(pos_p1, nusiz1[2:0]);
`endif
          end
          6'h2A: begin                                               // HMOVE
`ifdef TIA_P31
            pos_p0 <= mv(pos_p0, hmp0);
            pos_p1 <= mv(pos_p1, hmp1);
            pos_m0 <= mv(pos_m0, hmm0);
            pos_m1 <= mv(pos_m1, hmm1);
            pos_bl <= mv(pos_bl, hmbl);
`endif
            if (hs < 8'd68) hmove_blank <= 1'b1;
          end
          6'h2B: begin hmp0 <= 4'd0; hmp1 <= 4'd0; hmm0 <= 4'd0; hmm1 <= 4'd0; hmbl <= 4'd0; end
          6'h2C: begin                                               // CXCLR
            {c_m0p1, c_m0p0, c_m1p0, c_m1p1, c_p0pf, c_p0bl, c_p1pf, c_p1bl} <= 8'd0;
            {c_m0pf, c_m0bl, c_m1pf, c_m1bl, c_blpf, c_p0p1, c_m0m1} <= 7'd0;
          end
          default: ;
        endcase
      end
    end
  end

endmodule
