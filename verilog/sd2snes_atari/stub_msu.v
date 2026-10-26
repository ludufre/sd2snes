// SPDX-License-Identifier: GPL-2.0-or-later
// sd2snes_atari (STorm2600 core): port-compatible stub of `msu`.
// All outputs are tied to 0: this module is not used in the Atari mode,
// and XST removes whatever logic only fed from it.
`timescale 1ns / 1ps

module msu (
  input clkin,
  input enable,
  input [13:0] pgm_address,
  input [7:0] pgm_data,
  input pgm_we,
  input [2:0] reg_addr,
  input [7:0] reg_data_in,
  output [7:0] reg_data_out,
  input reg_oe_falling,
  input reg_oe_rising,
  input reg_we_rising,
  output [7:0] status_out,
  output [7:0] volume_out,
  output volume_latch_out,
  output [31:0] addr_out,
  output [15:0] track_out,
  input [5:0] status_reset_bits,
  input [5:0] status_set_bits,
  input status_reset_we,
  input [13:0] msu_address_ext,
  input msu_address_ext_write,
  output DBG_msu_reg_oe_rising,
  output DBG_msu_reg_oe_falling,
  output DBG_msu_reg_we_rising,
  output [13:0] DBG_msu_address,
  output DBG_msu_address_ext_write_rising
);
assign reg_data_out = 1'b0;
assign status_out = 1'b0;
assign volume_out = 1'b0;
assign volume_latch_out = 1'b0;
assign addr_out = 1'b0;
assign track_out = 1'b0;
assign DBG_msu_reg_oe_rising = 1'b0;
assign DBG_msu_reg_oe_falling = 1'b0;
assign DBG_msu_reg_we_rising = 1'b0;
assign DBG_msu_address = 1'b0;
assign DBG_msu_address_ext_write_rising = 1'b0;
endmodule
