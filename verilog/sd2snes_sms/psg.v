// sd2snes SMS core -- SN76489 PSG (the SMS/Game Gear variant, integrated in the
// VDP).  Three square-wave tone channels + one noise channel, 4-bit attenuation
// each, written through the Z80 I/O ports $40-$7F.
//
// CLOCKING.  The PSG is fed from the same 3.579545 MHz the Z80 runs at (CE from
// main.v's fractional-N accumulator) and prescales it by 16 -> 223.72 kHz, the
// rate at which every counter in the chip advances.  A tone counter reloads and
// TOGGLES its flip-flop each time it expires, so the output frequency is
// clk/(32*N) -- 109 Hz at N=$3FF up to 111.8 kHz at N=1.
//
// EVERYTHING IS REGISTERED and every cone is one adder/comparator deep: the SMS
// core is synthesised with IGNORE_TIMING=1 (main.sdc has no multicycles for the
// Z80/VDP), so new logic has to be trivially fast at 96 MHz by construction
// rather than by STA.
//
// SMS-VARIANT DETAILS THAT DIFFER FROM THE ORIGINAL SN76489 (see the spec notes
// next to this file for confidence levels):
//   * noise LFSR is 16 bits with taps 0 and 3 (the SG-1000/BBC part uses 15
//     bits, taps 0 and 1) -- getting this wrong changes every drum in every game
//   * the LFSR is reseeded to $8000 on a write to the noise control register
//   * the LFSR advances only on the noise flip-flop's 0->1 transition, i.e. once
//     per TWO counter expiries -> shift rate = clk/512, /1024, /2048
//   * a tone period of 0 holds the output at +1 instead of oscillating; games
//     use that (period 0 + volume-register modulation) to play PCM
module psg (
  input             CLK,   // CLK2 (96 MHz)
  input             RST,
  input             CE,    // SMS clock enable (~3.579545 MHz, 1-CLK pulse)
  input             WE,    // I/O write strobe to $40-$7F (1-CLK pulse)
  input      [7:0]  D,     // byte the Z80 wrote
  output reg [10:0] MIX,   // unsigned sum of the 4 channels, 0..1020
  output reg        TICK   // 1-CLK pulse at the 223.72 kHz internal rate
);

  // ---------------- /16 prescaler ----------------
  reg [3:0] presc;
  always @(posedge CLK) begin
    TICK <= 1'b0;
    if (RST) presc <= 4'd0;
    else if (CE) begin
      presc <= presc + 4'd1;
      TICK  <= (presc == 4'd15);
    end
  end

  // ---------------- register file + write decode ----------------
  // A byte with bit7=1 LATCHES a register (bits 6:5 = channel, bit4 = 1 for
  // volume) and carries the low 4 data bits; a byte with bit7=0 carries 6 more
  // bits into the LATCHED register (tone period only -- volume and the noise
  // control register are 4/3 bits and take the low nibble either way).
  reg [1:0] lat_ch;
  reg       lat_vol;
  reg [9:0] period [0:2];     // tone periods (10 bits)
  reg [2:0] nctl;             // noise: bit2 = white/periodic, bits1:0 = rate
  reg [7:0] lvl    [0:3];     // POST-attenuation level, updated on write only

  wire [1:0] w_ch  = D[7] ? D[6:5] : lat_ch;
  wire       w_vol = D[7] ? D[4]   : lat_vol;
  wire       w_noise_ctl = WE & ~w_vol & (w_ch == 2'b11);

  // 2 dB per attenuation step, 15 = off.  Peak 255 per channel is the calibration
  // point: 4 channels sum to 1020, so the DC-blocked mix in sms_core lands
  // exactly on the 10-bit signed full scale the dac.v sgb channel expects.
  function [7:0] atten2lvl(input [3:0] a);
    case (a)
      4'd0:  atten2lvl = 8'd255;  4'd1:  atten2lvl = 8'd203;
      4'd2:  atten2lvl = 8'd161;  4'd3:  atten2lvl = 8'd128;
      4'd4:  atten2lvl = 8'd102;  4'd5:  atten2lvl = 8'd81;
      4'd6:  atten2lvl = 8'd64;   4'd7:  atten2lvl = 8'd51;
      4'd8:  atten2lvl = 8'd40;   4'd9:  atten2lvl = 8'd32;
      4'd10: atten2lvl = 8'd26;   4'd11: atten2lvl = 8'd20;
      4'd12: atten2lvl = 8'd16;   4'd13: atten2lvl = 8'd13;
      4'd14: atten2lvl = 8'd10;   default: atten2lvl = 8'd0;
    endcase
  endfunction

  integer k;
  always @(posedge CLK) begin
    if (RST) begin
      lat_ch <= 2'd0; lat_vol <= 1'b0; nctl <= 3'd0;
      for (k = 0; k < 3; k = k + 1) period[k] <= 10'd0;
      for (k = 0; k < 4; k = k + 1) lvl[k] <= 8'd0;   // attenuation $F = silent
    end else if (WE) begin
      if (D[7]) begin lat_ch <= D[6:5]; lat_vol <= D[4]; end
      if (w_vol)                  lvl[w_ch] <= atten2lvl(D[3:0]);
      else if (w_ch == 2'b11)     nctl <= D[2:0];
      else if (D[7])              period[w_ch][3:0] <= D[3:0];
      else                        period[w_ch][9:4] <= D[5:0];
    end
  end

  // ---------------- tone channels ----------------
  reg [9:0] tcnt [0:2];
  reg       tff  [0:2];
  integer   t;
  always @(posedge CLK) begin
    if (RST) begin
      for (t = 0; t < 3; t = t + 1) begin tcnt[t] <= 10'd0; tff[t] <= 1'b0; end
    end else if (TICK) begin
      for (t = 0; t < 3; t = t + 1) begin
        if (tcnt[t] <= 10'd1) begin tcnt[t] <= period[t]; tff[t] <= ~tff[t]; end
        else                        tcnt[t] <= tcnt[t] - 10'd1;
      end
    end
  end

  // ---------------- noise channel ----------------
  // Rate 3 follows tone2's period register (that is how games sweep the noise
  // pitch); a period of 0 there would free-run the counter, so clamp to 1.
  reg  [9:0]  ncnt;
  reg         nff;
  reg  [15:0] lfsr;
  wire [9:0]  nper = (nctl[1:0] == 2'b00) ? 10'd16
                   : (nctl[1:0] == 2'b01) ? 10'd32
                   : (nctl[1:0] == 2'b10) ? 10'd64
                   : (period[2] == 10'd0) ? 10'd1 : period[2];
  wire        nfb  = nctl[2] ? (lfsr[0] ^ lfsr[3]) : lfsr[0];

  always @(posedge CLK) begin
    if (RST) begin ncnt <= 10'd16; nff <= 1'b0; lfsr <= 16'h8000; end
    else begin
      if (TICK) begin
        if (ncnt <= 10'd1) begin
          ncnt <= nper;
          nff  <= ~nff;
          if (~nff) lfsr <= {nfb, lfsr[15:1]};   // shift on the 0->1 edge only
        end else ncnt <= ncnt - 10'd1;
      end
      // a write to the noise register reseeds; it must WIN over a coincident
      // shift, hence after the TICK block.  This only works because both
      // assignments live in the SAME always block: the last non-blocking
      // assignment to lfsr in one block wins.  Splitting the reseed into a
      // second always block would be a multiple-driver error in synthesis and
      // a race in simulation -- keep them together.
      if (w_noise_ctl) lfsr <= 16'h8000;
    end
  end

  // ---------------- mix ----------------
  // Unipolar, exactly like the chip: a channel contributes its level while its
  // output bit is 1 and 0 otherwise.  Two registered stages (gate, then sum) so
  // the widest cone is a 4-input 8-bit adder tree.
  wire [3:0] chout = {lfsr[0],
                      (period[2] == 10'd0) ? 1'b1 : tff[2],
                      (period[1] == 10'd0) ? 1'b1 : tff[1],
                      (period[0] == 10'd0) ? 1'b1 : tff[0]};
  reg [7:0] g [0:3];
  integer   m;
  always @(posedge CLK) begin
    if (RST) begin
      for (m = 0; m < 4; m = m + 1) g[m] <= 8'd0;
      MIX <= 11'd0;
    end else if (TICK) begin
      for (m = 0; m < 4; m = m + 1) g[m] <= chout[m] ? lvl[m] : 8'd0;
      MIX <= {3'd0, g[0]} + {3'd0, g[1]} + {3'd0, g[2]} + {3'd0, g[3]};
    end
  end

endmodule
