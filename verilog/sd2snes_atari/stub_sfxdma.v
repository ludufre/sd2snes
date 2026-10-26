// SPDX-License-Identifier: GPL-2.0-or-later
// sd2snes_atari (STorm2600 core): port-compatible stub of `sfxdma`.
// All outputs are tied to 0: this module is not used in the Atari mode,
// and XST removes whatever logic only fed from it.
`timescale 1ns / 1ps

module sfxdma (
  input clkin,
  input reset,
  input [23:0] sfx_base,
  input [23:0] sfx_len,
  input kick,
  input disable_in,
  input DAC_STATUS,
  input sd_dma_active,
  input BUS_RDY,
  output BUS_RRQ,
  output [23:0] ROM_ADDR,
  input [7:0] ROM_DATA_IN,
  output dac_we,
  output [10:0] dac_addr,
  output [7:0] dac_data,
  output dac_active,
  output prime_hold,
  output active,
  output done
);
assign BUS_RRQ = 1'b0;
assign ROM_ADDR = 1'b0;
assign dac_we = 1'b0;
assign dac_addr = 1'b0;
assign dac_data = 1'b0;
assign dac_active = 1'b0;
assign prime_hold = 1'b0;
assign active = 1'b0;
assign done = 1'b0;
endmodule
