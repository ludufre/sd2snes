`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// Module Name:    bootleg
// Description:    Copy-protection hardware of the unlicensed LoROM SNES bootlegs
//                 (fullsnes "SNES Cart Unlicensed Variants", cross-checked against
//                 MAME src/devices/bus/snes/rom.cpp + src/mame/nintendo/snes.cpp).
//
//   variant (chipfeat[2:0], set by the MCU from the ROM CRC32, see src/bootleg.c)
//     0 = none      plain LoROM, this module is inert
//     1 = BITSWAP   "standard" latch: A Bug's Life, Aladdin 2000, Bananas de
//                   Pijamas, Digimon Adventure, KOF2000, Pocket Monster,
//                   Pokemon Gold Silver, Pokemon Stadium, Soul Edge vs Samurai,
//                   SF EX Plus Alpha, X-Men vs SF
//     2 = CONSTANT  Soul Blade
//     3 = ALU       Tekken 2, Street Fighter EX Plus Alpha
//     4 = PORT6     A Bug's Life, Bananas de Pijamas ("port 6xxx", nocash 2017)
//     5 = BITSWAP40 Marvel Super Heroes vs Street Fighter: the standard bitswap
//                   latch wired to other address lines (nocash 2017)
//     6 = KOF98     King of Fighters '98: standard BITSWAP plus a ROM bank switch
//                   (Revenant/nocash, nesdev t=15510, 2017)
//
//   BITSWAP:  banks with A23=1, A18..A16=000 (80,88,90..F8 -- MAME mask 0x780000),
//             ROM area only (A15=1, or any offset in C0-FF).  fullsnes names the
//             read port 80:8000-FFFF and the write port 88:8000-FFFF; both sit in
//             this window, so reads and writes share the one latch as in MAME.
//               write: latch <= D
//               read : D = latch bits {0,6,7,1,2,3,4,5} (msb first)
//   CONSTANT: 80-BF:8000-FFFF reads 55,0F,AA,F0 by A1..A0; C0-FF open bus.
//   ALU:      80-BF:8000-87FF, any read or write updates the state by A10..A8:
//               0: clear   1: (read port)  2..5: set bit0..3
//               6: set direction   7: set function
//             reads of 81xx return (4-bit value) +1 / -1 / <<1 / >>1, 8-bit
//             result exactly as MAME; other reads in the window are open bus.
//
//   PORT6:    00-3F/80-BF:6000-6FFF.  The real function is undocumented.  Both
//             games write sequences to 60xx/62xx/64xx/66xx/68xx and then check
//             the low nibble of the odd ports once:
//               Bananas 81:FF00: 61=2, 63=4, 65=F, 67=0; then writes 00 to
//                                60..68 and wants 61=0 (banks B0)
//               Bug's Life 00:CB15: (61 [AND 63 if 61=x5]) ^ 65 ^ 67 ^ 6F = E
//                                (writes bank 38, reads bank 32)
//             One answer set passes both: 61 = 2 while the last 60xx write was
//             non-zero (else 0), 63=4, 65=F, 67=0, 6F=3, anything else 0.
//             Verified in emulation (LakeSnes + this model) for both games.
//
//   BITSWAP40: same latch and read-back bit order as BITSWAP, decoded at
//             40-4F:8000-FFFF.  The game writes at A1=1 and reads at A1=0
//             (40:8182/8180, 46:84E2/84E0, 4E:9062/9060 ...), always as a
//             write immediately followed by the read 2 bytes lower; it never
//             reads ROM data in 40-7D.  Whether the chip decodes A1 or the /RD
//             /WR strobes can't be told from the game, so the core uses the
//             strobes (write -> latch, read -> swapped latch), which satisfies
//             both readings.  C0-CF is left alone (never accessed).
//
//   KOF98:    BITSWAP exactly as variant 1, plus a 4-bit bank register written
//             at C0-CF:8000-FFFF (the game only uses C0:8788, values 82/00).
//             While bit 7 is set, ROM accesses see SNES A19..A16 replaced by
//             the register's low nibble (a 74LS157 on the cart muxes those
//             lines; nocash's model).  The game copies a routine to WRAM $0700,
//             writes 82, calls 00:9767/00:9815 (= ROM bank 02) and writes 00.
//             Emulation: 14434 switches in 8000 frames, every switched read in
//             bank 00, no NMI/IRQ while switched.  The remap itself is applied
//             in main.v on the address fed to address.v (ROM accesses only).
//
//   Everything is gated by ~IS_PATCH so the in-game menu / savestate hooks that
//   own $F0-$FF (map_unlock etc.) keep working.
//////////////////////////////////////////////////////////////////////////////////
module bootleg(
  input         clk,
  input         reset,          // SNES_reset_strobe
  input  [2:0]  variant,

  input  [23:0] snes_addr,      // main.v SNES_ADDR (filtered)
  input  [7:0]  snes_data_in,   // main.v BUS_DATA (valid at WR_end)
  input         is_patch,
  input         rd_strobe,      // SNES_RD_start: one per read cycle
  input         wr_strobe,      // SNES_WR_end:   one per write cycle

  output        rd_hit,         // drive data_out on this read
  output [7:0]  data_out,
  output        open_bus,       // leave the SNES data bus undriven

  output        rom_bank_en,    // KOF98: remap ROM A19..A16 ...
  output [3:0]  rom_bank        //        ... to this value
);

localparam [2:0] V_NONE     = 3'd0,
                 V_BITSWAP  = 3'd1,
                 V_CONSTANT = 3'd2,
                 V_ALU      = 3'd3,
                 V_PORT6    = 3'd4,
                 V_BS40     = 3'd5,
                 V_KOF98    = 3'd6;

// registered decode of the variant (static while a game runs)
reg v_bitswap, v_constant, v_alu, v_port6, v_bs40, v_kof98;
always @(posedge clk) begin
  v_port6    <= (variant == V_PORT6);
  v_bs40     <= (variant == V_BS40);
  v_kof98    <= (variant == V_KOF98);
  v_bitswap  <= (variant == V_BITSWAP) | (variant == V_KOF98);
  v_constant <= (variant == V_CONSTANT);
  v_alu      <= (variant == V_ALU);
end

wire a23 = snes_addr[23];
wire a22 = snes_addr[22];
wire a15 = snes_addr[15];

// ---------------------------------------------------------------- BITSWAP
// KOF98: the bank-switch register (C0-CF:8000-FFFF, see below) is a separate latch
// (nocash: an 82 in the bitswap latch would upset it), so that window is carved out.
wire kb_addr = a23 & a22 & (snes_addr[21:20] == 2'b00) & a15;
wire bs_win = v_bitswap & ~is_patch & a23 & (snes_addr[18:16] == 3'b000) & (a22 | a15)
              & ~(v_kof98 & kb_addr);
// BITSWAP40 (Marvel): same latch, banks 40-4F:8000-FFFF
wire bs40_win = v_bs40 & ~is_patch & ~a23 & a22 & (snes_addr[21:20] == 2'b00) & a15;
wire bsl_win = bs_win | bs40_win;

reg [7:0] bs_latch;
always @(posedge clk) begin
  if(reset)                  bs_latch <= 8'h00;
  else if(bsl_win & wr_strobe) bs_latch <= snes_data_in;
end
wire [7:0] bs_dout = {bs_latch[0], bs_latch[6], bs_latch[7], bs_latch[1],
                      bs_latch[2], bs_latch[3], bs_latch[4], bs_latch[5]};

// ---------------------------------------------------------------- CONSTANT
wire cs_win  = v_constant & ~is_patch & a23 & ~a22 & a15;   // 80-BF:8000-FFFF
wire cs_void = v_constant & ~is_patch & a23 &  a22;         // C0-FF: open bus
reg [7:0] cs_dout;
always @* begin
  case(snes_addr[1:0])
    2'd0: cs_dout = 8'h55;
    2'd1: cs_dout = 8'h0F;
    2'd2: cs_dout = 8'hAA;
    2'd3: cs_dout = 8'hF0;
  endcase
end

// ---------------------------------------------------------------- ALU
wire al_win  = v_alu & ~is_patch & a23 & ~a22 & (snes_addr[15:11] == 5'b10000); // 80-BF:8000-87FF
wire al_port = al_win & (snes_addr[10:8] == 3'd1);                                // 81xx

reg [5:0] al_prot;   // {function, direction, data[3:0]}
always @(posedge clk) begin
  if(reset) al_prot <= 6'd0;
  else if(al_win & (rd_strobe | wr_strobe)) begin
    case(snes_addr[10:8])
      3'd0: al_prot <= 6'd0;
      3'd1: ;                                   // read port, no change
      3'd2: al_prot[0] <= 1'b1;
      3'd3: al_prot[1] <= 1'b1;
      3'd4: al_prot[2] <= 1'b1;
      3'd5: al_prot[3] <= 1'b1;
      3'd6: al_prot[4] <= 1'b1;                 // direction
      3'd7: al_prot[5] <= 1'b1;                 // function
    endcase
  end
end

wire [7:0] al_val = {4'h0, al_prot[3:0]};
reg  [7:0] al_dout;
always @* begin
  case(al_prot[5:4])
    2'b00: al_dout = al_val + 8'd1;             // count up
    2'b01: al_dout = al_val - 8'd1;             // count down
    2'b10: al_dout = {al_val[6:0], 1'b0};       // shift left
    2'b11: al_dout = {1'b0, al_val[7:1]};       // shift right
  endcase
end

// ---------------------------------------------------------------- PORT6
wire p6_win = v_port6 & ~is_patch & ~a22 & (snes_addr[15:12] == 4'h6);   // 00-3F,80-BF:6000-6FFF
reg  p6_arm;
always @(posedge clk) begin
  if(reset) p6_arm <= 1'b0;
  else if(p6_win & wr_strobe & (snes_addr[11:8] == 4'h0)) p6_arm <= |snes_data_in;
end
reg [7:0] p6_dout;
always @* begin
  case(snes_addr[11:8])
    4'h1:    p6_dout = p6_arm ? 8'h02 : 8'h00;
    4'h3:    p6_dout = 8'h04;
    4'h5:    p6_dout = 8'h0F;
    4'hF:    p6_dout = 8'h03;
    default: p6_dout = 8'h00;
  endcase
end

// ---------------------------------------------------------------- KOF98 bank switch
wire kb_win = v_kof98 & ~is_patch & kb_addr;  // C0-CF:8000-FFFF
reg [7:0] kb_reg;
always @(posedge clk) begin
  if(reset)                  kb_reg <= 8'h00;
  else if(kb_win & wr_strobe) kb_reg <= snes_data_in;
end
assign rom_bank_en = v_kof98 & kb_reg[7];
assign rom_bank    = kb_reg[3:0];

// ---------------------------------------------------------------- outputs
assign rd_hit   = bsl_win | cs_win | al_port | p6_win;
assign data_out = bsl_win ? bs_dout
                : cs_win ? cs_dout
                : p6_win ? p6_dout
                : al_dout;
assign open_bus = cs_void | (al_win & ~al_port);

endmodule
