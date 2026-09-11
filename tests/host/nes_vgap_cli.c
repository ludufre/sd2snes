/* nes_vgap_cli.c -- probes the REAL nhs_vgap_build with a synthetic $11 frame.
 *
 * Why this exists: the per-strip BG1VOFS (v3.1+) cannot be observed on the
 * device.  The renderer's own note says it -- "os mirrors de savestate nao
 * existem sob o core NES" -- and a USB read of the nes_dbg_* block through the
 * WRAM mirror comes back as the clear_wram $55 fill, so there is no readback
 * channel under the NES core.  Here the routine runs in the 65816 interpreter
 * (tests/host/m65816.c) against misc/nes_snes.bin, with addresses taken from
 * misc/nes_snes.map, so every number below comes out of the shipped assembly.
 *
 * The frame modelled is the Super Mario Bros. 1 status-bar split: strip 0 is
 * the HUD from scanline 0, strip 1 is the playfield from --split (default 32).
 * SMB1 does not scroll vertically in 1-1, so both strips publish sy=0; the
 * question the probe answers is what vertical origin the renderer then gives
 * the playfield strip.
 *
 * Build/run: see run_vgap_probe.sh next to this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m65816.h"

#define VIEWPORT_CROP_Y 8
#define NTARR_H 0
#define NTARR_V 1

struct sym { char name[64]; uint32_t addr; };
static struct sym syms[4096];
static int nsyms;

static void load_map(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "map: %s nao abre\n", path); exit(2); }
  char line[256];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    unsigned a; char n[64];
    if (sscanf(line, "%x %63s", &a, n) == 2) {
      if (nsyms < (int)(sizeof syms / sizeof syms[0])) {
        syms[nsyms].addr = a;
        snprintf(syms[nsyms].name, sizeof syms[nsyms].name, "%s", n);
        nsyms++;
      }
    }
  }
  fclose(f);
}

static uint32_t sym(const char *n) {
  for (int i = 0; i < nsyms; i++) if (!strcmp(syms[i].name, n)) return syms[i].addr;
  fprintf(stderr, "simbolo ausente no map: %s\n", n);
  exit(2);
}

int main(int argc, char **argv) {
  const char *bin = "misc/nes_snes.bin", *map = "misc/nes_snes.map";
  int split = 32, arr = NTARR_V, sy0 = 0, sy1 = 0, nt0 = 0, nt1 = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bin") && i + 1 < argc) bin = argv[++i];
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) map = argv[++i];
    else if (!strcmp(argv[i], "--split") && i + 1 < argc) split = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--sy0") && i + 1 < argc) sy0 = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--sy1") && i + 1 < argc) sy1 = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nt0") && i + 1 < argc) nt0 = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nt1") && i + 1 < argc) nt1 = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--arr") && i + 1 < argc) arr = atoi(argv[++i]);
  }

  FILE *f = fopen(bin, "rb");
  if (!f) { fprintf(stderr, "bin: %s nao abre\n", bin); return 2; }
  static uint8_t rom[1 << 20];
  size_t n = fread(rom, 1, sizeof rom, f);
  fclose(f);
  load_map(map);

  m_reset_memory();
  m_load_rom(rom, n);
  m_instr_budget = 20000000;
  m_call(sym("nes_boot_init"), 0);

  /* --- estado de entrada do $11: 2 faixas, HUD + playfield --- */
  uint32_t parse = 0x7F0400;             /* buffer livre p/ o payload */
  uint8_t pl[16];
  memset(pl, 0, sizeof pl);
  /* offset 2 em diante: [sl sx sy ntsel] por entrada */
  pl[2] = 0;    pl[3] = 0;   pl[4] = (uint8_t)sy0; pl[5] = (uint8_t)nt0;
  pl[6] = (uint8_t)split; pl[7] = 0; pl[8] = (uint8_t)sy1; pl[9] = (uint8_t)nt1;
  m_poke_block(parse, pl, sizeof pl);
  m_poke16(sym("nes_parse_ptr"), (uint16_t)(parse & 0xFFFF));
  m_poke(sym("nes_parse_ptr") + 2, (uint8_t)(parse >> 16));

  m_poke(sym("nes_ntarr"), (uint8_t)arr);
  m_poke(sym("nes_fb_target"), 1);
  m_poke16(sym("nes_sp_cnt"), 2);
  m_poke16(sym("nes_sp_bn"), 4);          /* 2 faixas x 2 bytes de indice */
  m_poke16(sym("nes_sp_snap") + 0, 0);
  m_poke16(sym("nes_sp_snap") + 2, (uint16_t)split);

  m_call(sym("nhs_vgap_build"), 0);

  printf("arranjo=%s  split=%d  sy=[%d,%d]  nt=[%d,%d]  CROP=%d\n",
         arr == NTARR_V ? "V" : "H", split, sy0, sy1, nt0, nt1, VIEWPORT_CROP_Y);
  printf("vadm (alguma faixa admitida?) = %d\n", m_peek(sym("nes_sp_vadm")));

  /* tabela HDMA do canal 4: blocos [count, lo, hi] ate' o terminador 0 */
  uint32_t tab = sym("nes_vgap_tab0");
  printf("nes_vgap_tab0 @ $%06X:\n", tab);
  int line = 0;
  for (int i = 0; i < 12; i++) {
    uint8_t cnt = m_peek(tab + i * 3);
    if (!cnt) { printf("  [%d] terminador\n", i); break; }
    uint16_t vofs = (uint16_t)(m_peek(tab + i * 3 + 1) | (m_peek(tab + i * 3 + 2) << 8));
    /* a linha logica exibida na primeira linha do bloco */
    printf("  [%d] linhas %3d..%3d  BG1VOFS=%5u  => linha logica no topo do bloco = %u\n",
           i, line, line + cnt - 1, vofs, (unsigned)((vofs + line) & 0x3FF));
    line += cnt;
  }
  return 0;
}
