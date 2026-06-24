`timescale 1ns/1ps
// Clock-accurate testbench: drives the REAL bsx.v receiver with the Satellaview
// Town's BATCHED read pattern (FUN_80c30b): per batch read $218A -> count=min(.,20);
// LOOP1 count*$218B; $218D; LOOP2 22*count*$218C.  The ring data is a ramp
// (ring[a]=a&0xff); a stall (bs_dl_addr frozen because bs_dt_queue==0) shows as a
// non-sequential byte.  Probes the receiver's internal queues at the first stall.
module tb;
  reg clkin = 0;
  always #5 clkin = ~clkin;

  reg reg_oe_falling=0, reg_oe_rising=0, reg_we_rising=0, pgm_we=0;
  reg [23:0] snes_addr_in=0;
  reg [7:0] reg_data_in=0, reg_reset_bits=0, reg_set_bits=0;
  reg use_bsx=1, bs_slot=0, bs_hirom=0, feat_bs_base_enable=1;
  reg [59:0] rtc_data_in=0;
  reg bs_dl_arm=0, bs_dl_stage=0;
  reg [9:0] bs_dl_chan=0;
  reg [16:0] bs_dl_base=0;
  reg [15:0] bs_dl_frames=0;

  wire [7:0] reg_data_out;
  wire [14:0] regs_out;
  wire data_ovr, flash_writable, bs_page_enable, bs_dl_enable;
  wire [9:0] bs_page_out;
  wire [8:0] bs_page_offset;
  wire [1:0] bs_erase_seq, bs_dl_seq;
  wire [3:0] bs_erase_blk;
  wire [16:0] bs_dl_offset;

  bsx dut(
    .clkin(clkin), .reg_oe_falling(reg_oe_falling), .reg_oe_rising(reg_oe_rising),
    .reg_we_rising(reg_we_rising), .snes_addr_in(snes_addr_in), .reg_data_in(reg_data_in),
    .reg_data_out(reg_data_out), .reg_reset_bits(reg_reset_bits), .reg_set_bits(reg_set_bits),
    .regs_out(regs_out), .pgm_we(pgm_we), .use_bsx(use_bsx), .bs_slot(bs_slot), .bs_hirom(bs_hirom),
    .data_ovr(data_ovr), .flash_writable(flash_writable), .rtc_data_in(rtc_data_in),
    .bs_page_out(bs_page_out), .bs_page_enable(bs_page_enable), .bs_page_offset(bs_page_offset),
    .feat_bs_base_enable(feat_bs_base_enable), .bs_erase_seq(bs_erase_seq), .bs_erase_blk(bs_erase_blk),
    .bs_dl_enable(bs_dl_enable), .bs_dl_offset(bs_dl_offset), .bs_dl_seq(bs_dl_seq),
    .bs_dl_arm(bs_dl_arm), .bs_dl_chan(bs_dl_chan), .bs_dl_stage(bs_dl_stage),
    .bs_dl_base(bs_dl_base), .bs_dl_frames(bs_dl_frames)
  );

  reg [7:0] rdata;
  integer K, Mg;   // bus timing (plusargs): read pulse width / inter-access idle clocks

  task idle; input integer n; integer i; begin
    for(i=0;i<n;i=i+1) @(posedge clkin);
  end endtask

  task wr; input [7:0] a; input [7:0] d; begin
    @(negedge clkin); snes_addr_in = 24'h002188 + (a-8'h08); reg_data_in = d;
    @(negedge clkin); reg_we_rising = 1;
    @(posedge clkin); #1 reg_we_rising = 0;
    idle(Mg);
  end endtask

  task rd; input [7:0] a; begin
    @(negedge clkin); snes_addr_in = 24'h002188 + (a-8'h08);
    @(negedge clkin); reg_oe_falling = 1;
    @(posedge clkin); #1 reg_oe_falling = 0;
    idle(K);
    if (bs_dl_enable) rdata = bs_dl_offset[7:0]; else rdata = reg_data_out; // ring[a]=a&0xff
    @(negedge clkin); reg_oe_rising = 1;
    @(posedge clkin); #1 reg_oe_rising = 0;
    idle(Mg);
  end endtask

  task stagefrag; input [16:0] base; input [15:0] frames; begin
    bs_dl_base = base; bs_dl_frames = frames;
    @(negedge clkin); bs_dl_stage = 1;
    @(posedge clkin); idle(2);
    @(negedge clkin); bs_dl_stage = 0;
    idle(2);
  end endtask

  integer i, b, count, expected, errors, total;
  integer frag, dgbyte, pendpref, st, guard;
  reg [1:0] last_seq;

  initial begin
    if(!$value$plusargs("K=%d", K)) K = 4;
    if(!$value$plusargs("Mg=%d", Mg)) Mg = 6;
    idle(10);
    bs_dl_chan = 10'h125;                  // MCU armed this download channel
    wr(8'h08, 8'h25); wr(8'h09, 8'h01);   // tune channel 0x125
    bs_dl_arm = 1;
    wr(8'h0b, 8'h01); wr(8'h0c, 8'h01);   // enable latches
    last_seq = bs_dl_seq;
    stagefrag(17'd0, 16'd1490);           // prime frag0 (base 0, 1490 frames)
    frag = 0; dgbyte = 0; errors = 0; guard = 0;
    // Town: receive a 4-fragment program, like the real download
    while (frag < 4 && guard < 2000) begin
      guard = guard+1;
      // model the host/MCU round-trip LATENCY before the fragment is staged (the Town
      // spins on $218A=0 during this, like hardware).
      if (bs_dl_seq != last_seq) begin last_seq = bs_dl_seq; idle(3000); stagefrag(17'd0, 16'd1490); end
      rd(8'h0a); count = rdata;            // $218A
      if (count & 8'h80) begin
        $display("ABORT(overrun) $218A=%02x | frag %0d dgbyte %0d", count, frag, dgbyte); $finish;
      end else if (count == 0) begin
        idle(60);
      end else begin
        if (count > 20) count = 20;
        pendpref = 0;
        for (i=0;i<count;i=i+1) begin      // LOOP1 prefixes
          rd(8'h0b);
          if (guard <= 2)
            $display("  pfx b%0d[%0d]=%02x | pf=%0d dt=%0d dlq=%0d first=%b addr=%0d",
                     guard, i, rdata, dut.bs_pf_queue, dut.bs_dt_queue, dut.bs_dl_queue, dut.bs_dl_first, dut.bs_dl_addr);
          if (rdata == 8'h90) errors = errors+1;  // 0x90 = bogus (start|end same frame)
          if (rdata & 8'h80) pendpref = 1; // Packet-End in the prefix
        end
        rd(8'h0d); st = rdata;             // $218D status (read once, between loops)
        for (i=0;i<22*count;i=i+1) begin   // LOOP2 data
          rd(8'h0c);
          if (rdata !== (dgbyte & 8'hff)) begin
            if (errors < 4)
              $display("DATA frag%0d @dg-byte %0d: got %02x want %02x | pf=%0d dt=%0d dlq=%0d addr=%0d st=%02x",
                       frag, dgbyte, rdata, (dgbyte & 8'hff),
                       dut.bs_pf_queue, dut.bs_dt_queue, dut.bs_dl_queue, dut.bs_dl_addr, st);
            errors = errors+1;
          end
          dgbyte = dgbyte+1;
        end
        if (pendpref || (st & 8'h80)) begin   // data group ended (Packet-End)
          $display("frag %0d DONE: dgbyte=%0d  $218D=%02x pendpref=%b (expect 32780)", frag, dgbyte, st, pendpref);
          frag = frag+1; dgbyte = 0;
        end
      end
    end
    $display("FINISHED: frags=%0d/4  errors=%0d  guard=%0d", frag, errors, guard);
    $finish;
  end
endmodule
