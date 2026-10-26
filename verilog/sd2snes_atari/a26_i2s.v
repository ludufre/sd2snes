// SPDX-License-Identifier: GPL-2.0-or-later
//
// a26_i2s.v -- serial audio output (I2S bus) for the DAC of the Mk.II board.
//
// Written from the I2S bus specification (Philips Semiconductors, "I2S bus specification", February 1986, revised June 5, 1996) and from the
// clock ratios the board already uses; nothing in this file comes from another project's source.
//   FONTE: I2S bus specification, "The I2S bus": three lines, continuous serial clock SCK, word select WS (here LRCK) and serial data SD;
//          data is MSB first; SD changes on the falling edge of SCK and is valid at the rising edge; WS changes with SCK falling edge and
//          selects the channel (WS = 0 left, WS = 1 right); the MSB follows one SCK period after the WS edge.
//   FONTE: medida, out/passo33.txt (pin timing of the Passo 31 build, kept bit for bit: this file must drive the same waveform).
//
// Clocks, for clkin = 96 MHz (all counts are in clkin periods):
//   MCLK  = clkin / 8           12 MHz   (256 x fs)
//   LRCK  = MCLK / 64           187.5 kHz (one 16-bit word per half period, left and right carry the same `sample`)
//   SCLK  = LRCK * 32           6 MHz    (16 data bits per LRCK half)
//   The DAC of the board takes the word right after the LRCK edge, shifted by one SCLK as in the I2S bus; MCLK leaves the chip inverted and
//   3 clkin late, LRCK 3 late and SCLK 2 late relative to the internal counter.
//   DAC of the Mk.II board: Cirrus Logic CS4344 (TSSOP-10; BOM entry CS4344-CZZ).  Found in the sd2snes board files pcb/kicad/RevF/sd2snes.net (value
//   CS4344) and pcb/kicad/cs4344.sch (nets DAC_MCLK, DAC_LRCK, DAC_SDIN in RevF/fpga.sch) and in pcb/kicad/RevF/sd2snes-BOM.ods.
//   Datasheet: Cirrus Logic, CS4344/5/8/9 (DS613) -- not stored in the repository; the part number is taken from the board files above, the
//   datasheet number is from memory of the vendor's document list and still has to be checked against the PDF.
//
// `sample` is signed and is sent on both channels (the TIA mix changes at 31.4 kHz: zero-order hold).  sclk_out is for the bench only.
`timescale 1ns / 1ps

module a26_i2s (
  input         clkin,
  input  [15:0] sample,
  output        sdout,
  output        mclk_out,
  output        lrck_out,
  output        sclk_out
);

  // free-running phase counter: bit 2 toggles every 4 clkin (MCLK), bit 3 every 8 (SCLK), bit 8 every 128 (LRCK)
  reg [10:0] phase = 11'h100;
  reg        started = 1'b0;                 // the first clkin after configuration only arms the edge detectors
  always @(posedge clkin) begin
    phase   <= phase + 11'd1;
    started <= 1'b1;
  end

  // SCLK falling edge = phase[3:0] wraps to 0; LRCK edge on the same instant = phase[7:0] wraps to 0
  wire sclk_fall = started && (phase[3:0] == 4'd0);
  wire word_edge = sclk_fall && (phase[7:4] == 4'd0);

  // serial word register: reloaded at each LRCK edge, otherwise shifted left; SD takes the MSB at every SCLK falling edge
  reg [15:0] word = 16'h0000;
  reg        sd   = 1'b0;
  always @(posedge clkin) begin
    if (sclk_fall) begin
      sd   <= word[15];
      word <= word_edge ? sample : {word[14:0], 1'b0};
    end
  end

  // pin delay lines (the board's MCLK / LRCK / SCLK leave late by 3 / 3 / 2 clkin)
  reg [2:0] d_mclk = 3'b000, d_lrck = 3'b000;
  reg [1:0] d_sclk = 2'b00;
  always @(posedge clkin) begin
    d_mclk <= {d_mclk[1:0], phase[2]};
    d_lrck <= {d_lrck[1:0], phase[8]};
    d_sclk <= {d_sclk[0],  phase[3]};
  end

  assign mclk_out = ~d_mclk[2];
  assign lrck_out =  d_lrck[2];
  assign sclk_out =  d_sclk[1];
  assign sdout    =  sd;

endmodule
