/* sd2snes fork -- in-game RAM trainer: MCU side.

   The trainer itself (search, filter, result browser, value editor) runs entirely on
   the SNES, inside the in-game TAB menu shell (snes/trainer.i65, bank $C8), because
   only the 65816 can read the console's own WRAM ($7E/$7F is not intercepted by the
   cartridge). The MCU owns exactly two things:

     1. the session gate -- trainer_stage() zeroes the block's magic on every game
        load, so a search can never leak into a different ROM (PSRAM banks $FA-$FC are
        never cleared automatically and survive a short power-cycle, exactly like the
        in-game menu's own IGM_POS_MAGIC), and trainer_invalidate() drops it when a
        savestate load or a reset makes the captured snapshot misleading.

     2. FREEZE. There is deliberately NO second patch engine: a frozen result becomes a
        NORMAL cheat record at $D00000, so cheat_program() emits it as one of the 20
        WRAM patches ($2AD8, `LDA #vv : STA $bbaaaa`) the NMI hook already executes.
        That is what makes a freeze obey the master switch (branch_wram = cheat_enable &
        wram_present, cheat.v), survive CMD_CHEAT_REPROGRAM, show up in the CHEATS tab,
        and disappear when another ROM is loaded -- all without new machinery.

   The request payload does NOT travel in MCU_PARAM (12 bytes, and this needs 9):
   the tab writes it into the meta block (which it owns, and which is writable in-game
   through the IS_PATCH identity window) and CMD_TRAINER_CHEAT carries nothing.

   Addresses live in src/memmap.h (SRAM_TRAINER_*), in lockstep with TRAINER_* and TR_* in
   snes/memmap.i65. */

#ifndef TRAINER_H
#define TRAINER_H

#include <stdint.h>

#define TRAINER_VERSION      (1)
#define TRAINER_FREEZE_MAX   (4)     /* lockstep with TR_FREEZE_MAX in snes/memmap.i65 */

/* Why the session was dropped, so the tab can say so once (read outside the magic
   guard -- by then the magic is already gone). */
#define TRAINER_NOTICE_NONE       (0)
#define TRAINER_NOTICE_LOADSTATE  (1)
#define TRAINER_NOTICE_RESET      (2)

/* TR_REQ values. */
#define TRAINER_REQ_NONE     (0)
#define TRAINER_REQ_FREEZE   (1)
#define TRAINER_REQ_UNFREEZE (2)
#define TRAINER_REQ_ADD      (3)   /* same record as FREEZE, but its enable bit starts CLEAR */

/* 64 bytes at SRAM_TRAINER_META_ADDR. Byte-for-byte lockstep with the TR_* offsets in
   snes/memmap.i65; the _Static_asserts in trainer.c prove it. */
typedef struct __attribute__ ((__packed__)) _trainer_blk {
  char     magic[4];                    /* +0  "TRNR"; 0 = no session */
  uint8_t  version;                     /* +4  TRAINER_VERSION */
  uint8_t  active;                      /* +5  1 = a search is in progress */
  uint8_t  mode;                        /* +6  TR_MODE_* */
  uint8_t  width;                       /* +7  1 = 8-bit, 2 = 16-bit LE */
  uint32_t count;                       /* +8  candidates; starts at 131072, so NOT 16-bit */
  uint16_t value;                       /* +12 last value entered */
  uint8_t  have_snap;                   /* +14 1 = the snapshot banks hold the last scan */
  uint8_t  notice;                      /* +15 TRAINER_NOTICE_* */
  uint32_t cursor;                      /* +16 selected result index */
  uint32_t top;                         /* +20 first result index shown */
  uint32_t sel_off;                     /* +24 WRAM offset of the selection */
  uint8_t  ui;                          /* +28 TR_UI_* */
  uint8_t  ui_row;                      /* +29 row inside the current screen */
  uint8_t  edit_digit;                  /* +30 digit cursor in the value editor */
  uint8_t  rsvd;                        /* +31 */
  uint16_t fz_idx[TRAINER_FREEZE_MAX];  /* +32 cheat-record index per slot, 0xFFFF = empty */
  uint32_t fz_off[TRAINER_FREEZE_MAX];  /* +40 WRAM offset held in that slot */
  uint8_t  req;                         /* +56 TRAINER_REQ_* */
  uint8_t  req_slot;                    /* +57 target freeze slot */
  uint32_t req_off;                     /* +58 WRAM offset to freeze */
  uint16_t req_val;                     /* +62 value to hold */
} trainer_blk_t;

/* Drop any session and clear the freeze bookkeeping. Called at every game load, next
   to igmenu_stage(); bounded, no SD. */
void trainer_stage(void);

/* Drop the session but leave a reason behind for the tab to display once.  Called on a
   savestate LOAD (the .state rewinds WRAM, so "increased/decreased" against the captured
   snapshot would actively lie) and on an MCU-mediated reset. */
void trainer_invalidate(uint8_t reason);

/* Serve SNES_CMD_TRAINER_CHEAT: turn the pending TR_REQ into a runtime cheat record and
   redeploy. Bounded (a handful of PSRAM block reads/writes + cheat_program), no SD. */
void trainer_serve_request(void);

#endif
