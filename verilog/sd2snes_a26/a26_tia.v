// a26_tia.v -- Atari 2600 TIA (Television Interface Adaptor), video + register
// file.  Behavioural model written from published register/timing descriptions;
// no vendor netlist or third-party HDL involved.
//
// SCOPE
//   Everything that decides what a colour clock looks like: playfield, two
//   players (NUSIZ copies/scale, reflect, vertical delay), two missiles
//   (RESMPx lock), the ball, object priority (including PFP and score mode),
//   horizontal motion (HMOVE/HMCLR and its 8-pixel comb), the 15 collision
//   latches, WSYNC/RSYNC/VSYNC/VBLANK and the INPT4/5 latches.  The audio
//   registers are LATCHED HERE but consumed elsewhere -- this module only
//   exposes AUDC/AUDF/AUDV so the sound module reads one copy of the truth.
//
// OUTPUT ENCODING (contract §6)
//   One 2-bit code per visible colour clock: 0=background, 1=playfield/ball,
//   2=player0/missile0, 3=player1/missile1, taken AFTER priority and score
//   mode.  The video stage turns those codes into 2bpp SNES tiles and feeds
//   the four line colours to CGRAM 0..3 by HDMA, which is why the code is the
//   colour index and no RGB is produced here.
//
// TIMING
//   228 colour clocks per line: 0..67 horizontal blank, 68..227 visible (160
//   pixels).  Everything is registered on CLK with the TIA_CE enable, so a
//   colour clock is one enabled cycle.  VID_CODE/VID_VISIBLE are REGISTERED
//   and computed one colour clock ahead, so during the enabled cycle that
//   carries colour clock H they already hold the answer for H.  That keeps the
//   priority cone one register deep (the house rule for 96 MHz) at the price
//   of register writes taking visual effect one colour clock later than an
//   idealised model; the CPU writes once every three colour clocks, and the
//   real chip has a comparable output pipeline, so the difference is not
//   observable in a kernel.
//
// POSITIONING
//   The real object counters advance only while horizontal blank is off, so
//   they make exactly one 160-clock revolution per line.  That is modelled
//   directly: each object keeps its start pixel in 0..159, RESPx computes it
//   from the colour clock carrying the write, and HMOVE adds -8..+7 to it once
//   per strobe.  The RESPx position is a single clamped expression,
//   max(BLANK, (clock - 68) + DELAY), whose four constants are all measured on
//   hardware -- there is no separate in-blank case, the clamp is it.
//
// KNOWN v0 SIMPLIFICATIONS (deliberate, listed so they are not mistaken for
// bugs): no polynomial position counters (comparator model, so the Cosmic Ark
// starfield style quirks are absent); a copy whose start wraps past the end of
// the line reappears on the SAME line instead of the next; HMOVE issued
// outside blank is applied at the following line start; RSYNC just restarts
// the horizontal counter; INPT0-3 read as zero (grounded, no paddles).

`timescale 1ns / 1ps

module a26_tia (
  input        CLK,
  input        RST,
  input        TIA_CE,           // one colour clock
  input        CPU_CE,           // one CPU cycle (colour clock / 3)

  // CPU side.  WR_STROBE/RD_STROBE are single-CLK pulses already qualified by
  // CPU_CE upstream (level & CE, never an edge on WE -- see the core contract).
  input        CS,
  input  [5:0] A,
  input  [7:0] DIN,
  output reg [7:0] DOUT,         // registered in CLK, stable long before RDY
  input        WR_STROBE,
  input        RD_STROBE,

  output       WSYNC_HALT,       // 1 = hold the CPU; drops on the last clock

  input  [1:0] INPT45,           // {INPT5, INPT4} live level, 0 = pressed

  // video
  output       VID_VISIBLE,
  output [1:0] VID_CODE,
  output       LINE_START,       // colour clock 0
  output       LINE_VIS_START,   // colour clock 68

  // live registers (the video stage snapshots the colours at LINE_VIS_START)
  output [6:0] COLUBK,
  output [6:0] COLUPF,
  output [6:0] COLUP0,
  output [6:0] COLUP1,
  output [7:0] CTRLPF,
  output       VSYNC_EDGE,       // rising edge of VSYNC bit1 = end of frame
  output       VBLANK_ON,

  // audio register file (latches only; the sound module owns the generators)
  output [3:0] AUDC0,
  output [3:0] AUDC1,
  output [4:0] AUDF0,
  output [4:0] AUDF1,
  output [3:0] AUDV0,
  output [3:0] AUDV1,

  // VSYNC as a LEVEL, appended so nothing that already connects by name has to
  // change and an instantiation may leave it open.  The frame origin defined
  // by the core contract is "first line with VBLANK off after the VSYNC EDGE",
  // an approximation that holds because every kernel keeps VBLANK asserted for
  // the whole VSYNC.  With this port a consumer can make it exact -- wait for
  // the level to fall instead of counting from the edge -- without any change
  // to this module.
  output       VSYNC_LEVEL
);

  // ---------------------------------------------------------------- geometry
  localparam [7:0] HTOTAL   = 8'd227;   // last colour clock of a line
  localparam [7:0] HBLANK   = 8'd68;    // first visible colour clock
  localparam [7:0] VISIBLE  = 8'd160;

  // RESPx pipeline offsets.
  //
  // The DELAY values are where an object starts when RESPx is strobed inside
  // the visible region, counted from the colour clock that carries the write.
  //
  // RESP_P_DELAY is MEASURED, not guessed.  Two hardware captures of the same
  // kernel with two different values of it (5 and 6) put both players at a
  // constant -3 and -2 pixels from the reference frame, on every one of the 96
  // lines and for both P0 and P1 -- one pixel of movement per unit, so the
  // value that lands on the reference is 8.  The captures also confirmed the
  // model around it: the kernel's two strobes land on colour clocks 78 and
  // 135, and (clock - 68) + DELAY reproduced all four measured positions
  // exactly.  Part of the 8 is the strobe being presented at the FIRST colour
  // clock of the CPU cycle while the write completes at the last, which is a
  // property of the bus, not of the object: it applies to every register
  // write, so the player/missile difference below is unaffected by it.
  //
  // RESP_M_DELAY is measured too, by a second kernel built to strobe
  // RESM0/RESM1/RESBL with the missiles and the ball enabled: six points
  // across clocks 78..198 all land on 7, and the BALL follows this missile
  // constant rather than the player one.
  //
  // The BLANK values are the CLAMP of that same formula, not a separate case:
  // sweeping the strobe clock across the blank boundary showed the linear term
  // already in force at clock 66 and the clamp holding at 63, which is exactly
  // where (clock - 68) + DELAY crosses each of them.  See the RESPx expression
  // below -- there is no boundary test any more.
  localparam [7:0] RESP_P_BLANK = 8'd3;   // measured; also the clamp of the formula
  localparam [7:0] RESP_M_BLANK = 8'd2;   // measured; missiles AND ball
  localparam [8:0] RESP_P_DELAY = 9'd8;   // measured on hardware
  localparam [8:0] RESP_M_DELAY = 9'd7;   // measured on hardware (missiles + ball)

  localparam [7:0] COMB_END = 8'd8;     // HMOVE comb covers pixels 0..7

  // ------------------------------------------------------------ write decode
  localparam [5:0] W_VSYNC  = 6'h00, W_VBLANK = 6'h01, W_WSYNC  = 6'h02,
                   W_RSYNC  = 6'h03, W_NUSIZ0 = 6'h04, W_NUSIZ1 = 6'h05,
                   W_COLUP0 = 6'h06, W_COLUP1 = 6'h07, W_COLUPF = 6'h08,
                   W_COLUBK = 6'h09, W_CTRLPF = 6'h0A, W_REFP0  = 6'h0B,
                   W_REFP1  = 6'h0C, W_PF0    = 6'h0D, W_PF1    = 6'h0E,
                   W_PF2    = 6'h0F, W_RESP0  = 6'h10, W_RESP1  = 6'h11,
                   W_RESM0  = 6'h12, W_RESM1  = 6'h13, W_RESBL  = 6'h14,
                   W_AUDC0  = 6'h15, W_AUDC1  = 6'h16, W_AUDF0  = 6'h17,
                   W_AUDF1  = 6'h18, W_AUDV0  = 6'h19, W_AUDV1  = 6'h1A,
                   W_GRP0   = 6'h1B, W_GRP1   = 6'h1C, W_ENAM0  = 6'h1D,
                   W_ENAM1  = 6'h1E, W_ENABL  = 6'h1F, W_HMP0   = 6'h20,
                   W_HMP1   = 6'h21, W_HMM0   = 6'h22, W_HMM1   = 6'h23,
                   W_HMBL   = 6'h24, W_VDELP0 = 6'h25, W_VDELP1 = 6'h26,
                   W_VDELBL = 6'h27, W_RESMP0 = 6'h28, W_RESMP1 = 6'h29,
                   W_HMOVE  = 6'h2A, W_HMCLR  = 6'h2B, W_CXCLR  = 6'h2C;

  wire wr = CS & WR_STROBE;
  wire wr_hit = wr & (A <= W_CXCLR);

  // ------------------------------------------------------------- register file
  reg        vsync_r, vsync_edge_r;
  reg  [7:0] vblank_r;                  // bit1 blank, bit6 input latch, bit7 dump
  reg  [6:0] colup0_r, colup1_r, colupf_r, colubk_r;
  reg  [7:0] ctrlpf_r;
  reg  [5:0] nusiz0_r, nusiz1_r;        // [2:0] copies/scale, [5:4] missile size
  reg        refp0_r, refp1_r;
  reg  [3:0] pf0_r;                     // high nibble of PF0 only
  reg  [7:0] pf1_r, pf2_r;
  reg  [7:0] grp0_new, grp0_old, grp1_new, grp1_old;
  reg        enam0_r, enam1_r, enabl_new, enabl_old;
  reg  [3:0] hmp0_r, hmp1_r, hmm0_r, hmm1_r, hmbl_r;
  reg        vdelp0_r, vdelp1_r, vdelbl_r;
  reg        resmp0_r, resmp1_r;
  reg  [3:0] audc0_r, audc1_r, audv0_r, audv1_r;
  reg  [4:0] audf0_r, audf1_r;

  reg  [7:0] pos_p0, pos_p1, pos_m0, pos_m1, pos_bl;

  reg  [7:0] hc;                        // colour clock counter
  reg        wsync_r;
  reg        hmove_defer;               // HMOVE seen outside blank
  reg        comb_r;                    // 8-pixel comb armed for this line
  reg        inpt4_lat, inpt5_lat;

  reg  [1:0] vid_code_r;
  reg        vid_vis_r;

  assign COLUBK     = colubk_r;
  assign COLUPF     = colupf_r;
  assign COLUP0     = colup0_r;
  assign COLUP1     = colup1_r;
  assign CTRLPF     = ctrlpf_r;
  assign VSYNC_EDGE = vsync_edge_r;
  assign VSYNC_LEVEL = vsync_r;
  assign VBLANK_ON  = vblank_r[1];
  assign AUDC0      = audc0_r;
  assign AUDC1      = audc1_r;
  assign AUDF0      = audf0_r;
  assign AUDF1      = audf1_r;
  assign AUDV0      = audv0_r;
  assign AUDV1      = audv1_r;
  assign VID_CODE   = vid_code_r;
  assign VID_VISIBLE = vid_vis_r;
  assign WSYNC_HALT = wsync_r;

  assign LINE_START     = TIA_CE & (hc == 8'd0);
  assign LINE_VIS_START = TIA_CE & (hc == HBLANK);

  // ------------------------------------------------------- next colour clock
  // Everything visual is evaluated for the clock we are about to enter, so the
  // registered outputs are already correct when that clock arrives.
  wire [7:0] hc_n  = (hc == HTOTAL) ? 8'd0 : (hc + 8'd1);
  wire       vis_n = (hc_n >= HBLANK);
  wire [7:0] px_n  = hc_n - HBLANK;                  // 0..159 when vis_n
  wire       comb_n = comb_r & (px_n < COMB_END);    // HMOVE comb -> code 0

  // ------------------------------------------------------------- RESPx value
  // ONE expression for the whole line (see the header): the linear term
  // counted from the colour clock that CARRIES the write (hc, not the clock
  // being evaluated for output), CLAMPED at the class minimum.
  //
  //   pos = max(BLANK_class, (hc - 68) + DELAY_class)
  //
  // The clamp IS the blank behaviour, so there is no boundary test: the object
  // counter is frozen while blanking, and a strobe early enough can only push
  // the object to the leftmost position its class can reach.  Both classes
  // cross over at clock 63, which is why a guard at 68 was wrong -- a strobe
  // at 64..67 already follows the linear term.
  wire signed [9:0] lin_p = $signed({2'b00, hc}) - $signed({2'b00, HBLANK})
                          + $signed({1'b0, RESP_P_DELAY});
  wire signed [9:0] lin_m = $signed({2'b00, hc}) - $signed({2'b00, HBLANK})
                          + $signed({1'b0, RESP_M_DELAY});
  wire signed [9:0] clp_p = (lin_p < $signed({2'b00, RESP_P_BLANK}))
                          ? $signed({2'b00, RESP_P_BLANK}) : lin_p;
  wire signed [9:0] clp_m = (lin_m < $signed({2'b00, RESP_M_BLANK}))
                          ? $signed({2'b00, RESP_M_BLANK}) : lin_m;
  wire [7:0] resp_p = (clp_p >= $signed({2'b00, VISIBLE}))
                    ? (clp_p[7:0] - VISIBLE) : clp_p[7:0];
  wire [7:0] resp_m = (clp_m >= $signed({2'b00, VISIBLE}))
                    ? (clp_m[7:0] - VISIBLE) : clp_m[7:0];

  // --------------------------------------------------------- horizontal move
  // The nibble is a signed count of colour clocks to move LEFT ($70 = 7 left,
  // $90 = 7 right), so the new position is pos - sign_extend(nibble), wrapped
  // into 0..159.
  function [7:0] hmove_apply;
    input [7:0] p;
    input [3:0] hm;
    reg signed [9:0] np;
    begin
      np = $signed({2'b00, p}) - $signed({{6{hm[3]}}, hm});
      if (np < 0)                          hmove_apply = np[7:0] + VISIBLE;
      else if (np >= $signed({2'b0, VISIBLE})) hmove_apply = np[7:0] - VISIBLE;
      else                                 hmove_apply = np[7:0];
    end
  endfunction

  // One instance of the adder per object, shared by the two places that apply
  // motion (in-blank strobe and the deferred one at line start).
  wire [7:0] hm_p0 = hmove_apply(pos_p0, hmp0_r);
  wire [7:0] hm_p1 = hmove_apply(pos_p1, hmp1_r);
  wire [7:0] hm_m0 = hmove_apply(pos_m0, hmm0_r);
  wire [7:0] hm_m1 = hmove_apply(pos_m1, hmm1_r);
  wire [7:0] hm_bl = hmove_apply(pos_bl, hmbl_r);

  // --------------------------------------------------------------- playfield
  // Canonical dot order: PF0 D4..D7, PF1 D7..D0, PF2 D0..D7.
  wire [19:0] pf_bits = {pf2_r[7], pf2_r[6], pf2_r[5], pf2_r[4],
                         pf2_r[3], pf2_r[2], pf2_r[1], pf2_r[0],
                         pf1_r[0], pf1_r[1], pf1_r[2], pf1_r[3],
                         pf1_r[4], pf1_r[5], pf1_r[6], pf1_r[7],
                         pf0_r[3], pf0_r[2], pf0_r[1], pf0_r[0]};
  // (index 0 = leftmost dot: pf_bits[0] = pf0_r[0] = PF0 D4)

  wire        pf_right = (px_n >= 8'd80);
  wire [7:0]  px_half  = pf_right ? (px_n - 8'd80) : px_n;
  wire [4:0]  dot_raw  = px_half[6:2];                     // 0..19
  wire [4:0]  dot      = (pf_right & ctrlpf_r[0]) ? (5'd19 - dot_raw) : dot_raw;
  wire        pf_pix   = pf_bits[dot];

  // ------------------------------------------------------------- ball/player
  wire [1:0] bl_size_l = ctrlpf_r[5:4];
  wire [7:0] bl_width  = 8'd1 << bl_size_l;
  wire       enabl_disp = vdelbl_r ? enabl_old : enabl_new;
  wire [7:0] d_bl = (px_n >= pos_bl) ? (px_n - pos_bl) : (px_n + VISIBLE - pos_bl);
  wire       bl_pix = enabl_disp & (d_bl < bl_width);

  // Player/missile copy pattern from NUSIZ[2:0]:
  //   0 one    1 two @16   2 two @32   3 three @16   4 two @64
  //   5 one x2 6 three @32 7 one x4
  function [1:0] pl_shift;      // log2 of the player scale
    input [2:0] n;
    begin
      case (n)
        3'd5:    pl_shift = 2'd1;
        3'd7:    pl_shift = 2'd2;
        default: pl_shift = 2'd0;
      endcase
    end
  endfunction

  function [7:0] cp_off1;       // second copy offset, 0 = none
    input [2:0] n;
    begin
      case (n)
        3'd1, 3'd3: cp_off1 = 8'd16;
        3'd2, 3'd6: cp_off1 = 8'd32;
        3'd4:       cp_off1 = 8'd64;
        default:    cp_off1 = 8'd0;
      endcase
    end
  endfunction

  function [7:0] cp_off2;       // third copy offset, 0 = none
    input [2:0] n;
    begin
      case (n)
        3'd3:    cp_off2 = 8'd32;
        3'd6:    cp_off2 = 8'd64;
        default: cp_off2 = 8'd0;
      endcase
    end
  endfunction

  // Player hit: the copies never overlap (spacing 16 > width 8, and the scaled
  // modes have a single copy), so the three windows can be tested in parallel
  // and the graphics bit index taken from whichever matched.
  //   d1/d2 wrap when the copy is not reached yet; d in 0..159 and the offsets
  //   are <= 64, so the wrapped value is always >= 192 and never inside the
  //   window.
  wire [1:0] sh0 = pl_shift(nusiz0_r[2:0]);
  wire [1:0] sh1 = pl_shift(nusiz1_r[2:0]);
  wire [7:0] pw0 = 8'd8 << sh0;
  wire [7:0] pw1 = 8'd8 << sh1;
  wire [7:0] o0a = cp_off1(nusiz0_r[2:0]), o0b = cp_off2(nusiz0_r[2:0]);
  wire [7:0] o1a = cp_off1(nusiz1_r[2:0]), o1b = cp_off2(nusiz1_r[2:0]);

  wire [7:0] d_p0  = (px_n >= pos_p0) ? (px_n - pos_p0) : (px_n + VISIBLE - pos_p0);
  wire [7:0] d_p0a = d_p0 - o0a;
  wire [7:0] d_p0b = d_p0 - o0b;
  wire       h_p00 = (d_p0  < pw0);
  wire       h_p0a = (o0a != 8'd0) & (d_p0a < pw0);
  wire       h_p0b = (o0b != 8'd0) & (d_p0b < pw0);
  wire [7:0] ds_p0 = h_p00 ? d_p0 : h_p0a ? d_p0a : d_p0b;
  wire [2:0] bi_p0 = ds_p0[4:0] >> sh0;
  wire [7:0] gr_p0 = vdelp0_r ? grp0_old : grp0_new;
  wire       p0_pix = (h_p00 | h_p0a | h_p0b) &
                      (refp0_r ? gr_p0[bi_p0] : gr_p0[3'd7 - bi_p0]);

  wire [7:0] d_p1  = (px_n >= pos_p1) ? (px_n - pos_p1) : (px_n + VISIBLE - pos_p1);
  wire [7:0] d_p1a = d_p1 - o1a;
  wire [7:0] d_p1b = d_p1 - o1b;
  wire       h_p10 = (d_p1  < pw1);
  wire       h_p1a = (o1a != 8'd0) & (d_p1a < pw1);
  wire       h_p1b = (o1b != 8'd0) & (d_p1b < pw1);
  wire [7:0] ds_p1 = h_p10 ? d_p1 : h_p1a ? d_p1a : d_p1b;
  wire [2:0] bi_p1 = ds_p1[4:0] >> sh1;
  wire [7:0] gr_p1 = vdelp1_r ? grp1_old : grp1_new;
  wire       p1_pix = (h_p10 | h_p1a | h_p1b) &
                      (refp1_r ? gr_p1[bi_p1] : gr_p1[3'd7 - bi_p1]);

  // Missiles share the player copy pattern; width is NUSIZ[5:4].  A missile
  // locked to its player (RESMPx) is never drawn.
  wire [7:0] mw0 = 8'd1 << nusiz0_r[5:4];
  wire [7:0] mw1 = 8'd1 << nusiz1_r[5:4];
  wire [7:0] d_m0  = (px_n >= pos_m0) ? (px_n - pos_m0) : (px_n + VISIBLE - pos_m0);
  wire [7:0] d_m0a = d_m0 - o0a;
  wire [7:0] d_m0b = d_m0 - o0b;
  wire       m0_on = enam0_r & ~resmp0_r;
  wire       m0_pix = m0_on & ((d_m0 < mw0) |
                               ((o0a != 8'd0) & (d_m0a < mw0)) |
                               ((o0b != 8'd0) & (d_m0b < mw0)));
  wire [7:0] d_m1  = (px_n >= pos_m1) ? (px_n - pos_m1) : (px_n + VISIBLE - pos_m1);
  wire [7:0] d_m1a = d_m1 - o1a;
  wire [7:0] d_m1b = d_m1 - o1b;
  wire       m1_on = enam1_r & ~resmp1_r;
  wire       m1_pix = m1_on & ((d_m1 < mw1) |
                               ((o1a != 8'd0) & (d_m1a < mw1)) |
                               ((o1b != 8'd0) & (d_m1b < mw1)));

  // Missile centring while RESMPx is set: the player is 8*scale wide, so its
  // centre sits 4*scale-1 pixels in (3 / 7 / 15).  This is a CONTINUOUS lock,
  // not a one-shot copy at the write.
  wire [7:0] mlock0 = (pos_p0 + (8'd4 << sh0) - 8'd1 >= VISIBLE)
                    ? (pos_p0 + (8'd4 << sh0) - 8'd1 - VISIBLE)
                    : (pos_p0 + (8'd4 << sh0) - 8'd1);
  wire [7:0] mlock1 = (pos_p1 + (8'd4 << sh1) - 8'd1 >= VISIBLE)
                    ? (pos_p1 + (8'd4 << sh1) - 8'd1 - VISIBLE)
                    : (pos_p1 + (8'd4 << sh1) - 8'd1);

  // ----------------------------------------------------------- priority/code
  wire pfp   = ctrlpf_r[2];
  wire score = ctrlpf_r[1] & ~pfp;
  wire obj0  = p0_pix | m0_pix;
  wire obj1  = p1_pix | m1_pix;

  // SCORE MODE PRIORITY -- measured, not assumed.
  //
  // Score mode does not merely recolour the playfield: it feeds the playfield
  // signal into the colour enable of the player whose colour it borrows, so
  // the scored playfield ENTERS THE PRIORITY CHAIN AT THAT PLAYER'S LEVEL --
  // left half at P0's, right half at P1's.  Since P0 outranks P1, a scored
  // playfield in the left half beats player 1, which a plain "recolour only"
  // model gets backwards.
  //
  // The evidence is a hardware capture of a kernel whose two halves differ in
  // NOTHING but CTRLPF ($00 vs $02) -- same GRP1, same NUSIZ, same RESPx.  On
  // the pixels where player 1 overlaps a playfield dot in the left half, the
  // reference frame flips from player 1 to the playfield colour exactly when
  // score mode turns on, while a pixel with player 1 alone keeps its colour in
  // both halves (so the sprite itself is unchanged).
  //
  // The right-half half of the rule was settled by a second kernel that puts
  // P0 on top of a scored playfield there: P0 WINS, so the playfield really is
  // sitting at the borrowed player's level and not above the players as a
  // class.  Both halves of the model are measured.
  wire pf_sc0 = pf_pix & score & ~pf_right;   // scored playfield at P0's level
  wire pf_sc1 = pf_pix & score &  pf_right;   // scored playfield at P1's level

  reg [1:0] code_n;
  always @(*) begin
    if (pfp) begin
      // PFP=1 gates score off, so the playfield is plain COLUPF here and the
      // whole playfield/ball class sits above the players.
      if      (pf_pix) code_n = 2'd1;
      else if (bl_pix) code_n = 2'd1;
      else if (obj0)   code_n = 2'd2;
      else if (obj1)   code_n = 2'd3;
      else             code_n = 2'd0;
    end else begin
      if      (obj0 | pf_sc0) code_n = 2'd2;
      else if (obj1 | pf_sc1) code_n = 2'd3;
      // only reachable with score off: an ordinary playfield sits below the
      // players, and the ball keeps COLUPF even in score mode
      else if (pf_pix)        code_n = 2'd1;
      else if (bl_pix)        code_n = 2'd1;
      else                    code_n = 2'd0;
    end
  end

  // ------------------------------------------------------------- collisions
  // Bit map of cx_r (the read mux below shifts the pair into D7:D6):
  //  0 M0-P1  1 M0-P0  2 M1-P0  3 M1-P1  4 P0-PF  5 P0-BL  6 P1-PF  7 P1-BL
  //  8 M0-PF  9 M0-BL 10 M1-PF 11 M1-BL 12 BL-PF 13 P0-P1 14 M0-M1
  reg [14:0] cx_r;
  wire cx_en = vis_n & ~comb_n;         // no objects during the comb
  wire [14:0] cx_hit = {
    m0_pix & m1_pix,                    // 14
    p0_pix & p1_pix,                    // 13
    bl_pix & pf_pix,                    // 12
    m1_pix & bl_pix,                    // 11
    m1_pix & pf_pix,                    // 10
    m0_pix & bl_pix,                    //  9
    m0_pix & pf_pix,                    //  8
    p1_pix & bl_pix,                    //  7
    p1_pix & pf_pix,                    //  6
    p0_pix & bl_pix,                    //  5
    p0_pix & pf_pix,                    //  4
    m1_pix & p1_pix,                    //  3
    m1_pix & p0_pix,                    //  2
    m0_pix & p0_pix,                    //  1
    m0_pix & p1_pix};                   //  0

  // ------------------------------------------------------------- read port
  reg [7:0] rd_mux;
  always @(*) begin
    case (A[3:0])
      4'h0: rd_mux = {cx_r[0],  cx_r[1],  6'd0};   // CXM0P
      4'h1: rd_mux = {cx_r[2],  cx_r[3],  6'd0};   // CXM1P
      4'h2: rd_mux = {cx_r[4],  cx_r[5],  6'd0};   // CXP0FB
      4'h3: rd_mux = {cx_r[6],  cx_r[7],  6'd0};   // CXP1FB
      4'h4: rd_mux = {cx_r[8],  cx_r[9],  6'd0};   // CXM0FB
      4'h5: rd_mux = {cx_r[10], cx_r[11], 6'd0};   // CXM1FB
      4'h6: rd_mux = {cx_r[12], 1'b0,     6'd0};   // CXBLPF
      4'h7: rd_mux = {cx_r[13], cx_r[14], 6'd0};   // CXPPMM
      4'hC: rd_mux = {vblank_r[6] ? inpt4_lat : INPT45[0], 7'd0};
      4'hD: rd_mux = {vblank_r[6] ? inpt5_lat : INPT45[1], 7'd0};
      default: rd_mux = 8'h00;                     // INPT0-3 grounded
    endcase
  end

  // ------------------------------------------------------------------- core
  integer i;
  always @(posedge CLK) begin
    DOUT <= rd_mux;
    vsync_edge_r <= 1'b0;

    if (RST) begin
      hc <= 8'd0;
      vsync_r <= 1'b0; vblank_r <= 8'd0;
      colup0_r <= 7'd0; colup1_r <= 7'd0; colupf_r <= 7'd0; colubk_r <= 7'd0;
      ctrlpf_r <= 8'd0; nusiz0_r <= 6'd0; nusiz1_r <= 6'd0;
      refp0_r <= 1'b0; refp1_r <= 1'b0;
      pf0_r <= 4'd0; pf1_r <= 8'd0; pf2_r <= 8'd0;
      grp0_new <= 8'd0; grp0_old <= 8'd0; grp1_new <= 8'd0; grp1_old <= 8'd0;
      enam0_r <= 1'b0; enam1_r <= 1'b0; enabl_new <= 1'b0; enabl_old <= 1'b0;
      hmp0_r <= 4'd0; hmp1_r <= 4'd0; hmm0_r <= 4'd0; hmm1_r <= 4'd0; hmbl_r <= 4'd0;
      vdelp0_r <= 1'b0; vdelp1_r <= 1'b0; vdelbl_r <= 1'b0;
      resmp0_r <= 1'b0; resmp1_r <= 1'b0;
      audc0_r <= 4'd0; audc1_r <= 4'd0; audv0_r <= 4'd0; audv1_r <= 4'd0;
      audf0_r <= 5'd0; audf1_r <= 5'd0;
      pos_p0 <= 8'd0; pos_p1 <= 8'd0; pos_m0 <= 8'd0; pos_m1 <= 8'd0; pos_bl <= 8'd0;
      wsync_r <= 1'b0; hmove_defer <= 1'b0; comb_r <= 1'b0;
      inpt4_lat <= 1'b1; inpt5_lat <= 1'b1;
      cx_r <= 15'd0; vid_code_r <= 2'd0; vid_vis_r <= 1'b0;
    end else begin

      // ---- colour clock ----
      if (TIA_CE) begin
        hc <= hc_n;

        // WSYNC releases on the last clock of the line so the first CPU cycle
        // after it lands on colour clock 0 (76 CPU cycles per line).
        if (hc == HTOTAL) wsync_r <= 1'b0;

        // line start: apply a deferred (late) HMOVE and re-arm the comb
        if (hc_n == 8'd0) begin
          comb_r <= hmove_defer;
          if (hmove_defer) begin
            pos_p0 <= hm_p0; pos_p1 <= hm_p1;
            pos_m0 <= hm_m0; pos_m1 <= hm_m1;
            pos_bl <= hm_bl;
          end
          hmove_defer <= 1'b0;
        end

        // missile lock follows the player continuously while RESMPx is set
        if (resmp0_r) pos_m0 <= mlock0;
        if (resmp1_r) pos_m1 <= mlock1;

        // collisions come from the raw object bits, before the priority mux
        if (cx_en) cx_r <= cx_r | cx_hit;

        vid_vis_r  <= vis_n & ~vblank_r[1];
        vid_code_r <= (vis_n & ~vblank_r[1] & ~comb_n) ? code_n : 2'd0;

        // input latches: transparent while VBLANK bit6 is clear, sticky low
        // (pressed) while it is set
        if (~vblank_r[6]) begin
          inpt4_lat <= 1'b1;
          inpt5_lat <= 1'b1;
        end else begin
          inpt4_lat <= inpt4_lat & INPT45[0];
          inpt5_lat <= inpt5_lat & INPT45[1];
        end
      end

      // ---- register writes ----
      if (wr_hit) begin
        case (A)
          W_VSYNC: begin
            if (DIN[1] & ~vsync_r) vsync_edge_r <= 1'b1;
            vsync_r <= DIN[1];
          end
          W_VBLANK: vblank_r <= DIN;
          W_WSYNC:  wsync_r  <= 1'b1;          // wins over the clear above
          W_RSYNC:  hc       <= 8'd0;
          W_NUSIZ0: nusiz0_r <= DIN[5:0];
          W_NUSIZ1: nusiz1_r <= DIN[5:0];
          W_COLUP0: colup0_r <= DIN[7:1];
          W_COLUP1: colup1_r <= DIN[7:1];
          W_COLUPF: colupf_r <= DIN[7:1];
          W_COLUBK: colubk_r <= DIN[7:1];
          W_CTRLPF: ctrlpf_r <= DIN;
          W_REFP0:  refp0_r  <= DIN[3];
          W_REFP1:  refp1_r  <= DIN[3];
          W_PF0:    pf0_r    <= DIN[7:4];
          W_PF1:    pf1_r    <= DIN;
          W_PF2:    pf2_r    <= DIN;
          W_RESP0:  pos_p0   <= resp_p;
          W_RESP1:  pos_p1   <= resp_p;
          W_RESM0:  if (~resmp0_r) pos_m0 <= resp_m;
          W_RESM1:  if (~resmp1_r) pos_m1 <= resp_m;
          W_RESBL:  pos_bl   <= resp_m;
          W_AUDC0:  audc0_r  <= DIN[3:0];
          W_AUDC1:  audc1_r  <= DIN[3:0];
          W_AUDF0:  audf0_r  <= DIN[4:0];
          W_AUDF1:  audf1_r  <= DIN[4:0];
          W_AUDV0:  audv0_r  <= DIN[3:0];
          W_AUDV1:  audv1_r  <= DIN[3:0];
          // Vertical delay: writing one player latches the OTHER one's delayed
          // copy, and writing GRP1 also latches the ball's.
          W_GRP0: begin grp0_new <= DIN; grp1_old <= grp1_new; end
          W_GRP1: begin grp1_new <= DIN; grp0_old <= grp0_new;
                        enabl_old <= enabl_new; end
          W_ENAM0:  enam0_r   <= DIN[1];
          W_ENAM1:  enam1_r   <= DIN[1];
          W_ENABL:  enabl_new <= DIN[1];
          W_HMP0:   hmp0_r    <= DIN[7:4];
          W_HMP1:   hmp1_r    <= DIN[7:4];
          W_HMM0:   hmm0_r    <= DIN[7:4];
          W_HMM1:   hmm1_r    <= DIN[7:4];
          W_HMBL:   hmbl_r    <= DIN[7:4];
          W_VDELP0: vdelp0_r  <= DIN[0];
          W_VDELP1: vdelp1_r  <= DIN[0];
          W_VDELBL: vdelbl_r  <= DIN[0];
          W_RESMP0: resmp0_r  <= DIN[1];
          W_RESMP1: resmp1_r  <= DIN[1];
          W_HMOVE: begin
            // Inside blank the motion lands on this line, comb included.
            // Outside it, the approximation is to apply it at the next line
            // start (the real chip would move the objects mid-line).
            if (hc < HBLANK) begin
              pos_p0 <= hm_p0; pos_p1 <= hm_p1;
              pos_m0 <= hm_m0; pos_m1 <= hm_m1;
              pos_bl <= hm_bl;
              comb_r <= 1'b1;
            end else begin
              hmove_defer <= 1'b1;
            end
          end
          W_HMCLR: begin
            hmp0_r <= 4'd0; hmp1_r <= 4'd0;
            hmm0_r <= 4'd0; hmm1_r <= 4'd0; hmbl_r <= 4'd0;
          end
          W_CXCLR: cx_r <= 15'd0;
          default: ;
        endcase
      end
    end
  end

  // RD_STROBE is part of the frozen interface but the TIA has no read side
  // effects (CXCLR is a write, and the RIOT owns INTIM/INSTAT).
  wire _unused = &{1'b0, RD_STROBE, CPU_CE, 1'b0};

endmodule
