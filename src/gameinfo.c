/* sd2snes+ - pre-boot game info screen (MCU side). See gameinfo.h for the SRAM
 * contract and the hard safety rules (bounded, fail-safe, never hangs). */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "yaml.h"
#include "cover.h"
#include "msu1.h"     /* menu_music_* : looping FMV audio via the MSU-1 DAC */
#include "timer.h"    /* getticks()/MS_TO_TICKS/time_after for the FMV idle watchdog */
#include "gameinfo.h"

#ifndef CONFIG_MK2

/* UTF-8 codepoint -> sd2snes font byte. MUST match the ACCENTS map in
 * snes/utils/build_const.py (and snes/font.a65). Only the Latin accents the font
 * has glyphs for (codes 130..159); everything else renders as '?'. */
static const struct { uint16_t cp; uint8_t code; } gi_accents[] = {
  {0x00E1,130},{0x00E0,131},{0x00E2,132},{0x00E3,133},{0x00E9,134},{0x00EA,135},
  {0x00ED,136},{0x00F3,137},{0x00F4,138},{0x00F5,139},{0x00FA,140},{0x00E7,141},
  {0x00C1,142},{0x00C0,143},{0x00C2,144},{0x00C3,145},{0x00C9,146},{0x00CA,147},
  {0x00CD,148},{0x00D3,149},{0x00D4,150},{0x00D5,151},{0x00DA,152},{0x00C7,153},
  {0x00F1,154},{0x00D1,155},{0x00FC,156},{0x00DC,157},{0x00BF,158},{0x00A1,159},
};

/* Copy src -> dst (NUL-terminated, bounded), decoding UTF-8 to font byte codes.
 * Plain ASCII is copied verbatim; mapped accents become 130..159; anything else
 * becomes '?'. Bounded by dstsize; safe on truncated/invalid UTF-8. */
static void gi_utf8_to_font(const char *src, char *dst, int dstsize) {
  int di = 0;
  const unsigned char *s = (const unsigned char *)src;
  while(*s && di < dstsize - 1) {
    unsigned char c = *s;
    if(c < 0x80) { dst[di++] = (char)c; s++; continue; }
    /* lead byte: how many continuation bytes follow */
    uint32_t cp; int cont;
    if((c & 0xE0) == 0xC0)      { cp = c & 0x1F; cont = 1; }
    else if((c & 0xF0) == 0xE0) { cp = c & 0x0F; cont = 2; }
    else if((c & 0xF8) == 0xF0) { cp = c & 0x07; cont = 3; }
    else { s++; dst[di++] = '?'; continue; } /* stray continuation/invalid lead */
    s++;
    int ok = 1;
    for(int k = 0; k < cont; k++) {
      if((*s & 0xC0) != 0x80) { ok = 0; break; }   /* incl. *s==0 (truncated) */
      cp = (cp << 6) | (*s & 0x3F);
      s++;
    }
    if(!ok) { dst[di++] = '?'; continue; }
    uint8_t code = 0;
    for(unsigned k = 0; k < sizeof(gi_accents) / sizeof(gi_accents[0]); k++) {
      if(gi_accents[k].cp == cp) { code = gi_accents[k].code; break; }
    }
    dst[di++] = code ? (char)code : '?';
  }
  dst[di] = 0;
}

/* dst = a + b, bounded (no snprintf dependency). */
static void gi_join(char *dst, int dstsize, const char *a, const char *b) {
  int n = strlen(a);
  if(n > dstsize - 1) n = dstsize - 1;
  memcpy(dst, a, n);
  int m = strlen(b);
  if(n + m > dstsize - 1) m = dstsize - 1 - n;
  memcpy(dst + n, b, m);
  dst[n + m] = 0;
}

/* If a key is present, font-encode its value into the field (bounded). */
static void gi_field(const char *key, char *field, int size) {
  yaml_token_t tok;
  if(yaml_get_itemvalue(key, &tok)) {
    gi_utf8_to_font(tok.stringvalue, field, size);
  }
}

/* leave a single "-" placeholder for an empty metadata field (the SNES then
 * prints every field unconditionally - no empty check needed there). */
static void gi_dash(char *field) {
  if(!field[0]) { field[0] = '-'; field[1] = 0; }
}

/* stream `size` bytes from the open file to PSRAM at `addr` (bounded chunks). */
static int gi_stream(uint32_t addr, uint32_t size) {
  UINT got;
  while(size) {
    UINT want = (size > sizeof(file_buf)) ? sizeof(file_buf) : (UINT)size;
    file_res = f_read(&file_handle, file_buf, want, &got);
    if(file_res || got != want) return 0;
    sram_writeblock(file_buf, addr, (uint16_t)got);
    addr += got; size -= got;
  }
  return 1;
}

/* Load the DirectColor /sd2snes/info/<rom>.gd: validate the header, stream the tilemap into
 * SRAM_GAMEINFO_TMAP_ADDR and the 8bpp tiles into SRAM_GAMEINFO_TILES_ADDR, and
 * record the geometry in the meta struct. Returns 1 on success. Bounded/fail-safe. */
static int gi_load_image(const char *path, gameinfo_meta_t *meta) {
  uint8_t hdr[GD_HEADER_SIZE];
  UINT got;
  file_open((uint8_t *)path, FA_READ);
  if(file_res) return 0;
  file_res = f_read(&file_handle, hdr, GD_HEADER_SIZE, &got);
  if(file_res || got != GD_HEADER_SIZE
     || hdr[0] != GD_MAGIC0 || hdr[1] != GD_MAGIC1 || hdr[2] != GD_VERSION) {
    file_close(); return 0;
  }
  uint8_t  w = hdr[4], h = hdr[5];
  uint16_t ntiles = (uint16_t)(hdr[6] | (hdr[7] << 8));
  /* VRAM holds at most 512 unique 8bpp tiles (two 256-tile windows, see snes/gameinfo.a65) */
  if(w == 0 || w > 32 || h == 0 || h > 32 || ntiles == 0 || ntiles > 512) {
    file_close(); return 0;
  }
  /* file order: header, tilemap (w*h*2), tiles (ntiles*64) */
  if(!gi_stream(SRAM_GAMEINFO_TMAP_ADDR,  (uint32_t)w * h * 2)) { file_close(); return 0; }
  if(!gi_stream(SRAM_GAMEINFO_TILES_ADDR, (uint32_t)ntiles * 64)) { file_close(); return 0; }
  file_close();
  meta->img_w_tiles   = w;
  meta->img_h_tiles   = h;
  meta->img_num_tiles = ntiles;
  meta->flags |= GAMEINFO_FLAG_IMAGE;
  return 1;
}

/* ---- animated screenshot (.fmv) streaming -------------------------------------
 * One .fmv is held open and read SEQUENTIALLY, one fixed-size frame per CMD_FMV_NEXT,
 * looping at EOF. A DEDICATED FIL (not the shared file_handle) so it survives across
 * the menu's per-frame pump without clobbering other file ops. Frame data lands at
 * SRAM_GAMEINFO_TILES_ADDR ($CA0000) -- the same bank the static .gd cover would use,
 * free in the .fmv path (the cover is the sibling .cov in bank C9). All bounded. */
static FIL      gi_fmv_fil;
static uint8_t  gi_fmv_open;        /* 1 while gi_fmv_fil is a valid open .fmv */
static uint8_t  gi_fmv_fps;         /* playback fps (from the header) -> audio->frame mapping */
static uint16_t gi_fmv_frames;      /* total frame count */
static uint16_t gi_fmv_cur;         /* frame index currently staged in $CA0000 (file is at cur+1) */
static tick_t   gi_fmv_last_tick;   /* getticks() of the last CMD_FMV_NEXT (idle watchdog) */
static char     gi_fmv_path[300];   /* the open .fmv's path, saved so a transient close can
                                     * reopen it mid-playback (gi_fmv_reopen). "" = none. */

/* In-place retries before a read error closes the file: a single glitched SD read used to
 * kill the FMV for the whole session (no reopen path), freezing the panel until you left the
 * screen or power-cycled. Bounded -> never hangs. */
#define FMV_READ_RETRIES 3

static void gi_fmv_close(void) {
  if(gi_fmv_open) { f_close(&gi_fmv_fil); gi_fmv_open = 0; }
}

/* stream ONE frame (FMV_FRAME_BYTES) from the current file position into PSRAM at
 * SRAM_GAMEINFO_TILES_ADDR. A glitched read is retried in place (rewind to the frame start)
 * a few times before giving up. Bounded; on persistent error closes the file and returns 0. */
static int gi_fmv_read_frame(void) {
  DWORD start = gi_fmv_fil.fptr;       /* frame start, so a failed chunk can rewind + retry */
  int tries;
  for(tries = 0; tries < FMV_READ_RETRIES; tries++) {
    uint32_t addr = SRAM_GAMEINFO_TILES_ADDR;
    uint32_t size = FMV_FRAME_BYTES;
    UINT got;
    int ok = 1;
    if(tries && f_lseek(&gi_fmv_fil, start)) break;   /* rewind to retry; seek error -> give up */
    while(size) {
      UINT want = (size > sizeof(file_buf)) ? sizeof(file_buf) : (UINT)size;
      if(f_read(&gi_fmv_fil, file_buf, want, &got) || got != want) { ok = 0; break; }
      sram_writeblock(file_buf, addr, (uint16_t)got);
      addr += got; size -= got;
      /* The 6912-byte frame read is the longest stretch with no DAC refill -> the FMV audio
         buffer (~11.6 ms) can starve mid-read and click. Pump the DAC after each ~512-byte
         chunk (file_buf already staged): every chunk is a refill point, so it never starves.
         No-op when no FMV music is playing (menusfx_active == 0). */
      menu_sfx_pump();
    }
    if(ok) return 1;
  }
  gi_fmv_close();
  return 0;
}

/* Open <rom>.fmv, validate the header, stage frame 0, and arm the meta struct. Leaves the
 * file positioned at frame 1 (gi_fmv_cur = 0). Returns 1 on success. Fixed geometry MUST
 * match GI_FMV_W/GI_FMV_H on the SNES side. Bounded + fail-safe. */
static int gi_fmv_begin(const char *fmvpath, gameinfo_meta_t *meta) {
  uint8_t hdr[FMV_HEADER_SIZE];
  UINT got;
  gi_fmv_close();
  if(f_open(&gi_fmv_fil, fmvpath, FA_READ)) return 0;
  if(f_read(&gi_fmv_fil, hdr, FMV_HEADER_SIZE, &got) || got != FMV_HEADER_SIZE
     || hdr[0] != FMV_MAGIC0 || hdr[1] != FMV_MAGIC1 || hdr[2] != FMV_VERSION
     || hdr[4] != FMV_BOX_W   || hdr[5] != FMV_BOX_H) { f_close(&gi_fmv_fil); return 0; }
  uint16_t nframes = (uint16_t)(hdr[6] | (hdr[7] << 8));
  if(nframes == 0) { f_close(&gi_fmv_fil); return 0; }
  gi_fmv_frames = nframes;
  gi_fmv_fps    = hdr[8] ? hdr[8] : 12;
  gi_fmv_cur    = 0;
  gi_fmv_open   = 1;
  gi_fmv_last_tick = getticks();         /* arm the idle watchdog */
  if(!gi_fmv_read_frame()) return 0;     /* stage frame 0; file now at frame 1 */
  gi_join(gi_fmv_path, sizeof(gi_fmv_path), fmvpath, "");  /* remember it for a reopen */
  meta->flags     |= GAMEINFO_FLAG_FMV;
  meta->fmv_frames = nframes;
  meta->fmv_fps    = gi_fmv_fps;
  return 1;
}

/* Self-heal: a transient SD read closed the .fmv mid-playback. Reopen the saved path (no meta
 * or frame-0 re-stage) and seek to the frame after the one currently shown, so the normal
 * sequential advance resumes where it left off (at wrap the next call re-seeks anyway).
 * Bounded; on failure leaves gi_fmv_open = 0 and the panel just holds its last frame. */
static int gi_fmv_reopen(void) {
  uint8_t hdr[FMV_HEADER_SIZE];
  UINT got;
  uint32_t nextf;
  if(!gi_fmv_path[0]) return 0;
  if(f_open(&gi_fmv_fil, (const TCHAR*)gi_fmv_path, FA_READ)) return 0;
  if(f_read(&gi_fmv_fil, hdr, FMV_HEADER_SIZE, &got) || got != FMV_HEADER_SIZE
     || hdr[0] != FMV_MAGIC0 || hdr[1] != FMV_MAGIC1 || hdr[2] != FMV_VERSION
     || hdr[4] != FMV_BOX_W   || hdr[5] != FMV_BOX_H) { f_close(&gi_fmv_fil); return 0; }
  nextf = (uint32_t)gi_fmv_cur + 1u;
  if(nextf >= gi_fmv_frames) nextf = 0;
  if(f_lseek(&gi_fmv_fil, FMV_HEADER_SIZE + nextf * FMV_FRAME_BYTES)) { f_close(&gi_fmv_fil); return 0; }
  gi_fmv_open = 1;
  return 1;
}

/* CMD_FMV_NEXT pump: stage the frame that matches the AUDIO playback position, so the video
 * stays LOCKED to the music (no drift). Free-runs sequentially when no .pcm is playing. The
 * SNES re-DMAs $CA0000 every pump, so "hold" (return without reading) just repeats a frame. */
void gameinfo_fmv_next(void) {
  if(!gi_fmv_open && !gi_fmv_reopen()) return;   /* self-heal a transient close, else hold */
  gi_fmv_last_tick = getticks();         /* the panel is alive; refresh the idle watchdog */
  uint32_t target;
  if(menu_music_active() && gi_fmv_fps) {
    /* frame = audio_seconds * fps = samples * fps / 44100 (MSU-1 rate), wrapped to the loop */
    uint32_t s = menu_music_samples();
    target = (uint32_t)(((uint64_t)s * gi_fmv_fps) / 44100u) % gi_fmv_frames;
  } else {
    target = (uint32_t)((gi_fmv_cur + 1u) % gi_fmv_frames);   /* no audio: free-run */
  }
  if(target == gi_fmv_cur) return;                            /* in the same frame -> hold */
  if(target < gi_fmv_cur) {
    if((uint32_t)(gi_fmv_cur - target) < (uint32_t)(gi_fmv_frames / 2u))
      return;                                                 /* video slightly ahead -> hold */
    /* loop wrap (target jumped back near 0) -> seek there (cheap, near the file start) */
    if(f_lseek(&gi_fmv_fil, FMV_HEADER_SIZE + target * FMV_FRAME_BYTES)) { gi_fmv_close(); return; }
    if(gi_fmv_read_frame()) gi_fmv_cur = (uint16_t)target;
    return;
  }
  /* forward: read up to target (catch-up discards the skipped frames; the last read stays in
   * $CA0000). A large jump -- which steady play never produces -- seeks instead. */
  if(target - gi_fmv_cur > 30u) {
    if(f_lseek(&gi_fmv_fil, FMV_HEADER_SIZE + target * FMV_FRAME_BYTES)) { gi_fmv_close(); return; }
    if(gi_fmv_read_frame()) gi_fmv_cur = (uint16_t)target;
    return;
  }
  while(gi_fmv_cur < target) {
    if(!gi_fmv_read_frame()) return;     /* read the next frame into $CA0000 (closes on error) */
    gi_fmv_cur++;
  }
}

/* Stop the FMV: close the file + stop the looping audio. Called when the info screen closes. */
void gameinfo_fmv_stop(void) {
  gi_fmv_close();
  menu_music_stop();
}

/* Idle watchdog: the menu pumps CMD_FMV_NEXT continuously while the info screen is up. If it
 * goes quiet the screen closed WITHOUT a trailing command (e.g. back to the Favorites/Recents
 * list, which issues none) -> stop the FMV so the audio doesn't linger. Call from the menu
 * loop; bounded, no-op when nothing is playing. */
void gameinfo_fmv_idle_check(void) {
  if(!gi_fmv_open && !menu_music_active()) return;
  if(time_after(getticks(), gi_fmv_last_tick + MS_TO_TICKS(300)))
    gameinfo_fmv_stop();
}

void gameinfo_load(uint8_t *rom_path) {
  /* static (not stack): the menu loop is single-threaded and non-reentrant, so this
   * keeps a ~1.25 KB frame off the tight LPC stack (see cfg.c note on frame overrun). */
  static gameinfo_meta_t meta;
  static char base[288];
  static char path[300];
  int fmv_eligible = 1;                 /* only probe <rom>.fmv if the .yml declares "fmv:" (or
                                         * there is no .yml). Skips a full scan of the (huge)
                                         * info dir for the 99% of games that have no video. */

  memset(&meta, 0, sizeof(meta));
  meta.magic[0] = GAMEINFO_MAGIC0;
  meta.magic[1] = GAMEINFO_MAGIC1;
  /* The screen always shows when ShowGameInfo is on: a ROM with no .yml still
   * gets a title (its filename), "-" metadata, and -- if a sibling <rom>.cov
   * exists -- the OBJ box-art floated in the band where the .gd cover would be. */
  meta.status   = GAMEINFO_STATUS_OK;

  /* build "/sd2snes/info/<C>/<stem>" (extension stripped). The info files are bucketed by
   * first character (0-9, A-Z, else '_') into a subfolder so each f_open scans ~one letter's
   * worth of files, not the whole flat /sd2snes/info (utils/reorg_info.py; bucket logic MUST
   * match). gi_bucket = the bucket char + '/'. */
  const char *leaf = strrchr((const char *)rom_path, '/');
  leaf = leaf ? leaf + 1 : (const char *)rom_path;
  { unsigned char c = (unsigned char)leaf[0];
    if(c >= 'a' && c <= 'z') c -= 32;
    if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) c = '_';
    char bdir[3]; bdir[0] = (char)c; bdir[1] = '/'; bdir[2] = 0;
    gi_join(path, sizeof(path), GAMEINFO_DIR, bdir);   /* "/sd2snes/info/D/" */
    gi_join(base, sizeof(base), path, leaf);           /* "/sd2snes/info/D/<leaf>" */
  }
  { char *dot = strrchr(base, '.'); if(dot) *dot = 0; }

  /* /sd2snes/info/<stem>.yml -- now OPTIONAL: a missing .yml is no longer a skip,
   * it just leaves every field empty (filled by the fallbacks below). */
  gi_join(path, sizeof(path), base, ".yml");
  yaml_file_open(path, FA_READ);
  if(!file_res) {
    gi_field("title",        meta.title,        sizeof(meta.title));
    gi_field("developer",    meta.developer,    sizeof(meta.developer));
    gi_field("release_year", meta.year,         sizeof(meta.year));
    gi_field("players",      meta.players,      sizeof(meta.players));
    gi_field("genre",        meta.genre,        sizeof(meta.genre));
    gi_field("special_chip", meta.special_chip, sizeof(meta.special_chip));
    gi_field("description",  meta.description,  sizeof(meta.description));
    { yaml_token_t tok; fmv_eligible = yaml_get_itemvalue("fmv", &tok) ? 1 : 0; }
    yaml_file_close();
  } else {
    file_res = 0; /* soft fail: no .yml is fine; fmv_eligible stays 1 (probe .fmv as before) */
  }

  /* title fallback: the ROM stem (no extension; base is ".../<C>/<stem>"); "-" for other
   * empty fields. Applied unconditionally so the .yml-less screen is filled. The +2 skips
   * the "<C>/" bucket folder added to GAMEINFO_DIR above. */
  if(!meta.title[0])
    gi_utf8_to_font(base + (sizeof(GAMEINFO_DIR) - 1) + 2, meta.title, sizeof(meta.title));
  gi_dash(meta.developer);
  gi_dash(meta.year);
  gi_dash(meta.players);
  gi_dash(meta.genre);
  gi_dash(meta.special_chip);

  /* band: prefer an animated /sd2snes/info/<rom>.fmv (screenshot box plays; the cover is
   * the sibling <rom>.cov floated as OBJ); else the static composited <rom>.gd; else just
   * the <rom>.cov box-art. All bounded + fail-safe; if nothing exists the band is gradient.
   * The .fmv f_open scans the (huge) info dir, so it is gated behind the .yml "fmv:" flag. */
  gi_fmv_close();                            /* drop any prior open .fmv (reentry) */
  gi_fmv_path[0] = 0;                         /* invalidate the saved reopen path */
  menu_music_stop();                         /* and any prior FMV audio clip */
  {
    int is_fmv = 0;
    if(fmv_eligible) {
      gi_join(path, sizeof(path), base, ".fmv");
      is_fmv = gi_fmv_begin(path, &meta);
    }
    if(is_fmv) {
      load_cover(rom_path, SRAM_COVER_ADDR); /* OBJ cover next to the FMV box */
      /* optional sibling <rom>.pcm: loop it through the DAC while the screen is open. Silent
       * if absent. The menu stops it when the info screen closes (main.c / idle watchdog). */
      gi_join(path, sizeof(path), base, ".pcm");
      menu_music_play(path);
    } else {
      gi_join(path, sizeof(path), base, ".gd");
      if(!gi_load_image(path, &meta))
        load_cover(rom_path, SRAM_COVER_ADDR);
    }
  }

  sram_writeblock(&meta, SRAM_GAMEINFO_ADDR, sizeof(meta));
}

#else /* CONFIG_MK2: flash is too tight for the parser. Stub that always reports
       * "no info" so the (fail-safe) SNES side skips the screen and boots. */

void gameinfo_load(uint8_t *rom_path) {
  (void)rom_path;
  gameinfo_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.magic[0] = GAMEINFO_MAGIC0;
  meta.magic[1] = GAMEINFO_MAGIC1;
  meta.status   = GAMEINFO_STATUS_NONE;
  sram_writeblock(&meta, SRAM_GAMEINFO_ADDR, sizeof(meta));
}

void gameinfo_fmv_next(void)       { /* no FMV on mk2 (flash too tight; parser stubbed) */ }
void gameinfo_fmv_stop(void)       { }
void gameinfo_fmv_idle_check(void) { }

#endif /* CONFIG_MK2 */
