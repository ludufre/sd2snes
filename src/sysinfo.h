#ifndef _SYSINFO_H
#define _SYSINFO_H

#include <stddef.h>
#include <stdint.h>

/* --- System Information block, v2: the MCU publishes VALUES, the menu formats the screen.
 *
 * The firmware used to write 13 ready-made 40-char text lines to SRAM_SYSINFO_ADDR, which
 * meant one localized template set per language had to live in MCU flash. It now publishes
 * this packed struct and the menu builds every line from its own string pool, so a new
 * language costs the firmware nothing.
 *
 * THE BYTE OFFSETS ARE THE INTERFACE. They are mirrored as SI2_* in snes/memmap.i65 and must
 * never be re-derived on the SNES side; the _Static_asserts below pin the three string
 * offsets and the total size so a field inserted in the middle breaks the build here rather
 * than silently shifting the menu's reads.
 *
 * The menu re-checks magic[] before reading any field, which is what makes "new menu + old
 * firmware" safe: an older firmware leaves plain text at this address, and no legacy line 0,
 * in any of the 5 languages, ever started with 'S', so the check cannot false-positive on
 * leftover text.
 *
 * Anything that would cost the 65816 arithmetic is pre-split here: the SD revision nibbles,
 * the access time integer and fractional parts, and the manufacturing year already offset by
 * 2000. Strings are plain NUL-terminated ASCII (the font maps ASCII 1:1 for these).
 */
#define SYSINFO_MAGIC0               ('S')
#define SYSINFO_MAGIC1               ('I')
#define SYSINFO_VERSION              (1)

#define SYSINFO_FLAG_SD_GONE         (0x01)  /* card removed or the USB server owns the SD: the SD/card/access-time fields are all zero */
#define SYSINFO_FLAG_FS_BUSY         (0x02)  /* partial block: NONE of the card fields is valid yet (maker, oem, product, revision, serial, manufacturing date, tacc_*, card_used_mb, card_total_mb are all still zero while the free-space count runs). The menu must blank the card lines for as long as this is set -- printing the zeros would be plausible-looking nonsense, not an obvious placeholder */
#define SYSINFO_FLAG_ACC_MEASURING   (0x04)  /* access time not sampled yet; the tacc_* fields are zero */
#define SYSINFO_FLAG_CLK_MEASURING   (0x08)  /* SNES master clock not sampled yet; snes_clk_hz is zero */
#define SYSINFO_FLAG_U16             (0x10)  /* an Ultra16 was detected; u16_serial is valid */
#define SYSINFO_FLAG_U16_AUTOBOOT    (0x20)  /* Ultra16 autoboot is on (only meaningful with SYSINFO_FLAG_U16) */

/* Canonical SGB BIOS state, ordered by severity so the menu can index its word table
   directly. Deliberately NOT the SGB_BIOS_* order of sgb.h (CHECK=0 OK=1 MISMATCH=2
   MISSING=3) -- sysinfo.c maps between the two. */
#define SYSINFO_SGB_CHECKING         (0)
#define SYSINFO_SGB_MISSING          (1)
#define SYSINFO_SGB_MISMATCH         (2)
#define SYSINFO_SGB_OK               (3)

typedef struct __attribute__((__packed__)) _sysinfo_blk {
  uint8_t  magic[2];        /* +0   ($00) 'S','I' */
  uint8_t  version;         /* +2   ($02) SYSINFO_VERSION */
  uint8_t  flags;           /* +3   ($03) SYSINFO_FLAG_* */
  uint8_t  sgb_ver;         /* +4   ($04) CFG.sgb_bios_version, the N in sgbN_boot.bin */
  uint8_t  sgb_state;       /* +5   ($05) SYSINFO_SGB_* */
  uint8_t  cic_state;       /* +6   ($06) raw enum cicstates; cic_str carries the text */
  uint8_t  sd_maker;        /* +7   ($07) CID manufacturer ID, shown as hex */
  char     sd_oem[2];       /* +8   ($08) CID OEM/application ID, not NUL-terminated */
  char     sd_product[5];   /* +10  ($0A) CID product name, not NUL-terminated */
  uint8_t  sd_rev_maj;      /* +15  ($0F) CID revision, high nibble already split out */
  uint8_t  sd_rev_min;      /* +16  ($10) CID revision, low nibble */
  uint8_t  sd_serial[4];    /* +17  ($11) CID serial number, shown as hex */
  uint8_t  pad0;            /* +21  ($15) */
  uint16_t sd_mfd_year;     /* +22  ($16) manufacturing year, 2000 already added */
  uint8_t  sd_mfd_month;    /* +24  ($18) manufacturing month, 1..12 */
  uint8_t  pad1;            /* +25  ($19) */
  uint16_t tacc_avg_int;    /* +26  ($1A) average access time, whole milliseconds */
  uint16_t tacc_avg_frac;   /* +28  ($1C) average access time, thousandths (0..999) */
  uint16_t tacc_max_int;    /* +30  ($1E) worst access time, whole milliseconds */
  uint16_t tacc_max_frac;   /* +32  ($20) worst access time, thousandths (0..999) */
  uint32_t card_used_mb;    /* +34  ($22) */
  uint32_t card_total_mb;   /* +38  ($26) */
  uint32_t snes_clk_hz;     /* +42  ($2A) measured SNES master clock */
  uint16_t u16_serial;      /* +46  ($2E) Ultra16 serial number */
  char     fw_str[24];      /* +48  ($30) CONFIG_VERSION, NUL-terminated */
  char     cic_str[28];     /* +72  ($48) CIC state text, NUL-terminated. 28 because the
                                    longest English name, "SuperCIC detected, not used",
                                    is 27 chars (cic.c owns that table) */
  char     model_str[28];   /* +100 ($64) DEVICE_NAME, NUL-terminated */
} sysinfo_blk_t;            /* 128 bytes */

_Static_assert(offsetof(sysinfo_blk_t, fw_str) == 48, "sysinfo_blk_t.fw_str must stay at SI2_FW_STR (+48)");
_Static_assert(offsetof(sysinfo_blk_t, cic_str) == 72, "sysinfo_blk_t.cic_str must stay at SI2_CIC_STR (+72)");
_Static_assert(offsetof(sysinfo_blk_t, model_str) == 100, "sysinfo_blk_t.model_str must stay at SI2_MODEL_STR (+100)");
_Static_assert(sizeof(sysinfo_blk_t) == 128, "sysinfo_blk_t must stay 128 bytes (SI2_SIZE in snes/memmap.i65)");
/* the block replaces the 13x40 text layout in place, so it can never outgrow it */
_Static_assert(sizeof(sysinfo_blk_t) <= 13 * 40, "sysinfo_blk_t must fit the old 13-line text region");

void sysinfo_loop(void);

#endif
