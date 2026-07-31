/* nes_chr_stage_cli.c -- prova de host do caminho de CHR-RAM do renderer NES
 * (Fase 2.2-lite): SHADOW + SWIZZLE no stage + DMA EM BLOCO na NMI.
 *
 * O QUE ELE SIMULA.  Uma transcricao fiel do que snes/nes/nes_render.a65 faz
 * com cada CMD_CHR_RUN ($41):
 *   1. grava o payload cru no shadow da CHR-RAM (nes_chr_shadow);
 *   2. deriva a faixa de TILES tocada (t0 = off>>4 .. (off+len-1)>>4) e
 *      reconverte esses tiles INTEIROS a partir do shadow (nes_chrq_swz) pros
 *      arrays packed BG (16 B/tile) e OBJ (32 B/tile) -- os bytes 16..31 de
 *      cada slot de OBJ (planos 2/3) sao zerados UMA VEZ e nunca mais
 *      escritos, exatamente como no boot do renderer;
 *   3. faz os 2 DMAs em bloco (nes_chrq_dma_dsc) pro modelo de VRAM, com
 *      VMAIN=$80 e o par $2118/$2119 (modo 1), que e' o caminho normal de
 *      escrita de VRAM do renderer.
 * Depois despeja a VRAM no MESMO layout que utils/nes_chr_convert.py produz.
 * PASS do gate = `cmp` byte-exato contra o conversor de referencia.
 *
 * POR QUE ISTO E' A PROVA CERTA.  O que pode dar errado no renderer nao e'
 * "o formato do tile" isolado -- e' a composicao: shadow + expansao de um run
 * PARCIAL para tiles inteiros + empacotamento dos arrays + endereco de VRAM
 * derivado de t0.  Um erro em qualquer uma dessas pecas aparece aqui como
 * bytes diferentes.  A versao anterior deste CLI simulava o "plane-DMA" por
 * VMAIN (2 DMAs por tile/plano): o FORMATO passava, mas o custo era ~6x o que
 * cabe no vblank -- por isso o caminho mudou, e este arquivo mudou junto.
 *
 * O QUE ELE NAO E'.  Nao ha codigo C compartilhado com o firmware (o caminho
 * real e' assembly), entao esta e' uma TRANSCRICAO que precisa ser mantida em
 * lockstep -- os nomes dos rotulos do .a65 estao citados em cada bloco.  As
 * constantes de layout vem por -D do run_nes_chr.sh, que as extrai do
 * nes_equates.i65 (nao ha numero de layout duplicado aqui).
 *
 * Uso:
 *   nes_chr_stage_cli <in.chr> <out.bin> --bpp 2|4 [--split N]
 *                                                  [--split-rand SEED]
 *                                                  [--clamp-probe]
 *   --split N        parte o blob em runs de N bytes (1..255).  N que nao
 *                    divide 16 gera runs DESALINHADOS de proposito.
 *   --split-rand S   comprimentos pseudoaleatorios em 1..255 (semente S).
 *   --clamp-probe    injeta runs FORA da CHR-RAM (off >= teto) e runs que
 *                    ATRAVESSAM o teto, pra provar que o clamp do renderer
 *                    nao muda o resultado nem escreve fora.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Layout: vem por -D do run_nes_chr.sh (extraido do nes_equates.i65). */
#ifndef VRAM_BG_CHR_WORD
#error "defina VRAM_BG_CHR_WORD via -D (run_nes_chr.sh extrai do nes_equates.i65)"
#endif
#ifndef VRAM_OBJ_CHR_WORD
#error "defina VRAM_OBJ_CHR_WORD via -D"
#endif
#ifndef NES_CHR_RAM_BYTES
#error "defina NES_CHR_RAM_BYTES via -D"
#endif
#ifndef NES_CHR_TILES
#error "defina NES_CHR_TILES via -D"
#endif

#define VRAM_BYTES 65536

static uint8_t vram[VRAM_BYTES];
static uint8_t shadow[NES_CHR_RAM_BYTES];
/* arrays packed do buffer de stage (nes_chrq_bg0 / nes_chrq_ob0) */
static uint8_t sbg[NES_CHR_TILES * 16];
static uint8_t sob[NES_CHR_TILES * 32];

/* ---- modelo dos registradores de VRAM (book1 Appendix A) ---- */
static uint8_t  reg_vmain;   /* $2115 */
static uint16_t reg_vmadd;   /* $2116/$2117 */

static uint16_t vmain_step(void) {
  switch(reg_vmain & 0x03) {
    case 0:  return 1;
    case 1:  return 32;
    case 2:  return 64;
    default: return 128;
  }
}
static void wr2118(uint8_t v) {   /* VMDATAL: byte BAIXO da word */
  vram[((uint32_t)reg_vmadd * 2) & (VRAM_BYTES - 1)] = v;
  if(!(reg_vmain & 0x80)) reg_vmadd = (uint16_t)(reg_vmadd + vmain_step());
}
static void wr2119(uint8_t v) {   /* VMDATAH: byte ALTO da word */
  vram[((uint32_t)reg_vmadd * 2 + 1) & (VRAM_BYTES - 1)] = v;
  if(reg_vmain & 0x80) reg_vmadd = (uint16_t)(reg_vmadd + vmain_step());
}
/* GP-DMA modo 1 = 2 registradores ($2118 depois $2119), A-bus incrementando.
   E' o modo que nes_chrq_dma_init programa ($4300=$01, $4301=$18). */
static void dma_mode1(const uint8_t *src, uint32_t len) {
  uint32_t i;
  for(i = 0; i < len; i++) {
    if(i & 1) wr2119(src[i]); else wr2118(src[i]);
  }
}

/* ---- transcricao de nes_chrq_swz (nes_render.a65) ----
 * tile NES  = [plano0 linhas 0-7][plano1 linhas 0-7]
 * tile SNES = por linha r: byte BAIXO = plano0(r), byte ALTO = plano1(r)
 * Escreve o MESMO par de bytes no array BG (passo 16) e no array OBJ (passo
 * 32); os bytes 16..31 de cada slot OBJ nunca sao tocados. */
static void chrq_swz(uint16_t t0, uint16_t nt, uint16_t tofs) {
  uint16_t i, r;
  for(i = 0; i < nt; i++) {
    const uint8_t *src = shadow + (uint32_t)(t0 + i) * 16;
    uint8_t *bg = sbg + (uint32_t)(tofs + i) * 16;
    uint8_t *ob = sob + (uint32_t)(tofs + i) * 32;
    for(r = 0; r < 8; r++) {
      bg[r * 2 + 0] = src[r];       /* plano 0 -> byte BAIXO */
      ob[r * 2 + 0] = src[r];
      bg[r * 2 + 1] = src[8 + r];   /* plano 1 -> byte ALTO  */
      ob[r * 2 + 1] = src[8 + r];
    }
  }
}

/* ---- transcricao de nes_chrq_dma_dsc (nes_render.a65) ----
 * BG : word VRAM_BG_CHR_WORD  + t0*8,  16*nt B a partir de bg[tofs]
 * OBJ: word VRAM_OBJ_CHR_WORD + t0*16, 32*nt B a partir de ob[tofs]
 * Os dois destinos sao CONTIGUOS porque os arrays sao packed com o mesmo
 * passo do formato de tile do SNES -- e' o ponto todo do desenho. */
static void chrq_dma_dsc(uint16_t t0, uint16_t nt, uint16_t tofs) {
  reg_vmain = 0x80;
  reg_vmadd = (uint16_t)(VRAM_BG_CHR_WORD + (uint32_t)t0 * 8);
  dma_mode1(sbg + (uint32_t)tofs * 16, (uint32_t)nt * 16);
  reg_vmadd = (uint16_t)(VRAM_OBJ_CHR_WORD + (uint32_t)t0 * 16);
  dma_mode1(sob + (uint32_t)tofs * 32, (uint32_t)nt * 32);
}

/* ---- transcricao de nes_chr_handle_run + nes_chrq_stage ----
 * Um run e': clamp -> shadow -> faixa de tiles -> swizzle -> 2 DMAs.
 * (O tofs real depende do empacotamento da fila; para o resultado em VRAM ele
 *  e' irrelevante, entao aqui usa-se tofs = t0, que exercita o mesmo codigo
 *  com o mesmo alinhamento de passo.) */
static uint16_t tiles_touched;   /* espelha nes_chrq_tacc (banda) */
static uint32_t runs_clamped;

static void chr_handle_run(uint32_t off, uint32_t len, const uint8_t *data) {
  uint32_t cl = len, room;
  uint16_t t0, t1, nt;
  if(off >= (uint32_t)NES_CHR_RAM_BYTES) { runs_clamped++; return; }
  room = (uint32_t)NES_CHR_RAM_BYTES - off;
  if(cl > room) { cl = room; runs_clamped++; }
  if(!cl) return;
  memcpy(shadow + off, data, cl);
  t0 = (uint16_t)(off >> 4);
  t1 = (uint16_t)((off + cl - 1) >> 4);
  nt = (uint16_t)(t1 - t0 + 1);
  tiles_touched = (uint16_t)(tiles_touched + nt);
  chrq_swz(t0, nt, t0);
  chrq_dma_dsc(t0, nt, t0);
}

/* PRNG deterministico (xorshift32) -- nao usa rand() pra o gate ser
   reprodutivel em qualquer libc. */
static uint32_t xs32(uint32_t *s) {
  uint32_t x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (*s = x);
}

static void usage(void) {
  fprintf(stderr, "uso: nes_chr_stage_cli <in.chr> <out.bin> --bpp 2|4"
                  " [--split N] [--split-rand SEED] [--clamp-probe]\n");
}

int main(int argc, char **argv) {
  const char *inp = NULL, *outp = NULL;
  int bpp = 0, split = 0, rand_split = 0, clamp_probe = 0, i;
  uint32_t seed = 0;
  FILE *f;
  long fsz;
  uint8_t *blob;
  size_t size, tiles, pos, olen, ti, r;
  uint8_t *out;

  for(i = 1; i < argc; i++) {
    if(!strcmp(argv[i], "--bpp") && i + 1 < argc)             bpp = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--split") && i + 1 < argc)      split = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--split-rand") && i + 1 < argc) { rand_split = 1; seed = (uint32_t)strtoul(argv[++i], NULL, 0); }
    else if(!strcmp(argv[i], "--clamp-probe"))                clamp_probe = 1;
    else if(!inp)  inp = argv[i];
    else if(!outp) outp = argv[i];
    else { usage(); return 2; }
  }
  if(!inp || !outp || (bpp != 2 && bpp != 4)) { usage(); return 2; }
  if(split < 0 || split > 255) { fprintf(stderr, "--split fora de 1..255\n"); return 2; }
  if(!seed) seed = 0xC0FFEEu;

  f = fopen(inp, "rb");
  if(!f) { perror(inp); return 1; }
  fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
  if(fsz <= 0 || (fsz & 15)) { fprintf(stderr, "CHR de %ld B nao e' multiplo de 16\n", fsz); fclose(f); return 1; }
  if(fsz > NES_CHR_RAM_BYTES) {
    fprintf(stderr, "CHR de %ld B > %d (teto da CHR-RAM); use um blob truncado\n",
            fsz, NES_CHR_RAM_BYTES);
    fclose(f); return 1;
  }
  size = (size_t)fsz;
  blob = (uint8_t*)malloc(size);
  if(!blob || fread(blob, 1, size, f) != size) { fprintf(stderr, "read %s\n", inp); fclose(f); return 1; }
  fclose(f);
  tiles = size / 16;

  /* VRAM, shadow e arrays de stage nascem ZERADOS -- nes_boot_init zera os
     64KB de VRAM, e nes_boot_zero_chrq zera o shadow e os arrays OBJ.  E' o
     que faz os planos 2/3 do OBJ (bytes 16..31 de cada slot) ficarem zero sem
     ninguem escreve-los: o dump abaixo os LE, nao os assume. */
  memset(vram, 0, sizeof(vram));
  memset(shadow, 0, sizeof(shadow));
  memset(sbg, 0, sizeof(sbg));
  memset(sob, 0, sizeof(sob));
  reg_vmain = 0x80;

  /* Runs fora da CHR-RAM ANTES do conteudo real: o clamp tem que descarta-los
     por inteiro, sem tocar shadow/VRAM (senao o cmp final acusa). */
  if(clamp_probe) {
    static uint8_t junk[255];
    for(i = 0; i < (int)sizeof(junk); i++) junk[i] = (uint8_t)(0xA5 ^ i);
    chr_handle_run((uint32_t)NES_CHR_RAM_BYTES, 255, junk);
    chr_handle_run((uint32_t)NES_CHR_RAM_BYTES + 0x100, 16, junk);
    chr_handle_run(0xFFFF, 255, junk);
  }

  /* Parte o blob em runs (o RTL fatia em <=255 B) e aplica na ordem. */
  pos = 0;
  while(pos < size) {
    size_t n;
    if(rand_split)   n = (size_t)(xs32(&seed) % 255u) + 1u;
    else if(split)   n = (size_t)split;
    else             n = 255;
    if(n > size - pos) n = size - pos;
    chr_handle_run((uint32_t)pos, (uint32_t)n, blob + pos);
    pos += n;
  }

  /* Run que ATRAVESSA o teto da CHR-RAM: a parte de dentro tem que ser
     aplicada e a de fora truncada.  Alimenta os bytes que ja' estao la',
     entao o resultado nao pode mudar. */
  if(clamp_probe && size >= 32) {
    uint32_t off = (uint32_t)NES_CHR_RAM_BYTES - 16;
    static uint8_t tail[255];
    memset(tail, 0, sizeof(tail));
    memcpy(tail, shadow + off, 16);
    chr_handle_run(off, 255, tail);
  }

  /* Despeja no MESMO layout do utils/nes_chr_convert.py:
       bpp 2 -> 16 B/tile: por linha r, [plano0, plano1]  (regiao BG)
       bpp 4 -> 32 B/tile: os 16 acima + 16 B de planos 2/3 (regiao OBJ) */
  olen = tiles * (size_t)(bpp == 2 ? 16 : 32);
  out = (uint8_t*)calloc(1, olen);
  if(!out) { fprintf(stderr, "oom\n"); return 1; }
  for(ti = 0; ti < tiles; ti++) {
    if(bpp == 2) {
      for(r = 0; r < 16; r++)
        out[ti * 16 + r] = vram[(uint32_t)VRAM_BG_CHR_WORD * 2 + ti * 16 + r];
    } else {
      for(r = 0; r < 32; r++)
        out[ti * 32 + r] = vram[(uint32_t)VRAM_OBJ_CHR_WORD * 2 + ti * 32 + r];
    }
  }

  f = fopen(outp, "wb");
  if(!f) { perror(outp); return 1; }
  if(fwrite(out, 1, olen, f) != olen) { fprintf(stderr, "write %s\n", outp); fclose(f); return 1; }
  fclose(f);
  free(out); free(blob);
  return 0;
}
