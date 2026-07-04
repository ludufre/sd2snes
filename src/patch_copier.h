/* sd2snes - SD card based universal cartridge for the SNES
   patch_copier.h: BPS-via-FPGA-copier acceleration (Approach B: the MCU fires the
   copier one op at a time via the FPGA's SAFE 1-trigger queue -- small ops stream
   without a per-op busy-poll).

   The firmware build (patch_copier.c) fires copier ops; the host test build
   (tests/host/shim.c) replays a forward copy over a fake SDRAM, proving the
   BPS->copier decomposition is byte-identical to the legacy byte-by-byte apply.

   (A descriptor list-mode was tried and REMOVED -- it hangs on real hardware.
   See the patch_copier.c header.)
*/

#ifndef PATCH_COPIER_H
#define PATCH_COPIER_H

#include <stdint.h>

/* Start a fresh apply. */
void patch_copier_reset(void);

/* Immediate single copier op -- used for the source backup, which must run BEFORE
   run_actions so it reads the pristine source.  Returns 0 ok, non-zero on failure. */
int patch_copier_op_now(uint32_t src, uint32_t dst, uint32_t len);

/* Replay one SourceCopy/TargetCopy as a copier op.  Returns 0 ok, non-zero on
   failure -> bps_apply aborts. */
int patch_copier_emit(uint32_t src, uint32_t dst, uint32_t len);

/* Wait for the last queued op to drain and check the copier overrun bit.
   Returns 0 ok, non-zero on failure. */
int patch_copier_finish(void);

/* Number of descriptors accumulated so far (stats / host test). */
uint32_t patch_copier_count(void);

/* Firmware only: does the currently-loaded core carry the MCU copier? (probe). */
int patch_copier_available(void);

#endif /* PATCH_COPIER_H */
