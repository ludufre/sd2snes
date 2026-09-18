/* sd2snes fork -- in-game RAM trainer: MCU side.

   The trainer itself (search, filter, result browser, value editor) runs entirely on
   the SNES, inside the in-game TAB menu shell (snes/trainer.i65, bank $C8), because
   only the 65816 can read the console's own WRAM ($7E/$7F is not intercepted by the
   cartridge). The MCU owns exactly three things:

     1. the session gate -- trainer_stage() zeroes the block's magic (and empties the
        pin table) on every load, so a search can never leak into a different ROM
        (PSRAM banks $FA-$FC are never cleared automatically and survive a short
        power-cycle), and trainer_invalidate() drops the search on an MCU-mediated
        reset. A savestate LOAD does NOT drop it: the savestate handler re-baselines
        the snapshot from the state image instead (ss_trainer_rebase, snes/).

     2. FREEZE. There is deliberately NO second patch engine, but a freeze is NOT a
        cheat record either (it never shows up in the CHEATS tab or the .yml): the
        SNES keeps a small PIN table (trainer_pin_t, SRAM_TRAINER_PINS_ADDR) of the
        addresses the user froze or set, and cheat_program() calls
        trainer_program_freezes() first thing, which emits every FROZEN pin as one
        or two of the 20 WRAM patches ($2AD8, `LDA #vv : STA $bbaaaa`) the NMI hook
        already executes. So a freeze obeys the master switch (branch_wram =
        cheat_enable & wram_present, cheat.v), survives CMD_CHEAT_REPROGRAM, and dies
        with the next load.

     3. SAVE CHEAT -- the only way a trainer address reaches the cheat list: the MCU
        stages the address/value into the CHEAT_EDIT block and the caller runs the
        cheat editor's own ADD (cheat_edit_serve), then rewrites the game's .yml.

   The request payload does NOT travel in MCU_PARAM (12 bytes, and this needs 9):
   the tab writes it into the meta block (which it owns, and which is writable in-game
   through the IS_PATCH identity window) and CMD_TRAINER_CHEAT carries nothing.

   Addresses live in src/memmap.h (SRAM_TRAINER_*), in lockstep with TRAINER_* and TR_* in
   snes/memmap.i65. */

#ifndef TRAINER_H
#define TRAINER_H

#include <stdint.h>

/* Bumped whenever the meaning of the block or the pin table changes: the SNES side
   re-inits a session whose version it does not know, and the MCU refuses requests and
   programs no freeze from one, so a mismatched firmware/igmenu.bin pair is inert
   instead of freezing an address the other side never meant. */
#define TRAINER_VERSION      (2)
#define TRAINER_FREEZE_MAX   (4)     /* frozen pins at once (WRAM patch budget); lockstep with TR_FREEZE_MAX */
#define TRAINER_PIN_MAX      (8)     /* pin table entries; lockstep with TR_PIN_MAX in snes/memmap.i65 */

/* Why the session was dropped, so the tab can say so once (read outside the magic
   guard -- by then the magic is already gone). */
#define TRAINER_NOTICE_NONE       (0)
#define TRAINER_NOTICE_RESET      (2)     /* 1 was "savestate load"; a load no longer drops the search */

/* TR_REQ values. */
#define TRAINER_REQ_NONE     (0)
#define TRAINER_REQ_APPLY    (1)   /* the pin table changed (freeze/unfreeze): redeploy */
#define TRAINER_REQ_SAVE     (3)   /* add req_off/req_val (blk.width) to the cheat list + .yml */

/* trainer_pin_t.flags */
#define TRAINER_PIN_FROZEN   (0x01)
#define TRAINER_PIN_WIDE     (0x02)  /* 16-bit value at off, off+1 */
#define TRAINER_PIN_EMPTY    (0xFFFFFFFFUL)

/* One pin: an address the user froze or set a value on. The SNES owns the table (it
   keeps it in insertion order and evicts the oldest UNFROZEN pin when it is full); the
   MCU only reads it, except for SAVE CHEAT clearing the FROZEN bit it takes over.
   Lockstep with the TR_PIN_* offsets in snes/memmap.i65. */
typedef struct __attribute__ ((__packed__)) _trainer_pin {
  uint32_t off;                         /* +0 WRAM offset 0..$1FFFF, TRAINER_PIN_EMPTY = unused */
  uint16_t val;                         /* +4 value set/held */
  uint8_t  flags;                       /* +6 TRAINER_PIN_* */
  uint8_t  rsvd;                        /* +7 */
} trainer_pin_t;

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
  uint8_t  rsvd2[24];                   /* +32 retired v1 freeze slot table (pins replaced it) */
  uint8_t  req;                         /* +56 TRAINER_REQ_* */
  uint8_t  req_rsvd;                    /* +57 */
  uint32_t req_off;                     /* +58 WRAM offset to save */
  uint16_t req_val;                     /* +62 value to save */
} trainer_blk_t;

/* Drop any session and empty the pin table. Called on EVERY load, menu included (a
   stale frozen pin would otherwise be emitted into the menu's own NMI hook); bounded,
   no SD. */
void trainer_stage(void);

/* Drop the search but leave a reason behind for the tab to display once. Called on an
   MCU-mediated reset (the game re-initialises WRAM, so the captured snapshot means
   nothing). The pins survive: they are the user's, not the search's. */
void trainer_invalidate(uint8_t reason);

/* Called by cheat_program() before the .yml cheats: emit every FROZEN pin as WRAM
   patches (cheat_program_single). Needs version == TRAINER_VERSION, never more than
   TRAINER_FREEZE_MAX pins. */
void trainer_program_freezes(void);

/* Serve SNES_CMD_TRAINER_CHEAT. APPLY redeploys and returns 0. SAVE stages the request
   into the CHEAT_EDIT block as an ADD and returns 1: the caller then runs
   cheat_edit_serve(1) and, when that succeeded, trainer_save_done() and the .yml write
   -- as SIBLING calls, so the stack peaks at the deepest one, not their sum. */
int trainer_serve_request(void);

/* After a successful SAVE: the new cheat now holds the value, so drop the FROZEN bit
   of the pin at that offset (the pin stays listed) and redeploy. */
void trainer_save_done(void);

#endif
