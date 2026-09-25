`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// c6_fb.v -- the Game Boy picture as a framebuffer in M9K, read by the SNES as
// 8bpp direct-colour tiles (GBC-CORE-CONTRACT.md section 14, wire $03).
//
// PIXELS IN.  sgb_cpu.v hands over one pixel per LCD dot (PX_*), already in
// CGB colour (BGR555) and with the BG/OBJ priority resolved -- the image an LCD
// would show.  Here it is quantised to the SNES direct-colour byte BBGGGRRR
// (Book I A-17: R = {D2 D1 D0, CL0, 0}, G = {D5 D4 D3, CL1, 0}, B = {D7 D6, CL2,
// 0, 0}) with the three palette bits CL of the CURRENT FRAME (section 14.4,
// half-up, saturating at the top), and written into the FB.
//
// STORAGE.  160x144 = 360 cells of 8x8; one 64-bit word = one line of one cell
// (8 pixels, chunky: the leftmost pixel byte in [63:56]).  The cells are split
// by column parity into two memories, even and odd, 180 cells x 8 lines = 1440
// words each: that is what lets a stretched read (below) fetch the two
// neighbouring cells a 5-pixel group can straddle in ONE cycle, and it costs no
// block RAM -- 2 x (1440 x 64) packs into the same 4 x 3 M9K per memory as
// 1 x (2880 x 64) would into 4 x 6.  Each memory is TRUE dual port: port A is
// the PPU side (read the old word, then write the new one: the read-before-
// write is what the dirty bits are made of), port B is the SNES view.
//
// TWO RULES FOR ANY NEW RAM IN THIS CORE (learned on the pilot fit, PILOT-C6.md):
//   * a wide RAM deeper than 512 words gets maximum_depth = 512, or Quartus
//     builds it out of 1K x 8 blocks and spends 16 M9K where 12 suffice (the
//     first pilot fit asked for 59 of the 56 blocks);
//   * a small array (32 entries) that is read by index gets ramstyle = "logic",
//     or each one lands in an M9K of its own.  M9K is the resource this core
//     runs out of (54 of 56 with the FB), not logic.
//
// SNES VIEW, bank $E4 (section 14.3).  Tile-linear 8bpp, 64 B per tile, tile
// row stride 32:
//   $E4: {tr[4:0], tc[4:0], b[5:0]}   tr = tile row 0..17, tc = tile column,
//   b = byte of the 8bpp tile (b[3:1] = line, plane = {b[5:4], b[0]}, bit 7 =
//   the leftmost pixel).  Unstretched: tc 0..19 (160 px), tc 20..31 read $00.
//   Stretched (CTL bit 2): tc 0..31 = 256 px, every 5 source pixels become 8
//   with the symmetric pattern [2,1,2,1,2] (the a26_video.v one) -- output pixel
//   j of column tc is source pixel 5*tc + {0,0,1,2,2,3,4,4}[j].  tr >= 18 reads
//   $00.
// Read pipeline: address -> RAM (cycle N), the plane pick of each memory's
// port B word registered (N+1), the left/right swap and the stretch into the
// bridge's view_data_r (N+2), DATA_OUT (N+3): THREE cycles.  A register
// between the M9K and the view mux is what closed 84 MHz with the chunky +
// stretch read (the unregistered M9K clock-to-out of ~3.3 ns plus the whole
// pick missed it by 0.5-1.2 ns in every fit that kept two cycles).
//
// FREEZING, PER CELL ROW (section 14.6).  A cell row r (8 scanlines, 20 cells)
// is written while it is OPEN; it FREEZES on the cycle the compare of its last
// word (x = 159, y = 8r+7) updates the dirty bitmap, and it only THAWS on the
// first pixel of its first scanline (x = 0, y = 8r) after the player has sent
// ROW_DONE(r) -- with HOLD in force it stays frozen.  Pixels of a frozen row
// are discarded, so a frozen row always holds the eight scanlines of ONE
// frame (contract invariant 39); a beam that reaches (0, 8r) with the row
// still frozen loses that frame of that row (c6_rows_dropped).
//
// DIRTY.  360 bits, one per 8x8 cell, set when a line written into the cell
// DIFFERS from the word it replaces (not merely "was written"); the compare is
// registered before it marks.  Cleared per row by ROW_DONE (section 14.2), set
// to all ones when FB_EN takes effect.
//
// CL, PER FRAME BY MAJORITY (section 14.6.5-6).  Every visible pixel votes per
// channel on whether the offset level or the base level of the direct-colour
// grid is nearer; at the vblank the candidate is the sign of the three sums,
// and after C6_CL_HYST frames of the same candidate the switch happens at the
// next LY=0 -- only with every row carrying the current id (so at most two
// quantisations are ever alive in the FB) and with CL_LOCK clear.
//
// CONTROL, $EF0006 (CTL_WR): b0 HOLD, b1 FB_EN, b2 STRETCH_H, b3 CL_LOCK.
// HOLD and FB_EN take effect at the next LY=0 (TAP_LY0); FB_EN 0->1 restarts
// everything below.  $EF0007 (ROWDONE_WR): the row index 0..17, or $FF for all
// frozen rows; anything else, or a row that is not frozen, is counted in
// c6_rowdone_bad and ignored.
//
// STATUS (section 14.5, bytes +58..+91 of the status block, all LIVE):
//   +58 flags   +59 frames   +5A..5C ROW_FROZEN   +5D CL   +5E rows_dropped
//   +60..8C dirty[359:0]   +8D..8F ROW_CLID   +90..91 COLW_N (from the bridge)
//////////////////////////////////////////////////////////////////////////////////
`include "config.vh"

module c6_fb(
  input             CLK,
  input             RST,

  input             PX_VALID,
  input      [7:0]  PX_X,
  input      [7:0]  PX_Y,
  input      [14:0] PX_BGR,
  input             TAP_LY0,       // first dot of mode 2 on line 0
  input             TAP_VBLANK,    // entry into LY=144

  input             CTL_WR,        // $EF0006
  input             ROWDONE_WR,    // $EF0007
  input      [7:0]  WR_DATA,

  input             SEL,           // stage 0: bank $E4 on the bus
  input      [15:0] VADDR,
  output     [7:0]  VIEW_BYTE,     // stage 2, meaningful two cycles after SEL

  input      [7:0]  STAT_OFF,
  output reg [7:0]  STAT_BYTE,
  output            STAT_HIT,
  input      [15:0] COLW_N,        // published by the bridge (status +90..91)

  // GBDG (contract section 10)
  output     [7:0]  FLAGS,         // = status +58
  output     [7:0]  CL_BYTE,       // = status +5D
  output     [15:0] ROWS_DROPPED,
  output     [15:0] ROWDONE_BAD,
  output     [15:0] CL_SWITCHES,
  output     [15:0] FRAMES
);

//-------------------------------------------------------------------
// control
//-------------------------------------------------------------------
reg         ctl_hold_r, ctl_en_r, ctl_stretch_r, ctl_lock_r;   // as written
reg         hold_eff_r, en_eff_r;                              // in force (LY=0)

// Both stores are registered once on the way in.  They come straight off the
// SNES address decode, and ROW_DONE fans out to 360 dirty bits and 36 row
// flags; one cycle of latency is invisible to the SNES (a store is held for
// many cycles and the player never reads back within one) and keeps that
// decode off the 84 MHz path.
reg         ctl_wr_r, rowdone_wr_r;
reg  [7:0]  wr_data_r;
always @(posedge CLK) begin
  ctl_wr_r     <= CTL_WR & ~RST;
  rowdone_wr_r <= ROWDONE_WR & ~RST;
  if (CTL_WR | ROWDONE_WR) wr_data_r <= WR_DATA;
end

always @(posedge CLK) begin
  if (RST) begin
    ctl_hold_r    <= 1'b0;
    ctl_en_r      <= 1'b0;
    ctl_stretch_r <= 1'b0;
    ctl_lock_r    <= 1'b0;
  end
  else if (ctl_wr_r) begin
    ctl_hold_r    <= wr_data_r[0];
    ctl_en_r      <= wr_data_r[1];
    ctl_stretch_r <= wr_data_r[2];
    ctl_lock_r    <= wr_data_r[3];
  end
end

// FB_EN 0->1 taking effect restarts the C6; 1->0 opens every row and leaves
// the FB as it is.  Both, like HOLD, only ever change at LY=0, so a cell row is
// never written half under one setting and half under another.
wire        c6_reset = TAP_LY0 &  ctl_en_r & ~en_eff_r;
wire        c6_off   = TAP_LY0 & ~ctl_en_r &  en_eff_r;

//-------------------------------------------------------------------
// state
//-------------------------------------------------------------------
reg  [17:0]  row_frozen_r, thaw_pend_r, row_clid_r;
reg  [359:0] dirty_r;
reg  [2:0]   cl_cur_r, cl_prev_r, cl_cand_r;
reg          cl_id_r;
reg  [3:0]   cl_hyst_r;
reg  [15:0]  vr_r, vg_r, vb_r;             // vote sums, two's complement
reg  [15:0]  rows_dropped_r, rowdone_bad_r, cl_switches_r, frames_r;

//-------------------------------------------------------------------
// pixel -> direct colour byte (section 14.4, half-up, CL of this frame)
//-------------------------------------------------------------------
wire [4:0]  r5 = PX_BGR[4:0];
wire [4:0]  g5 = PX_BGR[9:5];
wire [4:0]  b5 = PX_BGR[14:10];
// R3 = min(7, (r5 + 2 - 2*CL0) >> 2), B2 = min(3, (b5 + 4 - 4*CL2) >> 3): the
// adjusted value is at most 33 / 35, so "bit 5 set" is exactly the saturation.
wire [5:0]  r_adj = {1'b0, r5} + 6'd2 - {4'd0, cl_cur_r[0], 1'b0};
wire [5:0]  g_adj = {1'b0, g5} + 6'd2 - {4'd0, cl_cur_r[1], 1'b0};
wire [5:0]  b_adj = {1'b0, b5} + 6'd4 - {3'd0, cl_cur_r[2], 2'b00};
wire [2:0]  r3  = r_adj[5] ? 3'd7 : r_adj[4:2];
wire [2:0]  g3  = g_adj[5] ? 3'd7 : g_adj[4:2];
wire [1:0]  b2  = b_adj[5] ? 2'd3 : b_adj[4:3];
wire [7:0]  px8 = {b2, g3, r3};

//-------------------------------------------------------------------
// the vote (section 14.6.5): +1 the offset level is nearer, -1 the base level
// is nearer, 0 a tie.  R and G step 4 with offset 2 (levels saturate at 28 /
// 30, hence c = 31 votes for the offset); B steps 8 with offset 4 (24 / 28,
// hence c >= 30).
//-------------------------------------------------------------------
function [1:0] vote_rg(input [4:0] c);
  begin
    if (c[1:0] == 2'd0)             vote_rg = 2'b11;   // -1
    else if (c[1:0] == 2'd2 || c == 5'd31) vote_rg = 2'b01;   // +1
    else                            vote_rg = 2'b00;
  end
endfunction
function [1:0] vote_b(input [4:0] c);
  begin
    if (c >= 5'd30)                                   vote_b = 2'b01;
    else if (c[2:0] == 3'd3 || c[2:0] == 3'd4 || c[2:0] == 3'd5) vote_b = 2'b01;
    else if (c[2:0] == 3'd0 || c[2:0] == 3'd1 || c[2:0] == 3'd7) vote_b = 2'b11;
    else                                              vote_b = 2'b00;
  end
endfunction
wire [1:0]  dr = vote_rg(r5);
wire [1:0]  dg = vote_rg(g5);
wire [1:0]  db = vote_b(b5);

//-------------------------------------------------------------------
// write side: one word per 8 pixels, read-before-write on port A
//-------------------------------------------------------------------
wire [4:0]  px_row   = PX_Y[7:3];
wire        px_vis   = PX_VALID & (PX_Y < 8'd144) & (PX_X < 8'd160);
wire        px_first = px_vis & (PX_X == 8'd0) & (PX_Y[2:0] == 3'd0);   // (0, 8r)
wire        px_last  = (PX_X == 8'd159) & (PX_Y[2:0] == 3'd7);           // (159, 8r+7)

// The thaw is decided ON the pixel (0, 8r) so that this pixel already goes in
// (section 14.6.3); HOLD is the value in force, latched at LY=0.
wire        row_is_frozen = row_frozen_r[px_row];
wire        thaw_now = px_first & row_is_frozen & thaw_pend_r[px_row] & ~hold_eff_r;
wire        drop_now = px_first & row_is_frozen & ~thaw_now;
wire        wr_en    = en_eff_r & (~row_is_frozen | thaw_now);

// The word accumulator is also the RAM's write data: the eighth pixel of a
// word lands in acc_r on the same edge that starts the write (wr0), and the
// next pixel -- the only thing that moves acc_r -- is a whole LCD dot (~20
// CLK) away, while the write and its compare are over two cycles later.
reg  [63:0] acc_r;
wire [63:0] acc_nx = {acc_r[55:0], px8};

reg         wr0_r, wr1_r, wr2_r;
reg         wr_last0_r, wr_last1_r, wr_last2_r;
reg         wr_diff_r;
reg  [4:0]  wr_row_r, wr_row2_r;          // cell row 0..17
reg  [4:0]  wr_col_r, wr_col2_r;          // cell column 0..19
reg  [10:0] wr_addr_r;
reg         wr_bank_r;

wire [7:0]  px_rb   = {px_row, 3'b000} + {2'b00, px_row, 1'b0};   // row * 10
wire [7:0]  px_cb   = px_rb + {4'h0, PX_X[7:4]};                    // pair index

wire [63:0] q_a0, q_a1, q_b0, q_b1;
wire [63:0] old_word = wr_bank_r ? q_a1 : q_a0;

always @(posedge CLK) begin
  if (px_vis & wr_en) acc_r <= acc_nx;
  wr0_r <= 1'b0;
  if (px_vis & wr_en & (&PX_X[2:0])) begin
    wr0_r      <= 1'b1;
    wr_last0_r <= px_last;
    wr_addr_r  <= {px_cb, PX_Y[2:0]};
    wr_bank_r  <= PX_X[3];
    wr_col_r   <= PX_X[7:3];
    wr_row_r   <= px_row;
  end
  wr1_r      <= wr0_r;
  wr_last1_r <= wr_last0_r;
  // The compare is registered before it marks the bitmap: M9K clock-to-out
  // (~3.3 ns unregistered) + a 64-bit compare + the 360-way decode did not fit
  // one 84 MHz cycle (first pilot fit: -0.74 ns).
  wr2_r      <= wr1_r;
  wr_last2_r <= wr_last1_r;
  wr_diff_r  <= (old_word != acc_r);
  wr_col2_r  <= wr_col_r;
  wr_row2_r  <= wr_row_r;
end

//-------------------------------------------------------------------
// ROW_DONE decode ($EF0007)
//-------------------------------------------------------------------
wire        rd_all     = rowdone_wr_r & (wr_data_r == 8'hFF);
wire        rd_one     = rowdone_wr_r & (wr_data_r < 8'd18);
wire [4:0]  rd_row     = wr_data_r[4:0];
wire        rd_hit_one = rd_one & row_frozen_r[rd_row];
wire        rd_bad     = rowdone_wr_r & ~rd_all & ~rd_hit_one;
wire [17:0] rd_mask    = rd_all ? row_frozen_r : (rd_hit_one ? (18'd1 << rd_row) : 18'd0);

//-------------------------------------------------------------------
// rows, dirty, CL, counters
//-------------------------------------------------------------------
wire        cl_ready  = (cl_hyst_r >= `C6_CL_HYST);
wire        cl_switch = TAP_LY0 & en_eff_r & cl_ready & ~ctl_lock_r
                      & (row_clid_r == {18{cl_id_r}});
wire        vr_pos    = ~vr_r[15] & |vr_r;
wire        vg_pos    = ~vg_r[15] & |vg_r;
wire        vb_pos    = ~vb_r[15] & |vb_r;
wire [2:0]  cl_vote   = {vb_pos, vg_pos, vr_pos};

always @(posedge CLK) begin
  if (RST | c6_reset) begin
    en_eff_r       <= c6_reset;
    hold_eff_r     <= c6_reset & ctl_hold_r;
    row_frozen_r   <= 18'd0;
    thaw_pend_r    <= 18'd0;
    row_clid_r     <= 18'd0;
    cl_cur_r       <= 3'd0;
    cl_prev_r      <= 3'd0;
    cl_cand_r      <= 3'd0;
    cl_id_r        <= 1'b0;
    cl_hyst_r      <= 4'd0;
    vr_r           <= 16'd0;
    vg_r           <= 16'd0;
    vb_r           <= 16'd0;
    rows_dropped_r <= 16'd0;
    rowdone_bad_r  <= 16'd0;
    cl_switches_r  <= 16'd0;
    frames_r       <= 16'd0;
  end
  else begin
    // ---- the two latched controls -------------------------------------
    if (TAP_LY0) begin
      en_eff_r   <= ctl_en_r;
      hold_eff_r <= ctl_hold_r;
    end
    if (c6_off) begin
      row_frozen_r <= 18'd0;
      thaw_pend_r  <= 18'd0;
    end

    // ---- ROW_DONE: arm the thaw (the dirty bits are cleared below) -------
    if (|rd_mask) thaw_pend_r <= thaw_pend_r | rd_mask;
    if (rd_bad & ~(&rowdone_bad_r)) rowdone_bad_r <= rowdone_bad_r + 16'd1;

    // ---- thaw / drop at (0, 8r) -------------------------------------------
    if (thaw_now) begin
      row_frozen_r[px_row] <= 1'b0;
      thaw_pend_r[px_row]  <= 1'b0;
      row_clid_r[px_row]   <= cl_id_r;
    end
    if (drop_now & ~(&rows_dropped_r)) rows_dropped_r <= rows_dropped_r + 16'd1;

    // ---- the compare of the last word lands: the row freezes -------------
    // On the same edge its dirty bit is set (below), never before it: a row
    // that reads frozen has every bit of its last word in the bitmap.
    if (wr2_r & wr_last2_r) row_frozen_r[wr_row2_r] <= 1'b1;

    // ---- the vote -----------------------------------------------------
    if (TAP_VBLANK & en_eff_r) begin
      vr_r <= 16'd0;
      vg_r <= 16'd0;
      vb_r <= 16'd0;
      if (cl_vote == cl_cur_r)       cl_hyst_r <= 4'd0;
      else if (cl_vote == cl_cand_r) cl_hyst_r <= (&cl_hyst_r) ? cl_hyst_r : cl_hyst_r + 4'd1;
      else begin
        cl_cand_r <= cl_vote;
        cl_hyst_r <= 4'd1;
      end
      if (~(&frames_r)) frames_r <= frames_r + 16'd1;
    end
    else if (px_vis & en_eff_r) begin
      vr_r <= vr_r + {{14{dr[1]}}, dr};
      vg_r <= vg_r + {{14{dg[1]}}, dg};
      vb_r <= vb_r + {{14{db[1]}}, db};
    end

    // ---- the CL switch, at LY=0 only ---------------------------------------
    if (cl_switch) begin
      cl_prev_r <= cl_cur_r;
      cl_cur_r  <= cl_cand_r;
      cl_id_r   <= ~cl_id_r;
      cl_hyst_r <= 4'd0;
      if (~(&cl_switches_r)) cl_switches_r <= cl_switches_r + 16'd1;
    end
  end
end

//-------------------------------------------------------------------
// the dirty bitmap, 18 rows x 20 cells.  Every bit is one 4-input function
//     d = (set_row[r] & set_col[c]) | (q & ~clr_row[r])
// with the restart (FB_EN taking effect: all ones) folded into both set lines
// and RST into the row clear.  A set wins over a ROW_DONE clear of the same
// cycle; the two never meet on one row anyway, since ROW_DONE only touches a
// frozen row and a frozen row takes no writes.
//-------------------------------------------------------------------
wire        dset    = wr2_r & wr_diff_r;
wire [17:0] set_row = {18{c6_reset}} | ({18{dset}} & (18'd1 << wr_row2_r));
wire [19:0] set_col = {20{c6_reset}} | (20'd1 << wr_col2_r);
wire [17:0] clr_row = rd_mask | {18{RST}};
genvar gr, gc;
generate
  for (gr = 0; gr < 18; gr = gr + 1) begin : g_drow
    for (gc = 0; gc < 20; gc = gc + 1) begin : g_dcol
      always @(posedge CLK)
        dirty_r[gr*20 + gc] <= (set_row[gr] & set_col[gc]) | (dirty_r[gr*20 + gc] & ~clr_row[gr]);
    end
  end
endgenerate

//-------------------------------------------------------------------
// the two memories (even / odd cell columns)
//-------------------------------------------------------------------
wire [10:0] rd_addr0, rd_addr1;

c6_fb_ram fb0 (
  .clock(CLK),
  .address_a(wr_addr_r), .data_a(acc_r),     .wren_a(wr1_r & ~wr_bank_r), .q_a(q_a0),
  .address_b(rd_addr0),  .q_b(q_b0)
);
c6_fb_ram fb1 (
  .clock(CLK),
  .address_a(wr_addr_r), .data_a(acc_r),     .wren_a(wr1_r &  wr_bank_r), .q_a(q_a1),
  .address_b(rd_addr1),  .q_b(q_b1)
);

//-------------------------------------------------------------------
// read side (SNES view), stage 0: the address
//-------------------------------------------------------------------
wire [4:0]  tr = VADDR[15:11];
wire [4:0]  tc = VADDR[10:6];
wire [2:0]  vy = VADDR[3:1];
wire [2:0]  vp = {VADDR[5:4], VADDR[0]};

wire [7:0]  tc5 = {1'b0, tc, 2'b00} + {3'b000, tc};    // 5 * tc, 0..155
wire [4:0]  c0  = ctl_stretch_r ? tc5[7:3] : tc;      // leftmost source cell
wire [2:0]  o   = ctl_stretch_r ? tc5[2:0] : 3'd0;    // offset of the group in it
wire        oor = (tr > 5'd17) | (~ctl_stretch_r & (tc > 5'd19));
wire [7:0]  rd_rb = {tr, 3'b000} + {2'b00, tr, 1'b0};    // tr * 10
wire [7:0]  rd_cb = rd_rb + {4'h0, c0[4:1]};
// Cell c0 is in memory c0[0]; its right neighbour c0+1 in the other one.  The
// odd memory always holds pair c0>>1; the even one holds c0 (c0 even) or
// c0 + 1 = pair (c0>>1) + 1 (c0 odd).
assign      rd_addr1 = {rd_cb, vy};
assign      rd_addr0 = {rd_cb + {7'd0, c0[0]}, vy};

reg         a1_odd_r, a1_oor_r, a1_str_r;
reg  [2:0]  a1_o_r, a1_p_r;
always @(posedge CLK) begin
  a1_odd_r <= c0[0];
  a1_oor_r <= oor;
  a1_o_r   <= o;
  a1_p_r   <= vp;
  a1_str_r <= ctl_stretch_r;
end

//-------------------------------------------------------------------
// stage 1: the plane pick, straight off the RAMs' port B.  Each memory gives
// the plane-p bits of its 8 pixels (bit 7 = the leftmost pixel) and only those
// 16 bits are registered.  (The pilot registered the two 64-bit words and
// picked the plane after the left/right swap: 128 more flip-flops and 128 2:1
// muxes for the same three-cycle answer.)  An 8:1 bit pick per pixel byte:
// the arithmetic index this once was came out as a 7-level chain.
//-------------------------------------------------------------------
function [7:0] plane8(input [63:0] word, input [2:0] p);
  integer   k;
  reg [7:0] b;
  begin
    for (k = 0; k < 8; k = k + 1) begin
      b            = word[63 - 8*k -: 8];
      plane8[7-k]  = b[p];
    end
  end
endfunction

reg  [7:0]  p0_r, p1_r;
reg         s1_odd_r, s1_oor_r, s1_str_r;
reg  [2:0]  s1_o_r;
always @(posedge CLK) begin
  p0_r     <= plane8(q_b0, a1_p_r);
  p1_r     <= plane8(q_b1, a1_p_r);
  s1_odd_r <= a1_odd_r;
  s1_oor_r <= a1_oor_r;
  s1_o_r   <= a1_o_r;
  s1_str_r <= a1_str_r;
end

//-------------------------------------------------------------------
// stage 2 (combinational into the bridge's view_data_r): the left/right swap
// and the stretch.  W = the plane-p bits of the 16 source pixels starting at
// the left cell, W[15] = its leftmost pixel.
//-------------------------------------------------------------------
wire [15:0] w = s1_odd_r ? {p1_r, p0_r} : {p0_r, p1_r};

reg  [7:0]  vb;
integer     vj;
reg  [2:0]  sj;
always @(*) begin
  for (vj = 0; vj < 8; vj = vj + 1) begin
    case (vj)
      0, 1:    sj = 3'd0;
      2:       sj = 3'd1;
      3, 4:    sj = 3'd2;
      5:       sj = 3'd3;
      default: sj = 3'd4;
    endcase
    vb[7-vj] = s1_str_r ? w[15 - (s1_o_r + sj)] : w[15 - vj];
  end
end

assign VIEW_BYTE = s1_oor_r ? 8'h00 : vb;

//-------------------------------------------------------------------
// status bytes (section 14.5), all live
//-------------------------------------------------------------------
wire [7:0]  flags = {1'b1, |row_frozen_r, cl_ready, ctl_lock_r, ctl_stretch_r,
                     en_eff_r, hold_eff_r, &row_frozen_r};
wire [7:0]  clb   = {1'b0, cl_prev_r, cl_id_r, cl_cur_r};

// The dirty bitmap, +60..+8C, as an ARRAY of bytes read by a 6-bit index:
// +60..+7F -> 0..31, +80..+8C -> 32..44, no subtractor.  Written as the part
// select dirty_r[8*(off - $60) +: 8] it came out as a 360-bit shifter: 1.3 K
// LE, a tenth of the chip, for a 45-byte mux.
wire [7:0]  dbytes [0:63];
genvar      gd;
generate
  for (gd = 0; gd < 64; gd = gd + 1) begin : g_dbytes
    if (gd < 45) begin : g_on
      assign dbytes[gd] = dirty_r[8*gd +: 8];
    end else begin : g_off
      assign dbytes[gd] = 8'h00;
    end
  end
endgenerate
wire [5:0]  didx  = STAT_OFF[7] ? {2'b10, STAT_OFF[3:0]} : {1'b0, STAT_OFF[4:0]};
wire        dhit  = (STAT_OFF >= 8'h60) & (STAT_OFF <= 8'h8C);

assign      STAT_HIT = (STAT_OFF >= 8'h58) & (STAT_OFF <= 8'h91);
always @(*) begin
  case (STAT_OFF)
    8'h58:   STAT_BYTE = flags;
    8'h59:   STAT_BYTE = frames_r[7:0];
    8'h5A:   STAT_BYTE = row_frozen_r[7:0];
    8'h5B:   STAT_BYTE = row_frozen_r[15:8];
    8'h5C:   STAT_BYTE = {6'd0, row_frozen_r[17:16]};
    8'h5D:   STAT_BYTE = clb;
    8'h5E:   STAT_BYTE = rows_dropped_r[7:0];
    8'h8D:   STAT_BYTE = row_clid_r[7:0];
    8'h8E:   STAT_BYTE = row_clid_r[15:8];
    8'h8F:   STAT_BYTE = {6'd0, row_clid_r[17:16]};
    8'h90:   STAT_BYTE = COLW_N[7:0];
    8'h91:   STAT_BYTE = COLW_N[15:8];
    default: STAT_BYTE = dhit ? dbytes[didx] : 8'h00;
  endcase
end

assign FLAGS        = flags;
assign CL_BYTE      = clb;
assign ROWS_DROPPED = rows_dropped_r;
assign ROWDONE_BAD  = rowdone_bad_r;
assign CL_SWITCHES  = cl_switches_r;
assign FRAMES       = frames_r;

// verilator lint_off UNUSED
wire _unused_ok = &{1'b0, SEL, 1'b0};
// verilator lint_on UNUSED

endmodule

//-------------------------------------------------------------------
// 1440 x 64 true dual port, port B read-only.  Instantiated as the primitive,
// not inferred: one write port plus TWO read ports (A's read-before-write and
// the SNES on B) is a shape Quartus may otherwise implement by duplicating the
// array.  Unregistered outputs, like every other RAM in this core; the read
// side registers port B itself (see above).
//-------------------------------------------------------------------
module c6_fb_ram(
  input         clock,
  input  [10:0] address_a,
  input  [63:0] data_a,
  input         wren_a,
  output [63:0] q_a,
  input  [10:0] address_b,
  output [63:0] q_b
);
  altsyncram altsyncram_component (
    .address_a (address_a), .address_b (address_b),
    .clock0 (clock),
    .data_a (data_a), .data_b (64'd0),
    .wren_a (wren_a), .wren_b (1'b0),
    .q_a (q_a), .q_b (q_b),
    .aclr0 (1'b0), .aclr1 (1'b0),
    .addressstall_a (1'b0), .addressstall_b (1'b0),
    .byteena_a (1'b1), .byteena_b (1'b1),
    .clock1 (1'b1),
    .clocken0 (1'b1), .clocken1 (1'b1), .clocken2 (1'b1), .clocken3 (1'b1),
    .eccstatus (),
    .rden_a (1'b1), .rden_b (1'b1));
  defparam
    altsyncram_component.address_reg_b = "CLOCK0",
    altsyncram_component.clock_enable_input_a = "BYPASS",
    altsyncram_component.clock_enable_input_b = "BYPASS",
    altsyncram_component.clock_enable_output_a = "BYPASS",
    altsyncram_component.clock_enable_output_b = "BYPASS",
    altsyncram_component.indata_reg_b = "CLOCK0",
    altsyncram_component.intended_device_family = "Cyclone IV E",
    altsyncram_component.lpm_type = "altsyncram",
    // 512 x 16 per M9K -> 4 wide x 3 deep = 12 blocks.  Left to AUTO, Quartus
    // 25.1 builds 1440 x 64 out of 1K x 8 blocks, 8 wide x 2 deep = 16 (the
    // first pilot run: 59 / 56 M9K, "Can't fit design in device").
    altsyncram_component.maximum_depth = 512,
    altsyncram_component.numwords_a = 1440,
    altsyncram_component.numwords_b = 1440,
    altsyncram_component.operation_mode = "BIDIR_DUAL_PORT",
    altsyncram_component.outdata_aclr_a = "NONE",
    altsyncram_component.outdata_aclr_b = "NONE",
    altsyncram_component.outdata_reg_a = "UNREGISTERED",
    altsyncram_component.outdata_reg_b = "UNREGISTERED",
    altsyncram_component.power_up_uninitialized = "FALSE",
    altsyncram_component.read_during_write_mode_mixed_ports = "OLD_DATA",
    altsyncram_component.read_during_write_mode_port_a = "OLD_DATA",
    altsyncram_component.read_during_write_mode_port_b = "OLD_DATA",
    altsyncram_component.widthad_a = 11,
    altsyncram_component.widthad_b = 11,
    altsyncram_component.width_a = 64,
    altsyncram_component.width_b = 64,
    altsyncram_component.width_byteena_a = 1,
    altsyncram_component.width_byteena_b = 1,
    altsyncram_component.wrcontrol_wraddress_reg_b = "CLOCK0";
endmodule
