/* Host-build shim for src/config.h, used ONLY by the cfg.c gate (run_cfg.sh
 * puts this directory FIRST on the include path, ahead of shim/).
 *
 * Unlike shim/config.h it does NOT "#define __attribute__(x)": cfg_t is
 * __packed__ and cfg.c pins it to the menu's CFG offset map (snes/memmap.i65)
 * with a wall of _Static_assert(offsetof(...)), so neutralizing the attribute
 * would add host padding and fire all of them.  The rest is what the real
 * config.h gives cfg.c and yaml.c: system headers and the placement macro. */
#ifndef HOST_CFG_CONFIG_H
#define HOST_CFG_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp / strncasecmp */

/* firmware-only placement macro (real config.h): a no-op on the host */
#define IN_AHBRAM

#endif
