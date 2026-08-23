/* nes_render_cli.c -- runs the CHR-RAM path of the REAL renderer.
 *
 * Loads the bytes of misc/nes_snes.bin into a 65816 interpreter
 * (tests/host/m65816.c) and calls the real routines -- nes_boot_init,
 * nes_chr_handle_run ($41), nes_chrq_publish, nes_chrq_service -- against a
 * VRAM/DMA model.  Nothing here reimplements the swizzle, tile range,
 * descriptor, merge, clamp or VRAM address: all of it comes out of the
 * assembly, and the dump at the end is compared against the contract,
 * utils/nes_chr_convert.py.  Addresses are NOT copied either: they come from
 * misc/nes_snes.map (snes/nes/gen_map.py), which carries the size + CRC32 of
 * the .bin, and a desynchronized pair is refused.
 *
 * The three $41 paths, chosen by nes_chr_handle_run from the frame's ppumask:
 *   --path stage    rendering ON: incremental stage in MAIN, descriptor drain
 *                   in the NMI.  The normal path.
 *   --path fast     rendering OFF: MAIN only marks the dirty range and the
 *                   conversion becomes a sliced forced-blank pass
 *                   (nes_chrfd_service).
 *   --rebuild-at N  arms nes_chrq_full after run N: the queue is dropped and
 *                   the whole CHR-RAM is rebuilt from the shadow
 *                   (nes_chrq_rebuild), the fail-safe that never fires in
 *                   normal operation.
 * All three must leave the SAME VRAM, equal to the contract.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "m65816.h"

/* Layout: comes by -D from run_nes_chr.sh (extracted from nes_equates.i65). */
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

/* Transport mailbox (nes_mailbox_buf, 8KB): where the $41 TLV is dropped for
 * the renderer to read through [nes_parse_ptr],y.  The address comes from the
 * MAP, like every other one. */
static uint32_t mailbox;

/* ---------------- symbol table (misc/nes_snes.map) ---------------- */
typedef struct { char name[64]; uint32_t addr; } sym_t;
static sym_t *syms; static size_t nsyms;
static uint32_t map_rom_bytes, map_rom_crc;

static void die(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr); exit(1);
}

static void load_map(const char *path) {
  char line[256];
  FILE *f = fopen(path, "r");
  size_t cap = 64;
  if(!f) die("cannot open map %s", path);
  syms = (sym_t*)malloc(cap * sizeof(sym_t));
  while(fgets(line, sizeof(line), f)) {
    unsigned long v; char nm[64];
    if(line[0] == '#') {
      if(sscanf(line, "# rom_bytes %lu", &v) == 1) map_rom_bytes = (uint32_t)v;
      else if(sscanf(line, "# rom_crc32 %lX", &v) == 1) map_rom_crc = (uint32_t)v;
      continue;
    }
    if(sscanf(line, "%lX %63s", &v, nm) != 2) continue;
    if(nsyms == cap) { cap *= 2; syms = (sym_t*)realloc(syms, cap * sizeof(sym_t)); }
    snprintf(syms[nsyms].name, sizeof(syms[nsyms].name), "%s", nm);
    syms[nsyms].addr = (uint32_t)v;
    nsyms++;
  }
  fclose(f);
  if(!nsyms) die("%s: mapa vazio", path);
}

static uint32_t sym(const char *name) {
  size_t i;
  for(i = 0; i < nsyms; i++) if(!strcmp(syms[i].name, name)) return syms[i].addr;
  die("symbol '%s' missing from the map -- renamed/removed in the renderer?", name);
  return 0;
}

/* CRC32 (the zlib one gen_map.py uses) */
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
  static uint32_t tab[256]; static int init;
  uint32_t c = 0xffffffffu; size_t i;
  if(!init) { uint32_t k, j; for(k = 0; k < 256; k++) { uint32_t r = k;
      for(j = 0; j < 8; j++) r = (r & 1) ? (0xedb88320u ^ (r >> 1)) : (r >> 1); tab[k] = r; }
    init = 1; }
  for(i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

/* ---------------- deterministic PRNG (same as stage_cli) ---------------- */
static uint32_t xs32(uint32_t *s) {
  uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (*s = x);
}

/* ---------------- driver ---------------- */
static uint32_t a_handle_run, a_publish, a_service, a_boot_init;
static uint32_t d_ppumask, d_parse_ptr, d_parse_bank;
static uint32_t d_chrq_wr, d_chrq_rdy, d_chrq_full, d_chrfd_mode, d_chrq_drop;
static uint32_t d_chrq_tmax, d_chrq_rb_t, d_chrfd_pass, d_chrfd_tacc;
static int verbose;
/* Evidence that the requested path REALLY ran: a --path fast run that falls
 * into the normal path leaves the same final bytes, so the lost coverage
 * would go unnoticed. */
static int saw_rb_t;

static void do_run(uint32_t off, uint32_t len, const uint8_t *data) {
  uint8_t tlv[4];
  tlv[0] = 0x41;
  tlv[1] = (uint8_t)off;
  tlv[2] = (uint8_t)(off >> 8);
  tlv[3] = (uint8_t)len;
  m_poke_block(mailbox, tlv, 4);
  m_poke_block(mailbox + 4, data, len);
  m_poke16(d_parse_ptr, (uint16_t)mailbox);
  m_poke(d_parse_bank, (uint8_t)(mailbox >> 16));
  m_call(a_handle_run, 0);
}

/* One vblank: publish (frame close, MAIN context) + service (NMI). */
/* V walk, one value per vblank (--v-walk).  Half the list lets the NMI drain
 * nothing: 224 is active display, and 258/261 read as $02/$05 in the low byte
 * of $213D -- the renderer's "V>=256 reads as 0-4 and DEFERS everything".
 * 255 lands exactly on the gate limit (V + cost == NES_CHRQ_V_LIMIT).  With V
 * fixed the gate never defers.
 *
 * The upper limit of the V gate only decides IN WHICH vblank the drain
 * happens, never what gets written, and this program drains until the renderer
 * is idle -- so the DEFERRAL is all that stays observable, and that is what
 * --v-walk demands having seen. */
static const uint16_t v_walk[] = { 255, 258, 225, 224, 250, 261 };
static int v_walk_on, v_walk_i, saw_defer;

static void do_vblank(void) {
  uint8_t rdy_before;
  if(v_walk_on) {
    m_vcounter = v_walk[v_walk_i % (int)(sizeof(v_walk) / sizeof(v_walk[0]))];
    v_walk_i++;
  }
  m_call(a_publish, 0);
  rdy_before = m_peek(d_chrq_rdy);
  m_call(a_service, 0);
  /* A published buffer still published = the NMI DEFERRED (cursor intact). */
  if(rdy_before && m_peek(d_chrq_rdy)) saw_defer = 1;
  /* rb_t != 0 = full rebuild IN PROGRESS: the cursor persisted across NMIs is
     the observable evidence that it is sliced. */
  if(m_peek16(d_chrq_rb_t)) saw_rb_t = 1;
}

static void usage(void) {
  fprintf(stderr,
    "uso: nes_render_cli <in.chr> <out.bin> --rom <nes_snes.bin> --map <nes_snes.map>\n"
    "     --bpp 2|4 [--split N] [--split-rand SEED] [--clamp-probe]\n"
    "     [--path stage|fast] [--rebuild-at N] [--rpf N] [--v V | --v-walk]\n"
    "     [--expect-drop] [--selftest] [--verbose]\n");
}

int main(int argc, char **argv) {
  const char *inp = NULL, *outp = NULL, *rompath = NULL, *mappath = NULL;
  const char *path = "stage";
  int bpp = 0, split = 0, rand_split = 0, clamp_probe = 0, i;
  int expect_drop = 0, selftest = 0, split_given = 0;
  long rebuild_at = -1, rpf = 8, vline = 225;
  uint32_t seed = 0;
  FILE *f; long fsz; uint8_t *blob, *romb;
  size_t size, tiles, pos, olen, ti, r, romsz;
  uint8_t *out;
  int nrun = 0, in_frame = 0, iter, rebuild_armed = 0;
  static uint8_t vram_after_boot[0x10000];

  for(i = 1; i < argc; i++) {
    if(!strcmp(argv[i], "--bpp") && i + 1 < argc)             bpp = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--split") && i + 1 < argc)      { split = atoi(argv[++i]); split_given = 1; }
    else if(!strcmp(argv[i], "--split-rand") && i + 1 < argc) { rand_split = 1; seed = (uint32_t)strtoul(argv[++i], NULL, 0); }
    else if(!strcmp(argv[i], "--clamp-probe"))                clamp_probe = 1;
    else if(!strcmp(argv[i], "--rom") && i + 1 < argc)        rompath = argv[++i];
    else if(!strcmp(argv[i], "--map") && i + 1 < argc)        mappath = argv[++i];
    else if(!strcmp(argv[i], "--path") && i + 1 < argc)       path = argv[++i];
    else if(!strcmp(argv[i], "--rebuild-at") && i + 1 < argc) rebuild_at = strtol(argv[++i], NULL, 0);
    else if(!strcmp(argv[i], "--rpf") && i + 1 < argc)        rpf = strtol(argv[++i], NULL, 0);
    else if(!strcmp(argv[i], "--v") && i + 1 < argc)          vline = strtol(argv[++i], NULL, 0);
    else if(!strcmp(argv[i], "--v-walk"))                     v_walk_on = 1;
    else if(!strcmp(argv[i], "--expect-drop"))                expect_drop = 1;
    else if(!strcmp(argv[i], "--selftest"))                   selftest = 1;
    else if(!strcmp(argv[i], "--verbose"))                    verbose = 1;
    else if(!inp)  inp = argv[i];
    else if(!outp) outp = argv[i];
    else { usage(); return 2; }
  }
  if(selftest) {
    int nf = m_selftest();
    if(nf) die("m65816: %d micro-teste(s) do modelo falharam", nf);
    printf("m65816 selftest: ok\n");
    return 0;
  }
  if(!inp || !outp || !rompath || !mappath || (bpp != 2 && bpp != 4)) { usage(); return 2; }
  if(split_given && (split < 1 || split > 255))
    die("--split fora de 1..255 (omita a opcao para o default de 255)");
  if(strcmp(path, "stage") && strcmp(path, "fast")) die("--path tem de ser stage ou fast");
  /* 225 = first NTSC vblank scanline.  Below it the renderer's V gate defers
     forever; above 255 the value does not fit the byte $213D returns.  Both
     extremes are usage errors, not tests. */
  if(vline < 225 || vline > 255) die("--v fora de 225..255");
  if(!seed) seed = 0xC0FFEEu;

  /* --- ROM + map, with the pair checked --- */
  f = fopen(rompath, "rb");
  if(!f) die("cannot open ROM %s", rompath);
  fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
  if(fsz <= 0) die("%s vazio", rompath);
  romsz = (size_t)fsz;
  romb = (uint8_t*)malloc(romsz);
  if(!romb || fread(romb, 1, romsz, f) != romsz) die("leitura de %s", rompath);
  fclose(f);
  load_map(mappath);
  if(map_rom_bytes != romsz)
    die("%s descreve uma ROM de %u B, mas %s tem %zu B -- par dessincronizado "
        "(refaca `make -C snes/nes`)", mappath, map_rom_bytes, rompath, romsz);
  if(map_rom_crc != crc32_buf(romb, romsz))
    die("%s carimba CRC32 %08X, mas %s tem %08X -- par dessincronizado "
        "(refaca `make -C snes/nes`)", mappath, map_rom_crc, rompath, crc32_buf(romb, romsz));

  /* --- payload --- */
  f = fopen(inp, "rb");
  if(!f) die("cannot open %s", inp);
  fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
  if(fsz <= 0 || (fsz & 15)) die("CHR of %ld B is not a multiple of 16", fsz);
  if(fsz > NES_CHR_RAM_BYTES) die("CHR de %ld B > %d (teto da CHR-RAM)", fsz, NES_CHR_RAM_BYTES);
  size = (size_t)fsz;
  blob = (uint8_t*)malloc(size);
  if(!blob || fread(blob, 1, size, f) != size) die("leitura de %s", inp);
  fclose(f);
  tiles = size / 16;

  /* --- REAL renderer boot --- */
  m_reset_memory();
  m_load_rom(romb, romsz);
  m_vcounter = (uint16_t)vline;

  a_boot_init  = sym("nes_boot_init");
  a_handle_run = sym("nes_chr_handle_run");
  a_publish    = sym("nes_chrq_publish");
  a_service    = sym("nes_chrq_service");
  d_ppumask    = sym("nes_regs_ppumask");
  d_parse_ptr  = sym("nes_parse_ptr");
  d_parse_bank = sym("nes_parse_bank");
  d_chrq_wr    = sym("nes_chrq_wr");
  d_chrq_rdy   = sym("nes_chrq_rdy");
  d_chrq_full  = sym("nes_chrq_full");
  d_chrfd_mode = sym("nes_chrfd_mode");
  d_chrq_drop  = sym("nes_chrq_drop");
  d_chrq_tmax  = sym("nes_chrq_tmax");
  d_chrq_rb_t  = sym("nes_chrq_rb_t");
  d_chrfd_pass = sym("nes_chrfd_pass");
  d_chrfd_tacc = sym("nes_chrfd_tacc");
  mailbox      = sym("nes_mailbox_buf");

  m_call(a_boot_init, 0);
  /* VRAM snapshot right after boot: the baseline of the OVERFLOW check at the
     end.  The CHR-RAM path may only touch the two CHR regions, and a DMA with
     the wrong length or address would not show up in the `cmp`, which only
     looks at the payload tiles.  The baseline comes from the CODE (the boot
     clears VRAM by DMA), which is why VRAM starts poisoned with $A5. */
  memcpy(vram_after_boot, m_vram, sizeof(vram_after_boot));

  /* ppumask picks the $41 dispatcher path (bit3 BG, bit4 OBJ). */
  m_poke(d_ppumask, (uint8_t)(!strcmp(path, "stage") ? 0x18 : 0x00));

  /* Runs OUTSIDE the CHR-RAM before the real content: the clamp has to drop
     them whole, touching neither shadow nor VRAM. */
  if(clamp_probe) {
    uint8_t junk[255];
    for(i = 0; i < (int)sizeof(junk); i++) junk[i] = (uint8_t)(0xA5 ^ i);
    do_run((uint32_t)NES_CHR_RAM_BYTES, 255, junk);
    do_run((uint32_t)NES_CHR_RAM_BYTES + 0x100, 16, junk);
    do_run(0xFFFF, 255, junk);
  }

  /* --- run stream, grouped into frames --- */
  pos = 0;
  while(pos < size) {
    size_t n;
    if(rand_split)   n = (size_t)(xs32(&seed) % 255u) + 1u;
    else if(split)   n = (size_t)split;
    else             n = 255;
    if(n > size - pos) n = size - pos;
    do_run((uint32_t)pos, (uint32_t)n, blob + pos);
    pos += n;
    nrun++;
    if(rebuild_at >= 0 && nrun >= rebuild_at && !rebuild_armed) { m_poke(d_chrq_full, 1); rebuild_armed = 1; }
    if(rpf > 0 && ++in_frame >= rpf) { do_vblank(); in_frame = 0; }
  }

  /* Payload too short to reach --rebuild-at N: arm it anyway at the end of
     the stream.  The full rebuild does not depend on the payload size. */
  if(rebuild_at >= 0 && !rebuild_armed) { m_poke(d_chrq_full, 1); rebuild_armed = 1; }

  /* Run that CROSSES the CHR-RAM ceiling: the inside is applied, the outside
     truncated.  It feeds back bytes already there, so nothing may change. */
  if(clamp_probe && size >= 32) {
    uint32_t off = (uint32_t)NES_CHR_RAM_BYTES - 16;
    uint8_t tail[255];
    memset(tail, 0, sizeof(tail));
    for(i = 0; i < 16; i++) tail[i] = m_peek(sym("nes_chr_shadow") + off + i);
    do_run(off, 255, tail);
  }

  /* --- empty vblanks until the renderer goes idle: the queue publishes and
     drains, the sliced rebuild converges, the fast-dump pass finishes.  The
     ceiling is deliberately high but FINITE -- a path that does not converge
     has to blow up here instead of running forever. --- */
  for(iter = 0; iter < 20000; iter++) {
    do_vblank();
    if(!m_peek(d_chrq_wr) && !m_peek(d_chrq_rdy) &&
       !m_peek(d_chrq_full) && !m_peek(d_chrfd_mode)) break;
  }
  if(iter >= 20000)
    die("renderer did not go idle within 20000 vblanks "
        "(wr=%u rdy=%u full=%u fd_mode=%u)",
        m_peek(d_chrq_wr), m_peek(d_chrq_rdy), m_peek(d_chrq_full), m_peek(d_chrfd_mode));

  /* --- did the requested path really run? --- */
  if(!strcmp(path, "fast")) {
    if(!m_peek16(d_chrfd_pass) || !m_peek16(d_chrfd_tacc))
      die("--path fast: o passe de despejo nunca rodou (pass=%u tacc=%u) -- "
          "o gate do dispatcher mudou e o teste perdeu a cobertura",
          m_peek16(d_chrfd_pass), m_peek16(d_chrfd_tacc));
  } else if(!m_peek16(d_chrq_tmax) && !saw_rb_t) {
    die("--path stage: MAIN converted no tile (tmax=0) and no rebuild ran "
        "-- the $41 dispatcher never took the stage path");
  }
  if(rebuild_at >= 0 && !saw_rb_t)
    die("--rebuild-at %ld: nes_chrq_rb_t nunca foi visto != 0 -- o rebuild "
        "sliced service never ran (or is no longer sliced)", rebuild_at);

  /* --- THE FAIL-SAFE MUST NOT BE WHAT MAKES THE TEST PASS. ---
   * Overflowing the queue (nes_chrq_drop) escalates to the full rebuild, which
   * reconstructs the whole CHR-RAM from the shadow and hands back correct VRAM
   * -- so a bug in the normal drain can stay INVISIBLE inside a `cmp`.  Every
   * regime that did NOT ask for an overflow/rebuild must therefore end with
   * drop == 0 and no rebuild observed. */
  {
    uint16_t drop = m_peek16(d_chrq_drop);
    if(expect_drop) {
      if(!drop) die("--expect-drop: nes_chrq_drop == 0 -- the queue never overflowed, "
                    "the regime lost the coverage it claimed");
      if(!saw_rb_t) die("--expect-drop: the queue overflowed (drop=%u) but the rebuild "
                        "total nunca rodou", drop);
    } else {
      if(drop) die("nes_chrq_drop = %u: the queue OVERFLOWED in a regime that should "
                   "dreiar normalmente -- a VRAM so' fechou porque o rebuild "
                   "total a reconstruiu", drop);
      if(saw_rb_t && rebuild_at < 0)
        die("nes_chrq_rb_t != 0: a full rebuild ran in a regime that never asked for one");
    }
  }
  /* --- OVERFLOW: nothing outside the two CHR regions may have changed. --- */
  {
    uint32_t bg0 = (uint32_t)VRAM_BG_CHR_WORD * 2,  bgn = (uint32_t)NES_CHR_TILES * 16;
    uint32_t ob0 = (uint32_t)VRAM_OBJ_CHR_WORD * 2, obn = (uint32_t)NES_CHR_TILES * 32;
    uint32_t k;
    for(k = 0; k < 0x10000; k++) {
      if((k >= bg0 && k < bg0 + bgn) || (k >= ob0 && k < ob0 + obn)) continue;
      if(m_vram[k] != vram_after_boot[k])
        die("VRAM $%04X mudou de $%02X para $%02X FORA das regioes de CHR "
            "(BG $%04X+$%04X, OBJ $%04X+$%04X) -- um DMA transbordou",
            k, vram_after_boot[k], m_vram[k], bg0, bgn, ob0, obn);
    }
  }

  if(v_walk_on && !saw_defer)
    die("--v-walk: a NMI nunca adiou um buffer publicado -- o gate de V deixou "
        "deferring and the regime no longer covers anything");

  if(verbose)
    fprintf(stderr, "  [%s] runs=%d vblanks=%d drop=%u tmax=%u fdpass=%u rb=%d defer=%d instr=%llu\n",
            path, nrun, iter + 1, m_peek16(d_chrq_drop), m_peek16(d_chrq_tmax),
            m_peek16(d_chrfd_pass), saw_rb_t, saw_defer, (unsigned long long)m_cpu.instrs);

  /* --- dump in the SAME layout utils/nes_chr_convert.py produces --- */
  olen = tiles * (size_t)(bpp == 2 ? 16 : 32);
  out = (uint8_t*)calloc(1, olen);
  if(!out) die("oom");
  for(ti = 0; ti < tiles; ti++) {
    if(bpp == 2) for(r = 0; r < 16; r++) out[ti * 16 + r] = m_vram[(uint32_t)VRAM_BG_CHR_WORD * 2 + ti * 16 + r];
    else         for(r = 0; r < 32; r++) out[ti * 32 + r] = m_vram[(uint32_t)VRAM_OBJ_CHR_WORD * 2 + ti * 32 + r];
  }

  f = fopen(outp, "wb");
  if(!f) die("cannot open %s for writing", outp);
  if(fwrite(out, 1, olen, f) != olen) die("escrita em %s", outp);
  fclose(f);
  free(out); free(blob); free(romb); free(syms);
  return 0;
}
