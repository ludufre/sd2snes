/* The saves screen's actions (copy, delete, the slot a game starts with): menu-only, card
   overlay code (see ovl.h).  Moved out of memory.c to go to the card; the block the screen
   draws from (saves_stage) stays resident, the in-game SAVES tab uses it too. */
#include <string.h>
#include "config.h"
#include "ff.h"
#include "fileops.h"
#include "memory.h"
#include "cfg.h"
#include "gameopt.h"
#include "patch.h"

extern cfg_t CFG;

/* The saves screen's actions on slot `slot` of `kind` (SAVES_OP_*): make it the slot the
   game starts with, copy it over slot `dst`, or delete it.  Returns SAVES_RES_*. */
static uint8_t saves_do(const char *src, uint8_t op, uint8_t kind, uint8_t slot, uint8_t dst) {
  char path[256];
  uint8_t srm, ss;
  if(kind > SAVES_KIND_STATE || slot >= SAVES_SLOTS) return SAVES_RES_FAIL;
  if(op == SAVES_OP_USE) {
    if(!(kind == SAVES_KIND_SRM ? CFG.enable_sram_slots : CFG.enable_savestate_slots))
      return SAVES_RES_FAIL;
    gameopt_slots(src, &srm, &ss);
    if(kind == SAVES_KIND_SRM) srm = slot; else ss = slot;
    return gameopt_slots_set(src, srm, ss) ? SAVES_RES_OK : SAVES_RES_FAIL;
  }
  if(saves_path(path, sizeof(path), src, kind, slot) < 0) return SAVES_RES_FAIL;
  if(op == SAVES_OP_DELETE)
    return f_unlink((TCHAR *)path) == FR_OK ? SAVES_RES_OK : SAVES_RES_FAIL;
  if(op != SAVES_OP_COPY || dst >= SAVES_SLOTS || dst == slot) return SAVES_RES_FAIL;
  file_open((uint8_t *)path, FA_READ);
  if(file_res) { file_res = 0; return SAVES_RES_FAIL; }
  /* the source is open: its name is dead, build the destination over it */
  if(saves_path(path, sizeof(path), src, kind, dst) < 0) {
    file_close();
    file_res = 0;
    return SAVES_RES_FAIL;
  }
  path_asset_mkdir(path);
  return patch_copy_to(path) ? SAVES_RES_OK : SAVES_RES_FAIL;
}

void saves_command(const char *src, uint8_t op, uint8_t kind, uint8_t slot, uint8_t dst) {
  uint8_t res = op == SAVES_OP_INFO ? SAVES_RES_OK : saves_do(src, op, kind, slot, dst);
  saves_stage(src, 1, res);
}
