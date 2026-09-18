/* host stand-in for src/cheat.h: the constants cheatedit.c reasons about, verbatim, plus
   the entry points it calls (the harness implements the PSRAM ones over the fake array
   and counts the deploy calls). */
#ifndef HOST_CHEAT_H
#define HOST_CHEAT_H
#include <stdint.h>
#include "cheatcode.h"
#define CHEAT_FLAG_ENABLE          (0x80)
#define CHEAT_FLAG_RUNTIME         (0x40)
#define CHEAT_NUM_CODES_PER_CHEAT  (40)
#define CHEAT_WRAM_MAX             (20)
#define CHEAT_RECORD_MAX           (512)
#define CHEAT_REC_ORIG_OFS         (416)
#define CHEAT_REC_APPLIED_OFS      (456)
#define CHEAT_NAME_INGAME_MAX      (64)
#define CHEAT_NAME_INGAME_LEN      (64)
void cheat_program(void);
void cheat_stage_names_window(int base);
uint8_t cheat_rom_psram_mode(void);
void cheat_rom_psram_apply(void);
void cheat_sync_flags_from_mirror(void);
void cheat_write_code_string(int cheat_idx, int code_idx, const char *s);
int  cheat_read_code_string(int cheat_idx, int code_idx, char *out);
#endif
