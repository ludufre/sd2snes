#ifndef _PCMPLAY_H
#define _PCMPLAY_H

#include <stddef.h>
#include <stdint.h>

/* --- Menu PCM player: play a .pcm (MSU-1 audio track) straight from the browser ---
 *
 * Selecting a .pcm with A opens a player screen (snes/pcmplay.a65) with a progress bar
 * and play/pause.  This module is a thin shell around machinery that already exists:
 * menu_music_play/stop + menu_sfx_pump in src/msu1.c drive the cartridge DAC and were
 * built for the game-info FMV soundtrack.  Nothing about the audio path is new here.
 *
 * WHO OWNS THE DAC.  There is exactly ONE DAC, shared by three consumers: the FMV clip
 * audio (gameinfo.c), the navigation blips (menu_sfx_play) and this player.  The blips
 * already stand down on their own (menu_main_loop checks menu_music_active), but the two
 * FMV stop paths do not: the 300 ms idle watchdog (gameinfo_fmv_idle_check) and the
 * per-command gate (menucmd_fmv_gate) would each kill this track -- the watchdog because
 * the player issues no CMD_FMV_NEXT, the gate because it fires on every command the info
 * screen does not own, INCLUDING the player's own pause.  So the player takes an explicit
 * claim, menu_music_lock(), which both of them honour.
 *
 * WHY A BINARY BLOCK, PUBLISHED WITHOUT A COMMAND.  The screen redraws every frame
 * (60 Hz) but the MCU_CMD handshake runs off a ~20 ms menu-loop pass, so a per-frame
 * status command would both miss frames and steal the pump's time budget.  Instead the
 * MCU is the sole writer of the block below and republishes it once per menu-loop pass;
 * the SNES only ever READS it.  Same shape as the SAVEINFO block.
 *
 * THE FIELDS ARE PRE-CHEWED ON PURPOSE.  Seconds and a 0..255 progress fraction, not raw
 * sample counts: the 65816 has no divide, and this is the same split SAVEINFO uses (the
 * MCU pre-formats, the menu just prints).  The bar is progress * width / 256, a shift.
 *
 * THE BYTE OFFSETS ARE THE INTERFACE.  Mirrored as PCM_* in snes/memmap.i65; the
 * _Static_asserts below pin them so a field inserted in the middle breaks the build here
 * instead of silently shifting the menu's reads.
 */
#define PCMPLAY_MAGIC0               ('P')
#define PCMPLAY_MAGIC1               ('C')
#define PCMPLAY_VERSION              (1)

#define PCMPLAY_STATE_NONE           (0)  /* nothing playing */
#define PCMPLAY_STATE_PLAYING        (1)
#define PCMPLAY_STATE_PAUSED         (2)  /* DAC read pointer frozen; the file stays open */
#define PCMPLAY_STATE_ERR_OPEN       (3)  /* the file would not open -- or stopped reading
                                             mid-track (see pcmplay_publish), which is why
                                             the menu's message for it says "read" */
#define PCMPLAY_STATE_ERR_MAGIC      (4)  /* opened, but it is not an "MSU1" PCM */

/* Control codes carried in MCU_PARAM's low byte by SNES_CMD_PCM_CTL */
#define PCMPLAY_CTL_STOP             (0)
#define PCMPLAY_CTL_PAUSE            (1)
#define PCMPLAY_CTL_RESUME           (2)

typedef struct __attribute__((__packed__)) _pcmplay_blk {
  uint8_t  magic[2];   /* +0 ($00) 'P','C' -- the menu zeroes these before CMD_PLAY_PCM as
                              a capability sentinel: an older dispatcher ACKs an unknown
                              command, so the ACK alone proves nothing */
  uint8_t  version;    /* +2 ($02) PCMPLAY_VERSION */
  uint8_t  state;      /* +3 ($03) PCMPLAY_STATE_* */
  uint16_t pos_sec;    /* +4 ($04) elapsed seconds */
  uint16_t total_sec;  /* +6 ($06) track length in seconds */
  uint8_t  progress;   /* +8 ($08) 0..255 position fraction, pre-divided for the bar */
  uint8_t  flags;      /* +9 ($09) reserved for flags; published as 0.  The slot is kept so
                              the block can grow a flag without shifting pad[] or bumping
                              the version -- the menu already tolerates unknown bits. */
  uint8_t  pad[22];    /* +10 ($0a) reserved to the 32-byte block */
} pcmplay_blk_t;       /* 32 bytes, the whole reservation at SRAM_PCMPLAY_ADDR */

_Static_assert(offsetof(pcmplay_blk_t, state)     ==  3, "pcmplay_blk_t.state must stay at PCM_STATE (+3)");
_Static_assert(offsetof(pcmplay_blk_t, pos_sec)   ==  4, "pcmplay_blk_t.pos_sec must stay at PCM_POS (+4)");
_Static_assert(offsetof(pcmplay_blk_t, total_sec) ==  6, "pcmplay_blk_t.total_sec must stay at PCM_TOTAL (+6)");
_Static_assert(offsetof(pcmplay_blk_t, progress)  ==  8, "pcmplay_blk_t.progress must stay at PCM_PROGRESS (+8)");
_Static_assert(offsetof(pcmplay_blk_t, flags)     ==  9, "pcmplay_blk_t.flags must stay at PCM_FLAGS (+9)");
_Static_assert(sizeof(pcmplay_blk_t)              == 32, "pcmplay_blk_t must stay 32 bytes (PCM_SIZE in snes/memmap.i65)");

/* Open and start a .pcm.  Fail-safe: a missing/!"MSU1" file publishes an error state and
   plays nothing -- it never hangs and never leaves the DAC half-armed. */
void pcmplay_start(const char *filename);

/* Stop + close + publish PCMPLAY_STATE_NONE.  Safe to call when nothing is playing. */
void pcmplay_stop(void);

/* Freeze/unfreeze the DAC read pointer.  NOT menu_sfx_stop(): that also disarms the FPGA
   fetcher and clears the active flag, which would lose the open file and the position. */
void pcmplay_pause(int paused);

/* Recompute pos/progress and write the block.  Called once per menu_main_loop pass;
   bounded, touches no SD (the position comes from the already-open file handle). */
void pcmplay_publish(void);

/* Park PCMPLAY_STATE_NONE in the block at cold boot, next to memtest_clear(): PSRAM keeps
   whatever the last power-on left there, and garbage would look like a live track. */
void pcmplay_clear(void);

#endif
