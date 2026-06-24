`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company:
// Engineer:
//
// Create Date:    02:43:54 02/06/2011
// Design Name:
// Module Name:    bsx
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
module bsx(
  input clkin,
  input reg_oe_falling,
  input reg_oe_rising,
  input reg_we_rising,
  input [23:0] snes_addr_in,
  input [23:0] mapped_addr_in,  // DEBUG (TEMP): now fed CTX_ROM_ADDRr (the RESOLVED CTX write target)
  input        ctx_we_hit_in,   // DEBUG (TEMP): main.v CTX_WE_HIT (the CTX write actually commits)
  input [7:0] reg_data_in,
  output [7:0] reg_data_out,
  input [7:0] reg_reset_bits,
  input [7:0] reg_set_bits,
  output [14:0] regs_out,
  input pgm_we,
  input use_bsx,
  input bs_slot,    // slotted cart: pack only, no MCC.  LoROM slot -> flash $C0-$DF
  input bs_hirom,   // HiROM slot -> window $E0-$EF
  output data_ovr,
  output flash_writable,
  input [59:0] rtc_data_in,
  output [9:0] bs_page_out, // support only page 0000-03ff
  output bs_page_enable,
  output [8:0] bs_page_offset,
  input feat_bs_base_enable,
  // Flash erase request to the MCU (the FPGA can't fill 64KB; the MCU does the
  // sram_memset(0xFF)).  bs_erase_seq increments on each erase; the MCU compares it
  // to its last-seen value and erases bs_erase_blk (0xF = whole pack).
  output [1:0] bs_erase_seq,
  output [3:0] bs_erase_blk,
  // --- BS-X satellite RECEIVER (over-the-air program download) ----------------
  // The Town downloads a program via the $218A/$218B/$218C queue/prefix/data stream
  // (the DCD-BSA receiver the stock FPGA never implemented).  The MCU streams
  // 22-byte-aligned fragments into a ring at 0x980000 and stages a descriptor; the
  // FPGA serves them drain-gated.  Behavior is a register-for-register port of the
  // golden model bsx_receiver_model.py (validated byte-exact).  Until bs_dl_arm is
  // set on the tuned channel the legacy page path is byte-IDENTICAL to before.
  output bs_dl_enable,         // route $218C read to the download ring (0x980000)
  output [16:0] bs_dl_offset,  // byte offset into the ring (<=128KB window)
  output [1:0] bs_dl_seq,      // notify: +1 once per fragment-needed (MCU compares, like bs_erase_seq)
  input bs_dl_arm,             // MCU: 1 = a download is active
  input [9:0] bs_dl_chan,      // the channel (page number) that carries the program
  input bs_dl_stage,           // MCU: rising = a fragment is staged in the ring
  input [16:0] bs_dl_base,     // ring offset of the staged fragment
  input [15:0] bs_dl_frames,   // 22-byte frames in the staged fragment (a 32KB data group = 1490)
  // --- DEBUG probe (read live via opcode 0xf8) — TEMPORARY -----------------------
  output [15:0] bs_dl_dbg_q,   // bs_dl_queue (frames left to advertise)
  output [15:0] bs_dl_dbg_sf,  // bs_dl_staged_frames (loaded frame count)
  output [7:0]  bs_dl_dbg_fl,  // {first,armed,staged,need,pf_latch,dt_latch,2'b0}
  // CTX flash-write target computed HERE from snes_addr (the proven-correct held flash-write address),
  // bypassing address.v's epoch-mixing.  Only for the base-unit (broadcast) pack ($C0-$DF -> 0x400000).
  output [23:0] bs_ctx_target, // = 0x400000 + (BSX_ADDR & 0x0fffff): where the directory byte must land
  output        bs_ctx_use     // this is a base-unit (non-slotted) flash program write -> use bs_ctx_target
);

`define BSX_ENABLE

`ifndef BSX_ENABLE
assign reg_data_out = 0;
assign regs_out = 0;
assign data_ovr = 0;
assign flash_writeable = 0;
assign bs_page_out = 0;
assign bs_page_enable = 0;
assign bs_page_offset = 0;
`else
reg [59:0] rtc_data; always @(posedge clkin) rtc_data <= rtc_data_in;
reg [23:0] snes_addr; always @(posedge clkin) snes_addr <= snes_addr_in;

wire [3:0] reg_addr = snes_addr[19:16]; // 00-0f:5000-5fff
wire [4:0] base_addr = snes_addr[4:0];  // 88-9f -> 08-1f
wire [15:0] flash_addr = snes_addr[15:0];

reg flash_ovr_r;
reg flash_we_r;
reg flash_status_r = 0;
reg [7:0] flash_cmd0;

// MCC registers (00-0f:5xxx) exist only on the BS-X base cart, never on a
// slotted cart -> gate them off in slot mode so the game's low banks are free.
wire cart_enable = (use_bsx) && ~bs_slot && ((snes_addr[23:12] & 12'hf0f) == 12'h005);

wire base_enable = feat_bs_base_enable
                   & (use_bsx) && (!snes_addr[22] && (snes_addr[15:0] >= 16'h2188)
                                 && (snes_addr[15:0] <= 16'h219f));

// flash window: base-unit AND LoROM slot $C0-$DF, HiROM slot $E0-$EF.
// NB: the base case is the FULL $C0-$DF bank (was just bank $C0).  The Town programs/erases the
// 512KB LoROM body across banks $C0..$Cn (HiROM-linear MCC mode), so flash_writable + the command
// handler must cover the whole window or the upper banks' program bytes are never written.
wire flash_enable = bs_hirom
                    ? (snes_addr[23:20] == 4'he)
                    : (snes_addr[23:21] == 3'b110);

// command/vendor mode returns the register; array mode ($FF) reads PSRAM
wire flash_ovr = (use_bsx) && (flash_enable & flash_ovr_r);

// program data phase; main.v does the write (skips $FF) via the copier path
assign flash_writable = (use_bsx)
                        && flash_enable
                        && flash_we_r;

// CTX target computed from snes_addr (the held flash-write address; sf-capture proved this register
// holds $C0:7FB0 at the write).  Base-unit pack = 0x400000 + (BSX_ADDR & 0x0fffff); the broadcast runs
// HiROM-linear (bsx_regs[2]=1 via set_bsx_regs 0xf6) so the offset is snes_addr[19:0] -> $C0:7FB0 =>
// 0x407FB0, $C4:xxxx => 0x44xxxx, etc.  bs_ctx_use selects this over address.v for non-slotted writes.
assign bs_ctx_target = {4'h4, snes_addr[19:0]};
assign bs_ctx_use    = flash_writable & ~bs_slot;

assign data_ovr = (cart_enable | base_enable | flash_ovr) & ~bs_page_enable & ~bs_dl_enable;

// --- Flash block-erase tracking -------------------------------------------
// block-erase = $20 (setup) then $D0 (confirm) at the block address.
// 64KB blocks: HiROM bank $E0+blk, LoROM 2 banks/block ($C0+2*blk).
reg erase_setup_r = 0;      // saw $20 (block erase setup)
reg erase_all_setup_r = 0;  // saw $A7 (chip/all erase setup)
// a delete can fire several erases, so a 1-bit toggle would alias -> 2-bit seq the
// MCU compares (any change = erase).  bs_erase_blk = last block, 0xF = whole pack.
reg [1:0] bs_erase_seq_r = 0;
reg [3:0] bs_erase_blk_r = 0;
assign bs_erase_seq = bs_erase_seq_r;
assign bs_erase_blk = bs_erase_blk_r;
wire [3:0] erase_blk_of_addr = bs_hirom ? snes_addr[19:16] : snes_addr[20:17];
// busy timer held after a $D0 so the game waits while the MCU erases the block (~0.4s)
reg [27:0] erase_busy_cnt = 0;  // widened 25->28 bits (~0.35s -> ~2.8s): hold the Town in status-poll
                                // long enough for the ASYNC MCU sram_memset erase to finish BEFORE the
                                // Town programs, else the late erase wipes the just-written directory.
wire erase_busy = (erase_busy_cnt != 0);


reg [9:0] bs_page0;
reg [9:0] bs_page1;

reg [8:0] bs_page0_offset;
reg [8:0] bs_page1_offset;
reg [4:0] bs_stb0_offset;
reg [4:0] bs_stb1_offset;

wire bs_sta0_en = base_addr == 5'h0a;
wire bs_stb0_en = base_addr == 5'h0b;
wire bs_page0_en = base_addr == 5'h0c;

wire bs_sta1_en = base_addr == 5'h10;
wire bs_stb1_en = base_addr == 5'h11;
wire bs_page1_en = base_addr == 5'h12;

// === BS-X satellite RECEIVER (stream 0) — port of bsx_receiver_model.py =======
// $218A=0x0a Queue, $218B=0x0b Prefix, $218C=0x0c Data.  Armed only while the Town
// is tuned to the program channel; otherwise the legacy page path is byte-identical.
reg [16:0] bs_dl_addr = 0;          // running ring offset (= base + bytes read)
reg [15:0] bs_dl_queue = 0;         // 22-byte frames left to ADVERTISE (a 32KB data group = 1490 > 8 bits)
reg [7:0]  bs_pf_queue = 0;         // advertised status frames not yet read (cap 0x7F)
reg [7:0]  bs_dt_queue = 0;         // advertised data frames not yet read (cap 0x7F)
reg [4:0]  bs_data_cnt = 0;         // 0..21 byte counter within the current frame
reg        bs_dl_first = 1'b1;      // next $218B carries the 0x10 Packet-Start
reg        bs_pf_latch = 0;
reg        bs_dt_latch = 0;
reg        bs_dl_need = 0;          // one-shot gate for the notify
reg [1:0]  bs_dl_seq_r = 0;
reg        bs_dl_staged = 0;        // a fragment descriptor is waiting to be loaded
reg [16:0] bs_dl_staged_base = 0;
reg [15:0] bs_dl_staged_frames = 0;
reg        bs_dl_stage_s = 0;       // edge detect of bs_dl_stage
// DEBUG (TEMPORARY) — capture the Town's SAVE writes during a download, FILTERED to the
// pack write windows (flash $C0-$EF OR PSRAM $x6000-$7FFF in banks $80-$FF) to exclude the
// city's normal writes.  Answers WHERE the save lands (which did NOT reach PSRAM 0x400000).
reg        bs_dbg90_seen = 0;       // reused: "a pack-window write was captured this session"
reg [23:0] bs_cap_min    = 24'hffffff; // min pack-window write address
reg [23:0] bs_cap_max    = 0;       // max pack-window write address
reg [15:0] bs_cap_cnt    = 0;       // count of pack-window writes (saturating)
reg        bs_cap_flash  = 0;       // any write hit the $C0-$EF flash window
reg        bs_cap_6xxx   = 0;       // any write hit the $80-$FF:6000-7FFF PSRAM window
reg        bs_cap_cmdran = 0;       // the flash command handler (gated by regs_outr[12]) ran
reg        bs_cap_fwr    = 0;       // flash_writable asserted (a program byte went to the CTX path)
reg        bs_cap_regsC  = 0;       // latched regs_outr[12] (flash-write-enable) — avoids forward ref
reg        bs_dl_arm_s   = 0;
assign bs_dl_seq = bs_dl_seq_r;
assign bs_dl_offset = bs_dl_addr;
// DEBUG probe (TEMPORARY) — repurposed: the captured save-write address + path
assign bs_dl_dbg_q  = bs_cap_min[23:8];                           // RESOLVED CTX write target [23:8] (where CTX commits)
assign bs_dl_dbg_sf = bs_cap_max[23:8];                           // FIRST SNES flash-write addr [23:8] ($C0:7Fxx?)
assign bs_dl_dbg_fl = {bs_dbg90_seen, bs_cap_flash, bs_cap_6xxx,  // seen, flash-window, $x6xxx
                       bs_cap_regsC, bs_cap_cmdran, bs_cap_fwr,   // FLASH-ENABLE, cmd-ran, prog-wr
                       bs_cap_cnt[9:8]};                          // count magnitude (low)

wire bs_dl_armed = bs_dl_arm & (bs_page0 == bs_dl_chan);
// $218C ($218 base_addr 0x0c) read serves the ring at 0x980000 instead of 0x900000
assign bs_dl_enable = base_enable & bs_dl_armed & bs_page0_en;

// delta terms (combine the every-clock pacer with per-frame read drains)
wire bs_pacer_inc = bs_dl_armed & (bs_dl_queue != 8'h0) & (bs_pf_queue < 8'h7f)
                                & bs_pf_latch & bs_dt_latch;
wire bs_pf_dec = reg_oe_rising & bs_dl_armed & bs_stb0_en  & (bs_pf_queue != 8'h0); // $218B read
wire bs_dt_dec = reg_oe_rising & bs_dl_armed & bs_page0_en & (bs_data_cnt == 5'd21)
                                                          & (bs_dt_queue != 8'h0);  // $218C 22nd byte
// values returned to the SNES on a read (reg_oe_falling mux)
wire [7:0] bs_queue_val  = bs_pf_queue;                                  // $218A (cap 0x7F -> bit7 clear)
wire [7:0] bs_prefix_val = bs_pf_latch                                   // 0 until $218B latched (model read_prefix)
                         ? ((bs_dl_first ? 8'h10 : 8'h00)                // Packet-Start
                          | ((bs_dl_queue == 8'h0 && bs_pf_queue == 8'h1) ? 8'h80 : 8'h00)) // Packet-End
                         : 8'h00;

assign bs_page_enable = base_enable & ((|bs_page0 & ~bs_dl_armed & (bs_page0_en | bs_sta0_en | bs_stb0_en))
                                      |(|bs_page1 & (bs_page1_en | bs_sta1_en | bs_stb1_en)));

assign bs_page_out = (bs_page0_en | bs_sta0_en | bs_stb0_en) ? bs_page0 : bs_page1;

assign bs_page_offset = bs_sta0_en ? 9'h032
                      : bs_stb0_en ? (9'h034 + bs_stb0_offset)
                      : bs_sta1_en ? 9'h032
                      : bs_stb1_en ? (9'h034 + bs_stb1_offset)
                      : (9'h048 + (bs_page0_en ? bs_page0_offset : bs_page1_offset));

reg [1:0] pgm_we_sreg;
always @(posedge clkin) pgm_we_sreg <= {pgm_we_sreg[0], pgm_we};
wire pgm_we_rising = (pgm_we_sreg[1:0] == 2'b01);

reg [14:0] regs_tmpr;
reg [14:0] regs_outr;
reg [7:0] reg_data_outr;

reg [7:0] base_regs[31:8];
reg [4:0] bsx_counter;
reg [7:0] flash_vendor_data[7:0];

assign regs_out = regs_outr;
assign reg_data_out = reg_data_outr;

reg [7:0] rtc_sec, rtc_sec_pre0, rtc_sec_pre1;

reg [7:0] rtc_min, rtc_min_pre0, rtc_min_pre1;
reg [7:0] rtc_hour, rtc_hour_pre0, rtc_hour_pre1;
reg [7:0] rtc_day, rtc_day_pre0, rtc_day_pre1;
reg [7:0] rtc_month, rtc_month_pre0, rtc_month_pre1;
reg [7:0] rtc_dow;
reg [7:0] rtc_year1, rtc_year1_pre0, rtc_year1_pre1;
reg [7:0] rtc_year100, rtc_year100_pre0, rtc_year100_pre1;
reg [15:0] rtc_year, rtc_year_pre0, rtc_year_pre1, rtc_year_pre2;

// wire [7:0] rtc_sec_pre = rtc_data[3:0] + (rtc_data[7:4] << 3) + (rtc_data[7:4] << 1);
// wire [7:0] rtc_min_pre = rtc_data[11:8] + (rtc_data[15:12] << 3) + (rtc_data[15:12] << 1);
// wire [7:0] rtc_hour_pre = rtc_data[19:16] + (rtc_data[23:20] << 3) + (rtc_data[23:20] << 1);
// wire [7:0] rtc_day_pre = rtc_data[27:24] + (rtc_data[31:28] << 3) + (rtc_data[31:28] << 1);
// wire [7:0] rtc_month_pre = rtc_data[35:32] + (rtc_data[39:36] << 3) + (rtc_data[39:36] << 1);
// wire [7:0] rtc_dow_pre = {4'b0,rtc_data[59:56]};
// wire [7:0] rtc_year1_pre = rtc_data[43:40] + (rtc_data[47:44] << 3) + (rtc_data[47:44] << 1);
// wire [7:0] rtc_year100_pre = rtc_data[51:48] + (rtc_data[55:52] << 3) + (rtc_data[55:52] << 1);
// wire [15:0] rtc_year_pre = (rtc_year100 << 6) + (rtc_year100 << 5) + (rtc_year100 << 2) + rtc_year1;

always @(posedge clkin) begin
  rtc_sec_pre1 <= rtc_data[3:0];
  rtc_sec_pre0 <= rtc_sec_pre1 + (rtc_data[7:4] << 3);
  rtc_sec <= rtc_sec_pre0 + (rtc_data[7:4] << 1);

  rtc_min_pre1 <= rtc_data[11:8];
  rtc_min_pre0 <= rtc_min_pre1 + (rtc_data[15:12] << 3);
  rtc_min <= rtc_min_pre0 + (rtc_data[15:12] << 1);

  rtc_hour_pre1 <= rtc_data[19:16];
  rtc_hour_pre0 <= rtc_hour_pre1 + (rtc_data[23:20] << 3);
  rtc_hour <= rtc_hour_pre0 + (rtc_data[23:20] << 1);
  
  rtc_day_pre1 <= rtc_data[27:24];
  rtc_day_pre0 <= rtc_day_pre1 + (rtc_data[31:28] << 3);
  rtc_day <= rtc_day_pre0 + (rtc_data[31:28] << 1);
  
  rtc_month_pre1 <= rtc_data[35:32];
  rtc_month_pre0 <= rtc_month_pre1 + (rtc_data[39:36] << 3);
  rtc_month <= rtc_month_pre0 + (rtc_data[39:36] << 1);
  
  rtc_dow <= {4'b0, rtc_data[59:56]};
  
  rtc_year1_pre1 <= rtc_data[43:40];
  rtc_year1_pre0 <= rtc_year1_pre1 + (rtc_data[47:44] << 3);
  rtc_year1 <= rtc_year1_pre0 + (rtc_data[47:44] << 1);
  
  rtc_year100_pre1 <= rtc_data[51:48];
  rtc_year100_pre0 <= rtc_year100_pre1 + (rtc_data[55:52] << 3);
  rtc_year100 <= rtc_year100_pre0 + (rtc_data[55:52] << 1);
  
  rtc_year_pre2 <= (rtc_year100 << 6);
  rtc_year_pre1 <= rtc_year_pre2 + (rtc_year100 << 5);
  rtc_year_pre0 <= rtc_year_pre1 + (rtc_year100 << 2);
  rtc_year <= rtc_year_pre0 + rtc_year1;
end

initial begin
  regs_tmpr <= 15'b000101111101100;
  regs_outr <= 15'b000101111101100;
  bsx_counter <= 0;
  base_regs[5'h08] <= 0;
  base_regs[5'h09] <= 0;
  base_regs[5'h0a] <= 8'h01;
  base_regs[5'h0b] <= 0;
  base_regs[5'h0c] <= 0;
  base_regs[5'h0d] <= 0;
  base_regs[5'h0e] <= 0;
  base_regs[5'h0f] <= 0;
  base_regs[5'h10] <= 8'h01;
  base_regs[5'h11] <= 0;
  base_regs[5'h12] <= 0;
  base_regs[5'h13] <= 0;
  base_regs[5'h14] <= 0;
  base_regs[5'h15] <= 0;
  base_regs[5'h16] <= 0;
  base_regs[5'h17] <= 0;
  base_regs[5'h18] <= 0;
  base_regs[5'h19] <= 0;
  base_regs[5'h1a] <= 0;
  base_regs[5'h1b] <= 0;
  base_regs[5'h1c] <= 0;
  base_regs[5'h1d] <= 0;
  base_regs[5'h1e] <= 0;
  base_regs[5'h1f] <= 0;
  flash_vendor_data[3'h0] <= 8'h4d;
  flash_vendor_data[3'h1] <= 8'h00;
  flash_vendor_data[3'h2] <= 8'h50;
  flash_vendor_data[3'h3] <= 8'h00;
  flash_vendor_data[3'h4] <= 8'h00;
  flash_vendor_data[3'h5] <= 8'h00;
  flash_vendor_data[3'h6] <= 8'h1a;
  flash_vendor_data[3'h7] <= 8'h00;
  flash_ovr_r <= 1'b0;
  flash_we_r <= 1'b0;
  bs_page0 <= 10'h0;
  bs_page1 <= 10'h0;
  bs_page0_offset <= 9'h0;
  bs_page1_offset <= 9'h0;
  bs_stb0_offset <= 5'h00;
  bs_stb1_offset <= 5'h00;
end

always @(posedge clkin) begin
  if(erase_busy_cnt != 0) erase_busy_cnt <= erase_busy_cnt - 1'b1;  // WSM busy timer (a $D0 reloads it below)
  if(reg_oe_rising && base_enable) begin
    case(base_addr)
      5'h0b: begin
        bs_stb0_offset <= bs_stb0_offset + 1;
        // Accumulate the prefix flags into the $218D status (golden model read_prefix:
        // status |= prefix).  While ARMED the receiver returns bs_prefix_val on $218B, so
        // OR THAT (not reg_data_in) into $218D — the Town reads $218D for the Packet-End
        // (0x80); without this it stays 0x00 and reception fails with Error 21.  ($218D
        // read + clear already exists below for base_addr 0x0d.)
        base_regs[5'h0d] <= base_regs[5'h0d] | (bs_dl_armed ? bs_prefix_val : reg_data_in);
      end
      5'h0c: bs_page0_offset <= bs_page0_offset + 1;
      5'h11: begin
        bs_stb1_offset <= bs_stb1_offset + 1;
        base_regs[5'h13] <= base_regs[5'h13] | reg_data_in;
      end
      5'h12: bs_page1_offset <= bs_page1_offset + 1;
    endcase
  end else if(reg_oe_falling) begin
    if(cart_enable)
      reg_data_outr <= {regs_outr[reg_addr], 7'b0};
    else if(base_enable) begin
      if (bs_dl_armed && bs_sta0_en) reg_data_outr <= bs_queue_val;        // $218A Queue
      else if (bs_dl_armed && bs_stb0_en) reg_data_outr <= bs_prefix_val;  // $218B Prefix
      else case(base_addr)                                                 // ($218C data = ring via bs_dl_enable)
        5'h0c, 5'h12: begin
          case (bs_page1_offset)
            4: reg_data_outr <= 8'h3;
            5: reg_data_outr <= 8'h1;
            6: reg_data_outr <= 8'h1;
            10: reg_data_outr <= rtc_sec;
            11: reg_data_outr <= rtc_min;
            12: reg_data_outr <= rtc_hour;
            13: reg_data_outr <= rtc_dow;
            14: reg_data_outr <= rtc_day;
            15: reg_data_outr <= rtc_month;
            16: reg_data_outr <= rtc_year[7:0];
            17: reg_data_outr <= rtc_hour;
            default: reg_data_outr <= 8'h0;
          endcase
        end
        5'h0d, 5'h13: begin
          reg_data_outr <= base_regs[base_addr];
          base_regs[base_addr] <= 8'h00;
        end
        default:
          reg_data_outr <= base_regs[base_addr];
      endcase
    end else if (flash_enable) begin
      // CSR ready = $80, busy = $00 (bit7); the game polls this after a $D0 and waits
      // while erase_busy so the MCU can fill the block before the next erase.
      casex (flash_addr)
        16'b1111111100000xxx:
          reg_data_outr <= flash_status_r ? (erase_busy ? 8'h00 : 8'h80) : flash_vendor_data[flash_addr&16'h0007];
        16'b1111111100001xxx,
        16'b11111111000100xx:
          reg_data_outr <= flash_status_r ? (erase_busy ? 8'h00 : 8'h80) : 8'h00;
        default:
          reg_data_outr <= (erase_busy ? 8'h00 : 8'h80);
      endcase
    end
  end else if(pgm_we_rising) begin
    regs_tmpr[8:1] <= (regs_tmpr[8:1] | reg_set_bits[7:0]) & ~reg_reset_bits[7:0];
    regs_outr[8:1] <= (regs_outr[8:1] | reg_set_bits[7:0]) & ~reg_reset_bits[7:0];
  end else if(reg_we_rising && cart_enable) begin
    if(reg_addr == 4'he)
      regs_outr <= regs_tmpr;
    else begin
      regs_tmpr[reg_addr] <= reg_data_in[7];
      if(reg_addr == 4'h1) regs_outr[reg_addr] <= reg_data_in[7];
    end
  end else if(reg_we_rising && base_enable) begin
    case(base_addr)
      5'h09: begin
        base_regs[8'h09] <= reg_data_in;
        bs_page0 <= {reg_data_in[1:0], base_regs[8'h08]};
        bs_page0_offset <= 9'h00;
      end
      5'h0b: begin
        bs_stb0_offset <= 5'h00;
      end
      5'h0c: begin
        bs_page0_offset <= 9'h00;
      end
      5'h0f: begin
        base_regs[8'h0f] <= reg_data_in;
        bs_page1 <= {reg_data_in[1:0], base_regs[8'h0e]};
        bs_page1_offset <= 9'h00;
      end
      5'h11: begin
        bs_stb1_offset <= 5'h00;
      end
      5'h12: begin
        bs_page1_offset <= 9'h00;
      end
      default:
        base_regs[base_addr] <= reg_data_in;
    endcase
  end else if(reg_we_rising && flash_enable && (regs_outr[4'hc] | bs_slot)) begin
    if(flash_we_r) begin
      flash_we_r <= 0;  // this write is the program byte, not a command
    end else begin
      // program/erase are issued at the target/block address (tracked here regardless
      // of address); program only arms the write byte and leaves the read mode alone.
      case(reg_data_in)
        8'h10, 8'h40: begin flash_we_r <= 1'b1; erase_setup_r <= 1'b0; erase_all_setup_r <= 1'b0; end
        8'h20: begin erase_setup_r <= 1'b1; erase_all_setup_r <= 1'b0; end
        8'ha7: begin erase_all_setup_r <= 1'b1; erase_setup_r <= 1'b0; end
        8'hd0: begin
          // block = the $D0 (confirm) address, not $20 (setup): $20 is at the command
          // port, $D0 at the block address ($C4:8000 -> block 2).
          if(erase_setup_r) begin
            bs_erase_seq_r <= bs_erase_seq_r + 1'b1; bs_erase_blk_r <= erase_blk_of_addr;
            erase_busy_cnt <= {28{1'b1}};
            flash_ovr_r <= 1'b1; flash_status_r <= 1'b1;
          end else if(erase_all_setup_r) begin
            bs_erase_seq_r <= bs_erase_seq_r + 1'b1; bs_erase_blk_r <= 4'hf;
            erase_busy_cnt <= {28{1'b1}};
            flash_ovr_r <= 1'b1; flash_status_r <= 1'b1;
          end
          erase_setup_r <= 1'b0; erase_all_setup_r <= 1'b0;
        end
        default: begin erase_setup_r <= 1'b0; erase_all_setup_r <= 1'b0; end
      endcase
      // mode commands ($FF/$70/$72/$75) only at the command port, so a stray pack
      // write can't flip the read mode (HiROM $E0:0000 / LoROM $C0:0000)
      if(bs_hirom ? ((snes_addr[23:16] == 8'he0) && (flash_addr[14:0] == 15'h0000))
         : bs_slot ? ((snes_addr[23:16] == 8'hc0) && (flash_addr[14:0] == 15'h0000))
         : (flash_addr == 16'h0000)) begin
        flash_cmd0 <= reg_data_in;
        if(flash_cmd0 == 8'h72 && reg_data_in == 8'h75) begin
          flash_ovr_r <= 1'b1; flash_status_r <= 1'b0;
        end else if(reg_data_in == 8'hff) begin
          flash_ovr_r <= 1'b0;
        end else if(reg_data_in[7:1] == 7'b0111000 || (flash_cmd0 == 8'h38 && reg_data_in == 8'hd0)) begin
          flash_ovr_r <= 1'b1; flash_status_r <= 1'b1;
        end
      end
    end
  end
end

// === BS-X RECEIVER FSM — drives only bs_dl_* regs (legacy path untouched) ======
// Register-for-register port of bsx_receiver_model.py.  The "fully consumed" test
// REQUIRES bs_dl_queue==0 (not just the pf/dt queues) so a $218A read landing right
// after a load — before the pacer advertises a frame — does not skip the fragment.
always @(posedge clkin) begin
  bs_dl_stage_s <= bs_dl_stage;
  if (bs_dl_stage & ~bs_dl_stage_s) begin   // MCU staged a fragment in the ring
    bs_dl_staged        <= 1'b1;
    bs_dl_staged_base   <= bs_dl_base;
    bs_dl_staged_frames <= bs_dl_frames;
  end

  if (reg_we_rising && base_enable && (base_addr == 5'h09)) begin
    // channel (re)tune -> reset the stream sequence (mirrors the bs_page0 reset)
    bs_dl_queue <= 8'h0; bs_pf_queue <= 8'h0; bs_dt_queue <= 8'h0;
    bs_data_cnt <= 5'h0; bs_dl_first <= 1'b1; bs_dl_need <= 1'b0;
    bs_dl_staged <= 1'b0;   // discard a fragment staged for the OLD channel (model _reset_fragment_seq)
  end else if (reg_we_rising && base_enable && (base_addr == 5'h0b)) begin
    bs_pf_latch <= (reg_data_in != 8'h0); bs_pf_queue <= 8'h0;   // $218B latch enable / ack
  end else if (reg_we_rising && base_enable && (base_addr == 5'h0c)) begin
    bs_dt_latch <= (reg_data_in != 8'h0); bs_dt_queue <= 8'h0;   // $218C latch enable / ack
  end else if (reg_oe_rising && bs_dl_armed && bs_sta0_en) begin
    // $218A read: advance to the next fragment ONLY when fully consumed
    if (bs_dl_queue == 8'h0 && bs_pf_queue == 8'h0 && bs_dt_queue == 8'h0) begin
      bs_data_cnt <= 5'h0;
      if (bs_dl_staged) begin
        bs_dl_addr   <= bs_dl_staged_base;
        bs_dl_queue  <= bs_dl_staged_frames;
        bs_dl_first  <= 1'b1;
        bs_dl_need   <= 1'b0;
        bs_dl_staged <= 1'b0;
      end else if (!bs_dl_need) begin            // edge: notify the MCU exactly once
        bs_dl_need  <= 1'b1;
        bs_dl_seq_r <= bs_dl_seq_r + 1'b1;
      end
    end
  end else begin
    // every-clock pacer + per-frame read drains, delta-combined (race-safe)
    bs_pf_queue <= bs_pf_queue + bs_pacer_inc - bs_pf_dec;
    bs_dt_queue <= bs_dt_queue + bs_pacer_inc - bs_dt_dec;
    bs_dl_queue <= bs_dl_queue - bs_pacer_inc;
    if (bs_pf_dec) bs_dl_first <= 1'b0;           // $218B consumed -> Packet-Start spent
    if (reg_oe_rising && bs_dl_armed && bs_page0_en && (bs_dt_queue != 8'h0)) begin // $218C data byte read
      // advance ONLY when a data frame is advertised (model read_data: `if dt_queue:`);
      // a stray/early $218C with dt_queue==0 must not move the ring ptr or the %22 phase.
      bs_dl_addr  <= bs_dl_addr + 1'b1;
      bs_data_cnt <= (bs_data_cnt == 5'd21) ? 5'd0 : (bs_data_cnt + 1'b1);
    end
  end
end

// DEBUG (TEMPORARY) — capture the Town's SAVE writes, FILTERED to the pack write windows:
// the $C0-$EF flash window (flash_enable) OR a $x6000-$7FFF write in banks $80-$FF.  Tracks
// min/max address + count + which window(s) hit.  Cleared when a new download arms.
wire bs_wr_pack = reg_we_rising & (flash_enable
                                 | (snes_addr[23] & (snes_addr[15:13] == 3'b011)));
always @(posedge clkin) begin
  bs_dl_arm_s <= bs_dl_arm;
  if (bs_dl_arm & ~bs_dl_arm_s) begin           // new download armed -> clear capture
    bs_dbg90_seen <= 1'b0;
    bs_cap_min    <= 24'hffffff; // reused: MIN SNES write addr while flash_writable (across the save)
    bs_cap_max    <= 24'h0;      // reused: MAX SNES write addr while flash_writable (across the save)
    bs_cap_cnt    <= 16'h0;
    bs_cap_flash  <= 1'b0;
    bs_cap_6xxx   <= 1'b0;
    bs_cap_cmdran <= 1'b0;
    bs_cap_fwr    <= 1'b0;
    bs_cap_regsC  <= 1'b0;
  end else begin
    if (bs_wr_pack) begin
      if (bs_cap_cnt != 16'hffff)      bs_cap_cnt <= bs_cap_cnt + 16'h1;
      if (flash_enable)                bs_cap_flash <= 1'b1;
    end
    if (bs_slot)                       bs_cap_6xxx  <= 1'b1;  // MEASURE: is this pack treated as SLOTTED? (fl bit5)
    // did the flash command handler run (gate regs_outr[12]|bs_slot)? did a program byte write?
    if (reg_we_rising & flash_enable & (regs_outr[4'hc] | bs_slot)) bs_cap_cmdran <= 1'b1;
    if (reg_we_rising & flash_writable & bs_ctx_use) bs_cap_regsC <= 1'b1; // MEASURE: bs_ctx_use during flash write (fl bit4)
    if (reg_we_rising & flash_writable) begin   // a program byte (flash) write from the SNES
      bs_cap_fwr <= 1'b1;
      if (~bs_cap_fwr) bs_cap_max <= snes_addr; // the FIRST SNES flash-write address ($C0:7FB0?)
    end
    if (ctx_we_hit_in & ~bs_dbg90_seen) begin   // the FIRST time the CTX write ACTUALLY commits to SDRAM
      bs_dbg90_seen <= 1'b1;                     // (repurposed) "a CTX write committed this save"
      bs_cap_min    <= mapped_addr_in;           // = CTX_ROM_ADDRr: where the CTX write RESOLVED to
    end
  end
end
`endif

endmodule