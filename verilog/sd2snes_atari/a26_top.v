// SPDX-License-Identifier: GPL-2.0-or-later
//
// a26_top.v -- the whole Atari subsystem behind the port list of the shell's
//              `dma` module (sd2snes_base/dma.v), plus an audio sample output.
//              Milestone 7b.
//
// In the Atari shell this module is instantiated where `dma snes_dma` was:
//   * enable / reg_*  : the $2020-$202F window (dma_enable), also reachable by
//                       the MCU while it holds the SNES in reset (MCU_DMA_*),
//                       so the firmware can set `run` and the mapper itself
//   * BUS_* / ROM_*   : the arbiter slot used by the FPGA copier, carrying the
//                       ROM load into block RAM (once per SNES reset) and the
//                       frame-buffer writes
//   * loop_enable/busy: not used (tied to 0)
//   * aud_sample      : signed 16-bit, same value for both channels, for a26_i2s
//
// clkin must be the 96 MHz CLK2 (a26_core.CC_INC assumes it).
//
// STATUS: untested when written.  Verify with tb_a26_top.v.
//
`timescale 1ns / 1ps

module a26_top #(
  parameter [23:0] ROM_BASE = 24'h300000,
  parameter [23:0] FB_BASE  = 24'h380000
) (
  input             clkin,
  input             reset,

  input             enable,
  input      [3:0]  reg_addr,
  input      [7:0]  reg_data_in,
  output     [7:0]  reg_data_out,
  input             reg_oe_falling,
  input             reg_we_rising,

  output            loop_enable,
  output     [1:0]  busy,

  input             BUS_RDY,
  output            BUS_RRQ,
  output            BUS_WRQ,
  output     [23:0] ROM_ADDR,
  output     [15:0] ROM_DATA_OUT,
  input      [15:0] ROM_DATA_IN,
  output            ROM_WORD_ENABLE,

  output     [4:0]  aud_mix,
  output signed [15:0] aud_sample
);

  assign loop_enable = 1'b0;
  assign busy        = 2'b00;

  // ---- SNES-visible registers ---------------------------------------------
  wire [7:0] swcha, swchb;
  wire       fire0, fire1, run, pause, sc;
  wire [6:0] yofs;
  wire [1:0] pal;
  wire       lock_v;
  wire [1:0] lock_i, fb_newest;
  wire [3:0] inpt;
  wire [2:0] mapper;
  wire       rom_miss, fifo_ovf, loaded;
  wire [7:0] fb_frames, fb_nlines, fb_dropped;
  wire [9:0] tile_n;
  wire       diag_clr, rom_miss_ev;
  wire [7:0] lat_max;
  reg  [7:0] miss_cnt;
  reg        miss_ev_r;

  a26_regs regs (
    .clk(clkin), .rst(reset),
    .enable(enable), .reg_addr(reg_addr), .reg_data_in(reg_data_in),
    .reg_data_out(reg_data_out),
    .reg_oe_falling(reg_oe_falling), .reg_we_rising(reg_we_rising),
    .swcha(swcha), .swchb(swchb), .fire0(fire0), .fire1(fire1),
    .inpt(inpt), .run(run), .mapper(mapper), .pause(pause), .sc(sc),
    .yofs(yofs), .pal(pal), .lock_v(lock_v), .lock_i(lock_i),
    .fb_newest(fb_newest), .fb_frames(fb_frames), .fb_nlines(fb_nlines),
    .fb_dropped(fb_dropped), .tile_n(tile_n),
    .rom_miss(rom_miss), .fifo_ovf(fifo_ovf), .loaded(loaded),
    .diag_clr(diag_clr), .miss_cnt(miss_cnt), .lat_max(lat_max));

  // ---- core ----------------------------------------------------------------
  wire [14:0] crom_addr;   // (not rom_addr: XST rejects names that differ only by case from ROM_ADDR)
  wire        rom_rd, rom_ack;
  wire [7:0]  rom_data;
  wire        pix_stb, pix_vis, vsync, vblank;
  wire [7:0]  pix_x, hcount;
  wire [1:0]  pix_src;
  wire [6:0]  pix_col, colubk, colupf, colup0, colup1;
  wire        cpu_rdy, cpu_sync;
  wire [15:0] cpu_ab;
  // the core (and the video writer) stay stopped until the ROM is in the block RAM
  wire        core_rst = reset | ~run | ~loaded;

  a26_core core (
    .clk(clkin), .rst(core_rst), .mapper(mapper), .sc_en(sc),
    .swcha_in(swcha), .swchb_in(swchb), .fire0(fire0), .fire1(fire1), .inpt(inpt),
    .pause(pause),
    .rom_addr(crom_addr), .rom_rd(rom_rd), .rom_data(rom_data), .rom_ack(rom_ack),
    .pix_stb(pix_stb), .pix_x(pix_x), .pix_vis(pix_vis),
    .pix_src(pix_src), .pix_col(pix_col),
    .hcount(hcount), .vsync(vsync), .vblank(vblank),
    .colubk(colubk), .colupf(colupf), .colup0(colup0), .colup1(colup1),
    .aud_mix(aud_mix), .cpu_rdy(cpu_rdy), .cpu_ab(cpu_ab), .cpu_sync(cpu_sync),
    .rom_miss(rom_miss), .diag_clr(diag_clr), .rom_miss_ev(rom_miss_ev));

  // ---- diagnostics: CPU cycles that used a stale ROM byte ---------------------
  // The event is registered first, so the counter does not sit behind the core's
  // slow logic.  Stopping the core (run = 0, or the ROM not loaded yet) clears the
  // miss counter like reg 5 does; the loader latency is only cleared by reg 5 or reset.
  wire diag_rst = diag_clr | core_rst;
  always @(posedge clkin) begin
    miss_ev_r <= rom_miss_ev;
    if (diag_rst)                              miss_cnt <= 8'd0;
    else if (miss_ev_r && miss_cnt != 8'hFF)   miss_cnt <= miss_cnt + 8'd1;
  end

  // ---- video writer -----------------------------------------------------------
  wire        fb_wr;
  wire [15:0] fb_waddr;
  wire [15:0] fb_wdata;
  wire        ev_row, ev_end, eng_ack, eng_ovf, m_wr, fifo_low, wr_idle;
  wire [7:0]  end_lines;
  wire [15:0] m_waddr, m_wdata;

  a26_vidwr vid (
    .clk(clkin), .rst(core_rst),
    .pix_stb(pix_stb), .pix_x(pix_x), .pix_vis(pix_vis), .pix_src(pix_src), .pix_col(pix_col),
    .vsync(vsync), .vblank(vblank),
    .colubk(colubk), .colupf(colupf), .colup0(colup0), .colup1(colup1),
    .fb_wr(fb_wr), .fb_waddr(fb_waddr), .fb_wdata(fb_wdata),
    .fb_newest(fb_newest), .fb_frames(fb_frames), .fb_nlines(fb_nlines),
    .lock_v(lock_v), .lock_i(lock_i), .yofs(yofs), .pal(pal),
    .ev_row(ev_row), .ev_end(ev_end), .end_lines(end_lines),
    .eng_ack(eng_ack), .eng_ovf(eng_ovf), .fb_dropped(fb_dropped));

  // ---- tile dictionary: tile words -> header, map and distinct tiles ----------------
  a26_tdict dict (
    .clk(clkin), .rst(core_rst),
    .v_wr(fb_wr), .v_waddr(fb_waddr), .v_wdata(fb_wdata),
    .ev_row(ev_row), .ev_end(ev_end), .end_lines(end_lines),
    .eng_ack(eng_ack), .eng_ovf(eng_ovf), .tile_n(tile_n),
    .m_wr(m_wr), .m_waddr(m_waddr), .m_wdata(m_wdata),
    .fifo_low(fifo_low), .wr_idle(wr_idle));

  // ---- memory client ------------------------------------------------------------
  a26_mem #(.ROM_BASE(ROM_BASE), .FB_BASE(FB_BASE)) memc (
    .clk(clkin), .rst(reset),
    .core_addr(crom_addr), .rom_rd(rom_rd), .rom_data(rom_data), .rom_ack(rom_ack),
    .fb_wr(m_wr), .fb_waddr(m_waddr), .fb_wdata(m_wdata),
    .BUS_RDY(BUS_RDY), .BUS_RRQ(BUS_RRQ), .BUS_WRQ(BUS_WRQ),
    .ROM_ADDR(ROM_ADDR), .ROM_DATA_OUT(ROM_DATA_OUT),
    .ROM_WORD_ENABLE(ROM_WORD_ENABLE), .ROM_DATA_IN(ROM_DATA_IN),
    .fifo_ovf(fifo_ovf), .fifo_low(fifo_low), .wr_idle(wr_idle),
    .loaded(loaded), .diag_clr(diag_clr), .rd_lat_max(lat_max));

  // ---- audio: mix 0..30 -> signed 16 bit, centred at 15, x1536 (shift-add) --------
  wire signed [15:0] mixc = $signed({11'b00000000000, aud_mix}) - 16'sd15;
  assign aud_sample = (mixc <<< 10) + (mixc <<< 9);

endmodule
