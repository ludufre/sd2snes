// upd77c25_extpgm.v
//
// External program memory for the ST011 uPD96050 core.
//
// The ST011 program is 16384 24-bit words, too large for on-chip memory. It
// lives in the board's Bus 2 SRAM (8-bit, 45 ns; 256 KB usable, ~49 KB used),
// which no other part of this core drives. Each word is stored as 3 bytes at
// byte address word*3, in the order the MCU sends them (PGM_DI); the
// byte-order correction to the true instruction is applied on read.
//
// Structure, in the order a fetch is served:
//   1. loop buffer   8 recent words in flip-flops, zero latency
//   2. pinned table  words 0..255 (every word used during DMA transfers),
//                    block RAM, prewarmed after the firmware download
//      cache         512-entry direct-mapped block RAM for all other words,
//                    filled on demand (both looked up in parallel)
//   3. SRAM          ~32 cycles per word (miss_latency_tb)
//
// No dependency on the core's RST: the MCU downloads the firmware (PGM_WR,
// $E9) while the DSP is held in reset, so this module must accept writes
// then. Power-up state comes from initial values applied at configuration.
//
// RAM_OE/RAM_WE are active low. Each byte access presents the address for
// one cycle with both deasserted before asserting OE or WE.

module upd77c25_extpgm (
  input CLK,
  input enable,

  // fetch interface, consumed by upd77c25.v's opcode_w mux
  input [13:0] pc,

  // Lookahead port (throughput fix -- see CACHE LOOKUP TIMING below).
  // pc_early carries the core's combinational next-pc while it is still
  // in STATE_STORE, one cycle before `pc` itself changes. Driving the
  // cache's read address from it removes the lookup latency from the
  // instruction's critical tail entirely: the tag/data for the new pc
  // are already registered by the time the core reaches STATE_NEXT, so
  // `ready` is combinationally true on the first cycle of that state.
  // Tie pc_early_valid low to fall back to the old behaviour (one extra
  // stall cycle per instruction, still functionally correct).
  input [13:0] pc_early,
  input pc_early_valid,

  output [23:0] dout,
  output ready,

  // firmware download interface (same signals mcu_cmd.v already drives)
  input PGM_WR,
  input [23:0] PGM_DI,
  input [13:0] PGM_WR_ADDR,
  output wr_busy,

  // Readback checksum sweep ($E5 start, $F5 result).
  // PSRAM fetch path (PGM_IN_PSRAM=1, unused and incomplete).
  output reg psram_rrq = 1'b0,
  output reg [23:0] psram_addr = 24'd0,
  input [15:0] psram_din,
  input psram_rdy,

  input vsum_start,
  output reg vsum_busy = 1'b0,
  output reg [31:0] vsum = 32'd0,

  // physical SRAM bus (Bus 2 SRAM, 8-bit, 45ns)
  output reg [18:0] RAM_ADDR,
  inout [7:0] RAM_DATA,
  output reg RAM_OE = 1'b1, // active-low: 1 = deasserted/idle
  output reg RAM_WE = 1'b1
);

  // SRAM needs 45ns (4.3 cycles at 96MHz). The 2-cycle synchronizer comes out
  // of this budget, so HOLD=8 leaves 62.5ns of settling, ~1.4x the spec.
  localparam HOLD_CYCLES = 8;

  localparam S_IDLE       = 5'd0,
             S_WR_ADDR0   = 5'd1, S_WR_HOLD0 = 5'd2, S_WR_POST0 = 5'd3,
             S_WR_ADDR1   = 5'd4, S_WR_HOLD1 = 5'd5, S_WR_POST1 = 5'd6,
             S_WR_ADDR2   = 5'd7, S_WR_HOLD2 = 5'd8, S_WR_POST2 = 5'd9,
             S_RD_ADDR0   = 5'd10, S_RD_HOLD0 = 5'd11,
             S_RD_ADDR1   = 5'd12, S_RD_HOLD1 = 5'd13,
             S_RD_ADDR2   = 5'd14, S_RD_HOLD2 = 5'd15,
             S_RD_CALC    = 5'd16;

  localparam POST_CYCLES = 2; // address/data hold margin after WE deasserts

  reg [4:0] state = S_IDLE;
  reg [6:0] hold_cnt;
  reg [1:0] post_cnt;

  // Pinned table + general cache (see PINNED TABLE below). A hit costs no SRAM
  // access. Set CACHE_ENABLE to 0 to send every fetch to the SRAM
  // (diagnostic builds only).
  parameter CACHE_ENABLE = 1;

  // Throughput requires the cache: three sequential 45ns SRAM reads per word
  // cannot keep up with ST011's DMA transfers (see upd77c25.v THROUGHPUT).
  // Read-verify: read each missed word twice, commit on agreement. Keep 0.
  // The SRAM path has been verified (16,384-word readback sweep), and at 1 a
  // miss costs 62 cycles instead of 32 -- more than one DMA byte slot.
  parameter READ_VERIFY = 0;

  // ---- LOOP BUFFER (off by default) -------------------------------------
  //
  // Kept at 0. It existed to protect the transfer loops from eviction, which
  // the pinned table now does; over the full MesenCE trace 8 entries and 0
  // give the same miss rate (8.0%). It is also expensive: LOOPBUF_ENTRIES
  // associative 14-bit compares hang off pc and feed `ready` and the opcode
  // decode, which was the failing mk2 path (TS_CLK21, -2.655 ns).
  //
  // Fully-associative buffer of recently fetched words, in flip-flops, checked
  // in parallel with the cache and hit with zero latency. It serves loops whose
  // words collide in the 512-entry cache: they run at full speed after their
  // first iteration. Inserts
  // only happen on demand fetches, so a resident loop stays resident.
  //
  // Sized 8: it is a combinational compare feeding `ready`, and the ST011
  // transfer loops are 4 words. 0 removes it.
  parameter LOOPBUF_ENTRIES = 0;
  localparam LB_IDX = (LOOPBUF_ENTRIES <= 2) ? 1 : (LOOPBUF_ENTRIES <= 4) ? 2
                    : (LOOPBUF_ENTRIES <= 8) ? 3 : 4;
  // Width used for declarations. At LOOPBUF_ENTRIES = 0 the vectors below
  // would be [-1:0]; XST rejects that ("Xst:678 Can not evaluate constant"),
  // so they keep one unused bit, which the generate blocks never write and
  // synthesis folds away.
  localparam LB_N = (LOOPBUF_ENTRIES == 0) ? 1 : LOOPBUF_ENTRIES;

  // Packed vectors, not arrays: `always @*` does not re-trigger on writes to
  // a Verilog memory in several simulators, so an array-based associative
  // lookup reads stale and never hits. Packed also keeps this in LUTs rather
  // than inferring a RAM, which is the point.
  reg [14*LB_N-1:0] lb_tag;
  reg [24*LB_N-1:0] lb_data;
  reg [LB_N-1:0]    lb_val;
  reg [LB_IDX-1:0] lb_next;
  initial begin lb_tag=0; lb_data=0; lb_val=0; lb_next=0; end

  // XST rejects a variable index into a packed vector ("Variable index is
  // not supported in signal"), so both the lookup and the write are built
  // with generate/genvar -- every index is then constant at elaboration.
  wire [LB_N-1:0] lb_match;
  wire [LB_N-1:0] lb_first;
  wire [23:0] lb_chain [0:LB_N];
  assign lb_chain[0] = 24'd0;

  genvar lbg;
  generate
    for(lbg = 0; lbg < LOOPBUF_ENTRIES; lbg = lbg + 1) begin : LB
      assign lb_match[lbg] = lb_val[lbg] && (lb_tag[14*lbg +: 14] == pc);
      // Priority: take the LOWEST matching entry, so duplicate entries can never
      // OR together into garbage.
      assign lb_first[lbg] = lb_match[lbg] & ~|(lb_match & ((1<<lbg)-1));
      assign lb_chain[lbg+1] = lb_chain[lbg]
                             | (lb_first[lbg] ? lb_data[24*lbg +: 24] : 24'd0);
    end
    // No entries: drive the unused bit so |lb_match is 0 rather than x.
    if(LOOPBUF_ENTRIES == 0) begin : LB_NONE
      assign lb_match = 1'b0;
      assign lb_first = 1'b0;
    end
  endgenerate

  wire        lb_hit_r    = |lb_match;
  wire [23:0] lb_hit_data = lb_chain[LOOPBUF_ENTRIES];
  wire lb_hit = (LOOPBUF_ENTRIES != 0) && lb_hit_r && enable;

  // Write port. lb_insert registers the request; the generate block below
  // applies it one cycle later, which is why the index is captured rather
  // than read from lb_next (which has already advanced).
  reg              pgm_wr_r;
  always @(posedge CLK) pgm_wr_r <= PGM_WR;

  reg              lb_wr;
  reg [LB_IDX-1:0] lb_wr_idx;
  reg [13:0]       lb_wr_addr;
  reg [23:0]       lb_wr_data;
  initial begin pgm_wr_r = 1'b0; lb_wr = 1'b0; lb_wr_idx = 0; lb_wr_addr = 0; lb_wr_data = 0; end

  generate
    for(lbg = 0; lbg < LOOPBUF_ENTRIES; lbg = lbg + 1) begin : LBW
      always @(posedge CLK) begin
        // Invalidate on PGM_WR, the cycle after it, and for the whole SRAM write
        // sequence: a fetch in flight or a registered lb_insert would otherwise land
        // afterwards with the pre-write word. extpgm_tb covers this.
        if(PGM_WR || pgm_wr_r || wr_busy)   lb_val[lbg] <= 1'b0;
        else if(lb_wr && (lb_wr_idx == lbg)) begin
          lb_tag [14*lbg +: 14] <= lb_wr_addr;
          lb_data[24*lbg +: 24] <= lb_wr_data;
          lb_val [lbg]          <= 1'b1;
        end
      end
    end
  endgenerate

  task lb_insert;
    input [13:0] a;
    input [23:0] d;
    begin
      if(LOOPBUF_ENTRIES != 0) begin
        lb_wr      <= 1'b1;
        lb_wr_idx  <= lb_next;
        lb_wr_addr <= a;
        lb_wr_data <= d;
        lb_next    <= lb_next + 1'b1;
      end
    end
  endtask

  // PSRAM program fetch (PGM_IN_PSRAM=1): word N at PGM_PSRAM_BASE + N*3.
  // UNUSED AND INCOMPLETE -- fails its own testbench. The Bus 2 SRAM path
  // (PGM_IN_PSRAM=0) is the verified one.
  parameter PGM_IN_PSRAM = 0;
  parameter [23:0] PGM_PSRAM_BASE = 24'hD00000;

  localparam P_IDLE = 3'd0, P_REQ0 = 3'd1, P_WAIT0 = 3'd2,
             P_REQ1 = 3'd3, P_WAIT1 = 3'd4, P_DONE = 3'd5;
  reg [2:0] pstate = P_IDLE;
  reg [15:0] pword0;
  reg [23:0] pbyte_addr;

  // Retry cap for READ_VERIFY: after MAX_RETRY disagreements the last read is
  // accepted, so a noisy read can never stall the DSP.
  localparam MAX_RETRY = 4;

  reg [13:0] vsum_addr = 14'd0;
  reg vsum_active = 1'b0;    // sweep in progress: drives pc_r instead of pc

  reg [23:0] rd_word_a;      // first read, awaiting confirmation
  reg verify_pass = 1'b0;    // 0 = first read, 1 = confirming read
  reg [2:0] retry_cnt = 3'd0;
  reg [15:0] verify_errors = 16'd0; // mismatches seen (diagnostic counter)

  // ---- PINNED TABLE + GENERAL CACHE ------------------------------------
  //
  // Two block RAMs, looked up in parallel from cache_raddr:
  //
  //   pin_data    256 x 25  {valid, word}          words 0..255 only
  //   cache_data  512 x 30  {valid, tag[4:0], word} direct-mapped on pc[8:0],
  //                                                 all other words
  //
  // Words 0..255 hold everything ST011 executes during a DMA transfer (0-2,
  // 31, 197-200, 243-247). They are prewarmed into their own table after the
  // download and nothing else can ever displace them. (With one shared
  // cache, a routine at words 12485-12490 evicted the inbound transfer loop
  // by aliasing, and the next transfer dropped a byte: the "freeze on
  // capturing a piece", found with sim/replay_timed_tb.v.)
  //
  // The general cache holds everything else and is filled on demand. Words
  // 256..511 index it with tag 0 and never collide with the pinned range.
  // Modelled over the full MesenCE trace, this pair misses less (8.0%) than
  // the previous single 4096-entry cache with pinning (10.2%) while using 2
  // block RAMs instead of 6 on mk2. Tight loops that still miss run from the
  // loop buffer after their first iteration.
  localparam CACHE_BITS = 9;                   // 512 entries
  localparam CACHE_TAG_BITS = 14 - CACHE_BITS; // 5
  localparam PIN_BITS = 8;                     // 256 words
  (* ram_style = "block" *)
  reg [CACHE_TAG_BITS+24:0] cache_data [0:(1<<CACHE_BITS)-1];
  (* ram_style = "block" *)
  reg [24:0]                pin_data   [0:(1<<PIN_BITS)-1];

  // Invalidation sweep: clears one general-cache entry and one pinned entry
  // (init_addr[7:0]) per clock after configuration -- block RAM has no reset.
  // Lives in the main always block so each array keeps one write port. Takes
  // 512 cycles, long before the first fetch; cache_ready gates hits until then.
  reg [CACHE_BITS-1:0] init_addr = {CACHE_BITS{1'b0}};
  reg cache_ready = 1'b0;

  // Write requests, funnelled into one write port per array at the end of
  // the always block (block RAM is only inferred with a single write address
  // expression). Blocking assignments.
  //   sweep_we  clear entry init_addr in both arrays
  //   inv_we    firmware write to wr_addr_r: invalidate its entry
  //   fill_we   fetched word fill_word for pc_r: pinned table if pc_r < 256,
  //             else general cache
  reg        sweep_we;
  reg        inv_we;
  reg        fill_we;
  reg [23:0] fill_word;
  reg                       c_we, p_we;     // resolved per-array write port
  reg [CACHE_BITS-1:0]      c_addr;
  reg [PIN_BITS-1:0]        p_addr;
  reg [CACHE_TAG_BITS+24:0] c_val;
  reg [24:0]                p_val;

  // ---- CACHE LOOKUP TIMING ------------------------------------------
  //
  // The lookup is issued every cycle from cache_raddr, which follows pc_early
  // during the core's STATE_STORE and pc otherwise. The result for the NEXT
  // instruction is registered before the core asks for it, so `ready` is true
  // on the first cycle of STATE_NEXT and a hit costs no stall cycles.
  wire [13:0] cache_raddr = pc_early_valid ? pc_early : pc;

  reg [CACHE_TAG_BITS+24:0] cache_rdata_e;   // registered general-cache read
  reg [24:0]                pin_rdata;       // registered pinned-table read
  reg [13:0] cache_q_pc = 14'h3fff; // address both registered reads belong to
  // Read-during-write guard. The registered reads are loaded at the top of
  // the always block while writes land at the bottom, so a lookup issued on
  // the cycle its own entry is written captures the PRE-write contents.
  // Treated like "not yet current": wait one cycle and re-check. extpgm_tb
  // has a cycle-aligned case for this.
  reg cache_q_dirty = 1'b0;
  wire cache_q_low = (cache_q_pc[13:PIN_BITS] == 0);
  wire [23:0] cache_rdata = cache_q_low ? pin_rdata[23:0] : cache_rdata_e[23:0];

  // Only believable when the registered lookup corresponds to the pc being
  // asked about. For one cycle after an address change it does not, and the
  // FSM must wait rather than treat it as a miss.
  wire cache_q_current = (cache_q_pc == pc) && !cache_q_dirty;
  wire cache_hit_now = (CACHE_ENABLE != 0)
                   && cache_ready      // no hits until the sweep has run
                   && cache_q_current
                   && (cache_q_low ? pin_rdata[24]                        // valid
                                   : (cache_rdata_e[CACHE_TAG_BITS+24]    // valid
                                      && (cache_rdata_e[CACHE_TAG_BITS+23:24]
                                          == pc[13:CACHE_BITS])));       // tag

  reg [13:0] pc_r;          // pc the in-flight (or most recent) read is for

  reg [13:0] pc_last_done = 14'h3fff; // "never fetched" sentinel;
                                       // guaranteed mismatch vs pc=0
  reg [23:0] dout_r;        // last completed demand fetch
  reg [7:0] byte0, byte1;

  // ---- ONE-SHOT CACHE PREWARM ----------------------------------------
  //
  // The first execution of any word costs a full external fetch (~32 cycles,
  // about one DMA byte slot), so a cold transfer loop drops a byte. Once the
  // firmware download has gone quiet, words 0..255 are walked through the
  // normal read path into the pinned table before the core may fetch.
  // `ready` is held low meanwhile (~0.2ms, before the game first uses the DSP).
  // PREWARM_ENABLE=0 removes it.
  parameter PREWARM_ENABLE = 1;
  localparam [15:0] PREWARM_WR_IDLE = 16'hffff; // ~0.68ms of no PGM_WR

  reg prewarm_armed  = 1'b0;  // a download has been seen since last warm
  reg prewarm_active = 1'b0;
  reg prewarm_done   = 1'b0;
  reg [PIN_BITS-1:0] prewarm_addr = {PIN_BITS{1'b0}};
  reg prewarm_abort  = 1'b0;
  reg [15:0] wr_idle_cnt = 16'd0;

  // Any PGM_WR pulse is captured immediately regardless of current state,
  // so a write request arriving mid-read is never silently dropped.
  reg wr_pending = 1'b0;
  reg [13:0] wr_pending_addr;
  reg [23:0] wr_pending_data;
  reg [13:0] wr_addr_r;
  reg [23:0] wr_data_r;

  reg [7:0] ram_data_out;
  reg ram_data_drive = 1'b0;

  // Two-stage synchronizer: RAM_DATA is driven by an external asynchronous
  // SRAM, so sampling it directly would be a metastability hazard (invisible
  // in simulation).
  reg [7:0] ram_data_s1;
  reg [7:0] ram_data_s2;
  always @(posedge CLK) begin
    ram_data_s1 <= RAM_DATA;
    ram_data_s2 <= ram_data_s1;
  end

  assign RAM_DATA = ram_data_drive ? ram_data_out : 8'bz;
  assign wr_busy = wr_pending | (state >= S_WR_ADDR0 && state <= S_WR_POST2);

  // Combinational, so `ready` drops the same cycle pc changes (cpu_wait=0 in
  // ST011). A registered ready would briefly show the previous fetch's result
  // for the new pc. A hit already registered by the lookup, or a loop-buffer
  // hit, is served in the same cycle.
  assign ready = ~enable
               | (((pc_last_done == pc) | cache_hit_now | lb_hit)
                  & ~wr_busy & ~prewarm_active);

  // Priority matters: pc_last_done is the authority whenever it matches,
  // because during the fill cycle of an external read the cache array is
  // being written at this very address and its read-during-write value
  // is not defined.
  assign dout = (pc_last_done == pc) ? dout_r
              : lb_hit                ? lb_hit_data
              :                         cache_rdata;

  wire pc_stale = enable && (pc_last_done != pc);

  // word address -> byte address (x3), via shift+add rather than a
  // pc_r * 3 (word -> byte address), registered alongside every pc_r load to
  // keep the carry chain off the pc_r -> RAM_ADDR path (mk2 timing).
  reg  [16:0] pc_r_byte0_r = 17'd0;
  wire [16:0] pc_r_byte0      = pc_r_byte0_r;
  wire [16:0] wr_addr_r_byte0 = {wr_addr_r, 1'b0} + wr_addr_r;

  /* PSRAM program fetch.
     Two 16-bit reads cover the three bytes of a word:
       addr+0 -> bytes 0,1     addr+2 -> byte 2 (low half)
     Byte order matches what the MCU stores, and the same reversal the
     SRAM path applies is applied here, so the CPU sees identical data
     either way. Runs only when PGM_IN_PSRAM=1; otherwise this block is
     inert and the SRAM path below is used unchanged. */
  always @(posedge CLK) begin
    sweep_we  = 1'b0;          // defaults: no write this cycle
    inv_we    = 1'b0;
    fill_we   = 1'b0;
    fill_word = 24'd0;
    if(PGM_IN_PSRAM != 0) begin
      // ---- PSRAM fetch path ----
      // Continuous cache lookup, same as the SRAM path below: `ready`
      // now consults cache_hit_now, so these registers must track a real
      // address on this path too rather than holding whatever they were
      // initialised to.
      cache_rdata_e <= cache_data[cache_raddr[CACHE_BITS-1:0]];
      pin_rdata     <= pin_data[cache_raddr[PIN_BITS-1:0]];
      cache_q_pc <= cache_raddr;
      // Firmware writes are still captured so the download path is
      // unaffected by where the program is read from.
      if(PGM_WR) begin
        wr_pending <= 1'b1;
        wr_pending_addr <= PGM_WR_ADDR;
        wr_pending_data <= PGM_DI;
      end
      // Cache invalidation sweep must run on this path too, or cache_ready
      // never asserts.
      if(!cache_ready) begin
        sweep_we = 1'b1;
        if(init_addr == {CACHE_BITS{1'b1}}) cache_ready <= 1'b1;
        else init_addr <= init_addr + 1'b1;
      end
      psram_rrq <= 1'b0;
      if(cache_ready)
      case(pstate)
        P_IDLE: begin
          if(enable && (pc_last_done != pc) && !wr_busy) begin
            pc_r <= pc;
            pc_r_byte0_r <= {pc, 1'b0} + pc;
            pbyte_addr <= PGM_PSRAM_BASE + {pc, 1'b0} + pc; // base + pc*3
            pstate <= P_REQ0;
          end
        end
        P_REQ0: begin
          psram_addr <= pbyte_addr;
          psram_rrq <= 1'b1;
          pstate <= P_WAIT0;
        end
        P_WAIT0: begin
          if(psram_rdy && !psram_rrq) begin
            pword0 <= psram_din;
            pstate <= P_REQ1;
          end
        end
        P_REQ1: begin
          psram_addr <= pbyte_addr + 24'd2;
          psram_rrq <= 1'b1;
          pstate <= P_WAIT1;
        end
        P_WAIT1: begin
          if(psram_rdy && !psram_rrq) begin
            // pword0 = {byte1, byte0}, psram_din[7:0] = byte2
            // reassembled with the same byte-order correction the SRAM
            // path uses, so the CPU sees the same instruction either way
            dout_r <= {pword0[7:0], pword0[15:8], psram_din[7:0]};
            lb_insert(pc_r, {pword0[7:0], pword0[15:8], psram_din[7:0]});
            fill_we = 1'b1; fill_word = {pword0[7:0], pword0[15:8], psram_din[7:0]};
            pc_last_done <= pc_r;
            pstate <= P_IDLE;
          end
        end
        default: pstate <= P_IDLE;
      endcase

    end else begin
    // ---- Bus 2 SRAM fetch path (PGM_IN_PSRAM=0) ----
    // lb_wr is a one-shot and must be cleared every cycle on this path.
    lb_wr <= 1'b0;


    // Continuous cache lookup. Issued every cycle, unconditionally, from
    // cache_raddr -- see the CACHE LOOKUP TIMING note above. Read and
    // write of these arrays stay in this one always block so Quartus
    // still infers simple dual-port block RAM; the write port is the
    // sweep/inv/fill funnel applied at the very end.
    cache_rdata_e <= cache_data[cache_raddr[CACHE_BITS-1:0]];
    pin_rdata     <= pin_data[cache_raddr[PIN_BITS-1:0]];
    cache_q_pc <= cache_raddr;

    // capture write requests unconditionally, every cycle -- no RST gating,
    // see module header comment for why
    if(PGM_WR) begin
      wr_pending <= 1'b1;
      wr_pending_addr <= PGM_WR_ADDR;
      wr_pending_data <= PGM_DI;
      // A download invalidates any prewarm: re-arm, and flag an abort
      // for one that happens to be running. The abort is deferred to
      // S_IDLE rather than clearing prewarm_active here, so a read
      // already in flight is never orphaned mid-sequence with the
      // completion path no longer recognising it as a prewarm word.
      prewarm_armed  <= 1'b1;
      prewarm_done   <= 1'b0;
      prewarm_abort  <= 1'b1;
      wr_idle_cnt <= 16'd0;
    end else if(wr_idle_cnt != PREWARM_WR_IDLE) begin
      wr_idle_cnt <= wr_idle_cnt + 1'b1;
    end

    if(!cache_ready) begin
      // Cache invalidation sweep (see cache_ready declaration above for
      // why this is a sweep rather than an initial-block loop, and why
      // it lives in this always block rather than its own). One entry
      // per clock; the state machine is held off until it completes.
      // PGM_WR capture above still runs, so nothing is lost meanwhile.
      sweep_we = 1'b1;
      if(init_addr == {CACHE_BITS{1'b1}}) cache_ready <= 1'b1;
      else init_addr <= init_addr + 1'b1;
    end else
    case(state)
      S_IDLE: begin
        RAM_OE <= 1'b1;
        RAM_WE <= 1'b1;
        ram_data_drive <= 1'b0;
        if(wr_pending) begin
          wr_addr_r <= wr_pending_addr;
          wr_data_r <= wr_pending_data;
          wr_pending <= 1'b0;
          state <= S_WR_ADDR0;
        end else if(vsum_start & ~vsum_busy) begin
          // begin readback sweep: walk every word, checksum what the
          // SRAM actually returns
          vsum <= 32'd0;
          vsum_addr <= 14'd0;
          vsum_busy <= 1'b1;
          vsum_active <= 1'b1;
          pc_r <= 14'd0;
          pc_r_byte0_r <= 17'd0;
          verify_pass <= 1'b0;
          retry_cnt <= 3'd0;
          state <= S_RD_ADDR0;
        end else if(vsum_busy) begin
          // next word of the sweep
          pc_r <= vsum_addr;
          pc_r_byte0_r <= {vsum_addr, 1'b0} + vsum_addr;
          vsum_active <= 1'b1;
          verify_pass <= 1'b0;
          retry_cnt <= 3'd0;
          state <= S_RD_ADDR0;
        end else if(prewarm_active) begin
          // Continue (or abandon) the one-shot prewarm. Sits below the
          // write and checksum branches so neither is ever delayed by
          // more than the single word currently in flight.
          if(prewarm_abort) begin
            prewarm_active <= 1'b0;
            prewarm_abort <= 1'b0;
          end else begin
            verify_pass <= 1'b0;
            retry_cnt <= 3'd0;
            state <= S_RD_ADDR0;
          end
        end else if(PREWARM_ENABLE != 0 && enable && prewarm_armed
                    && !prewarm_done && cache_ready
                    && (wr_idle_cnt == PREWARM_WR_IDLE)) begin
          // One-shot prewarm, taken in preference to the core's own
          // fetch: `ready` is gated by prewarm_active, so the core just
          // stalls until this finishes. See the PREWARM block above.
          prewarm_active <= 1'b1;
          prewarm_addr <= {PIN_BITS{1'b0}};
          pc_r <= 14'd0;
          pc_r_byte0_r <= 17'd0;
          verify_pass <= 1'b0;
          retry_cnt <= 3'd0;
          state <= S_RD_ADDR0;
        end else if(pc_stale) begin
          // Fresh fetch: clear any half-finished verify state. The
          // re-read path jumps straight to S_RD_ADDR0 and never passes
          // through here, so this only ever resets an abandoned pass
          // (e.g. one interrupted by a firmware write taking priority).
          verify_pass <= 1'b0;
          retry_cnt <= 3'd0;
          if(PGM_WR) begin
            // A write is landing on this very edge. Do not start
            // anything; it was captured into wr_pending above and the
            // wr_pending branch will service it next cycle, after which
            // pc_stale is still true and the fetch re-issues naturally
            // against post-write data.
            state <= S_IDLE;
          end else if(CACHE_ENABLE != 0 && !cache_q_current) begin
            // The lookup has not caught up with this pc yet (pc_early not driven).
            // Wait one cycle rather than treat an unrelated tag as a miss.
            state <= S_IDLE;
          end else if(CACHE_ENABLE != 0 && cache_hit_now) begin
            // Fast path: no external SRAM access at all. `ready` and
            // `dout` are already serving this from cache_rdata
            // combinationally; this just latches it so the value stays
            // held once the lookup address moves on.
            //
            // PGM_WR is checked LIVE, not via the registered wr_pending:
            // wr_pending is assigned non-blocking, so it still reads 0
            // during the very edge a write arrives. Without this a write
            // landing on this edge for the address being checked would
            // let the hit serve the stale pre-write value. A write that
            // arrived strictly earlier was already taken by the
            // wr_pending branch above and never reaches here.
            dout_r <= cache_rdata;
            pc_last_done <= pc;
            lb_insert(pc, cache_rdata);
            state <= S_IDLE;
          end else begin
            // Miss: external SRAM fetch. pc*3 is computed from pc_r in
            // S_RD_CALC, one cycle later: computing it from pc here put the
            // core's next-pc mux (retimed by XST register balancing) in front
            // of the adder, the worst mk2 path (TS_CLK21 -0.774ns). Costs one
            // cycle per miss.
            pc_r <= pc;
            state <= S_RD_CALC;
          end
        end
      end

      // ---- write byte 0 (bits 7:0) ----
      S_WR_ADDR0: begin
        // address presented, WE still deasserted: basic address-setup
        // margin before the write pulse
        RAM_ADDR <= {2'b0, wr_addr_r_byte0};
        ram_data_out <= wr_data_r[7:0];
        ram_data_drive <= 1'b1;
        RAM_WE <= 1'b1;
        hold_cnt <= HOLD_CYCLES;
        state <= S_WR_HOLD0;
      end
      S_WR_HOLD0: begin
        RAM_WE <= 1'b0; // assert
        if(hold_cnt == 0) begin
          RAM_WE <= 1'b1; // deassert -- write pulse complete
          post_cnt <= POST_CYCLES;
          state <= S_WR_POST0;
        end else hold_cnt <= hold_cnt - 1;
      end
      S_WR_POST0: begin
        // address/data held stable a little longer (tAH/tDH margin) --
        // nothing here changes RAM_ADDR/ram_data_out
        if(post_cnt == 0) state <= S_WR_ADDR1;
        else post_cnt <= post_cnt - 1;
      end
      // ---- write byte 1 (bits 15:8) ----
      S_WR_ADDR1: begin
        RAM_ADDR <= {2'b0, RAM_ADDR[16:0] + 17'd1}; // = byte0 + 1 (set in S_WR_ADDR0)
        ram_data_out <= wr_data_r[15:8];
        RAM_WE <= 1'b1;
        hold_cnt <= HOLD_CYCLES;
        state <= S_WR_HOLD1;
      end
      S_WR_HOLD1: begin
        RAM_WE <= 1'b0;
        if(hold_cnt == 0) begin
          RAM_WE <= 1'b1;
          post_cnt <= POST_CYCLES;
          state <= S_WR_POST1;
        end else hold_cnt <= hold_cnt - 1;
      end
      S_WR_POST1: begin
        if(post_cnt == 0) state <= S_WR_ADDR2;
        else post_cnt <= post_cnt - 1;
      end
      // ---- write byte 2 (bits 23:16) ----
      S_WR_ADDR2: begin
        RAM_ADDR <= {2'b0, RAM_ADDR[16:0] + 17'd1}; // = byte0 + 2
        ram_data_out <= wr_data_r[23:16];
        RAM_WE <= 1'b1;
        hold_cnt <= HOLD_CYCLES;
        state <= S_WR_HOLD2;
      end
      S_WR_HOLD2: begin
        RAM_WE <= 1'b0;
        if(hold_cnt == 0) begin
          RAM_WE <= 1'b1;
          post_cnt <= POST_CYCLES;
          state <= S_WR_POST2;
        end else hold_cnt <= hold_cnt - 1;
      end
      S_WR_POST2: begin
        if(post_cnt == 0) begin
          ram_data_drive <= 1'b0;
          // A write landed: invalidate the last fetch result. (The DSP is held in
          // reset during download, so this is a safeguard.)
          pc_last_done <= 14'h3fff;
          // and its cache entry (no tag check needed).
          inv_we = 1'b1;
          state <= S_IDLE;
        end else post_cnt <= post_cnt - 1;
      end

      // ---- read the three bytes of one word ----
      //
      // OE is asserted once and held low across all three bytes: only the address
      // changes, so the bus never floats between bytes and each byte needs only
      // address-access time.
      S_RD_CALC: begin
        pc_r_byte0_r <= {pc_r, 1'b0} + pc_r;
        state <= S_RD_ADDR0;
      end
      S_RD_ADDR0: begin
        RAM_ADDR <= {2'b0, pc_r_byte0};
        RAM_OE <= 1'b0; // assert once, stays low for the whole word
        hold_cnt <= HOLD_CYCLES;
        state <= S_RD_HOLD0;
      end
      S_RD_HOLD0: begin
        RAM_OE <= 1'b0; // held
        if(hold_cnt == 0) begin
          byte0 <= ram_data_s2; // synchronized (2-stage), not raw RAM_DATA
          state <= S_RD_ADDR1;
        end else hold_cnt <= hold_cnt - 1;
      end
      S_RD_ADDR1: begin
        RAM_ADDR <= {2'b0, RAM_ADDR[16:0] + 17'd1}; // = byte0 + 1 (set in S_RD_ADDR0)
        RAM_OE <= 1'b0; // held -- no float between bytes
        hold_cnt <= HOLD_CYCLES;
        state <= S_RD_HOLD1;
      end
      S_RD_HOLD1: begin
        RAM_OE <= 1'b0;
        if(hold_cnt == 0) begin
          byte1 <= ram_data_s2;
          state <= S_RD_ADDR2;
        end else hold_cnt <= hold_cnt - 1;
      end
      S_RD_ADDR2: begin
        RAM_ADDR <= {2'b0, RAM_ADDR[16:0] + 17'd1}; // = byte0 + 2
        RAM_OE <= 1'b0; // held
        hold_cnt <= HOLD_CYCLES;
        state <= S_RD_HOLD2;
      end
      S_RD_HOLD2: begin
        RAM_OE <= 1'b0;
        if(hold_cnt == 0) begin
          RAM_OE <= 1'b1; // release only now, after the whole word
          // Byte-order correction: PGM_DI is byte-reversed relative to the true
          // instruction. See also upd77c25.v dat_doutb_fixed.
          if(prewarm_active) begin
            // Prewarm word complete: fill the entry and step on. Bypasses
            // dout_r/pc_last_done (the core is stalled). Single read, no READ_VERIFY.
            fill_we = 1'b1; fill_word = {byte0, byte1, ram_data_s2};
            verify_pass <= 1'b0;
            if(prewarm_addr == {PIN_BITS{1'b1}}) begin
              prewarm_active <= 1'b0;
              prewarm_done <= 1'b1;
            end else begin
              prewarm_addr <= prewarm_addr + 1'b1;
              pc_r <= {{(14-PIN_BITS){1'b0}}, prewarm_addr} + 14'd1;
              pc_r_byte0_r <= {({{(14-PIN_BITS){1'b0}}, prewarm_addr} + 14'd1), 1'b0}
                            + ({{(14-PIN_BITS){1'b0}}, prewarm_addr} + 14'd1);
            end
            state <= S_IDLE;
          end else if(vsum_active) begin
            // Sweep word complete: accumulate and move on. Deliberately
            // bypasses dout/cache/pc_last_done so the sweep leaves no
            // trace on normal fetch state.
            // Accumulate the RAW STORED word, i.e. PGM_DI as the MCU sent
            // it ({SRAM[+2],SRAM[+1],SRAM[+0]}), NOT the byte-corrected
            // instruction -- so this is directly comparable with the
            // checksum load_dspx computes while sending.
            vsum <= vsum + {8'd0, ram_data_s2, byte1, byte0};
            vsum_active <= 1'b0;
            verify_pass <= 1'b0;
            if(vsum_addr == 14'd16383) begin
              vsum_busy <= 1'b0;   // sweep finished
            end else begin
              vsum_addr <= vsum_addr + 1'b1;
            end
            state <= S_IDLE;
          end else if(READ_VERIFY == 0) begin
            dout_r <= {byte0, byte1, ram_data_s2};
            pc_last_done <= pc_r;
            lb_insert(pc_r, {byte0, byte1, ram_data_s2});
            fill_we = 1'b1; fill_word = {byte0, byte1, ram_data_s2};
            state <= S_IDLE;
          end else if(!verify_pass) begin
            // First read of this word: stash it and read the same word
            // again, rather than committing straight away.
            rd_word_a <= {byte0, byte1, ram_data_s2};
            verify_pass <= 1'b1;
            state <= S_RD_ADDR0;
          end else if({byte0, byte1, ram_data_s2} == rd_word_a) begin
            // Two consecutive reads agree -- commit.
            dout_r <= rd_word_a;
            pc_last_done <= pc_r;
            lb_insert(pc_r, rd_word_a);
            fill_we = 1'b1; fill_word = {rd_word_a[23:16], rd_word_a[15:0]};
            verify_pass <= 1'b0;
            state <= S_IDLE;
          end else if(retry_cnt >= MAX_RETRY) begin
            // Too many disagreements. Accept the latest read rather than
            // retrying forever -- a stalled fetch hangs the DSP outright,
            // which is worse than an occasional wrong word.
            verify_errors <= verify_errors + 1'b1;
            dout_r <= {byte0, byte1, ram_data_s2};
            pc_last_done <= pc_r;
            lb_insert(pc_r, {byte0, byte1, ram_data_s2});
            fill_we = 1'b1; fill_word = {byte0, byte1, ram_data_s2};
            verify_pass <= 1'b0;
            retry_cnt <= 3'd0;
            state <= S_IDLE;
          end else begin
            // Disagreement: at least one of the two reads was wrong.
            // Start over rather than guessing which. Counted so the
            // rate is observable if a readback path is ever added.
            verify_errors <= verify_errors + 1'b1;
            retry_cnt <= retry_cnt + 1'b1;
            verify_pass <= 1'b0;
            state <= S_RD_ADDR0;
          end
        end else hold_cnt <= hold_cnt - 1;
      end

      default: state <= S_IDLE;
    endcase
    end

    // Write ports: one address, one data, one enable per array.
    // Sweep and invalidate write a cleared (valid=0) entry; the data bits of
    // an invalid entry are don't-care.
    begin
      c_we = 1'b0; p_we = 1'b0;
      c_addr = {CACHE_BITS{1'b0}}; p_addr = {PIN_BITS{1'b0}};
      c_val = {(CACHE_TAG_BITS+25){1'b0}}; p_val = 25'd0;
      if(sweep_we) begin
        c_we = 1'b1; c_addr = init_addr;
        p_we = 1'b1; p_addr = init_addr[PIN_BITS-1:0];
      end else if(inv_we) begin
        c_we = 1'b1; c_addr = wr_addr_r[CACHE_BITS-1:0];
        p_we = (wr_addr_r[13:PIN_BITS] == 0); p_addr = wr_addr_r[PIN_BITS-1:0];
      end else if(fill_we) begin
        if(pc_r[13:PIN_BITS] == 0) begin
          p_we = 1'b1; p_addr = pc_r[PIN_BITS-1:0]; p_val = {1'b1, fill_word};
        end else begin
          c_we = 1'b1; c_addr = pc_r[CACHE_BITS-1:0];
          c_val = {1'b1, pc_r[13:CACHE_BITS], fill_word};
        end
      end
      if(c_we) cache_data[c_addr] <= c_val;
      if(p_we) pin_data[p_addr]   <= p_val;
      // Deliberately pessimistic: any write this cycle marks the registered
      // read stale, without comparing addresses. The address comparators used
      // to sit behind the write-port priority mux and were the worst mk2 path
      // (post_cnt -> cache_q_dirty, TS_CLK21 -2.209ns). The cost is one extra
      // cycle on the rare lookup that coincides with a fill, a PGM_WR
      // invalidate or the power-on sweep.
      cache_q_dirty <= c_we || p_we;
    end
  end

endmodule
