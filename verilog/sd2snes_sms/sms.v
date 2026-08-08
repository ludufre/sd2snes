// sd2snes SMS core — SMS subsystem (Z80 + Sega mapper + WRAM + VDP capture).
//
// RTL port of the validated C behavioral model (ciclone sms_model.cpp): the Z80
// runs the SMS ROM (from PSRAM via rom_*) and the VDP captures VRAM/CRAM/regs,
// mirroring the C model that boots Sonic. The SMS->SNES translation bridge (M7.3)
// and the main.v PSRAM/SNES integration (M7.4) come next; VRAM/CRAM are exposed
// via registered read ports for that.
//
// BRAM-friendly (M7.2b): WRAM and VRAM are registered-read so Quartus infers M9K;
// the Z80 is stalled via wait_n for the extra read latency (ROM = PSRAM multi-cyc,
// WRAM = 1 cyc). The Sega mapper bank registers live in dedicated FFs (combinational
// for address mapping) and are mirrored into WRAM for read-back. vcounter + frame
// IRQ come from a free-running 228-cyc/line, 262-line counter (enough for Sonic's
// boot V==$B0 wait + per-frame vint). TODO_SMS_COMPAT: exact VDP timing, line IRQ,
// sprite zoom. cen=1 here (Z80 every clk); real SMS-rate pacing is M7.4.
module sms (
  input         CLK,
  input         RST,
  input         CE,            // SMS clock-enable (~3.58MHz). CE=1 = full-speed (sim).

  // ROM read interface to PSRAM (serviced by the arbiter/host)
  output reg        ROM_RRQ,
  output reg [23:0] ROM_ADDR,
  input      [7:0]  ROM_DATA,
  input             ROM_RDY,

  // SMS controller port $DC (active low; bit0..5 = U D L R B1 B2)
  input      [7:0]  PAD1,

  // translation read ports (M7.3) — registered (1-cycle)
  input      [13:0] TR_VRAM_ADDR,
  output reg [7:0]  TR_VRAM_DATA,
  input      [4:0]  TR_CRAM_ADDR,
  output     [7:0]  TR_CRAM_DATA,

  // debug / status
  output     [15:0] DBG_PC,
  output     [7:0]  DBG_BANK0,
  output     [7:0]  DBG_BANK1,
  output     [7:0]  DBG_BANK2,
  output reg [31:0] DBG_VRAM_WRITES,
  output reg [31:0] DBG_REG_WRITES,
  output reg [31:0] DBG_CRAM_WRITES,
  output     [7:0]  DBG_REG1,
  output     [7:0]  DBG_REG2,
  output            DBG_DISPLAY_ON,
  // for the translation engine + frame sync (M7.4 integration)
  output     [127:0] VREGS,        // all 16 VDP registers, flat (reg0=[7:0]..reg15=[127:120])
  output            FRAME_TICK,    // 1-cycle pulse at start of vblank (line 192)
  output reg [7:0]  DIRTY_LO_MIN,  // changed-tile range in the LOW half (tiles 0..255 = BG)
  output reg [7:0]  DIRTY_LO_MAX,  //   (empty range: MIN>MAX)
  output reg [7:0]  DIRTY_HI_MIN,  // changed-tile range in the HIGH half (tiles 256..511 = sprites),
  output reg [7:0]  DIRTY_HI_MAX,  //   index is within the half (absolute tile = 256+index)
  input             DIRTY_SNAP,    // pulse at translation start: publish live ranges + reset them
  input             BACK_SET,      // which double-buffer set is being written this pass (= ~sms_buf_front)
  output reg [31:0] DIRTY_COLS,    // per-set dirty name-table COLUMN bitmap (1 = re-emit that column)
  input             DBG_FORCE_FULL,// test: publish all 32 columns dirty (full tilemap emit)
  output [1:0]      DBG_WAIT,      // Z80 wait-stall live: bit0=ROM read, bit1=WRAM read
  // audio: unipolar PSG mix (0..1020) + the chip's internal 223.72kHz tick.
  // Amplitude conditioning for the DAC lives in sms_core.v (the wrap), not here.
  output [10:0]     PSG_MIX,
  output            PSG_TICK
);

  // ---------------- Z80 ----------------
  wire        m1_n, mreq_n, iorq_n, rd_n, wr_n, rfsh_n, halt_n, busak_n;
  wire [15:0] A;
  wire [7:0]  dout;
  reg  [7:0]  di;
  reg         wait_n;
  wire        int_n;

  tv80s_ce #(.Mode(0)) cpu (
    .reset_n (~RST), .clk (CLK), .cen (CE), .wait_n (wait_n),
    .int_n (int_n), .nmi_n (1'b1), .busrq_n (1'b1),
    .m1_n (m1_n), .mreq_n (mreq_n), .iorq_n (iorq_n), .rd_n (rd_n), .wr_n (wr_n),
    .rfsh_n (rfsh_n), .halt_n (halt_n), .busak_n (busak_n),
    .A (A), .di (di), .dout (dout)
  );

  // ---------------- memories (BRAM-inferred) ----------------
  reg [7:0] wram [0:8191];      // 8 KB WRAM (registered read)
  reg [7:0] vram [0:16383];     // 16 KB VDP VRAM (registered reads)
  reg [7:0] cram [0:31];        // 32 B CRAM (small -> logic)
  reg [7:0] vreg [0:15];        // VDP registers (logic)

  // Sega mapper bank registers in dedicated FFs (combinational for mapping)
  reg [7:0] bk_ctrl, bk0, bk1, bk2;
  assign DBG_BANK0 = bk0;
  assign DBG_BANK1 = bk1;
  assign DBG_BANK2 = bk2;
  assign DBG_REG1  = vreg[1];
  assign DBG_REG2  = vreg[2];
  assign DBG_DISPLAY_ON = vreg[1][6];
  assign TR_CRAM_DATA = cram[TR_CRAM_ADDR];

  // ---------------- free-running line/frame timing ----------------
  reg [8:0] line;
  reg [7:0] dot;
  reg       vblank_flag;
  reg       line_tick;
  always @(posedge CLK) begin
    line_tick <= 1'b0;
    if (RST) begin dot <= 0; line <= 0; end
    else if (CE) begin                       // line/dot advance at the SMS rate
      if (dot == 8'd227) begin
        dot <= 0;
        line_tick <= 1'b1;
        line <= (line == 9'd261) ? 9'd0 : (line + 1'b1);
      end else dot <= dot + 1'b1;
    end
  end
  wire [7:0] vcounter = (line <= 9'h0DA) ? line[7:0] : (line[7:0] - 8'd6);
  wire frame_start = line_tick && (line == 9'd191);
  assign int_n = ~(vblank_flag & vreg[1][5]);
  // translation trigger = start of ACTIVE display (line wrap), NOT vblank start:
  // the game has just finished its vblank VDP writes -> the VDP state is a fresh,
  // consistent frame, and the pass runs while the game does (no CE freeze needed;
  // rare mid-pass writes self-heal via the per-set dirty accumulators next pass).
  assign FRAME_TICK = line_tick && (line == 9'd0);
  assign VREGS = {vreg[15],vreg[14],vreg[13],vreg[12],vreg[11],vreg[10],vreg[9],vreg[8],
                  vreg[7],vreg[6],vreg[5],vreg[4],vreg[3],vreg[2],vreg[1],vreg[0]};

  // ---------------- address decode ----------------
  wire mem_rd = ~mreq_n & ~rd_n;
  wire io_rd  = ~iorq_n & ~rd_n;
  wire is_rom = A < 16'hC000;            // 0x0000-0xBFFF -> ROM (Sega mapper)
  wire rom_rd  = mem_rd & is_rom;
  wire wram_rd = mem_rd & ~is_rom;       // 0xC000-0xFFFF -> WRAM

  reg [23:0] map_addr;
  always @* begin
    if      (A < 16'h0400) map_addr = {8'd0, A};
    else if (A < 16'h4000) map_addr = (bk0 * 24'h4000) + {8'd0, A};
    else if (A < 16'h8000) map_addr = (bk1 * 24'h4000) + {8'd0, (A - 16'h4000)};
    else                   map_addr = (bk2 * 24'h4000) + {8'd0, (A - 16'h8000)};
  end

  // ---------------- ROM read: streaming prefetch FIFO ----------------
  // Each PSRAM fetch via the SNES-shared arbiter has a long latency (free-slot wait +
  // access) -> a Z80 that fetches one-byte-at-a-time eats wait states -> ~half speed.
  // The arbiter THROUGHPUT (≈1 byte / SNES cycle) easily exceeds the Z80's read rate,
  // so we stream sequential ROM bytes AHEAD of the Z80 into a small FIFO: the Z80's
  // (mostly sequential) fetches then pop with zero wait. A jump / bank cross makes the
  // requested addr != the FIFO head addr -> flush and re-stream from the new address.
  localparam [2:0] PF_N = 3'd4;
  reg  [7:0]  pf_buf [0:3];
  reg  [1:0]  pf_head;
  reg  [2:0]  pf_count;
  reg  [23:0] pf_head_addr;     // mapped addr of pf_buf[pf_head]
  reg  [23:0] pf_next_addr;     // next mapped addr to prefetch (== head_addr + count)
  reg         pf_busy, pf_kill; // a prefetch is in flight / discard it (stale after a flush)
  reg         rom_have;
  reg  [7:0]  rom_lat;

  wire        pf_aligned = (pf_head_addr == map_addr);
  wire        ld_hit  = rom_rd & ~rom_have & pf_aligned & (pf_count != 3'd0);
  wire        ld_miss = rom_rd & ~rom_have & ~pf_aligned;     // jump / bank cross
  wire        pf_done = pf_busy & ROM_RDY;
  wire        do_push = pf_done & ~pf_kill & ~ld_miss;        // commit a fetched byte
  wire [1:0]  pf_tail = pf_head + pf_count[1:0];
  wire [2:0]  cnt_aft = ld_miss ? 3'd0
                       : (pf_count + (do_push?3'd1:3'd0) - (ld_hit?3'd1:3'd0));
  wire [23:0] nxt_aft = ld_miss ? map_addr
                       : (do_push ? pf_next_addr + 24'd1 : pf_next_addr);
  wire        pf_free = ~pf_busy | pf_done;
  wire        do_issue = pf_free & (cnt_aft < PF_N);

  always @(posedge CLK) begin
    if (RST) begin
      pf_head <= 2'd0; pf_count <= 3'd0; pf_head_addr <= 24'd0; pf_next_addr <= 24'd0;
      pf_busy <= 1'b0; pf_kill <= 1'b0; rom_have <= 1'b0; ROM_RRQ <= 1'b0;
    end else begin
      ROM_RRQ <= 1'b0;
      if (~rom_rd) rom_have <= 1'b0;                  // Z80 done -> arm the next read
      if (ld_hit) begin rom_lat <= pf_buf[pf_head]; rom_have <= 1'b1; end
      if (do_push) pf_buf[pf_tail] <= ROM_DATA;
      if (ld_miss) begin                              // flush + re-stream from map_addr
        pf_count <= 3'd0; pf_head <= 2'd0;
        pf_head_addr <= map_addr; pf_next_addr <= map_addr;
        if (pf_busy & ~ROM_RDY) pf_kill <= 1'b1;      // the in-flight fetch is now stale
      end else begin
        if (ld_hit) begin pf_head <= pf_head + 2'd1; pf_head_addr <= pf_head_addr + 24'd1; end
        pf_count <= pf_count + (do_push?3'd1:3'd0) - (ld_hit?3'd1:3'd0);
        if (do_push) pf_next_addr <= pf_next_addr + 24'd1;
      end
      if (pf_done) begin pf_busy <= 1'b0; pf_kill <= 1'b0; end
      if (do_issue) begin ROM_RRQ <= 1'b1; ROM_ADDR <= nxt_aft; pf_busy <= 1'b1; end
    end
  end

  // ---------------- WRAM: clean single-port BRAM template ----------------
  // addr = A[12:0]; write on wram_we (CPU store to 0xC000-0xFFFF); read every cyc.
  wire wram_we = wr_edge & iorq_n & ~mreq_n & (A >= 16'hC000);  // memory write (iorq_n HIGH)
  reg [7:0] wram_q;
  reg       wram_rd_d;
  always @(posedge CLK) begin
    if (wram_we) wram[A[12:0]] <= dout;
    wram_q <= wram[A[12:0]];
    wram_rd_d <= wram_rd;
  end
  wire wram_have = wram_rd_d;

  // ---------------- VRAM: clean true-dual-port BRAM template ----------------
  // Port A = CPU at vdp_addr (R/W); Port B = translation read at TR_VRAM_ADDR.
  // vram_q (port A read) feeds the VDP read buffer. vram_we = data-port write,
  // non-CRAM. (vdp_addr/vdp_code are maintained in the VDP block below.)
  wire vram_we = wr_edge & ~iorq_n & (A[7:6] == 2'b10) & ~A[0] & (vdp_code != 2'b11);
  reg [7:0] vram_q;
  always @(posedge CLK) begin
    if (vram_we) vram[vdp_addr] <= dout;
    vram_q       <= vram[vdp_addr];
    TR_VRAM_DATA <= vram[TR_VRAM_ADDR];
  end

  // dirty-tile RANGES (two halves), PER DOUBLE-BUFFER SET: a pattern-table write extends
  // the live range for its half. SMS BG tiles sit in the LOW half (0..255), sprite tiles
  // in the HIGH half (256..511, reg6 bit2) -> tracking each half separately keeps both
  // spans narrow. CRITICAL: the FPGA writes a DIFFERENT physical set each frame (the
  // double-buffer alternates), so a set written this frame must carry the union of every
  // change since IT was last written (2 frames ago), not just this frame's. We keep TWO
  // accumulators per range; every tile write extends BOTH; DIRTY_SNAP publishes + resets
  // ONLY the set being written this pass (BACK_SET = ~sms_buf_front). Resetting a single
  // live range every frame (the old bug) dropped the other set's pending dirt -> alternate-
  // frame tile flicker. A write overrides its own set's coincident reset so it's not lost.
  wire [13:0] nt_base_w = {vreg[2][3:1], 11'd0};
  wire [8:0]  wtile = vdp_addr[13:5];        // SMS tile = byte/32 (0..511)
  wire        whalf = wtile[8];              // 0 = low half (BG), 1 = high half (sprites)
  wire [7:0]  widx  = wtile[7:0];            // index within the half
  wire        wr_tile = vram_we & (vdp_addr < nt_base_w);
  // EXPLICIT per-set regs (no array / no variable-index write -> no Quartus inference
  // ambiguity). set 0 and set 1 each hold a low + high range. DIRTY_SNAP publishes the
  // BACK_SET via a plain 2:1 mux and resets only that set (via snap0/snap1); a tile write
  // extends BOTH sets, and for the set being reset this cycle it lands in the freshly-reset
  // range (snapN term in the write).
  reg  [7:0]  lo_min0, lo_max0, hi_min0, hi_max0;   // set 0 live ranges
  reg  [7:0]  lo_min1, lo_max1, hi_min1, hi_max1;   // set 1 live ranges
  wire        snap0 = DIRTY_SNAP & ~BACK_SET;       // publish+reset set 0 this pass
  wire        snap1 = DIRTY_SNAP &  BACK_SET;
  wire        wlo   = wr_tile & ~whalf;             // low-half (BG) tile write
  wire        whi   = wr_tile &  whalf;             // high-half (sprite) tile write
  always @(posedge CLK) begin
    if (RST) begin
      DIRTY_LO_MIN <= 8'd0; DIRTY_LO_MAX <= 8'd255;   // force a full first upload, both halves
      DIRTY_HI_MIN <= 8'd0; DIRTY_HI_MAX <= 8'd255;
      lo_min0 <= 8'd0; lo_max0 <= 8'd255; hi_min0 <= 8'd0; hi_max0 <= 8'd255;
      lo_min1 <= 8'd0; lo_max1 <= 8'd255; hi_min1 <= 8'd0; hi_max1 <= 8'd255;
    end else begin
      if (DIRTY_SNAP) begin                            // publish the BACK_SET (2:1 mux)
        DIRTY_LO_MIN <= BACK_SET ? lo_min1 : lo_min0;  DIRTY_LO_MAX <= BACK_SET ? lo_max1 : lo_max0;
        DIRTY_HI_MIN <= BACK_SET ? hi_min1 : hi_min0;  DIRTY_HI_MAX <= BACK_SET ? hi_max1 : hi_max0;
      end
      // set 0 (a write extends; snap0 resets; snap0+write -> freshly reset to widx)
      if (wlo) lo_min0 <= (snap0 | (widx < lo_min0)) ? widx : lo_min0; else if (snap0) lo_min0 <= 8'd255;
      if (wlo) lo_max0 <= (snap0 | (widx > lo_max0)) ? widx : lo_max0; else if (snap0) lo_max0 <= 8'd0;
      if (whi) hi_min0 <= (snap0 | (widx < hi_min0)) ? widx : hi_min0; else if (snap0) hi_min0 <= 8'd255;
      if (whi) hi_max0 <= (snap0 | (widx > hi_max0)) ? widx : hi_max0; else if (snap0) hi_max0 <= 8'd0;
      // set 1
      if (wlo) lo_min1 <= (snap1 | (widx < lo_min1)) ? widx : lo_min1; else if (snap1) lo_min1 <= 8'd255;
      if (wlo) lo_max1 <= (snap1 | (widx > lo_max1)) ? widx : lo_max1; else if (snap1) lo_max1 <= 8'd0;
      if (whi) hi_min1 <= (snap1 | (widx < hi_min1)) ? widx : hi_min1; else if (snap1) hi_min1 <= 8'd255;
      if (whi) hi_max1 <= (snap1 | (widx > hi_max1)) ? widx : hi_max1; else if (snap1) hi_max1 <= 8'd0;
    end
  end

  // dirty name-table COLUMN bitmap (per set): a name-table write dirties its cell's column;
  // a coarse V-scroll change (reg9[7:3]) shifts the row windowing for EVERY cell -> dirty all
  // 32 columns. Accumulated per set (2-frame, like the tile ranges); the translate FSM then
  // re-emits only dirty columns instead of the full 2KB tilemap every frame.
  wire        wr_nt = vram_we & (vdp_addr >= nt_base_w) & (vdp_addr < (nt_base_w + 14'h700));
  wire [4:0]  wcol  = vdp_addr[5:1];                   // column (nt_base is 0x800-aligned)
  reg  [4:0]  reg9row_p;
  wire        reg9_chg = (vreg[9][7:3] != reg9row_p);  // coarse V-scroll changed
  wire [31:0] col_set  = (wr_nt ? (32'd1 << wcol) : 32'd0) | (reg9_chg ? 32'hFFFFFFFF : 32'd0);
  reg  [31:0] cols0, cols1;
  always @(posedge CLK) begin
    if (RST) begin
      DIRTY_COLS <= 32'hFFFFFFFF; cols0 <= 32'hFFFFFFFF; cols1 <= 32'hFFFFFFFF; reg9row_p <= 5'd0;
    end else begin
      reg9row_p <= vreg[9][7:3];
      if (DIRTY_SNAP) DIRTY_COLS <= DBG_FORCE_FULL ? 32'hFFFFFFFF : (BACK_SET ? cols1 : cols0);
      cols0 <= (snap0 ? 32'd0 : cols0) | col_set;     // accumulate; publish+clear set 0 on snap0
      cols1 <= (snap1 ? 32'd0 : cols1) | col_set;
    end
  end

  // ---------------- VDP port state ----------------
  reg [13:0] vdp_addr;
  reg [1:0]  vdp_code;
  reg [7:0]  vdp_latch_lo;
  reg        vdp_latch;
  reg [7:0]  vdp_readbuf;

  // I/O read data (combinational; side effects on strobes below)
  reg [7:0] io_data;
  always @* begin
    case (A[7:6])
      2'b01: io_data = vcounter;
      2'b10: io_data = A[0] ? {vblank_flag, 7'h00} : vdp_readbuf;
      2'b11: io_data = A[0] ? 8'hFF : PAD1;
      default: io_data = 8'hFF;
    endcase
  end

  // di mux + wait
  always @* begin
    if (io_rd)        di = io_data;
    else if (is_rom)  di = rom_lat;
    else              di = wram_q;
  end
  always @* wait_n = ((rom_rd & ~rom_have) | (wram_rd & ~wram_have)) ? 1'b0 : 1'b1;
  assign DBG_WAIT = {(wram_rd & ~wram_have), (rom_rd & ~rom_have)};

  // ---------------- bus strobes ----------------
  reg rd_a_d, wr_a_d;
  wire rd_edge = (~rd_n) & ~rd_a_d;
  wire wr_edge = (~wr_n) & ~wr_a_d;
  always @(posedge CLK) begin rd_a_d <= ~rd_n; wr_a_d <= ~wr_n; end

  assign DBG_PC = A;

  // ---------------- PSG (SN76489) ----------------
  // The SMS decodes ANY I/O write in $40-$7F to the sound chip. Same
  // wr_edge + iorq_n qualification the VDP/WRAM write templates use.
  wire psg_we = wr_edge & ~iorq_n & (A[7:6] == 2'b01);

  psg u_psg (
    .CLK(CLK), .RST(RST), .CE(CE),
    .WE(psg_we), .D(dout),
    .MIX(PSG_MIX), .TICK(PSG_TICK)
  );

  // ---------------- VDP/WRAM writes, mapper, VDP read side effects ----------------
  always @(posedge CLK) begin
    if (RST) begin
      vdp_addr <= 0; vdp_code <= 0; vdp_latch <= 0; vdp_latch_lo <= 0; vdp_readbuf <= 0;
      vblank_flag <= 0;
      DBG_VRAM_WRITES <= 0; DBG_REG_WRITES <= 0; DBG_CRAM_WRITES <= 0;
      bk_ctrl <= 8'h00; bk0 <= 8'h00; bk1 <= 8'h01; bk2 <= 8'h02;  // Sega mapper power-on
    end else begin
      if (frame_start) vblank_flag <= 1'b1;

      // writes (rising edge of the write strobe)
      if (wr_edge) begin
        if (~iorq_n) begin
          if (A[7:6] == 2'b10) begin
            if (A[0]) begin
              // $BF control port
              if (!vdp_latch) begin vdp_latch_lo <= dout; vdp_latch <= 1'b1; end
              else begin
                vdp_latch <= 1'b0;
                vdp_code  <= dout[7:6];
                vdp_addr  <= {dout[5:0], vdp_latch_lo};
                if (dout[7:6] == 2'b10) begin vreg[dout[3:0]] <= vdp_latch_lo; DBG_REG_WRITES <= DBG_REG_WRITES + 1; end
                else if (dout[7:6] == 2'b00) begin vdp_addr <= {dout[5:0], vdp_latch_lo}; end  // VRAM read setup
              end
            end else begin
              // $BE data port (VRAM/CRAM write handled by the RAM templates; count + advance here)
              vdp_latch <= 1'b0;
              if (vdp_code == 2'b11) begin cram[vdp_addr[4:0]] <= dout; DBG_CRAM_WRITES <= DBG_CRAM_WRITES + 1; end
              else                  DBG_VRAM_WRITES <= DBG_VRAM_WRITES + 1;
              vdp_readbuf <= dout;
              vdp_addr <= vdp_addr + 1'b1;
            end
          end
          // $40-$7F -> PSG (handled by u_psg via psg_we; nothing to do here)
        end else if (~mreq_n) begin
          // memory write: WRAM data goes via the wram template (wram_we); here we
          // only latch the Sega mapper bank regs into their dedicated FFs.
          if (A >= 16'hC000) begin
            case (A[15:0])
              16'hFFFC: bk_ctrl <= dout;
              16'hFFFD: bk0 <= dout;
              16'hFFFE: bk1 <= dout;
              16'hFFFF: bk2 <= dout;
              default: ;
            endcase
          end
        end
      end

      // VDP read side effects (rising edge of read strobe, I/O only)
      if (rd_edge && ~iorq_n && (A[7:6] == 2'b10)) begin
        vdp_latch <= 1'b0;
        if (A[0]) vblank_flag <= 1'b0;
        else begin vdp_readbuf <= vram_q; vdp_addr <= vdp_addr + 1'b1; end  // buffered read from port-A
      end
    end
  end

endmodule
