`timescale 1ns / 1ps
// gsu3: the mk2-only FX3-capable flavour of the GSU core.  The variant is
// selected by the GSU3 Verilog macro, which this project's .xise sets globally
// (Verilog Macros property) -- NOT by this header, because gsu.v derives its
// FX3 gate from the global macros without including config.vh.  This file
// exists to mirror the sd2snes_sgb_msu variant layout and to keep the HEADER
// dependency of common.mk pointing at a file that lives with the project.
`ifndef _config_vh
`define _config_vh

`ifdef MK2
  `ifdef DEBUG
    `define MK2_DEBUG
  `endif
`endif

`endif
