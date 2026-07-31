`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// nes_wrap -- sd2snes plumbing adapter around the vendored fpganes `NES` core.
//
// Role: everything the CONTRACT (NES-CORE-CONTRACT.md) says main.v expects from a
// core's top module (ROM_BUS_* PSRAM handshake, a breadcrumb/config register bus
// shaped like the SGB core's reg_group/index/value/invmask/read bus, a reset
// input) lives here, translated into what the raw `NES` module (nes.v) actually
// exposes (a linear 22-bit memory bus that is only valid while `ce` is held low,
// plus a `ce` clock-enable that the *consumer* is responsible for pulsing -- see
// NES-FPGANES-ANALYSIS.md SS2.1/SS7.1).
//
// THE HANDSHAKE (the one thing this file exists to get right)
// -------------------------------------------------------------------------------
//   ANALYSIS SS7.1 proves every register in the vendored core is gated by a single
//   `ce`, so holding `ce` low for extra CLK2 cycles is *always* safe -- there is
//   no free-running sub-domain to fall out of sync. CONTRACT SS3.2 shows the PSRAM
//   arbiter's request/ready latency is NOT fixed (7+1+1=9 CLK2 cycles typical,
//   ~18 worst case if a same-cycle SNES/MCU access is already in flight) -- so
//   `ce` must be generated *on demand* from the arbiter's own busy/done signal,
//   never from a fixed-ratio NCO (decision #1 in the task brief; ANALYSIS SS3.3
//   risk #1 is exactly this).
//
//   Every core "tick" is one lap of a small FSM (S_PACE added by the SYSCLK
//   pacer -- see "PACER" below):
//
//     S_IDLE  --(need_read)-->  [assert ROM_BUS_RRQ]            --> S_REQ
//     S_IDLE  --(need_write)--> [assert ROM_BUS_WRQ]            --> S_REQ
//     S_IDLE  --(no request)-------------------------------------> S_PACE
//     S_REQ   --(RDY observed low = arbiter took it)------------> S_WAIT
//     S_WAIT  --(!ROM_BUS_RDY)--> stay in S_WAIT (stretch: PSRAM/arbiter busy)
//     S_WAIT  --(ROM_BUS_RDY)--> latch RDDATA -------------------> S_PACE
//     S_PACE  --(pace credit available)--> [pulse ce for 1 CLK2] --> S_SETTLE
//     S_SETTLE ---------------------------------------------------> S_IDLE
//
//   S_SETTLE exists ONLY to avoid a same-edge race: `ce` pulsed during cycle T
//   causes the `NES` module's registers (and therefore its combinational
//   memory_addr/read_cpu/read_ppu/write outputs) to update at the T->T+1 edge.
//   If the FSM re-evaluated need_read/need_write at that same edge it would
//   still be reading the PRE-update (stale, already-serviced) request. One dead
//   SETTLE cycle guarantees S_IDLE always evaluates already-fresh (post
//   most-recent-ce) outputs.
//
//   PPU CHR-fetch turnaround (ANALYSIS SS2.3/SS7.2) needs the data available by
//   the very next `ce` -- there is zero slack. Because `ce` is *never* pulsed
//   while a PSRAM transaction is outstanding (S_WAIT and S_PACE both precede
//   it), and RDDATA is latched into mem_rdata_ppu_r (or _cpu_r -- one latch
//   PER CONSUMER, see the declaration comment) on the S_WAIT->S_PACE edge,
//   the byte is guaranteed stable for >= 2 full CLK2 cycles before the ce
//   that consumes it -- satisfying that zero-slack requirement regardless of
//   how long the wait actually took.
//
// PACER (real-time rate control + the enabler for the multicycle SDC)
// -------------------------------------------------------------------------------
//   Without a pacer the core free-runs at "one ce per FSM lap" (~4-12 CLK2 =
//   7-21 MHz dot clock -- way too fast for a real NES, and it also leaves no
//   guaranteed spacing between ce pulses for timing closure). SNES_SYSCLK
//   (~21.477272 MHz NTSC, the SNES master clock pin already wired into main.v)
//   is synchronized into the CLK2 domain by a 3-FF shifter + rising-edge
//   detector (same pattern as sgb_icd2.v's clk_sys_r / dac.v's sysclk_sreg),
//   and a saturating credit counter grants AT MOST one `ce` per 4 detected
//   SYSCLK rising edges: 21.477272/4 = 5.369318 MHz = the exact NTSC NES dot
//   clock. The PSRAM access OVERLAPS the pacing window: S_IDLE fires the
//   request immediately, the memory usually resolves in ~9-12 CLK2 (< the
//   ~15.65-cycle pacing period), and S_PACE then absorbs the remainder -- so
//   PSRAM latency is hidden and the average ce rate equals the dot clock
//   whenever memory resolves inside the window (a stall longer than the window
//   stretches that one ce, which ANALYSIS SS7.1 proves safe; lost credit is NOT
//   accumulated -- see below -- so there is never a catch-up burst).
//
//   Credit counter: pace_ctr saturates at 4 (increment on detected SYSCLK edge
//   while < 4) and RESETS TO 0 on the edge that issues `ce` (an edge landing on
//   that same cycle is deliberately dropped). Saturation + reset-to-0 is what
//   guarantees a MINIMUM SPACING between consecutive ce pulses -- an
//   accumulate-and-subtract scheme would burst back-to-back ce's after a long
//   PSRAM stall, destroying the spacing guarantee the SDC multicycle
//   constraints (main.sdc) are built on.
//
//   GUARANTEED MINIMUM ce-to-ce SPACING (worst case, derived):
//     - SYSCLK period = 46.56ns = 3.911 CLK2 cycles (84/21.477272).
//     - Detected-edge positions are quantized to the CLK2 grid, so the gap
//       between consecutive detected edges is 3 or 4 CLK2 cycles (avg 3.911),
//       and the cumulative gap over 3 consecutive intervals is >= floor(3 x
//       3.911) = 11 cycles.
//     - Worst case: ce high during cycle C (pace_ctr reset at the edge into C);
//       first counted edge during C itself (increments at C->C+1); 4th edge as
//       early as cycle C+11 -> pace_ctr==4 registered at (C+11)->(C+12);
//       S_PACE sees it during C+12; ce_pulse_r set at (C+12)->(C+13) -> next ce
//       during cycle C+13.
//     => MINIMUM 13 CLK2 CYCLES between consecutive ce pulses (typical 15-16;
//        average exactly 15.646 when memory never out-stalls the window). The
//        multicycle values in main.sdc (setup 4) leave a 3x+ margin under this
//        floor -- if this pacer is ever changed, re-derive BOTH numbers.
//
// Address map (ANALYSIS SS2.4, SS7.4 item 3) -- identical to the vendored
// GameLoader's convention, so no offset needs to be re-derived by the loader:
//   PRG      0x000000-0x0FFFFF (<=1MB)
//   CHR      0x200000-0x2FFFFF (<=1MB)
//   CIRAM    0x300000-0x3007FF (2KB nametable RAM)
//   CPU-RAM  0x380000-0x3807FF (2KB CPU work RAM)
//   CART-RAM 0x3C0000-0x3C1FFF (8KB PRG-RAM, when the mapper uses it)
// ROM_BUS_ADDR is 24 bits (main.v/CONTRACT SS1); the core's own address is 22
// bits, so ROM_BUS_ADDR = {2'b00, memory_addr} keeps everything in the PSRAM's
// first 4MB quadrant, leaving the upper 12MB free for the MCU/SD-DMA side.
//
// Breadcrumb (CONTRACT SS9 item 7): a `reg_group`/`reg_index`/`reg_value`/
// `reg_invmask`/`reg_we`/`reg_read` bus shaped exactly like sgb.v's config_r[]
// (mcu_cmd.v's CONFIG_READ/CONFIG_WRITE opcodes 0xf9/0xfa), reusing group 8'h04
// (SGB already claims 8'h03; only one core is ever loaded at a time, but keeping
// them distinct costs nothing and avoids confusion if that ever changes). Holds
// PC/opcode/flags/SP snapshotted every time the CPU begins fetching a new
// instruction (State==0, post-reset) -- for the iverilog testbench this is
// secondary, since the testbench reaches the same information directly via
// hierarchical references into the `core` instance below; it matters for the
// eventual hardware bring-up (MCU-side breadcrumb reads), not for this gate.
//////////////////////////////////////////////////////////////////////////////////

module nes_wrap(
  input         RST,
  output        CPU_RST,
  input         CLK,
  input         SNES_SYSCLK,  // ~21.477272 MHz async; pacer reference (see header)

  // ROM interface -- identical shape to sgb.v's ROM_BUS_* (main.v side unchanged)
  input         ROM_BUS_RDY,
  output reg    ROM_BUS_RRQ,
  output reg    ROM_BUS_WRQ,
  output        ROM_BUS_WORD,
  output [23:0] ROM_BUS_ADDR,
  input  [7:0]  ROM_BUS_RDDATA,
  output [7:0]  ROM_BUS_WRDATA,
  output        ROM_FREE_SLOT,

  // Audio interface -- same shape as sgb.v's APU_DAT/APU_CLK_EDGE (dac.v side
  // unchanged); Phase -1 has no audio requirement (CONTRACT SS6/SS9), wired for
  // completeness only -- not validated.
  output [19:0] APU_DAT,
  output        APU_CLK_EDGE,
  // DAC-internal liveness (computed inside dac.v, same CLK2 domain -- no CDC;
  // published as group 0x04 idx16/17, firmware NDBG v3 +22/+23):
  //   DAC_DBG_CIC = max-hold |sgb-CIC out ch0| [9:2], sticky, cleared by the
  //                 firmware dac_reset pulse (every load)
  //   DAC_DBG_I2S = rolling count of LRCK rising edges (I2S clock alive)
  input  [7:0]  DAC_DBG_CIC,
  input  [7:0]  DAC_DBG_I2S,
  //   DAC_DBG_DAT = max-hold |vol_sample_sat|[15:8] (the exact word the I2S
  //                 shifter reloads) -- group 0x04 idx18, NDBG v3 +24
  input  [7:0]  DAC_DBG_DAT,
  //   DAC_LRCK_MON = raw LRCK from dac.v; the wrap counts rising edges per
  //                  NES frame (latched at frame_tick) -> idx20/21 (NDBG v3
  //                  +26/+27, lo/hi).  Expected ~2731 at 164.06kHz vs the
  //                  60.10Hz NES frame; any other value = wrong DAC clock.
  input         DAC_LRCK_MON,

  // Mapper/iNES configuration. Phase -1 only exercises mapper 0 (nestest.nes),
  // so this is intentionally the simplest thing that could work: the full
  // 32-bit `mapper_flags` the vendored MultiMapper expects, assembled by the
  // caller (today: the testbench; eventually: main.v/mcu_cmd.v, which only has
  // a 4-bit MAPPER register and 24-bit ROM_MASK/SAVERAM_MASK to work with --
  // deriving prg_size/chr_size bank-count classes from those masks, and mapper
  // numbers >15 from CHIPFEAT, is explicitly deferred; see report).
  input  [31:0] mapper_flags_in,

  // Breadcrumb / config register bus -- shaped exactly like sgb.v's reg_*_in /
  // config_data_out (mcu_cmd.v CONFIG_READ/CONFIG_WRITE, group 8'h04 for this
  // core).
  input  [7:0]  reg_group_in,
  input  [7:0]  reg_index_in,
  input  [7:0]  reg_value_in,
  input  [7:0]  reg_invmask_in,
  input         reg_we_in,
  input  [7:0]  reg_read_in,
  output [7:0]  config_data_out,

  // ---- Video bridge: SNES-facing mailbox window ($6000-$7FFF) + control block ----
  // main.v maps NESBOX_ADDR to SNES_ADDR[12:0] under nesbox_enable, muxes
  // NESBOX_DATA into the SNES read path, and mirrors the control block into the
  // snescmd window at $2BD0-$2BDF (spec SS3.3 named $2A00 -- RELOCATED: it
  // collided with the fork's SNES<->MCU boot protocol; see address.v map).
  input  [12:0] NESBOX_ADDR,
  output [7:0]  NESBOX_DATA,
  output [15:0] NESBOX_FRAME_SEQ,   // bridge-written
  output [15:0] NESBOX_FRAME_LEN,   // bridge-written
  output [7:0]  NESBOX_STATUS,      // bridge-written
  input  [15:0] NESBOX_FRAME_ACK,   // renderer-written
  input         NESBOX_BUF_SEL,     // renderer-written
  input  [15:0] NESBOX_CTRL_P1,     // renderer-written joypad
  input  [15:0] NESBOX_CTRL_P2,
  // Boot handshake (control block +0x0E, NES_GO -- main.v): 0 = core NES held
  // in reset (renderer uploads CHR from PSRAM $50/$60 -> VRAM with ZERO core
  // PSRAM traffic -- requests AND ce are gated by core_hold_r below); the
  // renderer writes 1 after the upload and the core boots.  Cleared by SNES
  // reset, so every load/IGR redoes the handshake.  Testbenches tie it 1.
  input         NESBOX_GO
);

  localparam NES_BREADCRUMB_GROUP = 8'h04;
  localparam NES_BC_REGISTERS     = 8;

  assign CPU_RST = RST;

  // ---------------------------------------------------------------------------
  // The vendored core itself. No PPU/APU are dropped: the CONTRACT (decision #2)
  // requires the whole `module NES` to stay intact so CPU+DMA+DMC+mapper-0
  // interplay (MemoryMultiplex's CPU/PPU arbitration, DmaController) is exercised
  // exactly as fpganes wrote it -- dissecting the top module would risk
  // reintroducing bugs that instance already avoids.
  // ---------------------------------------------------------------------------
  wire        core_ce;
  // core_reset: pure register output (single source, no logic level -- the
  // reset net fans out to every core register and the STA margin is razor
  // thin).  core_hold_r extends the RST strobe into a LEVEL that lasts until
  // the renderer writes NES_GO=1 (boot handshake; see NESBOX_GO port).  The
  // 1-cycle assertion lag vs the raw strobe is irrelevant (reset is a level,
  // held for ms until GO).  Initial 1: core held from power-up until the
  // first handshake.
  reg         core_hold_r; initial core_hold_r = 1'b1;
  always @(posedge CLK) core_hold_r <= RST | ~NESBOX_GO;
  wire        core_reset = core_hold_r;

  wire [21:0] mem_addr;
  wire        mem_read_cpu;
  wire        mem_read_ppu;
  wire        mem_write;
  wire [7:0]  mem_dout;
  // TWO read-data latches, one per consumer -- "latch A" (CPU) and "latch B"
  // (PPU) in the upstream interface's own words (nes.v port comments). They
  // must be separate registers: when rendering is enabled the PPU's CHR/CIRAM
  // fetch is serviced on the ce right after a CPU read, and the CPU only
  // consumes memory_din_cpu two ce later (sub-cycle #2, ANALYSIS SS2.3) -- a
  // single shared latch gets clobbered by the PPU byte in between, turning
  // fetched opcodes/vectors into pattern-table garbage. (Found by the blargg
  // interrupt suite: nestest never enables rendering, so a single latch
  // passes it -- the failure signature was clean code suddenly executing BRK
  // with vectors pointing into tile data.)
  reg  [7:0]  mem_rdata_cpu_r;
  reg  [7:0]  mem_rdata_ppu_r;

  wire [15:0] sample;
  wire [5:0]  color;
  wire        joypad_strobe;
  wire [1:0]  joypad_clock;
  wire [8:0]  cycle_dbg;
  wire [8:0]  scanline_dbg;
  wire [31:0] dbgadr_dbg;
  wire [1:0]  dbgctr_dbg;

  // CPU debug state, exported by the core as real ports (see breadcrumb block)
  wire [59:0] dbg_cpu_w;
  wire        bc_apu_ce;
  wire        bc_pause_cpu;

  // Video-bridge taps out of the core (threaded through nes.v) -- ALL of these
  // are REGISTERED inside NES:core (ppu.v / nes.v registered output stages;
  // timing isolation that cost an STA run, see those files): the we strobes
  // are ce-qualified 1-CLK2 pulses (do NOT re-gate with ce_pulse_r -- they
  // arrive 1 cycle after the ce, the AND would always be 0), data/levels are
  // register outputs whose exported cones are exactly one register.
  wire        ppu_tap_pal_we;
  wire [4:0]  ppu_tap_pal_idx;
  wire [5:0]  ppu_tap_pal_val;
  wire        ppu_tap_oam_we;
  wire [7:0]  ppu_tap_oam_addr;
  wire [7:0]  ppu_tap_oam_val;
  wire [14:0] ppu_tap_loopy_t;
  wire [2:0]  ppu_tap_fine_x;
  wire        ppu_tap_loopy_w; // $2005/$2006 write toggle (0 = pair complete)
  wire [7:0]  ppu_tap_ppuctrl;
  wire [7:0]  ppu_tap_ppumask;
  wire        tapA_we_w;       // Class A: 1 pulse per memory-bus write (in-core reg)
  wire [21:0] tapA_addr_w;
  wire [7:0]  tapA_data_w;
  wire        tapJ_strobe_w;   // joypad strobe/clock LEVELS (in-core ce-registered)
  wire [1:0]  tapJ_clock_w;
  wire [1:0]  joypad_data_w;   // from bridge shift-registers to the core
  wire        chr_snap_s1p_w;  // CHR-bank snapshot (registered in mmu.v MultiMapper)
  wire [7:0]  chr_snap_s0b_w;
  wire [7:0]  chr_snap_s1b_w;
  // v2.5 CHR window vector (mapper 4 / MMC3) -- same registered-in-MultiMapper
  // molde as chr_snap_*.  Declared EXPLICITLY (an implicit net would synthesize
  // as a silent 1-bit GND, Quartus Warning 10236, and ship a zero vector).
  wire [63:0] chr_snap_win_w;
  wire [7:0]  chr_snap_win_flags_w;
  wire        chr_snap_win_en_w;
  wire [1:0]  nt_snap_arr_w;   // NT-arrangement snapshot (v2.0a; dynamic mmu tap)

  NES core(
    .clk(CLK),
    .reset(core_reset),
    .ce(core_ce),
    .mapper_flags(mapper_flags_in),
    .sample(sample),
    .color(color),
    .joypad_strobe(joypad_strobe),
    .joypad_clock(joypad_clock),
    .joypad_data(joypad_data_w), // fed by the bridge control-block shift-registers
    .audio_channels(5'b11111),  // all channels enabled; unused until audio phase
    .memory_addr(mem_addr),
    .memory_read_cpu(mem_read_cpu),
    .memory_din_cpu(mem_rdata_cpu_r),
    .memory_read_ppu(mem_read_ppu),
    .memory_din_ppu(mem_rdata_ppu_r),
    .memory_write(mem_write),
    .memory_dout(mem_dout),
    .cycle(cycle_dbg),
    .scanline(scanline_dbg),
    .dbgadr(dbgadr_dbg),
    .dbgctr(dbgctr_dbg),
    .dbg_cpu(dbg_cpu_w),
    .dbg_apu_ce(bc_apu_ce),
    .dbg_pause_cpu(bc_pause_cpu),
    .ppu_tap_pal_we(ppu_tap_pal_we),
    .ppu_tap_pal_idx(ppu_tap_pal_idx),
    .ppu_tap_pal_val(ppu_tap_pal_val),
    .ppu_tap_oam_we(ppu_tap_oam_we),
    .ppu_tap_oam_addr(ppu_tap_oam_addr),
    .ppu_tap_oam_val(ppu_tap_oam_val),
    .ppu_tap_loopy_t(ppu_tap_loopy_t),
    .ppu_tap_fine_x(ppu_tap_fine_x),
    .ppu_tap_loopy_w(ppu_tap_loopy_w),
    .ppu_tap_ppuctrl(ppu_tap_ppuctrl),
    .ppu_tap_ppumask(ppu_tap_ppumask),
    .tapA_we(tapA_we_w),
    .tapA_addr(tapA_addr_w),
    .tapA_data(tapA_data_w),
    .tapJ_strobe(tapJ_strobe_w),
    .tapJ_clock(tapJ_clock_w),
    .chr_snap_s1_present(chr_snap_s1p_w),
    .chr_snap_s0_bank(chr_snap_s0b_w),
    .chr_snap_s1_bank(chr_snap_s1b_w),
    .chr_snap_win(chr_snap_win_w),
    .chr_snap_win_flags(chr_snap_win_flags_w),
    .chr_snap_win_en(chr_snap_win_en_w),
    .nt_snap_arr(nt_snap_arr_w)
  );

  // ---------------------------------------------------------------------------
  // On-demand `ce` generator / PSRAM handshake FSM (see header comment).
  //
  // GOTCHA (found by simulation, not by reading the CONTRACT -- record it so it
  // isn't reintroduced): ROM_BUS_RDY is idle-high and, like the real main.v FSM
  // (see e.g. RQ_SGB_ROM_RDYr in sd2snes_sgb/main.v), only drops to 0 on the
  // clock edge AFTER a request pulse is sampled -- i.e. issuing RRQ/WRQ for
  // exactly 1 cycle and then checking `if (ROM_BUS_RDY)` on the very next cycle
  // sees RDY *still reading 1* (the arbiter hasn't registered the request yet),
  // which reads as an immediate false-positive "transaction already done" and
  // skips the wait entirely, latching garbage data. The fix is a dedicated
  // S_REQ phase that holds RRQ/WRQ asserted (not a single blip) until RDY is
  // observed to have actually dropped to 0 -- proof the arbiter registered
  // *this* request -- before moving on to S_WAIT, which watches for RDY to
  // return to 1 (genuine completion). This is unambiguous regardless of how
  // many cycles the grant or the transaction itself actually takes.
  // ---------------------------------------------------------------------------
  localparam S_IDLE    = 3'd0;
  localparam S_REQ     = 3'd1;
  localparam S_WAIT    = 3'd2;
  localparam S_PACE    = 3'd3;
  localparam S_SETTLE  = 3'd4;

  reg [2:0] state;
  reg       req_is_write;
  reg       req_is_ppu;   // which latch the in-flight read returns to

  // Requests are gated by core_hold_r (boot handshake): while the core is held
  // the wrap issues ZERO PSRAM traffic (the CHR upload gets the whole bus minus
  // MCU) and the FSM parks in S_PACE (ce also gated below).  core_hold_r is a
  // register -> +1 LUT on cones already covered by main.sdc #2 (setup-2 into
  // the wrap FSM) / trivially short.
  wire need_read  = (mem_read_cpu | mem_read_ppu) & ~core_hold_r;
  wire need_write = mem_write & ~core_hold_r;

  reg    ce_pulse_r;
  assign core_ce = ce_pulse_r;

  // -- SYSCLK synchronizer + rising-edge detect (see PACER in the header) --
  reg [2:0] sysclk_sr;
  always @(posedge CLK) sysclk_sr <= {sysclk_sr[1:0], SNES_SYSCLK};
  wire sysclk_edge = (sysclk_sr[2:1] == 2'b01);

  // -- pacing credit: saturate at 4, reset to 0 on ce (never accumulates
  //    past one full grant => guaranteed minimum ce spacing, see header) --
  reg [2:0] pace_ctr;
  wire pace_ok = (pace_ctr == 3'd4);

  assign ROM_BUS_ADDR   = {2'b00, mem_addr};
  assign ROM_BUS_WRDATA = mem_dout;
  assign ROM_BUS_WORD   = 1'b0;      // NES core only ever does 8-bit accesses
  // Free slot for the MCU (main.v arbiter): every cycle we are provably not
  // about to raise RRQ/WRQ -- S_PACE (waiting for credit, memory already done)
  // and S_SETTLE (dead cycle) qualify, plus an S_IDLE with no request. An MCU
  // access admitted here can delay OUR next request by up to one arbiter
  // transaction (~9 CLK2); that fits inside the ~15.65-cycle pacing window in
  // the typical case and merely stretches the one ce otherwise (safe).
  assign ROM_FREE_SLOT  = (state == S_PACE) || (state == S_SETTLE) ||
                          ((state == S_IDLE) && !need_read && !need_write);

  always @(posedge CLK) begin
    ce_pulse_r  <= 1'b0;

    if (RST) begin
      state       <= S_IDLE;
      ROM_BUS_RRQ <= 1'b0;
      ROM_BUS_WRQ <= 1'b0;
      pace_ctr    <= 3'd0;
    end else begin
      if (sysclk_edge && !pace_ok) pace_ctr <= pace_ctr + 3'd1;

      case (state)
        S_IDLE: begin
          ROM_BUS_RRQ <= 1'b0;
          ROM_BUS_WRQ <= 1'b0;
          if (need_read) begin
            ROM_BUS_RRQ  <= 1'b1;
            req_is_write <= 1'b0;
            req_is_ppu   <= mem_read_ppu; // MemoryMultiplex: mutually exclusive with mem_read_cpu
            state        <= S_REQ;
          end else if (need_write) begin
            ROM_BUS_WRQ  <= 1'b1;
            req_is_write <= 1'b1;
            state        <= S_REQ;
          end else begin
            state <= S_PACE;
          end
        end
        S_REQ: begin
          // Keep asserting the request every cycle until the arbiter visibly
          // grants it (RDY observed low). Idle-to-busy is guaranteed to happen
          // within a bounded number of cycles (the arbiter's own ST_IDLE->
          // ST_*_ADDR transition), so this never spins forever.
          if (!ROM_BUS_RDY) begin
            ROM_BUS_RRQ <= 1'b0;
            ROM_BUS_WRQ <= 1'b0;
            state       <= S_WAIT;
          end
        end
        S_WAIT: begin
          if (ROM_BUS_RDY) begin
            if (!req_is_write) begin
              if (req_is_ppu) mem_rdata_ppu_r <= ROM_BUS_RDDATA;
              else            mem_rdata_cpu_r <= ROM_BUS_RDDATA;
            end
            state <= S_PACE;
          end
          // else: stretch -- PSRAM/arbiter still busy (typ. 9 CLK2 cycles, up to
          // ~18 under worst-case contention per CONTRACT SS3.2); this is exactly
          // the "esticar e' seguro" contract ANALYSIS SS7.1 establishes.
        end
        S_PACE: begin
          // Memory (if any) already resolved; wait for the pacing credit.
          // NOTE: pace_ctr <= 0 here intentionally OVERRIDES the increment
          // above (same always block, last assignment wins) -- an edge landing
          // on the ce-issue cycle is dropped, which is what makes the minimum-
          // spacing derivation in the header hold.
          // core_hold_r (boot handshake) parks the FSM here with ce gated:
          // no core ticks, no taps, no requests while the renderer uploads
          // CHR.  On release the saturated credit fires one ce and normal
          // pacing resumes (min-spacing guarantee unaffected -- the credit
          // reset-on-ce rule is unchanged).
          if (pace_ok && !core_hold_r) begin
            ce_pulse_r <= 1'b1;
            pace_ctr   <= 3'd0;
            state      <= S_SETTLE;
          end
        end
        S_SETTLE: begin
          state <= S_IDLE;
        end
        default: state <= S_IDLE;
      endcase
    end
  end

  // ---------------------------------------------------------------------------
  // Audio: sgb.v-style [19:0] = {ch1[9:0], ch0[9:0]}, both channels fed the same
  // mono sample (ANALYSIS SS6.2/SS7.4 item 4: unsigned/offset-binary Sample ->
  // signed via top-bit XOR, then truncated to the dac.v path's 10-bit-per-
  // channel format). Not validated -- audio is out of scope for Phase -1.
  // ---------------------------------------------------------------------------
  wire [15:0] sample_signed = {~sample[15], sample[14:0]};

  // ---------------------------------------------------------------------------
  // AMPLITUDE-DOMAIN REALIGNMENT (device symptom: "faint hum" -- signal alive
  // but ~-8..-12dB below what the DAC chain expects).  Derivation:
  //   * dac.v's sgb-channel gain (<<4) was calibrated for the GB domain:
  //     sgb_cpu.v sums 4x5-bit channels x (NR50+1) -> unsigned 0..992 across
  //     the FULL 10-bit range (music uses ~60-100% FS).
  //   * The 2A03 LUT is 16-bit unsigned but REAL music content lives in
  //     ~8k..35k of 65536 -> the raw [15:6] slice delivers only ~25-38% FS.
  //   * The offset-binary flip also leaves a HUGE DC (~-16k signed: the 2A03
  //     is unipolar, resting level ~0x4150, not centered at 0x8000) -- any
  //     direct gain saturates on the DC, and the physical DAC is AC-coupled
  //     so the DC buys nothing.
  // Fix: 1-pole DC blocker (leaky average, shift 16 -> ~13Hz cutoff at the
  // 5.37MHz ce rate, converges in ~12ms) followed by x4 gain with EXPLICIT
  // saturation.  Music then lands at ~60-100% FS like the GB.  ce-gated,
  // reg-to-reg (same pipeline discipline as the breadcrumbs; synthesis #15).
  // ---------------------------------------------------------------------------
  // GAIN KNOB: each +1 = +6dB.  2 (=x4) maps NORMAL 2A03 music (~12-25k pp
  // of the 16-bit LUT range) to ~47-78%% of the 10-bit FS -- the GB-domain
  // level dac.v was calibrated for.  The Excitebike engine sound alone is
  // envelope-quiet (~570 pp measured) and legitimately sits ~-29dBFS after
  // x4; do NOT calibrate the knob to it (device spectrum 2026-07: -47dBFS
  // peak was the QUIET engine through the old /4 chain, not a broken path).
  localparam APU_GAIN_SHIFT = 2;
  reg  signed [15:0] apu_x_r;       // raw sample capture (shortest cone)
  reg  signed [31:0] apu_avg_r;     // DC estimate, 16.16 fixed point
  reg  signed [15:0] apu_ac_r;      // DC-blocked sample
  reg  signed [9:0]  apu_out_r;     // gained + saturated 10-bit output
  wire signed [15:0] apu_avg_hi  = apu_avg_r[31:16];
  wire signed [16:0] apu_ac_w    = apu_x_r - apu_avg_hi;
  wire signed [16:0] apu_err_w   = apu_ac_w;                 // same difference
  // x4 with saturation to 14 bits pre-slice (14+2 = 16): saturate when the
  // AC sample exceeds +/-8191 (rare fortissimo peaks)
  wire        apu_clip_hi = (apu_ac_r[15] == 1'b0) && (|apu_ac_r[14:14-APU_GAIN_SHIFT+1]);
  wire        apu_clip_lo = (apu_ac_r[15] == 1'b1) && (~&apu_ac_r[14:14-APU_GAIN_SHIFT+1]);
  wire signed [15:0] apu_gained_w = apu_clip_hi ? 16'sh7FFF :
                                    apu_clip_lo ? -16'sh8000 :
                                    (apu_ac_r <<< APU_GAIN_SHIFT);
  // warm-up: the fpganes LUT capture regs (tmp_a/b) have no reset and read X
  // until the first ce -- one X into the leaky accumulator sticks forever in
  // simulation.  Hold the filter off for the first 16 ce (harmless in
  // silicon, saves the sim gate).
  reg [4:0] apu_warm_r;
  always @(posedge CLK) begin
    if (core_reset) begin
      apu_warm_r<= 5'd0;
      apu_x_r   <= 16'sd0;
      apu_avg_r <= 32'sd0;
      apu_ac_r  <= 16'sd0;
      apu_out_r <= 10'sd0;
    end else if (ce_pulse_r) begin
      if (!apu_warm_r[4]) apu_warm_r <= apu_warm_r + 5'd1;
      else begin
        apu_x_r   <= sample_signed;                     // stage 1: raw capture
        apu_avg_r <= apu_avg_r + {{15{apu_err_w[16]}}, apu_err_w};  // += err>>16 (16.16)
        apu_ac_r  <= (apu_ac_w[16] == apu_ac_w[15]) ? apu_ac_w[15:0]
                   : apu_ac_w[16] ? -16'sh8000 : 16'sh7FFF;  // clamp 17->16
        apu_out_r <= apu_gained_w[15:6];                // stage 3: slice to 10b
      end
    end
  end
  assign APU_DAT = {apu_out_r, apu_out_r};

  // ---------------------------------------------------------------------------
  // Audio liveness breadcrumbs (group 0x04 idx 14/15; firmware publishes them
  // as NDBG v3 +20/+21).  Everything registered, ce-gated, shallow cones.
  //   idx 14 = MAX-HOLD of |sample| >> 2 (8 bits) -- sticky since core reset,
  //            cleared by core_reset only.  The RESTING level is NOT zero:
  //            the 2A03 idles at a DC of |sample| ~ 251 (triangle parked on a
  //            step + DMC level -> tnd lookup), so on working silicon this
  //            reads ~62 shortly after boot; 0 means the sample path itself
  //            is dead in silicon.
  //   idx 15 = rolling count of sample CHANGES (|delta| > 2, ce-sampled).
  //            This is the DISCRIMINANT for "audio alive": a nonzero-level
  //            counter (as first proposed) would spin forever on the DC idle
  //            alone; CHANGES only accumulate when the APU is actually
  //            producing a waveform.  Music playing -> increments steadily
  //            (reads differ between polls); flat/mute -> frozen.
  // ---------------------------------------------------------------------------
  // PIPELINE STAGE (synthesis #15 fix, setup -1.515ns): these breadcrumb
  // registers live in the WRAP, so the SDC multicycle {*|NES:core|*} does NOT
  // cover them -- the raw M9K(lookup)->adder(Sample)->abs/compare cone was
  // timed single-cycle and violated.  Capture the RAW sample into one
  // ce-gated register first (no arithmetic in that cone), then do abs/delta/
  // compare reg-to-reg inside the wrap on the NEXT ce (>=13 CLK2 of slack).
  // Costs one ce of latency; max-hold and delta-count semantics unchanged.
  reg  signed [9:0]  apu_samp_r;
  wire        [9:0]  apu_abs_w   = apu_samp_r[9] ? (~apu_samp_r + 10'd1) : apu_samp_r;
  reg  signed [9:0]  apu_prev_r;
  wire signed [10:0] apu_delta_w = {apu_samp_r[9], apu_samp_r} - {apu_prev_r[9], apu_prev_r};
  wire        [10:0] apu_dabs_w  = apu_delta_w[10] ? (~apu_delta_w + 11'd1) : apu_delta_w;
  reg  [7:0] apu_max_r;   // idx 14
  reg  [7:0] apu_act_r;   // idx 15
  // idx 19 (NDBG v3 +25): rolling count of CLASS-A CIRAM writes as captured
  // by the tap (br_nt_we).  Silicon-side discriminator for the wedge/donut
  // symptom family: a controlled redraw (e.g. title) has a KNOWN write count;
  // captured-count != delivered-cell-count separates "tap misses/duplicates
  // writes in silicon" from "content corrupted later".
  reg  [7:0] ntw_cnt_r;   // idx 19
  always @(posedge CLK) begin
    if (core_reset) begin
      apu_samp_r <= 10'sd0;
      apu_max_r  <= 8'd0;
      apu_act_r  <= 8'd0;
      apu_prev_r <= 10'sd0;
    end else if (ce_pulse_r) begin
      apu_samp_r <= sample_signed[15:6];   // raw capture: shortest possible cone
      apu_prev_r <= apu_samp_r;
      if (apu_abs_w[9:2] > apu_max_r) apu_max_r <= apu_abs_w[9:2];
      if (apu_dabs_w > 11'd2)         apu_act_r <= apu_act_r + 8'd1;
    end
  end

  assign APU_CLK_EDGE = ce_pulse_r;

  // ---------------------------------------------------------------------------
  // Breadcrumb: 8x8-bit registers snapshotting PC/A/X/Y/P/SP/cycle-lo/cycle-hi
  // every time the CPU begins a new instruction fetch, mirroring sgb.v's
  // config_r[] mechanism 1:1 (mcu_cmd.v 0xf9/0xfa, group 8'h04). The CPU state
  // arrives through real ports (dbg_cpu/dbg_apu_ce/dbg_pause_cpu on the NES
  // instance) -- Quartus cannot synthesize hierarchical references.
  // ---------------------------------------------------------------------------
  integer i;
  reg [7:0] bc_r [NES_BC_REGISTERS-1:0];
  initial for (i = 0; i < NES_BC_REGISTERS; i = i + 1) bc_r[i] = 8'h00;

  reg [15:0] bc_cyc_r;
  reg        bc_was_reset_r;

  wire        bc_cpu_ce      = bc_apu_ce && !bc_pause_cpu;
  wire        bc_is_reset    = dbg_cpu_w[59];
  wire [2:0]  bc_cpu_state   = dbg_cpu_w[58:56];
  wire [7:0]  bc_p           = dbg_cpu_w[55:48];
  wire [7:0]  bc_sp          = dbg_cpu_w[47:40];
  wire [7:0]  bc_y           = dbg_cpu_w[39:32];
  wire [7:0]  bc_x           = dbg_cpu_w[31:24];
  wire [7:0]  bc_a           = dbg_cpu_w[23:16];
  wire [15:0] bc_pc          = dbg_cpu_w[15:0];

  // Single always block: the snapshot and the MCU write path drive bc_r from
  // the same process (two separate always blocks = multiple drivers, which
  // simulates but does not synthesize). MCU write wins on collision.
  always @(posedge CLK) begin
    if (RST) begin
      bc_cyc_r       <= 16'h0000;
      bc_was_reset_r <= 1'b1;
    end else begin
      if (bc_apu_ce) begin
        if (bc_cpu_ce && bc_cpu_state == 3'd0 && !bc_is_reset) begin
          bc_r[0] <= bc_pc[7:0];
          bc_r[1] <= bc_pc[15:8];
          bc_r[2] <= bc_a;
          bc_r[3] <= bc_x;
          bc_r[4] <= bc_y;
          bc_r[5] <= bc_p;
          bc_r[6] <= bc_sp;
          bc_r[7] <= bc_cyc_r[7:0]; // low byte only; ample for spot-checks
        end
        bc_cyc_r <= bc_cyc_r + 16'd1;
      end
      if (bc_cpu_ce) bc_was_reset_r <= bc_is_reset;
      if (reg_we_in && (reg_group_in == NES_BREADCRUMB_GROUP) && (reg_index_in < NES_BC_REGISTERS))
        bc_r[reg_index_in] <= (bc_r[reg_index_in] & reg_invmask_in) | (reg_value_in & ~reg_invmask_in);
    end
  end

  // ---------------------------------------------------------------------------
  // Video bridge integration (Fase 1a).  Taps -> nes_bridge -> mailbox window +
  // control block.  See nes_bridge.v.  The bridge module itself is validated
  // byte-exact by tb/tb_bridge.v against the golden bridge-sim mailbox streams;
  // here it is wired to the LIVE taps for hardware.
  //
  // Taps are accepted ALWAYS (frame-N+1 events accumulate while frame N
  // serializes): nes_bridge ping-pongs the dirty bitmaps/counters, copies OAM
  // at the tick and reads ciram/pal live -- see the SNAPSHOT note in
  // nes_bridge.v (the old "S_IDLE-gated taps" hardware gap is CLOSED).
  //
  // fb_hint tied 0: forced_blank is an SNES-VRAM-residency (LRU) hint the FPGA
  // cannot observe -- the renderer computes it host-side (see nes_bridge header).
  // CHR-bank snapshot comes from the REAL mmu.v tap (chr_snap_*, registered in
  // MultiMapper; convention == bridge_sim chr_slots(), validated by
  // tb/run_chrtap.sh against the bridge_sim.mappers golden).
  // ---------------------------------------------------------------------------
  // Class A taps: from the IN-CORE registered write tap (tapA_*, nes.v), NOT
  // from the raw mem_addr/mem_dout bus nets -- mem_dout's cone includes the
  // deep din/dbus render-readback path (see nes.v/ppu.v tap comments), which
  // must never feed the bridge's free-running registers directly.  The region
  // decode below runs on register outputs (shallow).  Same 1:1 event stream
  // as sampling at ROM_BUS_WRQ (spec SS2.1/SS12), one CLK2 later.
  wire br_nt_we  = tapA_we_w && (tapA_addr_w[21:18] == 4'b1100); // CIRAM 0x300000
  wire br_chr_we = tapA_we_w && (tapA_addr_w[21:20] == 2'b10);   // CHR-RAM 0x200000

  // idx19 counter lives here (br_nt_we declared just above)
  always @(posedge CLK) begin
    if (core_reset)     ntw_cnt_r <= 8'd0;
    else if (br_nt_we)  ntw_cnt_r <= ntw_cnt_r + 8'd1;   // registered tap pulse
  end

  // frame close: entering NES vblank (scanline 241), sampled on the core tick.
  reg [8:0] scanline_prev_r;
  reg       frame_tick_r;
  always @(posedge CLK) begin
    frame_tick_r <= 1'b0;
    if (RST) scanline_prev_r <= 9'd0;
    else if (ce_pulse_r) begin
      if (scanline_dbg == 9'd241 && scanline_prev_r != 9'd241) frame_tick_r <= 1'b1;
      scanline_prev_r <= scanline_dbg;
    end
  end

  // idx20/21: LRCK-per-frame frequency proof
  reg [1:0]  lrckmon_sr;
  reg [15:0] lrck_cnt_r, lrck_frame_r;
  always @(posedge CLK) begin
    lrckmon_sr <= {lrckmon_sr[0], DAC_LRCK_MON};
    if (RST) begin
      lrck_cnt_r <= 16'd0; lrck_frame_r <= 16'd0;
    end else if (frame_tick_r) begin
      lrck_frame_r <= lrck_cnt_r + {15'd0, (lrckmon_sr == 2'b01)};
      lrck_cnt_r   <= 16'd0;
    end else if (lrckmon_sr == 2'b01)
      lrck_cnt_r <= lrck_cnt_r + 16'd1;
  end

  // -------- mid-display scroll snapshot (protocol v1.2) --------------------
  // Root cause of the "frozen playfield" bug: split games (Excitebike)
  // rewrite $2005/$2000 mid-frame; snapping loopy_t at the frame CLOSE always
  // captured the last split (HUD = 0,0) -- 2200/2200 frames of a real race
  // shipped scroll 0.  The DOMINANT region owns mid-display: latch loopy_T +
  // fine_x at the first ce of scanline 120 (T only changes on writes, so
  // this equals "state after all display writes above the midline" -- the
  // playfield's own scroll in top-HUD and bottom-HUD layouts alike).
  // Mirrored EXACTLY by bridge_sim (ppustate.note_scanline: freeze before
  // the first event with 120<=scanline<=240, else at close).  Remaining v1
  // limitation (documented): true per-split rendering is Phase 3; the sky
  // strip scrolls along with the playfield.
  reg [14:0] loopy_mid_r;
  reg [2:0]  finex_mid_r;
  reg        mid_seen_r;
  always @(posedge CLK) begin
    if (RST) begin
      loopy_mid_r <= 15'd0; finex_mid_r <= 3'd0; mid_seen_r <= 1'b0;
    end else begin
      if (frame_tick_r) mid_seen_r <= 1'b0;        // re-arm each frame
      else if (ce_pulse_r && scanline_dbg == 9'd120 && !mid_seen_r) begin
        loopy_mid_r <= ppu_tap_loopy_t;
        finex_mid_r <= ppu_tap_fine_x;
        mid_seen_r  <= 1'b1;
      end
    end
  end

  // -------- mid-display OAM freeze (protocol v1.4b OAM-tear fix) -----------
  // Snapshot the bridge's live OAM at scanline 120 (MID-DISPLAY) instead of at
  // the frame tick (scanline 241, in vblank right next to the game's ~dot-253
  // OAM-DMA).  Games rewrite OAM only via DMA in vblank, so oam[]@120 ==
  // oam[]@frame-close -- the frozen bytes are IDENTICAL (goldens unchanged), but
  // the 257-cycle copy now runs where OAM is provably stable, entirely out of
  // the vblank/OAM-DMA window (the copy-at-tick path snapshotted IN vblank).
  // Same one-shot-per-frame pattern as loopy_mid_r (re-armed at frame_tick);
  // the copy finishes ~121 scanlines before the tick, so the serializer always
  // reads a stable oam_frz.
  reg oam_frz_seen_r;
  reg oam_freeze_r;
  always @(posedge CLK) begin
    oam_freeze_r <= 1'b0;
    if (RST) oam_frz_seen_r <= 1'b0;
    else begin
      if (frame_tick_r) oam_frz_seen_r <= 1'b0;        // re-arm each frame
      else if (ce_pulse_r && scanline_dbg == 9'd120 && !oam_frz_seen_r) begin
        oam_freeze_r   <= 1'b1;
        oam_frz_seen_r <= 1'b1;
      end
    end
  end

  // -------- multi-scroll split capture (protocol v1.3.1 + v1.4b) -----------
  // Up to K=4 (scanline, loopy_T, fine_x) entries per frame.  Entry 0 = state
  // at display start; nes_bridge derives sx/sy/ntsel from each entry's raw T/fx
  // exactly like CMD_REGS and emits CMD_SPLITS only when cnt>=2 (split-less
  // games emit NOTHING).  EXTRACTED into nes_split_capture.v so it is directly
  // testable (tb/tb_split_capture.v) -- the byte-exact bridge gate feeds the
  // entries via the stimulus and never exercises this capture.
  //
  // v1.4b: the capture finalizes an entry only on a COMPLETE $2005/$2006 write
  // pair (ppu_tap_loopy_w==0), so the intermediate T of a SCANLINE-STRADDLING
  // pair (Excitebike's $2000@57 + pair@58) never becomes a phantom entry -- the
  // residual boundary flicker the v1.3.1 <=1-scanline coalescence still left
  // (~7%/split).  See nes_split_capture.v header + bridge_sim note_split_change.
  wire [2:0]  spl_cnt;
  wire        spl_ovf;
  wire [31:0] spl_sl_flat;
  wire [59:0] spl_t_flat;
  wire [11:0] spl_fx_flat;
  nes_split_capture spl_cap(
    .CLK(CLK), .RST(RST), .ce(ce_pulse_r), .frame_tick(frame_tick_r),
    .scanline(scanline_dbg), .loopy_t(ppu_tap_loopy_t), .fine_x(ppu_tap_fine_x),
    .ppumask(ppu_tap_ppumask), .w(ppu_tap_loopy_w),
    .spl_cnt_o(spl_cnt), .spl_ovf_o(spl_ovf),
    .spl_sl_flat(spl_sl_flat), .spl_t_flat(spl_t_flat), .spl_fx_flat(spl_fx_flat)
  );

  // -------- CHR raster-split capture (protocol v2.3, CMD_CHR_SPLITS) -------
  // Same shape as spl_cap above (K=4 entries, entry 0 = display start, <=1-line
  // coalescence, shortest-strip eviction) but over the mmu CHR slot-0 bank tap:
  // MMC1 games that re-bank MID-DISPLAY every frame (RoboCop 2: HUD scanlines
  // 0-31 from one 4K bank, playfield from another) are not representable by the
  // once-per-frame CMD_CHR_STATE, so the bridge now also ships WHERE the bank
  // changed and the renderer raster-splits CHR via HDMA $210B.
  // No w-gating (the MMC1 commit is atomic on the 5th serial write and the tap
  // is registered under ce inside MultiMapper); a mid-frame 8K<->4K flip poisons
  // the frame instead.  See nes_chrsplit_capture.v.
  // Output nets declared EXPLICITLY: an implicit net here would synthesize as a
  // silent 1-bit GND (Quartus Warning 10236) and the whole command would ship
  // zeros.
  wire [2:0]  cspl_cnt;
  wire        cspl_ovf;
  wire        cspl_poison;
  wire [31:0] cspl_sl_flat;
  wire [31:0] cspl_bank_flat;
  nes_chrsplit_capture cspl_cap(
    .CLK(CLK), .RST(RST), .ce(ce_pulse_r), .frame_tick(frame_tick_r),
    .scanline(scanline_dbg),
    .s0_bank(chr_snap_s0b_w), .s1_present(chr_snap_s1p_w),
    .ppumask(ppu_tap_ppumask),
    .cspl_cnt_o(cspl_cnt), .cspl_ovf_o(cspl_ovf), .cspl_poison_o(cspl_poison),
    .cspl_sl_flat(cspl_sl_flat), .cspl_bank_flat(cspl_bank_flat)
  );

  // -------- CHR window-vector raster capture (v2.5, CMD_CHR_SPLITS8) -------
  // The mapper-4 twin of cspl_cap above, over the EIGHT 1KB window vector the
  // mmu publishes for MMC3 (chr_snap_win).  Same K=4 / entry-0 / coalescence /
  // eviction rules; no poison (the vector has no mode flip to invalidate).
  // Exactly ONE of the two captures can ever fire in a given run: outside mapper
  // 4 chr_snap_win is a constant 0, inside mapper 4 the legacy slot tap is a
  // constant sentinel -- both inert by construction, no gating needed here.
  // Output nets declared EXPLICITLY (Quartus Warning 10236 -- an implicit net
  // would be a silent 1-bit GND and the command would ship zeros).
  wire [2:0]   cwin_cnt;
  wire         cwin_ovf;
  wire [31:0]  cwin_sl_flat;
  wire [255:0] cwin_win_flat;
  nes_chrwin_capture cwin_cap(
    .CLK(CLK), .RST(RST), .ce(ce_pulse_r), .frame_tick(frame_tick_r),
    .scanline(scanline_dbg),
    .win(chr_snap_win_w),
    .ppumask(ppu_tap_ppumask),
    .cwin_cnt_o(cwin_cnt), .cwin_ovf_o(cwin_ovf),
    .cwin_sl_flat(cwin_sl_flat), .cwin_win_flat(cwin_win_flat)
  );

  reg [15:0] nes_frame_ctr;   // free-running frame number for FRAME_HDR
  always @(posedge CLK) begin
    if (RST) nes_frame_ctr <= 16'd0;
    else if (frame_tick_r) nes_frame_ctr <= nes_frame_ctr + 16'd1;
  end

  wire [15:0] band_bytes_last, band_frames, band_overruns;
  wire [7:0]  pal_sum_w, pal_wcnt_w;   // idx22/23 (NDBG v3 +28/+29)

  nes_bridge bridge(
    .clk(CLK), .rst(RST),
    .nt_we(br_nt_we),  .nt_addr(tapA_addr_w[10:0]), .nt_data(tapA_data_w),
    // v2.4: the CHR tap carries OFFSET + DATA (CMD_CHR_RUN 0x41 is inline now).
    // tapA_addr_w[12:0] is the mapper-resolved BYTE offset inside the <=8 KiB
    // CHR-RAM -- the same address the old chr_tile derived (>>4) its tile index
    // from, so the correspondence with bridge_sim's chr_flat_addr() is unchanged.
    .chr_we(br_chr_we), .chr_off(tapA_addr_w[12:0]), .chr_data(tapA_data_w),
    // pal/oam strobes are ALREADY ce-qualified 1-CLK2 pulses (registered in
    // ppu.v) -- re-gating with ce_pulse_r would kill them (they arrive one
    // cycle after the ce).
    .pal_we(ppu_tap_pal_we), .pal_idx(ppu_tap_pal_idx), .pal_data(ppu_tap_pal_val),
    .oam_we(ppu_tap_oam_we), .oam_addr(ppu_tap_oam_addr), .oam_data(ppu_tap_oam_val),
    .oam_freeze(oam_freeze_r),   // v1.4b: mid-display (scanline 120) OAM snapshot
    .frame_tick(frame_tick_r),
    .snap_frame(nes_frame_ctr),
    .snap_loopy_t(loopy_mid_r), .snap_fine_x(finex_mid_r),   // v1.2 mid-display
    .snap_ppuctrl(ppu_tap_ppuctrl), .snap_ppumask(ppu_tap_ppumask),
    .snap_fb_hint(1'b0),
    // NT arrangement (v2.0a): DYNAMIC mirror mode from the REAL mmu tap
    // (nt_snap_arr, registered in MultiMapper -- same molde/timing as
    // chr_snap_*).  Replaces the old STATIC mapper_flags[14] feed: MMC1/AxROM
    // change mirroring at runtime and the iNES header flag does NOT reflect it
    // (metroid = header-`h` but runs 1794 V + 6 1A frames; Nintendo Tetris
    // MMC1 = the header-mirror-vs-runtime mismatch behind the boot tile-0 sea).
    // The mmu tap already carries the protocol NTARR code (H=0/V=1/1A=2/1B=3),
    // so the bridge's l_ntarr <= snap_ntarr and the renderer's nes_handle_hdr
    // now see the true per-frame arrangement.
    .snap_ntarr(nt_snap_arr_w),
    // CHR-bank snapshot from the REAL mmu tap (registered in MultiMapper);
    // slot0 is always present (every v0 mapper reports it -- matches the
    // simulator's chr_slots(), which never omits slot 0).
    .snap_s0_present(1'b1), .snap_s0_bank(chr_snap_s0b_w),
    .snap_s1_present(chr_snap_s1p_w), .snap_s1_bank(chr_snap_s1b_w),
    // v1.3 multi-scroll: K=4 raw (scanline,loopy_t,fine_x) entries captured
    // above (spl_*); the bridge derives sx/sy/ntsel and emits CMD_SPLITS.
    .snap_split_cnt(spl_cnt), .snap_split_ovf(spl_ovf),
    .snap_spl_sl(spl_sl_flat), .snap_spl_t(spl_t_flat), .snap_spl_fx(spl_fx_flat),
    // v2.3 CHR raster-split: K=4 (scanline, slot0 bank) entries captured above
    // (cspl_*); the bridge emits CMD_CHR_SPLITS only when cnt>=2 && !poison.
    .snap_cspl_cnt(cspl_cnt), .snap_cspl_ovf(cspl_ovf),
    .snap_cspl_poison(cspl_poison),
    .snap_cspl_sl(cspl_sl_flat), .snap_cspl_bank(cspl_bank_flat),
    // v2.5 mapper-4 CHR window vector: the per-frame state (CMD_CHR_STATE8, gated
    // by the mmu's own "this mapper publishes windows" bit) plus the K=4 raster
    // entries from cwin_cap (CMD_CHR_SPLITS8, emitted when cnt>=2).
    .snap_chr_win_en(chr_snap_win_en_w), .snap_chr_win(chr_snap_win_w),
    .snap_chr_win_flags(chr_snap_win_flags_w),
    .snap_cwin_cnt(cwin_cnt), .snap_cwin_ovf(cwin_ovf),
    .snap_cwin_sl(cwin_sl_flat), .snap_cwin_win(cwin_win_flat),
    .frame_seq_o(NESBOX_FRAME_SEQ), .frame_len_o(NESBOX_FRAME_LEN),
    .status_o(NESBOX_STATUS), .frame_done_o(),
    .frame_ack_i(NESBOX_FRAME_ACK), .buf_sel_i(NESBOX_BUF_SEL),
    .resync_en(1'b1),          // hardware: full-state resync on boot/overrun
    .win_addr(NESBOX_ADDR), .win_data(NESBOX_DATA),
    .ctrl_p1_i(NESBOX_CTRL_P1), .ctrl_p2_i(NESBOX_CTRL_P2),
    // in-core ce-registered LEVELS; the bridge falling-edge-detects joy_clock
    .joy_strobe(tapJ_strobe_w), .joy_clock(tapJ_clock_w), .joypad_data_o(joypad_data_w),
    .bc_bytes_last(band_bytes_last), .bc_frames(band_frames), .bc_overruns(band_overruns),
    .dbg_pal_sum(pal_sum_w), .dbg_pal_wcnt(pal_wcnt_w)
  );

  // Config-bus read (mcu_cmd 0xf9): 0..7 = CPU breadcrumb; 8..13 = band counters.
  assign config_data_out =
      (reg_read_in <  NES_BC_REGISTERS) ? bc_r[reg_read_in]  :
      (reg_read_in == 8'd8)  ? band_bytes_last[7:0]  :
      (reg_read_in == 8'd9)  ? band_bytes_last[15:8] :
      (reg_read_in == 8'd10) ? band_frames[7:0]      :
      (reg_read_in == 8'd11) ? band_frames[15:8]     :
      (reg_read_in == 8'd12) ? band_overruns[7:0]    :
      (reg_read_in == 8'd13) ? band_overruns[15:8]   :
      (reg_read_in == 8'd14) ? apu_max_r             :  // audio max-hold (NDBG v3 +20)
      (reg_read_in == 8'd15) ? apu_act_r             :  // audio activity  (NDBG v3 +21)
      (reg_read_in == 8'd16) ? DAC_DBG_CIC           :  // DAC CIC max-hold (NDBG v3 +22)
      (reg_read_in == 8'd17) ? DAC_DBG_I2S           :  // DAC I2S activity (NDBG v3 +23)
      (reg_read_in == 8'd18) ? DAC_DBG_DAT           :  // DAC shifter-word max (NDBG v3 +24)
      (reg_read_in == 8'd19) ? ntw_cnt_r             :  // CIRAM tap write count (NDBG v3 +25)
      (reg_read_in == 8'd20) ? lrck_frame_r[7:0]     :  // LRCK/frame lo (NDBG v3 +26)
      (reg_read_in == 8'd21) ? lrck_frame_r[15:8]    :  // LRCK/frame hi (NDBG v3 +27)
      (reg_read_in == 8'd22) ? pal_sum_w             :  // palette fingerprint (NDBG v3 +28)
      (reg_read_in == 8'd23) ? pal_wcnt_w            :  // palette write count (NDBG v3 +29)
      // v2.4 (+30): the bridge's own status byte.  bit0 = renderer overrun
      // (pre-existing), bit1 = cb_ovf = "a CHR-RAM byte was DROPPED because the
      // capture ring was full".  Exposed here because the ring sizing (4096 B
      // vs a measured peak of ~2250) is an ASSUMPTION, and the only way to find
      // out it broke on real silicon is to read the flag: a corrupted tile with
      // bit1 clear means "frame lost" (the known CHR-recovery limitation, see
      // nes_bridge.v), with bit1 set it means "ring too small".  The SNES already
      // reads status_o live at $2xx7; this mirror is for the USB breadcrumb.
      // TODO 2.4: src/nes.c has to publish index 24 in the NDBG block.
      (reg_read_in == 8'd24) ? NESBOX_STATUS         :  // bridge status (NDBG +30)
      8'h00;

endmodule
