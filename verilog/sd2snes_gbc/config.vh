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

// The GBC core is Mk.III only (EP4CE15).  There is no MK2 branch here: the
// Spartan-3 of the Mk.II has neither the logic nor the block RAM for the CGB
// delta, and .gbc keeps routing to the SGB core on that board.

// MSU_AUDIO is load-bearing even though this core has no MSU-1: dac.v only
// instantiates its CIC decimator (and therefore its cartridge audio path)
// under this define.  Dropping it silences the GB APU.
`define MSU_AUDIO

// Debug pipe from the MCU into GB state (VRAM/OAM/HRAM/regs/GBDG) over USB.
// This is the read path the contract's PSRAM map relies on for
// 0x808000-0x80BFFF, 0x80FE00+ and 0x810000 (GBDG).
`define SGB_MCU_ACCESS

// Link port.  Kept: the blargg/mooneye test ROMs report their verdict on it and
// it is the only text channel the core has.
`define SGB_SERIAL

// MBC1M/HuC3/camera decode in sgb.v.
`define SGB_EXTRA_MAPPERS

// Deliberately NOT defined here (see GBC-CORE-CONTRACT.md invariant 17):
//   MSU_DATA          - no MSU-1 data port; frees the 16 M9K msu_databuf
//   BRIGHTNESS_PATCH  - the $2100 patcher is gone from main.v
//   BRIGHTNESS_LIMIT  - idem
//   SGB_SAVE_STATES   - the 64 KB CTX machine cannot describe CGB state
//   SGB_DEBUG         - breakpoint/step unit and the big DBG register dump
//   SGB_SPR_INCREASE  - 16 sprites/line; the re-render already lifts that limit
//                       on the SNES side and the OAM LUT costs logic here

// Audio probe -- DEBUG BUILD ONLY, never defined in a shipped core: replaces
// the APU output with a ~1.02 kHz square wave (+-255 of 10 bits, both
// channels) so the DAC path can be proven audible on the console before the
// APU itself is trusted (the "beep -> probe -> music" ladder of the plan).
//`define GBC_AUDIO_PROBE

// CHIPFEAT word (opcode 0xEF, mcu_cmd.v), written by the firmware BEFORE the
// core comes out of reset -- see main.sdc for the false path this implies.
`define SGB_FEAT_VOL_BOOST    2:0
`define GBC_FEAT_FORCE_DMG    3:3
`define GBC_FEAT_CGB_HDR      4:4
`define GBC_FEAT_SYNC_EXACT   5:5

// C6, the framebuffer mode (GBC-CORE-CONTRACT.md section 14, wire $03).
// C6_CL_HYST: frames the majority vote has to name the same palette-bit
// candidate before the CL of the frame switches (section 14.6.6).
`define C6_CL_HYST 4'd4

`endif
