`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// nes_bridge -- NES->SNES video bridge v1 (Fase 1a).  Golden: the byte-exact
// mailbox command stream produced by nes-tests/bridge-sim (`out/<trace>/mailbox.bin`).
//
// WHAT THIS MODULE IS
// -------------------------------------------------------------------------------
//   It is the ENCODER + dirty-tracking + double-buffered mailbox + control block.
//   Its INPUTS are the "taps" (NES-BRIDGE-SPEC.md SS2):
//     * Class A (PSRAM bus, wired from nes_wrap.v): nametable writes (post-mirror
//       physical CIRAM offset) and CHR-RAM dirty tiles.
//     * Class B (new ppu.v ports, threaded via nes.v; molde dbg_cpu): palette
//       writes (idx,val), OAM bytes, and a per-frame snapshot latched at
//       scanline==241 (loopy_t + fine_x + PPUCTRL + PPUMASK + chr bank per slot).
//   The gate testbench drives these ports DIRECTLY from a Python stimulus
//   generator that replays each Mesen trace through bridge_sim; in hardware
//   nes_wrap.v wires the real taps to the same ports.
//
// DELIBERATE FEED-INS (documented spec/sim/RTL boundaries)
// -------------------------------------------------------------------------------
//   * SCROLL from loopy_T (not loopy_V): bridge_sim/ppustate.py::current_scroll
//     derives scroll from loopy_T (the reload reg).  At vblank loopy_V holds the
//     STALE last-rendered VRAM address; loopy_T holds scroll intent.  So the ppu.v
//     tap exports loopy_t_out (contradicting spec SS2.2 which names loopy_v).
//   * CHR bank: per-slot bank tap; the bridge diffs vs prev (matches chr_slots()).
//   * forced_blank flag: across the whole 15-trace golden dataset the NON-CHR vram
//     total is structurally <=4448 B (< 6000 budget; SS13.4 proves <=4456), so
//     forced_blank is EXCLUSIVELY driven by chr_bank_reupload -- an SNES-VRAM-LRU
//     residency quantity the FPGA cannot observe.  It is FED as `snap_fb_hint`.
//     The other three flags (full_redraw, palette_present, chr_present) ARE
//     computed here from the bridge's own dirty state.
//
// Byte layout is byte-exact with bridge_sim/mailbox.py (SS4.3); canonical order
// with bridge_sim/encoder.py (SS4.2).  Multi-byte little-endian.
//
// MEMORY TIMING: shadow BRAMs use a COMBINATIONAL read-address mux (nt_rd_a/
// chr_rd_a/oam_rd_a) feeding a registered read (1-cycle latency).  Every scan is
// a 2-state request/consume loop (S_*A sets the address via the FSM reg it maps
// to, S_*B consumes the registered read output).  Runs are built streaming so the
// run length is known at close -- no backpatch, inline XOR.
//
// M9K INFERENCE (Quartus, cost the first synthesis run -- don't regress):
//   * Every array read must be SYNCHRONOUS and land DIRECTLY in its own register
//     (`q <= mem[addr]`).  Muxing two async array reads in front of one register
//     ("win_data <= sel ? mbox1[a] : mbox0[a]") is "asynchronous read logic" to
//     quartus_map: both arrays uninfer (Info 276007) and the 2x8KiB mailbox
//     falls back to ~131k FFs => Error 276003 aborts the fit.  The window read
//     is therefore one dedicated q-register PER buffer + comb mux AFTER them
//     (see "window read" block).  ciram/ntdirty/oam and the v2.4 CHR ring
//     (chrbuf/chrdsc) already follow the sync-read template and infer.
//   * `pal` (32x6 = 192 FFs) is DELIBERATELY left as an uninferred register
//     array: same acceptable pattern as ppu.v's oam/sprtemp/palette arrays,
//     uninferred since Fase -1.  Its READ is nonetheless REGISTERED (pal_q,
//     request/consume S_PAL*_RD/_WR pair) -- see the SERIALIZER TIMING note.
//
// CHR-RAM DELIVERY (v2.6, the Gate 2.4 field fix): CMD_CHR_RUN 0x41 is
// AT-LEAST-ONCE.  The payload ring is held until the ACK proves the frame was
// applied and a recovery frame re-emits whatever is unconfirmed; a run is
// idempotent at the renderer (shadow, last-wins).  Full derivation, capacity
// numbers and the byte-identity argument: the ACK-GATED DRAIN block next to the
// ring pointers.  Nothing about it is visible to the byte-exact golden gate.
//
// SERIALIZER TIMING (cost the second STA run -- do not regress): every value
// that lands in mb_wdata/xor_acc must come from a REGISTER (pal_q/ciram_q/
// oam_q/pal_cnt_r/FSM regs), never from {scan -> async array mux} combinational
// chains.  The original S_PAL chained {popcnt32(pal_dirty) -> use_full select
// -> pal[idx] 192-FF mux -> mb_wdata -> xor_acc} in one cycle: setup -4.85ns
// (worst paths pal_dirty[*] -> xor_acc/mb_wdata).  Fixes: pal_cnt_r is
// maintained incrementally at the tap (popcount deleted); the pal read gets a
// request/consume state pair like every other array.  NT/OAM/CHR already
// consumed registered reads (ciram_q/oam_q/ntdirty_q/chrbuf_q/chrdsc_q).  Extra cost:
// +1 cycle per LIST entry, 2 cycles/byte in FULL (~ +34 CLK2 worst = ~0.4us,
// vs a ~1.27ms NES vblank -- noise).
//////////////////////////////////////////////////////////////////////////////////

module nes_bridge(
  input         clk,
  input         rst,

  // ---- Class A tap: nametable write (physical CIRAM offset, post-mirror) ----
  input         nt_we,
  input  [10:0] nt_addr,           // 0..0x7FF (2 KiB CIRAM; H/V/1-screen)
  input  [7:0]  nt_data,

  // ---- Class A tap: CHR-RAM write (offset + DATA) -- v2.4 ---------------------
  // Fase 2.2-lite redefined CMD_CHR_RUN 0x41 to carry the payload INLINE
  // (chr_off(2) len(1) data(len), the CMD_NT_RUN shape), so the tap now has to
  // deliver the WRITTEN BYTE, not just "tile N is dirty".  chr_off is the BYTE
  // offset inside the (<=8 KiB) CHR-RAM, i.e. the mapper-resolved flat address
  // -- the same quantity the old chr_tile carried shifted right by 4
  // (nes_wrap: tapA_addr_w[12:0] instead of tapA_addr_w[16:4]).  CHR-RAM > 8 KiB
  // is MENU_ERR_NOIMPL in the loader (design SS5.1), so 13 bits is the whole space.
  input         chr_we,
  input  [12:0] chr_off,           // byte offset in CHR-RAM (mapper-resolved)
  input  [7:0]  chr_data,          // the byte written

  // ---- Class B tap: palette write (raw idx 0..31; bridge applies NES mirror) ----
  input         pal_we,
  input  [4:0]  pal_idx,
  input  [5:0]  pal_data,

  // ---- Class B tap: OAM byte ----
  input         oam_we,
  input  [7:0]  oam_addr,
  input  [7:0]  oam_data,
  // ---- OAM freeze trigger (v1.4b OAM-tear fix) ------------------------------
  // A 1-cycle pulse that snapshots the live oam[] into oam_frz[] (257 cycles).
  // nes_wrap pulses it MID-DISPLAY (~scanline 120), where OAM is provably stable
  // -- games only rewrite OAM via DMA in vblank, so OAM@120 == OAM@frame-close.
  // Freezing there removes the snapshot from the vblank/OAM-DMA window entirely
  // (the copy-at-tick path snapshotted IN vblank, next to the game's ~dot-253
  // OAM-DMA).  Testbenches tie it to frame_tick (copy-at-tick), byte-identical
  // to the old S_CPY: the NT scan alone is >=4096 cycles before OAM is read.
  input         oam_freeze,

  // ---- Frame close (scanline==241) + snapshot ----
  input         frame_tick,
  input  [15:0] snap_frame,
  input  [14:0] snap_loopy_t,
  input  [2:0]  snap_fine_x,
  input  [7:0]  snap_ppuctrl,
  input  [7:0]  snap_ppumask,
  input         snap_fb_hint,
  // NT arrangement (protocol v1.1, FRAME_HDR.flags[5:4] -- lockstep with
  // bridge_sim/mailbox.py NTARR_*): 0=horizontal mirroring (CIRAM A10=PPU
  // A11, DK), 1=vertical (A10=PPU A10, SMB1), 2=single-screen low,
  // 3=single-screen high.  Sampled per frame at the tick like every other
  // snap_* -- the field already carries DYNAMIC mirroring; v1.1 wiring feeds
  // the static mapper_flags[14] (see nes_wrap), v1.2 swaps in a live mmu tap
  // without touching this module or the protocol.
  input  [1:0]  snap_ntarr,
  input         snap_s0_present,
  input  [7:0]  snap_s0_bank,
  input         snap_s1_present,
  input  [7:0]  snap_s1_bank,

  // ---- v1.3 multi-scroll (CMD_SPLITS, opcode 0x11) -- ADDITIVE ----------------
  // Up to K=4 per-frame scroll entries latched at the tick (snapshot inputs, same
  // discipline as snap_loopy_t).  Emitted ONLY when cnt>=2: split-less games (DK)
  // feed cnt<=1 and NOTHING is emitted -> their command stream (CMD_REGS keeping
  // the mid-display fallback) is byte-identical to v1.2.  Each entry is RAW
  // (scanline, loopy_t, fine_x); sx/sy/ntsel are derived HERE at the tick exactly
  // like CMD_REGS (l_sx={T[4:0],fx}, l_sy={T[9:5],T[14:12]}, ntsel=T[11:10]), so
  // nes_wrap only has to CAPTURE raw taps (mirror of loopy_mid_r).  Flat packed
  // vectors: entry i = [i*8 +:8] / [i*15 +:15] / [i*3 +:3].
  input  [2:0]  snap_split_cnt,   // 0..4 valid entries
  input         snap_split_ovf,   // >4 changes this frame: keep first K, flag it
  input  [31:0] snap_spl_sl,      // 4x scanline[7:0]
  input  [59:0] snap_spl_t,       // 4x loopy_t[14:0]
  input  [11:0] snap_spl_fx,      // 4x fine_x[2:0]

  // ---- v2.3 CHR raster-split (CMD_CHR_SPLITS, opcode 0x13) -- ADDITIVE --------
  // Up to K=4 per-frame (scanline, CHR slot-0 bank) entries, captured by
  // nes_chrsplit_capture.v with the SAME discipline as the scroll splits above
  // (entry 0 = display start, <=1-line coalescence, shortest-strip eviction).
  // Motivation: CMD_CHR_STATE (0x12) is one ABSOLUTE state per frame, which
  // cannot describe games that re-bank MID-DISPLAY every frame (RoboCop 2, MMC1
  // 4K).  Emitted ONLY when cnt>=2 AND !poison: everything else feeds cnt<=1 and
  // NOTHING is emitted, so those frames stay byte-identical to v2.2.
  // `poison` = a mid-frame 8K<->4K (s1_present) flip made the captured strips
  // meaningless; suppress rather than ship an uninterpretable list.
  // Flat packed vectors: entry i = [i*8 +:8] in both.
  input  [2:0]  snap_cspl_cnt,    // 0..4 valid entries
  input         snap_cspl_ovf,    // >4 changes this frame: evicted, flag it
  input         snap_cspl_poison, // frame invalidated (CHR mode changed mid-frame)
  input  [31:0] snap_cspl_sl,     // 4x scanline[7:0]
  input  [31:0] snap_cspl_bank,   // 4x s0_bank[7:0]

  // ---- v2.5 CHR WINDOW VECTOR, mapper 4 / MMC3 (CMD_CHR_STATE8 0x14 +
  //      CMD_CHR_SPLITS8 0x15) -- ADDITIVE ---------------------------------------
  // MMC3 replaces the "pair of 4KB halves" CHR model with a VECTOR OF EIGHT 1KB
  // WINDOWS, which the slot0/slot1 encoding of CMD_CHR_STATE 0x12 cannot express.
  // The vector arrives here already NORMALIZED and SIZE-MASKED by the mmu tap
  // (chr_snap_win): window k = the physical 1KB bank the PPU fetches for CHR
  // addresses k*1024..k*1024+1023, in [k*8 +: 8].
  //
  //   snap_chr_win_en  -- the ACTIVE mapper publishes a window vector (mapper 4
  //                       only).  It is the ONE gate: 0x14 is emitted on EVERY
  //                       frame while it is set and NEVER while it is clear, so
  //                       every frame of every mapper 0/1/2/3/7/28 game stays
  //                       byte-identical (the same additivity clause that closed
  //                       v1.3/v2.3).
  //   snap_chr_win_flags -- the 0x14 flags BYTE verbatim (bit0 = CHR-RAM, bits
  //                       7:1 reserved zero).  Carried as a byte, not a bit, so
  //                       a future flag costs no new port; LOCKSTEP with
  //                       bridge_sim/mailbox.py CHR8_FLAG_CHR_RAM.
  //   snap_cwin_*      -- K=4 raster entries from nes_chrwin_capture.v (entry 0 =
  //                       display start, <=1-line coalescence, shortest-strip
  //                       eviction).  NO POISON port: the window vector has no
  //                       mode flip that can invalidate captured strips, so that
  //                       valve has no trigger here (it stays on the 0x13 path).
  input         snap_chr_win_en,
  input  [63:0] snap_chr_win,
  input  [7:0]  snap_chr_win_flags,
  input  [2:0]  snap_cwin_cnt,    // 0..4 valid entries
  input         snap_cwin_ovf,    // >4 changes this frame: evicted, flag it
  input  [31:0] snap_cwin_sl,     // 4x scanline[7:0]
  input  [255:0] snap_cwin_win,   // 4x win[63:0]

  // ---- Control block outputs ----
  output reg [15:0] frame_seq_o,
  output reg [15:0] frame_len_o,
  output reg [7:0]  status_o,
  output reg        frame_done_o,

  // ---- Control block inputs ----
  input  [15:0] frame_ack_i,
  input         buf_sel_i,
  // Full-state resync enable (hardware ties 1; the byte-exact gate tb ties 0
  // -- its golden assumes perfect consumption and never writes ACK, which
  // would otherwise read as "renderer never caught up" and force full frames).
  // When enabled, a frame is serialized FULL-STATE (whole CIRAM as NT runs +
  // PALETTE_FULL + re-emitted CHR banks + FULL_REDRAW|PAL flags) whenever:
  //   (a) no ACK has been seen since reset (boot: the renderer's first
  //       consumed frame is always complete -- the lost-boot-frames fix), or
  //   (b) the renderer fell >=2 frames behind at frame close (a never-ACKed
  //       buffer is about to be overwritten = real loss; the next frame
  //       re-carries everything).
  // OAM is unconditional already; CHR_DIRTY is NOT forced (CHR-RAM resync is
  // Fase 2; v1 renderer parses it as no-op anyway).
  input         resync_en,

  // ---- SNES-facing mailbox window read ($6000-$7FFF), 1-cycle registered ----
  input  [12:0] win_addr,
  output [7:0]  win_data,

  // ---- Joypad (deliverable f) ----
  input  [15:0] ctrl_p1_i,
  input  [15:0] ctrl_p2_i,
  input         joy_strobe,
  input  [1:0]  joy_clock,
  output [1:0]  joypad_data_o,

  // ---- Breadcrumb band counters ----
  // palette liveness fingerprint (device "bad boot loses palette" probe;
  // group 0x04 idx22/23 via nes_wrap, NDBG v3 +28/+29):
  //   dbg_pal_sum  = sum mod 256 of the 32 live pal[] entries (registered;
  //                  SUM -- not XOR -- so identical-pair writes don't cancel;
  //                  the EXPECTED value is trivially computed from Mesen/sim)
  //   dbg_pal_wcnt = rolling count of tapped palette writes since reset
  output reg [7:0] dbg_pal_sum,
  output reg [7:0] dbg_pal_wcnt,
  output reg [15:0] bc_bytes_last,
  output reg [15:0] bc_frames,
  output reg [15:0] bc_overruns
);

  // ============================================================ opcodes / flags
  localparam [7:0] OP_FRAME_HDR = 8'h01;
  localparam [7:0] OP_REGS      = 8'h10;
  localparam [7:0] OP_SPLITS    = 8'h11;   // v1.3 multi-scroll (CMD_SPLITS)
  // v2.2 CMD_CHR_STATE: ABSOLUTE current CHR-bank state, emitted EVERY frame
  // right after CMD_REGS (fixed offset 14 = HDR 6 + REGS 8).  Design decision
  // after 3 device failures in the event-driven CHR-bank family (first-valid
  // swallow, resync re-announce runaway, 4K-mode inference): follow the
  // hardware -- publish STATE, renderer reconciles idempotently.  4 bytes:
  //   +0 0x12 | +1 s0_bank | +2 (s1_present ? s1_bank : 0) | +3 {7'd0, s1_present}
  // s1_present is the 8K/4K discriminator that never travelled before.  The
  // event CMD_CHR_BANK ($40) stays as an ADVISORY timing/forced-blank hint
  // (drives FLAG_CHR_PRESENT); the renderer MUST reconcile from CHR_STATE.
  localparam [7:0] OP_CHR_STATE = 8'h12;
  // v2.3 CMD_CHR_SPLITS: WHERE the CHR slot-0 bank changed inside the display.
  // ADDITIVE on top of CHR_STATE (which stays unconditional at fixed offset 14);
  // this one is emitted right after it, ONLY when cnt>=2 && !poison:
  //   +0 0x13 | +1 hdr {ovf, 4'd0, cnt[2:0]} | then cnt x [scanline bank]
  // Entry 0 = the bank at display start; each entry is valid until the next
  // entry's scanline (the last one until scanline 240).  The renderer
  // raster-splits CHR via HDMA $210B from this list and still reconciles the
  // absolute state from CHR_STATE, so a frame without the command (the common
  // case) behaves exactly as in v2.2.
  localparam [7:0] OP_CHR_SPLITS= 8'h13;
  // v2.5 CMD_CHR_STATE8 / CMD_CHR_SPLITS8 -- the mapper-4 (MMC3) CHR model.
  // BYTE LAYOUT (this block is the CONTRACT; bridge_sim/mailbox.py mirrors it):
  //
  //   CMD_CHR_STATE8  0x14 : 14 win0 win1 win2 win3 win4 win5 win6 win7 flags
  //                          10 bytes, FIXED FRAME OFFSET 18 (= HDR 6 + REGS 8 +
  //                          CHR_STATE 4), emitted UNCONDITIONALLY on every
  //                          mapper-4 frame.  win[k] = the physical 1KB bank the
  //                          PPU fetches for CHR k*1024..k*1024+1023 (already
  //                          normalized for chr_a12_invert and masked by the CHR
  //                          size class).  flags bit0 = CHR-RAM (the renderer
  //                          picks the window SOURCE from it: pre-converted
  //                          CHR-ROM in PSRAM vs the converted CHR-RAM mirror in
  //                          WRAM); bits 7:1 RESERVED ZERO.
  //   CMD_CHR_SPLITS8 0x15 : 15 hdr cnt x [scanline win0..win7]
  //                          hdr = {ovf, 4'd0, cnt[2:0]}; 2 + cnt*9 bytes, at
  //                          FIXED FRAME OFFSET 28 (immediately after the 0x14),
  //                          emitted ONLY when cnt>=2.  Each entry is an
  //                          ABSOLUTE snapshot of the whole vector, valid until
  //                          the next entry's scanline (the last until 240).
  //
  // THE TWO HARD RULES, and how each one is enforced structurally:
  //   (1) 0x14/0x15 NEVER appear outside mapper 4  -- snap_chr_win_en is the
  //       only path into S_CWST, and the mmu drives it from flags[7:0]==4.
  //   (2) 0x13 NEVER appears IN mapper 4           -- twice over: the legacy
  //       chr_snap_s0/s1 tap is a CONSTANT for mapper 4 (so its capture can
  //       never reach cnt>=2, see mmu.v), AND the state chain below routes
  //       S_CHRST -> S_CWST -> S_CWSP_* -> S_SPL_*, never touching S_CSPL_OP
  //       when the window vector is enabled.
  localparam [7:0] OP_CHR_STATE8 = 8'h14;
  localparam [7:0] OP_CHR_SPLITS8= 8'h15;
  localparam [7:0] OP_PALETTE   = 8'h20;
  localparam [7:0] OP_PAL_FULL  = 8'h21;
  localparam [7:0] OP_NT_RUN    = 8'h30;
  localparam [7:0] OP_CHR_BANK  = 8'h40;
  // v2.4 CMD_CHR_RUN (was CMD_CHR_DIRTY): the opcode BYTE is unchanged (0x41)
  // but the payload is now INLINE -- `41 off_lo off_hi len data[len]`, exactly
  // the CMD_NT_RUN shape, with off = BYTE offset in CHR-RAM (0..0x1FFF) and
  // len 1..255.  The old form (`41 base_tile(2) count(1)`, a POINTER into
  // PSRAM) forced the renderer to read CHR-RAM back over the core's own PSRAM
  // bus and the bridge to keep an 8 KiB M9K shadow; W6's measurement
  // (chrram-study: 99.6-99.9% of the writes sequential, p99 = 1 run/frame,
  // worst frame 2120 B) makes carrying the bytes strictly cheaper.  Only
  // CHR-RAM games are affected: chr_we can only fire when the mapper allows
  // CHR writes, so every CHR-ROM golden stays byte-identical.
  localparam [7:0] OP_CHR_RUN   = 8'h41;
  localparam [7:0] OP_OAM       = 8'h50;
  localparam [7:0] OP_FRAME_DONE= 8'hF0;

  localparam [7:0] FLAG_FORCED_BLANK    = 8'h01;
  localparam [7:0] FLAG_FULL_REDRAW     = 8'h02;
  localparam [7:0] FLAG_PALETTE_PRESENT = 8'h04;
  localparam [7:0] FLAG_CHR_PRESENT     = 8'h08;

  localparam NT_SIZE   = 2048;
  // v2.4 CHR-RAM capture ring (replaces the 512-entry dirty bitmap + its pend
  // generations, all three of which became dead the moment 0x41 started
  // carrying data -- see the CHR RING block below for the sizing argument).
  localparam CB_SZ     = 8192;   // payload ring, bytes  (power of 2: free wrap)
  localparam DSC_N     = 256;    // run-descriptor ring, entries
  // MAILBOX CAPACITY VALVE for the CHR drain (v2.6).  A mailbox buffer is 8192
  // bytes and wptr is 13 bits: it WRAPS silently.  Before ack-gating, one
  // frame could only ever carry one frame's worth of CHR (<=2120 B measured,
  // worst golden frame 2721 B total), so the wrap was unreachable in practice.
  // With at-least-once the ring can hold ~3 frames of unconfirmed payload and a
  // single recovery frame would try to ship all of it, on top of a possibly
  // large NT union -> wrap = a corrupt frame, the one thing this module must
  // never produce.  So a run is only STARTED while wptr < CHR_WSTOP; whatever
  // does not fit stays in the ring (the drain cursor simply stops there) and is
  // shipped by the next frame -- at-least-once is preserved, the frame is not.
  //   worst tail after the last accepted start = 4 (run header) + 255 (payload)
  //   + 257 (OAM) + 2 (FRAME_DONE) = 518  =>  CHR_WSTOP <= 8192-518 = 7674.
  // 7600 leaves 74 B of slack.  INERT for every golden (max frame 2721 B).
  localparam [12:0] CHR_WSTOP = 13'd7600;
  // RETRANSMISSION WINDOW -- the ONLY thing this fix is allowed to bound.
  //
  // The v2.6b cut capped the DRAIN itself (CHR_BUDGET bytes of 0x41 per frame).
  // That was wrong in a way no skip scenario could show: with a PERFECT lockstep
  // consumer (lag<=1, zero skips, zero recoveries) a game writing more than the
  // cap per frame has its surplus held back every frame, the ring fills, and the
  // tap DROPS -- measured 6585 drops at 2500 B/frame, 26685 at 3000, 49955 at
  // 3700, all with cb_ovf set, while the same RTL with the cap removed dropped
  // ZERO.  And the "margin" over the corpus peak was an illusion: 2120 B/frame
  // is a PLATEAU (megaman1_usa and megaman2_ultra, attract AND gameplay, all
  // exactly 2120 = a saturating value), not a tail, so 2176 was 2.6% above a
  // number that does not describe the maximum of anything.
  //
  // The drain is therefore UNCAPPED again -- byte for byte the v2.5 behaviour
  // for bytes that have never been emitted.  What is bounded instead is the
  // RETRANSMISSION WINDOW: the span of already-emitted-but-unconfirmed payload
  // the bridge is willing to keep for a re-send.  Every tick TRIMS it back to
  // this size (see chr_trim_w), which buys three properties at once:
  //   * a recovery frame costs at most RETX_CHUNK extra bytes on top of a normal
  //     v2.5 frame (the WINDOW is what may be retained, the CHUNK is what may be
  //     re-sent in one frame) -- the cascade ("NUNCA full-storm") stays bounded;
  //   * FRESH bytes are never delayed by old ones, because only the old part is
  //     ever capped -- the head-of-line starvation that blacked out the device
  //     cannot be expressed any more;
  //   * ring occupancy stays bounded, so the tap keeps its room.  Do NOT trust
  //     the tidy "window + one frame" arithmetic here: MEASURED peak occupancy
  //     at 2200 CHR B/frame with skips is 7798 of 8184, because the drain, the
  //     writes and the trim interleave across a whole frame rather than lining
  //     up.  The per-cycle bound in the tap is what actually holds this, not the
  //     arithmetic -- and the rate sweep in tb/run_chrram_skip.sh is what proves
  //     it, point by point, instead of a paragraph asserting it.
  // SIZING.  The window has to hold at least ONE worst frame, or it cannot
  // re-send a single lost dump -- and the corpus figure of 2120 B/frame is a
  // SATURATING PLATEAU (megaman1_usa and megaman2_ultra, attract and gameplay,
  // all exactly 2120), so it is a floor on the real maximum, not a ceiling.
  // 3072 holds one such frame with room to spare, while keeping the peak ring
  // occupancy (window + one frame of writes) far below the 8184 the tap needs:
  // safe up to ~5100 B/frame, i.e. more than twice the physical NES maximum.
  // The DESCRIPTOR window is bounded separately, because scattered writes
  // exhaust chrdsc with the payload ring nearly empty (~90 descriptors/frame
  // measured) -- a byte-only trim leaves that mode wedged.
  localparam [12:0] RETX_WINDOW  = 13'd3072;
  // PER-FRAME RETRANSMISSION CHUNK.  A recovery does NOT have to re-send the
  // whole window in one frame, and it must not: at Mega-Man rate (2200 B/frame
  // of fresh data) a 3 KB re-send DOUBLES the frame, the renderer -- which
  // applies ~1400 B/frame -- falls further behind, and the fix ends up losing
  // MORE than doing nothing (measured 8192 of 8192 bytes wrong versus 5992 for
  // the pre-fix control).  So the re-sent part is trickled: at most RETX_CHUNK
  // bytes per frame, IN ORDER, and the committed tail advances to whatever was
  // re-sent, so each unconfirmed byte gets a BOUNDED number of extra attempts
  // instead of an unbounded one.  Fresh data is never delayed by more than this.
  // TUNED BY MEASUREMENT, both directions matter:
  //   768 -> too mean: the ordinary skip scenarios stopped healing completely
  //          (606 and 829 bytes lost where 0 is achievable).
  //  3072 -> too generous: at Mega-Man rate it doubled the frame and lost MORE
  //          than the pre-fix control (8192 vs 5992).
  //  1536 -> WRONG, and it is what the W16e field deploy shipped.  It was tuned
  //          against tb streams that carried 140 tiles/frame with ppumask
  //          rendering ON -- a case the NES cannot produce (the physical ceiling
  //          with rendering on is ~24 tiles/frame; the 2120 B/frame plateau only
  //          ever happens with the screen BLANKED) -- so the tuning optimised an
  //          impossible operating point.
  //
  // THE BOUND IS NOT A TUNING KNOB, IT IS ARITHMETIC ON THE CONSUMER.  The
  // renderer stages CHR through a buffer of NES_CHRQ_TILES_MAX = 192 tiles =
  // 3072 BYTES per frame (snes/nes/nes_equates.i65).  Past that it escalates to
  // nes_chrq_full, a whole-shadow rebuild.  A frame carries FRESH + RETX, and
  // fresh alone reaches the 2120 B plateau on a level-entry dump, so:
  //     RETX_CHUNK <= 3072 - 2120 = 952
  // 1536 breaks it by 584 B: any frame where a rewind lands on a dump is 228
  // tiles, over the renderer's ceiling, and forces the rebuild path.  Observed
  // in the renderer harness the moment a >192-tile frame was fed to the REAL
  // renderer: nothing staged (dev tmax=0), nes_chrq_full=1 left pending, shadow
  // incomplete -- and on the device, a renderer that never finishes a frame
  // never ACKs, which is exactly the field signature (publisher alive, 6502
  // alive, overruns advancing 1:1 with frames, zero frames consumed).
  //  768 -> chosen: under the 952 ceiling with margin for a fresh burst above
  //          the plateau, and a whole number of 16-byte tiles (48).
  //
  // *** ABI LOCKSTEP -- snes/nes/nes_equates.i65 NES_CHRQ_TILES_MAX ***
  // This is a two-sided contract in the same sense as memory.h <-> memmap.i65:
  //     fresh_plateau (2120 B) + RETX_CHUNK  <=  NES_CHRQ_TILES_MAX * 16
  // Break it and NOTHING fails offline -- the byte-exact goldens contain no
  // retransmission at all, so they cannot see it; only the renderer notices, by
  // escalating to a whole-shadow rebuild and never finishing a frame, i.e. by
  // never ACKing.  That is how W16e reached silicon.  Change either side and
  // re-check the other, and keep tb/run_retx_renderer.sh (which asserts the
  // 3072 B/frame ceiling structurally over every generated stream) green.
  //
  // TWO CALIBRATIONS, so nobody reads more into this than it says:
  //  * the rendering-ON write budget is a RANGE, 10-35 tiles/frame depending on
  //    where in the frame the writes land -- not the single "24" figure an
  //    earlier revision of this comment used.
  //  * `fresh <= 2120 B` is an EMPIRICAL plateau of the corpus, NOT something
  //    this module enforces.  A CHR-RAM game with a tighter dump loop than Mega
  //    Man's blows the 192-tile ceiling with NO retransmission at all, so the
  //    livelock is LATENT in the shipping v2.3 as well; W16e only lowered the
  //    threshold that triggers it (232 -> 180 tiles of headroom).
  // TODO (post-hardware, bundle with ack_moved and the mid-run clamp fix): make
  // this structural instead of empirical.  Either a dynamic allowance computed
  // at the tick, `retx_allow = 3072 - fresh_pending`, or -- the advisor's
  // recommendation -- a HARD 3072 B/frame cap on the CHR section with DEFERRAL
  // in the CHR_WSTOP style (the surplus keeps its place in the ring instead of
  // being dropped).  The cap is preferred because it degrades VISIBLY through
  // cb_ovf rather than as a silent livelock.
  localparam [12:0] RETX_CHUNK   = 13'd768;
  localparam [7:0]  RETX_DSC     = 8'd64;     // descriptors kept for re-send
  // HIGH-WATER = HALF the ring, and that number is derived, not chosen: a rewind
  // makes the next frame's slice (window + one frame) instead of (one frame), so
  // the ring has to hold roughly TWICE what v2.5 held.  Gating the rewind at
  // half the ring bounds post-rewind occupancy to CB_SZ/2 and leaves the other
  // half for the frame of writes that follows -- i.e. the retransmission simply
  // switches itself off (falling back to v2.5) at rates where the ring cannot
  // hold two frames.  3/4 was measured to be too generous: at 2800 B/frame the
  // slice reached 5600 on top of a 3072 window and the tap dropped 2160 bytes
  // with cb_ovf, while v2.5 at the same rate dropped none.
  localparam [12:0] RING_HIWATER = 13'd4096;  // CB_SZ/2
  localparam [7:0]  DSC_HIWATER  = 8'd128;    // DSC_N/2

  // ============================================================ shadow memories
  //
  // SNAPSHOT / PING-PONG (pre-hardware fix; do not regress) -- taps are accepted
  // ALWAYS (no longer gated on S_IDLE): on live hardware frame-N+1 events (the
  // game's NMI OAM-DMA lands EARLY in vblank, DURING frame N's serialization)
  // must keep accumulating while the serializer reads FROZEN state.  Per
  // category:
  //   * dirty bitmaps + per-frame counters (ntdirty/pal_dirty/counts, and the
  //     chr_any flag):
  //     PING-PONG (2 banks: taps write bank `live`, serializer scans + clears
  //     bank ~live, flip at frame_tick).  Correct because they RESET per frame.
  //   * OAM: COPY-AT-TICK (oam live + oam_frz; S_CPY, pipelined 1B/cycle, 258
  //     cycles).  NOT ping-pong: OAM is PERSISTENT -- ping-pong would emit
  //     2-frame-old bytes whenever a game skips its OAM-DMA (lag frames do);
  //     the copy freezes the tick-instant contents.  Residual race: a DMA byte
  //     landing inside the ~3us copy window may be captured one frame early --
  //     same benign class as the trace-F/scanline-241 offset (spec SS13.3).
  //   * ciram + pal[] VALUES: SINGLE, read LIVE by the serializer.  A frame-N+1
  //     write read early is emitted with the NEW value in frame N AND re-emitted
  //     in frame N+1 (its dirty bit is in the live bank) -- the renderer
  //     converges; saves a 2KiB BRAM copy.
  // M9K delta of that fix: ntdirty x2 (+2Kb) + oam_frz (+2Kb).  The CHR dirty
  // bitmap and its pend generations that used to live here are GONE since v2.4:
  // 0x41 carries data now, so a bitmap cannot describe what to send.
  // A frame_tick that collides with an in-flight nt RMW (or, theoretically,
  // lands outside S_IDLE) is latched in tick_pend and accepted at the next
  // S_IDLE cycle -- ticks are never silently dropped anymore.
  reg [7:0] ciram    [0:NT_SIZE-1];    // single, read LIVE (see note)
  reg       ntdirty0 [0:NT_SIZE-1];    // ping-pong bank 0
  reg       ntdirty1 [0:NT_SIZE-1];    // ping-pong bank 1
  reg [7:0] oam      [0:255];          // live (taps)
  reg [7:0] oam_frz  [0:255];          // frozen copy (S_CPY at tick)
  // ============================================ CHR RING (v2.4, CMD_CHR_RUN)
  // MICROARCHITECTURE (the one decision this block exists to record):
  //
  //   chrbuf  = a CIRCULAR byte ring of the CHR-RAM payload, appended by the
  //             tap 1 byte/cycle, drained by the serializer.
  //   chrdsc  = a CIRCULAR ring of RUN DESCRIPTORS {off[12:0], ptr[12:0]},
  //             ONE write, at run OPEN only.  The run's LENGTH is never stored:
  //             it is `next_descriptor.ptr - this.ptr` (and, for the last run of
  //             a frame, `l_cb_end - this.ptr`), so nothing has to be
  //             back-patched and the tap never needs more than ONE write port
  //             on either array in a cycle.  That is what keeps the tap a
  //             single-cycle event exactly like the old dirty-bit tap -- no
  //             micro-sequencer, no skid buffer, no assumption about how fast
  //             the NES can hit $2007.
  //
  // Why not the obvious alternatives:
  //   * 8 KiB CHR shadow + the existing tile dirty bitmap (the `ciram`+`ntdirty`
  //     shape): 8 M9K, and only ~7 are free (fit is at 49/56) -- and the design
  //     (SS5.4) kills the shadow on purpose.
  //   * headers written IN-BAND into the payload ring: one memory instead of
  //     two, but opening a run then costs 5 byte-writes = a 5-cycle sequencer
  //     plus a skid, i.e. a timing ASSUMPTION about tap spacing.  Rejected.
  //   * a descriptor array holding (off, len): len is only known at CLOSE, so
  //     close+open collide on the same array in the same cycle.  Storing `ptr`
  //     instead removes the close write entirely.
  //
  // SIZING (chrram-study/out/*_frames.csv, 13 traces / 23 100 frames; the ring
  // DOUBLED to 8192 in v2.6 because the drain is now ACK-GATED -- see the
  // ACK-GATED DRAIN block for the capacity derivation):
  //   worst single frame = 2120 payload bytes (Mega Man's screen-off dumps),
  //   max runs/frame = 4, longest run 133 tiles; +~50-130 bytes accumulate
  //   during the ~8k-cycle serialization => ~2250 B per unconfirmed frame.
  //   The ring must now hold every frame published-but-not-confirmed plus the
  //   accumulating one, so the quantity that sizes it is the worst MULTI-FRAME
  //   window, NOT the worst frame times a guess.  Sliding sums of chr_writes
  //   over the CHR-RAM traces (the fantasy_zone column of that study is
  //   CHR-ROM, where chr_we never fires, so it does not count):
  //     1 frame 2120 | 2 frames 4234 | 3 frames 6346 | 4 frames 7722
  //   (Mega Man 1/2 refilling the whole 8 KB with the screen off).  Lag
  //   oscillates 1<->2 in normal operation = 2-3 frames unconfirmed = <=6346 of
  //   8184 usable; the measured 4-frame worst case still fits (7722 < 8184).
  //   Descriptors: 4 runs + the <=255 slicing of a 2120-byte run (9 slices)
  //   ~= 13/frame, x4 frames = 52 of 256 = 20% -- the descriptor ring did NOT
  //   have to grow.
  //   RESIDUAL, documented on purpose: a renderer more than ~4 frames behind
  //   DURING a max-rate dump saturates the ring; the byte is then DROPPED, the
  //   run is closed (so no hole ever appears INSIDE a run) and `cb_ovf` latches
  //   into status_o[1] -- partial degradation, never a corrupt run, and still
  //   far better than v2.5 (which lost a whole frame's CHR on ANY skip).  There
  //   is no cheap fix above this: the fit is at 55/56 M9K, and 16 KiB would
  //   cost 8 more.
  // M9K BUDGET (check this against the fitter report -- the fit was at 51/56
  // AFTER v2.4): +4 (chrbuf 4096->8192 x8 = +32768 b); chrdsc widens 25->26 b
  // and still costs ONE block (256x36 config) => NET +4 -> ~55/56.  If
  // quartus_map ever refuses to infer the 26-bit-wide chrdsc as RAM (Info
  // 276007 -> a 6656-FF register array), split it into two arrays (256x13 off
  // + 256x13 ptr) and pay one extra M9K; do NOT mux two async array reads in
  // front of chrdsc_q.
  reg [7:0]  chrbuf [0:CB_SZ-1];   // M9K (8 blocks)
  reg [25:0] chrdsc [0:DSC_N-1];   // M9K (1 block): {off[12:0], ptr[12:0]}
  // ---- v2.6 publish HISTORY: where the drain stopped for the last 8 published
  // frames, indexed by seq[2:0].  Tiny register arrays (8x13 + 8x8 + 8x1 = 176
  // FFs), read combinationally by seq index and landing in a REGISTER (cb_cp /
  // dsc_cp) -- not in the mb_wdata/xor_acc cone, so the SERIALIZER TIMING rule
  // is untouched.
  // DEPTH 8, NOT 4 -- this cost a FIELD FAILURE (v2.6 first cut, Mega Man: black
  // screen, +63 overruns/s).  With depth 4 the commit needed lag<=3 while
  // lost_r ARMS A RECOVERY at lag>=3, so a renderer sitting at lag>=4 -- the
  // normal regime during a level-entry CHR dump -- could never commit anything:
  // every frame rewound to the same frozen tail and re-shipped the same oldest
  // bytes, the new writes never got out (head-of-line starvation) and cb_ovf
  // stayed CLEAR, so the breadcrumb showed nothing.  The commit window must
  // extend BEYOND the recovery trigger, not stop exactly at it.
  reg [12:0] hist_cb  [0:7];
  reg [7:0]  hist_dsc [0:7];
  reg        hist_cov [0:7];       // this frame's drain STARTED at the committed tail
  // PENDING accumulators (chain-breaker; cost a hardware iteration -- see the
  // LOSS THRESHOLD note): union of the dirty bits of every serialized-but-not-
  // yet-confirmed frame.  A loss no longer emits a FULL frame (~2.4KB, whose
  // apply on the real renderer takes 2-3 frame periods and re-triggers loss =
  // the self-sustaining degeneration seen on hardware); it emits frozen|pending
  // = the REAL delta since the last consumed frame (typically tens of bytes).
  // The scan REWRITES every visited offset each serialization
  // (pend <= frz | (pend & epoch)), so "clearing" is just dropping the epoch
  // bit -- no extra pass.  Cleared when ACK catches up (tick with ack==seq
  // and no unprocessed skip) or, per generation, when the recovery frame
  // that sealed it is confirmed consumed (ack >= recov_seq).  Worst case
  // (ACK never in time): the union saturates toward full = degrades to the
  // OLD behavior, never worse.  Boot (~saw_ack) still uses true FULL frames.
  // TWO GENERATIONS per cell (bit1 = A "sealed", bit0 = B "young") -- see the
  // GENERATION note at the pend registers below.
  // CHR has NO pend generation: the payload ring IS the pending store -- see
  // the ACK-GATED DRAIN block below (v2.6, plan (b) of the v2.4 note, taken
  // after Gate 2.4 hardware showed Mega Man (mapper 2, CHR-RAM) with a
  // permanently corrupt background and 79 overruns in ~33 s: every lost frame
  // dropped its CHR bytes for ever, and a ONE-SHOT boot dump never comes back).
  // Diagnosis first: status_o[1] (cb_ovf) says "ring full", NOT "frame lost".
  reg [1:0] ntpend  [0:NT_SIZE-1];
  reg [7:0] mbox0   [0:8191];   // M9K (inferred; see M9K INFERENCE in the header)
  reg [7:0] mbox1   [0:8191];   // M9K (inferred)
  reg [5:0] pal     [0:31];     // register array ON PURPOSE (192 FFs); read via pal_q reg

  integer k, kk;
  integer rk;
  initial begin
    for (k=0;k<NT_SIZE;k=k+1)   begin ciram[k]=8'h00; ntdirty0[k]=1'b0; ntdirty1[k]=1'b0; ntpend[k]=2'b00; end
    for (k=0;k<256;k=k+1)       begin oam[k]=8'h00; oam_frz[k]=8'h00; end
    // GOTCHA (cost a synthesis run when CB_SZ went 4096 -> 8192): quartus_map
    // refuses a constant loop of more than 5000 iterations -- "Error (10106):
    // Verilog HDL Loop error ... loop must terminate within 5000 iterations",
    // and it kills ELABORATION of the whole module, so nothing downstream even
    // runs.  iverilog does not care.  Clearing the 8192-byte ring therefore has
    // to be a NEST (64 x 128), not a flat sweep.
    for (k=0;k<CB_SZ/128;k=k+1)
      for (kk=0;kk<128;kk=kk+1) chrbuf[k*128+kk]=8'h00;
    for (k=0;k<DSC_N;k=k+1)     chrdsc[k]=26'd0;
    for (k=0;k<8;k=k+1)         begin hist_cb[k]=13'd0; hist_dsc[k]=8'd0; hist_cov[k]=1'b0; end
    for (k=0;k<32;k=k+1)        pal[k]=6'h00;
  end

  // ============================================================ dirty bookkeeping
  // All per-frame accumulators ping-ponged by `live`; the serializer reads the
  // frozen views *_frz.  pal counts stay INCREMENTAL registers (the serializer-
  // timing fix: no popcount -- see SERIALIZER TIMING in the header).
  reg        live;         // bank the TAPS write; ~live = frozen (serialized)
  reg        tick_pend;    // frame_tick latched until accepted in S_IDLE
  reg        saw_ack;      // any nonzero NES_FRAME_ACK seen since reset
  reg        lost_r;       // registered (seq-ack)>=3 (genuine loss)
  reg        force_full;   // BOOT ONLY now: serialize true full state
  reg        lost_hold;    // this frame is a RECOVERY: scan adds the pending union
  reg        seal_hold;    // this recovery SEALS generation A (A was empty/confirmed)
  // GENERATIONS (cost the tb_handshake "bootskip" scenario -- a self-
  // sustaining degenerate chain REBORN through the union): with a single
  // epoch, the only safe clear is a tick with ack==seq, which a phase-adverse
  // consumer NEVER hits while recovery frames are large -- a big burst that
  // was already delivered and CONFIRMED stays in the union forever, every
  // recovery re-carries ~2.4KB, the apply exceeds one period, the consumer
  // skips again -> ack_skip fires again -> chain.  Fix: the union lives in
  // TWO generations per cell.  New dirty accumulates in B (young).  A
  // recovery frame delivers frz|A|B and SEALS: A := frz|A|B, B := 0 (done by
  // the scan's rewrite pass, no extra sweep).  When the consumer confirms a
  // frame at/after the last recovery (ack >= recov_seq, mod 2^16), everything
  // sealed in A was delivered IN that recovery -> kill A via its valid bit
  // (no sweep), regardless of lag or later skips.  Dirty of frames after the
  // recovery is in B and survives.  The chain now dies in ~2 recoveries: the
  // second one carries only B (small).
  reg        pend_valid;   // generation B (young) holds content
  reg        pend_a_valid; // generation A (sealed at the last recovery) holds content
  reg        pend_avf;     // pend_a_valid latched at the tick (stable for the scan)
  reg        pend_bvf;     // pend_valid   latched at the tick
  reg [31:0] pal_pend_a, pal_pend_b;
  reg        recov_active; // a recovery frame is in flight, unconfirmed
  reg [15:0] recov_seq;    // seq of the LAST recovery frame published
  // ACK-SKIP detector (cost a hardware iteration -- "EVERY death transition
  // loses the background"): a 1-2 frame loss never raises seq-ack to 3 (the
  // renderer catches right back up), so the >=3 lag trigger misses it and the
  // pending union is never emitted -- the skipped transition burst is lost
  // until the next burst.  The EXACT, false-positive-free loss signal is the
  // ACK SEQUENCE itself: the renderer ACKs every frame it consumes, so an ACK
  // that ADVANCES BY >=2 in one step proves the intermediate frames were
  // skipped.  Arm a recovery for the next tick when that happens.
  reg [15:0] ack_prev;
  reg        ack_skip;     // sticky until consumed by a tick
  reg [11:0] nt_cnt0, nt_cnt1;
  reg        chr_any0, chr_any1;
  reg [31:0] pal_dirty0, pal_dirty1;
  reg [5:0]  pal_cnt0, pal_cnt1;

  wire       pal_mirror = pal_idx[4] & ~pal_idx[1] & ~pal_idx[0];
  wire [4:0] pal_eff    = pal_mirror ? {1'b0, pal_idx[3:0]} : pal_idx;

  wire [11:0] nt_cnt_frz    = live ? nt_cnt0    : nt_cnt1;
  wire        chr_any_frz   = live ? chr_any0   : chr_any1;
  wire [31:0] pal_dirty_frz = live ? pal_dirty0 : pal_dirty1;
  wire [5:0]  pal_cnt_frz   = live ? pal_cnt0   : pal_cnt1;
  wire        pal_pend_any  = lost_hold & ((pend_avf & (pal_pend_a != 32'd0))
                                         | (pend_bvf & (pal_pend_b != 32'd0)));
  wire        pal_present   = (pal_cnt_frz != 6'd0) | force_full | pal_pend_any;
  // recovery always uses PALETTE_FULL when palette data is owed: emitting the
  // 33-byte full palette avoids a popcount over frz|pend (the serializer-
  // timing rule) and is cheap
  wire        use_full      = (pal_cnt_frz > 6'd16) | force_full | pal_pend_any;

  // ============================================================ FSM state decl
  // WIDTH: 6 bits since v2.3 (the CMD_CHR_SPLITS states pushed the count past
  // 32).  Every state literal below MUST stay 6'dN -- a leftover 5'dN silently
  // zero-extends in comparisons but truncates in assignments.
  // v2.5 added FIVE states (S_CWST + S_CWSP_*), taking the highest literal to
  // 6'd43 of the 63 a 6-bit register holds -- still 20 spare, no width change.
  localparam [5:0]
    S_IDLE  = 6'd0,  S_SETUP = 6'd1,  S_HDR  = 6'd2,  S_REGS = 6'd3,
    S_PAL   = 6'd4,  S_NTA   = 6'd5,  S_NTB  = 6'd6,  S_NTHDR= 6'd7,
    S_NTDA  = 6'd8,  S_NTDB  = 6'd9,  S_CBANK= 6'd10,
    // v2.4 CMD_CHR_RUN emission (replaces the old S_CA/S_CB/S_CHDR dirty-bitmap
    // scan): walk the frozen slice of the descriptor ring, then stream each
    // run's bytes straight out of the payload ring.  Same request/consume
    // shape as S_NTA/S_NTB and S_NTDA/S_NTDB (every array read is REGISTERED
    // and mb_wdata only ever sees a register -- SERIALIZER TIMING).
    S_CR0   = 6'd11,  // descriptor cursor test; request chrdsc[dsc_i]
    S_CR1   = 6'd12,  // consume descriptor -> cur_coff/cur_cptr; advance dsc_i
    S_CR2   = 6'd13,  // wait state: chrdsc[dsc_i+1] read registers
    S_CR3   = 6'd35,  // consume NEXT ptr -> cur_clen; park cb_rp at cur_cptr
    S_CRH   = 6'd36,  // emit 41 off_lo off_hi len
    S_CRA   = 6'd37,  // request chrbuf[cb_rp]
    S_CRB   = 6'd38,  // emit payload byte
    S_OAMA  = 6'd14, S_OAMB = 6'd15,
    S_DONE  = 6'd16, S_CLEAR = 6'd17, S_FINISH=6'd18,
    // palette emission, pipelined (request/consume like NT/OAM/CHR -- the pal
    // array read is REGISTERED into pal_q; mb_wdata/xor_acc consume registers):
    S_PALL_CNT = 6'd19,  // LIST: emit count byte (pal_cnt_r)
    S_PALL_FIND= 6'd20,  // LIST: scan pal_dirty, emit idx byte on hit
    S_PALL_RD  = 6'd21,  // LIST: dead cycle, pal_q <= pal[pal_i]
    S_PALL_WR  = 6'd22,  // LIST: emit value byte (pal_q)
    S_PALF_RD  = 6'd23,  // FULL: dead cycle, pal_q <= pal[pal_i]
    S_PALF_WR  = 6'd24,  // FULL: emit value byte (pal_q)
    S_CPY      = 6'd25,  // OAM copy-at-tick (live -> frz, pipelined 1B/cycle)
    // v1.3 CMD_SPLITS emission (after S_CHRST/S_CSPL_*, before S_PAL/S_NTA):
    S_SPL_OP   = 6'd26,  // emit opcode 0x11
    S_SPL_HDR  = 6'd27,  // emit hdr byte {ovf, cnt}
    S_SPL_LD   = 6'd28,  // consume: cur_* <= l_spl_*[spl_i]  (idx mux off the mb path)
    S_SPL_E    = 6'd29,  // emit the 4 bytes of entry spl_i (sl, sx, sy, ntsel)
    // v2.2 CMD_CHR_STATE emission (UNCONDITIONAL, right after S_REGS -- fixed
    // frame offset 14 = HDR(6)+REGS(8); absolute CURRENT bank state every
    // frame, the renderer reconciles idempotently -- see the localparam
    // OP_CHR_STATE comment for the design rationale):
    S_CHRST    = 6'd30,  // emit 12 s0_bank (s1p?s1_bank:0) {7'd0,s1p}
    // v2.3 CMD_CHR_SPLITS emission (right after S_CHRST, BEFORE S_SPL_*), same
    // request/consume shape as S_SPL_*: S_CSPL_LD moves the indexed array read
    // off the mb_wdata/xor_acc cone, S_CSPL_E emits from single registers.
    S_CSPL_OP  = 6'd31,  // emit opcode 0x13
    S_CSPL_HDR = 6'd32,  // emit hdr byte {ovf, cnt}
    S_CSPL_LD  = 6'd33,  // consume: cur_c* <= l_cspl_*[cspl_i]
    S_CSPL_E   = 6'd34,  // emit the 2 bytes of entry cspl_i (scanline, bank)
    // v2.5 CMD_CHR_STATE8 / CMD_CHR_SPLITS8 emission (mapper 4).  The 0x14 sits
    // between S_CHRST and the split states so its 10 bytes land at the fixed
    // frame offset 18; the 0x15 states copy the S_CSPL_* request/consume split
    // one for one (S_CWSP_LD moves the indexed 64-bit array read off the
    // mb_wdata/xor_acc cone; S_CWSP_E emits from single registers).
    S_CWST     = 6'd39,  // emit 14 win0..win7 flags   (10 bytes, sub 0..9)
    S_CWSP_OP  = 6'd40,  // emit opcode 0x15
    S_CWSP_HDR = 6'd41,  // emit hdr byte {ovf, cnt}
    S_CWSP_LD  = 6'd42,  // consume: cur_cw* <= l_cwsp_*[cwsp_i]
    S_CWSP_E   = 6'd43;  // emit the 9 bytes of entry cwsp_i (scanline + vector)

  reg [5:0]  st;
  reg [3:0]  sub;
  reg [12:0] wptr;
  reg [7:0]  xor_acc;
  reg [15:0] new_seq;
  reg        mb_wbuf;

  // latched snapshot
  reg [15:0] l_frame;
  reg [7:0]  l_ppuctrl, l_ppumask, l_flags, l_sx, l_sy;
  reg [1:0]  l_ntarr;
  reg [1:0]  l_ntsel;   // REGS byte 7: NT select = loopy_T[11:10] of the mid
                        // snapshot (v1.2) -- NOT ppuctrl[1:0] of the close,
                        // which in split games is the HUD's (always 0)
  reg        l_s0p, l_s1p, l_s0chg, l_s1chg;
  reg [7:0]  l_s0b, l_s1b;

  // v1.3 CMD_SPLITS: entries derived + latched at the tick (single registers,
  // read during emission via a request/consume LD state so the mb_wdata/xor
  // cone is a pure single-register mux -- SERIALIZER TIMING).
  reg [2:0]  l_split_cnt;         // 0..4
  reg [7:0]  l_split_hdr;         // {ovf, 4'd0, cnt[2:0]}
  reg [7:0]  l_spl_sl [0:3];
  reg [7:0]  l_spl_sx [0:3];
  reg [7:0]  l_spl_sy [0:3];
  reg [1:0]  l_spl_nt [0:3];
  reg [2:0]  spl_i;               // emission entry cursor
  reg [7:0]  cur_sl, cur_sx, cur_sy;   // consumed entry (loaded in S_SPL_LD)
  reg [1:0]  cur_nt;
  integer    si;

  // v2.3 CMD_CHR_SPLITS: same latch-at-the-tick + request/consume discipline.
  // No derivation needed (the payload is already a raw scanline + bank byte).
  reg [2:0]  l_cspl_cnt;          // 0..4
  reg [7:0]  l_cspl_hdr;          // {ovf, 4'd0, cnt[2:0]}
  reg        l_cspl_go;           // cnt>=2 && !poison -> emit this frame
  reg [7:0]  l_cspl_sl [0:3];
  reg [7:0]  l_cspl_bk [0:3];
  reg [2:0]  cspl_i;              // emission entry cursor
  reg [7:0]  cur_csl, cur_cbk;    // consumed entry (loaded in S_CSPL_LD)

  // v2.5 CMD_CHR_STATE8 / CMD_CHR_SPLITS8 (mapper 4): same latch-at-the-tick +
  // request/consume discipline, payload 8x wider.  l_cwsp_w is the frozen copy
  // the area budget calls out (4 x 64 b = 256 FF); it exists for the same reason
  // l_cspl_bk does -- the serializer must read a stable list while the capture
  // keeps accumulating the NEXT frame.
  reg        l_cwin_en;           // mapper 4 this frame -> emit 0x14
  reg [63:0] l_cwin;              // the frame-close window vector
  reg [7:0]  l_cwin_flags;        // 0x14 flags byte (bit0 = CHR-RAM)
  reg [2:0]  l_cwsp_cnt;          // 0..4
  reg [7:0]  l_cwsp_hdr;          // {ovf, 4'd0, cnt[2:0]}
  reg        l_cwsp_go;           // cnt>=2 -> emit 0x15 this frame
  reg [7:0]  l_cwsp_sl [0:3];
  reg [63:0] l_cwsp_w  [0:3];
  reg [2:0]  cwsp_i;              // emission entry cursor
  reg [7:0]  cur_cwsl;            // consumed entry scanline (S_CWSP_LD)
  reg [63:0] cur_cwin;            // consumed entry vector   (S_CWSP_LD)

  // chr bank prev tracking
  reg        s0_valid, s1_valid;
  reg [7:0]  s0_prev, s1_prev;

  // scan cursors / run builders
  reg [11:0] nt_i;         // 0..NT_SIZE
  reg        nt_inrun;
  reg [10:0] run_start;
  reg [8:0]  run_len, run_k;
  reg [8:0]  oam_i;        // 0..256
  reg [5:0]  pal_i;

  // clear-pass cursor (NT dirty bitmap only since v2.4)
  reg [11:0] clr_nt;

  // ---- v2.4 CHR ring pointers / run builder (see the CHR RING block) --------
  // TAP side (all updated in ONE cycle per chr_we, no sequencer):
  reg [12:0] cb_wp;        // payload ring head
  reg [7:0]  dsc_wp;       // descriptor ring head
  reg        cb_open;      // a run is open (cleared at every frame tick)
  reg [7:0]  cb_len;       // bytes in the open run (slice at 255)
  reg [12:0] cb_next_off;  // CHR offset a sequential write must carry
  reg        cb_ovf;       // sticky: a byte was dropped (ring full)
  // FROZEN slice, latched at the tick:
  reg [7:0]  l_dsc_end;    // one past the last descriptor of the closing frame
  reg [12:0] l_cb_end;     // one past the last payload byte of the closing frame
  // SERIALIZER side:
  reg [12:0] cb_rp;        // payload ring tail (drain cursor)
  reg [7:0]  dsc_i;        // descriptor cursor (== the ring tail between frames)
  reg [12:0] cur_coff;     // consumed descriptor: CHR offset
  reg [12:0] cur_cptr;     // consumed descriptor: payload start
  reg [12:0] cur_clen;     // derived length (next ptr - this ptr), <=255
  reg [7:0]  crun_k;       // payload byte cursor inside the current run
  reg [12:0] chr_emit;     // CHR payload bytes shipped by THIS frame (observability)
  // RETRANSMISSION FENCE.  On a rewind the drain is pulled back, so the frame's
  // slice is [tail, fresh0) ++ [fresh0, l_cb_end): re-sent bytes first, then the
  // ones that have never been emitted.  The fence records fresh0 so the walk can
  // ABANDON the rest of the re-sent part once it has spent RETX_WINDOW bytes and
  // JUMP straight to the fresh data.  Without it the cap has to be on the WHOLE
  // frame, and that is the bug that cost the field deploy twice over: a whole-
  // frame cap starves fresh bytes (v2.6a) or, if you remove it, a rewind plus a
  // CHR_WSTOP cut leaves an un-emitted remainder that makes the NEXT frame
  // bigger, which cuts again -- measured 7445 CHR bytes in one frame and 3996
  // dropped bytes at Mega-Man rate.  Capping only the RE-SENT part breaks that
  // loop while leaving fresh delivery exactly as unlimited as v2.5.
  reg        in_retx;      // the walk is still inside the re-sent region
  reg [12:0] l_fresh0;     // payload boundary: first byte never emitted
  reg [7:0]  l_dsc_fresh0; // descriptor boundary of the same point
  // ==================== v2.6 ACK-GATED DRAIN (at-least-once for 0x41) =======
  // WHY.  Until v2.5 the drain cursor advanced as the serializer emitted, so a
  // frame the renderer never applied took its CHR bytes with it: NT and palette
  // have pend/recovery, CHR had NOTHING.  Gate 2.4 hardware: Mega Man (mapper
  // 2, pure CHR-RAM) drew a permanently corrupt background with 79 overruns in
  // ~33 s -- every skipped frame silently deleted tiles, and the ONE-SHOT boot
  // dump never comes back.  The offline gate could not see it: bridge_sim is a
  // LOCKSTEP consumer that never loses a frame.
  //
  // MECHANISM (deliberately NOT the NT two-generation union -- the ring already
  // stores content in arrival order, which is all a retransmission needs):
  //   cb_cp/dsc_cp = COMMITTED TAIL, the oldest byte/descriptor not yet proven
  //     applied.  Ring occupancy is measured from IT, so the tap can never
  //     overwrite a byte that may still have to be re-sent.
  //   commit  = at the tick, when the ACK names a frame we still have in the
  //     8-deep publish history (lag <= 7 -- the commit window must reach PAST
  //     the recovery trigger at lag >= 3, or a renderer parked at lag 4 can
  //     never confirm anything) AND the applied chain from cb_cp is unbroken:
  //     cb_cp jumps to where that frame's drain STOPPED (which may be short of
  //     the frame's whole slice if a valve cut the walk -- retiring exactly what
  //     was shipped is what makes that safe).
  //   rewind  = at the tick of a RECOVERY frame (the very same
  //     recovery_now_w = lost_r | ack_skip that arms the NT pending union), the
  //     drain cursor is pulled BACK to the committed tail, so the frame
  //     re-emits every unconfirmed run.  Runs are re-emitted in ARRIVAL ORDER
  //     and the renderer's 0x41 handler is last-wins over an 8 KB shadow
  //     (nes_render.a65 nes_chr_handle_run: MVN into nes_chr_shadow, then
  //     reconvert the touched tiles), so a redundant re-emission is a no-op.
  //   chain_broken = the ONE correctness guard.  Committing to frame S is only
  //     safe if everything before S's slice was applied too.  A skip breaks
  //     that chain, and a frame published BEFORE the skip is not evidence any
  //     more (the classic bug shape: ack still names a pre-skip frame at the
  //     next tick and would retire bytes the renderer never saw).  So a skip
  //     sets chain_broken and only a frame flagged hist_cov can clear it when
  //     its own ACK arrives.
  //     hist_cov is the WEAK condition on purpose: "this frame's drain STARTED
  //     at the committed tail", NOT "this frame carried everything unconfirmed".
  //     A valve (CHR_WSTOP, or the RETX_CHUNK quota) can stop such a frame half
  //     way, and
  //     that is still sound, because the commit target is where its drain
  //     STOPPED (hist_cb), not the end of its slice: everything retired was in
  //     THAT frame, and the frame was applied.  Requiring the strong condition
  //     would deadlock the chain repair exactly when the valves are active.
  //   RETX CAP + ABANDON = the anti-starvation valve (field failure).  A rewind
  //     re-ships the OLDEST bytes first, so rewinding forever means the NEWEST
  //     writes never leave the ring -- the renderer keeps applying stale tiles
  //     and the screen never converges (black screen, +63 overruns/s).  Two
  //     things stop that: only RETX_CHUNK bytes of re-send fit in a frame (the
  //     quota fence jumps the drain to the fresh data when it runs out), and the
  //     window itself is TRIMMED once it grows past RETX_WINDOW -- so NEW data
  //     always gets the rest of the frame and the ring always gets its room
  //     back.
  //   INVARIANT: this module never drops a CHR byte that v2.5 would have
  //     accepted.  The tap accepts whenever EITHER view has room, and room_d IS
  //     the v2.5 test, so at-least-once is a layer on top of the old behaviour
  //     and never a regression of it.  (An earlier cut tried to express this as
  //     a "reclaim" that fired only when room_h failed while room_d held -- dead
  //     code by construction, because a rewind makes the two views identical.
  //     What actually keeps the ring roomy is the window TRIM at the tick.)
  //
  // BYTE IDENTITY.  chr_hold is `resync_en`: the byte-exact gate tb
  // ties resync_en=0 and never ACKs, so the tail follows the drain exactly as
  // before and every golden stays byte-identical.  On hardware, without a skip
  // the emission is also unchanged -- the rewind is the ONLY new emission path
  // and it needs recovery_now_w, which no golden and no lockstep sim produces.
  //
  // ACK TEAR (this fix is the FIRST consumer that INDEXES frame_ack_i, so the
  // property has to be written down): the renderer writes NES_FRAME_ACK as TWO
  // byte stores ($2BD4 lo, $2BD5 hi -> main.v nes_frame_ack[7:0]/[15:8]), so a
  // tick landing between them samples {old_hi, new_lo}.  That is safe here, and
  // not by luck of the index: the low byte is the NEW one, so the history index
  // (= ack[2:0]) is always the correct slot; a torn high byte only
  // displaces the value by a multiple of 256, which drives the lag far past the
  // depth-8 window (ack_win_q) and SUPPRESSES the commit for that tick.  Suppression is
  // the safe direction, and the hi byte only ever changes once every 256 frames.
  //
  // DEGRADATION, measured with a PERFECT lockstep consumer (tb_budget, the
  // advisor's own probe: ack = newest published seq, zero skips) at a sweep of
  // write rates, fix vs pre-fix drops:
  //   2120 (corpus plateau) 0/0 | 2500  0/0 | 3000  0/0 | 3700  0/0
  //   5000  19866/31938 | 6000  38916/71898 | 7000  41216/111820
  // i.e. nothing is dropped anywhere near a rate an NES can produce, and past
  // that cliff -- which is a property of the 8 KiB ring, at 2.4x the measured
  // per-frame maximum -- the fix still drops LESS than the design it replaces.
  // Never a corrupt run, never worse than before the fix.
  reg [12:0] cb_cp;        // committed payload tail
  reg [7:0]  dsc_cp;       // committed descriptor tail
  reg        chain_broken; // a skip invalidated the "everything before was applied" chain
  reg        big_skip;     // the last skip was bigger than a window can repair
  reg        l_chr_cov;    // frame being serialized starts AT the committed tail
  // Occupancy from the pointers themselves (exact under the modular wrap; no
  // counter to keep in sync with cb_rp's per-run JUMP in S_CR3).  The room
  // margin covers the 1-descriptor look-ahead dsc_i takes while emitting.
  // TWO occupancies: from the COMMITTED tail (what at-least-once needs) and from
  // the DRAIN cursor (what v2.5 needed).  Room from either one is enough to
  // accept a byte, and the tap ALSO bounds the window per cycle (the reclaim in
  // the CHR tap) -- the tick-rate trim alone lets cb_used_h alias between ticks.
  // HOLD FROM RESET, not from the first ACK.  Gating this on saw_ack left the
  // whole boot window running as plain v2.5 -- and the boot window is where the
  // ONE-SHOT screen-off dump lives, i.e. the exact loss this fix exists to stop
  // (measured: a frame skipped at seq 2 dropped its 64 B with saw_ack still 0).
  // Nothing can wedge in that window: before the first ACK nothing commits, so
  // the window only grows -- and growth is exactly what trim_n_q watches (the
  // no-commit branch of the trim), backed per cycle by the reclaim in the tap.
  // Both fire without needing an ACK, so the worst case in the boot window
  // degenerates to v2.5 instead of stalling.  The byte-exact gate is unaffected
  // (it ties resync_en=0).
  wire        chr_hold  = resync_en;
  // EVERY ring distance goes through an explicitly SIZED wire.  This is not
  // style: a modular difference inlined into a comparison against a localparam
  // is evaluated at the INTEGER width of that localparam, so the 13-bit wrap
  // never happens and a just-wrapped head reads as ~4.29e9.  Cost: three
  // CHR-RAM goldens started dropping bytes the moment these were folded into
  // the comparisons (battletoads/megaman1/ducktales2, caught by run_bridge).
  wire [12:0] cb_used_h  = cb_wp  - cb_cp;
  wire [7:0]  dsc_used_h = dsc_wp - dsc_cp;
  wire [12:0] cb_used_d  = cb_wp  - cb_rp;
  wire [7:0]  dsc_used_d = dsc_wp - dsc_i;
  // Room flags stay COMBINATIONAL, as they were before v2.6.  Registering them
  // (v2.6b) let the tap drop a byte on the exact boundary that the previous
  // cycle's view still called full -- one byte the v2.5 design accepted, and
  // worse, it dirtied the sticky cb_ovf, which is the ONLY field breadcrumb
  // this subsystem has.  The timing problem was never here: the critical path
  // was ack -> cb_rp (the history mux chain), which is registered below.
  wire        cb_room_h = (cb_used_h < (CB_SZ - 8)) && (dsc_used_h < (DSC_N - 4));
  wire        cb_room_d = (cb_used_d < (CB_SZ - 8)) && (dsc_used_d < (DSC_N - 4));
  // ACCEPT IF EITHER VIEW HAS ROOM.  room_d is what v2.5 used, so this can never
  // drop a byte v2.5 would have taken.  The reclaim that used to ride along here
  // is GONE: after a rewind cb_rp==cb_cp makes the two views identical, so it
  // was dead code exactly in the regime it was written for (proved by the
  // scatter gate: 2335 drops with reclaim "firing" 8 times).  The window trim at
  // the tick is what actually keeps the ring from filling now.
  wire        cb_room   = chr_hold ? (cb_room_h | cb_room_d) : cb_room_d;
  // reported occupancy (breadcrumb/tb only; nothing in the logic reads it)
  wire [12:0] cb_used   = chr_hold ? cb_used_h : cb_used_d;
  // A write opens a NEW run when there is none, when it is not the sequential
  // successor of the last one, when the open run already hit the 255-byte
  // ceiling of the len field, or when the offset WRAPPED to 0.
  //
  // PROTOCOL INVARIANT (do not regress): **off + len <= 0x2000, ALWAYS.**
  // `cb_next_off <= chr_off + 1` is 13 bits, so a write at 0x1FFF leaves
  // cb_next_off == 0x0000 and a following write at 0x0000 looked SEQUENTIAL --
  // the run then described bytes PAST the end of the 8 KiB CHR-RAM and the
  // renderer would apply the tail somewhere it must not (measured in the first
  // v2.4 goldens: battletoads frame 8 shipped `off=0x1FEF len=255`, 238 bytes
  // past the end; castlevania 121 bytes).  0x1FFF->0x0000 is the ONLY wrap
  // point of a monotonically incrementing 13-bit offset, so testing
  // `chr_off == 0` while a run is open covers the general case -- it is not a
  // special case of the len==255 slicing.  Mirrored EXACTLY by
  // bridge_sim/ppustate.py (which breaks the run when flat wraps to 0).
  wire        cb_newrun = ~cb_open | (chr_off != cb_next_off)
                        | (chr_off == 13'd0) | (cb_len == 8'd255);

  // OAM copy cursor + engine (decoupled from the serialize FSM -- v1.4b).
  reg [8:0]  cpy_i;
  reg        cpy_run;    // a live->frz copy is in flight (triggered by oam_freeze)

  // ============================================================ read plumbing
  // Live and frozen banks have SEPARATE read addresses (they can be active in
  // the same cycle: tap RMW on the live bank while the serializer scans the
  // frozen one).  All reads registered (M9K template).
  reg        ntdirty0_q, ntdirty1_q;
  reg [1:0]  ntpend_q;
  reg [7:0]  ciram_q, oam_q, oam_frz_q;
  reg [5:0]  pal_q;        // registered read of pal[pal_i] (S_PAL*_RD/_WR pair)
  reg [25:0] chrdsc_q;     // registered read of chrdsc[dsc_i]  (S_CR0/1, S_CR2/3)
  reg [7:0]  chrbuf_q;     // registered read of chrbuf[cb_rp]  (S_CRA/S_CRB)

  // nametable RMW tap phase
  reg        nt_rmw;
  reg [10:0] nt_rmw_addr;
  reg [7:0]  nt_rmw_data;

  wire [10:0] nt_tap_ra  = nt_we ? nt_addr : nt_rmw_addr;   // live-bank read (RMW check)
  wire [10:0] nt_scan_ra = nt_i[10:0];                      // frozen-bank read (scan)
  wire [10:0] ciram_ra   = run_start + run_k[7:0];          // serializer run data
  wire        ntdirty_live_q = live ? ntdirty1_q : ntdirty0_q;
  wire        ntdirty_frz_q  = live ? ntdirty0_q : ntdirty1_q;
  wire        nt_pend_eff    = lost_hold & ((pend_avf & ntpend_q[1])  | (pend_bvf & ntpend_q[0]));

  // ---- tick-time generation decisions (values are stable in the accepting
  // S_IDLE cycle; all consumers are registered there).  recov_seq tracks the
  // FIRST unconfirmed recovery: later recoveries deliver A|B WITHOUT
  // re-sealing (seal only when A is empty/just-confirmed), otherwise the ack
  // -- always behind while frames are big -- chases a recov_seq that re-arms
  // every tick and A never dies (the degenerate chain reborn; found by mode
  // "bootskip").  confirm_a is guarded by ~ack_skip: an ack that JUMPED past
  // a skipped recovery must not confirm it (the union it carried was never
  // applied); the skip arms a fresh recovery on this same tick instead.
  wire [15:0] ack_minus_recov = frame_ack_i - recov_seq;
  wire        confirm_a_w  = recov_active & ~ack_skip & ~ack_minus_recov[15];
  wire        caught_w     = (frame_ack_i == frame_seq_o) & ~ack_skip;
  wire        avf_next_w   = pend_a_valid & ~caught_w & ~confirm_a_w;
  wire        bvf_next_w   = pend_valid   & ~caught_w;
  wire        recovery_now_w = resync_en & saw_ack & (lost_r | ack_skip);
  wire        seal_now_w   = recovery_now_w & ~avf_next_w;
  // ---- v2.6 CHR commit / rewind decisions (all consumed at the tick) --------
  // The ACK may only retire CHR payload when (a) we still hold the named
  // frame's endpoint (lag<=7 = the 8-deep history; the window must reach PAST
  // the lag>=3 recovery trigger or a renderer parked at lag 4 never commits),
  // (b) no unprocessed skip is
  // sitting on this very tick, and (c) the applied chain from the committed
  // tail is intact -- or the named frame carried EVERYTHING unconfirmed by
  // itself (hist_cov), which repairs the chain.
  // ack_skip/chain_broken are REGISTERED one cycle after the jump they detect.
  // A tick landing in that very cycle would still read them clear and could
  // retire payload the skip just invalidated (a 1-in-280k-cycle race, but a
  // silent-data-loss one), so the CHR decision uses the COMBINATIONAL jump too.
  // Deliberately not retrofitted onto caught_w/confirm_a_w: the NT generation
  // algebra is hardware-validated as it stands and is not this fix's business.
  wire [15:0] ack_fwd_w    = frame_ack_i - ack_prev;   // sized: modular
  // PLAUSIBILITY IS A WINDOW, NOT A CONSTANT.  The first cut tested "moved
  // forward by <= 64 and sits within 64 of the published seq", and that constant
  // is a LATCH-UP: one ACK step past 64 frames (a renderer paused >1.07 s -- the
  // in-game shell, a savestate, staging a manual page, any SD operation, all of
  // which the NES core free-runs through) leaves ack_prev frozen, every later
  // ACK even further away, and the test can never become true again short of a
  // reset.  Measured on a 200-frame stall: ack_prev frozen, 240000 rejects, and
  // -- far worse than the CHR path -- ack_skip permanently dead, which disarms
  // the skip branch of the NT recovery algebra that IS hardware-validated.
  // The legal range of an ACK is [ack_prev, frame_seq_o]: it never runs backwards
  // and never gets ahead of what has been published.  Expressed that way the
  // window GROWS as frames are published, so any genuine value -- a 200-frame
  // catch-up included -- falls inside it, while the {old_hi,new_lo} tear of a
  // high-byte rollover reads as ~255 BACKWARDS and stays outside.
  wire [15:0] ack_span_w   = frame_seq_o - ack_prev;
  wire        ack_plaus_w  = (ack_fwd_w != 16'd0) & (ack_fwd_w <= ack_span_w);
  wire        ack_jump_w   = saw_ack & (frame_ack_i != ack_prev)
                           & ack_plaus_w & (ack_fwd_w >= 16'd2);
  // TICK INPUTS ARE PRE-REGISTERED (cost an STA run: -3.383 ns, TNS -132, path
  // nes_frame_ack[*] -> cb_rp[*]).  Done inline, the tick decision was an 8:1
  // history mux feeding a 13-bit subtract feeding a compare feeding the commit
  // AND-tree feeding a mux feeding ANOTHER subtract and compare -- and its
  // result had to reach cb_rp's D input in one CLK2 period.  All of it derives
  // from values that are STATIC between ticks, and ticks are ~280k cycles
  // apart, so a few cycles of pipelining costs nothing.  DO NOT INLINE AGAIN.
  //
  // ONE SAMPLE, ONE PIPELINE.  Every derived term must describe the SAME ack
  // value.  In the first cut hist_*_q was one register deep and commit_fwd_q /
  // the backlog tests were two, so an ack that moved at T-1 with a tick at T had
  // the forward guard validating the PREVIOUS history entry.  That is not
  // theoretical: hist_cb is NOT monotonic (a truncated recovery frame records a
  // stop behind its predecessor), so hist_cb[ack+1] < cb_cp <= hist_cb[ack] is
  // reachable, cb_cp would step BACKWARDS and the same tick's rewind would walk
  // descriptors the tap has already overwritten -- garbage off/len in an emitted
  // 0x41, i.e. silent CHR corruption.  So: the ack is sampled into a pipeline,
  // every consumer reads the stage that matches its own depth, and the tick only
  // commits while the whole pipeline agrees (ack_stable_w).
  reg [15:0] ack_s0, ack_s1, ack_s2;
  reg [12:0] hist_cb_q;
  reg [7:0]  hist_dsc_q;
  reg        hist_cov_q;
  reg        ack_win_q;      // (seq - ack) <= 7, i.e. inside the history depth
  reg        commit_fwd_q;   // the commit target lies inside [cb_cp, cb_wp]
  reg        trim_c_q;       // backlog > WINDOW, ASSUMING a commit
  reg        trim_n_q;       // ... assuming no commit
  reg        room_lo_q;      // occupancy already past half the ring
  // sized distances (see the note on the room flags -- never inline these into a
  // comparison against a localparam: the integer width kills the modular wrap)
  wire [12:0] cb_adv_w  = hist_cb_q - cb_cp;   // commit target, from the tail
  wire [12:0] bk_c_w    = cb_rp - hist_cb_q;   // backlog if we commit
  wire [12:0] bk_n_w    = cb_rp - cb_cp;       // backlog if we do not
  wire [7:0]  dk_c_w    = dsc_i - hist_dsc_q;  // ... in DESCRIPTORS
  wire [7:0]  dk_n_w    = dsc_i - dsc_cp;
  // NB: this block owns ack_s*/hist_*_q/… COMPLETELY, reset included.  Splitting
  // a reg's reset into the main FSM block and its updates into this one is two
  // always blocks driving one reg: iverilog simulates it happily and
  // quartus_map refuses it outright ("Can't resolve multiple constant drivers"),
  // which is the house gotcha this file already documents for debug taps.
  always @(posedge clk) if (rst) begin
    ack_s0<=16'd0; ack_s1<=16'd0; ack_s2<=16'd0;
    hist_cb_q<=13'd0; hist_dsc_q<=8'd0; hist_cov_q<=1'b0;
    ack_win_q<=1'b0; commit_fwd_q<=1'b0;
    trim_c_q<=1'b0; trim_n_q<=1'b0; room_lo_q<=1'b0;
  end else begin
    ack_s0       <= frame_ack_i;
    ack_s1       <= ack_s0;
    ack_s2       <= ack_s1;
    hist_cb_q    <= hist_cb [ack_s0[2:0]];     // aligned with ack_s1
    hist_dsc_q   <= hist_dsc[ack_s0[2:0]];
    hist_cov_q   <= hist_cov[ack_s0[2:0]];
    ack_win_q    <= ((frame_seq_o - ack_s1) <= 16'd7);   // aligned with ack_s2
    // FORWARD-ONLY guard: the tail can be forced ahead of a stored checkpoint
    // (window trim), so a stale entry could otherwise pull cb_cp BACKWARDS --
    // which reads as a nearly full ring and strangles the tap.
    commit_fwd_q <= (cb_adv_w <= cb_used_h);            // aligned with ack_s2
    // WINDOW TRIM tests, both branches precomputed.  The <= window test rejects
    // a WRAPPED distance: a rewind pulls cb_rp back and a commit aimed at a
    // frame published before it targets a point AHEAD of the cursor.
    trim_c_q     <= ((bk_c_w <= cb_used_h)  & (bk_c_w > RETX_WINDOW))
                  | ((dk_c_w <= dsc_used_h) & (dk_c_w > RETX_DSC));
    trim_n_q     <= ((bk_n_w <= cb_used_h)  & (bk_n_w > RETX_WINDOW))
                  | ((dk_n_w <= dsc_used_h) & (dk_n_w > RETX_DSC));
    // high-water on EITHER ring: past this the retransmission window is a
    // luxury the tap cannot afford, and giving it up is what keeps "never drops
    // a byte v2.5 would have taken" true by construction.
    room_lo_q    <= (cb_used_h > RING_HIWATER) | (dsc_used_h > DSC_HIWATER);
  end
  // the whole pipeline must describe one settled ack value
  wire        ack_stable_w = (frame_ack_i == ack_s0) & (ack_s0 == ack_s1)
                           & (ack_s1 == ack_s2);
  // ack_jump_w stays COMBINATIONAL: it is the one term that must see the ack
  // write in the very cycle a tick could land on it (the silent-data-loss race),
  // and it is only a subtract + compare.
  wire        chr_commit_w = chr_hold & saw_ack & ~ack_skip & ~ack_jump_w
                           & ack_stable_w & ack_win_q & commit_fwd_q
                           & (~chain_broken | hist_cov_q);
  wire [12:0] cb_cp_next   = chr_commit_w ? hist_cb_q  : cb_cp;
  wire [7:0]  dsc_cp_next  = chr_commit_w ? hist_dsc_q : dsc_cp;
  // TRIM: ALL-OR-NOTHING.  When the already-emitted unconfirmed span passes
  // RETX_WINDOW (in bytes OR in descriptors), or either ring is past its
  // high-water, the WHOLE window is given up -- the tail jumps to the drain
  // cursor.  It does not shave the span back to the limit; there is no cheap way
  // to land on a descriptor boundary mid-window, and abandoning the lot is the
  // safe direction (it degenerates to v2.5 for those bytes).  This is what keeps
  // the ring roomy at the tick; the per-cycle bound lives in the tap.
  wire        chr_trim_w   = chr_hold & ((chr_commit_w ? trim_c_q : trim_n_q)
                                         | room_lo_q);
  // A rewind is also pointless -- and actively harmful -- once the renderer is
  // further behind than the history is deep: past lag 7 we cannot even say which
  // frames it applied, the gap is far larger than one window, and re-sending
  // sinks a consumer that is already drowning (measured on a 100-frame pause:
  // 3938 bytes lost with the rewind versus 3792 without, i.e. WORSE than doing
  // nothing).  ack_win_q is exactly that horizon, and it is already registered.
  // NB this does NOT disarm the skip detection itself: ack_skip / chain_broken
  // still fire, so the NT recovery algebra keeps working on its own terms.
  // TODO (follow-up, AFTER hardware validation -- deliberately NOT in this
  // build): gate the rewind on a pure CAPACITY signal instead of inferring
  // capacity from the lag.  `ack_moved` = 1 FF + 1 AND: SET whenever ack_prev
  // updates (the renderer acknowledged something since the last frame), CLEARED
  // at every accepted tick, and folded in as `chr_rewind_w &= ack_moved`.  It
  // says exactly what the lag-based terms only approximate -- "the consumer made
  // progress during the last frame, so spending part of this one on a re-send is
  // affordable" -- and it is the term that would separate the two regimes the
  // current gate cannot: retransmission pays off at low CHR rates (lag scenario,
  // 100% recovered) and costs at saturating ones.  Bundle it with the mid-run
  // clamp fix documented above.
  wire        chr_rewind_w = recovery_now_w & ~chr_trim_w & ack_win_q & ~big_skip;
  // "this frame will start at the committed tail": either the drain is pulled
  // back to it (rewind) or it already sits there (nothing unconfirmed).  After a
  // give-up the tail IS the drain cursor, so that also counts.
  wire        chr_cov_w    = chr_rewind_w | chr_trim_w
                           | ((cb_rp == cb_cp_next) & (dsc_i == dsc_cp_next));

  // port-A write nets to shadow BRAMs.
  // Per ping-pong bank there is exactly ONE writer per cycle: the tap writes
  // the live bank, the clear pass writes the frozen bank -- the `live` muxes
  // below can never collide.
  reg [10:0] ntb_addr;  reg ntb_cwe; reg [7:0] ntb_cwr; reg nttap_dwe;
  // Clear-pass strobes AND addresses registered TOGETHER (do not regress): the
  // strobe fires one cycle after S_CLEAR schedules it, so using the (already
  // incremented) clr_* counters combinationally in the write mux cleared
  // addresses 1..N instead of 0..N-1.  On the NT bank the counter wrap
  // (2048[10:0]==0) hid it by luck; the SAME bug on the (now removed) CHR
  // dirty bitmap left index 512 off the array so TILE 0 WAS NEVER CLEARED and
  // a stale CMD_CHR_DIRTY of tile 0 was re-emitted forever (caught by the
  // megaman2/metroid gate FAILs right after the ping-pong refactor).  Kept
  // written down because the NT half of the pattern is still live.
  reg        ntclr_we;
  reg [10:0] ntclr_a;

  // bank-0/1 single write ports (mux by role)
  wire        nt0_we = live ? ntclr_we   : nttap_dwe;
  wire [10:0] nt0_wa = live ? ntclr_a    : ntb_addr;
  wire        nt0_wd = live ? 1'b0       : 1'b1;
  wire        nt1_we = live ? nttap_dwe  : ntclr_we;
  wire [10:0] nt1_wa = live ? ntb_addr   : ntclr_a;
  wire        nt1_wd = live ? 1'b1       : 1'b0;

  // mailbox write
  reg        mb_we; reg [12:0] mb_waddr; reg [7:0] mb_wdata;

  // ------------- helper: what byte to write this cycle (drives mb_* + xor) -----
  // We use a task-like inline via mb_put(byte): set mb_we/mb_waddr/mb_wdata, bump
  // wptr and xor.  Implemented by assigning in each state (non-blocking).
  // (kept inline below for clarity.)

  always @(posedge clk) begin
    // default strobes
    mb_we        <= 1'b0;
    ntb_cwe      <= 1'b0;
    nttap_dwe    <= 1'b0;
    ntclr_we     <= 1'b0;
    frame_done_o <= 1'b0;

    // registered reads (comb address; live/frozen banks read in parallel)
    ntdirty0_q  <= ntdirty0[live ? nt_scan_ra : nt_tap_ra];
    ntdirty1_q  <= ntdirty1[live ? nt_tap_ra  : nt_scan_ra];
    ntpend_q    <= ntpend[nt_scan_ra];
    ciram_q     <= ciram[ciram_ra];
    oam_q       <= oam[cpy_i[7:0]];        // copy source (S_CPY)
    oam_frz_q   <= oam_frz[oam_i[7:0]];    // serializer source (S_OAM*)
    pal_q       <= pal[pal_i[4:0]];        // register-array mux lands in a register
    // v2.4 CHR ring reads: simple-dual-port template (sync read here, sync
    // write in the tap below).  The serializer only ever reads the FROZEN
    // slice [dsc_i, l_dsc_end) / [cb_rp, l_cb_end) while the tap appends at
    // dsc_wp/cb_wp >= those ends, so read-during-write never lands on a byte
    // whose value is USED (the one look-ahead read at dsc_i==l_dsc_end is
    // discarded in favour of l_cb_end).
    chrdsc_q    <= chrdsc[dsc_i];
    chrbuf_q    <= chrbuf[cb_rp];

    if (rst) begin
      st<=S_IDLE; frame_seq_o<=0; frame_len_o<=0; status_o<=0;
      live<=0; tick_pend<=0; saw_ack<=0; lost_r<=0; force_full<=0;
      lost_hold<=0; seal_hold<=0; pend_valid<=0; pend_a_valid<=0;
      pend_avf<=0; pend_bvf<=0; pal_pend_a<=0; pal_pend_b<=0;
      recov_active<=0; recov_seq<=0; ack_prev<=0; ack_skip<=0;
      nt_cnt0<=0; nt_cnt1<=0; chr_any0<=0; chr_any1<=0;
      pal_dirty0<=0; pal_dirty1<=0; pal_cnt0<=0; pal_cnt1<=0; nt_rmw<=0;
      s0_valid<=0; s1_valid<=0; bc_bytes_last<=0; bc_frames<=0; bc_overruns<=0;
      dbg_pal_sum<=0; dbg_pal_wcnt<=0;
      l_split_cnt<=3'd0; l_split_hdr<=8'd0; spl_i<=3'd0;
      l_cspl_cnt<=3'd0; l_cspl_hdr<=8'd0; l_cspl_go<=1'b0; cspl_i<=3'd0;
      l_cwin_en<=1'b0; l_cwin<=64'd0; l_cwin_flags<=8'd0;
      l_cwsp_cnt<=3'd0; l_cwsp_hdr<=8'd0; l_cwsp_go<=1'b0; cwsp_i<=3'd0;
      cpy_run<=0; cpy_i<=9'd0;
      cb_wp<=13'd0; cb_rp<=13'd0; dsc_wp<=8'd0; dsc_i<=8'd0;
      cb_open<=1'b0; cb_len<=8'd0; cb_next_off<=13'd0; cb_ovf<=1'b0;
      l_dsc_end<=8'd0; l_cb_end<=13'd0; crun_k<=8'd0;
      cur_coff<=13'd0; cur_cptr<=13'd0; cur_clen<=13'd0;
      cb_cp<=13'd0; dsc_cp<=8'd0; chain_broken<=1'b0; l_chr_cov<=1'b0;
      in_retx<=1'b0; l_fresh0<=13'd0; l_dsc_fresh0<=8'd0; big_skip<=1'b0;
      // THE HISTORY MUST BE RESET, not just initialised.  rst pulses on the
      // SNES reset strobe / IGR, which zeroes cb_cp/cb_rp/frame_seq_o and the
      // renderer-written ack -- but the arrays kept PRE-RESET pointers, so the
      // first post-reset ACK indexed hist[1..7] with pointers from another life.
      // A small stale value passes the forward-only guard and drags cb_cp (and,
      // on a recovery, cb_rp) forward over descriptors that were never emitted,
      // deleting runs from the stream; a stale hist_cov=1 clears chain_broken on
      // top of it.
      for (rk=0; rk<8; rk=rk+1) begin
        hist_cb[rk]<=13'd0; hist_dsc[rk]<=8'd0; hist_cov[rk]<=1'b0;
      end
      chr_emit<=13'd0;
    end else begin
      if (frame_tick) tick_pend <= 1'b1;   // never drop a tick (cleared on accept)
      if (frame_ack_i != 16'd0) saw_ack <= 1'b1;
      // LOSS THRESHOLD IS >=3, NOT >=2 (cost a hardware iteration -- periodic
      // ~200ms stutter bursts, +12 overruns every ~3s): the renderer marks a
      // frame "seen" at the poll but only writes ACK at ITS vblank, so in
      // NORMAL operation the observed lag oscillates 1<->2 purely with the
      // (slowly drifting) phase between the bridge tick and the SNES vblank.
      // A >=2 trigger fires on that false positive; each full frame (~2.4KB)
      // lengthens the renderer's apply, which HOLDS the lag at 2 -> a CASCADE
      // of full frames until the phase slips back = the burst.  lag==2
      // sustained means "everything consumed, ACK one vblank behind" (no loss);
      // lag>=3 means a never-seen buffer is about to be overwritten = genuine
      // loss.  Kept CONTINUOUS while >=3 (no one-shot): under newest-wins the
      // renderer consumes the NEWEST frame, so during genuine loss every
      // published frame must be full or the consumed one rebuilds partial
      // state.  A real 1-frame stutter now costs 1-2 full frames, not a storm.
      // Validated by tb_handshake mode "phase" (adversarial ACK phase: zero
      // spurious fulls) + mode "slow" (sustained loss: all consumed are full).
      lost_r <= ((frame_seq_o - frame_ack_i) >= 16'd3);   // registered: tick uses FFs only
      // PLAUSIBILITY FILTER (v2.6c).  NES_FRAME_ACK is written as TWO byte
      // stores ($2BD4 lo, $2BD5 hi -> main.v nes_frame_ack[7:0]/[15:8]), so
      // every time the HIGH byte rolls over the bridge sees the intermediate
      // {old_hi, new_lo} for as long as the renderer takes between the two
      // stores.  Modulo 2^16 that intermediate reads as a jump of 65281, which
      // the bare ">=2" detector scored as a skip -- setting ack_skip AND
      // chain_broken twice, deterministically, every 256 frames (~4.3 s), each
      // one forcing a recovery + rewind nobody asked for.  A real ACK only ever
      // moves FORWARD by a few frames and never runs ahead of what has been
      // published, so anything outside that envelope is discarded whole: no
      // skip, no chain break, and ack_prev is NOT poisoned with the torn value.
      // ack_prev tracks every STABLE, in-range ACK -- including a huge catch-up
      // after a long pause.  A tear is a TRANSIENT and out-of-range value, so it
      // is rejected on both counts; a stall is a stable, in-range one, so it is
      // accepted and scored as the skip it really is.
      if ((frame_ack_i != ack_prev) && ack_stable_w && ack_plaus_w) begin
        if (saw_ack && (ack_fwd_w >= 16'd2)) begin
          ack_skip     <= 1'b1;
          // A jump bigger than the publish history is a hole this window cannot
          // repair (it holds ONE frame): re-sending into it just spends the
          // bandwidth the renderer needs for the CURRENT screen.  Measured on a
          // 100-frame blackout: retransmitting ended 3938 bytes wrong against
          // 3792 for doing nothing.  Latch it and leave CHR retransmission off
          // until something commits again -- the skip itself is still scored, so
          // the NT recovery algebra is untouched.
          big_skip     <= (ack_fwd_w > 16'd8);
          // the CHR chain of "everything before the committed tail was applied"
          // is broken from HERE (not at the tick): a frame published before the
          // skip must stop being usable as commit evidence immediately, even if
          // the ACK still names it on the next tick.
          chain_broken <= 1'b1;
        end
        ack_prev <= frame_ack_i;
      end
      // (generation-A confirmation is processed AT THE TICK, guarded by
      // ~ack_skip -- see confirm_a_w below.  An async drop here was wrong
      // twice: (1) HISTORY: a single-epoch drop discarded the pending dirtys
      // of frames serialized after the recovery ("background vanishes until
      // the next death") -- generation B now preserves those; (2) an ack for
      // a NORMAL frame past a SKIPPED recovery must not confirm A -- only
      // the tick sees ack_skip and can tell the difference.)

      // -------- port-A writes to shadow BRAMs (bank-muxed single ports) ------
      if (ntb_cwe)  ciram[ntb_addr]  <= ntb_cwr;
      if (nt0_we)   ntdirty0[nt0_wa] <= nt0_wd;
      if (nt1_we)   ntdirty1[nt1_wa] <= nt1_wd;
      if (oam_we)   oam[oam_addr]    <= oam_data;
      // -------- OAM freeze copy engine (v1.4b -- decoupled from the FSM) ------
      // oam_freeze pulses live oam[] -> oam_frz[] over 257 cycles (oam_q is the
      // 1-cycle-registered read of oam[cpy_i]; write lags by one, exactly the
      // old S_CPY pipeline).  In hardware the pulse lands MID-DISPLAY (scanline
      // 120) so the copy is DONE long before the frame tick -> the serializer
      // reads a stable, race-free oam_frz and never overlaps the vblank OAM-DMA.
      if (oam_freeze && !cpy_run) begin cpy_run<=1'b1; cpy_i<=9'd0; end
      else if (cpy_run) begin
        cpy_i <= cpy_i + 9'd1;
        if (cpy_i == 9'd256) cpy_run <= 1'b0;
      end
      if (cpy_run && cpy_i != 9'd0) oam_frz[cpy_i[7:0]-8'd1] <= oam_q;
      if (mb_we) begin
        if (mb_wbuf) mbox1[mb_waddr] <= mb_wdata; else mbox0[mb_waddr] <= mb_wdata;
      end

      // -------- palette fingerprint: INCREMENTAL sum-mod-256 on write ------
      // (synthesis #21: the 32-entry adder tree missed setup by -1.4ns; the
      // delta form is 1 sub + 1 add of 8 bits.  pal[] is deliberately an FF
      // array, so the combinational read below returns the OLD value in the
      // same cycle the tap write lands -- exact running sum of the 32 live
      // entries, same semantics as the tree.)
      if (pal_we) begin
        dbg_pal_sum  <= dbg_pal_sum + {2'b00, (pal_data & 6'h3F)}
                                    - {2'b00, pal[pal_eff]};
        dbg_pal_wcnt <= dbg_pal_wcnt + 8'd1;
      end

      // -------- tap accumulation (ALWAYS ON -- see SNAPSHOT note) --------
      begin
        if (pal_we) begin
          pal[pal_eff] <= pal_data & 6'h3F;
          if (live) begin
            pal_dirty1[pal_eff] <= 1'b1;
            if (!pal_dirty1[pal_eff]) pal_cnt1 <= pal_cnt1 + 6'd1;
          end else begin
            pal_dirty0[pal_eff] <= 1'b1;
            if (!pal_dirty0[pal_eff]) pal_cnt0 <= pal_cnt0 + 6'd1;
          end
        end
        // ---- PER-CYCLE WINDOW CLAMP (must NOT be inside `if (chr_we)`) ------
        // The test is on the WINDOW ITSELF (cb_rp - cb_cp), not on the room
        // flags.  Room-based detection is too late and self-defeating: once
        // cb_used_h has aliased past CB_SZ it reads SMALL, so cb_room_h says
        // "space available" with the ring full and the clamp never fires --
        // which is precisely how a tick-rate-only bound produced 1944 drops and
        // cb_ovf at 2800 B/frame while 2200 and 3200 were clean.  The window is
        // a quantity this module CONTROLS, so clamping it every cycle keeps
        // occupancy bounded.
        // BE HONEST ABOUT WHAT THIS DOES NOT DO: cb_used_h STILL ALIASES -- 2941
        // cycles of it at 2800 B/frame, 2214 at 3400, with samples as extreme as
        // used_h=208 while used_d=8191.  The clamp bounds the WINDOW, not that
        // derived difference.  It is safe because nothing load-bearing reads
        // cb_used_h: its only consumers are commit_fwd_q and trim_c_q/trim_n_q,
        // and BOTH fail in the safe direction when aliased -- commit_fwd_q
        // suppresses a commit (payload is retained, never retired early) and the
        // trim tests suppress a trim (the window is kept, and the per-cycle
        // clamp below catches it anyway).  The thing that must never alias is
        // the window itself, and that is what is clamped here.
        // AND IT HAS TO BE EVERY CYCLE, not every write: the window grows while
        // the SERIALIZER walks (cb_rp advances) and a real game is not writing
        // CHR at that moment, so a clamp gated on chr_we simply never runs
        // during the very phase that grows it -- measured: the same 1944 drops,
        // unchanged, until this moved out of the tap's if.
        // KNOWN AND ACCEPTED (documented rather than fixed, on purpose): this
        // pair can land MID-RUN.  The serializer takes a one-descriptor look
        // ahead in S_CR1, so while a run is being emitted cb_rp is inside run K
        // while dsc_i already points at K+1; clamping in that window stores a
        // tail that is not a run boundary (measured signature: rp=424 dsc_i=169
        // with the descriptor's ptr=425, 3-43 occurrences per multi-million-cycle
        // run at 3100/3400/3700 B/frame).  It SELF-CORRECTS at the next S_CR3,
        // which re-parks cb_rp from the descriptor, so the emitted stream stays
        // correct and every golden is unaffected.  The real cost is narrow: up
        // to 254 bytes of the run in flight drop out of the retransmission
        // window.  Nothing load-bearing reads the tail as a run boundary before
        // S_CR3 re-parks it.  The cheap fix (clamp to cur_cptr / dsc_i-1 while
        // the FSM is inside a run) is deferred to the same follow-up as
        // ack_moved below -- no logic changes this round.
        if (chr_hold & ((bk_n_w > RETX_WINDOW) | (dk_n_w > RETX_DSC)
                        | ~cb_room_h)) begin
          cb_cp        <= cb_rp;
          dsc_cp       <= dsc_i;
          chain_broken <= 1'b1;   // abandoned bytes stop being evidence
        end
        // -------- CHR-RAM tap (v2.4): append payload + open runs, 1 cycle -----
        // Everything below is a single-cycle event: at most ONE chrbuf write
        // (the data byte) and ONE chrdsc write (only when a run opens), on two
        // DIFFERENT arrays.  That is the whole point of storing `ptr` instead
        // of `len` in the descriptor -- see the CHR RING block.
        // A frame_tick is DEFERRED while chr_we is high (S_IDLE accept), so a
        // tap and a tick never race for the same cycle and the byte always
        // belongs to the CLOSING frame -- the same convention nt_rmw uses, and
        // the one the stimulus order (`4` lines before the `5` line) encodes.
        if (chr_we) begin
          if (live) chr_any1 <= 1'b1; else chr_any0 <= 1'b1;
          // PER-CYCLE BOUND ON THE RETRANSMISSION WINDOW.  The trim at the tick
          // runs ONCE PER FRAME while the tap runs every cycle, so between two
          // ticks cb_used_h can pass CB_SZ and ALIAS: measured used_h=1806 while
          // used_d=4400 for 131k cycles, i.e. cb_room_h reporting "space" with
          // the ring full, and the `bk <= cb_used_h` window test -- whose whole
          // job is to reject a wrapped distance -- being evaluated against the
          // already-wrapped value, so the trim was suppressed exactly when the
          // ring was fullest (a resonance band: clean at 2200/2500, 1944 drops
          // with cb_ovf at 2800, clean again at 3200).
          // The bound therefore has to live where the pressure is, in the tap.
          // This is the old "reclaim", and it is NOT dead code any more: since
          // the quota fence the drain cursor moves ahead of the committed tail
          // during the frame, so the two views are no longer identical after a
          // rewind.  Tail and descriptor move in ONE statement -- they must never
          // disagree.
          if (!cb_room) begin
            // Ring full: DROP the byte and close the run, so the loss shows up
            // as a missing run, never as a hole inside one.  Unreachable in the
            // 13-trace corpus (peak ~2250 of 4096); latched for the breadcrumb.
            cb_ovf  <= 1'b1;
            cb_open <= 1'b0;
          end else begin
            chrbuf[cb_wp] <= chr_data;
            cb_wp         <= cb_wp + 13'd1;
            if (cb_newrun) begin
              chrdsc[dsc_wp] <= {chr_off, cb_wp};   // ptr = PRE-increment head
              dsc_wp         <= dsc_wp + 8'd1;
              cb_len         <= 8'd1;
            end else cb_len <= cb_len + 8'd1;
            cb_open     <= 1'b1;
            cb_next_off <= chr_off + 13'd1;
          end
        end
        if (nt_we && !nt_rmw) begin
          nt_rmw<=1'b1; nt_rmw_addr<=nt_addr; nt_rmw_data<=nt_data;
        end else if (nt_rmw) begin
          ntb_addr<=nt_rmw_addr; ntb_cwr<=nt_rmw_data; ntb_cwe<=1'b1;
          nttap_dwe<=1'b1;
          if (!ntdirty_live_q) begin
            if (live) nt_cnt1 <= nt_cnt1 + 12'd1;
            else      nt_cnt0 <= nt_cnt0 + 12'd1;
          end
          nt_rmw<=1'b0;
        end
      end

      // ============================ serialize FSM =============================
      case (st)
        // tick accept: deferred while an nt RMW is in flight (a coincident tap
        // belongs to the CLOSING frame and must land in the pre-flip bank).
        // v2.4 adds !chr_we for the same reason on the CHR ring: accepting in
        // the same cycle a CHR byte lands would snapshot l_cb_end/l_dsc_end
        // BEFORE that byte's pointer bump, orphaning it between two frames.
        S_IDLE: if ((frame_tick | tick_pend) && !nt_rmw && !nt_we && !chr_we) begin
          tick_pend<= 1'b0;
          // v2.4: freeze the CHR payload/descriptor slice of the closing frame
          // and force the next write to open a fresh run (runs NEVER span
          // frames -- lockstep with bridge_sim begin_frame()).
          l_dsc_end<= dsc_wp;
          l_cb_end <= cb_wp;
          cb_open  <= 1'b0;
          // v2.6 ACK-GATED DRAIN: retire what the ACK proves applied, then --
          // on a recovery frame -- pull the drain cursor back to the (possibly
          // just advanced) committed tail so this frame re-emits every
          // unconfirmed run.  With chr_hold=0 (byte-exact gate tb, or before
          // the renderer's first ACK) the tail simply tracks the drain and the
          // whole mechanism is inert.
          l_chr_cov <= chr_cov_w;
          if (chr_hold) begin
            cb_cp  <= cb_cp_next;
            dsc_cp <= dsc_cp_next;
            if (chr_commit_w) begin chain_broken <= 1'b0; big_skip <= 1'b0; end
            if (chr_rewind_w) begin
              cb_rp        <= cb_cp_next;  // REWIND: re-emit the unconfirmed runs
              dsc_i        <= dsc_cp_next;
              l_fresh0     <= cb_rp;       // ... and remember where FRESH starts
              l_dsc_fresh0 <= dsc_i;
            end
            // THE FENCE IS PER-FRAME.  Latching in_retx only on a rewind left it
            // set when a frame ended before reaching its fence, and the NEXT
            // frame -- which had not rewound and whose fence was stale -- then
            // jumped the drain BACKWARDS onto descriptors the tap had already
            // recycled: garbage runs, and the whole CHR-RAM wrong (measured:
            // 8192 of 8192 bytes mismatched at Mega-Man rate).  Assign it
            // unconditionally at every tick.
            in_retx <= chr_rewind_w & (cb_rp != cb_cp_next);
            if (chr_trim_w) begin
              // TRIM: the retransmission window is over budget (or the ring is
              // past half full).  Give it up -- those bytes are abandoned, which
              // is exactly what v2.5 did with every unconfirmed byte -- so that
              // FRESH data keeps the whole frame and the tap keeps its room.
              // Written AFTER the commit assignment on purpose: cb_rp is at or
              // ahead of cb_cp_next whenever the trim tests pass, so the tail
              // only ever moves forward.
              cb_cp  <= cb_rp;
              dsc_cp <= dsc_i;
            end
          end else begin
            cb_cp  <= cb_rp;
            dsc_cp <= dsc_i;
          end
          live     <= ~live;                 // flip: filled bank becomes frozen
          new_seq  <= frame_seq_o + 16'd1;
          mb_wbuf  <= ~frame_seq_o[0];
          l_frame  <= snap_frame;
          l_ntarr  <= snap_ntarr;
          l_ntsel  <= snap_loopy_t[11:10];
          l_ppuctrl<= snap_ppuctrl; l_ppumask<= snap_ppumask;
          l_sx <= {snap_loopy_t[4:0],  snap_fine_x};
          l_sy <= {snap_loopy_t[9:5],  snap_loopy_t[14:12]};
          l_s0p<=snap_s0_present; l_s0b<=snap_s0_bank;
          l_s1p<=snap_s1_present; l_s1b<=snap_s1_bank;
          // v2.0a first-valid fix (device-proven on Tetris USA, MMC1-4K): the old
          // `s_valid & (bank!=prev)` SWALLOWED the first sample (present-rise
          // loaded prev without emitting) -- a game entering 4K mode after the
          // ~1-frame boot resync window (Tetris USA: 4K at frame 2, banks (0,0),
          // first real switch only at frame 801) never announced slot 1 -> PT1
          // rendered from the wrong half for ~13s.
          //   slot 1: presence-RISE is itself information (8K->4K; PT1 remaps
          //     even at bank 0) -> ~s1_valid counts as change, ALWAYS.
          //   slot 0: always present; its first-valid compares against the BOOT
          //     BASELINE bank 0 (power-on state of every v0 mapper register ==
          //     the renderer's boot full-CHR upload) -- emits only if the game
          //     already switched during frame 1.  An unconditional ~s0_valid
          //     would inject CHR_BANK(0,0) into frame 1 of EVERY game (goldens
          //     of no-switch games must stay byte-identical).
          // Lockstep: bridge_sim/encoder.py first-seen rule (same two cases).
          l_s0chg<= snap_s0_present & ((s0_valid ? (snap_s0_bank!=s0_prev)
                                                 : (snap_s0_bank!=8'd0))
                                        | (s0_valid & resync_en & (~saw_ack | lost_r | ack_skip)));
          l_s1chg<= snap_s1_present & (~s1_valid | (snap_s1_bank!=s1_prev)
                                        | (resync_en & (~saw_ack | lost_r | ack_skip)));
          // v1.3 CMD_SPLITS: derive sx/sy/ntsel per entry from the raw snapshot
          // (bit-slicing identical to CMD_REGS's l_sx/l_sy) -- pure wiring, no
          // arithmetic; done once/frame here so emission reads registers only.
          l_split_cnt <= snap_split_cnt;
          l_split_hdr <= {snap_split_ovf, 4'd0, snap_split_cnt};
          // v2.3 CMD_CHR_SPLITS: raw (scanline, bank) pairs -- nothing to derive.
          // The EMISSION GATE is latched here too so the FSM branch reads a
          // single register: cnt>=2 (a lone display-start entry is what every
          // non-splitting frame produces) AND !poison (a mid-frame 8K<->4K flip
          // invalidated the strips -- see nes_chrsplit_capture.v).
          l_cspl_cnt <= snap_cspl_cnt;
          l_cspl_hdr <= {snap_cspl_ovf, 4'd0, snap_cspl_cnt};
          l_cspl_go  <= (snap_cspl_cnt >= 3'd2) & ~snap_cspl_poison;
          // v2.5 CMD_CHR_STATE8/SPLITS8 (mapper 4): the 0x14 gate is the mapper
          // itself (unconditional while enabled), the 0x15 gate is cnt>=2 -- no
          // poison term, the window vector has no mode flip to invalidate it.
          l_cwin_en  <= snap_chr_win_en;
          l_cwin     <= snap_chr_win;
          l_cwin_flags <= snap_chr_win_flags;
          l_cwsp_cnt <= snap_cwin_cnt;
          l_cwsp_hdr <= {snap_cwin_ovf, 4'd0, snap_cwin_cnt};
          l_cwsp_go  <= snap_chr_win_en & (snap_cwin_cnt >= 3'd2);
          for (si=0; si<4; si=si+1) begin
            l_spl_sl[si] <= snap_spl_sl[si*8 +: 8];
            l_spl_sx[si] <= {snap_spl_t[si*15 +: 5],    snap_spl_fx[si*3 +: 3]};
            l_spl_sy[si] <= {snap_spl_t[si*15+5 +: 5],  snap_spl_t[si*15+12 +: 3]};
            l_spl_nt[si] <= snap_spl_t[si*15+10 +: 2];
            l_cspl_sl[si]<= snap_cspl_sl[si*8 +: 8];
            l_cspl_bk[si]<= snap_cspl_bank[si*8 +: 8];
            l_cwsp_sl[si]<= snap_cwin_sl[si*8 +: 8];
            l_cwsp_w[si] <= snap_cwin_win[si*64 +: 64];
          end
          force_full <= resync_en & ~saw_ack;          // BOOT only: true full
          lost_hold  <= recovery_now_w;                // recovery: delta union
          seal_hold  <= seal_now_w;                    // this recovery seals A
          if (ack_skip) ack_skip <= 1'b0;              // consumed by this tick
          // Epoch handling (see the GENERATION note + the wires above).
          // caught_w is gated on ~ack_skip: a skip whose catch-up lands
          // exactly on a caught-up tick still needs THIS recovery to carry
          // the union -- ungated, the recovery would be emitted EMPTY and
          // the union dropped = permanent loss of the skipped delta.
          // The clears must be visible to THIS frame's scan (an earlier
          // revision sampled the frame-latch PRE-clear: the union never
          // actually cleared and grew forever).
          pend_avf <= avf_next_w;
          pend_bvf <= bvf_next_w;
          if (caught_w) pend_valid <= 1'b0;
          if (caught_w | confirm_a_w) begin
            pend_a_valid <= 1'b0;                      // re-set by S_FINISH if sealing
            recov_active <= 1'b0;
          end
          // palette pending (plain registers -- same generation algebra,
          // done here since there is no sweep):
          if (seal_now_w) begin
            pal_pend_a <= (bvf_next_w ? pal_pend_b : 32'd0)
                        | (live ? pal_dirty1 : pal_dirty0);
            pal_pend_b <= 32'd0;
          end else begin
            pal_pend_a <= (avf_next_w ? pal_pend_a : 32'd0);
            pal_pend_b <= ((bvf_next_w ? pal_pend_b : 32'd0)
                        | (live ? pal_dirty1 : pal_dirty0));
          end
          // OAM was already frozen by the oam_freeze copy engine (mid-display in
          // hardware; at the tick in the tbs) -> go straight to serialize.
          chr_emit<=13'd0;                   // per-frame CHR shipped counter
          wptr<=0; xor_acc<=0; sub<=0; st<=S_SETUP;
        end

        S_SETUP: begin
          l_flags <= (snap_fb_hint ? FLAG_FORCED_BLANK : 8'd0)
                   | (((nt_cnt_frz > 12'd1536) | force_full) ? FLAG_FULL_REDRAW : 8'd0)
                   | (pal_present ? FLAG_PALETTE_PRESENT : 8'd0)
                   | ((l_s0chg|l_s1chg|chr_any_frz) ? FLAG_CHR_PRESENT : 8'd0)
                   | {2'd0, l_ntarr, 4'd0};   // flags[5:4] = NT arrangement (v1.1)
          st<=S_HDR; sub<=0;
        end

        // -------- FRAME_HDR: 01 seq(2) frame(2) flags(1) --------
        S_HDR: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=OP_FRAME_HDR;   xor_acc<=xor_acc^OP_FRAME_HDR;   end
            1: begin mb_wdata<=new_seq[7:0];   xor_acc<=xor_acc^new_seq[7:0];   end
            2: begin mb_wdata<=new_seq[15:8];  xor_acc<=xor_acc^new_seq[15:8];  end
            3: begin mb_wdata<=l_frame[7:0];   xor_acc<=xor_acc^l_frame[7:0];   end
            4: begin mb_wdata<=l_frame[15:8];  xor_acc<=xor_acc^l_frame[15:8];  end
            5: begin mb_wdata<=l_flags;        xor_acc<=xor_acc^l_flags;        end
          endcase
          if (sub==5) begin st<=S_REGS; sub<=0; end else sub<=sub+4'd1;
        end

        // -------- REGS: 10 sx(2) sy(2) ctrl mask ntsel --------
        S_REGS: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=OP_REGS;               xor_acc<=xor_acc^OP_REGS; end
            1: begin mb_wdata<=l_sx;                  xor_acc<=xor_acc^l_sx; end
            2: begin mb_wdata<=8'h00;                 xor_acc<=xor_acc^8'h00; end
            3: begin mb_wdata<=l_sy;                  xor_acc<=xor_acc^l_sy; end
            4: begin mb_wdata<=8'h00;                 xor_acc<=xor_acc^8'h00; end
            5: begin mb_wdata<=l_ppuctrl;             xor_acc<=xor_acc^l_ppuctrl; end
            6: begin mb_wdata<=l_ppumask;             xor_acc<=xor_acc^l_ppumask; end
            7: begin mb_wdata<={6'd0,l_ntsel};        xor_acc<=xor_acc^{6'd0,l_ntsel}; end
          endcase
          if (sub==7) begin st<=S_CHRST; sub<=0; end   // v2.2: CHR_STATE sempre
          else sub<=sub+4'd1;
        end

        // -------- CMD_CHR_STATE (v2.2): 12 s0_bank s1_bank {7'd0,s1p} --------
        // UNCONDITIONAL, fixed frame offset 14 -- absolute current bank state;
        // the renderer reconciles (see OP_CHR_STATE comment).  s1_bank is
        // gated by presence (l_s1p ? l_s1b : 0) so the injected-snapshot tb
        // and the real-mmu path (which drives chr_bank_1 raw even in 8K mode)
        // emit IDENTICAL bytes.  mb_wdata reads registers only (l_*).
        S_CHRST: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=OP_CHR_STATE;          xor_acc<=xor_acc^OP_CHR_STATE; end
            1: begin mb_wdata<=l_s0b;                 xor_acc<=xor_acc^l_s0b; end
            2: begin mb_wdata<=(l_s1p ? l_s1b : 8'd0);xor_acc<=xor_acc^(l_s1p ? l_s1b : 8'd0); end
            3: begin mb_wdata<={7'd0,l_s1p};          xor_acc<=xor_acc^{7'd0,l_s1p}; end
          endcase
          if (sub==3) begin
            // v2.5: in mapper 4 the chain goes to CMD_CHR_STATE8 and NEVER to
            // S_CSPL_OP -- that is hard rule (2) of the OP_CHR_STATE8 block,
            // enforced here by the ORDER of this if-chain (and, independently,
            // by the constant legacy tap in mmu.v, which keeps l_cspl_go at 0).
            if (l_cwin_en) begin st<=S_CWST; sub<=0; end  // v2.5: CMD_CHR_STATE8
            else if (l_cspl_go) st<=S_CSPL_OP;            // v2.3: emit CMD_CHR_SPLITS
            else if (l_split_cnt >= 3'd2) st<=S_SPL_OP;   // v1.3: emit CMD_SPLITS
            else if (pal_present) st<=S_PAL;
            else begin st<=S_NTA; nt_i<=0; nt_inrun<=1'b0; end
          end else sub<=sub+4'd1;
        end

        // -------- CMD_CHR_STATE8 (v2.5): 14 win0..win7 flags -----------------
        // UNCONDITIONAL in mapper 4, fixed frame offset 18 (right after the
        // 0x12, which keeps ITS fixed offset 14 so the parser never moves; the
        // renderer ignores the 0x12 once it has seen a 0x14).  mb_wdata reads
        // registers only: a 10-way case over slices of the SINGLE latched
        // register l_cwin (byte-select mux, same class as the 8-way S_REGS one)
        // -- SERIALIZER TIMING.
        S_CWST: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=OP_CHR_STATE8;      xor_acc<=xor_acc^OP_CHR_STATE8; end
            1: begin mb_wdata<=l_cwin[7:0];        xor_acc<=xor_acc^l_cwin[7:0]; end
            2: begin mb_wdata<=l_cwin[15:8];       xor_acc<=xor_acc^l_cwin[15:8]; end
            3: begin mb_wdata<=l_cwin[23:16];      xor_acc<=xor_acc^l_cwin[23:16]; end
            4: begin mb_wdata<=l_cwin[31:24];      xor_acc<=xor_acc^l_cwin[31:24]; end
            5: begin mb_wdata<=l_cwin[39:32];      xor_acc<=xor_acc^l_cwin[39:32]; end
            6: begin mb_wdata<=l_cwin[47:40];      xor_acc<=xor_acc^l_cwin[47:40]; end
            7: begin mb_wdata<=l_cwin[55:48];      xor_acc<=xor_acc^l_cwin[55:48]; end
            8: begin mb_wdata<=l_cwin[63:56];      xor_acc<=xor_acc^l_cwin[63:56]; end
            9: begin mb_wdata<=l_cwin_flags;       xor_acc<=xor_acc^l_cwin_flags; end
          endcase
          if (sub==4'd9) begin
            if (l_cwsp_go) st<=S_CWSP_OP;                 // v2.5: CMD_CHR_SPLITS8
            else if (l_split_cnt >= 3'd2) st<=S_SPL_OP;   // v1.3: CMD_SPLITS
            else if (pal_present) st<=S_PAL;
            else begin st<=S_NTA; nt_i<=0; nt_inrun<=1'b0; end
          end else sub<=sub+4'd1;
        end

        // -------- CMD_CHR_SPLITS8 (v2.5): 15 hdr cnt x [sl win0..win7] --------
        // Emitted only when l_cwsp_go (mapper 4 && cnt>=2), so a mapper-4 frame
        // with no mid-display window change walks straight from S_CWST to the
        // pre-v2.5 chain.  Same request/consume split as S_CSPL_*: S_CWSP_LD
        // consumes l_cwsp_*[cwsp_i] into cur_cw* (the 4-deep indexed read of a
        // 64-bit array stays OFF the mb_wdata/xor cone), S_CWSP_E emits the 9
        // bytes as a byte-select mux over those single registers.
        S_CWSP_OP: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<=OP_CHR_SPLITS8; xor_acc<=xor_acc^OP_CHR_SPLITS8;
          st<=S_CWSP_HDR;
        end
        S_CWSP_HDR: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<=l_cwsp_hdr; xor_acc<=xor_acc^l_cwsp_hdr;
          cwsp_i<=3'd0; sub<=4'd0; st<=S_CWSP_LD;
        end
        S_CWSP_LD: begin
          cur_cwsl<=l_cwsp_sl[cwsp_i[1:0]]; cur_cwin<=l_cwsp_w[cwsp_i[1:0]];
          st<=S_CWSP_E;
        end
        S_CWSP_E: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=cur_cwsl;        xor_acc<=xor_acc^cur_cwsl; end
            1: begin mb_wdata<=cur_cwin[7:0];   xor_acc<=xor_acc^cur_cwin[7:0]; end
            2: begin mb_wdata<=cur_cwin[15:8];  xor_acc<=xor_acc^cur_cwin[15:8]; end
            3: begin mb_wdata<=cur_cwin[23:16]; xor_acc<=xor_acc^cur_cwin[23:16]; end
            4: begin mb_wdata<=cur_cwin[31:24]; xor_acc<=xor_acc^cur_cwin[31:24]; end
            5: begin mb_wdata<=cur_cwin[39:32]; xor_acc<=xor_acc^cur_cwin[39:32]; end
            6: begin mb_wdata<=cur_cwin[47:40]; xor_acc<=xor_acc^cur_cwin[47:40]; end
            7: begin mb_wdata<=cur_cwin[55:48]; xor_acc<=xor_acc^cur_cwin[55:48]; end
            8: begin mb_wdata<=cur_cwin[63:56]; xor_acc<=xor_acc^cur_cwin[63:56]; end
          endcase
          if (sub==4'd8) begin
            // exit = the SAME decision S_CWST took after the last 0x14 byte
            if (cwsp_i+3'd1 >= l_cwsp_cnt) begin
              if (l_split_cnt >= 3'd2) st<=S_SPL_OP;
              else if (pal_present) st<=S_PAL;
              else begin st<=S_NTA; nt_i<=0; nt_inrun<=1'b0; end
            end else begin cwsp_i<=cwsp_i+3'd1; sub<=4'd0; st<=S_CWSP_LD; end
          end else sub<=sub+4'd1;
        end

        // -------- CMD_CHR_SPLITS (v2.3): 13 hdr(ovf|cnt) cnt x [sl bank] ------
        // Emitted only when l_cspl_go (cnt>=2 && !poison), so a frame with no
        // mid-display CHR bank change walks EXACTLY the pre-v2.3 state chain and
        // its bytes are unchanged.  Same request/consume split as S_SPL_*:
        // S_CSPL_LD consumes l_cspl_*[cspl_i] into cur_c* (indexed array read
        // kept OFF the mb_wdata/xor cone -- SERIALIZER TIMING), S_CSPL_E emits
        // the 2 bytes from those single registers.
        S_CSPL_OP: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<=OP_CHR_SPLITS; xor_acc<=xor_acc^OP_CHR_SPLITS;
          st<=S_CSPL_HDR;
        end
        S_CSPL_HDR: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<=l_cspl_hdr; xor_acc<=xor_acc^l_cspl_hdr;
          cspl_i<=3'd0; sub<=4'd0; st<=S_CSPL_LD;
        end
        S_CSPL_LD: begin
          cur_csl<=l_cspl_sl[cspl_i[1:0]]; cur_cbk<=l_cspl_bk[cspl_i[1:0]];
          st<=S_CSPL_E;
        end
        S_CSPL_E: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=cur_csl;         xor_acc<=xor_acc^cur_csl; end
            1: begin mb_wdata<=cur_cbk;         xor_acc<=xor_acc^cur_cbk; end
          endcase
          if (sub==4'd1) begin
            // exit = the SAME decision the pre-v2.3 chain took at the end of
            // S_CHRST (scroll splits -> palette -> nametable)
            if (cspl_i+3'd1 >= l_cspl_cnt) begin
              if (l_split_cnt >= 3'd2) st<=S_SPL_OP;
              else if (pal_present) st<=S_PAL;
              else begin st<=S_NTA; nt_i<=0; nt_inrun<=1'b0; end
            end else begin cspl_i<=cspl_i+3'd1; sub<=4'd0; st<=S_CSPL_LD; end
          end else sub<=sub+4'd1;
        end

        // -------- CMD_SPLITS (v1.3): 11 hdr(ovf|cnt) cnt x [sl sx sy ntsel] ----
        // Emitted only when cnt>=2 (guaranteed by the S_REGS branch).  Entries
        // walked one at a time: S_SPL_LD consumes l_spl_*[spl_i] into cur_*
        // (the idx mux stays OFF the mb_wdata/xor cone), S_SPL_E emits the 4
        // bytes from cur_* (single-register case mux, like S_HDR/S_REGS).
        S_SPL_OP: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<=OP_SPLITS; xor_acc<=xor_acc^OP_SPLITS;
          st<=S_SPL_HDR;
        end
        S_SPL_HDR: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<=l_split_hdr; xor_acc<=xor_acc^l_split_hdr;
          spl_i<=3'd0; sub<=4'd0; st<=S_SPL_LD;
        end
        S_SPL_LD: begin
          cur_sl<=l_spl_sl[spl_i[1:0]]; cur_sx<=l_spl_sx[spl_i[1:0]];
          cur_sy<=l_spl_sy[spl_i[1:0]]; cur_nt<=l_spl_nt[spl_i[1:0]];
          st<=S_SPL_E;
        end
        S_SPL_E: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=cur_sl;          xor_acc<=xor_acc^cur_sl; end
            1: begin mb_wdata<=cur_sx;          xor_acc<=xor_acc^cur_sx; end
            2: begin mb_wdata<=cur_sy;          xor_acc<=xor_acc^cur_sy; end
            3: begin mb_wdata<={6'd0,cur_nt};   xor_acc<=xor_acc^{6'd0,cur_nt}; end
          endcase
          if (sub==4'd3) begin
            if (spl_i+3'd1 >= l_split_cnt) begin
              if (pal_present) st<=S_PAL;
              else begin st<=S_NTA; nt_i<=0; nt_inrun<=1'b0; end
            end else begin spl_i<=spl_i+3'd1; sub<=4'd0; st<=S_SPL_LD; end
          end else sub<=sub+4'd1;
        end

        // -------- PALETTE / PALETTE_FULL (pipelined; see pal_cnt_r comment) ---
        // Byte stream identical to the monolithic version (opcode, [count],
        // then values/(idx,val) pairs ascending); only cycle count changed
        // (+1/dirty entry in LIST, 2/byte in FULL) -- the tb compares bytes.
        S_PAL: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          if (use_full) begin
            mb_wdata<=OP_PAL_FULL; xor_acc<=xor_acc^OP_PAL_FULL;
            pal_i<=6'd0; st<=S_PALF_RD;
          end else begin
            mb_wdata<=OP_PALETTE; xor_acc<=xor_acc^OP_PALETTE;
            st<=S_PALL_CNT;
          end
        end
        S_PALL_CNT: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<={2'd0,pal_cnt_frz}; xor_acc<=xor_acc^{2'd0,pal_cnt_frz};
          pal_i<=6'd0; st<=S_PALL_FIND;
        end
        S_PALL_FIND: begin
          if (pal_i>=6'd32) begin st<=S_NTA; nt_i<=0; nt_inrun<=1'b0; end
          else if (pal_dirty_frz[pal_i[4:0]]) begin
            mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
            mb_wdata<={3'd0,pal_i[4:0]}; xor_acc<=xor_acc^{3'd0,pal_i[4:0]};
            st<=S_PALL_RD;
          end else pal_i<=pal_i+6'd1;
        end
        S_PALL_RD: st<=S_PALL_WR;   // pal_q <= pal[pal_i] this edge
        S_PALL_WR: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<={2'd0,pal_q}; xor_acc<=xor_acc^{2'd0,pal_q};
          pal_i<=pal_i+6'd1; st<=S_PALL_FIND;
        end
        S_PALF_RD: st<=S_PALF_WR;   // pal_q <= pal[pal_i] this edge
        S_PALF_WR: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          mb_wdata<={2'd0,pal_q}; xor_acc<=xor_acc^{2'd0,pal_q};
          if (pal_i==6'd31) begin st<=S_NTA; nt_i<=0; nt_inrun<=1'b0; end
          else begin pal_i<=pal_i+6'd1; st<=S_PALF_RD; end
        end

        // -------- NT runs: streaming 2-state scan (S_NTA req, S_NTB consume) --
        S_NTA: begin
          if (nt_i >= NT_SIZE) begin
            if (nt_inrun) begin st<=S_NTHDR; sub<=0; end   // close final run
            else begin st<=S_CBANK; sub<=0; end
          end else st<=S_NTB;   // nt_rd_a==nt_i this cycle -> ntdirty_q next cycle
        end
        S_NTB: begin
          // pending union rewrite: pend accumulates the dirty of EVERY
          // serialized frame until confirmation drops the epoch (see the
          // PENDING note).  One write per visited offset; idempotent on
          // re-visits after a run close.
          // generation rewrite: SEAL (A := frz|A|B, B := 0) only when this
          // recovery seals (seal_hold); otherwise -- normal frames AND
          // recoveries with A still unconfirmed -- keep the generations
          // separate and accumulate in B.  Delivery (nt_pend_eff) is A|B
          // either way; delivery != rewrite.  The tick-latched pend_avf/bvf
          // materialize any pending invalidation into the stored bits.
          ntpend[nt_i[10:0]] <= seal_hold
            ? {ntdirty_frz_q | (pend_avf & ntpend_q[1]) | (pend_bvf & ntpend_q[0]), 1'b0}
            : {(pend_avf & ntpend_q[1]), (pend_bvf & ntpend_q[0]) | ntdirty_frz_q};
          if (!nt_inrun) begin
            if (ntdirty_frz_q | force_full | nt_pend_eff) begin
                                 run_start<=nt_i[10:0]; run_len<=9'd1; nt_inrun<=1'b1;
                                 nt_i<=nt_i+12'd1; st<=S_NTA; end
            else begin nt_i<=nt_i+12'd1; st<=S_NTA; end
          end else begin
            if ((ntdirty_frz_q | force_full | nt_pend_eff) && run_len<9'd255) begin
              run_len<=run_len+9'd1; nt_i<=nt_i+12'd1; st<=S_NTA;
            end else begin
              st<=S_NTHDR; sub<=0;   // close run (nt_i points at the breaking offset)
            end
          end
        end
        S_NTHDR: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=OP_NT_RUN;            xor_acc<=xor_acc^OP_NT_RUN; end
            1: begin mb_wdata<=run_start[7:0];       xor_acc<=xor_acc^run_start[7:0]; end
            2: begin mb_wdata<={5'd0,run_start[10:8]};xor_acc<=xor_acc^{5'd0,run_start[10:8]}; end
            3: begin mb_wdata<=run_len[7:0];         xor_acc<=xor_acc^run_len[7:0]; end
          endcase
          if (sub==3) begin st<=S_NTDA; run_k<=9'd0; end else sub<=sub+4'd1;
        end
        S_NTDA: st<=S_NTDB;   // nt_rd_a==run_start+run_k -> ciram_q next cycle
        S_NTDB: begin
          mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=ciram_q; wptr<=wptr+13'd1;
          xor_acc<=xor_acc^ciram_q;
          if (run_k+9'd1==run_len) begin nt_inrun<=1'b0; st<=S_NTA; end  // resume scan at nt_i
          else begin run_k<=run_k+9'd1; st<=S_NTDA; end
        end

        // -------- CHR_BANK (0..2): 40 slot bank --------
        S_CBANK: begin
          case (sub)
            0: if (l_s0chg) begin mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=OP_CHR_BANK;
                     xor_acc<=xor_acc^OP_CHR_BANK; wptr<=wptr+13'd1; sub<=4'd1; end
               else sub<=4'd3;
            1: begin mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=8'd0; xor_acc<=xor_acc^8'd0;
                     wptr<=wptr+13'd1; sub<=4'd2; end
            2: begin mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=l_s0b; xor_acc<=xor_acc^l_s0b;
                     wptr<=wptr+13'd1; sub<=4'd3; end
            3: if (l_s1chg) begin mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=OP_CHR_BANK;
                     xor_acc<=xor_acc^OP_CHR_BANK; wptr<=wptr+13'd1; sub<=4'd4; end
               else st<=S_CR0;
            4: begin mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=8'd1; xor_acc<=xor_acc^8'd1;
                     wptr<=wptr+13'd1; sub<=4'd5; end
            5: begin mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=l_s1b; xor_acc<=xor_acc^l_s1b;
                     wptr<=wptr+13'd1; st<=S_CR0; end
          endcase
        end

        // -------- CMD_CHR_RUN (v2.4): 41 off_lo off_hi len data[len] ----------
        // Walk the frozen descriptor slice [dsc_i, l_dsc_end) in ARRIVAL order
        // (NOT sorted/deduplicated as the old dirty-bitmap scan was: two writes
        // to the same byte in one frame ship twice and the renderer's last-wins
        // apply converges -- 99.6-99.9% of real writes are sequential, so the
        // cost is noise).  Emitted for CHR-RAM only: with CHR-ROM the tap never
        // fires, l_dsc_end==dsc_i, and this whole block is a single cycle that
        // adds ZERO bytes (byte-identity of every CHR-ROM golden).
        // The walk stops on the MAILBOX valve only (CHR_WSTOP: a buffer is
        // 8192 B and wptr wraps silently).  There is deliberately NO per-frame
        // byte cap on the drain -- see the RETX_WINDOW note: capping the drain
        // drops data with a perfectly healthy consumer.  Whatever CHR_WSTOP
        // holds back keeps its place in the ring (dsc_i / cb_rp stay where they
        // are and the next frame's slice starts there), so it is DEFERRED, never
        // lost.
        S_CR0: begin
          if (in_retx && (dsc_i == l_dsc_fresh0)) in_retx <= 1'b0;   // reached fresh
          if (in_retx && (chr_emit >= RETX_CHUNK)) begin
            // This frame's re-send quota is spent: jump to the fresh boundary so
            // the NEW bytes still get the frame.
            //
            // BE PRECISE ABOUT WHAT THIS IS -- it is BOUNDED EFFORT, not an
            // in-order trickle, and that is the intended design.  Advancing the
            // committed tail here ABANDONS the remainder of the window
            // ([quota_stop, l_fresh0)) in the same breath, so this frame's
            // contribution is deliberately NON-CONTIGUOUS: [old_cp, quota_stop)
            // then [l_fresh0, l_cb_end).  A later commit on this frame's seq
            // retires that gap too -- which is consistent, not a leak, because
            // the gap was already given up HERE, at the moment the quota ran
            // out.  Net effect: each unconfirmed byte gets AT MOST ONE extra
            // transmission attempt per loss episode.  The alternative (keep the
            // tail and re-send the same head next frame) is precisely the
            // head-of-line loop that blacked out the device, so it is rejected
            // on purpose.  Stay in S_CR0 one cycle so the registered chrdsc read
            // catches the cursor.
            dsc_i   <= l_dsc_fresh0;
            cb_rp   <= l_fresh0;
            cb_cp   <= cb_rp;      // bounded-effort: these attempts are spent
            dsc_cp  <= dsc_i;
            in_retx <= 1'b0;
          end
          else if ((dsc_i == l_dsc_end) || (wptr >= CHR_WSTOP))
            begin st<=S_OAMA; oam_i<=0; end
          else st<=S_CR1;          // chrdsc_ra==dsc_i now -> chrdsc_q next cycle
        end
        S_CR1: begin
          cur_coff <= chrdsc_q[25:13];
          cur_cptr <= chrdsc_q[12:0];
          dsc_i    <= dsc_i + 8'd1;   // look ahead at the NEXT descriptor
          st       <= S_CR2;
        end
        S_CR2: st<=S_CR3;            // chrdsc_ra==dsc_i(+1) -> chrdsc_q next cycle
        S_CR3: begin
          // LENGTH IS DERIVED, never stored: the next run starts where this one
          // ends, and the LAST run of the frame ends at the frozen head.  The
          // 12-bit subtraction wraps with the ring, so a run straddling the
          // wrap measures correctly.
          cur_clen <= ((dsc_i == l_dsc_end) ? l_cb_end : chrdsc_q[12:0]) - cur_cptr;
          cb_rp    <= cur_cptr;      // park the drain cursor (immune to orphans)
          sub      <= 4'd0;
          st       <= S_CRH;
        end
        S_CRH: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          case (sub)
            0: begin mb_wdata<=OP_CHR_RUN;             xor_acc<=xor_acc^OP_CHR_RUN; end
            1: begin mb_wdata<=cur_coff[7:0];          xor_acc<=xor_acc^cur_coff[7:0]; end
            2: begin mb_wdata<={3'd0,cur_coff[12:8]};  xor_acc<=xor_acc^{3'd0,cur_coff[12:8]}; end
            3: begin mb_wdata<=cur_clen[7:0];          xor_acc<=xor_acc^cur_clen[7:0]; end
          endcase
          if (sub==3) begin st<=S_CRA; crun_k<=8'd0; end else sub<=sub+4'd1;
        end
        S_CRA: st<=S_CRB;            // chrbuf_ra==cb_rp -> chrbuf_q next cycle
        S_CRB: begin
          mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=chrbuf_q; wptr<=wptr+13'd1;
          xor_acc<=xor_acc^chrbuf_q;
          cb_rp    <= cb_rp + 13'd1;
          chr_emit <= chr_emit + 13'd1;
          // TERMINATION IS NOT ALLOWED TO DEPEND ON AN INVARIANT (house rule
          // #1: never wedge).  cur_clen is 13 bits and crun_k is 8; the tap
          // guarantees 1 <= cur_clen <= 255, but if that ever broke (a
          // descriptor corrupted by a ring bug, cur_clen[12:8]!=0) the equality
          // alone would NEVER fire -- crun_k wraps at 256 and the FSM would
          // emit payload for ever, overrunning the mailbox and hanging the
          // frame.  crun_k==254 is the LAST legal value (len<=255 => last byte
          // is index 254), so this second exit is unreachable for every legal
          // run and is a hard ceiling for an illegal one.
          if (({5'd0,crun_k} + 13'd1 == cur_clen) || (crun_k == 8'd254)) st<=S_CR0;
          else begin crun_k<=crun_k+8'd1; st<=S_CRA; end
        end

        // -------- OAM: 50 + 256 bytes --------
        S_OAMA: begin
          if (oam_i==9'd0) begin
            mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=OP_OAM; xor_acc<=xor_acc^OP_OAM;
            wptr<=wptr+13'd1; oam_i<=9'd1; st<=S_OAMB;   // oam_rd_a==0 -> oam_q next cyc
          end
        end
        S_OAMB: begin
          mb_we<=1'b1; mb_waddr<=wptr; mb_wdata<=oam_frz_q; wptr<=wptr+13'd1;
          xor_acc<=xor_acc^oam_frz_q;
          if (oam_i==9'd256) begin st<=S_DONE; sub<=0; end
          else oam_i<=oam_i+9'd1;   // oam_rd_a follows oam_i
        end

        // -------- FRAME_DONE: F0 xor(body) --------
        S_DONE: begin
          mb_we<=1'b1; mb_waddr<=wptr; wptr<=wptr+13'd1;
          if (sub==0) begin mb_wdata<=OP_FRAME_DONE; sub<=4'd1; end
          else begin mb_wdata<=xor_acc; st<=S_CLEAR; clr_nt<=0; end
        end

        // -------- clear the NT dirty bitmap (the CHR one is gone in v2.4) -----
        S_CLEAR: begin
          // clears target the FROZEN banks (the nt0/1_w* muxes route them);
          // taps keep writing the live banks in parallel, race-free
          if (clr_nt < NT_SIZE)   begin ntclr_we<=1'b1;  ntclr_a<=clr_nt[10:0];   clr_nt<=clr_nt+12'd1; end
          else st<=S_FINISH;
        end

        // -------- publish + reset per-frame --------
        S_FINISH: begin
          frame_len_o   <= {3'd0, wptr};
          frame_seq_o   <= new_seq;
          bc_bytes_last <= {3'd0, wptr};
          bc_frames     <= bc_frames + 16'd1;
          if ((frame_seq_o - frame_ack_i) > 16'd1) begin
            bc_overruns <= bc_overruns + 16'd1; status_o[0] <= 1'b1;
          end
          // v2.4: sticky "a CHR-RAM byte was dropped because the capture ring
          // was full".  Unreachable in the corpus; a device seeing this bit set
          // is telling you the ring sizing assumption broke, not that the
          // renderer is behind (that is bit 0).
          if (cb_ovf) status_o[1] <= 1'b1;
          // v2.6: record WHERE THIS FRAME'S CHR DRAIN STOPPED, indexed by the
          // seq the renderer will ACK with.  dsc_i/cb_rp are exactly that
          // point -- l_dsc_end/l_cb_end when the whole slice was shipped, or
          // wherever a valve (CHR_WSTOP, or the RETX_CHUNK quota) cut it short --
          // retiring exactly what WAS shipped is what makes a truncated frame
          // safe to commit to.  hist_cov records whether the frame's drain
          // STARTED at the committed tail (the weak condition; see the
          // chain_broken note), which is what lets its ACK repair a broken chain.
          hist_cb [new_seq[2:0]] <= cb_rp;
          hist_dsc[new_seq[2:0]] <= dsc_i;
          hist_cov[new_seq[2:0]] <= l_chr_cov;
          if (l_s0p) begin s0_prev<=l_s0b; s0_valid<=1'b1; end else s0_valid<=1'b0;
          if (l_s1p) begin s1_prev<=l_s1b; s1_valid<=1'b1; end else s1_valid<=1'b0;
          if (live) begin nt_cnt0<=0; chr_any0<=0; pal_dirty0<=0; pal_cnt0<=0; end
          else      begin nt_cnt1<=0; chr_any1<=0; pal_dirty1<=0; pal_cnt1<=0; end
          pend_valid <= 1'b1;   // pending arrays fully rewritten by this scan
          if (lost_hold & seal_hold) begin
            // this recovery SEALED generation A: everything it delivered now
            // lives in A -- arm the confirmation watch (ack >= recov_seq).
            // Non-sealing recoveries keep recov_seq at the FIRST unconfirmed
            // recovery (see the tick-wire note).
            pend_a_valid <= 1'b1;
            recov_active <= 1'b1;
            recov_seq    <= new_seq;
          end
          frame_done_o<=1'b1; st<=S_IDLE;
        end

        default: st<=S_IDLE;
      endcase
    end
  end

  // ============================================================ window read
  // One registered read PER mailbox buffer + combinational mux AFTER the
  // registers.  Quartus M9K inference REQUIRES the RAM read itself to be
  // synchronous: the previous `win_data <= sel ? mbox1[a] : mbox0[a]` (mux of
  // two async array reads in front of one register) uninfers BOTH arrays
  // (quartus_map Info 276007) and the 2x8KiB fall back to ~131k FFs (Error
  // 276003).  Each block below is a clean simple-dual-port template (sync
  // write in the FSM block, sync read here).  Total window latency is
  // UNCHANGED: win_data valid 1 cycle after win_addr (buf_sel_i is stable for
  // the whole drain, so the post-register mux adds no cycle).  The two buffers
  // never read the address being written (renderer reads the completed buffer;
  // the FSM writes the other one) => read-during-write don't-care.
  reg [7:0] mbox0_q, mbox1_q;
  always @(posedge clk) begin
    mbox0_q <= mbox0[win_addr];
    mbox1_q <= mbox1[win_addr];
  end
  assign win_data = buf_sel_i ? mbox1_q : mbox0_q;

  // ============================================================ joypad
  // joy_strobe/joy_clock are LEVELS (in hardware: nes.v's ce-registered
  // tapJ_strobe/tapJ_clock; a $4016/$4017 read holds joypad_clock high for a
  // whole CPU cycle = 3 ce ticks = dozens of CLK2 cycles).  The controller
  // model shifts on the FALLING edge of the clock level -- one shift per read
  // -- exactly like the canonical fpganes top-level consumer (NES_Nexys4:
  // `if (!joypad_clock[0] && last_joypad_clock[0]) shift`).  Level-shifting
  // (the first version of this block) shifted every CLK2 cycle the level was
  // high = dozens of shifts per read: latent functional bug, never exercised
  // by the byte-exact gate (the tb ties joypad off).  The core samples
  // joypad_data DURING the high level (pre-shift bit); the falling edge then
  // advances to the next bit -- correct NES serial semantics.
  // BIT ORDER (cost a hardware iteration -- "erratic controls"): the shift is
  // MSB-FIRST.  The renderer packs A=bit7, B=bit6, Select=bit5, Start=bit4,
  // Up=bit3, Down=bit2, Left=bit1, Right=bit0 ("classic serial order",
  // nes_transport_device.a65) and the NES reads A on the FIRST $4016 read --
  // so bit7 must come out first.  The original LSB-first shift returned the
  // byte TRANSPOSED (Right read as A, Left as B, Up as Start...): the game
  // responded, but to the wrong buttons.  Post-shift fill = 1s, so reads 9+
  // return 1 (real NES controller convention; games may depend on it).
  // Validated end-to-end by tb/run_joypad.sh (real 6502 micro-ROM doing
  // strobe + 10 reads against the full core + bridge).
  // Byte-tearing note: all 8 buttons live in CTRL_P1's LOW byte, written
  // atomically by ONE 8-bit bus write ($2BDA); the high byte is zero padding
  // -- cross-frame button tearing is impossible by construction.
  reg [7:0] sr1, sr2;
  reg [1:0] joy_clock_prev;
  always @(posedge clk) begin
    if (rst) begin sr1<=8'hFF; sr2<=8'hFF; joy_clock_prev<=2'b00; end
    else begin
      joy_clock_prev <= joy_clock;
      if (joy_strobe) begin sr1<=ctrl_p1_i[7:0]; sr2<=ctrl_p2_i[7:0]; end
      else begin
        if (!joy_clock[0] && joy_clock_prev[0]) sr1<={sr1[6:0],1'b1};
        if (!joy_clock[1] && joy_clock_prev[1]) sr2<={sr2[6:0],1'b1};
      end
    end
  end
  assign joypad_data_o = {sr2[7], sr1[7]};

endmodule
