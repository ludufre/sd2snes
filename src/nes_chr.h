/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   nes.c: carregador de .nes (iNES) -- Fase 0 do core NES, espelho do sgb.c.

   nes_chr.h: conversao de tile CHR NES -> tile CHR SNES (Fase 1c).

   CONTRATO CANONICO (byte-exato, nao renegociar sem atualizar TODOS os
   lados): utils/nes_chr_convert.py.  Este par .c/.h e' a implementacao C do
   MESMO contrato (nes_tile_to_snes2/nes_tile_to_snes4 do script Python) --
   o teste host (tests/host/run_nes_chr.sh) compila este arquivo REAL (nao
   uma copia) e compara byte-a-byte contra o script.  Ver NES-CORE-CONTRACT.md
   Sec. 10.5.

   Deliberadamente SEM dependencia de nenhum header do firmware (so
   <stdint.h>) -- e' o que permite compilar isto isoladamente no host sem
   nenhum shim de hardware (ao contrario de src/patch.c, que precisa de
   tests/host/shim/).  nes.c (que tem as dependencias de hardware/PSRAM)
   chama estas funcoes por tile durante o load; ver nes_convert_chr() la.

   Formatos (NES-BRIDGE-SPEC.md Sec. 2.4 e errata Sec. 13.2):
   - Tile NES 2bpp (16 B): bytes 0..7 = plano 0 (linhas 0..7), bytes 8..15 =
     plano 1.
   - Tile SNES 2bpp (16 B): por linha r, byte[2r] = plano 0, byte[2r+1] =
     plano 1 (intercalado).  Usado pelas BGs em Mode 0 (BG 2bpp -- 1:1 em
     tamanho com o NES).
   - Tile SNES 4bpp (32 B): bytes 0..15 = pares plano0/plano1 por linha
     (igual ao 2bpp), bytes 16..31 = pares plano2/plano3 por linha, SEMPRE
     ZERO (o NES so tem 2 planos).  Usado pelos OBJ (sprites SNES sao sempre
     4bpp -- 2x o tamanho do tile NES). */

#ifndef NES_CHR_H
#define NES_CHR_H

#include <stdint.h>

#define NES_CHR_TILE_BYTES        (16)  /* tamanho de 1 tile NES (entrada) */
#define NES_CHR_TILE_SNES2_BYTES  (16)  /* tamanho de 1 tile SNES 2bpp (BG) */
#define NES_CHR_TILE_SNES4_BYTES  (32)  /* tamanho de 1 tile SNES 4bpp (OBJ) */

/* tile[16] NES -> out[16] SNES 2bpp (intercala plano 0/1 por linha). */
void nes_chr_tile_to_snes2(const uint8_t tile[NES_CHR_TILE_BYTES],
                            uint8_t out[NES_CHR_TILE_SNES2_BYTES]);

/* tile[16] NES -> out[32] SNES 4bpp (planos 2/3 zerados). */
void nes_chr_tile_to_snes4(const uint8_t tile[NES_CHR_TILE_BYTES],
                            uint8_t out[NES_CHR_TILE_SNES4_BYTES]);

#endif
