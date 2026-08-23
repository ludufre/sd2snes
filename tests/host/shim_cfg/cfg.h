/* Not a shim: a forwarder to the REAL src/cfg.h.  shim/cfg.h carries a
 * stand-in cfg_t and is next on the include path, so this directory must stay
 * ahead of shim/ in -I order or the stand-in wins. */
#ifndef HOST_CFG_REAL_H
#define HOST_CFG_REAL_H

#include "../../../src/cfg.h"

#endif
