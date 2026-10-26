// SPDX-License-Identifier: GPL-2.0-or-later
//
// riot6532.v -- MOS 6532 RIOT as wired in the Atari 2600, written from
//               scratch.  Milestone 2.
//
// Wiring in the console (decoded OUTSIDE this module):
//   chip selected : A12 = 0 and A7 = 1
//   rs            : A9      (0 = 128-byte RAM, 1 = I/O and timer)
//   a[6:0]        : A6..A0
//
// Strobes (all single-clock pulses, already qualified by chip select):
//   tick : one pulse per phi2 cycle.  The timer must keep running while the
//          CPU is halted by WSYNC, so this is NOT the same as the CPU's ce.
//   rd   : the CPU read cycle completes (cpu_ce & ~we & cs)
//   wr   : the CPU write cycle completes (cpu_ce &  we & cs)
//   Ports rd/wr/tick may fall on the same clock edge; a timer write wins
//   over that edge's tick.  Strobes need at least 2 clocks between them
//   (the RAM read is registered).
//
// Register map (rs = 1), as the Atari sees it:
//   a[2]=0 : 00 SWCHA  01 SWACNT  10 SWCHB  11 SWBCNT
//   a[2]=1 read : a[0]=0 INTIM   a[0]=1 INSTAT (bit7 timer, bit6 PA7 edge)
//   a[2]=1 write: a[4]=1 timer, a[1:0] = divide by 1/8/64/1024
//                 a[4]=0 edge detect control, a[0]=1 positive edge
//
// TIMER MODEL (the 6532 as Stella models it: the first decrement comes on the
// cycle right after the write)
//   write N with divisor d at cycle W:  INTIM = N - 1 - floor((k-1)/d) for k >= 1
//   cycles after W, so it reads 0 from k = (N-1)*d + 1 to k = N*d; at
//   k = N*d + 1 it wraps to $FF, INSTAT bit7 is set, and from then on it counts
//   down every cycle.  Reading INTIM clears bit7.  Writing the timer clears bit7.
//   (Until 2026-10-11 the first decrement came d cycles after the write: every
//   timed wait was one interval too long, and games made 264 lines instead of 262.)
//
// STATUS: untested when written.  Verify with tb_riot.v.
//
`timescale 1ns / 1ps

module riot6532 (
  input             clk,
  input             rst,
  input             tick,
  input             rd,
  input             wr,
  input             rs,
  input      [6:0]  a,
  input      [7:0]  din,
  output     [7:0]  dout,
  input      [7:0]  pa_in,
  output     [7:0]  pa_out,
  output     [7:0]  pa_oe,
  input      [7:0]  pb_in,
  output     [7:0]  pb_out,
  output     [7:0]  pb_oe
);

  // ---- 128 x 8 RAM (registered read: maps to one block RAM) -----------
  reg [7:0] ram [0:127];
  reg [7:0] ramq;
  always @(posedge clk) begin
    if (wr && !rs) ram[a] <= din;
    ramq <= ram[a];
  end

  // ---- registers -------------------------------------------------------
  reg [7:0] dra, ddra, drb, ddrb;
  reg [7:0] timer;
  reg [9:0] pre;
  reg [1:0] sh;
  reg       fast, flag_t, flag_pa, edge_pos, pa7_d;

  assign pa_out = dra;   assign pa_oe = ddra;
  assign pb_out = drb;   assign pb_oe = ddrb;

  wire [7:0] porta_r = (dra & ddra) | (pa_in & ~ddra);
  wire [7:0] portb_r = (drb & ddrb) | (pb_in & ~ddrb);

  reg [7:0] iod;
  always @* begin
    if (!a[2]) begin
      case (a[1:0])
        2'd0:    iod = porta_r;
        2'd1:    iod = ddra;
        2'd2:    iod = portb_r;
        default: iod = ddrb;
      endcase
    end else begin
      iod = a[0] ? {flag_t, flag_pa, 6'b000000} : timer;
    end
  end
  assign dout = rs ? iod : ramq;

  // ---- decode ----------------------------------------------------------
  wire io_wr    = wr & rs;
  wire io_rd    = rd & rs;
  wire tim_wr   = io_wr & a[2] &  a[4];
  wire edge_wr  = io_wr & a[2] & ~a[4];
  wire intim_rd = io_rd & a[2] & ~a[0];
  wire instat_rd= io_rd & a[2] &  a[0];

  reg  [9:0] curmax;
  always @* begin
    case (sh)
      2'd0:    curmax = 10'd0;
      2'd1:    curmax = 10'd7;
      2'd2:    curmax = 10'd63;
      default: curmax = 10'd1023;
    endcase
  end

  wire edge_hit = edge_pos ? (~pa7_d &  pa_in[7]) : (pa7_d & ~pa_in[7]);

  // ---- sequencer -------------------------------------------------------
  always @(posedge clk) begin
    if (rst) begin
      dra <= 8'h00; ddra <= 8'h00; drb <= 8'h00; ddrb <= 8'h00;
      timer <= 8'hFF; pre <= 10'd0; sh <= 2'd3; fast <= 1'b0;
      flag_t <= 1'b0; flag_pa <= 1'b0; edge_pos <= 1'b0; pa7_d <= 1'b1;
    end else begin
      // I/O registers
      if (io_wr && !a[2]) begin
        case (a[1:0])
          2'd0: dra  <= din;
          2'd1: ddra <= din;
          2'd2: drb  <= din;
          default: ddrb <= din;
        endcase
      end
      if (edge_wr) edge_pos <= a[0];

      // PA7 edge flag: clear on INSTAT read, set wins over clear
      if (instat_rd) flag_pa <= 1'b0;
      if (tick) begin
        pa7_d <= pa_in[7];
        if (edge_hit) flag_pa <= 1'b1;
      end

      // timer
      if (tim_wr) begin
        timer <= din; sh <= a[1:0]; pre <= 10'd0; fast <= 1'b0; flag_t <= 1'b0;
      end else begin
        if (intim_rd) flag_t <= 1'b0;
        if (tick) begin
          if (fast) begin
            timer <= timer - 8'd1;
          end else if (pre == 10'd0) begin
            pre   <= curmax;
            timer <= timer - 8'd1;
            if (timer == 8'h00) begin flag_t <= 1'b1; fast <= 1'b1; end
          end else begin
            pre <= pre - 10'd1;
          end
        end
      end
    end
  end

endmodule
