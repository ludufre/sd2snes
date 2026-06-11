/* Host shim for src/cfg.h: only the field patch.c reads. */
#ifndef HOST_CFG_H
#define HOST_CFG_H

#include <stdint.h>

typedef struct {
  uint8_t patch_verify_integrity;
} cfg_t;

#endif
