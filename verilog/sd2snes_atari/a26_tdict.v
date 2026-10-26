// SPDX-License-Identifier: GPL-2.0-or-later
//
// a26_tdict.v -- tile dictionary: sends every DIFFERENT 8x8 tile of a frame to the
//                PSRAM once, plus a tile map.  Sits between a26_vidwr and a26_mem.
//
// WHY
//   A frame is 29 x 32 = 928 tiles of 16 bytes (14.5 KB), but only about a hundred of
//   them are different (measured: at most 141 in six games).  With a map the SNES
//   copies about 4 KB per frame instead of 14 KB.
//
// WHAT COMES IN (from a26_vidwr, all of it changing on pix_stb only)
//   v_wr / v_waddr / v_wdata : the writer's word stream, unchanged.
//       v_waddr[13] = 1 : colour / window table word -> passed on to the PSRAM as is
//       v_waddr[13] = 0 : tile word { buffer[1:0], 0, r[4:0], c[4:0], line[2:0] }
//                         -> kept here, in a line buffer of two tile rows (r even / odd)
//   ev_row   : toggles when the 8th line of a tile row has been written
//   ev_end   : toggles when a frame with at least one line has ended; end_lines is the
//              number of lines it had (so end_lines % 8 lines of a last, partial tile row)
// WHAT GOES OUT (to a26_mem; word offsets inside the buffer, bytes = 2 x)
//   word $0000        : number of distinct tiles of the frame (tile 0 included)
//   words $0001-$0007 : 0
//   word $0008 + r*32 + c : map entry = tile number (bits 9:0)
//   word $0400 + i*8 + line : tile i, same 8 words as the writer made
//   The buffer number is the one of the tile words that came in.
//
// HOW
//   * Tile 0 is always the all-zero tile; numbers are given in order of first
//     appearance, row by row, starting again every frame.
//   * At the end of every tile row the 32 tiles are looked up one by one:
//       CRC-16 of the 8 words -> bucket (10 bits) + tag (6 bits) of a 1024-entry hash
//       table with linear probing.  A candidate with the right tag is then compared
//       WORD FOR WORD with the dictionary copy (4096 x 16 block RAM = 512 tiles), so
//       two tiles get the same number only if their 16 bytes are equal.
//     A tile that is not found gets the next number, goes into the dictionary and its
//     8 words go to the PSRAM.  Then the map word goes out.
//   * Lines that were not written in a partial last row count as zero, and the map
//     rows after it are filled with 0.
//   * More than CAP = 512 distinct tiles, or a tile row arriving before the row two
//     before it is done: eng_ovf, nothing more is written for that frame.
//   * End of frame: last row, map fill, header, hash table cleared, then wait until
//     a26_mem has really written everything, and only then eng_ack := ev_end.  The
//     writer publishes the buffer (or drops the frame if eng_ovf) after that.
//   * Words go out one at a time and only while the FIFO of a26_mem is less than half
//     full; a table word of the writer always goes first.
//
// This module runs at clock rate (it is NOT in the multi-cycle groups of ucf.extra):
// every block RAM output is registered once more before it is used.
//
// STATUS: untested when written.  Verify with tb_a26_tdict.v.
//
`timescale 1ns / 1ps

module a26_tdict (
  input             clk,
  input             rst,

  input             v_wr,
  input      [15:0] v_waddr,
  input      [15:0] v_wdata,
  input             ev_row,
  input             ev_end,
  input      [7:0]  end_lines,
  output reg        eng_ack,
  output reg        eng_ovf,
  output reg [9:0]  tile_n,           // distinct tiles of the last finished frame

  output            m_wr,
  output     [15:0] m_waddr,
  output     [15:0] m_wdata,
  input             fifo_low,         // a26_mem: FIFO less than half full
  input             wr_idle           // a26_mem: FIFO empty and nothing in flight
);

  localparam [3:0] S_IDLE = 4'd0,  S_T0  = 4'd1,  S_T0N = 4'd2,  S_HRD = 4'd3,
                   S_PRB  = 4'd4,  S_CMP = 4'd5,  S_NEW = 4'd6,  S_NRD = 4'd7,
                   S_NNX  = 4'd8,  S_MAP = 4'd9,  S_MNX = 4'd10, S_FILL = 4'd11,
                   S_HDR  = 4'd12, S_HDN = 4'd13, S_CLR = 4'd14, S_PUSH = 4'd15;

  function [15:0] crc16;              // CRC-16/CCITT over one 16-bit word
    input [15:0] c;
    input [15:0] d;
    integer i;
    reg [15:0] x;
    begin
      x = c;
      for (i = 15; i >= 0; i = i - 1)
        x = {x[14:0], 1'b0} ^ ((x[15] ^ d[i]) ? 16'h1021 : 16'h0000);
      crc16 = x;
    end
  endfunction

  reg [3:0]  st, rs;                  // state; state after a push
  reg [3:0]  t;                       // step inside a state
  reg [4:0]  c, row;
  reg [3:0]  nl;                      // lines of this tile row that were written (1..8)
  reg        last;                    // the partial row of an end-of-frame job
  reg        fresh;                   // no row of the current frame processed yet
  reg        inrow, waiti;
  reg [9:0]  count;                   // tiles in the dictionary
  reg [15:0] h;
  reg [9:0]  bkt;
  reg [5:0]  tag;
  reg [8:0]  cand, idx;
  reg        neq, pd;
  reg [2:0]  wl;
  reg [9:0]  mp, ca;
  reg [1:0]  rows_pend;
  reg        end_pend, er_d, ee_d;
  reg [1:0]  wbuf;
  reg        wq_valid;
  reg [15:0] wq_addr, wq_data;

  // ---- the three RAMs, each read through one more register ------------------------
  reg [15:0] lbuf [0:511];            // line buffer: 2 tile rows x 32 tiles x 8 words
  reg [15:0] dict [0:4095];           // 512 tiles x 8 words
  reg [15:0] htab [0:1023];           // valid, tile number [8:0], tag [5:0]
  reg [15:0] lb_q, lbq, d_q, dq, h_q, hq;

  wire        lb_we  = v_wr & ~v_waddr[13];
  wire [8:0]  lb_wa  = {v_waddr[8], v_waddr[7:0]};
  wire        rdw    = (st == S_NRD);
  wire [8:0]  lb_ra  = {row[0], c, rdw ? wl : t[2:0]};

  wire        acc    = wq_valid & fifo_low & ~(v_wr & v_waddr[13]);
  wire        clr    = (st == S_CLR) & ~waiti;
  wire        d_we   = (clr & (ca[9:3] == 7'd0)) | ((st == S_PUSH) & acc & pd);
  wire [11:0] d_wa   = clr ? {2'b00, ca} : {idx, wl};
  wire [15:0] d_wd   = clr ? 16'h0000 : wq_data;
  wire [11:0] d_ra   = {cand, t[2:0]};
  wire        h_new  = (st == S_NEW) & ~count[9];
  wire        h_we   = clr | h_new;
  wire [9:0]  h_wa   = clr ? ca : bkt;
  wire [15:0] h_wd   = clr ? ((ca == 10'd0) ? 16'h8000 : 16'h0000) : {1'b1, count[8:0], tag};

  always @(posedge clk) begin
    if (lb_we) lbuf[lb_wa] <= v_wdata;
    lb_q <= lbuf[lb_ra];
    lbq  <= lb_q;
  end
  always @(posedge clk) begin
    if (d_we) dict[d_wa] <= d_wd;
    d_q <= dict[d_ra];
    dq  <= d_q;
  end
  always @(posedge clk) begin
    if (h_we) htab[h_wa] <= h_wd;
    h_q <= htab[bkt];
    hq  <= h_q;
  end

  // the word in lbq belongs to line wi of the tile; lines that were not written are zero
  wire [2:0]  wi  = rdw ? wl : (t[2:0] - 3'd2);
  wire [15:0] lbm = ({1'b0, wi} < nl) ? lbq : 16'h0000;

  // ---- way out: a table word of the writer first, then ours ---------------------------
  wire vtbl = v_wr & v_waddr[13];
  assign m_wr    = vtbl | acc;
  assign m_waddr = vtbl ? v_waddr : wq_addr;
  assign m_wdata = vtbl ? v_wdata : wq_data;

  wire rp_inc = (ev_row != er_d);
  wire go     = (st == S_IDLE) & ((rows_pend != 2'd0) | end_pend);
  wire rp_dec = go & ~fresh & (rows_pend != 2'd0);
  wire [13:0] map_a = 14'd8 + {4'd0, row, c};

  always @(posedge clk) begin
    if (rst) begin
      st <= S_CLR; rs <= S_IDLE; t <= 4'd0; c <= 5'd0; row <= 5'd0; nl <= 4'd8;
      last <= 1'b0; fresh <= 1'b1; inrow <= 1'b0; waiti <= 1'b0; count <= 10'd1;
      h <= 16'd0; bkt <= 10'd0; tag <= 6'd0; cand <= 9'd0; idx <= 9'd0; neq <= 1'b0; pd <= 1'b0;
      wl <= 3'd0; mp <= 10'd0; ca <= 10'd0; rows_pend <= 2'd0; end_pend <= 1'b0;
      er_d <= 1'b0; ee_d <= 1'b0; wbuf <= 2'd0;
      wq_valid <= 1'b0; wq_addr <= 16'd0; wq_data <= 16'd0;
      eng_ack <= 1'b0; eng_ovf <= 1'b0; tile_n <= 10'd0;
    end else begin
      er_d <= ev_row;
      ee_d <= ev_end;
      if (ev_end != ee_d) end_pend <= 1'b1;
      rows_pend <= rows_pend + {1'b0, rp_inc} - {1'b0, rp_dec};
      if (lb_we) begin
        wbuf <= v_waddr[15:14];
        // the writer is back in the half that is still being read: this frame is lost
        if ((inrow || rows_pend != 2'd0) && v_waddr[8] == row[0]) eng_ovf <= 1'b1;
      end
      t <= t + 4'd1;

      case (st)
        S_IDLE: if (go) begin
          if (fresh) begin                               // first job of a frame: tile 0
            fresh <= 1'b0; eng_ovf <= 1'b0; count <= 10'd1; wl <= 3'd0; st <= S_T0;
          end else if (rows_pend != 2'd0) begin
            if (!eng_ovf) begin
              nl <= 4'd8; last <= 1'b0; inrow <= 1'b1; c <= 5'd0; h <= 16'd0; t <= 4'd0; st <= S_HRD;
            end
          end else begin
            end_pend <= 1'b0;
            if (eng_ovf) begin ca <= 10'd0; waiti <= 1'b0; st <= S_CLR; end
            else if (end_lines[2:0] != 3'd0) begin
              nl <= {1'b0, end_lines[2:0]}; last <= 1'b1; inrow <= 1'b1; c <= 5'd0; h <= 16'd0; t <= 4'd0; st <= S_HRD;
            end else begin
              mp <= {row, 5'd0}; st <= S_FILL;
            end
          end
        end

        // tile 0 of the list: 8 zero words
        S_T0: begin
          wq_valid <= 1'b1; wq_addr <= {wbuf, 14'h0400 + {11'd0, wl}}; wq_data <= 16'h0000;
          pd <= 1'b0; rs <= S_T0N; st <= S_PUSH;
        end
        S_T0N: begin
          wl <= wl + 3'd1;
          st <= (wl == 3'd7) ? S_IDLE : S_T0;
        end

        // hash of the tile at column c: word k is in lbq on step k + 2
        S_HRD: begin
          if (t >= 4'd2 && t <= 4'd9) h <= crc16(h, lbm);
          if (t == 4'd10) begin bkt <= h[9:0]; tag <= h[15:10]; t <= 4'd0; st <= S_PRB; end
        end

        // hash table entry of bucket bkt is in hq on step 2
        S_PRB: if (t == 4'd2) begin
          if (!hq[15]) st <= S_NEW;
          else if (hq[5:0] == tag) begin cand <= hq[14:6]; neq <= 1'b0; t <= 4'd0; st <= S_CMP; end
          else begin bkt <= bkt + 10'd1; t <= 4'd0; end
        end

        // exact compare with the dictionary copy of the candidate
        S_CMP: begin
          if (t >= 4'd2 && t <= 4'd9 && lbm != dq) neq <= 1'b1;
          if (t == 4'd10) begin
            if (neq) begin bkt <= bkt + 10'd1; t <= 4'd0; st <= S_PRB; end
            else begin idx <= cand; st <= S_MAP; end
          end
        end

        // not in the dictionary: next number (the hash entry is written in this step)
        S_NEW: begin
          if (count[9]) begin                            // 512 tiles already: give the frame up
            eng_ovf <= 1'b1; inrow <= 1'b0;
            if (last) begin ca <= 10'd0; waiti <= 1'b0; st <= S_CLR; end
            else st <= S_IDLE;
          end else begin
            idx <= count[8:0]; wl <= 3'd0; t <= 4'd0; st <= S_NRD;
          end
        end
        S_NRD: if (t == 4'd2) begin                      // word wl of the tile: to the PSRAM and the dictionary
          wq_valid <= 1'b1; wq_addr <= {wbuf, 14'h0400 + {2'd0, idx, wl}}; wq_data <= lbm;
          pd <= 1'b1; rs <= S_NNX; st <= S_PUSH;
        end
        S_NNX: begin
          wl <= wl + 3'd1; t <= 4'd0;
          if (wl == 3'd7) begin count <= count + 10'd1; st <= S_MAP; end
          else st <= S_NRD;
        end

        S_MAP: begin
          wq_valid <= 1'b1; wq_addr <= {wbuf, map_a}; wq_data <= {7'd0, idx};
          pd <= 1'b0; rs <= S_MNX; st <= S_PUSH;
        end
        S_MNX: begin
          c <= c + 5'd1; h <= 16'd0; t <= 4'd0;
          if (c == 5'd31) begin
            inrow <= 1'b0; row <= row + 5'd1;
            if (last) begin mp <= {row + 5'd1, 5'd0}; st <= S_FILL; end
            else st <= S_IDLE;
          end else st <= S_HRD;
        end

        // map rows that were never written: entry 0
        S_FILL: begin
          if (mp >= 10'd928) begin wl <= 3'd0; st <= S_HDR; end
          else begin
            wq_valid <= 1'b1; wq_addr <= {wbuf, 14'd8 + {4'd0, mp}}; wq_data <= 16'h0000;
            pd <= 1'b0; rs <= S_FILL; st <= S_PUSH; mp <= mp + 10'd1;
          end
        end

        // header: word 0 = number of tiles, words 1..7 = 0
        S_HDR: begin
          wq_valid <= 1'b1; wq_addr <= {wbuf, 11'd0, wl};
          wq_data <= (wl == 3'd0) ? {6'd0, count} : 16'h0000;
          pd <= 1'b0; rs <= S_HDN; st <= S_PUSH;
        end
        S_HDN: begin
          wl <= wl + 3'd1;
          if (wl == 3'd7) begin tile_n <= count; ca <= 10'd0; waiti <= 1'b0; st <= S_CLR; end
          else st <= S_HDR;
        end

        // empty dictionary for the next frame: hash table cleared, tile 0 = zero in bucket 0
        // (CRC of 16 zero bytes is 0) and in the dictionary; then wait for the PSRAM writes
        S_CLR: begin
          if (!waiti) begin
            ca <= ca + 10'd1;
            if (ca == 10'd1023) waiti <= 1'b1;
          end else if (wr_idle) begin
            eng_ack <= ev_end; fresh <= 1'b1; row <= 5'd0; count <= 10'd1; st <= S_IDLE;
          end
        end

        S_PUSH: if (acc) begin wq_valid <= 1'b0; t <= 4'd0; st <= rs; end

        default: st <= S_IDLE;
      endcase
    end
  end

endmodule
