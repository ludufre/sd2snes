/* Host-build shim for src/config.h.
 *
 * Pre-includes every system header the firmware sources touch, then
 * neutralizes __attribute__ so firmware-only section/alignment attributes
 * (.ahbram etc.) don't trip the host compiler. The system headers must come
 * FIRST so the macro never rewrites their own attribute uses. */
#ifndef HOST_CONFIG_H
#define HOST_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __attribute__(x)

#endif
