/* Conformance test for the published position maths of the menu PCM player, the card
 * module src/modules/pcmplay/pcmmod.c, compiled the way host builds of the firmware link it
 * (-DMODULE_HOST_LINKED).  The audio engine is a fake module_api_t (the music_* calls below);
 * what is under test is the ONE piece of arithmetic that the screen cannot show you is
 * wrong -- the 0..255 progress fraction, whose naive form (pos * 255 / body) silently wraps
 * a 32-bit product once the track body passes ~16.8 MB, i.e. at about 95 seconds of
 * 44.1 kHz stereo.  Every real MSU-1 track is longer than that.
 *
 * The block is checked as the MENU reads it: through the byte offsets, out of the fake
 * BSRAM the api's sram_write fills, not through the struct the writer used. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"
#include "memmap.h"
#include "pcmplay.h"
#include "msu1.h"

/* ---- fake BSRAM ------------------------------------------------------------------- */
static uint8_t blk[64];
static uint8_t repeat;   /* CFG.player_repeat, read live by the module */

static int writes;   /* every one of these is FPGA SPI traffic competing with the DAC pump */

static uint16_t fake_sram_write(void *buf, uint32_t addr, uint16_t size) {
  if(addr != SRAM_PCMPLAY_ADDR || size > sizeof(blk)) { printf("FAIL bad publish addr/size\n"); exit(1); }
  memcpy(blk, buf, size);
  writes++;
  return size;
}
static uint16_t fake_sram_read(void *buf, uint32_t addr, uint16_t size) {
  if(addr != SRAM_CFG_PLAYER_REPEAT_ADDR || size != 1) { printf("FAIL unexpected read\n"); exit(1); }
  *(uint8_t *)buf = repeat;
  return size;
}

static uint8_t  b8(int off)  { return blk[off]; }
static uint16_t b16(int off) { return (uint16_t)(blk[off] | (blk[off + 1] << 8)); }

/* ---- fake clip ------------------------------------------------------------------- */
static uint32_t clip_size, clip_pos, clip_loop;   /* clip_loop: header field, nothing reads it back */
static int      clip_res = 0xA0;
static int      clip_playing;

/* the engine's own names (msu1.h): what the firmware puts in the api table */
int      menu_music_play(const char *f) { (void)f; if(clip_res == 0xA0) clip_playing = 1; return clip_res; }
void     menu_music_stop(void)          { clip_playing = 0; }
int      menu_music_active(void)        { return clip_playing; }
void     menu_music_pause(int p)        { (void)p; }
static int clip_locked;                  /* the DAC claim the FMV stop paths honour */
void     menu_music_lock(int l)         { clip_locked = l; }
int      menu_music_locked(void)        { return clip_locked; }
uint32_t menu_music_tell(void)          { return clip_pos; }
uint32_t menu_music_size(void)          { return clip_size; }
static int      clip_noloop, clip_ended, clip_seek_ok = 1;
static uint32_t clip_seek_to = 0xffffffffu;      /* last body offset menu_music_seek got */
void     menu_music_noloop(int on)      { clip_noloop = on; }
int      menu_music_ended(void)         { return clip_ended; }
int      menu_music_seek(uint32_t pos)  {
  clip_seek_to = pos;
  if(!clip_seek_ok) { clip_playing = 0; return 0; }
  clip_ended = 0; clip_playing = 1; clip_pos = 8 + pos + MSU_DAC_BUFSIZE;
  return 1;
}

/* ---- the module, driven as module.c drives it ------------------------------------- */
extern const module_fns_t pcmmod_fns;
static module_api_t api;
static uint64_t mod_state[64];          /* the scratch FRAME it gets */

static void pcmplay_start(const char *path) { pcmmod_fns.start(mod_state, &api, path); }
static void pcmplay_publish(void)           { pcmmod_fns.idle(mod_state); }   /* a menu-loop pass */
static void pcmplay_stop(void)              { pcmmod_fns.ctl(mod_state, PCMPLAY_CTL_STOP); }
static void pcmplay_pause(int on)
{ pcmmod_fns.ctl(mod_state, on ? PCMPLAY_CTL_PAUSE : PCMPLAY_CTL_RESUME); }
static void pcmplay_seek_sec(uint16_t s)
{ pcmmod_fns.ctl(mod_state, PCMPLAY_CTL_SEEK | ((uint32_t)s << 8)); }
static void pcmplay_seek_frac(uint8_t f)
{ pcmmod_fns.ctl(mod_state, PCMPLAY_CTL_SEEKFRAC | ((uint32_t)f << 8)); }

/* ---- harness ---------------------------------------------------------------------- */
static int fails;

static void check(const char *what, long got, long want) {
  if(got == want) return;
  printf("FAIL %s: got %ld, want %ld\n", what, got, want);
  fails++;
}

/* The model the firmware has to match, in wide arithmetic so it cannot wrap. */
static uint8_t model_progress(uint32_t pos, uint32_t body) {
  if(!body) return 0;
  if(pos > body) pos = body;
  return (uint8_t)(((uint64_t)pos * 255u) / body);
}

#define BPS (44100u * 4u)

static void play(uint32_t body_bytes, uint32_t loop) {
  clip_size = body_bytes + 8;
  clip_loop = loop;
  clip_pos  = 8;                 /* read head at the start of the body */
  clip_res  = 0xA0;
  pcmplay_start("/x.pcm");
}

static void at(uint32_t body_off) {
  clip_pos = 8 + body_off;
  pcmplay_publish();
}

int main(void) {
  uint32_t body, off;

  api.sram_read    = fake_sram_read;
  api.sram_write   = fake_sram_write;
  api.music_play   = menu_music_play;
  api.music_stop   = menu_music_stop;
  api.music_pause  = menu_music_pause;
  api.music_lock   = menu_music_lock;
  api.music_noloop = menu_music_noloop;
  api.music_seek   = menu_music_seek;
  api.music_active = menu_music_active;
  api.music_ended  = menu_music_ended;
  api.music_tell   = menu_music_tell;
  api.music_size   = menu_music_size;

  /* --- a short clip: 10 s --------------------------------------------------------- */
  body = 10u * BPS;
  play(body, 0);
  check("short: magic0",   b8(0), PCMPLAY_MAGIC0);
  check("short: magic1",   b8(1), PCMPLAY_MAGIC1);
  check("short: version",  b8(2), PCMPLAY_VERSION);
  check("short: state",    b8(3), PCMPLAY_STATE_PLAYING);
  check("short: total",    b16(6), 10);
  check("short: flags reserved", b8(9), 0);
  at(0);
  check("short: pos@0",      b16(4), 0);
  check("short: progress@0", b8(8), 0);
  at(body / 2);
  check("short: pos@half",      b16(4), 5);
  check("short: progress@half", b8(8), model_progress(body / 2, body));
  at(body);
  check("short: pos@end",      b16(4), 10);
  check("short: progress@end", b8(8), 255);

  /* --- a REAL track length: 4 minutes.  Body is 42 MB, well past the point where
         pos * 255 wraps 32 bits -- this is the case the naive form gets wrong. ------ */
  body = 240u * BPS;
  play(body, 1234);
  check("long: total",    b16(6), 240);
  for(off = 0; off <= body; off += body / 16) {
    char what[64];
    snprintf(what, sizeof(what), "long: progress@%lu", (unsigned long)off);
    at(off);
    check(what, b8(8), model_progress(off, body));
    snprintf(what, sizeof(what), "long: pos@%lu", (unsigned long)off);
    check(what, b16(4), off / BPS);
  }
  /* progress must never go backwards as the head advances */
  {
    int prev = -1;
    for(off = 0; off <= body; off += BPS) {
      at(off);
      if((int)b8(8) < prev) { printf("FAIL long: progress went backwards at %lu\n", (unsigned long)off); fails++; break; }
      prev = b8(8);
    }
  }

  /* --- the read head runs up to one DAC buffer AHEAD of what is audible, so it can
         legitimately sit past EOF: that must clamp, not wrap. ----------------------- */
  body = 30u * BPS;
  play(body, 0);
  at(body + MSU_DAC_BUFSIZE);
  check("clamp: pos",      b16(4), 30);
  check("clamp: progress", b8(8), 255);

  /* --- degenerate: a header-only file (no body) must not divide by zero ------------ */
  play(0, 0);
  at(0);
  check("empty: total",    b16(6), 0);
  check("empty: progress", b8(8), 0);

  /* --- pause keeps the position; stop clears the block ---------------------------- */
  body = 60u * BPS;
  play(body, 0);
  at(body / 4);
  pcmplay_pause(1);
  check("pause: state",    b8(3), PCMPLAY_STATE_PAUSED);
  check("pause: pos kept", b16(4), 15);
  check("pause: DAC still claimed", menu_music_locked(), 1);
  pcmplay_pause(0);
  check("resume: state",   b8(3), PCMPLAY_STATE_PLAYING);
  pcmplay_stop();
  check("stop: state",     b8(3), PCMPLAY_STATE_NONE);
  check("stop: DAC released",  menu_music_locked(), 0);
  check("stop: magic kept", b8(0), PCMPLAY_MAGIC0);   /* the menu still needs to read it */

  /* --- errors are reported, not hidden, and never look "active" ------------------- */
  clip_res = 0x01;
  pcmplay_start("/missing.pcm");
  check("openfail: state",  b8(3), PCMPLAY_STATE_ERR_OPEN);
  check("openfail: no claim", menu_music_locked(), 0);
  clip_res = 0x02;
  pcmplay_start("/notmsu1.pcm");
  check("badmagic: state",  b8(3), PCMPLAY_STATE_ERR_MAGIC);
  check("badmagic: no claim", menu_music_locked(), 0);

  /* --- a clip that dies inside the pump (unreadable file) must not leave the screen
         showing a track that stopped playing.  It is reported as the READ error it is:
         plain "nothing playing" would repaint an empty bar and 0:00 with no reason
         given, which reads as a bug rather than as a bad file. --------------------- */
  body = 60u * BPS;
  play(body, 0);
  clip_playing = 0;                /* menu_sfx_pump gave up on it */
  pcmplay_publish();
  check("died: state",  b8(3), PCMPLAY_STATE_ERR_OPEN);
  check("died: DAC released", menu_music_locked(), 0);
  pcmplay_stop();                  /* and closing the screen still clears it */
  check("died: cleared", b8(3), PCMPLAY_STATE_NONE);

  /* --- a publish that changes NOTHING must not write.  This is what keeps the block
         from generating ~50 SPI bursts a second for a display that ticks once a second;
         enough of that traffic starves the DAC refill and stops the track (hardware). -- */
  body = 300u * BPS;
  play(body, 0);
  at(10u * BPS);
  writes = 0;
  for(off = 0; off < 50; off++) pcmplay_publish();   /* one menu-second's worth of passes */
  check("idle: no redundant writes", writes, 0);
  at(11u * BPS);                                     /* the clock ticked -> exactly one write */
  check("tick: one write", writes, 1);
  pcmplay_stop();

  /* --- starting a track always writes the block, even one identical to the last: the
         menu zeroed the magic before the command and reads it back as its sentinel -------- */
  body = 20u * BPS;
  play(body, 0);
  blk[0] = blk[1] = 0;                               /* what the menu does */
  play(body, 0);                                     /* same length, same block */
  check("restart: magic rewritten", b8(0), PCMPLAY_MAGIC0);

  /* --- the player plays a track ONCE (the menu moves on to the next one) --------- */
  body = 100u * BPS;
  play(body, 0);
  check("start: noloop", clip_noloop, 1);

  /* --- seek by seconds: exact byte target, clamped to the track BEFORE the multiply
         (65535 s * 176400 wraps 32 bits; the engine itself keeps the last DAC buffer) -- */
  pcmplay_seek_sec(40);
  check("seek: target", (long)clip_seek_to, 40L * BPS);
  check("seek: published at once", b16(4), 40);
  check("seek: still playing", b8(3), PCMPLAY_STATE_PLAYING);
  pcmplay_seek_sec(0xffff);
  check("seek: clamped target", (long)clip_seek_to, 100L * BPS);
  pcmplay_seek_sec(0);
  check("seek: start", (long)clip_seek_to, 0);
  check("seek: start pos", b16(4), 0);

  /* --- seek by fraction (a click on the bar) ------------------------------------- */
  pcmplay_seek_frac(128);
  check("frac: target", (long)clip_seek_to, (long)((body >> 8) * 128u));
  check("frac: pos", b16(4), 50);
  {   /* a 4-minute track: body * frac would wrap 32 bits, (body >> 8) * frac does not */
    uint32_t big = 400u * BPS;
    play(big, 0);
    pcmplay_seek_frac(255);
    check("frac big: no wrap", (long)clip_seek_to, (long)((big >> 8) * 255u));
    check("frac big: pos", b16(4), (long)(((big >> 8) * 255u + MSU_DAC_BUFSIZE) / BPS));
  }

  /* --- a seek keeps the pause ----------------------------------------------------- */
  play(body, 0);
  pcmplay_pause(1);
  pcmplay_seek_sec(30);
  check("paused seek: state", b8(3), PCMPLAY_STATE_PAUSED);
  check("paused seek: pos", b16(4), 30);
  pcmplay_pause(0);

  /* --- the end: ENDED (not an error), the bar full, the DAC still claimed ---------- */
  clip_pos = 8 + body;
  clip_playing = 0;
  clip_ended = 1;
  pcmplay_publish();
  check("end: state", b8(3), PCMPLAY_STATE_ENDED);
  check("end: pos", b16(4), 100);
  check("end: progress", b8(8), 255);
  check("end: still claimed", menu_music_locked(), 1);
  pcmplay_pause(1);                                  /* nothing to pause */
  check("end: pause ignored", b8(3), PCMPLAY_STATE_ENDED);
  pcmplay_seek_sec(90);                              /* a seek plays it again from there */
  check("end+seek: playing", b8(3), PCMPLAY_STATE_PLAYING);
  check("end+seek: pos", b16(4), 90);

  /* --- a seek whose refill read fails reports the read error ---------------------- */
  clip_seek_ok = 0;
  pcmplay_seek_sec(10);
  check("seekfail: state", b8(3), PCMPLAY_STATE_ERR_OPEN);
  check("seekfail: released", menu_music_locked(), 0);
  clip_seek_ok = 1;
  clip_seek_to = 0xffffffffu;
  pcmplay_seek_sec(10);                              /* nothing open: no engine call */
  check("seek on error: ignored", (long)clip_seek_to, (long)0xffffffffu);
  pcmplay_stop();

  /* --- Repeat (CFG.player_repeat, read live): the engine loops the track, and an EOF
         seen while Repeat was off restarts it from the top once Repeat goes on ------- */
  repeat = 1;
  play(body, 0);
  check("repeat: loops", clip_noloop, 0);
  clip_ended = 1;
  clip_seek_to = 0xffffffffu;
  pcmplay_publish();
  check("repeat: from the top", (long)clip_seek_to, 0);
  check("repeat: playing", b8(3), PCMPLAY_STATE_PLAYING);
  repeat = 0;
  pcmplay_stop();

  if(fails) { printf("%d failure(s)\n", fails); return 1; }
  printf("pcmpos: all checks passed\n");
  return 0;
}
