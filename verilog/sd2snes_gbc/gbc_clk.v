`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module Name:    gbc_clk
// Project Name:   sd2snes_gbc
// Target Devices: EP4CE15 (Mk.III)
// Description:    Game Boy clock-enable generator and genlock loop.
//
// Replaces the clock half of the SGB's ICD2 (sgb_icd2.v:105-172).  The ICD2
// derived the GB clock from a /20 divider with one skipped base clock every
// 737, which is 84 MHz * 736/737 / 20 = 4194301.2 Hz.  This core uses a phase
// accumulator instead, because the genlock loop below has to be able to steer
// the rate continuously and because the double-speed enable has to stay
// aligned with the single-speed one by construction.
//
// UNITS, once, because every constant here depends on them:
//   * 1 clock enable = 1 dot = 1 T-cycle (sgb_cpu.v:1671).  A GB frame is
//     154 lines * 456 dots = 70224 CE.  The M-cycle is ce1/4.
//   * The rate word K2 is 24 fractional bits at 84 MHz: f_ce1 = 84e6*K2/2^25.
//     1 LSB of K2 is 84e6/2^25 = 2.5 Hz on ce1, i.e. about 0.6 ppm.
//   * The phase error is in DOTS, signed, one GB frame wide: [-35112, +35112).
//   * The loop gain that ties the two: at K2_NOM one SNES frame is 70224 dots,
//     so 1 LSB of K2 moves the phase by 70224/K2_NOM = 0.04166 dots per frame.
//     That is what makes kp = 16 a sensible number: 16 LSB per dot of error
//     removes 2/3 of the error every frame (pole at 0.333, no overshoot).
//
// The genlock loop (contract section 2) is a PI controller closed once per
// SNES frame on the SYNC strobe ($EF0004, written by the player at the top of
// its NMI).  It is a PI and not a pure integrator because the thing being
// controlled is a phase (the plant already integrates the frequency error): a
// pure integrator adds a second pole at z=1 and the loop rings instead of
// settling.  The integral term exists only to null the static phase offset the
// proportional term would otherwise leave behind against a SNES oscillator
// that is not exactly nominal (+-100 ppm is ~169 LSB of K2, ~10 dots of
// standing error at kp = 16), so ki is small and, critically, is only
// integrated inside the linear region (ERR_INT_MAX): integrating while the
// output is pinned to the clamp is windup, and windup on a loop whose slew is
// bounded by that same clamp overshoots by hundreds of dots.
//
// Reference: GBC-CORE-CONTRACT.md sections 2 and 13.14.
//////////////////////////////////////////////////////////////////////////////////
`include "config.vh"

module gbc_clk(
  input         RST,          // cold reset (SNES reset strobe)
  input         CLK,          // 84 MHz base clock

  // configuration
  input         exact_mode,   // CHIPFEAT[5]: 1 = Exact (4.194304 MHz), 0 = genlock
  input         sync_strobe,  // SNES wrote $EF0004 (one CLK pulse), phase reference

  // genlock inputs, from gbc_bridge
  input  [16:0] frame_dot_ctr, // 0..70223, zeroed at LY=0, frozen while LCD off
  input         lcd_on,        // LCDC.7: the loop is frozen while this is 0

  // clock dilation, from sgb_cpu: freeze the accumulator for this CLK
  input         hold,

  // clock enables, all one-CLK pulses in the base clock domain
  output        ce1,          // 1x  -- PPU dot / APU / CPU at normal speed
  output        ce2,          // 2x  -- CPU/timers/DMA at KEY1 double speed
  output        bus_edge,     // ce1 / 4 -- the GB M-cycle
  output        locked,       // genlock has phase lock (see below)
  output [15:0] sync_err_last // last phase error in dots, two's complement
);

//-------------------------------------------------------------------
// Rate constants
//-------------------------------------------------------------------

// 24 fractional bits at 84 MHz.  ce2 is the accumulator carry, ce1 is every
// second carry, so f_ce1 = 84e6 * K2 / 2^25.
//
//   Exact:   K2 = 2 * 837723 = 1675446  -> 4194303.3 Hz  (-0.17 ppm), which
//            is the best a 24-bit rate word can do: the exact value is
//            1675446.29.
//   Genlock: 70224 dots per SNES frame of 357368 master cycles, at a master
//            clock of 945/44 MHz, is 60.098478 * 70224 = 4220355.5 Hz
//            (+0.6210 % over the Game Boy's own rate).
//            K2_NOM below is the contract's 1685862, which is 4220378.6 Hz --
//            5.3 ppm fast, because the contract rounded the target frequency
//            to 4220378 Hz before deriving it (the exact ratio wants 1685853).
//            It is left alone on purpose: K2_NOM is a contract constant that
//            the .bi3/player pair is versioned on, and a static 5 ppm is what
//            the integral term is there to absorb -- it is a fifth of a
//            +-25 ppm oscillator, K_i settles about 9 LSB low, and the phase
//            error still goes to zero.  In Exact mode nothing absorbs it,
//            which is why that constant is the one that has to be right.
localparam [23:0] K2_EXACT = 24'd1675446;
localparam [23:0] K2_NOM   = 24'd1685862;

//-------------------------------------------------------------------
// Genlock loop constants
//-------------------------------------------------------------------

// Target phase: LY=0 at SNES V=182, three lines ahead of the V-IRQ at V=185,
// i.e. 43 SNES lines after the NMI = 43 * 1364 / 5.089 ... = 11525 dots.
localparam [16:0] ALVO     = 17'd11525;
localparam [18:0] DOT_MOD  = 19'd70224;   // dots per GB frame
localparam [18:0] DOT_HALF = 19'd35112;   // half a frame: the wrap point

// kp = 2^KP_SHIFT LSB of K2 per dot, ki = 2^-KI_SHIFT LSB of K2 per dot per
// frame.  Tuned in gbc/tests/rtl-tb/tb_p2_genlock.v: worst case over all
// initial phases and +-100 ppm is lock in 60 frames (1 s), and 52 frames for
// the four phases the test tabulates.  Raising kp past ~24 (= 1/0.04166) makes
// the proportional step overshoot the target phase, which is where a loop like
// this starts to ring.
localparam        KP_SHIFT    = 4;
localparam        KI_SHIFT    = 2;
// Anti-windup: only integrate once the proportional term is inside the clamp
// band (|err| < 16858/16 = 1054 dots), with margin.
localparam [16:0] ERR_INT_MAX = 17'd512;
// And bound the integrator itself.  It only ever has to cover the SNES
// oscillator's tolerance (+-100 ppm = +-169 LSB); +-4096 is 25x that.
localparam signed [17:0] KI_MAX = 18'sd4096;

// Clamp: K2_NOM * [0.99, 1.01].  Also the loop's authority over the phase:
// +-1 % of 70224 dots = +-702 dots of correction per frame, so half a frame of
// initial error (35112 dots) takes 50 frames to walk out.  That is the number
// the "locks in <= 120 frames" requirement is actually made of.
localparam signed [23:0] K2_MIN = 24'sd1669004;   // ceil (K2_NOM * 0.99)
localparam signed [23:0] K2_MAX = 24'sd1702720;   // floor(K2_NOM * 1.01)

// Slew per frame.  The unlocked one is wider than the whole clamp band and so
// never binds -- it is there because the contract asks for it.  The locked one
// is half the band and does bind, on exactly the sample it is meant to: a
// locked loop handed one bad frame_dot_ctr (a torn counter, a stray $EF0004)
// walks 0.5 % instead of taking the full 1 % jump to the clamp.
localparam signed [23:0] SLEW_UNLOCKED = 24'sd84293;  // 5   % of K2_NOM
localparam signed [23:0] SLEW_LOCKED   = 24'sd8429;   // 0.5 % of K2_NOM

// locked = |err| < 64 dots for 8 consecutive frames.
localparam [16:0] LOCK_ERR    = 17'd64;
localparam [3:0]  LOCK_FRAMES = 4'd8;

//-------------------------------------------------------------------
// Phase accumulator
//-------------------------------------------------------------------

reg  [23:0] k2_r;
reg  [23:0] acc_r;
reg         ce2_r;
reg         ce1_r;
reg         ce_phase_r;   // toggles on every carry; ce1 fires on the odd ones

wire [24:0] acc_next = {1'b0, acc_r} + {1'b0, k2_r};
wire        carry    = acc_next[24];

always @(posedge CLK) begin
  if (RST) begin
    acc_r      <= 24'd0;
    ce2_r      <= 1'b0;
    ce1_r      <= 1'b0;
    ce_phase_r <= 1'b0;
  end
  else begin
    // hold freezes the whole phase: no enable of any rate fires and the
    // fraction is kept, so the GB loses exactly the held CLK and nothing else
    // (sgb_cpu.v, "Clock dilation").  The genlock loop sees it as a GB that
    // ran slow for a moment and steers it back like any other phase error.
    if (~hold) acc_r <= acc_next[23:0];

    // GOTCHA (sgb_icd2.v:155): the SGB never assigns its clk_cpu_edge_r in the
    // reset branch.  In simulation it stays X, the bus counter that samples it
    // latches X forever, and the GB sits at PC=0000 with a clock that looks
    // like it is running.  Every enable register here is reset above.
    ce2_r <= carry & ~hold;
    ce1_r <= carry & ce_phase_r & ~hold;

    if (carry & ~hold) ce_phase_r <= ~ce_phase_r;
  end
end

assign ce1 = ce1_r;
assign ce2 = ce2_r;

//-------------------------------------------------------------------
// Bus (M-cycle) edge
//-------------------------------------------------------------------

// The GB bus clock is always ce1/4.  sgb_cpu.v:280 keeps an identical counter
// off the same enable and the same reset, so the two stay in step.
reg  [1:0] bus_ctr_r;
always @(posedge CLK) bus_ctr_r <= RST ? 2'b00 : bus_ctr_r + (ce1_r ? 2'd1 : 2'd0);

assign bus_edge = ce1_r & &bus_ctr_r;

//-------------------------------------------------------------------
// Genlock PI loop
//-------------------------------------------------------------------

// The update runs as a ten-state walk instead of one combinational lump, and
// the rule for the split is ONE CARRY CHAIN PER STATE: no state chains an
// adder into a comparison, or a comparison into another adder.  It costs ten
// of the 1.4 million clocks in a frame -- the SYNC strobes are 190 us apart --
// and it is the difference between this module being free at 84 MHz and it
// owning the core's critical path.
//
// It has already owned it once: with the clamp and the slew limit in the same
// cycle, k2_r -> (k2_r +- slew) -> compare against tgt -> mux -> k2_r was two
// chained 26-bit carry chains, 11.6 ns of the 11.9 ns period, and the twelve
// worst paths in the whole core were all k2_r[*] -> k2_r[*].  ST_SLEW1 now
// registers the two bounds and ST_SLEW2 only compares and muxes, so no
// k2_r -> k2_r path exists at all.  Do not re-merge these states to save a
// register: the registers are what makes the loop free.
localparam [3:0] ST_IDLE  = 4'd0,
                 ST_ERR1  = 4'd1,   // dot - ALVO
                 ST_ERR2  = 4'd2,   // wrap into [-35112, +35112)
                 ST_ABS   = 4'd3,   // |err|, and publish it for GBDG
                 ST_INT1  = 4'd4,   // kp*err, K_i +- step, the lock counter
                 ST_INT2  = 4'd5,   // clamp K_i
                 ST_TGT1  = 4'd6,   // K2_NOM + K_i
                 ST_TGT2  = 4'd7,   // ... - kp*err
                 ST_CLAMP = 4'd8,   // clamp to K2_NOM * [0.99, 1.01]
                 ST_SLEW1 = 4'd9,   // k2 +- slew, both bounds
                 ST_SLEW2 = 4'd10;  // pick one, write k2_r

// Working width for the output arithmetic: signed 24 bits.  The widest
// intermediate is K2_NOM + K_i - kp*err = 2251750 and the widest bound is
// K2_MAX + SLEW_UNLOCKED = 1787013, both far inside +-8388607.  That also
// makes $signed(k2_r) correct as written: k2_r never exceeds K2_MAX < 2^21,
// so its bit 23 is always 0 and reading it as signed cannot go negative.
reg  [3:0]        st_r;
reg  [16:0]       dot_q;        // frame_dot_ctr latched at the strobe
reg               frozen_q;     // this update only republishes K2_NOM + K_i
reg               locked_q;     // lock state this update is allowed to slew at
reg  signed [18:0] diff_r;      // dot - ALVO, before the modular reduction
reg  signed [18:0] err_q;       // dots, [-35112, +35112)
reg  [16:0]       err_abs_q;
reg  signed [17:0] ki_r;        // the integral term, in LSB of K2
reg  signed [17:0] ki_next_r;   // ... before its clamp
reg  signed [21:0] p_term_r;    // kp * err, in LSB of K2
reg  signed [23:0] tgt_r;
reg  signed [23:0] k2_hi_r;     // k2 + slew, registered so ST_SLEW2 only compares
reg  signed [23:0] k2_lo_r;     // k2 - slew
reg  [3:0]        lock_ctr_r;
reg               locked_r;
reg               skip_r;       // skip the next update (LCD off / first SYNC after on)
reg  [15:0]       err_last_r;

// err = wrap_signed(frame_dot_ctr - ALVO, 70224).  The modular reduction is
// what makes the loop take the short way round: without it a GB sitting 60000
// dots "ahead" is chased 60000 dots forward instead of 10224 dots back, which
// is a correct-looking loop that takes twice as long to acquire.  The subtract
// and the reduction are one state apart because together they are two chained
// 19-bit chains.
wire signed [18:0] diff_w = $signed({2'b00, dot_q}) - $signed({2'b00, ALVO});
wire signed [18:0] err_w  = (diff_r >=  $signed(DOT_HALF)) ? diff_r - $signed(DOT_MOD)
                          : (diff_r <  -$signed(DOT_HALF)) ? diff_r + $signed(DOT_MOD)
                                                           : diff_r;

// Integrator step, magnitude first so the shift rounds toward zero.  An
// arithmetic right shift of a negative error rounds toward -inf, which turns
// err = -1 into a permanent +1 LSB per frame ratchet on K_i.
wire [16:0]        ki_step_w = err_abs_q >> KI_SHIFT;
wire signed [17:0] ki_next_w = err_q[18] ? ki_r + $signed({1'b0, ki_step_w})
                                         : ki_r - $signed({1'b0, ki_step_w});

// Slew limit, against the K2 that is live right now.  The limit follows the
// lock state SAMPLED WITH THE ERROR, not the one the same update has just
// recomputed: a single bad frame_dot_ctr sample (a torn counter, a stray
// $EF0004) clears the lock inside this very walk, and reading locked_r here
// would hand that first bad sample the wide unlocked slew -- which is exactly
// the sample a slew limiter exists to reject.
wire signed [23:0] k2_cur_w  = $signed(k2_r);
wire signed [23:0] slew_w    = locked_q ? SLEW_LOCKED : SLEW_UNLOCKED;

always @(posedge CLK) begin
  if (RST) begin
    k2_r       <= exact_mode ? K2_EXACT : K2_NOM;
    st_r       <= ST_IDLE;
    dot_q      <= 17'd0;
    frozen_q   <= 1'b0;
    locked_q   <= 1'b0;
    diff_r     <= 19'sd0;
    err_q      <= 19'sd0;
    err_abs_q  <= 17'd0;
    ki_r       <= 18'sd0;
    ki_next_r  <= 18'sd0;
    p_term_r   <= 22'sd0;
    tgt_r      <= 24'sd0;
    k2_hi_r    <= 24'sd0;
    k2_lo_r    <= 24'sd0;
    lock_ctr_r <= 4'd0;
    locked_r   <= 1'b0;
    skip_r     <= 1'b1;
    err_last_r <= 16'd0;
  end
  else begin
    // CHIPFEAT is written before the core leaves reset, so this never moves
    // while the GB is running (see the false path in main.sdc).  In Exact mode
    // the loop still measures and publishes the phase error -- it is free
    // telemetry, and watching it walk is how one tells "Exact, as designed"
    // from "genlock, broken" over USB.
    if (exact_mode) k2_r <= K2_EXACT;

    case (st_r)
      // A strobe that lands while the walk is still running is ignored.  The
      // SNES cannot produce one: the shortest write cycle on the cartridge bus
      // is 6 master cycles = 23 of these clocks, and the walk is 6.
      ST_IDLE: if (sync_strobe) begin
        dot_q    <= frame_dot_ctr;
        frozen_q <= ~lcd_on | skip_r;
        locked_q <= locked_r;
        skip_r   <= 1'b0;
        st_r     <= ST_ERR1;
      end

      ST_ERR1: begin
        diff_r <= diff_w;
        st_r   <= ST_ERR2;
      end

      ST_ERR2: begin
        err_q <= err_w;
        st_r  <= ST_ABS;
      end

      ST_ABS: begin
        err_abs_q <= err_q[18] ? (~err_q[16:0] + 17'd1) : err_q[16:0];
        // GBDG +10 is 16 bits and the error is 17; saturate rather than wrap,
        // so a pre-lock error reads as "huge" instead of as its own negation.
        // Not published for a frozen update: the dot counter is stopped with
        // the LCD off, so that sample is not a phase error, and leaving the
        // last real one in place is what makes the field readable over USB.
        if (!frozen_q)
          err_last_r <= (err_q >  $signed(19'sd32767)) ? 16'h7fff
                      : (err_q < -$signed(19'sd32768)) ? 16'h8000
                                                       : err_q[15:0];
        st_r <= ST_INT1;
      end

      ST_INT1: begin
        // The integrator's next value is computed for every update and
        // committed by ST_INT2 only when it is allowed to be: the adder is
        // there anyway, and keeping it out of the gated branch keeps this
        // state one chain deep.
        ki_next_r <= ki_next_w;

        if (frozen_q) begin
          // Frozen: republish K2_NOM + K_i and touch nothing else.  The phase
          // is meaningless (the dot counter is stopped with the LCD off, and
          // the first frame after it comes back starts wherever the game left
          // it), so feeding it to the loop would kick a locked clock off the
          // SNES for no reason.
          p_term_r <= 22'sd0;
        end
        else begin
          p_term_r <= $signed(err_q) <<< KP_SHIFT;

          if (!exact_mode) begin
            if (err_abs_q < LOCK_ERR) begin
              if (lock_ctr_r != LOCK_FRAMES) lock_ctr_r <= lock_ctr_r + 4'd1;
              if (lock_ctr_r == LOCK_FRAMES - 4'd1) locked_r <= 1'b1;
            end
            else begin
              lock_ctr_r <= 4'd0;
              locked_r   <= 1'b0;
            end
          end
        end
        st_r <= ST_INT2;
      end

      ST_INT2: begin
        if (!frozen_q && !exact_mode && err_abs_q < ERR_INT_MAX)
          ki_r <= (ki_next_r >  KI_MAX) ?  KI_MAX
                : (ki_next_r < -KI_MAX) ? -KI_MAX
                                        :  ki_next_r;
        st_r <= ST_TGT1;
      end

      ST_TGT1: begin
        tgt_r <= $signed({1'b0, K2_NOM[22:0]}) + $signed(ki_r);
        st_r  <= ST_TGT2;
      end

      ST_TGT2: begin
        tgt_r <= tgt_r - $signed(p_term_r);
        st_r  <= ST_CLAMP;
      end

      ST_CLAMP: begin
        tgt_r <= (tgt_r < K2_MIN) ? K2_MIN : (tgt_r > K2_MAX) ? K2_MAX : tgt_r;
        st_r  <= ST_SLEW1;
      end

      ST_SLEW1: begin
        k2_hi_r <= k2_cur_w + slew_w;
        k2_lo_r <= k2_cur_w - slew_w;
        st_r    <= ST_SLEW2;
      end

      ST_SLEW2: begin
        // Only compares and muxes: both bounds and the target are registers,
        // so nothing here reads k2_r.
        if (!exact_mode)
          k2_r <= (tgt_r > k2_hi_r) ? k2_hi_r[23:0]
                : (tgt_r < k2_lo_r) ? k2_lo_r[23:0]
                                    : tgt_r[23:0];
        st_r <= ST_IDLE;
      end

      default: st_r <= ST_IDLE;
    endcase

    // LCD off re-arms the skip and drops the lock: the phase reference is
    // gone, and coming back with locked still set would hold the loop to the
    // 0.5 %/frame slew exactly when it needs the 5 % one to re-acquire.
    if (!lcd_on) begin
      skip_r     <= 1'b1;
      lock_ctr_r <= 4'd0;
      locked_r   <= 1'b0;
    end
  end
end

//-------------------------------------------------------------------
// Outputs
//-------------------------------------------------------------------

// In Exact mode there is no loop, so the clock is at its target rate by
// construction and reports locked.
assign locked        = exact_mode | locked_r;
assign sync_err_last = err_last_r;

endmodule
