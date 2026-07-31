`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// Company: Rehkopf
// Engineer: Rehkopf (sd2snes_sgb original), adapted for sd2snes_nes Phase -1
//
// Module Name:    main
// Description: Master Control FSM -- sd2snes_nes
//
// ADAPTED FROM sd2snes_sgb/main.v. This is a mechanical, NOT synthesis-validated
// port (see the Phase -1 report -- no Quartus run has touched this file). It
// keeps every piece of "chassis" the CONTRACT calls generic (PSRAM/SRAM
// arbitration FSMs, SPI, mcu_cmd, SD DMA, DAC, MSU) essentially untouched, and
// replaces the `sgb` instantiation with `nes_wrap`. Renamed `SGB_ROM_*` signals
// to `NES_ROM_*` throughout (same FSM, same 9-cycle-typical/~18-worst-case
// latency contract nes_wrap.v's own S_REQ/S_WAIT states were built against --
// see nes_wrap.v's header comment for the handshake this side must honor:
// RRQ/WRQ is a genuine pulse from nes_wrap's side, and NES_ROM_RDY must only
// drop on the cycle *after* the pulse is recognized, exactly like
// RQ_SGB_ROM_RDYr below).
//
// Removed relative to sd2snes_sgb/main.v (none of this exists yet for the NES
// core -- see CONTRACT SS3.5/SS7/SS9 for why each is out of scope for Phase -1):
//   - SGB_SAVE_STATES ctx_state_r save-state copier (no savestate concept yet
//     without a SNES-side program to hook, and no button-combo detector).
//   - RTC wiring (SGB_RTC_DAT/MCU_RTC_*) -- nes_wrap has no RTC port.
//   - `cheat` instantiation and everything downstream of it (button_ctx_valid/
//     mode, snescmd_we_cheat, pad_latch/pad_cnt/snes_ajr $4200/$4016 snooping,
//     DBG_CHEAT_DATA_OUT) -- all of it existed to feed the SGB save-state combo
//     and the button-snoop shortcut for the sys-sgb SNES-side program; neither
//     applies without one.
//   - SGB_SNES_DATA_OUT / sgb_enable terms in the SNES_DATA/OE/DIR muxes --
//     address.v (Phase -1) has no MMIO window to arbitrate.
// Kept: SNES bus synchronizers, sd_dma, dac, msu, spi, mcu_cmd, address, the
// MCU<->PSRAM and MCU<->SRAM(Bus 2) arbitration FSMs (renamed ST_NES_ROM_* from
// ST_SGB_ROM_*), r213f/r2100 compatibility patches (core-agnostic fixups, see
// CONTRACT SS5.1).
//////////////////////////////////////////////////////////////////////////////////
`include "config.vh"

// ============================================================================
// IMPLICIT NETS = FUTURE SILENCE (rule added after the audio-mute hunt, map
// report #18 Warning 10236): an undeclared name in a port connection creates
// a 1-bit implicit wire SILENTLY.  When such a net has a driver it happens to
// work (the SGB core ships that way and sounds); when it does not -- or when
// the intended signal is wider than 1 bit -- synthesis grounds the cone with
// nothing but a warning.  This file now declares EVERY net explicitly and
// compiles under `default_nettype none (reverted to wire at end-of-file so
// the other compilation-unit files are unaffected).  Keep it that way: any
// new undeclared name is a hard error here on purpose.
// ============================================================================
`default_nettype none

module main(
`ifdef MK2
  /* Bus 1: PSRAM, 128Mbit, 16bit, 70ns */
  output [22:0] ROM_ADDR,
  output ROM_CE,
  input MCU_OVR,
  /* debug */
  output p113_out,
`endif
`ifdef MK3
  input SNES_CIC_CLK,
  /* Bus 1: 2x PSRAM, 64Mbit, 16bit, 70ns */
  output [21:0] ROM_ADDR,
  output ROM_1CE,
  output ROM_2CE,
  output ROM_ZZ,
  /* debug */
  output PM6_out,
  output PN6_out,
  input  PT5_in,
`endif

  /* input clock */
  input CLKIN,

  /* SNES signals */
  input [23:0] SNES_ADDR_IN,
  input SNES_READ_IN,
  input SNES_WRITE_IN,
  input SNES_ROMSEL_IN,
  inout [7:0] SNES_DATA,
  input SNES_CPU_CLK_IN,
  input SNES_REFRESH,
  output SNES_IRQ,
  output SNES_DATABUS_OE,
  output SNES_DATABUS_DIR,
  input SNES_SYSCLK,

  input [7:0] SNES_PA_IN,
  input SNES_PARD_IN,
  input SNES_PAWR_IN,

  /* SRAM signals */
  inout [15:0] ROM_DATA,
  output ROM_OE,
  output ROM_WE,
  output ROM_BHE,
  output ROM_BLE,

  /* Bus 2: SRAM, 4Mbit, 8bit, 45ns */
  inout [7:0] RAM_DATA,
  output [18:0] RAM_ADDR,
  output RAM_OE,
  output RAM_WE,

  /* MCU signals */
  input SPI_MOSI,
  inout SPI_MISO,
  input SPI_SS,
  input SPI_SCK,

  output MCU_RDY,

  output DAC_MCLK,
  output DAC_LRCK,
  output DAC_SDOUT,

  /* SD signals */
  input [3:0] SD_DAT,
  inout SD_CMD,
  inout SD_CLK
);

wire CLK2;

wire MCU_ROM;
wire MCU_RAM;
wire MCU_RRQ;
wire MCU_WRQ;
wire [7:0] MCU_DOUT;

wire [7:0] spi_cmd_data;
wire [7:0] spi_param_data;
wire [7:0] spi_input_data;
wire [31:0] spi_byte_cnt;
wire [2:0] spi_bit_cnt;
wire [23:0] MCU_ADDR;
wire [3:0] MAPPER;
// mapper_flags[15:0] do core NES, escrito pelo firmware via CHIPFEAT (SPI 0xef
// -> nes_feat_out em mcu_cmd.v). Quase-estatico por CONTRATO (false-path no
// main.sdc): o firmware so' escreve durante o game-load, antes de soltar o
// reset do core (SNES_reset_strobe), e nunca reprograma com o core rodando.
wire [15:0] NES_FEAT;
wire [23:0] SAVERAM_MASK;
wire [23:0] ROM_MASK;
wire [7:0] SD_DMA_SRAM_DATA;
wire [1:0] SD_DMA_TGT;
wire [10:0] SD_DMA_PARTIAL_START;
wire [10:0] SD_DMA_PARTIAL_END;

wire [10:0] dac_addr;
wire [2:0] dac_vol_select_out;
wire [8:0] dac_ptr_addr;
wire [7:0] msu_volumerq_out;

// --- formerly-IMPLICIT nets, now explicit (see header rule). Groups: ---
// dac/msu handshake (all driven: mcu_cmd/msu/dac outputs)
wire        DAC_STATUS;
wire        dac_play;
wire        dac_reset;
wire        dac_palmode_out;
wire        msu_volume_latch_out;
// pll
wire        DCM_LOCKED;
wire        DCM_RST;            // driven by the assign below (constant 0)
// sd_dma
wire        SD_DMA_EN;
wire        SD_DMA_STATUS;
wire        SD_DMA_SRAM_WE;
wire        SD_DMA_NEXTADDR;
wire        SD_DMA_PARTIAL;
wire        SD_DMA_START_MID_BLOCK;
wire        SD_DMA_END_MID_BLOCK;
wire [10:0] SD_DMA_DBG_cyclecnt; // was a 1-bit implicit: DBG bus silently truncated
wire [2:0]  SD_DMA_DBG_clkcnt;   // was a 1-bit implicit: DBG bus silently truncated
// spi
wire        spi_cmd_ready;
wire        spi_param_ready;
wire        spi_endmessage;
wire        spi_startmessage;
wire        MCU_WRITE;
// msu <-> mcu_cmd
wire        msu_enable;
wire        msu_status_reset_we;
wire        msu_addr_reset;
wire        DBG_msu_reg_oe_rising;
wire        DBG_msu_reg_oe_falling;
wire        DBG_msu_reg_we_rising;
wire        DBG_msu_address_ext_write_rising;
wire [13:0] DBG_msu_address;     // was a 1-bit implicit: DBG bus silently truncated
// mcu_cmd / address decodes
wire        mcu_region;
wire        snescmd_we_mcu;
wire        IS_WRITABLE;
wire        r213f_enable;
wire        r2100_hit;
wire        snescmd_enable;
wire [7:0] msu_status_out;
wire [31:0] msu_addressrq_out;
wire [15:0] msu_trackrq_out;
wire [13:0] msu_write_addr;
wire [13:0] msu_ptr_addr;
wire [7:0] MSU_SNES_DATA_IN;
wire [7:0] MSU_SNES_DATA_OUT;
wire [5:0] msu_status_reset_bits;
wire [5:0] msu_status_set_bits;

wire [19:0] NES_APU_DAT;
wire        NES_APU_CLK_EDGE;

wire       NES_ROM_RRQ;
wire       NES_ROM_WRQ;
wire       NES_ROM_WORD;
wire        NES_FREE_SLOT;

wire [15:0] featurebits;
wire feat_cmd_unlock = featurebits[5];

wire [23:0] MAPPED_SNES_ADDR;
wire ROM_ADDR0;

wire [15:0] dsp_feat;

wire [8:0] snescmd_addr_mcu;
wire [7:0] snescmd_data_out_mcu;
wire [7:0] snescmd_data_in_mcu;

// config / breadcrumb bus (mcu_cmd.v 0xf9/0xfa; see nes_wrap.v)
wire [7:0] reg_group;
wire [7:0] reg_index;
wire [7:0] reg_value;
wire [7:0] reg_invmask;
wire       reg_we;
wire [7:0] reg_read;
wire [7:0] nes_config_data;

// video-bridge mailbox window + control block (declared before the nes_wrap
// instance that drives/consumes them; logic + decode further down)
wire        nesbox_enable, nesctl_enable, nes_chr_enable;
wire [7:0]  NESBOX_DATA;
wire [15:0] NESBOX_FRAME_SEQ, NESBOX_FRAME_LEN;
wire [7:0]  NESBOX_STATUS;
reg  [15:0] nes_frame_ack = 16'd0;
reg         nes_buf_sel   = 1'b0;
reg  [15:0] nes_ctrl_p1   = 16'd0, nes_ctrl_p2 = 16'd0;
// NES_GO (control block +0x0E, boot handshake): 0 apos reset do SNES -> o core
// NES nasce SEGURO em reset (nes_wrap core_hold) enquanto o renderer faz o
// upload de CHR ($50/$60 -> VRAM, ~3ms); o renderer escreve 1 quando termina e
// o core solta.  Limpo por SNES_reset_strobe (novo load/IGR refaz o handshake).
reg         nes_go        = 1'b0;
// SNES-CHR read client data register (declarado aqui p/ o mux SNES_DATA;
// logica no bloco "SNES-CHR read client" adiante)
reg  [7:0]  NES_CHR_DINr;
// v2.0b: byte efetivamente servido pela janela (cache/prefetch; assign no
// bloco do cliente adiante -- forward decl pelo mesmo motivo do DINr acima)
wire [7:0]  NES_CHR_SERVE;
// (hoisted: era declarado DEPOIS do primeiro uso no mux SNES_DATA -- Quartus
// tolera a forward-ref, iverilog nao; movido pra ca p/ o tb_mcu_path compilar
// o main.v REAL sem copia.  Semantica original mantida: sem programa SNES na
// Fase -1, mailbox destravada p/ o MCU exercitar READMEM/WRITEMEM/SNESCMD_*.)
wire snescmd_unlock = 1'b1;

reg [7:0] SNES_PARDr = 8'b11111111;
reg [7:0] SNES_PAWRr = 8'b11111111;
reg [7:0] SNES_READr = 8'b11111111;
reg [7:0] SNES_WRITEr = 8'b11111111;
reg [7:0] SNES_CPU_CLKr = 8'b00000000;
reg [7:0] SNES_ROMSELr = 8'b11111111;
reg [7:0] SNES_PULSEr = 8'b11111111;
reg [23:0] SNES_ADDRr [6:0];
reg [7:0] SNES_PAr [6:0];
reg [7:0] SNES_DATAr [4:0];

reg SNES_DEADr = 1;
reg SNES_reset_strobe = 0;

reg free_strobe = 0;
reg ram_free_strobe = 0;

wire [23:0] SNES_ADDR = (SNES_ADDRr[6] & SNES_ADDRr[5]);
wire [7:0] SNES_PA = (SNES_PAr[6] & SNES_PAr[5]);
wire [7:0] SNES_DATA_IN = (SNES_DATAr[3] & SNES_DATAr[2]);

wire SNES_PULSE_IN = SNES_READ_IN & SNES_WRITE_IN & ~SNES_CPU_CLK_IN;

wire SNES_PULSE_end = (SNES_PULSEr[6:1] == 6'b000011);
wire SNES_PARD_start = (SNES_PARDr[6:1] == 6'b111110);
wire SNES_PARD_end = (SNES_PARDr[6:1] == 6'b000001);
wire SNES_PAWR_start = (SNES_PAWRr[7:1] == (({SNES_ADDR[22], SNES_ADDR[15:0]} == 17'h02100) ? 7'b1110000 : 7'b1000000));
wire SNES_PAWR_end = (SNES_PAWRr[6:1] == 6'b000001);
wire SNES_RD_start = (SNES_READr[6:1] == 6'b111110);
wire SNES_RD_end = (SNES_READr[6:1] == 6'b000001);
wire SNES_WR_end = (SNES_WRITEr[6:1] == 6'b000001);
wire SNES_cycle_start = (SNES_CPU_CLKr[6:1] == 6'b000001);
wire SNES_cycle_end = (SNES_CPU_CLKr[6:1] == 6'b111110);
wire SNES_WRITE = SNES_WRITEr[2] & SNES_WRITEr[1];
wire SNES_READ = SNES_READr[2] & SNES_READr[1];
wire SNES_READ_late = SNES_READr[5] & SNES_READr[4];
wire SNES_READ_narrow = SNES_READ | SNES_READ_late;
wire SNES_CPU_CLK = SNES_CPU_CLKr[2] & SNES_CPU_CLKr[1];
wire SNES_PARD = SNES_PARDr[2] & SNES_PARDr[1];
wire SNES_PAWR = SNES_PAWRr[2] & SNES_PAWRr[1];

wire SNES_ROMSEL = (SNES_ROMSELr[5] & SNES_ROMSELr[4]);

reg [7:0] BUS_DATA;

always @(posedge CLK2) begin
  if(~SNES_READ) BUS_DATA <= SNES_DATA;
  else if(~SNES_WRITE) BUS_DATA <= SNES_DATA_IN;
end

wire free_slot = NES_FREE_SLOT;
wire ram_free_slot;

wire ROM_HIT;
wire IS_ROM;
assign DCM_RST=0;
wire IS_SAVERAM;

always @(posedge CLK2) begin
  SNES_PULSEr <= {SNES_PULSEr[6:0], SNES_PULSE_IN};
  SNES_PARDr  <= {SNES_PARDr[6:0], SNES_PARD_IN};
  SNES_PAWRr  <= {SNES_PAWRr[6:0], SNES_PAWR_IN};
  SNES_READr  <= {SNES_READr[6:0], SNES_READ_IN};
  SNES_WRITEr <= {SNES_WRITEr[6:0], SNES_WRITE_IN};
  SNES_CPU_CLKr <= {SNES_CPU_CLKr[6:0], SNES_CPU_CLK_IN};
  SNES_ROMSELr <= {SNES_ROMSELr[6:0], SNES_ROMSEL_IN};
  SNES_ADDRr[6] <= SNES_ADDRr[5];
  SNES_ADDRr[5] <= SNES_ADDRr[4];
  SNES_ADDRr[4] <= SNES_ADDRr[3];
  SNES_ADDRr[3] <= SNES_ADDRr[2];
  SNES_ADDRr[2] <= SNES_ADDRr[1];
  SNES_ADDRr[1] <= SNES_ADDRr[0];
  SNES_ADDRr[0] <= SNES_ADDR_IN;
  SNES_PAr[6] <= SNES_PAr[5];
  SNES_PAr[5] <= SNES_PAr[4];
  SNES_PAr[4] <= SNES_PAr[3];
  SNES_PAr[3] <= SNES_PAr[2];
  SNES_PAr[2] <= SNES_PAr[1];
  SNES_PAr[1] <= SNES_PAr[0];
  SNES_PAr[0] <= SNES_PA_IN;
  SNES_DATAr[4] <= SNES_DATAr[3];
  SNES_DATAr[3] <= SNES_DATAr[2];
  SNES_DATAr[2] <= SNES_DATAr[1];
  SNES_DATAr[1] <= SNES_DATAr[0];
  SNES_DATAr[0] <= SNES_DATA;
end

parameter ST_IDLE            = 11'b00000000001;
parameter ST_MCU_RD_ADDR     = 11'b00000000010;
parameter ST_MCU_RD_END      = 11'b00000000100;
parameter ST_MCU_WR_ADDR     = 11'b00000001000;
parameter ST_MCU_WR_END      = 11'b00000010000;
parameter ST_NES_ROM_RD_ADDR = 11'b00000100000;
parameter ST_NES_ROM_RD_END  = 11'b00001000000;
parameter ST_NES_ROM_WR_ADDR = 11'b00010000000;
parameter ST_NES_ROM_WR_END  = 11'b00100000000;
// SNES-CHR read client (4o cliente do arbiter Bus-1: janelas $50/$60 do
// renderer -- ver bloco "SNES-CHR read client" abaixo)
parameter ST_NESCHR_RD_ADDR  = 11'b01000000000;
parameter ST_NESCHR_RD_END   = 11'b10000000000;

parameter SNES_DEAD_TIMEOUT = 17'd84000; // 1ms

parameter ROM_CYCLE_LEN = 4'd7;

reg [10:0] STATE;
initial STATE = ST_IDLE;

assign MSU_SNES_DATA_IN = BUS_DATA;

sd_dma snes_sd_dma(
  .CLK(CLK2),
  .SD_DAT(SD_DAT),
  .SD_CLK(SD_CLK),
  .SD_DMA_EN(SD_DMA_EN),
  .SD_DMA_STATUS(SD_DMA_STATUS),
  .SD_DMA_SRAM_WE(SD_DMA_SRAM_WE),
  .SD_DMA_SRAM_DATA(SD_DMA_SRAM_DATA),
  .SD_DMA_NEXTADDR(SD_DMA_NEXTADDR),
  .SD_DMA_PARTIAL(SD_DMA_PARTIAL),
  .SD_DMA_PARTIAL_START(SD_DMA_PARTIAL_START),
  .SD_DMA_PARTIAL_END(SD_DMA_PARTIAL_END),
  .SD_DMA_START_MID_BLOCK(SD_DMA_START_MID_BLOCK),
  .SD_DMA_END_MID_BLOCK(SD_DMA_END_MID_BLOCK),
  .DBG_cyclecnt(SD_DMA_DBG_cyclecnt),
  .DBG_clkcnt(SD_DMA_DBG_clkcnt)
);

wire SD_DMA_TO_ROM = (SD_DMA_STATUS && (SD_DMA_TGT == 2'b00)) && MCU_ROM;
wire SD_DMA_TO_RAM = (SD_DMA_STATUS && (SD_DMA_TGT == 2'b00)) && MCU_RAM;

wire [7:0] dac_dbg_cic_max;
wire [7:0] dac_dbg_i2s_act;
wire [7:0] dac_dbg_dat_max;

dac snes_dac(
  .clkin(CLK2),
  .sysclk(SNES_SYSCLK),
  .mclk_out(DAC_MCLK),
  .lrck_out(DAC_LRCK),
  .sdout(DAC_SDOUT),
  .we(SD_DMA_TGT==2'b01 ? SD_DMA_SRAM_WE : 1'b1),
  .pgm_address(dac_addr),
  .pgm_data(SD_DMA_SRAM_DATA),
  .DAC_STATUS(DAC_STATUS),
  .volume(msu_volumerq_out),
  .sgb_apu_dat(NES_APU_DAT),
  .sgb_apu_clk_edge(NES_APU_CLK_EDGE),
  .vol_latch(msu_volume_latch_out),
  .vol_select(dac_vol_select_out),
  .sgb_vol_select(3'b000), // no per-core volume-boost feature bit wired up yet
  .palmode(dac_palmode_out),
  .play(dac_play),
  .reset(dac_reset),
  .dac_address_ext(dac_ptr_addr),
  // debug liveness -> nes_wrap group 0x04 idx16/17 (audio-mute probe)
  .dbg_cic_max(dac_dbg_cic_max),
  .dbg_i2s_act(dac_dbg_i2s_act),
  .dbg_dat_max(dac_dbg_dat_max)
);

msu snes_msu (
  .clkin(CLK2),
  .enable(msu_enable),
  .pgm_address(msu_write_addr),
  .pgm_data(SD_DMA_SRAM_DATA),
  .pgm_we(SD_DMA_TGT==2'b10 ? SD_DMA_SRAM_WE : 1'b1),
  .reg_addr(SNES_ADDR[2:0]),
  .reg_data_in(MSU_SNES_DATA_IN),
  .reg_data_out(MSU_SNES_DATA_OUT),
  .reg_oe_falling(SNES_RD_start),
  .reg_oe_rising(SNES_RD_end),
  .reg_we_rising(SNES_WR_end),
  .status_out(msu_status_out),
  .volume_out(msu_volumerq_out),
  .volume_latch_out(msu_volume_latch_out),
  .addr_out(msu_addressrq_out),
  .track_out(msu_trackrq_out),
  .status_reset_bits(msu_status_reset_bits),
  .status_set_bits(msu_status_set_bits),
  .status_reset_we(msu_status_reset_we),
  .msu_address_ext(msu_ptr_addr),
  .msu_address_ext_write(msu_addr_reset),
  .DBG_msu_reg_oe_rising(DBG_msu_reg_oe_rising),
  .DBG_msu_reg_oe_falling(DBG_msu_reg_oe_falling),
  .DBG_msu_reg_we_rising(DBG_msu_reg_we_rising),
  .DBG_msu_address(DBG_msu_address),
  .DBG_msu_address_ext_write_rising(DBG_msu_address_ext_write_rising)
);

spi snes_spi(
  .clk(CLK2),
  .MOSI(SPI_MOSI),
  .MISO(SPI_MISO),
  .SSEL(SPI_SS),
  .SCK(SPI_SCK),
  .cmd_ready(spi_cmd_ready),
  .param_ready(spi_param_ready),
  .cmd_data(spi_cmd_data),
  .param_data(spi_param_data),
  .endmessage(spi_endmessage),
  .startmessage(spi_startmessage),
  .input_data(spi_input_data),
  .byte_cnt(spi_byte_cnt),
  .bit_cnt(spi_bit_cnt)
);

// RAM contains the SNES-side program (if any) and is memory mapped to 880000-8FFFFF
assign MCU_RAM = MCU_ADDR[23:19] == {4'h8,1'b1};
assign MCU_ROM = ~MCU_RAM;

wire [23:0] NES_ROM_ADDR;
wire [7:0]  NES_ROM_WRDATA;
reg  [7:0]  NES_ROM_DINr;
wire        NES_ROM_RDY;

//-------------------------------------------------------------------
// NES core (nes_wrap.v -- fpganes CPU+PPU+APU+MultiMapper, adapted)
//-------------------------------------------------------------------

nes_wrap nes_core (
  .RST(SNES_reset_strobe),
  .CPU_RST(),
  .CLK(CLK2),
  .SNES_SYSCLK(SNES_SYSCLK), // async pacer reference; synchronized inside nes_wrap

  .ROM_BUS_RDY(NES_ROM_RDY),
  .ROM_BUS_RRQ(NES_ROM_RRQ),
  .ROM_BUS_WRQ(NES_ROM_WRQ),
  .ROM_BUS_WORD(NES_ROM_WORD),
  .ROM_BUS_ADDR(NES_ROM_ADDR),
  .ROM_BUS_RDDATA(NES_ROM_DINr),
  .ROM_BUS_WRDATA(NES_ROM_WRDATA),
  .ROM_FREE_SLOT(NES_FREE_SLOT),

  .APU_DAT(NES_APU_DAT),
  .APU_CLK_EDGE(NES_APU_CLK_EDGE),
  .DAC_DBG_CIC(dac_dbg_cic_max),
  .DAC_DBG_I2S(dac_dbg_i2s_act),
  .DAC_DBG_DAT(dac_dbg_dat_max),
  .DAC_LRCK_MON(DAC_LRCK),

  // mapper_flags (Fase 0, convencao REAL -- substitui o placeholder mapper-0):
  // o firmware (src/nes.c) monta a palavra de 16 bits no formato EXATO do
  // GameLoader do fpganes original (NES_Nexys4.v:122) e a entrega inteira via
  // CHIPFEAT (SPI 0xef -> nes_feat_out):
  //   NES_FEAT[7:0]   mapper number (8 bits -- cobre o 28, que o SETMAPPER de
  //                   4 bits nao alcancava)
  //   NES_FEAT[10:8]  prg_size class (bancos de 16KB: <=1->0, <=2->1, ... 7)
  //   NES_FEAT[13:11] chr_size class (bancos de 8KB, mesma tabela)
  //   NES_FEAT[14]    mirroring (iNES flags6 bit0: 1 = vertical)
  //   NES_FEAT[15]    has_chr_ram (chr_8k_banks == 0)
  // mapper_flags[31:16] = 0, identico ao fpganes. O SETMAPPER de 4 bits
  // (MAPPER) fica sem consumidor neste core -- a fonte canonica e' esta.
  // INVARIANTE (main.sdc false-path nes_feat_out -> NES): o firmware escreve
  // o CHIPFEAT antes de soltar o reset do core e nunca com o core rodando.
  .mapper_flags_in({16'b0, NES_FEAT}),

  .reg_group_in(reg_group),
  .reg_index_in(reg_index),
  .reg_value_in(reg_value),
  .reg_invmask_in(reg_invmask),
  .reg_we_in(reg_we),
  .reg_read_in(reg_read),
  .config_data_out(nes_config_data),

  // video-bridge mailbox window ($6000-$7FFF) + control block ($2BD0-$2BDF --
// realocado de $2A00 por colidir com SNESCMD_MCU_CMD/PARAM; ver address.v)
  .NESBOX_ADDR(SNES_ADDR[12:0]),
  .NESBOX_DATA(NESBOX_DATA),
  .NESBOX_FRAME_SEQ(NESBOX_FRAME_SEQ),
  .NESBOX_FRAME_LEN(NESBOX_FRAME_LEN),
  .NESBOX_STATUS(NESBOX_STATUS),
  .NESBOX_FRAME_ACK(nes_frame_ack),
  .NESBOX_BUF_SEL(nes_buf_sel),
  .NESBOX_CTRL_P1(nes_ctrl_p1),
  .NESBOX_CTRL_P2(nes_ctrl_p2),
  .NESBOX_GO(nes_go)
);

reg [7:0] MCU_DINr;
reg [7:0] MCU_ROM_DINr;
reg [7:0] MCU_RAM_DINr;

mcu_cmd snes_mcu_cmd(
  .clk(CLK2),
  .snes_sysclk(SNES_SYSCLK),
  .cmd_ready(spi_cmd_ready),
  .param_ready(spi_param_ready),
  .cmd_data(spi_cmd_data),
  .param_data(spi_param_data),
  .mcu_mapper(MAPPER),
  .mcu_write(MCU_WRITE),
  .mcu_data_in(MCU_DINr),
  .mcu_data_out(MCU_DOUT),
  .spi_byte_cnt(spi_byte_cnt),
  .spi_bit_cnt(spi_bit_cnt),
  .spi_data_out(spi_input_data),
  .addr_out(MCU_ADDR),
  .saveram_mask_out(SAVERAM_MASK),
  .rom_mask_out(ROM_MASK),
  .SD_DMA_EN(SD_DMA_EN),
  .SD_DMA_STATUS(SD_DMA_STATUS),
  .SD_DMA_NEXTADDR(SD_DMA_NEXTADDR),
  .SD_DMA_SRAM_DATA(SD_DMA_SRAM_DATA),
  .SD_DMA_SRAM_WE(SD_DMA_SRAM_WE),
  .SD_DMA_TGT(SD_DMA_TGT),
  .SD_DMA_PARTIAL(SD_DMA_PARTIAL),
  .SD_DMA_PARTIAL_START(SD_DMA_PARTIAL_START),
  .SD_DMA_PARTIAL_END(SD_DMA_PARTIAL_END),
  .SD_DMA_START_MID_BLOCK(SD_DMA_START_MID_BLOCK),
  .SD_DMA_END_MID_BLOCK(SD_DMA_END_MID_BLOCK),
  .dac_addr_out(dac_addr),
  .DAC_STATUS(DAC_STATUS),
  .dac_play_out(dac_play),
  .dac_reset_out(dac_reset),
  .dac_vol_select_out(dac_vol_select_out),
  .dac_palmode_out(dac_palmode_out),
  .dac_ptr_out(dac_ptr_addr),
  .msu_addr_out(msu_write_addr),
  .MSU_STATUS(msu_status_out),
  .msu_status_reset_out(msu_status_reset_bits),
  .msu_status_set_out(msu_status_set_bits),
  .msu_status_reset_we(msu_status_reset_we),
  .msu_volumerq(msu_volumerq_out),
  .msu_addressrq(msu_addressrq_out),
  .msu_trackrq(msu_trackrq_out),
  .msu_ptr_out(msu_ptr_addr),
  .msu_reset_out(msu_addr_reset),
  .rtc_data_in(56'h0),
  .rtc_data_out(),
  .rtc_pgm_we(),
  .rtc_pgm_rd(),
  .nes_feat_out(NES_FEAT),   // CHIPFEAT 0xef = mapper_flags[15:0] (ver nes_wrap acima)
  // config / breadcrumb bus
  .reg_group_out(reg_group),
  .reg_index_out(reg_index),
  .reg_value_out(reg_value),
  .reg_invmask_out(reg_invmask),
  .reg_we_out(reg_we),
  .reg_read_out(reg_read),
  .nes_config_data_in(nes_config_data),
  .featurebits_out(featurebits),
  .mcu_rrq(MCU_RRQ),
  .mcu_wrq(MCU_WRQ),
  .mcu_rq_rdy(MCU_RDY),
  .region_out(mcu_region),
  .snescmd_addr_out(snescmd_addr_mcu),
  .snescmd_we_out(snescmd_we_mcu),
  .snescmd_data_out(snescmd_data_out_mcu),
  .snescmd_data_in(snescmd_data_in_mcu),
  // cheat_pgm_* left unconnected: there is no cheat.v in this core (no
  // SNES-side program to hook yet), so opcode 0xd3 programs registers inside
  // mcu_cmd that nothing consumes. Harmless; synthesis sweeps them.
  .cheat_pgm_idx_out(),
  .cheat_pgm_data_out(),
  .cheat_pgm_we_out(),
  .dsp_feat_out(dsp_feat)
);

address snes_addr(
  .CLK(CLK2),
  .featurebits(featurebits),
  .SNES_ADDR(SNES_ADDR),
  .SNES_PA(SNES_PA),
  .SNES_ROMSEL(SNES_ROMSEL),
  .ROM_ADDR(MAPPED_SNES_ADDR),
  .ROM_HIT(ROM_HIT),
  .IS_SAVERAM(IS_SAVERAM),
  .IS_ROM(IS_ROM),
  .IS_WRITABLE(IS_WRITABLE),
  .msu_enable(msu_enable),
  .r213f_enable(r213f_enable),
  .r2100_hit(r2100_hit),
  .snescmd_enable(snescmd_enable),
  .nesbox_enable(nesbox_enable),
  .nesctl_enable(nesctl_enable),
  .nes_chr_enable(nes_chr_enable)
);

wire [7:0] snescmd_dout;

// ---------------------------------------------------------------------------
// Video-bridge SNES-side plumbing (Fase 1a): control block logic ($2BD0-$2BDF;
// base realocada de $2A00 -- colidia com o protocolo SNES<->MCU, ver address.v).
// (Declarations of nesbox_enable/nesctl_enable/NESBOX_*/nes_frame_ack/... are up
// near the config-bus wires, before the nes_wrap instance that uses them.)
// Reads return bridge state (SEQ/LEN/STATUS + magic), writes latch the
// renderer's ACK/BUF_SEL/CTRL for the bridge.
// ---------------------------------------------------------------------------
always @(posedge CLK2) begin
  if (SNES_reset_strobe) begin
    // novo boot/IGR: renderer refaz o handshake inteiro (GO volta a segurar o
    // core; ACK/BUF_SEL/CTRL zerados junto -- a bridge tambem reseta no strobe)
    nes_go        <= 1'b0;
    nes_frame_ack <= 16'd0;
    nes_buf_sel   <= 1'b0;
    nes_ctrl_p1   <= 16'd0;
    nes_ctrl_p2   <= 16'd0;
  end else if (SNES_WR_end & nesctl_enable) begin
    case (SNES_ADDR[3:0])
      4'h4: nes_frame_ack[7:0]  <= SNES_DATA;
      4'h5: nes_frame_ack[15:8] <= SNES_DATA;
      4'h6: nes_buf_sel         <= SNES_DATA[0];
      4'ha: nes_ctrl_p1[7:0]    <= SNES_DATA;
      4'hb: nes_ctrl_p1[15:8]   <= SNES_DATA;
      4'hc: nes_ctrl_p2[7:0]    <= SNES_DATA;
      4'hd: nes_ctrl_p2[15:8]   <= SNES_DATA;
      4'he: nes_go              <= SNES_DATA[0];   // NES_GO (boot handshake)
      default: ;
    endcase
  end
end

wire [7:0] nesctl_dout =
    (SNES_ADDR[3:0]==4'h0) ? 8'h4E :                    // 'N'  NES_MAGIC
    (SNES_ADDR[3:0]==4'h1) ? 8'h42 :                    // 'B'
    (SNES_ADDR[3:0]==4'h2) ? NESBOX_FRAME_SEQ[7:0]  :
    (SNES_ADDR[3:0]==4'h3) ? NESBOX_FRAME_SEQ[15:8] :
    (SNES_ADDR[3:0]==4'h4) ? nes_frame_ack[7:0]     :
    (SNES_ADDR[3:0]==4'h5) ? nes_frame_ack[15:8]    :
    (SNES_ADDR[3:0]==4'h6) ? {7'd0, nes_buf_sel}    :
    (SNES_ADDR[3:0]==4'h7) ? NESBOX_STATUS          :
    (SNES_ADDR[3:0]==4'h8) ? NESBOX_FRAME_LEN[7:0]  :
    (SNES_ADDR[3:0]==4'h9) ? NESBOX_FRAME_LEN[15:8] :
    (SNES_ADDR[3:0]==4'ha) ? nes_ctrl_p1[7:0]       :
    (SNES_ADDR[3:0]==4'hb) ? nes_ctrl_p1[15:8]      :
    (SNES_ADDR[3:0]==4'hc) ? nes_ctrl_p2[7:0]       :
    (SNES_ADDR[3:0]==4'hd) ? nes_ctrl_p2[15:8]      :
    (SNES_ADDR[3:0]==4'he) ? {7'd0, nes_go}         : 8'h00;

parameter ST_R213F_ARMED     = 4'b0001;
parameter ST_R213F_WAITBUS   = 4'b0010;
parameter ST_R213F_OVERRIDE  = 4'b0100;
parameter ST_R213F_HOLD      = 4'b1000;

reg [7:0] r213fr;
reg r213f_forceread;
reg [2:0] r213f_delay;
reg [1:0] r213f_state;
initial r213fr = 8'h55;
initial r213f_forceread = 0;
initial r213f_state = 2'b01;
initial r213f_delay = 3'b000;

reg [7:0] r2100r = 0;
reg r2100_forcewrite = 0;
reg r2100_forcewrite_pre = 0;
`ifdef BRIGHTNESS_LIMIT
wire [3:0] r2100_limit = featurebits[10:7];
`else
wire [3:0] r2100_limit = 4'hF;
`endif
wire [3:0] r2100_limited = (SNES_DATA[3:0] > r2100_limit) ? r2100_limit : SNES_DATA[3:0];
`ifdef BRIGHTNESS_PATCH
wire r2100_patch = featurebits[6];
`else
wire r2100_patch = 0;
`endif
wire r2100_enable = r2100_hit & (r2100_patch | ~(&r2100_limit));

always @(posedge CLK2) begin
  r2100_forcewrite <= r2100_forcewrite_pre;
end

reg nmi_match; initial nmi_match = 0;
reg irq_match; initial irq_match = 0;

always @(posedge CLK2) nmi_match <= {SNES_ADDR[23:1],1'b0} == 24'h00FFEA;
always @(posedge CLK2) irq_match <= {SNES_ADDR[23:1],1'b0} == 24'h00FFEE;

assign SNES_DATA = (r213f_enable & ~SNES_PARD & ~r213f_forceread) ? r213fr
                   :(r2100_enable & ~SNES_PAWR & r2100_forcewrite) ? r2100r
                   :((~SNES_READ ^ (r213f_forceread & r213f_enable & ~SNES_PARD))
                                & ~(r2100_enable & ~SNES_PAWR & ~r2100_forcewrite & ~IS_ROM & ~IS_WRITABLE))
                                ? ( msu_enable ? MSU_SNES_DATA_OUT
                                  : nesctl_enable ? nesctl_dout       // control block $2BD0-$2BDF (before snescmd)
                                  : nesbox_enable ? NESBOX_DATA       // mailbox window $6000-$7FFF
                                  : nes_chr_enable ? NES_CHR_SERVE    // CHR-SNES windows $50/$60 (Bus-1 client; v2.0b cache/prefetch serve)
                                  : ((snescmd_unlock | feat_cmd_unlock) & snescmd_enable) ? snescmd_dout
                                  : RAM_DATA // the RAM module holds up to 512KB
                                  ) : 8'bZ;

reg [3:0] ST_MEM_DELAYr;

// MCU
reg MCU_RD_PENDr = 0;
reg MCU_WR_PENDr = 0;
reg [23:0] ROM_ADDRr;

reg RQ_MCU_RDYr;
initial RQ_MCU_RDYr = 1'b1;

wire MCU_WE_HIT = |(STATE & ST_MCU_WR_ADDR);
wire MCU_WR_HIT = |(STATE & (ST_MCU_WR_ADDR | ST_MCU_WR_END));
wire MCU_RD_HIT = |(STATE & (ST_MCU_RD_ADDR | ST_MCU_RD_END));
wire MCU_HIT = MCU_WR_HIT | MCU_RD_HIT;

// NES core ROM bus (PSRAM), mirrors the SGB_ROM_* pipeline 1:1
reg NES_ROM_RD_PENDr; initial NES_ROM_RD_PENDr = 0;
reg NES_ROM_WR_PENDr; initial NES_ROM_WR_PENDr = 0;
reg [23:0] NES_ROM_ADDRr;
reg [7:0]  NES_ROM_DATAr;

reg RQ_NES_ROM_RDYr; initial RQ_NES_ROM_RDYr = 1;
assign NES_ROM_RDY = RQ_NES_ROM_RDYr;
// NES_ROM_WRDATA is driven by nes_core's own ROM_BUS_WRDATA output port
// (bound above at instantiation time) -- it already carries the real byte
// for CIRAM/CPU-RAM/CART-RAM writes (nes_wrap.v's ROM_BUS_WRDATA = mem_dout).
// An earlier draft had a stray `assign NES_ROM_WRDATA = 8'h00` placeholder
// here, which is a multiple-driver conflict on the same wire (caught by
// review, not by the iverilog elaboration pass above -- iverilog only
// warned about the unrelated apu.v SquareChan port width, it did NOT flag
// this dual-driver wire, so do not rely on a clean `-t null` pass alone to
// catch this class of bug in future edits).

wire NES_WE_HIT     = |(STATE & ST_NES_ROM_WR_ADDR);
wire NES_ROM_RD_HIT = |(STATE & (ST_NES_ROM_RD_ADDR | ST_NES_ROM_RD_END));
wire NES_ROM_WR_HIT = |(STATE & (ST_NES_ROM_WR_ADDR | ST_NES_ROM_WR_END));
wire NES_ROM_HIT    = NES_ROM_RD_HIT | NES_ROM_WR_HIT;

// ---------------------------------------------------------------------------
// SNES-CHR read client (4o cliente do arbiter Bus-1) -- serve as janelas
// CHR-SNES $50-$5F/$60-$6F (PSRAM 0x500000/0x600000, identidade) pro DMA do
// renderer.  Prioridade: ABAIXO do core NES (que nunca e' estolado -- spec
// SS5.3), ACIMA do MCU, admitido no mesmo gate free_slot que o MCU.
//
// v2.0b DEADLINE FIX (regressao de hardware do Tetris USA: CHR re-DMAda pelo
// renderer 0x9d19 chegava EMBARALHADA -- reproduzido no tb_arb com timing real
// de /RD: 2-4 bytes STALE por burst de 768 com o core vivo).  O arm antigo em
// SNES_RD_start (queda do /RD + ~71ns de sync) deixava ~119ns de budget para
// um servico de ~107ns -- QUALQUER transacao em voo (core ou MCU) estourava a
// janela do /RD e o SNES latchava o byte ANTERIOR (era o "MARGINAL/ESTOURA"
// documentado aqui; nunca exercitado em HW ate o renderer passar a ler as
// janelas com o core rodando).  Dois mecanismos, ambos servindo de REGISTRADOR:
//   1. EARLY-ARM por ENDERECO (nivel com retry, nao pulso): SNES_ADDR (ja
//      deglitchado por r[6]&r[5]) estavel por 2 amostras, dentro da janela e
//      != ultimo endereco tratado -> arma a leitura JA NA FASE DE ENDERECO
//      (~90-190ns antes do /RD cair) e re-tenta enquanto PEND/HIT ocupam --
//      um arm suprimido nao se perde (o pulso antigo perdia).
//   2. PREFETCH SEQUENCIAL (o DMA do renderer e' sequencial por definicao --
//      janela identidade): ao completar o byte A (demand ou prefetch), arma
//      A+1 especulativo -> bytes 2..N de um burst sao servidos de REGISTRADOR
//      (deadline zero); so o byte 1 usa o caminho demand (coberto pelo
//      early-arm).  Prefetch so dentro das janelas $50/$60.
// COERENCIA: qualquer escrita do MCU no Bus-1 (boot upload / re-conversao) ou
// SD-DMA invalida o cache+dedup (V=0, seen=0) -> a proxima leitura re-busca.
// Janela e' read-only pro SNES; arm especulativo em ciclo nao-read e' inocuo.
// Custo: +2 regs de 8b + 2 de 24b + comparadores; trafego Bus-1 do burst ~2x
// transacoes (prefetch), ainda ~13%% de duty no pior burst -- MCU segue
// atendido (tb_arb cenario C).
// ---------------------------------------------------------------------------
reg NES_CHR_RD_PENDr; initial NES_CHR_RD_PENDr = 0;
reg [23:0] NES_CHR_ADDRr;
// (NES_CHR_DINr declarado junto do bloco NESBOX, antes do mux SNES_DATA)
reg        NES_CHR_PEND_PFr;  initial NES_CHR_PEND_PFr = 0;  // txn em voo e' prefetch
reg [7:0]  NES_CHR_DATr;                                     // byte servido (addr corrente)
reg [23:0] NES_CHR_DAT_ADDRr;
reg        NES_CHR_DAT_Vr;    initial NES_CHR_DAT_Vr = 0;
reg [7:0]  NES_CHR_PFr;                                      // byte prefetchado (addr+1)
reg [23:0] NES_CHR_PF_ADDRr;
reg        NES_CHR_PF_Vr;     initial NES_CHR_PF_Vr = 0;
reg [23:0] NES_CHR_LASTr;                                    // dedup do early-arm
reg        NES_CHR_SEENr;     initial NES_CHR_SEENr = 0;

wire NES_CHR_HIT = |(STATE & (ST_NESCHR_RD_ADDR | ST_NESCHR_RD_END));

// endereco estavel (SNES_ADDR ja e' o AND deglitchado de 2 amostras; mais uma
// igualdade fecha qualquer skew residual)
reg [23:0] SNES_ADDR_D1r;
always @(posedge CLK2) SNES_ADDR_D1r <= SNES_ADDR;
wire        nes_chr_addr_stable = (SNES_ADDR == SNES_ADDR_D1r);
wire        nes_chr_new  = nes_chr_enable & nes_chr_addr_stable &
                           (~NES_CHR_SEENr | (SNES_ADDR != NES_CHR_LASTr));
wire        nes_chr_pf_hit = NES_CHR_PF_Vr & (SNES_ADDR == NES_CHR_PF_ADDRr);
wire [23:0] nes_chr_next   = SNES_ADDR + 24'd1;
wire        nes_chr_next_in_win = (nes_chr_next[23:20] == 4'h5) | (nes_chr_next[23:20] == 4'h6);
wire [23:0] nes_chr_pend_next   = NES_CHR_ADDRr + 24'd1;
wire        nes_chr_pend_next_in_win = (nes_chr_pend_next[23:20] == 4'h5) | (nes_chr_pend_next[23:20] == 4'h6);
// byte servido no mux SNES_DATA (registrado; DINr cru e' o fallback legado)
assign NES_CHR_SERVE = nes_chr_pf_hit ? NES_CHR_PFr
                     : (NES_CHR_DAT_Vr & (SNES_ADDR == NES_CHR_DAT_ADDRr)) ? NES_CHR_DATr
                     : NES_CHR_DINr;

// invalidacao SO' quando a escrita toca as janelas ($50/$60): uma invalidacao
// global em qualquer MCU-write matava o prefetch a cada byte SPI do trafego
// NDBG (fora da janela) e REGREDIA o cenario C do tb_arb (14 misses, stride 7
// = cadencia SPI).  ROM_ADDRr = o endereco da transacao MCU corrente.
wire nes_chr_mcu_wr_hit_win = (|(STATE & ST_MCU_WR_ADDR)) &
                              ((ROM_ADDRr[23:20] == 4'h5) | (ROM_ADDRr[23:20] == 4'h6));
always @(posedge CLK2) begin
  if (nes_chr_mcu_wr_hit_win | SD_DMA_TO_ROM) begin
    // conteudo da PSRAM pode ter mudado sob o cache: invalida tudo
    NES_CHR_DAT_Vr <= 1'b0;
    NES_CHR_PF_Vr  <= 1'b0;
    NES_CHR_SEENr  <= 1'b0;
    if (STATE & ST_NESCHR_RD_END) NES_CHR_RD_PENDr <= 1'b0;  // nunca perder o clear
  end else if (STATE & ST_NESCHR_RD_END) begin
    // transacao completou: byte em NES_CHR_DINr (latchado no ST_NESCHR_RD_ADDR)
    NES_CHR_RD_PENDr <= 1'b0;
    if (NES_CHR_PEND_PFr) begin
      NES_CHR_PFr      <= NES_CHR_DINr;
      NES_CHR_PF_ADDRr <= NES_CHR_ADDRr;
      NES_CHR_PF_Vr    <= 1'b1;
    end else begin
      NES_CHR_DATr      <= NES_CHR_DINr;
      NES_CHR_DAT_ADDRr <= NES_CHR_ADDRr;
      NES_CHR_DAT_Vr    <= 1'b1;
      // demand completo p/ A -> ja dispara o prefetch de A+1 (burst sequencial)
      if (nes_chr_pend_next_in_win) begin
        NES_CHR_RD_PENDr <= 1'b1;
        NES_CHR_ADDRr    <= nes_chr_pend_next;
        NES_CHR_PEND_PFr <= 1'b1;
      end
    end
  end else if (nes_chr_new & ~NES_CHR_RD_PENDr & ~NES_CHR_HIT) begin
    NES_CHR_LASTr <= SNES_ADDR;
    NES_CHR_SEENr <= 1'b1;
    if (nes_chr_pf_hit) begin
      // hit no prefetch: promove (registro->registro, deadline zero) e ja
      // prefetcha o proximo
      NES_CHR_DATr      <= NES_CHR_PFr;
      NES_CHR_DAT_ADDRr <= NES_CHR_PF_ADDRr;
      NES_CHR_DAT_Vr    <= 1'b1;
      NES_CHR_PF_Vr     <= 1'b0;
      if (nes_chr_next_in_win) begin
        NES_CHR_RD_PENDr <= 1'b1;
        NES_CHR_ADDRr    <= nes_chr_next;
        NES_CHR_PEND_PFr <= 1'b1;
      end
    end else begin
      // demand: leitura do proprio endereco (identidade: long addr == PSRAM)
      NES_CHR_RD_PENDr <= 1'b1;
      NES_CHR_ADDRr    <= SNES_ADDR;
      NES_CHR_PEND_PFr <= 1'b0;
    end
  end
end

`ifdef MK2
my_dcm snes_dcm(
  .CLKIN(CLKIN),
  .CLKFX(CLK2),
  .LOCKED(DCM_LOCKED),
  .RST(DCM_RST)
);

assign ROM_ADDR  = (SD_DMA_TO_ROM) ? MCU_ADDR[23:1] : MCU_HIT ? ROM_ADDRr[23:1] : NES_CHR_HIT ? NES_CHR_ADDRr[23:1] : NES_ROM_ADDRr[23:1];
assign ROM_ADDR0 = (SD_DMA_TO_ROM) ? MCU_ADDR[0]    : MCU_HIT ? ROM_ADDRr[0]    : NES_CHR_HIT ? NES_CHR_ADDRr[0]    : NES_ROM_ADDRr[0];

assign ROM_CE = 1'b0;

assign p113_out = 1'b0;

snescmd_buf snescmd (
  .clka(CLK2),
  .wea(SNES_WR_end & ((snescmd_unlock | feat_cmd_unlock) & snescmd_enable)),
  .addra(SNES_ADDR[8:0]),
  .dina(SNES_DATA),
  .douta(snescmd_dout),
  .clkb(CLK2),
  .web(snescmd_we_mcu),
  .addrb(snescmd_addr_mcu),
  .dinb(snescmd_data_out_mcu),
  .doutb(snescmd_data_in_mcu)
);
`endif

`ifdef MK3
pll snes_pll(
  .inclk0(CLKIN),
  .c0(CLK2),
  .locked(DCM_LOCKED),
  .areset(DCM_RST)
);

wire ROM_ADDR22;
assign ROM_ADDR22 = (SD_DMA_TO_ROM) ? MCU_ADDR[1]    : MCU_HIT ? ROM_ADDRr[1]    : NES_CHR_HIT ? NES_CHR_ADDRr[1]    : NES_ROM_ADDRr[1];
assign ROM_ADDR   = (SD_DMA_TO_ROM) ? MCU_ADDR[23:2] : MCU_HIT ? ROM_ADDRr[23:2] : NES_CHR_HIT ? NES_CHR_ADDRr[23:2] : NES_ROM_ADDRr[23:2];
assign ROM_ADDR0  = (SD_DMA_TO_ROM) ? MCU_ADDR[0]    : MCU_HIT ? ROM_ADDRr[0]    : NES_CHR_HIT ? NES_CHR_ADDRr[0]    : NES_ROM_ADDRr[0];

assign ROM_ZZ = 1'b1;
assign ROM_1CE = ROM_ADDR22;
assign ROM_2CE = ~ROM_ADDR22;
assign PM6_out = 1'b0;
assign PN6_out = 1'b0;

snescmd_buf snescmd (
  .clock(CLK2),
  .wren_a(SNES_WR_end & ((snescmd_unlock | feat_cmd_unlock) & snescmd_enable)),
  .address_a(SNES_ADDR[8:0]),
  .data_a(SNES_DATA),
  .q_a(snescmd_dout),
  .wren_b(snescmd_we_mcu),
  .address_b(snescmd_addr_mcu),
  .data_b(snescmd_data_out_mcu),
  .q_b(snescmd_data_in_mcu)
);
`endif

// (snescmd_unlock declarado junto do bloco NESBOX, antes do mux SNES_DATA)

// OE always active. Overridden by WE when needed.
assign ROM_OE = 1'b0;

reg[17:0] SNES_DEAD_CNTr;
initial SNES_DEAD_CNTr = 0;

// MCU r/w request
always @(posedge CLK2) begin
  if(MCU_RRQ & MCU_ROM) begin
    MCU_RD_PENDr <= 1'b1;
    RQ_MCU_RDYr <= 1'b0;
    ROM_ADDRr <= MCU_ADDR;
  end else if(MCU_WRQ & MCU_ROM) begin
    MCU_WR_PENDr <= 1'b1;
    RQ_MCU_RDYr <= 1'b0;
    ROM_ADDRr <= MCU_ADDR;
  end else if(STATE & (ST_MCU_RD_END | ST_MCU_WR_END)) begin
    MCU_RD_PENDr <= 1'b0;
    MCU_WR_PENDr <= 1'b0;
    RQ_MCU_RDYr <= 1'b1;
  end
end

// NES core ROM r/w request
always @(posedge CLK2) begin
  if(NES_ROM_RRQ) begin
    NES_ROM_RD_PENDr <= 1'b1;
    RQ_NES_ROM_RDYr <= 1'b0;
    NES_ROM_ADDRr <= NES_ROM_ADDR;
  end else if(NES_ROM_WRQ) begin
    NES_ROM_WR_PENDr <= 1'b1;
    RQ_NES_ROM_RDYr <= 1'b0;
    NES_ROM_ADDRr <= NES_ROM_ADDR;
    NES_ROM_DATAr <= NES_ROM_WRDATA;
  end else if(|(STATE & (ST_NES_ROM_RD_ADDR)) & ~|ST_MEM_DELAYr) begin
    // enable rdy/response 1 cycle earlier
    RQ_NES_ROM_RDYr <= 1'b1;
  end else if(STATE & (ST_NES_ROM_RD_END | ST_NES_ROM_WR_END)) begin
    NES_ROM_RD_PENDr <= 1'b0;
    NES_ROM_WR_PENDr <= 1'b0;
    RQ_NES_ROM_RDYr <= 1'b1;
  end
end

always @(posedge CLK2) begin
  if(~SNES_CPU_CLKr[1]) SNES_DEAD_CNTr <= SNES_DEAD_CNTr + 1;
  else SNES_DEAD_CNTr <= 17'h0;
end

always @(posedge CLK2) begin
  SNES_reset_strobe <= 1'b0;
  if(SNES_CPU_CLKr[1]) begin
    SNES_DEADr <= 1'b0;
    if(SNES_DEADr) SNES_reset_strobe <= 1'b1;
  end
  else if(SNES_DEAD_CNTr > SNES_DEAD_TIMEOUT) SNES_DEADr <= 1'b1;
end

always @(posedge CLK2) begin
  if(SNES_DEADr & SNES_CPU_CLKr[1]) STATE <= ST_IDLE; // interrupt+restart an ongoing MCU access when the SNES comes alive
  else
  case(STATE)
    ST_IDLE: begin
      STATE <= ST_IDLE;

      if (NES_ROM_RD_PENDr) begin
        STATE <= ST_NES_ROM_RD_ADDR;
        ST_MEM_DELAYr <= ROM_CYCLE_LEN;
      end
      else if (NES_ROM_WR_PENDr) begin
        STATE <= ST_NES_ROM_WR_ADDR;
        ST_MEM_DELAYr <= ROM_CYCLE_LEN;
      end
      else if (free_slot | SNES_DEADr) begin
        // SNES-CHR acima do MCU: a leitura do SNES tem deadline duro (/RD);
        // o MCU nao tem nenhum.
        if(NES_CHR_RD_PENDr) begin
          STATE <= ST_NESCHR_RD_ADDR;
          ST_MEM_DELAYr <= ROM_CYCLE_LEN;
        end else if(MCU_RD_PENDr) begin
          STATE <= ST_MCU_RD_ADDR;
          ST_MEM_DELAYr <= ROM_CYCLE_LEN;
        end else if(MCU_WR_PENDr) begin
          STATE <= ST_MCU_WR_ADDR;
          ST_MEM_DELAYr <= ROM_CYCLE_LEN;
        end
      end
    end
    ST_MCU_RD_ADDR: begin
      STATE <= ST_MCU_RD_ADDR;
      ST_MEM_DELAYr <= ST_MEM_DELAYr - 1;
      if(ST_MEM_DELAYr == 0) STATE <= ST_MCU_RD_END;
      MCU_ROM_DINr <= (ROM_ADDR0 ? ROM_DATA[7:0] : ROM_DATA[15:8]);
    end
    ST_MCU_WR_ADDR: begin
      STATE <= ST_MCU_WR_ADDR;
      ST_MEM_DELAYr <= ST_MEM_DELAYr - 1;
      if(ST_MEM_DELAYr == 0) STATE <= ST_MCU_WR_END;
    end
    ST_NES_ROM_RD_ADDR: begin
      STATE <= ST_NES_ROM_RD_ADDR;
      ST_MEM_DELAYr <= ST_MEM_DELAYr - 1;
      if(ST_MEM_DELAYr == 0) STATE <= ST_NES_ROM_RD_END;
      NES_ROM_DINr <= (ROM_ADDR0 ? ROM_DATA[7:0] : ROM_DATA[15:8]);
    end
    ST_NES_ROM_WR_ADDR: begin
      STATE <= ST_NES_ROM_WR_ADDR;
      ST_MEM_DELAYr <= ST_MEM_DELAYr - 1;
      if(ST_MEM_DELAYr == 0) STATE <= ST_NES_ROM_WR_END;
    end
    ST_NESCHR_RD_ADDR: begin
      STATE <= ST_NESCHR_RD_ADDR;
      ST_MEM_DELAYr <= ST_MEM_DELAYr - 1;
      if(ST_MEM_DELAYr == 0) STATE <= ST_NESCHR_RD_END;
      NES_CHR_DINr <= (ROM_ADDR0 ? ROM_DATA[7:0] : ROM_DATA[15:8]);
    end
    ST_MCU_RD_END, ST_MCU_WR_END, ST_NES_ROM_RD_END, ST_NES_ROM_WR_END,
    ST_NESCHR_RD_END: begin
      STATE <= ST_IDLE;
    end
  endcase
end

/***********************
 * R213F read patching *
 ***********************/
always @(posedge CLK2) begin
  case(r213f_state)
    ST_R213F_HOLD: begin
      r213f_state <= ST_R213F_HOLD;
      if(SNES_PULSE_end) begin
        r213f_forceread <= 1'b1;
        r213f_state <= ST_R213F_ARMED;
      end
    end
    ST_R213F_ARMED: begin
      r213f_state <= ST_R213F_ARMED;
      if(SNES_PARD_start & r213f_enable) begin
        r213f_delay <= 3'b001;
        r213f_state <= ST_R213F_WAITBUS;
      end
    end
    ST_R213F_WAITBUS: begin
      r213f_state <= ST_R213F_WAITBUS;
      r213f_delay <= r213f_delay - 1;
      if(r213f_delay == 3'b000) begin
        r213f_state <= ST_R213F_OVERRIDE;
        r213fr <= {SNES_DATA[7:5], mcu_region, SNES_DATA[3:0]};
      end
    end
    ST_R213F_OVERRIDE: begin
      r213f_state <= ST_R213F_HOLD;
      r213f_forceread <= 1'b0;
    end
  endcase
end

reg MCU_WRITE_1;
always @(posedge CLK2) MCU_WRITE_1<= MCU_WRITE;

// odd addresses xxx1
assign ROM_DATA[7:0] = ROM_ADDR0
                       ?(SD_DMA_TO_ROM ? (!MCU_WRITE_1 ? MCU_DOUT : 8'bZ)
                                       : NES_ROM_WR_HIT ? NES_ROM_DATAr
                                       : MCU_WR_HIT ? MCU_DOUT : 8'bZ
                        )
                       :8'bZ;

// even addresses xxx0
assign ROM_DATA[15:8] = ROM_ADDR0
                        ? 8'bZ
                        :(SD_DMA_TO_ROM ? (!MCU_WRITE_1 ? MCU_DOUT : 8'bZ)
                                        : NES_ROM_WR_HIT ? NES_ROM_DATAr
                                        : MCU_WR_HIT ? MCU_DOUT
                                        : 8'bZ
                         );

assign ROM_WE = SD_DMA_TO_ROM
                ?MCU_WRITE
                : NES_WE_HIT ? 1'b0
                : MCU_WE_HIT ? 1'b0
                : 1'b1;

assign ROM_BHE =  ROM_ADDR0 && !(!SD_DMA_TO_ROM && NES_ROM_HIT && NES_ROM_WORD);
assign ROM_BLE = !ROM_ADDR0 && !(!SD_DMA_TO_ROM && NES_ROM_HIT && NES_ROM_WORD);

//--------------
// RAM Pipeline (Bus 2, 512KB) -- the SNES-side renderer program (nes_snes.bin
// @ MCU 0x880000) EXECUTES from here via the LoROM window (address.v ROM_HIT =
// IS_ROM = ~ROMSEL since Fase 1; the "ROM_HIT hardwired 0" note that used to
// sit here was Phase -1 only and misled a hardware investigation -- with the
// renderer live, ram_free_strobe is ~never 1 and MCU Bus-2 grants ride on
// SNES_PULSE_end, one slot per SNES CPU cycle; validated in HW since Fase 1).
//--------------
parameter ST_RAM_IDLE            = 5'b00001;
parameter ST_RAM_MCU_RD_ADDR     = 5'b00010;
parameter ST_RAM_MCU_RD_END      = 5'b00100;
parameter ST_RAM_MCU_WR_ADDR     = 5'b01000;
parameter ST_RAM_MCU_WR_END      = 5'b10000;

parameter RAM_CYCLE_LEN = 4'd5;

reg [4:0] RAM_STATE; initial RAM_STATE = ST_RAM_IDLE;
reg [3:0] ST_RAM_DELAYr;

assign ram_free_slot = SNES_PULSE_end | ram_free_strobe;

// Provide full bandwidth if snes is not accessing the bus.
always @(posedge CLK2) begin
  ram_free_strobe <= 1'b0;
  if (SNES_cycle_start) ram_free_strobe <= ~ROM_HIT;
end

// MCU state machine
reg MCU_RAM_RD_PENDr = 0;
reg MCU_RAM_WR_PENDr = 0;
reg [18:0] RAM_ADDRr;

reg RQ_RAM_MCU_RDYr;
initial RQ_RAM_MCU_RDYr = 1'b1;

wire MCU_RAM_WE_HIT = |(RAM_STATE & ST_RAM_MCU_WR_ADDR);
wire MCU_RAM_WR_HIT = |(RAM_STATE & (ST_RAM_MCU_WR_ADDR | ST_RAM_MCU_WR_END));
wire MCU_RAM_RD_HIT = |(RAM_STATE & (ST_RAM_MCU_RD_ADDR | ST_RAM_MCU_RD_END));
wire MCU_RAM_HIT = MCU_RAM_WR_HIT | MCU_RAM_RD_HIT;

// MCU RAM1 r/w request
always @(posedge CLK2) begin
  if(MCU_RRQ & MCU_RAM) begin
    MCU_RAM_RD_PENDr <= 1'b1;
    RQ_RAM_MCU_RDYr <= 1'b0;
    RAM_ADDRr <= MCU_ADDR;
  end else if(MCU_WRQ & MCU_RAM) begin
    MCU_RAM_WR_PENDr <= 1'b1;
    RQ_RAM_MCU_RDYr <= 1'b0;
    RAM_ADDRr <= MCU_ADDR;
  end else if(RAM_STATE & (ST_RAM_MCU_RD_END | ST_RAM_MCU_WR_END)) begin
    MCU_RAM_RD_PENDr <= 1'b0;
    MCU_RAM_WR_PENDr <= 1'b0;
    RQ_RAM_MCU_RDYr <= 1'b1;
  end
end

// RAM state machine
always @(posedge CLK2) begin
  if(SNES_DEADr & SNES_CPU_CLKr[1]) RAM_STATE <= ST_RAM_IDLE; // interrupt+restart an ongoing MCU access when the SNES comes alive
  else
  case(RAM_STATE)
    ST_RAM_IDLE: begin
      if(ram_free_slot | SNES_DEADr) begin
        if(MCU_RAM_RD_PENDr) begin
          RAM_STATE <= ST_RAM_MCU_RD_ADDR;
          ST_RAM_DELAYr <= RAM_CYCLE_LEN;
        end
        else if(MCU_RAM_WR_PENDr) begin
          RAM_STATE <= ST_RAM_MCU_WR_ADDR;
          ST_RAM_DELAYr <= RAM_CYCLE_LEN;
        end
      end
    end
    ST_RAM_MCU_RD_ADDR: begin
      ST_RAM_DELAYr <= ST_RAM_DELAYr - 1;
      if(ST_RAM_DELAYr == 0) RAM_STATE <= ST_RAM_MCU_RD_END;
      MCU_RAM_DINr <= RAM_DATA;
    end
    ST_RAM_MCU_WR_ADDR: begin
      ST_RAM_DELAYr <= ST_RAM_DELAYr - 1;
      if(ST_RAM_DELAYr == 0) RAM_STATE <= ST_RAM_MCU_WR_END;
    end
    ST_RAM_MCU_RD_END, ST_RAM_MCU_WR_END: begin
      RAM_STATE <= ST_RAM_IDLE;
    end
  endcase
end

assign RAM_ADDR = (SD_DMA_TO_RAM) ? MCU_ADDR[18:0] : MCU_RAM_HIT ? RAM_ADDRr[18:0] : MAPPED_SNES_ADDR[18:0];

assign RAM_DATA[7:0] = (SD_DMA_TO_RAM ? (!MCU_WRITE_1 ? MCU_DOUT : 8'bZ)
                                      : (ROM_HIT & ~SNES_WRITE) ? SNES_DATA
                                      : MCU_RAM_WR_HIT ? MCU_DOUT
                                      : 8'bZ
                       );

// NOTE: IS_SAVERAM should never assert
assign RAM_WE = (SD_DMA_TO_RAM ? MCU_WRITE
                               : (ROM_HIT & IS_SAVERAM & SNES_CPU_CLK) ? SNES_WRITE
                               : MCU_RAM_WE_HIT ? 1'b0
                               : 1'b1
                );

assign RAM_OE = 1'b0;

always @(posedge CLK2) begin
  // flop data based on source
  if (STATE & ST_MCU_RD_END) begin
    MCU_DINr <= MCU_ROM_DINr;
  end
  else if (RAM_STATE & ST_RAM_MCU_RD_END) begin
    MCU_DINr <= MCU_RAM_DINr;
  end
end

assign MCU_RDY = RQ_MCU_RDYr & RQ_RAM_MCU_RDYr;

//--------------

assign SNES_DATABUS_OE = msu_enable & ~(SNES_READ_narrow & SNES_WRITE) ? 1'b0 :
                         nesbox_enable & ~(SNES_READ_narrow & SNES_WRITE) ? 1'b0 :
                         nesctl_enable & ~(SNES_READ_narrow & SNES_WRITE) ? 1'b0 :
                         nes_chr_enable & ~(SNES_READ_narrow & SNES_WRITE) ? 1'b0 :
                         snescmd_enable & ~(SNES_READ_narrow & SNES_WRITE) ? ~(snescmd_unlock | feat_cmd_unlock) :
                         (r213f_enable & ~SNES_PARD) ? 1'b0 :
                         (r2100_enable & ~SNES_PAWR) ? 1'b0 :
                         ( (IS_ROM & SNES_ROMSEL)
                         | (!IS_ROM & !IS_SAVERAM & !IS_WRITABLE)
                         | (SNES_READ_narrow & SNES_WRITE)
                         );

/* data bus direction: 0 = SNES -> FPGA; 1 = FPGA -> SNES
 * data bus is always SNES -> FPGA to avoid fighting except when:
 *  a) the SNES wants to read
 *  b) we want to force a value on the bus
 */
assign SNES_DATABUS_DIR = (~SNES_READ | (~SNES_PARD & (r213f_enable)))
                           ? (1'b1 ^ (r213f_forceread & r213f_enable & ~SNES_PARD)
                                   ^ (r2100_enable & ~SNES_PAWR & ~r2100_forcewrite & ~IS_ROM & ~IS_WRITABLE))
                           : ((~SNES_PAWR & r2100_enable) ? r2100_forcewrite
                             : 1'b0);

assign SNES_IRQ = 1'b0;

endmodule

`default_nettype wire
