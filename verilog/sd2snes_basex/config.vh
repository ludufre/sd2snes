`timescale 1ns / 1ps
// basex: the mk2-only flavour of the base core that keeps the cartridge-specific address
// decoder extensions (Sufami Turbo, Gamars, BS Memory Pack slot). The variant is selected
// by the BASE_EXT Verilog macro, which this project's .xise sets globally (Verilog Macros
// property) -- NOT by this header, because address.v derives its gate from the global
// macros. This file exists to keep the HEADER dependency of common.mk pointing at a file
// that lives with the project, like sd2snes_gsu3.
`ifndef _config_vh
`define _config_vh

`ifdef MK2
  `ifdef DEBUG
    `define MK2_DEBUG
  `endif
`endif

`endif
