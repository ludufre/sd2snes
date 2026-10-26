// SPDX-License-Identifier: GPL-2.0-or-later
//
// a26_mem.v -- memory client for the Atari core.  Milestone 7a, ROM in block RAM.
//
// Speaks the same request protocol as sd2snes_base/dma.v, i.e. it can be
// dropped into the arbiter slot that the FPGA copier uses (DMA_RRQ/DMA_WRQ,
// DMA_ADDR, DMA_DOUT, DMA_WORD, DMA_DINr, DMA_RDY):
//   * a request is made by raising RRQ/WRQ while BUS_RDY = 1; the arbiter
//     drops BUS_RDY on the same clock edge and raises it again when the
//     access is done.  For reads, ROM_DATA_IN is valid when BUS_RDY returns.
//   * byte read   (WORD = 0): ROM_DATA_IN[7:0] is the addressed byte, for even
//     and odd addresses alike.
//   * word write  (WORD = 1, even address): ROM_DATA_OUT[7:0] goes to the even
//     address and [15:8] to the odd one (little endian).
//
// CARTRIDGE ROM (the PSRAM slot is too slow for a read per CPU cycle: on the
// real arbiter a grant comes once per SNES CPU cycle, after the MCU)
//   * The image lives in a 16 KB block RAM.  After every rst (the SNES leaving
//     reset) the loader reads ROM_SIZE bytes from ROM_BASE through the slot,
//     one byte per request, with no deadline, and then raises `loaded`.
//     Nothing is loaded before the first rst.  a26_top keeps the core stopped
//     until `loaded`, so the loader never competes with the video writes.
//   * The core's reads (rom_rd / rom_ack / rom_data, unchanged protocol) are
//     answered from the block RAM.  rom_ack comes ACK_DLY clocks after rom_rd
//     although the data is there after two: the block RAM output is then stable
//     for more than 12 clk before the core samples it, which is what lets
//     ucf.extra give that path the same 125 ns as the RIOT RAM.
//   * core_addr[14] is IGNORED: only 16 KB are held, so mapper F4 (32 KB)
//     aliases banks 4..7 onto 0..3 until it is supported.  2K, 4K, F8 and F6
//     are complete.
//
// The video writer's 16-bit word writes still go to the PSRAM, through a
// 16-entry FIFO (a line is 32 tile words plus up to 10 table words).
//
// ROM_BASE / FB_BASE are PSRAM BYTE addresses and must agree with the
// firmware (where it loads the .a26) and with the SNES player (where it DMAs
// the frame buffer from).  fb_waddr is a word address of 16 bits (buffer[1:0],
// offset[13:0]), so the three frame buffers are 32 KB each from FB_BASE.
//
// STATUS: untested when written.  Verify with tb_a26_mem.v / tb_a26_sys.v.
//
`timescale 1ns / 1ps

module a26_mem #(
  parameter [23:0] ROM_BASE = 24'h300000,
  parameter [23:0] FB_BASE  = 24'h380000,
  parameter [4:0]  ACK_DLY  = 5'd14          // rom_ack is seen ACK_DLY + 2 clk after rom_rd
) (
  input             clk,
  input             rst,

  // from a26_core
  input      [14:0] core_addr,
  input             rom_rd,
  output reg [7:0]  rom_data,
  output reg        rom_ack,

  // from a26_vidwr
  input             fb_wr,
  input      [15:0] fb_waddr,
  input      [15:0] fb_wdata,

  // arbiter slot (same shape as dma.v)
  input             BUS_RDY,
  output            BUS_RRQ,
  output            BUS_WRQ,
  output     [23:0] ROM_ADDR,
  output     [15:0] ROM_DATA_OUT,
  output            ROM_WORD_ENABLE,
  input      [15:0] ROM_DATA_IN,

  output reg        fifo_ovf,         // sticky: a video write was dropped
  output            fifo_low,         // FIFO less than half full (a26_tdict only writes then)
  output            wr_idle,          // FIFO empty and no access in flight
  output reg        loaded,           // the block RAM holds the whole image

  // diagnostics
  input             diag_clr,         // clears rd_lat_max
  output reg [7:0]  rd_lat_max        // worst loader request -> answer latency in clk, saturates at 255
);

  localparam [2:0] S_IDLE = 3'd0, S_RD_REQ = 3'd1, S_RD_WAIT = 3'd2,
                   S_WR_REQ = 3'd3, S_WR_WAIT = 3'd4;

  reg [2:0]  state;

  // ---- cartridge ROM: 16 KB of block RAM, registered read --------------------
  reg [7:0]  cart [0:16383];
  reg [13:0] rd_a;                    // read address, changes only on rom_rd
  reg [13:0] ld_a;                    // loader write address
  reg        ld_on;                   // loader running (set by rst, never before it)
  wire       ld_we = (state == S_RD_WAIT) & BUS_RDY;

  always @(posedge clk) begin
    if (ld_we) cart[ld_a] <= ROM_DATA_IN[7:0];
    rom_data <= cart[rd_a];
  end

  // answer to the core: ACK_DLY clocks after the request
  reg [4:0]  ack_cnt;
  reg        ack_run;

  // latency of the loader read in flight: 1 on the clk after the request is
  // raised, so the stored value is the number of clk until the byte is taken
  reg [7:0]  lat_cnt;

  reg [31:0] fifo [0:15];
  reg [3:0]  wp, rp;
  reg [4:0]  cnt;
  wire [31:0] head = fifo[rp];

  assign BUS_RRQ = BUS_RDY & (state == S_RD_REQ);
  assign BUS_WRQ = BUS_RDY & (state == S_WR_REQ);

  assign ROM_ADDR = (state == S_WR_REQ)
                  ? (FB_BASE  + {7'b0000000, head[31:16], 1'b0})
                  : (ROM_BASE + {10'b0000000000, ld_a});
  assign ROM_DATA_OUT    = head[15:0];
  assign ROM_WORD_ENABLE = (state == S_WR_REQ);

  assign fifo_low = (cnt[4:3] == 2'b00);
  assign wr_idle  = (cnt == 5'd0) & (state == S_IDLE);

  wire pop  = BUS_WRQ;
  wire push = fb_wr & ((cnt != 5'd16) | pop);

  initial begin
    state = S_IDLE; ld_on = 1'b0; loaded = 1'b0; ld_a = 14'd0;
    rom_ack = 1'b0; ack_run = 1'b0; fifo_ovf = 1'b0; rd_lat_max = 8'd0;
  end

  always @(posedge clk) begin
    rom_ack <= 1'b0;
    if (rst) begin
      state <= S_IDLE; wp <= 4'd0; rp <= 4'd0; cnt <= 5'd0;
      fifo_ovf <= 1'b0;
      ld_on <= 1'b1; loaded <= 1'b0; ld_a <= 14'd0;
      ack_run <= 1'b0; ack_cnt <= 5'd0; rd_a <= 14'd0;
      lat_cnt <= 8'd0; rd_lat_max <= 8'd0;
    end else begin
      // FIFO bookkeeping
      if (fb_wr && cnt == 5'd16 && !pop) fifo_ovf <= 1'b1;
      if (push) begin fifo[wp] <= {fb_waddr, fb_wdata}; wp <= wp + 4'd1; end
      if (pop)  rp <= rp + 4'd1;
      cnt <= cnt + {4'b0000, push} - {4'b0000, pop};

      // loader read latency (diagnostic)
      if (state == S_RD_REQ || state == S_RD_WAIT) begin
        if (lat_cnt != 8'hFF) lat_cnt <= lat_cnt + 8'd1;
      end
      if (diag_clr)                            rd_lat_max <= 8'd0;
      else if (ld_we && lat_cnt > rd_lat_max)  rd_lat_max <= lat_cnt;

      // request sequencer (the loader comes first; the core is stopped meanwhile)
      case (state)
        S_IDLE: begin
          if (ld_on)          begin state <= S_RD_REQ; lat_cnt <= 8'd1; end
          else if (cnt != 0)  state <= S_WR_REQ;
        end
        S_RD_REQ: if (BUS_RDY) state <= S_RD_WAIT;
        S_RD_WAIT: if (BUS_RDY) begin              // ld_we: the byte goes to cart[ld_a]
          ld_a  <= ld_a + 14'd1;
          if (ld_a == 14'h3FFF) begin ld_on <= 1'b0; loaded <= 1'b1; end
          state <= S_IDLE;
        end
        S_WR_REQ:  if (BUS_RDY) state <= S_WR_WAIT;
        S_WR_WAIT: if (BUS_RDY) state <= S_IDLE;
        default:   state <= S_IDLE;
      endcase

      // the core's read: latch the address (14 bits, see the header) and answer later
      if (rom_rd) begin
        rd_a <= core_addr[13:0]; ack_cnt <= ACK_DLY; ack_run <= 1'b1;
      end else if (ack_run) begin
        if (ack_cnt == 5'd0) begin rom_ack <= 1'b1; ack_run <= 1'b0; end
        else                 ack_cnt <= ack_cnt - 5'd1;
      end
    end
  end

endmodule
