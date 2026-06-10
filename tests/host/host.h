/* Host-only helpers shared by shim.c and harness.c. */
#ifndef HOST_HOST_H
#define HOST_HOST_H

#include <stdint.h>

extern uint8_t *host_sdram;
void host_sdram_init(void);

#endif
