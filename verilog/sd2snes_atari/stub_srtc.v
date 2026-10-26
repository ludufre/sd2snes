// SPDX-License-Identifier: GPL-2.0-or-later
// sd2snes_atari (STorm2600 core): port-compatible stub of `srtc`.
// All outputs are tied to 0: this module is not used in the Atari mode,
// and XST removes whatever logic only fed from it.
`timescale 1ns / 1ps

module srtc (
  input clkin,
  input addr_in,
  input [3:0] data_in,
  output [7:0] data_out,
  input [59:0] rtc_data_in,
  output [59:0] rtc_data_out,
  input reg_oe_falling,
  input reg_oe_rising,
  input reg_we_rising,
  input enable,
  output rtc_we,
  input reset,
  output [4:0] srtc_state,
  output srtc_reg_we_rising,
  output [3:0] srtc_rtc_ptr,
  output [5:0] srtc_we_sreg
);
assign data_out = 1'b0;
assign rtc_data_out = 1'b0;
assign rtc_we = 1'b0;
assign srtc_state = 1'b0;
assign srtc_reg_we_rising = 1'b0;
assign srtc_rtc_ptr = 1'b0;
assign srtc_we_sreg = 1'b0;
endmodule
