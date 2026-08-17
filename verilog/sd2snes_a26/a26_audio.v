`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// a26_audio -- TIA sound generator (2 channels), clean-room from published data.
//
// SOURCES (published documents only; no third-party RTL or emulator source was
// consulted or transcribed):
//   [SPG]  Stella Programmer's Guide (Atari, 1979), AUDCx/AUDFx/AUDVx -- the
//          16-mode table quoted verbatim in the matrix below.
//   [SFG]  E. Stolberg, "Atari 2600 VCS Sound Frequency and Waveform Guide"
//          (1997), mirrored on 7800.8bitdev.org -- prints, per mode, the
//          REPEATING WAVEFORM BIT PATTERN and the shift rate it runs at.  That
//          table is the reference this file is verified against, bit for bit,
//          by the unit bench (see tests/rtl-tb/audio_ref.py).
//   [2K6]  problemkaputt.de/2k6specs.htm, audio section.
// The waveforms are hardware facts; the logic that produces them here is ours.
//
// ---------------------------------------------------------------------------
// CLOCKING -- there are TWO shift rates, and AUDC picks which one
// ---------------------------------------------------------------------------
// TIA_CE is the 3.579545MHz color-clock enable.  The audio state machine runs
// at TIA_CE/114 -> TICK.  114 is THE DIVISOR, not a rounded frequency: a
// scanline is 228 color clocks, so TICK is exactly 2 ticks per scanline
// (3579545/114 = 31399.5Hz).  Never re-derive this constant from "31.4kHz".
//
// [SFG] states the shift registers run at "pixelclock/114 or CPUclock/114"
// (31440Hz / 10480Hz NTSC).  The CPU clock is the color clock / 3, so the
// second rate is TICK/3, and it is what AUDC $C-$F select: that is where the
// /6 and /93 of those modes come from, and it is why the poly5 ALSO runs three
// times slower in $F (its published pattern is the $7/$9 pattern at the slower
// rate).  Modelling the /6 as a full-rate 3-stage counter instead reproduces
// /6 and /93 but NOT the published $E/$F bit patterns -- checked, rejected.
//
// Every register below moves only on TICK; TICK itself is a one-CLK strobe
// aligned with TIA_CE, so nothing in this file runs off a free clock.
//
// ---------------------------------------------------------------------------
// STRUCTURE (per channel)
// ---------------------------------------------------------------------------
//   TICK -> [prescaler: AUDF+1, then /3 for the $C-$F family] -> STATE CLOCK
//   STATE CLOCK -> poly5 (5-bit LFSR, period 31)        : free running
//                  /31 level FF, toggled by the poly5 decode below
//   STATE CLOCK & GATE -> output section selected by AUDC[3:2]:
//                  poly4 (period 15) | toggle FF | poly9 (period 511) | poly5
//   GATE is selected by AUDC[1:0]: 00/01 = every state clock, 10 = the /31
//   decode, 11 = the poly5 output bit.
//   Channel level = output bit * AUDV (0..15); MIX = (ch0 + ch1) * 17, i.e. a
//   unipolar 0..510 against the 9-bit full scale of 511.  The DC (modes $0/$B
//   sit at full level) is removed by the DC blocker in the core wrapper -- do
//   not "fix" it here, AUDV is a straight DAC input on real hardware too, and
//   [SFG] documents $0/$B + volume writes as the 4-bit PCM playback mode.
//
// ---------------------------------------------------------------------------
// THE 16 MODES: [SPG] text, then the published [SFG] waveform this file emits
// ---------------------------------------------------------------------------
//   AUDC  [SPG] text                  [SFG] pattern (rate)         our path
//   $0   "Set to 1"                   always high        (1x)   forced high
//   $1   "4 bit poly"                 001010000111011    (1x)   poly4
//   $2   "div 15 -> 4 bit poly"       poly4 clocked by
//                                     0100000000000000000100000000000 (1x)
//                                                               poly4 gated by
//                                                               the /31 decode
//   $3   "5 bit poly -> 4 bit poly"   poly4 clocked by the poly5 pattern (1x)
//                                                               poly4 gated by
//                                                               poly5
//   $4   "div 2 pure tone"            01                 (1x)   toggle FF
//   $5   "div 2 pure tone"            01                 (1x)   same as $4
//   $6   "div 31 pure tone"           1111111111111
//                                     000000000000000000 (1x)   /31 level FF
//   $7   "5 bit poly -> div 2"        0010110011111000
//                                     110111010100001    (1x)   toggle FF gated
//                                                               by poly5
//   $8   "9 bit poly (white noise)"   "511 bits long"    (1x)   poly9
//   $9   "5 bit poly"                 same 31-bit pattern as $7 (1x)  poly5
//   $A   "div 31 pure tone"           same as $6         (1x)   /31 level FF
//   $B   "Set last 4 bits to 1"       always high        (1x)   forced high
//   $C   "div 6 pure tone"            10                 (/3)   toggle FF
//   $D   "div 6 pure tone"            10                 (/3)   same as $C
//   $E   "div 93 pure tone"           same as $6         (/3)   /31 level FF
//   $F   "5 bit poly -> div 6"        same as $7         (/3)   toggle FF gated
//                                                               by poly5
//
// NOTE 1  THE /31 DIVIDER IS A DECODE OF THE POLY5, NOT A MOD-31 COUNTER.  Two
//   of the poly5's 31 states are decoded ((p5 & 5'h1e) == 5'h02); they fall 13
//   and 18 state clocks apart, so the level FF they toggle spends 13 states
//   high and 18 low -- exactly the [SFG] $6/$A pattern (thirteen 1s then
//   eighteen 0s), and exactly the spacing of the two 1s in the [SFG] clock
//   modifier printed for $2.  A symmetric 16/15 divider has the same pitch but
//   the wrong timbre: a 50%-duty rectangle has no even harmonics at all, while
//   the real 13:18 pulse has plenty (the 2nd harmonic alone moves by ~13dB).
//   Decoding the poly5 also means the divider has no state of its own beyond
//   the level FF.
// NOTE 2  POLY4 POLARITY.  [SFG]'s $1 pattern has seven 1s in fifteen states,
//   i.e. it is the COMPLEMENT of the plain XOR m-sequence this LFSR generates
//   (which has eight).  The poly4 section therefore outputs the inverted tap.
//   Verified as a cyclic rotation of the published string, and it also lands
//   $2 and $3 on their published 465-bit patterns.
// NOTE 3  TOGGLE FF RESET VALUE IS NOT ARBITRARY.  For $7/$F the FF integrates
//   the poly5 bit stream, and the running parity of an m-sequence is either a
//   shift of that same m-sequence or its complement, decided purely by the
//   FF's starting value.  [SFG] prints the SAME pattern for $7 and $9, so the
//   shift is the correct one: out_r must reset to 1 (with 0 the channel emits
//   the complement -- fifteen 1s per 31 instead of sixteen).  For $4/$5/$C/$D
//   this only moves the phase, which is why the published table can print "01"
//   for $4/$5 and "10" for $C/$D.
// NOTE 4  SELF-STARTING LFSRs.  The feedback of all three polys is forced when
//   the register reads all zeros, so a power-up state of 0 (every Cyclone IV FF
//   comes up at 0) cannot leave $1/$2/$3/$7/$8/$9/$F stuck silent if RST is
//   ever missed.  Costs two LUTs and changes no steady-state waveform.
// NOTE 5  PRESCALER IS "==", WITH A WRAP GUARD.  The counter fires when it
//   equals AUDF, exactly like the chip: writing a SMALLER AUDF while the count
//   is already past it makes the channel run once around the 5-bit counter
//   before the new pitch takes effect.  That micro-stall (up to 32 ticks, ~1ms)
//   is real hardware behaviour that the games' sound drivers were written
//   against, so it is kept; the "== 31" term is only the counter's own wrap and
//   costs nothing.
// NOTE 6  poly5 and the /31 level FF free-run on the state clock; poly4 and
//   poly9 only advance when the gate lets the state clock through.
//
// LFSR taps (all primitive; the poly5 tap is confirmed by [SFG]'s $7/$9 string,
// which is a cyclic rotation of the sequence this tap produces):
//   poly4: x^4+x^3+1   (feedback = bit0 ^ bit1)   period 15, output inverted
//   poly5: x^5+x^3+1   (feedback = bit0 ^ bit2)   period 31
//   poly9: x^9+x^5+1   (feedback = bit0 ^ bit4)   period 511
//////////////////////////////////////////////////////////////////////////////////

module a26_audio(
  input        CLK,
  input        RST,
  input        TIA_CE,          // 3.579545MHz color-clock enable

  input  [3:0] AUDC0,
  input  [3:0] AUDC1,
  input  [4:0] AUDF0,
  input  [4:0] AUDF1,
  input  [3:0] AUDV0,
  input  [3:0] AUDV1,

  output [8:0] MIX,             // unipolar 0..510
  output       TICK             // one-CLK strobe, TIA_CE/114 (31.3995kHz)
);

  // ---------------------------------------------------------------------------
  // audio tick: color clock / 114 (= 2 per scanline)
  // ---------------------------------------------------------------------------
  localparam [6:0] TICK_DIV = 7'd114;

  reg [6:0] tick_cnt;
  always @(posedge CLK) begin
    if (RST) tick_cnt <= 7'd0;
    else if (TIA_CE)
      tick_cnt <= (tick_cnt == TICK_DIV - 7'd1) ? 7'd0 : tick_cnt + 7'd1;
  end
  assign TICK = TIA_CE & (tick_cnt == TICK_DIV - 7'd1);

  // ---------------------------------------------------------------------------
  // two channels
  // ---------------------------------------------------------------------------
  wire [4:0] lvl0_n, lvl1_n;    // next-tick levels, 0..15

  a26_audio_ch ch0(
    .CLK(CLK), .RST(RST), .TICK(TICK),
    .AUDC(AUDC0), .AUDF(AUDF0), .AUDV(AUDV0), .LEVEL_NEXT(lvl0_n)
  );

  a26_audio_ch ch1(
    .CLK(CLK), .RST(RST), .TICK(TICK),
    .AUDC(AUDC1), .AUDF(AUDF1), .AUDV(AUDV1), .LEVEL_NEXT(lvl1_n)
  );

  // ---------------------------------------------------------------------------
  // mono mix: (ch0 + ch1) * 17, sum 0..30 -> 0..510 (fits 9 bits, peak below
  // the 511 full scale the wrapper's conditioning expects)
  // ---------------------------------------------------------------------------
  wire [5:0] sum_n = {1'b0, lvl0_n} + {1'b0, lvl1_n};
  wire [9:0] mix_n = {sum_n, 4'd0} + {4'd0, sum_n};   // x16 + x1, <= 510

  reg [8:0] mix_r;
  always @(posedge CLK) begin
    if (RST) mix_r <= 9'd0;
    else if (TICK) mix_r <= mix_n[8:0];
  end
  assign MIX = mix_r;

endmodule

//////////////////////////////////////////////////////////////////////////////////
// one TIA audio channel (see the mode matrix in the header above)
//////////////////////////////////////////////////////////////////////////////////
module a26_audio_ch(
  input        CLK,
  input        RST,
  input        TICK,

  input  [3:0] AUDC,
  input  [4:0] AUDF,
  input  [3:0] AUDV,

  output [4:0] LEVEL_NEXT       // level this channel will hold after this TICK
);

  reg [4:0] pre_cnt;            // AUDF prescaler
  reg [1:0] div3_cnt;           // the CPUclock/114 rate of the $C-$F family
  reg [4:0] p5;                 // 5-bit poly, free running
  reg [3:0] p4;                 // 4-bit poly, gated
  reg [8:0] p9;                 // 9-bit poly, gated
  reg       d31lvl;             // /31 level, toggled by the poly5 decode
  reg       out_r;              // output flip-flop of the shifting sections

  // --- state clock -----------------------------------------------------------
  wire slow      = (AUDC[3:2] == 2'b11);      // $C-$F run at TICK/3 (see header)
  wire pre_wrap  = (pre_cnt == AUDF) | (pre_cnt == 5'd31);   // NOTE 5
  wire div3_wrap = (div3_cnt == 2'd2);
  wire state_clk = pre_wrap & (~slow | div3_wrap);

  // --- section / gate select -------------------------------------------------
  wire sect_poly4 = (AUDC[3:2] == 2'b00);
  wire sect_shreg = (AUDC[3:2] == 2'b10);     // poly9 ($8) or poly5 ($9/$A/$B)
  wire sect_poly9 = sect_shreg & (AUDC[1:0] == 2'b00);

  // NOTE 1: two poly5 states, 13 and 18 state clocks apart
  wire d31_dec = ((p5 & 5'h1E) == 5'h02);

  wire gate = (AUDC[1] == 1'b0) ? 1'b1
            : (AUDC[0] == 1'b0) ? d31_dec
                                : p5[0];      // sampled before poly5 shifts

  // --- next state ------------------------------------------------------------
  wire fb5 = (p5[0] ^ p5[2]) | (p5 == 5'd0);  // NOTE 4: self-starting
  wire fb4 = (p4[0] ^ p4[1]) | (p4 == 4'd0);
  wire fb9 = (p9[0] ^ p9[4]) | (p9 == 9'd0);

  wire [4:0] p5_n   = state_clk ? {fb5, p5[4:1]} : p5;
  wire       d31l_n = (state_clk & d31_dec) ? ~d31lvl : d31lvl;
  wire       shift  = state_clk & gate;
  wire [3:0] p4_n   = (shift & sect_poly4) ? {fb4, p4[3:1]} : p4;
  wire [8:0] p9_n   = (shift & sect_poly9) ? {fb9, p9[8:1]} : p9;

  wire out_r_n = !shift     ? out_r
               : sect_poly4 ? ~p4_n[0]        // NOTE 2: published polarity
               : sect_shreg ? (sect_poly9 ? p9_n[0] : p5_n[0])
                            : ~out_r;         // $4-$7 and $C-$F

  // $0 / $B shift ones into the output stage until it sits high: a steady DC
  // level, which is why "AUDC = 0" is how games mute a channel (and why $0/$B
  // plus volume writes is the published 4-bit PCM playback trick).
  wire const_hi    = (AUDC == 4'h0) | (AUDC == 4'hB);
  // $6/$A/$E: the /31 level IS the waveform
  wire use_d31_lvl = (AUDC[1:0] == 2'b10) & ~sect_poly4;

  wire out_bit_n = const_hi    ? 1'b1
                 : use_d31_lvl ? d31l_n
                               : out_r_n;

  assign LEVEL_NEXT = out_bit_n ? {1'b0, AUDV} : 5'd0;

  // --- registers -------------------------------------------------------------
  always @(posedge CLK) begin
    if (RST) begin
      pre_cnt  <= 5'd0;
      div3_cnt <= 2'd0;
      p5       <= 5'h1F;
      p4       <= 4'hF;
      p9       <= 9'h1FF;
      d31lvl   <= 1'b0;
      out_r    <= 1'b1;         // NOTE 3: not arbitrary
    end else if (TICK) begin
      pre_cnt  <= pre_wrap ? 5'd0 : pre_cnt + 5'd1;
      if (pre_wrap) div3_cnt <= div3_wrap ? 2'd0 : div3_cnt + 2'd1;
      p5       <= p5_n;
      p4       <= p4_n;
      p9       <= p9_n;
      d31lvl   <= d31l_n;
      out_r    <= out_r_n;
    end
  end

endmodule
