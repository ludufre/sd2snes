`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company:
// Engineer:
//
// Create Date:    16:53:07 07/01/2014
// Design Name:
// Module Name:    cheat
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

module cheat(
  input clk,
  input [7:0] SNES_PA,
  input [23:0] SNES_ADDR,
  input [7:0] SNES_DATA,
  input SNES_wr_strobe,
  input SNES_rd_strobe,
  input SNES_reset_strobe,
  input snescmd_enable,
  input nmicmd_enable,
  input return_vector_enable,
  input reset_vector_enable,
  input branch1_enable,
  input branch2_enable,
  
  input snescmd_rdy,
  output snescmd_we_cheat,
  output [8:0] snescmd_addr_cheat,
  output [7:0] snescmd_data_cheat,
  input  [11:0] DBG_ADDR,
  output [7:0] DBG_DATA_OUT,

  input pad_latch,
  input snes_ajr,
  input SNES_cycle_start,
  input [2:0] pgm_idx,
  input pgm_we,
  input [31:0] pgm_in,
  output [7:0] data_out,
  output cheat_hit,
  output snescmd_unlock  
);

wire snescmd_wr_strobe = snescmd_enable & SNES_wr_strobe;

reg cheat_enable = 0;
reg nmi_enable = 0;
reg irq_enable = 0;
reg holdoff_enable = 0; // temp disable hooks after reset
reg buttons_enable = 0;
reg wram_present = 0;

// The SGB snooped $010F11/$010F12 -- the WRAM addresses where the stock SGB2
// SNES ROM stores the joypad -- to detect in-game reset combos without an NMI
// hook.  That trick depends on that exact SNES ROM, and the SNES program under
// this core is gbc_snes.bin instead.  The player does its own combo matching
// and writes MCU_CMD ($2A00) directly (contract section 9), so the snoop and
// the button state machine it fed are gone.  What is left here is the reset
// vector patch that boots the SNES into the snescmd buffer.

wire [1:0]  rst_match_bits = {SNES_ADDR == 24'h00FFFC, SNES_ADDR == 24'h00FFFD};

reg  [29:0] hook_enable_count = 0;
// support the reset hook for booting the SNES ROM
reg  [1:0]  reset_unlock_r = 2'b10;
wire        reset_unlock = |reset_unlock_r;
wire        rst_addr_match = |rst_match_bits;
wire        hook_enable = ~|hook_enable_count;
reg         snescmd_unlock_r = 0;

assign      data_out = rst_match_bits[1] ? 8'h7d : 8'h2a;
assign      cheat_hit = (reset_unlock & rst_addr_match);
assign      snescmd_unlock = snescmd_unlock_r;

// make patched reset vector visible for first fetch only
// (including masked read by Ultra16)
always @(posedge clk) begin
  if(SNES_reset_strobe) begin
    reset_unlock_r <= 2'b11;
  end else if(SNES_cycle_start) begin
    if(rst_addr_match & |reset_unlock_r) begin
      reset_unlock_r <= reset_unlock_r - 1;
    end
  end
end

// handle unlocks for reset
reg snescmd_unlock_disable_strobe = 1'b0;
reg [6:0] snescmd_unlock_disable_countdown = 0;
reg snescmd_unlock_disable = 0;

always @(posedge clk) begin
  if(SNES_reset_strobe) begin
    snescmd_unlock_r <= 0;
    snescmd_unlock_disable <= 0;
  end else begin
    if(SNES_rd_strobe) begin
      if(rst_match_bits[1] & |reset_unlock_r) begin
        snescmd_unlock_r <= 1;
      end
    end
    // give some time to exit snescmd memory and jump to original vector
    if(SNES_cycle_start) begin
      if(snescmd_unlock_disable) begin
        if(|snescmd_unlock_disable_countdown) begin
          snescmd_unlock_disable_countdown <= snescmd_unlock_disable_countdown - 1;
        end else if(snescmd_unlock_disable_countdown == 0) begin
          snescmd_unlock_r <= 0;
          snescmd_unlock_disable <= 0;
        end
      end
    end
    if(snescmd_unlock_disable_strobe) begin
      snescmd_unlock_disable_countdown <= 7'd6;
      snescmd_unlock_disable <= 1;
    end
  end
end

// CMD 0x85: disable hooks for 10 seconds
always @(posedge clk) begin
  if((snescmd_unlock & snescmd_wr_strobe & ~|SNES_ADDR[8:0] & (SNES_DATA == 8'h85))
     | (holdoff_enable & SNES_reset_strobe)) begin
    hook_enable_count <= 30'd840000000; // 10 seconds
  end else if (|hook_enable_count) begin
    hook_enable_count <= hook_enable_count - 1;
  end
end

// MCU writes and internal updates
// Only need to support IRQ/NMI (now hook enable) buttons, and holdoff
always @(posedge clk) begin
  if(SNES_reset_strobe) begin
    snescmd_unlock_disable_strobe <= 1'b0;
  end else begin
    snescmd_unlock_disable_strobe <= 1'b0;

    if(snescmd_unlock & snescmd_wr_strobe) begin
      if(~|SNES_ADDR[8:0]) begin
        case(SNES_DATA)
          8'h82: cheat_enable <= 1;
          8'h83: cheat_enable <= 0;
          8'h84: {nmi_enable, irq_enable} <= 2'b00;
        endcase
      end else if(SNES_ADDR[8:0] == 9'h1fd) begin
        snescmd_unlock_disable_strobe <= 1'b1;
      end
    end else if(pgm_we) begin
      if(pgm_idx == 7) begin // set/reset global enable / hooks
      // pgm_in[13:8] are reset bit flags
      // pgm_in[5:0] are set bit flags
        {wram_present, buttons_enable, holdoff_enable, irq_enable, nmi_enable, cheat_enable}
         <= ({wram_present, buttons_enable, holdoff_enable, irq_enable, nmi_enable, cheat_enable}
          & ~pgm_in[13:8])
          | pgm_in[5:0];
      end
    end
  end
end

assign snescmd_we_cheat   = 1'b0;
assign snescmd_addr_cheat = 9'h000;
assign snescmd_data_cheat = 8'h00;

reg  [7:0]  dbg_data_r;
assign DBG_DATA_OUT = dbg_data_r;

always @(posedge clk) begin
  casez(DBG_ADDR[3:0])
    4'h6:    dbg_data_r <= snescmd_unlock_r;
    4'h7:    dbg_data_r <= hook_enable;
    4'h8:    dbg_data_r <= nmi_enable;
    4'h9:    dbg_data_r <= irq_enable;
    4'hA:    dbg_data_r <= buttons_enable;
    4'hB:    dbg_data_r <= cheat_enable;

    default:  dbg_data_r <= 0;
  endcase
end

endmodule
