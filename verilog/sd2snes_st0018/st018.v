`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////
// st018.v -- Seta ST018 (ARMv3 coprocessor) for sd2snes
//
// Written for sd2snes (GPL-2.0). Behaviour of the host/ARM mailbox follows
// the published emulator implementations (the only public references).
//
// ARM address map (bits 31:29 select the region, as on the real chip):
//   0x0xxxxxxx  program ROM 128 KB (mirrored)  -> Bus 2 SRAM 0x00000, cached
//   0xAxxxxxxx  data ROM     32 KB (mirrored)  -> Bus 2 SRAM 0x20000, cached
//   0xExxxxxxx  work RAM     16 KB (mirrored)  -> on-chip block RAM
//   0x4xxxxxxx  I/O:  +00 W  byte to host      (sets status bit 0)
//                     +10 R  byte from host    (clears status bit 3)
//                     +10 W  "signal"          (sets status bit 2)
//                     +20 R  status
//                     +20..+2C W  timer -- no observable effect in either
//                                   reference; accepted and ignored
//   everything else reads 0, writes ignored. Instruction fetches from the I/O
//   region read 0 and have no side effects.
//
// Host side ($00-3F,$80-BF:3800-38FF, decoded by address.v; A0 ignored):
//   $3800 R  byte from ARM (clears status bit 0)
//   $3802 R  (clears status bit 2, bus not driven)   W  byte to ARM (sets bit 3)
//   $3804 R  status                                   W  !=0 holds ARM in reset
//   status = {~reset, 0, 0, 0, host->ARM full, signal, 0, ARM->host full}
//            (bit 7 reflects either reset source: $3804 or the MCU hold)
//   Status bit 5 must read 0: the firmware jumps to 0x60000000 if it is set.
//
// Program/data ROM live in the board's Bus 2 SRAM (8 bit, 45 ns); the MCU
// streams st018.rom (160 KB) into it through the loader port while the ARM is
// held in reset. A direct-mapped read-only cache with 16-byte lines sits in
// front of it; work RAM and I/O never touch the SRAM.
//////////////////////////////////////////////////////////////////////////////
module st018_core #(
  parameter CACHE_LB = 8,           // log2(lines): 8 -> 4 KB (mk2), 10 -> 16 KB (mk3)
  parameter SRAM_RD_CYC = 8,        // enabled CLK cycles per byte in a read burst (see below)
  parameter SRAM_WE_CYC = 6,        // WE low time in enabled CLK cycles
  parameter IN_REG = 0              // 1: take every input through a register first (HALF_RATE)
)(
  input             clk,
  input             ce,             // clock enable: every register here advances only when high
  input             mcu_reset_i,    // MCU hold: 1 while firmware is loaded
  input             snes_reset_i,   // console reset strobe

  // SNES host side
  input             snes_sel_i,     // $00-3F/$80-BF:3800-38FF
  input      [2:1]  snes_a_i,
  input      [7:0]  snes_di_i,
  output     [7:0]  a2s_data_o,     // read data for $3800 / $3804, muxed by the wrapper
  output     [7:0]  status_o,
  input             snes_rd_start_i,
  input             snes_rd_end_i,
  input             snes_wr_end_i,

  // MCU loader (streamed firmware image) and verification sweep
  input             ld_reset_i,     // rising edge: pointer = 0, cache invalidate
  input             ld_we_i,        // rising edge: write ld_data at pointer, pointer++
  input      [7:0]  ld_data_i,
  output            ld_busy,
  input             sum_start_i,    // rising edge: checksum the 160 KB image in SRAM
  output reg        sum_busy,
  output reg [31:0] sum,

  // Bus 2 SRAM
  output reg [18:0] RAM_ADDR,
  inout      [7:0]  RAM_DATA,
  output reg        RAM_OE,
  output reg        RAM_WE,

  // debug / verification
  output            dbg_retire,
  output     [31:0] dbg_pc,
  output     [31:0] dbg_ir,
  output            dbg_rf_we,
  output     [4:0]  dbg_rf_wa,
  output     [31:0] dbg_rf_wd,
  output     [31:0] dbg_cpsr,
  output reg [31:0] dbg_miss_cnt
);

localparam IMG_SIZE = 18'h28000;
localparam TAGW = 14 - CACHE_LB;              // sram address bits 17 .. 4+LB

// ---------------------------------------------------------------------------
// input stage. IN_REG = 1 registers every input under ce, so that each path
// into the logic below starts at a register of this module (see the wrapper).
// ---------------------------------------------------------------------------
wire       mcu_reset, snes_reset, snes_sel, snes_rd_start, snes_rd_end, snes_wr_end;
wire       ld_reset, ld_we, sum_start;
wire [2:1] snes_a;
wire [7:0] snes_di, ld_data;
generate if (IN_REG) begin : g_inreg
  reg       r_mcu_reset = 1'b1, r_snes_reset = 1'b0, r_sel = 1'b0;
  reg       r_rd_start = 1'b0, r_rd_end = 1'b0, r_wr_end = 1'b0;
  reg       r_ld_reset = 1'b0, r_ld_we = 1'b0, r_sum_start = 1'b0;
  reg [2:1] r_a = 2'b00;
  reg [7:0] r_di = 8'h00, r_ld_data = 8'h00;
  always @(posedge clk) if (ce) begin
    r_mcu_reset <= mcu_reset_i; r_snes_reset <= snes_reset_i; r_sel <= snes_sel_i;
    r_rd_start <= snes_rd_start_i; r_rd_end <= snes_rd_end_i; r_wr_end <= snes_wr_end_i;
    r_ld_reset <= ld_reset_i; r_ld_we <= ld_we_i; r_sum_start <= sum_start_i;
    r_a <= snes_a_i; r_di <= snes_di_i; r_ld_data <= ld_data_i;
  end
  assign mcu_reset = r_mcu_reset; assign snes_reset = r_snes_reset; assign snes_sel = r_sel;
  assign snes_rd_start = r_rd_start; assign snes_rd_end = r_rd_end; assign snes_wr_end = r_wr_end;
  assign ld_reset = r_ld_reset; assign ld_we = r_ld_we; assign sum_start = r_sum_start;
  assign snes_a = r_a; assign snes_di = r_di; assign ld_data = r_ld_data;
end else begin : g_noinreg
  assign mcu_reset = mcu_reset_i; assign snes_reset = snes_reset_i; assign snes_sel = snes_sel_i;
  assign snes_rd_start = snes_rd_start_i; assign snes_rd_end = snes_rd_end_i; assign snes_wr_end = snes_wr_end_i;
  assign ld_reset = ld_reset_i; assign ld_we = ld_we_i; assign sum_start = sum_start_i;
  assign snes_a = snes_a_i; assign snes_di = snes_di_i; assign ld_data = ld_data_i;
end endgenerate

// ---------------------------------------------------------------------------
// mailbox
// ---------------------------------------------------------------------------
reg [7:0] a2s_data, s2a_data;
reg       a2s_ready, s2a_ready, signal, arm_hold;
reg       a2s_wr_since_rd, sig_set_since_rd;
reg       rd_pend_a2s, rd_pend_sig;
wire [7:0] status = {~(arm_hold | mcu_reset), 3'b000, s2a_ready, signal, 1'b0, a2s_ready};

initial begin
  a2s_data = 8'd0; s2a_data = 8'd0; a2s_ready = 1'b0; s2a_ready = 1'b0;
  signal = 1'b0; arm_hold = 1'b0; a2s_wr_since_rd = 1'b0; sig_set_since_rd = 1'b0;
  rd_pend_a2s = 1'b0; rd_pend_sig = 1'b0;
end
assign a2s_data_o = a2s_data;
assign status_o   = status;

wire cpu_rst = mcu_reset | arm_hold | snes_reset;

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
wire        m_req, m_we, m_byte, m_code;
wire [31:0] m_addr, m_wdata;
reg  [31:0] m_rdata;
reg         m_ack;

st018_cpu u_cpu (
  .clk(clk), .ce(ce), .rst(cpu_rst),
  .m_req(m_req), .m_we(m_we), .m_byte(m_byte), .m_code(m_code),
  .m_addr(m_addr), .m_wdata(m_wdata), .m_rdata(m_rdata), .m_ack(m_ack),
  .dbg_retire(dbg_retire), .dbg_pc(dbg_pc), .dbg_ir(dbg_ir),
  .dbg_rf_we(dbg_rf_we), .dbg_rf_wa(dbg_rf_wa), .dbg_rf_wd(dbg_rf_wd),
  .dbg_cpsr(dbg_cpsr)
);

// ---------------------------------------------------------------------------
// address decode of the CPU request (m_addr is a register in the CPU and is
// stable for as long as m_req is held)
// ---------------------------------------------------------------------------
wire [2:0]  region  = m_addr[31:29];
wire        r_rom   = (region == 3'd0) || (region == 3'd5);
wire        r_wram  = (region == 3'd7);
wire        r_io    = (region == 3'd2);
wire [17:0] sram_a  = (region == 3'd5) ? {3'b100, m_addr[14:0]} : {1'b0, m_addr[16:0]};
wire [CACHE_LB-1:0] c_idx = sram_a[CACHE_LB+3:4];
wire [TAGW-1:0]     c_tag = sram_a[17:CACHE_LB+4];

// ---------------------------------------------------------------------------
// work RAM: 4 byte lanes x 4096 (single port, block RAM)
// ---------------------------------------------------------------------------
reg  [3:0] wr_be;
wire [11:0] wr_a = m_addr[13:2];
reg  [7:0] wram0 [0:4095];
reg  [7:0] wram1 [0:4095];
reg  [7:0] wram2 [0:4095];
reg  [7:0] wram3 [0:4095];
reg  [7:0] wq0, wq1, wq2, wq3;
always @(posedge clk) if (ce) begin
  if (wr_be[0]) wram0[wr_a] <= m_wdata[7:0];
  wq0 <= wram0[wr_a];
end
always @(posedge clk) if (ce) begin
  if (wr_be[1]) wram1[wr_a] <= m_wdata[15:8];
  wq1 <= wram1[wr_a];
end
always @(posedge clk) if (ce) begin
  if (wr_be[2]) wram2[wr_a] <= m_wdata[23:16];
  wq2 <= wram2[wr_a];
end
always @(posedge clk) if (ce) begin
  if (wr_be[3]) wram3[wr_a] <= m_wdata[31:24];
  wq3 <= wram3[wr_a];
end

// ---------------------------------------------------------------------------
// ROM cache: data (lines x 4 words x 32 bit) and tags ({valid, tag})
// ---------------------------------------------------------------------------
reg                    cd_we;
reg  [CACHE_LB+1:0]    cd_wa;
reg  [31:0]            cd_wd;
wire [CACHE_LB+1:0]    cd_ra = {c_idx, sram_a[3:2]};
reg  [31:0]            cdata [0:(4<<CACHE_LB)-1];
reg  [31:0]            cd_q;
always @(posedge clk) if (ce) begin
  if (cd_we) cdata[cd_wa] <= cd_wd;
  cd_q <= cdata[cd_we ? cd_wa : cd_ra];
end

reg                    ct_we;
reg  [CACHE_LB-1:0]    ct_wa;
reg  [TAGW:0]          ct_wd;
reg  [TAGW:0]          ctag [0:(1<<CACHE_LB)-1];
reg  [TAGW:0]          ct_q;
integer ti;
initial for (ti = 0; ti < (1 << CACHE_LB); ti = ti + 1) ctag[ti] = {(TAGW+1){1'b0}};
always @(posedge clk) if (ce) begin
  if (ct_we) ctag[ct_wa] <= ct_wd;
  ct_q <= ctag[ct_we ? ct_wa : c_idx];
end

// ---------------------------------------------------------------------------
// SRAM data input synchroniser (the SRAM is asynchronous to CLK)
// ---------------------------------------------------------------------------
reg [7:0] ram_s1, ram_s2;
reg [7:0] ram_dout;
reg       ram_drive;
always @(posedge clk) if (ce) begin
  ram_s1 <= RAM_DATA;
  ram_s2 <= ram_s1;
end
assign RAM_DATA = ram_drive ? ram_dout : 8'bz;

// ---------------------------------------------------------------------------
// memory / SRAM state machine
// ---------------------------------------------------------------------------
localparam [3:0] F_IDLE = 4'd0, F_RD = 4'd1, F_LOOK = 4'd2, F_FILL = 4'd3,
                 F_RELOOK = 4'd4, F_LDW = 4'd5, F_SUM = 4'd6, F_INV = 4'd7, F_RELOOK2 = 4'd8;
reg [3:0]  fs;
reg [7:0]  io_val;          // I/O read result captured at accept time
reg [1:0]  rd_sel;          // 0 = zero, 1 = work RAM, 2 = I/O
reg [17:0] fill_a;          // SRAM byte address of the burst
reg [3:0]  fill_n;          // byte within line
reg [23:0] fill_w;          // assembled bytes 0..2 of the current word
reg [3:0]  tmr;
reg [17:0] ld_ptr;
reg        ld_pend;
reg [7:0]  ld_byte;
reg        inv_pend;
reg [CACHE_LB-1:0] inv_i;
reg        sum_pend;
reg [17:0] sum_a;


initial begin
  fs = F_IDLE; m_ack = 1'b0; m_rdata = 32'd0; RAM_ADDR = 19'd0; RAM_OE = 1'b1; RAM_WE = 1'b1;
  ram_drive = 1'b0; ram_dout = 8'd0; ld_ptr = 18'd0; ld_pend = 1'b0; inv_pend = 1'b0;
  sum_pend = 1'b0; sum_busy = 1'b0; sum = 32'd0; tmr = 4'd0; dbg_miss_cnt = 32'd0;
end

wire accept = m_req && !m_ack && (fs == F_IDLE) && !ld_pend && !inv_pend && !sum_pend;
wire io_hit00 = m_byte ? (m_addr[5:0] == 6'h00) : (m_addr[5:2] == 4'h0);
wire io_hit10 = m_byte ? (m_addr[5:0] == 6'h10) : (m_addr[5:2] == 4'h4);
wire io_hit20 = m_byte ? (m_addr[5:0] == 6'h20) : (m_addr[5:2] == 4'h8);

always @* begin
  wr_be = 4'b0000;
  if (accept && r_wram && m_we) begin
    if (m_byte) wr_be[m_addr[1:0]] = 1'b1;
    else        wr_be = 4'b1111;
  end
end

// MCU-side strobes are taken on their rising edge: mcu_cmd.v holds some of
// them (e.g. the $E5 verify start) high for a whole SPI byte.
reg ld_we_r = 1'b0, ld_reset_r = 1'b0, sum_start_r = 1'b0;
always @(posedge clk) if (ce) begin ld_we_r <= ld_we; ld_reset_r <= ld_reset; sum_start_r <= sum_start; end
wire ld_req_new = ld_we & ~ld_we_r;
// with IN_REG the strobe is one enabled cycle old when it gets here: busy from that cycle on
assign ld_busy = ld_pend | (fs == F_LDW) | (IN_REG ? ld_req_new : 1'b0);
wire ld_rst_new = ld_reset & ~ld_reset_r;
wire sum_new    = sum_start & ~sum_start_r;
wire snes_rd_3800 = snes_sel && snes_rd_start && (snes_a == 2'b00);
wire snes_rd_3802 = snes_sel && snes_rd_start && (snes_a == 2'b01);

always @(posedge clk) if (ce) begin
  m_ack <= 1'b0;
  cd_we <= 1'b0;
  ct_we <= 1'b0;

  case (fs)
    F_IDLE: begin
      RAM_OE <= 1'b1; RAM_WE <= 1'b1; ram_drive <= 1'b0;
      if (inv_pend) begin
        inv_pend <= 1'b0; inv_i <= {CACHE_LB{1'b0}}; fs <= F_INV;
      end else if (ld_pend) begin
        ld_pend  <= 1'b0;
        RAM_ADDR <= {1'b0, ld_ptr};
        ram_dout <= ld_byte;
        ram_drive <= 1'b1;
        ld_ptr   <= ld_ptr + 18'd1;
        tmr      <= 4'd0;
        fs       <= F_LDW;
      end else if (sum_pend) begin
        sum_pend <= 1'b0; sum <= 32'd0; sum_a <= 18'd0;
        RAM_ADDR <= 19'd0; RAM_OE <= 1'b0; tmr <= 4'd0; fs <= F_SUM;
      end else if (accept) begin
        if (r_rom && !m_we) begin
          fs <= F_LOOK;
        end else begin
          rd_sel <= r_wram ? 2'd1 : ((r_io && !m_code) ? 2'd2 : 2'd0);
          io_val <= 8'd0;
          if (r_io && !m_code) begin
            if (!m_we) begin
              if (io_hit10) begin io_val <= s2a_data; s2a_ready <= 1'b0; end
              else if (io_hit20) io_val <= status;
            end else begin
              if (io_hit00) begin a2s_data <= m_wdata[7:0]; a2s_ready <= 1'b1; a2s_wr_since_rd <= 1'b1; end
              if (io_hit10) begin signal <= 1'b1; sig_set_since_rd <= 1'b1; end
            end
          end
          fs <= F_RD;
        end
      end
    end

    F_RD: begin
      m_ack <= 1'b1;
      case (rd_sel)
        2'd1:    m_rdata <= {wq3, wq2, wq1, wq0};
        2'd2:    m_rdata <= {24'd0, io_val} << {m_byte ? m_addr[1:0] : 2'b00, 3'b000};
        default: m_rdata <= 32'd0;
      endcase
      fs <= F_IDLE;
    end

    F_LOOK: begin
      if (ct_q[TAGW] && ct_q[TAGW-1:0] == c_tag) begin
        m_rdata <= cd_q;
        m_ack   <= 1'b1;
        fs      <= F_IDLE;
      end else begin
        fill_a   <= {sram_a[17:4], 4'd0};
        RAM_ADDR <= {1'b0, sram_a[17:4], 4'd0};
        RAM_OE   <= 1'b0;
        fill_n   <= 4'd0;
        tmr      <= 4'd0;
        dbg_miss_cnt <= dbg_miss_cnt + 32'd1;
        fs       <= F_FILL;
      end
    end

    // Address-controlled read burst: OE stays low, the address advances every
    // SRAM_RD_CYC cycles and the byte is taken from the synchroniser output,
    // i.e. the pin value (SRAM_RD_CYC-2) cycles after the address changed:
    // 6 x 10.4 ns = 62.5 ns at 96 MHz against the SRAM's 45 ns tAA (HALF_RATE:
    // 3 x 20.8 ns with SRAM_RD_CYC = 5).
    F_FILL: begin
      tmr <= tmr + 4'd1;
      if (tmr == SRAM_RD_CYC - 1) begin
        tmr    <= 4'd0;
        fill_n <= fill_n + 4'd1;
        RAM_ADDR <= {1'b0, fill_a[17:4], fill_n + 4'd1};
        case (fill_n[1:0])
          2'd0: fill_w[7:0]   <= ram_s2;
          2'd1: fill_w[15:8]  <= ram_s2;
          2'd2: fill_w[23:16] <= ram_s2;
          default: begin
            cd_we <= 1'b1;
            cd_wa <= {fill_a[CACHE_LB+3:4], fill_n[3:2]};
            cd_wd <= {ram_s2, fill_w};
          end
        endcase
        if (fill_n == 4'd15) begin
          RAM_OE <= 1'b1;
          ct_we  <= 1'b1;
          ct_wa  <= fill_a[CACHE_LB+3:4];
          ct_wd  <= {1'b1, fill_a[17:CACHE_LB+4]};
          fs     <= F_RELOOK;
        end
      end
    end

    // the last data word and the tag are written at the end of F_RELOOK
    // (cd_we/ct_we are registered); F_RELOOK2 re-reads the request address
    F_RELOOK:  fs <= F_RELOOK2;
    F_RELOOK2: fs <= F_LOOK;

    // MCU loader write: address + data set up in F_IDLE, WE low for
    // SRAM_WE_CYC cycles, then two cycles of address/data hold.
    F_LDW: begin
      tmr <= tmr + 4'd1;
      if (tmr == 4'd0) RAM_WE <= 1'b0;
      if (tmr == SRAM_WE_CYC) RAM_WE <= 1'b1;
      if (tmr == SRAM_WE_CYC + 2) begin ram_drive <= 1'b0; fs <= F_IDLE; end
    end

    F_SUM: begin
      tmr <= tmr + 4'd1;
      if (tmr == SRAM_RD_CYC - 1) begin
        tmr   <= 4'd0;
        sum   <= sum + {24'd0, ram_s2};
        sum_a <= sum_a + 18'd1;
        RAM_ADDR <= {1'b0, sum_a + 18'd1};
        if (sum_a == IMG_SIZE - 1) begin
          RAM_OE <= 1'b1; sum_busy <= 1'b0; fs <= F_IDLE;
        end
      end
    end

    F_INV: begin
      ct_we <= 1'b1;
      ct_wa <= inv_i;
      ct_wd <= {(TAGW+1){1'b0}};
      inv_i <= inv_i + 1'b1;
      if (&inv_i) fs <= F_IDLE;
    end

    default: fs <= F_IDLE;
  endcase

  // ---- loader / sweep requests (MCU side); after the FSM so they win ----
  // Only while the MCU holds the ARM in reset: the firmware image is never
  // rewritten or swept under a running game, even if a stray $E5 (RTCSET on
  // other cores) or $E8/$E9 were ever sent to this core.
  if (ld_rst_new && mcu_reset) begin ld_ptr <= 18'd0; inv_pend <= 1'b1; end
  if (ld_req_new && mcu_reset) begin ld_pend <= 1'b1; ld_byte <= ld_data; end
  if (sum_new && mcu_reset) begin sum_pend <= 1'b1; sum_busy <= 1'b1; end

  // a CPU reset abandons the request in flight (a half-filled line is simply
  // left invalid -- its tag is only written once the line is complete)
  if (cpu_rst && (fs == F_RD || fs == F_LOOK || fs == F_FILL || fs == F_RELOOK || fs == F_RELOOK2)) begin
    fs <= F_IDLE; m_ack <= 1'b0; RAM_OE <= 1'b1; cd_we <= 1'b0; ct_we <= 1'b0;
  end

  // ---- host side of the mailbox ----
  if (snes_rd_3800) begin rd_pend_a2s <= 1'b1; a2s_wr_since_rd <= 1'b0; end
  if (snes_rd_3802) begin rd_pend_sig <= 1'b1; sig_set_since_rd <= 1'b0; end
  if (snes_rd_end) begin
    if (rd_pend_a2s && !a2s_wr_since_rd) a2s_ready <= 1'b0;
    if (rd_pend_sig && !sig_set_since_rd) signal <= 1'b0;
    rd_pend_a2s <= 1'b0; rd_pend_sig <= 1'b0;
  end
  if (snes_sel && snes_wr_end) begin
    if (snes_a == 2'b01) begin s2a_data <= snes_di; s2a_ready <= 1'b1; end
    if (snes_a == 2'b10) begin
      arm_hold <= |snes_di;
      if (!arm_hold && |snes_di) begin          // entering reset clears the mailbox
        a2s_ready <= 1'b0; s2a_ready <= 1'b0; signal <= 1'b0;
      end
    end
  end
  if (snes_reset) begin
    arm_hold <= 1'b0; a2s_ready <= 1'b0; s2a_ready <= 1'b0; signal <= 1'b0;
  end
end

endmodule

//////////////////////////////////////////////////////////////////////////////
// st018 -- the core above behind its bus-side wrapper.
//
// HALF_RATE = 0: the core runs on every CLK cycle and all inputs pass straight
// through; this is the original design (mk3).
//
// HALF_RATE = 1 (mk2): the core is enabled on every other CLK cycle. The ARM
// datapath does not make one 96 MHz period on the Spartan-3 (ISE: 14 ns on the
// best of 60 placements); with every register of the core behind the same
// enable each of its internal paths has two periods, which main.ucf states as
// a FROM-TO constraint on the instance. The ARM runs at half speed and the SRAM
// timing is counted in enabled cycles (SRAM_RD_CYC 5, SRAM_WE_CYC 3 keep the
// same 62.5 ns access and write pulse).
// The strobes from the SNES bus and the MCU are one CLK cycle wide, so each is
// latched here, together with the bus values of that cycle, until the core's
// input register has taken it. The host read data is registered here on the
// strobe itself, exactly as before, so a $3800/$3804 read is not delayed.
//////////////////////////////////////////////////////////////////////////////
module st018 #(
  parameter CACHE_LB = 8,
  parameter SRAM_RD_CYC = 8,
  parameter SRAM_WE_CYC = 6,
  parameter HALF_RATE = 0
)(
  input             clk,
  input             mcu_reset,      // MCU hold: 1 while firmware is loaded
  input             snes_reset,     // console reset strobe

  // SNES host side
  input             snes_sel,       // $00-3F/$80-BF:3800-38FF
  input      [2:1]  snes_a,
  input      [7:0]  snes_di,
  output reg [7:0]  snes_do,
  input             snes_rd_start,
  input             snes_rd_end,
  input             snes_wr_end,

  // MCU loader (streamed firmware image) and verification sweep
  input             ld_reset,       // rising edge: pointer = 0, cache invalidate
  input             ld_we,          // rising edge: write ld_data at pointer, pointer++
  input      [7:0]  ld_data,
  output            ld_busy,
  input             sum_start,      // rising edge: checksum the 160 KB image in SRAM
  output            sum_busy,
  output     [31:0] sum,

  // Bus 2 SRAM
  output     [18:0] RAM_ADDR,
  inout      [7:0]  RAM_DATA,
  output            RAM_OE,
  output            RAM_WE,

  // debug / verification
  output            dbg_retire,
  output     [31:0] dbg_pc,
  output     [31:0] dbg_ir,
  output            dbg_rf_we,
  output     [4:0]  dbg_rf_wa,
  output     [31:0] dbg_rf_wd,
  output     [31:0] dbg_cpsr,
  output     [31:0] dbg_miss_cnt
);

wire       ce;
wire       c_snes_reset, c_sel, c_rd_start, c_rd_end, c_wr_end, c_ld_reset, c_ld_we, c_sum_start;
wire [2:1] c_a;
wire [7:0] c_di, c_ld_data;
wire       core_ld_busy, brg_ld_busy, core_sum_busy, brg_sum_busy;
wire [7:0] a2s_data, status;

generate if (HALF_RATE) begin : g_half
  reg ce_r = 1'b0;
  always @(posedge clk) ce_r <= ~ce_r;
  assign ce = ce_r;

  reg       p_reset = 1'b0, p_rd_start = 1'b0, p_rd_end = 1'b0, p_wr_end = 1'b0;
  reg       p_ld_reset = 1'b0, p_ld_we = 1'b0, p_sum = 1'b0;
  reg       f_ld_reset = 1'b0, f_ld_we = 1'b0, f_sum = 1'b0;
  reg       h_sel = 1'b0;
  reg [2:1] h_a = 2'b00;
  reg [7:0] h_di = 8'h00, h_ld = 8'h00;
  always @(posedge clk) begin
    // a strobe stays latched until an enabled cycle has sampled it
    if (snes_reset)    p_reset    <= 1'b1; else if (ce) p_reset    <= 1'b0;
    if (snes_rd_start) p_rd_start <= 1'b1; else if (ce) p_rd_start <= 1'b0;
    if (snes_rd_end)   p_rd_end   <= 1'b1; else if (ce) p_rd_end   <= 1'b0;
    if (snes_wr_end)   p_wr_end   <= 1'b1; else if (ce) p_wr_end   <= 1'b0;
    if (snes_rd_start | snes_rd_end | snes_wr_end) begin
      h_sel <= snes_sel; h_a <= snes_a; h_di <= snes_di;
    end
    // the MCU strobes are edges of levels that may last one cycle or a whole SPI byte
    f_ld_reset <= ld_reset; f_ld_we <= ld_we; f_sum <= sum_start;
    if (ld_reset & ~f_ld_reset) p_ld_reset <= 1'b1; else if (ce) p_ld_reset <= 1'b0;
    if (ld_we & ~f_ld_we) begin p_ld_we <= 1'b1; h_ld <= ld_data; end
    else if (ce) p_ld_we <= 1'b0;
    if (sum_start & ~f_sum) p_sum <= 1'b1; else if (ce) p_sum <= 1'b0;
  end
  assign c_snes_reset = p_reset;   assign c_sel = h_sel; assign c_a = h_a; assign c_di = h_di;
  assign c_rd_start = p_rd_start;  assign c_rd_end = p_rd_end; assign c_wr_end = p_wr_end;
  assign c_ld_reset = p_ld_reset;  assign c_ld_we = p_ld_we;   assign c_ld_data = h_ld;
  assign c_sum_start = p_sum;
  assign brg_ld_busy  = (ld_we & ~f_ld_we) | p_ld_we;
  assign brg_sum_busy = (sum_start & ~f_sum) | p_sum;
end else begin : g_full
  assign ce = 1'b1;
  assign c_snes_reset = snes_reset; assign c_sel = snes_sel; assign c_a = snes_a; assign c_di = snes_di;
  assign c_rd_start = snes_rd_start; assign c_rd_end = snes_rd_end; assign c_wr_end = snes_wr_end;
  assign c_ld_reset = ld_reset;     assign c_ld_we = ld_we;         assign c_ld_data = ld_data;
  assign c_sum_start = sum_start;
  assign brg_ld_busy = 1'b0;
  assign brg_sum_busy = 1'b0;
end endgenerate

assign ld_busy  = core_ld_busy  | brg_ld_busy;
assign sum_busy = core_sum_busy | brg_sum_busy;

st018_core #(
  .CACHE_LB(CACHE_LB), .SRAM_RD_CYC(SRAM_RD_CYC), .SRAM_WE_CYC(SRAM_WE_CYC), .IN_REG(HALF_RATE)
) u_core (
  .clk(clk), .ce(ce),
  .mcu_reset_i(mcu_reset), .snes_reset_i(c_snes_reset),
  .snes_sel_i(c_sel), .snes_a_i(c_a), .snes_di_i(c_di),
  .a2s_data_o(a2s_data), .status_o(status),
  .snes_rd_start_i(c_rd_start), .snes_rd_end_i(c_rd_end), .snes_wr_end_i(c_wr_end),
  .ld_reset_i(c_ld_reset), .ld_we_i(c_ld_we), .ld_data_i(c_ld_data), .ld_busy(core_ld_busy),
  .sum_start_i(c_sum_start), .sum_busy(core_sum_busy), .sum(sum),
  .RAM_ADDR(RAM_ADDR), .RAM_DATA(RAM_DATA), .RAM_OE(RAM_OE), .RAM_WE(RAM_WE),
  .dbg_retire(dbg_retire), .dbg_pc(dbg_pc), .dbg_ir(dbg_ir),
  .dbg_rf_we(dbg_rf_we), .dbg_rf_wa(dbg_rf_wa), .dbg_rf_wd(dbg_rf_wd),
  .dbg_cpsr(dbg_cpsr), .dbg_miss_cnt(dbg_miss_cnt)
);

// host read data: registered on the read strobe from the bus values of that cycle
initial snes_do = 8'h00;
always @(posedge clk) begin
  if (snes_sel && snes_rd_start) begin
    case (snes_a)
      2'b00:   snes_do <= a2s_data;
      2'b10:   snes_do <= status;
      default: snes_do <= 8'h00;
    endcase
  end
end

endmodule
