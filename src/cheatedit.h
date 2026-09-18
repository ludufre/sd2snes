/* sd2snes fork -- cheat EDITOR: MCU side.

   Add / edit / delete cheats from the menu cheat list (snes/cheatedit.a65) and from
   the in-game CHEATS tab (snes/cheatedit.i65).  Both UIs speak ONE protocol: they
   fill the CHEAT_EDIT block at SRAM_CHEAT_EDIT_ADDR ($FF0800 -- writable by the SNES
   in both modes and the editor's own working buffer, so no UI needs a big local
   copy), write MCU_CMD = SNES_CMD_CHEAT_EDIT and wait for the ACK.  The MCU is the
   only side that can touch the records: $D00000 is read-only to the menu and IS the
   game's ROM in-game.

   Operations (CHEAT_EDIT_OP_*):
     FETCH   idx -> block: name (63 chars), the code strings, numcodes.  In-game the
             SNES cannot read $D0/$D4, so this is how the editor opens a record.
     ADD     block -> a NEW record at index 0 (= the top of the .yml, which
             cheat_yaml_write emits in index order).  Every existing record, its
             code strings and its flag-mirror byte are shifted up by one.  The new
             cheat starts ENABLED.  The in-game trainer's SAVE CHEAT is this same op
             (trainer_serve_request stages the block).
     REPLACE block -> record idx.  The description is rewritten only when
             CHEAT_EDIT_FLAG_NAME is set (a name longer than the 63-char field the
             editor shows survives an edit of the codes alone).  The enable flag is
             untouched.
     DELETE  record idx removed, everything above it shifted down.

   After ADD/REPLACE/DELETE the caller persists the list with
   cheat_yaml_save_current() -- called as a SIBLING, never nested, so the stack depth
   is max(edit, save) and not their sum.  The result byte is written before the ACK;
   the SNES pre-zeroes it, so 0 after the ACK == an old firmware that ACKed an
   unknown command.

   Lockstep: CHEAT_EDIT_OFS_* / CHEAT_EDIT_OP_* / CHEAT_EDIT_RES_* with the CE_* defines
   in snes/memmap.i65. */

#ifndef CHEATEDIT_H
#define CHEATEDIT_H

#include <stdint.h>

/* block layout (offsets from SRAM_CHEAT_EDIT_ADDR) */
#define CHEAT_EDIT_OFS_OP        (0)
#define CHEAT_EDIT_OFS_RESULT    (1)
#define CHEAT_EDIT_OFS_IDX       (2)   /* u16 LE */
#define CHEAT_EDIT_OFS_FLAGS     (4)
#define CHEAT_EDIT_OFS_NUMCODES  (5)
#define CHEAT_EDIT_OFS_NAME      (8)   /* [CHEAT_EDIT_NAME_LEN] NUL-terminated */
#define CHEAT_EDIT_OFS_CODES     (72)  /* [CHEAT_EDIT_MAX_CODES][CHEAT_EDIT_CODE_LEN] */
#define CHEAT_EDIT_NAME_LEN      (64)  /* 63 visible + NUL == CHEAT_NAME_INGAME_LEN */
#define CHEAT_EDIT_CODE_LEN      (10)  /* 9 visible + NUL */
#define CHEAT_EDIT_MAX_CODES     (40)  /* == CHEAT_NUM_CODES_PER_CHEAT */

/* op */
#define CHEAT_EDIT_OP_NONE       (0)
#define CHEAT_EDIT_OP_FETCH      (1)
#define CHEAT_EDIT_OP_ADD        (2)
#define CHEAT_EDIT_OP_REPLACE    (3)
#define CHEAT_EDIT_OP_DELETE     (4)

/* flags */
#define CHEAT_EDIT_FLAG_NAME     (0x01) /* REPLACE: the name field is authoritative */

/* result */
#define CHEAT_EDIT_RES_PENDING   (0)    /* the SNES pre-writes this; still 0 after the ACK = unsupported firmware */
#define CHEAT_EDIT_RES_OK        (1)
#define CHEAT_EDIT_RES_BADCODE   (2)    /* a code string failed cheat_code_valid; nothing changed */
#define CHEAT_EDIT_RES_FULL      (3)    /* CHEAT_RECORD_MAX records already; nothing changed */
#define CHEAT_EDIT_RES_BADREQ    (4)    /* unknown op / index out of range / bad count; nothing changed */
#define CHEAT_EDIT_RES_SAVEFAIL  (6)    /* records changed in PSRAM but the .yml could not be written */

/* Serve the request in the block. in_game != 0 selects the in-game rules: the live
   $FF0500 toggles are folded into the records first, and the cheats are re-deployed
   (cheat_program + cheat_rom_psram_apply) before returning.  Returns 1 when the record
   set changed (the caller must persist it), 0 otherwise.  Always writes the result byte and
   clears the op.  Bounded; no SD access. */
int cheat_edit_serve(int in_game);

#endif
