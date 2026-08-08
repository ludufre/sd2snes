`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date:    00:31:19 01/19/2019 
// Design Name: 
// Module Name:    config 
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
// cartridge-audio "sgb" channel the SMS PSG feeds).  There the MSU-1 CIC
// always-block sits behind `ifdef MSU_AUDIO; in the SMS core that block used to
// be unconditional, so the define below keeps the MSU CIC built (the internal
// registers shrank 64->28 bits with the new dac.v, but the output slice
// [27:12] is bit-identical -- 28 bits is the exact Hogenauer minimum).
// Without it the MSU integrators would freeze at 0.  The MSU path is dead
// logic on SMS loads (fpga_sms only ever boots .sms ROMs), but keeping the
// define means sms/dac.v stays byte-identical to nes/dac.v, so a future
// re-sync of the two files cannot silently regress either core.
`define MSU_AUDIO

`ifdef MK2
  `ifdef DEBUG
    `define MK2_DEBUG
  `endif
`endif

`endif