// SPDX-License-Identifier: GPL-2.0-or-later
// sd2snes_atari (STorm2600 core): port-compatible stub of `ctx`.
// All outputs are tied to 0: this module is not used in the Atari mode,
// and XST removes whatever logic only fed from it.
`timescale 1ns / 1ps

module ctx (
  input clkin,
  input reset,
  input [23:0] SNES_ADDR,
  input [7:0] SNES_PA,
  input SNES_RD_end_PRE,
  input SNES_WR_end_PRE,
  input SNES_PARD_end_PRE,
  input SNES_PAWR_end_PRE,
  input [7:0] SNES_DATA_IN_PRE,
  output OE_WR_ENABLE,
  output OE_PAWR_ENABLE,
  output OE_PARD_ENABLE,
  output BUS_WRQ,
  input BUS_RDY,
  input snescmd_unlock,
  output [23:0] ROM_ADDR,
  output [15:0] ROM_DATA,
  output ROM_WORD_ENABLE,
  output DBG
);
assign OE_WR_ENABLE = 1'b0;
assign OE_PAWR_ENABLE = 1'b0;
assign OE_PARD_ENABLE = 1'b0;
assign BUS_WRQ = 1'b0;
assign ROM_ADDR = 1'b0;
assign ROM_DATA = 1'b0;
assign ROM_WORD_ENABLE = 1'b0;
assign DBG = 1'b0;
endmodule
