`timescale 1 ns / 1 ns
//////////////////////////////////////////////////////////////////////////////////
// address.v -- sd2snes_nes SNES-bus decode. Phase -1 has no SNES-side program
// (CONTRACT SS3.5/SS9: nothing boots on the SNES yet, the MCU talks to the core
// only through the PSRAM/config-register plumbing), so this is deliberately the
// minimal decode that keeps the rest of main.v (the mailbox, the r213f/r2100
// compatibility patches that are NOT core-specific) working, per CONTRACT SS5.3
// ("esse bloco inteiro de address.v pode ficar minimalista").
//
// Dropped relative to sd2snes_sgb/address.v: sgb_enable (SGB's $6000-$7FFF MMIO
// window -- no such window exists here), button_enable/button_addr (SGB's
// joypad-snoop shortcut, tied to the sys-sgb SNES-side program's WRAM layout,
// meaningless without one). ROM_HIT/IS_ROM/IS_WRITABLE are tied off (no cart
// mapped to the SNES in Phase -1); wiring in a real LoROM/HiROM map is future
// work once a SNES-side program exists (see CONTRACT SS7 for what that would
// need: a reset-vector hook at minimum, matching sys-sgb's convention).
//////////////////////////////////////////////////////////////////////////////////

module address(
  input CLK,
  input [15:0] featurebits, // peripheral enable/disable
  input [23:0] SNES_ADDR,   // requested address from SNES
  input [7:0] SNES_PA,      // peripheral address from SNES
  input SNES_ROMSEL,        // ROMSEL from SNES
  output [23:0] ROM_ADDR,   // Address to request from SRAM0
  output ROM_HIT,           // enable SRAM0
  output IS_SAVERAM,        // address/CS mapped as SRAM?
  output IS_ROM,            // address mapped as ROM?
  output IS_WRITABLE,       // address somehow mapped as writable area?
  output msu_enable,
  output r213f_enable,
  output r2100_hit,
  output snescmd_enable,
  output nesbox_enable,   // video-bridge mailbox window $6000-$7FFF (8 KiB)
  output nesctl_enable,   // video-bridge control block $2A00-$2A0F
  output nes_chr_enable   // CHR-SNES windows: banks $50-$5F/$60-$6F -> PSRAM Bus-1
);

/* feature bits. see src/fpga_spi.c for mapping */
parameter [2:0]
  FEAT_DSPX = 0,
  FEAT_ST0010 = 1,
  FEAT_SRTC = 2,
  FEAT_MSU1 = 3,
  FEAT_213F = 4,
  FEAT_2100 = 6
;

// -------------------------------------------------------------------------
// Renderer program (nes_snes.bin) -- LoROM no BUS-2 (SRAM 512KB), molde
// sd2snes_sgb/address.v EXATO (o sys-sgb roda do Bus-2 do mesmo jeito).
// O "0x880000" do contrato e' o ENDERECO NO ESPACO DO MCU do Bus-2 (main.v:
// MCU_RAM = addr[23:19]==10001 -> 0x880000-0x8FFFFF; comentario do proprio
// main.v: "RAM contains the SNES-side program ... mapped to 880000-8FFFFF").
// O firmware grava o nes_snes.bin la via sram_writeblock; a janela LoROM
// abaixo squasha o acesso do SNES para RAM_ADDR[18:0] (o +0x880000 nominal
// trunca fora nos pinos do Bus-2) -> mesmo lugar.  Fetch de codigo do
// renderer = Bus-2 passthrough (SEM arbitragem, ZERO contencao com o core
// NES no Bus-1): a analise de pior caso do cliente Bus-1 vale so pros DMAs
// de CHR $50/$60, nunca pra codigo.
//
// SEM gate de featurebit (root cause da TELA PRETA da iteracao 3 de
// hardware): a versao anterior exigia featurebits[5], um bit que o firmware
// nunca setou -> ROM_HIT=0 -> SNES_DATABUS_OE nunca habilitava -> o fetch do
// reset vector em $00:FFFC lia open bus -> o renderer nunca executou (WRAM/
// CIRAM 100% zeradas, GO nunca escrito).  IS_ROM = ~ROMSEL como em TODOS os
// cores do chassi (sgb/base address.v); conteudo residente e' garantido pela
// ordem do load, e o handshake NES_GO ja ordena o boot do core NES.
// IS_WRITABLE=0 (programa read-only; shadows do renderer vivem na WRAM).
// -------------------------------------------------------------------------
localparam [23:0] NES_RENDERER_BASE = 24'h880000;

assign IS_ROM      = ~SNES_ROMSEL;
assign IS_SAVERAM  = 1'b0;
assign IS_WRITABLE = 1'b0;
assign ROM_ADDR    = NES_RENDERER_BASE + {2'b00, SNES_ADDR[21:16], SNES_ADDR[14:0]};
assign ROM_HIT     = IS_ROM;

assign msu_enable = featurebits[FEAT_MSU1] & (!SNES_ADDR[22] && ((SNES_ADDR[15:0] & 16'hfff8) == 16'h2000));

assign r213f_enable = featurebits[FEAT_213F] & (SNES_PA == 8'h3f);
assign r2100_hit = (SNES_PA == 8'h00);

// Mailbox decode -- identical range/pattern to sd2snes_sgb/address.v (banks
// $2A00-$2FFF); kept working even with no SNES-side program yet so the MCU
// can use it (SETADDR/READMEM/WRITEMEM already do, via mcu_cmd.v) without a
// second decode scheme needing to be invented later.
assign snescmd_enable = ({SNES_ADDR[22], SNES_ADDR[15:9]} == 8'b0_0010101);

// Video-bridge mailbox window: $6000-$7FFF (8 KiB), low banks (!A22).  Molde
// sd2snes_sgb sgb_enable, mas janela contígua inteira (streamed por DMA).
assign nesbox_enable = !SNES_ADDR[22] && (SNES_ADDR[15:13] == 3'b011);

// Video-bridge control block: $2BD0-$2BDF (16 B), dentro da página snescmd mas
// SERVIDO pela bridge (não pela BRAM snescmd) -- reads devolvem estado da bridge
// (SEQ/LEN/STATUS + magic), writes latcham entradas do renderer (ACK/BUF_SEL/
// CTRL/GO).  Decode dedicado (evita um 3o escritor no snescmd_buf).
//
// BASE $2BD0 -- NAO MOVER sem re-validar contra o mapa REAL da janela snescmd
// do fork (src/snes.h), que este decode SOMBREIA com prioridade no mux de
// SNES_DATA.  A base original da spec SS3.3 ($2A00) COLIDIA com o protocolo
// SNES<->MCU (SNESCMD_MCU_CMD $2A00 / SNES_CMD $2A02 / MCU_PARAM $2A04-$2A0F):
// o menu lia o NES_MAGIC 'NB' no lugar do ACK $55 -> handshake de boot travado
// dos dois lados (o wedge do primeiro teste de hardware do core; root cause
// cravada por log de fases).  Mapa real da janela ($2A00-$2BFF, src/snes.h):
//   $2A00-$2A0F  MCU_CMD/SNES_CMD/MCU_PARAM  (protocolo de boot -- INTOCAVEL)
//   $2A10-       SNESCMD_INGAME_HOOK   (codigo injetado)
//   $2A7D-       SNESCMD_RESET_HOOK    (codigo)
//   $2AD8-       SNESCMD_WRAM_CHEATS   (codigo, CRESCE -> base $2B00 rejeitada)
//   $2BA0-$2BAB  SNESCMD_NMI_* entry points
//   $2BB0/$2BB2  COMBO_VERSION / MAP
//   $2BB4-$2BEF  GAP nao-referenciado (documentado no snes.h; $2BE0 = SFX
//                mailbox, 1 byte)  <- o control block vive AQUI ($2BD0-$2BDF)
//   $2BFC/$2BFE  NMI_ENABLE_BUTTONS / NMI_DISABLE_WRAM
assign nesctl_enable = ({SNES_ADDR[22], SNES_ADDR[15:4]} == 13'b0_001010111101);

// CHR-SNES read windows (renderer DMA source, lockstep com os equates do
// renderer `snes/nes/nes_equates.i65`: NES_CHR_BG_PSRAM_BANK=$50,
// NES_CHR_OBJ_PSRAM_BANK=$60): banks $50-$5F -> PSRAM 0x500000+ (CHR BG
// 2bpp pre-convertida), $60-$6F -> 0x600000+ (CHR OBJ 4bpp).  Mapeamento
// IDENTIDADE (long addr SNES == endereco PSRAM linear); leitura servida pelo
// cliente SNES-CHR do arbiter Bus-1 em main.v (nunca pela Bus-2).  Sem
// colisao com nesbox/nesctl/snescmd/msu (todos exigem A22=0; estes bancos
// tem A22=1).  Read-only (IS_WRITABLE continua 0).
assign nes_chr_enable = (SNES_ADDR[23:20] == 4'h5) | (SNES_ADDR[23:20] == 4'h6);

endmodule
