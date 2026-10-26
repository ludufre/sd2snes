// SPDX-License-Identifier: GPL-2.0-or-later
//
// tia_audio.v -- Atari 2600 TIA audio (2 channels), written from scratch.
//                Milestone 4.
//
// CLOCKING
//   `tick` = audio clock, 3.579545 MHz / 114 = 31.4 kHz = twice per scanline.
//   Derive it in the shell from the TIA colour clock counter, e.g.
//       tick = cc_ce & (hcount == 9 || hcount == 81)
//
// CHANNEL MODEL (per AUDC value; "step" = one divider expiry)
//   divider period = (AUDF + 1) ticks, times 3 for AUDC = 12..15
//    0, 11 : constant 1 (output is just the volume)
//    1     : 4-bit poly, one step per divider expiry
//    2     : 4-bit poly, advanced only when the div-31 pattern is 1
//    3     : 4-bit poly, advanced only when the 5-bit poly is 1
//    4, 5  : square wave (toggle every expiry)
//    6, 10 : div-31 pattern (13 high : 18 low)
//    7     : square wave toggled only when the 5-bit poly is 1
//    8     : 9-bit poly (white noise)
//    9     : 5-bit poly
//    12, 13: square wave, divider x3
//    14    : div-31 pattern, divider x3
//    15    : like 7, divider x3
//   Channel output = poly/tone bit ? AUDV : 0.  Mix = vol0 + vol1 (0..30).
//
// STATUS: untested when written.  The exact poly bit patterns and a few mode
// details are reconstructed from public documentation; listening against a
// reference emulator is the real validation.  tb_tia_audio.v checks periods
// and bit counts of this model.
//
`timescale 1ns / 1ps

module aud_chan (
  input        clk,
  input        rst,
  input        tick,
  input  [3:0] audc,
  input  [4:0] audf,
  input  [3:0] audv,
  output [3:0] vol
);
  // pattern ROMs: bit i = table[i]
  localparam [14:0] B4  = 15'b001010000111011;                  // 15 states, 7 ones
  localparam [30:0] B5  = 31'b1000010101110110001111100110100;  // 31 states, 16 ones
  localparam [30:0] D31 = 31'h0001FFF0;                         // 31 states, 13 ones

  reg [6:0] cnt;
  reg [4:0] p5;
  reg [3:0] p4;
  reg [8:0] p9;
  reg       obit;

  wire       div3 = (audc[3:2] == 2'b11);
  wire [6:0] base = {2'b00, audf} + 7'd1;
  wire [6:0] maxc = div3 ? (base * 7'd3) : base;

  wire [4:0] p5n = (p5 == 5'd30) ? 5'd0 : p5 + 5'd1;
  wire [3:0] p4n = (p4 == 4'd14) ? 4'd0 : p4 + 4'd1;
  wire [8:0] p9n = {p9[0] ^ p9[4], p9[8:1]};
  wire       g_d31 = D31[p5n];
  wire       g_b5  = B5[p5n];
  wire       b4n   = B4[p4n];

  always @(posedge clk) begin
    if (rst) begin
      cnt <= 7'd1; p5 <= 5'd0; p4 <= 4'd0; p9 <= 9'h1FF; obit <= 1'b0;
    end else if (tick) begin
      if (cnt > 7'd1) begin
        cnt <= cnt - 7'd1;
      end else begin
        cnt <= maxc;
        p5  <= p5n;
        case (audc)
          4'd0, 4'd11:                 obit <= 1'b1;
          4'd1:  begin p4 <= p4n; obit <= b4n; end
          4'd2:  if (g_d31) begin p4 <= p4n; obit <= b4n; end
          4'd3:  if (g_b5)  begin p4 <= p4n; obit <= b4n; end
          4'd4, 4'd5, 4'd12, 4'd13:    obit <= ~obit;
          4'd6, 4'd10, 4'd14:          obit <= g_d31;
          4'd7, 4'd15:   if (g_b5)     obit <= ~obit;
          4'd8:  begin p9 <= p9n; obit <= p9n[0]; end
          default:                     obit <= g_b5;           // 9
        endcase
      end
    end
  end

  assign vol = (audc == 4'd0 || audc == 4'd11 || obit) ? audv : 4'd0;
endmodule

module tia_audio (
  input        clk,
  input        rst,
  input        tick,
  input  [3:0] audc0,
  input  [3:0] audc1,
  input  [4:0] audf0,
  input  [4:0] audf1,
  input  [3:0] audv0,
  input  [3:0] audv1,
  output [3:0] vol0,
  output [3:0] vol1,
  output [4:0] mix
);
  aud_chan c0 (.clk(clk), .rst(rst), .tick(tick), .audc(audc0), .audf(audf0), .audv(audv0), .vol(vol0));
  aud_chan c1 (.clk(clk), .rst(rst), .tick(tick), .audc(audc1), .audf(audf1), .audv(audv1), .vol(vol1));
  assign mix = {1'b0, vol0} + {1'b0, vol1};
endmodule
