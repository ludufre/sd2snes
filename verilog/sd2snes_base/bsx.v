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
  input [23:0] mapped_addr_in,  // debug: CTX write resolved target
  input        ctx_we_hit_in,   // debug: CTX write commits
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
  // flash erase request to the MCU (it does the memset). bs_erase_seq bumps per
  // erase; bs_erase_blk = block, 0xF = whole pack.
  output [1:0] bs_erase_seq,
  output [3:0] bs_erase_blk,
  input bs_erase_act,  // pack-memset in flight; fall = erase done
  // BS-X satellite receiver (over-the-air program download).
  // Program arrives via the $218A/$218B/$218C queue/prefix/data stream; MCU stages
  // 22-byte-aligned fragments in a ring at 0x980000, FPGA serves them drain-gated.
  // Legacy page path is byte-identical until bs_dl_arm on the tuned channel.
  output bs_dl_armed_out,      // gates main.v prefetcher + address.v armed-stream decode
  // armed $218A/$218D must be frozen for the whole read window: $218A is a live
  // counter (pacer ripples it) and $218D self-clears on read. Latch both at
  // reg_oe_falling and serve latched.
  output [7:0] bs_dl_pard_data, // latched value for armed $218A/$218D reads
  output       bs_dl_pard_hit,  // armed & base decode hits one of those two
  output bs_dl_enable,         // route $218C read to the download ring (0x980000)
  output [16:0] bs_dl_offset,  // byte offset into the ring (<=128KB window)
  output [16:0] bs_dl_daddr,   // raw ring data pointer (for address.v's EARLY decode)
  output [10:0] bs_dl_pidx,    // raw prefix-table index (idem)
  input  [7:0] bs_dl_pfx_srv,  // prefix byte served from the ring table on armed $218B, accumulated into $218D
  output bs_dl_reload,         // toggles on any (re)load of the serve pointers -> main.v invalidates its prefetch
  output [1:0] bs_dl_seq,      // notify: +1 once per fragment-needed (MCU compares, like bs_erase_seq)
  input bs_dl_arm,             // MCU: 1 = a download is active
  input [9:0] bs_dl_chan,      // the channel (page number) that carries the program
  input bs_dl_stage,           // MCU: rising = a fragment is staged in the ring
  input [16:0] bs_dl_base,     // ring offset of the staged fragment
  input [15:0] bs_dl_frames,   // 22-byte frames in the staged fragment (a 32KB data group = 1490)
  // debug probe (read via opcode 0xf8)
  output [15:0] bs_dl_dbg_q,   // bs_dl_queue (frames left to advertise)
  output [15:0] bs_dl_dbg_sf,  // bs_dl_staged_frames (loaded frame count)
  output [7:0]  bs_dl_dbg_fl,  // {first,armed,staged,need,pf_latch,dt_latch,2'b0}
  // CTX flash-write target from snes_addr; base-unit pack only ($C0-$DF -> 0x400000)
  output [23:0] bs_ctx_target, // = 0x400000 + (BSX_ADDR & 0x0fffff)
  output        bs_ctx_use     // base-unit (non-slotted) flash write -> use bs_ctx_target
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
assign bs_dl_reload = 0;
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
// base case is the FULL $C0-$DF bank: the Town programs the 512KB LoROM body across
// $C0..$Cn (HiROM-linear MCC), so the window must cover all of it.
wire flash_enable = bs_hirom
                    ? (snes_addr[23:20] == 4'he)
                    : (snes_addr[23:21] == 3'b110);

// command/vendor mode returns the register; array mode ($FF) reads PSRAM
wire flash_ovr = (use_bsx) && (flash_enable & flash_ovr_r);

// program data phase; main.v does the write (skips $FF) via the copier path
assign flash_writable = (use_bsx)
                        && flash_enable
                        && flash_we_r;

// CTX target from snes_addr. Base-unit pack = 0x400000 + snes_addr[19:0] (broadcast runs
// HiROM-linear): $C0:7FB0 => 0x407FB0, $C4:xxxx => 0x44xxxx. bs_ctx_use picks this over
// address.v for non-slotted writes.
assign bs_ctx_target = {4'h4, snes_addr[19:0]};
assign bs_ctx_use    = flash_writable & ~bs_slot;

assign data_ovr = (cart_enable | base_enable | flash_ovr) & ~bs_page_enable & ~bs_dl_enable;

// flash block-erase tracking
// block-erase = $20 (setup) then $D0 (confirm) at the block address.
// 64KB blocks: HiROM bank $E0+blk, LoROM 2 banks/block ($C0+2*blk).
reg erase_setup_r = 0;      // saw $20 (block erase setup)
reg erase_all_setup_r = 0;  // saw $A7 (chip/all erase setup)
// a delete fires several erases -> 2-bit seq (1-bit would alias); MCU compares.
// bs_erase_blk = last block, 0xF = whole pack.
reg [1:0] bs_erase_seq_r = 0;
reg [3:0] bs_erase_blk_r = 0;
assign bs_erase_seq = bs_erase_seq_r;
assign bs_erase_blk = bs_erase_blk_r;
// erase block index must match the program/read geometry or $D0 memsets the wrong block.
// base-unit runs HiROM-linear: 1 bank = 1 block, block = snes_addr[19:16] (bs_hirom is
// FALSE for the base unit). Only the LoROM .mpk slot (2 banks/block) uses [20:17].
wire [3:0] erase_blk_of_addr = (bs_hirom | ~bs_slot) ? snes_addr[19:16] : snes_addr[20:17];
// busy timer held after a $D0 so the game waits while the erase runs.
// base-unit: bs_erase_act (rise->fall) ends the busy window; the 28-bit reload (~2.8s)
// is only a fail-safe ceiling.
// slotted .mpk: MCU memset has no done-signal, so keep the timer at 25-bit width (~0.35s).
reg [27:0] erase_busy_cnt = 0;
wire erase_busy = (erase_busy_cnt != 0);
reg bs_erase_act_seen = 0;      // bs_erase_act rose since the $D0 (memset actually started)


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

// BS-X satellite receiver (stream 0).
// $218A=0x0a queue, $218B=0x0b prefix, $218C=0x0c data. Armed only while tuned to the
// program channel; otherwise the legacy page path is byte-identical.
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
reg        bs_dl_reload_r = 0;      // toggles on ANY serve-pointer (re)load -> main.v prefetch invalidate
reg        bs_dl_staged = 0;        // a fragment descriptor is waiting to be loaded
reg [16:0] bs_dl_staged_base = 0;
reg [15:0] bs_dl_staged_frames = 0;
reg [15:0] bs_dl_total  = 0;        // frames of the LOADED fragment (0 = none loaded)
reg [16:0] bs_dl_ldbase = 0;        // its ring base, for the latch-write restart
reg        bs_dl_stage_s = 0;       // edge detect of bs_dl_stage
// debug: capture the Town's save writes during a download, filtered to the pack write
// windows (flash $C0-$EF or PSRAM $x6000-$7FFF in banks $80-$FF).
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
assign bs_dl_reload = bs_dl_reload_r;
reg [10:0] bs_dl_pfx_idx = 0;   // frames' prefixes consumed since the fragment (re)start
// data reads follow the ring pointer; armed $218B reads follow the prefix table
assign bs_dl_offset = bs_stb0_en ? (17'h08100 + {6'h0, bs_dl_pfx_idx}) : bs_dl_addr;
assign bs_dl_daddr  = bs_dl_addr;
assign bs_dl_pidx   = bs_dl_pfx_idx;
// debug probe: what the FPGA serves on the armed stream reads
reg [7:0] dbg_pfx_first = 8'hEE;   // first bs_prefix_val served on an armed $218B read after a LOAD
reg [7:0] dbg_cnt_first = 8'hEE;   // first bs_queue_val served on an armed $218A read after a LOAD
reg       dbg_have_pfx = 0, dbg_have_cnt = 0;
reg [7:0] dbg_n_starts = 0;        // prefixes with bit4 (0x10) served since ARM
reg [7:0] dbg_n_loads  = 0;        // fragment loads since ARM
reg [15:0] dbg_pfx_sum = 0;        // SUM of every bs_prefix_val served on armed $218B reads
assign bs_dl_dbg_q  = dbg_pfx_sum;            // served-prefix checksum (mod 65536)
assign bs_dl_dbg_sf = mapped_addr_in[23:8];   // main.v capture: served-DATA checksum (mod 65536)
assign bs_dl_dbg_fl = {dbg_n_starts[3:0], dbg_n_loads[3:0]};      // mod-16 magnitudes

wire bs_dl_armed = bs_dl_arm & (bs_page0 == bs_dl_chan);
assign bs_dl_armed_out = bs_dl_armed;
// read-event atomicity: the trailing-edge strobe can complete after the bus address
// moved to the next access -> spurious consume with the wrong decode. Latch the decode
// at the leading edge (reg_oe_falling) and gate every trailing-edge consumer on it.
reg [4:0] oe_addr_lat = 5'h1f;
reg       oe_base_lat = 1'b0;
reg       oe_armed_lat = 1'b0;
always @(posedge clkin) begin
  if (reg_oe_falling) begin
    oe_addr_lat  <= base_addr;
    oe_base_lat  <= base_enable;
    oe_armed_lat <= bs_dl_armed;
  end else if (reg_oe_rising) begin
    oe_base_lat  <= 1'b0;        // one consume per access window
    oe_armed_lat <= 1'b0;
  end
end
// prefix-through-ring: armed $218B prefixes are served by address from a host-written
// table in the ring (ring+0x8100+frame_idx: 0x10,0,...,0x80), same machinery as $218C
// data. Internal bs_prefix_val stays for the $218D accumulation and the debug probe.
// $218C read serves the ring data; armed $218B serves the ring prefix table
assign bs_dl_enable = base_enable & bs_dl_armed & (bs_page0_en | bs_stb0_en);

// delta terms (combine the every-clock pacer with per-frame read drains)
// advertise cap = 8 frames (not 0x7f): a latch rewrite discards buffered prefix credits
// while data keeps flowing, so a deep buffer permanently desyncs prefix<->data
wire bs_pacer_inc = bs_dl_armed & (bs_dl_queue != 8'h0) & (bs_pf_queue < 8'h08)
                                & bs_pf_latch & bs_dt_latch;
// oe_base_lat is essential: without the full base decode, instruction fetches whose low
// bits alias the stream regs fire phantom consume events
wire bs_pf_dec = reg_oe_rising & oe_armed_lat & oe_base_lat & (oe_addr_lat == 5'h0b) & (bs_pf_queue != 8'h0); // $218B read
wire bs_dt_dec = reg_oe_rising & oe_armed_lat & oe_base_lat & (oe_addr_lat == 5'h0c) & (bs_data_cnt == 5'd21)
                                                          & (bs_dt_queue != 8'h0);  // $218C 22nd byte
// values returned to the SNES on a read (reg_oe_falling mux)
wire [7:0] bs_queue_val  = bs_pf_queue;                                  // $218A (cap 0x7F -> bit7 clear)
wire [7:0] bs_prefix_val = bs_pf_latch                                   // 0 until $218B latched
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

// latched serving for the armed $218A/$218D reads. Captured once at reg_oe_falling
// ($218D reads its pre-clear value, $218A a coherent count snapshot) and held for the
// whole window.
reg [7:0] bs_dl_pard_lat = 8'h00;
always @(posedge clkin) begin
  if (reg_oe_falling & bs_dl_armed & base_enable & (bs_sta0_en | (base_addr == 5'h0d)))
    bs_dl_pard_lat <= bs_sta0_en ? bs_queue_val : base_regs[5'h0d];
end
assign bs_dl_pard_hit  = bs_dl_armed & base_enable
                       & (bs_sta0_en | (base_addr == 5'h0d));
assign bs_dl_pard_data = bs_dl_pard_lat;

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
  // broadcast erase: memset signals rise->fall; end the busy window at the fall
  if(bs_erase_act) bs_erase_act_seen <= 1'b1;
  else if(bs_erase_act_seen) begin
    bs_erase_act_seen <= 1'b0;
    erase_busy_cnt <= 0;
  end
  if(reg_oe_rising && oe_base_lat) begin
    case(oe_addr_lat)
      5'h0b: begin
        bs_stb0_offset <= bs_stb0_offset + 1;
        // accumulate the prefix flags into the $218D status. While armed the SNES
        // received the byte from the ring prefix table (bs_dl_pfx_srv) -> OR that in.
        // The Town reads $218D for the Packet-End (0x80). ($218D read+clear below.)
        base_regs[5'h0d] <= base_regs[5'h0d] | (bs_dl_armed ? bs_dl_pfx_srv : reg_data_in);
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
      // $218D/$2193 (prefix-accumulator status): writes ignored, like real HW. The BIOS
      // enables the latches with 16-bit STAs whose high byte lands here; storing it would
      // poison the status with garbage flags.
      5'h0d, 5'h13: ;
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
            // .mpk: 25-bit ~0.35s. broadcast: 28-bit ~2.8s fail-safe ceiling (bs_erase_act
            // fall ends it in ~ms).
            erase_busy_cnt <= bs_slot ? 28'h1ffffff : 28'hfffffff;
            flash_ovr_r <= 1'b1; flash_status_r <= 1'b1;
          end else if(erase_all_setup_r) begin
            bs_erase_seq_r <= bs_erase_seq_r + 1'b1; bs_erase_blk_r <= 4'hf;
            erase_busy_cnt <= bs_slot ? 28'h1ffffff : 28'hfffffff;
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

// BS-X receiver FSM — drives only bs_dl_* regs (legacy path untouched).
// The "fully consumed" test requires bs_dl_queue==0 (not just the pf/dt queues) so a
// $218A read landing right after a load, before the pacer advertises, doesn't skip it.
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
    bs_dl_staged <= 1'b0;   // discard a fragment staged for the OLD channel
    bs_dl_total  <= 16'h0;  // and the loaded one (a latch write must not resurrect it cross-channel)
    bs_dl_pfx_idx <= 11'h0;
    bs_dl_reload_r <= ~bs_dl_reload_r;
  end else if (reg_we_rising && base_enable && (base_addr == 5'h0b)) begin
    bs_pf_latch <= (reg_data_in != 8'h0); bs_pf_queue <= 8'h0;   // $218B latch enable / ack
    // BIOS latch rewrite = stream re-arm. Re-serve the loaded fragment from the top so
    // the re-armed BIOS gets a clean DG start (0x10) and prefix<->data stays synced.
    if (bs_dl_total != 16'h0) begin
      bs_dl_queue <= bs_dl_total; bs_dl_addr <= bs_dl_ldbase;
      bs_dt_queue <= 8'h0; bs_data_cnt <= 5'h0; bs_dl_first <= 1'b1;
      bs_dl_pfx_idx <= 11'h0;
      bs_dl_reload_r <= ~bs_dl_reload_r;
    end
  end else if (reg_we_rising && base_enable && (base_addr == 5'h0c)) begin
    bs_dt_latch <= (reg_data_in != 8'h0); bs_dt_queue <= 8'h0;   // $218C latch enable / ack
    if (bs_dl_total != 16'h0) begin                              // same restart (see $218B)
      bs_dl_queue <= bs_dl_total; bs_dl_addr <= bs_dl_ldbase;
      bs_pf_queue <= 8'h0; bs_data_cnt <= 5'h0; bs_dl_first <= 1'b1;
      bs_dl_pfx_idx <= 11'h0;
      bs_dl_reload_r <= ~bs_dl_reload_r;
    end
  end else if (reg_oe_rising && oe_armed_lat && oe_base_lat && (oe_addr_lat == 5'h0a)) begin
    // $218A read: advance to the next fragment ONLY when fully consumed
    if (bs_dl_queue == 8'h0 && bs_pf_queue == 8'h0 && bs_dt_queue == 8'h0) begin
      bs_data_cnt <= 5'h0;
      if (bs_dl_staged) begin
        bs_dl_addr   <= bs_dl_staged_base;
        bs_dl_queue  <= bs_dl_staged_frames;
        bs_dl_total  <= bs_dl_staged_frames;     // remember for the latch-write restart
        bs_dl_ldbase <= bs_dl_staged_base;
        bs_dl_first  <= 1'b1;
        bs_dl_need   <= 1'b0;
        bs_dl_staged <= 1'b0;
        bs_dl_pfx_idx <= 11'h0;
        bs_dl_reload_r <= ~bs_dl_reload_r;       // ring CONTENT changed under a possibly equal address
      end else begin
        if (!bs_dl_need) begin                   // edge: notify the MCU exactly once
          bs_dl_need  <= 1'b1;
          bs_dl_seq_r <= bs_dl_seq_r + 1'b1;
        end
        // carousel-of-one: while the host stages the next fragment, re-serve the current
        // one instead of leaving the channel empty (the real broadcast is an endless
        // carousel). Re-served data groups are deduped by the BIOS block bitmap.
        if (bs_dl_total != 16'h0) begin
          bs_dl_addr   <= bs_dl_ldbase;
          bs_dl_queue  <= bs_dl_total;
          bs_dl_first  <= 1'b1;
          bs_dl_pfx_idx <= 11'h0;
          bs_dl_reload_r <= ~bs_dl_reload_r;
        end
      end
    end
  end else begin
    // every-clock pacer + per-frame read drains, delta-combined (race-safe)
    bs_pf_queue <= bs_pf_queue + bs_pacer_inc - bs_pf_dec;
    bs_dt_queue <= bs_dt_queue + bs_pacer_inc - bs_dt_dec;
    bs_dl_queue <= bs_dl_queue - bs_pacer_inc;
    if (bs_pf_dec) begin
      bs_dl_first <= 1'b0;                        // $218B consumed -> Packet-Start spent
      bs_dl_pfx_idx <= bs_dl_pfx_idx + 11'h1;     // next $218B read serves the next table entry
    end
    if (reg_oe_rising && oe_armed_lat && oe_base_lat && (oe_addr_lat == 5'h0c) && (bs_dt_queue != 8'h0)) begin // $218C data byte read
      // advance ONLY when a data frame is advertised; a stray/early $218C with
      // dt_queue==0 must not move the ring ptr or the %22 phase.
      bs_dl_addr  <= bs_dl_addr + 1'b1;
      bs_data_cnt <= (bs_data_cnt == 5'd21) ? 5'd0 : (bs_data_cnt + 1'b1);
    end
  end
end

// debug: capture the Town's save writes, filtered to the pack write windows (flash
// $C0-$EF or $x6000-$7FFF in banks $80-$FF). Tracks min/max/count + which window hit.
wire bs_wr_pack = reg_we_rising & (flash_enable
                                 | (snes_addr[23] & (snes_addr[15:13] == 3'b011)));
// mirrors the fragment-load condition in the receiver FSM (keep in sync)
wire dbg_load = reg_oe_rising & bs_dl_armed & bs_sta0_en & bs_dl_staged
              & (bs_dl_queue == 16'h0) & (bs_pf_queue == 8'h0) & (bs_dt_queue == 8'h0);
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
    dbg_pfx_first <= 8'hEE; dbg_cnt_first <= 8'hEE;
    dbg_have_pfx  <= 1'b0;  dbg_have_cnt  <= 1'b0;
    dbg_n_starts  <= 8'h0;  dbg_n_loads   <= 8'h0;
    dbg_pfx_sum   <= 16'h0;
  end else begin
    // what the FPGA SERVES on armed stream reads (value at the OE-falling latch moment)
    if (dbg_load) begin
      dbg_n_loads  <= dbg_n_loads + 8'h1;
      dbg_have_pfx <= 1'b0;                    // re-capture the FIRST prefix of this fragment
      dbg_have_cnt <= 1'b0;
    end
    if (reg_oe_falling & bs_dl_armed & bs_stb0_en) begin
      if (~dbg_have_pfx) begin dbg_pfx_first <= bs_prefix_val; dbg_have_pfx <= 1'b1; end
      if (bs_prefix_val[4]) dbg_n_starts <= dbg_n_starts + 8'h1;
      dbg_pfx_sum <= dbg_pfx_sum + {8'h0, bs_prefix_val};
    end
    if (reg_oe_falling & bs_dl_armed & bs_sta0_en & ~dbg_have_cnt & (bs_queue_val != 8'h0)) begin
      dbg_cnt_first <= bs_queue_val; dbg_have_cnt <= 1'b1;
    end
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