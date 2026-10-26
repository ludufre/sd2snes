// SPDX-License-Identifier: GPL-2.0-or-later
//
// a26_regs.v -- SNES-visible register window for the Atari mode.
//               Milestone 7a.
//
// Same bus protocol as sd2snes_base/msu.v and dma.v.  In the Atari shell it
// takes the place of the FPGA copier's window at $2020-$202F (dma_enable).
// `enable` is the decoded address, reg_addr the offset,
// reg_we_rising / reg_oe_falling the SNES write / read strobes, and read data
// is registered on reg_oe_falling.
//
// WRITE (SNES player -> core)
//   0  SWCHA   joystick port, active low (P0 = bits 7:4, P1 = bits 3:0)
//   1  SWCHB   console switches
//   2  bit0 = fire0 pin level, bit1 = fire1 pin level  (1 = released)
//   3  bit0 = run (0 holds the core in reset), bits 3:1 = mapper,
//      bit4 = pause (1 stops the colour clock: CPU, TIA, RIOT and audio freeze
//      where they are, nothing is reset, the published frame stays),
//      bit5 = Superchip RAM (F8 / F6: $1000-$107F write, $1080-$10FF read; Passo 36)
//   4  bits 3:0 = INPT0..3 pin levels (paddles; leave 0 for joysticks)
//   5  any value: clear the diagnostics (rom_miss flag, miss counter, max latency)
//   6  vertical offset of the picture window, signed 8 bit, -16..+47 (values
//      outside are clamped; default 0): the frame buffer starts at line
//      YS0 + offset after the end of VSYNC.  Taken when VSYNC ends, so it
//      applies from the next frame on.
//   7  buffer lock: bit7 = lock valid, bits 1:0 = buffer (0..2) the SNES is
//      reading; the core never starts a frame in that buffer.  Bit7 = 0 releases.
//   8  bits 1:0 = palette: 0 Color-1, 1 Color-2, 2 Color-3, 3 Black/White (default 0).  Taken
//      when VSYNC ends, so it applies from the next frame on.
//   9..15 unused (the $2020-$202F window of the FPGA copier is 16 bytes)
// READ
//   0  bits 1:0 = buffer (0..2) holding the newest complete frame, bits 7:2 = 0
//   1  completed-frame counter
//   2  lines written in the last completed frame (232 = the whole window)
//   3  control register readback
//   4..7  "A26" + version (4 = as 3, picture 160 columns wide 1:1), to detect the core
//   8  CPU cycles that used a stale ROM byte (saturates at 255)
//   9  worst latency of the ROM loader's PSRAM reads, in 96 MHz clocks
//      (saturates at 255): the real latency of the arbiter slot
//   10 vertical offset in use (readback of write register 6 after clamping)
//   11 bit6 = rom_miss (sticky), bit5 = fifo overflow, bit4 = ROM loaded into the
//      block RAM (the core runs only with run & loaded); these were in register 0
//   12 buffer lock readback (bit7, bits 1:0)
//   13 frames dropped since the core started (dictionary full or not ready; saturates at 255)
//   14 distinct tiles of the last frame the dictionary finished, bits 7:0
//   15 the same, bits 9:8
//
// Power-on state: pads and switches released (all ones), core stopped,
// mapper = 4K.
//
// STATUS: untested when written.  Verify with tb_a26_sys.v.
//
`timescale 1ns / 1ps

module a26_regs (
  input            clk,
  input            rst,

  input            enable,
  input      [3:0] reg_addr,
  input      [7:0] reg_data_in,
  output     [7:0] reg_data_out,
  input            reg_oe_falling,
  input            reg_we_rising,

  output     [7:0] swcha,
  output     [7:0] swchb,
  output           fire0,
  output           fire1,
  output     [3:0] inpt,
  output           run,
  output     [2:0] mapper,
  output           pause,
  output           sc,
  output     [6:0] yofs,
  output     [1:0] pal,
  output           lock_v,
  output     [1:0] lock_i,

  input      [1:0] fb_newest,
  input      [7:0] fb_frames,
  input      [7:0] fb_nlines,
  input      [7:0] fb_dropped,
  input      [9:0] tile_n,
  input            rom_miss,
  input            fifo_ovf,
  input            loaded,

  output reg       diag_clr,
  input      [7:0] miss_cnt,
  input      [7:0] lat_max
);

  reg [7:0] r_swcha, r_swchb;
  reg [1:0] r_fire;
  reg [5:0] r_ctrl;
  reg [3:0] r_inpt;
  reg [6:0] r_yofs;
  reg [1:0] r_pal;
  reg [2:0] r_lock;
  reg [7:0] data_out_r;

  initial begin
    r_swcha = 8'hFF; r_swchb = 8'hFF; r_fire = 2'b11;
    r_ctrl = 6'b000010; r_inpt = 4'h0; r_yofs = 7'd0; r_pal = 2'd0; r_lock = 3'd0; data_out_r = 8'h00; diag_clr = 1'b0;
  end

  assign swcha  = r_swcha;
  assign swchb  = r_swchb;
  assign fire0  = r_fire[0];
  assign fire1  = r_fire[1];
  assign inpt   = r_inpt;
  assign run    = r_ctrl[0];
  assign mapper = r_ctrl[3:1];
  assign pause  = r_ctrl[4];
  assign sc     = r_ctrl[5];
  assign yofs   = r_yofs;
  assign pal    = r_pal;
  assign lock_v = r_lock[2];
  assign lock_i = r_lock[1:0];

  // register 6: signed 8 bit clamped to -16..+47, kept as 7 bits
  wire [7:0] d = reg_data_in;
  wire [6:0] yofs_d =  d[7] ? ((d[6:4] == 3'b111)     ? {1'b1, d[5:0]} : 7'b1110000)     // < -16 -> -16
                            : ((d[6] | (d[5] & d[4])) ? 7'd47          : {1'b0, d[5:0]}); // > 47 -> 47
  assign reg_data_out = data_out_r;

  always @(posedge clk) begin
    if (rst) begin
      r_swcha <= 8'hFF; r_swchb <= 8'hFF; r_fire <= 2'b11;
      r_ctrl  <= 6'b000010;                     // mapper 1 (4K), not running, not paused, no Superchip
      r_inpt  <= 4'h0;
      r_yofs  <= 7'd0;
      r_pal   <= 2'd0;
      r_lock  <= 3'd0;
    end else if (reg_we_rising & enable) begin
      case (reg_addr)
        4'd0: r_swcha <= reg_data_in;
        4'd1: r_swchb <= reg_data_in;
        4'd2: r_fire  <= reg_data_in[1:0];
        4'd3: r_ctrl  <= reg_data_in[5:0];
        4'd4: r_inpt  <= reg_data_in[3:0];
        4'd6: r_yofs  <= yofs_d;
        4'd8: r_pal   <= reg_data_in[1:0];
        4'd7: r_lock  <= {reg_data_in[7], reg_data_in[1:0]};
        default: ;
      endcase
    end
  end

  always @(posedge clk)
    diag_clr <= ~rst & reg_we_rising & enable & (reg_addr == 4'd5);

  always @(posedge clk) begin
    if (reg_oe_falling & enable) begin
      case (reg_addr)
        4'd0: data_out_r <= {6'b000000, fb_newest};
        4'd1: data_out_r <= fb_frames;
        4'd2: data_out_r <= fb_nlines;
        4'd3: data_out_r <= {2'b00, r_ctrl};
        4'd4: data_out_r <= 8'h41;             // 'A'
        4'd5: data_out_r <= 8'h32;             // '2'
        4'd6: data_out_r <= 8'h36;             // '6'
        4'd7: data_out_r <= 8'h0E;             // version
        4'd8: data_out_r <= miss_cnt;
        4'd9: data_out_r <= lat_max;
        4'd10: data_out_r <= {r_yofs[6], r_yofs};
        4'd11: data_out_r <= {1'b0, rom_miss, fifo_ovf, loaded, 4'b0000};
        4'd12: data_out_r <= {r_lock[2], 5'b00000, r_lock[1:0]};
        4'd13: data_out_r <= fb_dropped;
        4'd14: data_out_r <= tile_n[7:0];
        4'd15: data_out_r <= {6'b000000, tile_n[9:8]};
        default: data_out_r <= 8'h00;
      endcase
    end
  end

endmodule
