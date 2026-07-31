/* sd2snes - SD card based universal cartridge for the SNES
   patchmeta.c: per-ROM patch metadata sidecar (see patchmeta.h)
*/

#include "config.h"
#include "uart.h"
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "fpga_spi.h"
#include "patch.h"
#include "patchmeta.h"
#include "yaml.h"

#include <string.h>

/* Header mode <-> yml spelling.  Written quoted so the value can never be
   mistaken for a bool or a number by yaml_detect_value; read unquoted-tolerant
   because a human may well edit this file by hand. */
static const char *pm_hdr_name(uint8_t mode) {
  switch(mode) {
    case PATCH_HDR_HEADERED:   return "headered";
    case PATCH_HDR_HEADERLESS: return "headerless";
    default:                   return "auto";
  }
}

static uint8_t pm_hdr_mode(const char *s) {
  if(!strcasecmp(s, "headered"))   return PATCH_HDR_HEADERED;
  if(!strcasecmp(s, "headerless")) return PATCH_HDR_HEADERLESS;
  return PATCH_HDR_AUTO;
}

/* Sidecar path scratch.  IN_AHBRAM rather than a stack array: every entry point
   here is reached with a caller-owned 256-byte path buffer already live AND a
   ~272-byte yaml_token_t of its own, and the LPC175x leaves only a couple of KB
   of stack.  Safe in .ahbram (NOLOAD, not zeroed) because it is always written
   by path_asset before being read, and it is never a DMA target. */
static char pm_path[256] IN_AHBRAM;

/* Build the sidecar path and open it for reading.
   Returns 1 when a sidecar was opened. */
static int patchmeta_open(const uint8_t *rom_path) {
  if(path_asset(pm_path, sizeof(pm_path), PATCH_BASEDIR, (const char *)rom_path, ".yml") < 0)
    return 0;
  yaml_file_open(pm_path, FA_READ);
  if(file_res) {
    file_res = 0;  /* soft fail: having no sidecar is the normal case */
    return 0;
  }
  return 1;
}

/* Length of the ROM stem (leaf without its extension), for patch_display_name. */
static unsigned pm_stem_len(const uint8_t *rom_path) {
  const char *path = (const char *)rom_path;
  const char *leaf = path, *dot = NULL, *p;
  for(p = path; *p; p++) if(*p == '/') leaf = p + 1;
  for(p = leaf; *p; p++) if(*p == '.') dot = p;
  return dot ? (unsigned)(dot - leaf) : (unsigned)strlen(leaf);
}

/* Index of the scanned entry whose basename matches, or -1 (an orphan). */
static int pm_find(const patch_entry_t *ents, uint8_t count, const char *basename) {
  for(uint8_t i = 0; i < count; i++)
    if(!strcasecmp(ents[i].basename, basename)) return (int)i;
  return -1;
}

int patchmeta_apply(const uint8_t *rom_path, patch_entry_t *ents, uint8_t count,
                    uint32_t sram_addr) {
  yaml_token_t tok;

  if(!patchmeta_open(rom_path)) return 0;

  while(yaml_next_item()) {
    int idx;
    if(!yaml_get_itemvalue("Patch", &tok)) continue;
    yaml_decode_entities(tok.stringvalue);
    idx = pm_find(ents, count, tok.stringvalue);
    if(idx < 0) continue;  /* patch deleted from the card: ignore, prune on save */

    if(yaml_get_itemvalue("Header", &tok)) {
      uint8_t mode = pm_hdr_mode(tok.stringvalue);
      ents[idx].flags = (ents[idx].flags & ~PATCH_FLAG_HDR_MASK)
                      | (uint8_t)(mode << PATCH_FLAG_HDR_SHIFT);
      sram_writebyte(ents[idx].flags, sram_addr + IPS_FLAGS_BASE + idx);
    }

    /* A Name: takes precedence over the derived suffix.  It is written straight
       into the staged display slot rather than buffered per entry -- the whole
       point of running this pass after patch_publish.

       The cut to IPS_NAME_BADGE-1 (41) characters is the DISPLAY WIDTH, not an
       arbitrary buffer size: the row prints 42 columns and the badge starts right
       after.  Be aware that it is also the only copy that survives -- patchmeta_save
       rebuilds the yml from these staged slots, so a hand-authored Name: longer than
       this comes back shortened the next time the file is rewritten.  Round-tripping
       it verbatim would mean parsing the old yml while writing the new one, and
       ystate is a single global; nesting that inside the write path is exactly the
       stack pairing that has hung the cheat menu before (see the note on sibling
       frames in patch.c).  A label that cannot be displayed is not worth that risk. */
    if(yaml_get_itemvalue("Name", &tok) && tok.stringvalue[0]) {
      yaml_decode_entities(tok.stringvalue);
      tok.stringvalue[IPS_NAME_BADGE - 1] = 0;
      sram_writeblock(tok.stringvalue,
                      sram_addr + IPS_NAME_BASE + (uint32_t)idx * IPS_NAME_LEN,
                      (uint16_t)(strlen(tok.stringvalue) + 1));
    }
  }

  yaml_file_close();
  file_res = 0;
  return 1;
}

uint8_t patchmeta_flags_for(const uint8_t *rom_path, const char *patch_basename,
                            uint8_t patch_type) {
  yaml_token_t tok;
  uint8_t flags = patch_type & PATCH_FLAG_TYPE_MASK;

  if(!patchmeta_open(rom_path)) return flags;

  while(yaml_next_item()) {
    if(!yaml_get_itemvalue("Patch", &tok)) continue;
    yaml_decode_entities(tok.stringvalue);
    if(strcasecmp(tok.stringvalue, patch_basename)) continue;
    if(yaml_get_itemvalue("Header", &tok))
      flags |= (uint8_t)(pm_hdr_mode(tok.stringvalue) << PATCH_FLAG_HDR_SHIFT);
    break;
  }

  yaml_file_close();
  file_res = 0;
  return flags;
}

void patchmeta_save(const uint8_t *rom_path, const patch_entry_t *ents,
                    uint8_t count) {
  char cur[IPS_NAME_BADGE];   /* display name as currently staged */
  char def[IPS_NAME_BADGE];   /* what the derivation alone would produce */
  unsigned stem_len = pm_stem_len(rom_path);
  int worth_saving = 0;

  if(path_asset(pm_path, sizeof(pm_path), PATCH_BASEDIR, (const char *)rom_path, ".yml") < 0)
    return;

  /* Only remember what cannot be re-derived.  Without this every ROM the user
     merely opens the patch dialog for would leave a redundant yml behind. */
  for(uint8_t i = 0; i < count && !worth_saving; i++) {
    if(PATCH_HDR_MODE(ents[i].flags) != PATCH_HDR_AUTO) worth_saving = 1;
  }
  if(!worth_saving) {
    for(uint8_t i = 0; i < count; i++) {
      sram_readstrn(cur, SRAM_IPS_LIST_ADDR + IPS_NAME_BASE + (uint32_t)i * IPS_NAME_LEN,
                    sizeof(cur));
      patch_display_name(def, sizeof(def), ents[i].basename, stem_len);
      if(strcmp(cur, def)) { worth_saving = 1; break; }
    }
  }

  /* Any prior FPGA SPI transaction that left its chip-select asserted would
     corrupt the SD card's SPI traffic; release it before touching FatFs. */
  FPGA_DESELECT();

  if(!worth_saving) {
    f_chmod((TCHAR *)pm_path, 0, AM_RDO | AM_HID | AM_SYS);
    f_unlink((TCHAR *)pm_path);
    file_res = 0;
    return;
  }

  path_asset_mkdir(pm_path);   /* WRITE path only, and only once the name is built */
  f_chmod((TCHAR *)pm_path, 0, AM_RDO | AM_HID | AM_SYS);
  f_unlink((TCHAR *)pm_path);

  file_open((uint8_t *)pm_path, FA_WRITE | FA_CREATE_ALWAYS);
  if(file_res) {
    /* Same fallback as cheat_yaml_save: some volumes reject truncate-on-open. */
    file_open((uint8_t *)pm_path, FA_WRITE | FA_OPEN_ALWAYS);
    if(!file_res) {
      f_lseek(&file_handle, 0);
      f_truncate(&file_handle);
    }
  }
  if(file_res) {
    printf("patchmeta_save: cannot write %s (%d)\n", pm_path, file_res);
    file_res = 0;
    return;
  }

  f_puts("---\n# Generated by sd2snes\n", &file_handle);
  for(uint8_t i = 0; i < count; i++) {
    uint8_t mode = PATCH_HDR_MODE(ents[i].flags);

    sram_readstrn(cur, SRAM_IPS_LIST_ADDR + IPS_NAME_BASE + (uint32_t)i * IPS_NAME_LEN,
                  sizeof(cur));
    patch_display_name(def, sizeof(def), ents[i].basename, stem_len);
    if(mode == PATCH_HDR_AUTO && !strcmp(cur, def)) continue;  /* nothing to remember */

    /* EVERY item carries ALL THREE keys, even when a value is the default.  This is
       not cosmetic: yaml_get_itemvalue() on a key that is absent from the current
       item runs the search off the end of it, consumes the NEXT item's ITEM_START
       and leaves ystate.parent_offset pointing at that next item -- so the reader
       silently skips it and drops its overrides, and the next save then prunes them
       from the file for good.  cheat_yaml_save avoids this the same way. */
    f_puts("- Patch: \"", &file_handle);
    yaml_puts_escaped(&file_handle, ents[i].basename);
    f_puts("\"\n", &file_handle);
    f_puts("  Name: \"", &file_handle);
    yaml_puts_escaped(&file_handle, cur);
    f_puts("\"\n", &file_handle);
    f_printf(&file_handle, "  Header: \"%s\"\n", pm_hdr_name(mode));
  }
  file_close();
  file_res = 0;
}
