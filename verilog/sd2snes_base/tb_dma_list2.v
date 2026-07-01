`timescale 1ns/1ps
// Accurate testbench: replicates the REAL base main.v DMA arbiter (ROM_CYCLE_LEN=7,
// the RQ_DMA_RDYr handshake, the ST_DMA_RD/WR states) to reproduce the hardware hang
// the 3-cycle approximation in tb_dma_list.v missed.
module tb_dma_list2;
  reg clk = 0; always #5 clk = ~clk;
  reg reset = 1, enable = 0;
  reg [3:0] reg_addr = 0; reg [7:0] reg_data_in = 0;
  reg reg_oe_falling = 0, reg_we_rising = 0;
  wire loop_enable, busy;
  wire BUS_RRQ, BUS_WRQ;
  wire [23:0] ROM_ADDR_dma;       // dma.v ROM_ADDR output
  wire [15:0] ROM_DATA_OUT;
  wire ROM_WORD_ENABLE;
  reg  [15:0] DMA_DINr = 0;       // main.v read latch -> dma.v ROM_DATA_IN
  wire BUS_RDY;

  dma dut(.clkin(clk), .reset(reset), .enable(enable),
    .reg_addr(reg_addr), .reg_data_in(reg_data_in), .reg_data_out(),
    .reg_oe_falling(reg_oe_falling), .reg_we_rising(reg_we_rising),
    .loop_enable(loop_enable), .busy(busy),
    .BUS_RDY(BUS_RDY), .BUS_RRQ(BUS_RRQ), .BUS_WRQ(BUS_WRQ),
    .ROM_ADDR(ROM_ADDR_dma), .ROM_DATA_OUT(ROM_DATA_OUT),
    .ROM_WORD_ENABLE(ROM_WORD_ENABLE), .ROM_DATA_IN(DMA_DINr));

  reg [7:0] mem [0:1048575];

  // ---- exact main.v DMA arbiter (SNES held dead -> dispatch always allowed) ----
  localparam A_IDLE=0, A_RD=1, A_RD_END=2, A_WR=3, A_WR_END=4;
  reg [2:0] astate = A_IDLE;
  reg [3:0] mem_delay = 0;
  reg dma_rd_pend=0, dma_wr_pend=0, rq_rdy=1;
  reg [23:0] dma_rom_addr=0; reg [15:0] dma_rom_data=0; reg dma_rom_word=0;

  wire dma_rd_hit = (astate==A_RD)||(astate==A_RD_END);
  wire dma_wr_hit = (astate==A_WR)||(astate==A_WR_END);
  wire dma_hit = dma_rd_hit | dma_wr_hit;
  wire [23:0] rom_addr = dma_hit ? dma_rom_addr : 24'h0;          // ROM_ADDR mux (DMA_HIT)
  wire [23:0] aeven = {rom_addr[23:1],1'b0};
  wire [15:0] rom_data_rd = {mem[aeven], mem[aeven|24'h1]};       // [15:8]=even byte, [7:0]=odd

  assign BUS_RDY = rq_rdy;

  // request block (main.v: latch on RRQ/WRQ, release at *_END)
  always @(posedge clk) begin
    if (BUS_RRQ) begin dma_rd_pend<=1; rq_rdy<=0; dma_rom_addr<=ROM_ADDR_dma; dma_rom_word<=ROM_WORD_ENABLE; end
    else if (BUS_WRQ) begin dma_wr_pend<=1; rq_rdy<=0; dma_rom_addr<=ROM_ADDR_dma; dma_rom_data<=ROM_DATA_OUT; dma_rom_word<=ROM_WORD_ENABLE; end
    else if (astate==A_RD_END || astate==A_WR_END) begin dma_rd_pend<=0; dma_wr_pend<=0; rq_rdy<=1; end
  end

  // state machine (main.v: ROM_CYCLE_LEN=7; latch DMA_DINr each cycle in A_RD)
  always @(posedge clk) begin
    case (astate)
      A_IDLE: begin
        if (dma_rd_pend) begin astate<=A_RD; mem_delay<=7; end
        else if (dma_wr_pend) begin astate<=A_WR; mem_delay<=7; end
      end
      A_RD: begin mem_delay<=mem_delay-1; if(mem_delay==0) astate<=A_RD_END;
                  DMA_DINr <= (rom_addr[0] ? rom_data_rd : {rom_data_rd[7:0],rom_data_rd[15:8]}); end
      A_WR: begin mem_delay<=mem_delay-1; if(mem_delay==0) astate<=A_WR_END;
                  // commit write (word mode: both bytes; main.v swaps -> copy preserves bytes)
                  mem[{dma_rom_addr[23:1],1'b0}]        <= dma_rom_data[7:0];
                  if (dma_rom_word) mem[{dma_rom_addr[23:1],1'b0}|24'h1] <= dma_rom_data[15:8]; end
      A_RD_END, A_WR_END: astate<=A_IDLE;
    endcase
  end

  task wreg(input [3:0] a, input [7:0] d);
    begin @(posedge clk); reg_addr<=a; reg_data_in<=d; reg_we_rising<=1; @(posedge clk); reg_we_rising<=0; end
  endtask
  task desc(input [23:0] off, input [23:0] s, input [23:0] dd, input [23:0] len);
    begin mem[off+0]=dd[23:16]; mem[off+1]=s[23:16]; mem[off+2]=s[7:0]; mem[off+3]=s[15:8];
          mem[off+4]=dd[7:0]; mem[off+5]=dd[15:8]; mem[off+6]=len[7:0]; mem[off+7]=len[15:8];
          mem[off+8]=len[23:16]; mem[off+9]=8'h00; end
  endtask

  integer i, errs;
  initial begin
    for (i=0;i<1048576;i=i+1) mem[i]=0;
    for (i=0;i<32;i=i+1) mem[24'h1000+i] = i + 8'h40;
    for (i=0;i<16;i=i+1) mem[24'h1800+i] = 8'hF0 ^ i;
    desc(24'h2000, 24'h1000, 24'h3000, 24'd32);
    desc(24'h200A, 24'h1800, 24'h3100, 24'd16);
    desc(24'h2014, 24'h1010, 24'h3200, 24'd8);

    enable=0; reset=1; repeat(4) @(posedge clk); reset=0; @(posedge clk);
    enable=1;
    wreg(4'd10,8'h00); wreg(4'd11,8'h20); wreg(4'd12,8'h00);
    wreg(4'd13,8'd3);  wreg(4'd14,8'd0);
    wreg(4'd15,8'h01);
    @(posedge clk); enable=0;
    repeat(3) @(posedge clk);
    i=0; while (busy && i<200000) begin @(posedge clk); i=i+1; end
    if (busy) begin $display("FAIL/HANG: still busy after %0d cycles (state=%0d astate=%0d fidx=%0d)", i, dut.state, astate, dut.fetch_idx); $finish; end
    $display("list done in %0d cycles", i);
    repeat(16) @(posedge clk);
    errs=0;
    for (i=0;i<32;i=i+1) if (mem[24'h3000+i]!==mem[24'h1000+i]) errs=errs+1;
    for (i=0;i<16;i=i+1) if (mem[24'h3100+i]!==mem[24'h1800+i]) errs=errs+1;
    for (i=0;i<8;i=i+1)  if (mem[24'h3200+i]!==mem[24'h1010+i]) errs=errs+1;
    if (errs==0) $display("PASS: accurate-arbiter list copies byte-correct");
    else         $display("FAIL: %0d byte mismatches", errs);
    $finish;
  end
  initial begin #5000000 $display("FAIL: global timeout (HANG)"); $finish; end
endmodule
