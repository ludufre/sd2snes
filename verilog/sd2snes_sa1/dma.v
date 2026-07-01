`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date:    19:31:43 08/22/2017 
// Design Name: 
// Module Name:    dma 
// Project Name: 
// Target Devices: 
// Tool versions: 
// Description: 
//
// Dependencies: 
//
// Revision: 
// Revision 0.01 - File Created
// Additional Comments: 
//
//////////////////////////////////////////////////////////////////////////////////
module dma(
  input clkin,
  input reset,
  input enable,

  input [3:0] reg_addr,
  input [7:0] reg_data_in,
  output [7:0] reg_data_out,

  input reg_oe_falling,
  input reg_we_rising,
  
  output loop_enable,
  output [1:0] busy,   // [0]=running, [1]=overrun (1-trigger queue dropped an op)

  input  BUS_RDY,
  output BUS_RRQ,
  output BUS_WRQ,
  
  output [23:0] ROM_ADDR,
  output [15:0] ROM_DATA_OUT,
  output        ROM_WORD_ENABLE,
  input  [15:0] ROM_DATA_IN
);

parameter ST_IDLE   = 0;
parameter ST_READ   = 1;
parameter ST_WRITE  = 2;
parameter ST_DONE   = 3;
// List mode (Approach B+): the copier reads a list of 10-byte descriptors from
// PSRAM and processes the whole batch autonomously, so the MCU issues ONE trigger
// + ONE busy-poll for thousands of copies instead of one SPI round-trip per op.
parameter ST_LFETCH  = 4;   // issue read of one descriptor word (5 words/descriptor)
parameter ST_LCAP    = 5;   // read completed -> capture the word into a field
parameter ST_LDECODE = 6;   // descriptor fully latched -> compute word-mode, start copy
parameter ST_LNEXT   = 7;   // advance list_ptr/count to the next descriptor

parameter OP_COPY  = 0;
parameter OP_RESET = 1;
parameter OP_SET   = 2;
parameter OP_DEBUG = 3;

// Register bank
reg [7:0] dma_r[15:0];

reg [2:0]  state; initial state = ST_IDLE;

initial begin
  dma_r[0] = 8'h00; // dst bank
  dma_r[1] = 8'h00; // src bank
  dma_r[2] = 8'h00; // dst[7:0]
  dma_r[3] = 8'h00; // dst[15:8]
  dma_r[4] = 8'h00; // src[7:0]
  dma_r[5] = 8'h00; // src[15:0]
  dma_r[6] = 8'h00; // len[7:0]
  dma_r[7] = 8'h00; // len[15:8]
  dma_r[8] = 8'h00; // len[23:16]
  dma_r[9] = 8'h00; // opcode[7:3], loop, direction, trigger
end

reg [7:0] data_out_r;
assign reg_data_out = data_out_r;

integer i;
always @(posedge clkin) begin
  if(reg_oe_falling & enable) begin
    case(reg_addr)
      4'h0: data_out_r <= 8'h53;
      4'h1: data_out_r <= 8'h2D;
      4'h2: data_out_r <= 8'h44;
      4'h3: data_out_r <= 8'h4D;
      4'h4: data_out_r <= 8'h41;
      4'h5: data_out_r <= 8'h31;
      4'h9: data_out_r <= dma_r[reg_addr];
		  default: data_out_r <= 8'h00;
    endcase
  end
end

always @(posedge clkin) begin
  if (reset) begin
    for (i = 0; i < 16; i = i + 1) dma_r[i] <= 0;  // incl. list regs 10..15 (trigger!)
  end
  else if(reg_we_rising & enable) begin
    dma_r[reg_addr] <= reg_data_in;
  end
  else if (state == ST_DONE) begin
    dma_r[9][0]  <= 0;   // clear single-op trigger
    dma_r[15][0] <= 0;   // clear list trigger
  end
end

wire [23:0] SRC_ADDR, DST_ADDR, LEN;
wire [4:0] OPCODE;
wire LOOP, DIR, TRIG, WORD_MODE;

assign SRC_ADDR = {dma_r[1], dma_r[3], dma_r[2]};
assign DST_ADDR = {dma_r[0], dma_r[5], dma_r[4]};
assign LEN      = {dma_r[8], dma_r[7], dma_r[6]};
assign OPCODE   = dma_r[9][7:3];
assign LOOP     = dma_r[9][2];
assign DIR      = dma_r[9][1];
assign TRIG     = dma_r[9][0];
// this covers misaligned addresses, misaligned length, as well as byte overlaps
assign WORD_MODE = !SRC_ADDR[0] && !DST_ADDR[0] && !LEN[0];

// ---- List mode (Approach B+) -------------------------------------------------
// dma_r[10..12] = list base addr (LE), dma_r[13..14] = descriptor count (LE),
// dma_r[15][0] = list trigger.  Each descriptor is 10 contiguous bytes = 5 LE
// words (the FPGA word-read at an even address returns a little-endian word):
//   w0 = {srcBank,dstBank}, w1 = src[15:0], w2 = dst[15:0],
//   w3 = len[15:0],         w4 = {opcode,len[23:16]}.
wire [23:0] LIST_BASE  = {dma_r[12], dma_r[11], dma_r[10]};
wire [15:0] LIST_COUNT = {dma_r[14], dma_r[13]};
wire        LTRIG      = dma_r[15][0];

reg [15:0] data;
reg [4:0]  opcode_r;
reg        loop_r;      initial loop_r = 0;
reg        dir_r;
reg        trig_r;      initial trig_r = 0;
reg        ltrig_r;     initial ltrig_r = 0;
reg        word_mode_r;
reg [23:0] src_addr_r, dst_addr_r, length_r, mod_r;
reg [23:0] list_ptr_r;
reg [15:0] list_cnt_r;
reg [2:0]  fetch_idx;
reg        in_list;     initial in_list = 0;

// ---- 1-trigger queue (#3) ----------------------------------------------------
// The MCU fires the next single-op WITHOUT polling busy: a trigger (a write to
// dma_r[9] with bit0 set) arriving while the copier is BUSY is latched as
// `pending` + the op snapshotted into a shadow, and started in ST_IDLE when the
// current op finishes.  A 2nd trigger while already pending sets the sticky
// `overrun_r` (the FPGA fell behind on a big op) -- the MCU reads it via busy[1]
// and aborts to the per-op path.  This is the SAFE alternative to the list mode:
// the MCU still supplies every src/dst/len, so there is no autonomous descriptor
// fetch that could runaway on a mis-sampled word.
reg        pending;     initial pending = 0;
reg        overrun_r;   initial overrun_r = 0;
reg [23:0] shadow_src, shadow_dst, shadow_len;
reg        shadow_word, shadow_dir;
reg [4:0]  shadow_op;
wire op_kick = reg_we_rising & enable & (reg_addr == 4'd9) & reg_data_in[0];

wire [23:0] fetch_addr = list_ptr_r + {19'b0, fetch_idx, 1'b0}; // list_ptr + fetch_idx*2

assign BUS_RRQ = BUS_RDY && (state == ST_READ || state == ST_LFETCH);
assign BUS_WRQ = BUS_RDY && (state == ST_WRITE);
assign ROM_ADDR = (state == ST_LFETCH) ? fetch_addr
                : (state == ST_READ)   ? src_addr_r
                :                        dst_addr_r;
assign ROM_DATA_OUT =   (opcode_r == OP_COPY)  ? (dst_addr_r[0] ? {ROM_DATA_IN[7:0],ROM_DATA_IN[15:8]} : ROM_DATA_IN)
                      : (opcode_r == OP_RESET) ? 16'h0000
                      : (opcode_r == OP_SET)   ? 16'hFFFF
                      : (opcode_r == OP_DEBUG) ? {dst_addr_r[3:0],loop_enable,ROM_WORD_ENABLE,mod_r[1:0],dst_addr_r[3:0],loop_enable,ROM_WORD_ENABLE,mod_r[1:0]}
                      : 0;
assign ROM_WORD_ENABLE = (state == ST_LFETCH) ? 1'b1 : word_mode_r; // fetch reads full words
assign loop_enable = loop_r;
/* busy[0] = copier running (state != ST_IDLE); busy[1] = overrun (a queued trigger
   was dropped).  The MCU reads this via mcu_cmd 0xd5: it polls bit0 for big ops and
   checks bit1 at the end to detect a queue overrun. */
assign busy = {overrun_r, (state != ST_IDLE)};

wire [23:0] length_next = length_r - (word_mode_r ? 2 : 1);

always @(posedge clkin) begin
  if (reset) begin
    loop_r    <= 0;
    state     <= ST_IDLE;
    trig_r    <= 0;
    ltrig_r   <= 0;
    in_list   <= 0;
    pending   <= 0;
    overrun_r <= 0;
  end
  else begin
    trig_r  <= TRIG;
    ltrig_r <= LTRIG;

    // 1-trigger queue: a trigger arriving while the copier is BUSY is latched as
    // `pending` (+ the op snapshotted); a 2nd one while already pending overruns.
    // (When ST_IDLE, the fresh trigger is handled by the TRIG-edge path below.)
    if (op_kick && (state != ST_IDLE)) begin
      if (pending) overrun_r <= 1'b1;
      else begin
        pending     <= 1'b1;
        shadow_src  <= SRC_ADDR;
        shadow_dst  <= DST_ADDR;
        shadow_len  <= LEN;
        shadow_word <= WORD_MODE;
        shadow_op   <= reg_data_in[7:3];  // dma_r[9] being written this cycle
        shadow_dir  <= reg_data_in[1];
      end
    end

    case (state)
      ST_IDLE: begin
        if (pending) begin
          // 1-trigger queue: start the op snapshotted while the previous one ran
          src_addr_r  <= shadow_src;
          dst_addr_r  <= shadow_dst;
          length_r    <= shadow_len;
          mod_r       <= shadow_word ? (shadow_dir ? -2 : 2) : (shadow_dir ? -1 : 1);
          opcode_r    <= shadow_op;
          word_mode_r <= shadow_word;
          dir_r       <= shadow_dir;
          loop_r      <= 0;
          in_list     <= 0;
          pending     <= 0;
          if (shadow_op == OP_COPY) state <= ST_READ;
          else                      state <= ST_WRITE;
        end
        else if (op_kick) begin
          // single-op start, detected by the dma_r[9] WRITE (op_kick) rather than a
          // TRIG bit-edge -- robust to the bit already being 1 (the ST_DONE clear
          // can be skipped by a colliding reg-write in no-poll mode).  opcode/loop/
          // dir come from the byte being written (reg_data_in); src/dst/len from
          // dma_r[0..8] (written just before).
          src_addr_r  <= SRC_ADDR;
          dst_addr_r  <= DST_ADDR;
          length_r    <= LEN;
          mod_r       <= WORD_MODE ? (reg_data_in[1] ? -2 : 2) : (reg_data_in[1] ? -1 : 1);
          opcode_r    <= reg_data_in[7:3];
          loop_r      <= reg_data_in[2];
          dir_r       <= reg_data_in[1];
          word_mode_r <= WORD_MODE;
          in_list     <= 0;

          if (reg_data_in[7:3] == OP_COPY) state <= ST_READ;
          else                             state <= ST_WRITE;
        end
        else if (LTRIG && (ltrig_r ^ LTRIG)) begin
          // list mode: process LIST_COUNT descriptors from LIST_BASE
          list_ptr_r <= LIST_BASE;
          list_cnt_r <= LIST_COUNT;
          fetch_idx  <= 0;
          loop_r     <= 0;
          in_list    <= 1;
          state      <= (LIST_COUNT == 0) ? ST_DONE : ST_LFETCH;
        end
      end
      ST_LFETCH: begin
        // BUS_RRQ reads fetch_addr this cycle; the word is valid a few cycles later
        if (BUS_RDY) state <= ST_LCAP;
      end
      ST_LCAP: begin
        if (BUS_RDY) begin   // read complete: ROM_DATA_IN is the LE descriptor word
          case (fetch_idx)
            3'd0: begin dst_addr_r[23:16] <= ROM_DATA_IN[7:0];    // dstBank
                        src_addr_r[23:16] <= ROM_DATA_IN[15:8]; end // srcBank
            3'd1: src_addr_r[15:0] <= ROM_DATA_IN;
            3'd2: dst_addr_r[15:0] <= ROM_DATA_IN;
            3'd3: length_r[15:0]   <= ROM_DATA_IN;
            3'd4: begin length_r[23:16] <= ROM_DATA_IN[7:0];
                        opcode_r        <= ROM_DATA_IN[12:8]; end
          endcase
          if (fetch_idx == 3'd4) state <= ST_LDECODE;
          else begin fetch_idx <= fetch_idx + 1; state <= ST_LFETCH; end
        end
      end
      ST_LDECODE: begin
        // descriptor fully latched; list copies are forward (DIR=0)
        word_mode_r <= !src_addr_r[0] && !dst_addr_r[0] && !length_r[0];
        mod_r       <= (!src_addr_r[0] && !dst_addr_r[0] && !length_r[0]) ? 2 : 1;
        if (opcode_r == OP_COPY) state <= ST_READ;
        else                     state <= ST_WRITE;
      end
      ST_READ: begin
        if (BUS_RDY) begin
          src_addr_r <= src_addr_r + mod_r;
          state <= ST_WRITE;
        end
      end
      ST_WRITE: begin
        if (BUS_RDY) begin
          dst_addr_r <= dst_addr_r + mod_r;
          length_r <= length_next;

          if      (length_next == 0)    state <= in_list ? ST_LNEXT : ST_DONE;
          else if (opcode_r == OP_COPY) state <= ST_READ;
          else                          state <= ST_WRITE;
        end
      end
      ST_LNEXT: begin
        if (list_cnt_r == 16'd1) state <= ST_DONE;  // that was the last descriptor
        else begin
          list_ptr_r <= list_ptr_r + 24'd10;        // 10-byte descriptors
          list_cnt_r <= list_cnt_r - 1;
          fetch_idx  <= 0;
          state      <= ST_LFETCH;
        end
      end
      ST_DONE: begin
        loop_r  <= 0;
        in_list <= 0;
        state   <= ST_IDLE;
      end
    endcase
  end
end

endmodule
