/* sd2snes - Atari 2600 experimental core launch

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   atari.c: .a26 launch via the FPGA_A26 core. See atari.h for the model.

   Flow (identical to the SMS/SGB launches in memory.c/load_rom):
     1. a26_id()          -- detect the .a26 extension, scan the image and derive
                             the bankswitch scheme + Superchip flag; an image the
                             v0 core cannot map => error = MENU_ERR_NOIMPL (clean
                             popup in the menu, no reset, never a hang).
     2. a26_update_file() -- swap the booted filename for the SNES-side player
                             (/sd2snes/a26_snes.bin), which is what load_rom then
                             streams as the LoROM image.
     3. a26_load_rom()    -- stage the cartridge image at PSRAM A26_ROM_PSRAM.

   The detector (ATARI-BANKSWITCH.md at the workspace root) is data of our own:
   hotspot ADDRESSES are facts of the cartridge hardware, and the byte patterns
   that reference them are generated here from the 6502 opcode tables below.

   mk2: the same detect/swap/stage code serves the sd2snes_atari core (FPGA_ATARI);
   only the hand-over of the scheme differs (memory.c: a byte in the player image,
   not CHIPFEAT).
*/

#include <string.h>

#include "cfg.h"
#include "config.h"
#include "fileops.h"
#include "ff.h"
#include "memory.h"
#include "snes.h"
#include "atari.h"
#include "uart.h"
#include "scratch.h"

a26_romprops_t a26_romprops;

extern cfg_t CFG;   /* a26_video_width -> feat16[5] (picture width) in a26_id */

/* Both buffers live in the shared scratch (scratch.h, AHB SRAM) -- NOT plain .bss
   on the main SRAM, whose stack/heap headroom is tight. Both are written before
   they are ever read (the path via strncpy on detection, the scan block via
   f_read before parsing).
   The path is part of the game load's FRAME: a26_id writes it and a26_load_rom
   reads it back later in the same load_rom (sms.c keeps its path in the same
   bytes; a file is never both a .sms and a .a26). */
typedef struct { char rompath[256]; } a26_frame_t;
SCRATCH_FITS(a26_frame_t, SCRATCH_FRAME_BYTES);
#define a26_rompath (SCRATCH_FRAME(a26_frame_t)->rompath)

/* One block of the streaming scan plus the 2-byte carry-over that keeps the
   3-byte sliding window intact across a block boundary. The image is at most
   32KB, so the whole scan is one linear pass through this buffer (LEAF scratch,
   held for the scan only). */
#define A26_SCAN_BLOCK 512
#define A26_SCAN_CARRY 2
typedef struct { uint8_t buf[A26_SCAN_BLOCK + A26_SCAN_CARRY]; } a26_scan_scratch_t;
SCRATCH_FITS(a26_scan_scratch_t, SCRATCH_LEAF_BYTES);
#define a26_scanbuf (SCRATCH_LEAF(a26_scan_scratch_t)->buf)

/* 6502 opcodes carrying a 16-bit absolute operand. These two tables ARE the
   signature generator: a reference to an address is <opcode><lo><hi>, so a window
   whose first byte is in a table and whose operand names one of the addresses
   below is a reference to it.

   DIRECT = plain absolute addressing (LDA/STA/STX/STY/BIT/JMP/JSR/LDX/LDY/CMP/
   ORA/AND/EOR/ADC/SBC/CPX/CPY). The read-modify-write absolutes (ASL/ROL/LSR/ROR/
   DEC/INC abs) are deliberately left out: they are rare in real bankswitch code,
   and a false negative is benign here (it only costs the image its default
   scheme, see a26_decide()).

   INDEXED = abs,X / abs,Y, whose base usually sits a few bytes below the address
   the index walks onto. Only the E0 slice counter looks at those, and that
   counter is diagnostic (see a26_hotspot_hit); the Superchip and UA evidence take
   direct references only. */
static const uint8_t a26_op_direct[] = {
  0xAD, 0x8D, 0x8E, 0x8C, 0x2C, 0x4C, 0x20, 0xAE, 0xAC,
  0xCD, 0x0D, 0x2D, 0x4D, 0x6D, 0xED, 0xEC, 0xCC
};
static const uint8_t a26_op_indexed[] = {
  0xBD, 0xB9, 0xBC, 0xBE, 0x9D, 0x99
};

static uint8_t a26_in_table(const uint8_t *tbl, uint8_t n, uint8_t op) {
  uint8_t i;
  for(i = 0; i < n; i++) {
    if(tbl[i] == op) return 1;
  }
  return 0;
}

static uint8_t a26_is_direct(uint8_t op) {
  return a26_in_table(a26_op_direct, sizeof(a26_op_direct), op);
}

static uint8_t a26_is_indexed(uint8_t op) {
  return a26_in_table(a26_op_indexed, sizeof(a26_op_indexed), op);
}

/* Accumulator of the one-pass scan. Counters are 16-bit because a 32KB image can
   in principle produce one match per byte; they are clamped to 8 bits only when
   published in a26_romprops (the decision thresholds are single digits). */
typedef struct {
  uint16_t score_3f;      /* weighted evidence for the 3F/3E family (diagnostic) */
  uint16_t dist_3f;       /* image positions that scored for 3F (diagnostic) */
  uint16_t abs_3f;        /* "8D 3F 00" windows -- the only 3F evidence that decides */
  uint16_t score_ua;      /* weighted evidence for UA */
  uint8_t  ua_ops;        /* bit0 = $0220 seen, bit1 = $0240 seen */
  uint8_t  e0_slices;     /* bit N = 1K slice group N of the E0 window referenced */
  uint16_t sc_hits;       /* references into the Superchip RAM read port */
  uint8_t  sc_seen[16];   /* bitmap of the 128 operands those references name */
  uint8_t  sc_dist;       /* how many DISTINCT operands, i.e. popcount(sc_seen) */
  uint8_t  sc_lead_const; /* every 4K bank still has a constant 256-byte lead-in */
  uint8_t  sc_lead_fill;  /* the fill byte of the bank being checked */
#ifdef CONFIG_MK2
  uint8_t  sc_mir_ok;     /* Stella's rule: the first 128 bytes of every 4K bank repeat in the next 128 */
  uint8_t  sc_mir_banks;  /* 4K banks whose 256-byte lead-in was actually checked for that rule */
  uint8_t  fe_hist[4];    /* the 4 bytes before the current one (FE/F8 windows) */
  uint8_t  fe_seen;       /* how many of fe_hist are valid, saturates at 4 */
  uint8_t  fe_sigs;       /* bit N = FE signature N of a26_fe_sig found */
  uint8_t  f8_sta[2];     /* "STA $1FF9" / "STA $FFF9" windows, saturating */
#endif
} a26_scan_t;

#ifdef CONFIG_MK2
/* FE: the same five signatures and the same rule as Stella's CartDetector
   (isProbablyFE, and "isProbablyFE(image) && !f8" in the 8K branch of
   autodetectType): any one of these 5-byte sequences anywhere in an 8K image, and
   neither "STA $1FF9" nor "STA $FFF9" found twice. */
static const uint8_t a26_fe_sig[5][5] = {
  { 0x20, 0x00, 0xD0, 0xC6, 0xC5 },  /* JSR $D000; DEC $C5  Decathlon */
  { 0x20, 0xC3, 0xF8, 0xA5, 0x82 },  /* JSR $F8C3; LDA $82  Robot Tank */
  { 0xD0, 0xFB, 0x20, 0x73, 0xFE },  /* BNE $FB; JSR $FE73  Space Shuttle (NTSC/PAL) */
  { 0xD0, 0xFB, 0x20, 0x68, 0xFE },  /* BNE $FB; JSR $FE68  Space Shuttle (SECAM) */
  { 0x20, 0x00, 0xF0, 0x84, 0xD6 }   /* JSR $F000; STY $D6  Thwocker */
};

/* Byte-wise, over every image byte exactly once (independent of the 3-byte window
   and its carry in a26_scan_block). */
static void a26_fe_byte(a26_scan_t *st, uint8_t b) {
  uint8_t s;
  if(st->fe_seen >= 2 && st->fe_hist[2] == 0x8D && st->fe_hist[3] == 0xF9) {
    if(b == 0x1F && st->f8_sta[0] < 255) st->f8_sta[0]++;
    if(b == 0xFF && st->f8_sta[1] < 255) st->f8_sta[1]++;
  }
  if(st->fe_seen >= 4) {
    for(s = 0; s < 5; s++) {
      if(b == a26_fe_sig[s][4] && !memcmp(st->fe_hist, a26_fe_sig[s], 4))
        st->fe_sigs |= (uint8_t)(1 << s);
    }
  }
  st->fe_hist[0] = st->fe_hist[1];
  st->fe_hist[1] = st->fe_hist[2];
  st->fe_hist[2] = st->fe_hist[3];
  st->fe_hist[3] = b;
  if(st->fe_seen < 4) st->fe_seen++;
}
#endif /* CONFIG_MK2 */

static void a26_scan_reset(a26_scan_t *st) {
  memset(st, 0, sizeof(a26_scan_t));
  st->sc_lead_const = 1;
#ifdef CONFIG_MK2
  st->sc_mir_ok = 1;
#endif
}

/* Which 1K slice groups of the E0 window ($1FE0-$1FE7, $1FE8-$1FEF, $1FF0-$1FF7)
   the image references. DIAGNOSTIC ONLY: that window is ordinary ROM in an
   F8/F6/F4 image and bank trampolines commonly live at $1FE0/$1FE8 there, so a
   reference into it is not evidence of anything the decision can act on (see
   a26_decide()). F8/F6/F4 need no counter of their own either -- they are the
   defaults for their sizes.

   A reference is attributed to the group its operand names, and an INDEXED base
   that sits BELOW the window (base = hotspot - N, up to 8 bytes short of $1FE0)
   to the first group, which is the only one it can reach. One instruction never
   lights two groups: walking its whole index range into the NEXT group would let
   a single accidental byte triple light two of them at once. */
static void a26_hotspot_hit(a26_scan_t *st, uint8_t lo, uint8_t indexed) {
  if(lo >= 0xE0 && lo <= 0xE7)      st->e0_slices |= 0x01;
  else if(lo >= 0xE8 && lo <= 0xEF) st->e0_slices |= 0x02;
  else if(lo >= 0xF0 && lo <= 0xF7) st->e0_slices |= 0x04;
  else if(indexed && lo >= 0xD8 && lo <= 0xDF) st->e0_slices |= 0x01;
}

/* Scan one block of the image. buf[0..len) is the window to look at, of which
   the first `carry` bytes are the tail of the previous block (already seen by
   the byte-wise checks, but still needed for the sliding window); `base` is the
   image offset of buf[0]; `last` marks the final block, where the trailing
   2-byte patterns have to be matched too because no carry follows.

   Kept free of any file/PSRAM access on purpose: this and a26_decide() are the
   whole detector, and the host gate (tests/host/run_a26_detect.sh) drives them
   through a26_id() over a stdio-backed FatFs. */
static void a26_scan_block(a26_scan_t *st, const uint8_t *buf, uint32_t len,
                           uint32_t carry, uint32_t base, uint8_t last) {
  uint32_t i, end;

  /* Superchip evidence #1: the RAM shadows the first 256 bytes of every 4K bank,
     so that ROM is dead space and mastering leaves it at a constant filler.
     Byte-wise over the bytes THIS block brings in (the carry bytes were counted
     by the previous call). */
  for(i = carry; i < len; i++) {
    uint32_t off = (base + i) & 0xFFF;
    if(off >= 0x100) continue;
    if(off == 0) st->sc_lead_fill = buf[i];
    else if(buf[i] != st->sc_lead_fill) st->sc_lead_const = 0;
  }
#ifdef CONFIG_MK2
  /* Stella's rule needs no buffer of its own: the blocks are 512 bytes and start on
     multiples of 512 (the carry bytes only precede the first new byte), so the 256-byte
     lead-in of a bank is always inside ONE block, at buf[carry]. sc_mir_ok only records
     "no evidence against", so every bank it vouches for is counted: a bank that was never
     read (truncated or skipped scan) must not pass for a mirrored one, see
     a26_superchip_stella(). */
  if(((base + carry) & 0xFFF) == 0) {
    if(len - carry < 256 || memcmp(buf + carry, buf + carry + 128, 128)) st->sc_mir_ok = 0;
    else if(st->sc_mir_banks < 255) st->sc_mir_banks++;
  }
#endif
#ifdef CONFIG_MK2
  for(i = carry; i < len; i++) a26_fe_byte(st, buf[i]);
#endif

  if(len < 2) return;
  if(last) {
    end = len - 1;      /* the last two starts see a truncated window */
  } else {
    if(len < 3) return;
    end = len - 2;      /* those two starts come back as the next carry */
  }

  for(i = 0; i < end; i++) {
    uint8_t b0 = buf[i];
    uint8_t b1 = buf[i + 1];
    uint8_t full = (i + 2 < len);
    uint8_t b2 = full ? buf[i + 2] : 0;

    /* 3F: own literal, NOT a generated hotspot signature -- the bank write goes
       to $003F, so the cartridge-window filter used further down does not apply
       to it. Only the 3-byte absolute store decides (see a26_decide()): the
       2-byte zero-page pair "85 3F" turns up by accident in ordinary data far
       too often (~12% of random 8K images, and a striped playfield motif can
       repeat it a dozen times), so it is counted for the diagnostic line and
       nothing else.
       The 3E variant is caught by the SAME pattern: it selects ROM banks through
       the very same $003F write and only adds $003E on top for its RAM banks.
       Scanning $3E as well would therefore find nothing new while doubling the
       rate at which random data trips the refusal. */
    if(b1 == 0x3F) {
      if(full && b0 == 0x8D && b2 == 0x00) {
        st->score_3f += 2;
        st->dist_3f++;
        st->abs_3f++;
      } else if(b0 == 0x85) {
        st->score_3f += 1;
        st->dist_3f++;
      }
    }

    if(!full) continue;

    /* UA: hotspots $0220/$0240, again outside the cartridge-window filter. The
       two are tracked as a mask, not as a count of positions: a scheme with more
       than one hotspot is only credibly identified when the image references
       MORE THAN ONE of them (repeats of a single operand are much weaker). */
    if(b2 == 0x02 && (b1 == 0x20 || b1 == 0x40) && a26_is_direct(b0)) {
      st->score_ua += 2;
      st->ua_ops |= (b1 == 0x20) ? 0x01 : 0x02;
    }

    /* Superchip evidence #2: plain absolute references into the RAM read port
       $1080-$10FF (or its $F0xx mirror), counted by DISTINCT operand. In a
       cartridge WITHOUT the RAM that window is ordinary ROM, so a single hit
       proves nothing -- a lone accidental byte triple in graphics data reads
       exactly like one (see a26_decide(), which asks for two). Indexed reads are
       left out for the same reason: a table walk through that address range is
       the normal way to read ROM there. */
    if((b2 == 0x10 || b2 == 0xF0) && b1 >= 0x80 && a26_is_direct(b0)) {
      uint8_t idx = (uint8_t)(b1 - 0x80);
      uint8_t bit = (uint8_t)(1 << (idx & 7));
      if(st->sc_hits < 0xFFFF) st->sc_hits++;
      if(!(st->sc_seen[idx >> 3] & bit)) {
        st->sc_seen[idx >> 3] |= bit;
        st->sc_dist++;
      }
    }

    /* Generated hotspot signatures. Primary filter: only the cartridge window
       ($1Fxx) and its high mirror ($FFxx) are accepted as the operand high byte.
       The other mirrors practically never appear in real code, so narrowing the
       filter buys nothing but fewer false positives. */
    if(b2 == 0x1F || b2 == 0xFF) {
      if(a26_is_direct(b0))       a26_hotspot_hit(st, b1, 0);
      else if(a26_is_indexed(b0)) a26_hotspot_hit(st, b1, 1);
    }
  }
}

/* "Strong evidence" predicates. Accidental bytes in data must never outvote the
   default for the image size, so only patterns that are implausible as data
   count -- and each of them needs more than one occurrence. */

/* 3F: two or more "STA $003F" (8D 3F 00) at distinct positions. The 3-byte
   store is what the bank switch actually is; the zero-page pair is data noise
   and stays out of this. */
static uint8_t a26_strong_3f(const a26_scan_t *st) {
  return (uint8_t)(st->abs_3f >= 2);
}

static uint8_t a26_ua_distinct(const a26_scan_t *st) {
  return (uint8_t)((st->ua_ops & 1) + ((st->ua_ops >> 1) & 1));
}

/* UA: both hotspots referenced. score_ua >= 4 is implied by that (each of the
   two operands scores 2) and is kept only so the threshold reads explicitly. */
static uint8_t a26_strong_ua(const a26_scan_t *st) {
  return (st->score_ua >= 4) && (a26_ua_distinct(st) >= 2);
}

/* Superchip: the RAM shadows the first 256 bytes of EVERY 4K bank -- that ROM is
   dead space and mastering leaves it at a constant filler -- AND the code reads
   the RAM back through at least two distinct addresses of the read port. */
static uint8_t a26_superchip(const a26_scan_t *st) {
  return (uint8_t)(st->sc_lead_const && st->sc_dist >= 2);
}

#ifdef CONFIG_MK2
/* Superchip, Stella's rule (CartDetector::isProbablySC): the first 128 bytes of every
   4K bank repeat in the next 128 (the RAM is mirrored there on the cartridge). It also
   catches images whose lead-in is not one constant filler (Crystal Castles, Dig Dug, ...)
   or that never read the RAM through two distinct addresses. Used for 8K (F8SC) and 16K
   (F6SC) only, in addition to a26_superchip().
   The rule holds only when EVERY bank was checked: sc_mir_ok starts at 1 and is only ever
   cleared, so a scan that was skipped (scratch refused) or cut short (read error) would
   otherwise vouch for banks it never saw and put RAM over $1000-$10FF of a plain cart. */
static uint8_t a26_superchip_stella(const a26_scan_t *st, uint32_t romsize) {
  return (uint8_t)(st->sc_mir_ok && st->sc_mir_banks == (romsize >> 12));
}

/* FE: a signature, and no "potential F8" (Stella asks for 2 hits of one store). */
static uint8_t a26_probably_fe(const a26_scan_t *st) {
  return (uint8_t)(st->fe_sigs && st->f8_sta[0] < 2 && st->f8_sta[1] < 2);
}
#endif

static void a26_noimpl(a26_romprops_t *props, const char *param) {
  props->error = MENU_ERR_NOIMPL;
  props->error_param = (const uint8_t*)param;
}

/* Decision table. THE policy: a NOIMPL is only ever raised on evidence that is
   both POSITIVE and STRONG -- the default for the image size wins over weak
   evidence, and an image with no recognizable signature at all boots as that
   default (same rule the fork already applies to a raw .sms dump).

   E0 and E7 have no test here on purpose. Their hotspots ($1FE0-$1FEF) are plain
   ROM in an F8/F6/F4 image, and bank trampolines commonly live at $1FE0/$1FE8 in
   exactly those images, so a reference there is not evidence -- refusing on it
   turned ordinary F8/F6 titles away. Both schemes fall through to the default for
   their size and run wrong, which is the outcome this policy accepts for a scheme
   no signature can separate from a normal image. */
static void a26_decide(a26_romprops_t *props, const a26_scan_t *st,
                       uint32_t romsize) {
  uint8_t sc = a26_superchip(st);

  switch(romsize) {
    case 2048:
      props->scheme = A26_BS_2K;   /* the Superchip needs a 4K window: ignored */
      props->size_class = 0;
      break;

    case 4096:
      if(a26_strong_3f(st)) { a26_noimpl(props, "3F"); return; }
      props->scheme = A26_BS_4K;
      props->size_class = 1;
      props->superchip = sc;
      break;

    case 8192:
      if(a26_strong_3f(st))      { a26_noimpl(props, "3F"); return; }
      if(a26_strong_ua(st))      { a26_noimpl(props, "UA"); return; }
      props->scheme = A26_BS_F8;
      props->size_class = 2;
#ifdef CONFIG_MK2
      sc |= a26_superchip_stella(st, romsize);
#endif
      props->superchip = sc;
#ifdef CONFIG_MK2
      /* after the refusals above and only without Superchip evidence: Stella tests
         SC, 3F and UA before FE as well, and every other 8K image stays F8 */
      if(!sc && a26_probably_fe(st)) props->scheme = A26_BS_FE;
#endif
      break;

    case 16384:
      if(a26_strong_3f(st)) { a26_noimpl(props, "3F"); return; }
      props->scheme = A26_BS_F6;
      props->size_class = 3;
#ifdef CONFIG_MK2
      sc |= a26_superchip_stella(st, romsize);
#endif
      props->superchip = sc;
      break;

    case 32768:
      if(a26_strong_3f(st)) { a26_noimpl(props, "3F"); return; }
#ifdef CONFIG_MK2
      /* the sd2snes_atari core holds 16 KB of cartridge ROM (the Spartan-3 block RAM is
         full): F4 does not fit, so refuse it here instead of booting half a game */
      a26_noimpl(props, "F4"); return;
#endif
      props->scheme = A26_BS_F4;
      props->size_class = 4;
      props->superchip = sc;
      break;

    default:
      /* 12K is FA (CBS RAM+), 64K is a 3F-class image; anything else is not a
         cartridge size this core can map at all. */
      a26_noimpl(props, romsize == 12288 ? "12K"
                      : romsize == 65536 ? "64K" : "?");
      break;
  }
}

static uint8_t a26_clamp8(uint16_t v) {
  return (uint8_t)(v > 255 ? 255 : v);
}

/* case-insensitive ".a26" extension check + one streaming pass over the image */
void a26_id(a26_romprops_t *props, uint8_t *filename) {
  a26_scan_t st;
  uint32_t romsize, pos = 0, carry = 0;
  char *ext;

  memset(props, 0, sizeof(a26_romprops_t));
  props->error = MENU_ERR_OK;

  if(!filename) return;
  ext = strrchr((char*)filename, '.');
  if(!ext || strcasecmp(ext + 1, "a26")) return;

  props->has_a26 = 1;
  strncpy(a26_rompath, (char*)filename, sizeof(a26_rompath) - 1);
  a26_rompath[sizeof(a26_rompath) - 1] = 0;

  /* the file is already open (same contract as sgb_id()/nes_id()) and fileops
     has a single global handle, so the scan borrows it and rewinds when done */
  romsize = file_handle.fsize;
  props->romsize_bytes = romsize;

  a26_scan_reset(&st);
  /* A refused scratch skips the scan: like a truncated one (below) it can only
     LOSE evidence, so the image boots as the default for its size (sc_mir_ok, the
     one flag that starts out "yes", is held to the bank count it was checked on). */
  if((romsize == 2048 || romsize == 4096 || romsize == 8192
      || romsize == 16384 || romsize == 32768)
     && scratch_leaf_take(SCR_A26)) {
    f_lseek(&file_handle, 0);
    while(pos < romsize) {
      UINT got = 0;
      UINT want = (romsize - pos > A26_SCAN_BLOCK) ? A26_SCAN_BLOCK
                                                   : (UINT)(romsize - pos);
      file_res = f_read(&file_handle, a26_scanbuf + carry, want, &got);
      if(file_res || !got) {
        /* A truncated scan can only LOSE evidence, never invent it, and every
           NOIMPL below needs evidence -- so deciding on the part that was read
           stays on the conservative side (at worst the image boots as the
           default for its size). That holds for the counters and for
           sc_lead_const, whose Superchip verdict also needs read-port references
           from the bytes that WERE read. The exception is sc_mir_ok (Mk.II), a
           pure "nothing against" flag: a26_superchip_stella() accepts it only
           when every bank was checked. Reported, not escalated. */
        printf("A26: scan truncated at %ld/%ld (res=%d)\n",
               (long)pos, (long)romsize, file_res);
        break;
      }
      a26_scan_block(&st, a26_scanbuf, carry + got, carry, pos - carry,
                     (uint8_t)(pos + got >= romsize));
      if(carry + got >= A26_SCAN_CARRY) {
        a26_scanbuf[0] = a26_scanbuf[carry + got - 2];
        a26_scanbuf[1] = a26_scanbuf[carry + got - 1];
        carry = A26_SCAN_CARRY;
      }
      pos += got;
    }
    f_lseek(&file_handle, 0);
    scratch_leaf_drop();
  }

  a26_decide(props, &st, romsize);

  props->score_3f          = a26_clamp8(st.score_3f);
  props->addr_distinct_3f  = a26_clamp8(st.dist_3f);
  props->abs_stores_3f     = a26_clamp8(st.abs_3f);
  props->score_ua          = a26_clamp8(st.score_ua);
  props->addr_distinct_ua  = a26_ua_distinct(&st);
  props->e0_slices_hit     = st.e0_slices;
  props->sc_read_refs      = st.sc_dist;

  if(props->error == MENU_ERR_OK) {
    props->feat16 = (uint16_t)((props->scheme & 0x0F)
                  | (props->superchip ? (1 << 4) : 0)
                  | (CFG.a26_video_width ? (1 << 5) : 0)  /* 0 = 160 px 1:1, 1 = 256 px stretched
                       (A26VideoWidth, menu: Chip Options). Sampled here, at .a26 load:
                       cfg_load() has run since boot and the menu writes the byte straight into the
                       shared CFG block, so the value is always the one the user last picked. */
                  | (0 << 6)   /* tv */
                  | ((props->size_class & 0x0F) << 8));
  }

  /* 3F=<score>/<positions>/<absolute stores>, SC=<flag>/<distinct read refs>,
     E0=<slice mask>: the last two of each are what the decision looks at, the
     rest is there to explain a verdict from a log alone. */
#ifdef CONFIG_MK2
  printf("A26: %ld B scheme=%d SC=%d/%d 3F=%d/%d/%d UA=%d/%d E0=%x FE=%x/%d/%d feat16=%04x %s%s\n",
         (long)romsize, props->scheme, props->superchip, props->sc_read_refs,
         props->score_3f, props->addr_distinct_3f, props->abs_stores_3f,
         props->score_ua, props->addr_distinct_ua,
         props->e0_slices_hit, st.fe_sigs, st.f8_sta[0], st.f8_sta[1], props->feat16,
         props->error ? "NOIMPL:" : "ok",
         props->error ? (const char*)props->error_param : "");
#else
  printf("A26: %ld B scheme=%d SC=%d/%d 3F=%d/%d/%d UA=%d/%d E0=%x feat16=%04x %s%s\n",
         (long)romsize, props->scheme, props->superchip, props->sc_read_refs,
         props->score_3f, props->addr_distinct_3f, props->abs_stores_3f,
         props->score_ua, props->addr_distinct_ua,
         props->e0_slices_hit, props->feat16,
         props->error ? "NOIMPL:" : "ok",
         props->error ? (const char*)props->error_param : "");
#endif
}

/* boot the SNES-side player instead of the .a26; the .a26 is staged separately */
uint8_t a26_update_file(uint8_t **filename_ref) {
  if(!a26_romprops.has_a26) return 1;
  FILINFO fno;
  fno.lfname = NULL;            /* _USE_LFN=1: f_stat writes the long name through
                                   fno.lfname; NULL it or it writes through stack garbage
                                   (wild pointer) on every load. Same guard as file_exists(). */
  if(f_stat((const TCHAR*)A26_PLAYER_FILE, &fno) != FR_OK) {
    printf("A26: player %s missing\n", (char*)A26_PLAYER_FILE);
    return 0;
  }
  *filename_ref = (uint8_t*)A26_PLAYER_FILE;
  return 1;
}

/* stage the cartridge image where the core copies it into BRAM at reset */
void a26_load_rom(void) {
  if(!a26_romprops.has_a26) return;
  printf("A26: staging %s -> PSRAM %06lx\n", a26_rompath, (long)A26_ROM_PSRAM);
  load_sram_offload((uint8_t*)a26_rompath, A26_ROM_PSRAM, 0);
}
