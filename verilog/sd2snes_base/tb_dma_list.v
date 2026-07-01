`timescale 1ns/1ps
// Testbench for dma.v list mode (Approach B+): validates the descriptor fetch
// (LE word assembly), the list iteration (ptr/count), and the copies, against a
// fake PSRAM whose bus behaviour mirrors main.v (read -> LE word; a copy
// preserves bytes -- hand-derived: DMA_DINr = mem[a]|(mem[a+1]<<8), the write
// stores ROM_DATA_OUT[7:0]->mem[dst], [15:8]->mem[dst+1]).
module tb_dma_list;
  reg clk = 0; always #5 clk = ~clk;

  reg reset = 1, enable = 0;
  reg [3:0] reg_addr = 0;
  reg [7:0] reg_data_in = 0;
  reg reg_oe_falling = 0, reg_we_rising = 0;
  wire loop_enable, busy;
  reg  BUS_RDY = 1;
  wire BUS_RRQ, BUS_WRQ;
  wire [23:0] ROM_ADDR;
  wire [15:0] ROM_DATA_OUT;
  wire ROM_WORD_ENABLE;
  reg  [15:0] ROM_DATA_IN = 0;

  dma dut(.clkin(clk), .reset(reset), .enable(enable),
    .reg_addr(reg_addr), .reg_data_in(reg_data_in), .reg_data_out(),
    .reg_oe_falling(reg_oe_falling), .reg_we_rising(reg_we_rising),
    .loop_enable(loop_enable), .busy(busy),
    .BUS_RDY(BUS_RDY), .BUS_RRQ(BUS_RRQ), .BUS_WRQ(BUS_WRQ),
    .ROM_ADDR(ROM_ADDR), .ROM_DATA_OUT(ROM_DATA_OUT),
    .ROM_WORD_ENABLE(ROM_WORD_ENABLE), .ROM_DATA_IN(ROM_DATA_IN));

  reg [7:0] mem [0:65535];

  // bus arbiter: 3-cycle read/write, mirrors main.v handshake (RDY 1->0 on req,
  // back to 1 with data latched when done).
  integer ar_state = 0, ar_cnt = 0;
  reg [23:0] ar_addr; reg ar_word; reg [15:0] ar_wdata;
  always @(posedge clk) begin
    case (ar_state)
      0: begin
        BUS_RDY <= 1;
        if (BUS_RRQ)      begin ar_addr<=ROM_ADDR; ar_word<=ROM_WORD_ENABLE; ar_cnt<=3; ar_state<=1; BUS_RDY<=0; end
        else if (BUS_WRQ) begin ar_addr<=ROM_ADDR; ar_word<=ROM_WORD_ENABLE; ar_wdata<=ROM_DATA_OUT; ar_cnt<=3; ar_state<=2; BUS_RDY<=0; end
      end
      1: begin if (ar_cnt==0) begin ROM_DATA_IN <= mem[ar_addr] | (mem[ar_addr+1]<<8); BUS_RDY<=1; ar_state<=0; end else ar_cnt<=ar_cnt-1; end
      2: begin if (ar_cnt==0) begin mem[ar_addr]<=ar_wdata[7:0]; if (ar_word) mem[ar_addr+1]<=ar_wdata[15:8]; BUS_RDY<=1; ar_state<=0; end else ar_cnt<=ar_cnt-1; end
    endcase
  end

  task wreg(input [3:0] a, input [7:0] d);
    begin @(posedge clk); reg_addr<=a; reg_data_in<=d; reg_we_rising<=1; @(posedge clk); reg_we_rising<=0; end
  endtask

  integer i, errs;
  // build a 10-byte descriptor at mem[off]: copy src->dst, len (word-aligned), opcode 0
  task desc(input [23:0] off, input [23:0] src, input [23:0] dst, input [23:0] len);
    begin
      mem[off+0]=dst[23:16]; mem[off+1]=src[23:16];
      mem[off+2]=src[7:0];   mem[off+3]=src[15:8];
      mem[off+4]=dst[7:0];   mem[off+5]=dst[15:8];
      mem[off+6]=len[7:0];   mem[off+7]=len[15:8]; mem[off+8]=len[23:16];
      mem[off+9]=8'h00;
    end
  endtask

  initial begin
    for (i=0;i<65536;i=i+1) mem[i]=0;
    // source patterns
    for (i=0;i<32;i=i+1) mem[24'h1000+i] = i + 8'h40;   // 0x40,0x41,...
    for (i=0;i<16;i=i+1) mem[24'h1800+i] = 8'hF0 ^ i;
    // descriptor list at 0x2000: 3 copies
    mem[24'h5000]=8'hA5; mem[24'h5001]=8'h3C;          // RLE seed word
    desc(24'h2000, 24'h1000, 24'h3000, 24'd32);  // 32 bytes
    desc(24'h200A, 24'h1800, 24'h3100, 24'd16);  // 16 bytes
    desc(24'h2014, 24'h1010, 24'h3200, 24'd8);   // 8 bytes (mid of pattern)
    desc(24'h201E, 24'h5000, 24'h5002, 24'd8);   // OVERLAP (dst=src+2): word RLE fill

    for (i=0;i<16;i=i+1) mem[24'h6000+i] = 8'h10 + i;   // single-op source

    enable=0; reset=1; repeat(4) @(posedge clk); reset=0; @(posedge clk);

    // ---- single-op mode (used by the source backup + self-test) ----
    enable=1;
    wreg(4'd0,8'h00); wreg(4'd1,8'h00);                 // dst/src bank 0
    wreg(4'd2,8'h00); wreg(4'd3,8'h60);                 // src=0x6000
    wreg(4'd4,8'h00); wreg(4'd5,8'h70);                 // dst=0x7000
    wreg(4'd6,8'd16); wreg(4'd7,8'h00); wreg(4'd8,8'h00); // len=16
    wreg(4'd9,8'h01);                                   // opcode COPY + trigger
    @(posedge clk); enable=0;
    repeat(3) @(posedge clk);                           // let the trigger edge raise busy
    i=0; while (busy && i<5000) begin @(posedge clk); i=i+1; end
    repeat(8) @(posedge clk);
    errs=0;
    for (i=0;i<16;i=i+1) if (mem[24'h7000+i]!==mem[24'h6000+i]) begin errs=errs+1; $display("  single-op mismatch @%0d", i); end
    if (errs==0) $display("PASS: single-op copy ok"); else $display("FAIL: single-op broken (%0d)", errs);

    reset=1; repeat(4) @(posedge clk); reset=0; @(posedge clk);
    // ---- list mode: program list regs (enable gates reg writes) ----
    enable=1;
    wreg(4'd10, 8'h00); wreg(4'd11, 8'h20); wreg(4'd12, 8'h00); // list_base=0x002000
    wreg(4'd13, 8'd4);  wreg(4'd14, 8'd0);                       // count=4
    wreg(4'd15, 8'h01);                                          // trigger
    @(posedge clk); enable=0;
    $display("after trig: dma_r[15]=%h base=%h%h%h cnt=%h%h state=%h busy=%b ltrig=%b",
      dut.dma_r[15], dut.dma_r[12],dut.dma_r[11],dut.dma_r[10],
      dut.dma_r[14],dut.dma_r[13], dut.state, busy, dut.ltrig_r);
    repeat(3) @(posedge clk);
    $display("3 later:   state=%h busy=%b list_ptr=%h cnt=%h fidx=%h",
      dut.state, busy, dut.list_ptr_r, dut.list_cnt_r, dut.fetch_idx);

    // wait for completion
    i=0; while (busy && i<20000) begin @(posedge clk); i=i+1; end
    if (busy) begin $display("FAIL: still busy after %0d cycles", i); $finish; end
    $display("list done in %0d cycles", i);
    repeat(8) @(posedge clk);  // let the last in-flight write commit (MCU SPI poll latency covers this in HW)

    errs=0;
    for (i=0;i<32;i=i+1) if (mem[24'h3000+i]!==mem[24'h1000+i]) begin errs=errs+1; $display("  copy0 mismatch @%0d: %h != %h", i, mem[24'h3000+i], mem[24'h1000+i]); end
    for (i=0;i<16;i=i+1) if (mem[24'h3100+i]!==mem[24'h1800+i]) begin errs=errs+1; $display("  copy1 mismatch @%0d", i); end
    for (i=0;i<8;i=i+1)  if (mem[24'h3200+i]!==mem[24'h1010+i]) begin errs=errs+1; $display("  copy2 mismatch @%0d", i); end
    // overlap: 0x5002..0x5009 should be the AB seed repeated (A5,3C,A5,3C,...)
    for (i=0;i<8;i=i+1) if (mem[24'h5002+i]!==((i[0]) ? 8'h3C : 8'hA5)) begin errs=errs+1; $display("  RLE mismatch @%0d: %h", i, mem[24'h5002+i]); end
    if (errs==0) $display("PASS: all 4 list copies byte-correct (incl. RLE overlap)");
    else         $display("FAIL: %0d byte mismatches", errs);
    $finish;
  end

  initial begin #2000000 $display("FAIL: timeout"); $finish; end
endmodule
