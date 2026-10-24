/* sd2snes+ - pre-boot game info screen (MCU side). See gameinfo.h for the SRAM
 * contract and the hard safety rules (bounded, fail-safe, never hangs). */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "cfg.h"      /* cfg_t / CFG : game info "Show video" + "Play video music" toggles */
#include "yaml.h"
#include "cover.h"
#include "msu1.h"     /* menu_music_* : looping FMV audio via the MSU-1 DAC */
#include "timer.h"    /* getticks()/MS_TO_TICKS/time_after for the FMV idle watchdog */
#include "gameinfo.h"
#include "psram_io.h"
#include "util.h"
#include "dirindex.h"
#include "scratch.h"  /* gameinfo_load FRAME + three LEAF helpers (see scratch.h) */
#include "cjkglyph.h" /* a Japanese/Chinese title or description: the glyphs it needs */
#include "crc32.h"    /* the ROM's CRC32 on the title row (gameinfo_crc_*) */
#include "gameinfo_int.h"   /* what the loader on the card (giload.c) shares with this half */

extern cfg_t CFG;   /* game info "Show video" / "Play video music" toggles (game_info_video/_music) */

/* The struct is blitted whole to SRAM_GAMEINFO_ADDR and the full-description region
   starts right after it, so a field added to gameinfo_meta_t writes over the text the
   same screen reads back -- silently, neither side bounds-checks the other.  The other
   end of the block is asserted in cfg.c, where MAX_FAVORITE_GAMES lives. */
_Static_assert(SRAM_GAMEINFO_ADDR + sizeof(gameinfo_meta_t) <= SRAM_GAMEINFO_CRC_ADDR,
               "gameinfo_meta_t runs into SRAM_GAMEINFO_CRC_ADDR");

/* UTF-8 codepoint -> sd2snes font byte. Every glyph entry MUST match ENCODE (ACCENTS +
 * HOMOGLYPHS) in snes/utils/build_const.py -- guarded by tests/test_i18n_parity.py, which parses
 * these two tables. Every mapped codepoint lives in Latin-1 (the pt/es/fr/it/de accents) or in
 * the Cyrillic block (ru), so each block is a direct lookup indexed by the codepoint; 0 = no
 * glyph. Cyrillic homoglyphs map to the Latin tile that draws them (А -> 'A'). The Latin-1 table
 * also carries plain-ASCII stand-ins in slots the font has no glyph for (« » -> '"', the
 * no-break space -> ' ', ° º -> 'o', ...): same lookup, zero extra bytes. Anything longer, or
 * outside these blocks, goes through gi_fb_cp/gi_fb_str below. */
#define GI_FONT_LATIN1_BASE   0x00A0
#define GI_FONT_CYRILLIC_BASE 0x0400
static const uint8_t gi_font_latin1[96] = {
   32, 159,   0,   0,   0,   0,   0,   0,   0,   0,  97,  34,   0,   0,   0,   0,  /* U+00A0 */
  111,   0,   0,   0,  39,   0,   0,  46,   0,   0, 111,  34,   0,   0,   0, 158,  /* U+00B0 */
  143, 142, 144, 145, 239,   0,   0, 153, 232, 146, 147,   0, 233, 148,   0,   0,  /* U+00C0 */
    0, 155, 234, 149, 150, 151, 240, 120,   0, 235, 152,   0, 157,   0,   0, 238,  /* U+00D0 */
  131, 130, 132, 133, 236,   0,   0, 141, 224, 134, 135, 228, 230, 136, 226, 227,  /* U+00E0 */
    0, 154, 231, 137, 138, 139, 237,   0,   0, 225, 140, 229, 156,   0,   0,   0,  /* U+00F0 */
};

static const uint8_t gi_font_cyrillic[96] = {
    0, 181,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  /* U+0400 */
   65, 178,  66, 179, 180,  69, 182, 183, 184, 185,  75, 186,  77,  72,  79, 187,  /* U+0410 */
   80,  67,  84, 177, 188,  88, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198,  /* U+0420 */
   97, 199, 200, 201, 202, 101, 203, 204, 205, 206, 207, 208, 209, 210, 111, 211,  /* U+0430 */
  112,  99, 212, 121, 213, 120, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,  /* U+0440 */
    0, 228,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  /* U+0450 */
};

/* Typography with no glyph of its own, spelled with glyphs the font has (up to 3 bytes; ""
 * = drop the codepoint). Measured on the GameDB descriptions, the em dash, the curly quotes and
 * apostrophe, the ellipsis and the bullet alone were ~23k '?' across the seven languages. Only
 * characters with an obvious ASCII spelling belong here: kana/kanji/hangul stay '?'. Sorted by
 * codepoint (the test checks it); the two parallel arrays keep 5 bytes per entry. */
static const uint16_t gi_fb_cp[] = {
  0x00AD, 0x00C6, 0x00E6, 0x0100, 0x0101, 0x0112, 0x0113, 0x012A, 0x012B, 0x014C, 0x014D,
  0x0152, 0x0153, 0x016A, 0x016B, 0x200B, 0x200C, 0x200D, 0x2018, 0x2019, 0x201A, 0x201B,
  0x201C, 0x201D, 0x201E, 0x201F, 0x2022, 0x2026, 0x202F, 0x2032, 0x2033, 0x2122, 0x2212,
  0xFEFF,
};
static const char gi_fb_str[][3] = {
  "",    "AE", "ae", "A",  "a",  "E",  "e",  "I",  "i",  "O",  "o",
  "OE",  "oe", "U",  "u",  "",   "",   "",   "'",  "'",  "'",  "'",
  "\"", "\"", "\"", "\"", "*",  "...", " ",  "'",  "\"", "TM", "-",
  "",
};

/* CJK (menu languages 8 = Japanese, 9 = Chinese). The menu font has no ideographs: the menu
 * draws them from a glyph cache in VRAM (snes/cjk.a65). Its own strings bring their glyphs; the
 * text of a game (title_<code> / description_<code> in the .yml) cannot, so this file gives each
 * distinct ideograph of the current game an index, writes the text as the byte pair
 * {$FF, $80 + index} (the cache's glyphs 640..767), and draws the glyphs into
 * SRAM_GAMEINFO_CJK_ADDR from the language's full font, /sd2snes/lang/<code>.fnt
 * (snes/utils/gen_cjk_font.py). More distinct ideographs than GI_CJK_MAX print as '?'. */
#define GI_CJK_FIRST 0x2E80             /* kana, ideographs, full-width forms (build_const) */
uint16_t gi_cjk_cp[GI_CJK_MAX] IN_AHBRAM;   /* written before read (gi_cjk_n) */
uint8_t  gi_cjk_n;
const char *gi_cjk_code;         /* "ja"/"zh" while the menu language is CJK, else NULL */


/* index of an ideograph for this game (a new one is added); -1 = the table is full */
static int gi_cjk_index(uint32_t cp) {
  for(int k = 0; k < gi_cjk_n; k++)
    if(gi_cjk_cp[k] == cp) return k;
  if(gi_cjk_n >= GI_CJK_MAX) return -1;
  gi_cjk_cp[gi_cjk_n] = (uint16_t)cp;
  return gi_cjk_n++;
}


/* Decoded (>= 0x80) codepoint -> 0..3 font bytes into out. A glyph when the font has one, a
 * typographic stand-in when it has not, '?' otherwise. */
int gi_cp_emit(uint32_t cp, uint8_t *out) {
  uint8_t b = 0;
  if(gi_cjk_code && cp >= GI_CJK_FIRST && cp <= 0xFFFF) {
    int k = gi_cjk_index(cp);
    if(k >= 0) { out[0] = 0xFF; out[1] = (uint8_t)(0x80 + k); return 2; }
    out[0] = '?';
    return 1;
  }
  if(cp - GI_FONT_LATIN1_BASE < sizeof(gi_font_latin1))
    b = gi_font_latin1[cp - GI_FONT_LATIN1_BASE];
  else if(cp - GI_FONT_CYRILLIC_BASE < sizeof(gi_font_cyrillic))
    b = gi_font_cyrillic[cp - GI_FONT_CYRILLIC_BASE];
  else if(cp - 0x2002 <= 0x200A - 0x2002) b = ' ';     /* the typographic spaces */
  else if(cp - 0x2010 <= 0x2015 - 0x2010) b = '-';     /* hyphens and dashes */
  if(b) { out[0] = b; return 1; }
  for(unsigned k = 0; k < sizeof(gi_fb_cp) / sizeof(gi_fb_cp[0]); k++) {
    if(gi_fb_cp[k] != cp) continue;
    int n = 0;
    while(n < 3 && gi_fb_str[k][n]) { out[n] = (uint8_t)gi_fb_str[k][n]; n++; }
    return n;
  }
  out[0] = '?';
  return 1;
}

/* gi_cp_emit for the file manager's text viewer (module api font_cp): no CJK glyphs, those
 * belong to the Ficha's sheet. */
int gameinfo_font_cp(uint32_t cp, uint8_t *out) {
  const char *code = gi_cjk_code;
  int n;
  gi_cjk_code = NULL;
  n = gi_cp_emit(cp, out);
  gi_cjk_code = code;
  return n;
}



/* Build "/sd2snes/info/[<ns>/]<BB>/<stem>" into `out` (namespace + bucket, extension stripped). Thin
 * wrapper over path_asset so THE bucket rule lives in exactly one place (fileops.c); the .man
 * viewer (manual.c) reaches the same layout through this. Callers that need the stem should take
 * the offset path_asset returns rather than computing it -- see gi_utf8_to_font below. Bounded. */
void gameinfo_info_base(const uint8_t *rom_path, char *out, int outsize) {
  if(path_asset(out, outsize, GAMEINFO_DIR, (const char *)rom_path, "") < 0)
    out[0] = 0;
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
char     gi_fmv_path[300] IN_AHBRAM;  /* the open .fmv's path, saved so a transient close can
                                     * reopen it mid-playback (gi_fmv_reopen). "" = none. IN_AHBRAM: the
                                     * main LPC175x SRAM is tight and growing .bss can silently corrupt a
                                     * global. .ahbram is NOLOAD (not zeroed at boot), which is safe here
                                     * for the SAME reason as gi_yml_path below: the reader (gi_fmv_reopen,
                                     * via CMD_FMV_NEXT) only arrives after a GAME_INFO, and gameinfo_load
                                     * zeroes gi_fmv_path[0] on EVERY load before any FMV pump can arrive. */

/* The last-loaded .yml path, saved by gameinfo_load so the "full description" (Y) command
 * (gameinfo_desc_full) can re-open it without a fresh selection round-trip. IN_AHBRAM: the
 * main LPC175x SRAM is tight and growing .bss can silently corrupt a global; .ahbram is
 * NOLOAD (not zeroed at boot), which is fine here because gameinfo_load writes this in full
 * BEFORE any read (the Y command only arrives after a GAME_INFO). "" = none. */
char     gi_yml_path[300] IN_AHBRAM;

/* In-place retries before a read error closes the file: a single glitched SD read used to
 * kill the FMV for the whole session (no reopen path), freezing the panel until you left the
 * screen or power-cycled. Bounded -> never hangs. */
#define FMV_READ_RETRIES 3

void gi_fmv_close(void) {
  if(gi_fmv_open) { f_close(&gi_fmv_fil); gi_fmv_open = 0; }
}

/* Stage ONE frame (palette + tiles) from the current file position. On disk a frame is the
 * 176-byte FMV palette (88 colours) THEN the 6912-byte tiles: the palette goes to $CA1B00 (the SNES
 * DMAs it to CGRAM 168..255 each frame) and the tiles to $CA0000 (re-DMA'd to the FMV VRAM set). A glitched
 * read rewinds to the frame start and retries. Bounded; closes the file on persistent error.
 * menu_sfx_pump runs between chunks so the FMV audio buffer never starves mid-read. */
static int gi_fmv_read_frame(void) {
  DWORD start = gi_fmv_fil.fptr;
  int tries;
  for(tries = 0; tries < FMV_READ_RETRIES; tries++) {
    if(tries && f_lseek(&gi_fmv_fil, start)) break;
    if(psram_stream(&gi_fmv_fil, SRAM_GAMEINFO_TILES_ADDR + FMV_FRAME_BYTES,
                    FMV_FRAME_PAL_BYTES, menu_sfx_pump)
       && psram_stream(&gi_fmv_fil, SRAM_GAMEINFO_TILES_ADDR,
                       FMV_FRAME_BYTES, menu_sfx_pump)) return 1;
  }
  gi_fmv_close();
  return 0;
}

/* Open <rom>.fmv (v4, cover-LESS), validate, stage frame 0, and arm the meta struct. Leaves the file
 * at frame 1 (gi_fmv_cur = 0). 1 frame = static screenshot (the pump no-ops). The cover is the
 * sibling .gcv (gi_load_gcv), staged separately. Bounded. */
int gi_fmv_begin(const char *fmvpath, gameinfo_meta_t *meta) {
  uint8_t hdr[FMV_HEADER_SIZE];
  UINT got;
  gi_fmv_close();
  if(f_open(&gi_fmv_fil, fmvpath, FA_READ)) return 0;
  if(f_read(&gi_fmv_fil, hdr, FMV_HEADER_SIZE, &got) || got != FMV_HEADER_SIZE
     || hdr[0] != FMV_MAGIC0 || hdr[1] != FMV_MAGIC1 || hdr[2] != FMV_VERSION
     || hdr[4] != FMV_BOX_W   || hdr[5] != FMV_BOX_H) { f_close(&gi_fmv_fil); return 0; }
  uint16_t nframes = (uint16_t)(hdr[8] | (hdr[9] << 8));
  if(nframes == 0) { f_close(&gi_fmv_fil); return 0; }
  /* cover-LESS: the cover is the sibling .gcv, so there is NO cover block to stage here */
  gi_fmv_frames = nframes;
  gi_fmv_fps    = hdr[7] ? hdr[7] : 12;
  gi_fmv_cur    = 0;
  gi_fmv_open   = 1;
  gi_fmv_last_tick = getticks();
  if(!gi_fmv_read_frame()) return 0;     /* stage frame 0; file now at frame 1 */
  strlcpy_nul(gi_fmv_path, fmvpath, sizeof(gi_fmv_path));
  meta->flags     |= GAMEINFO_FLAG_FMV;
  meta->fmv_frames = nframes;
  meta->fmv_fps    = gi_fmv_fps;
  return 1;
}

/* Self-heal: a transient SD read closed the .fmv mid-playback. Reopen + re-seek to the next frame.
 * The cover block stays staged in PSRAM (not cleared), so it is NOT restreamed. Bounded. */
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
  if(f_lseek(&gi_fmv_fil, FMV_DATA_START + nextf * FMV_FRAME_STRIDE)) { f_close(&gi_fmv_fil); return 0; }
  gi_fmv_open = 1;
  /* The idle watchdog's stop kills the clip AUDIO too, and this reopen used to bring back only
   * the VIDEO -- any screen that holds the pump >300ms (e.g. the guide picker) resumed a silent
   * clip. Restart the soundtrack under the same gate gameinfo_load uses; the pump then re-locks
   * the video to the fresh audio position. Only the .fmv clip ever ships a .qoa/.pcm, so the .gss
   * snapshot path (no pump, no audio) never gets here with a non-.fmv suffix anyway. The
   * extension is swapped IN PLACE in gi_fmv_path and restored right after: menu_music_play
   * opens the file synchronously and keeps no pointer/name cache (menusfx_open_name = 0), and
   * this avoids a 300-byte buffer (AHB is down to ~100B free; a stack copy would be a needless
   * bite out of the ~2.5KB headroom). */
  if(CFG.game_info_music && !menu_music_active()) {
    size_t n = strlen(gi_fmv_path);
    if(n > 4 && !strcmp(gi_fmv_path + n - 4, ".fmv")) {
      int st;
      memcpy(gi_fmv_path + n - 4, ".qoa", 4);  /* the compressed soundtrack wins over a .pcm */
      st = menu_music_play(gi_fmv_path);
      if(st != 0xA0) {
        memcpy(gi_fmv_path + n - 4, ".pcm", 4);
        st = menu_music_play(gi_fmv_path);     /* silent + harmless if the .pcm is absent */
      }
      memcpy(gi_fmv_path + n - 4, ".fmv", 4);
      /* The pump LOCKS video to audio: fresh audio starts at sample 0 while gi_fmv_cur still
       * points at the frame where the watchdog struck, so the "video slightly ahead -> hold"
       * arm would FREEZE the picture until the music caught back up to that frame. Restart the
       * video too: parking cur on the LAST frame makes the very next pump take its loop-wrap
       * arm (target ~0, gap >= half the clip) and seek+stage frame 0 -- both tracks restart
       * together. Audio absent/failed -> leave cur alone (silent clips keep free-running from
       * where they stopped, the pre-fix behaviour). */
      if(st == 0xA0 && gi_fmv_frames)
        gi_fmv_cur = (uint16_t)(gi_fmv_frames - 1u);
    }
  }
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
    target = ((s / 44100u) * gi_fmv_fps + (s % 44100u) * gi_fmv_fps / 44100u) % gi_fmv_frames;  /* == s*fps/44100 without 64-bit division */
  } else {
    target = (uint32_t)((gi_fmv_cur + 1u) % gi_fmv_frames);   /* no audio: free-run */
  }
  if(target == gi_fmv_cur) return;                            /* in the same frame -> hold */
  if(target < gi_fmv_cur) {
    if((uint32_t)(gi_fmv_cur - target) < (uint32_t)(gi_fmv_frames / 2u))
      return;                                                 /* video slightly ahead -> hold */
    /* loop wrap (target jumped back near 0) -> seek there (cheap, near the file start) */
    if(f_lseek(&gi_fmv_fil, FMV_DATA_START + target * FMV_FRAME_STRIDE)) { gi_fmv_close(); return; }
    if(gi_fmv_read_frame()) gi_fmv_cur = (uint16_t)target;
    return;
  }
  /* forward: read up to target (catch-up discards the skipped frames; the last read stays in
   * $CA0000). A large jump -- which steady play never produces -- seeks instead. */
  if(target - gi_fmv_cur > 30u) {
    if(f_lseek(&gi_fmv_fil, FMV_DATA_START + target * FMV_FRAME_STRIDE)) { gi_fmv_close(); return; }
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
/* The GAME_INFO command is done: the idle watchdog counts from here, not from gi_fmv_begin.
   What follows the clip's start in that command (opening the soundtrack, the guides scan of
   the game's info folder) takes over 300 ms on a big card, so the watchdog stopped every clip
   the moment the command ended and the first CMD_FMV_NEXT reopened the .fmv and the music. */
void gameinfo_fmv_touch(void) {
  gi_fmv_last_tick = getticks();
}

void gameinfo_fmv_idle_check(void) {
  /* Somebody else holds the DAC (the menu PCM player) and gets no CMD_FMV_NEXT at all --
     without this bail-out the watchdog below would stop their track 300 ms in. */
  if(menu_music_locked()) return;
  if(!gi_fmv_open && !menu_music_active()) return;
  if(time_after(getticks(), gi_fmv_last_tick + MS_TO_TICKS(300)))
    gameinfo_fmv_stop();
}

/* ---- The ROM's CRC32 on the title row ----------------------------------------------------
 * The bytes hashed are the ones the catalog (the Manager, the GameDB) indexes, the same rule as
 * the Manager's romSpan(): an .sfrom = the image inside the container (sfrom_span, what a load
 * stages); an .nes that starts with the iNES magic = the file minus its 16-byte header; any other
 * file = minus a 512-byte copier header when size % 1024 == 512, else whole. A list entry with a
 * patch is the BASE ROM's (sel_name cuts the tag), an MSU-1 folder the ROM inside it.
 * Reading a whole ROM takes seconds, so it never runs inside a command: gameinfo_load publishes
 * "pending" (or the cached value) and the menu loop hashes one 512-byte sector per step between
 * its other work (snes.c), so a command waits at most one sector. Any command the info screen
 * does not send cancels it (menucmd_fmv_gate); so does a write to the card (ff_wgen). The job
 * keeps its file in the FRAME, which is free from the end of gameinfo_load to the next FRAME
 * owner, and every owner is reached through a command -- the cancel comes first. */
uint8_t sfrom_span(FIL *fp, uint32_t *rom_offset, uint32_t *rom_size);   /* memory.c */

#define GI_CRC_PENDING 1
#define GI_CRC_READY   2
#define GI_CRC_FAILED  3        /* shown as the dashes too */
#define GI_CRC_CACHE_N 8

typedef struct {
  FIL      fil;
  uint32_t key;                 /* the cache key (gi_crc_start), then... */
  uint32_t crc;                 /* ...the running value: the two make the cache entry */
  uint32_t left;                /* bytes still to hash; 0 = the file is not open yet */
  uint32_t gen;                 /* ff_wgen when the job started */
  char     path[256];
} gi_crc_frame_t;
SCRATCH_FITS(gi_crc_frame_t, SCRATCH_FRAME_BYTES);

/* The cache at SRAM_GAMEINFO_CRCCACHE_ADDR: GI_CRC_CACHE_N {key, crc} pairs, filled round
   robin. The key mixes the write generation and the mount id into the path's hash, so an entry
   from before a write to the card, or from another card, simply never matches again. */
static uint8_t gi_crc_slot;

uint8_t gameinfo_crc_on;        /* 1 = a job owns the FRAME */

/* The 12-byte GI_CRC block: 8 hex digits (or dashes) + NUL, state, 'C','R' (state and magic
   last). */
static void gi_crc_publish(uint8_t state, uint32_t crc) {
  uint8_t b[12];
  for(int i = 0; i < 8; i++, crc <<= 4)
    b[i] = state == GI_CRC_READY ? "0123456789ABCDEF"[crc >> 28] : '-';
  b[8]  = 0;
  b[9]  = state;
  b[10] = 'C';
  b[11] = 'R';
  sram_writeblock(b, SRAM_GAMEINFO_CRC_ADDR, sizeof(b));
}

/* noinline: its cache copy stays out of gameinfo_load's frame */
__attribute__((noinline)) void gi_crc_start(const uint8_t *rom_path) {
  gi_crc_frame_t *const j = SCRATCH_FRAME(gi_crc_frame_t);
  uint32_t c[2 * GI_CRC_CACHE_N];
  uint32_t key = ff_wgen ^ ((uint32_t)ff_vol_id() << 16);
  for(const uint8_t *p = rom_path; *p; p++) key = crc32_update(key, *p);
  sram_readblock(c, SRAM_GAMEINFO_CRCCACHE_ADDR, sizeof(c));
  for(int i = 0; i < 2 * GI_CRC_CACHE_N; i += 2) {
    if(c[i] == key) { gi_crc_publish(GI_CRC_READY, c[i + 1]); return; }
  }
  gi_crc_publish(GI_CRC_PENDING, 0);
  j->key  = key;
  j->left = 0;
  j->gen  = ff_wgen;
  strlcpy_nul(j->path, (const char *)rom_path, sizeof(j->path));
  gameinfo_crc_on = 1;
}

void gameinfo_crc_cancel(void) {
  gameinfo_crc_on = 0;                       /* the FIL is read-only: nothing to close */
}

/* One step of the job: open + find the span, or hash one sector. Bounded (one SD read).
   noinline: inlined into the menu loop it made the compiler duplicate that loop. */
__attribute__((noinline)) void gameinfo_crc_step(void) {
  gi_crc_frame_t *const j = SCRATCH_FRAME(gi_crc_frame_t);
  UINT got;
  if(!gameinfo_crc_on) return;
  if(j->gen != ff_wgen) goto fail;           /* the card changed under the file */
  if(!j->left) {
    uint32_t off = 0, len;
    if(f_open(&j->fil, j->path, FA_READ) != FR_OK) goto fail;
    const char *dot = strrchr(j->path, '.');
    if(!(dot && !strcasecmp(dot + 1, "sfrom") && sfrom_span(&j->fil, &off, &len))) {
      off = 0;
      len = j->fil.fsize;
      if(dot && !strcasecmp(dot + 1, "nes")
         && f_read(&j->fil, file_buf, 4, &got) == FR_OK && got == 4
         && *(uint32_t *)file_buf == 0x1a53454eu)          /* "NES" $1A, the iNES magic */
        off = 16;
      if(!off && (len & 1023) == 512) off = 512;
      len -= off;
    }
    if(!len || f_lseek(&j->fil, off) != FR_OK) goto fail;
    j->left = len;
    j->crc  = crc32_init();
    return;
  }
  UINT want = j->left < sizeof(file_buf) ? (UINT)j->left : sizeof(file_buf);
  if(f_read(&j->fil, file_buf, want, &got) != FR_OK || got != want) goto fail;
  for(UINT i = 0; i < got; i++) j->crc = crc32_update(j->crc, file_buf[i]);
  if((j->left -= got)) return;
  j->crc = crc32_finalize(j->crc);           /* {key, crc}: one cache entry */
  sram_writeblock(&j->key, SRAM_GAMEINFO_CRCCACHE_ADDR + (gi_crc_slot++ % GI_CRC_CACHE_N) * 8u, 8);
  gi_crc_publish(GI_CRC_READY, j->crc);
  gameinfo_crc_on = 0;
  return;
fail:
  gi_crc_publish(GI_CRC_FAILED, 0);
  gameinfo_crc_on = 0;
}
