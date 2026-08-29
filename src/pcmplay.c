/* Menu PCM player -- see src/pcmplay.h for what this is and why the block looks the
   way it does.  Everything audio-related is done by src/msu1.c; this file only tracks
   which track is up, whether it is paused, and publishes the status block the menu
   screen (snes/pcmplay.a65) renders. */

#include <string.h>
#include "config.h"
#include "memory.h"
#include "msu1.h"
#include "pcmplay.h"

static uint8_t  pcm_state = PCMPLAY_STATE_NONE;
static uint16_t pcm_total_sec = 0;
static uint32_t pcm_body_bytes = 0;   /* track length without the 8-byte header */

/* 44100 stereo 16-bit frames = 176400 bytes per second.  Kept as one constant so the
   two places that convert bytes<->seconds cannot drift apart. */
#define PCM_BYTES_PER_SEC ((uint32_t)44100 * 4)

/* Last block published, so publish can skip a write that would change nothing (see
   pcm_write_blk). */
static pcmplay_blk_t pcm_last;
static int pcm_last_valid;

static void pcm_write_blk(void) {
  pcmplay_blk_t blk;
  uint32_t pos = 0;

  memset(&blk, 0, sizeof(blk));
  blk.magic[0] = PCMPLAY_MAGIC0;
  blk.magic[1] = PCMPLAY_MAGIC1;
  blk.version  = PCMPLAY_VERSION;
  blk.state    = pcm_state;

  if(pcm_state == PCMPLAY_STATE_PLAYING || pcm_state == PCMPLAY_STATE_PAUSED) {
    blk.total_sec = pcm_total_sec;
    pos = menu_music_tell();
    pos = (pos > MSU_PCM_OFFSET_WAVEDATA) ? (pos - MSU_PCM_OFFSET_WAVEDATA) : 0;
    if(pos > pcm_body_bytes) pos = pcm_body_bytes;   /* the read head runs one DAC buffer ahead */
    blk.pos_sec = (uint16_t)(pos / PCM_BYTES_PER_SEC);
    /* 0..255 fraction, pre-divided here because the 65816 has no divide.  num * 255 must
       not wrap 32 bits, so scale BOTH sides down until the product fits: a real track is
       tens of MB, far past the 16.8 MB where the bare product overflows.  Everything here
       is uint32_t on purpose -- `255UL` would be 64-bit in a host build, so the same code
       would quietly behave differently there than on the 32-bit MCU (and the host test
       guarding this would prove nothing). */
    if(pcm_body_bytes) {
      uint32_t num = pos, den = pcm_body_bytes;
      while(den > (uint32_t)0x00ffffff) { den >>= 8; num >>= 8; }
      blk.progress = (uint8_t)((num * (uint32_t)255) / den);
    }
  }

  /* Every write here is FPGA SPI traffic interleaved with the DAC pump, which has to land
     a 1 KB refill every ~5.8 ms.  Publishing unconditionally meant a burst per menu-loop
     pass (~50 Hz) to drive a display that only changes once a SECOND; writing only on a
     real change drops that to ~1 Hz.  Housekeeping, not a fix: the mid-playback stops seen
     during bringup came from menucmd_fmv_gate, not from this traffic. */
  if(pcm_last_valid && !memcmp(&blk, &pcm_last, sizeof(blk))) return;
  pcm_last = blk;
  pcm_last_valid = 1;
  sram_writeblock(&blk, SRAM_PCMPLAY_ADDR, sizeof(blk));
}

void pcmplay_start(const char *filename) {
  int res = menu_music_play(filename);   /* 0xA0 playing / 0x01 open-fail / 0x02 bad magic */

  pcm_total_sec  = 0;
  pcm_body_bytes = 0;

  if(res == 0xA0) {
    uint32_t size = menu_music_size();
    menu_music_lock(1);        /* the FMV stop paths must leave our track alone */
    pcm_body_bytes = (size > MSU_PCM_OFFSET_WAVEDATA) ? (size - MSU_PCM_OFFSET_WAVEDATA) : 0;
    pcm_total_sec  = (uint16_t)(pcm_body_bytes / PCM_BYTES_PER_SEC);
    pcm_state      = PCMPLAY_STATE_PLAYING;
  } else {
    /* menu_music_play already closed the handle and left the DAC alone on both error
       paths, so there is nothing to unwind here -- just report it. */
    pcm_state = (res == 0x02) ? PCMPLAY_STATE_ERR_MAGIC : PCMPLAY_STATE_ERR_OPEN;
  }
  pcm_write_blk();
}

void pcmplay_stop(void) {
  menu_music_lock(0);
  menu_music_stop();
  pcm_state      = PCMPLAY_STATE_NONE;
  pcm_total_sec  = 0;
  pcm_body_bytes = 0;
  pcm_write_blk();
}

void pcmplay_pause(int paused) {
  if(pcm_state != PCMPLAY_STATE_PLAYING && pcm_state != PCMPLAY_STATE_PAUSED) return;
  menu_music_pause(paused);
  pcm_state = paused ? PCMPLAY_STATE_PAUSED : PCMPLAY_STATE_PLAYING;
  pcm_write_blk();
}

void pcmplay_publish(void) {
  if(pcm_state == PCMPLAY_STATE_NONE) return;
  /* The clip dying under us -- an unreadable file makes the pump give up (menu_sfx_stop
     inside menu_sfx_pump) -- must not leave the screen showing a track that is no longer
     playing.  Reported as the READ error it is, rather than as plain "nothing playing":
     the latter would repaint an empty bar and 0:00 with no explanation at all. */
  if(pcm_state == PCMPLAY_STATE_PLAYING && !menu_music_active()) {
    menu_music_lock(0);
    menu_music_stop();                    /* already stopped; this only drops the handle */
    pcm_state = PCMPLAY_STATE_ERR_OPEN;
  }
  pcm_write_blk();
}

void pcmplay_clear(void) {
  menu_music_lock(0);
  pcm_last_valid = 0;          /* PSRAM holds whatever the last power-on left: always write */
  pcm_state      = PCMPLAY_STATE_NONE;
  pcm_total_sec  = 0;
  pcm_body_bytes = 0;
  pcm_write_blk();
}
