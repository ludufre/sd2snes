/* Host shim for src/patchmeta.h: the patch metadata sidecar lives on the SD
 * card, which the host harness does not model.  patch.c only calls
 * patchmeta_apply(), and the harness stages the flags byte directly, so the
 * stub in shim.c is a no-op that leaves the staged values alone.
 * The real patch_entry_t comes from the real src/patch.h (-I ../../src). */
#ifndef HOST_PATCHMETA_H
#define HOST_PATCHMETA_H

#include <stdint.h>
#include "patch.h"

/* Asset root, like the gameinfo/cheat shims: patch_unlink_rom_assets names the
 * per-ROM patch sidecar with it. */
#define PATCH_BASEDIR ("/sd2snes/patches/")

int patchmeta_apply(const uint8_t *rom_path, uint32_t sram_addr, uint8_t count);

#endif
