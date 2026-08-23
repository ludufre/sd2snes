/* Host shim for src/cfg.h: a stand-in cfg_t with only the fields the sources
 * under test read.  The layout is NOT the shipping one.  The cfg.c gate needs
 * the real struct, so run_cfg.sh puts shim_cfg/ ahead of this directory. */
#ifndef HOST_CFG_H
#define HOST_CFG_H

#include <stdint.h>

typedef struct {
  uint8_t patch_verify_integrity;  /* src/patch.c */
  uint8_t a26_video_width;         /* src/atari.c: feat16[5] of CHIPFEAT */
} cfg_t;

#endif
