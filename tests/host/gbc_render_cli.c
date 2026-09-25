/* gbc_render_cli.c -- runs the REAL Game Boy Color player under a 65816.
 *
 * Loads the bytes of misc/gbc_snes_harness.bin into the interpreter
 * (tests/host/m65816.c) and EXECUTES it: the ROM's own Reset (register
 * bring-up, VRAM/CGRAM/OAM clear, HDMA tables, the wire-version handshake,
 * GO) and then GbcFrameOnce once per frame, against a set of views served
 * from a GBVW file.  What comes out is the VRAM/CGRAM/OAM the player's own
 * DMAs wrote, and the gate compares that -- byte for byte -- against the
 * IDEAL player, gbc/tests/viewsim/snes_model.py.
 *
 * Nothing here reimplements the transfer engine.  The dirty-bit fold, the
 * carry-over backlog, the round-robin, the coalescing, the budget arithmetic
 * and the V guard all come out of the assembly; this file only serves the
 * views, rewrites the dirty bitmaps between frames (the bridge's job on
 * silicon -- the player clears only what it copied) and reads the result.
 *
 * NO ADDRESS IS COPIED IN HERE.  Routine addresses come from
 * misc/gbc_snes_harness.map (snes/gbc/gen_map_asar.py), which carries the
 * size + CRC32 of the .bin, and a desynchronised pair is refused.  The
 * player's WRAM working set comes out of the GbcHarnessMap blob the harness
 * build emits into the image.  The six transfer classes -- source bank and
 * address, block count, block size, VRAM destination -- are read out of
 * GbcClsTab in the image itself, which is also what defines the VRAM regions
 * the frames are ALLOWED to write.  The view addresses are the one table
 * stated here, and every one of them is cross-checked against the binary
 * (six against GbcClsTab, three against the DMA the player actually fires).
 *
 * The budget constants (!GBC_BUDGET, !GBC_DMACOST) arrive by -D, extracted
 * from the .asm by run_gbc_player.sh -- same discipline as run_nes_chr.sh
 * with nes_equates.i65.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "m65816.h"
#include "gbc_c6.h"

/* Layout / budget constants, by -D from run_gbc_player.sh (out of the .asm). */
#ifndef GBC_BUDGET
#error "defina GBC_BUDGET via -D (run_gbc_player.sh extrai do gbc_snes.asm)"
#endif
#ifndef GBC_DMACOST
#error "defina GBC_DMACOST via -D"
#endif
#ifndef GBC_VER
#error "defina GBC_VER via -D"
#endif
/* Status-block field offsets, contract sec. 5 (also !GBC_ST_* in the .asm). */
#ifndef GBC_ST_VER
#error "defina GBC_ST_VER via -D"
#endif
#ifndef GBC_ST_FLAGS0
#error "defina GBC_ST_FLAGS0 via -D"
#endif
#ifndef GBC_ST_SEQ
#error "defina GBC_ST_SEQ via -D"
#endif
#ifndef GBC_ST_DCHR
#error "defina GBC_ST_DCHR via -D"
#endif
#ifndef GBC_ST_DOBJ
#error "defina GBC_ST_DOBJ via -D"
#endif
#ifndef GBC_ST_DMAP
#error "defina GBC_ST_DMAP via -D"
#endif
#ifndef GBC_ST_DMISC
#error "defina GBC_ST_DMISC via -D"
#endif
#ifndef GBC_ST_SCX
#error "defina GBC_ST_SCX via -D"
#endif
#ifndef GBC_ST_LOGN
#error "defina GBC_ST_LOGN via -D"
#endif
/* Raster-compiler layout and the HDMA budget model (sec. 11.1/11.4). */
#ifndef GBC_SNAP_LEN
#error "defina GBC_SNAP_LEN via -D"
#endif
#ifndef GBC_LOG_MAX
#error "defina GBC_LOG_MAX via -D"
#endif
#ifndef GBC_BPLHDMA
#error "defina GBC_BPLHDMA via -D"
#endif
#ifndef GBC_DEADLINE
#error "defina GBC_DEADLINE via -D"
#endif
/* Diagnostic counter offsets inside the block GbcHarnessMap points at. */
#ifndef GBC_CTR_FRAMES
#error "defina GBC_CTR_FRAMES via -D"
#endif
#ifndef GBC_CTR_DMAS
#error "defina GBC_CTR_DMAS via -D"
#endif
#ifndef GBC_CTR_BYTES
#error "defina GBC_CTR_BYTES via -D"
#endif
#ifndef GBC_CTR_DEFER
#error "defina GBC_CTR_DEFER via -D"
#endif
#ifndef GBC_CTR_DROPS
#error "defina GBC_CTR_DROPS via -D"
#endif
#ifndef GBC_CTR_READY
#error "defina GBC_CTR_READY via -D"
#endif
#ifndef GBC_CTR_REGSLATE
#error "defina GBC_CTR_REGSLATE via -D"
#endif
/* $EF write-window offsets, contract sec. 3 (also !GBC_EFO_* in the .asm). */
#ifndef GBC_EFO_COMMIT
#error "defina GBC_EFO_COMMIT via -D"
#endif
#ifndef GBC_EFO_GO
#error "defina GBC_EFO_GO via -D"
#endif
#ifndef GBC_EFO_SYNC
#error "defina GBC_EFO_SYNC via -D"
#endif
#ifndef GBC_EFO_CONSUME
#error "defina GBC_EFO_CONSUME via -D"
#endif

/* Transfer classes: GbcClsTab's own length, passed in from !GBC_CLS_N so that
 * splitting or merging a class moves this file with the player. */
#ifndef GBC_CLS_N
#error "defina GBC_CLS_N via -D"
#endif
#define NCLS GBC_CLS_N
/* Offsets of the HDMA table slots inside a set (!GBC_T_*): the guard bands of
   --require-wram-quiet are derived from them, never written down here. */
#ifndef GBC_T_W1
#error "defina GBC_T_W1 via -D"
#endif
#ifndef GBC_T_CH0
#error "defina GBC_T_CH0 via -D"
#endif
#ifndef GBC_T_SLOT
#error "defina GBC_T_SLOT via -D"
#endif
/* The counter mailbox (contract sec. 3/10): !GBC_MBX_N words at $EF0000 +
   !GBC_EFO_MBX, lo then hi, holding FRAMES DEFER DROPS READY REGSLATE for the
   MCU.  The CLI plays the bridge's half of it (mbx_hook below). */
#ifndef GBC_EFO_MBX
#error "defina GBC_EFO_MBX via -D"
#endif
#ifndef GBC_MBX_N
#error "defina GBC_MBX_N via -D"
#endif
/* The $EF write window, as m_set_ef_sink declares it: the six strobes at
   +00..+05 and the mailbox at +10..+19. */
#define EF_LEN 0x20
#define CLSTAB_STRIDE 16  /* "16 bytes each so the index is a shift" */

static int verbose;

static void die(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr); exit(1);
}

/* ---------------- symbol table (misc/gbc_snes_harness.map) ---------------- */
typedef struct { char name[64]; uint32_t addr; } sym_t;
static sym_t  *syms; static size_t nsyms;
static uint32_t map_rom_bytes, map_rom_crc;

static void load_map(const char *path) {
  char line[256];
  FILE *f = fopen(path, "r");
  size_t cap = 64;
  if(!f) die("nao consigo abrir o mapa %s", path);
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
  die("simbolo '%s' ausente do mapa -- renomeado/removido no player?", name);
  return 0;
}

/* CRC32 (the zlib one gen_map_asar.py uses) */
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
  static uint32_t tab[256]; static int init;
  uint32_t c = 0xffffffffu; size_t i;
  if(!init) { uint32_t k, j; for(k = 0; k < 256; k++) { uint32_t r = k;
      for(j = 0; j < 8; j++) r = (r & 1) ? (0xedb88320u ^ (r >> 1)) : (r >> 1); tab[k] = r; }
    init = 1; }
  for(i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

/* ---------------- the ROM image, and reads into it ---------------- */
static uint8_t  rom[0x8000];
static size_t   rom_len;

/* A LoROM address inside bank $00 -> its offset in the image. */
static uint32_t rom_off(uint32_t addr24) {
  if((addr24 >> 16) != 0 || (uint16_t)addr24 < 0x8000)
    die("$%06X nao esta' na pagina LoROM do player", addr24);
  return (uint16_t)addr24 - 0x8000;
}
static uint8_t  rom8(uint32_t a)  { return rom[rom_off(a)]; }
static uint16_t rom16(uint32_t a) { return (uint16_t)(rom8(a) | (rom8(a + 1) << 8)); }

/* ---------------- views (GBVW) ---------------- */
/* The ONE table this file states, and every entry is checked against the
 * binary below (six against GbcClsTab, three against the DMAs the player
 * fires).  Harness layout: the !GBC_HARNESS block at the top of gbc_snes.asm
 * packs the $E0-$E3 view banks into the single bank $7F. */
typedef struct {
  const char *tag;
  uint32_t    addr;      /* where the harness build expects it */
  uint32_t    len;
  int         required;
} viewsec_t;

static viewsec_t VIEWS[] = {
  { "BGCH", 0x7f0000, 12288, 1 },
  { "OBCH", 0x7f4000, 16384, 1 },
  { "MC0_", 0x7f8000,  2048, 1 },
  { "MC1_", 0x7f8800,  2048, 1 },
  { "MK0_", 0x7f9000,  2048, 1 },
  { "MK1_", 0x7f9800,  2048, 1 },
  { "CGRM", 0x7fa000,   512, 1 },
  { "STAT", 0x7fa200,   256, 1 },
  { "OAMV", 0x7fb000,   544, 1 },
  { "LOG_", 0x7fa400,  2048, 0 },   /* phase 4; the player v1 never reads it */
};
#define NVIEWS ((int)(sizeof(VIEWS) / sizeof(VIEWS[0])))

static const viewsec_t *view_by_tag(const char *tag) {
  int i;
  for(i = 0; i < NVIEWS; i++) if(!strcmp(VIEWS[i].tag, tag)) return &VIEWS[i];
  return NULL;
}

static uint32_t stat_addr;   /* = view_by_tag("STAT")->addr, for readability */

/* ⚡ WIRE $03 IS ADDITIVE OVER $02 (contract sec. 14.1).  A status block a $02
 * bridge published has $00 in +58..+91, and +58.b7 = 0 is exactly "a $03
 * bridge without the C6" -- which the $03 player runs as the $02 player it
 * was.  So every view file the phase-2..5 goldens wrote under $02 is served
 * stamped with the version of the IMAGE under test (--image-ver, default the
 * .asm's), and nothing else in it changes: the tables and the VRAM those
 * cases demand are the same bytes, which is the whole point of keeping them.
 * The other direction (an older $02 image against a $03-stamped file) is the
 * same rule.  A block that carries C6 bytes is never re-stamped. */
static int image_ver = GBC_VER;
static void stamp_ver(void) {
  uint8_t v = m_peek(stat_addr + GBC_ST_VER);
  int k, c6 = 0;
  for(k = 0x58; k < 0x92; k++) if(m_peek(stat_addr + (uint32_t)k)) c6 = 1;
  if(!c6 && (v == 2 || v == 3) && v != image_ver && (image_ver == 2 || image_ver == 3))
    m_poke(stat_addr + GBC_ST_VER, (uint8_t)image_ver);
}

static void load_views(const char *path) {
  uint8_t hdr[8];
  FILE *f = fopen(path, "rb");
  int seen[NVIEWS], i;
  if(!f) die("nao consigo abrir as views %s", path);
  memset(seen, 0, sizeof(seen));
  if(fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "GBVW", 4))
    die("%s nao e' um dump GBVW", path);
  for(;;) {
    uint8_t sh[8]; char tag[5]; uint32_t len; const viewsec_t *vs;
    uint8_t *buf; size_t got;
    if(fread(sh, 1, 8, f) != 8) break;
    memcpy(tag, sh, 4); tag[4] = 0;
    len = (uint32_t)(sh[4] | (sh[5] << 8) | ((uint32_t)sh[6] << 16) | ((uint32_t)sh[7] << 24));
    vs = view_by_tag(tag);
    if(!vs) die("%s: secao '%s' desconhecida", path, tag);
    if(vs->required && len != vs->len)
      die("%s: secao %s tem %u B, o contrato diz %u", path, tag, len, vs->len);
    buf = (uint8_t*)malloc(len ? len : 1);
    got = fread(buf, 1, len, f);
    if(got != len) die("%s: secao %s truncada (%zu de %u B)", path, tag, got, len);
    m_poke_block(vs->addr, buf, len);
    free(buf);
    for(i = 0; i < NVIEWS; i++) if(&VIEWS[i] == vs) seen[i] = 1;
  }
  fclose(f);
  for(i = 0; i < NVIEWS; i++)
    if(VIEWS[i].required && !seen[i])
      die("%s: falta a secao %s -- o player le' essa view todo frame", path, VIEWS[i].tag);
}

/* ---------------- --views-at: a scene change in the middle of the run ------
 * "N:file;N:file": the bridge publishes ANOTHER scene from Game Boy frame N on
 * (1-based).  A single view file is one (snapshot, log) pair forever, and a
 * player bug that only shows on the TRANSITION between two scenes -- a raster
 * frame followed by a quiet one -- cannot be reached without it.  The running
 * SEQ survives the swap (a file carries its own), so the new scene is a fresh
 * snapshot, never a skipped one.  Served by BOTH drivers: the frame loop (one
 * GB frame per player frame) and --clock (at the LY=0 that publishes GB frame
 * N; a LY=0 skipped by the open read window swaps at the next one). */
static int   va_n, va_f[1024];
static char *va_p[1024];
static void views_at_parse(const char *arg) {
  char *b = strdup(arg), *p2 = b, *t;
  while((t = strsep(&p2, ";")) != NULL) {
    char *c = strchr(t, ':');
    if(!*t) continue;
    if(!c || va_n >= 1024) die("--views-at: N:arquivo[;N:arquivo] (no maximo 1024)");
    *c = 0;
    va_f[va_n] = atoi(t);
    va_p[va_n++] = c + 1;
  }
}
static void views_at_apply(int frame) {  /* every swap due by GB frame `frame` */
  static int done;
  while(done < va_n && va_f[done] <= frame) {
    uint8_t s0 = m_peek(stat_addr + GBC_ST_SEQ), s1 = m_peek(stat_addr + GBC_ST_SEQ + 1);
    load_views(va_p[done++]);
    stamp_ver();
    m_poke(stat_addr + GBC_ST_SEQ, s0);
    m_poke(stat_addr + GBC_ST_SEQ + 1, s1);
  }
}
static int g_cgram_view;                /* --require-cgram-view, for ck_line */

/* ---------------- the two HDMA sets, as the prologue would publish them ----
 * Both sets decoded line by line out of WRAM ($7E + the set offsets of the
 * .asm): the header's $420C mask, every armed channel's B-bus target and DMAP,
 * and each table run-length-expanded into one value per line.  Two uses:
 *   * --require-sets-equal: once nothing is owed, the HDMA alternates between
 *     the two sets every frame, so ANY difference between them is a picture
 *     that flickers between two frames' rasters on the console;
 *   * --require-no-colour: no armed channel of either set may write CGRAM
 *     ($2121) -- the property a log with no palette write owes.  */
#define SET_MAXLINES 260
typedef struct {
  uint8_t mask, bbad[6], dmap[6];
  int     nl[6];                        /* lines decoded, -1 = broken table */
  uint8_t v[6][SET_MAXLINES][4];
} hset_t;

static int hset_table(uint32_t at, int bpl, uint8_t (*v)[4]) {
  int n = 0, k, b;
  for(;;) {
    uint8_t c = m_peek(at++);
    if(!c) return n;
    if(c & 0x80) {
      for(k = 0; k < (c & 0x7f); k++) {
        if(n >= SET_MAXLINES) return -1;
        for(b = 0; b < bpl; b++) v[n][b] = m_peek(at + (uint32_t)b);
        n++; at += (uint32_t)bpl;
      }
    } else {
      for(k = 0; k < c; k++) {
        if(n >= SET_MAXLINES) return -1;
        for(b = 0; b < bpl; b++) v[n][b] = m_peek(at + (uint32_t)b);
        n++;
      }
      at += (uint32_t)bpl;
    }
  }
}

static void hset_decode(uint32_t base, hset_t *h) {
  int ch;
  memset(h, 0, sizeof(*h));
  h->mask = m_peek(base + GBC_H_MASK);
  for(ch = 0; ch < 4; ch++) {
    uint32_t e = base + GBC_H_CH + (uint32_t)ch * 4;
    if(!((h->mask >> ch) & 1)) continue;
    h->bbad[ch] = m_peek(e);
    h->dmap[ch] = m_peek(e + 3);
    h->nl[ch] = hset_table(base + (uint32_t)(m_peek(e + 1) | (m_peek(e + 2) << 8)),
                           4, h->v[ch]);
  }
  h->bbad[4] = 0x26; h->nl[4] = hset_table(base + GBC_T_W1, 2, h->v[4]);
  h->bbad[5] = 0x00; h->nl[5] = hset_table(base + GBC_T_INIDISP, 1, h->v[5]);
}

/* 0 = the two sets drive the same registers with the same values on every
 * line; otherwise prints the first difference and returns 1. */
static int hsets_differ(void) {
  static hset_t a, b;
  int ch, l;
  hset_decode(0x7e0000 + GBC_SETA_OFF, &a);
  hset_decode(0x7e0000 + GBC_SETB_OFF, &b);
  if(a.mask != b.mask) {
    fprintf(stderr, "FAIL conjuntos: $420C do conjunto A = $%02X, do B = $%02X -- "
            "o console alterna entre os dois a cada frame\n", a.mask, b.mask);
    return 1;
  }
  for(ch = 0; ch < 6; ch++) {
    if(ch < 4 && !((a.mask >> ch) & 1)) continue;
    if(a.bbad[ch] != b.bbad[ch] || a.dmap[ch] != b.dmap[ch] || a.nl[ch] != b.nl[ch]) {
      fprintf(stderr, "FAIL conjuntos: ch%d A = {$21%02X, modo %u, %d linhas}, "
              "B = {$21%02X, modo %u, %d linhas}\n", ch, a.bbad[ch], a.dmap[ch],
              a.nl[ch], b.bbad[ch], b.dmap[ch], b.nl[ch]);
      return 1;
    }
    for(l = 0; l < a.nl[ch]; l++)
      if(memcmp(a.v[ch][l], b.v[ch][l], 4)) {
        fprintf(stderr, "FAIL conjuntos: ch%d ($21%02X) difere na linha %d: "
                "A = %02X%02X%02X%02X, B = %02X%02X%02X%02X -- o console alterna "
                "entre os dois rasters a cada frame\n", ch, a.bbad[ch], l + 1,
                a.v[ch][l][0], a.v[ch][l][1], a.v[ch][l][2], a.v[ch][l][3],
                b.v[ch][l][0], b.v[ch][l][1], b.v[ch][l][2], b.v[ch][l][3]);
        return 1;
      }
  }
  return 0;
}

static int hsets_colour(void) {
  static hset_t h;
  int s, ch, bad = 0;
  for(s = 0; s < 2; s++) {
    hset_decode(0x7e0000 + (s ? GBC_SETB_OFF : GBC_SETA_OFF), &h);
    for(ch = 0; ch < 4; ch++)
      if(((h.mask >> ch) & 1) && h.bbad[ch] == 0x21) {
        fprintf(stderr, "FAIL cor: o conjunto %c arma o ch%d sobre $2121 (CGRAM) "
                "sem nenhuma escrita de paleta no log\n", s ? 'B' : 'A', ch);
        bad++;
      }
  }
  return bad;
}

/* ---------------- the colour channels, played into CGRAM -----------------
 * --require-cgram-view: after every frame the ARMED colour channels (B-bus
 * $2121, DMAP mode 3: $2121 twice, $2122 twice) are played into CGRAM line by
 * line, 0..223, the way the HDMA does it during the display, and at the end
 * the CGRAM the display was left with has to equal the CGRAM view.  The two
 * set checks above are about the TABLES; this is about the PICTURE, and it
 * needs a model of its own for one reason: on the SNES a CGRAM entry keeps the
 * last value written to it.  A stale colour channel that fires on every
 * OTHER frame, or one that fires only on two entries, leaves those entries
 * wrong for good, because the player only re-sends CGRAM when the snapshot's
 * CRAM changes -- a static screen never repairs it (Harvest Moon 3: every
 * sprite white and the text box lettering yellow-green on the farm).  Only
 * the channels that write CGRAM are modelled; everything else the HDMA drives
 * leaves CGRAM alone. */
static void cgv_hdma_frame(void) {
  const uint8_t *dr = m_dma_regs();
  uint8_t mask = m_cpu_regs()[0x0C], *cg = m_cgram();
  uint32_t ptr[8];
  int cnt[8], rep[8], xfer[8], live[8], ch, line;
  for(ch = 0; ch < 8; ch++) {
    uint8_t n;
    live[ch] = ((mask >> ch) & 1) && dr[ch * 16 + 1] == 0x21 && (dr[ch * 16] & 7) == 3;
    if(!live[ch]) continue;
    ptr[ch] = ((uint32_t)dr[ch * 16 + 4] << 16) | ((uint32_t)dr[ch * 16 + 3] << 8) | dr[ch * 16 + 2];
    n = m_peek(ptr[ch]++);
    if(!n) { live[ch] = 0; continue; }
    rep[ch] = n & 0x80; cnt[ch] = (n & 0x7f) ? (n & 0x7f) : 128; xfer[ch] = 1;
  }
  for(line = 0; line < 224; line++)
    for(ch = 0; ch < 8; ch++) {
      if(!live[ch]) continue;
      if(xfer[ch]) {                    /* {$2121, $2121, $2122 lo, $2122 hi} */
        unsigned a = m_peek(ptr[ch] + 1);
        cg[a * 2] = m_peek(ptr[ch] + 2);
        cg[a * 2 + 1] = (uint8_t)(m_peek(ptr[ch] + 3) & 0x7f);
        ptr[ch] += 4;
      }
      xfer[ch] = rep[ch];
      if(--cnt[ch] == 0) {
        uint8_t n = m_peek(ptr[ch]++);
        if(!n) { live[ch] = 0; continue; }
        rep[ch] = n & 0x80; cnt[ch] = (n & 0x7f) ? (n & 0x7f) : 128; xfer[ch] = 1;
      }
    }
}

static int cgv_verdict(void) {
  uint32_t at = view_by_tag("CGRM")->addr;
  const uint8_t *cg = m_cgram();
  int c, bad = 0, first = -1, reg[5] = { 0, 0, 0, 0, 0 };
  for(c = 0; c < 256; c++)
    if(cg[c * 2] != m_peek(at + (uint32_t)c * 2) || cg[c * 2 + 1] != m_peek(at + (uint32_t)c * 2 + 1)) {
      if(first < 0) first = c;
      bad++; reg[c < 128 ? c >> 5 : 4]++;
    }
  if(!bad) return 0;
  fprintf(stderr, "FAIL cgram: %d das 256 entradas que a tela mostra diferem da view "
          "(BG1 %d, BG2 %d, BG3 %d, BG4 %d, OBJ %d); a primeira, [%d] = $%04X, a view "
          "diz $%04X -- um canal de cor velho reescreveu a CGRAM e nada a reenvia\n",
          bad, reg[0], reg[1], reg[2], reg[3], reg[4], first,
          cg[first * 2] | (cg[first * 2 + 1] << 8),
          m_peek(at + (uint32_t)first * 2) | (m_peek(at + (uint32_t)first * 2 + 1) << 8));
  return 1;
}

/* ---------------- the six transfer classes, read out of GbcClsTab -------- */
/* Descriptor layout, from the comment above GbcClsTab:
 *   +$0 backlog DP base   +$2 cursor DP addr   +$4 block count
 *   +$5 bridgeable clean  +$6 block size       +$8 source 16-bit base
 *   +$A source bank       +$C VRAM word base   +$E src shift  +$F VRAM shift */
typedef struct {
  uint16_t backlog_dp, cursor_dp, blksz, src16, vram_word;
  uint8_t  nblk, bridge, src_bank, sh_src, sh_vram;
  uint32_t src24, vram_lo, vram_hi;   /* derived */
  const char *tag;                    /* the view it reads */
} cls_t;
static cls_t cls[NCLS];
static uint32_t backlog_base, backlog_end, backlog_misc; /* direct page */
static uint32_t dp_blmisc;      /* GbcHarnessMap: the OAM/CRAM backlog byte */

/* The view each class reads, WHERE INSIDE IT its slice starts, and which of the
 * bridge's dirty bitmaps names its blocks (contract sec. 4 / the !GBC_CLS_*
 * block).  Only this is stated; every address, length and shift below is
 * compared against GbcClsTab.
 *
 * Wire $02 turned the single BG chr class into six, because the 12 KB view now
 * goes to TWO 512-tile chr bases and the $8800 block of each bank belongs to
 * both: a class has to be contiguous in the view AND in VRAM, and these are
 * the six pieces that are.  The two DUPLICATE ones read the same view bytes as
 * a base-A class and carry their own backlog byte. */
enum { DSRC_CHR, DSRC_OBJ, DSRC_MAP };
static const struct {
  const char *tag; uint32_t voff; uint8_t dsrc, dbit0;
} CLS_INFO[NCLS] = {
  { "BGCH", 0x0000, DSRC_CHR,  0 },  /* base A tiles   0..255 -> VRAM $4000 */
  { "BGCH", 0x1800, DSRC_CHR, 24 },  /* base A tiles 256..511 -> VRAM $5000 */
  { "BGCH", 0x1000, DSRC_CHR, 16 },  /* base B tiles   0..127 -> VRAM $6000 */
  { "BGCH", 0x0800, DSRC_CHR,  8 },  /* base B tiles 128..255, the duplicate */
  { "BGCH", 0x2800, DSRC_CHR, 40 },  /* base B tiles 256..383 -> VRAM $7000 */
  { "BGCH", 0x2000, DSRC_CHR, 32 },  /* base B tiles 384..511, the duplicate */
  { "OBCH", 0x0000, DSRC_OBJ,  0 },
  { "MC0_", 0x0000, DSRC_MAP,  0 },  /* content of $9800 -> VRAM $8000 */
  { "MC1_", 0x0000, DSRC_MAP, 32 },  /* content of $9C00 -> VRAM $8800 */
  { "MK0_", 0x0000, DSRC_MAP,  0 },  /* carpet of $9800, the SAME 32 bits */
  { "MK1_", 0x0000, DSRC_MAP, 32 },  /* carpet of $9C00 */
};

/* How many bytes a view section covers together with the sections that follow
 * it without a gap.  The map classes span two GBVW sections ($9800's rows then
 * $9C00's), which is exactly what lets one class carry all 64 rows. */
static uint32_t view_span(const viewsec_t *vs) {
  uint32_t end = vs->addr + vs->len;
  int i, moved = 1;
  while(moved) {
    moved = 0;
    for(i = 0; i < NVIEWS; i++)
      if(VIEWS[i].addr == end) { end += VIEWS[i].len; moved = 1; break; }
  }
  return end - vs->addr;
}

static void load_classes(void) {
  uint32_t tab = sym("GbcClsTab");
  int i;
  uint32_t misc = 0;
  for(i = 0; i < NCLS; i++) {
    uint32_t e = tab + i * CLSTAB_STRIDE;
    const viewsec_t *vs;
    cls[i].backlog_dp = rom16(e + 0x00);
    cls[i].cursor_dp  = rom16(e + 0x02);
    cls[i].nblk       = rom8 (e + 0x04);
    cls[i].bridge     = rom8 (e + 0x05);
    cls[i].blksz      = rom16(e + 0x06);
    cls[i].src16      = rom16(e + 0x08);
    cls[i].src_bank   = rom8 (e + 0x0a);
    cls[i].vram_word  = rom16(e + 0x0c);
    cls[i].sh_src     = rom8 (e + 0x0e);
    cls[i].sh_vram    = rom8 (e + 0x0f);
    cls[i].src24      = ((uint32_t)cls[i].src_bank << 16) | cls[i].src16;
    cls[i].vram_lo    = (uint32_t)cls[i].vram_word * 2;
    cls[i].vram_hi    = cls[i].vram_lo + (uint32_t)cls[i].nblk * cls[i].blksz;
    cls[i].tag        = CLS_INFO[i].tag;

    /* Cross-check the stated view table against the binary: source address,
       and that the class's slice really fits inside the view it names. */
    vs = view_by_tag(CLS_INFO[i].tag);
    if(!vs) die("interno: tag de classe %d desconhecida", i);
    if(cls[i].src24 != vs->addr + CLS_INFO[i].voff)
      die("classe %d (%s+$%04X): GbcClsTab le' $%06X, a tabela de views diz "
          "$%06X\n  o layout do harness mudou no .asm -- ajuste VIEWS[] ou "
          "CLS_INFO[] em gbc_render_cli.c",
          i, CLS_INFO[i].tag, CLS_INFO[i].voff, cls[i].src24,
          vs->addr + CLS_INFO[i].voff);
    if(CLS_INFO[i].voff + (uint32_t)cls[i].nblk * cls[i].blksz > view_span(vs))
      die("classe %d (%s+$%04X): %u blocos x %u B passam do fim da view (%u B)",
          i, CLS_INFO[i].tag, CLS_INFO[i].voff, cls[i].nblk, cls[i].blksz,
          view_span(vs));
    if((1u << cls[i].sh_src) != cls[i].blksz)
      die("classe %d: shift de fonte %u nao casa com bloco de %u B",
          i, cls[i].sh_src, cls[i].blksz);
    if((1u << cls[i].sh_vram) * 2 != cls[i].blksz)
      die("classe %d: shift de VRAM %u nao casa com bloco de %u B",
          i, cls[i].sh_vram, cls[i].blksz);

    /* Where the backlog lives, derived and not copied.  ⚡ wire $02 stopped
       the bitmaps from being one contiguous run -- the two duplicate chr
       classes sit PAST the OAM/CRAM byte -- so the misc byte is read out of
       GbcHarnessMap instead of being guessed as "the one after the last". */
    if(i == 0 || cls[i].backlog_dp < backlog_base) backlog_base = cls[i].backlog_dp;
    { uint32_t end = cls[i].backlog_dp + ((cls[i].nblk + 7u) / 8u);
      if(end > misc) misc = end; }
  }
  backlog_end = misc;
  backlog_misc = dp_blmisc;
  if(backlog_misc < backlog_base || backlog_misc >= backlog_end)
    die("GbcHarnessMap: o byte OAM/CRAM ($%02X) nao esta' entre os bitmaps "
        "($%02X..$%02X)", backlog_misc, backlog_base, backlog_end);
}

/* Is anything at all pending?  ORs every class's bitmap plus the OAM/CRAM
   byte -- the ranges, not the span, because they are no longer contiguous. */
static uint8_t backlog_or(void) {
  uint8_t v = m_peek(0x7e0000 + backlog_misc);
  int i; uint32_t k;
  for(i = 0; i < NCLS; i++)
    for(k = 0; k < (cls[i].nblk + 7u) / 8u; k++)
      v |= m_peek(0x7e0000 + cls[i].backlog_dp + k);
  return v;
}

/* ---------------- the player's WRAM working set (GbcHarnessMap) ---------- */
/* The harness build emits "GBHM" + five 16-bit offsets into bank $7E.  Reading
 * them out of the image is what keeps this file free of WRAM addresses. */
static uint32_t a_statcopy, a_ctr, a_seta, a_setb, a_ef;
static uint32_t a_rctr, a_log, o_hmask, o_hch, a_snap, a_cram, a_dphdma;
static uint32_t hdma_set_len;
static uint32_t a_fb;          /* wire $03: the FB mode's state block */
static uint8_t  hmap_raw[26];   /* the thirteen words of GbcHarnessMap, LE */

static void load_harness_map(void) {
  uint32_t h = sym("GbcHarnessMap");
  if(rom8(h) != 'G' || rom8(h + 1) != 'B' || rom8(h + 2) != 'H' || rom8(h + 3) != 'M')
    die("GbcHarnessMap sem o magic 'GBHM' -- este .bin foi montado sem "
        "-DGBC_HARNESS=1 (use 'make -C snes/gbc harness')");
  a_statcopy = 0x7e0000 | rom16(h + 4);
  a_ctr      = 0x7e0000 | rom16(h + 6);
  a_seta     = 0x7e0000 | rom16(h + 8);
  a_setb     = 0x7e0000 | rom16(h + 10);
  a_ef       = 0x7e0000 | rom16(h + 12);
  /* Appended after the five the phase-2 harness knew; a reader that stops at
     the fifth still finds them where they always were. */
  a_rctr     = 0x7e0000 | rom16(h + 14);
  a_log      = 0x7e0000 | rom16(h + 16);
  o_hmask    = rom16(h + 18);
  o_hch      = rom16(h + 20);
  a_snap     = 0x7e0000 | rom16(h + 22);
  a_cram     = 0x7e0000 | rom16(h + 24);
  a_dphdma   = 0x7e0000 | rom16(h + 26);   /* direct page: $70-$73 */
  dp_blmisc  = rom16(h + 28);              /* direct page: the OAM/CRAM byte */
  a_fb       = 0x7e0000 | rom16(h + 30);   /* wire $03: GbcFb state (!GBC_FB_OFF) */
  { int k; for(k = 0; k < 13; k++) { uint16_t w = rom16(h + 4 + k * 2);
      hmap_raw[k * 2] = (uint8_t)w; hmap_raw[k * 2 + 1] = (uint8_t)(w >> 8); } }
  if(a_setb <= a_seta) die("GbcHarnessMap: set B ($%06X) nao vem depois do set A ($%06X)",
                           a_setb, a_seta);
  hdma_set_len = a_setb - a_seta;
}

/* ---------------- the $EF write window: strobe accounting ---------------- */
/* Contract sec. 13.5: SYNC, COMMIT and CONSUMED appear EXACTLY ONCE per frame,
 * COMMIT before any view read.  The window is watched rather than inferred --
 * a second COMMIT is a bridge-level bug the picture would not show. */
static unsigned ef_n[8], ef_total;
static int      ef_commit_seen_this_frame, ef_read_before_commit;
/* sec. 6/7 order the three strobes as SYNC -> COMMIT -> (all view reads) ->
 * CONSUMED.  "exactly once each" was already checked per frame; the ORDER was
 * not, and a COMMIT that beat its SYNC would move the genlock's phase
 * reference by a frame without changing a single pixel. */
static int      ef_sync_seen_this_frame, ef_consume_seen_this_frame;
static int      ef_commit_before_sync, ef_read_after_consume;
static uint8_t  mcu_cmd_last;
static unsigned c6_n[2], c6_outside;   /* $EF0006/7 stores; outside COMMIT..CONSUMED */

/* --clock (see ck_run below) takes over the per-frame bookkeeping: its frames
   are not the driver's GbcFrameOnce calls, and with the V-IRQ COMMIT comes
   BEFORE SYNC by design (window B opens at V=185, the NMI is at 225). */
static int  ck_mode;
static int  ck_running;
static void ck_ef(uint32_t off);
static void ck_dma(uint8_t bbad, uint32_t src24, uint32_t bytes, uint32_t dest);

/* ---------------- the counter mailbox, as gbc_bridge.v keeps it ----------
 * Same rule as the RTL: a low byte waits tagged with its word, and the word
 * only changes on the HIGH byte of the same word right after it; any other
 * high byte drops the pending low one.  The player publishes right AFTER
 * CONSUMED (the one strobe that closes every frame, whichever handler ran the
 * tail), so the check is armed at CONSUMED and made at the NEXT strobe: the
 * next frame's SYNC (NMI) or COMMIT (V-IRQ half), both of which come before
 * any counter of that frame moves.  What was published must then be exactly
 * the counters WRAM holds -- a word it forgot, a counter that moved after the
 * publish, or a pair stored out of order all show up as a mismatch.  A check
 * still armed when the run ends is dropped: a clocked run may stop between
 * CONSUMED and the publish. */
static uint16_t mbx_word[GBC_MBX_N];
static uint8_t  mbx_lo; static int mbx_lo_idx = -1;
static unsigned mbx_commits, mbx_checks, mbx_bad, mbx_stray;
static int      mbx_armed;
static char     mbx_first_bad[256];
/* ⚡ wire $03: word 5 is C6MODE = {fb_state, ROW_DONEs of the frame}, two
   bytes of the FB block, not a counter -- see mbx_value(). */
static const uint32_t mbx_src[GBC_MBX_N] = {
  GBC_CTR_FRAMES, GBC_CTR_DEFER, GBC_CTR_DROPS, GBC_CTR_READY, GBC_CTR_REGSLATE, 0 };
static const char *const mbx_name[GBC_MBX_N] = {
  "FRAMES", "DEFER", "DROPS", "READY", "REGSLATE", "C6MODE" };
static uint16_t mbx_value(int i) {
  if(i == 5) return (uint16_t)(m_peek(a_fb + 0) | (m_peek(a_fb + 4) << 8));
  return m_peek16(a_ctr + mbx_src[i]);
}

static void mbx_hook(uint32_t off, uint8_t v) {
  uint32_t k = off - GBC_EFO_MBX, idx = k >> 1;
  if(off < GBC_EFO_MBX || k >= 2u * GBC_MBX_N) { mbx_stray++; return; }
  if(!(k & 1)) { mbx_lo = v; mbx_lo_idx = (int)idx; return; }
  if(mbx_lo_idx == (int)idx) {
    mbx_word[idx] = (uint16_t)(mbx_lo | (v << 8));
    mbx_commits++;
  }
  mbx_lo_idx = -1;
}

static void mbx_check(void) {
  int i;
  mbx_armed = 0;
  mbx_checks++;
  for(i = 0; i < GBC_MBX_N && (i < 5 || image_ver >= 3); i++) {
    uint16_t w = mbx_value(i);
    if(w != mbx_word[i]) {
      if(!mbx_bad)
        snprintf(mbx_first_bad, sizeof mbx_first_bad,
                 "no frame n.%u: mailbox %s = $%04X, WRAM = $%04X",
                 mbx_checks, mbx_name[i], mbx_word[i], w);
      mbx_bad++;
    }
  }
}

/* Every run that closed at least one frame has to have published, and every
   publication has to agree with WRAM at the CONSUMED that follows it. */
static int mbx_verdict(void) {
  int fail = 0;
  /* A pinned run (GbcFrameOnce) ends on a whole frame, publish included, so
     its last armed check is made here; a clocked one may have stopped in the
     middle of the tail, so it only counts toward "never published". */
  if(mbx_armed) {
    if(!ck_mode) mbx_check();
    else if(!mbx_checks && !mbx_commits) mbx_checks = 1;
  }
  if(mbx_checks && !mbx_commits) {
    fprintf(stderr, "FAIL mailbox: %u frame(s) fechado(s) e nenhuma palavra "
            "publicada em $EF%04X (o player nao publica os contadores)\n",
            mbx_checks, GBC_EFO_MBX);
    fail++;
  } else if(mbx_bad) {
    fprintf(stderr, "FAIL mailbox: %u palavra(s) divergente(s) da WRAM em %u "
            "frame(s); a primeira %s\n", mbx_bad, mbx_checks, mbx_first_bad);
    fail++;
  }
  if(mbx_stray) {
    fprintf(stderr, "FAIL mailbox: %u escrita(s) em $EF fora dos strobes e da "
            "mailbox\n", mbx_stray);
    fail++;
  }
  if(getenv("GBC_MBX_REPORT"))
    printf("mailbox: %u palavra(s) publicada(s), %u frame(s) checado(s), %u "
           "divergencia(s); ultimo FRAMES=$%04X DEFER=$%04X DROPS=$%04X "
           "READY=$%04X REGSLATE=$%04X\n", mbx_commits, mbx_checks, mbx_bad,
           mbx_word[0], mbx_word[1], mbx_word[2], mbx_word[3], mbx_word[4]);
  return fail;
}

static void ef_hook(uint32_t off, uint8_t v) {
  if(off >= 8) { mbx_hook(off, v); return; }   /* data, not a strobe */
  /* ⚡ wire $03: $EF0006 C6_CTL / $EF0007 ROW_DONE belong to the C6 half of
     the bridge; they are not frame strobes (sec. 14.2) */
  if(off == GBC_EFO_C6CTL || off == GBC_EFO_ROWDONE) {
    c6_n[off - GBC_EFO_C6CTL]++;
    if(!ef_commit_seen_this_frame || ef_consume_seen_this_frame) c6_outside++;
    if(c6_enabled()) c6_ef(off, v);
    return;
  }
  if(mbx_armed) mbx_check();                   /* the previous frame's publish */
  ef_n[off]++;
  ef_total++;
  if(off == GBC_EFO_SYNC) ef_sync_seen_this_frame = 1;
  if(off == GBC_EFO_COMMIT) {
    if(!ef_sync_seen_this_frame && !ck_mode) ef_commit_before_sync = 1;
    ef_commit_seen_this_frame = 1;
  }
  if(off == GBC_EFO_CONSUME) {
    ef_consume_seen_this_frame = 1;
    mbx_armed = 1;
  }
  if(ck_mode) ck_ef(off);
}

static void cmd_hook(uint8_t v) { mcu_cmd_last = v; }

/* ---------------- DMA accounting ---------------- */
typedef struct { int ch, frame; uint8_t bbad; uint32_t src, bytes, dest; } dmarec_t;
static int cur_frame = 0;      /* 0 = the boot; 1.. = the frame loop */
/* Room for the whole run.  --interleave multiplies the DMA count by the number
   of injected bodies (each one fetches its own status block), and the record is
   what the per-frame and per-kind counts are read from -- running out of it is
   a failure by design, not a silent truncation. */
#define MAXDMA 32768
static dmarec_t dmas[MAXDMA];
static int      ndma;
static int      ndma_boot;   /* DMAs the BOOT fired: the 64 KB VRAM clear and
                                the two solid tiles.  Real cost, but not part
                                of any frame's budget -- counting them would
                                put ~66000 eq into the per-frame average. */

/* First DMA that landed somewhere its own class does not own.  Reported with
 * the class, the block number and both ends, because "one byte outside the
 * regions changed" is a symptom and this is the cause. */
static char stray[512];

static void dma_hook(int ch, uint8_t bbad, uint32_t src24, uint32_t bytes,
                     uint32_t bdest) {
  if(ndma < MAXDMA) {
    dmas[ndma].ch = ch; dmas[ndma].bbad = bbad; dmas[ndma].frame = cur_frame;
    dmas[ndma].src = src24; dmas[ndma].bytes = bytes; dmas[ndma].dest = bdest;
  }
  ndma++;
  if(ck_mode) ck_dma(bbad, src24, bytes, bdest);

  /* A block transfer (mode 1, $2118/$2119) must land WHOLLY inside one class's
     VRAM region and read WHOLLY inside that same class's view -- source and
     destination are the same shift of the same block number, so a pair that
     names two different classes is a run whose block index left its class.
     The boot's own VRAM clear and the solid tiles source ROM and are exempt. */
  if(c6_enabled() && ck_running)       /* the boot's clears are nobody's */
    c6_dma(bbad, src24, bytes, bdest, ck_mode ? m_dma_start_mc() : 0,
           ck_mode ? m_dma_end_mc() : 0);
  /* ⚡ wire $03: the framebuffer's tiles come out of $E4 into the FB footprint,
     which no class owns; gbc_c6.c checks those (k(c), whole tiles, never the
     solid tiles) */
  if(bbad == 0x18 && ndma > ndma_boot && (src24 >> 16) != 0x00 &&
     (src24 >> 16) != 0xE4 && !stray[0]) {
    /* The class is found by DESTINATION, which is unique -- ⚡ wire $02 made
       the SOURCE ranges overlap (a duplicate chr class reads the same view
       bytes as a base-A one), so asking "which class does this source belong
       to" no longer has one answer.  What is then required of the source is
       exactly the invariant the run builder is built on: the same block index
       derived two ways. */
    int c, dc = -1;
    for(c = 0; c < NCLS; c++)
      if(bdest >= cls[c].vram_lo && bdest < cls[c].vram_hi) dc = c;
    if(dc < 0 || src24 < cls[dc].src24 ||
       bdest + bytes > cls[dc].vram_hi ||
       src24 + bytes > cls[dc].src24 + (uint32_t)cls[dc].nblk * cls[dc].blksz ||
       ((src24 - cls[dc].src24) >> cls[dc].sh_src) !=
         ((bdest - cls[dc].vram_lo) >> cls[dc].sh_src)) {   /* both in BYTES */
      snprintf(stray, sizeof(stray),
        "DMA de %u B: fonte $%06X -> VRAM $%04X (%s)\n"
        "      o bloco tem que sair do MESMO indice dos dois lados e caber na\n"
        "      classe: fonte diz %ld, destino diz %ld, a classe tem %u",
        bytes, src24, (unsigned)bdest, dc >= 0 ? cls[dc].tag : "fora de toda regiao",
        dc >= 0 ? (long)((src24 - cls[dc].src24) >> cls[dc].sh_src) : -1L,
        dc >= 0 ? (long)((bdest - cls[dc].vram_lo) >> cls[dc].sh_src) : -1L,
        dc >= 0 ? cls[dc].nblk : 0);
    }
  }
  /* A view read before COMMIT would break contract sec. 13.5.  Only the DMAs
     that SOURCE a view count: the ones that source ROM (the VRAM clear, the
     solid tiles, the white map) are the player's own data. */
  if(!ef_commit_seen_this_frame || ef_consume_seen_this_frame) {
    const uint8_t bank = (uint8_t)(src24 >> 16);
    int i;
    for(i = 0; i < NVIEWS; i++)
      if(bank == (uint8_t)(VIEWS[i].addr >> 16) &&
         src24 >= VIEWS[i].addr && src24 < VIEWS[i].addr + VIEWS[i].len) {
        if(!ef_commit_seen_this_frame) ef_read_before_commit = 1;
        else ef_read_after_consume = 1;   /* the window closed at CONSUMED */
      }
  }
}

static const char *dma_kind(const dmarec_t *d) {
  int i;
  if(d->bbad == 0x80) return "status";
  if(d->bbad == 0x22) return "cgram";
  if(d->bbad == 0x04) return "oam";
  for(i = 0; i < NCLS; i++)
    if(d->src >= cls[i].src24 && d->src < cls[i].src24 + (uint32_t)cls[i].nblk * cls[i].blksz)
      return cls[i].tag;
  if((d->src >> 16) == 0x00) return "rom";   /* white map / solid tiles / clear */
  return "?";
}

/* ---------------- dirty scenarios ---------------- */
/* Applied to the STAT image between frames.  On silicon this is the bridge
 * re-publishing what it accumulated; the player clears only what it copied,
 * so a spec of "none" is what exercises the CARRY-OVER. */
typedef struct {
  uint8_t chr[6];    /* 48 bits */
  uint8_t obj[4];    /* 32 */
  uint8_t map[8];    /* 64 */
  uint8_t misc;
} dirty_t;

static void set_range(uint8_t *bm, int nbits, int lo, int hi) {
  int b;
  for(b = lo; b <= hi && b < nbits; b++) bm[b >> 3] |= (uint8_t)(1u << (b & 7));
}

/* "chr:all" / "chr:3" / "chr:3-11" */
static int parse_bits(const char *s, uint8_t *bm, int nbits, const char *what) {
  char buf[128], *p, *item;
  if(!strcmp(s, "all")) { set_range(bm, nbits, 0, nbits - 1); return 1; }
  snprintf(buf, sizeof(buf), "%s", s);
  p = buf;
  while((item = strsep(&p, ",")) != NULL) {
    int lo, hi;
    if(!*item) continue;
    if(sscanf(item, "%d..%d", &lo, &hi) == 2) { }
    else if(sscanf(item, "%d-%d", &lo, &hi) == 2) { }
    else if(sscanf(item, "%d", &lo) == 1) { hi = lo; }
    else die("faixa '%s' invalida em %s", item, what);
    if(lo < 0 || hi < lo || hi >= nbits)
      die("%s:%s fora de 0..%d", what, item, nbits - 1);
    set_range(bm, nbits, lo, hi);
  }
  return 1;
}

static void parse_dirty_spec(const char *spec, dirty_t *d) {
  char buf[256], *p, *tok;
  memset(d, 0, sizeof(*d));
  if(!strcmp(spec, "none")) return;
  if(!strcmp(spec, "all")) {
    set_range(d->chr, 48, 0, 47);
    set_range(d->obj, 32, 0, 31);
    set_range(d->map, 64, 0, 63);
    d->misc = 0x03;                     /* b0 OAM, b1 CRAM (contract sec. 5) */
    return;
  }
  snprintf(buf, sizeof(buf), "%s", spec);
  p = buf;
  while((tok = strsep(&p, "+")) != NULL) {
    if(!*tok) continue;
    if(!strncmp(tok, "chr:", 4))      parse_bits(tok + 4, d->chr, 48, "chr");
    else if(!strncmp(tok, "obj:", 4)) parse_bits(tok + 4, d->obj, 32, "obj");
    else if(!strncmp(tok, "map:", 4)) parse_bits(tok + 4, d->map, 64, "map");
    else if(!strcmp(tok, "oam"))      d->misc |= 0x01;
    else if(!strcmp(tok, "cram"))     d->misc |= 0x02;
    else if(!strcmp(tok, "msel"))     d->misc |= 0x10;
    else if(!strcmp(tok, "none"))     ;
    else die("item de --dirty desconhecido: '%s'", tok);
  }
}

static void apply_dirty(const dirty_t *d) {
  m_poke_block(stat_addr + GBC_ST_DCHR,  d->chr, 6);
  m_poke_block(stat_addr + GBC_ST_DOBJ,  d->obj, 4);
  m_poke_block(stat_addr + GBC_ST_DMAP,  d->map, 8);
  m_poke(stat_addr + GBC_ST_DMISC, d->misc);
}

/* ---------------- VRAM regions the frames may write ---------------- */
/* Derived from GbcClsTab.  Anything else must still hold what the boot left
 * there -- including the two solid tiles and, ⚡ since wire $02, the white
 * LCD-off map, which are all written ONCE during the bring-up. */
#define VRAM_WHITE_MAP 0xA000
#define VRAM_TILE768   0xB000
static int vram_is_class_dest(uint32_t a) {
  int i;
  for(i = 0; i < NCLS; i++) if(a >= cls[i].vram_lo && a < cls[i].vram_hi) return 1;
  return 0;
}

/* ---------------- dump ---------------- */
static void put32(FILE *f, uint32_t v) {
  fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
  fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}
static void put_sec(FILE *f, const char *tag, const void *p, uint32_t n) {
  fwrite(tag, 1, 4, f); put32(f, n); fwrite(p, 1, n, f);
}

/* SNST: "SNST" u32 version, then {tag[4], u32 len, payload}.
 *   VRAM 65536   CGRM 512   OAM_ 544
 *   REGS 256     $2100-$21FF, last byte written to each
 *   SCRL 16      BG1H,BG1V .. BG4H,BG4V as u16 LE, through the BGOFS latch
 *   HDMA 2*n     the two WRAM HDMA sets, verbatim
 *   CTRS 16      the player's own diagnostic counters
 *   DPAG 128     direct page $00-$7F: backlog, cursors, rotation, gates    */
static void dump_snes(const char *path) {
  FILE *f = fopen(path, "wb");
  int i;
  uint8_t scrl[16];
  if(!f) die("nao consigo escrever %s", path);
  fwrite("SNST", 1, 4, f); put32(f, 1);
  put_sec(f, "VRAM", m_vram, sizeof(m_vram));
  put_sec(f, "CGRM", m_cgram(), 512);
  put_sec(f, "OAM_", m_oam(), 544);
  put_sec(f, "REGS", m_ppu_regs(), 256);
  for(i = 0; i < 4; i++) {
    uint16_t h = m_bg_hofs(i + 1), v = m_bg_vofs(i + 1);
    scrl[i * 4 + 0] = (uint8_t)h; scrl[i * 4 + 1] = (uint8_t)(h >> 8);
    scrl[i * 4 + 2] = (uint8_t)v; scrl[i * 4 + 3] = (uint8_t)(v >> 8);
  }
  put_sec(f, "SCRL", scrl, 16);
  { uint8_t cd[3]; for(i = 0; i < 3; i++) cd[i] = m_coldata(i);   /* $2132, decoded */
    put_sec(f, "COLD", cd, 3); }
  { uint8_t *h = (uint8_t*)malloc(hdma_set_len * 2); uint32_t k;
    for(k = 0; k < hdma_set_len; k++) h[k] = m_peek(a_seta + k);
    for(k = 0; k < hdma_set_len; k++) h[hdma_set_len + k] = m_peek(a_setb + k);
    put_sec(f, "HDMA", h, hdma_set_len * 2); free(h); }
  { uint8_t c[16]; for(i = 0; i < 16; i++) c[i] = m_peek(a_ctr + i);
    put_sec(f, "CTRS", c, 16); }
  /* The phase-4 half: which channels the player armed, where it pointed them
     and what its own raster counters say.  $420C and $43xx are write-only on
     silicon, so they come out of the model's shadow, not out of a read. */
  put_sec(f, "CPUR", m_cpu_regs(), 256);
  put_sec(f, "DMAR", m_dma_regs(), 128);
  { uint8_t r[32]; for(i = 0; i < 32; i++) r[i] = m_peek(a_rctr + i);
    put_sec(f, "RCTR", r, 32); }
  put_sec(f, "HMAP", hmap_raw, (uint32_t)sizeof(hmap_raw));
  { uint8_t dp[128]; for(i = 0; i < 128; i++) dp[i] = m_peek(0x7e0000 + i);
    put_sec(f, "DPAG", dp, 128); }
  fclose(f);
}

/* ---------------- "exactly the dirty blocks moved" ---------------- */
/* The strongest statement the block model makes: DIRTY_CHR bit i IS byte
 * i*256 of the BG chr view and word (base + i*128) of VRAM, with no
 * permutation anywhere (the comment above GbcClsTab).  Checked here as: every
 * dirty block was covered, and every block covered but NOT dirty sits in a
 * gap the coalescing rule is allowed to bridge (contract sec. 6, HYBRID).
 *
 * The map classes are the interesting half.  DIRTY_MAP bit r names a row of
 * GB VRAM, not of a view: the BG pair follows LCDC.3 and the WIN pair follows
 * LCDC.6, so the SAME 32 incoming bits land in one, both or neither.  Getting
 * that backwards is invisible in any game that leaves LCDC.3 = LCDC.6, which
 * is why it is asserted rather than eyeballed.
 *
 * Only meaningful on a frame whose backlog was empty on entry -- a carried-over
 * block is legitimately drained without being dirty THIS frame.
 */
static int check_blocks(const dirty_t *d, int dma0, int frame) {
  int c, bad = 0;
  for(c = 0; c < NCLS; c++) {
    const uint8_t *bm;
    uint8_t want[64], got[64];
    int b, lo = -1, hi = -1, j, k, b0 = CLS_INFO[c].dbit0;
    memset(want, 0, sizeof(want));
    memset(got, 0, sizeof(got));
    /* ⚡ wire $02: no LCDC anywhere.  Every class names a slice of ONE of the
       bridge's three bitmaps, starting at a fixed bit -- including the map
       classes, where DIRTY_MAP bit r IS row r of the content class and row r
       of the carpet class, whatever LCDC says.  Wire $01 had to pick a half
       with LCDC.3 / LCDC.6 here, which is the thing this phase removed. */
    bm = CLS_INFO[c].dsrc == DSRC_CHR ? d->chr :
         CLS_INFO[c].dsrc == DSRC_OBJ ? d->obj : d->map;
    for(b = 0; b < cls[c].nblk; b++) {
      int r = b0 + b;
      want[b] = (bm[r >> 3] >> (r & 7)) & 1;
    }
    for(j = dma0; j < ndma && j < MAXDMA; j++) {
      uint32_t b0, nb;
      if(dmas[j].bbad != 0x18) continue;
      if(dmas[j].dest < cls[c].vram_lo || dmas[j].dest >= cls[c].vram_hi) continue;
      b0 = (dmas[j].dest - cls[c].vram_lo) / cls[c].blksz;
      nb = dmas[j].bytes / cls[c].blksz;
      for(k = 0; k < (int)nb && b0 + k < (uint32_t)cls[c].nblk; k++) got[b0 + k] = 1;
    }
    for(b = 0; b < cls[c].nblk; b++) if(want[b]) { if(lo < 0) lo = b; hi = b; }
    for(b = 0; b < cls[c].nblk; b++) {
      if(want[b] && !got[b]) {
        fprintf(stderr, "FAIL frame %d classe %d (%s): bloco %d estava sujo e "
                "nao foi copiado\n", frame, c, cls[c].tag, b);
        bad++; break;
      }
      if(got[b] && !want[b] && (lo < 0 || b < lo || b > hi)) {
        if(lo < 0)
          fprintf(stderr, "FAIL frame %d classe %d (%s): bloco %d foi copiado e "
                  "NENHUM bloco dessa classe estava sujo\n", frame, c, cls[c].tag, b);
        else
          fprintf(stderr, "FAIL frame %d classe %d (%s): bloco %d foi copiado sem "
                  "estar sujo e fora da faixa suja %d..%d -- so' um GAP entre dois "
                  "blocos sujos pode ser atravessado (sec. 6)\n",
                  frame, c, cls[c].tag, b, lo, hi);
        bad++; break;
      }
    }
  }
  return bad;
}


/* =========================================================================
 * --interleave: the NMI injected INTO a running compile
 *
 * The player's structural decision (the .asm's "WHERE IT RUNS", contract
 * sec. 11.4) is that the raster compile runs in the MAIN LOOP and the frame
 * body is free to interrupt it at any instruction.  GbcFrameOnce -- body then
 * compile, back to back -- is the one shape that cannot test that, and a
 * property nothing executes is a property nobody has.
 *
 * So: run GbcNmiBody, then run GbcRasterRun in slices of N instructions, and
 * between slices run a WHOLE further GbcNmiBody at the suspended stack depth,
 * with A/X/Y/P saved and restored exactly as the real NMI wrapper does.  The
 * views are MUTATED around each injection, so a body that re-copied an input
 * the compile is reading is caught by BYTES THAT CHANGED and not by an
 * assertion about the source.
 *
 * Asserted at every injection made while $80 says a compile is in flight:
 *   1. $80 (state) and $81 (target set) come back as they were;
 *   2. the target set is not the one on the bus ($18);
 *   3. the compiler's private direct page, $86-$FF, comes back untouched;
 *   4. the snapshot, the log copy and the CGRAM-view copy come back untouched;
 *   5. every byte of the PUBLISHED set comes back untouched -- nothing half
 *      compiled ever reaches the bus;
 *   6. the body left the stack pointer where it found it;
 *   7. the body wrote no scroll register the HDMA is driving (danger P2):
 *      $73 b0 = ch0/ch1 are the BG pair ($210F/$2110/$2113/$2114), b1 = ch2
 *      drives BG1 ($210D/$210E), b3 = ch3 drives BG3 ($2111/$2112).
 * ========================================================================= */
#define DP_PRIV_LO 0x86          /* first direct-page byte that is the compiler's
                                    alone; $80-$85 are the documented interface */
static long     opt_interleave;
static unsigned il_injections, il_checked;
static unsigned il_bodies;       /* extra GbcNmiBody runs, for the frame counter */

static void il_read(uint32_t a24, uint8_t *dst, uint32_t n) {
  uint32_t k; for(k = 0; k < n; k++) dst[k] = m_peek(a24 + k);
}
static long il_diff(const uint8_t *a, const uint8_t *b, uint32_t n) {
  uint32_t k; for(k = 0; k < n; k++) if(a[k] != b[k]) return (long)k;
  return -1;
}

/* The views the injected body will read: moved, so that a copy made during
 * the compile is a copy of DIFFERENT bytes.  Returns the saved image. */
typedef struct {
  uint8_t stat[16], logn[2];
  uint8_t log0[16], cram0[16];
} il_save_t;

static void il_mutate_views(il_save_t *sv, unsigned nonce) {
  const viewsec_t *lv = view_by_tag("LOG_"), *cv = view_by_tag("CGRM");
  int k;
  for(k = 0; k < 7; k++) {   /* SCX SCY WX WY BGP OBP0 OBP1; LCDC is left alone
                                because flipping it drags the white screen and
                                the map-select storm in with it */
    sv->stat[k] = m_peek(stat_addr + GBC_ST_SCX + k);
    m_poke(stat_addr + GBC_ST_SCX + k, (uint8_t)(sv->stat[k] ^ (0x55 + nonce)));
  }
  for(k = 0; k < 2; k++) {
    sv->logn[k] = m_peek(stat_addr + GBC_ST_LOGN + k);
    m_poke(stat_addr + GBC_ST_LOGN + k, k ? 0x01 : 0x23);   /* 291 entries */
  }
  for(k = 0; k < 16; k++) {
    sv->log0[k]  = m_peek(lv->addr + k);
    sv->cram0[k] = m_peek(cv->addr + k);
    m_poke(lv->addr + k,  (uint8_t)(sv->log0[k]  ^ 0xA5));
    m_poke(cv->addr + k, (uint8_t)(sv->cram0[k] ^ 0xA5));
  }
}

static void il_restore_views(const il_save_t *sv) {
  const viewsec_t *lv = view_by_tag("LOG_"), *cv = view_by_tag("CGRM");
  int k;
  for(k = 0; k < 7; k++) m_poke(stat_addr + GBC_ST_SCX + k, sv->stat[k]);
  for(k = 0; k < 2; k++) m_poke(stat_addr + GBC_ST_LOGN + k, sv->logn[k]);
  for(k = 0; k < 16; k++) {
    m_poke(lv->addr + k,  sv->log0[k]);
    m_poke(cv->addr + k, sv->cram0[k]);
  }
}

/* One injection.  Returns the number of failures printed. */
static int il_inject(uint32_t a_nmibody, int frame, unsigned nonce) {
  static uint8_t set_a[0x2000], set_b[0x2000];
  static uint8_t snap_a[256], snap_b[256], log_a[4096], log_b[4096];
  static uint8_t cram_a[1024], cram_b[1024], dp_a[0x100], dp_b[0x100];
  /* reg[] is zero-terminated: ⚡ the window's two halves are separate roles
     now and each drives only two registers. */
  static const struct { uint8_t bit; const char *what; uint16_t reg[4]; } PAIR[3] = {
    { 0x01, "par de scroll do BG (ch0/ch1)",   { 0x210F, 0x2110, 0x2113, 0x2114 } },
    { 0x02, "conteudo da janela (ch2, BG1)",   { 0x210D, 0x210E, 0, 0 } },
    { 0x08, "carpete da janela (ch3, BG3)",    { 0x2111, 0x2112, 0, 0 } },
  };
  uint32_t snaplen = GBC_SNAP_LEN, loglen = (uint32_t)GBC_LOG_MAX * 4;
  uint32_t cramlen = view_by_tag("CGRM")->len;
  uint32_t pubbase, tgtbase;
  uint8_t  st80, st81, st18, roles;
  uint32_t wr0[0x100];
  m_cpu_t  save;
  il_save_t views;
  long      d;
  int       bad = 0, k;

  st80 = m_peek(0x7e0080);
  if(st80 != 0x02) return 0;         /* no compile in flight: nothing to prove */
  st81 = m_peek(0x7e0081);
  st18 = m_peek(0x7e0018);
  roles = m_peek(0x7e0073);
  pubbase = st18 ? a_setb : a_seta;
  tgtbase = st81 ? a_setb : a_seta;
  il_checked++;

  if(hdma_set_len > sizeof(set_a)) die("conjunto de HDMA maior que o buffer");
  if(snaplen > sizeof(snap_a) || loglen > sizeof(log_a) || cramlen > sizeof(cram_a))
    die("buffer de comparacao pequeno demais");

  il_read(pubbase, set_a, hdma_set_len);
  il_read(a_snap, snap_a, snaplen);
  il_read(a_log,  log_a,  loglen);
  il_read(a_cram, cram_a, cramlen);
  il_read(0x7e0000, dp_a, 0x100);
  memcpy(wr0, m_ppu_reg_writes(), sizeof(wr0));

  if(tgtbase == pubbase) {
    fprintf(stderr, "FAIL interleave frame %d: a compilacao esta' escrevendo o "
            "conjunto QUE ESTA' NO BARRAMENTO ($18 = %u, $81 = %u)\n",
            frame, st18, st81);
    bad++;
  }

  /* The body, at the suspended stack depth and with the registers saved and
     restored exactly as the NMI wrapper (rep #$30 / pha phx phy ... rti) does.
     DP and DBR are never changed anywhere in the player, so they ride along. */
  il_mutate_views(&views, nonce);
  save = m_cpu;
  /* The NMI wrapper's prologue, which is what the body is entitled to assume:
     `rep #$30 / pha phx phy / sep #$20`.  The hardware vector does NOT touch
     M/X, so the widths the body runs with are the ones that wrapper sets --
     entering it with the compiler's widths would mis-size the first immediate
     and the model would execute rubbish.  DP and DBR ride along untouched,
     exactly as on silicon (nothing in the player ever moves them). */
  m_cpu.p = (uint8_t)((m_cpu.p | M_M) & (uint8_t)~M_X);
  ef_commit_seen_this_frame = 0;
  ef_sync_seen_this_frame = 0;
  ef_consume_seen_this_frame = 0;
  m_call(a_nmibody, 0);
  il_bodies++;
  if(m_cpu.s != save.s) {
    fprintf(stderr, "FAIL interleave frame %d: o corpo devolveu SP = $%04X, "
            "entrou com $%04X -- a pilha da compilacao esta' embaixo\n",
            frame, m_cpu.s, save.s);
    bad++;
  }
  { uint16_t sp = m_cpu.s; uint64_t ins = m_cpu.instrs;
    m_cpu = save; m_cpu.s = sp; m_cpu.instrs = ins; }
  il_restore_views(&views);
  il_injections++;

  il_read(pubbase, set_b, hdma_set_len);
  il_read(a_snap, snap_b, snaplen);
  il_read(a_log,  log_b,  loglen);
  il_read(a_cram, cram_b, cramlen);
  il_read(0x7e0000, dp_b, 0x100);

  if(m_peek(0x7e0080) != st80) {
    fprintf(stderr, "FAIL interleave frame %d: $80 (estado da compilacao) %u -> %u; "
            "o corpo tem que deixar uma compilacao em voo em paz\n",
            frame, st80, m_peek(0x7e0080));
    bad++;
  }
  if(m_peek(0x7e0081) != st81) {
    fprintf(stderr, "FAIL interleave frame %d: $81 (conjunto alvo) %u -> %u no meio "
            "de uma compilacao\n", frame, st81, m_peek(0x7e0081));
    bad++;
  }
  d = il_diff(dp_a + DP_PRIV_LO, dp_b + DP_PRIV_LO, 0x100 - DP_PRIV_LO);
  if(d >= 0) {
    fprintf(stderr, "FAIL interleave frame %d: o corpo mexeu na direct page do "
            "compilador -- $%02X = $%02X, era $%02X\n", frame,
            (unsigned)(DP_PRIV_LO + d), dp_b[DP_PRIV_LO + d], dp_a[DP_PRIV_LO + d]);
    bad++;
  }
  d = il_diff(snap_a, snap_b, snaplen);
  if(d >= 0) {
    fprintf(stderr, "FAIL interleave frame %d: o snapshot privado foi reescrito "
            "durante a compilacao (+$%02lX)\n", frame, d);
    bad++;
  }
  d = il_diff(log_a, log_b, loglen);
  if(d >= 0) {
    fprintf(stderr, "FAIL interleave frame %d: a COPIA DO LOG foi reescrita durante "
            "a compilacao (+$%04lX) -- e' o buffer que ela esta' lendo\n", frame, d);
    bad++;
  }
  d = il_diff(cram_a, cram_b, cramlen);
  if(d >= 0) {
    fprintf(stderr, "FAIL interleave frame %d: a copia da view de CGRAM foi "
            "reescrita durante a compilacao (+$%04lX)\n", frame, d);
    bad++;
  }
  d = il_diff(set_a, set_b, hdma_set_len);
  if(d >= 0) {
    fprintf(stderr, "FAIL interleave frame %d: o conjunto PUBLICADO (%c, $%06X) "
            "mudou durante a compilacao, em +$%04lX -- a HDMA estaria lendo uma "
            "tabela meio escrita\n", frame, st18 ? 'B' : 'A', pubbase, d);
    bad++;
  }
  { const uint32_t *wr1 = m_ppu_reg_writes();
    for(k = 0; k < 3; k++) {
      int r;
      if(!(roles & PAIR[k].bit)) continue;
      for(r = 0; r < 4 && PAIR[k].reg[r]; r++) {
        uint16_t a = PAIR[k].reg[r];
        if(wr1[a & 0xff] != wr0[a & 0xff]) {
          fprintf(stderr, "FAIL interleave frame %d: o corpo escreveu $%04X, que a "
                  "HDMA esta' dirigindo (%s) -- o latch BGOFS e' compartilhado e o "
                  "segundo registrador do grupo receberia o byte do primeiro\n",
                  frame, a, PAIR[k].what);
          bad++;
        }
      }
    } }
  return bad;
}

/* The whole frame, with the compile sliced.  Returns failures. */
static int il_frame(uint32_t a_nmibody, uint32_t a_rasterrun, int frame) {
  unsigned n_sync = ef_n[GBC_EFO_SYNC], n_commit = ef_n[GBC_EFO_COMMIT];
  unsigned n_consume = ef_n[GBC_EFO_CONSUME], bodies0 = il_bodies;
  int bad = 0, susp, guard = 0;
  m_call(a_nmibody, 0);
  susp = m_call_stepped(a_rasterrun, 0, (uint64_t)opt_interleave);
  while(susp) {
    bad += il_inject(a_nmibody, frame, (unsigned)guard);
    if(++guard > 100000) die("interleave: a compilacao do frame %d nao termina", frame);
    susp = m_step_resume((uint64_t)opt_interleave);
  }
  /* Contract sec. 13.5 is per BODY, and this frame ran 1 + the injections. */
  { unsigned want = 1 + (il_bodies - bodies0);
    if(ef_n[GBC_EFO_SYNC] - n_sync != want ||
       ef_n[GBC_EFO_COMMIT] - n_commit != want ||
       ef_n[GBC_EFO_CONSUME] - n_consume != want) {
      fprintf(stderr, "FAIL interleave frame %d: strobes %u/%u/%u para %u corpos\n",
              frame, ef_n[GBC_EFO_SYNC] - n_sync, ef_n[GBC_EFO_COMMIT] - n_commit,
              ef_n[GBC_EFO_CONSUME] - n_consume, want);
      bad++;
    } }
  return bad;
}

/* ===========================================================================
 * --clock: the player FREE-RUNNING on the interpreter's clock (phase 5).
 *
 * Everything above drives GbcFrameOnce with V pinned, which is exactly the
 * blind spot the header of run_gbc_player.sh names: nothing measures how much
 * of the window a frame costs, a transfer cannot be late, and the V-IRQ of
 * phase 5 -- a SECOND entry point into the frame body, interrupted in turn by
 * the NMI -- cannot be reached at all.  Here the boot stops at MainLoop as
 * usual and then the REAL main loop runs under m65816's clock: WAI, the V-IRQ
 * at VTIME and the NMI at V=225 dispatch through the ROM's own vectors, and
 * this driver plays the BRIDGE around it:
 *   * LY=0 once per Game Boy frame (--ly0 line, --ly0-drift lines per frame):
 *     the --dirty spec of that GB frame accumulates (VRAM writes are live);
 *     if the read window is open (COMMIT seen, CONSUMED not) the snapshot is
 *     SKIPPED and counted (contract sec. 7), else SEQ advances, the OAM/CRAM
 *     bits of the spec go pending and flags0.LOCKED is published (--locked);
 *   * COMMIT publishes the accumulator into the status image (sec. 6); a
 *     second one in the same window is ignored and counted;
 *   * CONSUMED closes the window.
 * What it checks, always (a violation fails the run):
 *   * no VRAM/CGRAM/OAM DMA overlaps a line the letterbox hands back to the
 *     picture (V = 40..184 with ch5 on) -- the silent-corruption failure;
 *   * no CGRAM DMA overlaps an HDMA line (0..224) while a channel writes
 *     $2121 (TABLE SPEC S9);
 *   * exactly one COMMIT, SYNC and CONSUMED per frame, and no view read
 *     outside COMMIT..CONSUMED;
 *   * the V-IRQ handler is never entered twice in one frame (a missing $4211
 *     acknowledge re-enters it on every RTI).
 * Frames are counted from line 100 to line 100 (mid-picture, where nothing
 * transfers), so window B (185..224) and window A (225..39) of one frame fall
 * in the same record.
 * =========================================================================== */
#define CK_MAXF 1100
typedef struct {
  unsigned dmas, bytes, eq;
  int irq_n, nmi_n, commit_n, sync_n, consume_n;
  int irq_v, commit_v, sync_v, sync_h, consume_v;
  int first_v, last_v;             /* first DMA start / last DMA end line */
  unsigned bytes_b;                /* of which in window B (185..224) */
  uint64_t main_mc;                /* time outside every handler */
  int backlog;                     /* backlog non-empty at the frame's end */
  int compiles;                    /* compiles that reached "done" */
  uint64_t instrs;                 /* instructions executed in the frame */
  int regs_v;                      /* line of GbcRegs' $2107 write (-1: none) */
} ck_frame_t;

/* ck_running (declared above): the boot's DMAs/strobes are no frame's */
static int      ck_stall_v = -1, ck_stall_n;   /* --stall-irq V:N (see ck_regs_hdma) */
static unsigned ck_stalls;
/* Which handler is on top: a stack of the kinds taken (0 NMI, 1 IRQ). */
static int      ck_istk[8], ck_isp;
static int      ck_start_line = 100, ck_ly0 = 182, ck_locked = -1;
static double   ck_drift = 0.0, ck_ly0_next;   /* in master cycles */
static int      ck_ly0_sync = 1;   /* locked: LY=0 = SYNC - ALVO, tracked */
/* --relock-every N: every N GB frames the LCD "toggles" and LY=0 jumps by
   --relock-jump lines (default: a spread, see ck_ly0_due); the genlock pulls it back at
   its full authority, 702 dots = 2.6 lines a frame (contract sec. 2), and
   LOCKED comes back after 8 frames within 64 dots (0.24 line).  The bridge
   publishes LOCKED --lock-lag frames late (default 1: the snapshot right after
   the jump still says locked). */
static int      ck_relock_every, ck_lock_lag = 1, ck_lock_cnt = 8;
static double   ck_relock_jump = -1.0, ck_err, ck_last_ly0 = -1.0;
static int      ck_relocks;
static int      ck_lock_hist[16];
static uint64_t ck_abs_line;       /* lines since the clock started */
static int      ck_busy, ck_gbf, ck_nf;
static uint8_t  ck_acc[18], ck_acc_misc, ck_dmisc_keep;
static unsigned ck_snap_skipped, ck_snaps, ck_commit_busy;
static unsigned ck_late, ck_s9, ck_irq_reentry;
static uint64_t ck_nmi_delay_max;
static const uint8_t *ck_on;
static char     ck_first_late[256];
static ck_frame_t ck_f[CK_MAXF];
static dirty_t *ck_specs; static int ck_nspecs;
static uint8_t  ck_prev80;
static uint64_t ck_arm_line; static int ck_armed;
static uint64_t ck_lat_max, ck_lat_sum; static unsigned ck_lat_n;
static uint64_t ck_main0, ck_instr0;
static FILE    *ck_trace;

static int ck_v_of(uint64_t mc) {
  return (int)((ck_start_line + mc / 1364) % 262);
}

static void ck_ly0_due(double now);
static void ck_catchup(uint64_t now) { if(ck_running) ck_ly0_due((double)now); }
static const char *sym_at(uint32_t a) {       /* nearest symbol at or below */
  size_t i; const char *best = "?"; uint32_t ba = 0;
  for(i = 0; i < nsyms; i++)
    if(syms[i].addr <= a && syms[i].addr >= ba) { ba = syms[i].addr; best = syms[i].name; }
  return best;
}
/* ---------------------------------------------------------------------------
 * --compile-profile: where the raster compiler's instructions go.
 *
 * A PC hook (m_set_pc_hook) sees every instruction.  A compile is the stretch
 * from GbcCompile's entry to the first instruction at GbcRrPub (it finished)
 * or GbcRrAbort (it asked for a CGRAM copy and gave up); only the ones that
 * finished are counted.  Inside a compile, each instruction is charged to the
 * PHASE it runs in -- the last pass entry the PC went through (the passes are
 * called one after the other from GbcCompile, so "last entered" is the phase)
 * -- and to the nearest symbol at or below its PC, for the hot-spot list.
 * Instructions an interrupt runs in the middle of a compile (--clock) are not
 * the compiler's and are left out; so are the master cycles they cost.
 *
 * The phase list is a set of labels, and a label the image does not have is
 * simply skipped -- so the same CLI profiles two players whose passes are not
 * named alike (the before/after of the compiler rewrite).
 * ------------------------------------------------------------------------- */
#define PROF_MAXPH 16
static const char *prof_path;
static int prof_on;
static const char *wram_path;
static int ck_settle;
static int poison_log;
static const char *prof_phase_names =
  "GbcCompile,GbcPass1,GbcLcdcScan,GbcP1Spares,GbcPassRegs,"
  "GbcPassColour,GbcPcFast,GbcPcDrop,GbcSetHeader";
static int      prof_nph;
static uint32_t prof_ph_addr[PROF_MAXPH];
static char     prof_ph_name[PROF_MAXPH][40];
static uint64_t prof_ph_ins[PROF_MAXPH], prof_ph_mc[PROF_MAXPH];
static uint64_t prof_cur_ins[PROF_MAXPH], prof_cur_mc[PROF_MAXPH];
static uint32_t prof_a_compile, prof_a_pub, prof_a_abort;
static int      prof_in, prof_ph, prof_last_depth;
static uint64_t prof_last_mc;
static unsigned prof_done, prof_aborted;
static uint64_t prof_first_ins, prof_first_mc, prof_min_ins = ~0ull, prof_max_ins;
static uint32_t *prof_pc_ins;      /* 16 MB of counters would be silly: the player
                                      is one 32 KB LoROM page, $00:8000-$FFFF */
static uint64_t *prof_pc_mc;       /* master cycles, same indexing */
static uint32_t prof_last_pc;
static uint32_t prof_lookup(const char *name) {
  size_t i;
  for(i = 0; i < nsyms; i++) if(!strcmp(syms[i].name, name)) return syms[i].addr;
  return 0xFFFFFFFFu;
}
static void prof_hook(uint32_t pc24) {
  int depth = m_int_depth(), i;
  uint64_t now = m_clock_mc();
  /* charge the instruction that just ran (it began at the previous hook) */
  if(prof_in && prof_last_depth == 0) {
    prof_cur_mc[prof_ph] += now - prof_last_mc;
    if(prof_pc_mc && (prof_last_pc >> 16) == 0 && (prof_last_pc & 0x8000))
      prof_pc_mc[prof_last_pc & 0x7FFF] += now - prof_last_mc;
  }
  prof_last_mc = now; prof_last_depth = depth; prof_last_pc = pc24;
  if(depth) return;
  if(!prof_in) {
    if(pc24 != prof_a_compile) return;
    prof_in = 1; prof_ph = 0;
    memset(prof_cur_ins, 0, sizeof prof_cur_ins);
    memset(prof_cur_mc, 0, sizeof prof_cur_mc);
  } else if(pc24 == prof_a_pub || pc24 == prof_a_abort) {
    uint64_t t = 0, tm = 0;
    prof_in = 0;
    if(pc24 == prof_a_abort) { prof_aborted++; return; }
    for(i = 0; i < prof_nph; i++) {
      t += prof_cur_ins[i]; tm += prof_cur_mc[i];
      prof_ph_ins[i] += prof_cur_ins[i]; prof_ph_mc[i] += prof_cur_mc[i];
    }
    if(!prof_done) { prof_first_ins = t; prof_first_mc = tm; }
    if(t < prof_min_ins) prof_min_ins = t;
    if(t > prof_max_ins) prof_max_ins = t;
    prof_done++;
    return;
  }
  for(i = 0; i < prof_nph; i++) if(pc24 == prof_ph_addr[i]) { prof_ph = i; break; }
  prof_cur_ins[prof_ph]++;
  if(prof_pc_ins && (pc24 >> 16) == 0 && (pc24 & 0x8000)) prof_pc_ins[pc24 & 0x7FFF]++;
}
static void prof_setup(void) {
  char buf[512], *p, *tok;
  prof_a_compile = prof_lookup("GbcCompile");
  prof_a_pub = prof_lookup("GbcRrPub");
  prof_a_abort = prof_lookup("GbcRrAbort");
  if(prof_a_compile == 0xFFFFFFFFu || prof_a_pub == 0xFFFFFFFFu)
    die("--compile-profile: GbcCompile/GbcRrPub ausentes do mapa");
  snprintf(buf, sizeof buf, "%s", prof_phase_names);
  p = buf;
  while((tok = strsep(&p, ",")) != NULL && prof_nph < PROF_MAXPH) {
    uint32_t a = prof_lookup(tok);
    if(a == 0xFFFFFFFFu) continue;
    prof_ph_addr[prof_nph] = a;
    snprintf(prof_ph_name[prof_nph], sizeof prof_ph_name[0], "%s", tok);
    prof_nph++;
  }
  prof_pc_ins = (uint32_t*)calloc(0x8000, sizeof(uint32_t));
  prof_pc_mc = (uint64_t*)calloc(0x8000, sizeof(uint64_t));
  prof_on = 1;
}
/* The one PC hook m65816 has, shared by the compile profiler and the C6
   drain's CPU meter (gbc_c6.c). */
static void pc_hook_all(uint32_t pc24) {
  if(prof_on) prof_hook(pc24);
  if(c6_enabled()) c6_pc(pc24, m_int_depth(), m_cpu.s, m_clock_now());
}
static int prof_cmp_desc(const void *a, const void *b) {
  const uint64_t *x = (const uint64_t*)a, *y = (const uint64_t*)b;
  return x[0] < y[0] ? 1 : x[0] > y[0] ? -1 : 0;
}
static void prof_report(void) {
  FILE *f = prof_path && strcmp(prof_path, "-") ? fopen(prof_path, "w") : stdout;
  uint64_t tot = 0, totmc = 0; int i; size_t k;
  uint16_t logn = m_peek16(a_rctr + 0x14);
  if(!f) die("--compile-profile: nao consigo escrever %s", prof_path);
  for(i = 0; i < prof_nph; i++) { tot += prof_ph_ins[i]; totmc += prof_ph_mc[i]; }
  fprintf(f, "profile: compiles=%u aborted=%u log_n=%u first_instrs=%llu first_mc=%llu "
          "min_instrs=%llu max_instrs=%llu avg_instrs=%llu\n", prof_done, prof_aborted,
          (unsigned)logn, (unsigned long long)prof_first_ins,
          (unsigned long long)prof_first_mc,
          (unsigned long long)(prof_done ? prof_min_ins : 0),
          (unsigned long long)prof_max_ins,
          (unsigned long long)(prof_done ? tot / prof_done : 0));
  for(i = 0; i < prof_nph; i++)
    fprintf(f, "phase %-16s instrs=%llu mc=%llu\n", prof_ph_name[i],
            (unsigned long long)(prof_done ? prof_ph_ins[i] / prof_done : 0),
            (unsigned long long)(prof_done ? prof_ph_mc[i] / prof_done : 0));
  { /* hot spots: instructions per nearest symbol, per compile */
    uint64_t (*agg)[3] = calloc(nsyms, sizeof *agg);
    for(k = 0; k < 0x8000; k++) {
      uint32_t a = 0x8000u | (uint32_t)k; size_t j, best = (size_t)-1; uint32_t ba = 0;
      if(!prof_pc_ins[k]) continue;
      for(j = 0; j < nsyms; j++)
        if(syms[j].addr <= a && syms[j].addr >= ba) { ba = syms[j].addr; best = j; }
      if(best != (size_t)-1) { agg[best][0] += prof_pc_ins[k]; agg[best][1] = best;
                               agg[best][2] += prof_pc_mc[k]; }
    }
    qsort(agg, nsyms, sizeof *agg, prof_cmp_desc);
    for(k = 0; k < nsyms && k < 30 && agg[k][0]; k++)
      fprintf(f, "hot %-22s instrs=%llu mc=%llu\n", syms[agg[k][1]].name,
              (unsigned long long)(prof_done ? agg[k][0] / prof_done : 0),
              (unsigned long long)(prof_done ? agg[k][2] / prof_done : 0));
    free(agg);
  }
  if(f != stdout) fclose(f);
}
static int ck_pcdump = -1;          /* CK_PCDUMP=<frame>: where the CPU is, per line */
static void ck_line(int v) {
  ck_abs_line++;
  /* --stall-irq: at the first line start from V on (still in the vblank)
     where the V-IRQ handler is on top AND the NMI has already taken its turn
     ($15 = 1: the frame will be closed by this very handler), once a frame */
  { static int stalled_nf = -1;
    if(ck_stall_v >= 0 && v >= ck_stall_v && v <= 261 && stalled_nf != ck_nf &&
       ck_isp && ck_istk[ck_isp - 1] == 1 && m_peek(0x7e0015) == 1) {
      m_clock_stall((uint64_t)ck_stall_n * 1364u); ck_stalls++; stalled_nf = ck_nf;
    } }
  if(ck_pcdump == ck_nf)
    printf("  pc V=%3d %02X:%04X %s depth=%d\n", v, m_cpu.pbr, m_cpu.pc,
           sym_at(((uint32_t)m_cpu.pbr << 16) | m_cpu.pc), m_int_depth());
  /* compile bookkeeping: $80 is the compiler's state byte (0 idle, 1 armed,
     2 running, 3 done).  Sampled once a line -- a compile shorter than a line
     would be missed as "armed", never as "done". */
  { uint8_t s = m_peek(0x7e0080);
    if(s != ck_prev80) {
      if(s == 1) { ck_armed = 1; ck_arm_line = ck_abs_line; }
      if(s == 3 && ck_armed) {
        uint64_t lat = ck_abs_line - ck_arm_line;
        if(lat > ck_lat_max) ck_lat_max = lat;
        ck_lat_sum += lat; ck_lat_n++; ck_armed = 0;
        if(ck_nf < CK_MAXF) ck_f[ck_nf].compiles++;
      }
      ck_prev80 = s;
    } }
  /* --require-cgram-view: the frame's colour channels, played once its last
     picture line starts -- the set the prologue published for it is still on
     the bus and every CGRAM DMA of its vblank has landed */
  if(g_cgram_view && v == 224) cgv_hdma_frame();
  if(v == 100) {                     /* the frame record closes here */
    if(ck_nf < CK_MAXF) {
      ck_f[ck_nf].main_mc = m_mainloop_mc() - ck_main0;
      ck_f[ck_nf].backlog = backlog_or() ? 1 : 0;
      ck_f[ck_nf].instrs = m_cpu.instrs - ck_instr0;
    }
    ck_instr0 = m_cpu.instrs;
    ck_main0 = m_mainloop_mc();
    ck_nf++;
  }
  ck_ly0_due((double)m_clock_line_start());
  c6_line(v);
}

/* The bridge's LY=0, at a master-cycle instant.  Checked at every line start
   AND right before every strobe, so a LY=0 that falls a fraction of a line
   before a COMMIT is served before the window opens -- line granularity would
   call it skipped. */
static void ck_ly0_due(double now) {
  while(now >= ck_ly0_next) {
    const dirty_t *d = &ck_specs[ck_gbf < ck_nspecs ? ck_gbf : ck_nspecs - 1];
    int k;
    ck_gbf++;
    ck_last_ly0 = ck_ly0_next;
    if(c6_enabled()) c6_ly0((uint64_t)ck_last_ly0);   /* the PPU runs, skipped or not */
    ck_ly0_next += (262.0 + ck_drift) * 1364.0;
    for(k = 0; k < 6; k++) ck_acc[k] |= d->chr[k];
    for(k = 0; k < 4; k++) ck_acc[6 + k] |= d->obj[k];
    for(k = 0; k < 8; k++) ck_acc[10 + k] |= d->map[k];
    if(ck_relock_every && ck_gbf % ck_relock_every == 0) {
      /* the LCD came back on at a new phase: --relock-jump lines, or by
         default a deterministic spread over the frame (37..261) */
      double j = ck_relock_jump >= 0.0 ? ck_relock_jump
                                       : 37.0 + (double)((ck_relocks * 97) % 225);
      ck_relocks++;
      ck_ly0_next += j * 1364.0;
      ck_lock_cnt = 0;
    }
    if(ck_busy) {
      ck_snap_skipped++;
      if(getenv("CK_SKIPLOG"))
        printf("  skip gbf=%d V=%d err=%.1f irq_top=%d\n", ck_gbf, m_clock_line(), ck_err,
               ck_isp ? ck_istk[ck_isp - 1] : -1);
    } else {
      views_at_apply(ck_gbf);        /* before SEQ/flags0 are read: the new scene's */
      uint16_t seq = m_peek16(stat_addr + GBC_ST_SEQ);
      uint8_t f0 = m_peek(stat_addr + GBC_ST_FLAGS0);
      int lk = ck_locked;
      if(ck_relock_every) lk = ck_lock_hist[ck_lock_lag];
      m_poke16(stat_addr + GBC_ST_SEQ, (uint16_t)(seq + 1));
      m_poke(stat_addr + GBC_ST_FLAGS0,
             c6_snapshot((uint8_t)((f0 & ~0x10) | (lk ? 0x10 : 0))));
      ck_acc_misc |= d->misc;        /* OAM/CRAM/MSEL are snapshot differences */
      ck_snaps++;
    }
  }
}

static unsigned ck_irq_writes, ck_resume_irq, ck_close_over_irq;
/* ⚡ The GbcRegs guard (phase 5): a CPU store into the registers GbcRegs
   writes -- the map/chr bases $2107-$210C, the eight write-twice scroll
   registers $210D-$2114 and TM $212C -- on a line the HDMA runs (0..224, with
   $420C != 0) is the hazard the guard exists for: the BGOFS latch is shared
   with every scroll channel's transfer, and the LCDC channel writes
   $2108-$210B itself.  --stall-irq V:N stalls the CPU N lines at the start of
   line V when the V-IRQ handler is the one on top, i.e. makes the V-IRQ half
   of a frame it will close itself as slow as a CPU the model does not cover. */
static unsigned ck_regs_hdma;
static char     ck_first_regs_hdma[160];
static uint64_t ck_regs_t0, ck_regs_dur_max;
static int      ck_nmi_tok;          /* $15 as the NMI found it on entry */
static char ck_first_irqw[160];
/* ⚡ WHAT THE V-IRQ HALF MAY NOT TOUCH.  It runs while the compiler can be
   half way through a set, so its whole footprint is the frame body's own
   bytes ($00-$7F of the direct page minus $10-$14, the prologue's scratch)
   and the status copy.  A store into the compiler's direct page, either HDMA
   set, the log / digest / queue / CGRAM copies, the private snapshot or the
   raster counters, made while the V-IRQ is the handler on top and the frame
   is still open ($15 != 0, i.e. before the handoff to GbcFrameResume), is a
   violation.  Stores are seen as the CPU issues them: direct page and bank
   $00 low RAM mirror $7E:0000-$1FFF. */
static void ck_wwatch(uint32_t a) {
  uint32_t w;
  uint8_t bank = (uint8_t)(a >> 16);
  /* GbcRegs writes $2107 unconditionally, first of its register writes; it
     has to land in the vblank, before the HDMA init at V=0 (TWO WINDOWS) */
  if((uint16_t)a == 0x2107 && (bank <= 0x3f || (bank >= 0x80 && bank <= 0xbf)) &&
     ck_nf < CK_MAXF) { ck_f[ck_nf].regs_v = m_clock_line(); ck_regs_t0 = m_clock_mc(); }
  if((bank <= 0x3f || (bank >= 0x80 && bank <= 0xbf)) &&
     (((uint16_t)a >= 0x2107 && (uint16_t)a <= 0x2114) || (uint16_t)a == 0x212c)) {
    if(ck_regs_t0 && m_clock_mc() - ck_regs_t0 > ck_regs_dur_max &&
       m_clock_mc() - ck_regs_t0 < 262u * 1364u)
      ck_regs_dur_max = m_clock_mc() - ck_regs_t0;
    if(m_clock_line() <= 224 && m_cpu_regs()[0x0c]) {
      if(!ck_regs_hdma++)
        snprintf(ck_first_regs_hdma, sizeof(ck_first_regs_hdma),
                 "$%04X em V=%d H=%d com $420C = $%02X", (unsigned)(uint16_t)a,
                 m_clock_line(), m_clock_h(), m_cpu_regs()[0x0c]);
    }
  }
  if(!ck_isp || ck_istk[ck_isp - 1] != 1 || !m_peek(0x7e0015)) return;
  if(bank == 0x7e) w = (uint16_t)a;
  else if((bank <= 0x3f || (bank >= 0x80 && bank <= 0xbf)) && (uint16_t)a < 0x2000) w = (uint16_t)a;
  else return;
  if((w >= 0x0010 && w <= 0x0014) || (w >= 0x0080 && w <= 0x00ff) ||
     (w >= (a_rctr & 0xffff) && w < (a_snap & 0xffff) + GBC_SNAP_LEN) ||
     (w >= (a_seta & 0xffff) && w < (a_setb & 0xffff) + hdma_set_len) ||
     (w >= (a_log & 0xffff) && w < (a_cram & 0xffff) + 512)) {
    if(!ck_irq_writes++)
      snprintf(ck_first_irqw, sizeof(ck_first_irqw), "$7E:%04X em V=%d, PC=%02X:%04X",
               (unsigned)w, m_clock_line(), m_cpu.pbr, m_cpu.pc);
  }
}

static void ck_int(int kind, int v, int h) {
  (void)h;
  if(kind == 2) { if(ck_isp) ck_isp--; }
  else if(ck_isp < 8) ck_istk[ck_isp++] = kind;
  if(kind == 0) ck_nmi_tok = m_peek(0x7e0015);
  if(ck_nf >= CK_MAXF) return;
  if(kind == 1) {
    if(ck_f[ck_nf].irq_n++ == 0) ck_f[ck_nf].irq_v = v;
    else ck_irq_reentry++;
  } else if(kind == 0) ck_f[ck_nf].nmi_n++;
}

static void ck_ef(uint32_t off) {
  ck_frame_t *f = &ck_f[ck_nf < CK_MAXF ? ck_nf : CK_MAXF - 1];
  int v = m_clock_line();
  if(!ck_running) return;             /* the boot's GO */
  ck_ly0_due((double)m_clock_mc());
  if(off == GBC_EFO_COMMIT) {
    f->commit_n++; f->commit_v = v;
    if(ck_busy) { ck_commit_busy++; return; }
    ck_busy = 1;
    m_poke_block(stat_addr + GBC_ST_DCHR, ck_acc, 6);
    m_poke_block(stat_addr + GBC_ST_DOBJ, ck_acc + 6, 4);
    m_poke_block(stat_addr + GBC_ST_DMAP, ck_acc + 10, 8);
    m_poke(stat_addr + GBC_ST_DMISC, (uint8_t)(ck_dmisc_keep | ck_acc_misc));
    memset(ck_acc, 0, sizeof(ck_acc)); ck_acc_misc = 0;
    ef_commit_seen_this_frame = 1; ef_consume_seen_this_frame = 0;
  } else if(off == GBC_EFO_SYNC) {
    f->sync_n++; f->sync_v = v; f->sync_h = m_clock_h();
    /* LY_SYNC (status +0C, LIVE in the bridge: gbc_bridge.v loads it from
       TAP_LY on every SYNC, it is not a snapshot field): the Game Boy line the
       SYNC lands on.  A locked genlock puts SYNC ALVO = 11.525 dots after LY=0,
       i.e. LY_SYNC = 25; the player's V-IRQ gate reads it (GbcVirqWanted), and
       it is the only phase number the player sees in the Exato mode, where the
       RTL reports LOCKED = 1 by construction (gbc_clk.v). */
    if(ck_last_ly0 >= 0.0 && ck_ly0_next > ck_last_ly0) {
      double gbline = (ck_ly0_next - ck_last_ly0) / 154.0;
      int ly = (int)(((double)m_clock_mc() - ck_last_ly0) / gbline);
      m_poke(stat_addr + 0x0C, (uint8_t)(ly > 153 ? 153 : ly));
    }
    /* A LOCKED genlock puts LY=0 ALVO = 43 SNES lines (11.525 dots) before
       SYNC (contract sec. 2).  Modelled as perfect tracking of every SYNC,
       which is the pessimistic side: the real PI loop averages. */
    /* The genlock (contract sec. 2): the error is how far the last LY=0 sits
       from ALVO = 43 lines before this SYNC; the next GB frame is lengthened
       or shortened by it, clamped to the loop's authority of 702 dots = 2.6
       lines a frame; LOCKED = 8 frames in a row within 64 dots (0.24 line). */
    if(ck_locked && ck_ly0_sync && ck_drift == 0.0 && ck_last_ly0 >= 0.0) {
      int k;
      double d = ((double)m_clock_mc() - ck_last_ly0) / 1364.0 - 43.0, c;
      while(d >= 131.0) d -= 262.0;
      while(d < -131.0) d += 262.0;
      c = d > 2.6 ? 2.6 : (d < -2.6 ? -2.6 : d);
      if(ck_ly0_next < ck_last_ly0 + (262.0 + 2.6) * 1364.0)   /* not mid-jump */
        ck_ly0_next = ck_last_ly0 + (262.0 + c) * 1364.0;
      ck_err = d;
      if(getenv("CK_PLLLOG") && ck_gbf > 295 && ck_gbf < 360)
        printf("  pll gbf=%d d=%.2f next_in=%.1f lines locked=%d\n", ck_gbf, d,
               (ck_ly0_next - (double)m_clock_mc()) / 1364.0, ck_lock_cnt >= 8);
      if(d > -0.24 && d < 0.24) { if(ck_lock_cnt < 8) ck_lock_cnt++; }
      else ck_lock_cnt = 0;
      for(k = 15; k > 0; k--) ck_lock_hist[k] = ck_lock_hist[k - 1];
      ck_lock_hist[0] = ck_lock_cnt >= 8;
    }
  } else if(off == GBC_EFO_CONSUME) {
    f->consume_n++; f->consume_v = v;
    /* who closed the frame: the V-IRQ handler (it was still running when the
       NMI came -- the handoff's second case) or the NMI */
    if(ck_isp && ck_istk[ck_isp - 1] == 1) ck_resume_irq++;
    /* ⚡ ...and never an NMI that PRE-EMPTED the V-IRQ half BEFORE ITS
       HANDOFF: that half would still be inside the walk underneath, with the
       drain's direct page live, and a second walk on top of it is two owners
       of the same bytes.  The token says which: the IRQ half decrements $15
       as its LAST act on that state, so an NMI that finds $15 = 1 pre-empted
       only the handler's epilogue (rts, the pulls, rti), and closing the
       frame there is the design; finding it at 2 means the half was still
       working. */
    if(ck_isp >= 2 && ck_istk[ck_isp - 1] == 0 && ck_istk[ck_isp - 2] == 1 &&
       ck_nmi_tok == 2)
      ck_close_over_irq++;
    ck_busy = 0;
    ef_commit_seen_this_frame = 0; ef_consume_seen_this_frame = 1;
  }
}

static void ck_dma(uint8_t bbad, uint32_t src24, uint32_t bytes, uint32_t dest) {
  uint64_t s = m_dma_start_mc(), e = m_dma_end_mc(), t;
  ck_frame_t *f = &ck_f[ck_nf < CK_MAXF ? ck_nf : CK_MAXF - 1];
  int sv = ck_v_of(s), ev = ck_v_of(e ? e - 1 : e);
  const uint8_t en = m_cpu_regs()[0x0c];
  const uint8_t *dr = m_dma_regs();
  int ppu = (bbad == 0x18 || bbad == 0x19 || bbad == 0x22 || bbad == 0x04);
  if(!ck_running) return;
  f->dmas++; f->bytes += bytes; f->eq += bytes + GBC_DMACOST;
  if(!f->first_v && f->dmas == 1) f->first_v = sv;
  f->last_v = ev;
  if(sv >= 185 && sv < 225) f->bytes_b += bytes;
  if(ck_trace) fprintf(ck_trace, "%d %02X %06X %04X %u V=%d..%d\n", ck_nf, bbad, src24,
                       (unsigned)dest, bytes, sv, ev);
  /* every line the transfer touches.  ⚡ wire $03: which lines the screen is
     ON is read out of the letterbox table ch5 is pointed at ($2100 bit 7
     clear = on, VRAM/OAM/CGRAM closed), so the 16/16 letterbox of the C6
     second stage (on from line 16) and its all-blank table are judged by
     their own lines; for the 40/40 letterbox this is exactly 40..184. */
  { static uint8_t on[226]; int line = 1, k2;
    uint32_t a = ((uint32_t)dr[0x54] << 16) | dr[0x52] | (dr[0x53] << 8);
    memset(on, 0, sizeof on);
    if(en & 0x20)
      for(k2 = 0; k2 < 64 && line <= 224; k2 += 2) {
        uint8_t cnt = m_peek(a + (uint32_t)k2), val = m_peek(a + (uint32_t)k2 + 1), j;
        if(!cnt) break;
        if(cnt & 0x80) break;                    /* repeat mode: never used here */
        for(j = 0; j < cnt && line <= 224; j++, line++) on[line] = !(val & 0x80);
      }
    ck_on = on; }
  for(t = s - s % 1364; t < e; t += 1364) {
    int v = ck_v_of(t);
    if(ppu && (en & 0x20) && v >= 1 && v <= 224 && ck_on[v]) {
      if(!ck_late++)
        snprintf(ck_first_late, sizeof(ck_first_late),
                 "frame %d: DMA $%02X de %u B de V=%d a V=%d toca a linha %d, "
                 "que o letterbox ja' devolveu a imagem", ck_nf, bbad, bytes, sv, ev, v);
      break;
    }
    if(bbad == 0x22 && v <= 224) {
      int ch, col = 0;
      for(ch = 0; ch < 8; ch++) if((en >> ch) & 1 && dr[ch * 0x10 + 1] == 0x21) col = 1;
      if(col) { ck_s9++; break; }
    }
  }
  /* A DMA running across the start of V=225 holds the NMI back -- and with
     it the SYNC strobe the genlock measures its phase against. */
  { uint64_t k = s / 1364 + 1;
    while(ck_v_of(k * 1364) != 225) k++;
    if(k * 1364 < e && e - k * 1364 > ck_nmi_delay_max) ck_nmi_delay_max = e - k * 1364; }
}

typedef struct {
  int virq_line, no_virq, refresh_max, sync_spread_max, compiles_min;
  long snap_skipped_max, nmi_delay_max;
  int report, window_b, resume_irq_min;
  long regs_late_min, regs_late_max;
} ck_req_t;

static int ck_run(int nframes, dirty_t *specs, int nspecs, const ck_req_t *rq) {
  int i, fail = 0, refresh = -1, first_irq = -1, irq_frames = 0;
  int sync_hmin = 1 << 30, sync_hmax = -1, commit_b = 0, regs_late = 0, regs_max = -1;
  unsigned tot_bytes = 0, tot_b = 0, tot_dmas = 0;
  int lag_frames = 0, lag_run = 0, lag_max = 0;   /* frames ending with backlog */
  uint64_t main_sum = 0;
  ck_specs = specs; ck_nspecs = nspecs;
  if(ck_locked < 0) ck_locked = (ck_drift == 0.0);
  { int k; for(k = 0; k < 16; k++) ck_lock_hist[k] = 1; }
  if(ck_lock_lag < 0 || ck_lock_lag > 15) die("--lock-lag %d fora de 0..15", ck_lock_lag);
  if(getenv("CK_PCDUMP")) ck_pcdump = atoi(getenv("CK_PCDUMP"));
  ck_dmisc_keep = (uint8_t)(m_peek(stat_addr + GBC_ST_DMISC) & 0x0C);
  /* the first LY=0 is the first time line --ly0 comes round; after that a
     locked genlock follows SYNC (unless --ly0-fixed) */
  ck_ly0_next = (double)((ck_ly0 - ck_start_line + 262) % 262);
  if(ck_ly0_next == 0) ck_ly0_next = 262;
  ck_ly0_next *= 1364.0;
  m_set_line_hook(ck_line);
  m_set_int_hook(ck_int);
  m_set_write_watch(ck_wwatch);
  m_clock_start(ck_start_line, 262);
  ck_main0 = 0; ck_running = 1;
  for(i = 0; i < CK_MAXF; i++) ck_f[i].regs_v = -1;
  m_run_clocked((uint64_t)nframes * 262 * 1364);
  /* --settle: a compile still in flight ($80 = 2) when the frames run out has
     its counters half written (GbcRctrClear runs at its start).  Carry on, a
     line at a time and for at most ten more frames, until the compiler is
     between compiles -- so a dump of a long compile compares whole numbers.
     The frame bookkeeping above is untouched: these lines are past it. */
  if(ck_settle) {
    int k;
    for(k = 0; k < 262 * 10 && m_peek(0x7e0080) == 0x02; k++)
      m_run_clocked(m_clock_mc() + 1364);
    if(m_peek(0x7e0080) == 0x02) die("--settle: a compilacao nao terminou em 10 frames");
  }
  m_set_line_hook(NULL); m_set_int_hook(NULL); m_set_write_watch(NULL);

  for(i = 0; i < ck_nf && i < nframes && i < CK_MAXF; i++) {
    const ck_frame_t *f = &ck_f[i];
    if(rq->report)
      printf("ck %3d irq=%d@%-3d commit=%d@%-3d sync=%d@%d:%-4d consumed=%d@%-3d "
             "dmas=%2u bytes=%5u (B %5u) V=%d..%d main=%3u lines backlog=%s cmp=%d ins=%llu\n",
             i, f->irq_n, f->irq_v, f->commit_n, f->commit_v, f->sync_n, f->sync_v,
             f->sync_h, f->consume_n, f->consume_v, f->dmas, f->bytes, f->bytes_b,
             f->first_v, f->last_v, (unsigned)(f->main_mc / 1364),
             f->backlog ? "PEND" : "0", f->compiles, (unsigned long long)f->instrs);
    if(i == 0) continue;            /* the frame the clock started in is partial */
    if(f->sync_n != 1 || f->commit_n != 1 || f->consume_n != 1) {
      fprintf(stderr, "FAIL ck frame %d: SYNC/COMMIT/CONSUMED = %d/%d/%d, o "
              "contrato sec. 13.5 exige 1/1/1\n", i, f->sync_n, f->commit_n, f->consume_n);
      fail++;
    }
    if(f->irq_n) { irq_frames++; if(first_irq < 0) first_irq = i; }
    if(f->regs_v >= 0) {
      int off = (f->regs_v - 225 + 262) % 262;
      if(off > regs_max) regs_max = off;
      if(f->regs_v < 225) {
        if(!regs_late++)
          fprintf(stderr, "FAIL ck frame %d: GbcRegs escreveu $2107 em V=%d, fora do "
                  "vblank (a HDMA ja' corre)\n", i, f->regs_v);
        fail++;
      }
    }
    if(f->irq_n && rq->virq_line && f->irq_v != rq->virq_line) {
      fprintf(stderr, "FAIL ck frame %d: V-IRQ entrou em V=%d, esperado %d\n",
              i, f->irq_v, rq->virq_line);
      fail++;
    }
    if(f->commit_v >= 185 && f->commit_v < 225) commit_b++;
    if(f->sync_n) {                 /* offset from the start of V=225 */
      int off = ((f->sync_v - 225 + 262) % 262) * 1364 + f->sync_h;
      if(off < sync_hmin) sync_hmin = off;
      if(off > sync_hmax) sync_hmax = off;
    }
    tot_bytes += f->bytes; tot_b += f->bytes_b; tot_dmas += f->dmas;
    main_sum += f->main_mc;
    if(refresh < 0 && !f->backlog) refresh = i;
    if(f->backlog) { lag_frames++; if(++lag_run > lag_max) lag_max = lag_run; }
    else lag_run = 0;
  }
  { int nf = (ck_nf < nframes ? ck_nf : nframes) - 1;
    if(nf < 1) nf = 1;
    printf("clock: frames=%d gb_frames=%d snaps=%u snap_skipped=%u commit_while_busy=%u "
           "late=%u s9=%u irq_reentry=%u\n", nf, ck_gbf, ck_snaps, ck_snap_skipped,
           ck_commit_busy, ck_late, ck_s9, ck_irq_reentry);
    printf("clock: irq_frames=%d commit_in_B=%d first_irq=%d sync_mc=%d..%d "
           "nmi_delay_max=%llu mc refresh_frames=%d\n", irq_frames, commit_b, first_irq,
           sync_hmin, sync_hmax, (unsigned long long)ck_nmi_delay_max, refresh);
    printf("clock: resume_by_irq=%u regs_after_225_max=%d lines lag_frames=%d lag_max=%d\n",
           ck_resume_irq, regs_max, lag_frames, lag_max);
    printf("clock: regs_dur_max=%llu mc regs_late=%u regs_in_hdma=%u stalls=%u\n",
           (unsigned long long)ck_regs_dur_max, (unsigned)m_peek16(a_ctr + GBC_CTR_REGSLATE),
           ck_regs_hdma, ck_stalls);
    printf("clock: bytes/frame=%u (window B %u) dmas/frame=%.1f main_loop=%.1f "
           "lines/frame compiles=%u lat_max=%llu lat_avg=%.1f lines\n",
           tot_bytes / (unsigned)nf, tot_b / (unsigned)nf, (double)tot_dmas / nf,
           (double)main_sum / 1364.0 / nf, ck_lat_n,
           (unsigned long long)ck_lat_max, ck_lat_n ? (double)ck_lat_sum / ck_lat_n : 0.0);
  }
  if(ck_late) { fprintf(stderr, "FAIL ck: %u DMA(s) de VRAM/CGRAM/OAM em linha visivel; "
                        "o primeiro: %s\n", ck_late, ck_first_late); fail++; }
  if(ck_s9) { fprintf(stderr, "FAIL ck: %u DMA(s) de CGRAM sobre linhas de HDMA com um "
                      "canal de cor armado (S9)\n", ck_s9); fail++; }
  if(ck_irq_writes) { fprintf(stderr, "FAIL ck: a metade da V-IRQ escreveu %u vez(es) em "
                              "estado do compilador/HDMA; a primeira: %s\n", ck_irq_writes,
                              ck_first_irqw); fail++; }
  if(rq->window_b && commit_b < (ck_nf < nframes ? ck_nf : nframes) - 2) {
    fprintf(stderr, "FAIL ck: so' %d frame(s) abriram na janela B\n", commit_b); fail++; }
  if(ck_regs_hdma) {
    fprintf(stderr, "FAIL ck: %u store(s) da CPU nos registradores do GbcRegs numa linha "
            "de HDMA; o primeiro: %s\n", ck_regs_hdma, ck_first_regs_hdma); fail++; }
  if(rq->regs_late_min >= 0 && (long)m_peek16(a_ctr + GBC_CTR_REGSLATE) < rq->regs_late_min) {
    fprintf(stderr, "FAIL ck: GbcRegs adiado %u vez(es), esperado >= %ld\n",
            (unsigned)m_peek16(a_ctr + GBC_CTR_REGSLATE), rq->regs_late_min); fail++; }
  if(rq->regs_late_max >= 0 && (long)m_peek16(a_ctr + GBC_CTR_REGSLATE) > rq->regs_late_max) {
    fprintf(stderr, "FAIL ck: GbcRegs adiado %u vez(es), esperado <= %ld\n",
            (unsigned)m_peek16(a_ctr + GBC_CTR_REGSLATE), rq->regs_late_max); fail++; }
  if(ck_close_over_irq) {
    fprintf(stderr, "FAIL ck: %u frame(s) fechado(s) por uma NMI que interrompeu a "
            "metade da V-IRQ (o token de passagem nao foi respeitado)\n", ck_close_over_irq);
    fail++; }
  if((int)ck_resume_irq < rq->resume_irq_min) {
    fprintf(stderr, "FAIL ck: a V-IRQ fechou %u frame(s) ela mesma, esperado >= %d -- o "
            "segundo caso do token nao foi exercitado\n", ck_resume_irq, rq->resume_irq_min);
    fail++; }
  if(ck_irq_reentry) { fprintf(stderr, "FAIL ck: a V-IRQ entrou %u vez(es) a mais no "
                               "mesmo frame (o $4211 nao foi lido?)\n", ck_irq_reentry); fail++; }
  if(ck_commit_busy) { fprintf(stderr, "FAIL ck: %u COMMIT(s) com a janela aberta\n",
                               ck_commit_busy); fail++; }
  if(rq->no_virq && irq_frames) {
    fprintf(stderr, "FAIL ck: %d frame(s) com V-IRQ, esperado nenhum\n", irq_frames); fail++; }
  if(rq->virq_line && !irq_frames) {
    fprintf(stderr, "FAIL ck: nenhuma V-IRQ em %d frames\n", ck_nf); fail++; }
  if(rq->refresh_max && (refresh < 0 || refresh > rq->refresh_max)) {
    fprintf(stderr, "FAIL ck: refresh total em %d frame(s), esperado <= %d\n",
            refresh, rq->refresh_max); fail++; }
  if(rq->snap_skipped_max >= 0 && (long)ck_snap_skipped > rq->snap_skipped_max) {
    fprintf(stderr, "FAIL ck: snap_skipped = %u, esperado <= %ld\n", ck_snap_skipped,
            rq->snap_skipped_max); fail++; }
  if(rq->nmi_delay_max >= 0 && (long)ck_nmi_delay_max > rq->nmi_delay_max) {
    fprintf(stderr, "FAIL ck: um DMA segurou a NMI por %llu mc (limite %ld): o SYNC "
            "do genlock atrasou\n", (unsigned long long)ck_nmi_delay_max, rq->nmi_delay_max);
    fail++; }
  if(rq->sync_spread_max >= 0 && sync_hmax >= 0 && sync_hmax - sync_hmin > rq->sync_spread_max) {
    fprintf(stderr, "FAIL ck: o SYNC variou %d mc de um frame para o outro (limite %d)\n",
            sync_hmax - sync_hmin, rq->sync_spread_max); fail++; }
  if(rq->compiles_min && (int)ck_lat_n < rq->compiles_min) {
    fprintf(stderr, "FAIL ck: %u compilacao(oes) concluida(s), esperado >= %d\n",
            ck_lat_n, rq->compiles_min); fail++; }
  if(stray[0]) { fprintf(stderr, "FAIL ck: %s\n", stray); fail++; }
  if(ef_read_before_commit || ef_read_after_consume) {
    fprintf(stderr, "FAIL ck: view lida fora de COMMIT..CONSUMED\n"); fail++; }
  return fail;
}

/* ---------------- driver ---------------- */
int main(int argc, char **argv) {
  const char *rom_path = NULL, *map_path = NULL, *views_path = NULL;
  const char *dump_path = NULL;
  const char *dirty_arg = "all";
  int nframes = 1, vline = 230, i, a;
  int selftest = 0, poison = 1, budget_report = 0, seq_hold = 0;
  int req_converged = 0, req_defer_first = 0, req_no_defer_last = 0;
  int req_classes = 0, req_no_view_dma = 0;
  long req_drops_min = -1, req_last_dmas = -1, req_total_dmas = -1;
  long req_cgram_dmas = -1, req_oam_dmas = -1, req_420c = -1;
  int req_blocks = 0, req_hdma_budget = 0, req_wram_quiet = 0, req_dup = 0;
  int fair_k = 0, req_white = 0, req_grp = 0, req_whold = 0, whold_since = -1;
  ck_req_t ckrq = { 0, 0, 0, -1, 0, -1, -1, 0, 0, 0, -1, -1 };
  const char *ck_trace_path = NULL, *ck_dtrace_path = NULL;
  int cpurev = 2, pal = 0;
  uint32_t grp_w0[4];
  const char *churn_out = NULL;
  uint32_t churn_step = 0;
  const char *flags0_arg = NULL;
  int flags0v[64], nflags0 = 0;
  int req_sets_equal = 0, req_no_colour = 0, req_cgram_view = 0, req_raster_idle = 0;
  /* Fairness bookkeeping: bytes per class, the frame each class last moved in,
     and the worst gap between two frames in which it moved. */
  uint32_t cls_bytes[NCLS];
  int cls_last[NCLS], cls_gap[NCLS], cls_asked[NCLS], fair_warm = 0;
  memset(cls_bytes, 0, sizeof(cls_bytes));
  memset(cls_last, 0, sizeof(cls_last));
  memset(cls_gap, 0, sizeof(cls_gap));
  memset(cls_asked, 0, sizeof(cls_asked));
  /* --pad takes a ';' list, one entry per frame, last entry repeating -- the
     same shape as --dirty.  A single constant cannot exercise the IGR at all:
     the edge latch ($04) is born ARMED so that a combo still held through the
     $80 reset does not immediately fire again, which means a combo only counts
     on a release -> press transition. */
  const char *pad_arg = NULL;
  uint16_t pads[64]; int npads = 0;
  int ver_late = 0, no_snap = 0, req_mcu_cmd = -1;
  long req_go = -1;
  uint8_t  vram_init[0x10000];
  char    *specs[1024]; int nspecs = 0;
  dirty_t  dspec[1024];
  FILE    *rf;
  uint32_t a_reset, a_mainloop, a_frameonce, a_nmibody, a_rasterrun;
  unsigned prev_defer = 0, prev_drops = 0;
  int fail = 0;
  const char *c6_scene = NULL, *c6_dump = NULL;
  int c6_report = 0, c6_regs = 0, c6_noredund = 0;
  static uint8_t viewwin[5 * 0x10000];   /* $E0-$E4: only $E4 is served here */

  for(a = 1; a < argc; a++) {
    const char *s = argv[a];
    if(!strcmp(s, "--selftest")) selftest = 1;
    else if(!strcmp(s, "--rom") && a + 1 < argc) rom_path = argv[++a];
    else if(!strcmp(s, "--map") && a + 1 < argc) map_path = argv[++a];
    else if(!strcmp(s, "--views") && a + 1 < argc) views_path = argv[++a];
    else if(!strcmp(s, "--dump-snes") && a + 1 < argc) dump_path = argv[++a];
    else if(!strcmp(s, "--frames") && a + 1 < argc) nframes = atoi(argv[++a]);
    else if(!strcmp(s, "--dirty") && a + 1 < argc) dirty_arg = argv[++a];
    else if(!strcmp(s, "--v") && a + 1 < argc) vline = atoi(argv[++a]);
    else if(!strcmp(s, "--pad") && a + 1 < argc) pad_arg = argv[++a];
    else if(!strcmp(s, "--seq-hold")) seq_hold = 1;
    else if(!strcmp(s, "--ver-late") && a + 1 < argc) ver_late = atoi(argv[++a]);
    else if(!strcmp(s, "--no-snap") && a + 1 < argc) no_snap = atoi(argv[++a]);
    else if(!strcmp(s, "--require-go") && a + 1 < argc) req_go = atol(argv[++a]);
    else if(!strcmp(s, "--require-mcu-cmd") && a + 1 < argc)
      req_mcu_cmd = (int)strtoul(argv[++a], NULL, 0);
    else if(!strcmp(s, "--no-poison")) poison = 0;
    else if(!strcmp(s, "--budget-mode")) budget_report = 1;
    else if(!strcmp(s, "-v") || !strcmp(s, "--verbose")) verbose = 1;
    else if(!strcmp(s, "--require-converged")) req_converged = 1;
    else if(!strcmp(s, "--require-defer-first")) req_defer_first = 1;
    else if(!strcmp(s, "--require-no-defer-last")) req_no_defer_last = 1;
    else if(!strcmp(s, "--require-classes-advanced")) req_classes = 1;
    else if(!strcmp(s, "--require-blocks-match")) req_blocks = 1;
    else if(!strcmp(s, "--require-hdma-budget")) req_hdma_budget = 1;
    else if(!strcmp(s, "--require-dup-split")) req_dup = 1;
    else if(!strcmp(s, "--require-fair") && a + 1 < argc) fair_k = atoi(argv[++a]);
    else if(!strcmp(s, "--require-white-quiet")) req_white = 1;
    else if(!strcmp(s, "--require-white-hold")) req_whold = 1;
    else if(!strcmp(s, "--require-group-quiet")) req_grp = 1;
    else if(!strcmp(s, "--view-churn") && a + 1 < argc) churn_out = argv[++a];
    else if(!strcmp(s, "--flags0") && a + 1 < argc) flags0_arg = argv[++a];
    else if(!strcmp(s, "--views-at") && a + 1 < argc) views_at_parse(argv[++a]);
    else if(!strcmp(s, "--require-sets-equal")) req_sets_equal = 1;
    else if(!strcmp(s, "--require-no-colour")) req_no_colour = 1;
    else if(!strcmp(s, "--require-cgram-view")) g_cgram_view = req_cgram_view = 1;
    else if(!strcmp(s, "--require-raster-idle")) req_raster_idle = 1;
    else if(!strcmp(s, "--require-420c") && a + 1 < argc)
      req_420c = (long)strtoul(argv[++a], NULL, 0);
    else if(!strcmp(s, "--require-wram-quiet")) req_wram_quiet = 1;
    else if(!strcmp(s, "--interleave") && a + 1 < argc) opt_interleave = atol(argv[++a]);
    else if(!strcmp(s, "--require-no-view-dma")) req_no_view_dma = 1;
    else if(!strcmp(s, "--require-cgram-dmas") && a + 1 < argc) req_cgram_dmas = atol(argv[++a]);
    else if(!strcmp(s, "--require-oam-dmas") && a + 1 < argc) req_oam_dmas = atol(argv[++a]);
    else if(!strcmp(s, "--require-drops-min") && a + 1 < argc) req_drops_min = atol(argv[++a]);
    else if(!strcmp(s, "--require-last-frame-dmas") && a + 1 < argc) req_last_dmas = atol(argv[++a]);
    else if(!strcmp(s, "--require-total-dmas") && a + 1 < argc) req_total_dmas = atol(argv[++a]);
    /* --clock and its bridge / its requirements (see ck_run) */
    else if(!strcmp(s, "--clock")) ck_mode = 1;
    /* wire $03: the C6 half of the bridge (gbc_c6.c) */
    else if(!strcmp(s, "--c6-scene") && a + 1 < argc) c6_scene = argv[++a];
    else if(!strcmp(s, "--c6-dump") && a + 1 < argc) c6_dump = argv[++a];
    else if(!strcmp(s, "--c6-report")) c6_report = 1;
    else if(!strcmp(s, "--require-c6-regs")) c6_regs = 1;
    else if(!strcmp(s, "--require-c6-no-redundant")) c6_noredund = 1;
    else if(!strcmp(s, "--image-ver") && a + 1 < argc) image_ver = (int)strtol(argv[++a], NULL, 0);
    else if(!strcmp(s, "--compile-profile") && a + 1 < argc) prof_path = argv[++a];
    else if(!strcmp(s, "--dump-wram") && a + 1 < argc) wram_path = argv[++a];
    else if(!strcmp(s, "--settle")) ck_settle = 1;
    else if(!strcmp(s, "--poison-log")) poison_log = 1;
    else if(!strcmp(s, "--profile-phases") && a + 1 < argc) prof_phase_names = argv[++a];
    else if(!strcmp(s, "--clock-report")) ckrq.report = 1;
    else if(!strcmp(s, "--clock-start") && a + 1 < argc) ck_start_line = atoi(argv[++a]);
    else if(!strcmp(s, "--ly0") && a + 1 < argc) ck_ly0 = atoi(argv[++a]);
    else if(!strcmp(s, "--ly0-fixed")) ck_ly0_sync = 0;
    else if(!strcmp(s, "--relock-every") && a + 1 < argc) ck_relock_every = atoi(argv[++a]);
    else if(!strcmp(s, "--relock-jump") && a + 1 < argc) ck_relock_jump = atof(argv[++a]);
    else if(!strcmp(s, "--lock-lag") && a + 1 < argc) ck_lock_lag = atoi(argv[++a]);
    else if(!strcmp(s, "--ly0-drift") && a + 1 < argc) ck_drift = atof(argv[++a]);
    else if(!strcmp(s, "--locked") && a + 1 < argc) ck_locked = atoi(argv[++a]);
    else if(!strcmp(s, "--cpurev") && a + 1 < argc) cpurev = atoi(argv[++a]);
    else if(!strcmp(s, "--pal")) pal = 1;
    else if(!strcmp(s, "--trace-out") && a + 1 < argc) ck_trace_path = argv[++a];
    /* per-GB-frame dirty bitmaps recorded from a real game (dump_sameboy
       --dirty-trace): 'D' + chr[6] obj[4] map[8] misc[1] per frame */
    else if(!strcmp(s, "--dirty-trace") && a + 1 < argc) ck_dtrace_path = argv[++a];
    else if(!strcmp(s, "--require-virq") && a + 1 < argc) ckrq.virq_line = atoi(argv[++a]);
    else if(!strcmp(s, "--require-no-virq")) ckrq.no_virq = 1;
    else if(!strcmp(s, "--require-window-b")) ckrq.window_b = 1;
    else if(!strcmp(s, "--stall-irq") && a + 1 < argc) {
      if(sscanf(argv[++a], "%d:%d", &ck_stall_v, &ck_stall_n) != 2 ||
         ck_stall_v < 0 || ck_stall_v > 261 || ck_stall_n < 1 || ck_stall_n > 200)
        die("--stall-irq V:N (V 0..261, N 1..200 linhas)");
    }
    else if(!strcmp(s, "--require-regs-late-min") && a + 1 < argc) ckrq.regs_late_min = atol(argv[++a]);
    else if(!strcmp(s, "--require-regs-late-max") && a + 1 < argc) ckrq.regs_late_max = atol(argv[++a]);
    else if(!strcmp(s, "--require-resume-by-irq-min") && a + 1 < argc) ckrq.resume_irq_min = atoi(argv[++a]);
    else if(!strcmp(s, "--require-refresh-max") && a + 1 < argc) ckrq.refresh_max = atoi(argv[++a]);
    else if(!strcmp(s, "--require-snap-skipped-max") && a + 1 < argc) ckrq.snap_skipped_max = atol(argv[++a]);
    else if(!strcmp(s, "--require-nmi-delay-max") && a + 1 < argc) ckrq.nmi_delay_max = atol(argv[++a]);
    else if(!strcmp(s, "--require-sync-spread-max") && a + 1 < argc) ckrq.sync_spread_max = atoi(argv[++a]);
    else if(!strcmp(s, "--require-compiles-min") && a + 1 < argc) ckrq.compiles_min = atoi(argv[++a]);
    else die("argumento desconhecido: %s", s);
  }

  if(selftest) {
    int f = m_selftest();
    if(f) { fprintf(stderr, "m65816 selftest: %d falha(s)\n", f); return 1; }
    printf("m65816 selftest OK\n");
    return 0;
  }
  if(!rom_path || !map_path || !views_path)
    die("uso: gbc_render_cli --rom misc/gbc_snes_harness.bin --map <.map> "
        "--views <views.bin> [--frames N] [--dirty SPEC[;SPEC..]] [--v N] "
        "[--dump-snes out.bin] [--budget-mode] [--require-*] "
        "[--compile-profile out|-] [--profile-phases L1,L2..] [--poison-log] "
        "[--clock ... --settle] [--dump-wram out.bin]");
  /* 1024: the fairness gate needs a REGIME, not a handful of frames -- the
     rotation's period is the class count and the property is about windows of
     that size, so the run has to be many laps long. */
  if(nframes < 1 || nframes > 1024) die("--frames %d fora de 1..1024", nframes);
  if(opt_interleave < 0 || opt_interleave > 1000000)
    die("--interleave %ld fora de 0..1000000", opt_interleave);
  if(vline < 0 || vline > 511) die("--v %d: V so' tem 9 bits", vline);

  /* --- the (bin, map) pair -------------------------------------------- */
  load_map(map_path);
  rf = fopen(rom_path, "rb");
  if(!rf) die("nao consigo abrir %s", rom_path);
  rom_len = fread(rom, 1, sizeof(rom), rf);
  if(!feof(rf) && fgetc(rf) != EOF)
    die("%s e' maior que uma pagina LoROM (%zu B)", rom_path, sizeof(rom));
  fclose(rf);
  if(map_rom_bytes != rom_len)
    die("par dessincronizado: %s tem %zu B, %s carimbou %u",
        rom_path, rom_len, map_path, map_rom_bytes);
  { uint32_t c = crc32_buf(rom, rom_len);
    if(c != map_rom_crc)
      die("par dessincronizado: CRC32 do .bin = %08X, o mapa carimbou %08X\n"
          "  rode 'make -C snes/gbc harness' para regerar os dois juntos",
          c, map_rom_crc); }

  a_reset     = sym("Reset");
  a_mainloop  = sym("MainLoop");
  a_frameonce = sym("GbcFrameOnce");
  a_nmibody   = sym("GbcNmiBody");
  a_rasterrun = sym("GbcRasterRun");
  if(prof_path) prof_setup();
  load_harness_map();
  stat_addr = view_by_tag("STAT")->addr;
  if(c6_scene && !ck_mode) die("--c6-scene exige --clock (a maquina de linhas corre no tempo)");

  /* --- scenario list --------------------------------------------------- */
  { char *dbuf = strdup(dirty_arg), *p = dbuf, *tok;  /* a fixed buffer cut a
                                              long list in the middle of a token */
    while((tok = strsep(&p, ";")) != NULL) {
      if(nspecs >= 1024) die("--dirty: mais de 1024 cenarios");
      specs[nspecs] = tok;
      parse_dirty_spec(tok, &dspec[nspecs]);
      nspecs++;
    }
    if(!nspecs) die("--dirty vazio"); }

  /* --- wire the model -------------------------------------------------- */
  m_reset_memory();
  m_load_rom(rom, rom_len);
  m_gbc_mode(1);
  m_hdma_decoy(1);
  m_set_cpurev(cpurev);
  m_set_pal(pal);
  if(pad_arg) {
    char buf[512], *t;
    snprintf(buf, sizeof buf, "%s", pad_arg);
    for(t = strtok(buf, ";"); t && npads < 64; t = strtok(NULL, ";"))
      pads[npads++] = (uint16_t)strtoul(t, NULL, 0);
  }
  if(!npads) pads[npads++] = 0;
  m_set_pad(pads[0]);
  m_set_cmd_sink(cmd_hook);
  m_set_v(vline);
  m_set_ef_sink(a_ef, EF_LEN, ef_hook);
  m_set_dma_hook(dma_hook);
  /* Views BEFORE the boot: GbcLifeWait spins on status.ver until the bridge
     answers, and $4212 reads as "in vblank" forever here, so a missing view
     would hang rather than fail. */
  load_views(views_path);
  stamp_ver();
  load_classes();
  m_set_view_window(viewwin);
  if(c6_scene) {
    c6_setup(c6_scene, stat_addr, viewwin + 4 * 0x10000, a_fb);
    c6_set_syms(sym("GbcFbDrain"), sym("GbcFbRow"));
    c6_set_catchup(ck_catchup);
    c6_set_tables(sym("GbcTblInidisp"), sym("GbcTblW1"));
    c6_set_tables2(prof_lookup("GbcTblInidisp16"), prof_lookup("GbcTblVofs"));
  }
  if(prof_on || c6_scene) m_set_pc_hook(pc_hook_all);
  if(m_peek(stat_addr + GBC_ST_VER) != image_ver)
    die("STAT.ver = $%02X, o player fala $%02X -- as views vieram de outra "
        "wire version", m_peek(stat_addr + GBC_ST_VER), image_ver);
  if(!(m_peek(stat_addr + GBC_ST_FLAGS0) & 0x80))
    die("STAT.flags0 = $%02X sem SNAP_VALID: o player recusa o snapshot e o "
        "teste nao provaria nada (gere as views com --snap-valid)",
        m_peek(stat_addr + GBC_ST_FLAGS0));

  printf("player: %s (%zu B, crc32 %08X) + %s\n", rom_path, rom_len, map_rom_crc, map_path);
  if(verbose) {
    printf("  WRAM: statcopy $%06X ctr $%06X hdma A $%06X B $%06X (%u B) EF $%06X\n",
           a_statcopy, a_ctr, a_seta, a_setb, hdma_set_len, a_ef);
    for(i = 0; i < NCLS; i++)
      printf("  class %d %-4s src $%06X  %2u x %5u B -> VRAM $%04X-$%04X\n",
             i, cls[i].tag, cls[i].src24, cls[i].nblk, cls[i].blksz,
             cls[i].vram_lo, cls[i].vram_hi - 1);
  }

  /* --- boot: the ROM's own Reset, stopped at MainLoop ------------------- */
  ef_commit_seen_this_frame = 1;         /* the boot has no COMMIT to precede */
  m_run_to(a_reset, a_mainloop);

  /* The boot must leave a CLEAN slate: GbcClearVram zeroes all 64 KB, the two
     solid tiles go in, CGRAM is zeroed with entry 98 = $7FFF planted, every
     sprite is parked.  Checking it here is what makes the poison below
     meaningful -- and it is the only place the init path is observable. */
  { uint32_t bad = 0xffffffffu, k;
    for(k = 0; k < sizeof(m_vram); k++) {
      uint8_t want = 0x00;
      if(k >= VRAM_TILE768 && k < VRAM_TILE768 + 0x10)
        want = (k & 1) ? 0x00 : 0xff;                              /* tile 768 */
      else if(k >= VRAM_TILE768 + 0x10 && k < VRAM_TILE768 + 0x20)
        want = (k & 1) ? 0xff : 0x00;                              /* tile 769 */
      else if(k >= VRAM_WHITE_MAP && k < VRAM_WHITE_MAP + 0x800)
        want = (k & 1) ? 0x03 : 0x01;    /* {t=769, pal 0, pri 0}, once at init */
      if(m_vram[k] != want) { bad = k; break; }
    }
    if(bad != 0xffffffffu) {
      fprintf(stderr, "FAIL: VRAM apos o boot: $%04X = $%02X (o init nao limpou, "
              "ou os tiles solidos / o mapa branco sairam errado)\n",
              bad, m_vram[bad]);
      fail++;
    }
  }
  { int k, bad = -1; const uint8_t *cg = m_cgram();
    for(k = 0; k < 512; k++) {
      uint8_t want = 0x00;
      if(k == 98 * 2) want = 0xff;            /* CGRAM[98] = $7FFF */
      else if(k == 98 * 2 + 1) want = 0x7f;
      if(cg[k] != want) { bad = k; break; }
    }
    if(bad >= 0) {
      fprintf(stderr, "FAIL: CGRAM apos o boot: [%d] = $%02X (entrada 98 = $7FFF "
              "e' invariante do contrato sec. 13.7)\n", bad, cg[bad]);
      fail++;
    }
  }
  { int k, bad = -1; const uint8_t *oa = m_oam();
    for(k = 0; k < 512; k++) {
      uint8_t want = (k & 3) == 1 ? 0xf0 : 0x00;   /* Y = $F0, everything else 0 */
      if(oa[k] != want) { bad = k; break; }
    }
    for(k = 512; k < 544 && bad < 0; k++) if(oa[k]) bad = k;
    if(bad >= 0) {
      fprintf(stderr, "FAIL: OAM apos o boot: [%d] = $%02X (todo sprite parado em "
              "Y=$F0, tabela alta zerada -- contrato sec. 4.5/13.9)\n",
              bad, oa[bad]);
      fail++;
    }
  }
  memcpy(vram_init, m_vram, sizeof(vram_init));
  /* Every byte of bank $7E the player does NOT own, poisoned.  The working set
     is [$0000,$00FF] direct page, the stack under $2000, the blocks
     GbcHarnessMap names, and the compiler's areas between the log copy and the
     CGRAM copy; the two ranges below are the gaps around all of that, and
     nothing may write in them.  It is the memory-safety half of the hostile
     log test: a digest past 144 rows, a colour queue past 1024 entries or a
     table past its slot all land here.  The $EF write window is carved out:
     the harness build relocates it into this very bank. */
  if(req_wram_quiet) {
    uint32_t k;
    for(k = 0x7e0100; k < a_statcopy; k++)
      if(k < a_ef || k >= a_ef + EF_LEN) m_poke(k, 0x5C);
    for(k = a_cram + view_by_tag("CGRM")->len; k <= 0x7effff; k++)
      if(k < a_ef || k >= a_ef + EF_LEN) m_poke(k, 0x5C);
    /* ⚡ And the TAIL OF EVERY TABLE SLOT, in both sets.  The two ranges above
       are the gaps AROUND the working set, so a table that overruns its slot
       lands in the next slot -- or, from the last one, in the other set, which
       is what the HDMA is reading -- and neither shows up there.  The worst
       case of a well-formed table is 589 B of the 640 a channel slot has and
       295 of the 512 the window-1 slot has (contract sec. 11.4), so the bytes
       past those are memory the compiler may never touch. */
    for(k = 0; k < 2; k++) {
      uint32_t base = (k ? a_setb : a_seta), c;
      for(c = 0; c < 4; c++) {
        uint32_t lo = base + GBC_T_CH0 + c * GBC_T_SLOT + 600, j;
        for(j = lo; j < base + GBC_T_CH0 + (c + 1) * GBC_T_SLOT; j++) m_poke(j, 0x5C);
      }
      for(c = base + GBC_T_W1 + 320; c < base + GBC_T_CH0; c++) m_poke(c, 0x5C);
    }
  }
  /* --poison-log: the whole log area, and the byte past it, full of entries a
     compile would happily place -- BCPD on line 10, every index, as a longer
     log of an earlier frame would have left them.  The player copies only
     LOG_N entries into it each frame, so whatever it reads past them is
     exactly this; what stops the walk is the sentinel it writes itself. */
  if(poison_log) {
    uint32_t k;
    for(k = 0; k < (uint32_t)GBC_LOG_MAX * 4 + 4; k += 4) {
      m_poke(a_log + k, 10); m_poke(a_log + k + 1, 8);
      m_poke(a_log + k + 2, (uint8_t)((k / 4) & 0x3F)); m_poke(a_log + k + 3, (uint8_t)(k / 4));
    }
  }
  m_stack_low_reset();
  ndma_boot = ndma;
  printf("boot: %d DMA(s) (clear de VRAM + tiles solidos), fora do orcamento\n", ndma_boot);

  /* --- poison the destinations ----------------------------------------- */
  /* $A5 in every byte a frame is SUPPOSED to fill, so a byte that never got
     delivered stays visible; everything else keeps what the boot left, which
     is what makes "wrote outside its regions" detectable at all.  Poisoning
     the whole of VRAM instead would destroy the solid tiles, which no frame
     ever rewrites. */
  if(poison) {
    uint32_t k;
    for(i = 0; i < NCLS; i++)
      for(k = cls[i].vram_lo; k < cls[i].vram_hi; k++) m_vram[k] = 0xa5;
    memset(m_cgram(), 0x5a, 512);
    memset(m_oam(), 0x5a, 512);        /* the low table only: the high one is
                                          written once at init and no frame may
                                          touch it again (sec. 4.5) */
    }

  if(ck_mode) {
    if(ck_trace_path && !(ck_trace = fopen(ck_trace_path, "w")))
      die("nao consigo escrever %s", ck_trace_path);
    printf("clock: %d frames a partir de V=%d, LY=0 em V=%d (deriva %.3f linha/frame), "
           "locked=%d, cpurev=%d%s, dirty=%s\n", nframes, ck_start_line, ck_ly0, ck_drift,
           ck_locked < 0 ? (ck_drift == 0.0) : ck_locked, cpurev, pal ? " PAL" : "", dirty_arg);
    if(ck_dtrace_path) {
      FILE *df = fopen(ck_dtrace_path, "rb");
      dirty_t *dt; int n = 0, cap = 4096; uint8_t r[20];
      if(!df) die("nao consigo abrir %s", ck_dtrace_path);
      dt = (dirty_t*)calloc((size_t)cap, sizeof(dirty_t));
      while(fread(r, 1, 20, df) == 20 && r[0] == 'D') {
        if(n == cap) { cap *= 2; dt = (dirty_t*)realloc(dt, (size_t)cap * sizeof(dirty_t)); }
        memcpy(dt[n].chr, r + 1, 6); memcpy(dt[n].obj, r + 7, 4);
        memcpy(dt[n].map, r + 11, 8); dt[n].misc = r[19]; n++;
      }
      fclose(df);
      if(!n) die("%s: nenhum registro 'D'", ck_dtrace_path);
      printf("clock: dirty de %s (%d frames do GB)\n", ck_dtrace_path, n);
      fail += ck_run(nframes, dt, n, &ckrq);
    } else
    fail += ck_run(nframes, dspec, nspecs, &ckrq);
    if(ck_trace) fclose(ck_trace);
    goto ck_done;
  }

  /* --- frames ----------------------------------------------------------- */
  printf("frames=%d v=%d dirty=%s seq=%s\n", nframes, vline, dirty_arg,
         seq_hold ? "hold" : "advance");
  if(flags0_arg) {
    char *b = strdup(flags0_arg), *p2, *t;   /* not a fixed buffer: a long list was
                                                 cut at 256 chars in silence */
    p2 = b;
    while((t = strsep(&p2, ";")) != NULL) {
      if(!*t) continue;
      if(nflags0 >= 64) die("--flags0: no maximo 64 frames");
      flags0v[nflags0++] = (int)strtoul(t, NULL, 0);
    }
  }
  /* The fairness property is about the REGIME, not about the boot: the refresh
     of the first frames touches every class once whatever the rotation does,
     which is exactly how an accumulated mask can "prove" fairness that is not
     there.  So the window only starts counting after two full laps. */
  fair_warm = 2 * NCLS;
  { unsigned classes_moved = 0, back_in = 0;
    int whold_owed = 0;

    for(i = 0; i < nframes; i++) {
      const dirty_t *d = &dspec[i < nspecs ? i : nspecs - 1];
      const char *sname = specs[i < nspecs ? i : nspecs - 1];
      int dma0 = ndma, j;
      cur_frame = i + 1;
      unsigned defer, drops, eq = 0, bytes = 0;
      unsigned n_sync, n_commit, n_consume;

      back_in = backlog_or();
      /* GbcWhiteHold's own notion of "owed", sampled where the player takes
         it: after the previous frame's drain (its frame tail). */
      { uint32_t k; uint8_t o = (uint8_t)(m_peek(0x7e003a) & 0x03);
        for(k = 0x20; k < 0x3a; k++) o |= m_peek(0x7e0000 + k);
        o |= m_peek(0x7e003b) | m_peek(0x7e003c);
        whold_owed = o != 0; }
      views_at_apply(i + 1);
      apply_dirty(d);
      if(nflags0) m_poke(stat_addr + GBC_ST_FLAGS0,
                         (uint8_t)flags0v[i < nflags0 ? i : nflags0 - 1]);
      /* ⚡ THE VIEW HAS TO CHANGE, or a byte-exact comparison cannot tell a
         class that transferred once in frame 1 from one that is up to date:
         both leave VRAM equal to a view that never moved.  One byte of every
         class the spec asks for, at a rotating offset, flipped before the
         frame runs.  The player is only ever asked for blocks the spec has
         already marked dirty, so this is not extra work -- it is the same
         work with content that can be told apart. */
      if(churn_out) {
        int c;
        for(c = 0; c < NCLS; c++) {
          const uint8_t *bm = CLS_INFO[c].dsrc == DSRC_CHR ? d->chr :
                              CLS_INFO[c].dsrc == DSRC_OBJ ? d->obj : d->map;
          uint32_t span = (uint32_t)cls[c].nblk * cls[c].blksz, off;
          int b, want = 0;
          for(b = 0; b < cls[c].nblk && !want; b++) {
            int r = CLS_INFO[c].dbit0 + b;
            want = (bm[r >> 3] >> (r & 7)) & 1;
          }
          if(!want) continue;
          off = (churn_step * 977u + (uint32_t)c * 131u) % span;
          m_poke(cls[c].src24 + off, (uint8_t)(m_peek(cls[c].src24 + off) ^ 0x5A));
        }
        churn_step++;
      }
      if(!seq_hold) {
        uint16_t seq = (uint16_t)(m_peek(stat_addr + GBC_ST_SEQ) |
                                  (m_peek(stat_addr + GBC_ST_SEQ + 1) << 8));
        seq++;
        m_poke(stat_addr + GBC_ST_SEQ, (uint8_t)seq);
        m_poke(stat_addr + GBC_ST_SEQ + 1, (uint8_t)(seq >> 8));
      }
      /* The bridge goes away for the first `ver_late` frames and comes back:
         sec. 10's "ver == $00" degrade, and the recovery that has to re-issue
         GO because GbcLifeWait timed out at boot and never did. */
      if(ver_late)
        m_poke(stat_addr + GBC_ST_VER, i < ver_late ? 0x00 : (uint8_t)image_ver);
      if(no_snap) {
        uint8_t f0 = m_peek(stat_addr + GBC_ST_FLAGS0);
        m_poke(stat_addr + GBC_ST_FLAGS0,
               i < no_snap ? (uint8_t)(f0 & 0x7F) : (uint8_t)(f0 | 0x80));
      }
      m_set_pad(pads[i < npads ? i : npads - 1]);
      n_sync = ef_n[GBC_EFO_SYNC]; n_commit = ef_n[GBC_EFO_COMMIT];
      n_consume = ef_n[GBC_EFO_CONSUME];
      ef_commit_seen_this_frame = 0;
      ef_sync_seen_this_frame = 0;
      ef_consume_seen_this_frame = 0;

      if(req_grp) { int g; const uint32_t *w = m_ppu_reg_writes();
                    for(g = 0; g < 4; g++) grp_w0[g] = w[0x08 + g]; }
      if(opt_interleave) fail += il_frame(a_nmibody, a_rasterrun, i + 1);
      else               m_call(a_frameonce, 0);
      if(req_cgram_view) cgv_hdma_frame();

      /* ⚡ THE BODY MAY NOT WRITE A REGISTER THE HDMA IS DRIVING -- the same
         invariant GbcRegsScroll keeps for the scroll pairs, and the reason
         $73 b4 exists.  The group carries its own value on every line from
         line 1 on, so a CPU write here decides only line 0 (inside the top
         letterbox, not shown) and, on the frame the LCD goes off, undoes the
         $210A = $50 of the white screen.  $73 b4 still set at the end of the
         frame means the group was on the bus for the whole body -- the white
         branch is the only thing that clears it, and that is exactly the case
         this must not check. */
      if(req_grp && (m_peek(0x7e0073) & 0x10)) {
        const uint32_t *w = m_ppu_reg_writes();
        int g;
        for(g = 0; g < 4; g++)
          if(w[0x08 + g] != grp_w0[g]) {
            fprintf(stderr, "FAIL grupo frame %d: o corpo escreveu $21%02X, que "
                    "a HDMA esta' dirigindo ($73 b4) -- o grupo de LCDC carrega "
                    "o proprio valor em toda linha\n", i + 1, 0x08 + g);
            fail++;
          }
      }

      /* ⚡ ERRATA E2, MEASURED PER FRAME.  On a white screen ($210A points at
         the static white map) no channel may be driving $2108-$210B: it would
         write a carpet base over it from line 1 on and the frame would show
         the previous scene instead of white.  The compiler refuses to arm the
         group for a white snapshot -- but a compile only reaches the bus in
         the NEXT frame's prologue, so what is checked here is the FRAME, not
         the compile. */
      /* ⚡ THE WHITE HOLD (gbc_snes.asm, GbcWhiteHold): after the LCD comes
         back the white frame is kept while the previous frame left uploads
         owed, for at most GBC_WHITE_HOLD - 1 frames after F_FIRST, and never
         once a frame has found nothing owed.  "White" is what the white branch
         writes: BG4 alone over the static white map. */
      if(req_whold) {
        uint8_t f0 = m_peek(stat_addr + GBC_ST_FLAGS0);
        int bridge_white = !(f0 & 0x01) || (f0 & 0x20);
        int shown_white = m_ppu_regs()[0x2C] == 0x08 && m_ppu_regs()[0x0A] == 0x50;
        if(bridge_white) whold_since = 0;
        else if(whold_since >= 0) {
          int want;
          whold_since++;
          want = whold_owed && whold_since < GBC_WHITE_HOLD;
          if(want != shown_white) {
            fprintf(stderr, "FAIL segura-branco frame %d: %s, mas %s (%d quadro(s) "
                    "depois do F_FIRST, fila %s)\n", i + 1,
                    want ? "devia seguir branco" : "devia mostrar a cena",
                    shown_white ? "mostrou branco" : "mostrou a cena",
                    whold_since, whold_owed ? "com pendencias" : "vazia");
            fail++;
          }
          if(!want) whold_since = -1;
        }
        printf("segura-branco frame %d: ponte %s, tela %s, fila %s\n", i + 1,
               bridge_white ? "branca" : "cena", shown_white ? "branca" : "cena",
               whold_owed ? "pendente" : "vazia");
      }
      if(req_white) {
        uint8_t f0 = m_peek(stat_addr + GBC_ST_FLAGS0);
        int white = !(f0 & 0x01) || (f0 & 0x20);
        if(white) {
          unsigned mask = m_cpu_regs()[0x0C];
          const uint8_t *dr = m_dma_regs();
          int ch;
          if(m_ppu_regs()[0x0A] != 0x50) {
            fprintf(stderr, "FAIL branco frame %d: $210A = $%02X, esperado $50 "
                    "(BG4 no mapa branco de $A000)\n", i + 1, m_ppu_regs()[0x0A]);
            fail++;
          }
          for(ch = 0; ch < 4; ch++)
            if((mask >> ch) & 1 && dr[ch * 0x10 + 1] == 0x08) {
              fprintf(stderr, "FAIL branco frame %d: ch%d ainda dirige o grupo "
                      "de LCDC ($420C = $%02X) -- ele reescreve $210A em toda "
                      "linha e a tela branca vira um carpete\n",
                      i + 1, ch, mask);
              fail++;
            }
        }
      }
    
      defer = m_peek16(a_ctr + GBC_CTR_DEFER);
      drops = m_peek16(a_ctr + GBC_CTR_DROPS);
      for(j = dma0; j < ndma && j < MAXDMA; j++) {
        bytes += dmas[j].bytes;
        eq += dmas[j].bytes + GBC_DMACOST;
      }

      printf("frame %2d  spec=%-16s dmas=%2d bytes=%6u eq=%6u/%u "
             "defer=%+d drops=%+d  cursors=", i + 1, sname, ndma - dma0, bytes, eq,
             (unsigned)GBC_BUDGET, (int)(defer - prev_defer), (int)(drops - prev_drops));
      for(j = 0; j < NCLS; j++) printf("%s%u", j ? "," : "", m_peek(0x7e0000 + cls[j].cursor_dp));
      printf("\n");
      if(budget_report || verbose)
        for(j = dma0; j < ndma && j < MAXDMA; j++)
          printf("      dma ch%d %-6s src $%06X -> $%04X  %6u B  %6u eq\n",
                 dmas[j].ch, dma_kind(&dmas[j]), dmas[j].src, dmas[j].dest,
                 dmas[j].bytes, dmas[j].bytes + GBC_DMACOST);

      /* Contract sec. 13.5: exactly one of each, every frame.  With
         --interleave the frame ran more than one body and il_frame already
         checked the count against the number it ran. */
      if(!opt_interleave &&
        (ef_n[GBC_EFO_SYNC] != n_sync + 1 ||
         ef_n[GBC_EFO_COMMIT] != n_commit + 1 ||
         ef_n[GBC_EFO_CONSUME] != n_consume + 1)) {
        fprintf(stderr, "FAIL frame %d: strobes SYNC/COMMIT/CONSUMED = %u/%u/%u, "
                "o contrato sec. 13.5 exige 1/1/1 por frame\n", i + 1,
                ef_n[GBC_EFO_SYNC] - n_sync, ef_n[GBC_EFO_COMMIT] - n_commit,
                ef_n[GBC_EFO_CONSUME] - n_consume);
        fail++;
      }
      if(req_defer_first && i == 0 && defer == prev_defer) {
        fprintf(stderr, "FAIL: o frame 1 fechou tudo -- nao ha' carry-over para "
                "provar (o refresh total nao cabe em uma janela)\n");
        fail++;
      }
      if(req_no_defer_last && i == nframes - 1 && defer != prev_defer) {
        fprintf(stderr, "FAIL: o ultimo frame ainda adiou trabalho\n");
        fail++;
      }
      if(req_last_dmas >= 0 && i == nframes - 1 && (ndma - dma0) != req_last_dmas) {
        fprintf(stderr, "FAIL: ultimo frame com %d DMAs, esperado %ld\n",
                ndma - dma0, req_last_dmas);
        fail++;
      }
      for(j = dma0; j < ndma && j < MAXDMA; j++) {
        int c;
        for(c = 0; c < NCLS; c++)
          if(dmas[j].src >= cls[c].src24 &&
             dmas[j].src < cls[c].src24 + (uint32_t)cls[c].nblk * cls[c].blksz)
            classes_moved |= 1u << c;
      }
      /* ⚡ Per class and per FRAME, which is what an accumulated mask cannot
         say.  The class of a block transfer comes from the DESTINATION, which
         is unique -- the source ranges of a duplicate chr class and of the
         class it duplicates overlap. */
      { int c;
        uint32_t fr[NCLS];
        memset(fr, 0, sizeof(fr));
        for(j = dma0; j < ndma && j < MAXDMA; j++) {
          if(dmas[j].bbad != 0x18) continue;
          for(c = 0; c < NCLS; c++)
            if(dmas[j].dest >= cls[c].vram_lo && dmas[j].dest < cls[c].vram_hi)
              fr[c] += dmas[j].bytes;
        }
        for(c = 0; c < NCLS; c++) {
          /* A class with nothing dirty has nothing to transfer, and demanding
             that it move would be measuring the dirty spec and not the
             rotation -- so a frame that does not ask for it counts as
             satisfied.  Which blocks the spec asks for is the same derivation
             check_blocks makes. */
          const uint8_t *bm = CLS_INFO[c].dsrc == DSRC_CHR ? d->chr :
                              CLS_INFO[c].dsrc == DSRC_OBJ ? d->obj : d->map;
          int b, want = 0;
          for(b = 0; b < cls[c].nblk && !want; b++) {
            int r = CLS_INFO[c].dbit0 + b;
            want = (bm[r >> 3] >> (r & 7)) & 1;
          }
          cls_bytes[c] += fr[c];
          if(fr[c] || !want) cls_last[c] = i + 1;
          if(want) cls_asked[c]++;
          if(i + 1 > fair_warm) {
            int gap = (i + 1) - cls_last[c];   /* cls_last 0 = never moved */
            if(gap > cls_gap[c]) cls_gap[c] = gap;
          }
        }
      }
      /* Only the LAST frame, and only if it started with an EMPTY backlog:
         a block carried over from an earlier frame is legitimately drained
         without being dirty now, so the statement has no meaning otherwise. */
      if(req_blocks && i == nframes - 1) {
        if(back_in) {
          fprintf(stderr, "FAIL: --require-blocks-match no frame %d, mas o "
                  "backlog nao estava vazio na entrada -- rode os frames de "
                  "convergencia antes do cenario alvo\n", i + 1);
          fail++;
        } else {
          fail += check_blocks(d, dma0, i + 1);
        }
      }
      prev_defer = defer; prev_drops = drops;
    }

    if(ndma > MAXDMA) {
      fprintf(stderr, "FAIL: %d DMAs, o registro so' guarda %d -- as contagens "
              "por classe e por tipo ficariam incompletas em silencio\n",
              ndma, MAXDMA);
      fail++;
    }
    if(req_classes && classes_moved != (1u << NCLS) - 1) {
      fprintf(stderr, "FAIL: round-robin nao avancou todas as classes "
              "(mascara $%02X de $%02X) -- uma classe esta' faminta\n",
              classes_moved, (1u << NCLS) - 1);
      fail++;
    }
  }

ck_done:
  if(c6_regs) fail += c6_check_regs();
  if(c6_noredund && c6_redundant()) {
    fprintf(stderr, "FAIL c6: %u ROW_DONE(s) de linha ja' liberada esperando a revisita "
            "(o player nao sabe quais linhas estao pendentes)\n", c6_redundant());
    fail++;
  }
  fail += c6_finish(c6_dump, c6_report);
  { extern uint32_t *c6_prof(void);           /* C6_PROF=1: where the passes go */
    uint32_t *pp = c6_prof();
    if(pp) {
      size_t j, k; uint64_t *agg = calloc(nsyms, sizeof(uint64_t)), tot = 0;
      for(k = 0; k < 0x8000; k++) if(pp[k]) {
        uint32_t ad = 0x8000u | (uint32_t)k, ba = 0; size_t best = 0;
        for(j = 0; j < nsyms; j++) if(syms[j].addr <= ad && syms[j].addr >= ba) { ba = syms[j].addr; best = j; }
        agg[best] += pp[k]; tot += pp[k];
      }
      for(j = 0; j < nsyms; j++) if(agg[j] * 100 >= tot) printf("c6prof %-20s %llu\n", syms[j].name, (unsigned long long)agg[j]);
      printf("c6prof total %llu\n", (unsigned long long)tot);
    } }
  if(c6_report || c6_scene)
    printf("c6: $EF0006 x%u, $EF0007 x%u, %u fora de COMMIT..CONSUMED\n",
           c6_n[0], c6_n[1], c6_outside);
  if(c6_outside) {
    fprintf(stderr, "FAIL: %u store(s) em $EF0006/7 fora da janela COMMIT..CONSUMED\n",
            c6_outside);
    fail++;
  }
  /* --- invariants ------------------------------------------------------- */
  if(ef_read_before_commit) {
    fprintf(stderr, "FAIL: uma view foi lida ANTES do COMMIT do frame "
            "(contrato sec. 13.5)\n");
    fail++;
  }
  if(ef_commit_before_sync) {
    fprintf(stderr, "FAIL: COMMIT antes do SYNC -- a ordem de sec. 6/7 e' "
            "SYNC, COMMIT, views, CONSUMED\n");
    fail++;
  }
  if(ef_read_after_consume) {
    fprintf(stderr, "FAIL: uma view foi lida DEPOIS do CONSUMED (sec. 7: a "
            "janela de leitura do frame fecha ali)\n");
    fail++;
  }
  if(req_go >= 0 && (long)ef_n[GBC_EFO_GO] != req_go) {
    fprintf(stderr, "FAIL: %u strobe(s) de GO, esperado %ld (sec. 10: GO e' "
            "idempotente, mas uma bridge que aparece tarde PRECISA de um)\n",
            ef_n[GBC_EFO_GO], req_go);
    fail++;
  }
  if(req_mcu_cmd >= 0) {
    if(m_cmd_writes() != 1 || mcu_cmd_last != (uint8_t)req_mcu_cmd) {
      fprintf(stderr, "FAIL: MCU_CMD = %u escrita(s), ultima $%02X; esperado "
              "1 x $%02X (combo de IGR)\n", m_cmd_writes(), mcu_cmd_last,
              (unsigned)req_mcu_cmd);
      fail++;
    }
  } else if(m_cmd_writes()) {
    fprintf(stderr, "FAIL: %u escrita(s) em $2A00 (MCU_CMD) sem combo de IGR\n",
            m_cmd_writes());
    fail++;
  }
  if(stray[0]) {
    fprintf(stderr, "FAIL: transferencia fora da classe -- %s\n", stray);
    fail++;
  }

  if(req_420c >= 0 && m_cpu_regs()[0x0C] != (uint8_t)req_420c) {
    fprintf(stderr, "FAIL: $420C = $%02X, esperado $%02X -- os canais que o "
            "prologo pos no barramento nao sao os esperados\n",
            m_cpu_regs()[0x0C], (unsigned)req_420c);
    fail++;
  }

  if(req_wram_quiet) {
    uint32_t k, bad = 0xffffffffu, n = 0;
    uint16_t floor = (uint16_t)(a_snap + GBC_SNAP_LEN);
    for(k = 0x7e0100; k < a_statcopy; k++)
      if((k < a_ef || k >= a_ef + EF_LEN) && m_peek(k) != 0x5C)
        { n++; if(bad == 0xffffffffu) bad = k; }
    for(k = a_cram + view_by_tag("CGRM")->len; k <= 0x7effff; k++)
      if((k < a_ef || k >= a_ef + EF_LEN) && m_peek(k) != 0x5C)
        { n++; if(bad == 0xffffffffu) bad = k; }
    for(k = 0; k < 2; k++) {
      uint32_t base = (k ? a_setb : a_seta), c;
      for(c = 0; c < 4; c++) {
        uint32_t lo = base + GBC_T_CH0 + c * GBC_T_SLOT + 600, j;
        for(j = lo; j < base + GBC_T_CH0 + (c + 1) * GBC_T_SLOT; j++)
          if(m_peek(j) != 0x5C) { n++; if(bad == 0xffffffffu) bad = j; }
      }
      for(c = base + GBC_T_W1 + 320; c < base + GBC_T_CH0; c++)
        if(m_peek(c) != 0x5C) { n++; if(bad == 0xffffffffu) bad = c; }
    }
    if(bad != 0xffffffffu) {
      fprintf(stderr, "FAIL: %u byte(s) escritos FORA das areas do player (ou "
              "passando do fim de um slot de tabela) -- o primeiro e' "
              "$%06X = $%02X\n", n, bad, m_peek(bad));
      fail++;
    }
    /* And the stack, which sits directly on top of the private snapshot: the
       compiler is shallow ON PURPOSE (everything that has to survive a call
       lives in the direct page), so a chain that reaches down here is a
       recursion the contract's log shapes were not supposed to be able to
       cause. */
    printf("wram: fundo da pilha $%04X, piso $%04X (fim do snapshot privado)\n",
           m_stack_low(), floor);
    if(m_stack_low() < floor) {
      fprintf(stderr, "FAIL: a pilha desceu a $%04X, por cima do conjunto de "
              "trabalho que termina em $%04X\n", m_stack_low(), floor);
      fail++;
    }
  }

  /* --- sec. 11.1: the tail is worth 168 - (C + B) bytes a line -------------
     ch5 costs C+B = 2 and ch4 3 -- the two the letterbox and the window always
     put on the bus -- so with none of ch0..ch3 armed a line carries
     !GBC_BPLHDMA, and EVERY ch0..ch3 the raster compiler arms takes 5 more
     (one channel, four bytes).  GbcHpBplTab / GbcHpTailTab are that arithmetic
     as a table, and GbcCapacity divides the window with what the prologue
     copied out of them, so a table that does not move with the published mask
     hands the V guard a ceiling that is too high -- and the last DMA of an
     overloaded frame finishes past the deadline, silently.

     Read out of the IMAGE (the symbols come from the .map) and out of the
     direct-page block GbcHarnessMap exports, so nothing here is a number this
     file believes on its own. */
  /* --- THE 5A22 TAKES THE LOWEST HDMA CHANNEL (gbc_snes.asm) -------------
     ch0 is a decoy that must be armed whenever anything is, and must stay the
     decoy: mode 0, one byte to $21FF, its own table.  The interpreter keeps
     the logical layout for every other check and counts the physical $420C
     writes that broke the rule. */
  if(m_decoy_bad()) {
    fprintf(stderr, "FAIL decoy: %u escrita(s) de $420C armaram canais com o "
            "mais baixo fora de ch0 (a isca) / ch4 (a janela constante do FB), "
            "ou com o ch7 (o canal do DMA geral)\n", m_decoy_bad());
    fail++;
  }
  if(m_phys_420c() & 0x01) {
    const uint8_t *d = m_decoy_regs();
    uint32_t want = sym("GbcTblDecoy");
    if(d[0] != 0x00 || d[1] != 0xFF || d[4] != (uint8_t)(want >> 16) ||
       (uint32_t)(d[2] | d[3] << 8) != (want & 0xFFFF)) {
      fprintf(stderr, "FAIL decoy: ch0 = DMAP $%02X BBAD $%02X A1 $%02X:%04X, "
              "quer $00 $FF $%06X\n", d[0], d[1], d[4], d[2] | d[3] << 8, want);
      fail++;
    }
  }
  if(req_hdma_budget) {
    uint32_t tb = sym("GbcHpBplTab"), tt = sym("GbcHpTailTab");
    unsigned mask = m_cpu_regs()[0x0C], n = 0, k, roles = 0;
    unsigned bpl = m_peek(a_dphdma), armed = m_peek(a_dphdma + 3);
    unsigned tail = m_peek16(a_dphdma + 1);
    for(k = 0; k < 5; k++) {
      unsigned gb = rom8(tb + k), gt = rom16(tt + k * 2);
      unsigned wb = (unsigned)GBC_BPLHDMA - 5 * k, wt = wb * (unsigned)GBC_DEADLINE;
      if(gb != wb || gt != wt) {
        fprintf(stderr, "FAIL orcamento: com %u de ch0..ch3 armados a tabela diz "
                "%u B/linha e cauda %u; 168 - (C + B) = %u e %u x %u = %u\n",
                k, gb, gt, wb, (unsigned)GBC_DEADLINE, wb, wt);
        fail++;
      }
    }
    /* And the ROLES byte, which is what GbcRegsScroll and GbcDrainCgram gate
       on: b0 = ch0/ch1 drive the BG scroll pair, b1 = ch2 drives BG1 (the
       window's content), b2 = a channel is writing CGRAM, b3 = ch3 drives BG3
       (its carpet), b4 = a channel is driving the LCDC group.  The GB
       background is BG2+BG4 ($210F and $2113 in mode 3, two registers written
       twice), so it shows up as its two BBADs; ⚡ the window's two halves are
       SEPARATE bits, because the LCDC group can take ch3 and leave ch2.
       Derived here from the channel registers the prologue actually
       programmed, so a role published without the channel -- or a channel
       armed without its role -- is a failure. */
    for(k = 0; k < 4; k++) {
      unsigned bbad;
      if(!(mask & (1u << k))) continue;
      n++;
      bbad = m_dma_regs()[k * 0x10 + 1];
      if(bbad == 0x0F || bbad == 0x13)      roles |= 0x01;   /* BG2 / BG4 */
      else if(bbad == 0x0D)                 roles |= 0x02;   /* BG1: window content */
      else if(bbad == 0x11)                 roles |= 0x08;   /* BG3: window carpet */
      else if(bbad == 0x21)                 roles |= 0x04;   /* CGRAM */
      else if(bbad == 0x08)                 roles |= 0x10;   /* $2108-$210B, LCDC */
      else {
        fprintf(stderr, "FAIL orcamento: ch%u armado com BBAD $%02X, que nao e' "
                "scroll ($0F/$13), janela ($0D/$11), cor ($21) nem LCDC ($08)\n",
                k, bbad);
        fail++;
      }
    }
    if(armed != roles) {
      fprintf(stderr, "FAIL orcamento: $73 (papeis publicados) = $%02X, os "
              "canais de $420C = $%02X dizem $%02X\n", armed, mask, roles);
      fail++;
    }
    if(bpl != (unsigned)GBC_BPLHDMA - 5 * n ||
       tail != bpl * (unsigned)GBC_DEADLINE) {
      fprintf(stderr, "FAIL orcamento: $420C = $%02X (%u canais), mas o prologo "
              "publicou %u B/linha e cauda %u -- esperado %u e %u\n",
              mask, n, bpl, tail, (unsigned)GBC_BPLHDMA - 5 * n,
              ((unsigned)GBC_BPLHDMA - 5 * n) * (unsigned)GBC_DEADLINE);
      fail++;
    }
    printf("orcamento: $420C = $%02X, %u de ch0..ch3 armados, papeis $%02X, "
           "%u B/linha, cauda %u\n", mask, n, armed, bpl, tail);
  }
  /* ⚡ THE $8800 BLOCK GOES TO BOTH CHR BASES, AND ITS BACKLOG MAY ONLY CLEAR
     WHEN BOTH TRANSFERS HAVE GONE OUT.  That is what having a SEPARATE class,
     with its own backlog byte, is for: if the budget runs out between the two,
     the second one is an ordinary deferral and lands in the next frame.  A
     player that let one bit stand for both copies would clear it on the first
     DMA and leave base B holding the PREVIOUS tiles -- silently, because the
     two copies are byte-identical whenever nothing changed.
     Both halves of the statement are required: the copies agree at the end,
     AND at least one pair really was split across two frames (otherwise the
     scenario proved nothing and the case is vacuous). */
  /* ⚡ FAIRNESS OF THE ROTATION, MEASURED IN THE REGIME.
     The property the player states: with every class permanently dirty, EVERY
     class transfers more than zero bytes in ANY window of K frames.  It is
     checked here as "the longest run of consecutive frames in which a class
     moved nothing, after the warm-up, is at most K" -- which is the same
     statement and is what an accumulated "did it ever move" mask cannot say:
     the boot refresh sets every bit of that mask in the first two frames and
     it is never cleared again, so it proves a boot event and not a regime. */
  /* The churned views, in the same GBVW the harness reads, so the byte-exact
     comparison runs against what the bridge is publishing NOW and not against
     the file the run started from. */
  if(churn_out) {
    FILE *f = fopen(churn_out, "wb");
    int v;
    if(!f) die("nao consigo escrever %s", churn_out);
    fwrite("GBVW", 1, 4, f); put32(f, 1);
    for(v = 0; v < NVIEWS; v++) {
      uint8_t *b = (uint8_t*)malloc(VIEWS[v].len);
      uint32_t k;
      for(k = 0; k < VIEWS[v].len; k++) b[k] = m_peek(VIEWS[v].addr + k);
      put_sec(f, VIEWS[v].tag, b, VIEWS[v].len);
      free(b);
    }
    fclose(f);
    printf("views churned: %u passada(s) -> %s\n", churn_step, churn_out);
  }

  if(fair_k) {
    int c, bad = 0;
    printf("justica: K = %d frames, aquecimento %d, %d frames medidos\n",
           fair_k, fair_warm, nframes - fair_warm);
    printf("  %-6s %12s %8s %8s\n", "classe", "bytes", "pedidos", "pior gap");
    for(c = 0; c < NCLS; c++) {
      printf("  %-6d %12u %8d %8d%s\n", c, cls_bytes[c], cls_asked[c],
             cls_gap[c], cls_gap[c] > fair_k ? "   <-- STARVED" : "");
      if(cls_gap[c] > fair_k) bad++;
    }
    if(nframes <= fair_warm + fair_k) {
      fprintf(stderr, "FAIL justica: %d frames nao chegam para medir uma janela "
              "de %d depois do aquecimento de %d\n", nframes, fair_k, fair_warm);
      fail++;
    }
    if(bad) {
      fprintf(stderr, "FAIL justica: %d classe(s) passaram mais de %d frames sem "
              "transferir nada com TUDO sujo -- a rotacao esta' starvando\n",
              bad, fair_k);
      fail++;
    }
  }

  if(req_dup) {
    int d, a, pairs = 0, split = 0;
    for(d = 0; d < NCLS; d++) {
      uint32_t dlo = cls[d].src24, dn = (uint32_t)cls[d].nblk * cls[d].blksz;
      for(a = 0; a < NCLS; a++) {
        uint32_t alo, an, mirror; int fa = -1, fd = -1, j;
        if(a == d) continue;
        alo = cls[a].src24; an = (uint32_t)cls[a].nblk * cls[a].blksz;
        if(cls[a].blksz != cls[d].blksz || dlo < alo || dlo + dn > alo + an) continue;
        mirror = cls[a].vram_lo + (dlo - alo);
        pairs++;
        /* OVERLAP, not containment: the base-A transfer of these blocks is
           normally part of a longer run that starts earlier. */
        for(j = ndma_boot; j < ndma && j < MAXDMA; j++) {
          uint32_t lo = dmas[j].dest, hi = lo + dmas[j].bytes;
          if(dmas[j].bbad != 0x18) continue;
          if(lo < cls[d].vram_lo + dn && hi > cls[d].vram_lo) fd = dmas[j].frame;
          if(lo < mirror + dn && hi > mirror) fa = dmas[j].frame;
        }
        if(fd < 0) {
          fprintf(stderr, "FAIL duplicata: a classe %d (VRAM $%04X) nunca foi "
                  "transferida -- a copia em $%04X ficou com o que estava la'\n",
                  d, (unsigned)cls[d].vram_lo, (unsigned)cls[d].vram_lo);
          fail++;
        }
        if(memcmp(m_vram + cls[d].vram_lo, m_vram + mirror, dn)) {
          uint32_t k;
          for(k = 0; k < dn; k++)
            if(m_vram[cls[d].vram_lo + k] != m_vram[mirror + k]) break;
          fprintf(stderr, "FAIL duplicata: VRAM $%04X e $%04X divergem em +$%04X "
                  "($%02X vs $%02X) -- as duas bases tem que ver o MESMO bloco "
                  "$8800\n", (unsigned)cls[d].vram_lo, (unsigned)mirror,
                  (unsigned)k, m_vram[cls[d].vram_lo + k], m_vram[mirror + k]);
          fail++;
        }
        if(fd >= 0 && fa >= 0 && fd != fa) split++;
        printf("duplicata: VRAM $%04X (frame %d) == $%04X (frame %d), %u B\n",
               (unsigned)cls[d].vram_lo, fd, (unsigned)mirror, fa, dn);
      }
    }
    if(pairs != 2) {
      fprintf(stderr, "FAIL duplicata: %d par(es) derivado(s) de GbcClsTab, "
              "esperado 2 (o bloco $8800 de cada banco)\n", pairs);
      fail++;
    }
    if(!split) {
      fprintf(stderr, "FAIL duplicata: os dois DMAs de um par sairam sempre no "
              "MESMO frame -- o cenario nao exercitou o adiamento entre eles e "
              "nao prova nada\n");
      fail++;
    }
  }

  { uint32_t k, bad = 0xffffffffu;
    for(k = 0; k < sizeof(m_vram); k++)
      if(!vram_is_class_dest(k) && m_vram[k] != vram_init[k] &&
         !(c6_enabled() && k >= 0xA000 && k < 0xFEC0 && (k < 0xB000 || k >= 0xB040)) &&
         !(c6_enabled() && c6_ever_stretched() && k >= 0x4000 && k < 0xD480))
        { bad = k; break; }   /* ⚡ wire $03: the FB footprint (sec. 14.8), which
                                 gbc_c6.c checks byte for byte at every ROW_DONE */
    if(bad != 0xffffffffu) {
      fprintf(stderr, "FAIL: escrita FORA das regioes das classes: VRAM $%04X "
              "= $%02X, o boot deixou $%02X\n", bad, m_vram[bad], vram_init[bad]);
      fail++;
    }
  }
  { int k, bad = -1; const uint8_t *oa = m_oam();
    for(k = 512; k < 544; k++) if(oa[k] != 0x00) bad = k;
    if(bad >= 0) {
      fprintf(stderr, "FAIL: a tabela alta de OAM foi reescrita ([%d] = $%02X); "
              "o contrato sec. 4.5 diz UMA vez, no init\n", bad, oa[bad]);
      fail++;
    }
  }

  /* --- the totals ------------------------------------------------------- */
  if(!ck_mode) { unsigned frames = m_peek16(a_ctr + GBC_CTR_FRAMES);
    unsigned pdmas  = m_peek16(a_ctr + GBC_CTR_DMAS);
    uint32_t pbytes = (uint32_t)m_peek16(a_ctr + GBC_CTR_BYTES) |
                      ((uint32_t)m_peek16(a_ctr + GBC_CTR_BYTES + 2) << 16);
    unsigned defer  = m_peek16(a_ctr + GBC_CTR_DEFER);
    unsigned drops  = m_peek16(a_ctr + GBC_CTR_DROPS);
    unsigned ready  = m_peek16(a_ctr + GBC_CTR_READY);
    unsigned back = backlog_or();

    printf("counters: frames=%u dmas=%u bytes=%u defer=%u drops=%u ready_frame=%u "
           "backlog=%s\n", frames, pdmas, pbytes, defer, drops, ready,
           back ? "PENDING" : "empty");

    /* Evidence the path REALLY ran, in the NES gate's sense: a run that
       silently did nothing leaves the same "no diff outside the regions". */
    if(frames != (unsigned)nframes + il_bodies) {
      fprintf(stderr, "FAIL: o player contou %u frames, o driver chamou %u\n",
              frames, (unsigned)nframes + il_bodies);
      fail++;
    }
    if(opt_interleave) {
      printf("interleave: %ld instrucoes por fatia, %u corpo(s) injetado(s) "
             "durante uma compilacao em voo\n", opt_interleave, il_injections);
      if(!il_injections) {
        fprintf(stderr, "FAIL: --interleave nao injetou UM corpo dentro de uma "
                "compilacao (%u ponto(s) de suspensao examinado(s)) -- a fatia e' "
                "grande demais, ou nao ha' compilacao nenhuma neste caso\n",
                il_checked);
        fail++;
      }
    }
    if(req_total_dmas >= 0 && (long)pdmas != req_total_dmas) {
      fprintf(stderr, "FAIL: %u DMAs no total, esperado %ld\n", pdmas, req_total_dmas);
      fail++;
    }
    if(req_no_view_dma) {
      int j, n = 0;
      for(j = ndma_boot; j < ndma && j < MAXDMA; j++) {
        const char *k2 = dma_kind(&dmas[j]);
        if(strcmp(k2, "status") && strcmp(k2, "rom")) n++;
      }
      if(n) { fprintf(stderr, "FAIL: %d DMA(s) de view onde nenhuma era esperada\n", n); fail++; }
    }
    { long ncg = 0, noam = 0; int j;
      for(j = ndma_boot; j < ndma && j < MAXDMA; j++) {
        if(dmas[j].bbad == 0x22) ncg++;
        else if(dmas[j].bbad == 0x04) noam++;
      }
      printf("kinds: cgram=%ld oam=%ld\n", ncg, noam);
      if(req_cgram_dmas >= 0 && ncg != req_cgram_dmas) {
        fprintf(stderr, "FAIL: %ld DMA(s) de CGRAM, esperado %ld\n", ncg, req_cgram_dmas);
        fail++;
      }
      if(req_oam_dmas >= 0 && noam != req_oam_dmas) {
        fprintf(stderr, "FAIL: %ld DMA(s) de OAM, esperado %ld\n", noam, req_oam_dmas);
        fail++;
      }
    }
    if(req_drops_min >= 0 && (long)drops < req_drops_min) {
      fprintf(stderr, "FAIL: %u recusas da guarda de V, esperado >= %ld\n",
              drops, req_drops_min);
      fail++;
    }
    if(req_converged) {
      if(back) { fprintf(stderr, "FAIL: o backlog nao esvaziou\n"); fail++; }
      if(!ready) {
        fprintf(stderr, "FAIL: a tela nunca foi liberada (ready_frame = 0): "
                "CGRAM + OAM + os quatro mapas nao fecharam\n");
        fail++;
      }
      if(!pdmas) { fprintf(stderr, "FAIL: nenhum DMA foi disparado\n"); fail++; }
    }
    if(budget_report) {
      unsigned tot_eq = 0; int j, n = 0;
      for(j = ndma_boot; j < ndma && j < MAXDMA; j++) {
        tot_eq += dmas[j].bytes + GBC_DMACOST; n++;
      }
      printf("budget: %u eq em %d DMAs de frame (%u eq/frame medios, teto %u/frame)\n",
             tot_eq, n, nframes ? tot_eq / (unsigned)nframes : 0, (unsigned)GBC_BUDGET);
    }
  }

  if(req_sets_equal) fail += hsets_differ();
  if(req_no_colour) fail += hsets_colour();
  if(req_cgram_view) fail += cgv_verdict();
  /* --require-raster-idle: a scene whose log is empty and whose window
     registers stand still owes nothing once both sets carry its tables, so
     the compiler has to be back at rest: $80 = 0 (no compile armed, running
     or waiting to be published) and $82 = 0 (no set owed).  A debt that is
     never paid still draws the right picture -- it just recompiles every
     frame for ever, which only this sees. */
  if(req_raster_idle && (m_peek(0x7e0080) || m_peek(0x7e0082))) {
    fprintf(stderr, "FAIL raster: o compilador nao voltou ao repouso numa cena parada "
            "($80 = $%02X, $82 = $%02X) -- recompila todo frame\n",
            m_peek(0x7e0080), m_peek(0x7e0082));
    fail++;
  }
  fail += mbx_verdict();
  if(prof_path) prof_report();
  if(wram_path) {                 /* debug: bank $7E as the run left it */
    FILE *wf = fopen(wram_path, "wb"); uint32_t k;
    if(!wf) die("--dump-wram: %s", wram_path);
    for(k = 0x7e0000; k < 0x7f0000; k++) fputc(m_peek(k), wf);
    fclose(wf);
  }
  if(dump_path) dump_snes(dump_path);
  if(fail) fprintf(stderr, "%d falha(s)\n", fail);
  return fail ? 1 : 0;
}
