/* host stand-in for src/cheat.h: the constants and the patch word trainer.c reasons about,
   verbatim, plus the entry points it calls (the harness records the calls). */
#ifndef HOST_CHEAT_H
#define HOST_CHEAT_H
#include <stdint.h>
#define CHEAT_FLAG_ENABLE          (0x80)
#define CHEAT_NUM_CODES_PER_CHEAT  (40)
#define CHEAT_WRAM_MAX             (20)
#define CHEAT_RECORD_MAX           (512)
typedef union _cheat_patch_record {
  struct __attribute__ ((__packed__)) _patch_fields {
    uint8_t  patchvalue;
    uint16_t patchaddr;
    uint8_t  patchbank;
  } fields;
  uint32_t code;
} cheat_patch_record_t;
void cheat_program(void);
void cheat_program_single(cheat_patch_record_t *cheat);
#endif
