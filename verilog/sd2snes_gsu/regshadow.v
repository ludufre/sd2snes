`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// Company: sd2snes
// Module Name: regshadow
// Description:
//   Small write-only register shadow for the in-game cheat overlay.  The overlay
//   restores the SNES PPU ($2100-$213F) and CPU ($4200-$421F) registers from a
//   shadow of the last value written to each, read back through the NMI hook's
//   identity window as PSRAM:
//     $F90500-$F9057F : PPU regs, stride-2 words (low byte = value, high = $00)
//     $F90700-$F9071F : CPU $42xx regs, stride-1 bytes
//   On the base/DSP/SA-1 mk3 cores that shadow is a by-product of the full ctx.v
//   PSRAM mirror.  The coprocessor cores that only run the overlay (no full
//   savestate) don't carry ctx.v, so this replaces just the one piece the overlay
//   needs with a single RAMB16 (block RAM is plentiful; LUTs are the scarce
//   resource, especially on the mk2 Spartan-3).
//
//   Storage layout (256x8, one RAMB16):
//     mem[0x00-0x3F] : PPU reg PA  ($2100-$213F -> PA $00-$3F)
//     mem[0x40-0x5F] : CPU reg     ($4200-$421F)
//
//   Write snoop uses the settled A-bus/PA/data taps + end strobes from main.v.
//   The two write sources are mutually exclusive within a SNES cycle (a CPU
//   access is either an A-bus write or a B-bus PA write); the else-if keeps a
//   single write address/enable so XST infers a clean one-write/one-read RAMB16.
//////////////////////////////////////////////////////////////////////////////////
module regshadow(
  input clk,
  // write snoop (settled taps + end strobes from main.v)
  input pawr_end,            // settled rising edge of /PAWR
  input wr_end,              // settled rising edge of /WR
  input [23:0] snes_addr,    // settled A-bus
  input [7:0] snes_pa,       // settled B-bus (PA)
  input [7:0] snes_data,     // settled data tap
  // read-serve side
  input [8:0] rd_addr,       // shadow read index (see mapping above)
  output reg [7:0] rd_data
);

// Force Block RAM so this costs no LUTs (the whole point on the full mk2 core).
(* ram_style = "block" *) reg [7:0] mem [0:255];

// XST block-RAM template requirement: ONE write enable and ONE write address
// expression inside the clocked process.  Two conditional writes to different
// computed addresses (the previous form) fall outside the template and XST
// silently builds the memory out of 2048 flip-flops + muxes (~400 LUTs --
// blows the mk2 budget).  So mux enable/address combinationally first.
// `pawr_end` is wired to the core's PAWR_START strobe (see main.v): the write byte
// is on the raw bus at the START of the B-bus write (that's where the native $2100
// patch reads it); the PAWR_END detection is pipelined ~6 cyc late and would grab
// the NEXT instruction's operand-high byte ($21 for STA $21xx -- seen in HW).
wire       ppu_wr = pawr_end & (snes_pa < 8'h40);
// $4200-$421F in ANY bank with ADDR[22]=0 ($00-$3F and $80-$BF) -- the CPU regs are
// mirrored there and games write them via FastROM banks ($80+); a bank-$00-only
// decode misses those (e.g. Metal Combat writes $4200 from $80 -> $4200 uncaptured).
wire       cpu_wr = wr_end & ~snes_addr[22]
                           & (snes_addr[15:5] == 11'b01000010000);
wire       wr_en  = ppu_wr | cpu_wr;
wire [7:0] wr_a   = ppu_wr ? {2'b00, snes_pa[5:0]}
                           : {3'b010, snes_addr[4:0]};

// Single write port + synchronous read = clean one-RAMB16 inference.
always @(posedge clk) begin
  if (wr_en)
    mem[wr_a] <= snes_data;
  rd_data <= mem[rd_addr[7:0]];
end

endmodule
