// SPDX-License-Identifier: GPL-2.0-or-later
// sd2snes_atari (STorm2600 core): port-compatible stub of `bsx`.
// All outputs are tied to 0: this module is not used in the Atari mode,
// and XST removes whatever logic only fed from it.
`timescale 1ns / 1ps

module bsx (
  input clkin,
  input reg_oe_falling,
  input reg_oe_rising,
  input reg_we_rising,
  input [23:0] snes_addr_in,
  input [7:0] reg_data_in,
  output [7:0] reg_data_out,
  input [7:0] reg_reset_bits,
  input [7:0] reg_set_bits,
  output [14:0] regs_out,
  input pgm_we,
  input use_bsx,
  input bs_slot,
  input bs_hirom,
  output data_ovr,
  output flash_writable,
  input [59:0] rtc_data_in,
  output [9:0] bs_page_out,
  output bs_page_enable,
  output [8:0] bs_page_offset,
  input feat_bs_base_enable,
  output [1:0] bs_erase_seq,
  output [3:0] bs_erase_blk
);
assign reg_data_out = 1'b0;
assign regs_out = 1'b0;
assign data_ovr = 1'b0;
assign flash_writable = 1'b0;
assign bs_page_out = 1'b0;
assign bs_page_enable = 1'b0;
assign bs_page_offset = 1'b0;
assign bs_erase_seq = 1'b0;
assign bs_erase_blk = 1'b0;
endmodule
