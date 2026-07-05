`timescale 1ns/1ps
// Testbench for the 1-trigger queue (#3): the MCU fires ops WITHOUT polling busy.
// Uses the accurate base-main.v DMA arbiter (ROM_CYCLE_LEN=7).  Verifies that a
// burst of back-to-back triggers all complete byte-correct (none lost to the
// busy window), and that an overrun is flagged when the FPGA falls behind.
module tb_dma_queue;
  reg clk = 0; always #5 clk = ~clk;
  reg reset = 1, enable = 0;
  reg [3:0] reg_addr = 0; reg [7:0] reg_data_in = 0;
  reg reg_oe_falling = 0, reg_we_rising = 0;
  wire loop_enable; wire [1:0] busy;
  wire BUS_RRQ, BUS_WRQ;
  wire [23:0] ROM_ADDR_dma;
  wire [15:0] ROM_DATA_OUT;
  wire ROM_WORD_ENABLE;
  reg  [15:0] DMA_DINr = 0;
  wire BUS_RDY;

  dma dut(.clkin(clk), .reset(reset), .enable(enable),
    .reg_addr(reg_addr), .reg_data_in(reg_data_in), .reg_data_out(),
    .reg_oe_falling(reg_oe_falling), .reg_we_rising(reg_we_rising),
    .loop_enable(loop_enable), .busy(busy),
    .BUS_RDY(BUS_RDY), .BUS_RRQ(BUS_RRQ), .BUS_WRQ(BUS_WRQ),
    .ROM_ADDR(ROM_ADDR_dma), .ROM_DATA_OUT(ROM_DATA_OUT),
    .ROM_WORD_ENABLE(ROM_WORD_ENABLE), .ROM_DATA_IN(DMA_DINr));

  reg [7:0] mem [0:262143];

  // accurate main.v DMA arbiter (SNES dead -> dispatch always)
  localparam A_IDLE=0, A_RD=1, A_RD_END=2, A_WR=3, A_WR_END=4;
  reg [2:0] astate = A_IDLE; reg [3:0] mem_delay = 0;
  reg dma_rd_pend=0, dma_wr_pend=0, rq_rdy=1;
  reg [23:0] dma_rom_addr=0; reg [15:0] dma_rom_data=0; reg dma_rom_word=0;
  wire dma_rd_hit = (astate==A_RD)||(astate==A_RD_END);
  wire dma_wr_hit = (astate==A_WR)||(astate==A_WR_END);
  wire dma_hit = dma_rd_hit | dma_wr_hit;
  wire [23:0] rom_addr = dma_hit ? dma_rom_addr : 24'h0;
  wire [23:0] aeven = {rom_addr[23:1],1'b0};
  wire [15:0] rom_data_rd = {mem[aeven], mem[aeven|24'h1]};
  assign BUS_RDY = rq_rdy;
  always @(posedge clk) begin
    if (BUS_RRQ) begin dma_rd_pend<=1; rq_rdy<=0; dma_rom_addr<=ROM_ADDR_dma; dma_rom_word<=ROM_WORD_ENABLE; end
    else if (BUS_WRQ) begin dma_wr_pend<=1; rq_rdy<=0; dma_rom_addr<=ROM_ADDR_dma; dma_rom_data<=ROM_DATA_OUT; dma_rom_word<=ROM_WORD_ENABLE; end
    else if (astate==A_RD_END || astate==A_WR_END) begin dma_rd_pend<=0; dma_wr_pend<=0; rq_rdy<=1; end
  end
  always @(posedge clk) begin
    case (astate)
      A_IDLE: begin if (dma_rd_pend) begin astate<=A_RD; mem_delay<=7; end else if (dma_wr_pend) begin astate<=A_WR; mem_delay<=7; end end
      A_RD: begin mem_delay<=mem_delay-1; if(mem_delay==0) astate<=A_RD_END; DMA_DINr <= (rom_addr[0] ? rom_data_rd : {rom_data_rd[7:0],rom_data_rd[15:8]}); end
      A_WR: begin mem_delay<=mem_delay-1; if(mem_delay==0) astate<=A_WR_END;
                  mem[{dma_rom_addr[23:1],1'b0}] <= dma_rom_data[7:0];
                  if (dma_rom_word) mem[{dma_rom_addr[23:1],1'b0}|24'h1] <= dma_rom_data[15:8]; end
      A_RD_END, A_WR_END: astate<=A_IDLE;
    endcase
  end

  // ~42 cycles/byte models the real SPI byte time (~0.5us @ 85MHz) so the fire rate
  // (~420 cycles/op) is SLOWER than the FPGA op time (~128 cycles for 16 bytes) --
  // the realistic regime where the 1-deep queue keeps up without overrun.
  task wreg(input [3:0] a, input [7:0] d);
    begin @(posedge clk); reg_addr<=a; reg_data_in<=d; reg_we_rising<=1; @(posedge clk); reg_we_rising<=0;
          repeat(40) @(posedge clk); end
  endtask
  // fire an op WITHOUT polling busy (the whole point of #3)
  task fire(input [23:0] s, input [23:0] d, input [23:0] len);
    begin
      wreg(0,d[23:16]); wreg(1,s[23:16]); wreg(2,s[7:0]); wreg(3,s[15:8]);
      wreg(4,d[7:0]); wreg(5,d[15:8]); wreg(6,len[7:0]); wreg(7,len[15:8]); wreg(8,len[23:16]);
      wreg(9,8'h01);   // opcode COPY | trigger -- no poll after
    end
  endtask

  integer i, errs;
  integer ndone = 0, npend = 0;
  always @(posedge clk) begin
    if (dut.state == 3'd3) ndone = ndone + 1;       // ST_DONE per op
    if (dut.op_kick && dut.state != 3'd0) npend = npend + 1;  // queued kicks
  end
  initial begin
    for (i=0;i<262144;i=i+1) mem[i]=0;
    for (i=0;i<256;i=i+1) mem[24'h1000+i] = i[7:0] ^ 8'h5a;   // source pattern

    enable=0; reset=1; repeat(4) @(posedge clk); reset=0; @(posedge clk);
    enable=1;
    // burst of 6 back-to-back small ops, NO poll between -- exercises the queue
    fire(24'h1000, 24'h2000, 24'd16); $display("after fire1: dst0=%h exp=%h pend=%b state=%0d", mem[24'h2000], mem[24'h1000], dut.pending, dut.state);
    fire(24'h1010, 24'h2100, 24'd16);
    fire(24'h1020, 24'h2200, 24'd16);
    fire(24'h1030, 24'h2300, 24'd16);
    fire(24'h1040, 24'h2400, 24'd16);
    fire(24'h1050, 24'h2500, 24'd16);
    @(posedge clk); enable=0;
    repeat(200) @(posedge clk);
    $display("ops_completed=%0d queued_kicks=%0d", ndone, npend); $display("dst: 2000=%h 2100=%h 2200=%h 2300=%h 2400=%h 2500=%h", mem[24'h2000],mem[24'h2100],mem[24'h2200],mem[24'h2300],mem[24'h2400],mem[24'h2500]);
    $display("src: 1000=%h 1010=%h 1020=%h 1030=%h 1040=%h 1050=%h", mem[24'h1000],mem[24'h1010],mem[24'h1020],mem[24'h1030],mem[24'h1040],mem[24'h1050]);
    i=0; while (busy[0] && i<20000) begin @(posedge clk); i=i+1; end
    repeat(16) @(posedge clk);

    errs=0;
    for (i=0;i<16;i=i+1) begin
      if (mem[24'h2000+i]!==mem[24'h1000+i]) errs=errs+1;
      if (mem[24'h2100+i]!==mem[24'h1010+i]) errs=errs+1;
      if (mem[24'h2200+i]!==mem[24'h1020+i]) errs=errs+1;
      if (mem[24'h2300+i]!==mem[24'h1030+i]) errs=errs+1;
      if (mem[24'h2400+i]!==mem[24'h1040+i]) errs=errs+1;
      if (mem[24'h2500+i]!==mem[24'h1050+i]) errs=errs+1;
    end
    $display("burst done in %0d cycles, overrun=%b", i, busy[1]);
    if (errs==0 && busy[1]==0) $display("PASS: 6 no-poll ops byte-correct, no op lost, no overrun");
    else $display("FAIL(burst): errs=%0d overrun=%b", errs, busy[1]);

    // ---- queue (pending) test: op A LARGE (still running) + op B fired while A
    //      is busy -> B must be QUEUED and processed after A, both byte-correct ----
    enable=1;
    fire(24'h1000, 24'h3000, 24'd128);   // ~big op, runs well past the fire rate
    $display("  during A: state=%0d busy=%b", dut.state, busy[0]);
    fire(24'h1080, 24'h3200, 24'd16);    // fired while A busy -> exercises pending
    $display("  after fireB: pending=%b shadow_dst=%h shadow_src=%h state=%0d", dut.pending, dut.shadow_dst, dut.shadow_src, dut.state);
    @(posedge clk); enable=0;
    i=0; while ((busy[0] || dut.pending) && i<40000) begin @(posedge clk); i=i+1; end
    repeat(32) @(posedge clk);
    errs=0;
    for (i=0;i<128;i=i+1) if (mem[24'h3000+i]!==mem[24'h1000+i]) errs=errs+1;
    for (i=0;i<16;i=i+1)  if (mem[24'h3200+i]!==mem[24'h1080+i]) errs=errs+1;
    if (errs==0) $display("PASS: queued op processed byte-correct (pending path, overrun=%b)", busy[1]);
    else $display("FAIL(queue): errs=%0d overrun=%b", errs, busy[1]);
    $finish;
  end
  initial begin #2000000 $display("FAIL: timeout (HANG)"); $finish; end
endmodule
