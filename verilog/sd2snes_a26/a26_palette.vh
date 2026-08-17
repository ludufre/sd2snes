// a26_palette.vh -- TIA colour (7 bits) -> SNES BGR555 lookup table.
//
// GENERATED FILE -- DO NOT EDIT BY HAND.  Regenerate with the host tool
// that owns the formula (see the core's test tree); the table below is a
// pure function of it, so hand edits desynchronise RTL and golden model.
//
// The TIA colour register is %HHHH LLLx: hue = bits 6:3, luma = bits 2:0
// of this 7-bit index (D0 of the register is ignored by the hardware).
// The entry is computed from the NTSC signal the TIA actually emits -- a
// linear luma plus a chroma subcarrier of CONSTANT amplitude whose PHASE
// is the hue index:
//
//   Y     = luma / 7                       (0..1; $00 black, $0E white)
//   S     = 0.3000, or 0 for hue 0 (grey)
//   theta = -25.0 + (hue-1) * 24.7 degrees
//   I     = S*cos(theta) ; Q = S*sin(theta)
//   R     = Y + 0.9563*I + 0.6210*Q
//   G     = Y - 0.2721*I - 0.6474*Q
//   B     = Y - 1.1070*I + 1.7046*Q
//   each clamped to [0,1], rounded to 5 bits, packed (B<<10)|(G<<5)|R.
//
// No gamma correction: the SNES shows the CGRAM word as-is and the goal
// is consistency between RTL, model and the visual round-trip, not
// reference colorimetry.

(* ramstyle = "M9K" *) reg [14:0] a26_pal_rom [0:127];
initial begin
  a26_pal_rom[  0] = 15'h0000;  // hue  0 luma 0
  a26_pal_rom[  1] = 15'h1084;  // hue  0 luma 1
  a26_pal_rom[  2] = 15'h2529;  // hue  0 luma 2
  a26_pal_rom[  3] = 15'h35ad;  // hue  0 luma 3
  a26_pal_rom[  4] = 15'h4a52;  // hue  0 luma 4
  a26_pal_rom[  5] = 15'h5ad6;  // hue  0 luma 5
  a26_pal_rom[  6] = 15'h6f7b;  // hue  0 luma 6
  a26_pal_rom[  7] = 15'h7fff;  // hue  0 luma 7
  a26_pal_rom[  8] = 15'h0006;  // hue  1 luma 0
  a26_pal_rom[  9] = 15'h00aa;  // hue  1 luma 1
  a26_pal_rom[ 10] = 15'h012e;  // hue  1 luma 2
  a26_pal_rom[ 11] = 15'h01d3;  // hue  1 luma 3
  a26_pal_rom[ 12] = 15'h0a57;  // hue  1 luma 4
  a26_pal_rom[ 13] = 15'h1adc;  // hue  1 luma 5
  a26_pal_rom[ 14] = 15'h2f7f;  // hue  1 luma 6
  a26_pal_rom[ 15] = 15'h3fff;  // hue  1 luma 7
  a26_pal_rom[ 16] = 15'h0009;  // hue  2 luma 0
  a26_pal_rom[ 17] = 15'h004d;  // hue  2 luma 1
  a26_pal_rom[ 18] = 15'h00d2;  // hue  2 luma 2
  a26_pal_rom[ 19] = 15'h0d76;  // hue  2 luma 3
  a26_pal_rom[ 20] = 15'h1dfb;  // hue  2 luma 4
  a26_pal_rom[ 21] = 15'h329f;  // hue  2 luma 5
  a26_pal_rom[ 22] = 15'h431f;  // hue  2 luma 6
  a26_pal_rom[ 23] = 15'h57bf;  // hue  2 luma 7
  a26_pal_rom[ 24] = 15'h000a;  // hue  3 luma 0
  a26_pal_rom[ 25] = 15'h080f;  // hue  3 luma 1
  a26_pal_rom[ 26] = 15'h1893;  // hue  3 luma 2
  a26_pal_rom[ 27] = 15'h2918;  // hue  3 luma 3
  a26_pal_rom[ 28] = 15'h3dbc;  // hue  3 luma 4
  a26_pal_rom[ 29] = 15'h4e3f;  // hue  3 luma 5
  a26_pal_rom[ 30] = 15'h62df;  // hue  3 luma 6
  a26_pal_rom[ 31] = 15'h735f;  // hue  3 luma 7
  a26_pal_rom[ 32] = 15'h140a;  // hue  4 luma 0
  a26_pal_rom[ 33] = 15'h280f;  // hue  4 luma 1
  a26_pal_rom[ 34] = 15'h3873;  // hue  4 luma 2
  a26_pal_rom[ 35] = 15'h4cf7;  // hue  4 luma 3
  a26_pal_rom[ 36] = 15'h5d9c;  // hue  4 luma 4
  a26_pal_rom[ 37] = 15'h6e1f;  // hue  4 luma 5
  a26_pal_rom[ 38] = 15'h7e9f;  // hue  4 luma 6
  a26_pal_rom[ 39] = 15'h7f3f;  // hue  4 luma 7
  a26_pal_rom[ 40] = 15'h3008;  // hue  5 luma 0
  a26_pal_rom[ 41] = 15'h440c;  // hue  5 luma 1
  a26_pal_rom[ 42] = 15'h5451;  // hue  5 luma 2
  a26_pal_rom[ 43] = 15'h68f5;  // hue  5 luma 3
  a26_pal_rom[ 44] = 15'h797a;  // hue  5 luma 4
  a26_pal_rom[ 45] = 15'h7e1e;  // hue  5 luma 5
  a26_pal_rom[ 46] = 15'h7e9f;  // hue  5 luma 6
  a26_pal_rom[ 47] = 15'h7f3f;  // hue  5 luma 7
  a26_pal_rom[ 48] = 15'h4404;  // hue  6 luma 0
  a26_pal_rom[ 49] = 15'h5809;  // hue  6 luma 1
  a26_pal_rom[ 50] = 15'h686d;  // hue  6 luma 2
  a26_pal_rom[ 51] = 15'h7912;  // hue  6 luma 3
  a26_pal_rom[ 52] = 15'h7d96;  // hue  6 luma 4
  a26_pal_rom[ 53] = 15'h7e3b;  // hue  6 luma 5
  a26_pal_rom[ 54] = 15'h7ebf;  // hue  6 luma 6
  a26_pal_rom[ 55] = 15'h7f3f;  // hue  6 luma 7
  a26_pal_rom[ 56] = 15'h4c00;  // hue  7 luma 0
  a26_pal_rom[ 57] = 15'h5c24;  // hue  7 luma 1
  a26_pal_rom[ 58] = 15'h70a9;  // hue  7 luma 2
  a26_pal_rom[ 59] = 15'h7d4d;  // hue  7 luma 3
  a26_pal_rom[ 60] = 15'h7dd2;  // hue  7 luma 4
  a26_pal_rom[ 61] = 15'h7e56;  // hue  7 luma 5
  a26_pal_rom[ 62] = 15'h7efb;  // hue  7 luma 6
  a26_pal_rom[ 63] = 15'h7f7f;  // hue  7 luma 7
  a26_pal_rom[ 64] = 15'h4400;  // hue  8 luma 0
  a26_pal_rom[ 65] = 15'h5860;  // hue  8 luma 1
  a26_pal_rom[ 66] = 15'h6904;  // hue  8 luma 2
  a26_pal_rom[ 67] = 15'h7989;  // hue  8 luma 3
  a26_pal_rom[ 68] = 15'h7e2d;  // hue  8 luma 4
  a26_pal_rom[ 69] = 15'h7eb2;  // hue  8 luma 5
  a26_pal_rom[ 70] = 15'h7f56;  // hue  8 luma 6
  a26_pal_rom[ 71] = 15'h7fdb;  // hue  8 luma 7
  a26_pal_rom[ 72] = 15'h3040;  // hue  9 luma 0
  a26_pal_rom[ 73] = 15'h44c0;  // hue  9 luma 1
  a26_pal_rom[ 74] = 15'h5561;  // hue  9 luma 2
  a26_pal_rom[ 75] = 15'h69e5;  // hue  9 luma 3
  a26_pal_rom[ 76] = 15'h7a6a;  // hue  9 luma 4
  a26_pal_rom[ 77] = 15'h7f0e;  // hue  9 luma 5
  a26_pal_rom[ 78] = 15'h7f92;  // hue  9 luma 6
  a26_pal_rom[ 79] = 15'h7ff7;  // hue  9 luma 7
  a26_pal_rom[ 80] = 15'h1480;  // hue 10 luma 0
  a26_pal_rom[ 81] = 15'h2920;  // hue 10 luma 1
  a26_pal_rom[ 82] = 15'h39a0;  // hue 10 luma 2
  a26_pal_rom[ 83] = 15'h4a23;  // hue 10 luma 3
  a26_pal_rom[ 84] = 15'h5ec8;  // hue 10 luma 4
  a26_pal_rom[ 85] = 15'h6f4c;  // hue 10 luma 5
  a26_pal_rom[ 86] = 15'h7ff0;  // hue 10 luma 6
  a26_pal_rom[ 87] = 15'h7ff5;  // hue 10 luma 7
  a26_pal_rom[ 88] = 15'h00c0;  // hue 11 luma 0
  a26_pal_rom[ 89] = 15'h0540;  // hue 11 luma 1
  a26_pal_rom[ 90] = 15'h19e0;  // hue 11 luma 2
  a26_pal_rom[ 91] = 15'h2a63;  // hue 11 luma 3
  a26_pal_rom[ 92] = 15'h3f07;  // hue 11 luma 4
  a26_pal_rom[ 93] = 15'h4f8c;  // hue 11 luma 5
  a26_pal_rom[ 94] = 15'h63f0;  // hue 11 luma 6
  a26_pal_rom[ 95] = 15'h73f5;  // hue 11 luma 7
  a26_pal_rom[ 96] = 15'h00e0;  // hue 12 luma 0
  a26_pal_rom[ 97] = 15'h0160;  // hue 12 luma 1
  a26_pal_rom[ 98] = 15'h01e0;  // hue 12 luma 2
  a26_pal_rom[ 99] = 15'h0e84;  // hue 12 luma 3
  a26_pal_rom[100] = 15'h1f09;  // hue 12 luma 4
  a26_pal_rom[101] = 15'h33ad;  // hue 12 luma 5
  a26_pal_rom[102] = 15'h43f2;  // hue 12 luma 6
  a26_pal_rom[103] = 15'h57f6;  // hue 12 luma 7
  a26_pal_rom[104] = 15'h00c0;  // hue 13 luma 0
  a26_pal_rom[105] = 15'h0140;  // hue 13 luma 1
  a26_pal_rom[106] = 15'h01e3;  // hue 13 luma 2
  a26_pal_rom[107] = 15'h0268;  // hue 13 luma 3
  a26_pal_rom[108] = 15'h0b0c;  // hue 13 luma 4
  a26_pal_rom[109] = 15'h1b91;  // hue 13 luma 5
  a26_pal_rom[110] = 15'h2bf5;  // hue 13 luma 6
  a26_pal_rom[111] = 15'h3ff9;  // hue 13 luma 7
  a26_pal_rom[112] = 15'h0080;  // hue 14 luma 0
  a26_pal_rom[113] = 15'h0123;  // hue 14 luma 1
  a26_pal_rom[114] = 15'h01a8;  // hue 14 luma 2
  a26_pal_rom[115] = 15'h024c;  // hue 14 luma 3
  a26_pal_rom[116] = 15'h02d0;  // hue 14 luma 4
  a26_pal_rom[117] = 15'h0f55;  // hue 14 luma 5
  a26_pal_rom[118] = 15'h23f9;  // hue 14 luma 6
  a26_pal_rom[119] = 15'h33fe;  // hue 14 luma 7
  a26_pal_rom[120] = 15'h0043;  // hue 15 luma 0
  a26_pal_rom[121] = 15'h00c8;  // hue 15 luma 1
  a26_pal_rom[122] = 15'h016c;  // hue 15 luma 2
  a26_pal_rom[123] = 15'h01f1;  // hue 15 luma 3
  a26_pal_rom[124] = 15'h0295;  // hue 15 luma 4
  a26_pal_rom[125] = 15'h1319;  // hue 15 luma 5
  a26_pal_rom[126] = 15'h279e;  // hue 15 luma 6
  a26_pal_rom[127] = 15'h37ff;  // hue 15 luma 7
end
