`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// config.vh -- sd2snes_nes feature switches.
//
// Adapted from sd2snes_sgb/config.vh's mk2-vs-mk3 pattern (see
// NES-CORE-CONTRACT.md SS8): mk3 (Cyclone IV, more headroom) turns everything
// on; mk2 (Spartan-3, tight) would need the same kind of pruning the SGB core
// does (dropping NES_DEBUG first, since that's the CONFIG_READ/CONFIG_WRITE
// breadcrumb bus -- see mcu_cmd.v). Phase -1 is being brought up and debugged
// on mk3 only; the mk2 branch below is a straight-line placeholder, NOT
// measured against real Spartan-3 area (no synthesis has been run for this
// core at all yet -- see the Phase -1 report).
//////////////////////////////////////////////////////////////////////////////////

`ifndef _config_vh
`define _config_vh

`ifdef MK2
  // Placeholder mirroring sd2snes_sgb's mk2 pattern. Unmeasured for this core.
`else // MK3
  `define MSU_AUDIO
  `define MSU_DATA
  `define NES_DEBUG
`endif

// Breadcrumb config-register feature bits (see nes_wrap.v NES_BREADCRUMB_GROUP).
// Placeholder -- Phase -1 doesn't gate anything through featurebits/chipfeat
// yet; kept for parity with sd2snes_sgb's SGB_FEAT_* layout so main.v's
// CHIPFEAT (0xef) wiring has somewhere sensible to land bits as the mapper/
// iNES-flags plumbing grows past mapper 0.
`define NES_FEAT_MIRRORING     0:0
`define NES_FEAT_HAS_CHR_RAM   1:1
`define NES_FEAT_FOUR_SCREEN   2:2

`endif
