`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date:    00:31:19 01/19/2019
// Design Name:
// Module Name:    config (sd2snes_a26)
// Project Name: 
// Target Devices: 
// Tool versions: 
// Description: 
//
// Dependencies: 
//
// Revision: 
// Revision 0.01 - File Created
// Additional Comments: 
//
//////////////////////////////////////////////////////////////////////////////////

`ifndef _config_vh
`define _config_vh

// `define DEBUG

// dac.v is a VERBATIM copy of sd2snes_nes/dac.v (the version with the
// cartridge-audio "sgb" channel, which the a26 TIA audio mixer will feed).
// There the MSU-1 CIC always-block sits behind `ifdef MSU_AUDIO, so the define
// below is what keeps the MSU CIC built; without it the MSU integrators would
// freeze at 0.  The MSU path is dead logic on a26 loads (fpga_a26 only ever
// boots .a26 ROMs), but keeping the define means this dac.v stays
// byte-identical to the nes/sms one, so a future re-sync of the three files
// cannot silently regress any of the cores.
`define MSU_AUDIO

`ifdef MK2
  `ifdef DEBUG
    `define MK2_DEBUG
  `endif
`endif

`endif