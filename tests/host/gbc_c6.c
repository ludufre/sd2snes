/* gbc_c6.c -- the C6 half of the bridge (GBC-CORE-CONTRACT sec. 14, wire $03)
 * as the offline harness plays it around the REAL player (gbc_render_cli.c,
 * --clock --c6-scene).
 *
 * WHAT IS MODELLED, and where it comes from:
 *   * the row machine of sec. 14.6.1-4: a cell row freezes on the last word of
 *     its scan 8r+7, thaws on the pixel (0, 8r) of its next visit when it got a
 *     ROW_DONE and HOLD is not in force, a frozen row's pixels are dropped and
 *     counted; FB_EN and HOLD are latched at TAP_LY0 (sec. 14.2, advisor A4);
 *   * the time base of C6-SPEC c.1, NORMATIVE for this model and for the
 *     golden's freeze_model.py alike: pixel (x, y) of a Game Boy frame lands
 *     ((456 y + 92 + x) / 456) * 1.7011 SNES lines after that frame's LY=0;
 *   * the dirty compare (a cell is dirty when one of its eight words DIFFERS
 *     from the word it replaced), the half-up quantiser of sec. 14.4 with the
 *     CL in force when the row is written, the per-frame CL vote of sec. 14.6.5
 *     and its switch at TAP_LY0 with C6_CL_HYST = 4 (sec. 14.6.6), ROW_CLID;
 *   * the status bytes +58..+91 LIVE (sec. 14.5): they are rewritten in the
 *     STAT image the moment the machine moves, and the m65816 read watch makes
 *     the machine catch up to "now" before any read of them or of bank $E4;
 *   * the $E4 view (sec. 14.3): 8bpp tiles, stride 32, bit 7 = left pixel,
 *     the [0,0,1,2,2,3,4,4] stretch -- served from the FB content a row froze
 *     with, as the IMAGE OF THE BANK (C6-SPEC c.1: e4_ packed -> (tr<<11)|(tc<<6)).
 *
 * WHAT ABORTS THE RUN (a player bug the picture might hide):
 *   * any DMA into VRAM $B000-$B03F (the solid tiles the quad-layer shows);
 *   * a DMA out of $E4 that is not whole tiles of one row going to k(c);
 *   * ROW_DONE of a row that is not frozen (c6_rowdone_bad), or of a value
 *     that is neither 0..17 nor $FF;
 *   * ROW_DONE of a row with a dirty cell not uploaded since it froze
 *     (invariant 40), or whose VRAM (tiles at k(c), the 20 tilemap entries
 *     {k(c), CL(r)}) does not hold the frozen content.
 *
 * THE SCENE (--c6-scene FILE): one line per change, from a Game Boy frame on
 *   <gbframe> key=value ...
 *     img=PATH      160x144 u16 LE, R bits 0-4, G 5-9, B 10-14 (the .b15)
 *     fill=R,G,B    a flat picture, 5-bit channels
 *     alt=PATH      the odd frames of this line show PATH instead (two-frame
 *                   animation: DKC's menu gradient, 349-356 cells a frame)
 *     scroll=DX     the picture moves DX pixels a frame (wraps)
 *     storm=1       every cell changes every frame (R = 4 * phase)
 *     stormrows=A-B ... only the cells of rows A..B (a moving band)
 *     colw=N        COLW_N the bridge publishes with the snapshot
 *     ovf=0|1 lcd=0|1 compat=0|1 first=0|1   flags0 bits of the snapshot;
 *                   lcd=0 also stops the PPU: no thaw, no freeze, no vote
 *     stale=1       (first line only) the bridge comes up with FB_EN = 1 IN
 *                   FORCE from an earlier session -- a console reset that
 *                   reboots the player but not the bridge: the first restart
 *                   is that session's, and its dirty bits were all sent then
 * Keys not given keep the previous line's value.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "m65816.h"
#include "gbc_c6.h"

#ifndef GBC_ST_C6FLAGS
#error "defina GBC_ST_C6FLAGS via -D"
#endif
#ifndef GBC_ST_ROWF
#error "defina GBC_ST_ROWF via -D"
#endif
#ifndef GBC_ST_C6CL
#error "defina GBC_ST_C6CL via -D"
#endif
#ifndef GBC_ST_C6DIRTY
#error "defina GBC_ST_C6DIRTY via -D"
#endif
#ifndef GBC_ST_RCLI
#error "defina GBC_ST_RCLI via -D"
#endif
#ifndef GBC_ST_COLWN
#error "defina GBC_ST_COLWN via -D"
#endif
#ifndef GBC_EFO_C6CTL
#error "defina GBC_EFO_C6CTL via -D"
#endif
#ifndef GBC_EFO_ROWDONE
#error "defina GBC_EFO_ROWDONE via -D"
#endif

#define ROWS 18
#define COLS 20
#define CELLS 360
#define W 160
#define H 144
#define LINE_MC 1364.0
#define GB_LINE (1.7011 * LINE_MC)      /* C6-SPEC c.1 */
#define CL_HYST 4
#define MAXSNES 4096
#define MAXGB   4096

static int on;
static uint32_t st;                    /* STAT view in the harness build */
static uint8_t *e4;                    /* bank $E4, 64 KB */
static uint32_t a_fb;                  /* the player's FB block (WRAM) */

static void c6_die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#include <stdarg.h>
static void c6_die(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "FAIL c6 (V=%d): ", m_clock_line());
  vfprintf(stderr, fmt, ap); va_end(ap);
  fputc('\n', stderr);
  exit(2);
}

/* ---------------- the scene ---------------- */
typedef struct {
  int from;
  uint16_t *img;                       /* NULL = keep */
  uint16_t *alt;                       /* odd frames of the line show this */
  int scroll, storm, colw, ovf, lcd, compat, first, s_r0, s_r1;
} sc_t;
static sc_t sc[256];
static int stale_boot;                  /* scene "stale=1" (see the header) */
static int nsc;

static uint16_t *load_b15(const char *p) {
  FILE *f = fopen(p, "rb");
  uint16_t *b = (uint16_t*)malloc(W * H * 2);
  uint8_t raw[W * H * 2];
  int i;
  if(!f) c6_die("--c6-scene: nao consigo abrir %s", p);
  if(fread(raw, 1, sizeof raw, f) != sizeof raw) c6_die("%s: nao tem 46080 B", p);
  fclose(f);
  for(i = 0; i < W * H; i++) b[i] = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
  return b;
}

static void scene_load(const char *path) {
  FILE *f = fopen(path, "r");
  char line[1024];
  sc_t cur;
  memset(&cur, 0, sizeof cur);
  cur.lcd = 1;
  if(!f) c6_die("nao consigo abrir a cena %s", path);
  while(fgets(line, sizeof line, f)) {
    char *p = line, *tok;
    int fr;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == '#' || *p == '\n' || !*p) continue;
    if(nsc >= 256) c6_die("cena: mais de 256 linhas");
    fr = (int)strtol(p, &p, 0);
    cur.from = fr;
    cur.img = NULL;
    cur.alt = NULL;
    while((tok = strtok(p, " \t\n")) != NULL) {
      char *eq = strchr(tok, '=');
      p = NULL;
      if(!eq) c6_die("cena: '%s' sem '='", tok);
      *eq++ = 0;
      if(!strcmp(tok, "img")) {
        /* relative to the scene file */
        char full[1024];
        const char *sl = strrchr(path, '/');
        if(eq[0] != '/' && sl) snprintf(full, sizeof full, "%.*s/%s", (int)(sl - path), path, eq);
        else snprintf(full, sizeof full, "%s", eq);
        cur.img = load_b15(full);
      } else if(!strcmp(tok, "fill")) {
        int r, g, b, i;
        if(sscanf(eq, "%d,%d,%d", &r, &g, &b) != 3) c6_die("cena: fill=R,G,B");
        cur.img = (uint16_t*)malloc(W * H * 2);
        for(i = 0; i < W * H; i++) cur.img[i] = (uint16_t)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10));
      }
      else if(!strcmp(tok, "alt")) {
        char full[1024];
        const char *sl = strrchr(path, '/');
        if(eq[0] != '/' && sl) snprintf(full, sizeof full, "%.*s/%s", (int)(sl - path), path, eq);
        else snprintf(full, sizeof full, "%s", eq);
        cur.alt = load_b15(full);
      }
      else if(!strcmp(tok, "scroll")) cur.scroll = atoi(eq);
      else if(!strcmp(tok, "storm")) { cur.storm = atoi(eq); cur.s_r0 = 0; cur.s_r1 = 17; }
      else if(!strcmp(tok, "stormrows")) {
        if(sscanf(eq, "%d-%d", &cur.s_r0, &cur.s_r1) != 2) c6_die("cena: stormrows=A-B");
        cur.storm = 1;
      }
      else if(!strcmp(tok, "colw")) cur.colw = atoi(eq);
      else if(!strcmp(tok, "ovf")) cur.ovf = atoi(eq);
      else if(!strcmp(tok, "lcd")) cur.lcd = atoi(eq);
      else if(!strcmp(tok, "compat")) cur.compat = atoi(eq);
      else if(!strcmp(tok, "first")) cur.first = atoi(eq);
      else if(!strcmp(tok, "stale")) stale_boot = atoi(eq);
      else c6_die("cena: chave desconhecida '%s'", tok);
    }
    if(!cur.img) {
      int j;
      for(j = nsc - 1; j >= 0 && !cur.img; j--) cur.img = sc[j].img;
    }
    if(!cur.img) c6_die("cena: a primeira linha precisa de img= ou fill=");
    sc[nsc++] = cur;
  }
  fclose(f);
  if(!nsc) c6_die("cena %s vazia", path);
}

static const sc_t *scene_at(int k) {
  int i; const sc_t *s = &sc[0];
  for(i = 0; i < nsc; i++) if(sc[i].from <= k) s = &sc[i];
  return s;
}

static int scene_start(int k) {         /* the frame the current line began */
  int i, f = 0;
  for(i = 0; i < nsc; i++) if(sc[i].from <= k) f = sc[i].from;
  return f;
}

/* The Game Boy's 15-bit picture of frame k. */
static void picture(int k, uint16_t *out) {
  const sc_t *s = scene_at(k);
  int x, y, t = k - scene_start(k);
  for(y = 0; y < H; y++)
    for(x = 0; x < W; x++) {
      int sx = ((x + t * s->scroll) % W + W) % W;
      uint16_t v = ((s->alt && (t & 1)) ? s->alt : s->img)[y * W + sx];
      if(s->storm && (y >> 3) >= s->s_r0 && (y >> 3) <= s->s_r1) v = (uint16_t)((v & ~31) | (4 * ((k + (x >> 3) + (y >> 3)) & 7)));
      out[y * W + x] = v;
    }
}

/* sec. 14.4, half-up. */
static uint8_t quant(uint16_t c, int cl) {
  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  int R = (r + 2 - 2 * (cl & 1)) >> 2, G = (g + 2 - 2 * ((cl >> 1) & 1)) >> 2;
  int B = (b + 4 - 4 * ((cl >> 2) & 1)) >> 3;
  if(R > 7) R = 7;
  if(G > 7) G = 7;
  if(B > 3) B = 3;
  return (uint8_t)((B << 6) | (G << 3) | R);
}

/* sec. 14.6.5 */
static int vote_rg(int c) { if((c & 3) == 0) return -1; if((c & 3) == 2 || c == 31) return 1; return 0; }
static int vote_b(int c) {
  int l = c & 7;
  if(c >= 30) return 1;
  if(l == 3 || l == 4 || l == 5) return 1;
  if(l == 0 || l == 1 || l == 7) return -1;
  return 0;
}

/* ---------------- the machine ---------------- */
static uint8_t  ctl;                    /* last $EF0006 */
static int      fb_active, hold_active;
static int      frozen[ROWS], thaw_pend[ROWS], writing[ROWS], row_clid[ROWS];
static int      content_k[ROWS], vram_k[ROWS];
static uint8_t  dirty[45];
static uint8_t  sent[CELLS];
static uint8_t  sentS[ROWS][32];      /* second stage: stretched columns sent */
static int      cl_cur, cl_prev, cl_id, cl_cand, cl_hyst;
static unsigned drop_row[ROWS];
static unsigned rowdone_redundant;   /* ROW_DONE of a row already released */
static unsigned c6_frames, rows_dropped, rowdone_bad, cl_switches, rowdone_n;
static uint8_t  fbmem[W * H];
static uint16_t pic[W * H];             /* the frame being scanned */
static int      pic_k = -1;
static int      gbk = -1;               /* the last LY=0's frame */
static uint16_t colw_pub;

/* events */
typedef struct { uint64_t t; int kind, r, k; } ev_t;   /* kind 0 thaw 1 freeze 2 vblank */
#define EVQ 256
static ev_t evq[EVQ];
static int ev_h, ev_n;

/* records */
static int snes_n;                      /* SNES frame index (V=0 counts) */
static int shown[MAXSNES][ROWS];
static int state_at41[MAXSNES];
static unsigned drops_at41[MAXSNES];
/* what the SNES shows, sampled at V=1 of every SNES frame (C6-ENTRY) */
typedef struct { uint8_t state, m2105, m212c, m210a, m2107, r420c, white, lbox, ready, solid, blank; } disp_t;
static disp_t disp[MAXSNES];
static uint32_t pa_inidisp, pa_w1, pa_inidisp16 = 0xFFFFFFFFu, pa_vofs = 0xFFFFFFFFu;
static int fb_en_gb = -1;               /* first GB frame with FB_EN in force */
static int gb_snes[MAXGB];              /* SNES frame of each GB frame's LY=0 */
static uint8_t gb_changed[MAXGB][45];
static int gb_lcd[MAXGB];
static int ngb;
static int rd_frames[ROWS][MAXSNES / 2], rd_n[ROWS];
static unsigned e4_dmas, e4_bytes, map_rows;
static uint32_t forbid_lo[4], forbid_hi[4];
static int nforbid;

/* CPU of the drain (c6_pc) */
static uint32_t pa_drain, pa_row;
static int      pass_on, pass_depth, row_on;
static uint16_t pass_sp;
static uint64_t pass_last, row_cpu, dma_in_row;
static unsigned row_spans_cur;
static unsigned rows_meas, rows_meas3;
static uint64_t row_cpu_max3, row_cpu_sum3, row_cpu_sum, pass_cpu_sum;
static unsigned passes;
static double   vstart_sum[2]; static unsigned vstart_n[2];   /* where a pass begins, window B / A */
static uint32_t *prof_pc;   /* C6_PROF=1: instructions of the passes, per PC */
uint32_t *c6_prof(void) { return prof_pc; }
static uint64_t pass_cpu_cur, pass_dma_cur;

static int state_now(void) { return m_peek(a_fb); }
static int cl_of_row(int r) { return row_clid[r] == cl_id ? cl_cur : cl_prev; }
static int k_of(int c) { return c + 18 + (c >= 46); }
static int ever_stretched;
static int stretched(void) { if(ctl & 4) ever_stretched = 1; return (ctl & 4) != 0; }
int c6_ever_stretched(void) { return ever_stretched; }
/* second stage: the stretched columns source cell col covers (sec. 14.3) */
static int tc_lo(int col) { return (8 * col) / 5; }
static int tc_hi(int col) { return (8 * col + 7) / 5; }

static void publish(void) {
  int r, af = 1, anyf = 0;
  uint32_t rf = 0, rc = 0;
  for(r = 0; r < ROWS; r++) {
    if(frozen[r]) { rf |= 1u << r; anyf = 1; } else af = 0;
    if(row_clid[r]) rc |= 1u << r;
  }
  m_poke(st + GBC_ST_C6FLAGS, (uint8_t)(0x80 | (af ? 1 : 0) | (hold_active ? 2 : 0) |
                                        (fb_active ? 4 : 0) | ((ctl & 4) ? 8 : 0) |
                                        ((ctl & 8) ? 0x10 : 0) |
                                        (cl_hyst >= CL_HYST ? 0x20 : 0) | (anyf ? 0x40 : 0)));
  m_poke(st + GBC_ST_C6FLAGS + 1, (uint8_t)c6_frames);
  m_poke(st + GBC_ST_ROWF, (uint8_t)rf);
  m_poke(st + GBC_ST_ROWF + 1, (uint8_t)(rf >> 8));
  m_poke(st + GBC_ST_ROWF + 2, (uint8_t)(rf >> 16));
  m_poke(st + GBC_ST_C6CL, (uint8_t)(cl_cur | (cl_id << 3) | (cl_prev << 4)));
  m_poke(st + GBC_ST_C6CL + 1, (uint8_t)rows_dropped);
  m_poke(st + GBC_ST_C6CL + 2, 0);
  m_poke_block(st + GBC_ST_C6DIRTY, dirty, 45);
  m_poke(st + GBC_ST_RCLI, (uint8_t)rc);
  m_poke(st + GBC_ST_RCLI + 1, (uint8_t)(rc >> 8));
  m_poke(st + GBC_ST_RCLI + 2, (uint8_t)(rc >> 16));
  m_poke(st + GBC_ST_COLWN, (uint8_t)colw_pub);
  m_poke(st + GBC_ST_COLWN + 1, (uint8_t)(colw_pub >> 8));
}

/* The $E4 bytes of row r out of fbmem (sec. 14.3). */
static void serve_row(int r) {
  int tc, y, j, p, stretch = (ctl & 4) != 0;
  static const int S[8] = {0, 0, 1, 2, 2, 3, 4, 4};
  for(tc = 0; tc < 32; tc++) {
    uint8_t *t = e4 + ((uint32_t)r << 11) + ((uint32_t)tc << 6);
    memset(t, 0, 64);
    if(!stretch && tc >= COLS) continue;
    for(y = 0; y < 8; y++)
      for(j = 0; j < 8; j++) {
        int sx = stretch ? 5 * tc + S[j] : 8 * tc + j;
        uint8_t v = fbmem[(8 * r + y) * W + sx];
        for(p = 0; p < 8; p++)
          if((v >> p) & 1) t[16 * (p >> 1) + 2 * y + (p & 1)] |= (uint8_t)(0x80 >> j);
      }
  }
}

static void set_dirty(int c) { dirty[c >> 3] |= (uint8_t)(1u << (c & 7)); }
static int  get_dirty(int c) { return (dirty[c >> 3] >> (c & 7)) & 1; }

static void ev_push(uint64_t t, int kind, int r, int k) {
  int i;
  if(ev_n >= EVQ) c6_die("fila de eventos cheia");
  i = (ev_h + ev_n) % EVQ;
  evq[i].t = t; evq[i].kind = kind; evq[i].r = r; evq[i].k = k;
  ev_n++;
}

static void ev_thaw(int r, int k) {
  (void)k;
  if(!fb_active) return;
  if(frozen[r]) {
    if(thaw_pend[r] && !hold_active) {
      frozen[r] = 0; thaw_pend[r] = 0; row_clid[r] = cl_id;
    } else { rows_dropped++; drop_row[r]++; writing[r] = 0; return; }
  }
  writing[r] = 1;
}

static void ev_freeze(int r, int k) {
  int x, y, c;
  if(!fb_active || !writing[r]) return;
  if(pic_k != k) { picture(k, pic); pic_k = k; }
  writing[r] = 0;
  for(c = 0; c < COLS; c++) {
    int ch = 0;
    for(y = 8 * r; y < 8 * r + 8; y++)
      for(x = 8 * c; x < 8 * c + 8; x++) {
        uint8_t v = quant(pic[y * W + x], cl_cur);
        if(fbmem[y * W + x] != v) { ch = 1; fbmem[y * W + x] = v; }
      }
    if(ch) set_dirty(20 * r + c);
    sent[20 * r + c] = 0;
  }
  memset(sentS[r], 0, 32);
  frozen[r] = 1;
  content_k[r] = k;
  serve_row(r);
}

static void ev_vblank(int k) {
  int i;
  long vr = 0, vg = 0, vb = 0;
  int cand;
  if(!fb_active) return;
  if(pic_k != k) { picture(k, pic); pic_k = k; }
  for(i = 0; i < W * H; i++) {
    uint16_t c = pic[i];
    vr += vote_rg(c & 31); vg += vote_rg((c >> 5) & 31); vb += vote_b((c >> 10) & 31);
  }
  cand = (vb > 0 ? 4 : 0) | (vg > 0 ? 2 : 0) | (vr > 0 ? 1 : 0);
  if(cand == cl_cur) cl_hyst = 0;
  else if(cand == cl_cand) { if(cl_hyst < 15) cl_hyst++; }
  else { cl_cand = cand; cl_hyst = 1; }
  c6_frames++;
}

static void do_ly0(int k) {
  int r, all;
  int want_en = (ctl & 2) != 0;
  if(want_en && !fb_active) {             /* FB_EN 0 -> 1: the restart */
    fb_active = 1;
    memset(frozen, 0, sizeof frozen); memset(thaw_pend, 0, sizeof thaw_pend);
    memset(writing, 0, sizeof writing); memset(row_clid, 0, sizeof row_clid);
    memset(dirty, 0, sizeof dirty);
    for(r = 0; r < CELLS; r++) set_dirty(r);
    if(stale_boot) { memset(dirty, 0, sizeof dirty); stale_boot = -1; }
    memset(sent, 0, sizeof sent);
    cl_cur = cl_prev = cl_id = cl_cand = cl_hyst = 0;
    c6_frames = 0; rows_dropped = 0;
    for(r = 0; r < ROWS; r++) { content_k[r] = -1; vram_k[r] = -1; }
    if(stale_boot < 0) stale_boot = 0;  /* the earlier session's: not an entry */
    else if(fb_en_gb < 0) fb_en_gb = k;
  } else if(!want_en && fb_active) {
    fb_active = 0;
    memset(frozen, 0, sizeof frozen); memset(thaw_pend, 0, sizeof thaw_pend);
    memset(writing, 0, sizeof writing);
  }
  hold_active = ctl & 1;
  all = 1;
  for(r = 0; r < ROWS; r++) if(row_clid[r] != cl_id) all = 0;
  if(fb_active && cl_hyst >= CL_HYST && !(ctl & 8) && all) {
    cl_prev = cl_cur; cl_cur = cl_cand; cl_id ^= 1; cl_hyst = 0; cl_switches++;
  }
}

void c6_advance(uint64_t now) {
  int moved = 0;
  if(!on) return;
  while(ev_n && evq[ev_h].t <= now) {
    ev_t e = evq[ev_h];
    ev_h = (ev_h + 1) % EVQ; ev_n--;
    if(e.kind == 0) ev_thaw(e.r, e.k);
    else if(e.kind == 1) ev_freeze(e.r, e.k);
    else ev_vblank(e.k);
    moved = 1;
  }
  if(moved) publish();
}

static void (*catchup)(uint64_t now);
void c6_set_catchup(void (*fn)(uint64_t now)) { catchup = fn; }
static void rw_hook(uint32_t a) {
  uint64_t now = m_clock_now();
  (void)a;
  if(catchup) catchup(now);            /* a LY=0 due first (the CLI's genlock) */
  c6_advance(now);
}

void c6_ly0(uint64_t t) {
  int k, r, x, y, c;
  const sc_t *s;
  if(!on) return;
  c6_advance(t);                        /* everything of the previous frame */
  k = ++gbk;
  s = scene_at(k);
  if(k < MAXGB) {
    gb_snes[k] = snes_n; gb_lcd[k] = s->lcd; ngb = k + 1;
  }
  if(!s->lcd) { publish(); return; }    /* no LY=0 with the LCD off (sec. 14.6.7) */
  do_ly0(k);
  /* content algebra for the golden comparison: what changes in the FB bytes
     from frame k-1 to frame k, quantised with the CL in force now */
  if(k < MAXGB) {
    static uint8_t prevq[W * H]; static int have_prev;
    uint8_t q[W * H];
    picture(k, pic); pic_k = k;
    for(x = 0; x < W * H; x++) q[x] = quant(pic[x], cl_cur);
    memset(gb_changed[k], 0, 45);
    for(c = 0; c < CELLS; c++) {
      int rr = c / COLS, cc = c % COLS, ch = !have_prev;
      for(y = 8 * rr; y < 8 * rr + 8 && !ch; y++)
        for(x = 8 * cc; x < 8 * cc + 8; x++) if(q[y * W + x] != prevq[y * W + x]) { ch = 1; break; }
      if(ch) gb_changed[k][c >> 3] |= (uint8_t)(1u << (c & 7));
    }
    memcpy(prevq, q, sizeof q); have_prev = 1;
  }
  for(r = 0; r < ROWS; r++) {
    ev_push(t + (uint64_t)(((456.0 * (8 * r) + 92.0) / 456.0) * GB_LINE), 0, r, k);
    ev_push(t + (uint64_t)(((456.0 * (8 * r + 7) + 92.0 + 159.0) / 456.0) * GB_LINE), 1, r, k);
  }
  ev_push(t + (uint64_t)(((456.0 * 144 + 92.0) / 456.0) * GB_LINE), 2, 0, k);
  publish();
}

uint8_t c6_snapshot(uint8_t f0) {
  const sc_t *s;
  if(!on) return f0;
  s = scene_at(gbk < 0 ? 0 : gbk);
  colw_pub = (uint16_t)s->colw;
  publish();
  f0 = (uint8_t)(f0 & ~(0x01 | 0x04 | 0x20 | 0x40));
  f0 |= (uint8_t)((s->lcd ? 0x01 : 0) | (s->compat ? 0x04 : 0) | (s->first ? 0x20 : 0) |
                  (s->ovf ? 0x40 : 0));
  return f0;
}

static void check_row2(int r) {
  int c, tc, y, cl = cl_of_row(r);
  for(c = 0; c < COLS; c++)
    if(get_dirty(20 * r + c))
      for(tc = tc_lo(c); tc <= tc_hi(c); tc++)
        if(!sentS[r][tc])
          c6_die("ROW_DONE(%d) com a celula %d suja e a coluna esticada %d nao enviada (inv. 40)", r, c, tc);
  for(tc = 0; tc < 32; tc++) {
    int k = 32 * r + tc;
    const uint8_t *t = e4 + ((uint32_t)r << 11) + ((uint32_t)tc << 6);
    uint32_t va = 0x4000u + 64u * (uint32_t)k, ma = 0xD000u + 2u * (uint32_t)k;
    for(y = 0; y < 64; y++)
      if(m_vram[va + y] != t[y])
        c6_die("ROW_DONE(%d) (2a etapa): VRAM $%04X (tile %d) = $%02X, o FB esticado tem $%02X",
               r, va + y, k, m_vram[va + y], t[y]);
    if(m_vram[ma] != (uint8_t)k || m_vram[ma + 1] != (uint8_t)((cl << 2) | (k >> 8)))
      c6_die("ROW_DONE(%d) (2a etapa): tilemap $%04X = %02X %02X, esperado {k=%d, CL=%d}",
             r, ma, m_vram[ma], m_vram[ma + 1], k, cl);
  }
}

static void check_row(int r) {
  int c, cl = cl_of_row(r);
  if(stretched()) { check_row2(r); return; }
  for(c = 0; c < COLS; c++) {
    int cell = 20 * r + c, k = k_of(cell), y;
    const uint8_t *t = e4 + ((uint32_t)r << 11) + ((uint32_t)c << 6);
    uint32_t va = 0xA000u + 64u * (uint32_t)k, ma = 0xA000u + 2u * (uint32_t)(32 * r + c);
    if(get_dirty(cell) && !sent[cell])
      c6_die("ROW_DONE(%d) com a celula %d suja e nao enviada (inv. 40)", r, c);
    for(y = 0; y < 64; y++)
      if(m_vram[va + y] != t[y])
        c6_die("ROW_DONE(%d): VRAM $%04X (tile %d, celula %d) = $%02X, o FB congelado tem $%02X",
               r, va + y, k, c, m_vram[va + y], t[y]);
    if(m_vram[ma] != (uint8_t)k || m_vram[ma + 1] != (uint8_t)((cl << 2) | (k >> 8)))
      c6_die("ROW_DONE(%d): tilemap $%04X = %02X %02X, esperado {k=%d, CL=%d}",
             r, ma, m_vram[ma], m_vram[ma + 1], k, cl);
  }
}

static void row_done(int r) {
  int c;
  if(r < 0 || r >= ROWS || !frozen[r]) {
    rowdone_bad++;
    c6_die("ROW_DONE($%02X) de linha %s", r, (r >= 0 && r < ROWS) ? "ABERTA" : "inexistente");
  }
  for(c = 0; c < COLS; c++) dirty[(20 * r + c) >> 3] &= (uint8_t)~(1u << ((20 * r + c) & 7));
  if(thaw_pend[r]) rowdone_redundant++;
  thaw_pend[r] = 1;
  vram_k[r] = content_k[r];
  rowdone_n++;
  if(rd_n[r] < MAXSNES / 2) rd_frames[r][rd_n[r]++] = snes_n;
}

void c6_ef(uint32_t off, uint8_t v) {
  if(!on) return;
  if(catchup) catchup(m_clock_now());
  c6_advance(m_clock_now());
  if(off == GBC_EFO_C6CTL) {
    uint8_t old = ctl;
    ctl = v;
    if((old ^ v) & 4) { int r; for(r = 0; r < ROWS; r++) serve_row(r); }
  } else if(off == GBC_EFO_ROWDONE) {
    if(v == 0xFF) { int r; for(r = 0; r < ROWS; r++) if(frozen[r]) row_done(r); }
    else {
      if(v < ROWS && frozen[v]) check_row(v);
      row_done(v);
    }
  }
  publish();
}

void c6_dma(uint8_t bbad, uint32_t src24, uint32_t bytes, uint32_t dest, uint64_t s, uint64_t e) {
  int i;
  if(!on) return;
  /* the FB's own tiles (and any view) never go to $B000-$B03F in the 1st
     stage; the ROM's solid-tile upload (boot, the 2nd stage's way back) does */
  if((bbad == 0x18 || bbad == 0x19) && !stretched() && (src24 >> 16) != 0x00)
    for(i = 0; i < nforbid; i++)
      if(dest < forbid_hi[i] && dest + bytes > forbid_lo[i])
        c6_die("DMA de %u B para VRAM $%04X toca $%04X-$%04X (os tiles solidos)",
               bytes, dest, forbid_lo[i], forbid_hi[i] - 1);
  if((src24 >> 16) == 0xE4 && stretched()) {
    uint32_t o = src24 & 0xFFFF, r = o >> 11, tc = (o >> 6) & 31, n = bytes / 64, c;
    if(bbad != 0x18 || (o & 63) || (bytes & 63) || !n || r >= ROWS || tc + n > 32)
      c6_die("DMA do $E4 (2a etapa) fora do formato: src $%06X, %u B", src24, bytes);
    for(c = tc; c < tc + n; c++) {
      uint32_t want = 0x4000u + 64u * (32u * r + c);
      if(dest + 64 * (c - tc) != want)
        c6_die("DMA do $E4 (2a etapa) linha %u coluna %u foi para VRAM $%04X, quer $%04X",
               r, c, dest + 64 * (c - tc), want);
      sentS[r][c] = 1;
    }
    e4_dmas++; e4_bytes += bytes; row_spans_cur++;
  } else if((src24 >> 16) == 0xE4) {
    uint32_t o = src24 & 0xFFFF, r = o >> 11, tc = (o >> 6) & 31, n = bytes / 64, c;
    if(bbad != 0x18 || (o & 63) || (bytes & 63) || !n || r >= ROWS || tc + n > COLS)
      c6_die("DMA do $E4 fora do formato: src $%06X, %u B, BBAD $%02X", src24, bytes, bbad);
    for(c = tc; c < tc + n; c++) {
      uint32_t want = 0xA000u + 64u * (uint32_t)k_of((int)(20 * r + c));
      if(dest + 64 * (c - tc) != want)
        c6_die("DMA do $E4 linha %u celula %u foi para VRAM $%04X, k(c) quer $%04X",
               r, c, dest + 64 * (c - tc), want);
      sent[20 * r + c] = 1;
    }
    e4_dmas++; e4_bytes += bytes;
    row_spans_cur++;
  }
  if(row_on) dma_in_row += e - s;       /* the tilemap's DMA too: not CPU */
  if(pass_on) pass_dma_cur += e - s;
}

/* The letterbox ch5 is pointed at, compared with the ROM's (the same bytes
   the boot seeds both WRAM sets with). */
static int lbox_at(uint32_t want) {
  const uint8_t *d = m_dma_regs();
  uint32_t a = ((uint32_t)d[0x54] << 16) | d[0x52] | (d[0x53] << 8);
  int k;
  if(want == 0xFFFFFFFFu) return 0;
  for(k = 0; k < 64; k++) {
    uint8_t x = m_peek(a + (uint32_t)k), y = m_peek(want + (uint32_t)k);
    if(x != y) return 0;
    if(k % 2 == 0 && x == 0) return 1;       /* {count, value} pairs, 0 ends */
  }
  return 1;
}
/* 1 = the 40/40 letterbox (1st stage and the quad-layer), 2 = the 16/16 one of
   the 2nd stage; 0 = something else (or ch5 off) */
static int lbox_ok(void) {
  if(!(m_cpu_regs()[0x0c] & 0x20)) return 0;
  if(lbox_at(pa_inidisp)) return 1;
  if(lbox_at(pa_inidisp16)) return 2;
  return 0;
}
static int white_ok(void) {
  int k;
  for(k = 0; k < 0x800; k += 2) if(m_vram[0xA000 + k] != 0x01 || m_vram[0xA001 + k] != 0x03) return 0;
  return 1;
}

void c6_line(int v) {
  int r;
  if(!on) return;
  c6_advance(m_clock_line_start());
  if(v == 0) snes_n++;
  if(v == 1 && snes_n < MAXSNES) {
    const uint8_t *p = m_ppu_regs();
    disp_t *q = &disp[snes_n];
    q->state = m_peek(a_fb); q->m2105 = p[0x05]; q->m212c = p[0x2c]; q->m210a = p[0x0a];
    q->m2107 = p[0x07]; q->r420c = m_cpu_regs()[0x0c]; q->white = (uint8_t)white_ok();
    q->lbox = (uint8_t)lbox_ok();
    q->ready = 1;
    for(r = 0; r < ROWS; r++) if(vram_k[r] < 0) q->ready = 0;
    { static const uint8_t sol[32] = {0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,
                                      0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF,0,0xFF};
      q->solid = (uint8_t)!memcmp(m_vram + 0xB000, sol, 32); }
    { const uint8_t *d = m_dma_regs(); int k, bl = 1;
      uint32_t a = ((uint32_t)d[0x54] << 16) | d[0x52] | (d[0x53] << 8);
      for(k = 0; k < 64; k += 2) { uint8_t cn = m_peek(a + (uint32_t)k);
        if(!cn) break;
        if(m_peek(a + (uint32_t)k + 1) != 0x80) bl = 0; }
      q->blank = (uint8_t)(bl && (m_cpu_regs()[0x0c] & 0x20)); }
  }
  if(v == 41 && snes_n < MAXSNES) {
    for(r = 0; r < ROWS; r++) shown[snes_n][r] = vram_k[r];
    state_at41[snes_n] = m_peek(a_fb);
    drops_at41[snes_n] = rows_dropped;
  }
}

void c6_set_syms(uint32_t a_drain, uint32_t a_row) { pa_drain = a_drain; pa_row = a_row; }
void c6_set_tables(uint32_t a_inidisp, uint32_t a_w1) { pa_inidisp = a_inidisp; pa_w1 = a_w1; }
void c6_set_tables2(uint32_t a_inidisp16, uint32_t a_vofs) { pa_inidisp16 = a_inidisp16; pa_vofs = a_vofs; }

/* --require-c6-regs: with the FB on the screen, the register set of contract
   sec. 14.8 (1st stage), the $2132 of CL_CUR, and ch4/ch5 on the ROM tables. */
static int c6_check_regs2(void) {
  const uint8_t *p = m_ppu_regs(), *d = m_dma_regs();
  static const struct { uint8_t reg, val; } want[] = {
    {0x05, 0x03}, {0x07, 0x68}, {0x0b, 0x02}, {0x2c, 0x01}, {0x23, 0x0c}, {0x25, 0x8c},
    {0x2b, 0x00}, {0x30, 0x11}, {0x31, 0x20}, {0x33, 0x00}, {0x2d, 0x00}, {0x28, 0}, {0x29, 255} };
  int k, bad = 0, cl = m_peek(st + GBC_ST_C6CL) & 7;
  for(k = 0; k < (int)(sizeof want / sizeof want[0]); k++)
    if(p[want[k].reg] != want[k].val) {
      fprintf(stderr, "FAIL c6 regs (2a etapa): $21%02X = $%02X, quer $%02X\n", want[k].reg, p[want[k].reg], want[k].val);
      bad++;
    }
  if(m_coldata(0) != 2 * (cl & 1) || m_coldata(1) != 2 * ((cl >> 1) & 1) || m_coldata(2) != 4 * ((cl >> 2) & 1)) {
    fprintf(stderr, "FAIL c6 regs (2a etapa): $2132 nao e' o CL_CUR %d\n", cl); bad++; }
  if(m_bg_hofs(1) != 0) { fprintf(stderr, "FAIL c6 regs (2a etapa): BG1HOFS = $%03X\n", m_bg_hofs(1)); bad++; }
  if(m_cpu_regs()[0x0c] != 0x31) { fprintf(stderr, "FAIL c6 regs (2a etapa): $420C = $%02X, quer $31\n", m_cpu_regs()[0x0c]); bad++; }
  if(d[0x00] != 0x02 || d[0x01] != 0x0e ||     /* BG1VOFS; the spec's $10 is BG2's */
     (((uint32_t)d[0x04] << 16) | d[0x02] | (d[0x03] << 8)) != pa_vofs) {
    fprintf(stderr, "FAIL c6 regs (2a etapa): ch0 nao e' o BG1VOFS modo 2 na GbcTblVofs\n"); bad++; }
  if((((uint32_t)d[0x54] << 16) | d[0x52] | (d[0x53] << 8)) != pa_inidisp16) {
    fprintf(stderr, "FAIL c6 regs (2a etapa): ch5 nao esta' no letterbox 16/16\n"); bad++; }
  if(((m_cpu_regs()[0x0a] & 1) << 8 | m_cpu_regs()[0x09]) != 209) {
    fprintf(stderr, "FAIL c6 regs (2a etapa): VTIME = %d, quer 209\n", (m_cpu_regs()[0x0a] & 1) << 8 | m_cpu_regs()[0x09]); bad++; }
  if(!bad) printf("c6 regs: sec. 14.8 2a etapa ok (CL_CUR %d, $2107 $68, $210B $02, W2 0..255, ch0 VOFS modo 2, letterbox 16/16, VTIME 209)\n", cl);
  return bad;
}

int c6_check_regs(void) {
  const uint8_t *p = m_ppu_regs(), *d = m_dma_regs();
  if(m_peek(a_fb) == 2 && stretched()) return c6_check_regs2();
  static const struct { uint8_t reg, val; } want[] = {
    {0x05, 0x03}, {0x07, 0x50}, {0x0b, 0x05}, {0x2c, 0x01}, {0x23, 0x0c}, {0x25, 0x8c},
    {0x2b, 0x00}, {0x30, 0x11}, {0x31, 0x20}, {0x33, 0x00}, {0x2d, 0x00}, {0x28, 48}, {0x29, 207} };
  int k, bad = 0, cl = m_peek(st + GBC_ST_C6CL) & 7;
  if(m_peek(a_fb) != 2) { fprintf(stderr, "FAIL c6 regs: o modo FB nao esta' na tela (estado %d)\n", m_peek(a_fb)); return 1; }
  for(k = 0; k < (int)(sizeof want / sizeof want[0]); k++)
    if(p[want[k].reg] != want[k].val) {
      fprintf(stderr, "FAIL c6 regs: $21%02X = $%02X, o sec. 14.8 quer $%02X\n", want[k].reg, p[want[k].reg], want[k].val);
      bad++;
    }
  if(!(p[0x2e] & 1)) { fprintf(stderr, "FAIL c6 regs: $212E sem BG1 (W2 nao mascara)\n"); bad++; }
  if(m_coldata(0) != 2 * (cl & 1) || m_coldata(1) != 2 * ((cl >> 1) & 1) || m_coldata(2) != 4 * ((cl >> 2) & 1)) {
    fprintf(stderr, "FAIL c6 regs: $2132 = R%d G%d B%d, CL_CUR = %d quer R%d G%d B%d\n", m_coldata(0), m_coldata(1),
            m_coldata(2), cl, 2 * (cl & 1), 2 * ((cl >> 1) & 1), 4 * ((cl >> 2) & 1));
    bad++;
  }
  if(m_bg_hofs(1) != 0x3d0 || m_bg_vofs(1) != 0x3d7) {
    fprintf(stderr, "FAIL c6 regs: BG1 HOFS/VOFS = $%03X/$%03X, quer $3D0/$3D7\n", m_bg_hofs(1), m_bg_vofs(1));
    bad++;
  }
  if(m_cpu_regs()[0x0c] != 0x30) { fprintf(stderr, "FAIL c6 regs: $420C = $%02X, quer $30\n", m_cpu_regs()[0x0c]); bad++; }
  if((((uint32_t)d[0x54] << 16) | d[0x52] | (d[0x53] << 8)) != pa_inidisp ||
     (((uint32_t)d[0x44] << 16) | d[0x42] | (d[0x43] << 8)) != pa_w1) {
    fprintf(stderr, "FAIL c6 regs: ch5/ch4 nao apontam para GbcTblInidisp/GbcTblW1\n"); bad++;
  }
  if(!bad) printf("c6 regs: sec. 14.8 ok (CL_CUR %d: $2132 = R%d G%d B%d, BG1 $3D0/$3D7, $420C $30)\n",
                  cl, m_coldata(0), m_coldata(1), m_coldata(2));
  return bad;
}

static void row_close(void) {
  uint64_t cpu;
  if(!row_on) return;
  cpu = row_cpu > dma_in_row ? row_cpu - dma_in_row : 0;
  row_on = 0;
  rows_meas++;
  row_cpu_sum += cpu;
  if(row_spans_cur <= 3) {
    rows_meas3++;
    row_cpu_sum3 += cpu;
    if(cpu > row_cpu_max3) row_cpu_max3 = cpu;
  }
}

/* CPU of the C6 drain: the master cycles of the instructions run at the depth
   the pass was entered on, minus the DMAs they fired (an interrupt taken in
   the middle is at another depth and is not the drain's).  A row runs from
   GbcFbRow's entry to the next row or the end of the pass, so the pass's own
   loop over rows it skips is charged to the row before -- pessimistic. */
void c6_pc(uint32_t pc24, int depth, uint16_t sp, uint64_t now) {
  if(!on || !pa_drain) return;
  if(pass_on && depth == pass_depth) {
    uint64_t d = now - pass_last;
    pass_cpu_cur += d;
    if(row_on) row_cpu += d;
    if(prof_pc && (pc24 >> 16) == 0 && (pc24 & 0x8000)) { prof_pc[pc24 & 0x7FFF]++; }
  }
  if(!pass_on || depth == pass_depth) pass_last = now;
  if(pass_on && depth == pass_depth && sp > pass_sp) {   /* the pass returned */
    row_close();
    pass_on = 0;
    passes++;
    if(getenv("C6_PASSLOG")) fprintf(stderr, "pass end V=%d.%02d cpu=%.1f lines\n", m_clock_line(),
                                      (int)(m_clock_h() * 100 / LINE_MC), (double)pass_cpu_cur / LINE_MC);
    pass_cpu_sum += pass_cpu_cur > pass_dma_cur ? pass_cpu_cur - pass_dma_cur : 0;
  }
  if(pc24 == pa_drain && !pass_on) {
    int v = m_clock_line(), w = (v >= 185 && v < 225) ? 0 : 1;
    double vv = v + (double)m_clock_h() / LINE_MC;
    if(w == 1 && vv < 185) vv += 262;
    if(state_now() == 2) { vstart_sum[w] += vv; vstart_n[w]++; }
    if(getenv("C6_PASSLOG")) fprintf(stderr, "pass start V=%.2f\n", vv > 262 ? vv - 262 : vv);
    pass_on = 1; pass_depth = depth; pass_sp = sp; pass_cpu_cur = 0; pass_dma_cur = 0;
    pass_last = now;
  } else if(pass_on && pc24 == pa_row && depth == pass_depth) {
    row_close();
    row_on = 1; row_cpu = 0; dma_in_row = 0; row_spans_cur = 0;
  }
}

void c6_forbid(uint32_t lo, uint32_t hi) {
  if(nforbid < 4) { forbid_lo[nforbid] = lo; forbid_hi[nforbid] = hi; nforbid++; }
}

void c6_setup(const char *scene, uint32_t stat, uint8_t *e4bank, uint32_t fb) {
  uint32_t lo[2], hi[2];
  int r;
  on = 1; st = stat; e4 = e4bank; a_fb = fb;
  if(getenv("C6_PROF")) prof_pc = (uint32_t*)calloc(0x8000, sizeof(uint32_t));
  scene_load(scene);
  if(stale_boot) ctl = 2;               /* FB_EN left on by an earlier session */
  for(r = 0; r < ROWS; r++) { content_k[r] = -1; vram_k[r] = -1; }
  memset(e4, 0, 0x10000);
  lo[0] = st + GBC_ST_C6FLAGS; hi[0] = st + GBC_ST_COLWN + 1;
  lo[1] = 0xE40000; hi[1] = 0xE4FFFF;
  m_set_read_watch(2, lo, hi, rw_hook);
  c6_forbid(0xB000, 0xB040);
  publish();
}

int c6_enabled(void) { return on; }
unsigned c6_redundant(void) { return rowdone_redundant; }

int c6_finish(const char *dump, int report) {
  int r, n, k;
  if(!on) return 0;
  if(report) {
    int maxgap = 0, from = -1;
    for(n = 0; n < snes_n && n < MAXSNES; n++) if(state_at41[n] == 2) { from = n; break; }
    for(r = 0; r < ROWS; r++) {
      int i;
      for(i = 1; i < rd_n[r]; i++)
        if(rd_frames[r][i - 1] >= from + 2 && from >= 0 &&
           rd_frames[r][i] - rd_frames[r][i - 1] > maxgap)
          maxgap = rd_frames[r][i] - rd_frames[r][i - 1];
    }
    printf("c6: rowdone_redundant=%u (a ROW_DONE of a row still waiting for its revisit)\n",
           rowdone_redundant);
    printf("c6: gb_frames=%d fb_en_gb=%d on_from_snes=%d rowdone=%u rowdone_bad=%u "
           "rows_dropped=%u cl_switches=%u cl=%d/%d id=%d e4_dmas=%u e4_bytes=%u "
           "maxgap=%d\n", ngb, fb_en_gb, from, rowdone_n, rowdone_bad, rows_dropped,
           cl_switches, cl_cur, cl_prev, cl_id, e4_dmas, e4_bytes, maxgap);
    printf("c6: FB passes begin at V=%.1f (window B) and V=%.1f (window A) on average\n",
           vstart_n[0] ? vstart_sum[0] / vstart_n[0] : 0.0, vstart_n[1] ? vstart_sum[1] / vstart_n[1] : 0.0);
    printf("c6: cpu passes=%u rows=%u rows<=3spans=%u row_cpu_avg=%.2f lines "
           "row_cpu_max(<=3)=%.2f lines row_cpu_avg(<=3)=%.2f lines pass_cpu_avg=%.2f lines\n",
           passes, rows_meas, rows_meas3,
           rows_meas ? (double)row_cpu_sum / rows_meas / LINE_MC : 0.0,
           (double)row_cpu_max3 / LINE_MC,
           rows_meas3 ? (double)row_cpu_sum3 / rows_meas3 / LINE_MC : 0.0,
           passes ? (double)pass_cpu_sum / passes / LINE_MC : 0.0);
  }
  if(dump) {
    FILE *f = fopen(dump, "w");
    if(!f) c6_die("nao consigo escrever %s", dump);
    fprintf(f, "{\"fb_en_gb\": %d, \"rows_dropped\": %u, \"rowdone_bad\": %u, "
               "\"cl_switches\": %u, \"e4_dmas\": %u, \"e4_bytes\": %u,\n",
            fb_en_gb, rows_dropped, rowdone_bad, cl_switches, e4_dmas, e4_bytes);
    fprintf(f, " \"pass_v_b\": %.2f, \"pass_v_a\": %.2f,\n",
            vstart_n[0] ? vstart_sum[0] / vstart_n[0] : 0.0, vstart_n[1] ? vstart_sum[1] / vstart_n[1] : 0.0);
    fprintf(f, " \"row_cpu_max3_mc\": %llu, \"row_cpu_avg3_mc\": %.1f, \"rows3\": %u,\n",
            (unsigned long long)row_cpu_max3,
            rows_meas3 ? (double)row_cpu_sum3 / rows_meas3 : 0.0, rows_meas3);
    fprintf(f, " \"drop_row\": [");
    for(r = 0; r < ROWS; r++) fprintf(f, "%s%u", r ? ", " : "", drop_row[r]);
    fprintf(f, "],\n \"gb\": [");
    for(k = 0; k < ngb; k++) {
      int b;
      fprintf(f, "%s[%d, %d, \"", k ? ", " : "", gb_snes[k], gb_lcd[k]);
      for(b = 44; b >= 0; b--) fprintf(f, "%02x", gb_changed[k][b]);
      fprintf(f, "\"]");
    }
    fprintf(f, "],\n \"snes\": [");
    for(n = 0; n <= snes_n && n < MAXSNES; n++) {
      fprintf(f, "%s[%d, %u, [", n ? ", " : "", state_at41[n], drops_at41[n]);
      for(r = 0; r < ROWS; r++) fprintf(f, "%s%d", r ? "," : "", shown[n][r]);
      fprintf(f, "]]");
    }
    fprintf(f, "],\n \"disp\": [");
    for(n = 0; n <= snes_n && n < MAXSNES; n++) {
      const disp_t *q = &disp[n];
      fprintf(f, "%s[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]", n ? ", " : "", q->state, q->m2105, q->m212c,
              q->m210a, q->m2107, q->r420c, q->white, q->lbox, q->ready, q->solid, q->blank);
    }
    fprintf(f, "],\n \"rowdone\": [");
    for(r = 0; r < ROWS; r++) {
      int i;
      fprintf(f, "%s[", r ? ", " : "");
      for(i = 0; i < rd_n[r]; i++) fprintf(f, "%s%d", i ? "," : "", rd_frames[r][i]);
      fprintf(f, "]");
    }
    fprintf(f, "]}\n");
    fclose(f);
  }
  return 0;
}
