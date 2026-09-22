`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// Company: Rehkopf
// Engineer: Rehkopf
//
// Create Date:    01:13:46 05/09/2009
// Design Name:
// Module Name:    address
// Project Name:
// Target Devices:
// Tool versions:
// Description: Address logic w/ SaveRAM masking
//
// Dependencies:
//
// Revision:
// Additional Comments:
//
//////////////////////////////////////////////////////////////////////////////////
module address(
  input CLK,
  input [15:0] featurebits, // peripheral enable/disable
  input [2:0] MAPPER,       // MCU detected mapper
  input [23:0] SNES_ADDR_early,   // requested address from SNES
  input SNES_WRITE_early,
  input [7:0] SNES_PA,      // peripheral address from SNES
  input SNES_ROMSEL,        // ROMSEL from SNES
  output [23:0] ROM_ADDR,   // Address to request from SRAM0
  output ROM_HIT,           // enable SRAM0
  output IS_SAVERAM,        // address/CS mapped as SRAM?
  output IS_ROM,            // address mapped as ROM?
  output IS_WRITABLE,       // address somehow mapped as writable area?
  output IS_PATCH,          // linear address map (C0-FF, Ex/Fx)
  input [7:0] SAVERAM_BASE,
  input [23:0] SAVERAM_MASK,
  input [23:0] ROM_MASK,
  input [7:0] CC_DR,        // event cart (CC92/PF94) game-select latch
  output cc_sel,            // event cart select/status register window
  input  map_unlock,
  input  map_Ex_rd_unlock,
  input  map_Ex_wr_unlock,
  input  map_Fx_rd_unlock,
  input  map_Fx_wr_unlock,
  input  snescmd_unlock,
  output msu_enable,
  output dma_enable,
  output st018_enable,
  output r213f_enable,
  output r2100_hit,
  output snescmd_enable,
  output nmicmd_enable,
  output return_vector_enable,
  output branch1_enable,
  output branch2_enable,
  output branch3_enable,
  output exe_enable,
  output map_enable
);

/* feature bits. see src/fpga_spi.c for mapping */
parameter [3:0]
  FEAT_DSPX = 0,
  // ST011 uses FEAT_ST0010 -- the bit means "uPD96050 present", and the
  // ST010/ST011 split is by CORE (fpga_dsp vs fpga_st0011), not by bit.
  // There is no free bit to split them with: 0-13 are allocated in
  // src/fpga_spi.h (7-10 by FEAT_2100_LIMIT) and 14/15 are FEAT_CC92 and
  // FEAT_PF94 below, which fpga_spi.h does not list. Setting bit 14 makes
  // this decode treat the cart as a Campus Challenge '92 event board and
  // remaps ROM/SaveRAM -- black screen from the first fetch.
  FEAT_ST0010 = 1,
  FEAT_MSU1 = 3,
  FEAT_213F = 4,
  FEAT_SNESUNLOCK = 5,
  FEAT_2100 = 6,
  FEAT_DMA1 = 11,
  FEAT_CC92 = 14,           // Campus Challenge '92 event board
  FEAT_PF94 = 15            // PowerFest '94 event board
;

integer i;
reg [7:0] MAPPER_DEC; always @(posedge CLK) for (i = 0; i < 8; i = i + 1) MAPPER_DEC[i] <= (MAPPER == i);
reg [23:0] SNES_ADDR; always @(posedge CLK) SNES_ADDR <= SNES_ADDR_early;

wire [23:0] SRAM_SNES_ADDR;
wire [23:0] SAVERAM_ADDR = {4'hE,1'b0,SAVERAM_BASE,11'h0};

/* currently supported mappers:
   Index     Mapper
      000      HiROM
      001      LoROM
      010      ExHiROM (48-64Mbit)
      011      BS-X
      110      brainfuck interleaved 96MBit Star Ocean =)
      111      menu (ROM in upper SRAM)
*/

/* HiROM:   SRAM @ Bank 0x30-0x3f, 0xb0-0xbf
            Offset 6000-7fff */

assign IS_ROM = ~SNES_ROMSEL;

assign IS_SAVERAM_pre = (~map_unlock & SAVERAM_MASK[0])
/*  PF'94 event board: SRAM @ Bank 0x30-0x3f, 0xb0-0xbf, offset 6000-7fff
 *  (HiROM-style window; the menu program keeps its bookkeeping at $306420).
 *  Replaces the LoROM rule below, whose bank $70 window would collide with
 *  the menu mirror there.  CC'92 keeps the standard LoROM rule (its menu
 *  writes $700420).
 *  (The ST010/ST011 $68-$6F SRAM window of the core this was derived from is
 *  gone: ST018 carts use the standard LoROM SRAM rule.) */
                    &(featurebits[FEAT_PF94]
                      ? ((SNES_ADDR_early[22:20] == 3'b011)
                        & (SNES_ADDR_early[15:13] == 3'b011))
                      :((MAPPER_DEC[3'b000]
                        || MAPPER_DEC[3'b010]
                        || MAPPER_DEC[3'b110])
                      ? (!SNES_ADDR_early[22]
                         & SNES_ADDR_early[21]
                         & &SNES_ADDR_early[14:13]
                         & !SNES_ADDR_early[15]
                        )
/*  LoROM:   SRAM @ Bank 0x70-0x7d, 0xf0-0xff
 *  Offset 0000-7fff for ROM >= 32 MBit, otherwise 0000-ffff
 *
 *  ST018 board (ares: ARM-LOROM-RAM, "map address=68-6f,f0-ff:0000-7fff"):
 *  the 8 KB SaveRAM also answers at banks 0x68-0x6f, offset 0000-7fff, and
 *  that is the window the game actually uses -- its save/load routines at
 *  $00:CCB8 and $00:CCD2 copy 0x0E80 bytes between $68:0180 and $7E:4180
 *  with long addressing. Without this window the writes go nowhere and the
 *  reads return ROM, so the game appears to save and then finds no file.
 *  Unconditional here: this core is only ever loaded for ST018 carts.
 *  Bit 23 is qualified so the 0xe8-0xef mirror stays out, matching the
 *  board; the 0xf0-0xff half is already covered by the rule above. */
                      :(MAPPER_DEC[3'b001])
                      ? ((&SNES_ADDR_early[22:20]
                          & (~SNES_ROMSEL)
                          & (~SNES_ADDR_early[15] | ~ROM_MASK[21])
                         )
                         | (~SNES_ADDR_early[23]
                            & (SNES_ADDR_early[22:19] == 4'b1101)
                            & (~SNES_ROMSEL)
                            & ~SNES_ADDR_early[15]
                           )
                        )
/*  Menu mapper: 8Mbit "SRAM" @ Bank 0xf0-0xff (entire banks!) */
                      :(MAPPER_DEC[3'b111])
                      ? (&SNES_ADDR_early[23:20])
                      : 1'b0));

reg IS_SAVERAM_r; always @(posedge CLK) IS_SAVERAM_r <= IS_SAVERAM_pre;
assign IS_SAVERAM = IS_SAVERAM_r;

// give the patch free reign over $F0-$FF banks
// map_unlock: F0-FF
// map_Ex: E0-EF
// map_Fx: F0-FF
// snescmd_unlock: C0-FF
assign IS_PATCH = ( (map_unlock 
                     | (map_Fx_rd_unlock & SNES_WRITE_early)
                     | (map_Fx_wr_unlock & ~SNES_WRITE_early)
                    ) & (&SNES_ADDR[23:20])
                  )
                | ( ((map_Ex_rd_unlock & SNES_WRITE_early)
                    |(map_Ex_wr_unlock & ~SNES_WRITE_early)
                    ) & ({SNES_ADDR[23:20]} == 4'hE)
                  )
                | (snescmd_unlock & &SNES_ADDR[23:22]); // full access to C0-FF

assign IS_WRITABLE = IS_SAVERAM
                     |IS_PATCH; // allow writing of the patch region

/* Event carts (CC'92 / PF'94): 256 KB menu chip + 3 game chips staged linearly
 * in PSRAM (menu at 0, games at +040000/+0C0000/+140000).  The menu program's
 * select latch (CC_DR, in main.v) decides which chip answers the game area;
 * the menu chip stays visible at its own region so the games can jump back
 * into it, and an unknown/zero select falls back to the menu (the reset vector
 * is fetched from bank $00, which must land in the menu chip).
 * CC'92: menu @ banks 80-ff:8000+, games @ 00-7f (all LoROM folds).
 * PF'94: menu @ banks 20-3f/60-7f/a0-bf/e0-ff:8000+; game 2 is HiROM-style
 * (linear A[18:0]), game 3 a 1 MB LoROM fold. */
wire [23:0] cc_menu_addr = {6'b0, SNES_ADDR[18:16], SNES_ADDR[14:0]};
wire [23:0] cc_lofold    = {5'b0, SNES_ADDR[19:16], SNES_ADDR[14:0]};
wire [23:0] cc92_rom_addr =
    (SNES_ADDR[23] & SNES_ADDR[15]) ? cc_menu_addr
  : (CC_DR == 8'h09) ? cc_lofold + 24'h040000
  : (CC_DR == 8'h05) ? cc_lofold + 24'h0C0000
  : (CC_DR == 8'h03) ? cc_lofold + 24'h140000
  : cc_menu_addr;
wire [23:0] pf94_rom_addr =
    (SNES_ADDR[21] & SNES_ADDR[15]) ? cc_menu_addr
  : (CC_DR == 8'h09) ? cc_lofold + 24'h040000
  : (CC_DR == 8'h0C) ? {5'b0, SNES_ADDR[18:0]} + 24'h0C0000
  : (CC_DR == 8'h0A) ? {4'b0, SNES_ADDR[20:16], SNES_ADDR[14:0]} + 24'h140000
  : cc_menu_addr;

assign SRAM_SNES_ADDR = IS_PATCH
                        ? SNES_ADDR
                        : (featurebits[FEAT_CC92] | featurebits[FEAT_PF94])
                        ?(IS_SAVERAM
                          ? SAVERAM_ADDR + ({SNES_ADDR[20:16], SNES_ADDR[14:0]}
                                          & SAVERAM_MASK)
                          : featurebits[FEAT_CC92] ? cc92_rom_addr
                                                   : pf94_rom_addr)
                        : ((MAPPER_DEC[3'b000])
                          ?(IS_SAVERAM
                            ? SAVERAM_ADDR + ({SNES_ADDR[20:16], SNES_ADDR[12:0]}
                                            & SAVERAM_MASK)
                            : ({1'b0, SNES_ADDR[22:0]} & ROM_MASK))

                          :(MAPPER_DEC[3'b001])
                          ?(IS_SAVERAM
                            ? SAVERAM_ADDR + ({SNES_ADDR[20:16], SNES_ADDR[14:0]}
                                            & SAVERAM_MASK)
                            : ({1'b0, ~SNES_ADDR[23], SNES_ADDR[22:16], SNES_ADDR[14:0]}
                               & ROM_MASK))

                          :(MAPPER_DEC[3'b010])
                          ?(IS_SAVERAM
                            ? SAVERAM_ADDR + ({SNES_ADDR[20:16], SNES_ADDR[12:0]}
                                            & SAVERAM_MASK)
                            : ({1'b0, !SNES_ADDR[23], SNES_ADDR[21:0]}
                               & ROM_MASK))
                           :(MAPPER_DEC[3'b110])
                           ?(IS_SAVERAM
                             ? SAVERAM_ADDR + ((SNES_ADDR[14:0] - 15'h6000)
                                             & SAVERAM_MASK)
                             :(SNES_ADDR[15]
                               ?({1'b0, SNES_ADDR[23:16], SNES_ADDR[14:0]})
                               :({2'b10,
                                  SNES_ADDR[23],
                                  SNES_ADDR[21:16],
                                  SNES_ADDR[14:0]}
                                )
                              )
                            )
                           :(MAPPER_DEC[3'b111])
                           ?(IS_SAVERAM
                             ? SNES_ADDR
                             : (({1'b0, SNES_ADDR[22:0]} & ROM_MASK)
                                + 24'hC00000)
                            )
                           : 24'b0);

assign ROM_ADDR = SRAM_SNES_ADDR;

assign ROM_HIT = (IS_ROM | IS_WRITABLE) & ~cc_sel;

assign msu_enable = featurebits[FEAT_MSU1] & (!SNES_ADDR[22] && ((SNES_ADDR[15:0] & 16'hfff8) == 16'h2000));
// dma.v is not instantiated in the ST018 core (see main.v): never claim $2020.
assign dma_enable = 1'b0;
assign exe_enable =                           (!SNES_ADDR[22] && ((SNES_ADDR[15:0] & 16'hffff) == 16'h2C00));
assign map_enable =                           (!SNES_ADDR[22] && ((SNES_ADDR[15:0] & 16'hffff) == 16'h2BB2));

// ST018: host registers at $00-3F/$80-BF:3800-38FF (A2:A1 select, A0 and
// A7:A3 ignored -- the reference emulators decode addr & $FF06). Unconditional:
// this core is
// only ever loaded for ST018 carts (the cart is identified by core, not by a
// featurebit -- all 16 featurebits are allocated, see src/fpga_spi.h).
assign st018_enable = !SNES_ADDR[22] && (SNES_ADDR[15:8] == 8'h38);

// Event cart select/status window.  Reads return the status byte (CC_SR in
// main.v: bit 1 = round timer expired); writes with A21=1 latch the game
// select (CC_DR).  The boards decode loosely, so whole banks mirror:
// CC'92: banks c0-cf (status, read @ $C00000) / e0-ef (select, write @ $E00000)
// PF'94: banks 10-2f/90-af : 6000-7fff (read @ $106000, write @ $206000)
// Muzzled while any patch/snescmd unlock is held: the in-game overlay and
// savestate handlers own banks $C0-$FF then, and a handler write landing in
// $E0-$EF would otherwise latch garbage into CC_DR.
wire cc_unlocked = map_unlock | snescmd_unlock
                 | map_Ex_rd_unlock | map_Ex_wr_unlock
                 | map_Fx_rd_unlock | map_Fx_wr_unlock;
assign cc_sel = ~cc_unlocked
              & (featurebits[FEAT_CC92]
                ? ((SNES_ADDR[23:20] == 4'hC) | (SNES_ADDR[23:20] == 4'hE))
                : featurebits[FEAT_PF94]
                ? (((SNES_ADDR[23:20] == 4'h1) | (SNES_ADDR[23:20] == 4'h2)
                   |(SNES_ADDR[23:20] == 4'h9) | (SNES_ADDR[23:20] == 4'hA))
                  & (SNES_ADDR[15:13] == 3'b011))
                : 1'b0);

assign r213f_enable = featurebits[FEAT_213F] & (SNES_PA == 8'h3f);
assign r2100_hit = (SNES_PA == 8'h00);

// snescmd covers $2A00-$2FFF.  This overlaps with at least one hardware cheat device address range.
assign snescmd_enable = ({SNES_ADDR[22], SNES_ADDR[15:11]} == 6'b0_00101) && (SNES_ADDR[10:9] != 2'b00);
assign nmicmd_enable = (SNES_ADDR == 24'h002BF2);
assign return_vector_enable = (SNES_ADDR == 24'h002A6C);
assign branch1_enable = (SNES_ADDR == 24'h002A1F);
assign branch2_enable = (SNES_ADDR == 24'h002A59);
assign branch3_enable = (SNES_ADDR == 24'h002A5E);
endmodule
