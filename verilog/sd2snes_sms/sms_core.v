// sd2snes SMS core — integrated SMS subsystem (M7.4a).
//
// Wires sms.v (Z80 + Sega mapper + WRAM + VDP) to sms_translate.v (SMS->SNES
// translation) and streams the translated SNES-format buffers to PSRAM:
//   - the Z80 fetches the SMS ROM from PSRAM via ROM_* (the main.v arbiter
//     services this as a free-slot client),
//   - at each SMS vblank (FRAME_TICK) the translation runs and its byte stream
//     (OUT_*) is written to the PSRAM buffer region via BUF_* (another free-slot
//     client), with backpressure (STALL) since PSRAM writes are slower than the
//     1 byte/cyc the engine emits.
//
// Buffer region (PSRAM), per OUT_SEL: tiles 0x00000, tilemap 0x04000,
// cgram 0x04800, oam 0x05000 (relative to BUF_BASE). The SNES reads these via
// address.v ($E0-$E3 -> BUF_BASE..). Double-buffering is added at the main.v
// graft (toggle BUF_BASE between two sets); here a single set validates the
// datapath. CE = SMS clock-enable (=1 in sim for full speed).
module sms_core (
  input         CLK,
  input         RST,
  input         CE,

  // SMS ROM read (PSRAM, via arbiter)
  output        ROM_RRQ,
  output [23:0] ROM_ADDR,
  input  [7:0]  ROM_DATA,
  input         ROM_RDY,

  // translated-buffer write (PSRAM, via arbiter)
  output reg        BUF_WRQ,
  output reg [23:0] BUF_ADDR,
  output reg [7:0]  BUF_DATA,
  input             BUF_WACK,

  input  [7:0]  PAD1,
  output        FRAME_DONE,        // pulses when a translated frame is complete

  output [15:0] DBG_PC,
  output [31:0] DBG_VRAM_WRITES,
  output        DBG_DISPLAY_ON,
  output        DBG_TR_BUSY,
  // debug read of the VDP state (valid when ~DBG_TR_BUSY): lets the tb compute the
  // expected translation from the RTL's own VRAM/CRAM/regs and compare exactly.
  input  [13:0] DBG_VRAM_ADDR,
  output [7:0]  DBG_VRAM_DATA,
  input  [4:0]  DBG_CRAM_ADDR,
  output [7:0]  DBG_CRAM_DATA,
  output [127:0] DBG_VREGS,
  input         DBG_FORCE_TR,      // force a translation pass (test: run on a frozen VDP)
  input         TR_GATE,           // hold off new translations while the back buffer is unconsumed
  input         BACK_SET,          // which double-buffer set is written this pass (= ~sms_buf_front)
  input         DBG_FORCE_FULL,    // test: force a full tilemap emit (all columns dirty)
  output [1:0]  DBG_WAIT,          // Z80 wait-stall live: bit0=ROM, bit1=WRAM (perf instr.)
  output        DBG_FRAME_TICK,    // SMS vblank tick (frame pacing, perf instrumentation)
  // audio to dac.v's cartridge channel: {ch1[9:0], ch0[9:0]}, both fed the
  // same mono sample, plus one strobe per sample (see AUDIO CONDITIONING below)
  output [19:0] APU_DAT,
  output        APU_CLK_EDGE
);

  wire [31:0]   sms_dcols;         // dirty name-table column bitmap (sms.v -> sms_translate)
  localparam [23:0] BUF_BASE = 24'h200000;

  // ---- translation <-> VDP read port (muxed with the debug read) ----
  wire       tr_busy, tr_done, tr_out_we;
  wire [1:0] tr_out_sel;
  wire [15:0] tr_out_addr;
  wire [7:0] tr_out_data;
  wire [13:0] tr_vram_addr;     // from translate
  wire [4:0]  tr_cram_addr;
  wire [7:0]  vdp_vram_data;    // from sms TR port
  wire [7:0]  vdp_cram_data;
  wire [127:0] vregs;
  wire         frame_tick;
  wire [7:0]   sms_dlo_min, sms_dlo_max, sms_dhi_min, sms_dhi_max;

  // when translating, the VDP read port follows the engine; otherwise the debug read
  wire [13:0] vdp_vram_addr = tr_busy ? tr_vram_addr : DBG_VRAM_ADDR;
  wire [4:0]  vdp_cram_addr = tr_busy ? tr_cram_addr : DBG_CRAM_ADDR;
  assign DBG_VRAM_DATA = vdp_vram_data;
  assign DBG_CRAM_DATA = vdp_cram_data;
  assign DBG_VREGS     = vregs;

  // ---- sms (Z80 + VDP) ----
  // NO CE freeze during translation (the old gate cost ~3-5% game speed): the pass
  // now triggers at ACTIVE-display start (FRAME_TICK in sms.v), right after the
  // game's vblank VDP writes, so it reads a fresh consistent frame while the game
  // keeps running. Geometry regs are snapshotted at START inside sms_translate;
  // rare mid-pass VRAM/CRAM/SAT writes self-heal via the per-set dirty tracking.
  //
  // AUDIO DEPENDS ON THIS STAYING UNGATED. The PSG hangs off the same CE inside
  // sms.v and APU_CLK_EDGE below is that same CE, so anything that freezes
  // sms_ce_gated freezes the PSG counters (pitch drops with the duty cycle of
  // the gate) AND stalls the dac.v cartridge CIC (its output rate is edge/64).
  // If a CE gate ever comes back, the PSG has to be moved out of it and fed the
  // raw CE, and APU_CLK_EDGE with it.
  wire sms_ce_gated = CE;
  wire [10:0] psg_mix;
  wire        psg_tick;
  sms u_sms (
    .CLK(CLK), .RST(RST), .CE(sms_ce_gated),
    .ROM_RRQ(ROM_RRQ), .ROM_ADDR(ROM_ADDR), .ROM_DATA(ROM_DATA), .ROM_RDY(ROM_RDY),
    .PAD1(PAD1),
    .TR_VRAM_ADDR(vdp_vram_addr), .TR_VRAM_DATA(vdp_vram_data),
    .TR_CRAM_ADDR(vdp_cram_addr), .TR_CRAM_DATA(vdp_cram_data),
    .DBG_PC(DBG_PC), .DBG_BANK0(), .DBG_BANK1(), .DBG_BANK2(),
    .DBG_VRAM_WRITES(DBG_VRAM_WRITES), .DBG_REG_WRITES(), .DBG_CRAM_WRITES(),
    .DBG_REG1(), .DBG_REG2(), .DBG_DISPLAY_ON(DBG_DISPLAY_ON),
    .VREGS(vregs), .FRAME_TICK(frame_tick),
    .DIRTY_LO_MIN(sms_dlo_min), .DIRTY_LO_MAX(sms_dlo_max),
    .DIRTY_HI_MIN(sms_dhi_min), .DIRTY_HI_MAX(sms_dhi_max), .DIRTY_SNAP(tr_start),
    .BACK_SET(BACK_SET), .DIRTY_COLS(sms_dcols), .DBG_FORCE_FULL(DBG_FORCE_FULL),
    .DBG_WAIT(DBG_WAIT),
    .PSG_MIX(psg_mix), .PSG_TICK(psg_tick)
  );
  assign DBG_FRAME_TICK = frame_tick;

  // ---- translation ----
  reg        tr_start;
  reg        tr_stall;

  sms_translate u_tr (
    .CLK(CLK), .RST(RST), .START(tr_start), .STALL(tr_stall),
    .BUSY(tr_busy), .DONE(tr_done),
    .REG0(vregs[7:0]),   .REG1(vregs[15:8]),  .REG2(vregs[23:16]),
    .REG5(vregs[47:40]), .REG6(vregs[55:48]), .REG7(vregs[63:56]),
    .REG8(vregs[71:64]), .REG9(vregs[79:72]),
    .DIRTY_LO_MIN(sms_dlo_min), .DIRTY_LO_MAX(sms_dlo_max),
    .DIRTY_HI_MIN(sms_dhi_min), .DIRTY_HI_MAX(sms_dhi_max), .DIRTY_COLS(sms_dcols),
    .VRAM_ADDR(tr_vram_addr), .VRAM_DATA(vdp_vram_data),
    .CRAM_ADDR(tr_cram_addr), .CRAM_DATA(vdp_cram_data),
    .OUT_WE(tr_out_we), .OUT_SEL(tr_out_sel), .OUT_ADDR(tr_out_addr), .OUT_DATA(tr_out_data)
  );

  assign FRAME_DONE  = tr_done;
  assign DBG_TR_BUSY = tr_busy;

  // start one translation pass per SMS vblank (when idle); DBG_FORCE_TR also triggers
  always @(posedge CLK) begin
    if (RST) tr_start <= 1'b0;
    else     tr_start <= (frame_tick | DBG_FORCE_TR) & ~tr_busy & ~tr_start & ~TR_GATE;
  end

  // OUT_SEL -> PSRAM byte offset
  reg [23:0] sel_off;
  always @* case (tr_out_sel)
    2'd0: sel_off = 24'h00000;
    2'd1: sel_off = 24'h04000;
    2'd2: sel_off = 24'h04800;
    default: sel_off = 24'h05000;
  endcase

  // buffer write request + backpressure (write each emitted byte to PSRAM)
  wire wr_accept = BUF_WRQ & BUF_WACK;
  always @(posedge CLK) begin
    if (RST) BUF_WRQ <= 1'b0;
    else if (tr_out_we && !BUF_WRQ) begin
      BUF_WRQ  <= 1'b1;
      BUF_ADDR <= BUF_BASE + sel_off + {8'd0, tr_out_addr};
      BUF_DATA <= tr_out_data;
    end else if (wr_accept) begin
      BUF_WRQ <= 1'b0;
    end
  end
  // stall the engine while its current byte hasn't been written yet
  always @* tr_stall = tr_out_we & ~wr_accept;

  // ---------------------------------------------------------------------------
  // AUDIO CONDITIONING (mirrors nes_wrap.v's, same three stages, same reasons).
  //
  // The SN76489 is UNIPOLAR: the mix is 0..1020 with a large, program-dependent
  // DC (roughly half the active channels' levels).  dac.v's cartridge channel
  // wants a SIGNED 10-bit sample and the physical output is AC-coupled, so the
  // DC buys nothing and only eats headroom -> 1-pole DC blocker (leaky average,
  // shift 12 at the 223.72kHz PSG tick = 8.7Hz corner, converges in ~18ms),
  // then a gain knob with explicit saturation.
  //
  // HEADROOM (the whole reason the gain is 0 -- read before touching it):
  //   * per-channel peak level is 255, four channels sum to 1020, so with the
  //     DC removed a 50%-duty full-scale unison square sits at +/-510 against
  //     the 10-bit signed full scale of -512..+511: already at the rail.
  //   * the DC estimate is not a constant, it RIPPLES with the waveform (the
  //     corner is 8.7Hz but the tone can be 109Hz), and that droop adds
  //     overshoot on top: worst case is 573, i.e. all four channels in unison,
  //     attenuation 0, near the bottom of the tone range.  That is pathological
  //     -- no game plays four maxed channels in phase -- and the clamp below
  //     trims only the very tip of those peaks (573 -> 511, ~1dB), SATURATING,
  //     never wrapping, so it can never invert polarity into a crack.
  //   * a normal three-voice tune lands at 50-70% FS, the same band nes_wrap
  //     targets with its x4.
  // => SMS_GAIN_SHIFT IS FROZEN AT 0, PERMANENTLY.  Each +1 is +6dB and would
  //    clip everything, not just the pathological case.  If the SMS turns out
  //    to need more loudness, the knob is sgb_vol_select on the dac instance in
  //    main.v (post-CIC, designed for exactly this) -- NOT this shift.
  //
  // APU_CLK_EDGE = CE (3.579545MHz), NOT the 223.72kHz PSG tick: dac.v's
  // cartridge CIC decimates by a hardwired 64, so the edge rate sets the output
  // rate.  CE -> 55.9kHz (fine, and closer to the SGB's 4MHz design point than
  // the NES's 5.37MHz); the PSG tick would give 3.5kHz, i.e. everything above
  // 1.75kHz aliased away.  Feeding the same sample 16x is a zero-order hold and
  // costs nothing.
  // ---------------------------------------------------------------------------
  localparam SMS_DCB_SHIFT  = 12;   // DC-blocker corner: psg_tick/(2*pi*2^12)
  localparam SMS_GAIN_SHIFT = 0;    // FROZEN (see above): each +1 = +6dB

  reg  signed [11:0] au_x_r;        // raw capture (mix is unsigned -> sign bit 0)
  reg  signed [23:0] au_avg_r;      // DC estimate, 12.12 fixed point
  reg  signed [11:0] au_ac_r;       // DC-blocked sample
  reg  signed [9:0]  au_out_r;      // gained + saturated 10-bit output
  wire signed [11:0] au_avg_hi = au_avg_r[23:SMS_DCB_SHIFT];
  wire signed [12:0] au_ac_w   = au_x_r - au_avg_hi;
  wire signed [15:0] au_g      = $signed({{4{au_ac_r[11]}}, au_ac_r}) <<< SMS_GAIN_SHIFT;
  // Saturation, kept ENTIRELY in 16-bit signed arithmetic: mixing a signed
  // literal with an unsigned part-select (au_g[9:0]) in the same ternary makes
  // the WHOLE expression unsigned in Verilog and the comparisons stop meaning
  // what they read as.  au_sat is provably inside -512..+511, so slicing it to
  // 10 bits at the assignment below is exact.
  wire signed [15:0] au_sat    = (au_g >  16'sd511) ?  16'sd511
                               : (au_g < -16'sd512) ? -16'sd512
                               :  au_g;

  always @(posedge CLK) begin
    if (RST) begin
      au_x_r <= 12'sd0; au_avg_r <= 24'sd0; au_ac_r <= 12'sd0; au_out_r <= 10'sd0;
    end else if (psg_tick) begin
      au_x_r   <= $signed({1'b0, psg_mix});
      au_avg_r <= au_avg_r + $signed({{11{au_ac_w[12]}}, au_ac_w});   // += err >> 12
      // 13 -> 12 bits (unreachable with mix <= 1020, kept as a guard); the
      // bounds are written as bit patterns, not as -12'sd2048, which only
      // lands on the right value by wrapping twice.
      au_ac_r  <= (au_ac_w[12] == au_ac_w[11]) ? au_ac_w[11:0]
                : au_ac_w[12] ? 12'sh800 : 12'sh7FF;
      au_out_r <= au_sat[9:0];
    end
  end

  assign APU_CLK_EDGE = CE;

`ifdef SMS_AUDIO_PROBE
  // ---------------------------------------------------------------------------
  // BRING-UP PROBE (build-time only -- the `ifdef stays in the source, the
  // define does NOT go in main.qsf for production).  To build a diagnostic
  // bitstream: add to main.qsf
  //   set_global_assignment -name VERILOG_MACRO "SMS_AUDIO_PROBE=1"
  // then make mk3; REMOVE the line again before any production build.
  // Drives a ~999Hz square at
  // half scale into APU_DAT for the first 2 seconds after reset, INSTEAD of the
  // PSG.  It splits the two failure modes that look identical on the jack:
  // silence with the probe audible = the FPGA->DAC->jack path is fine and the
  // problem is upstream (PSG writes, mix, S-DSP gate); silence with the probe
  // inaudible = the path itself is dead and the PSG is irrelevant.
  // 224 ticks per period at 223.72kHz = 998.8Hz; 447443 ticks = 2.00s.
  // ---------------------------------------------------------------------------
  localparam [18:0] PROBE_TICKS = 19'd447443;
  localparam [6:0]  PROBE_HALF  = 7'd111;    // 112 ticks per half period
  reg [18:0] probe_ctr;
  reg [6:0]  probe_div;
  reg        probe_ff;
  reg        probe_on;
  wire signed [9:0] probe_sample = probe_ff ? 10'sd256 : -10'sd256;
  always @(posedge CLK) begin
    if (RST) begin
      probe_ctr <= 19'd0; probe_div <= 7'd0; probe_ff <= 1'b0; probe_on <= 1'b1;
    end else if (psg_tick & probe_on) begin
      if (probe_div == PROBE_HALF) begin probe_div <= 7'd0; probe_ff <= ~probe_ff; end
      else                               probe_div <= probe_div + 7'd1;
      if (probe_ctr == PROBE_TICKS) probe_on  <= 1'b0;
      else                          probe_ctr <= probe_ctr + 19'd1;
    end
  end
  assign APU_DAT = probe_on ? {probe_sample, probe_sample} : {au_out_r, au_out_r};
`else
  assign APU_DAT = {au_out_r, au_out_r};
`endif

endmodule
