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
  output        DBG_FRAME_TICK     // SMS vblank tick (frame pacing, perf instrumentation)
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
  wire sms_ce_gated = CE;
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
    .DBG_WAIT(DBG_WAIT)
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

endmodule
