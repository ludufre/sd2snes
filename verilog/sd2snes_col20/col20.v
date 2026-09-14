`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module Name:    col20
// Description:    Bank-select register for the Korean "Super 20 in 1" pirate
//                 LoROM multicart (NES games reimplemented as native 65816
//                 code, banked in 32KB pages). Modeled directly on
//                 sd2snes_obc1/obc1.v -- same clk/enable/reg_we_rising shape,
//                 but this chip has no chip-RAM: it's a single write-only
//                 8-bit register, no read path at all.
//
//                 Behaviour ported from MAME's sns_rom_20col_device
//                 (src/devices/bus/snes/rom.cpp, Priuli/byuu, GPL-2.0+):
//                   base_bank <= data & 0xdf   (bit 5 unused/always 0)
//                 bits 0-4: bank index (0-31, 32KB pages)
//                 bits 6-7: size mode -- 0x80 (bit7=1,bit6=0) locks the
//                   window to a single 32KB page (base_bank alone); any other
//                   combination lets consecutive SNES banks pull in
//                   consecutive ROM pages (see address.v).
//
// ASSUMPTION FLAGGED FOR VERIFICATION: `enable` is expected to be driven by
// address.v as a decode of SNES_ADDR == 24'h808000, per the one concrete
// detail in the MAME source comment ("written at 0x808000"). MAME's driver
// doesn't show the actual chip-select mask used by the cart slot config, and
// cheap pirate boards often decode addresses incompletely (mirroring across
// more banks/offsets than a real chip would). If games hang or crash when
// switching titles from the multicart's own launcher (bank 0), the most
// likely fix is widening col20_enable in address.v to also catch mirrors of
// $808000 -- verify by tracing the launcher's bank-switch write on real
// hardware or in a non-MAME 65816 disassembly of bank 0.
//////////////////////////////////////////////////////////////////////////////////
module col20(
  input clk,
  input reset,
  input enable,
  input [7:0] data_in,
  input reg_we_rising,
  output [7:0] base_bank
);

reg [7:0] base_bank_reg = 8'h00;
reg boot_done = 1'b0;

always @(posedge clk) begin

  if (reset & boot_done) begin
    // A console reset must go back through page 0 so the
    // cartridge can re-run its APU/SPC bootstrap.
    base_bank_reg <= 8'h00;
    boot_done <= 1'b0;
  end

  else if (enable & reg_we_rising) begin
    base_bank_reg <= data_in & 8'hdf;
    boot_done <= 1'b1;
  end

end

assign base_bank = base_bank_reg;

endmodule
