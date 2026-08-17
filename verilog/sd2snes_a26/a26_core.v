// sd2snes a26 core -- the integrated Atari 2600 subsystem.
//
// Wires a26.v (6507 + RIOT + cartridge + TIA + audio) to the chassis:
//   - the .a26 image is copied out of PSRAM into the cartridge block RAM at
//     boot via ROM_* (the main.v arbiter services this as a free-slot client),
//   - the video packer (a26_video) turns the TIA's per-pixel code stream into
//     SNES-format tiles and control blocks and streams them to the PSRAM
//     buffer region via BUF_* (another free-slot client, with BUF_WACK
//     backpressure since PSRAM writes are slower than a byte per cycle),
//   - the TIA's two audio channels are conditioned here and handed to dac.v's
//     cartridge channel.
//
// Buffer region (PSRAM): the wrapper emits BUF_ADDR = BUF_BASE + offset and
// main.v adds (front ? 0x180000 : 0x190000) on top, so the core writes the BACK
// set: front=1 -> 0x380000 (set A), front=0 -> 0x390000 (set B).  address.v
// reads the FRONT set with the opposite polarity ("front ? 0x390000 :
// 0x380000") -- the two must stay in sync.
`timescale 1ns / 1ps

module a26_core (
  input             CLK,
  input             RST,
  input             TIA_CE,           // color clock enable (3.579545MHz)
  input             CPU_CE,           // CPU cycle enable (TIA_CE / 3)

  input      [15:0] FEAT,             // CHIPFEAT 0xef (contract 7)

  // .a26 image read (PSRAM, via arbiter) -- boot copy only
  output            ROM_RRQ,
  output     [23:0] ROM_ADDR,
  input      [7:0]  ROM_DATA,
  input             ROM_RDY,

  // packed-frame write (PSRAM, via arbiter) -- driven by a26_video
  output            BUF_WRQ,
  output     [23:0] BUF_ADDR,
  output     [7:0]  BUF_DATA,
  input             BUF_WACK,

  // controls, mapped by main.v from the $EF player window (contract 6)
  input      [7:0]  SWCHA_IN,
  input      [7:0]  SWCHB_IN,
  input      [1:0]  INPT45,

  input             TR_GATE,          // back buffer not consumed yet: hold off
  input             BACK_SET,         // which set this pass writes (= ~front)
  input             SWAP_CONSUMED,    // the player took the swap: dirty bitmap clears
  output            FRAME_DONE,       // pulses when a packed frame is complete

  // audio to dac.v's cartridge channel: {ch1[9:0], ch0[9:0]}, both fed the
  // same mono sample, plus one strobe per sample (see AUDIO CONDITIONING)
  output     [19:0] APU_DAT,
  output            APU_CLK_EDGE,

  // debug / instrumentation
  output     [12:0] DBG_AB,
  output     [2:0]  DBG_BANK,
  output            DBG_WSYNC,
  output            DBG_BOOT
);

  localparam [23:0] BUF_BASE = 24'h200000;

  // ---- console ---------------------------------------------------------
  wire        vid_visible, line_start, line_vis_start, vsync_edge, vblank_on;
  wire [1:0]  vid_code;
  wire [6:0]  colubk, colupf, colup0, colup1;
  wire [7:0]  ctrlpf;
  wire [8:0]  au_mix;
  wire        au_tick;

  a26 a26_inst (
    .CLK(CLK), .RST(RST), .TIA_CE(TIA_CE), .CPU_CE(CPU_CE),
    .FEAT(FEAT),
    .ROM_RRQ(ROM_RRQ), .ROM_ADDR(ROM_ADDR), .ROM_DATA(ROM_DATA), .ROM_RDY(ROM_RDY),
    .BOOT_ACTIVE(DBG_BOOT),
    .SWCHA_IN(SWCHA_IN), .SWCHB_IN(SWCHB_IN), .INPT45(INPT45),
    .VID_VISIBLE(vid_visible), .VID_CODE(vid_code),
    .LINE_START(line_start), .LINE_VIS_START(line_vis_start),
    .COLUBK(colubk), .COLUPF(colupf), .COLUP0(colup0), .COLUP1(colup1),
    .CTRLPF(ctrlpf), .VSYNC_EDGE(vsync_edge), .VBLANK_ON(vblank_on),
    .AU_MIX(au_mix), .AU_TICK(au_tick),
    .DBG_AB(DBG_AB), .DBG_BANK(DBG_BANK), .DBG_WSYNC(DBG_WSYNC)
  );

  // ---- video packer ----------------------------------------------------
  // a26_video turns (vid_visible, vid_code, the line marks and the per-line
  // colour registers) into the tile + control + HDMA byte stream and IS the
  // PSRAM write client itself -- it drives BUF_* and honours BUF_WACK; the
  // BUF_* outputs of this wrapper come straight from it (BUF_ADDR already
  // carries BUF_BASE + set offset; main.v adds 0x180000/0x190000).
  a26_video vid (
    .CLK(CLK), .RST(RST), .TIA_CE(TIA_CE), .WIDTH_256(FEAT[5]),
    .VID_VISIBLE(vid_visible), .VID_CODE(vid_code),
    .LINE_START(line_start), .LINE_VIS_START(line_vis_start),
    .COLUBK(colubk), .COLUPF(colupf), .COLUP0(colup0), .COLUP1(colup1),
    .VSYNC_EDGE(vsync_edge),
    .BUF_WRQ(BUF_WRQ), .BUF_ADDR(BUF_ADDR), .BUF_DATA(BUF_DATA),
    .BUF_WACK(BUF_WACK),
    .TR_GATE(TR_GATE), .SWAP_CONSUMED(SWAP_CONSUMED), .FRAME_DONE(FRAME_DONE),
    .DBG_LINES(), .DBG_OVERRUN(), .DBG_DIRTY()
  );

  // ---------------------------------------------------------------------
  // AUDIO CONDITIONING (mirrors sms_core.v's, same three stages, same
  // reasons; only the DC-blocker shift changes, because the tick rate does).
  //
  // The TIA mixer is UNIPOLAR: two channels of 0..15 scaled to 0..510, with a
  // large program-dependent DC (roughly half the active channels' levels).
  // dac.v's cartridge channel wants a SIGNED 10-bit sample and the physical
  // output is AC-coupled, so the DC buys nothing and only eats headroom ->
  // 1-pole DC blocker (leaky average), then a gain knob with explicit
  // saturation.
  //
  // A26_DCB_SHIFT is 9, not the SMS's 12, because the tick here is
  // color/114 = 31.40kHz instead of the PSG's 223.72kHz: 31400/(2*pi*2^9) =
  // 9.8Hz, i.e. the SAME corner the SMS was tuned to. Keep the two in step if
  // either rate ever moves.
  //
  // HEADROOM (the whole reason the gain is 0 -- read before touching it):
  //   * both channels at full volume sum to 510, so with the DC removed a
  //     full-scale unison square sits at +/-255 against the 10-bit signed full
  //     scale of -512..+511: half scale, deliberately, because
  //   * the DC estimate is not a constant, it RIPPLES with the waveform (the
  //     corner is 9.8Hz but the tone can be 30Hz in the lowest AUDF settings),
  //     and that droop adds overshoot on top of the peak.
  //   * the clamp below only trims the very tip of those peaks, SATURATING,
  //     never wrapping, so it can never invert polarity into a crack.
  // => A26_GAIN_SHIFT stays 0. Each +1 is +6dB and would clip the normal case
  //    to buy loudness that belongs on sgb_vol_select in main.v (post-CIC,
  //    designed for exactly this).
  //
  // APU_CLK_EDGE = TIA_CE (3.579545MHz), NOT the 31.4kHz mixer tick: dac.v's
  // cartridge CIC decimates by a hardwired 64, so the edge rate sets the
  // output rate. TIA_CE -> 55.9kHz; the mixer tick would give 490Hz, i.e.
  // everything above 245Hz aliased away. Feeding the same sample 114x is a
  // zero-order hold and costs nothing.
  // ---------------------------------------------------------------------
  localparam A26_DCB_SHIFT  = 9;    // DC-blocker corner: au_tick/(2*pi*2^9)
  localparam A26_GAIN_SHIFT = 0;    // FROZEN (see above): each +1 = +6dB

  reg  signed [11:0] au_x_r;        // raw capture (mix is unsigned -> sign bit 0)
  reg  signed [20:0] au_avg_r;      // DC estimate, 12.9 fixed point
  reg  signed [11:0] au_ac_r;       // DC-blocked sample
  reg  signed [9:0]  au_out_r;      // gained + saturated 10-bit output
  wire signed [11:0] au_avg_hi = au_avg_r[20:A26_DCB_SHIFT];
  wire signed [12:0] au_ac_w   = au_x_r - au_avg_hi;
  wire signed [15:0] au_g      = $signed({{4{au_ac_r[11]}}, au_ac_r}) <<< A26_GAIN_SHIFT;
  // Saturation, kept ENTIRELY in 16-bit signed arithmetic: mixing a signed
  // literal with an unsigned part-select in the same ternary makes the WHOLE
  // expression unsigned in Verilog and the comparisons stop meaning what they
  // read as. au_sat is provably inside -512..+511, so slicing it to 10 bits at
  // the assignment below is exact.
  wire signed [15:0] au_sat    = (au_g >  16'sd511) ?  16'sd511
                               : (au_g < -16'sd512) ? -16'sd512
                               :  au_g;

  always @(posedge CLK) begin
    if (RST) begin
      au_x_r <= 12'sd0; au_avg_r <= 21'sd0; au_ac_r <= 12'sd0; au_out_r <= 10'sd0;
    end else if (au_tick) begin
      au_x_r   <= $signed({3'b000, au_mix});
      au_avg_r <= au_avg_r + $signed({{8{au_ac_w[12]}}, au_ac_w});   // += err >> 9
      // 13 -> 12 bits (unreachable with mix <= 510, kept as a guard); the
      // bounds are written as bit patterns, not as -12'sd2048, which only
      // lands on the right value by wrapping twice.
      au_ac_r  <= (au_ac_w[12] == au_ac_w[11]) ? au_ac_w[11:0]
                : au_ac_w[12] ? 12'sh800 : 12'sh7FF;
      au_out_r <= au_sat[9:0];
    end
  end

  assign APU_DAT      = {au_out_r, au_out_r};
  assign APU_CLK_EDGE = TIA_CE;

endmodule
