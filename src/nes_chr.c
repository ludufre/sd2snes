/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   nes_chr.c: conversao de tile CHR NES -> tile CHR SNES (Fase 1c).
   Ver nes_chr.h para o contrato completo. */

#include "nes_chr.h"

void nes_chr_tile_to_snes2(const uint8_t tile[NES_CHR_TILE_BYTES],
                            uint8_t out[NES_CHR_TILE_SNES2_BYTES]) {
  for (uint8_t r = 0; r < 8; r++) {
    out[2 * r]     = tile[r];       /* plano 0, linha r */
    out[2 * r + 1] = tile[8 + r];   /* plano 1, linha r */
  }
}

void nes_chr_tile_to_snes4(const uint8_t tile[NES_CHR_TILE_BYTES],
                            uint8_t out[NES_CHR_TILE_SNES4_BYTES]) {
  nes_chr_tile_to_snes2(tile, out);
  for (uint8_t i = 16; i < NES_CHR_TILE_SNES4_BYTES; i++) out[i] = 0x00;
}
