/* sd2snes - SD card based universal cartridge for the SNES
   patch_copier.h: BPS-via-FPGA-copier acceleration (Approach B+: the MCU batches
   SourceCopy/TargetCopy as a PSRAM descriptor list and the copier runs the whole
   batch autonomously -- one SPI trigger + poll instead of one per op).

   The firmware build (patch_copier.c) writes descriptors to PSRAM and fires
   fpga_copier_list; the host test build (tests/host/shim.c) accumulates them and
   replays a forward copy over a fake SDRAM, proving the BPS->copier decomposition
   is byte-identical to the legacy byte-by-byte apply.

   See patch_copier.c for the full data flow.
*/

#ifndef PATCH_COPIER_H
#define PATCH_COPIER_H

#include <stdint.h>

/* Start a fresh apply.  list_base = PSRAM address where the descriptor list is
   built (firmware); the host model ignores it and accumulates in an array. */
void patch_copier_reset(uint32_t list_base);

/* Immediate single copier op -- used for the source backup, which must run BEFORE
   run_actions so it reads the pristine source.  Returns 0 ok, non-zero on failure. */
int patch_copier_op_now(uint32_t src, uint32_t dst, uint32_t len);

/* Accumulate one SourceCopy/TargetCopy as a descriptor (deferred to patch_copier_
   finish).  Returns 0 ok, non-zero on overflow/failure -> bps_apply aborts. */
int patch_copier_emit(uint32_t src, uint32_t dst, uint32_t len);

/* Flush the accumulated list and run it on the copier (one trigger + poll).
   Returns 0 ok, non-zero on failure. */
int patch_copier_finish(void);

/* Number of descriptors accumulated so far (stats / host test). */
uint32_t patch_copier_count(void);

/* Firmware only: does the currently-loaded core carry the MCU copier? (probe). */
int patch_copier_available(void);

#endif /* PATCH_COPIER_H */
