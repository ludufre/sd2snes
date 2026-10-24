/* Header of overlay 2 (patchui.c, patchmeta.c, patchexp.c, savesop.c, bootleg.c), linked at
   ptrcache like overlay 1: one of them is in the block at a time. The entry order is the
   veneers' in ovl_stub.c. ovlpack.py fills in the build id, the size and the CRC. */
#include <stdint.h>
#include "../ovl.h"
void OVL_query_ips_patches(void); void OVL_stage_patch_from_entry(void);
void OVL_patchmeta_save(void); void OVL_patch_export_exists(void);
void OVL_patch_export_write(void); void OVL_patch_export_copy_assets(void);
void OVL_patch_unlink_rom_assets(void); void OVL_saves_command(void);
void OVL_bootleg_identify_file(void); void OVL_bootleg_apply(void);
__attribute__((section(".ovlhdr"), used)) const struct { ovl_hdr_t h; uint32_t e[10]; } ovl_hdr2 = {
  { OVL_MAGIC(2), 0, 0, 0 },
  { (uint32_t)OVL_query_ips_patches, (uint32_t)OVL_stage_patch_from_entry,
    (uint32_t)OVL_patchmeta_save, (uint32_t)OVL_patch_export_exists,
    (uint32_t)OVL_patch_export_write, (uint32_t)OVL_patch_export_copy_assets,
    (uint32_t)OVL_patch_unlink_rom_assets, (uint32_t)OVL_saves_command,
    (uint32_t)OVL_bootleg_identify_file, (uint32_t)OVL_bootleg_apply } };
