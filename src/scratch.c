/* sd2snes - shared scratch regions. See scratch.h for the ownership rules. */

#include "config.h"
#include "uart.h"     /* printf(): see the note in uart.h -- every printf caller includes this */
#include "scratch.h"

uint32_t scratch_frame[SCRATCH_FRAME_BYTES / 4] IN_AHBRAM;
uint32_t scratch_leaf[SCRATCH_LEAF_BYTES / 4] IN_AHBRAM;

static uint8_t scratch_leaf_owner;   /* 0 = free; .bss, so free at boot */

int scratch_leaf_take(uint8_t who) {
  if(scratch_leaf_owner) {
    printf("scratch: leaf refused to %u, held by %u\n", who, scratch_leaf_owner);
    return 0;
  }
  scratch_leaf_owner = who;
  return 1;
}

void scratch_leaf_drop(void) {
  scratch_leaf_owner = 0;
}
