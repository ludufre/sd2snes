`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// Company: Rehkopf
// Engineer: Rehkopf
//
// Create Date:    01:13:46 05/09/2009
// Design Name:
// Module Name:    address
// Project Name:
// Target Devices:
// Tool versions:
// Description: Address logic w/ SaveRAM masking
//
// Dependencies:
//
// Revision:
// Additional Comments:
//
//////////////////////////////////////////////////////////////////////////////////

`include "config.vh"

module address(
  input CLK,
  input [15:0] featurebits, // peripheral enable/disable
  //input [2:0] MAPPER,       // MCU detected mapper
  input [23:0] SNES_ADDR,   // requested address from SNES
  input [7:0] SNES_PA,      // peripheral address from SNES
  input SNES_ROMSEL,        // ROMSEL from SNES
  output [23:0] ROM_ADDR,   // Address to request from SRAM0
  output ROM_HIT,           // enable SRAM0
  output IS_SAVERAM,        // address/CS mapped as SRAM?
  output IS_ROM,            // address mapped as ROM?
  output IS_WRITABLE,       // address somehow mapped as writable area?
  //input [23:0] SAVERAM_MASK,
  //input [23:0] ROM_MASK,
  output msu_enable,
  //output srtc_enable,
  output gbc_view_enable,
  output gbc_ef_enable,
  output r213f_enable,
  output snescmd_enable
);

/* feature bits. see src/fpga_spi.c for mapping */
parameter [2:0]
  FEAT_DSPX = 0,
  FEAT_ST0010 = 1,
  FEAT_SRTC = 2,
  FEAT_MSU1 = 3,
  FEAT_213F = 4,
  FEAT_2100 = 6
;

wire [23:0] SRAM_SNES_ADDR;

/* currently supported mappers:
   Index     Mapper
   -         LoROM
*/

assign IS_ROM = ~SNES_ROMSEL;

// The SNES side of this cartridge is the gbc_snes.bin player, which has no
// SaveRAM: the GB's cart RAM is written by the GB, not by the SNES, and SNES
// access to the PSRAM would contend with it.
assign IS_SAVERAM = 0;

// LoROM: A23 = r03/r04  A22 = r06  A21 = r05  A20 = 0    A19 = d/c
assign IS_WRITABLE = IS_SAVERAM;

// Only a LOROM image of up to 512KB is supported.  MAPPER, ROMMASK, and RAMMASK have been repurposed for GB.
assign SRAM_SNES_ADDR = {5'h00, SNES_ADDR[19:16], SNES_ADDR[14:0]};

assign ROM_ADDR = SRAM_SNES_ADDR;

assign ROM_HIT = IS_ROM | IS_WRITABLE;

assign msu_enable = featurebits[FEAT_MSU1] & (!SNES_ADDR[22] && ((SNES_ADDR[15:0] & 16'hfff8) == 16'h2000));

// GBC bridge window (GBC-CORE-CONTRACT.md section 3).  Both live in ROMSEL
// territory (banks $C0-$FF assert /CART for the whole bank), so the generic
// IS_ROM path in main.v already turns the data bus around for them; no
// featurebit gates either one.
//
//   $E0-$E3        read  - chr/map/CGRAM/status/log/OAM views
//   $E4            read  - framebuffer view (C6, wire $03)
//   $EF00xx        write - joypad, COMMIT, GO, SYNC, CONSUMED
// Wire $03: bank $E4 is the framebuffer view (gbc_bridge.v / c6_fb.v),
// contract section 14.3; the bridge tells it from $E0-$E3 by address bit 18.
assign gbc_view_enable = (SNES_ADDR[23:18] == 6'b111000) | (SNES_ADDR[23:16] == 8'hE4);
assign gbc_ef_enable   = (SNES_ADDR[23:8] == 16'hEF00);

assign r213f_enable = featurebits[FEAT_213F] & (SNES_PA == 8'h3f);

assign snescmd_enable = ({SNES_ADDR[22], SNES_ADDR[15:9]} == 8'b0_0010101);

endmodule
