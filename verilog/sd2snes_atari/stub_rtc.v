// SPDX-License-Identifier: GPL-2.0-or-later
// sd2snes_atari (STorm2600 core): port-compatible stub of `rtc`.
// All outputs are tied to 0: this module is not used in the Atari mode,
// and XST removes whatever logic only fed from it.
`timescale 1ns / 1ps

module rtc (
  input clkin,
  input pgm_we,
  input [55:0] rtc_data_in,
  input we1,
  input [59:0] rtc_data_in1,
  output [59:0] rtc_data
);
assign rtc_data = 1'b0;
endmodule
