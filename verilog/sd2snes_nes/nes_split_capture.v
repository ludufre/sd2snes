`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// nes_split_capture -- per-frame multi-scroll (CMD_SPLITS) capture (protocol
// v1.3.1 + v1.4b w-gating).  Extracted VERBATIM from nes_wrap.v's spl_* block so
// it is independently testable (tb/tb_split_capture.v feeds it RAW $2005/$2006
// pairs); nes_wrap now instantiates it.  All state registered; the change test
// compares the live tap against a single "last change" register set, never a
// scan; array writes are 4-deep decodes/shifts.  Mirror of bridge_sim
// ppustate.{note_scanline entry-0 seed, note_split_change}.
//
// v1.4b FIX (SINTOMA 2 -- scanline-straddle residue):  a split entry is now
// finalized only on a COMPLETE $2005/$2006 write pair (`w`==0).  While a pair is
// mid-flight (`w`==1) loopy_t holds an INTERMEDIATE value; sampling it made the
// pair-straddling case emit a phantom intermediate-T entry for ~1 line (~7%/
// split, the device flicker on Excitebike's billboard line 57-58).  Gating the
// change detection on `w`==0 is "coalescence by EVENT", matching the sim, which
// processes writes discretely.  The entry-0 display-start anchor is NOT gated
// (it fires with w==0 at scanline 0 in practice) -- only the change path is.
//////////////////////////////////////////////////////////////////////////////////

module nes_split_capture(
  input         CLK,
  input         RST,
  input         ce,              // core tick (nes_wrap ce_pulse_r)
  input         frame_tick,      // frame close (nes_wrap frame_tick_r)
  input  [8:0]  scanline,        // core scanline
  input  [14:0] loopy_t,         // ppu_tap_loopy_t (scroll intent)
  // ppu_tap_loopy_v: the CPU-SIDE shadow of loopy_V, and its write strobe.
  input  [14:0] loopy_v,
  input         loopy_v_we,
  input  [2:0]  fine_x,          // ppu_tap_fine_x
  input  [7:0]  ppumask,         // ppu_tap_ppumask
  input         w,               // ppu_tap_loopy_w ($2005/$2006 toggle; 0 = pair done)
  output [2:0]  spl_cnt_o,
  output        spl_ovf_o,
  output [31:0] spl_sl_flat,
  output [59:0] spl_t_flat,
  output [11:0] spl_fx_flat
);
  reg [7:0]  spl_sl [0:3];
  reg [14:0] spl_t  [0:3];
  reg [2:0]  spl_fx [0:3];
  reg [2:0]  spl_cnt;
  reg        spl_ovf;
  reg        spl_frozen;
  reg [7:0]  spl_last_sl;     // scanline of the LAST CHANGE (coalesce chain)
  reg [14:0] spl_last_t;
  reg [2:0]  spl_last_fx;
  // ---- RASTER-ADVANCED VERTICAL (v3.9) ----------------------------------
  // v_shadow follows CPU writes ONLY (the $2006 pair / the pre-render copy).
  // The PPU's own per-scanline Y increment was never modelled, so a split whose
  // write pair is $2005-ONLY -- horizontal scroll, the classic status-bar split
  // -- published the vertical the raster had at the ANCHOR, not at the split.
  // Super Mario Bros. 1 is the pure case: it seeds v<=t (vertical 0) at the
  // pre-render line and then writes only $2005 at the sprite-0 hit, so the
  // playfield strip went out with sy=0 and the renderer anchored it at logical
  // line 0 -- the status-bar rows, drawn again inside the playfield, with the
  // ground pushed off the bottom.
  //
  // Instead of incrementing v_shadow per scanline (which would make eff_t
  // change every line and fire the change detector on every one of them, K=4
  // overflowing on any scrolling game), the vertical is carried as an ANCHOR
  // plus the raster delta, and only the PUBLISHED value uses it.  Detection is
  // untouched: it still compares eff_t, i.e. CPU intent.
  reg [7:0]  va_line;   // line inside the nametable at the anchor (== the sy byte)
  reg        va_nt;     // nametable Y (loopy bit 11) at the anchor
  reg [7:0]  va_sl;     // scanline the anchor was taken at
  // ---- THE EFFECTIVE T: vertical from V, horizontal from T --------------
  // A mid-frame split is fixed by whichever register the game actually wrote,
  // and the two halves come from DIFFERENT ones:
  //   * the VERTICAL (coarse Y + fine Y + NT bit 11) only reaches the rendered
  //     address through loopy_V, and only a $2006 PAIR puts it there;
  //   * the HORIZONTAL (coarse X + NT bit 10) comes from loopy_T, which the
  //     dot-257 reload copies into V every scanline.
  // Sampling loopy_T alone was a REAL bug, found in silicon and reproduced
  // offline: a $2006 pair on one scanline followed by a $2005 pair on the next
  // has its entry overwritten by the <=1-scanline coalescence with the $2005's
  // T (vertical 0), so the strip was published with sy=0.  Don Doko Don 2 lost
  // its "PUSH START" line and Goemon rendered 12 lines high.
  // Lockstep with bridge_sim ppustate._eff_t():
  //     (loopy_v & 0x7BE0) | (loopy_t & 0x041F)
  // i.e. {v[14:11], t[10], v[9:5], t[4:0]}.

  // The module keeps its OWN copy of V because of the PRE-RENDER COPY: with
  // rendering on, the PPU does `v <= t` at dots 280-304 of the pre-render line,
  // which is the one moment the vertical of T reaches V.  It is modelled at the
  // entry-0 seed below (same place bridge_sim does it), and from then on the
  // shadow follows the tap's write strobe.  A strobe and not a value-compare:
  // a $2006 pair that rewrites the same address must still be adopted.
  reg [14:0] v_shadow;

  // ---- INPUT PIPELINE STAGE (timing; SEMANTICALLY A NO-OP) -----------------
  // Same fix, same argument as nes_ppusplit_capture: every input here moves
  // ONLY on a core `ce` and the pacer guarantees >= 13 CLK2 between two of
  // them, so delaying `ce` by the SAME cycle as the data keeps each pair
  // together and the automaton sees exactly what it saw before, one CLK2
  // later.  Without it the cone
  //     PPU|ClockGen|scanline[0] -> spl_cap|spl_t[3][*]
  // measured -3.784 ns once the _eff_t mux landed in front of the comparator
  // and the eviction tree.  frame_tick rides the same delay so the reseed stays
  // paired with the payload; the bridge latches on tick_accept, thousands of
  // cycles after the last display ce, so the extra cycle is invisible to it.
  reg        q_ce, q_ft, q_vwe, q_w, q_rend;
  reg [8:0]  q_sl;
  reg [14:0] q_t, q_v;
  reg [2:0]  q_fx;
  always @(posedge CLK) begin
    if (RST) begin
      q_ce<=1'b0; q_ft<=1'b0; q_vwe<=1'b0; q_w<=1'b0; q_rend<=1'b0;
      q_sl<=9'd0; q_t<=15'd0; q_v<=15'd0; q_fx<=3'd0;
    end else begin
      q_ce<=ce; q_ft<=frame_tick; q_vwe<=loopy_v_we; q_w<=w;
      q_rend<=(ppumask[3] | ppumask[4]);
      q_sl<=scanline; q_t<=loopy_t; q_v<=loopy_v; q_fx<=fine_x;
    end
  end

  // ⚠️ V IS ADOPTED IN THE SAME ce AS ITS WRITE STROBE, not on the next one.
  // That is what the simulator does -- apply_cpu_write() sets loopy_v and the
  // note_split_change() that follows evaluates _eff_t() with the NEW value --
  // and getting it wrong put the entry one scanline late: for the $2006 pair at
  // sl 155 + $2005 pair at sl 156, the sim publishes ONE entry at 155 (the
  // <=1-scanline chain then absorbs the $2005), while adopting V a cycle later
  // published it at 156.  NO byte-exact gate can see that: run_bridge and
  // tb_maptap both INJECT snap_spl_* from the simulator's own entries and never
  // build this module.  It is caught by the scanline assertion in
  // tb_split_capture instead.
  wire [14:0] v_eff = q_vwe ? q_v : v_shadow;
  wire [14:0] eff_t = (v_eff & 15'h7BE0) | (q_t & 15'h041F);

  // PUBLISHED T: same composition as eff_t, but the vertical is the anchor
  // advanced by the raster.  One 240-line wrap at most (anchor <= 239 and
  // delta <= 239 => 478 < 480), and it toggles the nametable Y like the PPU's
  // coarse-Y wrap does.
  // The anchor is consumed COMBINATIONALLY on a write cycle, for the same
  // reason v_eff is: the registered update is not visible to this cycle's
  // publish, and a $2006 split must go out with the vertical it just WROTE
  // (delta 0), not the raster line.  Caught by the $2006 control bench.
  wire [7:0]  va_line_eff = q_vwe ? {q_v[9:5], q_v[14:12]} : va_line;
  wire        va_nt_eff   = q_vwe ? q_v[11]                : va_nt;
  wire [7:0]  va_sl_eff   = q_vwe ? q_sl[7:0]              : va_sl;
  wire [8:0]  va_raw   = {1'b0, va_line_eff} + ({1'b0, q_sl[7:0]} - {1'b0, va_sl_eff});
  wire        va_wrap  = (va_raw >= 9'd240);
  wire [8:0]  va_sub   = va_raw - 9'd240;
  wire [7:0]  pub_line = va_wrap ? va_sub[7:0] : va_raw[7:0];
  wire        pub_nt   = va_nt_eff ^ va_wrap;
  wire [14:0] pub_t    = {pub_line[2:0], pub_nt, q_t[10], pub_line[7:3], q_t[4:0]};

  wire spl_changed = (eff_t != spl_last_t) | (q_fx != spl_last_fx);
  // RENDERING GATE (ppumask BG|OBJ): a T/fine_x change while rendering is OFF is
  // not a visible scroll split (e.g. DK streams a nametable via $2006 at the top
  // of the frame with ppumask=0x06).  Keeps split-less games at cnt<=1.
  wire spl_render = q_rend;
  // v1.4b: only finalize a change on a COMPLETE write pair (w==0) -- the
  // intermediate-T of a mid-flight $2005/$2006 pair (w==1) is never an entry.
  wire spl_do_change = spl_changed & ~q_w;
  // eviction cones (consumed ONLY in the cnt==4 overflow branch; reg-to-reg,
  // 8-bit subtract + compare tree, single-cycle at CLK2 with room to spare)
  wire [7:0] spl_sl_now = q_sl[7:0];
  wire [7:0] spl_d1 = spl_sl[2] - spl_sl[1];
  wire [7:0] spl_d2 = spl_sl[3] - spl_sl[2];
  wire [7:0] spl_d3 = spl_sl_now - spl_sl[3];
  wire [7:0] spl_dn = 8'd240 - spl_sl_now;
  wire spl_ev1 = (spl_d1 <= spl_d2) && (spl_d1 <= spl_d3) && (spl_d1 <= spl_dn);
  wire spl_ev2 = !spl_ev1 && (spl_d2 <= spl_d3) && (spl_d2 <= spl_dn);
  wire spl_ev3 = !spl_ev1 && !spl_ev2 && (spl_d3 <= spl_dn);

  // ---- REGISTERED DECISION (timing; a 1-CYCLE DEFERRAL, semantically exact) -
  // Same fix, same shape as nes_chrwin_capture's parked eviction, applied one
  // step further: here it is not only the eviction that defers but EVERY array
  // write.  The reason is the measured one -- with the fine_x term restored the
  // seed-4 fit put its worst path inside this module
  //     nes_split_capture:spl_cap|spl_sl[1][1]
  // because the change compare
  //     (eff_t != spl_last_t) | (q_fx != spl_last_fx)
  // together with the <=1-scanline chain test and the 4-way eviction tree all
  // sat IN FRONT of the write enables and data muxes of four 26-bit entries.
  //
  // Split in two, that cone becomes two short ones:
  //   cycle 1 (the ce): the compare, the chain test and the eviction tree pick
  //     an ACTION and park it with its payload.  The NARROW state -- spl_cnt,
  //     spl_ovf, spl_frozen, v_shadow and the spl_last_* chain -- still updates
  //     here, because it feeds the NEXT comparison and must not lag it.
  //   cycle 2 (any cycle): the parked action is applied to the arrays.  Its
  //     cone is the decision registers plus the array reads the shifts need --
  //     no compare, no eviction tree.
  //
  // WHY THIS IS FREE: every input moves only on a core `ce` and the nes_wrap
  // pacer keeps consecutive `ce` pulses >= 13 CLK2 apart (derived in the
  // nes_wrap header; MEASURED at min 15 / max 23 over 79,665 pulses by
  // run_nestest).  The apply lands the CLK2 right after the ce that parked it,
  // i.e. >= 12 cycles before the next reader, and the bridge latches these
  // arrays only at tick_accept -- see the DEFERRED CAPTURE WRITES note in
  // nes_bridge.v's header for that leg.
  //
  // THE APPLY IS PLACED FIRST, NOT LAST (delta vs nes_chrwin_capture, and it
  // matters here).  chrwin's parked action only ever touches entries 1..3, so a
  // frame close landing on top of a pending apply cannot collide with its
  // entry-0 reseed and the apply is free to sit last.  Here the parked action
  // CAN be the entry-0 seed (ACT_WR with dp_idx 0).  Putting the apply first
  // makes the ordering come out exactly as the un-deferred module: the pending
  // write lands, and then the reseed overwrites entry 0 on top of it -- which
  // is the same final state the immediate version reaches.  Last-assignment-
  // wins is doing real work; do not move this block.
  localparam [2:0] ACT_WR   = 3'd1,   // full entry write at dp_idx (seed/append)
                   ACT_COAL = 3'd2,   // payload-only rewrite at dp_idx
                   ACT_EV1  = 3'd3,
                   ACT_EV2  = 3'd4,
                   ACT_EV3  = 3'd5;
  reg        dp_v;                    // a decision is parked
  reg [2:0]  dp_act;
  reg [2:0]  dp_idx;
  reg [7:0]  dp_sl;
  reg [14:0] dp_t;
  reg [2:0]  dp_fx;

  always @(posedge CLK) begin
    // ---- cycle 2 of every array update: apply the parked decision -----------
    // Runs on ANY cycle (not only a ce), so it always lands the CLK2 right
    // after the ce that parked it.  FIRST in the block on purpose -- see above.
    if (dp_v & ~RST) begin
      dp_v <= 1'b0;
      case (dp_act)
        ACT_WR: begin
          spl_sl[dp_idx[1:0]]<=dp_sl; spl_t[dp_idx[1:0]]<=dp_t;
          spl_fx[dp_idx[1:0]]<=dp_fx;
        end
        ACT_COAL: begin
          // the entry keeps its ORIGINAL scanline -- payload only
          spl_t[dp_idx[1:0]]<=dp_t; spl_fx[dp_idx[1:0]]<=dp_fx;
        end
        ACT_EV1: begin            // evict e1: shift e2/e3 down, new at [3]
          spl_sl[1]<=spl_sl[2]; spl_t[1]<=spl_t[2]; spl_fx[1]<=spl_fx[2];
          spl_sl[2]<=spl_sl[3]; spl_t[2]<=spl_t[3]; spl_fx[2]<=spl_fx[3];
          spl_sl[3]<=dp_sl;     spl_t[3]<=dp_t;     spl_fx[3]<=dp_fx;
        end
        ACT_EV2: begin
          spl_sl[2]<=spl_sl[3]; spl_t[2]<=spl_t[3]; spl_fx[2]<=spl_fx[3];
          spl_sl[3]<=dp_sl;     spl_t[3]<=dp_t;     spl_fx[3]<=dp_fx;
        end
        default: begin            // ACT_EV3
          spl_sl[3]<=dp_sl;     spl_t[3]<=dp_t;     spl_fx[3]<=dp_fx;
        end
      endcase
    end

    if (RST) begin
      spl_cnt<=3'd1; spl_ovf<=1'b0; spl_frozen<=1'b0;
      spl_sl[0]<=8'd0; spl_t[0]<=15'd0; spl_fx[0]<=3'd0;
      v_shadow<=15'd0;
      va_line<=8'd0; va_nt<=1'b0; va_sl<=8'd0;
      spl_last_sl<=8'd0; spl_last_t<=15'd0; spl_last_fx<=3'd0;
      dp_v<=1'b0; dp_act<=3'd0; dp_idx<=3'd0;
      dp_sl<=8'd0; dp_t<=15'd0; dp_fx<=3'd0;
    end else if (q_ft) begin
      // re-seed entry0 (fallback = close-time T) + re-arm capture for next frame
      spl_frozen<=1'b0; spl_ovf<=1'b0; spl_cnt<=3'd1;
      spl_sl[0]<=8'd0; spl_t[0]<=eff_t; spl_fx[0]<=q_fx;
      spl_last_sl<=8'd0; spl_last_t<=eff_t; spl_last_fx<=q_fx;
      if (q_vwe) begin
        v_shadow<=q_v;
        va_line<={q_v[9:5], q_v[14:12]}; va_nt<=q_v[11]; va_sl<=8'd0;
      end
    end else if (q_ce && q_sl <= 9'd239 && spl_render) begin
      // the CPU wrote V ($2006 pair or a $2007 access): adopt it, and
      // re-anchor the raster vertical at THIS scanline
      if (q_vwe) begin
        v_shadow<=q_v;
        va_line<={q_v[9:5], q_v[14:12]}; va_nt<=q_v[11]; va_sl<=q_sl[7:0];
      end
      if (!spl_frozen) begin
        // entry 0 = display-start state (chain anchored at scanline 0)
        // PRE-RENDER COPY: v <= t, then entry 0 is composed from it (which
        // makes entry 0 identical to the pre-fix behaviour -- v == t at the
        // start of the display -- while the LATER entries start telling the
        // two apart).  Written to the shadow AND used for this entry, since the
        // non-blocking update is not visible to eff_t in this same cycle.
        v_shadow<=q_t;
        // the pre-render copy is also the raster anchor of the frame
        va_line<={q_t[9:5], q_t[14:12]}; va_nt<=q_t[11]; va_sl<=8'd0;
        // PARK(seed)
        dp_v<=1'b1; dp_act<=ACT_WR; dp_idx<=3'd0;
        dp_sl<=8'd0; dp_t<=q_t; dp_fx<=q_fx;
        // ENDPARK
        spl_cnt<=3'd1; spl_frozen<=1'b1;
        spl_last_sl<=8'd0; spl_last_t<=eff_t; spl_last_fx<=q_fx;
      end else if (spl_do_change) begin
        if ((spl_sl_now - spl_last_sl) <= 8'd1) begin
          // coalesce (same/adjacent scanline): entry keeps its ORIGINAL
          // scanline; the chain advances so a pair may continue next line
          // PARK(coalesce)
          dp_v<=1'b1; dp_act<=ACT_COAL; dp_idx<=spl_cnt-3'd1;
          dp_sl<=spl_sl_now; dp_t<=pub_t; dp_fx<=q_fx;
          // ENDPARK
          spl_last_sl<=spl_sl_now;
          spl_last_t<=eff_t; spl_last_fx<=q_fx;
        end else if (spl_cnt >= 3'd4) begin
          spl_ovf<=1'b1;
          if (spl_ev1 | spl_ev2 | spl_ev3) begin
            // PARK(evict)
            dp_v<=1'b1;
            dp_act<=spl_ev1 ? ACT_EV1 : (spl_ev2 ? ACT_EV2 : ACT_EV3);
            dp_idx<=3'd3;
            dp_sl<=spl_sl_now; dp_t<=pub_t; dp_fx<=q_fx;
            // ENDPARK
            spl_last_sl<=spl_sl_now; spl_last_t<=eff_t; spl_last_fx<=q_fx;
          end
          // else: the new entry is the shortest strip -> dropped, chain frozen
        end else begin
          // PARK(append)
          dp_v<=1'b1; dp_act<=ACT_WR; dp_idx<=spl_cnt;
          dp_sl<=spl_sl_now; dp_t<=pub_t; dp_fx<=q_fx;
          // ENDPARK
          spl_cnt<=spl_cnt+3'd1;
          spl_last_sl<=spl_sl_now; spl_last_t<=eff_t; spl_last_fx<=q_fx;
        end
      end
    end
  end
  assign spl_sl_flat = {spl_sl[3],spl_sl[2],spl_sl[1],spl_sl[0]};
  assign spl_t_flat  = {spl_t[3], spl_t[2], spl_t[1], spl_t[0]};
  assign spl_fx_flat = {spl_fx[3],spl_fx[2],spl_fx[1],spl_fx[0]};
  assign spl_cnt_o   = spl_cnt;
  assign spl_ovf_o   = spl_ovf;
endmodule
