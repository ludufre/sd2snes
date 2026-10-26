// SPDX-License-Identifier: GPL-2.0-or-later
//
// a26_vidwr.v -- turns the TIA pixel stream into a SNES-ready frame buffer.
//                Milestone 6; 160 columns 1:1, three buffers with a lock, fixed window.
//
// WHY THIS FORMAT
//   The FPGA cannot drive the TV: the SNES PPU draws everything, so the frame
//   has to reach VRAM.  Per frame the budget is a few KB, hence:
//     * 2 bits per pixel = which of the 4 TIA colour sources (BK, PF, P0, P1)
//     * 4 colours per line, already as SNES colours in HDMA table form
//   The pixels are written already in SNES 2bpp TILE format, 256 pixels wide
//   (the 160 TIA pixels 1:1 in columns 48..207), so the 65816 only has to start a DMA
//   from this buffer to VRAM, and point four HDMA channels (CGRAM) plus one for
//   the window at the tables: nothing is converted on the SNES side.
//
// MEMORY FORMAT (16-bit word writes, little endian; 3 buffers of 16384 words = $8000 bytes)
//   fb_waddr = { buffer[1:0], offset[13:0] } in words; byte offsets below are per buffer
//   tiles    : NOT written to the PSRAM as they leave here.  A tile word comes out as
//             fb_waddr = { buffer, 0, r[4:0], c[4:0], line[2:0] } (tile row r = y/8, tile c,
//             line y%8) and a26_tdict.v turns the stream into header + map + list of the
//             distinct tiles at +$0000..+$3FFF (see there).
//             word = { plane1 byte, plane0 byte } -> plane0 at the even byte, SNES 2bpp
//             (bit 7 = left); tile c = 6..25 holds TIA pixels 8(c-6)..8(c-6)+7, one column
//             each (no stretching); tiles 0..5 and 26..31 are written as zero
//   colour tables, ready for the SNES HDMA (CGADD, CGADD, CGDATA, CGDATA):
//             channel k (0 COLUBK, 1 COLUPF, 2 COLUP0, 3 COLUP1) at +$4000 + k*$400,
//             line L (0..231) is 4 bytes at +4*L:
//               word 0 = k | k<<8
//               word 1 = the SNES BGR555 colour of the TIA colour (pal_bgr below, in the
//                        palette pal_f taken when VSYNC ends),
//                        $0000 on a line with VBLANK (or VSYNC) on
//             entry L = 232 of every channel is black (word 1 = $0000)
//   window table +$5000 : line L is one word at +2*L = left | right<<8,
//             left = 48, right = 207 (the 160 columns of the picture); left = 56 when the
//             first 8 TIA pixels of the line were blanked (the HMOVE bar; the tiles hold
//             colour source 0 there); entry 232 = 0 | 255<<8
//   Entry 232 is rewritten every frame, one word per line on lines 0..8.
//   The four colours of a line are taken at TIA pixel 16.  Per line the writes are:
//   20 tile words on pixels 7, 15 .. 159 (tiles 6..25); 12 zero tile words on pixels
//   5, 13 .. 93 (tiles 0..5 and 26..31); the 9 table words on pixels 19, 35 .. 147;
//   on lines 0..8 one word of entry 232 on pixel 155.
//
// FRAMING
//   Lines are numbered from the end of VSYNC: line 0 is the scan
//   line (HBLANK first) in which VSYNC goes low.  VSYNC is looked at on pixels 0
//   and 159 only: low on pixel 159 after high on pixel 0 means it fell during the
//   picture part, so the line that starts next is already line 1.  The buffer holds the MAXLINES lines from
//   line YS = YS0 + yofs on, whatever VBLANK does: a line with VBLANK on is written
//   as black (tiles 0, the four colours black).  yofs and the palette (pal) are taken when
//   VSYNC ends, so a change written during a frame applies from the next frame on.  Without any VSYNC a frame is AUTO_LINES lines long.
//
// THREE BUFFERS (for the SNES side)
//   fb_newest : buffer holding the last COMPLETE frame
//   fb_frames : counts completed frames (wraps)
//   fb_nlines : lines written in that frame (MAXLINES when the window was complete)
//   A frame ends at the first line start after VSYNC rises (ev_end toggles, end_lines =
//   its lines).  It is published only when a26_tdict has written its last tile row
//   (eng_ack == ev_end), on the next pixel; if the dictionary overflowed (eng_ovf) the
//   frame is dropped instead: fb_newest stays and fb_dropped counts.  If the window of
//   the next frame starts while one is still waiting, that next frame is left out whole
//   (and counted in fb_dropped too).  ev_row toggles after the 8th line of
//   every tile row.  The buffer of the next
//   frame is only chosen when its first line is about to be written: any buffer that
//   is neither fb_newest nor the one locked by the SNES (lock_v, lock_i), preferring
//   the one that was not fb_newest before.  So a buffer the SNES locks right after
//   reading fb_newest is never written, even if a frame ended in between.
//
// COLOURS OF A LINE (Passo 31): the four colour indices of a line are ALLOCATED as its
//   pixels come.  Each visible pixel's final colour (pix_col, the register as it was when
//   the TIA made the pixel) takes the index that already holds it on this line (its own
//   source's index first); if none does, a free index (its own source's first), which then
//   holds that colour for the rest of the line.  So a colour that a game changes in mid
//   line is kept, as long as the line has at most four colours.  The writer runs DLY
//   clocks after pix_stb on registered copies of the TIA outputs (cx, cvis, csrc, ccol,
//   cvb; strobe dstb, VSYNC vsl), so the allocation is not on a 1-clock path from the TIA
//   (ucf.extra puts dsh* and vsh* in a26_fast).  The colour words of a line are written
//   during the NEXT line (clp, copied at its pixel 159).
//
// LIMITS: a line with more than four colours keeps the first four it meets; a pixel
// whose colour finds no index keeps its source's index (and shows that index's colour).
// The HMOVE bar is detected as "pixel 0 is blanked with VBLANK off".
//
// TIMING NOTE: everything here changes on the delayed strobe only (ucf.extra gives those
// paths 125 ns), except fb_wr, vsync_d, pub_pend, dsh and vsh.  What arrives at clock rate (vsync,
// pub_pend, the lock) only goes through a few gates before a register.
//
`timescale 1ns / 1ps

module a26_vidwr #(
  parameter       DLY        = 13,       // clocks from pix_stb to the writer's strobe (>= 2, < pixel spacing)
  parameter       MAXLINES   = 232,
  parameter       AUTO_LINES = 320,
  parameter [8:0] YS0        = 9'd27      // first line of the window when yofs = 0
) (
  input             clk,
  input             rst,

  input             pix_stb,
  input      [7:0]  pix_x,
  input             pix_vis,
  input      [1:0]  pix_src,
  input      [6:0]  pix_col,          // final colour of the pixel (COLUxx[7:1] when it was made)
  input             vsync,
  input             vblank,
  input      [6:0]  colubk,
  input      [6:0]  colupf,
  input      [6:0]  colup0,
  input      [6:0]  colup1,

  output reg        fb_wr,
  output reg [15:0] fb_waddr,
  output reg [15:0] fb_wdata,

  output reg [1:0]  fb_newest,
  output reg [7:0]  fb_frames,
  output reg [7:0]  fb_nlines,

  input             lock_v,           // the SNES is reading buffer lock_i: do not write it
  input      [1:0]  lock_i,
  input      [6:0]  yofs,             // signed, -16..+47: lines added to YS0
  input      [1:0]  pal,              // palette 0..3 (see pal_bgr)

  // to / from a26_tdict
  output reg        ev_row,
  output reg        ev_end,
  output reg [7:0]  end_lines,
  input             eng_ack,
  input             eng_ovf,
  output reg [7:0]  fb_dropped        // frames dropped (dictionary full / not ready), saturates
);

  // The four colours of a line are sampled at pixel PAL_X, not at pixel 0: games
  // write COLUPx up to pixel 7 for the line being drawn (Pitfall at 4, River Raid at 7).
  localparam [7:0]  PAL_X   = 8'd16;

  // SNES BGR555 colour of a TIA colour (index c = COLUxx[7:1] = hue<<3 | lum) in palette p
  // (write register 8, bits 1:0): 0 = Color-1, 1 = Color-2, 2 = Color-3, 3 = Black/White (the
  // eight greys of Color-1, by the TIA luminance c[2:0]).  Copy in a26_pal4.hex (palette p at
  // 128*p + c).  Only used for tbw, whose inputs change on pix_stb only (125 ns paths).
  function [14:0] pal_bgr;
    input [1:0] p;
    input [6:0] c;
    begin
      case (p)
        2'd0: case (c)
          7'd0  : pal_bgr = 15'h0000;
          7'd1  : pal_bgr = 15'h2108;
          7'd2  : pal_bgr = 15'h35AD;
          7'd3  : pal_bgr = 15'h4A52;
          7'd4  : pal_bgr = 15'h56B5;
          7'd5  : pal_bgr = 15'h6318;
          7'd6  : pal_bgr = 15'h6F7B;
          7'd7  : pal_bgr = 15'h77BD;
          7'd8  : pal_bgr = 15'h0108;
          7'd9  : pal_bgr = 15'h098C;
          7'd10 : pal_bgr = 15'h1210;
          7'd11 : pal_bgr = 15'h1A73;
          7'd12 : pal_bgr = 15'h22D6;
          7'd13 : pal_bgr = 15'h2B39;
          7'd14 : pal_bgr = 15'h2F9C;
          7'd15 : pal_bgr = 15'h37FF;
          7'd16 : pal_bgr = 15'h00AE;
          7'd17 : pal_bgr = 15'h0910;
          7'd18 : pal_bgr = 15'h1572;
          7'd19 : pal_bgr = 15'h1DF5;
          7'd20 : pal_bgr = 15'h2637;
          7'd21 : pal_bgr = 15'h2E79;
          7'd22 : pal_bgr = 15'h36DB;
          7'd23 : pal_bgr = 15'h3F1D;
          7'd24 : pal_bgr = 15'h0070;
          7'd25 : pal_bgr = 15'h0CD2;
          7'd26 : pal_bgr = 15'h1955;
          7'd27 : pal_bgr = 15'h25B7;
          7'd28 : pal_bgr = 15'h2E19;
          7'd29 : pal_bgr = 15'h3A5B;
          7'd30 : pal_bgr = 15'h429D;
          7'd31 : pal_bgr = 15'h4AFF;
          7'd32 : pal_bgr = 15'h0011;
          7'd33 : pal_bgr = 15'h1093;
          7'd34 : pal_bgr = 15'h1CF5;
          7'd35 : pal_bgr = 15'h2D77;
          7'd36 : pal_bgr = 15'h39D9;
          7'd37 : pal_bgr = 15'h463B;
          7'd38 : pal_bgr = 15'h4E7D;
          7'd39 : pal_bgr = 15'h5ADF;
          7'd40 : pal_bgr = 15'h2C0F;
          7'd41 : pal_bgr = 15'h3891;
          7'd42 : pal_bgr = 15'h44F3;
          7'd43 : pal_bgr = 15'h4D75;
          7'd44 : pal_bgr = 15'h55D7;
          7'd45 : pal_bgr = 15'h5E19;
          7'd46 : pal_bgr = 15'h667B;
          7'd47 : pal_bgr = 15'h6EBD;
          7'd48 : pal_bgr = 15'h3C09;
          7'd49 : pal_bgr = 15'h488C;
          7'd50 : pal_bgr = 15'h50EF;
          7'd51 : pal_bgr = 15'h5971;
          7'd52 : pal_bgr = 15'h65D3;
          7'd53 : pal_bgr = 15'h6E16;
          7'd54 : pal_bgr = 15'h7678;
          7'd55 : pal_bgr = 15'h7EBA;
          7'd56 : pal_bgr = 15'h4002;
          7'd57 : pal_bgr = 15'h4886;
          7'd58 : pal_bgr = 15'h54E9;
          7'd59 : pal_bgr = 15'h5D6D;
          7'd60 : pal_bgr = 15'h65CF;
          7'd61 : pal_bgr = 15'h6E32;
          7'd62 : pal_bgr = 15'h7674;
          7'd63 : pal_bgr = 15'h7ED7;
          7'd64 : pal_bgr = 15'h4400;
          7'd65 : pal_bgr = 15'h4C83;
          7'd66 : pal_bgr = 15'h5507;
          7'd67 : pal_bgr = 15'h5D6A;
          7'd68 : pal_bgr = 15'h65CD;
          7'd69 : pal_bgr = 15'h6E2F;
          7'd70 : pal_bgr = 15'h7692;
          7'd71 : pal_bgr = 15'h7ED4;
          7'd72 : pal_bgr = 15'h3C60;
          7'd73 : pal_bgr = 15'h48E3;
          7'd74 : pal_bgr = 15'h5147;
          7'd75 : pal_bgr = 15'h5DCA;
          7'd76 : pal_bgr = 15'h662D;
          7'd77 : pal_bgr = 15'h6E6F;
          7'd78 : pal_bgr = 15'h76D2;
          7'd79 : pal_bgr = 15'h7F14;
          7'd80 : pal_bgr = 15'h2CA0;
          7'd81 : pal_bgr = 15'h3D23;
          7'd82 : pal_bgr = 15'h49A7;
          7'd83 : pal_bgr = 15'h560A;
          7'd84 : pal_bgr = 15'h5E6D;
          7'd85 : pal_bgr = 15'h6ACF;
          7'd86 : pal_bgr = 15'h7332;
          7'd87 : pal_bgr = 15'h7F74;
          7'd88 : pal_bgr = 15'h14E0;
          7'd89 : pal_bgr = 15'h2563;
          7'd90 : pal_bgr = 15'h31E7;
          7'd91 : pal_bgr = 15'h426A;
          7'd92 : pal_bgr = 15'h4ACD;
          7'd93 : pal_bgr = 15'h572F;
          7'd94 : pal_bgr = 15'h5F92;
          7'd95 : pal_bgr = 15'h6BF4;
          7'd96 : pal_bgr = 15'h00E0;
          7'd97 : pal_bgr = 15'h1164;
          7'd98 : pal_bgr = 15'h21E8;
          7'd99 : pal_bgr = 15'h2E6B;
          7'd100: pal_bgr = 15'h3ACE;
          7'd101: pal_bgr = 15'h4731;
          7'd102: pal_bgr = 15'h5394;
          7'd103: pal_bgr = 15'h5BF6;
          7'd104: pal_bgr = 15'h00E2;
          7'd105: pal_bgr = 15'h0D66;
          7'd106: pal_bgr = 15'h1DEA;
          7'd107: pal_bgr = 15'h2A4D;
          7'd108: pal_bgr = 15'h36D0;
          7'd109: pal_bgr = 15'h3F33;
          7'd110: pal_bgr = 15'h4B96;
          7'd111: pal_bgr = 15'h53F8;
          7'd112: pal_bgr = 15'h00C5;
          7'd113: pal_bgr = 15'h0D49;
          7'd114: pal_bgr = 15'h19CD;
          7'd115: pal_bgr = 15'h2630;
          7'd116: pal_bgr = 15'h3293;
          7'd117: pal_bgr = 15'h3EF6;
          7'd118: pal_bgr = 15'h4759;
          7'd119: pal_bgr = 15'h4FBB;
          7'd120: pal_bgr = 15'h00A8;
          7'd121: pal_bgr = 15'h0D2C;
          7'd122: pal_bgr = 15'h19B0;
          7'd123: pal_bgr = 15'h2213;
          7'd124: pal_bgr = 15'h2E76;
          7'd125: pal_bgr = 15'h36D9;
          7'd126: pal_bgr = 15'h3F3C;
          7'd127: pal_bgr = 15'h477F;
        endcase
        2'd1: case (c)
          7'd0  : pal_bgr = 15'h0000;
          7'd1  : pal_bgr = 15'h14A5;
          7'd2  : pal_bgr = 15'h2529;
          7'd3  : pal_bgr = 15'h35CE;
          7'd4  : pal_bgr = 15'h4631;
          7'd5  : pal_bgr = 15'h56B5;
          7'd6  : pal_bgr = 15'h6339;
          7'd7  : pal_bgr = 15'h739C;
          7'd8  : pal_bgr = 15'h0421;
          7'd9  : pal_bgr = 15'h14A5;
          7'd10 : pal_bgr = 15'h2529;
          7'd11 : pal_bgr = 15'h35CE;
          7'd12 : pal_bgr = 15'h4631;
          7'd13 : pal_bgr = 15'h56B5;
          7'd14 : pal_bgr = 15'h6339;
          7'd15 : pal_bgr = 15'h739C;
          7'd16 : pal_bgr = 15'h0066;
          7'd17 : pal_bgr = 15'h00EB;
          7'd18 : pal_bgr = 15'h0190;
          7'd19 : pal_bgr = 15'h0234;
          7'd20 : pal_bgr = 15'h1697;
          7'd21 : pal_bgr = 15'h2B1B;
          7'd22 : pal_bgr = 15'h3B7E;
          7'd23 : pal_bgr = 15'h4BBF;
          7'd24 : pal_bgr = 15'h00E0;
          7'd25 : pal_bgr = 15'h0180;
          7'd26 : pal_bgr = 15'h0226;
          7'd27 : pal_bgr = 15'h068B;
          7'd28 : pal_bgr = 15'h1B0F;
          7'd29 : pal_bgr = 15'h2F73;
          7'd30 : pal_bgr = 15'h3FF6;
          7'd31 : pal_bgr = 15'h4FF9;
          7'd32 : pal_bgr = 15'h0009;
          7'd33 : pal_bgr = 15'h008E;
          7'd34 : pal_bgr = 15'h0132;
          7'd35 : pal_bgr = 15'h15B6;
          7'd36 : pal_bgr = 15'h2A3A;
          7'd37 : pal_bgr = 15'h3ABD;
          7'd38 : pal_bgr = 15'h4B1F;
          7'd39 : pal_bgr = 15'h5B5F;
          7'd40 : pal_bgr = 15'h0100;
          7'd41 : pal_bgr = 15'h01A0;
          7'd42 : pal_bgr = 15'h0A22;
          7'd43 : pal_bgr = 15'h1E87;
          7'd44 : pal_bgr = 15'h330C;
          7'd45 : pal_bgr = 15'h4370;
          7'd46 : pal_bgr = 15'h4FF4;
          7'd47 : pal_bgr = 15'h5FF7;
          7'd48 : pal_bgr = 15'h040B;
          7'd49 : pal_bgr = 15'h0C2F;
          7'd50 : pal_bgr = 15'h20D3;
          7'd51 : pal_bgr = 15'h3177;
          7'd52 : pal_bgr = 15'h41FA;
          7'd53 : pal_bgr = 15'h527E;
          7'd54 : pal_bgr = 15'h62DF;
          7'd55 : pal_bgr = 15'h6F5F;
          7'd56 : pal_bgr = 15'h10C0;
          7'd57 : pal_bgr = 15'h1D80;
          7'd58 : pal_bgr = 15'h2E00;
          7'd59 : pal_bgr = 15'h3E86;
          7'd60 : pal_bgr = 15'h4AEB;
          7'd61 : pal_bgr = 15'h5B6F;
          7'd62 : pal_bgr = 15'h6BD2;
          7'd63 : pal_bgr = 15'h73F6;
          7'd64 : pal_bgr = 15'h1C09;
          7'd65 : pal_bgr = 15'h300F;
          7'd66 : pal_bgr = 15'h40B3;
          7'd67 : pal_bgr = 15'h5137;
          7'd68 : pal_bgr = 15'h5DDA;
          7'd69 : pal_bgr = 15'h6E5D;
          7'd70 : pal_bgr = 15'h76DF;
          7'd71 : pal_bgr = 15'h7B3F;
          7'd72 : pal_bgr = 15'h20A0;
          7'd73 : pal_bgr = 15'h3940;
          7'd74 : pal_bgr = 15'h49C1;
          7'd75 : pal_bgr = 15'h5647;
          7'd76 : pal_bgr = 15'h66AB;
          7'd77 : pal_bgr = 15'h732F;
          7'd78 : pal_bgr = 15'h7F93;
          7'd79 : pal_bgr = 15'h7FF7;
          7'd80 : pal_bgr = 15'h3808;
          7'd81 : pal_bgr = 15'h480D;
          7'd82 : pal_bgr = 15'h5491;
          7'd83 : pal_bgr = 15'h6534;
          7'd84 : pal_bgr = 15'h71B8;
          7'd85 : pal_bgr = 15'h7E3B;
          7'd86 : pal_bgr = 15'h7EBC;
          7'd87 : pal_bgr = 15'h7F3D;
          7'd88 : pal_bgr = 15'h3880;
          7'd89 : pal_bgr = 15'h4CC0;
          7'd90 : pal_bgr = 15'h5964;
          7'd91 : pal_bgr = 15'h69E9;
          7'd92 : pal_bgr = 15'h766D;
          7'd93 : pal_bgr = 15'h7ED1;
          7'd94 : pal_bgr = 15'h7F55;
          7'd95 : pal_bgr = 15'h7FD9;
          7'd96 : pal_bgr = 15'h4403;
          7'd97 : pal_bgr = 15'h5409;
          7'd98 : pal_bgr = 15'h60CD;
          7'd99 : pal_bgr = 15'h7151;
          7'd100: pal_bgr = 15'h7DD5;
          7'd101: pal_bgr = 15'h7E57;
          7'd102: pal_bgr = 15'h7EDA;
          7'd103: pal_bgr = 15'h7F5C;
          7'd104: pal_bgr = 15'h4800;
          7'd105: pal_bgr = 15'h5463;
          7'd106: pal_bgr = 15'h6508;
          7'd107: pal_bgr = 15'h718D;
          7'd108: pal_bgr = 15'h7E11;
          7'd109: pal_bgr = 15'h7E95;
          7'd110: pal_bgr = 15'h7F18;
          7'd111: pal_bgr = 15'h7F9C;
          7'd112: pal_bgr = 15'h0421;
          7'd113: pal_bgr = 15'h14A5;
          7'd114: pal_bgr = 15'h2529;
          7'd115: pal_bgr = 15'h35CE;
          7'd116: pal_bgr = 15'h4631;
          7'd117: pal_bgr = 15'h56B5;
          7'd118: pal_bgr = 15'h6339;
          7'd119: pal_bgr = 15'h739C;
          7'd120: pal_bgr = 15'h0421;
          7'd121: pal_bgr = 15'h14A5;
          7'd122: pal_bgr = 15'h2529;
          7'd123: pal_bgr = 15'h35CE;
          7'd124: pal_bgr = 15'h4631;
          7'd125: pal_bgr = 15'h56B5;
          7'd126: pal_bgr = 15'h6339;
          7'd127: pal_bgr = 15'h739C;
        endcase
        2'd2: case (c)
          7'd0  : pal_bgr = 15'h0000;
          7'd1  : pal_bgr = 15'h1CE7;
          7'd2  : pal_bgr = 15'h318C;
          7'd3  : pal_bgr = 15'h4210;
          7'd4  : pal_bgr = 15'h5294;
          7'd5  : pal_bgr = 15'h5EF7;
          7'd6  : pal_bgr = 15'h6F7B;
          7'd7  : pal_bgr = 15'h7BDE;
          7'd8  : pal_bgr = 15'h00A7;
          7'd9  : pal_bgr = 15'h054B;
          7'd10 : pal_bgr = 15'h09CF;
          7'd11 : pal_bgr = 15'h0E53;
          7'd12 : pal_bgr = 15'h1AD7;
          7'd13 : pal_bgr = 15'h273A;
          7'd14 : pal_bgr = 15'h339D;
          7'd15 : pal_bgr = 15'h3FFF;
          7'd16 : pal_bgr = 15'h006B;
          7'd17 : pal_bgr = 15'h04EF;
          7'd18 : pal_bgr = 15'h0972;
          7'd19 : pal_bgr = 15'h15D5;
          7'd20 : pal_bgr = 15'h2259;
          7'd21 : pal_bgr = 15'h2EBC;
          7'd22 : pal_bgr = 15'h3B1D;
          7'd23 : pal_bgr = 15'h477E;
          7'd24 : pal_bgr = 15'h004E;
          7'd25 : pal_bgr = 15'h08B1;
          7'd26 : pal_bgr = 15'h1534;
          7'd27 : pal_bgr = 15'h21B7;
          7'd28 : pal_bgr = 15'h2E1A;
          7'd29 : pal_bgr = 15'h3A7D;
          7'd30 : pal_bgr = 15'h46FE;
          7'd31 : pal_bgr = 15'h535F;
          7'd32 : pal_bgr = 15'h000E;
          7'd33 : pal_bgr = 15'h1051;
          7'd34 : pal_bgr = 15'h20D4;
          7'd35 : pal_bgr = 15'h3157;
          7'd36 : pal_bgr = 15'h3DBA;
          7'd37 : pal_bgr = 15'h4A1D;
          7'd38 : pal_bgr = 15'h5A9E;
          7'd39 : pal_bgr = 15'h66FF;
          7'd40 : pal_bgr = 15'h280C;
          7'd41 : pal_bgr = 15'h344F;
          7'd42 : pal_bgr = 15'h44D2;
          7'd43 : pal_bgr = 15'h5156;
          7'd44 : pal_bgr = 15'h5DB9;
          7'd45 : pal_bgr = 15'h6A1C;
          7'd46 : pal_bgr = 15'h767D;
          7'd47 : pal_bgr = 15'h76DE;
          7'd48 : pal_bgr = 15'h3807;
          7'd49 : pal_bgr = 15'h484B;
          7'd50 : pal_bgr = 15'h54CF;
          7'd51 : pal_bgr = 15'h6152;
          7'd52 : pal_bgr = 15'h6DB5;
          7'd53 : pal_bgr = 15'h7618;
          7'd54 : pal_bgr = 15'h7A7B;
          7'd55 : pal_bgr = 15'h7EDD;
          7'd56 : pal_bgr = 15'h4001;
          7'd57 : pal_bgr = 15'h5066;
          7'd58 : pal_bgr = 15'h5CEA;
          7'd59 : pal_bgr = 15'h696E;
          7'd60 : pal_bgr = 15'h75D1;
          7'd61 : pal_bgr = 15'h7A35;
          7'd62 : pal_bgr = 15'h7AB8;
          7'd63 : pal_bgr = 15'h7F1B;
          7'd64 : pal_bgr = 15'h4000;
          7'd65 : pal_bgr = 15'h4C82;
          7'd66 : pal_bgr = 15'h5D06;
          7'd67 : pal_bgr = 15'h698A;
          7'd68 : pal_bgr = 15'h75ED;
          7'd69 : pal_bgr = 15'h7A50;
          7'd70 : pal_bgr = 15'h7AB3;
          7'd71 : pal_bgr = 15'h7F16;
          7'd72 : pal_bgr = 15'h3860;
          7'd73 : pal_bgr = 15'h48E2;
          7'd74 : pal_bgr = 15'h5564;
          7'd75 : pal_bgr = 15'h61E8;
          7'd76 : pal_bgr = 15'h6E6B;
          7'd77 : pal_bgr = 15'h76CF;
          7'd78 : pal_bgr = 15'h7B32;
          7'd79 : pal_bgr = 15'h7F95;
          7'd80 : pal_bgr = 15'h24A0;
          7'd81 : pal_bgr = 15'h3942;
          7'd82 : pal_bgr = 15'h45A3;
          7'd83 : pal_bgr = 15'h5627;
          7'd84 : pal_bgr = 15'h62AB;
          7'd85 : pal_bgr = 15'h6F0E;
          7'd86 : pal_bgr = 15'h7B91;
          7'd87 : pal_bgr = 15'h7FB4;
          7'd88 : pal_bgr = 15'h0CE0;
          7'd89 : pal_bgr = 15'h1D62;
          7'd90 : pal_bgr = 15'h2DE3;
          7'd91 : pal_bgr = 15'h3E67;
          7'd92 : pal_bgr = 15'h4EEB;
          7'd93 : pal_bgr = 15'h5B4E;
          7'd94 : pal_bgr = 15'h67D1;
          7'd95 : pal_bgr = 15'h73F4;
          7'd96 : pal_bgr = 15'h00E0;
          7'd97 : pal_bgr = 15'h0962;
          7'd98 : pal_bgr = 15'h15E5;
          7'd99 : pal_bgr = 15'h2669;
          7'd100: pal_bgr = 15'h32EC;
          7'd101: pal_bgr = 15'h3F6F;
          7'd102: pal_bgr = 15'h4FD3;
          7'd103: pal_bgr = 15'h5BF6;
          7'd104: pal_bgr = 15'h00C1;
          7'd105: pal_bgr = 15'h0964;
          7'd106: pal_bgr = 15'h0DE9;
          7'd107: pal_bgr = 15'h1A6C;
          7'd108: pal_bgr = 15'h26D0;
          7'd109: pal_bgr = 15'h3354;
          7'd110: pal_bgr = 15'h43B7;
          7'd111: pal_bgr = 15'h4FFA;
          7'd112: pal_bgr = 15'h00A4;
          7'd113: pal_bgr = 15'h0948;
          7'd114: pal_bgr = 15'h0DAC;
          7'd115: pal_bgr = 15'h1250;
          7'd116: pal_bgr = 15'h22B3;
          7'd117: pal_bgr = 15'h2F37;
          7'd118: pal_bgr = 15'h3B9A;
          7'd119: pal_bgr = 15'h47DD;
          7'd120: pal_bgr = 15'h0067;
          7'd121: pal_bgr = 15'h050C;
          7'd122: pal_bgr = 15'h0D8F;
          7'd123: pal_bgr = 15'h1213;
          7'd124: pal_bgr = 15'h2277;
          7'd125: pal_bgr = 15'h2AFB;
          7'd126: pal_bgr = 15'h3B5E;
          7'd127: pal_bgr = 15'h47BF;
        endcase
        default: case (c[2:0])
          3'd0: pal_bgr = 15'h0000;
          3'd1: pal_bgr = 15'h2108;
          3'd2: pal_bgr = 15'h35AD;
          3'd3: pal_bgr = 15'h4A52;
          3'd4: pal_bgr = 15'h56B5;
          3'd5: pal_bgr = 15'h6318;
          3'd6: pal_bgr = 15'h6F7B;
          3'd7: pal_bgr = 15'h77BD;
        endcase
      endcase
    end
  endfunction

  function [1:0] inc3;                // next buffer, modulo 3
    input [1:0] b;
    begin inc3 = (b == 2'd0) ? 2'd1 : (b == 2'd1) ? 2'd2 : 2'd0; end
  endfunction

  // ---- capture of the TIA outputs and the delayed strobe ------------------------------------
  reg  [7:0]     cx   = 8'd0;
  reg            cvis = 1'b0, cvb = 1'b0;
  reg  [1:0]     csrc = 2'd0;
  reg  [6:0]     ccol = 7'd0;
  (* SHREG_EXTRACT = "NO" *) reg [DLY-1:0] dsh = {DLY{1'b0}};   // pix_stb delayed: a26_fast (flip-flops, not SRL16)
  (* SHREG_EXTRACT = "NO" *) reg [DLY-1:0] vsh = {DLY{1'b0}};   // VSYNC delayed by the same DLY clocks: a26_fast
  wire           dstb = dsh[DLY-1];
  wire           vsl  = vsh[DLY-1];
  always @(posedge clk) begin
    dsh <= {dsh[DLY-2:0], pix_stb & ~rst};
    vsh <= {vsh[DLY-2:0], vsync};
    if (pix_stb) begin cx <= pix_x; cvis <= pix_vis; csrc <= pix_src; ccol <= pix_col; cvb <= vblank; end
  end

  reg [7:0] y;                        // lines written so far in this frame
  reg [8:0] vl;                       // line number since the end of VSYNC
  reg [6:0] yofs_r;
  reg [1:0] pal_f;                    // palette of this frame (pal taken on f_start)
  reg       cap, armed, vs_l, vs_e, pub_pend, vsync_d;   // vs_l / vs_e: VSYNC on pixel 0 / 159
  reg [1:0] wb, pb;                   // buffer being written; fb_newest before the last frame
  reg       pend;                     // a frame has ended and waits for the dictionary
  reg       skipf;                    // this frame could not start in time: it is left out whole
  reg       hmb;                      // pixel 0 of this line was blanked with VBLANK off (HMOVE bar)
  reg       blk;                      // VBLANK or VSYNC on at PAL_X: the line is black
  reg [6:0] sh0, sh1;
  reg [2:0] ph;                       // pixel within the group of 8
  reg [4:0] grp;                      // group 0..19 = tile column - 6
  reg [3:0] tj;                       // table word of this line: 0..8, 9 = the entry-MAXLINES slot
  reg [27:0] cl;                      // COLUBK, COLUPF, COLUP0, COLUP1 of this line
  reg        clv;                     // cl holds this line's colours (pixels PAL_X + 1 .. 159)

  // the pixel's index (allocation): c_eq = which indices hold the pixel's final colour; val = indices
  // taken on this line; the colour words of a line go out during the NEXT line, from clp (copied at 159)
  wire [1:0] s_own = cvis ? csrc : 2'b00;
  wire [3:0] c_eq  = {ccol == cl[6:0], ccol == cl[13:7], ccol == cl[20:14], ccol == cl[27:21]};
  reg  [3:0] val;
  reg         capp, blkp;
  reg  [7:0]  yp;
  reg  [27:0] clp;
  wire [3:0] c_eqv   = c_eq & val;
  wire       s_hit   = |c_eqv;
  wire [1:0] s_hitix = c_eqv[s_own] ? s_own : c_eqv[0] ? 2'd0 : c_eqv[1] ? 2'd1 : c_eqv[2] ? 2'd2 : 2'd3;
  wire [1:0] s_free  = ~val[0] ? 2'd0 : ~val[1] ? 2'd1 : ~val[2] ? 2'd2 : 2'd3;
  wire       s_alloc = cvis & ~s_hit & ~(&val);
  wire [1:0] s_new   = ~val[s_own] ? s_own : s_free;
  wire [1:0] s       = ~cvis ? 2'd0 : s_hit ? s_hitix : s_alloc ? s_new : s_own;
  wire [27:0] cl_fin = {(s_alloc && s_new == 2'd0) ? ccol : cl[27:21], (s_alloc && s_new == 2'd1) ? ccol : cl[20:14],
                        (s_alloc && s_new == 2'd2) ? ccol : cl[13:7],  (s_alloc && s_new == 2'd3) ? ccol : cl[6:0]};

  // ---- line start (pixel 0): frame boundary, line number, capture decision ---------
  wire       p0       = (cx == 8'd0);
  wire       vs_rise  = vsl & ~vsync_d;
  wire       ev_vend  = pub_pend | vs_rise;                 // VSYNC rose since the last line start
  wire       ev_fall  = (vs_l | ev_vend) & ~vsl;           // ... and is low again
  wire       fell_vis = vs_l & ~vs_e;                       // it fell before pixel 159: this is line 1
  wire       auto_ev  = (vl == AUTO_LINES - 1);
  wire       f_start  = ev_fall | auto_ev;
  wire       f_end    = ev_vend | auto_ev;
  wire [8:0] vl_inc   = vl + 9'd1;
  wire [8:0] ys       = YS0 + {{2{yofs_r[6]}}, yofs_r};
  wire       cap_want = armed & ~ev_vend & ~f_start & (vl_inc >= ys) & (y != MAXLINES);
  wire       cap_new  = cap_want & ~pend & ~skipf;

  // buffer for a new frame: not fb_newest, not the locked one, rather not the old fb_newest
  wire [1:0] c1 = inc3(fb_newest);
  wire [1:0] c2 = inc3(c1);
  wire [1:0] wb_new = (lock_v & (lock_i == c1)) ? c2
                    : (lock_v & (lock_i == c2)) ? c1
                    : (pb == c1)                ? c2 : c1;

  // ---- table words: one on pixel 8*g + 3 for g = 2, 4 .. 18 ---------------------------
  // word j = 0..7 is channel j/2, word j%2; j = 8 is the window word.  The tenth slot
  // (g = 19) carries word y of entry MAXLINES on lines 0..8.
  wire       tb_term = (tj == 4'd9);
  wire       tb_reg  = ~grp[0] & (grp != 5'd0) & ~tb_term;
  wire       tb_slot = (ph == 3'd3) & (tb_reg | ((grp == 5'd19) & tb_term & (y < 8'd9)));
  // ---- zero tile words: tiles 0..5 and 26..31, one on pixel 8*g + 5 for g = 0 .. 11 ------
  wire       z_slot  = (ph == 3'd5) & (grp < 5'd12);
  wire [4:0] z_col   = (grp < 5'd6) ? grp : grp + 5'd20;
  // table slots: tj 0..4 = index words of channels 0..3 and the window word (j = 0, 2, 4, 6, 8) of this line,
  // tj 5..8 = the colour words (j = 1, 3, 5, 7) of the previous line
  wire [3:0] tj_map  = (tj < 4'd5) ? {tj[2:0], 1'b0} : {tj[2:0] - 3'd5, 1'b1};
  wire [3:0] tb_j    = tb_term ? y[3:0] : tj_map;
  wire [1:0] tb_k    = tb_j[2:1];
  wire [7:0] tb_l    = tb_term ? MAXLINES : y;
  wire [6:0] tb_cp   = (tb_k == 2'd0) ? clp[27:21] : (tb_k == 2'd1) ? clp[20:14] : (tb_k == 2'd2) ? clp[13:7] : clp[6:0];
  // Passo 27: the colour word of the next table slot, registered one pixel ahead (tb_c is already the
  // slot's on the pixel before: tj only moves on the slot itself, y on pixel 159, cl/blk on PAL_X < 18).
  // Keeps the palette ROM out of the fb_wdata mux, whose tile-word input is a 1-clock path from the TIA.
  reg [15:0] tbw;

  always @(posedge clk) begin
    if (rst) begin
      fb_wr <= 1'b0; fb_waddr <= 16'd0; fb_wdata <= 16'd0;
      fb_newest <= 2'd0; fb_frames <= 8'd0; fb_nlines <= 8'd0;
      y <= 8'd0; vl <= 9'd0; yofs_r <= yofs; pal_f <= pal; cap <= 1'b0; armed <= 1'b0; vs_l <= 1'b0; vs_e <= 1'b0;
      pub_pend <= 1'b0; vsync_d <= 1'b0; wb <= 2'd1; pb <= 2'd2;
      hmb <= 1'b0; blk <= 1'b0; sh0 <= 7'd0; sh1 <= 7'd0;
      ph <= 3'd0; grp <= 5'd0; tj <= 4'd0; cl <= 28'd0; clv <= 1'b0; tbw <= 16'd0;
      val <= 4'd0; capp <= 1'b0; blkp <= 1'b0; yp <= 8'd0; clp <= 28'd0;
      ev_row <= 1'b0; ev_end <= 1'b0; end_lines <= 8'd0; pend <= 1'b0; skipf <= 1'b0; fb_dropped <= 8'd0;
    end else begin
      fb_wr   <= 1'b0;
      vsync_d <= vsl;
      if (vs_rise) pub_pend <= 1'b1;

      if (dstb) begin
        tbw <= (tb_term | blkp) ? 16'h0000 : {1'b0, pal_bgr(pal_f, tb_cp)};
        if (s_alloc) begin
          val[s_new] <= 1'b1;
          case (s_new)
            2'd0: cl[27:21] <= ccol;
            2'd1: cl[20:14] <= ccol;
            2'd2: cl[13:7]  <= ccol;
            2'd3: cl[6:0]   <= ccol;
          endcase
        end
        sh0 <= {sh0[5:0], s[0]};
        sh1 <= {sh1[5:0], s[1]};
        if (cx == PAL_X) begin
          blk <= cvb | vsl;
          clv <= 1'b1;
        end

        if (p0) begin
          pub_pend <= 1'b0;
          vs_l     <= vsl;
          hmb      <= ~cvis & ~cvb & ~vsl;
          cap      <= cap_new;
          vl       <= f_start ? {8'd0, ev_fall & fell_vis} : vl_inc;
          if (f_start)     begin armed <= 1'b1; yofs_r <= yofs; pal_f <= pal; end
          else if (ev_vend) armed <= 1'b0;
          if (f_end) begin
            y <= 8'd0;
            if (y != 8'd0) begin                 // a frame with at least one line: hand it over
              pend      <= 1'b1;
              ev_end    <= ~ev_end;
              end_lines <= y;
            end
          end
          if (cap_new && y == 8'd0) wb <= wb_new;
          // the window starts while the last frame is still waiting for the dictionary:
          // this frame is left out whole (starting it later would shift the picture)
          if (f_start) skipf <= 1'b0;
          else if (cap_want && pend && !skipf) begin
            skipf <= 1'b1;
            if (fb_dropped != 8'hFF) fb_dropped <= fb_dropped + 8'd1;
          end
        end else if (pend && eng_ack == ev_end) begin
          pend <= 1'b0;                          // the dictionary is done with the frame
          if (eng_ovf) begin
            if (fb_dropped != 8'hFF) fb_dropped <= fb_dropped + 8'd1;
          end else begin
            fb_newest <= wb;
            pb        <= fb_newest;
            fb_frames <= fb_frames + 8'd1;
            fb_nlines <= end_lines;
          end
        end

        if (capp && tb_slot && !tb_term && tb_j[0]) begin      // colour word k of the PREVIOUS line
          fb_wr    <= 1'b1;
          fb_waddr <= {wb, 3'b100, tb_k, yp, 1'b1};
          fb_wdata <= tbw;
        end
        // nothing is written on pixel 0, so the registered cap is enough from here on
        if (cap) begin
          if (tb_slot && (tb_term || !tb_j[0])) begin
            fb_wr <= 1'b1;
            if (tb_j[3]) begin                             // window: left | right << 8
              fb_waddr <= {wb, 6'b101000, tb_l};           // +$5000 + 2*L
              fb_wdata <= tb_term      ? {8'd255, 8'd0}
                        : (hmb & ~blk) ? {8'd207, 8'd56} : {8'd207, 8'd48};
            end else begin                                 // +$4000 + k*$400 + 4*L (+2)
              fb_waddr <= {wb, 3'b100, tb_k, tb_l, tb_j[0]};
              fb_wdata <= ~tb_j[0]       ? {6'd0, tb_k, 6'd0, tb_k}
                        :                   tbw;
            end
          end
          if (z_slot) begin                                // a tile outside the picture
            fb_wr    <= 1'b1;
            fb_waddr <= {wb, 1'b0, y[7:3], z_col, y[2:0]};
            fb_wdata <= 16'h0000;
          end
          if (ph == 3'd7) begin                            // 8 pixels, both planes
            fb_wr    <= 1'b1;
            fb_waddr <= {wb, 1'b0, y[7:3], grp + 5'd6, y[2:0]};
            fb_wdata <= {sh1, s[1], sh0, s[0]};
          end
        end

        // position within the line
        if (cx == 8'd159) begin
          vs_e <= vsl;
          clv <= 1'b0; val <= 4'd0;
          capp <= cap; yp <= y; clp <= cl_fin; blkp <= blk;
          ph <= 3'd0; grp <= 5'd0; tj <= 4'd0;
          if (cap) begin
            y <= y + 8'd1;
            if (y[2:0] == 3'd7) ev_row <= ~ev_row;
          end
        end else begin
          ph <= ph + 3'd1;
          if (ph == 3'd7) grp <= grp + 5'd1;
          if (ph == 3'd3 && tb_reg) tj <= tj + 4'd1;
        end
      end
    end
  end

endmodule
