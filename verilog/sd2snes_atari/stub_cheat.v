// SPDX-License-Identifier: GPL-2.0-or-later
// sd2snes_atari (STorm2600 core): port-compatible stub of `cheat`.
// All outputs are tied to 0: this module is not used in the Atari mode,
// and XST removes whatever logic only fed from it.
`timescale 1ns / 1ps

module cheat (
  input clk,
  input [7:0] SNES_PA,
  input [23:0] SNES_ADDR,
  input [7:0] SNES_DATA,
  input SNES_wr_strobe,
  input SNES_rd_strobe,
  input SNES_rd_end_strobe,
  input SNES_reset_strobe,
  input SNES_PAWR_raw,
  input snescmd_enable,
  input nmicmd_enable,
  input return_vector_enable,
  input branch1_enable,
  input branch2_enable,
  input branch3_enable,
  input exe_present,
  input pad_latch,
  input snes_ajr,
  input SNES_cycle_start,
  input [2:0] pgm_idx,
  input pgm_we,
  input [31:0] pgm_in,
  input feat_cmd_unlock_in,
  output [7:0] data_out,
  output cheat_hit,
  output snescmd_unlock,
  output map_unlock
);
assign data_out = 1'b0;
assign cheat_hit = 1'b0;
assign snescmd_unlock = 1'b0;
assign map_unlock = 1'b0;
endmodule
