/* host stand-in for src/cheat.h: the constants trainer.c reasons about, verbatim, plus
   stubs for the two entry points it calls (the harness counts the calls). */
#ifndef HOST_CHEAT_H
#define HOST_CHEAT_H
#define CHEAT_FLAG_ENABLE          (0x80)
#define CHEAT_NUM_CODES_PER_CHEAT  (40)
#define CHEAT_WRAM_MAX             (20)
#define CHEAT_RECORD_MAX           (512)
void cheat_program(void);
void cheat_stage_names_window(int base);
#endif
