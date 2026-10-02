`timescale 1ns / 1ps
// tb_st018.v -- bench for the ST018 subsystem (st018.v + st018_cpu.v) with the real firmware.
//
// Streams the 160 KB image through the MCU loader port into an asynchronous SRAM model with
// a 45 ns access time, runs the checksum sweep, releases the ARM and sends the $F1 self test
// and $F2 over the host mailbox, logging every answer with its time. The bus strobes are one
// CLK cycle wide with the bus values valid in that cycle only, at a random phase, as main.v
// drives them. Run it once per rate and compare the two logs: same sums, same answers, same
// instruction count up to the $F1 answer (the half-rate run takes about twice as long).
//
//   python3 -c "d=open('st018.rom','rb').read(); print('\n'.join('%02x'%b for b in d))" > st018.hex
//   iverilog -g2005 -P tb.HALF=0 -o tb_full st018.v st018_cpu.v tb_st018.v && vvp tb_full
//   iverilog -g2005 -P tb.HALF=1 -P tb.RD_CYC=5 -P tb.WE_CYC=3 -P tb.QUIET=900000 \
//            -o tb_half st018.v st018_cpu.v tb_st018.v && vvp tb_half
//
// st018.rom (163,840 bytes) is not part of this repository.
module tb;
  parameter HALF = 0;
  parameter RD_CYC = 8;
  parameter WE_CYC = 6;
  parameter integer TAA = 45;      // SRAM address access time, ns
  parameter integer QUIET = 300000;
  integer seed = 1;
  reg clk = 0; always #5.206 clk = ~clk;   // 96.04 MHz

  reg mcu_reset = 1, snes_reset = 0;
  reg snes_sel = 0; reg [2:1] snes_a = 0; reg [7:0] snes_di = 0;
  wire [7:0] snes_do;
  reg rd_start = 0, rd_end = 0, wr_end = 0;
  reg ld_reset = 0, ld_we = 0; reg [7:0] ld_data = 0; wire ld_busy;
  reg sum_start = 0; wire sum_busy; wire [31:0] sum;
  wire [18:0] RAM_ADDR; wire [7:0] RAM_DATA; wire RAM_OE, RAM_WE;
  wire dbg_retire; wire [31:0] dbg_pc;

  st018 #(.CACHE_LB(8), .SRAM_RD_CYC(RD_CYC), .SRAM_WE_CYC(WE_CYC), .HALF_RATE(HALF)) dut (
    .clk(clk), .mcu_reset(mcu_reset), .snes_reset(snes_reset),
    .snes_sel(snes_sel), .snes_a(snes_a), .snes_di(snes_di), .snes_do(snes_do),
    .snes_rd_start(rd_start), .snes_rd_end(rd_end), .snes_wr_end(wr_end),
    .ld_reset(ld_reset), .ld_we(ld_we), .ld_data(ld_data), .ld_busy(ld_busy),
    .sum_start(sum_start), .sum_busy(sum_busy), .sum(sum),
    .RAM_ADDR(RAM_ADDR), .RAM_DATA(RAM_DATA), .RAM_OE(RAM_OE), .RAM_WE(RAM_WE),
    .dbg_retire(dbg_retire), .dbg_pc(dbg_pc), .dbg_ir(), .dbg_rf_we(), .dbg_rf_wa(),
    .dbg_rf_wd(), .dbg_cpsr(), .dbg_miss_cnt());

  // ---- asynchronous SRAM, 512K x 8: data is X for TAA after any address change
  reg [7:0] mem [0:524287];
  reg [18:0] a_d; reg valid = 0;
  always @(RAM_ADDR) begin valid = 0; a_d <= #TAA RAM_ADDR; end
  always @(a_d) if (a_d === RAM_ADDR) valid = 1;
  assign RAM_DATA = (!RAM_OE && RAM_WE) ? (valid ? mem[RAM_ADDR] : 8'hxx) : 8'hzz;
  time we_fall; reg [18:0] we_addr; integer wr_errs = 0;
  always @(negedge RAM_WE) begin we_fall = $time; we_addr = RAM_ADDR; end
  always @(posedge RAM_WE) if ($time > 0) begin
    if ($time - we_fall < 30 || we_addr !== RAM_ADDR) begin wr_errs = wr_errs + 1; end
    mem[RAM_ADDR] = RAM_DATA;
  end
  always @(RAM_ADDR) if (!RAM_WE) wr_errs = wr_errs + 1;   // address moved under WE

  integer retired = 0;
  always @(posedge clk) if (dut.ce && dbg_retire) retired = retired + 1;

  // ---- stimulus helpers: strobes are one CLK cycle, bus values valid only in that cycle
  reg [7:0] rom [0:163839];
  integer i, n, f; reg [31:0] want; reg [7:0] b;
  task pulse_ld(input [7:0] d);
    begin repeat (1 + ($random(seed) & 1)) @(posedge clk); ld_data <= d; ld_we <= 1; @(posedge clk); ld_we <= 0; ld_data <= 8'hxx;
          @(posedge clk); while (ld_busy) @(posedge clk); end
  endtask
  task host_wr(input [1:0] a, input [7:0] d);
    begin repeat (6 + ($random(seed) & 1)) @(posedge clk); snes_sel <= 1; snes_a <= a; snes_di <= d; wr_end <= 1;
          @(posedge clk); wr_end <= 0; snes_sel <= 0; snes_a <= 2'bxx; snes_di <= 8'hxx; end
  endtask
  task host_rd(input [1:0] a, output [7:0] d);
    begin repeat (6 + ($random(seed) & 1)) @(posedge clk); snes_sel <= 1; snes_a <= a; rd_start <= 1;
          @(posedge clk); rd_start <= 0; snes_sel <= 0; snes_a <= 2'bxx;
          repeat (9) @(posedge clk); d = snes_do; rd_end <= 1; @(posedge clk); rd_end <= 0; end
  endtask
  // send one command byte, then log every byte the ARM answers until it has been quiet
  task command(input [7:0] c, input integer quiet);
    integer idle; reg [7:0] st, v;
    begin
      host_wr(2'b01, c); $fdisplay(f, "cmd %02x at %0.3f ms", c, $realtime/1e6);
      idle = 0;
      while (idle < quiet) begin
        host_rd(2'b10, st);
        if (st[0]) begin host_rd(2'b00, v); $fdisplay(f, "  -> %02x (status %02x) at %0.3f ms, retired %0d", v, st, $realtime/1e6, retired); idle = 0; end
        else idle = idle + 1;
      end
      host_rd(2'b10, st); $fdisplay(f, "  status %02x", st);
    end
  endtask


  initial begin
    $readmemh("st018.hex", rom);
    f = $fopen(HALF ? "out_half.txt" : "out_full.txt");
    want = 0; for (i = 0; i < 163840; i = i + 1) want = want + rom[i];
    repeat (20) @(posedge clk);
    @(posedge clk); ld_reset <= 1; @(posedge clk); ld_reset <= 0;
    repeat (600) @(posedge clk);                       // cache invalidation
    for (i = 0; i < 163840; i = i + 1) pulse_ld(rom[i]);
    @(posedge clk); sum_start <= 1; repeat (8) @(posedge clk); sum_start <= 0;   // held like a SPI byte
    @(posedge clk); while (sum_busy) @(posedge clk);
    $fdisplay(f, "loader: sum %08x want %08x %s, sram write errors %0d", sum, want,
              (sum === want) ? "OK" : "MISMATCH", wr_errs);
    n = 0; for (i = 0; i < 163840; i = i + 1) if (mem[i] !== rom[i]) n = n + 1;
    $fdisplay(f, "sram image: %0d bytes differ", n);
    mcu_reset <= 0;
    @(posedge clk); snes_reset <= 1; @(posedge clk); snes_reset <= 0;
    repeat (20000) @(posedge clk);
    host_rd(2'b10, b); $fdisplay(f, "status after release %02x, retired %0d", b, retired);
    command(8'hf1, QUIET);
    $fdisplay(f, "retired %0d", retired);
    command(8'hf2, QUIET);
    $fdisplay(f, "retired %0d, time %0t", retired, $time);
    $fclose(f); $finish;
  end
endmodule
