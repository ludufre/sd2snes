/* sd2snes+ - in-game manual/guides viewer (MCU side). See manual.h for the SRAM contract and the
 * hard safety rules (bounded, fail-safe, never hangs the MCU command loop).
 *
 * CYCLE 2: a game may ship UP TO 8 guides ("<stem>.man" plus "<stem>.02.man".."<stem>.08.man",
 * all bucketed under /sd2snes/info like the game-info assets). At game load manual_stage_meta()
 * PROBES the 8 candidates directly (<=8 f_open + 40B header reads, gap-robust, compacted), caches
 * a per-guide table in AHB, and publishes:
 *   - MANUAL_GUIDES ($FF9000, 260B): the compacted guide list the GUIDES tab reads.
 *   - MANUAL_META   ($FF0760, 16B) : present bit + guide-0 npages + meta_abi sanity byte.
 * The in-game GUIDES tab then requests ONE WHOLE page at a time: SNES_CMD_MANUAL_S1PAGE ($46)
 * stages a scale-1 page (4bpp, 256px wide) via manual_stage_s1page().
 *
 * The 2x zoom is a SEPARATE path: SNES_CMD_MANUAL_ZPAGE ($45) stages ONE WHOLE scrollable 2x page
 * (4bpp, per-tile palette, <=119KB) into $C5/$C6 via manual_stage_zpage(), after which the viewer
 * pans over it with pure PSRAM->VRAM DMA and NO further MCU traffic. */

#include "config.h"
#include <string.h>
#include "ff.h"
#include "memory.h"
#include "cfg.h"        /* CFG.enable_game_manual gate */
#include "gameinfo.h"   /* gameinfo_info_base: shared /sd2snes/info bucket-path derivation */
#include "fileops.h"    /* file_lfn: the current game's path (in-game rebuild source, see below) */
#include "manual.h"
#include "psram_io.h"

extern cfg_t CFG;

/* .man header (40B, was 16B; the +4 ver / +5 bpp offsets are PRESERVED so the existing magic/
 * version/bpp check keeps its shape):
 *   +0  4  "MANL"        magic
 *   +4  1  ver = 1       (unchanged -- maintainer mandate: .man stays version 1 until release)
 *   +5  1  bpp = 8       (unchanged)
 *   +6  1  npages
 *   +7  1  flags         bit0 = LEGACY quadrant zoom (ignored), bit1 = scrollable zoom section
 *   +8  2  page_w = 256  LE
 *   +10 2  band_h = 224  LE
 *   +12 2  nblocks       LE  (1x navigable blocks; also the scrollable zoom PAGE count)
 *   +14 2  rsvd = 0      LE  (was zoom_nblocks under the retired quadrant scheme)
 *   +16 24 title[24]     FONT-ENCODED by the host, NUL-terminated (23 visible + NUL) */
#define MAN_MAGIC0        ('M')
#define MAN_MAGIC1        ('A')
#define MAN_MAGIC2        ('N')
#define MAN_MAGIC3        ('L')
#define MAN_VERSION       (1)
#define MAN_BPP           (8)
#define MAN_HEADER_SIZE   (40)
#define MAN_NPAGES_OFS    (6)
#define MAN_FLAGS_OFS     (7)
#define MAN_NBLOCKS_OFS   (12)   /* u16 LE */
#define MAN_TITLE_OFS     (16)
#define MAN_TITLE_LEN     (24)
#define MAN_FLAG_ZOOM     (0x01) /* header flags bit0: LEGACY quadrant zoom. NEVER honoured any more --
                                    the quadrant scheme is gone, so a stale asset must degrade to
                                    "1x only" rather than feed 8bpp screens to the scrollable path. */
#define MAN_FLAG_ZOOM2    (0x02) /* header flags bit1: scrollable 4bpp zoom section present */

/* --- scrollable zoom section (see utils/gen_man.py for the authoritative layout) ---------------
 * Z0 = MAN_HEADER_SIZE + nblocks*MAN_INDEX_ENTRY, then a 32B sub-header and an nblocks-entry page
 * directory. A zoom PAGE is a 1x block rendered at 2x: 512 x up-to-448, cut into 8px tile rows.
 *   sub-header: +0 "ZOOM" +4 zbpp=4 +5 zpal_count=8 +6 ztiles_per_row=64 +7 zattr_stride=64
 *               +8 zrow_bytes=2048 (u16) +10 zpal_bytes=256 (u16) +12 nzrows (u32)
 *               +16 zmax_rows (u16) +18 rsvd +20 pagedir_ofs (u32)
 *               +24 zpages (u16) +26 rsvd +28 blockmap_ofs (u32)
 *   dir entry (20B): tile_ofs u32, attr_ofs u32, pal_ofs u32, nrows u16, pix_h u16,
 *                    first_block u16, pdf_page u8, nblk_in_page u8
 *   block map (4B per 1x block @ blockmap_ofs): zoom page u16, entry Y u16 (2x pixels) --
 *                    where panning should start when the user presses Y on that 1x block
 * Every base is sector-aligned and every stride a sector multiple, so all zoom reads take FatFs's
 * direct multi-sector path with no window copy. */
#define MAN_ZHDR_BYTES     (32)
#define MAN_ZDIR_ENTRY     (20)
#define MAN_ZMAP_ENTRY     (4)      /* per 1x block: zoom page u16, entry Y u16 (2x pixels) */
#define MAN_Z_TILES_W      (64)     /* tiles across a 512px zoom page */
#define MAN_Z_ROW_BYTES    (2048u)  /* one tile row: 64 tiles x 32B (4bpp) = exactly 4 sectors */
#define MAN_Z_HALF_BYTES   (1024u)  /* per BG half of a row (32 tiles); rows are PRE-SPLIT in-file */
#define MAN_Z_ATTR_STRIDE  (64)     /* one attr byte per tile, value = palette << 2 */
#define MAN_Z_TMAP_STRIDE  (128)    /* prebuilt tilemap row: 64 entries x 2B */
#define MAN_Z_PAL_BYTES    (256)    /* 8 palettes x 16 BGR555 -> CGRAM 0..127 */
/* Tile rows in a zoom page. A zoom page is a WHOLE PDF PAGE at 2x, NOT a 1x band: cutting the
   zoom image at band boundaries made the view JUMP half-way down every page instead of panning
   continuously. 96 rows = 768px of 2x content, which is exactly what the two tile halves fit in
   $C50000..$C7FFFF; the host splits a taller page into consecutive zoom pages. */
#define MAN_Z_MAX_ROWS     (96)
#define MAN_Z_TILE0        (16)     /* first image tile; 0..15 are the viewer's OBJ HUD glyphs */
/* VRAM ring depth on the viewer side. The MCU bakes slot = r % MAN_Z_RING_ROWS into the tilemap
   words it stages, so this is a LOCKSTEP constant with MAN_Z_RING_ROWS in snes/memmap.i65 -- not a
   viewer-local choice. 28 rows are on screen, the 29th is the partial row sub-tile scroll needs. */
#define MAN_Z_RING_ROWS    (29)

/* --- scale-1 (1x) scrollable section, "SCL1": the SAME machinery at 256px wide. Its 32B
 * sub-header sits right after the ZOOM one and has the identical shape; its page directory uses
 * the same 20B entry. 32 tiles/row = 1024B, one BG layer, no split. */
#define MAN_S1_TILES_W     (32)
#define MAN_S1_ROW_BYTES   (1024u)
#define MAN_S1_ATTR_STRIDE (32)
#define MAN_S1_TMAP_STRIDE (64)     /* 32 entries x 2B */
#define MAN_S1_MAX_ROWS    (64)     /* 512px at 1x; fills bank C3 exactly, no bank crossing */

/* index entry (8B): offset(u32 LE), page(u8), block(u8), band_content_rows(u8), zflags(u8). The
 * index sits right after the header (file offset MAN_INDEX_OFS = 40) and holds exactly nblocks
 * entries -- the 1x stream only; the scrollable zoom section has its own page directory past it.
 * A block's absolute file offset is still read from its entry ON DEMAND (one tiny bounded read),
 * the index is the source of truth for smart-seam layouts. */
#define MAN_INDEX_ENTRY   (8)
#define MAN_INDEX_OFS     (MAN_HEADER_SIZE)

/* navigable-block cap (u16 sanity bound; no per-block state, so this is generous). */
#define MAN_MAX_BLOCKS    (4096)

/* up to 8 guides per game ("<stem>.man" + "<stem>.02.man".."<stem>.08.man"). */
#define MAN_MAX_GUIDES    (8)

/* MANUAL_GUIDES ($FF9000, 260B) -- the compacted guide list. Lockstep with memory.h / memmap.i65.
 *   +0  1  count      (0..8)
 *   +1  1  selected   (active guide in the tab; the SNES writes it; default 0)
 *   +2  2  rsvd
 *   +4  record[8], 32B each:
 *        +0  1  present       (1 = valid; list is compacted, so 0..count-1 are present)
 *        +1  1  nn            (0 = ".man", 2..8 = ".0N.man")
 *        +2  1  flags         (bit1 = scrollable zoom section present; bit0 legacy, never set)
 *        +3  1  npages
 *        +4  2  nblocks       LE
 *        +6  2  zoom_pages    LE  (= nblocks when zoom present, else 0)
 *        +8  24 title[24]     FONT-ENCODED NUL-term (copied raw from the .man header) */
#define MAN_GUIDES_HEAD     (4)
#define MAN_GUIDE_REC_BYTES (32)

/* MANUAL_META (16B @ SRAM_MANUAL_META_ADDR): +0 flags, +1 npages(guide 0), +2 meta_abi, +3.. rsvd */
#define MAN_META_FLAG_PRESENT  (0x01)   /* >=1 valid guide staged AND EnableGameManual is on */
#define MAN_META_FLAG_ERROR    (0x02)   /* a transient block-read failure (viewer shows error) */
#define MAN_META_FLAG_ZREADY   (0x04)   /* a zoom page is staged and valid in $C5/$C6 */
#define MAN_META_ABI           (3)      /* firmware<->igmenu.bin data-contract sanity (memmap.i65). 3: title[0] may carry a document-type slug (1..5) the shell translates */
#define MAN_META_ZNROWS_OFS    (6)      /* u8 tile rows in the staged zoom page */
#define MAN_META_ZPIXH_OFS     (7)      /* u16 LE 2x pixel height of the staged zoom page */
#define MAN_META_ZPAGE_OFS     (9)      /* u8 staged zoom page */
#define MAN_META_ZGUIDE_OFS    (10)     /* u8 staged zoom guide */
#define MAN_META_ZENTRYY_OFS   (11)     /* u16 LE entry Y for a block->zoom transition */
#define MAN_META_ZFIRSTBLK_OFS (13)     /* u16 LE first 1x block covering the staged zoom page */
#define MAN_META_ZNBLK_OFS     (15)     /* u8 how many 1x blocks cover it (leave-zoom targeting) */
/* MANUAL_S1META (16B @ SRAM_MANUAL_S1META_ADDR): the scale-1 twin of the zoom meta block. */
#define MAN_S1_FLAG_READY      (0x01)
#define MAN_S1META_NROWS_OFS   (1)      /* u8 tile rows in the staged 1x page */
#define MAN_S1META_PIXH_OFS    (2)      /* u16 LE 1x pixel height */
#define MAN_S1META_PAGE_OFS    (4)      /* u8 staged page */
#define MAN_S1META_GUIDE_OFS   (5)      /* u8 staged guide */
#define MAN_S1META_NPAGES_OFS  (6)      /* u16 LE scale-1 page count */

/* retry a glitched block read in place before giving up (mirror gi_fmv FMV_READ_RETRIES). */
#define MAN_READ_RETRIES  (3)

/* Dedicated FIL (like gameinfo's gi_fmv_fil) so a block stream never clobbers the shared
 * file_handle. The AHB guide table and the shared read buffer live IN_AHBRAM: the main LPC175x
 * SRAM is tight and a growing .bss can silently corrupt a global (and the mk2 AHB is tight too --
 * so NO persistent path buffer: the full guide path is built transiently in man_buf, and the
 * "/sd2snes/info/[<ns>/]<BB>/<stem>" base is re-derived on demand from the game path each time, never
 * retained; and NO per-guide title in RAM: titles are written straight through to the BSRAM guide
 * records during the probe). man_guides holds only the few fields the page stagers/HUD need
 * (nn/npages/nblocks/zoom) -- 8 * 5B. .ahbram is NOLOAD (not zeroed at boot), which is
 * fine: man_guide_count gates every reader and is reset by manual_stage_meta before anything else
 * runs. Touched only from the menu/in-game loop, not an IRQ.
 *
 * Path source: the probe re-derives from `rom_path` (manual_stage_meta's arg = the game being
 * loaded); the page stagers re-derive from the global `file_lfn`, which is the current game's
 * path in-game (the same source savestate.c / the COMBO_TRANSITION reload use). A rare base
 * mismatch (e.g. a patched game whose recent-list entry differs) just fails the reopen -> the
 * error latch -> the viewer shows an error, never a hang or wrong file. */
static FIL      man_fil;
static uint8_t  man_open;                 /* 1 while man_fil is a valid open .man */
static uint8_t  man_guide_count;          /* compacted valid guide count (0..8) */
static uint8_t  man_open_nn;              /* nn of the guide currently open in man_fil; 0xff = none */
static struct { uint8_t nn, npages, zoom;      /* zoom: 1 = scrollable zoom section present */
                uint16_t nblocks; } man_guides[MAN_MAX_GUIDES] IN_AHBRAM;
static uint8_t  man_buf[512]  IN_AHBRAM;  /* shared path/header/index/stream scratch */
/* Which zoom page is currently resident in $C5/$C6. Staging one is ~180KB of SPI (~180ms), and
   BOTH 1x bands of a PDF page map to the SAME zoom page -- so remembering it turns "toggle 2x on
   the other half of the page" from a visible reload into an instant scroll. 0xff = none. */
static uint8_t  man_zres_guide = 0xff;
static uint16_t man_zres_page;
static uint8_t  man_s1res_guide = 0xff;   /* same idea for the scale-1 page (own PSRAM region, so */
static uint16_t man_s1res_page;           /*   1x and 2x stay resident together -> instant toggle) */
/* Last path staged by manual_stage_meta_cached() (the MENU-side entry point). The pre-boot game
   info screen restages the meta on EVERY Up/Down, and a full stage is a whole directory pass plus
   up to 8 f_open + header reads -- AND it clears the in-game session magic ($F4819E). Remembering
   the path makes a repeat visit to the same game a true no-op on both counts.
   The path buffer is IN_AHBRAM: 256B is a big bite out of the tight main SRAM, it is never a DMA
   target, and it is always written before it is read. The VALIDITY FLAG deliberately stays in the
   normal .bss (1B, zero-initialised at boot): .ahbram is NOLOAD, so a flag living there would
   start as power-on garbage and could fake a hit against a garbage path. */
static char     man_meta_cache_path[256] IN_AHBRAM;
static uint8_t  man_meta_cache_valid;     /* .bss on purpose -- see above */
static uint8_t  man_meta_cache_cfg;       /* CFG.enable_game_manual captured at arm time: toggling
                                             the option must invalidate the hit, or a cached
                                             "present" would survive the user turning it OFF */
/* Where man_stage_zattrs builds one prebuilt tilemap row, INSIDE man_buf (attr bytes land at +0,
   the 128B of entries at +MAN_ZMAP_OFS). A dedicated buffer is a bad idea: the AHB region is down
   to ~100B free (measured from the .map after man_meta_cache_path went in; it was ~356B before),
   and the main .bss is exactly where a growing global silently corrupts something else (see
   memory.h). 64 + 128 fits man_buf twice over. */
#define MAN_ZMAP_OFS  (256)

/* Read a u16/u32 LE out of man_buf without alignment assumptions (man_buf is a byte scratch). */
static uint16_t man_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t man_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void man_close(void) {
  if(man_open) { f_close(&man_fil); man_open = 0; }
}

/* Re-derive "/sd2snes/info/[<ns>/]<BB>/<stem>" from `rom_path` into man_buf, append suffix(nn), and open
   read-only (man_open/man_open_nn set on success). The path only has to live across f_open, so
   man_buf is safe to reuse for header/index/stream after. nn==0 -> ".man"; 2..8 -> ".0N.man".
   Bounded; 0 on error. */
static uint8_t man_probe_hit[MAN_MAX_GUIDES];  /* which candidates the directory scan found */
/* 1 only once the directory scan actually RAN. Without this, any reason the scan could not run
   (path did not fit, no '/', f_opendir failed) leaves man_probe_hit all-zero and silently hides
   every guide -- worse than the un-optimised code, which at least still tried to open them. */
static uint8_t man_probe_scanned;

static int man_open_guide(const uint8_t *rom_path, uint8_t nn) {
  int n;
  /* reserve 8 bytes (longest suffix ".0N.man" = 7 + NUL) so the append can never overrun. */
  gameinfo_info_base(rom_path, (char *)man_buf, (int)sizeof(man_buf) - 8);
  n = (int)strlen((char *)man_buf);
  if(!n) return 0;
  if(nn == 0) {
    man_buf[n++] = '.'; man_buf[n++] = 'm'; man_buf[n++] = 'a'; man_buf[n++] = 'n';
  } else {
    man_buf[n++] = '.'; man_buf[n++] = '0'; man_buf[n++] = (uint8_t)('0' + nn);
    man_buf[n++] = '.'; man_buf[n++] = 'm'; man_buf[n++] = 'a'; man_buf[n++] = 'n';
  }
  man_buf[n] = 0;
  if(f_open(&man_fil, (const TCHAR *)man_buf, FA_READ)) return 0;
  man_open = 1;
  man_open_nn = nn;
  return 1;
}

/* Zero MANUAL_GUIDES ($FF9000): head (count/selected/rsvd) + all 8 record slots. Fail-safe reset;
   the probe then writes real records write-through and finally patches the count into the head. */
static void man_guides_clear(void) {
  uint8_t z[MAN_GUIDE_REC_BYTES];
  int k;
  memset(z, 0, sizeof(z));
  sram_writeblock(z, SRAM_MANUAL_GUIDES_ADDR, MAN_GUIDES_HEAD);
  for(k = 0; k < MAN_MAX_GUIDES; k++)
    sram_writeblock(z, SRAM_MANUAL_GUIDES_ADDR + MAN_GUIDES_HEAD
                       + (uint32_t)k * MAN_GUIDE_REC_BYTES, MAN_GUIDE_REC_BYTES);
}

/* Publish MANUAL_META ($FF0760, 16B): present bit + guide-0 npages + meta_abi. Clears the error
   latch (fresh at game load). meta_abi = MAN_META_ABI (=3) so a mismatched igmenu.bin degrades cleanly. */
static void man_publish_meta(void) {
  uint8_t meta[16];
  memset(meta, 0, sizeof(meta));
  if(man_guide_count) {
    meta[0] = MAN_META_FLAG_PRESENT;
    meta[1] = man_guides[0].npages;
  }
  meta[2] = MAN_META_ABI;
  sram_writeblock(meta, SRAM_MANUAL_META_ADDR, sizeof(meta));
}

void manual_stage_meta(uint8_t *rom_path) {
  UINT     got;
  int      c;

  /* Drop the menu-side path cache before anything else: every caller of THIS function wants a
     real restage (the game-load path in memory.c depends on the probe + the $F4819E clear running
     again). Only the manual_stage_meta_cached() wrapper re-arms it, after the stage completed. */
  man_meta_cache_valid = 0;

  /* fail-safe FIRST: reset state and publish "not present" so a missing/bad set of guides (or the
     CFG-off / early-return paths) never leaves stale meta from a previous game. */
  man_close();
  man_guide_count = 0;
  man_open_nn     = 0xff;
  man_zres_guide  = 0xff;   /* a new game invalidates whatever zoom page was resident */
  man_s1res_guide = 0xff;
  man_guides_clear();
  man_publish_meta();

  /* Invalidate the in-game menu session gate: a NEW game must not resume the previous game's
     remembered manual position or reopen on its last tab. PSRAM $F4 survives a short power-cycle,
     so clearing it here (runs on every game load, before the CFG-off return) is what actually
     bounds the persistence to one game session. Lockstep with man_pos_magic in snes/igmenu.a65. */
  sram_writeshort(0x0000, IGMENU_PERSIST_MAGIC_ADDR);

  if(!CFG.enable_game_manual) return;   /* toggle off -> stay "not present" */

  /* Find which of the 8 candidates exist with ONE directory pass. A failed f_open scans the
     WHOLE directory comparing long names, and /sd2snes/info/<C> holds hundreds of entries, so
     seven misses cost seven full scans (~1.1s of the game load, measured). One f_readdir pass
     finds them all, and only the guides that actually exist get opened.
     Resolving the path once with f_chdir was tried and REMOVED: it barely dented the cost (the
     scan was the cost, not the walk), and it made the probe depend on global CWD. */
  { DIR      dir;
    FILINFO  fno;
    char    *lfn  = (char *)man_buf;                  /* 256B LFN work area */
    char    *base = (char *)man_buf + 256;            /* "/sd2snes/info/[<ns>/]<BB>/<stem>" */
    const char *leaf;
    int i, n, cut = -1, slen;

    memset(man_probe_hit, 0, sizeof(man_probe_hit));
    man_probe_scanned = 0;
    gameinfo_info_base(rom_path, base, 256);
    n = (int)strlen(base);
    for(i = 0; i < n; i++) if(base[i] == '/') cut = i;
    if(cut > 0) {
      base[cut] = 0;
      leaf  = base + cut + 1;
      slen  = (int)strlen(leaf);
      fno.lfname = lfn;
      fno.lfsize = 256;
      if(f_opendir(&dir, (const TCHAR *)base) == FR_OK) {
        man_probe_scanned = 1;                       /* the hit table is now authoritative */
        while(f_readdir(&dir, &fno) == FR_OK) {
          const char *nm = *fno.lfname ? fno.lfname : fno.fname;
          const char *sfx;
          if(!nm[0]) break;                            /* end of directory */
          if(fno.fattrib & AM_DIR) continue;
          if(strncasecmp(nm, leaf, (size_t)slen)) continue;
          sfx = nm + slen;
          if(!strcasecmp(sfx, ".man")) man_probe_hit[0] = 1;
          else if(sfx[0] == '.' && sfx[1] == '0' && sfx[2] >= '2' && sfx[2] <= '8'
                  && !strcasecmp(sfx + 3, ".man"))
            man_probe_hit[sfx[2] - '0' - 1] = 1;
        }
      }
      base[cut] = '/';
    }
  }

  /* PROBE 8 candidates directly: nn = 0 (".man"), then 2..8 (".0N.man"). Missing/invalid -> skip
     and continue (gap-robust: deleting .03.man does not stop the scan at .02.man). Valid guides are
     COMPACTED (only the present ones enter the list, in probe order). <=8 f_open + 40B header reads.
     Titles are written STRAIGHT THROUGH to the BSRAM record (no title kept in RAM). */
  for(c = 0; c < MAN_MAX_GUIDES; c++) {
    uint8_t  nn = (c == 0) ? 0 : (uint8_t)(c + 1);   /* 0, 2, 3, 4, 5, 6, 7, 8 */
    uint8_t  npages, flags, k, zoom;
    uint16_t nblocks, zpages;
    uint8_t  rec[MAN_GUIDE_REC_BYTES];

    /* Skip only what the scan POSITIVELY ruled out. If the scan could not run, probe everything --
       slower, but it can never hide a guide that is really there. */
    if(man_probe_scanned && !man_probe_hit[c]) continue;
    if(!man_open_guide(rom_path, nn)) continue;

    if(f_read(&man_fil, man_buf, MAN_HEADER_SIZE, &got) || got != MAN_HEADER_SIZE
       || man_buf[0] != MAN_MAGIC0 || man_buf[1] != MAN_MAGIC1
       || man_buf[2] != MAN_MAGIC2 || man_buf[3] != MAN_MAGIC3
       || man_buf[4] != MAN_VERSION || man_buf[5] != MAN_BPP) {
      man_close(); continue;
    }
    npages  = man_buf[MAN_NPAGES_OFS];
    flags   = man_buf[MAN_FLAGS_OFS];
    nblocks = (uint16_t)(man_buf[MAN_NBLOCKS_OFS] | (man_buf[MAN_NBLOCKS_OFS + 1] << 8));
    if(nblocks == 0) { man_close(); continue; }
    if(nblocks > MAN_MAX_BLOCKS) nblocks = MAN_MAX_BLOCKS;
    /* A zoom PAGE is a 1x block rendered at 2x, so the page count IS nblocks -- there is no
       separate count in the header to trust. Legacy bit0 is deliberately not consulted: a stale
       quadrant-era asset must degrade to 1x-only, never feed 8bpp screens to the scrollable path. */
    zoom   = (uint8_t)((flags & MAN_FLAG_ZOOM2) ? 1 : 0);
    zpages = 0;
    if(zoom) {
      /* A zoom page is a whole PDF page (split only if very tall), so the count lives in the
         zoom sub-header -- it is NOT nblocks. One extra 32B read per guide during the probe. */
      UINT g2;
      uint32_t z0 = (uint32_t)MAN_HEADER_SIZE + (uint32_t)nblocks * MAN_INDEX_ENTRY;
      uint8_t  zh[MAN_ZHDR_BYTES];
      if(!f_lseek(&man_fil, z0)
         && !f_read(&man_fil, zh, MAN_ZHDR_BYTES, &g2) && g2 == MAN_ZHDR_BYTES
         && zh[0] == 'Z' && zh[1] == 'O' && zh[2] == 'O' && zh[3] == 'M')
        zpages = man_rd16(zh + 24);
      if(!zpages) zoom = 0;                 /* unreadable/empty zoom section -> 1x only */
    }

    k = man_guide_count++;
    man_guides[k].nn      = nn;
    man_guides[k].npages  = npages;
    man_guides[k].nblocks = nblocks;
    man_guides[k].zoom    = zoom;

    /* write-through the full BSRAM record now (title lives only here, straight from man_buf). */
    memset(rec, 0, sizeof(rec));
    rec[0] = 1;                                        /* present */
    rec[1] = nn;
    rec[2] = (uint8_t)(flags & MAN_FLAG_ZOOM2);        /* publish only the bit we honour */
    rec[3] = npages;
    rec[4] = (uint8_t)(nblocks & 0xff);
    rec[5] = (uint8_t)(nblocks >> 8);
    rec[6] = (uint8_t)(zpages & 0xff);
    rec[7] = (uint8_t)(zpages >> 8);
    memcpy(rec + 8, man_buf + MAN_TITLE_OFS, MAN_TITLE_LEN);
    rec[MAN_GUIDE_REC_BYTES - 1] = 0;                 /* guarantee title NUL-term (host should too) */
    sram_writeblock(rec, SRAM_MANUAL_GUIDES_ADDR + MAN_GUIDES_HEAD
                         + (uint32_t)k * MAN_GUIDE_REC_BYTES, sizeof(rec));

    man_close();
  }

  /* open-on-demand model: don't hold a FIL open between meta-stage and the first block. */
  man_close();
  man_open_nn = 0xff;

  if(man_guide_count) {
    uint8_t head[4];
    head[0] = man_guide_count; head[1] = 0; head[2] = 0; head[3] = 0;   /* selected default 0 */
    sram_writeblock(head, SRAM_MANUAL_GUIDES_ADDR, sizeof(head));
    man_publish_meta();
  }
}

/* Menu-side wrapper: stage the meta only when the game actually changed. See manual.h. */
void manual_stage_meta_cached(uint8_t *rom_path) {
  unsigned len;

  if(man_meta_cache_valid && man_meta_cache_cfg == CFG.enable_game_manual
     && !strcmp(man_meta_cache_path, (const char *)rom_path)) return;

  manual_stage_meta(rom_path);          /* clears man_meta_cache_valid itself */

  /* Re-arm the cache with the path we just staged. A path that does not FIT is simply left
     uncached (restage every time, i.e. today's behaviour): storing a truncated key would make two
     long paths sharing a prefix collide and show the wrong game's guides. */
  len = (unsigned)strlen((const char *)rom_path);
  if(len && len < sizeof(man_meta_cache_path)) {
    memcpy(man_meta_cache_path, rom_path, len + 1);
    man_meta_cache_cfg = CFG.enable_game_manual;
    man_meta_cache_valid = 1;
  }
}

/* Drop the "this page is already resident in PSRAM" memo. MANDATORY after anything overwrites
 * $C30000.. or $C50000.. behind our back -- concretely CMD_READDIR, whose file-STRING table grows
 * from SRAM_MANUAL_S1TILES_ADDR ($C30000) and runs right through both staging regions.
 *
 * Without this the menu-side viewer (snes/manhost.a65: X on the game-info screen) breaks on the
 * SECOND open: its restore fires a READDIR to rebuild the browser's string table, the re-query is
 * a manual_stage_meta_cached() HIT so nothing else resets anything, and the next
 * manual_stage_s1page() for the same (guide, page) takes the skip branch -- leaving the SNES to
 * DMA the browser's FILENAMES into VRAM as 4bpp tiles.
 *
 * Only the residency memo is dropped, on purpose: the guide TABLE stays valid, so this costs
 * nothing but one page restage instead of a whole directory pass per viewer exit. */
void manual_invalidate_resident(void) {
  man_zres_guide  = 0xff;
  man_s1res_guide = 0xff;
}

/* ---------------------------------------------------------------- scrollable 2x zoom ----------
 * Stage ONE WHOLE 2x page (<=119KB) so the viewer can pan over it with nothing but PSRAM->VRAM
 * DMA. This is the design's load-bearing choice: with the page resident there is no per-row MCU
 * traffic during a scroll, so the pan cannot stall, cannot show a half-streamed row, and needs no
 * prefetch FIFO or watchdog. The cost lands once per page turn instead of once per nav step. */

/* Read + validate the zoom sub-header and page `page`'s directory entry. Validates exactly the
   fields (and in the order) that utils/gen_man.py's --verify audit checks, so a file the host
   rejects is a file this rejects. 0 on any error. */
/* Locate one of the two scroll sub-headers into man_buf. `which` = 0 -> "ZOOM" (2x), 1 -> "SCL1"
   (1x). They are 32B each, back to back at Z0, and share the same field layout. 0 on error. */
static int man_scroll_hdr(uint8_t guide, uint8_t which) {
  UINT     got;
  uint32_t z0 = (uint32_t)MAN_HEADER_SIZE
              + (uint32_t)man_guides[guide].nblocks * MAN_INDEX_ENTRY
              + (uint32_t)which * MAN_ZHDR_BYTES;
  if(f_lseek(&man_fil, z0)) return 0;
  if(f_read(&man_fil, man_buf, MAN_ZHDR_BYTES, &got) || got != MAN_ZHDR_BYTES) return 0;
  if(which == 0) {
    if(man_buf[0] != 'Z' || man_buf[1] != 'O' || man_buf[2] != 'O' || man_buf[3] != 'M') return 0;
  } else {
    if(man_buf[0] != 'S' || man_buf[1] != 'C' || man_buf[2] != 'L' || man_buf[3] != '1') return 0;
  }
  if(man_buf[4] != 4) return 0;                     /* 4bpp */
  return 1;
}

static int man_zdir(uint8_t guide, uint16_t page, uint32_t *tile_ofs, uint32_t *attr_ofs,
                    uint32_t *pal_ofs, uint16_t *nrows, uint16_t *pix_h,
                    uint16_t *first_block, uint8_t *pdf_page, uint8_t *nblk) {
  UINT     got;
  uint32_t pagedir_ofs;
  uint16_t zpages;

  if(!man_scroll_hdr(guide, 0)) return 0;
  if(man_buf[6] != MAN_Z_TILES_W         /* ztiles_per_row  */
     || man_buf[7] != MAN_Z_ATTR_STRIDE  /* zattr_stride    */
     || man_rd16(man_buf + 8)  != MAN_Z_ROW_BYTES
     || man_rd16(man_buf + 10) != MAN_Z_PAL_BYTES) return 0;
  pagedir_ofs = man_rd32(man_buf + 20);
  zpages      = man_rd16(man_buf + 24);
  if(page >= zpages) return 0;

  if(f_lseek(&man_fil, pagedir_ofs + (uint32_t)page * MAN_ZDIR_ENTRY)) return 0;
  if(f_read(&man_fil, man_buf, MAN_ZDIR_ENTRY, &got) || got != MAN_ZDIR_ENTRY) return 0;
  *tile_ofs    = man_rd32(man_buf + 0);
  *attr_ofs    = man_rd32(man_buf + 4);
  *pal_ofs     = man_rd32(man_buf + 8);
  *nrows       = man_rd16(man_buf + 12);
  *pix_h       = man_rd16(man_buf + 14);
  *first_block = man_rd16(man_buf + 16);
  *pdf_page    = man_buf[18];
  *nblk        = man_buf[19] ? man_buf[19] : 1;
  if(*nrows == 0) return 0;
  if(*nrows > MAN_Z_MAX_ROWS) *nrows = MAN_Z_MAX_ROWS;     /* clamp, never trust the file */
  if(*pix_h > (uint16_t)(*nrows * 8)) *pix_h = (uint16_t)(*nrows * 8);
  if(*first_block >= man_guides[guide].nblocks) *first_block = 0;
  return 1;
}

/* Resolve 1x block -> (zoom page, entry Y) through the block map. This is what makes pressing Y
   on the SECOND band of a page land half-way down the SAME zoom page instead of on a page of its
   own -- the whole point of the page being the PDF page rather than the band. */
static int man_zmap_lookup(uint8_t guide, uint16_t block, uint16_t *zpage, uint16_t *zy) {
  UINT     got;
  uint32_t blockmap_ofs;

  if(block >= man_guides[guide].nblocks) return 0;
  if(!man_scroll_hdr(guide, 0)) return 0;
  blockmap_ofs = man_rd32(man_buf + 28);
  if(!blockmap_ofs) return 0;
  if(f_lseek(&man_fil, blockmap_ofs + (uint32_t)block * MAN_ZMAP_ENTRY)) return 0;
  if(f_read(&man_fil, man_buf, MAN_ZMAP_ENTRY, &got) || got != MAN_ZMAP_ENTRY) return 0;
  *zpage = man_rd16(man_buf + 0);
  *zy    = man_rd16(man_buf + 2);
  return 1;
}

/* One pass over the attr array, turning each 64-byte attr row into the 64 prebuilt tilemap entries
   the viewer DMAs straight into both tilemaps. Doing it here (not on the 65816) keeps the viewer's
   per-8px-scroll work down to four DMAs with no CPU loop.
     entry = tile | (pal << 10),  tile = MAN_Z_TILE0 + (r % MAN_Z_RING_ROWS)*32 + col
   The BG1 half (cols 0-31) and BG2 half (cols 32-63) use the SAME tile numbering because each is
   indexed against its own char base, so one formula serves both. Attr bytes are pre-shifted
   (palette << 2) by the host, so the palette bits are just `attr << 8`. */
static int man_stage_zattrs(uint32_t attr_ofs, uint16_t nrows, uint16_t row0) {
  UINT     got;
  uint16_t r;
  uint8_t  slot = (uint8_t)row0;           /* r % MAN_Z_RING_ROWS, kept as a counter: no division.
                                              row0 (centering pad, see MAN_VIS_ROWS) is only ever
                                              nonzero for pages < one window, so slot never wraps */

  if(f_lseek(&man_fil, attr_ofs)) return 0;
  for(r = 0; r < nrows; r++) {
    uint8_t *m = man_buf + MAN_ZMAP_OFS;
    uint16_t base = (uint16_t)(MAN_Z_TILE0 + (uint16_t)slot * 32);
    int c;
    if(f_read(&man_fil, man_buf, MAN_Z_ATTR_STRIDE, &got) || got != MAN_Z_ATTR_STRIDE) return 0;
    for(c = 0; c < MAN_Z_TILES_W; c++) {
      uint16_t tile = (uint16_t)(base + (c & 31));        /* col within this layer's own half */
      m[c * 2]     = (uint8_t)(tile & 0xff);
      m[c * 2 + 1] = (uint8_t)((tile >> 8) | man_buf[c]); /* attr = pal<<2 == word bits 10-12 */
    }
    sram_writeblock(m, SRAM_MANUAL_ZTMAP_ADDR + (uint32_t)(row0 + r) * MAN_Z_TMAP_STRIDE,
                    MAN_Z_TMAP_STRIDE);
    if(++slot >= MAN_Z_RING_ROWS) slot = 0;
  }
  return 1;
}

/* One sequential pass over the tile rows. Each in-file row is already PRE-SPLIT (1024B of cols
   0-31 then 1024B of cols 32-63), so this is two contiguous copies per row into the two banks --
   no de-interleaving, and no DMA the viewer issues ever crosses a bank boundary. */
static int man_stage_ztiles(uint32_t tile_ofs, uint16_t nrows, uint16_t row0) {
  uint16_t r;

  if(f_lseek(&man_fil, tile_ofs)) return 0;
  for(r = 0; r < nrows; r++) {
    uint32_t dst[2];
    int      half;
    dst[0] = SRAM_MANUAL_ZTILES_A_ADDR + (uint32_t)(row0 + r) * MAN_Z_HALF_BYTES;
    dst[1] = SRAM_MANUAL_ZTILES_B_ADDR + (uint32_t)(row0 + r) * MAN_Z_HALF_BYTES;
    for(half = 0; half < 2; half++)
      if(!psram_stream_buf(&man_fil, dst[half], MAN_Z_HALF_BYTES,
                           man_buf, sizeof(man_buf), 0)) return 0;
  }
  return 1;
}

/* ---------------------------------------------------------------- scale-1 (1x) page ----------
 * The 1x view is the same scrollable machine at 256px: 32 tiles/row, one BG layer, no split.
 * Staging is therefore the 2x path minus the second half -- one 1024B copy per row instead of
 * two, and a 64B tilemap row instead of 128. */
static int man_stage_s1attrs(uint32_t attr_ofs, uint16_t nrows, uint16_t row0) {
  UINT     got;
  uint16_t r;
  uint8_t  slot = (uint8_t)row0;           /* r % MAN_Z_RING_ROWS -- same ring depth as the 2x;
                                              row0 nonzero only for pages < one window (no wrap) */

  if(f_lseek(&man_fil, attr_ofs)) return 0;
  for(r = 0; r < nrows; r++) {
    uint8_t *m = man_buf + MAN_ZMAP_OFS;
    uint16_t base = (uint16_t)(MAN_Z_TILE0 + (uint16_t)slot * MAN_S1_TILES_W);
    int c;
    if(f_read(&man_fil, man_buf, MAN_S1_ATTR_STRIDE, &got) || got != MAN_S1_ATTR_STRIDE) return 0;
    for(c = 0; c < MAN_S1_TILES_W; c++) {
      uint16_t tile = (uint16_t)(base + c);
      m[c * 2]     = (uint8_t)(tile & 0xff);
      m[c * 2 + 1] = (uint8_t)((tile >> 8) | man_buf[c]);   /* attr = pal<<2 == word bits 10-12 */
    }
    sram_writeblock(m, SRAM_MANUAL_S1TMAP_ADDR + (uint32_t)(row0 + r) * MAN_S1_TMAP_STRIDE,
                    MAN_S1_TMAP_STRIDE);
    if(++slot >= MAN_Z_RING_ROWS) slot = 0;
  }
  return 1;
}

static int man_stage_s1tiles(uint32_t tile_ofs, uint16_t nrows, uint16_t row0) {
  if(f_lseek(&man_fil, tile_ofs)) return 0;
  /* one contiguous run: rows are not split at 1x */
  return psram_stream_buf(&man_fil,
                          SRAM_MANUAL_S1TILES_ADDR + (uint32_t)row0 * MAN_S1_ROW_BYTES,
                          (uint32_t)nrows * MAN_S1_ROW_BYTES,
                          man_buf, sizeof(man_buf), 0);
}

/* ---- viewport floor for SHORT pages -------------------------------------------------------
 * The visible window is MAN_VIS_ROWS tile rows (224px) in BOTH views, and the viewer refills
 * that whole window from the per-row PSRAM arrays regardless of the page's nrows. A page
 * shorter than the window (pre-spread-split guides shipped 54-92px pages) therefore shows
 * whatever PSRAM held beyond the content -- the previous guide's tiles, the browser string
 * table -- as garbage rows. Fix at the STAGE: CENTER the content vertically (row0 =
 * (MAN_VIS_ROWS - nrows) / 2) and pad the rows above and below with zeroed 4bpp tiles
 * (colour 0 = transparent -> backdrop, which CGRAM 0 makes the page's own background colour)
 * plus matching prebuilt tilemap words, then publish the PADDED nrows/pix_h so the viewer
 * treats the page as exactly one window tall (scroll/pan clamp collapses to 0; no code change
 * on the 65816 side). The 1x<->2x toggle is offset-safe: with zero pan range the 2x always
 * shows its own centered page from the top, so the two views never need matching offsets.
 * Rows < MAN_Z_RING_ROWS by construction, so slot == row and the padded words point at the
 * rows just zeroed. */
#define MAN_VIS_ROWS 28

static void man_blank_s1rows(uint16_t from, uint16_t to) {
  uint16_t r;
  for(r = from; r < to; r++) {
    uint8_t *m = man_buf + MAN_ZMAP_OFS;
    uint16_t base = (uint16_t)(MAN_Z_TILE0 + r * MAN_S1_TILES_W);
    int c;
    for(c = 0; c < MAN_S1_TILES_W; c++) {
      uint16_t tile = (uint16_t)(base + c);
      m[c * 2]     = (uint8_t)(tile & 0xff);
      m[c * 2 + 1] = (uint8_t)(tile >> 8);               /* attr 0: tile data below is zeroed */
    }
    sram_writeblock(m, SRAM_MANUAL_S1TMAP_ADDR + (uint32_t)r * MAN_S1_TMAP_STRIDE,
                    MAN_S1_TMAP_STRIDE);
    sram_memset(SRAM_MANUAL_S1TILES_ADDR + (uint32_t)r * MAN_S1_ROW_BYTES, MAN_S1_ROW_BYTES, 0);
  }
}

static void man_blank_zrows(uint16_t from, uint16_t to) {
  uint16_t r;
  for(r = from; r < to; r++) {
    uint8_t *m = man_buf + MAN_ZMAP_OFS;
    uint16_t base = (uint16_t)(MAN_Z_TILE0 + r * 32);
    int c;
    for(c = 0; c < MAN_Z_TILES_W; c++) {
      uint16_t tile = (uint16_t)(base + (c & 31));
      m[c * 2]     = (uint8_t)(tile & 0xff);
      m[c * 2 + 1] = (uint8_t)(tile >> 8);
    }
    sram_writeblock(m, SRAM_MANUAL_ZTMAP_ADDR + (uint32_t)r * MAN_Z_TMAP_STRIDE,
                    MAN_Z_TMAP_STRIDE);
    sram_memset(SRAM_MANUAL_ZTILES_A_ADDR + (uint32_t)r * MAN_Z_HALF_BYTES, MAN_Z_HALF_BYTES, 0);
    sram_memset(SRAM_MANUAL_ZTILES_B_ADDR + (uint32_t)r * MAN_Z_HALF_BYTES, MAN_Z_HALF_BYTES, 0);
  }
}

void manual_stage_s1page(uint8_t guide, uint16_t page) {
  int      tries;
  uint8_t  nn;
  uint16_t npages = 0;

  sram_writebyte(0, SRAM_MANUAL_S1META_ADDR);     /* not ready until fully staged */

  if(guide >= man_guide_count) return;
  if(!man_guides[guide].zoom) return;             /* no scroll sections at all */
  nn = man_guides[guide].nn;
  if(nn != man_open_nn || !man_open) {
    man_close();
    if(!man_open_guide(file_lfn, nn)) man_open_nn = 0xff;
  }

  for(tries = 0; tries < MAN_READ_RETRIES; tries++) {
    uint32_t pagedir_ofs, tile_ofs, attr_ofs, pal_ofs;
    uint16_t nrows, pix_h;
    uint8_t  pdf_page;
    UINT     got;

    if(!man_open && !man_open_guide(file_lfn, nn)) break;
    if(!man_scroll_hdr(guide, 1)) { man_close(); continue; }   /* "SCL1" */
    if(man_buf[6] != MAN_S1_TILES_W || man_rd16(man_buf + 8) != MAN_S1_ROW_BYTES) {
      man_close(); continue;
    }
    pagedir_ofs = man_rd32(man_buf + 20);
    npages      = man_rd16(man_buf + 24);
    if(page >= npages) { man_close(); break; }

    if(f_lseek(&man_fil, pagedir_ofs + (uint32_t)page * MAN_ZDIR_ENTRY)
       || f_read(&man_fil, man_buf, MAN_ZDIR_ENTRY, &got) || got != MAN_ZDIR_ENTRY) {
      man_close(); continue;
    }
    tile_ofs = man_rd32(man_buf + 0);
    attr_ofs = man_rd32(man_buf + 4);
    pal_ofs  = man_rd32(man_buf + 8);
    nrows    = man_rd16(man_buf + 12);
    pix_h    = man_rd16(man_buf + 14);
    pdf_page = man_buf[18];
    if(nrows == 0) { man_close(); continue; }
    if(nrows > MAN_S1_MAX_ROWS) nrows = MAN_S1_MAX_ROWS;
    if(pix_h > (uint16_t)(nrows * 8)) pix_h = (uint16_t)(nrows * 8);

    if(!(man_s1res_guide == guide && man_s1res_page == page)) {
      if(f_lseek(&man_fil, pal_ofs)
         || f_read(&man_fil, man_buf, MAN_Z_PAL_BYTES, &got) || got != MAN_Z_PAL_BYTES) {
        man_close(); continue;
      }
      uint16_t row0 = (nrows < MAN_VIS_ROWS) ? (uint16_t)((MAN_VIS_ROWS - nrows) / 2) : 0;
      sram_writeblock(man_buf, SRAM_MANUAL_S1PAL_ADDR, MAN_Z_PAL_BYTES);
      if(!man_stage_s1attrs(attr_ofs, nrows, row0)) { man_close(); man_s1res_guide = 0xff; continue; }
      if(!man_stage_s1tiles(tile_ofs, nrows, row0)) { man_close(); man_s1res_guide = 0xff; continue; }
      if(row0) man_blank_s1rows(0, row0);
      if((uint16_t)(row0 + nrows) < MAN_VIS_ROWS) man_blank_s1rows((uint16_t)(row0 + nrows), MAN_VIS_ROWS);
      man_s1res_guide = guide;
      man_s1res_page  = page;
    }
    /* publish the PADDED size for short pages (see man_blank_s1rows) */
    if(nrows < MAN_VIS_ROWS) nrows = MAN_VIS_ROWS;
    if(pix_h < (uint16_t)(MAN_VIS_ROWS * 8)) pix_h = (uint16_t)(MAN_VIS_ROWS * 8);

    sram_writebyte((uint8_t)nrows,           SRAM_MANUAL_S1META_ADDR + MAN_S1META_NROWS_OFS);
    sram_writebyte((uint8_t)(pix_h & 0xff),  SRAM_MANUAL_S1META_ADDR + MAN_S1META_PIXH_OFS);
    sram_writebyte((uint8_t)(pix_h >> 8),    SRAM_MANUAL_S1META_ADDR + MAN_S1META_PIXH_OFS + 1);
    sram_writebyte((uint8_t)page,            SRAM_MANUAL_S1META_ADDR + MAN_S1META_PAGE_OFS);
    sram_writebyte(guide,                    SRAM_MANUAL_S1META_ADDR + MAN_S1META_GUIDE_OFS);
    sram_writebyte((uint8_t)(npages & 0xff), SRAM_MANUAL_S1META_ADDR + MAN_S1META_NPAGES_OFS);
    sram_writebyte((uint8_t)(npages >> 8),   SRAM_MANUAL_S1META_ADDR + MAN_S1META_NPAGES_OFS + 1);
    /* The HUD reads MANUAL_META+3 for "Pg X/N" in BOTH views, so the 1x has to publish it too --
       otherwise it shows whatever the 2x left there and the page number disagrees with what is
       on screen. */
    sram_writebyte(pdf_page,                 SRAM_MANUAL_META_ADDR + 3);
    sram_writebyte(MAN_S1_FLAG_READY,        SRAM_MANUAL_S1META_ADDR);   /* publish LAST */
    return;
  }
  man_s1res_guide = 0xff;
}

void manual_stage_zpage(uint8_t guide, uint16_t index, uint8_t mode) {
  int      tries;
  uint8_t  nn, f;
  uint16_t page, entry_y = 0;

  /* Drop the ready flag FIRST: if anything below fails, the viewer must see "no zoom page"
     rather than pan over whatever the previous page left in $C5/$C6. */
  f = sram_readbyte(SRAM_MANUAL_META_ADDR);
  sram_writebyte((uint8_t)(f & ~(MAN_META_FLAG_ZREADY | MAN_META_FLAG_ERROR)),
                 SRAM_MANUAL_META_ADDR);

  if(guide >= man_guide_count) return;               /* ACK still clears */
  if(!man_guides[guide].zoom) return;                /* guide has no scrollable zoom section */
  nn = man_guides[guide].nn;

  if(nn != man_open_nn || !man_open) {
    man_close();
    if(!man_open_guide(file_lfn, nn)) man_open_nn = 0xff;
  }
  if(!man_open && !man_open_guide(file_lfn, nn)) goto zfail;

  if(mode & MAN_ZMODE_PAGE) {
    page = index;                                    /* page turn: index IS the zoom page */
  } else {
    uint16_t zp, zy;                                 /* entering from 1x: resolve the block */
    if(!man_zmap_lookup(guide, index, &zp, &zy)) goto zfail;
    page = zp; entry_y = zy;
  }

  for(tries = 0; tries < MAN_READ_RETRIES; tries++) {
    uint32_t tile_ofs, attr_ofs, pal_ofs;
    uint16_t nrows, pix_h, first_block;
    uint8_t  pdf_page, nblk;
    UINT     got;

    if(!man_open && !man_open_guide(file_lfn, nn)) break;
    if(!man_zdir(guide, page, &tile_ofs, &attr_ofs, &pal_ofs, &nrows, &pix_h,
                 &first_block, &pdf_page, &nblk)) { man_close(); continue; }
    if(first_block + nblk > man_guides[guide].nblocks)
      nblk = (uint8_t)(man_guides[guide].nblocks - first_block);

    /* Skip the ~180KB restage when this page is ALREADY resident. Both 1x bands of a PDF page
       map to the same zoom page, so this turns "toggle 2x from the lower half" into an instant
       scroll instead of a visible reload. */
    if(!(man_zres_guide == guide && man_zres_page == page)) {
      if(f_lseek(&man_fil, pal_ofs)
         || f_read(&man_fil, man_buf, MAN_Z_PAL_BYTES, &got) || got != MAN_Z_PAL_BYTES) {
        man_close(); continue;
      }
      uint16_t row0 = (nrows < MAN_VIS_ROWS) ? (uint16_t)((MAN_VIS_ROWS - nrows) / 2) : 0;
      sram_writeblock(man_buf, SRAM_MANUAL_ZPAL_ADDR, MAN_Z_PAL_BYTES);
      if(!man_stage_zattrs(attr_ofs, nrows, row0)) { man_close(); man_zres_guide = 0xff; continue; }
      if(!man_stage_ztiles(tile_ofs, nrows, row0)) { man_close(); man_zres_guide = 0xff; continue; }
      if(row0) man_blank_zrows(0, row0);
      if((uint16_t)(row0 + nrows) < MAN_VIS_ROWS) man_blank_zrows((uint16_t)(row0 + nrows), MAN_VIS_ROWS);
      man_zres_guide = guide;
      man_zres_page  = page;
    }
    /* publish the PADDED size for short pages (see man_blank_zrows) */
    if(nrows < MAN_VIS_ROWS) nrows = MAN_VIS_ROWS;
    if(pix_h < (uint16_t)(MAN_VIS_ROWS * 8)) pix_h = (uint16_t)(MAN_VIS_ROWS * 8);

    /* publish LAST, so the ready flag is only ever set over a fully staged page. */
    sram_writebyte(pdf_page,                        SRAM_MANUAL_META_ADDR + 3);
    sram_writebyte((uint8_t)nrows,                  SRAM_MANUAL_META_ADDR + MAN_META_ZNROWS_OFS);
    sram_writebyte((uint8_t)(pix_h & 0xff),         SRAM_MANUAL_META_ADDR + MAN_META_ZPIXH_OFS);
    sram_writebyte((uint8_t)(pix_h >> 8),           SRAM_MANUAL_META_ADDR + MAN_META_ZPIXH_OFS + 1);
    sram_writebyte((uint8_t)page,                   SRAM_MANUAL_META_ADDR + MAN_META_ZPAGE_OFS);
    sram_writebyte(guide,                           SRAM_MANUAL_META_ADDR + MAN_META_ZGUIDE_OFS);
    sram_writebyte((uint8_t)(entry_y & 0xff),       SRAM_MANUAL_META_ADDR + MAN_META_ZENTRYY_OFS);
    sram_writebyte((uint8_t)(entry_y >> 8),         SRAM_MANUAL_META_ADDR + MAN_META_ZENTRYY_OFS + 1);
    sram_writebyte((uint8_t)(first_block & 0xff),   SRAM_MANUAL_META_ADDR + MAN_META_ZFIRSTBLK_OFS);
    sram_writebyte((uint8_t)(first_block >> 8),     SRAM_MANUAL_META_ADDR + MAN_META_ZFIRSTBLK_OFS + 1);
    sram_writebyte(nblk,                            SRAM_MANUAL_META_ADDR + MAN_META_ZNBLK_OFS);
    f = sram_readbyte(SRAM_MANUAL_META_ADDR);
    sram_writebyte((uint8_t)(f | MAN_META_FLAG_ZREADY), SRAM_MANUAL_META_ADDR);
    return;
  }

zfail:
  man_zres_guide = 0xff;                             /* nothing trustworthy is resident */
  f = sram_readbyte(SRAM_MANUAL_META_ADDR);
  sram_writebyte((uint8_t)(f | MAN_META_FLAG_ERROR), SRAM_MANUAL_META_ADDR);
}
