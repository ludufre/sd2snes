/* Host-only helpers shared by shim.c and harness.c. */
#ifndef HOST_HOST_H
#define HOST_HOST_H

#include <stdint.h>

extern uint8_t *host_sdram;
void host_sdram_init(void);

/* BPS copier-mode test model: patch_copier_emit() (declared in patch_copier.h)
   collects descriptors here; host_copier_replay() applies them to host_sdram
   exactly like the FPGA copier so the copier path can be checked byte-for-byte
   against the legacy apply.  patch_copier_count() returns the descriptor count. */
void host_copier_replay(void);

#endif
