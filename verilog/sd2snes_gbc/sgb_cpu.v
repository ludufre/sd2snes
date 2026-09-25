`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company:
// Engineer:
//
// Create Date:
// Design Name:
// Module Name:
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
`include "config.vh"

module sgb_cpu(
  input         RST,
  input         CPU_RST,
  input         CLK,
  // Clock enables from gbc_clk (contract section 2): one CLK pulse per GB
  // T-cycle at single speed (CE1, also the PPU dot and the APU tick) and at
  // double speed (CE2, every carry of the phase accumulator; CE1 is every
  // second one, so the two are aligned by construction).  The CPU-side edge is
  // muxed between them by KEY1 bit 7 in the Clocks section below.
  input         CLK_CE1,
  input         CLK_CE2,

  // SYS out
  input         SYS_RDY,
  output        SYS_REQ,
  output        SYS_WR,
  output [15:0] SYS_ADDR,
  input  [7:0]  SYS_RDDATA,
  output [7:0]  SYS_WRDATA,

  output        BOOTROM_ACTIVE,
  output        FREE_SLOT,
  // M-cycles in which the GB's own system-bus access (CPU or OAM DMA) was still
  // in flight when the next bus edge arrived, i.e. the PSRAM chain overran the
  // 39-CLK double-speed budget.  Must stay 0 (GBDG +0C, contract section 10).
  output [15:0] STARVATION,
  // Hold the GB's clock enables for this CLK (gbc_clk.v): a CPU access to the
  // system bus has not retired and the next CPU edge would be the bus edge
  // that consumes it.  See "Clock dilation" in the MCT section.
  output        CLK_HOLD,
  // M-cycles that were stretched by CLK_HOLD (GBDG +1C, saturating).
  output [15:0] DILATION,

  // PPU out
  output        PPU_DOT_EDGE,
  output        PPU_PIXEL_VALID,
  output [1:0]  PPU_PIXEL,
  output        PPU_VSYNC_EDGE,
  output        PPU_HSYNC_EDGE,
  output        PPU_LCD_ON,
  // C6 (GBC-CORE-CONTRACT.md section 14.4): the LCD picture, one pixel per
  // dot, in CGB colour and with the CGB BG/OBJ priority resolved.  C6_PX_VALID
  // is a one CLK pulse a few cycles after the dot that output the pixel.
  output        C6_PX_VALID,
  output [7:0]  C6_PX_X,
  output [7:0]  C6_PX_Y,
  output [14:0] C6_PX_BGR,

  // CGB state exported to sgb.v (WRAM banking, cart header masking)
  output        CGB_COMPAT,        // KEY0[3:2] != 0 -> DMG compatibility mode
  output [2:0]  CGB_SVBK,          // SVBK[2:0], raw (0 still means bank 1)

  // Bridge taps (GBC-CORE-CONTRACT.md sections 5-8).  Nothing in this module
  // reads any of them back; they exist so gbc_bridge.v can build the dirty
  // accumulator, the LY=0 snapshot and the mid-frame log without reaching into
  // the CPU's hierarchy.
  output        TAP_VRAM_WE,       // a VRAM byte was written (CPU or HDMA)
  output        TAP_VRAM_BANK,
  output [12:0] TAP_VRAM_ADDR,
  output        TAP_OAM_WE,        // an OAM byte was written (CPU or OAM-DMA)
  output        TAP_CRAM_WE,
  output [6:0]  TAP_CRAM_IDX,      // {is_obj, index[5:0]}
  output [7:0]  TAP_CRAM_DATA,

  output        TAP_LY0,           // first dot of mode 2 on line 0
  output        TAP_VBLANK,        // entry into LY=144

  output [7:0]  TAP_LY,            // live LY, for the bridge's LY_SYNC
  output [7:0]  TAP_LCDC,
  output [7:0]  TAP_SCX,
  output [7:0]  TAP_SCY,
  output [7:0]  TAP_WX,
  output [7:0]  TAP_WY,
  output [7:0]  TAP_BGP,
  output [7:0]  TAP_OBP0,
  output [7:0]  TAP_OBP1,
  output [7:0]  TAP_OPRI,
  output [7:0]  TAP_KEY0,
  output [7:0]  TAP_KEY1,
  output [7:0]  TAP_VBK,
  output [7:0]  TAP_SVBK,
  output        TAP_SPEED_REQ,     // KEY1[0]: phase 3 arms the switch off this

  output        TAP_MF_VALID,      // mid-frame write, LY 0..143 (contract 8)
  output [3:0]  TAP_MF_KIND,       // 0 SCX 1 SCY 2 WX 3 WY 4 LCDC 5 BGP
                                   // 6 OBP0 7 OBP1 8 BCPD 9 OCPD
  output [5:0]  TAP_MF_IDX,        // BCPD/OCPD: index BEFORE the auto-increment
  output [7:0]  TAP_MF_VALUE,
  output [7:0]  TAP_MF_LY,
  output        TAP_MF_BEFORE_MODE3,

  // VRAM port B, one per bank.  The bridge owns it; the MCU debug pipe takes
  // whatever cycle the bridge did not ask for (contract section 1).
  input         VRAM0_B_REQ,
  input  [12:0] VRAM0_B_ADDR,
  output [7:0]  VRAM0_B_DATA,
  input         VRAM1_B_REQ,
  input  [12:0] VRAM1_B_ADDR,
  output [7:0]  VRAM1_B_DATA,

  // OAM snapshot port, same arbitration rule.  The bridge's copy engine walks
  // all 160 bytes at the LY=0 instant; outside that the debug pipe has it.
  input         OAM_RD_REQ,
  input  [7:0]  OAM_RD_ADDR,
  output [7:0]  OAM_RD_DATA,

  // CRAM snapshot port, same arbitration rule.
  input         CRAM_RD_REQ,
  input  [6:0]  CRAM_RD_ADDR,      // {is_obj, index[5:0]}
  output [7:0]  CRAM_RD_DATA,

  // APU out
  output [19:0] APU_DAT,

  // P1
  output [1:0]  P1O,
  input  [3:0]  P1I,

  // SER
  inout         SER_CLK,
  input         SER_IN,
  output        SER_OUT,

  // Halt
  input         HLT_REQ,
  output        HLT_RSP,
  input         IDL_ICD,

  // Features
  input  [15:0]  FEAT,

  // State
  output        REG_REQ,
  output [7:0]  REG_ADDR,
  output [7:0]  REG_REQ_DATA,
  input  [7:0]  MBC_REG_DATA,

  // DBG
  input         MCU_RRQ,
  input         MCU_WRQ,
  input  [18:0] MCU_ADDR,
  input  [7:0]  MCU_DATA_IN,
  output        MCU_RSP,
  output [7:0]  MCU_DATA_OUT,

  output [11:0] DBG_ADDR,
  input  [7:0]  DBG_GBDG_DATA_IN,
  input  [7:0]  DBG_MBC_DATA_IN,
  input  [7:0]  DBG_CHEAT_DATA_IN,
  input  [7:0]  DBG_MAIN_DATA_IN,

  input  [8*8-1:0] DBG_CONFIG,
  output        DBG_BRK
);

integer i;

//-------------------------------------------------------------------
// DESCRIPTION
//-------------------------------------------------------------------

// This is the SGB2-CPU chip which consists of the following logic:
//
// CPU - Central Processing Unit
//   IFD - Instruction Fetch and Decode
//   EXE - Register Read, EXEcute/Memory, and Writeback
//   ICT - Interrupt ConTroller
// PPU - Pixel Processing Unit
// APU - Audio Processing Unit
// MCT - Memory ConTroller for internal (VRAM, OAM, HRAM, REG) and external state (WRAM and CART)
// DMA - DMA engine for copying data to the OAM
// SER - SERial state machine
//
// DBG - DeBuG state available for breakpoint/watchpoint

//-------------------------------------------------------------------
// MISC
//-------------------------------------------------------------------

`define APU

`define OPR_I       4'd0
`define OPR_PC      4'd1
`define OPR_S8      4'd2
`define OPR_U16     4'd3
`define OPR_BC      4'd4
`define OPR_DE      4'd5
`define OPR_SP      4'd6
`define OPR_AF      4'd7
`define OPR_B       4'd8
`define OPR_C       4'd9
`define OPR_D       4'd10
`define OPR_E       4'd11
`define OPR_H       4'd12
`define OPR_L       4'd13
`define OPR_HL      4'd14
`define OPR_A       4'd15

`define GRP_SPC     4'd0
`define GRP_MOV     4'd1
`define GRP_INC     4'd2
`define GRP_DEC     4'd3
`define GRP_ALU     4'd4
`define GRP_BIT     4'd5
`define GRP_JMP     4'd6
`define GRP____     4'd7
`define GRP_MST     4'd8
`define GRP_MLD     4'd9
`define GRP_MIC     4'd10
`define GRP_MDC     4'd11
`define GRP_MLU     4'd12
`define GRP_MBT     4'd13
`define GRP_CLL     4'd14
`define GRP_RET     4'd15

`define DEC_SZE     15:14
`define DEC_LAT     13:12
`define DEC_DST     11:8
`define DEC_SRC     7:4
`define DEC_GRP     3:0

// Forwarded signals
//
// IFD outputs
//
wire        IFD_EXE_valid;
wire [23:0] IFD_EXE_op;
wire [15:0] IFD_EXE_decode;
wire [15:0] IFD_EXE_pc_start;
wire [15:0] IFD_EXE_pc_end;
wire [15:0] IFD_EXE_pc_next;
wire        IFD_EXE_cb;
wire        IFD_EXE_new;
wire        IFD_EXE_int;
wire        IFD_MCT_req_val;
wire [15:0] IFD_MCT_req_addr_d1;
wire [7:0]  IFD_REG_ic;
//
// EXE outputs
//
wire        EXE_MCT_req_val;
wire [15:0] EXE_MCT_req_addr_d1;
wire        EXE_MCT_req_wr;
wire [7:0]  EXE_MCT_req_data_d1;

wire        EXE_IFD_redirect;
wire [15:0] EXE_IFD_target;
wire        EXE_IFD_ready;
// The STOP that is the CGB speed switch, flagged on its own handoff edge by
// the IFD (see IFD_stop_switch there) and acted on by the REG block.
wire        IFD_stop_switch;
wire        EXE_IFD_ime;

wire        EXE_DMA_halt;
wire        EXE_REG_ime;
//
// REG outputs
//
wire [7:0]  REG_data;
wire        REG_MCT_rsp_val;
wire        REG_DBG_rsp_val;
wire        REG_DMA_start;
wire        REG_req_val;
wire        REG_req_dbg;
wire [7:0]  REG_address;
wire [7:0]  REG_req_data;
//
// MCT outputs
//
wire [7:0]  MCT_data;
wire        MCT_IFD_rsp_val;
wire        MCT_EXE_rsp_val;

wire        MCT_VRAM_wren;
wire [12:0] MCT_VRAM_address;
wire [7:0]  MCT_VRAM_data;

wire        MCT_OAM_wren;
wire [7:0]  MCT_OAM_address;
wire [7:0]  MCT_OAM_data;

wire        MCT_HRAM_wren;
wire [6:0]  MCT_HRAM_address;
wire [7:0]  MCT_HRAM_data;

wire        MCT_REG_req_val;
wire        MCT_REG_wren;
wire [7:0]  MCT_REG_address;
wire [7:0]  MCT_REG_data;
//
// MCT input data
//
wire [7:0]  VRAM_data;
wire [7:0]  OAM_data;
wire [7:0]  HRAM_data;
//
// PPU outputs
//
wire        PPU_VRAM_active;
wire        PPU_PAL_lock;    // CGB palettes closed to the CPU (mode 3 and four dots past it)
wire        PPU_MCT_vram_active; // VRAM closed to the CPU (the PPU's own use, stretched to STAT's mode 3)
wire        PPU_MCT_oam_active;  // OAM closed to the CPU (same)
wire [12:0] PPU_VRAM_address;
wire        PPU_OAM_active;
wire [7:0]  PPU_OAM_address;
wire        PPU_REG_vblank;
wire        PPU_REG_lcd_stat;
wire [7:0]  PPU_LY_read;     // LY as the CPU reads it (leads the line by a few dots)
wire        PPU_vblank;
//
// DMA outputs
//
wire        DMA_SYS_active;
wire        DMA_VRAM_active;
wire        DMA_active;

wire        DMA_req_val;
wire [15:0] DMA_address;

wire        DMA_OAM_req_val;
wire [7:0]  DMA_OAM_address;
wire [7:0]  DMA_OAM_req_data;
//
// APU outputs
//
wire [3:0]  APU_REG_enable;

wire        SER_REG_done;

wire        DBG_EXE_step;

wire        DBG_REG_req_val;
wire        DBG_REG_wren;
wire [7:0]  DBG_REG_address;
wire [7:0]  DBG_REG_data;
wire        DBG_advance;

wire        HLT_REQ_sync;
wire        HLT_IFD_rsp;
wire        HLT_EXE_rsp;
wire        HLT_DMA_rsp;
wire        HLT_SER_rsp;

assign      HLT_RSP = HLT_REQ_sync & HLT_IFD_rsp & HLT_EXE_rsp & HLT_DMA_rsp & HLT_SER_rsp;

//-------------------------------------------------------------------
// Clocks
//-------------------------------------------------------------------

// KEY1 double speed (contract section 2).  Two enables come in from gbc_clk,
// both registered there and aligned by construction (CE1 is every second CE2):
//
//   CLK_PPU_EDGE  always CE1.  The PPU dot, the APU tick and everything the
//                 bridge counts (frame_dot_ctr) do not change rate with KEY1.
//   CLK_CPU_EDGE  CE1 at single speed, CE2 at double speed.  The CPU pipeline,
//                 the memory controller, DIV/TIMA, OAM DMA, HDMA/GDMA and the
//                 serial port all run off this one, so at 2x an M-cycle is
//                 4 x CE2 = 39 or 40 CLK (the accumulator dithers) instead of 80.
//
// The select is a REGISTER (cpu_speed_r, KEY1 bit 7) and nothing else: the
// mux sits in front of the highest-fanout enable net in the core, so it must
// be one LUT on a flop, never a decoded condition.  cpu_speed_r only ever
// toggles on a CLK_BUS_EDGE, i.e. between two M-cycles: an M-cycle is never
// made of edges from both rates, and no edge is lost or doubled at the switch
// (both enables are single-CLK pulses at least 9 CLK apart, so the CLK after
// a bus edge both are 0 whatever the select does).
//
// At single speed CLK_CPU_EDGE == CLK_CE1 bit for bit, which is what keeps
// every 1x cycle count of the phase-2 baselines exactly where it was.
//
// Coming back DOWN costs the CPU domain one dot against the LCD: the first
// CE1 after the 2x -> 1x flip is swallowed (cpu_slip_r).  gambatte
// PPU::speedChange does `now -= isDoubleSpeed()` on the way down and nothing
// on the way up, i.e. after a round trip every LCD event reaches the CPU one
// dot earlier than before it; without the slip the 17 speedchange2_* /
// speedchange5_* `*_m3stat_scx*_2` ROMs read mode 3 where the hardware
// already shows mode 0, with it the whole directory gains and loses nothing.
// The slip flop is one more input of the same LUT: still one LUT on flops.
reg         cpu_speed_r;                // KEY1 bit 7; written in the REG block
reg         cpu_speed_d1_r;
reg         cpu_slip_r;
wire        CLK_CPU_EDGE = (cpu_speed_r ? CLK_CE2 : CLK_CE1) & ~cpu_slip_r;
wire        CLK_PPU_EDGE = CLK_CE1;

// Generate a BUS clock edge from the incoming CPU clock edge.  The
// BUS clock is always /4.
reg  [1:0]  clk_bus_ctr_r; always @(posedge CLK) clk_bus_ctr_r <= RST ? 0 : clk_bus_ctr_r + (CLK_CPU_EDGE ? 1 : 0);
wire        CLK_BUS_EDGE = CLK_CPU_EDGE & &clk_bus_ctr_r;

// Synchronize reset to bus edge.  Want a full bus clock prior to first edge assertion and the system bus to be ready
reg         cpu_ireset_r; always @(posedge CLK) cpu_ireset_r <= RST | CPU_RST | (cpu_ireset_r & (~CLK_BUS_EDGE | ~SYS_RDY));

// The slip pair (see CLK_CPU_EDGE).  Only a STOP switch slips: a GB reset
// taken at double speed drops cpu_speed_r too, and the reset keeps the pair
// clear through it.
always @(posedge CLK) begin
  cpu_speed_d1_r <= (RST | cpu_ireset_r) ? 1'b0 : cpu_speed_r;
  if (RST | cpu_ireset_r)                   cpu_slip_r <= 1'b0;
  else if (cpu_speed_d1_r & ~cpu_speed_r)   cpu_slip_r <= 1'b1;
  else if (CLK_CE1)                         cpu_slip_r <= 1'b0;
end

// Assume GB only needs PSRAM on the first of the 4 CPU clocks.  Each CPU clock is a minimum of 16 CLK2
// and PSRAM should only need ~8 of those clocks to perform the access.  It's possible that the GB timing
// will spill into the first free slot, but that will just remove that slot for MCU use.

// The delay needs to match the mct request pipe to the system:
// -1 - CLK_BUS_EDGE
//  0 - MCT request             dma_req_r SYS/REQ
//  1 - MCT decode              ReqPendr
//  2 - MCT mct_req_r/SYS_REQ
//  3 - ReqPendr
reg  [5:0]  cpu_free_slot_r;
always @(posedge CLK) begin
  // MCU needs more bandwidth so now we only block the last empty CPU clock cycle shifted by a value greater than the MCT delay
  cpu_free_slot_r <= {cpu_free_slot_r[4:0],(~&clk_bus_ctr_r)};
end

assign FREE_SLOT = cpu_free_slot_r[5];

reg  [1:0]  ppu_vblank_sync_r;
reg         hlt_req_sync_r;

always @(posedge CLK) begin
  if (CLK_BUS_EDGE) ppu_vblank_sync_r <= {ppu_vblank_sync_r[0],PPU_vblank};
  hlt_req_sync_r <= cpu_ireset_r ? 0 : (CLK_BUS_EDGE && ppu_vblank_sync_r == 2'b01) ? HLT_REQ : hlt_req_sync_r;
end

assign HLT_REQ_sync = hlt_req_sync_r;

//-------------------------------------------------------------------
// REG/MMIO
//-------------------------------------------------------------------

`define P1_I              3:0
`define P1_O              5:4

`define TAC_FREQ_DIV      1:0
`define TAC_ENABLE        2:2

`define LCDC_BG_EN        0:0
`define LCDC_SP_EN        1:1
`define LCDC_SP_SIZE      2:2
`define LCDC_BG_MAP_SEL   3:3
`define LCDC_BG_TILE_SEL  4:4
`define LCDC_WD_EN        5:5
`define LCDC_WD_MAP_SEL   6:6
`define LCDC_DS_EN        7:7

// PPU mode, as STAT bits 1:0 report it.  Defined up here rather than next to
// the PPU because the register block below has to know when the CPU wrote
// before mode 3 of the current line (contract section 8).
`define MODE_H      0 // HBLANK
`define MODE_V      1 // VBLANK
`define MODE_O      2 // OAM READ
`define MODE_D      3 // DISPLAY WRITE

`define STAT_MODE         1:0
`define STAT_ACTIVE       1:1
`define STAT_LYC_MATCH    2:2
`define STAT_INT_H_EN     3:3 // mode 0
`define STAT_INT_V_EN     4:4 // mode 1
`define STAT_INT_O_EN     5:5 // mode 2
`define STAT_INT_M_EN     6:6

`define PAL0              1:0
`define PAL1              3:2
`define PAL2              5:4
`define PAL3              7:6

`define BOOT_ROM_DI       0:0

`define IE_VBLANK         0:0
`define IE_LCD_STAT       1:1
`define IE_TIMER          2:2
`define IE_SERIAL         3:3
`define IE_JOYPAD         4:4

// square1
`define NR10_SWEEP_SHIFT  2:0
`define NR10_SWEEP_NEG    3:3
`define NR10_SWEEP_TIME   6:4
`define NR11_LENGTH       5:0
`define NR11_DUTY         7:6
`define NR12_ENV_PERIOD   2:0
`define NR12_ENV_DIR      3:3
`define NR12_ENV_VOLUME   7:4
`define NR13_FREQ_LSB     7:0
`define NR14_FREQ_MSB     2:0
`define NR14_FREQ_STOP    6:6
`define NR14_FREQ_ENABLE  7:7

// square2
`define NR21_LENGTH       5:0
`define NR21_DUTY         7:6
`define NR22_ENV_PERIOD   2:0
`define NR22_ENV_DIR      3:3
`define NR22_ENV_VOLUME   7:4
`define NR23_FREQ_LSB     7:0
`define NR24_FREQ_MSB     2:0
`define NR24_FREQ_STOP    6:6
`define NR24_FREQ_ENABLE  7:7

// wave
`define NR30_WAVE_ENABLE  7:7
`define NR31_LENGTH       7:0
`define NR32_LEVEL        6:5
`define NR33_FREQ_LSB     7:0
`define NR34_FREQ_MSB     2:0
`define NR34_FREQ_STOP    6:6
`define NR34_FREQ_ENABLE  7:7

// noise
`define NR41_LENGTH       5:0
`define NR42_ENV_PERIOD   2:0
`define NR42_ENV_DIR      3:3
`define NR42_ENV_VOLUME   7:4
`define NR43_LFSR_DIV     2:0
`define NR43_LFSR_WIDTH   3:3
`define NR43_LFSR_SHIFT   7:4
`define NR44_FREQ_STOP    6:6
`define NR44_FREQ_ENABLE  7:7

// control
`define NR50_MASTER_LEFT_VOLUME   2:0
`define NR50_MASTER_LEFT_ENABLE   3:3
`define NR50_MASTER_RIGHT_VOLUME  6:4
`define NR50_MASTER_RIGHT_ENABLE  7:7
`define NR51_SELECT_LEFT_CH0      0:0
`define NR51_SELECT_LEFT_CH1      1:1
`define NR51_SELECT_LEFT_CH2      2:2
`define NR51_SELECT_LEFT_CH3      3:3
`define NR51_SELECT_RIGHT_CH0     4:4
`define NR51_SELECT_RIGHT_CH1     5:5
`define NR51_SELECT_RIGHT_CH2     6:6
`define NR51_SELECT_RIGHT_CH3     7:7
`define NR52_CONTROL_CH0_ACTIVE   0:0
`define NR52_CONTROL_CH1_ACTIVE   1:1
`define NR52_CONTROL_CH2_ACTIVE   2:2
`define NR52_CONTROL_CH3_ACTIVE   3:3
`define NR52_CONTROL_ENABLE       7:7

reg [15:0]  PC_r;
reg [7:0]   A_r;
reg [7:0]   F_r;
reg [7:0]   B_r;
reg [7:0]   C_r;
reg [7:0]   D_r;
reg [7:0]   E_r;
reg [7:0]   H_r;
reg [7:0]   L_r;
reg [15:0]  SP_r;

`define AF_r {A_r,F_r}
`define BC_r {B_r,C_r}
`define DE_r {D_r,E_r}
`define HL_r {H_r,L_r}

`define FLAG_Z 7
`define FLAG_N 6
`define FLAG_H 5
`define FLAG_C 4

reg [7:0]   REG_P1_r;   // FF00
reg [7:0]   REG_SB_r;   // FF01
reg [7:0]   REG_SC_r;   // FF02

reg [15:0]  REG_DIV_r;  // FF04 top 8b
reg [7:0]   REG_TIMA_r; // FF05
reg [7:0]   REG_TMA_r;  // FF06
reg [7:0]   REG_TAC_r;  // FF07

reg [7:0]   REG_IF_r;   // FF0F

// APU
reg [7:0]   REG_NR10_r; // FF10
reg [7:0]   REG_NR11_r; // FF11
reg [7:0]   REG_NR12_r; // FF12
reg [7:0]   REG_NR13_r; // FF13
reg [7:0]   REG_NR14_r; // FF14

reg [7:0]   REG_NR21_r; // FF16
reg [7:0]   REG_NR22_r; // FF17
reg [7:0]   REG_NR23_r; // FF18
reg [7:0]   REG_NR24_r; // FF19

reg [7:0]   REG_NR30_r; // FF1A
reg [7:0]   REG_NR31_r; // FF1B
reg [7:0]   REG_NR32_r; // FF1C
reg [7:0]   REG_NR33_r; // FF1D
reg [7:0]   REG_NR34_r; // FF1E

reg [7:0]   REG_NR41_r; // FF20
reg [7:0]   REG_NR42_r; // FF21
reg [7:0]   REG_NR43_r; // FF22
reg [7:0]   REG_NR44_r; // FF23

reg [7:0]   REG_NR50_r; // FF24
reg [7:0]   REG_NR51_r; // FF25
reg [7:0]   REG_NR52_r; // FF26

reg [7:0]   REG_WAV_r[15:0]; // FF30-FF3F

// PPU
reg [7:0]   REG_LCDC_r; // FF40
reg [7:0]   REG_STAT_r; // FF41
reg [7:0]   REG_SCY_r;  // FF42
reg [7:0]   REG_SCX_r;  // FF43
reg [7:0]   REG_LY_r;   // FF44
reg [7:0]   REG_LYC_r;  // FF45
reg [7:0]   REG_DMA_r;  // FF46
reg [7:0]   REG_BGP_r;  // FF47
reg [7:0]   REG_OBP0_r; // FF48
reg [7:0]   REG_OBP1_r; // FF49
reg [7:0]   REG_WY_r;   // FF4A
reg [7:0]   REG_WX_r;   // FF4B

// MISC
reg [0:0]   REG_BOOT_r; // FF50
reg [7:0]   REG_IE_r;   // FFFF

// CGB
// Behaviour taken from Pan Docs "CGB Registers" and cross-checked against
// SameBoy Core/memory.c (the read switch around line 620 and the write switch
// around line 1610).  Where the two disagree the comment says which one won.
reg [7:0]   REG_KEY0_r; // FF4C  bit 3/2 = DMG compatibility, locked by FF50
reg [7:0]   REG_KEY1_r; // FF4D  bit 0 armed, bit 7 = current speed (phase 3)
reg         REG_VBK_r;  // FF4F  VRAM bank
reg [2:0]   REG_RP_r;   // FF56  {bit7, bit6, bit0} of the IR port
reg         REG_BCPS_ai_r;
reg [5:0]   REG_BCPS_r; // FF68
reg         REG_OCPS_ai_r;
reg [5:0]   REG_OCPS_r; // FF6A
reg         REG_OPRI_r; // FF6C  bit 0
reg [2:0]   REG_SVBK_r; // FF70  WRAM bank, 0 still reads back as 0
reg [7:0]   REG_PSWX_r; // FF72
reg [7:0]   REG_PSWY_r; // FF73
reg [7:0]   REG_PSW_r;  // FF74
reg [2:0]   REG_PGB_r;  // FF75  bits 6:4 only

// HDMA/GDMA, implemented after the memory controller (it borrows the system
// bus the same way the OAM DMA does).  Declared here because the register
// read-back and the CPU stall are needed above.
wire        hdma_running;    // HDMA5 bit 7 read-back is ~this
wire [6:0]  hdma_left;       // HDMA5 bits 6:0: blocks remaining - 1
wire        hdma_cpu_stall;  // freeze the CPU pipeline for the current block
wire        hdma_bus_busy;   // ... and, narrower, hold off instruction fetch
wire        hdma_req_pending; // an H-Blank block has been requested and not started yet
wire        HDMA_SYS_active; // the HDMA owns the system bus this cycle
wire        hdma_sys_req;
wire [15:0] hdma_sys_addr;

parameter
  ST_REG_IDLE     = 3'b001,
  ST_REG_REQ      = 3'b010,
  ST_REG_END      = 3'b100;

reg         reg_req_r;
reg [2:0]   reg_state_r;
reg [7:0]   reg_addr_r;
reg         reg_src_r;
reg         reg_wr_r;
reg [7:0]   reg_wr_data_r;
reg [7:0]   reg_mdr_r;

reg         tmr_apu_step_r;
reg         tmr_ovf_1024_r;
reg         tmr_ovf_16_r;
reg         tmr_ovf_64_r;
reg         tmr_ovf_256_r;
reg         tmr_ovf_tima_r;
reg         tmr_cpu_edge_d1_r;

reg         reg_dma_start_r;
reg         reg_int_write_r;
reg  [7:0]  reg_int_write_data_r;
reg         exe_stop_d1_r;      // STOP edge, for the CGB speed switch
// Declared up here rather than with the rest of the EXE result registers,
// because the REG block below has to see it: a STOP with KEY1 bit 0 armed is
// the CGB speed switch and clears that bit when it retires.
reg         exe_res_stop_r;

// CGB speed switch state (KEY1 + STOP; contract section 2).  cpu_speed_r
// itself lives in the Clocks section because the enable mux reads it.
//
// Reference: SameBoy Core/sm83_cpu.c stop() and Core/timing.c
// GB_advance_cycles(); gambatte libgambatte/src/memory.cpp Memory::stop()
// agrees on every number used here.
//
//   * A STOP with KEY1.0 armed and NO button held is the switch.  With a
//     button held it is an "exit by joypad" and the arming bit is left alone
//     (sm83_cpu.c:392-393).
//   * Everything is timed from the STOP's own handoff edge, `cc` in gambatte's
//     Memory::stop(): the end of the M-cycle that fetched the STOP opcode.
//     On that edge DIV is reset and KEY1.0 clears (memory.cpp:445-447).  The
//     clock itself flips on that same edge going 2x -> 1x, and TWO M-cycles
//     later (still at 1x) going 1x -> 2x (memory.cpp:455, `cc_ = cc + 8 *
//     !isDoubleSpeed()`).  The asymmetry is load-bearing: it is what makes
//     the resume land 0x20004 T after the DIV reset in BOTH directions with
//     DIV reading 0x20004 there (age spsw-div), and it is what puts the 1x
//     M-cycle grid after a switch down on the phase the LCD-relative tests
//     measure (gambatte `*_lcdoffds*`, `*_lcdoffset1*`, age spsw-mode0).
//     Flipping one 2x M-cycle later -- the first bus edge AFTER the handoff,
//     which is what this core did before -- lands every instruction after a
//     switch down 2 dots late against the LCD, and the error accumulates over
//     round trips.
//   * Unless IE & IF was already pending at the STOP (sm83_cpu.c:433,444), the
//     CPU is then HALTED, PPU/APU/timers running (HDMA is NOT: see the HDMA
//     section), until the first M-cycle of the next instruction lands 0x20004
//     T-cycles after the DIV reset, counted on the CPU's own clock (gambatte
//     memory.cpp:443: unhalt at cc + 0x20000 + 4; SameBoy sm83_cpu.c:434 has
//     0x20008 and a TODO saying the timing is unverified): 32769 M-cycles.
//     Like any HALT it ends early the moment IE & IF becomes pending
//     (sm83_cpu.c:1643-1662), and like any HALT that wake costs the extra
//     M-cycle (spd_wake_r, gambatte memory.cpp:299 `cc += 4`).
//
// SPEED_SWITCH_MCYC is the number of PARKED bus edges.  The handoff edge (DIV
// reset) precedes them and the STOP hands over on the edge after the last one,
// so the reset-to-resume distance is SPEED_SWITCH_MCYC + 1 = 32769 M-cycles =
// 0x20004 T.  Pinned by age-test-roms speed-switch/spsw-div (verified on CPU
// CGB B/C/E): after a switch, DIV reads 0 through 60 NOPs and 1 through 61.
// One M-cycle longer (SameBoy's 0x20008) and DIV reads 1 through 60 as well.
localparam [15:0] SPEED_SWITCH_MCYC = 16'd32768;   // parked edges = 0x20000 T; +1 edge = 0x20004 T end to end
reg         spd_go_r;           // 1x -> 2x: the flip is pending (two bus edges after the handoff)
reg         spd_go_ctr_r;       // ... the first of those two edges has not passed yet
reg         spd_stall_r;        // CPU parked after the switch (HALT with a timeout)
reg  [15:0] spd_ctr_r;          // M-cycles of park remaining
reg         spd_stop_r;         // the STOP now in EXE is the switch, not a wait for input
reg         spd_wake_r;         // the park ended on IE & IF: hold the STOP one more bus edge (the CGB's wake cost)
reg         spd_tail_r;         // the park ended on its timer: the edge on which the STOP retires (HDMA wake check)

// DMG compatibility mode.  SameBoy: cgb_mode = !(KEY0 & 0xC) -- two bits decide
// it, not just bit 2.  Everything CGB-only keys off ~cgb_compat.
wire        cgb_compat = |REG_KEY0_r[3:2];
wire        cgb_mode   = ~cgb_compat;

// The palette registers keep working while the boot ROM is mapped even in
// compatibility mode: the boot ROM is what writes the DMG palettes
// (SameBoy memory.c: "if (!gb->cgb_mode && gb->boot_rom_finished) return").
wire        cram_cpu_ok  = cgb_mode | ~REG_BOOT_r[`BOOT_ROM_DI];
// CGB palette RAM is inaccessible during mode 3, exactly like VRAM.  The MCU
// debug pipe is not the CPU and is never blocked (the same exemption the wave
// RAM reads already carry).
wire        cram_blocked = (PPU_VRAM_active | PPU_PAL_lock) & ~reg_src_r;

// The CRAM index the CPU port is looking at.  It has to be valid one cycle
// BEFORE ST_REG_REQ, because the palette RAM has a registered read port, so it
// is driven from the address the REG state machine is about to latch.
wire [7:0]  reg_addr_pending = MCT_REG_req_val ? MCT_REG_address : DBG_REG_address;
wire [7:0]  cram_reg_addr    = |(reg_state_r & ST_REG_IDLE) ? reg_addr_pending : reg_addr_r;
wire        cram_a_obj       = (cram_reg_addr == 8'h6B);
wire [6:0]  cram_a_index     = {cram_a_obj, cram_a_obj ? REG_OCPS_r : REG_BCPS_r};

assign BOOTROM_ACTIVE = ~REG_BOOT_r[`BOOT_ROM_DI];
assign P1O = REG_P1_r[5:4];

assign REG_MCT_rsp_val = |(reg_state_r & ST_REG_END) & ~reg_src_r;
assign REG_DBG_rsp_val = |(reg_state_r & ST_REG_END) &  reg_src_r;
assign REG_data = reg_mdr_r;

assign REG_DMA_start = reg_dma_start_r;

assign REG_req_val  = |(reg_state_r & ST_REG_REQ) & reg_wr_r;
`ifdef SGB_SAVE_STATES
assign REG_req_dbg  = |(reg_state_r & ST_REG_REQ) & reg_wr_r &  reg_src_r;
`else
assign REG_req_dbg  = 0;
`endif
assign REG_address  = reg_addr_r;
assign REG_req_data = reg_wr_data_r;

assign REG_REQ      = REG_req_dbg;
assign REG_ADDR     = REG_address;
assign REG_REQ_DATA = REG_req_data;

//-------------------------------------------------------------------
// CRAM (CGB palette RAM)
//-------------------------------------------------------------------

// 2 x 64 bytes, addressed {is_obj, index[5:0]}.  Port A is the GB's
// BCPD/OCPD access, port B is the bridge snapshot / MCU debug read.  This is
// the inferred true-dual-port shape on purpose: an array read
// combinationally would become 1024 flip-flops instead of one M9K.
//
// Port A read-during-write is a don't-care (a write's read-back is never
// used), and a port A / port B collision on the same index is covered by the
// contract's "set wins" dirty rule (section 0.5), not by this RAM.
wire        cram_wr_hit = |(reg_state_r & ST_REG_REQ) & reg_wr_r
                        & ((reg_addr_r == 8'h69) | (reg_addr_r == 8'h6B));
wire        cram_we_a   = cram_wr_hit & cram_cpu_ok & ~cram_blocked;

// Port B: the bridge wins the cycle it asks for, the debug pipe waits.
// cram_dbg_addr is driven from dbg_addr_r down in the DBG block.
wire [6:0]  cram_dbg_addr;
// C6: the pixel pipeline reads the colour of every dot through port B, two
// bytes on the two cycles after the dot (c6_cram_req / c6_cram_addr, below in
// the PPU).  The snapshot copy engine keeps its priority: it only runs from
// the first dot of mode 2 on line 0 and is over ~160 CLK later, while the first
// pixel of that line is output ~80 dots (~1600 CLK) after it, so the two never
// meet; the debug pipe waits for both.
wire        c6_cram_req;
wire [6:0]  c6_cram_addr;
wire        cram_b_busy = CRAM_RD_REQ | c6_cram_req;
wire [6:0]  cram_b_addr = CRAM_RD_REQ ? CRAM_RD_ADDR : c6_cram_req ? c6_cram_addr : cram_dbg_addr;

reg  [7:0]  cram_r[0:127];
reg  [7:0]  cram_q_a_r;
reg  [7:0]  cram_q_b_r;
reg         cram_b_busy_d1_r;

always @(posedge CLK) begin
  if (cram_we_a) cram_r[cram_a_index] <= reg_wr_data_r;
  cram_q_a_r <= cram_r[cram_a_index];
  cram_q_b_r <= cram_r[cram_b_addr];
  cram_b_busy_d1_r <= cram_b_busy;
end

assign CRAM_RD_DATA = cram_q_b_r;

assign TAP_CRAM_WE   = cram_we_a;
assign TAP_CRAM_IDX  = cram_a_index;
assign TAP_CRAM_DATA = reg_wr_data_r;

//-------------------------------------------------------------------
// Mid-frame write log tap (GBC-CORE-CONTRACT.md section 8)
//-------------------------------------------------------------------

// Every write the GAME makes to a register the SNES re-render cannot express
// per frame, while a visible line is being drawn.  BCPD/OCPD only count when
// the write actually landed and means a colour: cram_we_a carries the mode 3
// block, but NOT the compatibility exclusion -- cram_cpu_ok is an OR, on
// purpose, because the boot ROM loads the DMG palettes through these ports
// before it drops into compatibility mode -- so cgb_mode is asked for here.
// Two writers are not the game and never enter the log: the MCU debug pipe
// (reg_src_r) and the boot ROM, whose logo animation scrolls with the LCD on.
wire        mf_reg_wr = |(reg_state_r & ST_REG_REQ) & reg_wr_r & ~reg_src_r;
wire        mf_is_cram = (reg_addr_r == 8'h69) | (reg_addr_r == 8'h6B);
wire        mf_is_reg  = (reg_addr_r == 8'h43) | (reg_addr_r == 8'h42)
                       | (reg_addr_r == 8'h4B) | (reg_addr_r == 8'h4A)
                       | (reg_addr_r == 8'h40) | (reg_addr_r == 8'h47)
                       | (reg_addr_r == 8'h48) | (reg_addr_r == 8'h49);

wire        mf_wr = ~BOOTROM_ACTIVE
                  & ((mf_reg_wr & mf_is_reg) | (cram_we_a & ~reg_src_r & mf_is_cram & cgb_mode));
assign TAP_MF_KIND  = (reg_addr_r == 8'h43) ? 4'd0    // SCX
                    : (reg_addr_r == 8'h42) ? 4'd1    // SCY
                    : (reg_addr_r == 8'h4B) ? 4'd2    // WX
                    : (reg_addr_r == 8'h4A) ? 4'd3    // WY
                    : (reg_addr_r == 8'h40) ? 4'd4    // LCDC
                    : (reg_addr_r == 8'h47) ? 4'd5    // BGP
                    : (reg_addr_r == 8'h48) ? 4'd6    // OBP0
                    : (reg_addr_r == 8'h49) ? 4'd7    // OBP1
                    : (reg_addr_r == 8'h69) ? 4'd8    // BCPD
                    :                         4'd9;   // OCPD
// The index the write used, i.e. BEFORE the auto-increment (contract 8).
assign TAP_MF_IDX   = mf_is_cram ? cram_a_index[5:0] : 6'h00;
assign TAP_MF_VALUE = reg_wr_data_r;
// TAP_MF_VALID, TAP_MF_LY and TAP_MF_BEFORE_MODE3 are assigned next to TAP_LY0,
// below the PPU state machine they are decoded from.

assign CGB_COMPAT = cgb_compat;
assign CGB_SVBK   = REG_SVBK_r;

assign TAP_LY   = REG_LY_r;
assign TAP_LCDC = REG_LCDC_r;
assign TAP_SCX  = REG_SCX_r;
assign TAP_SCY  = REG_SCY_r;
assign TAP_WX   = REG_WX_r;
assign TAP_WY   = REG_WY_r;
assign TAP_BGP  = REG_BGP_r;
assign TAP_OBP0 = REG_OBP0_r;
assign TAP_OBP1 = REG_OBP1_r;
assign TAP_OPRI = {7'h00,REG_OPRI_r};
assign TAP_KEY0 = REG_KEY0_r;
assign TAP_KEY1 = {cpu_speed_r,6'h00,REG_KEY1_r[0]};   // bit 7 = current speed, bit 0 = armed
assign TAP_VBK  = {7'h00,REG_VBK_r};
assign TAP_SVBK = {5'h00,REG_SVBK_r};
assign TAP_SPEED_REQ = REG_KEY1_r[0];

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    reg_state_r <= ST_REG_IDLE;
    reg_req_r   <= 0;

    tmr_cpu_edge_d1_r <= 0;
    tmr_ovf_tima_r <= 0;
    tmr_apu_step_r <= 0;

    reg_dma_start_r <= 0;
    reg_int_write_r <= 0;
    exe_stop_d1_r   <= 0;

    // A GB reset (GO = 0 included) always lands at single speed.
    cpu_speed_r  <= 1'b0;
    spd_go_r     <= 1'b0;
    spd_go_ctr_r <= 1'b0;
    spd_stall_r  <= 1'b0;
    spd_ctr_r    <= 16'd0;
    spd_stop_r   <= 1'b0;
    spd_wake_r   <= 1'b0;
    spd_tail_r   <= 1'b0;

    REG_P1_r[5:4] <= 2'b11;   // FF00
    //REG_SB_r;   // FF01
    //REG_SC_r;   // FF02

    REG_DIV_r     <= 16'h0000;  // FF04
    REG_TIMA_r    <= 8'h00; // FF05
    REG_TMA_r     <= 8'h00; // FF06
    REG_TAC_r     <= 8'h00; // FF07

    REG_IF_r      <= 8'h00;   // FF0F

    // Audio registers written by APU

    REG_LCDC_r    <= 8'h00; // FF40
    REG_STAT_r[7:3] <= 0;
    REG_SCY_r     <= 8'h00; // FF42
    REG_SCX_r     <= 8'h00; // FF43
    //REG_LY_r // FF44
    REG_LYC_r     <= 8'h00; // FF45
    // SIMULATION HYGIENE: REG_DMA_r was deliberately left out of this list
    // (hardware powers it up at 0 and the GB always writes it before starting
    // an OAM DMA).  In simulation it is X, and DMA_address = {REG_DMA_r, ...}
    // then issues X-addressed PSRAM reads.  No functional change.
    REG_DMA_r     <= 8'h00; // FF46
    REG_BGP_r     <= 8'hFC; // FF47
    REG_OBP0_r    <= 8'h00; // CGB hardware powers up OBP0/OBP1 at $00 (the DMG's $FF is a DMG-only thing); the boot ROM writes them in compat mode // FF48
    REG_OBP1_r    <= 8'h00; // FF49
    REG_WY_r      <= 8'h00; // FF4A
    REG_WX_r      <= 8'h00; // FF4B

    REG_BOOT_r    <= 8'h00; // FF50

    REG_IE_r      <= 8'h00; // FFFF

    // CGB.  KEY0 = 0 means CGB mode: a reset with no boot ROM has to land in
    // CGB mode, because that is the state the +NOBOOT stub assumes.
    REG_KEY0_r    <= 8'h00; // FF4C
    REG_KEY1_r    <= 8'h00; // FF4D
    REG_VBK_r     <= 1'b0;  // FF4F
    REG_RP_r      <= 3'h0;  // FF56
    REG_BCPS_ai_r <= 1'b0;  // FF68
    REG_BCPS_r    <= 6'h00;
    REG_OCPS_ai_r <= 1'b0;  // FF6A
    REG_OCPS_r    <= 6'h00;
    REG_OPRI_r    <= 1'b0;  // FF6C
    REG_SVBK_r    <= 3'h0;  // FF70
    REG_PSWX_r    <= 8'h00; // FF72
    REG_PSWY_r    <= 8'h00; // FF73
    REG_PSW_r     <= 8'h00; // FF74
    REG_PGB_r     <= 3'h0;  // FF75
  end
  else begin

    // timers.  DIV counts CPU T-cycles, so at double speed it and TIMA run
    // twice as fast (Pan Docs, KEY1: "Timer and Divider Registers" are on the
    // list of things that speed up).
    if (CLK_CPU_EDGE & DBG_advance) begin
      REG_DIV_r      <= REG_DIV_r + 1;
      tmr_ovf_16_r   <= &REG_DIV_r[3:0];
      tmr_ovf_64_r   <= &REG_DIV_r[5:0];
      tmr_ovf_256_r  <= &REG_DIV_r[7:0];
      tmr_ovf_1024_r <= &REG_DIV_r[9:0];
    end

    // APU frame sequencer clock: the falling edge of DIV bit 12 at single
    // speed and of bit 13 at double speed (SameBoy timing.c:247), which keeps
    // the sequencer at 512 Hz while DIV runs twice as fast.  The flag is STICKY
    // until the APU -- which ticks on CLK_PPU_EDGE, not on the CPU edge -- has
    // consumed it: with DIV on CE2 and the APU on CE1, a flag that only lived
    // for one CPU edge would be overwritten before the APU looked at it on
    // half of the DIV phases.  At single speed the two edges are the same edge
    // and this is exactly the old one-edge pulse.
    if (CLK_CPU_EDGE & DBG_advance & (cpu_speed_r ? &REG_DIV_r[13:0] : &REG_DIV_r[12:0]))
      tmr_apu_step_r <= 1'b1;
    else if (CLK_PPU_EDGE)
      tmr_apu_step_r <= 1'b0;

    tmr_cpu_edge_d1_r <= CLK_CPU_EDGE;

    // there are at least 16 base clocks in a CPU clock so update the timer state using a base clock delay
    if (REG_TAC_r[`TAC_ENABLE]) begin
      if (tmr_cpu_edge_d1_r) begin
        if (REG_TAC_r[`TAC_FREQ_DIV] == 0 ? tmr_ovf_1024_r : REG_TAC_r[`TAC_FREQ_DIV] == 1 ? tmr_ovf_16_r : REG_TAC_r[`TAC_FREQ_DIV] == 2 ? tmr_ovf_64_r : tmr_ovf_256_r) begin
          {tmr_ovf_tima_r,REG_TIMA_r} <= REG_TIMA_r + 1;
        end
      end
      else if (CLK_CPU_EDGE) begin
        tmr_ovf_tima_r <= 0;

        // load TMA into TIMA one CPU clock after the overflow
        if (tmr_ovf_tima_r) REG_TIMA_r <= REG_TMA_r;
      end
    end
    else begin
      tmr_ovf_tima_r <= 0;
    end

    // interrupt flags
    if (CLK_CPU_EDGE) begin
      // once we have halted instruction fetch then time has stopped and we need to avoid recording new interrupts
      if (~HLT_IFD_rsp) begin
        REG_IF_r[`IE_VBLANK]   <= (REG_IF_r[`IE_VBLANK]   | PPU_REG_vblank               | (reg_int_write_r & reg_int_write_data_r[`IE_VBLANK])  ) & ~(IFD_REG_ic[`IE_VBLANK]   | (reg_int_write_r & ~reg_int_write_data_r[`IE_VBLANK])  );
        REG_IF_r[`IE_LCD_STAT] <= (REG_IF_r[`IE_LCD_STAT] | PPU_REG_lcd_stat             | (reg_int_write_r & reg_int_write_data_r[`IE_LCD_STAT])) & ~(IFD_REG_ic[`IE_LCD_STAT] | (reg_int_write_r & ~reg_int_write_data_r[`IE_LCD_STAT]));
        REG_IF_r[`IE_TIMER]    <= (REG_IF_r[`IE_TIMER]    | tmr_ovf_tima_r               | (reg_int_write_r & reg_int_write_data_r[`IE_TIMER])   ) & ~(IFD_REG_ic[`IE_TIMER]    | (reg_int_write_r & ~reg_int_write_data_r[`IE_TIMER])   );
        REG_IF_r[`IE_SERIAL]   <= (REG_IF_r[`IE_SERIAL]   | SER_REG_done                 | (reg_int_write_r & reg_int_write_data_r[`IE_SERIAL])  ) & ~(IFD_REG_ic[`IE_SERIAL]   | (reg_int_write_r & ~reg_int_write_data_r[`IE_SERIAL])  );
        REG_IF_r[`IE_JOYPAD]   <= (REG_IF_r[`IE_JOYPAD]   | |(REG_P1_r[3:0] & ~P1I[3:0]) | (reg_int_write_r & reg_int_write_data_r[`IE_JOYPAD])  ) & ~(IFD_REG_ic[`IE_JOYPAD]   | (reg_int_write_r & ~reg_int_write_data_r[`IE_JOYPAD])  );
      end
      else if (~HLT_REQ) begin
        // clear any outstanding VBLANK so we don't take 2 of them in KDL2
        REG_IF_r[`IE_VBLANK] <= 0;
      end
    end

    if (CLK_CPU_EDGE) REG_P1_r[3:0] <= P1I[3:0];

    if (CLK_BUS_EDGE) reg_dma_start_r <= 0;
    if (CLK_CPU_EDGE) reg_int_write_r <= 0;

    // ---- CGB speed switch -----------------------------------------------
    // See the declarations of spd_*_r for the reference and the numbers.
    //
    // spd_stop_r marks the STOP sitting in EXE as the switch for as long as
    // it sits there.  It is what keeps the plain-STOP park term in exe_stall
    // from re-parking the CPU on that same instruction once KEY1.0 has been
    // cleared underneath it (the clear lands on the same edge the STOP enters
    // EXE), and it is released when the NEXT instruction is handed over --
    // not on the falling edge of the STOP decode, which two consecutive STOPs
    // would never produce.
    if (IFD_EXE_new & ~((IFD_EXE_op[7:0] == 8'h10) & ~IFD_EXE_cb)) spd_stop_r <= 1'b0;

    if (CLK_BUS_EDGE) begin
      spd_wake_r <= 1'b0;
      spd_tail_r <= 1'b0;
    end

    if (IFD_stop_switch) begin
      // The handoff edge of the switching STOP: gambatte's `cc`.
      spd_stop_r    <= 1'b1;
      REG_KEY1_r[0] <= 1'b0;
      REG_DIV_r     <= 16'h0000;
      spd_stall_r   <= ~|(REG_IE_r[4:0] & REG_IF_r[4:0]);
      spd_ctr_r     <= SPEED_SWITCH_MCYC;
      if (cpu_speed_r) begin
        cpu_speed_r  <= 1'b0;           // 2x -> 1x: the clock flips on this edge
      end
      else begin
        spd_go_r     <= 1'b1;           // 1x -> 2x: two bus edges later
        spd_go_ctr_r <= 1'b1;
      end
    end
    else begin
      if (spd_go_r & CLK_BUS_EDGE) begin
        spd_go_ctr_r <= 1'b0;
        if (~spd_go_ctr_r) begin
          spd_go_r    <= 1'b0;
          cpu_speed_r <= 1'b1;
        end
      end
      if (spd_stall_r) begin
        if (|(REG_IE_r[4:0] & REG_IF_r[4:0])) begin
          spd_stall_r <= 1'b0;
          spd_wake_r  <= 1'b1;          // one more edge: the CGB's wake-up cost
        end
        else if (CLK_BUS_EDGE) begin
          spd_ctr_r <= spd_ctr_r - 16'd1;
          if (spd_ctr_r == 16'd1) begin
            spd_stall_r <= 1'b0;
            spd_tail_r  <= 1'b1;        // the STOP retires on the next edge
          end
        end
      end
    end

    case (reg_state_r)
      ST_REG_IDLE: begin
        if      (MCT_REG_req_val) begin
          reg_src_r     <= 0;
          reg_addr_r    <= MCT_REG_address;
          reg_wr_r      <= MCT_REG_wren;
          reg_wr_data_r <= MCT_REG_data;

          reg_state_r <= ST_REG_REQ;
        end
        else if (DBG_REG_req_val) begin
          reg_src_r     <= 1;
          reg_addr_r    <= DBG_REG_address;
          reg_wr_r      <= DBG_REG_wren;
          reg_wr_data_r <= DBG_REG_data;

          reg_state_r <= ST_REG_REQ;
        end
      end
      ST_REG_REQ: begin
        case (reg_addr_r)
          8'h00: begin reg_mdr_r[7:0] <= {2'b11, REG_P1_r[5:0]}; if (reg_wr_r) REG_P1_r[5:4]   <= reg_wr_data_r[5:4];   end
          8'h01: reg_mdr_r[7:0] <= REG_SB_r;
          8'h02: reg_mdr_r[7:0] <= {REG_SC_r[7],6'h3F,REG_SC_r[0]};

          8'h04: begin reg_mdr_r[7:0] <= REG_DIV_r[15:8];         if (reg_wr_r) REG_DIV_r[15:0] <= 0;               end
          8'h05: begin reg_mdr_r[7:0] <= REG_TIMA_r;              if (reg_wr_r) REG_TIMA_r[7:0] <= reg_wr_data_r[7:0];  end
          8'h06: begin reg_mdr_r[7:0] <= REG_TMA_r;               if (reg_wr_r) REG_TMA_r[7:0]  <= reg_wr_data_r[7:0];  end
          8'h07: begin reg_mdr_r[7:0] <= {5'h1F,REG_TAC_r[2:0]};  if (reg_wr_r) REG_TAC_r[2:0]  <= reg_wr_data_r[2:0];  end

          8'h0F: begin reg_mdr_r[7:0] <= REG_IF_r;                if (reg_wr_r) begin reg_int_write_data_r <= reg_wr_data_r[7:0]; reg_int_write_r <= 1; end end

          // APU registers read here for MMIO accesses and written in APU
          8'h10: reg_mdr_r[7:0] <= {1'h1,REG_NR10_r[6:0]};
          8'h11: reg_mdr_r[7:0] <= {REG_NR11_r[7:6],6'h3F};
          8'h12: reg_mdr_r[7:0] <= REG_NR12_r[7:0];
          8'h13: reg_mdr_r[7:0] <= 8'hFF;
          8'h14: reg_mdr_r[7:0] <= {1'h1,REG_NR14_r[6],6'h3F};

          8'h16: reg_mdr_r[7:0] <= {REG_NR21_r[7:6],6'h3F};
          8'h17: reg_mdr_r[7:0] <= REG_NR22_r[7:0];
          8'h18: reg_mdr_r[7:0] <= 8'hFF;
          8'h19: reg_mdr_r[7:0] <= {1'h1,REG_NR24_r[6],6'h3F};

          8'h1A: reg_mdr_r[7:0] <= {REG_NR30_r[7:7],7'h7F};
          8'h1B: reg_mdr_r[7:0] <= 8'hFF;
          8'h1C: reg_mdr_r[7:0] <= {1'h1,REG_NR32_r[6:5],5'h1F};
          8'h1D: reg_mdr_r[7:0] <= 8'hFF;
          8'h1E: reg_mdr_r[7:0] <= {1'h1,REG_NR34_r[6],6'h3F};

          8'h20: reg_mdr_r[7:0] <= 8'hFF;
          8'h21: reg_mdr_r[7:0] <= REG_NR42_r;
          8'h22: reg_mdr_r[7:0] <= REG_NR43_r;
          8'h23: reg_mdr_r[7:0] <= {1'h1,REG_NR44_r[6],6'h3F};

          8'h24: reg_mdr_r[7:0] <= REG_NR50_r;
          8'h25: reg_mdr_r[7:0] <= REG_NR51_r;
          8'h26: reg_mdr_r[7:0] <= {REG_NR52_r[7:7],3'h7,({4{REG_NR52_r[7]}} & {APU_REG_enable})};

          8'h30: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h31: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h32: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h33: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h34: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h35: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h36: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h37: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h38: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h39: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h3A: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h3B: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h3C: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h3D: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h3E: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];
          8'h3F: reg_mdr_r[7:0] <= (APU_REG_enable[2] & ~reg_src_r) ? 8'hFF : REG_WAV_r[reg_addr_r[3:0]][7:0];

          8'h40: begin reg_mdr_r[7:0] <= REG_LCDC_r;                    if (reg_wr_r) REG_LCDC_r[7:0] <= reg_wr_data_r[7:0]; end
          8'h41: begin reg_mdr_r[7:0] <= {1'b1,REG_STAT_r[6:0]};        if (reg_wr_r) REG_STAT_r[7:3] <= reg_wr_data_r[7:3]; end
          8'h42: begin reg_mdr_r[7:0] <= REG_SCY_r;                     if (reg_wr_r) REG_SCY_r[7:0]  <= reg_wr_data_r[7:0]; end
          8'h43: begin reg_mdr_r[7:0] <= REG_SCX_r;                     if (reg_wr_r) REG_SCX_r[7:0]  <= reg_wr_data_r[7:0]; end
          8'h44: reg_mdr_r[7:0] <= PPU_LY_read;
          8'h45: begin reg_mdr_r[7:0] <= REG_LYC_r;                     if (reg_wr_r) REG_LYC_r[7:0]  <= reg_wr_data_r[7:0]; end
          8'h46: begin reg_mdr_r[7:0] <= REG_DMA_r;                     if (reg_wr_r) begin REG_DMA_r[7:0]  <= reg_wr_data_r[7:0]; reg_dma_start_r <= ~HLT_RSP; end end // don't trigger DMA on HLT
          8'h47: begin reg_mdr_r[7:0] <= REG_BGP_r;                     if (reg_wr_r) REG_BGP_r[7:0]  <= reg_wr_data_r[7:0]; end
          8'h48: begin reg_mdr_r[7:0] <= REG_OBP0_r;                    if (reg_wr_r) REG_OBP0_r[7:0] <= reg_wr_data_r[7:0]; end
          8'h49: begin reg_mdr_r[7:0] <= REG_OBP1_r;                    if (reg_wr_r) REG_OBP1_r[7:0] <= reg_wr_data_r[7:0]; end
          8'h4A: begin reg_mdr_r[7:0] <= REG_WY_r;                      if (reg_wr_r) REG_WY_r[7:0]   <= reg_wr_data_r[7:0]; end
          8'h4B: begin reg_mdr_r[7:0] <= REG_WX_r;                      if (reg_wr_r) REG_WX_r[7:0]   <= reg_wr_data_r[7:0]; end

          // CGB registers.  See the comment on the declarations: Pan Docs
          // "CGB Registers" plus SameBoy Core/memory.c for the access gates.
          //
          // KEY0 is writable only while the boot ROM is mapped and freezes
          // when $FF50 unmaps it, which is what makes DMG compatibility a
          // decision the boot ROM alone gets to make.  SameBoy returns $FF on
          // a KEY0 read; this core returns the latched value, which is what
          // the phase 1 contract asks for and what the bridge publishes.
          8'h4C: begin reg_mdr_r[7:0] <= REG_KEY0_r;                    if (reg_wr_r & BOOTROM_ACTIVE) REG_KEY0_r <= reg_wr_data_r; end
          // KEY1: bit 7 is the current speed (cpu_speed_r, toggled by the
          // switch above), bit 0 is the arming bit, bits 6:1 read as 1.
          // $FF in DMG compatibility mode (SameBoy memory.c:714-718).
          8'h4D: begin reg_mdr_r[7:0] <= cgb_mode ? {cpu_speed_r,6'h3F,REG_KEY1_r[0]} : 8'hFF;
                                                                        if (reg_wr_r & cgb_mode) REG_KEY1_r <= reg_wr_data_r; end
          8'h4F: begin reg_mdr_r[7:0] <= {7'h7F,REG_VBK_r};             if (reg_wr_r & cgb_mode) REG_VBK_r <= reg_wr_data_r[0]; end

          8'h50: begin reg_mdr_r[7:0] <= {7'h7F,REG_BOOT_r[`BOOT_ROM_DI]}; if (reg_wr_r & reg_wr_data_r[0]) REG_BOOT_r[`BOOT_ROM_DI] <= 1'b1; end

          // HDMA1-4 are write only and land in the HDMA engine below; only
          // HDMA5 reads back.  $FF means "no transfer running".
          8'h55: reg_mdr_r[7:0] <= cgb_mode ? {~hdma_running,hdma_left} : 8'hFF;

          // RP: no IR hardware.  The three bits that exist are latched so a
          // read-back looks like a CGB with nothing in front of the LED
          // (SameBoy stores the value too); bit 1 always reads 1, "no signal".
          8'h56: begin reg_mdr_r[7:0] <= cgb_mode ? {REG_RP_r[2:1],5'h1F,REG_RP_r[0]} : 8'hFF;
                                                                        if (reg_wr_r) REG_RP_r <= {reg_wr_data_r[7:6],reg_wr_data_r[0]}; end

          8'h68: begin reg_mdr_r[7:0] <= {REG_BCPS_ai_r,1'b1,REG_BCPS_r};
                                                                        if (reg_wr_r) begin REG_BCPS_ai_r <= reg_wr_data_r[7]; REG_BCPS_r <= reg_wr_data_r[5:0]; end end
          // The RAM write itself is cram_we_a above.  The auto-increment
          // happens even when the write was dropped because CRAM is blocked
          // (SameBoy memory.c: the blocked branch still bumps the index).
          8'h69: begin reg_mdr_r[7:0] <= (cram_cpu_ok & ~cram_blocked) ? cram_q_a_r : 8'hFF;
                                                                        if (reg_wr_r & cram_cpu_ok & REG_BCPS_ai_r) REG_BCPS_r <= REG_BCPS_r + 6'd1; end
          8'h6A: begin reg_mdr_r[7:0] <= {REG_OCPS_ai_r,1'b1,REG_OCPS_r};
                                                                        if (reg_wr_r) begin REG_OCPS_ai_r <= reg_wr_data_r[7]; REG_OCPS_r <= reg_wr_data_r[5:0]; end end
          8'h6B: begin reg_mdr_r[7:0] <= (cram_cpu_ok & ~cram_blocked) ? cram_q_a_r : 8'hFF;
                                                                        if (reg_wr_r & cram_cpu_ok & REG_OCPS_ai_r) REG_OCPS_r <= REG_OCPS_r + 6'd1; end
          // OPRI: writable from the boot ROM (which sets it in compatibility
          // mode) and in CGB mode, frozen for a DMG game afterwards.
          8'h6C: begin reg_mdr_r[7:0] <= {7'h7F,REG_OPRI_r};            if (reg_wr_r & (BOOTROM_ACTIVE | REG_KEY0_r[3] | cgb_mode)) REG_OPRI_r <= reg_wr_data_r[0]; end

          8'h70: begin reg_mdr_r[7:0] <= cgb_mode ? {5'h1F,REG_SVBK_r} : 8'hFF;
                                                                        if (reg_wr_r & (cgb_mode | BOOTROM_ACTIVE)) REG_SVBK_r <= reg_wr_data_r[2:0]; end

          // Pan Docs "Undocumented registers".  No hardware behind them; the
          // CGB boot ROM and a couple of test ROMs do read them back.
          8'h72: begin reg_mdr_r[7:0] <= REG_PSWX_r;                    if (reg_wr_r) REG_PSWX_r <= reg_wr_data_r; end
          8'h73: begin reg_mdr_r[7:0] <= REG_PSWY_r;                    if (reg_wr_r) REG_PSWY_r <= reg_wr_data_r; end
          8'h74: begin reg_mdr_r[7:0] <= cgb_mode ? REG_PSW_r : 8'hFF;  if (reg_wr_r) REG_PSW_r  <= reg_wr_data_r; end
          8'h75: begin reg_mdr_r[7:0] <= {1'b1,REG_PGB_r,4'hF};         if (reg_wr_r) REG_PGB_r  <= reg_wr_data_r[6:4]; end

          // PCM12/PCM34 are the APU's live sample taps.  Nothing on the SNES
          // side reads them and the APU here does not export the per-channel
          // samples, so they read 0 rather than an invented value.
          8'h76: reg_mdr_r[7:0] <= 8'h00;
          8'h77: reg_mdr_r[7:0] <= 8'h00;

`ifdef SGB_SAVE_STATES
          // special case debug source reads to read out arch state that isn't normally memory mapped

          // ARCH state
          8'h60: if (reg_src_r) reg_mdr_r <= A_r;
          8'h61: if (reg_src_r) reg_mdr_r <= F_r[7:0];
          8'h62: if (reg_src_r) reg_mdr_r <= B_r;
          8'h63: if (reg_src_r) reg_mdr_r <= C_r;
          8'h64: if (reg_src_r) reg_mdr_r <= D_r;
          8'h65: if (reg_src_r) reg_mdr_r <= E_r;
          8'h66: if (reg_src_r) reg_mdr_r <= H_r;
          8'h67: if (reg_src_r) reg_mdr_r <= L_r;
          8'h68: if (reg_src_r) reg_mdr_r <= SP_r[7:0];
          8'h69: if (reg_src_r) reg_mdr_r <= SP_r[15:8];
          8'h6A: if (reg_src_r) reg_mdr_r <= PC_r[7:0];
          8'h6B: if (reg_src_r) reg_mdr_r <= PC_r[15:8];
          8'h6C: if (reg_src_r) reg_mdr_r <= EXE_REG_ime;

          // MBC
          8'h70: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
          8'h71: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
          8'h72: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
          8'h73: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
          8'h74: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
          8'h75: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
          8'h76: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
          8'h77: if (reg_src_r) reg_mdr_r <= MBC_REG_DATA;
`endif

          8'hFF: begin reg_mdr_r[7:0] <= {3'h0,REG_IE_r[4:0]};          if (reg_wr_r) REG_IE_r[4:0]   <= reg_wr_data_r[4:0]; end
          default: reg_mdr_r <= 8'hFF;
        endcase

        reg_state_r <= ST_REG_END;
      end
      ST_REG_END: begin
        reg_state_r <= ST_REG_IDLE;
      end
    endcase
  end
end

//-------------------------------------------------------------------
// IFD
//-------------------------------------------------------------------

// IFD performs Instruction Fetch and Decode operations in one or more
// bus cycles.  The number of bytes fetched is based on the decoded
// operation in a prior cycle.

// Local
reg [7:0]   ifd_op_r;
reg [1:0]   ifd_size_r;
reg [7:0]   ifd_data_r;
reg [15:0]  ifd_decode_r;
reg         ifd_req_r;
// The HALT bug, decided in the EXE section next to the HALT park: the step
// that hands over the opcode after a HALT that never halted.
wire        ifd_halt_bug;   // IME=0: that opcode is read again -- PC does not advance
wire        ifd_halt_ret;   // IME=1: the interrupt taken there returns to the HALT
reg         ifd_complete_r;
// STOP is a TWO-byte opcode unless an interrupt is already pending (IE & IF)
// when it starts: the byte after it is then consumed and never executed
// (SameBoy Core/sm83_cpu.c stop(): `if (!interrupt_pending) cycle_read(gb,
// gb->pc++)`, and the comment above it: "When entering with IF&IE, the 2nd
// byte of STOP is actually executed"; Pan Docs, "Using the STOP instruction":
// 2-byte opcode unless an interrupt is pending).  Nearly every ROM writes
// `10 00`, where executing the $00 as a NOP and skipping it are the same
// thing -- which is why a 1-byte STOP passed every test ROM.  Yu-Gi-Oh! Duel
// Monsters 4 does its speed switch with `10 06 / pop hl / pop af / ret`:
// executing the $06 as `ld b,$E1` eats the POP HL and the RET lands in the
// middle of an RST $08 far call -- the game never leaves its boot code.
//
// The skipped byte still costs its M-cycle (SameBoy's cycle_read), and the
// cheapest exact way to spend it is to let the IFD fetch it as it always did
// and hand it to the EXE as $00: every `10 00` in the corpus keeps its
// timing to the M-cycle, and only the byte's MEANING changes.  Sampled on the
// STOP's own handoff edge, the same instant spd_stall_r samples IE & IF.
reg         ifd_stop_skip_r;
// The IFD does not START an instruction while an HDMA/GDMA request is pending
// or running -- but it finishes one it has already started.
//
// exe_stall keeps the EXE from moving past a COMPLETED instruction; it cannot
// stop one that is handed over in the middle of the copy.  That is what used to
// happen when the request matured with the EXE empty: the block started at
// once, the IFD went on assembling, and the EXE ran the instruction during the
// copy with the HDMA on the system bus, so its memory accesses were swallowed
// (Shantae: a `call` whose two pushes never reached WRAM; the matching `ret`
// popped a stale address and the game ended up in RST $38 for ever).
//
// The hardware services the DMA BETWEEN two instructions.  So:
//   * ifd_size_r == 0 -- nothing of the next instruction has been consumed yet:
//     this IS the boundary.  The IFD holds still for the whole stall and the
//     block starts (hdma_bound_edge).
//   * ifd_size_r != 0 -- the opcode is already consumed, the instruction has
//     started: the IFD keeps assembling it, the EXE runs it to completion, and
//     only then does the block start (hdma_bound_edge waits for it).  Its
//     effects therefore land before the copy, as they do on the hardware
//     (gambatte hdma_late_destl_1: the HDMA4 write still moves this block).
wire        ifd_step = CLK_BUS_EDGE & EXE_IFD_ready & ~(hdma_cpu_stall & ~|ifd_size_r);
reg         ifd_cb_r;
reg         ifd_int_r;
reg [2:0]   ifd_int_tgt_r;
reg [7:0]   ifd_int_ic_r;

// Outputs
reg         ifd_exe_valid_r;
reg [23:0]  ifd_exe_op_r;
reg [15:0]  ifd_exe_decode_r;
reg [15:0]  ifd_exe_pc_start_r;
reg [15:0]  ifd_exe_pc_end_r;
reg [15:0]  ifd_exe_pc_next_r;
reg         ifd_exe_cb_r;
reg         ifd_exe_new_r;
reg         ifd_exe_int_r;
reg [7:0]   ifd_reg_ic_r;

// decoder
wire [7:0]  dec_addr = ifd_op_r;
wire [15:0] dec_data;

// PC with bypass
wire [15:0] ifd_pc = EXE_IFD_redirect ? EXE_IFD_target : PC_r;

`ifdef MK2
dec_table dec (
  .clka(CLK),       // input clka
  .addra(dec_addr), // input [7 : 0] addra
  .douta(dec_data)  // output [15 : 0] douta
);
`endif
`ifdef MK3
dec_table dec (
  .clock(CLK),        // input clock
  .address(dec_addr), // input [7 : 0] address
  .q(dec_data)        // output [15 : 0] q
);
`endif

assign IFD_MCT_req_val = ifd_req_r;
assign IFD_MCT_req_addr_d1 = ifd_pc;

// The CGB speed switch is decided on the handoff edge of the STOP itself: the
// instruction that completes here is a STOP (handed over on its opcode byte,
// not an interrupt), KEY1 bit 0 is armed, no button is held (P1[3:0] reads F)
// and the machine is in CGB mode.  That edge is the `cc` gambatte's
// Memory::stop() reasons from; the REG block resets DIV and flips (or
// schedules) the clock on it.  The fetch this same edge issues is the
// hardware's prefetch of the byte after the STOP (age spsw-stop-prefetch): it
// is not repeated during the park, and unless an interrupt or an HDMA block
// is pending it is the STOP's second byte (ifd_stop_skip_r below).
//
// Only the switch resets DIV here.  A plain STOP (KEY1 bit 0 clear) does not,
// and it also leaves the timers and the LCD running while it waits for a
// button: SameBoy's enter_stop_mode() zeroes DIV and freezes both.  A known
// deviation, not touched by the second-byte rule.
//
// It is qualified by ifd_step and not by the bare bus edge, so it waits with
// the handoff while an HDMA request is pending (ifd_size_r is 0 here): the
// STOP is not handed to the EXE during the stall, and deciding the switch
// there would reset DIV for an instruction that has not been dispatched and
// decide it a second time when the stall lets go.
assign IFD_stop_switch = ifd_step & ~HLT_IFD_rsp & ifd_complete_r & ~ifd_int_r & ~|ifd_size_r
                       & (ifd_op_r == 8'h10) & REG_KEY1_r[0] & cgb_mode & &REG_P1_r[3:0];

assign IFD_EXE_valid = ifd_exe_valid_r;
assign IFD_EXE_decode = ifd_exe_decode_r;
assign IFD_EXE_op = ifd_exe_op_r;

assign IFD_EXE_pc_start = ifd_exe_pc_start_r;
assign IFD_EXE_pc_end = ifd_exe_pc_end_r;
assign IFD_EXE_pc_next = ifd_exe_pc_next_r;

assign IFD_EXE_cb = ifd_exe_cb_r;
assign IFD_EXE_new = ifd_exe_new_r;
assign IFD_EXE_int = ifd_exe_int_r;

assign IFD_REG_ic = ifd_reg_ic_r;

// idle when:
// - at instruction boundary
// - not taking an interrupt
// - no in-progress ICD transfers
assign HLT_IFD_rsp = HLT_REQ_sync & ~|ifd_size_r & ~ifd_int_r & IDL_ICD;

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    PC_r <= 0;

    ifd_exe_valid_r <= 0;
    ifd_size_r <= 0;
    ifd_req_r <= 1; // generate the initial request out of reset
    ifd_exe_new_r <= 0;

    ifd_int_ic_r <= 0;
  end
  else begin
    ifd_exe_new_r <= 0;

    if (CLK_CPU_EDGE) ifd_reg_ic_r <= 0;

    if (ifd_step) begin
      if (~HLT_IFD_rsp) begin
        PC_r <= ifd_pc + {15'd0, ~ifd_halt_bug};

        // Flop pipeline registers
        ifd_exe_valid_r <= ifd_complete_r;
        ifd_exe_new_r   <= ifd_complete_r;
        ifd_exe_int_r   <= ifd_complete_r & ifd_int_r;

        // Adjust current instrucion size
        ifd_size_r      <= ifd_complete_r ? 0 : ifd_size_r + 1;

        if (ifd_complete_r & ifd_int_r) ifd_reg_ic_r <= ifd_int_ic_r;
      end
      else begin
        // force bypassed PC to be accounted for in state
        PC_r <= ifd_pc;

        ifd_exe_valid_r <= 0;
        ifd_exe_new_r <= 0;
        ifd_exe_int_r <= 0;
      end
    end

    // The instruction fetch is otherwise unconditional -- the IFD re-reads the
    // byte at PC every bus cycle until EXE_IFD_ready lets it move on, and THAT
    // is what keeps ifd_data_r/ifd_op_r valid across a stall.  A HALT works for
    // exactly this reason.
    //
    // While the HDMA owns the system bus the fetch has to stop, because a
    // request issued now would be swallowed by the HDMA's SYS_REQ and the
    // memory controller would complete it with the HDMA's byte.  It stops for
    // the BUS, though, not for the whole stall: hdma_bus_busy drops between
    // blocks (ST_HDMA_BLKE), which is where the IFD refreshes ifd_data_r, and
    // that state is held long enough for the refresh to finish.
    //
    // Suppressing it for the whole stall instead is a trap that looks correct
    // and is not: the fetch of the opcode AFTER the store to $FF55 has usually
    // not happened yet when the store completes (the EXE owns the controller
    // that cycle), so ifd_data_r still holds the store's OPERAND, and the CPU
    // resumes by executing that byte as an instruction.
    //
    // The speed switch is the one stall where the fetch IS frozen, and that
    // is the hardware's prefetch: the opcode after STOP is read on STOP's
    // FIRST M-cycle and not again -- not on its second, not when the CPU
    // wakes (age-test-roms speed-switch/spsw-stop-prefetch runs STOP out of
    // $FF04 with TIMA as the next byte, once with a timer that ticks during
    // the park and once with one that ticks one M-cycle after STOP starts,
    // and expects the value TIMA had on STOP's first M-cycle both times).
    // The fetch issued on the handoff edge is that read (IFD_stop_switch is
    // that edge; spd_stall_r rises on it and suppresses the fetches of the
    // park).  Safe here because STOP has no memory operand: ifd_data_r/
    // ifd_op_r hold that opcode for the whole park and the retire edge hands
    // it over before the fetch that edge issues can answer.
    // With no interrupt pending that byte is the STOP's second byte and is
    // handed over as $00 (ifd_stop_skip_r): the fetch and its M-cycle stay,
    // what it executes does not.
    ifd_req_r <= CLK_BUS_EDGE & ~hdma_bus_busy & ~spd_stall_r;
  end

  if (ifd_step) begin
    case (ifd_size_r)
      0: ifd_exe_op_r[7:0]   <= ifd_int_r ? {2'h3,ifd_int_tgt_r,3'h7} : ifd_data_r;
      1: ifd_exe_op_r[15:8]  <=                                         ifd_data_r;
      2: ifd_exe_op_r[23:16] <=                                         ifd_data_r;
    endcase

    ifd_exe_decode_r <= ifd_decode_r;

    if (ifd_size_r == 0) ifd_exe_pc_start_r <= ifd_pc;
    ifd_exe_pc_end_r  <= ifd_pc;
    // The address an instruction ends at, which is what a CALL/RST/interrupt
    // pushes and what JR adds to.  Behind the HALT bug the opcode was read
    // without moving PC, so the instruction ends one byte earlier (an RST
    // there returns to itself, SameBoy sm83_cpu.c's pc-- after the fetch);
    // an interrupt taken on a HALT that never halted pushes the HALT itself
    // (halt(): "if (gb->ime) ... gb->pc--"), so it runs again after the RETI.
    // An interrupt dispatched in place of the byte a STOP skipped returns past
    // it (ifd_stop_skip_r).  The two interrupt terms never meet: halt_ret needs
    // the HALT in EXE at this edge, the skip needs the STOP there.
    ifd_exe_pc_next_r <= ifd_pc + (ifd_int_r ? (ifd_halt_ret ? 16'hFFFF : {15'd0, ifd_stop_skip_r})
                                             : {15'd0, ~ifd_halt_bug});
    ifd_exe_cb_r      <= ifd_cb_r;
  end

  // The byte after a skipping STOP is the only one fetched with size 0 while
  // ifd_stop_skip_r is set (PC does not move until the next handoff).
  if (MCT_IFD_rsp_val) begin
    ifd_data_r <= (ifd_stop_skip_r & ~|ifd_size_r) ? 8'h00 : MCT_data;
    if (ifd_size_r == 0) ifd_op_r <= ifd_stop_skip_r ? 8'h00 : MCT_data;
  end

  // Interrupts
  // this doesn't have to be the first base cycle since the instruction is contructed from constants and register state
  ifd_int_r <= EXE_IFD_ime & |(REG_IE_r & REG_IF_r) & ~|ifd_size_r;
  ifd_int_tgt_r <= (  (REG_IE_r[`IE_VBLANK]   & REG_IF_r[`IE_VBLANK]  ) ? 3'h0
                   :  (REG_IE_r[`IE_LCD_STAT] & REG_IF_r[`IE_LCD_STAT]) ? 3'h1
                   :  (REG_IE_r[`IE_TIMER]    & REG_IF_r[`IE_TIMER])    ? 3'h2
                   :  (REG_IE_r[`IE_SERIAL]   & REG_IF_r[`IE_SERIAL])   ? 3'h3
                   :  (REG_IE_r[`IE_JOYPAD]   & REG_IF_r[`IE_JOYPAD])   ? 3'h4
                   :                                                      3'h7
                   );

  if      (REG_IE_r[`IE_VBLANK]   & REG_IF_r[`IE_VBLANK]  ) ifd_int_ic_r <= 8'b00000001;
  else if (REG_IE_r[`IE_LCD_STAT] & REG_IF_r[`IE_LCD_STAT]) ifd_int_ic_r <= 8'b00000010;
  else if (REG_IE_r[`IE_TIMER]    & REG_IF_r[`IE_TIMER]   ) ifd_int_ic_r <= 8'b00000100;
  else if (REG_IE_r[`IE_SERIAL]   & REG_IF_r[`IE_SERIAL]  ) ifd_int_ic_r <= 8'b00001000;
  else if (REG_IE_r[`IE_JOYPAD]   & REG_IF_r[`IE_JOYPAD]  ) ifd_int_ic_r <= 8'b00010000;
  else                                                      ifd_int_ic_r <= 8'b00000000;

  ifd_complete_r <= (ifd_decode_r[`DEC_SZE] == ifd_size_r);

  ifd_cb_r <= ifd_op_r == 8'hCB;

  // SZE2, LAT2, DST4, SRC4, GRP4
  ifd_decode_r <= ( ifd_int_r ? {2'h0,2'h0,`OPR_SP,`OPR_PC,`GRP_CLL}
                  : ifd_cb_r  ? {2'h1,{(ifd_data_r[2:0] == 3'h6 ? 1'b1 : 1'b0),1'b0},{1'b1,ifd_data_r[2:0]},{1'b1,ifd_data_r[2:0]},({{(ifd_data_r[2:0] == 3'h6 ? 1'b1 : 1'b0),3'h0} | `GRP_BIT})}
                  :             dec_data
                  );

  // debug/state writes
  if (REG_req_dbg) begin
    case (REG_address)
      8'h6A: PC_r[7:0]  <= REG_req_data;
      8'h6B: PC_r[15:8] <= REG_req_data;
    endcase
  end

  // Set on the STOP's handoff, cleared on the next one (the NOP that stands in
  // for the skipped byte, or an interrupt dispatched in its place).  A debug
  // PC write moves the fetch elsewhere, so it drops a pending skip too.
  //
  // The one exception the hardware adds: an HDMA block requested DURING the
  // STOP's own fetch M-cycle is serviced in the M-cycle that would have
  // skipped the second byte, and that byte then executes as an opcode.
  // gambatte's hardware tests (CGB CPU C) pin both sides of the edge with the
  // same ROM one M-cycle apart: hdma_late_speedchange_inc_scx1_ds_1/_3 and
  // hdma_late_m3speedchange_inc_scx1_1/_3 (request one M-cycle after the
  // STOP's handoff: INC A skipped, out01) against _2 of both (request inside
  // the STOP's fetch: INC A executed, out02); the ldaaimm and 7fffstop tests
  // of hdma_transition_speedchange_* are the same edge.  SameBoy does not
  // model it (it skips in all nine); the rule below closes all nine.
  if (cpu_ireset_r) ifd_stop_skip_r <= 1'b0;
  else if (ifd_step & ~HLT_IFD_rsp & ifd_complete_r)
    ifd_stop_skip_r <= ~ifd_int_r & ~|ifd_size_r & (ifd_op_r == 8'h10)
                     & ~|(REG_IE_r[4:0] & REG_IF_r[4:0]) & ~hdma_req_pending;
  else if (REG_req_dbg & ((REG_address == 8'h6A) | (REG_address == 8'h6B)))
    ifd_stop_skip_r <= 1'b0;
end

//-------------------------------------------------------------------
// EXE
//-------------------------------------------------------------------

// EXE implements the execution component of the CPU.
//
// Overall instruction latency is defined as the following:
//
// OP         [operand fetch-1 + execution/memory time + writeback]
//
// For example:
// LD R,R     [0 + 0 + 1 = 1]
// LD R,n     [1 + 0 + 1 = 2]
// LD (HL),n  [1 + 1 + 1 = 3]
// RET CC     [0 + 1 + 1 = 2] // not taken
// RET CC     [0 + 4 + 1 = 5] // taken
//
// IFD handles operand fetch.  EXE is responsible for performing any
// data bus operations and writing back to register state.  Since
// the next opcode bus fetch is pipelined wrt Writeback,
// WB cannot peform any bus operations.
//
// This multi-cycle operation is composed of 4 distinct stages which can
// cover 1-5 cycles of latency in addition to the 1-2 cycles of operand fetch
// in IFD.
//
// Sequencing:
//  0*  3   2   1   0   // stage numbers
//                  WB  // LD R,R  LD R,n
//              LD  WB
//              ST  WB
//          LD  LD  WB
//          ST  ST  WB
//          LD  ST  WB
//  CC                  // JR CC not taken
//  CC      LD  LD  WB
//  CC      ST  ST  WB
//  CC  --  LD  LD  WB  // RET CC taken
//
// 0/0* - WB/CC is always stage 0*/0. They are effectively the same stage.
//        0* indicates that certain condition code instructions will evaluate and
//        possibly extend the execution time 1-4 additional clocks.
// 2/1  - Up to 2 memory data bus operation will be performed in the format of
//        LD, ST, LD-LD, ST-ST, or LD-ST.  These always occur in stage 2 and 1.
// 3    - This serves as an optional delay stage.  No arch state is modified.

// Local
reg         exe_advance_r;
reg         exe_ready_r;
reg         exe_complete_r;

reg [2:0]   exe_ctr_r;
reg [2:0]   exe_lat_add_r;

reg [15:0]  exe_src_r;
reg [15:0]  exe_dst_r;
reg [7:0]   exe_cc_r;
reg [7:0]   exe_src_alu_r;

reg [15:0]  exe_res_r;
reg [7:0]   exe_res_cc_r;
reg [7:0]   exe_res_los_r;
reg [7:0]   exe_res_cp_r;
// address arithmetic
reg         exe_res_hl_mod_r;
reg [15:0]  exe_res_hl_r;
reg         exe_res_sp_mod_r;
reg [15:0]  exe_res_sp_r;
// alu
reg         exe_res_c15_r;
reg         exe_res_c11_r;
reg         exe_res_c7_r;
reg         exe_res_c3_r;

reg         exe_res_int_enable_r;
reg         exe_res_int_disable_r;

reg         exe_res_halt_r;
// exe_res_stop_r is declared with the CGB registers above (the REG block needs
// it for the KEY1 speed switch).

reg         exe_mem_req_r;
reg [15:0]  exe_mem_data_r;
reg [15:0]  exe_mem_addr_mod_r;

reg         exe_ime_r;

wire        exe_loadopstore = (IFD_EXE_decode[`DEC_GRP] == `GRP_MBT || IFD_EXE_decode[`DEC_GRP] == `GRP_MIC || IFD_EXE_decode[`DEC_GRP] == `GRP_MDC);
// use condition codes directly since the flopped version causes problems with evaluating redirect
wire        exe_redirect_taken = ~(IFD_EXE_op[3] ^ (IFD_EXE_op[4] ? F_r[`FLAG_C] : F_r[`FLAG_Z]));
// hdma_cpu_stall is the third reason the pipeline can be frozen, next to HALT
// and STOP: GDMA/HDMA park the CPU while a block is copied.  It stops the
// EXECUTE stage only -- DIV, the timers, the PPU and the interrupt latches all
// keep running, which is what the hardware does and what makes an HBlank DMA
// visible as lost CPU time rather than as lost time.
// STOP parks the CPU until a button is pressed -- EXCEPT when KEY1 bit 0 is
// armed, which turns that same STOP into the CGB speed switch (the spd_*_r
// machine in the REG block).  A switch is NOT a wait for input: the STOP is
// held only until the clock has flipped (spd_go_r) and then for the
// post-switch park (spd_stall_r, a HALT with a timeout), and it retires
// normally after that.  spd_stop_r keeps the plain-STOP term from re-parking
// the CPU on the very same instruction once the switch has cleared KEY1.0
// underneath it.
//
// This is not cosmetic.  Once KEY1 reads back honestly -- bit 7 = 0, "not in
// double speed" -- every CGB-aware ROM stops believing it is already fast and
// runs the arm-then-STOP sequence.  With the plain STOP behaviour that is a
// permanent park: MEASURED, nine of the fourteen blargg ROMs went from PASS to
// TIMEOUT the moment KEY1 stopped reading $FF.
//
// Leaving HALT costs a CGB one extra M-cycle: the instruction (or interrupt
// dispatch) after the wake starts one M-cycle later than the plain "IE & IF
// released the stall" would give.  gambatte memory.cpp:299 (`cc += 4 *
// isCgb()` on the unhalt), SameBoy sm83_cpu.c:1632 (the halted loop advances
// 4 T before it looks at the queue); measured by gambatte's halt/
// m0int_m0stat_scx2 against m0int_m0stat/m0int_m0stat_scx2 -- same ISR, the
// HALTed variant needs one NOP less to hit the same STAT boundary.  halt_wake_r
// holds the HALT for one more bus edge once IE & IF appears; the switch park
// does the same with spd_wake_r.  A HALT that arrives with IE & IF already
// pending never parks and pays nothing (halt_park_r stays 0).
reg         halt_park_r;
reg         halt_wake_r;
always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    halt_park_r <= 1'b0;
    halt_wake_r <= 1'b0;
  end
  else begin
    if (CLK_BUS_EDGE & halt_wake_r) halt_wake_r <= 1'b0;
    if (~exe_res_halt_r)                              halt_park_r <= 1'b0;
    else if (~|(REG_IE_r[4:0] & REG_IF_r[4:0]))       halt_park_r <= 1'b1;
    else if (halt_park_r) begin
      halt_park_r <= 1'b0;
      halt_wake_r <= 1'b1;
    end
  end
end

// THE HALT BUG (Pan Docs, "halt bug"; SameBoy Core/sm83_cpu.c halt(), which
// notes it happens on a CGB too, in both modes).  A HALT that finds IE & IF
// already pending does not halt at all, and then:
//
//   IME = 0  the byte after the HALT is read TWICE: the opcode fetch does not
//            advance PC, so `halt / inc a` increments A twice and
//            `halt / ld a,$14` loads $3E and then executes $14;
//   IME = 1  the interrupt is taken at once and its return address is the
//            HALT itself, so after the RETI the HALT runs again (the
//            EI / HALT idiom with a request already pending: the handler runs
//            first, then the CPU halts).
//
// "Never halted" is exactly the case halt_park_r already separates: IE & IF
// was pending from the first clock the HALT was in the EXE, so it retires on
// its own M-cycle and the IFD hands over the next opcode on that same edge.
// halt_slept_r only remembers that a HALT did park once -- a HALT woken by an
// interrupt is not the bug, however it retires.  Both outputs are read only on
// that edge (ifd_step), when this HALT is the instruction retiring.
//
// It is forgotten on every handoff (IFD_EXE_new), not only when the EXE stops
// holding a HALT: exe_res_halt_r does not drop between two HALTs back to
// back, and after `halt` woken with IME=0 (IF stays set) a second `halt` IS
// the bug -- `halt / halt / inc a` increments A twice on SameBoy.
reg         halt_slept_r;
always @(posedge CLK)
  halt_slept_r <= ~cpu_ireset_r & exe_res_halt_r
                & ((halt_slept_r & ~IFD_EXE_new) | ~|(REG_IE_r[4:0] & REG_IF_r[4:0]));
wire        halt_quick   = exe_res_halt_r & ~halt_slept_r & ~|ifd_size_r;
assign      ifd_halt_bug = halt_quick & ~ifd_int_r;
assign      ifd_halt_ret = halt_quick &  ifd_int_r;

wire        exe_stall = (exe_res_halt_r & (~|(REG_IE_r[4:0] & REG_IF_r[4:0]) | halt_wake_r))
                      | (exe_res_stop_r & &REG_P1_r[3:0] & ~REG_KEY1_r[0] & ~spd_stop_r)
                      | spd_stall_r | spd_wake_r
                      | hdma_cpu_stall;

// The CPU is parked: a HALT with nothing pending, or the post-switch park.
// The HDMA engine reads this (no H-Blank block starts while the CPU is
// parked) and hdma_wake_edge marks the edge on which a park ends.
wire        cpu_parked     = (exe_res_halt_r & ~|(REG_IE_r[4:0] & REG_IF_r[4:0])) | spd_stall_r;
wire        hdma_wake_edge = halt_wake_r | spd_wake_r | spd_tail_r;

// latency/stage computation
// latency adder state is set after cycle 0 to avoid treating the delay as a non-CC/WB cycle.  Also, it is forced clear
// on the first base clock of a new op to make sure we do not make an access.
wire [2:0]  exe_lat   = IFD_EXE_decode[`DEC_LAT] + (IFD_EXE_new ? 0 : exe_lat_add_r);
wire [2:0]  exe_stage = exe_lat - exe_ctr_r; // must be correct on first cycle for memory op

// Outputs
reg         exe_ifd_redirect_r;
reg [15:0]  exe_ifd_redirect_target_r;

reg [15:0]  exe_pc_prev_r;
reg [15:0]  exe_pc_prev_redirect_r;
reg [15:0]  exe_target_prev_redirect_r;

assign      EXE_IFD_redirect = IFD_EXE_valid & exe_ifd_redirect_r;
assign      EXE_IFD_target   = exe_ifd_redirect_target_r;
assign      EXE_IFD_ready    = exe_ready_r;
assign      EXE_IFD_ime      = (exe_ime_r | (exe_res_int_enable_r & ~IFD_EXE_op[5])) & ~IFD_EXE_int & ~exe_res_int_disable_r; // RETI and disables need bypass.  EI is delayed a clock.

assign      EXE_MCT_req_val     = IFD_EXE_valid & exe_mem_req_r & ^exe_stage[1:0];
assign      EXE_MCT_req_addr_d1 = ( EXE_MCT_req_wr ? {((IFD_EXE_decode[`DEC_DST] == `OPR_S8 || IFD_EXE_decode[`DEC_DST] == `OPR_C) ? 8'hFF : exe_dst_r[15:8]),exe_dst_r[7:0]}
                                              : {((IFD_EXE_decode[`DEC_SRC] == `OPR_S8 || IFD_EXE_decode[`DEC_SRC] == `OPR_C) ? 8'hFF : exe_src_r[15:8]),exe_src_r[7:0]})
                                  + exe_mem_addr_mod_r;
assign      EXE_MCT_req_wr = (  IFD_EXE_decode[`DEC_GRP] == `GRP_MST
                             || IFD_EXE_decode[`DEC_GRP] == `GRP_CLL
                             // load-op-store operations need ST on second stage
                             || (exe_stage[0] & exe_loadopstore)
                             );
// always write MSB followed by LSB.  LOS ops need the result of math
assign      EXE_MCT_req_data_d1 = exe_loadopstore ? exe_res_los_r[7:0] : (exe_stage[1] ? exe_src_r[15:8] : exe_src_r[7:0]);

assign      EXE_DMA_halt = exe_res_halt_r;

assign      EXE_REG_ime = exe_ime_r;

assign      HLT_EXE_rsp = HLT_REQ_sync & ~IFD_EXE_valid;

reg         dbg_advance_r;
assign      DBG_advance = dbg_advance_r;

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    exe_ctr_r <= 0;

    exe_ime_r <= 0;

    dbg_advance_r <= 1;

    // SIMULATION HYGIENE (no effect on silicon, where every register powers up
    // at 0).  The GB register file has no reset of its own, and F_r[3:0] is
    // never written by anything -- all thirteen write sites are F_r[7:4],
    // because a DMG's F register has its low nibble hardwired to 0 and the
    // core models that by leaving the bits undriven.  In simulation they stay
    // X, PUSH AF writes the X into WRAM, a later POP pulls it back into a
    // register pair and it surfaces in the low nibble of an effective address.
    // That does not stop the CPU; it silently corrupts data, which is worse.
    // (PC_r is not here: it is already cleared by the IFD block above, and a
    //  register driven from two always blocks is a hard synthesis error.)
    SP_r <= 16'h0000;
    A_r  <= 8'h00;
    F_r  <= 8'h00;
    B_r  <= 8'h00;
    C_r  <= 8'h00;
    D_r  <= 8'h00;
    E_r  <= 8'h00;
    H_r  <= 8'h00;
    L_r  <= 8'h00;
  end
  else begin
    if (CLK_BUS_EDGE & exe_advance_r) begin
      exe_ctr_r <= exe_complete_r ? 0 : exe_ctr_r + 1;

      if (exe_complete_r) begin
        case (IFD_EXE_decode[`DEC_DST])
          //`OPR_I  : exe_src_r <= 0;
          // this is for RET.  could also make its target SP
          `OPR_PC : begin if (exe_res_sp_mod_r) SP_r <= exe_res_sp_r;                                                                end
          //`OPR_S8 : exe_dst_r[15:0] <= {8{IFD_EXE_op[15]},IFD_EXE_op[15:8]};
          //`OPR_U16: exe_dst_r[15:0] <= IFD_EXE_op[23:8];
          `OPR_BC : begin `BC_r <= exe_res_r;                                   SP_r <= exe_res_sp_r; F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_DE : begin `DE_r <= exe_res_r;                                   SP_r <= exe_res_sp_r; F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_SP : begin SP_r  <= exe_res_sp_mod_r ? exe_res_sp_r : exe_res_r;                       F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_AF : begin A_r   <= exe_res_r[15:8];                             SP_r <= exe_res_sp_r; F_r[7:4] <= exe_res_r[7:4];    end
          `OPR_B  : begin B_r   <= exe_res_r[7:0];                                                    F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_C  : begin C_r   <= exe_res_r[7:0];                                                    F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_D  : begin D_r   <= exe_res_r[7:0];                                                    F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_E  : begin E_r   <= exe_res_r[7:0];                                                    F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_H  : begin H_r   <= exe_res_r[7:0];                                                    F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_L  : begin L_r   <= exe_res_r[7:0];                                                    F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_HL : begin `HL_r <= exe_res_hl_mod_r ? exe_res_hl_r : exe_res_r; SP_r <= exe_res_sp_r; F_r[7:4] <= exe_res_cc_r[7:4]; end
          `OPR_A  : begin A_r   <= exe_res_r[7:0]; if (exe_res_hl_mod_r) `HL_r <= exe_res_hl_r;       F_r[7:4] <= exe_res_cc_r[7:4]; end
        endcase

        // delay 1 inst because fetch will see this in the following cycle
        exe_ime_r <= (exe_ime_r | exe_res_int_enable_r) & ~exe_res_int_disable_r & ~IFD_EXE_int;
        exe_pc_prev_r <= IFD_EXE_pc_start;
        if (exe_ifd_redirect_r) exe_pc_prev_redirect_r <= IFD_EXE_pc_start;
        if (exe_ifd_redirect_r) exe_target_prev_redirect_r <= exe_ifd_redirect_target_r;
      end
    end

    if (CLK_BUS_EDGE) dbg_advance_r <= ~IFD_EXE_valid | ~exe_complete_r | DBG_EXE_step;
  end

  // alu/bit/los input
  exe_src_alu_r[7:0] <= IFD_EXE_decode[3] ? exe_mem_data_r[7:0] : exe_src_r[7:0];

  // default to no redirect and no extended latency
  exe_ifd_redirect_r <= 0;
  exe_lat_add_r      <= 0;

  // default no mod to SP
  exe_res_sp_mod_r <= 0;
  exe_res_sp_r     <= SP_r;

  exe_res_int_enable_r  <= 0;
  exe_res_int_disable_r <= 0;

  exe_res_r    <= exe_dst_r;
  exe_res_cc_r <= exe_cc_r;

  exe_res_halt_r <= 0;
  exe_res_stop_r <= 0;

  // result computation
  case (IFD_EXE_decode[`DEC_GRP])
    `GRP_SPC: begin
      // NOP (0x00), STOP (0x10), HALT (0x76), EI (0xFB), DI (0xF3)

      exe_res_halt_r <= ~IFD_EXE_op[7] & IFD_EXE_op[4] &  IFD_EXE_op[2];
      exe_res_stop_r <= ~IFD_EXE_op[7] & IFD_EXE_op[4] & ~IFD_EXE_op[2];

      exe_res_int_enable_r  <= IFD_EXE_op[7] &  IFD_EXE_op[3];
      exe_res_int_disable_r <= IFD_EXE_op[7] & ~IFD_EXE_op[3];
    end
    `GRP_MOV: begin
      exe_res_r <= exe_src_r;
      exe_res_cc_r <= exe_cc_r;

      // LD HL,SP requires an extra clock
      if (&IFD_EXE_op[7:6]) exe_lat_add_r <= 1;
    end
    `GRP_MIC,`GRP_INC,`GRP_DEC,`GRP_MDC: begin
      {exe_res_c3_r,exe_res_los_r[3:0]}   <= exe_src_alu_r[3:0] + {{3{IFD_EXE_decode[0]}},1'b1};
      {exe_res_c7_r,exe_res_los_r[7:4]}   <= exe_src_alu_r[7:4] + {4{IFD_EXE_decode[0]}} + exe_res_c3_r;
      exe_res_r[7:0]                      <= IFD_EXE_decode[3] ? exe_dst_r[7:0] : exe_res_los_r[7:0];
      if (~IFD_EXE_op[2]) exe_res_r[15:8] <= exe_src_r[15:8] + {8{IFD_EXE_decode[0]}} + exe_res_c7_r;
      exe_res_cc_r                        <= ~IFD_EXE_op[2] ? exe_cc_r : {~|exe_res_los_r,IFD_EXE_decode[0],IFD_EXE_decode[0]^exe_res_c3_r,exe_cc_r[`FLAG_C],exe_cc_r[3:0]};

      // 16b operations require an extra clock
      exe_lat_add_r <= IFD_EXE_op[1] ? 1 : 0;
    end
    `GRP_ALU,`GRP_MLU: begin
      if (~IFD_EXE_op[7] | (IFD_EXE_op[6] & ~IFD_EXE_op[2])) begin
        if      (IFD_EXE_op[3:0] == 4'h9) begin
          // ADD HL,BC
          // ADD HL,DE
          // ADD HL,HL
          // ADD HL,SP
          {exe_res_c11_r,exe_res_r[11:0]}  <= exe_dst_r[11:0] + exe_src_r[11:0];
          {exe_res_c15_r,exe_res_r[15:12]} <= exe_dst_r[15:12] + exe_src_r[15:12] + exe_res_c11_r;
          exe_res_cc_r                     <= {exe_cc_r[`FLAG_Z],1'b0,exe_res_c11_r,exe_res_c15_r,exe_cc_r[3:0]};

          exe_lat_add_r <= 1;
        end
        else if (IFD_EXE_op[3:0] == 4'h7) begin
          if (~IFD_EXE_op[4]) begin
            // DAA
            if (exe_cc_r[`FLAG_N]) begin
              exe_res_r[7:0] <= exe_src_r[7:0] - {(exe_cc_r[`FLAG_C] ? 4'h6 : 4'h0), (exe_cc_r[`FLAG_H] ? 4'h6 : 4'h0)};
              exe_res_c7_r <= 0;
            end
            else begin
              {exe_res_c3_r,exe_res_r[3:0]} <= exe_src_r[3:0] + ((exe_cc_r[`FLAG_H] | (exe_src_r[3] & | exe_src_r[2:1])) ? 4'h6 : 4'h0);
              {exe_res_c7_r,exe_res_r[7:4]} <= exe_src_r[7:4] + ((exe_cc_r[`FLAG_C] | (exe_src_r[7:0] > 8'h99)) ? 4'h6 : 4'h0) + exe_res_c3_r;
            end

            exe_res_cc_r <= {~|exe_res_r[7:0],exe_cc_r[`FLAG_N],1'b0,(exe_res_c7_r | exe_cc_r[`FLAG_C]),exe_cc_r[3:0]};
          end
          else begin
            // SCF
            exe_res_r    <= exe_dst_r;
            exe_res_cc_r <= {exe_cc_r[`FLAG_Z],1'b0,1'b0,1'b1,exe_cc_r[3:0]};
          end
        end
        else if (IFD_EXE_op[3:0] == 4'hF) begin
          if (~IFD_EXE_op[4]) begin
            // CPL
            exe_res_r    <= ~exe_src_r;
            exe_res_cc_r <= {exe_cc_r[`FLAG_Z],1'b1,1'b1,exe_cc_r[4:0]};
          end
          else begin
            // CCF
            exe_res_r    <= exe_dst_r;
            exe_res_cc_r <= {exe_cc_r[`FLAG_Z],1'b0,1'b0,~exe_cc_r[`FLAG_C],exe_cc_r[3:0]};
          end
        end
        else if (IFD_EXE_op[3:0] == 4'h8) begin
          if (~IFD_EXE_op[4]) begin
            // ADD SP,e
            {exe_res_c3_r,exe_res_r[3:0]} <= exe_dst_r[3:0] + exe_src_r[3:0];
            {exe_res_c7_r,exe_res_r[7:4]} <= exe_dst_r[7:4] + exe_src_r[7:4] + exe_res_c3_r;
            exe_res_r[15:8]               <= exe_dst_r[15:8] + exe_src_r[15:8] + exe_res_c7_r;
            exe_res_cc_r                  <= {1'b0,1'b0,exe_res_c3_r,exe_res_c7_r,exe_cc_r[3:0]};

            // hack to avoid memory access.  stage is always 0 on first cycle
            exe_lat_add_r <= |exe_ctr_r ? 2 : 1;
          end
          else begin
            // LD HL,SP+e
            {exe_res_c3_r,exe_res_r[3:0]} <= SP_r[3:0] + exe_src_r[3:0];
            {exe_res_c7_r,exe_res_r[7:4]} <= SP_r[7:4] + exe_src_r[7:4] + exe_res_c3_r;
            exe_res_r[15:8]               <= SP_r[15:8] + exe_src_r[15:8] + exe_res_c7_r;
            exe_res_cc_r                  <= {1'b0,1'b0,exe_res_c3_r,exe_res_c7_r,exe_cc_r[3:0]};

            exe_lat_add_r <= 1;
          end
        end
      end
      else begin
        // ALU
        case (IFD_EXE_op[5:3])
          3'h0,3'h1,3'h2,3'h3: begin // ADD,ADC,SUB,SBC
            {exe_res_c3_r,exe_res_r[3:0]} <= exe_dst_r[3:0] + ({4{IFD_EXE_op[4]}} ^ exe_src_alu_r[3:0]) + ((IFD_EXE_op[3] & exe_cc_r[4]) ^ (IFD_EXE_op[4]));
            {exe_res_c7_r,exe_res_r[7:4]} <= exe_dst_r[7:4] + ({4{IFD_EXE_op[4]}} ^ exe_src_alu_r[7:4]) + exe_res_c3_r;
            exe_res_cc_r              <= {~|exe_res_r[7:0],IFD_EXE_op[4],IFD_EXE_op[4]^exe_res_c3_r,IFD_EXE_op[4]^exe_res_c7_r,exe_cc_r[3:0]};
          end
          3'h4: begin // AND
            exe_res_r[7:0] <= exe_dst_r[7:0] & exe_src_alu_r[7:0];
            exe_res_cc_r   <= {~|exe_res_r[7:0],1'b0,1'b1,1'b0,exe_cc_r[3:0]};
          end
          3'h5: begin // XOR
            exe_res_r[7:0] <= exe_dst_r[7:0] ^ exe_src_alu_r[7:0];
            exe_res_cc_r   <= {~|exe_res_r[7:0],1'b0,1'b0,1'b0,exe_cc_r[3:0]};
          end
          3'h6: begin // OR
            exe_res_r[7:0] <= exe_dst_r[7:0] | exe_src_alu_r[7:0];
            exe_res_cc_r   <= {~|exe_res_r[7:0],1'b0,1'b0,1'b0,exe_cc_r[3:0]};
          end
          3'h7: begin // CP
            {exe_res_c3_r,exe_res_cp_r[3:0]} <= exe_dst_r[3:0] + ({4{1'b1}} ^ exe_src_alu_r[3:0]) + 1'b1;
            {exe_res_c7_r,exe_res_cp_r[7:4]} <= exe_dst_r[7:4] + ({4{1'b1}} ^ exe_src_alu_r[7:4]) + exe_res_c3_r;
            exe_res_cc_r                 <= {~|exe_res_cp_r[7:0],1'b1,~exe_res_c3_r,~exe_res_c7_r,exe_cc_r[3:0]};
          end
        endcase
      end
    end
    `GRP_BIT,`GRP_MBT: begin
      if (~IFD_EXE_cb) begin
        exe_res_r[7:0] <= IFD_EXE_op[3] ? {(IFD_EXE_op[4] ? exe_cc_r[`FLAG_C] : exe_src_r[0]),exe_src_r[7:1]} : {exe_src_r[6:0],(IFD_EXE_op[4] ? exe_cc_r[`FLAG_C] : exe_src_r[7])};
        exe_res_cc_r[7:0] <= {1'b0,1'b0,1'b0,(IFD_EXE_op[3] ? exe_src_r[0] : exe_src_r[7]),exe_cc_r[3:0]};
      end
      else begin
        // CB bit operations
        case (IFD_EXE_op[15:12])
          4'h0,4'h1,4'h2: begin
            // RLC,RRC
            // RL,RR
            // SLA,SRA
            exe_res_los_r[7:0] <= IFD_EXE_op[11] ? {(IFD_EXE_op[12] ? exe_cc_r[`FLAG_C] : (IFD_EXE_op[13] ? exe_src_alu_r[7] : exe_src_alu_r[0])),exe_src_alu_r[7:1]} : {exe_src_alu_r[6:0],(IFD_EXE_op[12] ? exe_cc_r[`FLAG_C] : (~IFD_EXE_op[13] & exe_src_alu_r[7]))};
            if (~IFD_EXE_decode[3]) exe_res_r[7:0] <= exe_res_los_r[7:0];
            exe_res_cc_r       <= {~|exe_res_los_r[7:0],1'b0,1'b0,(IFD_EXE_op[11] ? exe_src_alu_r[0] : exe_src_alu_r[7]),exe_cc_r[3:0]};
          end
          4'h3:begin
            // SWAP,SRL
            exe_res_los_r[7:0] <= IFD_EXE_op[11] ? {1'b0,exe_src_alu_r[7:1]} : {exe_src_alu_r[3:0],exe_src_alu_r[7:4]};
            if (~IFD_EXE_decode[3]) exe_res_r[7:0] <= exe_res_los_r[7:0];
            exe_res_cc_r       <= {~|exe_res_los_r[7:0],1'b0,1'b0,(IFD_EXE_op[11] & exe_src_alu_r[0]),exe_cc_r[3:0]};
          end
          4'h4,4'h5,4'h6,4'h7:begin
            // BIT
            exe_res_los_r[7:0] <= exe_src_alu_r[7:0];
            exe_res_cc_r       <= {~exe_res_los_r[IFD_EXE_op[13:11]],1'b0,1'b1,exe_cc_r[`FLAG_C],exe_cc_r[3:0]};

            // hack to avoid memory access.  skips stage 1 (ST)
            // NOTE: the previous version of this was holding lat_add from the prior op (2 cycle op, lat_add=1) during the first stage which caused us to not perform the LD
            // 1) lat_add to be 0 while the stage is 0
            // 2) lat_add to be 7 going into subsequent stages (keyed on BUS_EDGE)
            if (IFD_EXE_decode[3]) exe_lat_add_r <= (~exe_stage[1] | CLK_BUS_EDGE) ? 3'h7 : 0;

          end
          4'h8,4'h9,4'hA,4'hB:begin
            // RES
            exe_res_los_r[7:0] <= exe_src_alu_r[7:0] & ~(8'h1 << IFD_EXE_op[13:11]);
            if (~IFD_EXE_decode[3]) exe_res_r[15:0] <= {8'h00,exe_res_los_r[7:0]};
            exe_res_cc_r       <= exe_cc_r;
          end
          4'hC,4'hD,4'hE,4'hF:begin
            // SET
            exe_res_los_r[7:0] <= exe_src_alu_r[7:0] | (8'h1 << IFD_EXE_op[13:11]);
            if (~IFD_EXE_decode[3]) exe_res_r[15:0] <= {8'h00,exe_res_los_r[7:0]};
            exe_res_cc_r       <= exe_cc_r;
          end
        endcase
      end
    end
    `GRP_JMP: begin
      // branch control flow
      // redirect and the associated PC must be available 1 base clock after the start of the bus cycle for IFD to send
      // the correct address to MCT!  This is important for JMP HL.
      // It's ok to cause a redirect even if this isn't the final stage as long as we don't advance EXE.
      exe_ifd_redirect_r        <= (IFD_EXE_op[0] | (~IFD_EXE_op[7] & ~IFD_EXE_op[5])) | exe_redirect_taken;
      exe_ifd_redirect_target_r <= IFD_EXE_op[7] ? (IFD_EXE_op[5] ? `HL_r : exe_src_r) : (IFD_EXE_pc_next + exe_src_r);

      // JMP HL needs to be special cased since HL looks like it can be bypassed.
      exe_lat_add_r <= (exe_ifd_redirect_r & ~(IFD_EXE_op[7] & IFD_EXE_op[5])) ? 1 : 0;
    end
    `GRP_RET: begin
      // RET
      exe_ifd_redirect_r        <= IFD_EXE_op[0] | exe_redirect_taken;
      exe_ifd_redirect_target_r <= exe_mem_data_r;

      exe_lat_add_r <= exe_ifd_redirect_r ? (IFD_EXE_op[0] ? 3 : 4) : 1;

      exe_res_sp_mod_r <= exe_ifd_redirect_r;
      exe_res_sp_r     <= SP_r + 2;

      // RETI enables interrupts
      exe_res_int_enable_r <= IFD_EXE_op[4] & IFD_EXE_op[0];
    end
    `GRP_MST: begin
      if (IFD_EXE_decode[`DEC_DST] == `OPR_SP) begin
        exe_res_sp_mod_r <= 1;
        exe_res_sp_r     <= SP_r - 2;
      end
    end
    `GRP_MLD: begin
      exe_res_r <= exe_mem_data_r;
      exe_res_cc_r <= exe_cc_r;

      if (IFD_EXE_decode[`DEC_SRC] == `OPR_SP) exe_res_sp_r <= SP_r + 2;
    end
    `GRP_CLL: begin
      // CALL + RST
      exe_ifd_redirect_r        <= IFD_EXE_op[0] | exe_redirect_taken;
      exe_ifd_redirect_target_r <= IFD_EXE_op[1] ? {1'h0,IFD_EXE_int, IFD_EXE_op[5:3], 3'h0} : IFD_EXE_op[23:8];

      exe_lat_add_r <= exe_ifd_redirect_r ? (IFD_EXE_int ? 4 : 3) : 0;

      exe_res_sp_mod_r <= exe_ifd_redirect_r;
      exe_res_sp_r     <= SP_r - 2;
    end
    `GRP____: begin
    end
  endcase

  exe_res_hl_mod_r <= IFD_EXE_op[7:0] == 8'h22 || IFD_EXE_op[7:0] == 8'h2A || IFD_EXE_op[7:0] == 8'h32 || IFD_EXE_op[7:0] == 8'h3A;
  exe_res_hl_r     <= IFD_EXE_op[4] ? `HL_r - 1 : `HL_r + 1;

  // memory operations
  exe_mem_req_r <= ~cpu_ireset_r & CLK_BUS_EDGE;

  // force MSB -> LSB order for all memory operations to simplify 8/16b sequencing.
  // LD             (+1)  +0
  // ST             (+1)  +0
  // PUSH/CALL/RST  -1    -2
  // POP/RET        +1    +0
  // LD-OP-ST       +0    +0
  exe_mem_addr_mod_r <= ((exe_stage[1] & ~exe_loadopstore) ? 1 : 0) + ((IFD_EXE_decode[`DEC_DST] == `OPR_SP) ? -2 : 0);
  if (MCT_EXE_rsp_val & ~EXE_MCT_req_wr) if (exe_stage[1] & ~exe_loadopstore) exe_mem_data_r[15:8] <= MCT_data; else exe_mem_data_r[7:0] <= MCT_data;

  // op completion and pipe advance
  exe_complete_r <= IFD_EXE_valid & ~|exe_stage;
  exe_advance_r  <= IFD_EXE_valid & (~exe_complete_r | DBG_EXE_step & ~exe_stall);
  exe_ready_r    <= ~IFD_EXE_valid | (exe_complete_r & ~exe_stall & DBG_EXE_step);

  // operand read
  case (IFD_EXE_decode[`DEC_SRC])
    //`OPR_I  : exe_src_r <= 0;
    `OPR_PC : exe_src_r[15:0] <= IFD_EXE_pc_next;
    `OPR_S8 : exe_src_r[15:0] <= {{8{IFD_EXE_op[15]}},IFD_EXE_op[15:8]};
    `OPR_U16: exe_src_r[15:0] <= IFD_EXE_op[23:8];
    `OPR_BC : exe_src_r[15:0] <= `BC_r;
    `OPR_DE : exe_src_r[15:0] <= `DE_r;
    `OPR_SP : exe_src_r[15:0] <= SP_r;
    `OPR_AF : exe_src_r[15:0] <= `AF_r;
    `OPR_B  : exe_src_r[15:0] <= {8'h0,B_r};
    `OPR_C  : exe_src_r[15:0] <= {8'h0,C_r};
    `OPR_D  : exe_src_r[15:0] <= {8'h0,D_r};
    `OPR_E  : exe_src_r[15:0] <= {8'h0,E_r};
    `OPR_H  : exe_src_r[15:0] <= {8'h0,H_r};
    `OPR_L  : exe_src_r[15:0] <= {8'h0,L_r};
    `OPR_HL : exe_src_r[15:0] <= `HL_r;
    `OPR_A  : exe_src_r[15:0] <= {8'h0,A_r};
  endcase

  // operand read
  case (IFD_EXE_decode[`DEC_DST])
    //`OPR_I  : exe_src_r <= 0;
    `OPR_PC : exe_dst_r[15:0] <= IFD_EXE_pc_next;
    `OPR_S8 : exe_dst_r[15:0] <= {{8{IFD_EXE_op[15]}},IFD_EXE_op[15:8]};
    `OPR_U16: exe_dst_r[15:0] <= IFD_EXE_op[23:8];
    `OPR_BC : exe_dst_r[15:0] <= `BC_r;
    `OPR_DE : exe_dst_r[15:0] <= `DE_r;
    `OPR_SP : exe_dst_r[15:0] <= SP_r;
    `OPR_AF : exe_dst_r[15:0] <= `AF_r;
    `OPR_B  : exe_dst_r[15:0] <= {8'h0,B_r};
    `OPR_C  : exe_dst_r[15:0] <= {8'h0,C_r};
    `OPR_D  : exe_dst_r[15:0] <= {8'h0,D_r};
    `OPR_E  : exe_dst_r[15:0] <= {8'h0,E_r};
    `OPR_H  : exe_dst_r[15:0] <= {8'h0,H_r};
    `OPR_L  : exe_dst_r[15:0] <= {8'h0,L_r};
    `OPR_HL : exe_dst_r[15:0] <= `HL_r;
    `OPR_A  : exe_dst_r[15:0] <= {8'h0,A_r};
  endcase

  // condition code read
  exe_cc_r <= F_r;

  // debug writes
  if (REG_req_dbg) begin
    case (REG_address)
      8'h60: if (reg_src_r) A_r <= REG_req_data;
      8'h61: if (reg_src_r) F_r[7:4] <= REG_req_data[7:4];
      8'h62: if (reg_src_r) B_r <= REG_req_data;
      8'h63: if (reg_src_r) C_r <= REG_req_data;
      8'h64: if (reg_src_r) D_r <= REG_req_data;
      8'h65: if (reg_src_r) E_r <= REG_req_data;
      8'h66: if (reg_src_r) H_r <= REG_req_data;
      8'h67: if (reg_src_r) L_r <= REG_req_data;
      8'h68: if (reg_src_r) SP_r[7:0] <= REG_req_data;
      8'h69: if (reg_src_r) SP_r[15:8] <= REG_req_data;
      //8'h6A:
      //8'h6B:
      8'h6C: if (reg_src_r) exe_ime_r <= REG_req_data[0];
    endcase
  end
end

//-------------------------------------------------------------------
// DMA
//-------------------------------------------------------------------

parameter
  ST_DMA_IDLE      = 5'b00001,
  ST_DMA_READ      = 5'b00010,
  ST_DMA_READ_WAIT = 5'b00100,
  ST_DMA_HOLD      = 5'b01000,
  ST_DMA_WRITE     = 5'b10000;

reg  [4:0]  dma_state_r;
reg  [7:0]  dma_addr_r;
reg         dma_req_r;
reg         dma_src_r;
reg  [7:0]  dma_data_r;
reg  [1:0]  dma_cred_r;     // M-cycles granted to the transfer and not yet spent
reg         dma_warm_r;     // the warm-up M-cycle is over: the DMA has read its first byte
wire        dma_sys_free;   // assigned in the MCT section: the system bus is free

assign      DMA_active      = ~|(dma_state_r & ST_DMA_IDLE);
assign      DMA_VRAM_active = DMA_active &  dma_src_r;

// THE OAM DMA OWNS THE SYSTEM BUS ONLY WHILE ITS OWN READ IS IN FLIGHT -- not
// for the whole transfer, and not at a fixed point in the M-cycle.  On a CGB,
// work RAM sits on a bus of its own, so a DMA out of $C000-$DFFF does not stop
// the CPU reading ROM or cart RAM (SameBoy Core/memory.c bus_for_addr() and
// is_addr_in_dma_use(); Pan Docs, "OAM DMA transfer").  Here both go to the
// same PSRAM, so "different bus" has to be emulated by SHARING the M-cycle.
//
// Holding this signal up for the whole transfer -- as it used to be -- meant
// SYS_REQ, SYS_ADDR and SYS_WR were taken away from the controller for 160
// M-cycles while its ST_MCT_EXT still completed on SYS_RDY and still latched
// SYS_RDDATA.  Every CPU access to ROM, cart RAM or WRAM during an OAM DMA
// therefore returned the byte the DMA had just read, and every CPU WRITE was
// dropped (SYS_WR forced low).  A game whose DMA wait loop lives in ROM rather
// than HRAM -- which is legal on a CGB when the source is WRAM -- executed the
// OAM image instead of its own code.
assign      DMA_SYS_active  = |(dma_state_r & ST_DMA_READ_WAIT) & ~dma_src_r;

assign      DMA_req_val = |(dma_state_r & ST_DMA_READ_WAIT) & dma_req_r;
assign      DMA_address = {REG_DMA_r,dma_addr_r};

assign      DMA_OAM_req_val  = |(dma_state_r & ST_DMA_WRITE);
assign      DMA_OAM_address  = dma_addr_r;
assign      DMA_OAM_req_data = dma_data_r;

assign      HLT_DMA_rsp = HLT_REQ_sync & ~DMA_active;

// One M-cycle granted per bus edge, one spent per byte: a system-bus source
// spends it on the OAM write (the read floats), a VRAM source spends it on the
// read (read and write stay together, as they always were).
wire        dma_grant = CLK_BUS_EDGE & ~EXE_DMA_halt;
wire        dma_spend = |(dma_state_r & (dma_src_r ? ST_DMA_READ : ST_DMA_HOLD))
                      & |dma_cred_r;

// THE M-CYCLE BUDGET, AND WHY THE READ IS DECOUPLED FROM THE WRITE.
//
// A PSRAM access costs 3 CLK to the request, up to 18 in the arbiter behind an
// MCU access and 2 to retire -- up to 23 (see starvation_r).  The M-cycle is
// 80 CLK at single speed but only 39 at DOUBLE, and a CPU that is on the bus
// every M-cycle already spends ~20 of them.  Two worst-case accesses do NOT
// fit in 39: with one PSRAM port this core cannot serve both masters inside
// every double-speed M-cycle, and no arbitration order changes that.
//
// What it can do is stop the shortfall from COMPOUNDING, which is what the
// credit below is for.  The OAM write -- the part the software can see, since
// a game counts 160 M-cycles and then touches OAM -- is paced by the bus edge
// and by nothing else: one edge grants one credit, one OAM write spends one,
// and a transfer is 160 writes, so it can never run faster than the hardware's
// 160 M-cycles.  The SOURCE READ is free to go out at any point of the
// M-cycle, as soon as the bus is free, and to run into the next one: it is the
// elastic half.  Without this the DMA re-synchronised to the NEXT bus edge
// after a read that overran by a single clock, so one late arbiter answer cost
// a whole M-cycle and the transfer stretched far past 160 (measured: 197 at
// double speed under arbiter contention).  With it, a late read costs the
// transfer only the clocks it was actually late, and the credit (up to 3
// M-cycles) lets the DMA catch the time back up in the M-cycles the CPU does
// not use the bus in.
always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    dma_state_r <= ST_DMA_IDLE;
    dma_req_r <= 0;
    dma_cred_r <= 0;
    dma_warm_r <= 0;
  end
  else begin
    dma_src_r <= (REG_DMA_r[7:5] == 3'b100) ? 1 : 0;

    case (dma_state_r)
      ST_DMA_IDLE: begin
        dma_addr_r <= 0;
        dma_cred_r <= 0;
        dma_warm_r <= 0;

        // sync start of DMA request to BUS edge
        if (REG_DMA_start & CLK_BUS_EDGE) dma_state_r <= ST_DMA_READ;
      end
      ST_DMA_READ: begin
        // A VRAM source keeps the order it always had -- read and write inside
        // the M-cycle it is billed for -- because the CPU sees that read
        // through the VRAM port mux and through VBK, and moving it earlier
        // moves what a mid-transfer bank change lands on
        // (gambatte oamdma/oamdma_src8000_vrambankchange_*).
        //
        // A system-bus source is the one that has to float: it waits for the
        // controller to be off the bus and only for that, NOT for a bus edge.
        // Its byte is due at the NEXT edge, so anywhere inside this M-cycle
        // will do, and a slow arbiter costs the transfer nothing.
        if (dma_src_r ? dma_spend : dma_sys_free) begin
          dma_req_r   <= 1'b1;

          dma_state_r <= ST_DMA_READ_WAIT;
        end
      end
      ST_DMA_READ_WAIT: begin
        dma_req_r <= 0;
        dma_data_r <= dma_src_r ? VRAM_data : SYS_RDDATA;

        // address available 1 cycle early and we have the VRAM bus so not necessary to wait an extra clock
        if (~dma_req_r & (dma_src_r | SYS_RDY)) dma_state_r <= ST_DMA_HOLD;
      end
      ST_DMA_HOLD: begin
        // The byte is ready; it lands in OAM on the M-cycle it was billed for.
        // A VRAM source already spent its credit to get here.
        if (dma_src_r | dma_spend) dma_state_r <= ST_DMA_WRITE;
      end
      ST_DMA_WRITE: begin
        dma_addr_r <= dma_addr_r + 1;

        dma_state_r <= (dma_addr_r[7] & dma_addr_r[4] & &dma_addr_r[3:0]) ? ST_DMA_IDLE : ST_DMA_READ;
      end
    endcase

    // The credit.  Written after the case so that ST_DMA_IDLE's clear wins on
    // the cycle the transfer ends.  A bus edge that lands on the same clock as
    // a write cancels out, which is what keeps the bill at exactly one write
    // per M-cycle.  EXE_DMA_halt stops the GRANT, not the read: the DMA pauses
    // at an OAM-write boundary, the granularity SameBoy pauses at too
    // (Core/memory.c GB_dma_run: "if (halted || stopped) return").
    if (~|(dma_state_r & ST_DMA_IDLE)) begin
      if (dma_grant & ~dma_spend & ~&dma_cred_r) dma_cred_r <= dma_cred_r + 2'd1;
      if (dma_spend & ~dma_grant)                dma_cred_r <= dma_cred_r - 2'd1;
      // The warm-up (SameBoy Core/memory.c:256, is_addr_in_dma_use(): no
      // conflict while dma_current_dest is $FF or $00).  The $FF46 write
      // parks dest at $FF, the first step only takes it to $00 and the second
      // is the one that reads byte 0 -- so the CPU access on the M-cycle
      // right after the write (the first one after the start edge here) still
      // has its bus, and the collisions begin one M-cycle later, when the
      // latch holds THIS transfer's byte 0.  Paced by the grant, not the bare
      // edge: a HALT freezes SameBoy's DMA before its first step too.
      if (dma_grant) dma_warm_r <= 1'b1;
    end
  end
end

//-------------------------------------------------------------------
// PPU
//-------------------------------------------------------------------

// CGB VRAM is 2 x 8KB.  Port A is shared by the four masters, in the priority
// the contract fixes: HDMA > OAM-DMA > PPU > CPU.  The PPU here is a TIMING
// PPU and always fetches from bank 0; everyone else follows VBK.  Mode 3
// blocks the CPU on BOTH banks (there is one VRAM interface on the chip, not
// two), and a blocked read returns $FF -- MCT_TGT_VRAM already does that.
wire        hdma_vram_wren;
wire [12:0] hdma_vram_address;
wire [7:0]  hdma_vram_data;
wire        HDMA_VRAM_active;

wire        vram_wren    = HDMA_VRAM_active ? hdma_vram_wren    : DMA_VRAM_active ? 1'b0             : PPU_MCT_vram_active ? 1'b0         : MCT_VRAM_wren;
wire [12:0] vram_address = HDMA_VRAM_active ? hdma_vram_address : DMA_VRAM_active ? DMA_address[12:0] : PPU_VRAM_active ? PPU_VRAM_address : MCT_VRAM_address;
wire [7:0]  vram_wrdata  = HDMA_VRAM_active ? hdma_vram_data    : MCT_VRAM_data;
wire        vram_bank    = (PPU_VRAM_active & ~HDMA_VRAM_active & ~DMA_VRAM_active) ? 1'b0 : REG_VBK_r;

wire        vram0_wren   = vram_wren & ~vram_bank;
wire        vram1_wren   = vram_wren &  vram_bank;

wire [7:0]  vram0_rddata;
wire [7:0]  vram1_rddata;

// The read mux follows the bank with the RAM's own one-cycle latency, so a
// VBK write between issuing an address and consuming the data cannot hand the
// wrong bank's byte to whoever asked.
reg         vram_bank_d1_r;
always @(posedge CLK) vram_bank_d1_r <= vram_bank;
wire [7:0]  vram_rddata  = vram_bank_d1_r ? vram1_rddata : vram0_rddata;

// Port B, one per bank.  The bridge (phase 2) owns it whenever it asks; the
// MCU debug pipe uses the cycles left over and stalls otherwise.  In this
// phase VRAM*_B_REQ is tied low by sgb.v, so the debug pipe never waits.
wire        dbg_vram_wren;
wire [12:0] dbg_vram_address;
wire [7:0]  dbg_vram_wrdata;
wire        dbg_vram_bank;

// dbg_vram_wren already carries the arbitration (see dbg_vram_free below): it
// is only asserted on a cycle the bridge left alone.
wire        vram0_b_wren    = dbg_vram_wren & ~dbg_vram_bank;
wire [12:0] vram0_b_address = VRAM0_B_REQ ? VRAM0_B_ADDR : dbg_vram_address;
wire        vram1_b_wren    = dbg_vram_wren &  dbg_vram_bank;
wire [12:0] vram1_b_address = VRAM1_B_REQ ? VRAM1_B_ADDR : dbg_vram_address;

// The read data is one cycle behind the address, so the debug pipe may only
// believe it if the bridge asked for neither this cycle nor the last one.
reg         vram0_b_req_d1_r;
reg         vram1_b_req_d1_r;
always @(posedge CLK) begin
  vram0_b_req_d1_r <= VRAM0_B_REQ;
  vram1_b_req_d1_r <= VRAM1_B_REQ;
end
wire        dbg_vram_free = ~(dbg_vram_bank ? VRAM1_B_REQ      : VRAM0_B_REQ)
                          & ~(dbg_vram_bank ? vram1_b_req_d1_r : vram0_b_req_d1_r);

`ifdef MK2
vram vram0 (
  .clka(CLK), // input clka
  .wea(vram0_wren), // input [0 : 0] wea
  .addra(vram_address), // input [12 : 0] addra
  .dina(vram_wrdata), // input [7 : 0] dina
  .douta(vram0_rddata), // output [7 : 0] douta
  .clkb(CLK), // input clkb
  .web(vram0_b_wren), // input [0 : 0] web
  .addrb(vram0_b_address), // input [12 : 0] addrb
  .dinb(dbg_vram_wrdata), // input [7 : 0] dinb
  .doutb(VRAM0_B_DATA) // output [7 : 0] doutb
);
vram vram1 (
  .clka(CLK), // input clka
  .wea(vram1_wren), // input [0 : 0] wea
  .addra(vram_address), // input [12 : 0] addra
  .dina(vram_wrdata), // input [7 : 0] dina
  .douta(vram1_rddata), // output [7 : 0] douta
  .clkb(CLK), // input clkb
  .web(vram1_b_wren), // input [0 : 0] web
  .addrb(vram1_b_address), // input [12 : 0] addrb
  .dinb(dbg_vram_wrdata), // input [7 : 0] dinb
  .doutb(VRAM1_B_DATA) // output [7 : 0] doutb
);
`endif
`ifdef MK3
vram vram0 (
  .clock(CLK), // input clka
  .wren_a(vram0_wren), // input [0 : 0] wea
  .address_a(vram_address), // input [12 : 0] addra
  .data_a(vram_wrdata), // input [7 : 0] dina
  .q_a(vram0_rddata), // output [7 : 0] douta
  .wren_b(vram0_b_wren), // input [0 : 0] web
  .address_b(vram0_b_address), // input [12 : 0] addrb
  .data_b(dbg_vram_wrdata), // input [7 : 0] dinb
  .q_b(VRAM0_B_DATA) // output [7 : 0] doutb
);
vram vram1 (
  .clock(CLK), // input clka
  .wren_a(vram1_wren), // input [0 : 0] wea
  .address_a(vram_address), // input [12 : 0] addra
  .data_a(vram_wrdata), // input [7 : 0] dina
  .q_a(vram1_rddata), // output [7 : 0] douta
  .wren_b(vram1_b_wren), // input [0 : 0] web
  .address_b(vram1_b_address), // input [12 : 0] addrb
  .data_b(dbg_vram_wrdata), // input [7 : 0] dinb
  .q_b(VRAM1_B_DATA) // output [7 : 0] doutb
);
`endif

assign TAP_VRAM_WE   = vram_wren;
assign TAP_VRAM_BANK = vram_bank;
assign TAP_VRAM_ADDR = vram_address;

wire        oam_wren    = DMA_active ? DMA_OAM_req_val  : PPU_MCT_oam_active ? 0           : MCT_OAM_wren;
wire [7:0]  oam_address = DMA_active ? DMA_OAM_address  : PPU_OAM_active ? PPU_OAM_address : MCT_OAM_address;
wire [7:0]  oam_rddata;
wire [7:0]  oam_wrdata  = DMA_active ? DMA_OAM_req_data : MCT_OAM_data;

assign TAP_OAM_WE = oam_wren;

wire        dbg_oam_wren;
wire [7:0]  dbg_oam_address;
wire [7:0]  dbg_oam_rddata;
wire [7:0]  dbg_oam_wrdata;

// Port B of the OAM, shared exactly like the VRAM and CRAM ones: the bridge's
// snapshot copy engine wins the cycle it asks for, the MCU debug pipe stalls.
// dbg_oam_wren already carries the arbitration (dbg_oam_free below), so a debug
// write can never land on a cycle the copy engine is reading.
wire [7:0]  oam_b_address = OAM_RD_REQ ? OAM_RD_ADDR : dbg_oam_address;
wire        oam_b_wren    = dbg_oam_wren & ~OAM_RD_REQ;

// The read data is one cycle behind the address, so the debug pipe may only
// believe it if the bridge asked neither this cycle nor the last one.
reg         oam_rd_req_d1_r;
always @(posedge CLK) oam_rd_req_d1_r <= OAM_RD_REQ;
wire        dbg_oam_free = ~OAM_RD_REQ & ~oam_rd_req_d1_r;

assign OAM_RD_DATA = dbg_oam_rddata;

`ifdef MK2
oam oam (
  .clka(CLK), // input clka
  .wea(oam_wren), // input [0 : 0] wea
  .addra(oam_address), // input [7 : 0] addra
  .dina(oam_wrdata), // input [7 : 0] dina
  .douta(oam_rddata), // output [7 : 0] douta
  .clkb(CLK), // input clkb
  .web(oam_b_wren), // input [0 : 0] web
  .addrb(oam_b_address), // input [7 : 0] addrb
  .dinb(dbg_oam_wrdata), // input [7 : 0] dinb
  .doutb(dbg_oam_rddata) // output [7 : 0] doutb
);
`endif
`ifdef MK3
oam oam (
  .clock(CLK), // input clka
  .wren_a(oam_wren), // input [0 : 0] wea
  .address_a(oam_address), // input [7 : 0] addra
  .data_a(oam_wrdata), // input [7 : 0] dina
  .q_a(oam_rddata), // output [7 : 0] douta
  .wren_b(oam_b_wren), // input [0 : 0] web
  .address_b(oam_b_address), // input [7 : 0] addrb
  .data_b(dbg_oam_wrdata), // input [7 : 0] dinb
  .q_b(dbg_oam_rddata) // output [7 : 0] doutb
);
`endif

// MODE_H/V/O/D are defined with the other STAT bit names near the top.

`define OBJ_FIFO_PIXEL  1:0
`define OBJ_FIFO_PRI    2:2
`define OBJ_FIFO_PAL    3:3

parameter
  ST_PPU_OFF     = 13'b0000000000001,
  ST_PPU_FRM_NEW = 13'b0000000000010,
  ST_PPU_OAM_NEW = 13'b0000000000100,
  ST_PPU_OAM_POS = 13'b0000000001000,
  ST_PPU_PIX_NEW = 13'b0000000010000,
  ST_PPU_PIX_MAP = 13'b0000000100000,
  ST_PPU_PIX_DT0 = 13'b0000001000000,
  ST_PPU_PIX_DT1 = 13'b0000010000000,
  ST_PPU_PIX_OB0 = 13'b0000100000000,
  ST_PPU_PIX_OB1 = 13'b0001000000000,
  ST_PPU_PIX_OB2 = 13'b0010000000000,
  ST_PPU_HBL     = 13'b0100000000000,
  ST_PPU_VBL     = 13'b1000000000000;

reg  [12:0] ppu_state_r;

reg  [8:0]  ppu_dot_ctr_r;
reg  [5:0]  ppu_tile_ctr_r;   // 32 tiles with 8 pixels in a 256 pixel source tile data.  extra bit covers negative values
reg  [7:0]  ppu_pix_ctr_r;
reg  [7:0]  ppu_scanline_r;
reg  [2:0]  ppu_m0_scx_r;      // SCX & 7 as sampled at the start of mode 3 (the mode-3 penalty)
reg         hdma_win_r;        // the H-Blank window H-Blank triggers and park wakes look at

wire        ppu_vram_active = |(ppu_state_r & (ST_PPU_PIX_NEW | ST_PPU_PIX_MAP | ST_PPU_PIX_DT0 | ST_PPU_PIX_DT1 | ST_PPU_PIX_OB0 | ST_PPU_PIX_OB1 | ST_PPU_PIX_OB2));
wire        ppu_oam_active  = ~|(ppu_state_r & (ST_PPU_OFF | ST_PPU_HBL | ST_PPU_VBL));
assign      PPU_vblank      = |(ppu_state_r & ST_PPU_VBL);

reg         ppu_first_frame_r;

reg  [7:0]  ppu_oam_address_r;
reg  [7:0]  ppu_oam_rddata_r;
reg  [7:0]  ppu_oam_data_r;
reg  [12:0] ppu_vram_address_r;
reg  [7:0]  ppu_vram_data_r;

// OAM lookup table
`ifdef SGB_SPR_INCREASE
`define NUM_OAM_LUT 16
reg  [5:0]  ppu_oam_lut_cnt_r;
wire        ppu_feat_spr_increase = FEAT[`SGB_FEAT_SPR_INCREASE];
wire        ppu_oam_lut_full = ppu_feat_spr_increase ? ppu_oam_lut_cnt_r[4] : (ppu_oam_lut_cnt_r[3] & ppu_oam_lut_cnt_r[1]);
`else
`define NUM_OAM_LUT 10
reg  [3:0]  ppu_oam_lut_cnt_r;
wire        ppu_oam_lut_full = ppu_oam_lut_cnt_r[3] & ppu_oam_lut_cnt_r[1];
`endif
reg  [3:0]  ppu_oam_lut_ypos_r[`NUM_OAM_LUT-1:0];
reg  [7:0]  ppu_oam_lut_xpos_r[`NUM_OAM_LUT-1:0];
reg  [5:0]  ppu_oam_lut_index_r[`NUM_OAM_LUT-1:0];
wire        ppu_oam_end      = ppu_oam_address_r[7] & &ppu_oam_address_r[4:2]; // 159 + 1 = 160 bytes (40 entries of 4 bytes each)

// window
reg  [7:0]  ppu_pix_win_line_r;
reg  [4:0]  ppu_pix_win_tile_r;
wire [8:0]  ppu_pix_wx_m7      = {1'b0,REG_WX_r} - 7;
wire        ppu_pix_win_active = REG_LCDC_r[`LCDC_WD_EN] && ppu_scanline_r >= REG_WY_r && $signed(ppu_tile_ctr_r[5:0]) >= $signed(ppu_pix_wx_m7[8:3]);
reg         ppu_pix_win_active_r;

wire [7:0]  ppu_pix_row      = ppu_scanline_r + REG_SCY_r;
wire [4:0]  ppu_pix_col      = ppu_tile_ctr_r[4:0] + REG_SCX_r[7:3] + (|REG_SCX_r[2:0] ? 1 : 0);
wire [7:0]  ppu_pix_win_row  = ppu_pix_win_line_r;
wire [4:0]  ppu_pix_win_col  = ppu_pix_win_tile_r[4:0];

wire [1:0]  ppu_tile_ctr_next = ppu_tile_ctr_r[1:0] + 1;
wire        ppu_tile_dummy = ppu_tile_ctr_r[4] & ppu_tile_ctr_r[3];

wire        ppu_pix_end = ppu_pix_ctr_r[7] & ppu_pix_ctr_r[5];
// The dot is the 1x enable whatever KEY1 says (contract section 2): the LCD
// does not speed up, and the bridge's frame_dot_ctr and the genlock loop are
// built on 70224 of these per frame.
wire        ppu_dot_edge = CLK_PPU_EDGE;
wire        ppu_dot_end  = &ppu_dot_ctr_r[8:6] & &ppu_dot_ctr_r[2:0];             // 455+1 = 456 dots
wire        ppu_vis_end  = ppu_scanline_r[7] & &ppu_scanline_r[3:0];                          // 143+1 = 144 lines
wire        ppu_disp_end = ppu_scanline_r[7] & ppu_scanline_r[4] & ppu_scanline_r[3] & ppu_scanline_r[0]; // 153+1 = 154 lines
wire        ppu_tile_end = ~ppu_tile_dummy & ppu_tile_ctr_r[4] & &ppu_tile_ctr_r[1:0];      // 159+1 = 160 pixels, 19+1 = 20 tiles

wire        ppu_fifo_data = ~ppu_tile_dummy && ppu_pix_ctr_r[5:3] != ppu_tile_ctr_r[2:0] && ~ppu_pix_end;

// Where mode 0 lands for the CPU.  The end of mode 3 is taken off the pixel
// counter: pixel 152 + (SCX & 7) is being output on dot 250 + (SCX & 7), and
// ppu_m0_pre marks the dot edge that ends it (the edge that begins dot
// 251 + SCX).  This pipeline does not stretch mode 3 with SCX by itself, so
// everything the CPU can observe about "mode 3 is over" hangs off this one
// event, each at its own distance:
//
//   +1  STAT shows mode 0, and OAM / VRAM open up for the CPU (reads AND
//       writes): a read whose M-cycle begins on dot 252 + SCX is the first to
//       see mode 0.  mooneye intr_2_mode0_timing / intr_2_oam_ok_timing (read
//       on dot 251 -> 3 / locked, on dot 255 -> 0 / open, counted from the
//       mode-2 interrupt of a HALTed CPU), gambatte m2int_m3stat_1/_2,
//       oam_access/postread_*, vram_m3/postread_*, vramw_m3end_* (scx 0/2/3/5
//       pin the +1).  gambatte gates STAT, OAM and VRAM, reads and writes, on
//       the same `cc + 2 >= m0Time`; its cc is half a dot at double speed, so
//       there the point is one dot later (+2; vramw_m3end_scx5_ds_*).
//   +2  the H-Blank window of the HDMA engine opens (hdma_win_r): gambatte's
//       m0Time, the block being served at the first instruction boundary at
//       or after it.  hdma_start_{,scx2,scx3,scx5,ly0}_1/_2 and their _ds
//       twins all close with +2 and with nothing else (+0 starts scx2/scx3 one
//       M-cycle early, +3 starts scx5 one late).
//   +3  the mode-0 STAT interrupt is raised (IF one dot later): mooneye
//       intr_2_0_timing needs it to miss the instruction boundary on dot 255
//       and take the one on 259, gambatte m2int_m0irq_1/_2 read IF = 0 on dot
//       251 and IF = 2 on 255.  With this CPU's interrupt sampling (IF is
//       looked at on the handoff edge itself) +3 is the only value that
//       satisfies both.
//   +5  the CGB palettes open up (BCPD/OCPD), four dots after STAT: gambatte
//       cgbpAccessible `cc >= m0Time + 2`, cgbpal_m3/cgbpal_m3end_* (+6 at
//       double speed, measured with the same four dots).
//
// The old rules -- mode 0 at dot 264 once the FIFO had drained, the interrupt
// and the OAM/VRAM release on the entry into ST_PPU_HBL (dot 250, whatever
// SCX was) -- were what the `_2` half of gdma_cycles / hdma_cycles / *_m3stat
// and every postread_*_1 tripped on, at both speeds.
wire        ppu_m0_pre  = ppu_fifo_data & (ppu_pix_ctr_r == (8'd152 + {5'd0, ppu_m0_scx_r}))
                        & ~|(ppu_state_r & ST_PPU_OFF);
reg  [5:0]  ppu_m0_dly_r;      // ppu_m0_pre, one to six dots later
reg         ppu_m0_ds_r;       // KEY1 speed as it stood on the dot of ppu_m0_pre
reg         ppu_m0_irq_r;      // the mode-0 interrupt condition (level, until the line ends)
reg         ppu_pal_tail_r;    // STAT already shows mode 0, the palettes are still closed
// The taps are one-dot pulses, so which tap counts is decided ONCE, with the
// event: a 2x -> 1x switch landing between two taps must not be able to skip
// the pulse (STAT would sit in mode 3, and the CPU locked out of VRAM/OAM,
// until the next line) nor a 1x -> 2x one to take it twice.
wire        ppu_m0_vis  = ppu_m0_ds_r ? ppu_m0_dly_r[1] : ppu_m0_dly_r[0];
wire        ppu_m0_win  = ppu_m0_dly_r[1];
wire        ppu_m0_irq  = ppu_m0_irq_r | ppu_m0_dly_r[2];
wire        ppu_m0_pal  = ppu_m0_ds_r ? ppu_m0_dly_r[5] : ppu_m0_dly_r[4];

// What the CPU is locked out of while STAT still shows mode 3.  The PPU's own
// use of the two memories ends a dot or two earlier (ppu_vram_active /
// ppu_oam_active drop when the last fetch is done); the CPU side follows STAT.
wire        ppu_cpu_lock = REG_LCDC_r[`LCDC_DS_EN] & (REG_STAT_r[`STAT_MODE] == `MODE_D);
assign      PPU_PAL_lock = ppu_cpu_lock | ppu_pal_tail_r;

// LY as the CPU reads it: the next line's number from six dots before the
// line ends at 1x (five at 2x), gambatte video.h getLyReg() -- lycint_ly_1/_2
// read 5 on dot 451 and 6 on dot 455 of line 5, and the LY_TEST rows of age
// spsw-mode0 (two reads 16 dots apart straddling the increment) put the 1x
// edge at dot 450.  Line 153 is the exception (it already reads 0 from early
// in the line, handled with REG_LY_r itself), and so is the LCD being off.
assign      PPU_LY_read = (REG_LCDC_r[`LCDC_DS_EN] & ~|(ppu_state_r & ST_PPU_OFF) & (ppu_scanline_r != 8'd153)
                           & (ppu_dot_ctr_r >= (cpu_speed_r ? 9'd451 : 9'd450)))
                        ? REG_LY_r + 8'd1 : REG_LY_r;

wire [2:0]  ppu_bgw_fifo_index_start = (ppu_pix_win_active_r ? REG_WX_r[2:0] : ~REG_SCX_r[2:0]) + 1;
reg  [1:0]  ppu_bgw_fifo_r[31:0]; // 4 [tiles] * 8 [pixels/tile] * 2 [bpp]
reg  [7:0]  ppu_pix_bgw_data_r;

reg  [3:0]  ppu_obj_fifo_r[31:0]; // 4 [tiles] * 8 [pixels/tile] * 1+1+2[bpp,pri,pal]
reg  [7:0]  ppu_obj_fifo_transparent_r;
reg  [7:0]  ppu_pix_obj_data_r;

wire        ppu_hsync    = ppu_dot_edge & ppu_dot_end;
wire        ppu_vsync    = ppu_dot_edge & ppu_dot_end & ppu_disp_end;

reg         ppu_pix_phase_r;
reg         ppu_vblank_pulse_r;
reg         ppu_vblank_seen_r;
reg         ppu_stat_active_r;
reg  [7:0]  ppu_stat_match_r;
reg         ppu_dot_start_r;
reg         ppu_stat_write_r;

// OAM LUT lookup operation
reg         ppu_oam_lut_found;
reg  [3:0]  ppu_oam_lut_match;
// The sensitivity list used to name ppu_oam_lut_xpos_r[0] through [9] one by
// one, which is exactly NUM_OAM_LUT entries only while the LUT has ten of
// them: an implicit dependence of a combinational block on a `define.
always @(*) begin
  ppu_oam_lut_match = 4'hF;
  ppu_oam_lut_found = 0;
  for (i = 0; i < `NUM_OAM_LUT; i = i + 1) begin
    if (ppu_oam_lut_xpos_r[i][7:3] == ppu_tile_ctr_r[4:0] && ~ppu_oam_lut_found) begin
      ppu_oam_lut_match = i[3:0];
      ppu_oam_lut_found = 1;
    end
  end
end

reg         ppu_pix_oam_lut_found_r;
reg  [3:0]  ppu_pix_oam_lut_match_r;
reg  [7:0]  ppu_pix_oam_tile_num_r;
reg  [7:0]  ppu_pix_oam_flag_r;
reg  [7:0]  ppu_oam_obj_xpos_r;
wire [3:0]  ppu_pix_obj_row = (ppu_scanline_r[3:0] - ppu_oam_lut_ypos_r[ppu_pix_oam_lut_match_r][3:0]) ^ {4{ppu_pix_oam_flag_r[6]}};

reg         ppu_bgw_fifo_wr_req_r;
reg  [4:0]  ppu_bgw_fifo_wr_req_index_r;
reg  [1:0]  ppu_bgw_fifo_wr_req_data_r[7:0];
reg         ppu_bgw_fifo_wr_active_r;
reg  [4:0]  ppu_bgw_fifo_wr_index_r;
reg  [2:0]  ppu_bgw_fifo_wr_cnt_r;
reg  [1:0]  ppu_bgw_fifo_wr_data_r;

reg         ppu_obj_fifo_wr_req_r;
reg  [4:0]  ppu_obj_fifo_wr_req_index_r;
reg  [3:0]  ppu_obj_fifo_wr_req_data_r[7:0];
reg         ppu_obj_fifo_wr_active_r;
reg  [4:0]  ppu_obj_fifo_wr_index_r;
reg  [2:0]  ppu_obj_fifo_wr_cnt_r;
reg  [3:0]  ppu_obj_fifo_wr_data_r;
reg         ppu_obj_fifo_wr_req_clear_r;

//-------------------------------------------------------------------
// C6: the CGB half of the pixel pipeline
//-------------------------------------------------------------------
// The timing PPU above keeps a DMG pixel: 2 bits per FIFO slot for BG/window
// and {pal, pri, 2 bits} for OBJ.  The CGB needs, per slot, the BG map
// attribute (palette, priority) and, per OBJ pixel, the CGB palette and the
// OAM index (CGB object priority is OAM order, not X).  They ride in PARALLEL
// arrays written by the same two write engines at the same index, so none of
// the FIFO timing changes.  Attribute fetch: VRAM bank 1 at the map address,
// which port A of vram1 already presents -- both banks are addressed by the
// same vram_address, so the attribute costs no extra VRAM cycle.
// References: Pan Docs (CGB BG map attributes, OBJ attributes, "BG-to-OBJ
// Priority in CGB Mode", "Object priority"), SameBoy display.c (MIT).
reg  [7:0]  c6_bg_attr_r;                 // map attribute of the tile being fetched
reg  [3:0]  c6_bgw_req_attr_r;            // {pri, pal} handed to the write engine
reg  [3:0]  c6_bgw_wr_attr_r;
// ramstyle "logic": left to itself Quartus puts each of these 32-entry arrays
// in an M9K of its own (first pilot fit: 2 of the 56 blocks), and M9K is the
// resource the C6 runs out of.
(* ramstyle = "logic" *) reg  [3:0]  c6_bg_attr_fifo_r[31:0];      // {pri, pal[2:0]} per BG FIFO slot
reg  [8:0]  c6_obj_req_meta_r;            // {oam index, cgb pal}
reg  [8:0]  c6_obj_wr_meta_r;
(* ramstyle = "logic" *) reg  [8:0]  c6_obj_meta_fifo_r[31:0];     // {oam index[5:0], cgb pal[2:0]} per OBJ slot

// CGB object priority: the lower OAM index wins, wherever the sprites start.
// OPRI = 1 (and DMG compatibility) keep the X order the engine implements.
wire        c6_oam_order = cgb_mode & ~REG_OPRI_r;
wire [4:0]  c6_ow_i      = ppu_obj_fifo_wr_index_r[4:0];
wire        c6_ow_free   = (ppu_obj_fifo_r[c6_ow_i][1:0] == 2'd0);
wire        c6_ow_wins   = c6_oam_order & |ppu_obj_fifo_wr_data_r[1:0]
                         & (c6_obj_wr_meta_r[8:3] < c6_obj_meta_fifo_r[c6_ow_i][8:3]);
wire        c6_ow_we     = ppu_obj_fifo_wr_req_clear_r | c6_ow_free | c6_ow_wins;

// VRAM bank of the BG tile being fetched (attribute bit 3) and of the sprite
// (OAM flag bit 3).  Both are forced to bank 0 in compatibility mode.
wire [7:0]  c6_bg_tile_rd  = c6_bg_attr_r[3] ? vram1_rddata : vram0_rddata;
wire [7:0]  c6_obj_tile_rd = (cgb_mode & ppu_pix_oam_flag_r[3]) ? vram1_rddata : vram0_rddata;
wire [2:0]  c6_bg_row      = (ppu_pix_win_active_r ? ppu_pix_win_row[2:0] : ppu_pix_row[2:0]) ^ {3{c6_bg_attr_r[6]}};

assign      VRAM_data = vram_rddata;
assign      OAM_data  = oam_rddata;

assign      PPU_VRAM_active  = ppu_vram_active;
assign      PPU_VRAM_address = ppu_vram_address_r;
assign      PPU_OAM_active   = ppu_oam_active;
assign      PPU_OAM_address  = ppu_oam_address_r;
assign      PPU_REG_vblank   = ppu_vblank_pulse_r;
assign      PPU_REG_lcd_stat = ~ppu_stat_active_r & |ppu_stat_match_r;

assign      PPU_MCT_vram_active = ppu_vram_active | ppu_cpu_lock;
assign      PPU_MCT_oam_active  = ppu_oam_active  | ppu_cpu_lock;

wire  [1:0] ppu_bgw_index = ppu_bgw_fifo_r[ppu_pix_ctr_r[4:0]][1:0];
wire  [1:0] ppu_obj_index = ppu_obj_fifo_r[ppu_pix_ctr_r[4:0]][1:0];
reg         ppu_obj_pal;
reg         ppu_obj_pri;

// Xilinx compiler can silently fail if we don't expand out the obj fifo reads in a case statement.
always @(ppu_pix_ctr_r,
         ppu_obj_fifo_r[0 ][3:2],ppu_obj_fifo_r[1 ][3:2],ppu_obj_fifo_r[2 ][3:2],ppu_obj_fifo_r[3 ][3:2],ppu_obj_fifo_r[4 ][3:2],ppu_obj_fifo_r[5 ][3:2],ppu_obj_fifo_r[6 ][3:2],ppu_obj_fifo_r[7 ][3:2],
         ppu_obj_fifo_r[8 ][3:2],ppu_obj_fifo_r[9 ][3:2],ppu_obj_fifo_r[10][3:2],ppu_obj_fifo_r[11][3:2],ppu_obj_fifo_r[12][3:2],ppu_obj_fifo_r[13][3:2],ppu_obj_fifo_r[14][3:2],ppu_obj_fifo_r[15][3:2],
         ppu_obj_fifo_r[16][3:2],ppu_obj_fifo_r[17][3:2],ppu_obj_fifo_r[18][3:2],ppu_obj_fifo_r[19][3:2],ppu_obj_fifo_r[20][3:2],ppu_obj_fifo_r[21][3:2],ppu_obj_fifo_r[22][3:2],ppu_obj_fifo_r[23][3:2],
         ppu_obj_fifo_r[24][3:2],ppu_obj_fifo_r[25][3:2],ppu_obj_fifo_r[26][3:2],ppu_obj_fifo_r[27][3:2],ppu_obj_fifo_r[28][3:2],ppu_obj_fifo_r[29][3:2],ppu_obj_fifo_r[30][3:2],ppu_obj_fifo_r[31][3:2]
         ) begin
  case (ppu_pix_ctr_r[4:0])
    0:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[0 ][3:2];
    1:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[1 ][3:2];
    2:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[2 ][3:2];
    3:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[3 ][3:2];
    4:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[4 ][3:2];
    5:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[5 ][3:2];
    6:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[6 ][3:2];
    7:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[7 ][3:2];
    8:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[8 ][3:2];
    9:  {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[9 ][3:2];
    10: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[10][3:2];
    11: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[11][3:2];
    12: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[12][3:2];
    13: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[13][3:2];
    14: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[14][3:2];
    15: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[15][3:2];
    16: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[16][3:2];
    17: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[17][3:2];
    18: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[18][3:2];
    19: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[19][3:2];
    20: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[20][3:2];
    21: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[21][3:2];
    22: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[22][3:2];
    23: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[23][3:2];
    24: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[24][3:2];
    25: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[25][3:2];
    26: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[26][3:2];
    27: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[27][3:2];
    28: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[28][3:2];
    29: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[29][3:2];
    30: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[30][3:2];
    31: {ppu_obj_pri,ppu_obj_pal} = ppu_obj_fifo_r[31][3:2];
  endcase
end

// merge background and object indices and derive pixel values
assign      PPU_PIXEL = HLT_REQ_sync ? 2'b00
                      : (~|ppu_obj_index | (|ppu_bgw_index & ppu_obj_pri)) ? ( (ppu_bgw_index == 0) ? REG_BGP_r[1:0]
                                                                             : (ppu_bgw_index == 1) ? REG_BGP_r[3:2]
                                                                             : (ppu_bgw_index == 2) ? REG_BGP_r[5:4]
                                                                             :                        REG_BGP_r[7:6]
                                                                             )
                                                                           : ( (ppu_obj_index == 0) ? (ppu_obj_pal ? REG_OBP1_r[1:0] : REG_OBP0_r[1:0])
                                                                             : (ppu_obj_index == 1) ? (ppu_obj_pal ? REG_OBP1_r[3:2] : REG_OBP0_r[3:2])
                                                                             : (ppu_obj_index == 2) ? (ppu_obj_pal ? REG_OBP1_r[5:4] : REG_OBP0_r[5:4])
                                                                             :                        (ppu_obj_pal ? REG_OBP1_r[7:6] : REG_OBP0_r[7:6])
                                                                           );

//-------------------------------------------------------------------
// C6: the final CGB pixel (contract section 14.4)
//-------------------------------------------------------------------
// Same instant the DMG pixel above is taken (the dot edge that outputs FIFO
// slot ppu_pix_ctr_r), but resolved the CGB way:
//   * BG/window colour = CRAM BG palette (map attribute) entry of the 2-bit
//     index; OBJ colour = CRAM OBJ palette (OAM flag bits 2:0);
//   * OBJ colour 0 is transparent; BG wins over an opaque OBJ pixel only when
//     LCDC.0 = 1 AND the BG index is 1-3 AND (map attribute bit 7 OR OAM flag
//     bit 7) -- Pan Docs "BG-to-OBJ Priority in CGB Mode";
//   * in DMG compatibility mode the indices go through BGP/OBP0/OBP1 first and
//     select BG palette 0 / OBJ palette 0-1, and the DMG priority rule applies
//     (LCDC.0 = 0 already blanked the BG in the FIFO).
// The colour is read from the LIVE palette RAM through port B, two bytes on the
// two cycles after the dot, which is what a CGB does (BCPD/OCPD writes are
// blocked in mode 3, cram_blocked).
wire [3:0]  c6_bga = c6_bg_attr_fifo_r[ppu_pix_ctr_r[4:0]];
wire [8:0]  c6_obm = c6_obj_meta_fifo_r[ppu_pix_ctr_r[4:0]];

reg         c6_s0_r, c6_s1_r, c6_s2_r, c6_out_v_r;
reg  [7:0]  c6_s0_x_r, c6_s0_y_r, c6_s1_x_r, c6_s1_y_r, c6_s2_x_r, c6_s2_y_r, c6_out_x_r, c6_out_y_r;
reg  [1:0]  c6_s0_bgi_r, c6_s0_obi_r;
reg  [3:0]  c6_s0_bga_r;
reg  [2:0]  c6_s0_obp_r;
reg         c6_s0_objpri_r, c6_s0_dmgpal_r;
reg  [5:0]  c6_s1_idx_r;
reg  [7:0]  c6_lo_r;
reg  [14:0] c6_out_bgr_r;

wire        c6_obj_op  = |c6_s0_obi_r;
wire        c6_bg_op   = |c6_s0_bgi_r;
wire        c6_bg_wins = cgb_mode ? (~c6_obj_op | (REG_LCDC_r[`LCDC_BG_EN] & c6_bg_op & (c6_s0_bga_r[3] | c6_s0_objpri_r)))
                                  : (~c6_obj_op | (c6_bg_op & c6_s0_objpri_r));
wire [7:0]  c6_obp     = c6_s0_dmgpal_r ? REG_OBP1_r : REG_OBP0_r;
wire [1:0]  c6_bgcol   = cgb_mode ? c6_s0_bgi_r
                       : (c6_s0_bgi_r == 2'd0) ? REG_BGP_r[1:0] : (c6_s0_bgi_r == 2'd1) ? REG_BGP_r[3:2]
                       : (c6_s0_bgi_r == 2'd2) ? REG_BGP_r[5:4] : REG_BGP_r[7:6];
wire [1:0]  c6_obcol   = cgb_mode ? c6_s0_obi_r
                       : (c6_s0_obi_r == 2'd1) ? c6_obp[3:2] : (c6_s0_obi_r == 2'd2) ? c6_obp[5:4] : c6_obp[7:6];
// {is_obj, palette, colour}; the byte within the entry is the last CRAM bit.
wire [5:0]  c6_idx     = c6_bg_wins ? {1'b0, (cgb_mode ? c6_s0_bga_r[2:0] : 3'd0), c6_bgcol}
                                    : {1'b1, (cgb_mode ? c6_s0_obp_r : {2'b00, c6_s0_dmgpal_r}), c6_obcol};

assign      c6_cram_req  = c6_s0_r | c6_s1_r;
assign      c6_cram_addr = c6_s1_r ? {c6_s1_idx_r, 1'b1} : {c6_idx, 1'b0};

always @(posedge CLK) begin
  c6_s0_r <= ppu_dot_edge & DBG_advance & ppu_fifo_data & ~|(ppu_state_r & ST_PPU_OFF);
  if (ppu_dot_edge) begin
    c6_s0_x_r      <= ppu_pix_ctr_r;
    c6_s0_y_r      <= ppu_scanline_r;
    c6_s0_bgi_r    <= ppu_bgw_index;
    c6_s0_bga_r    <= cgb_mode ? c6_bga : 4'h0;
    c6_s0_obi_r    <= ppu_obj_index;
    c6_s0_obp_r    <= c6_obm[2:0];
    c6_s0_objpri_r <= ppu_obj_pri;
    c6_s0_dmgpal_r <= ppu_obj_pal;
  end
  // cycle s0: low byte addressed.  s1: high byte addressed, low byte arrives.
  // s2: high byte arrives.
  c6_s1_r     <= c6_s0_r;
  c6_s1_idx_r <= c6_idx;
  c6_s1_x_r   <= c6_s0_x_r;
  c6_s1_y_r   <= c6_s0_y_r;
  c6_s2_r     <= c6_s1_r;
  c6_s2_x_r   <= c6_s1_x_r;
  c6_s2_y_r   <= c6_s1_y_r;
  if (c6_s1_r) c6_lo_r <= cram_q_b_r;
  c6_out_v_r  <= c6_s2_r;
  if (c6_s2_r) begin
    c6_out_bgr_r <= {cram_q_b_r[6:0], c6_lo_r};
    c6_out_x_r   <= c6_s2_x_r;
    c6_out_y_r   <= c6_s2_y_r;
  end
end

assign      C6_PX_VALID = c6_out_v_r;
assign      C6_PX_X     = c6_out_x_r;
assign      C6_PX_Y     = c6_out_y_r;
assign      C6_PX_BGR   = c6_out_bgr_r;

assign      PPU_DOT_EDGE    = ppu_dot_edge;
assign      PPU_HSYNC_EDGE  = ppu_hsync;
assign      PPU_VSYNC_EDGE  = ppu_vsync;
assign      PPU_PIXEL_VALID = ppu_fifo_data;

// Live LCDC.7 for the bridge status block (contract section 5, flags0 bit 0).
// Pure tap: no logic here reads it back.
assign      PPU_LCD_ON      = REG_LCDC_r[`LCDC_DS_EN];

// Snapshot instant (contract section 7): the first dot of mode 2 on line 0.
// ST_PPU_OAM_NEW is one dot wide and is where the PPU publishes MODE_O, so
// this is that dot and not the one before or after it.
assign      TAP_LY0         = ppu_dot_edge & DBG_advance
                            & |(ppu_state_r & ST_PPU_OAM_NEW) & ~|ppu_scanline_r;
// Where a mid-frame write falls (contract section 8) is read off the PPU's OWN
// line and state, not off the registers the CPU sees:
//  * REG_LY_r already reads 0 from early in line 153, so "LY < 144" would file
//    every write of the last V-Blank line as a write to line 0 -- at the END of
//    the log of the frame that has just been drawn.  ppu_scanline_r is the line.
//  * REG_STAT_r is published one dot late at both ends of mode 2 (MODE_O is
//    written in ST_PPU_OAM_NEW's body, MODE_D in ST_PPU_PIX_NEW's).  A write on
//    the first dot of a line would read mode 0 and be filed one line late --
//    and a game that writes from a mode 0 handler lands on the same dot every
//    line.  "Before mode 3 of this line" is "still scanning OAM for it".
//  * On line 0 the frame's log starts AT the snapshot (TAP_LY0, the edge that
//    ends the ST_PPU_OAM_NEW dot).  A write before that edge is already in the
//    snapshot and the live buffer still belongs to the previous frame.
wire        mf_oam_scan     = |(ppu_state_r & (ST_PPU_OAM_NEW | ST_PPU_OAM_POS));
wire        mf_pre_ly0      = ~|ppu_scanline_r & |(ppu_state_r & (ST_PPU_FRM_NEW | ST_PPU_OAM_NEW));
assign      TAP_MF_VALID    = mf_wr & (ppu_scanline_r < 8'd144) & ~mf_pre_ly0;
assign      TAP_MF_LY       = ppu_scanline_r;
assign      TAP_MF_BEFORE_MODE3 = mf_oam_scan;
// Entry into LY=144, i.e. the dot the H-Blank of line 143 hands over to VBL.
assign      TAP_VBLANK      = ppu_dot_edge & DBG_advance
                            & |(ppu_state_r & ST_PPU_HBL) & ppu_dot_end & ppu_vis_end;

reg         dbg_state_valid_r;
reg  [7:0]  dbg_reg_ly_r;
reg  [8:0]  dbg_dot_ctr_r;
reg  [8:0]  dbg_dot_ctr_next_r;
reg         dbg_oam_active_r;
reg         dbg_vram_active_r;
reg         dbg_dma_active_r;
reg  [7:0]  dbg_ppu_stat_match_r;
reg  [8:0]  dbg_ppu_stat_dot_ctr_r;

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    REG_STAT_r[`STAT_MODE]      <= `MODE_H;
    REG_STAT_r[`STAT_LYC_MATCH] <= 0;
    REG_LY_r                    <= 0;

    ppu_scanline_r <= 0;

    ppu_tile_ctr_r <= 0;
    ppu_pix_ctr_r  <= 0;

    ppu_dot_ctr_r <= 0;

    ppu_state_r <= ST_PPU_OFF;

    ppu_vblank_pulse_r <= 0;
    ppu_stat_active_r <= 0;
    ppu_stat_match_r <= 0;

    ppu_bgw_fifo_wr_req_r <= 0;
    ppu_bgw_fifo_wr_active_r <= 0;
    ppu_obj_fifo_wr_req_r <= 0;
    ppu_obj_fifo_wr_active_r <= 0;

    ppu_stat_write_r <= 0;

    ppu_m0_scx_r    <= 3'd0;
    ppu_m0_dly_r    <= 6'd0;
    ppu_m0_ds_r     <= 1'b0;
    ppu_m0_irq_r    <= 1'b0;
    ppu_pal_tail_r  <= 1'b0;
    hdma_win_r      <= 1'b0;

    dbg_dot_ctr_next_r <= 0;
    dbg_dot_ctr_r      <= 0;
  end
  else begin
    // The active scanline pixel output is composed of 3 distinct phases:
    // - OAM test and buffer (~80 dot clocks)
    // - pixel output        (166-180 dot clocks)
    // - h-blank             (remaining dot clocks in 456 scanline)
    //
    // A VRAM access takes 2 dot clocks and an OAM access takes 1 dot clock.
    //
    // The phases of the display rendering are:
    // 0 - hblank/display disable
    // 1 - vblank
    // 2 - OAM testing
    // 3 - display
    //
    // Sequencing:
    // - [OFF->VBL] The display is enabled by the sofware during the vblank region.  This is the initial condition.
    //
    // - [VBL->OAM] OAM testing is performed to find up to 10 matching valid sprites in the scanline
    //   - Tests are performed on ypos and need to account for 8x8 vs 8x16 size.
    //   - There are 40 sprites to test and OAM is assumed to take 1 dot clock to read.  This is separated into a
    //     ypos read clock followed by a test/xpos read clock.
    //   - A lookup table is kept with xpos and a pointer to the associated OAM entry.
    // - [OAM->PIX] PIX reads the BG or window MAP, 2 bytes of 8 pixels, and then tests OAM matches on the current row.
    // - [PIX->HBL] HBL is when we are in hblank
    // - [HBL->OAM] From HBL we can transition back to OAM if the new line is visible
    // - [HBL->VBL] From HBL we can transition to VBL (vblank) if the visible lines are complete

    // debug
    if (CLK_BUS_EDGE) begin
      if      (exe_advance_r) begin
        dbg_state_valid_r <= 0;
      end
      else if (~dbg_state_valid_r) begin
        dbg_state_valid_r <= 1;

        dbg_reg_ly_r       <= REG_LY_r;
        {dbg_dot_ctr_r,dbg_dot_ctr_next_r} <= {dbg_dot_ctr_next_r,ppu_dot_ctr_r};
        dbg_oam_active_r   <= PPU_OAM_active;
        dbg_vram_active_r  <= PPU_VRAM_active;
        dbg_dma_active_r   <= DMA_active;
      end
    end

    // The STAT-write glitch (a write to STAT briefly enables every source) is
    // a quirk of the monochrome SoCs; a CGB does not have it (Pan Docs, "STAT
    // writing bug").  It is kept for DMG compatibility mode, where the games
    // that lean on it live (Road Rash, Zerd no Densetsu), and dropped in CGB
    // mode: gambatte's *_ds tests set STAT and never clear IF, and took the
    // glitch interrupt instead of the one they were waiting for
    // (miscmstatirq/*statwirq_trigger_*, m0enable/m0_enable_ds_*).
    if      (REG_req_val && REG_address == 8'h41) ppu_stat_write_r <= ~cgb_mode;
    else if (ppu_dot_edge)                        ppu_stat_write_r <= 0;

    // flop match
    ppu_pix_oam_lut_match_r <= ppu_oam_lut_match;
    ppu_pix_oam_lut_found_r <= ppu_oam_lut_found & ~(ppu_first_frame_r|~REG_LCDC_r[`LCDC_SP_EN] | DMA_active);

    ppu_oam_rddata_r <= oam_rddata;

    // Xilinx compiler (MK2) can silently fail if we don't expand out the pixel fifo writes in a case statement.  Same goes for the packet buffer in ICD and others.
    // This results in a lot of code verbosity, but it works.
    ppu_bgw_fifo_wr_req_r <= 0;
    if (ppu_bgw_fifo_wr_req_r) c6_bgw_wr_attr_r <= c6_bgw_req_attr_r;
    if (~ppu_bgw_fifo_wr_req_r & ppu_bgw_fifo_wr_active_r)
      c6_bg_attr_fifo_r[ppu_bgw_fifo_wr_index_r[4:0]] <= c6_bgw_wr_attr_r;
    if (ppu_obj_fifo_wr_req_r) c6_obj_wr_meta_r <= ppu_obj_fifo_wr_req_clear_r ? 9'h1F8 : c6_obj_req_meta_r;
    if (ppu_bgw_fifo_wr_req_r) begin
      ppu_bgw_fifo_wr_active_r <= 1;
      ppu_bgw_fifo_wr_index_r <= ppu_bgw_fifo_wr_req_index_r;
      ppu_bgw_fifo_wr_cnt_r <= 1;

      ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[0];
    end
    else if (ppu_bgw_fifo_wr_active_r) begin
      case (ppu_bgw_fifo_wr_index_r[4:0])
        0:  ppu_bgw_fifo_r[0 ] <= ppu_bgw_fifo_wr_data_r;
        1:  ppu_bgw_fifo_r[1 ] <= ppu_bgw_fifo_wr_data_r;
        2:  ppu_bgw_fifo_r[2 ] <= ppu_bgw_fifo_wr_data_r;
        3:  ppu_bgw_fifo_r[3 ] <= ppu_bgw_fifo_wr_data_r;
        4:  ppu_bgw_fifo_r[4 ] <= ppu_bgw_fifo_wr_data_r;
        5:  ppu_bgw_fifo_r[5 ] <= ppu_bgw_fifo_wr_data_r;
        6:  ppu_bgw_fifo_r[6 ] <= ppu_bgw_fifo_wr_data_r;
        7:  ppu_bgw_fifo_r[7 ] <= ppu_bgw_fifo_wr_data_r;
        8:  ppu_bgw_fifo_r[8 ] <= ppu_bgw_fifo_wr_data_r;
        9:  ppu_bgw_fifo_r[9 ] <= ppu_bgw_fifo_wr_data_r;
        10: ppu_bgw_fifo_r[10] <= ppu_bgw_fifo_wr_data_r;
        11: ppu_bgw_fifo_r[11] <= ppu_bgw_fifo_wr_data_r;
        12: ppu_bgw_fifo_r[12] <= ppu_bgw_fifo_wr_data_r;
        13: ppu_bgw_fifo_r[13] <= ppu_bgw_fifo_wr_data_r;
        14: ppu_bgw_fifo_r[14] <= ppu_bgw_fifo_wr_data_r;
        15: ppu_bgw_fifo_r[15] <= ppu_bgw_fifo_wr_data_r;
        16: ppu_bgw_fifo_r[16] <= ppu_bgw_fifo_wr_data_r;
        17: ppu_bgw_fifo_r[17] <= ppu_bgw_fifo_wr_data_r;
        18: ppu_bgw_fifo_r[18] <= ppu_bgw_fifo_wr_data_r;
        19: ppu_bgw_fifo_r[19] <= ppu_bgw_fifo_wr_data_r;
        20: ppu_bgw_fifo_r[20] <= ppu_bgw_fifo_wr_data_r;
        21: ppu_bgw_fifo_r[21] <= ppu_bgw_fifo_wr_data_r;
        22: ppu_bgw_fifo_r[22] <= ppu_bgw_fifo_wr_data_r;
        23: ppu_bgw_fifo_r[23] <= ppu_bgw_fifo_wr_data_r;
        24: ppu_bgw_fifo_r[24] <= ppu_bgw_fifo_wr_data_r;
        25: ppu_bgw_fifo_r[25] <= ppu_bgw_fifo_wr_data_r;
        26: ppu_bgw_fifo_r[26] <= ppu_bgw_fifo_wr_data_r;
        27: ppu_bgw_fifo_r[27] <= ppu_bgw_fifo_wr_data_r;
        28: ppu_bgw_fifo_r[28] <= ppu_bgw_fifo_wr_data_r;
        29: ppu_bgw_fifo_r[29] <= ppu_bgw_fifo_wr_data_r;
        30: ppu_bgw_fifo_r[30] <= ppu_bgw_fifo_wr_data_r;
        31: ppu_bgw_fifo_r[31] <= ppu_bgw_fifo_wr_data_r;
      endcase
      ppu_bgw_fifo_wr_index_r <= ppu_bgw_fifo_wr_index_r + 1;

      case (ppu_bgw_fifo_wr_cnt_r[2:0])
        0:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[0];
        1:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[1];
        2:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[2];
        3:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[3];
        4:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[4];
        5:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[5];
        6:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[6];
        7:  ppu_bgw_fifo_wr_data_r <= ppu_bgw_fifo_wr_req_data_r[7];
      endcase
      ppu_bgw_fifo_wr_cnt_r <= ppu_bgw_fifo_wr_cnt_r + 1;

      ppu_bgw_fifo_wr_active_r <= |ppu_bgw_fifo_wr_cnt_r;
    end

    ppu_obj_fifo_wr_req_r <= 0;
    if (ppu_obj_fifo_wr_req_r) begin
      ppu_obj_fifo_wr_active_r <= 1;
      ppu_obj_fifo_wr_index_r <= ppu_obj_fifo_wr_req_index_r;
      ppu_obj_fifo_wr_cnt_r <= 1;

      ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[0];
    end
    else if (ppu_obj_fifo_wr_active_r) begin
      // CGB: a later sprite also overwrites an opaque pixel if its OAM index
      // is lower (c6_ow_wins); the DMG rule is the c6_ow_free / clear half.
      if (c6_ow_we) begin
        ppu_obj_fifo_r[c6_ow_i][3:0] <= ppu_obj_fifo_wr_data_r[3:0];
        c6_obj_meta_fifo_r[c6_ow_i]  <= c6_obj_wr_meta_r;
      end
      ppu_obj_fifo_wr_index_r <= ppu_obj_fifo_wr_index_r + 1;

      case (ppu_obj_fifo_wr_cnt_r[2:0])
        0:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[0];
        1:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[1];
        2:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[2];
        3:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[3];
        4:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[4];
        5:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[5];
        6:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[6];
        7:  ppu_obj_fifo_wr_data_r <= ppu_obj_fifo_wr_req_data_r[7];
      endcase
      ppu_obj_fifo_wr_cnt_r <= ppu_obj_fifo_wr_cnt_r + 1;

      ppu_obj_fifo_wr_active_r <= |ppu_obj_fifo_wr_cnt_r;
    end

    // scanline/state rendering datapath
    if (ppu_dot_edge & DBG_advance) begin
      ppu_pix_phase_r <= 0;

      // read pointer advance
      if (ppu_fifo_data) ppu_pix_ctr_r <= ppu_pix_ctr_r + 1;

      ppu_vblank_pulse_r <= 0;
      ppu_stat_active_r <= |ppu_stat_match_r;
      if (~ppu_stat_active_r & |ppu_stat_match_r) begin
        dbg_ppu_stat_match_r <= ppu_stat_match_r;
        dbg_ppu_stat_dot_ctr_r <= ppu_dot_ctr_r;
      end

      ppu_oam_data_r <= ppu_oam_rddata_r;

      case (ppu_state_r)
        ST_PPU_OFF     : begin
          // clear display state
          REG_STAT_r[`STAT_MODE] <= `MODE_H;

          REG_LY_r         <= 0;
          ppu_scanline_r   <= 0;

          ppu_tile_ctr_r <= 0;
          ppu_pix_ctr_r  <= 0;

          ppu_first_frame_r <= 1;

          ppu_vblank_seen_r <= 0;

          ppu_stat_match_r <= 0;

          if (REG_LCDC_r[`LCDC_DS_EN]) ppu_state_r <= ST_PPU_FRM_NEW;
        end
        ST_PPU_FRM_NEW : begin
          // next frame
          ppu_pix_win_line_r <= 8'hFF;

          // TODO: should we flop WY here for current frame?

          REG_STAT_r[`STAT_MODE] <= `MODE_H;

          ppu_state_r <= ST_PPU_OAM_NEW;
        end
        ST_PPU_OAM_NEW : begin
          // start of new line

          // setup initial address
          ppu_oam_address_r <= 0;

          // use -16 (instead of -8) in order to put the xpos before the dummy tile for empty entries
          for (i = 0; i < `NUM_OAM_LUT; i = i + 1) ppu_oam_lut_xpos_r[i] <= 0-16;

          // initialize all entries to be invalid
          ppu_oam_lut_cnt_r <= 0;

          REG_STAT_r[`STAT_MODE] <= `MODE_O;

          // WARNING: this needs to only be one dot cycle to avoid multiple interrupts.  Or we need to guard the interrupt with the same condition.
          ppu_state_r <= ST_PPU_OAM_POS;
        end
        ST_PPU_OAM_POS : begin
          // read in xpos if ypos is on this line
          if (~ppu_oam_lut_full & ppu_pix_phase_r & ~DMA_active) begin
            if (ppu_oam_data_r <= (ppu_scanline_r + 16) && (ppu_scanline_r + 16) < (ppu_oam_data_r + (REG_LCDC_r[`LCDC_SP_SIZE] ? 16 : 8))) begin
              ppu_oam_lut_ypos_r[ppu_oam_lut_cnt_r]  <= ppu_oam_data_r[3:0];
              ppu_oam_lut_xpos_r[ppu_oam_lut_cnt_r]  <= ppu_oam_rddata_r - (|ppu_oam_rddata_r ? 8 : 16);
              ppu_oam_lut_index_r[ppu_oam_lut_cnt_r] <= ppu_oam_address_r[7:2];

              ppu_oam_lut_cnt_r <= ppu_oam_lut_cnt_r + 1;
            end
          end

          // calculate new address
          ppu_oam_address_r <= ppu_oam_address_r + (ppu_pix_phase_r ? 3 : 1);

          if (ppu_oam_end & ppu_pix_phase_r) begin
            ppu_state_r <= ST_PPU_PIX_NEW;
          end

          ppu_pix_phase_r <= ~ppu_pix_phase_r;
        end
        ST_PPU_PIX_NEW : begin
          // new visible scanline

          // reset counter/pointer state
          // Start at tile -1 to handle scrolling and window offsets
          ppu_tile_ctr_r <= 6'h3F;  // partial fifo write pointer
          ppu_pix_ctr_r <= 0;       // fifo read pointer

          ppu_pix_win_active_r <= 0;

          REG_STAT_r[`STAT_MODE] <= `MODE_D;
          ppu_m0_scx_r <= REG_SCX_r[2:0];

          ppu_state_r <= ST_PPU_PIX_MAP;
        end
        ST_PPU_PIX_MAP : begin
          // generate map address
          ppu_vram_address_r <= ppu_pix_win_active_r ? {1'b1,1'b1,REG_LCDC_r[`LCDC_WD_MAP_SEL],ppu_pix_win_row[7:3],ppu_pix_win_col[4:0]} : {1'b1,1'b1,REG_LCDC_r[`LCDC_BG_MAP_SEL],ppu_pix_row[7:3],ppu_pix_col[4:0]};
          ppu_vram_data_r <= vram_rddata;
          c6_bg_attr_r    <= cgb_mode ? vram1_rddata : 8'h00;

          if (ppu_pix_phase_r) ppu_state_r <= ST_PPU_PIX_DT0;

          ppu_pix_phase_r <= ~ppu_pix_phase_r;
        end
        ST_PPU_PIX_DT0 : begin
          // all BG tiles are consecutive 16B and naturally aligned
          ppu_vram_address_r <= {(~REG_LCDC_r[`LCDC_BG_TILE_SEL] & ~ppu_vram_data_r[7]),ppu_vram_data_r[7:0],c6_bg_row,1'b0};

          ppu_pix_bgw_data_r <= c6_bg_tile_rd;

          if (ppu_pix_phase_r) ppu_state_r <= ST_PPU_PIX_DT1;

          ppu_pix_phase_r <= ~ppu_pix_phase_r;
        end
        ST_PPU_PIX_DT1 : begin
          ppu_oam_address_r <= {ppu_oam_lut_index_r[ppu_pix_oam_lut_match_r],2'b10};
          ppu_vram_address_r <= {ppu_vram_address_r[12:1],1'b1};

          if (ppu_pix_phase_r) begin
            ppu_bgw_fifo_wr_req_r <= 1;

            // write bgw fifo with current pixel data
            ppu_bgw_fifo_wr_req_index_r <= {ppu_tile_ctr_r[1:0],ppu_bgw_fifo_index_start[2:0]};
            // CGB: LCDC.0 is the BG/OBJ master priority, not a BG enable, so
            // the tile is kept (the composition below applies the bit).
            // Attribute bit 5 = horizontal flip.
            for (i = 0; i < 8; i = i + 1) ppu_bgw_fifo_wr_req_data_r[i][0] <= (ppu_first_frame_r|(~REG_LCDC_r[`LCDC_BG_EN] & ~cgb_mode)) ? 1'b0 : (c6_bg_attr_r[5] ? ppu_pix_bgw_data_r[i] : ppu_pix_bgw_data_r[7-i]);
            for (i = 0; i < 8; i = i + 1) ppu_bgw_fifo_wr_req_data_r[i][1] <= (ppu_first_frame_r|(~REG_LCDC_r[`LCDC_BG_EN] & ~cgb_mode)) ? 1'b0 : (c6_bg_attr_r[5] ? c6_bg_tile_rd[i]         : c6_bg_tile_rd[7-i]);
            c6_bgw_req_attr_r <= {c6_bg_attr_r[7], c6_bg_attr_r[2:0]};

            // clear object fifo for next set of sprite tiles
            ppu_obj_fifo_wr_req_r <= 1;
            ppu_obj_fifo_wr_req_clear_r <= 1;
            ppu_obj_fifo_wr_req_index_r <= {ppu_tile_ctr_next[1:0],3'h0};
            for (i = 0; i < 8; i = i + 1) ppu_obj_fifo_wr_req_data_r[i][3:0] <= 0;
          end

          if (ppu_pix_phase_r) begin
            // determine if there is a transition to window.  If so, render the new active mode on top of the old by repeating the BGW tile fetch
            ppu_pix_win_active_r <= ppu_pix_win_active;
            if (ppu_pix_win_active_r ^ ppu_pix_win_active) ppu_pix_win_line_r <= ppu_pix_win_line_r + 1;
            if (ppu_pix_win_active_r ^ ppu_pix_win_active) ppu_pix_win_tile_r <= 0;

            ppu_state_r <= (ppu_pix_win_active_r ^ ppu_pix_win_active) ? ST_PPU_PIX_MAP : ST_PPU_PIX_OB0;
          end

          ppu_pix_phase_r <= ~ppu_pix_phase_r;
        end
        ST_PPU_PIX_OB0 : begin
          ppu_oam_obj_xpos_r <= ppu_oam_lut_xpos_r[ppu_pix_oam_lut_match_r];

          ppu_oam_address_r <= {ppu_oam_address_r[7:1],1'b1};

          if (~ppu_pix_phase_r) ppu_pix_oam_tile_num_r <= ppu_oam_rddata_r; else ppu_pix_oam_flag_r <= ppu_oam_rddata_r;

          if (ppu_pix_phase_r) begin
            if (~ppu_pix_oam_lut_found_r) ppu_tile_ctr_r <= ppu_tile_ctr_r + 1;
            if (~ppu_pix_oam_lut_found_r) ppu_pix_win_tile_r <= ppu_pix_win_tile_r + 1;

            ppu_state_r <= ppu_pix_oam_lut_found_r ? ST_PPU_PIX_OB1 : (ppu_tile_end ? ST_PPU_HBL : ST_PPU_PIX_MAP);
          end

          ppu_pix_phase_r <= ~ppu_pix_phase_r;
        end
        ST_PPU_PIX_OB1 : begin
          ppu_vram_address_r <= {1'b0,ppu_pix_oam_tile_num_r[7:1],(REG_LCDC_r[`LCDC_SP_SIZE] ? ppu_pix_obj_row[3] : ppu_pix_oam_tile_num_r[0]),ppu_pix_obj_row[2:0],1'b0};

          // read second half of tile
          ppu_pix_obj_data_r <= c6_obj_tile_rd;

          if (ppu_pix_phase_r) begin
            ppu_state_r <= ST_PPU_PIX_OB2;
          end

          ppu_pix_phase_r <= ~ppu_pix_phase_r;
        end
        ST_PPU_PIX_OB2 : begin
          ppu_vram_address_r <= {ppu_vram_address_r[12:1],1'b1};

          // clear match for second phase
          if (~ppu_pix_phase_r) ppu_oam_lut_xpos_r[ppu_pix_oam_lut_match_r] <= 0-16;

          if (ppu_pix_phase_r) begin
            // get address of next match
            ppu_oam_address_r <= {ppu_oam_lut_index_r[ppu_pix_oam_lut_match_r],2'b10};

            ppu_obj_fifo_wr_req_r <= 1;
            ppu_obj_fifo_wr_req_clear_r <= 0;
            ppu_obj_fifo_wr_req_index_r <= ppu_oam_obj_xpos_r[4:0];
            for (i = 0; i < 8; i = i + 1) ppu_obj_fifo_wr_req_data_r[i][0] <= ppu_pix_oam_flag_r[5] ? ppu_pix_obj_data_r[i] : ppu_pix_obj_data_r[7-i];
            for (i = 0; i < 8; i = i + 1) ppu_obj_fifo_wr_req_data_r[i][1] <= ppu_pix_oam_flag_r[5] ? c6_obj_tile_rd[i]     : c6_obj_tile_rd[7-i];
            // The OAM address still points at THIS sprite ({index, 2'b1x}) on
            // this edge: the next match is loaded into it by the statement
            // above in the same edge.
            c6_obj_req_meta_r <= {ppu_oam_address_r[7:2], cgb_mode ? ppu_pix_oam_flag_r[2:0] : 3'b000};
            for (i = 0; i < 8; i = i + 1) ppu_obj_fifo_wr_req_data_r[i][2] <= ppu_pix_oam_flag_r[4];
            for (i = 0; i < 8; i = i + 1) ppu_obj_fifo_wr_req_data_r[i][3] <= ppu_pix_oam_flag_r[7];

            ppu_state_r <= ST_PPU_PIX_OB0;
          end

          ppu_pix_phase_r <= ~ppu_pix_phase_r;
        end
        ST_PPU_HBL     : begin
          if (~ppu_fifo_data) begin
            ppu_tile_ctr_r <= 0;
            ppu_pix_ctr_r  <= 0;
            // STAT's mode 0 is written from ppu_m0_pre below, on the dot the
            // CPU has to see it on, not when the FIFO has drained.
          end

          ppu_vblank_seen_r <= 0;

          if (ppu_dot_end) begin
            ppu_state_r <= ppu_vis_end ? ST_PPU_VBL : ST_PPU_OAM_NEW;
          end
        end
        ST_PPU_VBL     : begin
          REG_STAT_r[`STAT_MODE] <= `MODE_V;

          if (~ppu_vblank_seen_r & ppu_dot_ctr_r[0]) begin
            // assert on dot clock 2
            ppu_vblank_pulse_r <= 1;

            ppu_vblank_seen_r <= 1;
          end

          ppu_first_frame_r <= 0;

          if (ppu_dot_end) begin
            if (ppu_disp_end) ppu_state_r <= ST_PPU_FRM_NEW;
          end
        end
      endcase

      if (~|(ppu_state_r & ST_PPU_OFF)) begin
        // It's possible for a write to happen on the last dot cycle which will cause us to miss a 1->0->1 transition.
        //
        // dot clk
        // 0 - LY_r
        // 1 - match
        // 2 - ppu_stat_active_r[0]
        // 3 - IF/earliest interrupt point
        // 3+?    - Wait for current instruction to finish. 4 * (0-6)
        // 3+?+20 - +20 = 5 * 4 dot clocks to take interrupt

        // P-M breaks if the transition to 0 on line 153 happens too early.
        // BMF expects REG_LY_r to transition from 153->0 early in the line.
        if (ppu_dot_end) ppu_scanline_r <= ppu_disp_end ? 0 : ppu_scanline_r + 1;
        if (ppu_dot_end) REG_LY_r <= ppu_disp_end ? 0 : REG_LY_r + 1; else if (&ppu_dot_ctr_r[3:2] & ppu_disp_end) REG_LY_r <= 0;

        // TODO: is the match clear necessary?  Seems like the use case for it originally was actually a STAT write spurious interrupt.
        // Definitely can't clear on the last line or it causes problems with double interrupts for LYC==0.
        REG_STAT_r[`STAT_LYC_MATCH] <= (REG_LY_r == REG_LYC_r && ~(ppu_dot_end & ~ppu_disp_end)) ? 1 : 0;

        // PBF limits IRQs by transitioning between enabled modes on the same cycle (M->O)
        // RR expects stat write to trigger spurious interrupt during V-Blank to make menu->game not lock.  144 V-Blank Int -> 147-148 Spurious STAT (V-Blank) Int -> 153 STAT (LY==LYC==0) Int -> 0 STAT (H-Blank) Int
        // The mode-0 source follows ppu_m0_irq (see ppu_m0_pre), not the state.
        ppu_stat_match_r[`STAT_INT_H_EN] <= (REG_STAT_r[`STAT_INT_H_EN] | ppu_stat_write_r) & ppu_m0_irq;
        ppu_stat_match_r[`STAT_INT_V_EN] <= (REG_STAT_r[`STAT_INT_V_EN] | ppu_stat_write_r) & |(ppu_state_r & ST_PPU_VBL);
        ppu_stat_match_r[`STAT_INT_O_EN] <= (REG_STAT_r[`STAT_INT_O_EN] | ppu_stat_write_r) & ((|ppu_scanline_r ? |(ppu_state_r & ST_PPU_OAM_NEW) : |(ppu_state_r & ST_PPU_FRM_NEW)) | (|(ppu_state_r & ST_PPU_VBL) & ~ppu_vblank_seen_r & ppu_dot_ctr_r[0]));  // pulse
        ppu_stat_match_r[`STAT_INT_M_EN] <= (REG_STAT_r[`STAT_INT_M_EN] | ppu_stat_write_r) & REG_STAT_r[`STAT_LYC_MATCH];
      end

      // Mode 0 and the H-Blank window (see ppu_m0_pre).  The STAT write is
      // last so it wins over the state machine's own writes in the same dot.
      // Flushed while the LCD is off: no tap in flight may outlive an OFF and
      // land in the first line of the next frame.
      ppu_m0_dly_r <= |(ppu_state_r & ST_PPU_OFF) ? 6'd0 : {ppu_m0_dly_r[4:0], ppu_m0_pre};
      if (ppu_m0_pre) ppu_m0_ds_r <= cpu_speed_r;
      if (ppu_dot_end | |(ppu_state_r & (ST_PPU_OFF | ST_PPU_VBL))) ppu_m0_irq_r <= 1'b0;
      else if (ppu_m0_dly_r[2])                                     ppu_m0_irq_r <= 1'b1;

      if (ppu_m0_pal | ppu_dot_end | |(ppu_state_r & (ST_PPU_OFF | ST_PPU_VBL))) ppu_pal_tail_r <= 1'b0;
      else if (ppu_m0_vis)                                                       ppu_pal_tail_r <= 1'b1;

      if (ppu_m0_vis & ~|(ppu_state_r & ST_PPU_OFF)) REG_STAT_r[`STAT_MODE] <= `MODE_H;

      if (ppu_m0_win & ~|(ppu_state_r & ST_PPU_OFF)) begin
        hdma_win_r <= 1'b1;
      end
      // The window closes on dot 454: a HALT whose fetch M-cycle began on dot
      // 450 parked inside it, one that began on 454 outside
      // (hdma_late_m0halt_1/_2); an HDMA5 write whose M-cycle begins on dot
      // 446 starts a block at once, on dot 450 it waits for the next line
      // (hdma_late_enable_1/_2).
      else if ((ppu_dot_ctr_r == 9'd453) | |(ppu_state_r & (ST_PPU_OFF | ST_PPU_VBL)))
        hdma_win_r <= 1'b0;

      // 1->0 display disable happens imediately.  it's only possible to go from 0->1 during vblank
      if (~REG_LCDC_r[`LCDC_DS_EN]) ppu_state_r <= ST_PPU_OFF;
      ppu_dot_ctr_r <= (ppu_dot_end | |(ppu_state_r & ST_PPU_OFF)) ? 0 : ppu_dot_ctr_r + 1;
    end
  end
end

//-------------------------------------------------------------------
// APU
//-------------------------------------------------------------------

`ifdef APU
reg  [2:0]  apu_frame_step_r;

// square1
reg         apu_square1_enable_r;
reg  [12:0] apu_square1_timer_r;
reg  [5:0]  apu_square1_length_r;
reg         apu_square1_env_enable_r;
reg  [2:0]  apu_square1_env_timer_r;
reg  [3:0]  apu_square1_volume_r;
reg  [2:0]  apu_square1_pos_r;

reg  [7:0]  apu_square1_duty_r;

reg         apu_square1_sweep_enable_r;
reg  [3:0]  apu_square1_sweep_timer_r;
reg  [10:0] apu_square1_sweep_freq_r;
wire [15:0] apu_square1_sweep_freq_next = REG_NR10_r[`NR10_SWEEP_NEG] ? ({5'h00,apu_square1_sweep_freq_r} - ({5'h00,apu_square1_sweep_freq_r} >> REG_NR10_r[`NR10_SWEEP_SHIFT])) : ({5'h00,apu_square1_sweep_freq_r} + ({5'h00,apu_square1_sweep_freq_r} >> REG_NR10_r[`NR10_SWEEP_SHIFT]));

wire [12:0] apu_square1_period = {REG_NR14_r[`NR14_FREQ_MSB],REG_NR13_r[`NR13_FREQ_LSB],2'b00};
wire signed [4:0] apu_square1_output = (apu_square1_enable_r & apu_square1_duty_r[apu_square1_pos_r]) ? {apu_square1_volume_r,1'b0} : 5'h00;

// square2
reg         apu_square2_enable_r;
reg  [12:0] apu_square2_timer_r;
reg  [5:0]  apu_square2_length_r;
reg         apu_square2_env_enable_r;
reg  [2:0]  apu_square2_env_timer_r;
reg  [3:0]  apu_square2_volume_r;
reg  [2:0]  apu_square2_pos_r;

reg  [7:0]  apu_square2_duty_r;

wire [12:0] apu_square2_period = {REG_NR24_r[`NR24_FREQ_MSB],REG_NR23_r[`NR23_FREQ_LSB],2'b00};
wire [4:0] apu_square2_output = (apu_square2_enable_r & apu_square2_duty_r[apu_square2_pos_r]) ? {apu_square2_volume_r,1'b0} : 5'h00;

// wave
reg         apu_wave_enable_r;
reg  [11:0] apu_wave_timer_r;
reg  [7:0]  apu_wave_length_r;
reg  [4:0]  apu_wave_pos_r;
reg         apu_wave_sample_update_r;
reg  [3:0]  apu_wave_sample_r;

reg  [3:0]  apu_wave_data_r;
//reg  [4:0]  apu_wave_data_shifted_r;
reg  [3:0]  apu_wave_data_shifted_r;
wire [11:0] apu_wave_period = {REG_NR34_r[`NR34_FREQ_MSB],REG_NR33_r[`NR33_FREQ_LSB],1'b0};
wire [4:0] apu_wave_output = (apu_wave_enable_r & |REG_NR32_r[`NR32_LEVEL]) ? {apu_wave_data_shifted_r,1'b0} : 5'h00;

// noise
reg         apu_noise_enable_r;
reg  [21:0] apu_noise_timer_r;
reg  [5:0]  apu_noise_length_r;
reg  [2:0]  apu_noise_env_timer_r;
reg  [3:0]  apu_noise_volume_r;
reg  [14:0] apu_noise_lfsr_r;

wire [21:0] apu_noise_period = {15'h0000,REG_NR43_r[`NR43_LFSR_DIV],~|REG_NR43_r[`NR43_LFSR_DIV],3'h0} << REG_NR43_r[`NR43_LFSR_SHIFT];
wire [4:0] apu_noise_output = (apu_noise_enable_r & apu_noise_lfsr_r[0]) ? {apu_noise_volume_r,1'b0} : 5'h00;

wire [6:0]  apu_data[1:0];
reg  [6:0] apu_data_r[1:0];
reg  [9:0] apu_data_volume_r[1:0];

assign apu_data[0][6:0] = ( ((apu_square1_output[4:0] & {5{REG_NR51_r[`NR51_SELECT_LEFT_CH0]  & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          + ((apu_square2_output[4:0] & {5{REG_NR51_r[`NR51_SELECT_LEFT_CH1]  & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          + ((apu_wave_output[4:0]    & {5{REG_NR51_r[`NR51_SELECT_LEFT_CH2]  & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          + ((apu_noise_output[4:0]   & {5{REG_NR51_r[`NR51_SELECT_LEFT_CH3]  & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          );
assign apu_data[1][6:0] = ( ((apu_square1_output[4:0] & {5{REG_NR51_r[`NR51_SELECT_RIGHT_CH0] & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          + ((apu_square2_output[4:0] & {5{REG_NR51_r[`NR51_SELECT_RIGHT_CH1] & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          + ((apu_wave_output[4:0]    & {5{REG_NR51_r[`NR51_SELECT_RIGHT_CH2] & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          + ((apu_noise_output[4:0]   & {5{REG_NR51_r[`NR51_SELECT_RIGHT_CH3] & REG_NR52_r[`NR52_CONTROL_ENABLE]}}))
                          );

assign APU_REG_enable = {apu_noise_enable_r, apu_wave_enable_r, apu_square2_enable_r, apu_square1_enable_r};

reg         apu_cpu_edge_d1_r;
reg         apu_reg_update_r;
reg  [7:0]  apu_reg_update_address_r;
reg         apu_reg_update_nr12_dir_r;
reg         apu_reg_update_nr22_dir_r;
reg         apu_reg_update_nr14_enable_r;
reg         apu_reg_update_nr24_enable_r;

// convert to signed
assign APU_DAT = {~apu_data_volume_r[1][9],apu_data_volume_r[1][8:0],~apu_data_volume_r[0][9],apu_data_volume_r[0][8:0]};

always @(posedge CLK) begin
  // Flop audio state since it is a critical path on MK2
  for (i = 0; i < 2; i = i + 1) apu_data_r[i] <= apu_data[i];

  apu_data_volume_r[0][9:0] <= apu_data_r[0][6:0] * ({7'h00,REG_NR50_r[`NR50_MASTER_LEFT_VOLUME]}  + 1);
  apu_data_volume_r[1][9:0] <= apu_data_r[1][6:0] * ({7'h00,REG_NR50_r[`NR50_MASTER_RIGHT_VOLUME]} + 1);

  apu_wave_data_shifted_r[3:0] <= apu_wave_sample_r[3:0] >> (REG_NR32_r[`NR32_LEVEL] - 1);

  case (REG_NR11_r[`NR11_DUTY])
    0: apu_square1_duty_r <= 8'b00000001;
    1: apu_square1_duty_r <= 8'b10000001;
    2: apu_square1_duty_r <= 8'b10000111;
    3: apu_square1_duty_r <= 8'b01111110;
  endcase

  case (REG_NR21_r[`NR21_DUTY])
    0: apu_square2_duty_r <= 8'b00000001;
    1: apu_square2_duty_r <= 8'b10000001;
    2: apu_square2_duty_r <= 8'b10000111;
    3: apu_square2_duty_r <= 8'b01111110;
  endcase

  apu_wave_sample_update_r <= 0;
  if (apu_wave_sample_update_r) apu_wave_sample_r <= apu_wave_pos_r[0] ? REG_WAV_r[apu_wave_pos_r[4:1]][3:0] : REG_WAV_r[apu_wave_pos_r[4:1]][7:4];

  if (cpu_ireset_r | ~REG_NR52_r[`NR52_CONTROL_ENABLE]) begin
    REG_NR10_r    <= 8'h00; // FF10
    if (cpu_ireset_r) REG_NR11_r[`NR11_LENGTH] <= 0; // FF11
    REG_NR11_r[`NR11_DUTY] <= 0;
    REG_NR12_r    <= 8'h00; // FF12
    REG_NR13_r    <= 8'h00; // FF13
    REG_NR14_r    <= 5'h00; // FF14

    if (cpu_ireset_r) REG_NR21_r[`NR21_LENGTH] <= 0; // FF16
    REG_NR21_r[`NR21_DUTY] <= 0;
    REG_NR22_r    <= 8'h00; // FF17
    REG_NR23_r    <= 8'h00; // FF18
    REG_NR24_r    <= 8'h00; // FF19

    REG_NR30_r    <= 8'h00; // FF1A
    REG_NR31_r    <= 8'h00; // FF1B
    REG_NR32_r    <= 8'h00; // FF1C
    REG_NR33_r    <= 8'h00; // FF1D
    REG_NR34_r    <= 8'h00; // FF1E

    if (cpu_ireset_r) REG_NR41_r[`NR41_LENGTH] <= 0; // FF16
    REG_NR42_r    <= 8'h00; // FF21
    REG_NR43_r    <= 8'h00; // FF22
    REG_NR44_r    <= 8'h00; // FF23

    REG_NR50_r    <= 8'h00; // FF24
    REG_NR51_r    <= 8'h00; // FF25
    REG_NR52_r    <= 8'h00; // FF26

    // RT1 uses uninitialized WAV RAM data.  One possible set of SGB2 values used.
    if (cpu_ireset_r) begin
      REG_WAV_r[0]  <= 8'h08;//8'hAC;
      REG_WAV_r[1]  <= 8'hF7;//8'hDD;
      REG_WAV_r[2]  <= 8'h04;//8'hDA;
      REG_WAV_r[3]  <= 8'hDF;//8'h48;
      REG_WAV_r[4]  <= 8'h08;//8'h36;
      REG_WAV_r[5]  <= 8'h66;//8'h02;
      REG_WAV_r[6]  <= 8'h00;//8'hCF;
      REG_WAV_r[7]  <= 8'h7F;//8'h16;
      REG_WAV_r[8]  <= 8'h00;//8'h2C;
      REG_WAV_r[9]  <= 8'h57;//8'h04;
      REG_WAV_r[10] <= 8'h02;//8'hE5;
      REG_WAV_r[11] <= 8'hFF;//8'h2C;
      REG_WAV_r[12] <= 8'h08;//8'hAC;
      REG_WAV_r[13] <= 8'hFF;//8'hDD;
      REG_WAV_r[14] <= 8'h00;//8'hDA;
      REG_WAV_r[15] <= 8'h9F;//8'h48;
    end

    apu_frame_step_r <= 0;

    apu_square1_enable_r       <= 0;
    apu_square1_timer_r        <= 0;
    apu_square1_env_enable_r   <= 0;
    apu_square1_env_timer_r    <= 0;
    apu_square1_volume_r       <= 0;
    apu_square1_pos_r          <= 0;
    apu_square1_sweep_enable_r <= 0;
    apu_square1_sweep_timer_r  <= 0;
    apu_square1_sweep_freq_r   <= 0;

    apu_square2_enable_r     <= 0;
    apu_square2_timer_r      <= 0;
    apu_square2_env_enable_r <= 0;
    apu_square2_env_timer_r  <= 0;
    apu_square2_volume_r     <= 0;
    apu_square2_pos_r        <= 0;

    apu_wave_enable_r <= 0;
    apu_wave_timer_r  <= 0;
    apu_wave_pos_r    <= 0;

    apu_noise_enable_r     <= 0;
    apu_noise_timer_r      <= 0;
    apu_noise_env_timer_r  <= 0;
    apu_noise_volume_r     <= 0;
    apu_noise_lfsr_r       <= 0;

    apu_cpu_edge_d1_r <= 0;
    apu_reg_update_r  <= 0;

    // handle APU enable
    if (REG_req_val) begin
      case(REG_address)
        8'h11: REG_NR11_r[`NR11_LENGTH] <= REG_req_data[`NR11_LENGTH];
        8'h16: REG_NR21_r[`NR21_LENGTH] <= REG_req_data[`NR21_LENGTH];
        8'h20: REG_NR41_r[`NR41_LENGTH] <= REG_req_data[`NR41_LENGTH];
        8'h26: {REG_NR52_r[7:7],REG_NR52_r[3:0]} <= {REG_req_data[7:7],REG_req_data[3:0]};
      endcase
    end
  end
  else begin
    // The whole APU is in the 1x domain (contract section 2: "All Sound
    // Timings and Frequencies" keep operating as usual at double speed), the
    // register hand-off included: a write lands on the tick after the next
    // APU edge, the same absolute time it always took at single speed.
    apu_cpu_edge_d1_r <= CLK_PPU_EDGE;

    if (apu_reg_update_r & apu_cpu_edge_d1_r) begin
      case (apu_reg_update_address_r)
        // square1
        8'h11:  apu_square1_length_r <= REG_NR11_r[`NR11_LENGTH];    // NR11
        8'h12: begin // NR12
          // volume side effect (inversion).  see register writes for additional side effects.
          if (apu_reg_update_nr12_dir_r ^ REG_NR12_r[`NR12_ENV_DIR]) apu_square1_volume_r <= ~apu_square1_volume_r + 1;

          if (apu_square1_enable_r) apu_square1_enable_r <= |REG_NR12_r[7:3];
        end
        8'h13: begin // NR13
        end
        8'h14: begin // NR14
          // SML end of stage time counting requires this for ringing effect
          if (apu_reg_update_nr14_enable_r) apu_square1_pos_r <= apu_square1_pos_r + 1;

          if (REG_NR14_r[`NR14_FREQ_ENABLE]) begin
            apu_square1_enable_r     <= (|REG_NR12_r[`NR12_ENV_VOLUME] | REG_NR12_r[`NR12_ENV_DIR]) & ~HLT_RSP;
            apu_square1_timer_r      <= apu_square1_period;
            apu_square1_length_r     <= &apu_square1_length_r ? 0 : apu_square1_length_r;
            apu_square1_env_enable_r <= 1;
            apu_square1_env_timer_r  <= REG_NR12_r[`NR12_ENV_PERIOD];
            apu_square1_volume_r     <= REG_NR12_r[`NR12_ENV_VOLUME];

            apu_square1_sweep_enable_r <= |REG_NR10_r[`NR10_SWEEP_TIME] | |REG_NR10_r[`NR10_SWEEP_SHIFT];
            apu_square1_sweep_timer_r  <= {~|REG_NR10_r[`NR10_SWEEP_TIME],REG_NR10_r[`NR10_SWEEP_TIME]};
            apu_square1_sweep_freq_r   <= {REG_NR14_r[`NR14_FREQ_MSB],REG_NR13_r[`NR13_FREQ_LSB]};
          end
        end

        // square2
        8'h16:  apu_square2_length_r <= REG_NR21_r[`NR21_LENGTH];    // NR21
        8'h17: begin // NR22
          // volume side effect (inversion).  see register writes for additional side effects.
          if (apu_reg_update_nr22_dir_r ^ REG_NR22_r[`NR22_ENV_DIR]) apu_square2_volume_r <= ~apu_square2_volume_r + 1;

          if (apu_square2_enable_r) apu_square2_enable_r <= |REG_NR22_r[7:3];
        end
        8'h18: begin // NR23
        end
        8'h19: begin // NR24
          // SML end of stage time counting requires this for ringing effect
          if (apu_reg_update_nr24_enable_r) apu_square2_pos_r <= apu_square2_pos_r + 1;

          if (REG_NR24_r[`NR24_FREQ_ENABLE]) begin
            apu_square2_enable_r     <= (|REG_NR22_r[`NR22_ENV_VOLUME] | REG_NR22_r[`NR22_ENV_DIR]) & ~HLT_RSP;
            apu_square2_timer_r      <= apu_square2_period;
            apu_square2_length_r     <= &apu_square2_length_r ? 0 : apu_square2_length_r;
            apu_square2_env_enable_r <= 1;
            apu_square2_env_timer_r  <= REG_NR22_r[`NR22_ENV_PERIOD];
            apu_square2_volume_r     <= REG_NR22_r[`NR22_ENV_VOLUME];
          end
        end

        // wave
        8'h1A:  if (apu_wave_enable_r) apu_wave_enable_r <= REG_NR30_r[`NR30_WAVE_ENABLE]; // NR30
        8'h1B:  apu_wave_length_r <= REG_NR31_r[`NR31_LENGTH];    // NR31
        8'h1E:  begin                                                         // NR34
          if (REG_NR34_r[`NR34_FREQ_ENABLE]) begin
            apu_wave_enable_r     <= REG_NR30_r[`NR30_WAVE_ENABLE] & ~HLT_RSP;
            apu_wave_timer_r      <= apu_wave_period;
            apu_wave_length_r     <= &apu_wave_length_r ? 0 : apu_wave_length_r;
            apu_wave_pos_r        <= 0;
          end
        end

        // noise
        8'h20:  apu_noise_length_r <= REG_NR41_r[`NR41_LENGTH];    // NR41
        8'h21:  if (apu_noise_enable_r) apu_noise_enable_r <= (REG_NR42_r[`NR42_ENV_DIR] | |REG_NR42_r[`NR42_ENV_VOLUME]);// NR42
        8'h23:  begin                                                         // NR44
          if (REG_NR44_r[`NR44_FREQ_ENABLE]) begin
            apu_noise_enable_r     <= (REG_NR42_r[`NR42_ENV_DIR] | |REG_NR42_r[`NR42_ENV_VOLUME]) & ~HLT_RSP;
            apu_noise_timer_r      <= apu_noise_period;
            apu_noise_lfsr_r       <= 15'h7FFF;
            apu_noise_length_r     <= &apu_noise_length_r ? 0 : apu_noise_length_r;
            apu_noise_env_timer_r  <= REG_NR42_r[`NR42_ENV_PERIOD];
            apu_noise_volume_r     <= REG_NR42_r[`NR42_ENV_VOLUME];
          end

        end
      endcase
    end
    else if (CLK_PPU_EDGE) begin
      if (tmr_apu_step_r) apu_frame_step_r <= apu_frame_step_r + 1;

      ////////////
      // square1
      ////////////
      if (tmr_apu_step_r) begin
        // period
        if (~apu_frame_step_r[0]) begin
          if (REG_NR14_r[`NR14_FREQ_STOP]) begin
            if (&apu_square1_length_r) apu_square1_enable_r <= 0; else apu_square1_length_r <= apu_square1_length_r + 1;
          end
        end

        // envelope
        if (&apu_frame_step_r) begin
          if (apu_square1_env_enable_r & |REG_NR12_r[`NR12_ENV_PERIOD]) begin
            apu_square1_env_timer_r <= apu_square1_env_timer_r - 1;

            if (apu_square1_env_timer_r == 1) begin
              if      ( REG_NR12_r[`NR12_ENV_DIR] & ~&apu_square1_volume_r) apu_square1_volume_r <= apu_square1_volume_r + 1;
              else if (~REG_NR12_r[`NR12_ENV_DIR] &  |apu_square1_volume_r) apu_square1_volume_r <= apu_square1_volume_r - 1;
              else                                                          apu_square1_env_enable_r <= 0;

              apu_square1_env_timer_r <= REG_NR12_r[`NR12_ENV_PERIOD];
            end
          end
        end

        // sweep
        if (apu_frame_step_r[1:0] == 2'b10) begin
          if (apu_square1_sweep_enable_r) begin
            if (|REG_NR10_r[`NR10_SWEEP_TIME]) begin
              if (|apu_square1_sweep_timer_r) begin
                apu_square1_sweep_timer_r <= apu_square1_sweep_timer_r - 1;

                if (apu_square1_sweep_timer_r == 1) begin
                  if (~|REG_NR10_r[`NR10_SWEEP_SHIFT]) apu_square1_enable_r <= 0;
                  if (~|REG_NR10_r[`NR10_SWEEP_SHIFT]) apu_square1_sweep_enable_r <= 0;
                  apu_square1_sweep_timer_r  <= {~|REG_NR10_r[`NR10_SWEEP_TIME],REG_NR10_r[`NR10_SWEEP_TIME]};

                  // need to update both reg and shadow here because period uses reg.  period needs to use reg because the program may update that manually.
                  // the shadow is used to compute the next frequency for shutting down the output for sweep
                  // TODO: determine if looking one frequency shift in the future is enough.
                  if (|REG_NR10_r[`NR10_SWEEP_SHIFT]) {REG_NR14_r[`NR14_FREQ_MSB],REG_NR13_r[`NR13_FREQ_LSB]} <= apu_square1_sweep_freq_next[10:0];
                  if (|REG_NR10_r[`NR10_SWEEP_SHIFT]) apu_square1_sweep_freq_r[10:0] <= apu_square1_sweep_freq_next[10:0];
                end
              end
            end
          end
        end
      end

      // duty cycle
      apu_square1_timer_r <= apu_square1_timer_r + 1;
      if (&apu_square1_timer_r) begin
        apu_square1_pos_r <= apu_square1_pos_r + 1;
        apu_square1_timer_r <= apu_square1_period;
      end

      // check sweep overflow
      if (apu_square1_sweep_enable_r & |REG_NR10_r[`NR10_SWEEP_SHIFT] & |apu_square1_sweep_freq_next[15:11]) begin
        apu_square1_enable_r <= 0;
        apu_square1_sweep_enable_r <= 0;
      end

      ////////////
      // square2
      ////////////
      if (tmr_apu_step_r) begin
        // period
        if (~apu_frame_step_r[0]) begin
          if (REG_NR24_r[`NR24_FREQ_STOP]) begin
            if (&apu_square2_length_r) apu_square2_enable_r <= 0; else apu_square2_length_r <= apu_square2_length_r + 1;
          end
        end

        // envelope
        if (&apu_frame_step_r) begin
          if (apu_square2_env_enable_r & |REG_NR22_r[`NR22_ENV_PERIOD]) begin
            apu_square2_env_timer_r <= apu_square2_env_timer_r - 1;
            if (apu_square2_env_timer_r == 1) begin
              if      ( REG_NR22_r[`NR22_ENV_DIR] & ~&apu_square2_volume_r) apu_square2_volume_r <= apu_square2_volume_r + 1;
              else if (~REG_NR22_r[`NR22_ENV_DIR] &  |apu_square2_volume_r) apu_square2_volume_r <= apu_square2_volume_r - 1;
              else                                                          apu_square2_env_enable_r <= 0;

              apu_square2_env_timer_r <= REG_NR22_r[`NR22_ENV_PERIOD];
            end
          end
        end
      end

      // duty cycle
      apu_square2_timer_r <= apu_square2_timer_r + 1;
      if (&apu_square2_timer_r) begin
        apu_square2_pos_r <= apu_square2_pos_r + 1;
        apu_square2_timer_r <= apu_square2_period;
      end

      ////////////
      // wave
      ////////////
      if (tmr_apu_step_r) begin
        // period
        if (~apu_frame_step_r[0]) begin
          if (REG_NR34_r[`NR34_FREQ_STOP]) begin
            if (&apu_wave_length_r) apu_wave_enable_r <= 0; else apu_wave_length_r <= apu_wave_length_r + 1;
          end
        end
      end

      apu_wave_timer_r <= apu_wave_timer_r + 1;
      if (&apu_wave_timer_r) begin
        apu_wave_pos_r   <= apu_wave_pos_r + 1;
        apu_wave_timer_r <= apu_wave_period;

        apu_wave_sample_update_r <= 1;
      end

      ////////////
      // noise
      ////////////
      if (tmr_apu_step_r) begin
        // period
        if (~apu_frame_step_r[0]) begin
          if (REG_NR44_r[`NR44_FREQ_STOP]) begin
            if (&apu_noise_length_r) apu_noise_enable_r <= 0; else apu_noise_length_r <= apu_noise_length_r + 1;
          end
        end

        // envelope
        if (&apu_frame_step_r) begin
          if (|apu_noise_env_timer_r) begin
            apu_noise_env_timer_r <= apu_noise_env_timer_r - 1;
            if (apu_noise_env_timer_r == 1) begin
              if      ( REG_NR42_r[`NR42_ENV_DIR] & ~&apu_noise_volume_r) apu_noise_volume_r <= apu_noise_volume_r + 1;
              else if (~REG_NR42_r[`NR42_ENV_DIR] &  |apu_noise_volume_r) apu_noise_volume_r <= apu_noise_volume_r - 1;

              apu_noise_env_timer_r <= REG_NR42_r[`NR42_ENV_PERIOD];
            end
          end
        end
      end

      // lfsr
      if (REG_NR43_r[`NR43_LFSR_SHIFT] < 4'hE) begin
        apu_noise_timer_r <= apu_noise_timer_r - 1;
        if (~|apu_noise_timer_r) begin
          apu_noise_timer_r <= apu_noise_period;

          apu_noise_lfsr_r <= {^apu_noise_lfsr_r[1:0],apu_noise_lfsr_r[14:8],(REG_NR43_r[`NR43_LFSR_WIDTH] ? ^apu_noise_lfsr_r[1:0] : apu_noise_lfsr_r[7]),apu_noise_lfsr_r[6:1]};
        end
      end
    end

    //--------------
    // APU REGISTERS
    //--------------

    // sync to one after CPU clock edge
    if (apu_cpu_edge_d1_r) apu_reg_update_r <= 0;

    if (REG_req_val) begin
      apu_reg_update_r <= 1;
      apu_reg_update_address_r <= REG_address;
      apu_reg_update_nr12_dir_r <= REG_NR12_r[`NR12_ENV_DIR];
      apu_reg_update_nr22_dir_r <= REG_NR22_r[`NR22_ENV_DIR];
      apu_reg_update_nr14_enable_r <= REG_NR14_r[`NR14_FREQ_ENABLE];
      apu_reg_update_nr24_enable_r <= REG_NR24_r[`NR24_FREQ_ENABLE];

      case (REG_address)
        8'h10: REG_NR10_r[7:0] <= REG_req_data[7:0];
        8'h11: REG_NR11_r[7:0] <= REG_req_data[7:0];
        8'h12: begin
          REG_NR12_r[7:0] <= REG_req_data[7:0];

          // volume side effects
          // P-M uses the first one to wrap the volume from F->0
          // CVL and probably many others use the second as predecrement of volume
          if      (REG_req_data[`NR12_ENV_DIR] & ~|REG_NR12_r[`NR12_ENV_PERIOD])                                                            apu_square1_volume_r <= apu_square1_volume_r + 1;
          else if (~REG_req_data[`NR12_ENV_DIR] & |REG_req_data[`NR12_ENV_PERIOD] & ~|REG_NR12_r[`NR12_ENV_PERIOD] & |apu_square1_volume_r) apu_square1_volume_r <= apu_square1_volume_r - 1;
        end
        8'h13: REG_NR13_r[7:0] <= REG_req_data[7:0];
        8'h14: REG_NR14_r[7:0] <= REG_req_data[7:0];

        8'h16: REG_NR21_r[7:0] <= REG_req_data[7:0];
        8'h17: begin
          REG_NR22_r[7:0] <= REG_req_data[7:0];

          // volume side effects
          // P-M uses the first one to wrap the volume from F->0
          // CVL and probably many others use the second as predecrement of volume
          if      (REG_req_data[`NR22_ENV_DIR] & ~|REG_NR22_r[`NR22_ENV_PERIOD])                                                            apu_square2_volume_r <= apu_square2_volume_r + 1;
          else if (~REG_req_data[`NR22_ENV_DIR] & |REG_req_data[`NR22_ENV_PERIOD] & ~|REG_NR22_r[`NR22_ENV_PERIOD] & |apu_square2_volume_r) apu_square2_volume_r <= apu_square2_volume_r - 1;
        end
        8'h18: REG_NR23_r[7:0] <= REG_req_data[7:0];
        8'h19: REG_NR24_r[7:0] <= REG_req_data[7:0];

        8'h1A: REG_NR30_r[7:0] <= REG_req_data[7:0];
        8'h1B: REG_NR31_r[7:0] <= REG_req_data[7:0];
        8'h1C: REG_NR32_r[7:0] <= REG_req_data[7:0];
        8'h1D: REG_NR33_r[7:0] <= REG_req_data[7:0];
        8'h1E: REG_NR34_r[7:0] <= REG_req_data[7:0];

        8'h20: REG_NR41_r[7:0] <= REG_req_data[7:0];
        8'h21: REG_NR42_r[7:0] <= REG_req_data[7:0];
        8'h22: REG_NR43_r[7:0] <= REG_req_data[7:0];
        8'h23: REG_NR44_r[7:0] <= REG_req_data[7:0];

        8'h24: REG_NR50_r[7:0] <= REG_req_data[7:0];
        8'h25: REG_NR51_r[7:0] <= REG_req_data[7:0];
        8'h26: {REG_NR52_r[7:7],REG_NR52_r[3:0]} <= {REG_req_data[7:7],REG_req_data[3:0]};

        8'h30, 8'h31, 8'h32, 8'h33, 8'h34, 8'h35, 8'h36, 8'h37,
        8'h38, 8'h39, 8'h3A, 8'h3B, 8'h3C, 8'h3D, 8'h3E, 8'h3F: if (~apu_wave_enable_r | REG_req_dbg) REG_WAV_r[REG_address[3:0]] <= REG_req_data;

      endcase
    end
  end
end
`endif

//-------------------------------------------------------------------
// MCT
//-------------------------------------------------------------------

// MCT is the memory controller that allows the CPU pipeline stages
// to access all memory-mapped architectural state including:
//
// ROM (both cart and boot)
// SaveRAM
// WRAM
// VRAM
// OAM
// HRAM
// REG (IO registers)
//
// The first 3 are stored in the SD2SNES's 16MB PSRAM.  The others
// are mapped to BRAM or registers because they need: higher bandwidth,
// concurrency (with other state), or partial byte support.
//
// In general, the GB's bandwidth requirements are very modest, but using
// BRAM simplifies having to balance the various sources.

//
// hram
//
wire        hram_wren    = MCT_HRAM_wren;
wire [6:0]  hram_address = MCT_HRAM_address;
wire [7:0]  hram_rddata;
wire [7:0]  hram_wrdata  = MCT_HRAM_data;

wire        dbg_hram_wren;
wire [6:0]  dbg_hram_address;
wire [7:0]  dbg_hram_rddata;
wire [7:0]  dbg_hram_wrdata;

`ifdef MK2
hram hram (
  .clka(CLK), // input clka
  .wea(hram_wren), // input [0 : 0] wea
  .addra(hram_address), // input [6 : 0] addra
  .dina(hram_wrdata), // input [7 : 0] dina
  .douta(hram_rddata), // output [7 : 0] douta
  .clkb(CLK), // input clkb
  .web(dbg_hram_wren), // input [0 : 0] web
  .addrb(dbg_hram_address), // input [6 : 0] addrb
  .dinb(dbg_hram_wrdata), // input [7 : 0] dinb
  .doutb(dbg_hram_rddata) // output [7 : 0] doutb
);
`endif
`ifdef MK3
hram hram (
  .clock(CLK), // input clka
  .wren_a(hram_wren), // input [0 : 0] wea
  .address_a(hram_address), // input [6 : 0] addra
  .data_a(hram_wrdata), // input [7 : 0] dina
  .q_a(hram_rddata), // output [7 : 0] douta
  .wren_b(dbg_hram_wren), // input [0 : 0] web
  .address_b(dbg_hram_address), // input [6 : 0] addrb
  .data_b(dbg_hram_wrdata), // input [7 : 0] dinb
  .q_b(dbg_hram_rddata) // output [7 : 0] doutb
);
`endif

// Sources: IFD, EXE
// Targets: EXT (ROM, SaveRAM, WRAM), VRAM, OAM, IO/HRAM

`define MCT_TGT_VRAM(a) (a[15:13] == 3'b100)  // 8000-9FFF
`define MCT_TGT_HIGH(a) (&a[15:9])            // FE00-FE9F,FF00-FF7F,FF80-FFFE,FFFF
// MCU-only: 8000-BFFF, i.e. both VRAM banks, bank = a[13].  The GB never sees
// this window; A000-BFFF is its cart RAM and goes out on the system bus.
`define DBG_TGT_VRAM(a) (a[15:14] == 2'b10)   // 8000-BFFF

parameter
  ST_MCT_IDLE     = 8'b00000001,
  ST_MCT_DEC      = 8'b00000010,
  ST_MCT_VRAM     = 8'b00000100,
  ST_MCT_OAM      = 8'b00001000,
  ST_MCT_REG      = 8'b00010000,
  ST_MCT_HRAM     = 8'b00100000,
  ST_MCT_EXT      = 8'b01000000,
  ST_MCT_END      = 8'b10000000;

reg  [1:0]  mct_req_r;
reg  [7:0]  mct_state_r;
reg  [15:0] mct_addr_r;
reg         mct_src_r;
reg         mct_wr_r;
reg  [7:0]  mct_mdr_r;
reg         mct_conflict_r;   // this EXT access collides with the OAM DMA's bus
wire        mct_bus_idle;     // the controller is not using the system bus

wire [15:0] mct_addr_d1 = mct_src_r ? EXE_MCT_req_addr_d1 : IFD_MCT_req_addr_d1;

//-------------------------------------------------------------------
// The CGB bus map, and which CPU accesses an OAM DMA collides with.
//
// SameBoy Core/memory.c bus_for_addr() (:15-27): $0000-$7FFF and $A000-$BFFF
// are the MAIN bus, $8000-$9FFF is the video bus, and $C000-$FDFF is a bus of
// its OWN -- but only on a CGB; on a DMG it is part of MAIN.  "A CGB" is the
// MACHINE there (GB_is_cgb(), Core/gb.c: gb->model >= GB_MODEL_CGB_0), not
// KEY0's mode (GB_is_cgb_in_cgb_mode()): the buses are wiring, and a CGB
// running a DMG cartridge in compatibility mode still has its work RAM on a
// bus of its own.  This core is always a CGB, so the map never looks at
// cgb_mode.  ST_MCT_EXT only ever sees MAIN and work-RAM addresses (VRAM and
// $FE00+ are decoded away above), so the rule of is_addr_in_dma_use()
// (:253-269) reduces to:
//
//   the first two M-cycles (warm-up)        -> nothing collides (see dma_warm_r)
//   source on the video bus                 -> nothing here collides
//   CPU address in work RAM                 -> collides with any other source
//   source in echo RAM ($E000-$FDFF)        -> collides with everything here
//   otherwise                               -> collides iff the source is MAIN
//
// What a colliding access DOES has three cases, all from the read path
// (:785-801) and the write path (:1826-1856):
//
//   CPU in work RAM and the source is NOT work RAM  -> the access is
//     REDIRECTED to ((src-1) & $1000) | (addr & $FFF) | $C000, i.e. to a real
//     work-RAM address in the half the DMA is not reading.  It still happens:
//     the read returns what is there and the write lands there.
//   CPU on the main bus and the source is echo RAM  -> reads $FF and the write
//     is dropped.  SameBoy flags this one as cart-specific (:786-789).
//   otherwise (same bus)                            -> the read takes the byte
//     the DMA has in its latch (SameBoy's "addr = dma_current_src - 1" IS that
//     byte) and the write is dropped.
//
// Only the last two never reach the bus; the redirect is an ordinary access
// with a rewritten address.
//
// dma_warm_r keeps the M-cycle right after the $FF46 write out of the map, as
// SameBoy's warm-up does; it is here and not in ST_MCT_DEC because the
// next-state decode of ST_MCT_DEC is the core's critical path (the six worst
// paths of the phase-6 fit end in mct_state_r[1:2]) and dma_on_sys is a flop
// AND away from it.  With it, the first collision is served byte 0 of THIS
// transfer, and every src-1 the redirect needs is inside the source page --
// which is what makes bit 12 of it REG_DMA_r[4].
wire        dma_on_sys      = DMA_active & ~dma_src_r & dma_warm_r;
wire        dma_src_ram     = &REG_DMA_r[7:6];          // $C0-$FF
wire        dma_src_echo    = &REG_DMA_r[7:5];          // $E0-$FF
wire        mct_addr_ram_d1 = &mct_addr_d1[15:14];      // $C000-$FDFF
wire        mct_dma_clash   = dma_on_sys
                            & (mct_addr_ram_d1 | dma_src_echo | ~dma_src_ram);
// (src-1) bit 12 is REG_DMA_r[4]: after the warm-up src-1 runs from the first
// byte of the page to the 160th, so the borrow never leaves the low byte.
wire        mct_dma_redir   = mct_dma_clash & mct_addr_ram_d1
                            & (~dma_src_ram | dma_src_echo);
wire        mct_dma_take    = mct_dma_clash & ~mct_dma_redir;
wire [15:0] mct_addr_next   = mct_dma_redir ? {3'b110, REG_DMA_r[4], mct_addr_d1[11:0]}
                                            : mct_addr_d1;
wire  [7:0] mct_clash_data  = (dma_src_echo & ~(&mct_addr_r[15:14])) ? 8'hFF : dma_data_r;

assign HRAM_data = hram_rddata;

assign MCT_VRAM_wren = mct_wr_r & |(mct_state_r & ST_MCT_VRAM) & mct_req_r[0];
assign MCT_VRAM_address = mct_addr_r[12:0];
assign MCT_VRAM_data = mct_mdr_r;

assign MCT_OAM_wren = mct_wr_r & |(mct_state_r & ST_MCT_OAM) & mct_req_r[0];
assign MCT_OAM_address = mct_addr_r[7:0];
assign MCT_OAM_data = mct_mdr_r;

assign MCT_HRAM_wren = mct_wr_r & |(mct_state_r & ST_MCT_HRAM) & mct_req_r[0];
assign MCT_HRAM_address = mct_addr_r[6:0];
assign MCT_HRAM_data = mct_mdr_r;

assign MCT_REG_wren = mct_wr_r & |(mct_state_r & ST_MCT_REG) & mct_req_r[0];
assign MCT_REG_address = mct_addr_r[7:0];
assign MCT_REG_data = mct_mdr_r;

// "The controller is not on the system bus and is not about to be": idle has
// to include the request that is only just being raised, because this is
// tested one CLK after a bus edge and ifd_req_r is assigned BY that edge.
// ST_HDMA_SYNC needs the same test, and shares it so the two cannot drift
// apart in a later edit.
assign mct_bus_idle = |(mct_state_r & ST_MCT_IDLE) & ~|mct_req_r
                    & ~IFD_MCT_req_val & ~EXE_MCT_req_val;

// The OAM DMA takes the bus only when neither the controller nor the HDMA is
// using it, which makes the "HDMA > OAM-DMA > CPU" rule below something the
// hardware implements rather than something three separate properties happen
// to guarantee.  HDMA_SYS_active, not hdma_bus_busy: ST_HDMA_SYNC waits for
// ~DMA_active, so gating on the wider signal would deadlock the pair.
assign dma_sys_free = mct_bus_idle & ~HDMA_SYS_active;

// System bus owner: HDMA > OAM-DMA > CPU.  The HDMA only ever gets here with
// the CPU stalled and the controller idle (ST_HDMA_SYNC); the OAM DMA now
// waits for both the same way (dma_sys_free), so neither takes a request away
// from an in-flight CPU access any more.  A CPU access that the OAM DMA
// COLLIDES with (mct_conflict_r) never reaches the bus at all -- see the bus
// map above -- so it is masked out of the request and the write strobe.
assign SYS_REQ    = HDMA_SYS_active ? hdma_sys_req  : DMA_SYS_active ? DMA_req_val : (|(mct_state_r & ST_MCT_EXT) & mct_req_r[0] & ~mct_conflict_r);
assign SYS_WR     = (HDMA_SYS_active | DMA_SYS_active | mct_conflict_r) ? 1'b0 : mct_wr_r & mct_req_r[0];
assign SYS_ADDR   = HDMA_SYS_active ? hdma_sys_addr : DMA_SYS_active ? DMA_address : mct_addr_r;
assign SYS_WRDATA = mct_mdr_r;

assign MCT_IFD_rsp_val = |(mct_state_r & ST_MCT_END) & ~mct_src_r;
assign MCT_EXE_rsp_val = |(mct_state_r & ST_MCT_END) &  mct_src_r;
assign MCT_data = mct_mdr_r;

assign MCT_REG_req_val = |(mct_state_r & ST_MCT_REG);

reg  [15:0] mct_oam_error_r;
reg  [15:0] mct_vram_error_r;

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    mct_state_r <= ST_MCT_IDLE;
    mct_req_r   <= 0;

    mct_oam_error_r <= 0;
    mct_vram_error_r <= 0;
    mct_conflict_r <= 0;
  end
  else begin
    case (mct_state_r)
      ST_MCT_IDLE: begin
        if      (EXE_MCT_req_val) begin
          mct_src_r   <= 1;
          mct_wr_r    <= EXE_MCT_req_wr;

          mct_state_r <= ST_MCT_DEC;
        end
        else if (IFD_MCT_req_val) begin
          mct_src_r   <= 0;
          mct_wr_r    <= 0;

          mct_state_r <= ST_MCT_DEC;
        end
      end
      ST_MCT_DEC: begin
        // data and address arrives one cycle late to simplify EXE register read -> AGEN
        mct_addr_r <= mct_addr_next;
        if (mct_wr_r) mct_mdr_r <= EXE_MCT_req_data_d1;

        if      (`MCT_TGT_VRAM(mct_addr_d1)) begin
          mct_state_r <= ST_MCT_VRAM;
        end
        else if (`MCT_TGT_HIGH(mct_addr_d1)) begin
          mct_state_r <= mct_addr_d1[8] ? ((~mct_addr_d1[7] | &mct_addr_d1[6:0]) ? ST_MCT_REG : ST_MCT_HRAM) : ST_MCT_OAM;
        end
        else if (mct_dma_take) begin
          // The OAM DMA is on this bus (see the bus map above) and this is one
          // of the two cases that never reach it: ST_MCT_EXT serves the read
          // from the DMA's own latch and the write goes nowhere.  A REDIRECTED
          // access is not here -- mct_addr_next already moved it into work RAM
          // and it goes out like any other.
          mct_conflict_r <= 1'b1;
          mct_state_r    <= ST_MCT_EXT;
        end
        else if (~DMA_SYS_active) begin
          // Do not step onto the system bus while the OAM DMA's own read is in
          // flight: ST_MCT_EXT completes on SYS_RDY and latches SYS_RDDATA
          // without checking whose transaction answered, so entering now would
          // finish this access with the DMA's byte from the DMA's address.
          //
          // Parking here is safe because mct_addr_d1 CANNOT move under it: it
          // is ifd_pc or the EXE's address generator, and PC_r only moves on
          // ifd_step (CLK_BUS_EDGE & ...), exe_mem_req_r is a one-clock pulse
          // off CLK_BUS_EDGE and exe_ctr_r only counts on CLK_BUS_EDGE -- so
          // the whole address is constant inside an M-cycle, and this wait is
          // bounded by one PSRAM access.
          mct_state_r <= ST_MCT_EXT;
        end
      end
      ST_MCT_VRAM: begin
        // SH writes 0 and checks for nonzero to see if the write failed.  Returning FF here allows correct detection of the fail
        if (~mct_wr_r) mct_mdr_r <= PPU_MCT_vram_active ? 8'hFF : VRAM_data;

        // avoid false errors by only looking at EXE src
        if (~|mct_req_r & PPU_MCT_vram_active & mct_src_r) mct_vram_error_r <= mct_vram_error_r + 1;
        if (~|mct_req_r) mct_state_r <= ST_MCT_END;
      end
      ST_MCT_OAM: begin
        if (~mct_wr_r) mct_mdr_r <= PPU_MCT_oam_active ? 8'hFF : OAM_data;

        // avoid false errors by only looking at EXE src
        if (~|mct_req_r & PPU_MCT_oam_active & mct_src_r) mct_oam_error_r <= mct_oam_error_r + 1;
        if (~|mct_req_r) mct_state_r <= ST_MCT_END;
      end
      ST_MCT_REG: begin
        if (~mct_wr_r) mct_mdr_r <= REG_data;

        if (~|mct_req_r & REG_MCT_rsp_val) mct_state_r <= ST_MCT_END;
      end
      ST_MCT_HRAM: begin
        if (~mct_wr_r) mct_mdr_r <= HRAM_data;

        if (~|mct_req_r) mct_state_r <= ST_MCT_END;
      end
      ST_MCT_EXT: begin
        // the main logic has a one entry buffer to always sink these requests
        if (~mct_wr_r) mct_mdr_r <= mct_conflict_r ? mct_clash_data : SYS_RDDATA;

        // A collided access has no transaction to wait for: SYS_REQ and SYS_WR
        // were masked, the read came out of the DMA's latch, and the write is
        // gone.  It retires as soon as the request pulse has cleared.
        if (~|mct_req_r & (mct_conflict_r | SYS_RDY)) mct_state_r <= ST_MCT_END;
      end
      ST_MCT_END: begin
        mct_conflict_r <= 1'b0;
        mct_state_r <= ST_MCT_IDLE;
      end
    endcase

    mct_req_r <= {mct_req_r[0],|(mct_state_r & ST_MCT_DEC)};
  end
end

// Clock dilation: the CPU's access ALWAYS retires before the bus edge that
// consumes it.
//
// The budget: a PSRAM access costs 3 CLK to the request, up to 18 in the
// arbiter behind an MCU access and 2 to retire, and the IFD needs four more
// CLK after the answer before the edge can use it (ifd_data_r/ifd_op_r ->
// the decoder M9K -> ifd_decode_r -> ifd_complete_r).  At single speed the
// M-cycle is 80 CLK; at double speed it is 39, which one master fits with room
// to spare and two do NOT: with an OAM DMA reading a system-bus source, a
// slow arbiter answer to the DMA parks the CPU's access in ST_MCT_DEC and the
// CPU's answer can land after, ON, or a clock or two before the edge that
// consumes it.  All three corrupt the instruction stream -- the last one
// silently, with the byte right and the decode of the byte before it
// (measured: p3_dmamid2x at a constant 17-CLK arbiter answer, every probe of
// the time at zero).  No arbitration order fixes it: the two accesses do not
// fit in 39 CLK.
//
// So the M-cycle is made longer instead.  While the controller is not idle and
// the next CPU edge would be the bus edge, CLK_HOLD freezes gbc_clk's phase
// accumulator: every enable of the GB -- CPU, PPU, APU, timers, the DMA's
// credit -- waits the same few CLK, so nothing inside the Game Boy can tell,
// and the edge arrives when the access is done.
//
// The freeze also covers the decode behind the answer, without a term of its
// own: it starts on the first CLK of the last CE interval of the M-cycle
// (clk_bus_ctr_r == 3), so time stops with that whole interval still to run,
// and the edge lands at least one CE interval (9 CLK at double speed, 20 at
// single) after the answer that releases it -- against the four CLK the IFD
// needs.  Measured: the closest answer-to-edge under dilation is 9 CLK at
// every constant latency from 17 to 26 CLK.
//
// What moves is only the GB's time against the SNES, which is what the
// genlock loop already absorbs (a stretch is a few CLK in an M-cycle of 39 and
// only happens while an OAM DMA shares the bus at 2x with a busy arbiter; the
// loop has +-1 % of authority, ~700 dots per frame).  A single master at the
// worst documented latency retires ~22 CLK after the edge, before the last CPU
// edge of the M-cycle at ~29, so on its own the hold never fires and every
// existing cycle count stays where it was.
//
// Nothing the held accumulator stops is needed to end the hold: ST_MCT_EXT
// waits for SYS_RDY from the arbiter, ST_MCT_DEC for the DMA's SYS_RDY,
// ST_MCT_REG for three CLK of the REG block, and none of them looks at an
// enable.
wire        mct_busy = ~|(mct_state_r & ST_MCT_IDLE);
assign CLK_HOLD = &clk_bus_ctr_r & mct_busy;

// The answer's decode window, for starvation_r below: the controller answered
// 1..3 CLK ago, so ifd_decode_r/ifd_complete_r still describe the byte before.
reg  [2:0]  mct_settle_r;
always @(posedge CLK) mct_settle_r <= cpu_ireset_r ? 3'b000
                                    : {mct_settle_r[1:0], |(mct_state_r & ST_MCT_END)};

reg         clk_hold_d1_r;
reg  [15:0] dilation_r;
always @(posedge CLK) begin
  clk_hold_d1_r <= CLK_HOLD;
  if (cpu_ireset_r)                              dilation_r <= 16'd0;
  else if (CLK_HOLD & ~clk_hold_d1_r & ~&dilation_r) dilation_r <= dilation_r + 16'd1;
end
assign DILATION = dilation_r;

// Starvation: the GB's work of the previous M-cycle has not retired when the
// next bus edge arrives.  Two things are watched, and each is watched at the
// point where being late actually costs something:
//
//   the memory controller's access, from the request to four CLK past the
//   answer: parked in ST_MCT_DEC, in flight in ST_MCT_EXT, answering in
//   ST_MCT_END, or answered too recently for the IFD's decode to have caught
//   up (mct_settle_r).  Any of them at a bus edge is a lost or half-used
//   access.  The clock dilation above exists to keep this at zero, so a
//   non-zero count is the dilation failing, not the arbiter being slow;
//
//   the OAM DMA's OAM WRITE, as an unspent credit -- not its source read.  The
//   read deliberately floats inside the M-cycle now (see the credit above), so
//   a read in flight at a bus edge is normal and says nothing; a credit still
//   unspent at the next edge is the DMA owing an OAM byte, which is the only
//   way the transfer can outlast its 160 M-cycles.
//
// The count is exported to GBDG +0C and has to read 0 on hardware; GBDG +1C
// (DILATION) says how often the regime that would have produced it was met.
reg  [15:0] starvation_r;
wire        sys_access_in_flight = ((|(mct_state_r & (ST_MCT_DEC | ST_MCT_EXT | ST_MCT_END)) & ~mct_conflict_r)
                                    | |mct_settle_r)
                                 | (|dma_cred_r);
always @(posedge CLK) begin
  if (cpu_ireset_r)                          starvation_r <= 16'd0;
  else if (CLK_BUS_EDGE & sys_access_in_flight & ~&starvation_r)
                                             starvation_r <= starvation_r + 16'd1;
end
assign STARVATION = starvation_r;

//-------------------------------------------------------------------
// HDMA / GDMA  ($FF51-$FF55)
//-------------------------------------------------------------------

// CGB VRAM DMA.  Two modes off one register pair:
//
//   HDMA5 bit 7 = 0  GDMA  -- (n+1) blocks of 16 bytes copied back to back,
//                             the CPU parked for the whole thing
//   HDMA5 bit 7 = 1  HDMA  -- one 16-byte block at the start of every H-Blank
//                             of a visible line, the CPU parked for that block
//
// Source is any normal read address ($0000-$7FF0 cart, $A000-$DFF0 cart RAM
// or WRAM); the low nibble is dropped.  Destination is always VRAM, bank VBK,
// bits 12:4 of HDMA3/4.  Semantics (cancel, read-back, the immediate first
// block when the transfer is armed inside an H-Blank) follow SameBoy
// Core/memory.c, GB_IO_HDMA1..HDMA5.
//
// The engine reuses the OAM DMA's shape on the system bus -- one-cycle request
// pulse, data valid with SYS_RDY -- because that handshake is what main.v's
// PSRAM arbiter is built around.  What it does NOT reuse is the OAM DMA's
// habit of stealing SYS_REQ from a live CPU access: ST_HDMA_SYNC waits for the
// memory controller to retire first, and the instruction fetch is held off
// while the HDMA has the bus.
//
// Two signals, not one, and the difference matters:
//
//   hdma_cpu_stall  the CPU is parked.  Whole transfer, every block, plus the
//                   padding.  Feeds exe_stall.
//   hdma_bus_busy   the HDMA owns the system bus.  Everything EXCEPT
//                   ST_HDMA_BLKE.  Feeds the instruction fetch gate.
//
// The instruction fetch has to keep running for the CPU to resume correctly
// (see the comment on ifd_req_r), so the block-end state deliberately gives
// the bus back while keeping the CPU parked, and stays there long enough --
// two bus edges -- for a fetch to be issued and answered.  Without that the
// resumed CPU executes whatever byte happened to be left in ifd_data_r.
//
// Not started at all in DMG compatibility mode (SameBoy: "if (!gb->cgb_mode)
// return"), which is also why the CGB boot ROM's three ClearVRAMViaHDMA calls
// work: they run before it writes KEY0.

parameter
  ST_HDMA_IDLE  = 3'd0,
  ST_HDMA_SYNC  = 3'd1,
  ST_HDMA_READ  = 3'd2,
  ST_HDMA_WAIT  = 3'd3,
  ST_HDMA_WRITE = 3'd4,
  ST_HDMA_BLKE  = 3'd5;

reg  [2:0]  hdma_state_r;
reg  [15:0] hdma_src_r;       // low nibble always 0
reg  [12:0] hdma_dst_r;       // offset inside the 8KB VRAM bank
reg  [7:0]  hdma_steps_r;     // blocks left, 1..128; 0 once the last one ran
reg         hdma_hbl_r;       // H-Blank mode armed
reg         hdma_go_r;        // a block has been requested
reg  [3:0]  hdma_byte_r;      // byte inside the current block
reg  [4:0]  hdma_mcyc_r;      // M-cycles billed for the current block (up to 16 at 2x)
reg  [1:0]  hdma_blke_r;      // bus edges spent in ST_HDMA_BLKE
reg         hdma_go_q_r;      // hdma_go_r as it stood at the last bus edge (the request is older than the M-cycle in flight)
reg         hdma_park_q_r;    // cpu_parked as it stood at the last bus edge (the park is older than the M-cycle in flight)
reg         hdma_park_win_r;  // the park began inside the H-Blank window: no block on the wake
reg         hdma_win_q_r;     // hdma_in_hbl as it stood at the last bus edge

// A 16-byte block costs the same REAL time whatever the CPU speed: 8 M-cycles
// at single speed, 16 at double (Pan Docs, "LCD VRAM DMA Transfers": "the
// transfer takes 8 M-cycles per 16 bytes in normal speed and 16 M-cycles in
// double speed"), because the copy is paced by the VRAM/PPU side, not by the
// CPU clock.  The stall is padded to that.
wire [4:0]  hdma_block_mcyc = cpu_speed_r ? 5'd16 : 5'd8;
reg         hdma_req_r;
reg  [7:0]  hdma_data_r;
reg         hdma_in_hbl_d1_r;

// A visible line's H-Blank, and only once per line.  ST_PPU_HBL alone is not
// enough: mode 0 is published a few dots into the state, and ST_PPU_FRM_NEW
// also parks STAT in mode 0 for one dot at the top of the frame.
wire        hdma_in_hbl   = hdma_win_r & REG_LCDC_r[`LCDC_DS_EN];
wire        hdma_hbl_edge = hdma_in_hbl & ~hdma_in_hbl_d1_r;

wire [7:0]  hdma_steps_m1 = hdma_steps_r - 8'd1;

assign      hdma_left       = hdma_steps_m1[6:0];
assign      hdma_running    = hdma_hbl_r | (hdma_state_r != ST_HDMA_IDLE);
assign      hdma_req_pending = hdma_go_r;   // read by the IFD (the STOP second-byte rule)
// A request stalls the CPU at the first instruction boundary at which it is
// older than the M-cycle in flight: gambatte services the DMA between two
// instructions, before the opcode fetch of the next one, when the request
// was raised at or before the start of that fetch.  In this pipeline the
// fetch of instruction N is the last M-cycle of N-1 and the handoff edge is
// its end, so a request raised INSIDE that M-cycle lets N run first and
// stalls the one after it (hdma_start_1/_2, hdma_late_m3halt_m2unhalt_*_2:
// a HALT whose fetch overlapped the trigger still gets the block).  A request
// raised on the wake edge of a park is older by construction (hdma_go_q_r is
// set on that edge too).
assign      hdma_cpu_stall  = (hdma_state_r != ST_HDMA_IDLE) | hdma_go_q_r;
assign      hdma_bus_busy   = (hdma_state_r != ST_HDMA_IDLE)
                            & (hdma_state_r != ST_HDMA_BLKE);

// No H-Blank block starts while the CPU is parked (HALT with nothing pending,
// or the speed-switch park): SameBoy display.c:2115 `hdma_on_hblank &&
// !halted && !stopped`, gambatte memory.cpp:224-231 (the request is masked by
// intreq_.halt() and re-evaluated on the unhalt).  When the park ends, ONE
// block runs if the transfer is armed, the LCD is inside the H-Blank window
// right now and the park did not begin inside that window (SameBoy
// `allow_hdma_on_wake = STAT & 3`, gambatte `haltHdmaState_ == hdma_low`).
// gambatte applies this to the timeout wake of the switch park too
// (intevent_unhalt) -- SameBoy does not -- and only gambatte's reading closes
// hdma_m3speedchange_late_m0wakeup_1/_2 (FF/00), so that is the one used.
// hdma_wake_edge is the bus edge the park releases the CPU on: it is also the
// instruction boundary the block is billed from (hdma_bound_edge below).
wire        hdma_wake_trig  = CLK_BUS_EDGE & hdma_wake_edge & hdma_hbl_r & hdma_in_hbl & ~hdma_park_win_r;

// The H-Blank trigger itself, masked while the CPU has been parked for longer
// than the M-cycle in flight (a HALT whose own M-cycle overlaps the trigger
// still takes the block, see hdma_cpu_stall).
wire        hdma_hbl_trig   = hdma_hbl_r & hdma_hbl_edge & ~hdma_park_q_r;

// The DMA is billed to the CPU from its next instruction boundary, not from
// the trigger, and the copy starts there too: 8 M-cycles per block at 1x, 16
// at 2x (Pan Docs), measured against gambatte's gdma_cycles_* / hdma_cycles_*
// / hdma_late_enable_1 (a bill of 9 lands every `_1` on the wrong side of the
// mode-0 boundary, a bill of 8 lands both).  The boundary is the first bus
// edge on which the request is older than the M-cycle in flight and the
// instruction in EXE has completed (a HALT/STOP counts as complete; the
// wake-up hold of halt_wake_r/spd_wake_r is not over yet).
wire        hdma_bound_edge = CLK_BUS_EDGE & (hdma_cpu_stall | hdma_wake_trig)
                            & (exe_complete_r | (~IFD_EXE_valid & ~|ifd_size_r)) & ~halt_wake_r & ~spd_wake_r;

assign      HDMA_SYS_active = (hdma_state_r == ST_HDMA_READ) | (hdma_state_r == ST_HDMA_WAIT);
assign      hdma_sys_req    = (hdma_state_r == ST_HDMA_WAIT) & hdma_req_r;
assign      hdma_sys_addr   = hdma_src_r;

assign      HDMA_VRAM_active  = (hdma_state_r == ST_HDMA_WRITE);
assign      hdma_vram_wren    = (hdma_state_r == ST_HDMA_WRITE);
assign      hdma_vram_address = hdma_dst_r;
assign      hdma_vram_data    = hdma_data_r;

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    hdma_state_r     <= ST_HDMA_IDLE;
    hdma_src_r       <= 16'h0000;
    hdma_dst_r       <= 13'h0000;
    hdma_steps_r     <= 8'h00;
    hdma_hbl_r       <= 1'b0;
    hdma_go_r        <= 1'b0;
    hdma_byte_r      <= 4'h0;
    hdma_mcyc_r      <= 5'h0;
    hdma_blke_r      <= 2'h0;
    hdma_go_q_r      <= 1'b0;
    hdma_park_q_r    <= 1'b0;
    hdma_park_win_r  <= 1'b0;
    hdma_win_q_r     <= 1'b0;
    hdma_req_r       <= 1'b0;
    hdma_data_r      <= 8'h00;
    hdma_in_hbl_d1_r <= 1'b0;
  end
  else begin
    hdma_in_hbl_d1_r <= hdma_in_hbl;

    // Bus-edge snapshots (see hdma_cpu_stall / hdma_hbl_trig).  Where the
    // LCD was when the CPU parked is judged at the parking instruction's own
    // fetch, one M-cycle before its handoff, which is the snapshot of the
    // previous edge (hdma_late_m0halt_1/_2: a HALT handed over on the last
    // dot of the line still counts as parked inside the window).
    if (CLK_BUS_EDGE) begin
      hdma_go_q_r   <= hdma_go_r | hdma_wake_trig | hdma_hbl_trig;
      hdma_win_q_r  <= hdma_in_hbl;
      if (cpu_parked & ~hdma_park_q_r) hdma_park_win_r <= hdma_win_q_r;
      hdma_park_q_r <= cpu_parked;
    end

    if (hdma_wake_trig | hdma_hbl_trig) hdma_go_r <= 1'b1;

    // ---- register writes ----------------------------------------------
    if (REG_req_val & cgb_mode) begin
      case (REG_address)
        8'h51: hdma_src_r[15:8] <= REG_req_data;
        8'h52: hdma_src_r[7:0]  <= {REG_req_data[7:4],4'h0};
        8'h53: hdma_dst_r[12:8] <= REG_req_data[4:0];
        8'h54: hdma_dst_r[7:0]  <= {REG_req_data[7:4],4'h0};
        8'h55: begin
          // The length is reloaded even by the write that cancels.
          hdma_steps_r <= {1'b0,REG_req_data[6:0]} + 8'd1;

          if (~REG_req_data[7] & hdma_hbl_r) begin
            // Clearing bit 7 while an H-Blank transfer is armed terminates it
            // and does NOT start a general purpose transfer -- a block already
            // requested but not yet started is cancelled too
            // (hdma_late_disable_1: the write lands between the trigger and
            // the CPU's next instruction boundary).
            hdma_hbl_r <= 1'b0;
            hdma_go_r  <= 1'b0;
          end
          else begin
            hdma_hbl_r <= REG_req_data[7];
            // GDMA runs now; HDMA runs now if we are already inside the
            // H-Blank window it would otherwise have waited for, and with
            // the LCD off it runs its first block now too (SameBoy
            // memory.c:1729, gambatte enableHdma(cc, lcden=false); same-suite
            // hdma_lcd_off reads HDMA5 = $02 right after writing $83, and
            // gdma_addr_mask is the same write with the address masks).
            hdma_go_r  <= ~REG_req_data[7] | hdma_in_hbl | ~REG_LCDC_r[`LCDC_DS_EN];
          end
        end
        // Turning the LCD off with an H-Blank transfer armed runs one block
        // unless the LCD was already in mode 0 (SameBoy display.c:569).
        8'h40: begin
          if (REG_LCDC_r[`LCDC_DS_EN] & ~REG_req_data[7] & hdma_hbl_r & (REG_STAT_r[`STAT_MODE] != `MODE_H))
            hdma_go_r <= 1'b1;
        end
      endcase
    end

    // ---- M-cycle accounting -------------------------------------------
    if (hdma_state_r == ST_HDMA_IDLE)          hdma_mcyc_r <= 5'h0;
    else if (CLK_BUS_EDGE & ~&hdma_mcyc_r)     hdma_mcyc_r <= hdma_mcyc_r + 5'd1;

    if (hdma_state_r != ST_HDMA_BLKE)          hdma_blke_r <= 2'h0;
    else if (CLK_BUS_EDGE & ~&hdma_blke_r)     hdma_blke_r <= hdma_blke_r + 2'd1;

    // ---- transfer -----------------------------------------------------
    case (hdma_state_r)
      ST_HDMA_IDLE: begin
        hdma_byte_r <= 4'h0;
        hdma_req_r  <= 1'b0;

        // The copy starts at the instruction boundary the bill starts at.
        if (hdma_go_r & hdma_bound_edge) begin
          hdma_go_r    <= 1'b0;
          hdma_state_r <= ST_HDMA_SYNC;
        end
      end
      ST_HDMA_SYNC: begin
        // Wait for the memory controller to be genuinely idle and for any OAM
        // DMA to finish before taking the bus.  Both are guaranteed to happen:
        // the CPU is stalled and its fetches are suppressed, and the OAM DMA
        // runs off the bus edge regardless of the CPU.
        //
        // "Idle" has to include the request that is only just being raised.
        // This state is entered on a bus edge, and the instruction fetch of
        // that very edge is NOT suppressed (hdma_bus_busy was still 0 when
        // ifd_req_r sampled it): the request reaches the controller one CLK
        // later, i.e. exactly when this test first runs and still sees
        // ST_MCT_IDLE.  Both then drove the system bus and the controller
        // completed the CPU's fetch with the HDMA's byte -- harmless when the
        // CPU already holds an instruction (the block-end refresh re-reads it),
        // fatal when the IFD is halfway through a multi-byte instruction and
        // goes on to use that byte as an operand (Shantae: `call $23A7` run
        // as `call $00A7`).  The same race sat between two GDMA blocks
        // (ST_HDMA_BLKE -> ST_HDMA_SYNC right after a bus edge), where it could
        // also corrupt the byte the HDMA itself was reading.
        if (mct_bus_idle & ~DMA_active)
          hdma_state_r <= ST_HDMA_READ;
      end
      ST_HDMA_READ: begin
        hdma_req_r   <= 1'b1;
        hdma_state_r <= ST_HDMA_WAIT;
      end
      ST_HDMA_WAIT: begin
        hdma_req_r  <= 1'b0;
        hdma_data_r <= SYS_RDDATA;

        if (~hdma_req_r & SYS_RDY) hdma_state_r <= ST_HDMA_WRITE;
      end
      ST_HDMA_WRITE: begin
        // One cycle: the VRAM port is ours (priority over PPU and CPU) and the
        // write lands from hdma_vram_* above.
        hdma_src_r   <= hdma_src_r + 16'd1;
        hdma_dst_r   <= hdma_dst_r + 13'd1;
        hdma_byte_r  <= hdma_byte_r + 4'd1;

        hdma_state_r <= (&hdma_byte_r) ? ST_HDMA_BLKE : ST_HDMA_READ;
      end
      ST_HDMA_BLKE: begin
        // A block costs 8 M-cycles of CPU time at single speed and 16 at
        // double speed (Pan Docs; hdma_block_mcyc above).  The copy itself
        // finishes sooner than that here, so the stall is PADDED to the real
        // cost instead of being handed back early: a game that counts cycles
        // across a DMA has to see what the hardware bills.
        //
        // The second term is not padding, it is correctness: the bus is free
        // in this state and the instruction fetch has to get a full request
        // and answer in before the CPU is let go (see hdma_bus_busy above).
        if ((hdma_mcyc_r >= hdma_block_mcyc) & (hdma_blke_r >= 2'd2)) begin
          hdma_byte_r  <= 4'h0;
          hdma_mcyc_r  <= 5'h0;
          hdma_steps_r <= hdma_steps_m1;

          if (hdma_steps_r <= 8'd1) begin
            hdma_hbl_r   <= 1'b0;          // done: HDMA5 now reads $FF
            hdma_state_r <= ST_HDMA_IDLE;
          end
          else if (hdma_hbl_r) begin
            hdma_state_r <= ST_HDMA_IDLE;  // this H-Blank's block is done
          end
          else begin
            hdma_state_r <= ST_HDMA_SYNC;  // GDMA: straight into the next block
          end
        end
      end
    endcase
  end
end

//-------------------------------------------------------------------
// SERIAL
//-------------------------------------------------------------------

// Functional on MK3 using 3-wire P* debug bus.  See config.vh to enable.

`ifdef SGB_SERIAL
reg  [7:0]  ser_clk_d_r;
reg  [9:0]  ser_ctr_r;
reg         ser_out_r;
reg  [3:0]  ser_pos_r;
reg         ser_done_r;

assign SER_CLK = ~REG_SC_r[0] ? 1'bZ : ser_ctr_r[9];
assign SER_OUT = ser_out_r;
//assign SER_OE = REG_SC_r[7]; // output (and input) enable when clock is valid
//assign SER_DIR = REG_SC_r[0]; // 0=clock driven by external source, 1=output when clock driven by this device

assign SER_REG_done = ser_done_r;

assign HLT_SER_rsp = HLT_REQ_sync & (~REG_SC_r[7] | ~REG_SC_r[0]);

assign ser_clk_d0 = ~REG_SC_r[7] ? 1'b1 : SER_CLK;

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    REG_SB_r <= 0;
    REG_SC_r <= 8'h7F;

    ser_done_r      <= 0;
    ser_ctr_r       <= 10'h200;
    ser_out_r       <= 1;
    ser_clk_d_r     <= 8'hFF;
  end
  else begin
    ser_clk_d_r <= {ser_clk_d_r[6:0],ser_clk_d0};

    if (REG_SC_r[7] & ~ser_pos_r[3]) begin
      // 1->0 transition
      if (ser_clk_d_r[5:0] == 6'b111000) begin
        ser_out_r <= REG_SB_r[7];
      end
      // 0->1 transition
      else if(ser_clk_d_r[5:0] == 6'b000111) begin
        REG_SB_r <= {REG_SB_r[6:0],SER_IN};
        ser_pos_r <= ser_pos_r + 1;
      end
    end

    if (CLK_CPU_EDGE) begin
      ser_done_r      <= 0;

      if (~REG_SC_r[7]) begin
        ser_ctr_r <= 10'h200;
        ser_pos_r <= 0;
      end
      else begin
        ser_ctr_r <= REG_SC_r[0] ? ser_ctr_r + 1 : ser_ctr_r;

        if (ser_pos_r[3]) begin
          REG_SC_r[7] <= 0;
          ser_done_r <= 1;
        end
      end
    end

    if (REG_req_val) begin
      case (REG_address)
        8'h01: REG_SB_r[7:0] <= REG_req_data[7:0];
        8'h02: begin
         REG_SC_r[7:0] <= REG_req_data[7:0];
         // support MCU/DBG triggering interrupt on 1->0 transition.  it's possible, but highly unlikely save states could trigger this and we
         // may not want that.  that would happen only on the few games that use this and require saving and then loading a state in
         // external clock mode when the state machine is active.  BMQ doesn't want to assert interrupt on CPU write of SC.
         if (reg_src_r & ~HLT_RSP) ser_done_r <= REG_SC_r[7] & ~REG_req_data[7];
        end
      endcase
    end
  end
end
`endif

//-------------------------------------------------------------------
// DBG
//-------------------------------------------------------------------

// DBG contains all the state we want to read out from the SGB via
// the MCU.  It mirrors the MCT pipe because the general operation is
// the same.  If fitting this logic becomes problematic it can either
// be removed entirely or integrated into the MCU pipe (with some
// concurrency limitations).

parameter
  ST_DBG_IDLE     = 8'b00000001,
  ST_DBG_DEC      = 8'b00000010,
  ST_DBG_VRAM     = 8'b00000100,
  ST_DBG_OAM      = 8'b00001000,
  ST_DBG_REG      = 8'b00010000,
  ST_DBG_HRAM     = 8'b00100000,
  ST_DBG_MISC     = 8'b01000000, // MISC state we want to export at 810000-87FFFF
  ST_DBG_END      = 8'b10000000;

reg  [1:0]  dbg_req_r;
reg  [7:0]  dbg_state_r;
reg  [15:0] dbg_addr_r;
reg         dbg_wr_r;
reg  [7:0]  dbg_mdr_r;

reg  [7:0]  dbg_misc_data_r = 0;

// return bogus data under reset to avoid having to keep parts of the CPU awake
assign MCU_RSP = |(dbg_state_r & ST_DBG_END) | cpu_ireset_r;
assign MCU_DATA_OUT = dbg_mdr_r;

// The debug window over VRAM is 16KB now, not 8: 0x808000-0x809FFF is bank 0
// and 0x80A000-0x80BFFF is bank 1 (contract section 1).  The GB's own decode
// (MCT_TGT_VRAM) is untouched at 8000-9FFF -- this wider one is the MCU's.
assign dbg_vram_wren = dbg_wr_r & |(dbg_state_r & ST_DBG_VRAM) & dbg_vram_free;
assign dbg_vram_address = dbg_addr_r[12:0];
assign dbg_vram_bank = dbg_addr_r[13];
assign dbg_vram_wrdata = dbg_mdr_r;

assign dbg_oam_wren = dbg_wr_r & |(dbg_state_r & ST_DBG_OAM);
assign dbg_oam_address = dbg_addr_r[7:0];
assign dbg_oam_wrdata = dbg_mdr_r;

assign dbg_hram_wren = dbg_wr_r & |(dbg_state_r & ST_DBG_HRAM);
assign dbg_hram_address = dbg_addr_r[6:0];
assign dbg_hram_wrdata = dbg_mdr_r;

assign DBG_REG_req_val = |(dbg_state_r & ST_DBG_REG);

assign DBG_REG_wren = dbg_wr_r & |(dbg_state_r & ST_DBG_REG);
assign DBG_REG_address = dbg_addr_r[7:0];
assign DBG_REG_data = dbg_mdr_r;

assign DBG_ADDR = dbg_addr_r[11:0];

// CRAM debug window (0x810100-0x81017F): the index into the palette RAM and
// the stall that keeps the pipe off port B while the bridge holds it.
assign cram_dbg_addr = dbg_addr_r[6:0];
wire dbg_cram_stall = (dbg_addr_r[11:8] == 4'h1) & (cram_b_busy | cram_b_busy_d1_r);

`ifdef SGB_DEBUG
wire [7:0] config_r[7:0];
`endif

always @(posedge CLK) begin
  if (cpu_ireset_r) begin
    dbg_state_r <= ST_DBG_IDLE;
    dbg_req_r   <= 0;
    dbg_wr_r    <= 0;
  end
  else begin
    case (dbg_state_r)
      ST_DBG_IDLE: begin
        if (MCU_RRQ | MCU_WRQ) begin
          dbg_addr_r  <= MCU_ADDR;
          dbg_wr_r    <= MCU_WRQ;
          if (MCU_WRQ) dbg_mdr_r <= MCU_DATA_IN;

          dbg_state_r <= ST_DBG_DEC;
        end
      end
      ST_DBG_DEC: begin
        if      (`DBG_TGT_VRAM(dbg_addr_r)) begin
          dbg_state_r <= ST_DBG_VRAM;
        end
        else if (`MCT_TGT_HIGH(dbg_addr_r)) begin
          dbg_state_r <= dbg_addr_r[8] ? ((~dbg_addr_r[7] | &dbg_addr_r[6:0]) ? ST_DBG_REG : ST_DBG_HRAM) : ST_DBG_OAM;
        end
        else begin
          dbg_state_r <= ST_DBG_MISC;
        end
      end
      ST_DBG_VRAM: begin
        if (~dbg_wr_r & dbg_vram_free) dbg_mdr_r <= dbg_vram_bank ? VRAM1_B_DATA : VRAM0_B_DATA;

        // Stall while the bridge holds port B (contract section 1: outside
        // HLT_REQ the port belongs to the views).
        if (dbg_vram_free) dbg_state_r <= ST_DBG_END;
      end
      ST_DBG_OAM: begin
        if (~dbg_wr_r & dbg_oam_free) dbg_mdr_r <= dbg_oam_rddata;

        // Stall while the bridge's snapshot copy engine holds port B, the same
        // rule the VRAM and CRAM windows follow.
        if (dbg_oam_free) dbg_state_r <= ST_DBG_END;
      end
      ST_DBG_REG: begin
        if (~dbg_wr_r) dbg_mdr_r <= REG_data;

        if (~dbg_req_r[0] & REG_DBG_rsp_val) dbg_state_r <= ST_DBG_END;
      end
      ST_DBG_HRAM: begin
        if (~dbg_wr_r) dbg_mdr_r <= dbg_hram_rddata;

        dbg_state_r <= ST_DBG_END;
      end
      ST_DBG_MISC: begin
        if (~dbg_wr_r) dbg_mdr_r <= dbg_misc_data_r;

        // DEC   - addr
        // MISC0 - dbg_req_r[0], dbg_row_rddata
        // MISC1 - dbg_req_r[1], data_in
        // MISC2 - dbg_misc_data_r
        //
        // The CRAM window rides the same port B rule as VRAM: the bridge wins
        // the cycle it asks for, the debug pipe waits until the snapshot
        // engine has let go.
        if (~|dbg_req_r & ~dbg_cram_stall) dbg_state_r <= ST_DBG_END;
      end
      ST_DBG_END: begin
        dbg_state_r <= ST_DBG_IDLE;
      end
    endcase

    dbg_req_r <= {dbg_req_r[0],|(dbg_state_r & ST_DBG_DEC)};
  end

`ifdef SGB_DEBUG
  case (dbg_addr_r[11:8])
    // ARCH
    4'h0: case(dbg_addr_r[7:0])
            8'h00:    dbg_misc_data_r <= ifd_pc[7:0];
            8'h01:    dbg_misc_data_r <= ifd_pc[15:8];
            8'h02:    dbg_misc_data_r <= F_r;
            8'h03:    dbg_misc_data_r <= A_r;
            8'h04:    dbg_misc_data_r <= C_r;
            8'h05:    dbg_misc_data_r <= B_r;
            8'h06:    dbg_misc_data_r <= E_r;
            8'h07:    dbg_misc_data_r <= D_r;
            8'h08:    dbg_misc_data_r <= L_r;
            8'h09:    dbg_misc_data_r <= H_r;
            8'h0A:    dbg_misc_data_r <= SP_r[7:0];
            8'h0B:    dbg_misc_data_r <= SP_r[15:8];

            default:  dbg_misc_data_r <= 0;
          endcase
    // ARCH/MMIO
    4'h1: casez(dbg_addr_r[7:0])
            8'h00:    dbg_misc_data_r <= REG_P1_r;
            8'h01:    dbg_misc_data_r <= REG_SB_r;
            8'h02:    dbg_misc_data_r <= REG_SC_r;

            8'h04:    dbg_misc_data_r <= REG_DIV_r;
            8'h05:    dbg_misc_data_r <= REG_TIMA_r;
            8'h06:    dbg_misc_data_r <= REG_TMA_r;
            8'h07:    dbg_misc_data_r <= REG_TAC_r;

            8'h0F:    dbg_misc_data_r <= REG_IF_r;

            8'h10:    dbg_misc_data_r <= REG_NR10_r;
            8'h11:    dbg_misc_data_r <= REG_NR11_r;
            8'h12:    dbg_misc_data_r <= REG_NR12_r;
            8'h13:    dbg_misc_data_r <= REG_NR13_r;
            8'h14:    dbg_misc_data_r <= REG_NR14_r;

            8'h16:    dbg_misc_data_r <= REG_NR21_r;
            8'h17:    dbg_misc_data_r <= REG_NR22_r;
            8'h18:    dbg_misc_data_r <= REG_NR23_r;
            8'h19:    dbg_misc_data_r <= REG_NR24_r;

            8'h1A:    dbg_misc_data_r <= REG_NR30_r;
            8'h1B:    dbg_misc_data_r <= REG_NR31_r;
            8'h1C:    dbg_misc_data_r <= REG_NR32_r;
            8'h1D:    dbg_misc_data_r <= REG_NR33_r;
            8'h1E:    dbg_misc_data_r <= REG_NR34_r;

            8'h20:    dbg_misc_data_r <= REG_NR41_r;
            8'h21:    dbg_misc_data_r <= REG_NR42_r;
            8'h22:    dbg_misc_data_r <= REG_NR43_r;
            8'h23:    dbg_misc_data_r <= REG_NR44_r;

            8'h24:    dbg_misc_data_r <= REG_NR50_r;
            8'h25:    dbg_misc_data_r <= REG_NR51_r;
            8'h26:    dbg_misc_data_r <= REG_NR52_r;

            8'h3?:    dbg_misc_data_r <= REG_WAV_r[dbg_addr_r[3:0]];

            8'h40:    dbg_misc_data_r <= REG_LCDC_r;
            8'h41:    dbg_misc_data_r <= REG_STAT_r;
            8'h42:    dbg_misc_data_r <= REG_SCY_r;
            8'h43:    dbg_misc_data_r <= REG_SCX_r;
            8'h44:    dbg_misc_data_r <= REG_LY_r;
            8'h45:    dbg_misc_data_r <= REG_LYC_r;
            8'h46:    dbg_misc_data_r <= REG_DMA_r;
            8'h47:    dbg_misc_data_r <= REG_BGP_r;
            8'h48:    dbg_misc_data_r <= REG_OBP0_r;
            8'h49:    dbg_misc_data_r <= REG_OBP1_r;
            8'h4A:    dbg_misc_data_r <= REG_WY_r;
            8'h4B:    dbg_misc_data_r <= REG_WX_r;

            8'h50:    dbg_misc_data_r <= REG_BOOT_r;

            8'hFF:    dbg_misc_data_r <= REG_IE_r;

            default:  dbg_misc_data_r <= 0;
          endcase
    // IFD
    4'h2: case(dbg_addr_r[7:0])
            8'h00:    dbg_misc_data_r <= ifd_op_r;
            8'h01:    dbg_misc_data_r <= ifd_size_r;
            8'h02:    dbg_misc_data_r <= ifd_data_r;
            8'h03:    dbg_misc_data_r <= ifd_decode_r[7:0];
            8'h04:    dbg_misc_data_r <= ifd_decode_r[15:8];
            8'h05:    dbg_misc_data_r <= MCT_IFD_rsp_val;
            8'h06:    dbg_misc_data_r <= ifd_size_r;
            8'h07:    dbg_misc_data_r <= ifd_decode_r[`DEC_SZE];
            8'h08:    dbg_misc_data_r <= ifd_pc[7:0];
            8'h09:    dbg_misc_data_r <= ifd_pc[15:8];
            8'h0A:    dbg_misc_data_r <= PC_r[7:0];
            8'h0B:    dbg_misc_data_r <= PC_r[15:8];
            8'h0C:    dbg_misc_data_r <= ifd_complete_r;

            default:  dbg_misc_data_r <= 0;
          endcase
    // EXE
    4'h3: case(dbg_addr_r[7:0])
            8'h00:    dbg_misc_data_r <= IFD_EXE_valid;
            8'h01:    dbg_misc_data_r <= IFD_EXE_op[7:0];
            8'h02:    dbg_misc_data_r <= IFD_EXE_op[15:8];
            8'h03:    dbg_misc_data_r <= IFD_EXE_op[23:16];
            8'h04:    dbg_misc_data_r <= exe_ime_r;

            8'h10:    dbg_misc_data_r <= IFD_EXE_decode[`DEC_GRP];
            8'h11:    dbg_misc_data_r <= IFD_EXE_decode[`DEC_LAT];
            8'h12:    dbg_misc_data_r <= IFD_EXE_decode[`DEC_DST];
            8'h13:    dbg_misc_data_r <= IFD_EXE_decode[`DEC_SRC];
            8'h14:    dbg_misc_data_r <= IFD_EXE_decode[`DEC_SZE];
            8'h15:    dbg_misc_data_r <= IFD_EXE_cb;

            8'h20:    dbg_misc_data_r <= exe_ctr_r;
            8'h21:    dbg_misc_data_r <= 0;
            8'h22:    dbg_misc_data_r <= exe_stage;
            8'h23:    dbg_misc_data_r <= exe_advance_r;
            8'h24:    dbg_misc_data_r <= exe_ready_r;
            8'h25:    dbg_misc_data_r <= exe_complete_r;
            8'h26:    dbg_misc_data_r <= exe_lat_add_r;
            8'h27:    dbg_misc_data_r <= exe_lat;

            8'h30:    dbg_misc_data_r <= IFD_EXE_pc_start[7:0];
            8'h31:    dbg_misc_data_r <= IFD_EXE_pc_start[15:8];
            8'h32:    dbg_misc_data_r <= IFD_EXE_pc_end[7:0];
            8'h33:    dbg_misc_data_r <= IFD_EXE_pc_end[15:8];
            8'h34:    dbg_misc_data_r <= IFD_EXE_pc_next[7:0];
            8'h35:    dbg_misc_data_r <= IFD_EXE_pc_next[15:8];

            8'h40:    dbg_misc_data_r <= exe_res_r[7:0];
            8'h41:    dbg_misc_data_r <= exe_res_r[15:8];
            8'h42:    dbg_misc_data_r <= exe_src_r[7:0];
            8'h43:    dbg_misc_data_r <= exe_src_r[15:8];
            8'h44:    dbg_misc_data_r <= exe_dst_r[7:0];
            8'h45:    dbg_misc_data_r <= exe_dst_r[15:8];
            8'h46:    dbg_misc_data_r <= exe_res_cc_r[7:0];
            8'h47:    dbg_misc_data_r <= exe_cc_r[7:0];
            8'h48:    dbg_misc_data_r <= EXE_MCT_req_addr_d1[7:0];
            8'h49:    dbg_misc_data_r <= EXE_MCT_req_addr_d1[15:8];
            8'h4A:    dbg_misc_data_r <= exe_mem_data_r[7:0];
            8'h4B:    dbg_misc_data_r <= exe_mem_data_r[15:8];
            8'h4C:    dbg_misc_data_r <= exe_res_los_r;
            8'h4D:    dbg_misc_data_r <= exe_src_alu_r;

            8'h50:    dbg_misc_data_r <= EXE_IFD_redirect;
            8'h51:    dbg_misc_data_r <= EXE_IFD_target[7:0];
            8'h52:    dbg_misc_data_r <= EXE_IFD_target[15:8];
            8'h53:    dbg_misc_data_r <= exe_pc_prev_r[7:0];
            8'h54:    dbg_misc_data_r <= exe_pc_prev_r[15:8];
            8'h55:    dbg_misc_data_r <= exe_pc_prev_redirect_r[7:0];
            8'h56:    dbg_misc_data_r <= exe_pc_prev_redirect_r[15:8];
            8'h57:    dbg_misc_data_r <= exe_target_prev_redirect_r[7:0];
            8'h58:    dbg_misc_data_r <= exe_target_prev_redirect_r[15:8];

            //8'h60:    dbg_misc_data_r <= tmp_div_r[3:0];
            //8'h61:    dbg_misc_data_r <= tmp_latency_r;
            //8'h62:    dbg_misc_data_r <= tmp_latency2_r;
            //8'h63:    dbg_misc_data_r <= tmp_latency3_r;

            8'h70:    dbg_misc_data_r <= IFD_EXE_int;

            default:  dbg_misc_data_r <= 0;
          endcase
`ifndef MK2
    // MCT,REG
    4'h4: casez(dbg_addr_r[7:0])
            8'h00:    dbg_misc_data_r <= mct_state_r;
            8'h01:    dbg_misc_data_r <= mct_req_r;
            8'h02:    dbg_misc_data_r <= mct_addr_r[7:0];
            8'h03:    dbg_misc_data_r <= mct_addr_r[15:8];
            8'h04:    dbg_misc_data_r <= mct_src_r;
            8'h05:    dbg_misc_data_r <= mct_wr_r;
            8'h06:    dbg_misc_data_r <= mct_mdr_r;

            8'h10:    dbg_misc_data_r <= reg_state_r;
            8'h11:    dbg_misc_data_r <= reg_req_r;
            8'h12:    dbg_misc_data_r <= reg_addr_r[6:0];
            //8'h13:    dbg_misc_data_r <= 0;
            8'h14:    dbg_misc_data_r <= reg_src_r;
            8'h15:    dbg_misc_data_r <= reg_wr_r;
            8'h16:    dbg_misc_data_r <= reg_mdr_r;

            8'h20:    dbg_misc_data_r <= mct_vram_error_r[7:0];
            8'h21:    dbg_misc_data_r <= mct_vram_error_r[15:8];
            8'h22:    dbg_misc_data_r <= mct_oam_error_r[7:0];
            8'h23:    dbg_misc_data_r <= mct_oam_error_r[15:8];

            8'hC0:    dbg_misc_data_r <= HLT_REQ_sync;
            8'hC1:    dbg_misc_data_r <= HLT_RSP;
            8'hC2:    dbg_misc_data_r <= HLT_IFD_rsp;
            8'hC3:    dbg_misc_data_r <= HLT_EXE_rsp;
            8'hC4:    dbg_misc_data_r <= HLT_DMA_rsp;
            8'hC5:    dbg_misc_data_r <= HLT_SER_rsp;
            8'hC6:    dbg_misc_data_r <= ~|ifd_size_r;
            8'hC7:    dbg_misc_data_r <= ~ifd_int_r;
            8'hC8:    dbg_misc_data_r <= EXE_IFD_ime;
            8'hC9:    dbg_misc_data_r <= IDL_ICD;

            8'hD?:    dbg_misc_data_r <= DBG_MAIN_DATA_IN;
            8'hE?:    dbg_misc_data_r <= DBG_CHEAT_DATA_IN;
            8'hF?:    dbg_misc_data_r <= DBG_MBC_DATA_IN;

            default:  dbg_misc_data_r <= 0;
          endcase
    // PPU
    4'h5: casez(dbg_addr_r[7:0])
            8'h00:    dbg_misc_data_r <= PPU_HSYNC_EDGE;
            8'h01:    dbg_misc_data_r <= PPU_VSYNC_EDGE;
            8'h02:    dbg_misc_data_r <= PPU_PIXEL_VALID;
            8'h03:    dbg_misc_data_r <= PPU_PIXEL;
            8'h04:    dbg_misc_data_r <= PPU_DOT_EDGE;

            8'h10:    dbg_misc_data_r <= ppu_state_r[7:0];
            8'h11:    dbg_misc_data_r <= ppu_state_r[12:8];
            8'h12:    dbg_misc_data_r <= ppu_dot_ctr_r[7:0];
            8'h13:    dbg_misc_data_r <= ppu_dot_ctr_r[8];

            8'h20:    dbg_misc_data_r <= ppu_tile_ctr_r[1:0];
            //8'h21:    dbg_misc_data_r <= 0;
            8'h22:    dbg_misc_data_r <= ppu_tile_ctr_r;
            8'h23:    dbg_misc_data_r <= ppu_pix_ctr_r;
            //8'h24:    dbg_misc_data_r <= 0;
            8'h25:    dbg_misc_data_r <= ppu_first_frame_r;

            8'h30:    dbg_misc_data_r <= dbg_reg_ly_r[7:0];
            8'h31:    dbg_misc_data_r <= dbg_dot_ctr_r[7:0];
            8'h32:    dbg_misc_data_r <= dbg_dot_ctr_r[8:8];
            8'h33:    dbg_misc_data_r <= dbg_oam_active_r;
            8'h34:    dbg_misc_data_r <= dbg_vram_active_r;
            8'h35:    dbg_misc_data_r <= dbg_dma_active_r;

            8'h40:    dbg_misc_data_r <= ppu_stat_active_r;
            8'h41:    dbg_misc_data_r <= ppu_stat_match_r;
            8'h42:    dbg_misc_data_r <= dbg_ppu_stat_match_r;
            8'h43:    dbg_misc_data_r <= dbg_ppu_stat_dot_ctr_r[7:0];
            8'h44:    dbg_misc_data_r <= dbg_ppu_stat_dot_ctr_r[8:8];
            //8'h45:    dbg_misc_data_r <= dbg_timer_ly_r;
            //8'h46:    dbg_misc_data_r <= dbg_timer_dot_ctr_r[7:0];
            //8'h47:    dbg_misc_data_r <= dbg_timer_dot_ctr_r[8:8];

            8'h50:    dbg_misc_data_r <= ser_clk_d_r;
            8'h51:    dbg_misc_data_r <= ser_done_r;
            8'h52:    dbg_misc_data_r <= ser_pos_r;
            8'h55:    dbg_misc_data_r <= ser_ctr_r[7:0];
            8'h56:    dbg_misc_data_r <= ser_ctr_r[9:8];

            8'hA0:    dbg_misc_data_r <= apu_square1_enable_r;
            8'hA1:    dbg_misc_data_r <= apu_square1_timer_r[7:0];
            8'hA2:    dbg_misc_data_r <= apu_square1_timer_r[12:8];
            8'hA3:    dbg_misc_data_r <= apu_square1_length_r;
            8'hA4:    dbg_misc_data_r <= apu_square1_env_timer_r;
            8'hA5:    dbg_misc_data_r <= apu_square1_volume_r;
            8'hA6:    dbg_misc_data_r <= apu_square1_pos_r;
            8'hA7:    dbg_misc_data_r <= apu_square1_sweep_enable_r;
            8'hA8:    dbg_misc_data_r <= apu_square1_sweep_freq_r[7:0];
            8'hA9:    dbg_misc_data_r <= apu_square1_sweep_freq_r[10:8];
            8'hAA:    dbg_misc_data_r <= apu_square1_period[7:0];
            8'hAB:    dbg_misc_data_r <= apu_square1_period[12:8];
            //8'hAC:    dbg_misc_data_r <= apu_square1_duty;
            8'hAF:    dbg_misc_data_r <= apu_square1_output[4:0];

            8'hB0:    dbg_misc_data_r <= apu_square2_enable_r;
            8'hB1:    dbg_misc_data_r <= apu_square2_timer_r[7:0];
            8'hB2:    dbg_misc_data_r <= apu_square2_timer_r[12:8];
            8'hB3:    dbg_misc_data_r <= apu_square2_length_r;
            8'hB4:    dbg_misc_data_r <= apu_square2_env_timer_r;
            8'hB5:    dbg_misc_data_r <= apu_square2_volume_r;
            8'hB6:    dbg_misc_data_r <= apu_square2_pos_r;
            //8'hB7:    dbg_misc_data_r <= apu_square2_sweep_enable_r;
            //8'hB8:    dbg_misc_data_r <= apu_square2_sweep_freq_r[7:0];
            //8'hB9:    dbg_misc_data_r <= apu_square2_sweep_freq_r[15:8];
            8'hBA:    dbg_misc_data_r <= apu_square2_period[7:0];
            8'hBB:    dbg_misc_data_r <= apu_square2_period[12:8];
            //8'hBC:    dbg_misc_data_r <= apu_square2_duty;
            8'hBF:    dbg_misc_data_r <= apu_square2_output[4:0];

            8'hC0:    dbg_misc_data_r <= apu_wave_enable_r;
            8'hC1:    dbg_misc_data_r <= apu_wave_length_r[7:0];
            //8'hC2:    dbg_misc_data_r <= 0;
            8'hC3:    dbg_misc_data_r <= apu_wave_pos_r;
            8'hC4:    dbg_misc_data_r <= apu_wave_timer_r[7:0];
            8'hC5:    dbg_misc_data_r <= apu_wave_timer_r[11:8];
            //8'hC6:    dbg_misc_data_r <= apu_wave_timer_r[23:16];
            //8'hC7:    dbg_misc_data_r <= apu_wave_timer_r[31:24];
            8'hC8:    dbg_misc_data_r <= apu_wave_period[7:0];
            8'hC9:    dbg_misc_data_r <= apu_wave_period[11:8];
            8'hCF:    dbg_misc_data_r <= apu_wave_output[4:0];

            8'hD0:    dbg_misc_data_r <= apu_noise_enable_r;
            8'hD1:    dbg_misc_data_r <= apu_noise_length_r;
            8'hD2:    dbg_misc_data_r <= apu_noise_env_timer_r;
            8'hD3:    dbg_misc_data_r <= apu_noise_volume_r;
            8'hD4:    dbg_misc_data_r <= apu_noise_timer_r[7:0];
            8'hD5:    dbg_misc_data_r <= apu_noise_timer_r[15:8];
            8'hD6:    dbg_misc_data_r <= apu_noise_timer_r[21:16];
            //8'hD7:    dbg_misc_data_r <= apu_noise_timer_r[31:24];
            8'hD8:    dbg_misc_data_r <= apu_noise_period[7:0];
            8'hD9:    dbg_misc_data_r <= apu_noise_period[15:8];
            8'hDA:    dbg_misc_data_r <= apu_noise_period[21:16];
            //8'hDB:    dbg_misc_data_r <= apu_noise_period[31:24];
            8'hDC:    dbg_misc_data_r <= apu_noise_lfsr_r[7:0];
            8'hDD:    dbg_misc_data_r <= apu_noise_lfsr_r[14:8];
            8'hDF:    dbg_misc_data_r <= apu_noise_output[4:0];

            8'hE0:    dbg_misc_data_r <= APU_DAT[7:0];
            8'hE1:    dbg_misc_data_r <= APU_DAT[9:8];
            8'hE2:    dbg_misc_data_r <= APU_DAT[17:10];
            8'hE3:    dbg_misc_data_r <= APU_DAT[19:18];

            default:  dbg_misc_data_r <= 0;
          endcase
`endif

    // bridge
    4'h6: dbg_misc_data_r <= DBG_GBDG_DATA_IN;

    // CONFIG
    4'h7: case(dbg_addr_r[7:0])
            8'h00:    dbg_misc_data_r <= config_r[0];
            8'h01:    dbg_misc_data_r <= config_r[1];
            8'h02:    dbg_misc_data_r <= config_r[2];
            8'h03:    dbg_misc_data_r <= config_r[3];
            8'h04:    dbg_misc_data_r <= config_r[4];
            8'h05:    dbg_misc_data_r <= config_r[5];
            8'h06:    dbg_misc_data_r <= config_r[6];
            8'h07:    dbg_misc_data_r <= config_r[7];

            default:  dbg_misc_data_r <= 0;
          endcase

    default: dbg_misc_data_r <= 0;
  endcase
`else
  // GBC debug window, reached whenever the MCU addresses the SGB space outside
  // VRAM/OAM/IO/HRAM.  The group is dbg_addr_r[11:8], so with the firmware
  // reading at 0x81xxxx:
  //   0x810000-0x81003F   GBDG   bridge telemetry (contract section 10)
  //   0x810100-0x81017F   CRAM   64B background + 64B object palettes
  //   0x810D00-0x810D0F   main.v state
  //   0x810E00-0x810E0F   cheat.v state
  //   0x810F00-0x810F0F   MBC state
  // Everything else reads $00.  The SGB's large architectural/PPU/APU dump
  // lives under SGB_DEBUG and is not built here.
  case (dbg_addr_r[11:8])
    4'h0:    dbg_misc_data_r <= DBG_GBDG_DATA_IN;
    4'h1:    dbg_misc_data_r <= cram_q_b_r;
    4'hD:    dbg_misc_data_r <= DBG_MAIN_DATA_IN;
    4'hE:    dbg_misc_data_r <= DBG_CHEAT_DATA_IN;
    4'hF:    dbg_misc_data_r <= DBG_MBC_DATA_IN;
    default: dbg_misc_data_r <= 0;
  endcase
`endif

end

reg         step_r;
assign      DBG_EXE_step = step_r;

`ifdef SGB_DEBUG
assign {config_r[7],config_r[6],config_r[5],config_r[4],config_r[3],config_r[2],config_r[1],config_r[0]} = DBG_CONFIG;

assign      dbg_brk_enabled          = config_r[0][0];
assign      dbg_brk_matchpartialinst = config_r[0][1];
//
wire [7:0]  dbg_brk_stepcnt      = config_r[1];
wire [7:0]  dbg_brk_data_watch   = config_r[4];
wire [15:0] dbg_brk_addr_watch   = {config_r[6],config_r[5]};

// breakpoints
reg         dbg_brk_inst_rd_byte = 0;
reg         dbg_brk_data_rd_byte = 0;
reg         dbg_brk_data_wr_byte = 0;
reg         dbg_brk_inst_rd_addr = 0;
reg         dbg_brk_data_rd_addr = 0;
reg         dbg_brk_data_wr_addr = 0;
reg         dbg_brk_data         = 0;
reg         dbg_brk_stop         = 0;
reg         dbg_brk_error        = 0;

reg [15:0]  dbg_brk_addr_r;
reg [7:0]   dbg_brk_data_r;

reg [7:0]   stepcnt_r = 0;
always @(posedge CLK) begin
  step_r <= ~dbg_brk_enabled | (stepcnt_r != dbg_brk_stepcnt);
  if (CLK_BUS_EDGE & exe_advance_r) stepcnt_r <= dbg_brk_stepcnt;
end

assign DBG_BRK = |(config_r[2] & {dbg_brk_error,dbg_brk_stop,dbg_brk_data_wr_addr,dbg_brk_data_rd_addr,dbg_brk_inst_rd_addr,dbg_brk_data_wr_byte,dbg_brk_data_rd_byte,dbg_brk_inst_rd_byte});// | RST;

reg dbg_mem_req_val_d1_r;
reg dbg_mem_req_wr_d1_r;

reg  [15:0] dbg_mct_vram_error_r;
reg  [15:0] dbg_mct_oam_error_r;

always @(posedge CLK) begin
  if (RST) begin
    dbg_brk_inst_rd_byte <= 0;
    dbg_brk_data_rd_byte <= 0;
    dbg_brk_data_wr_byte <= 0;

    dbg_brk_inst_rd_addr <= 0;
    dbg_brk_data_rd_addr <= 0;
    dbg_brk_data_wr_addr <= 0;
    dbg_brk_stop         <= 0;
    dbg_brk_error        <= 0;

    dbg_brk_addr_r       <= 0;
  end
  else begin
    dbg_brk_inst_rd_addr <= IFD_EXE_valid && (IFD_EXE_pc_start == dbg_brk_addr_r);
    dbg_brk_data_rd_addr <= (exe_advance_r && exe_complete_r) ? 0 : (IFD_EXE_valid && dbg_mem_req_val_d1_r && ~dbg_mem_req_wr_d1_r && EXE_MCT_req_addr_d1 == dbg_brk_addr_r); //&& (!config_r[2][0] ||     mmc_data_r[7:0] == dbg_brk_data_r);
    dbg_brk_data_wr_addr <= (exe_advance_r && exe_complete_r) ? 0 : (IFD_EXE_valid && dbg_mem_req_val_d1_r &&  dbg_mem_req_wr_d1_r && EXE_MCT_req_addr_d1 == dbg_brk_addr_r && (!config_r[2][0] || EXE_MCT_req_data_d1 == dbg_brk_data_r));
    dbg_brk_stop         <= IFD_EXE_valid & IFD_EXE_int;
    dbg_brk_error        <= (  0
                            || (mct_vram_error_r != dbg_mct_vram_error_r)
                            || (mct_oam_error_r != dbg_mct_oam_error_r)
                            );

    dbg_brk_addr_r <= dbg_brk_addr_watch;
    dbg_brk_data_r <= dbg_brk_data_watch;

    dbg_mem_req_val_d1_r <= EXE_MCT_req_val;
    dbg_mem_req_wr_d1_r <= EXE_MCT_req_wr;

    dbg_mct_vram_error_r <= mct_vram_error_r;
    dbg_mct_oam_error_r <= mct_oam_error_r;
  end
end

`else
always @(posedge CLK) step_r <= 1;

assign DBG_BRK = 0;
`endif

endmodule
