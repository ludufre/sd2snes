#include <string.h>
#include "config.h"
#include "uart.h"
#include "ff.h"
#include "diskio.h"
#include "spi.h"
#include "fpga_spi.h"
#include "cli.h"
#include "theme.h"
#include "fileops.h"
#include "msu1.h"
#include "snes.h"
#include "timer.h"
#include "smc.h"
#include "fpga.h"
#include "memory.h"
#include "led.h"
#include "usbinterface.h"
#include "savestate.h"
#include "cfg.h"
#include "module.h"
#include "psram_io.h"
#include "sufami.h"
#include "nesave.h"
#include "qoa.h"

FIL msudata;
FIL msuaudio;
FRESULT msu_res;
DWORD msu_cltbl[CLTBL_SIZE] IN_AHBRAM;
DWORD pcm_cltbl[CLTBL_SIZE] IN_AHBRAM;
UINT msu_audio_bytes_read = MSU_DAC_BUFSIZE / 2;
UINT msu_data_bytes_read = 1;

enum MSU_USAGE {
  MSU_IDLE = 0, // not in use
  MSU_BUSY      // in use
};

tick_t msu_last_sram_check;
uint32_t msu_last_crc;

extern snes_romprops_t romprops;
uint32_t msu_loop_point = 0;
uint32_t msu_page1_start = 0x0000;
uint32_t msu_page2_start = 0x2000;
uint16_t fpga_status_prev = 0;
uint16_t fpga_status_now = 0;
static uint8_t msu_repeat = 0;
static uint16_t dac_msb_prev = 0;   /* DAC_READ_MSB at the last audio refill */
static uint8_t msu_loop_active = 0; /* msu1_loop is running: msu1_audio_service may refill */

extern volatile cfg_t CFG;
extern volatile int reset_changed;   /* set by the reset-line edge (snes.c) */

int msu_audio_usage = MSU_IDLE;
int msu_data_usage = MSU_IDLE;

static void save_during_msu_shortreset(void) {
  snes_reset(1);
  delay_ms(1);
  if(romprops.ramsize_bytes && fpga_test() == FPGA_TEST_TOKEN) {
    writeled(1);
    save_srm(file_lfn, romprops.ramsize_bytes, SRAM_SAVE_ADDR);
    if(!file_res) saveinfo_stage(file_lfn);  /* keep the in-game SAVES tab block fresh */
    writeled(0);
  }
  nesave_flush_inreset(file_lfn);
  snes_reset(0);
}

/* Pause / resume the DAC around a long blocking operation (SD access) so a playing
   MSU-1 track doesn't re-wrap its 2 KB buffer audibly.  Both are no-ops unless a
   track is actually playing, which lets the shared command dispatcher
   (game_cmd_serve, snes.c) call them unconditionally -- outside an MSU-1 game
   msu_audio_usage is always MSU_IDLE. */
void msu_dac_hold(void) {
  if(msu_audio_usage == MSU_BUSY) dac_pause();
}

void msu_dac_release(void) {
  if(msu_audio_usage == MSU_BUSY) dac_play();
}

/* returns true if no MSU feature is in use at the moment so the SD card
   may be used to save the game */
static int is_msu_free_to_save(void) {
  return (msu_audio_usage == MSU_IDLE)
    && (msu_data_usage == MSU_IDLE);
}

/* check if SRAM content has changed and save
 * immediate: 0 = do not check if last check is less than one one second ago
 *            1 = check immediately
 */
static void msu_savecheck(int immediate) {
  uint32_t currentcrc;
  /* Serve the game's session module here too (the SPC7110 RTC-4513 backup).  msu1_loop
     is the second game loop and the switch(cmd) of the normal one never runs for an
     MSU-1 title, so anything that only lives there is silently dead in these games.
     Deliberately ABOVE the autosave gate: the RTC backup is not SRAM autosave and must
     not be switched off with it.  One branch when no module is open. */
  module_idle();
  /* Same reasoning for the Sufami Turbo Slot B battery: this is the second game loop,
     and a subsystem wired into only one of the two is dead in the other.  Above the
     autosave gate because it carries its own CFG.enable_autosave check. */
  sufami_slotb_autosave();
  /* Xeno Crisis keeps its save in the SRAM chip like a battery save, and the music never
     stops for long: without this the save would only reach the card on a reset. */
  if(!cfg_is_msu1_autosave_enabled() && !romprops.has_xc) {
    return;
  }
  /* NES battery PRG-RAM (an MSU-1 pack next to a .nes sends the game here too). Below the
     gate on purpose: a card write stalls the DAC, which is what the MSU-1 opt-in is for. */
  nesave_tick(file_lfn);
  if(immediate || (getticks() > msu_last_sram_check + MS_TO_TICKS(1000))) {
    currentcrc = calc_sram_crc(SRAM_SAVE_ADDR + romprops.srambase, romprops.sramsize_bytes, 0);
    if(msu_last_crc != currentcrc) {
      writeled(1);
      save_srm(file_lfn, romprops.ramsize_bytes, SRAM_SAVE_ADDR);
      if(!file_res) saveinfo_stage(file_lfn);  /* keep the in-game SAVES tab block fresh */
      writeled(0);
      msu_last_crc = currentcrc;
    }
    msu_last_sram_check = getticks();
  }
}

/* audio buffer refill: the DAC has moved on to the other half of its buffer */
static void __attribute__((noinline)) msu_audio_refill(uint16_t status) {
  if((status ^ dac_msb_prev) & MSU_FPGA_STATUS_DAC_READ_MSB) {
    dac_msb_prev = status & MSU_FPGA_STATUS_DAC_READ_MSB;
    set_dac_addr((status & MSU_FPGA_STATUS_DAC_READ_MSB) ? 0 : MSU_DAC_BUFSIZE / 2);
    sd_offload_tgt = 1;
    ff_sd_offload = 1;
    f_read(&msuaudio, file_buf, MSU_DAC_BUFSIZE / 2, &msu_audio_bytes_read);
  }
}

/* end of the track: loop or stop */
static void __attribute__((noinline)) msu_audio_end(void) {
  if(msu_audio_bytes_read < MSU_DAC_BUFSIZE / 2) {
    ff_sd_offload=0;
    sd_offload=0;
    DBG_MSU1 printf("wanted %u bytes, got %u (EOF)\n", MSU_DAC_BUFSIZE / 2, msu_audio_bytes_read);
    if(msu_repeat) {
      DBG_MSU1 printf("loop\n");
      ff_sd_offload=1;
      sd_offload_tgt=1;
      f_lseek(&msuaudio, MSU_PCM_OFFSET_WAVEDATA + msu_loop_point * 4);
      ff_sd_offload=1;
      sd_offload_tgt=1;
      DBG_MSU1 printf("---filling rest of buffer from loop point for %u bytes\n", (MSU_DAC_BUFSIZE / 2) - msu_audio_bytes_read);
      f_read(&msuaudio, file_buf, (MSU_DAC_BUFSIZE / 2) - msu_audio_bytes_read, &msu_audio_bytes_read);
    } else {
      set_msu_status(MSU_SNES_STATUS_CLEAR_AUDIO_PLAY);
      dac_pause();
      msu_audio_usage = MSU_IDLE;
    }
    msu_audio_bytes_read = MSU_DAC_BUFSIZE;
  }
}

/* Keeps an MSU-1 track playing from inside a long MCU job (the SaveRAM CRC and the save
   that Xeno Crisis runs while its music plays, see msu_savecheck), called between their
   sectors with the FPGA deselected.  The DAC buffer holds ~11.6 ms; a 32 KB CRC or save
   takes longer than that. */
void msu1_audio_service(void) {
  if(!msu_loop_active) return;
  msu_audio_refill(fpga_status());
  msu_audio_end();
}

static void prepare_audio_track(uint16_t msu_track, uint32_t audio_offset) {
  uint32_t audio_sect = audio_offset & ~0x1ff;
  uint32_t audio_sect_offset_sample = (audio_offset & 0x1ff) >> 2;
  DBG_MSU1 printf("offset=%08lx sect=%08lx sample=%08lx\n", audio_offset, audio_sect, audio_sect_offset_sample);
  /* open file, fill buffer */
  char suffix[11];
  dac_pause();
  f_close(&msuaudio);
  msu_audio_usage = MSU_IDLE;
  if(is_msu_free_to_save()) {
    msu_savecheck(0);
  }
  snprintf(suffix, sizeof(suffix), "-%d.pcm", msu_track);
  strcpy((char*)file_buf, (char*)file_lfn);
  strcpy(strrchr((char*)file_buf, (int)'.'), suffix);
  DBG_MSU1 printf("filename: %s\n", file_buf);
  dac_reset(audio_sect_offset_sample);
  set_msu_status(MSU_SNES_STATUS_CLEAR_AUDIO_PLAY | MSU_SNES_STATUS_CLEAR_AUDIO_REPEAT);
  if(f_open(&msuaudio, (const TCHAR*)file_buf, FA_READ) == FR_OK) {
    msuaudio.cltbl = pcm_cltbl;
    pcm_cltbl[0] = CLTBL_SIZE;
    /* A track in more fragments than the table holds (49) gets a truncated, unterminated
       map: past the last stored fragment f_read picks clusters out of whatever follows the
       table, plays the wrong data and finally fails for good (FatFs keeps the error), and
       the DAC loops its 2 KB buffer.  Such a file streams by the FAT chain instead. */
    if(f_lseek(&msuaudio, CREATE_LINKMAP) != FR_OK) msuaudio.cltbl = NULL;
    f_lseek(&msuaudio, MSU_PCM_OFFSET_LOOPPOINT);
    f_read(&msuaudio, &msu_loop_point, sizeof(msu_loop_point), &msu_audio_bytes_read);
    DBG_MSU1 printf("loop point: %ld samples\n", msu_loop_point);
    ff_sd_offload=1;
    sd_offload_tgt=1;
    f_lseek(&msuaudio, audio_sect);
    set_dac_addr(0);
    ff_sd_offload=1;
    sd_offload_tgt=1;
    f_read(&msuaudio, file_buf, MSU_DAC_BUFSIZE, &msu_audio_bytes_read);
    /* reset audio_busy + audio_error */
    set_msu_status(MSU_SNES_STATUS_CLEAR_AUDIO_BUSY | MSU_SNES_STATUS_CLEAR_AUDIO_ERROR);
//    msu_audio_usage = MSU_BUSY;
  } else {
    f_close(&msuaudio);
    /* reset audio_busy, set audio_error */
    set_msu_status(MSU_SNES_STATUS_CLEAR_AUDIO_BUSY | MSU_SNES_STATUS_SET_AUDIO_ERROR);
  }
}

static void prepare_data(uint32_t msu_offset) {
  uint32_t msu_sect = msu_offset & ~0x1ff;
  uint32_t msu_sect_offset = msu_offset & 0x1ff;
  static int seekcount = 0;
  static tick_t lasttime = 0;
  tick_t now;

  msu_data_usage = MSU_IDLE;
  if(is_msu_free_to_save()) {
    msu_savecheck(1);
  }

  DBG_MSU1 printf("Data requested! Offset=%08lx page1=%08lx page2=%08lx\n", msu_offset, msu_page1_start, msu_page2_start);
  if(   ((msu_offset < msu_page1_start)
     || (msu_offset >= msu_page1_start + MSU_DATA_BUFSIZE / 2))
     && ((msu_offset < msu_page2_start)
     || (msu_offset >= msu_page2_start + MSU_DATA_BUFSIZE / 2))) {
    seekcount++;
    if(!lasttime) {
      lasttime = getticks();
    } else {
      if((now = getticks()) >= lasttime + MS_TO_TICKS(1000)) {
        tick_t deltatime = now - lasttime;
        int seekrate = 100 * seekcount / deltatime;
        DBG_MSU1 printf("seek rate: %d per second\n", seekrate);
        seekcount = 0;
        lasttime = now;
      }
    }
    DBG_MSU1 printf("offset %08lx out of range (%08lx-%08lx, %08lx-%08lx), reload\n", msu_offset, msu_page1_start,
           msu_page1_start + MSU_DATA_BUFSIZE / 2 - 1, msu_page2_start, msu_page2_start + MSU_DATA_BUFSIZE / 2 - 1);
    /* "cache miss" - fill buffer */
    set_msu_addr(0x0);
    sd_offload_tgt=2;
    ff_sd_offload=1;
    msu_res = f_lseek(&msudata, msu_sect);
    DBG_MSU1 printf("seek to %08lx, res = %d\n", msu_sect, msu_res);
    sd_offload_tgt=2;
    ff_sd_offload=1;
    msu_res = f_read(&msudata, file_buf, MSU_DATA_BUFSIZE, &msu_data_bytes_read);
    DBG_MSU1 printf("read res = %d\n", msu_res);
    DBG_MSU1 printf("read %d bytes\n", msu_data_bytes_read);
    msu_reset(msu_sect_offset);
    msu_page1_start = msu_sect;
    msu_page2_start = msu_sect + MSU_DATA_BUFSIZE / 2;
    /* clear bank bit to mask bank reset artifact */
    fpga_status_now &= ~MSU_FPGA_STATUS_MSU_READ_MSB;
    fpga_status_prev &= ~MSU_FPGA_STATUS_MSU_READ_MSB;

  } else {
    uint16_t msu_read_offset;
    if (msu_offset >= msu_page1_start && msu_offset <= msu_page1_start + MSU_DATA_BUFSIZE / 2) {
      msu_read_offset = 0x0000 + msu_offset - msu_page1_start;
      msu_reset(msu_read_offset);
      fpga_status_now = (fpga_status_now & ~MSU_FPGA_STATUS_MSU_READ_MSB)
                      | (((msu_read_offset & (MSU_DATA_BUFSIZE / 2)) ? MSU_FPGA_STATUS_MSU_READ_MSB : 0x0000));
      DBG_MSU1 printf("inside page1, new offset: %04x\n", msu_read_offset);
      if(!(msu_page2_start == msu_page1_start + MSU_DATA_BUFSIZE / 2)) {
        set_msu_addr(MSU_DATA_BUFSIZE / 2);
        sd_offload_tgt=2;
        ff_sd_offload=1;
        f_read(&msudata, file_buf, MSU_DATA_BUFSIZE / 2, &msu_data_bytes_read);
        DBG_MSU1 printf("next page dirty (was: %08lx), loaded page2 (start now: ", msu_page2_start);
        msu_page2_start = msu_page1_start + MSU_DATA_BUFSIZE / 2;
        DBG_MSU1 printf("%08lx)\n", msu_page2_start);
      }
    } else if (msu_offset >= msu_page2_start && msu_offset <= msu_page2_start + MSU_DATA_BUFSIZE / 2) {
      msu_read_offset = 0x2000 + msu_offset - msu_page2_start;
      msu_reset(msu_read_offset);
      fpga_status_now = (fpga_status_now & ~MSU_FPGA_STATUS_MSU_READ_MSB)
                      | (((msu_read_offset & (MSU_DATA_BUFSIZE / 2)) ? MSU_FPGA_STATUS_MSU_READ_MSB : 0x0000));
      DBG_MSU1 printf("inside page2, new offset: %04x\n", msu_read_offset);
      if(!(msu_page1_start == msu_page2_start + MSU_DATA_BUFSIZE / 2)) {
        set_msu_addr(0x0);
        sd_offload_tgt=2;
        ff_sd_offload=1;
        f_read(&msudata, file_buf, MSU_DATA_BUFSIZE / 2, &msu_data_bytes_read);
        DBG_MSU1 printf("next page dirty (was: %08lx), loaded page1 (start now: ", msu_page1_start);
        msu_page1_start = msu_page2_start + MSU_DATA_BUFSIZE / 2;
        DBG_MSU1 printf("%08lx)\n", msu_page1_start);
      }
    } else printf("!!!WATWATWAT!!!\n");
  }

  /* If EOF is reached after last buffering then it's safe to assume
     that no further streaming is required unless a new data offset
     is requested.
     -> Set data_usage IDLE to enable saving.
     This is also the case if the MSU data file is 0 bytes so no special
     case will be required.
     Otherwise set data_usage BUSY as expected. */
  if(f_eof(&msudata)) {
    msu_data_usage = MSU_IDLE;
  } else {
    msu_data_usage = MSU_BUSY;
  }

  /* clear busy bit */
  set_msu_status(MSU_SNES_STATUS_CLEAR_DATA_BUSY);
}

int msu1_check(uint8_t* filename) {
/* open MSU file */
  strcpy((char*)file_buf, (char*)filename);
  strcpy(strrchr((char*)file_buf, (int)'.'), ".msu");
  printf("MSU datafile: %s\n", file_buf);
  if(f_open(&msudata, (const TCHAR*)file_buf, FA_READ) != FR_OK) {
    printf("MSU datafile not found\n");
    return 0;
  }
  msudata.cltbl = msu_cltbl;
  msu_cltbl[0] = CLTBL_SIZE;
  if(f_lseek(&msudata, CREATE_LINKMAP)) {
    printf("Error creating FF linkmap for MSU file!\n");
    msudata.cltbl = NULL;  /* truncated map: use the FAT chain (see prepare_audio_track) */
  }
  romprops.fpga_features |= FEAT_MSU1;
  return 1;
}

int msu1_loop() {
/* it is assumed that the MSU file is already opened by calling msu1_check(). */
  uint16_t dac_addr = 0;
  uint16_t msu_addr = 0;
  uint16_t msu_track = 0;
  uint32_t msu_offset = 0;
  int32_t resume_msu_track = -1;
  uint32_t resume_msu_offset = 0;
  int msu_res;
  uint8_t cmd;

  /* set initial last SRAM check to 1s in the past to trigger a single
     immediate SRAM check after booting the game */
  msu_last_sram_check = getticks() - MS_TO_TICKS(1000);
  msu_last_crc = calc_sram_crc(SRAM_SAVE_ADDR + romprops.srambase, romprops.sramsize_bytes, 0);

  msu_page1_start = 0x0000;
  msu_page2_start = MSU_DATA_BUFSIZE / 2;

  set_dac_addr(dac_addr);
  dac_pause();
  dac_reset(0);

  set_msu_addr(0x0);
  msu_reset(0x0);
  ff_sd_offload=1;
  sd_offload_tgt=2;
  f_lseek(&msudata, 0L);
  ff_sd_offload=1;
  sd_offload_tgt=2;
  f_read(&msudata, file_buf, MSU_DATA_BUFSIZE, &msu_data_bytes_read);

  prepare_audio_track(0, MSU_PCM_OFFSET_WAVEDATA);
  prepare_data(0);
  msu_data_usage = MSU_IDLE;

/* audio_start, data_start, 0, audio_ctrl[1:0], ctrl_start */
  msu_res = SNES_RESET_NONE;
  msu_repeat = 0;
  fpga_status_prev = fpga_status();
  fpga_status_now = fpga_status();
  dac_msb_prev = fpga_status_now & MSU_FPGA_STATUS_DAC_READ_MSB;
  msu_loop_active = 1;
  while(msu_res == SNES_RESET_NONE){
    /* FPGA liveness.  The main loop makes this its while condition and led_panics on
       the way out; this loop had no check at all, so a dead FPGA left it spinning on
       garbage status words with nothing to show for it.  led_panic never returns. */
    if(fpga_test() != FPGA_TEST_TOKEN) led_panic(LED_PANIC_FPGA_DEAD);
    cmd = snes_get_mcu_cmd();
    /* Serve USB-issued commands (RESET / MENU_RESET) exactly like the ones from
       the SNES.  This used to run AFTER the switch and only assign to cmd, which
       the next iteration overwrote with snes_get_mcu_cmd() -- so every USB command
       was silently dropped and MENU_RESET (flash.sh, Web Manager) did nothing while
       an MSU-1 game was running.  SNES commands win; GAMELOOP just means "you are
       already in the game loop", consumed here like the main loop does. */
    if(!cmd) {
      cmd = usbint_handler();
      if(cmd == SNES_CMD_GAMELOOP) cmd = 0;
    }
    /* A USB-driven boot/reset drives the reset line itself while usbint_handler works.
       Skip the rest of the pass like the main loop does -- and note this HAS to come
       before get_snes_reset_state(), which would otherwise read the line the server is
       driving as a user reset and tear the game down mid-transfer. */
    if(usbint_server_reset()) continue;
    /* Console reset sensed mid-game: re-arm the SRTC.  This loop only did it on the way
       out, so an SRTC game that was reset during play kept running on the stale state. */
    if(reset_changed) {
      printf("reset\n");
      reset_changed = 0;
      fpga_reset_srtc_state();
    }
    msu_res = get_snes_reset_state();
    /* Combo carts: snes_reset_loop() only reloads slot 0 when the reset came from the
       button or a combo (snes.c).  The main loop raises this flag on a short reset and
       in the RESET arm below; the MSU loop raised it nowhere. */
    if(msu_res == SNES_RESET_SHORT) resetButtonState = 1;
    if(cmd) {
      /* everything the in-game shell and the overlay issue is served by the shared
         dispatcher (snes.c), so this loop and the main one can never drift again.
         What stays here is what genuinely differs: leaving the loop restarts MSU
         streaming from scratch, which the main loop has no notion of. */
      if(!game_cmd_serve(cmd)) switch(cmd) {
        case SNES_CMD_RESET_LOOP_FAIL:
          msu_res = SNES_RESET_SHORT;
          snes_reset_loop();
          break;
        case SNES_CMD_RESET:
          msu_res = SNES_RESET_SHORT;
          resetButtonState = 1;   /* force the full ROM reset on a combo cart */
          snes_reset_pulse();
          break;
        case SNES_CMD_RESET_TO_MENU:
          msu_res = SNES_RESET_LONG;
          break;
        case SNES_CMD_COMBO_TRANSITION:
          /* multicart slot switch.  The reload runs msu1_check() again, which
             reopens .msu behind our back, so every streaming offset we hold is
             stale.  Return 0 instead of continuing: main.c loops on
             `while(!msu1_loop())`, so it re-enters and rebuilds the whole MSU
             state (handles, buffer pages, DAC, track) from scratch.  The short-
             reset save is skipped on purpose -- the reload already reset the SNES,
             exactly like the main loop's own COMBO_TRANSITION arm. */
          dac_pause();
          f_close(&msuaudio);
          msu_audio_usage = MSU_IDLE;
          msu_data_usage = MSU_IDLE;
          load_rom(file_lfn, SRAM_ROM_ADDR, LOADROM_WITH_COMBO | LOADROM_WITH_RESET);
          snes_set_mcu_cmd(0);
          return 0;
        default:
          printf("unknown cmd: %02x\n", cmd);
          break;
      }
      snes_set_mcu_cmd(0);
    }
    cli_entrycheck();

    fpga_status_now = fpga_status();

    /* ACK as fast as possible */
    if(fpga_status_now & MSU_FPGA_STATUS_CTRL_START) {
      set_msu_status(MSU_INT_STATUS_CLEAR_CTRL_PENDING);
    }

    /* Data buffer refill */
    if((fpga_status_now & MSU_FPGA_STATUS_MSU_READ_MSB) != (fpga_status_prev & MSU_FPGA_STATUS_MSU_READ_MSB)) {
      DBG_MSU1 printf("old MSB=%04x new MSB=%04x data\n", fpga_status_prev & MSU_FPGA_STATUS_MSU_READ_MSB, fpga_status_now & MSU_FPGA_STATUS_MSU_READ_MSB);
      if(fpga_status_now & MSU_FPGA_STATUS_MSU_READ_MSB) {
        msu_addr = 0x0;
        msu_page1_start = msu_page2_start + MSU_DATA_BUFSIZE / 2;
      } else {
        msu_addr = MSU_DATA_BUFSIZE / 2;
        msu_page2_start = msu_page1_start + MSU_DATA_BUFSIZE / 2;
      }
      set_msu_addr(msu_addr);
      sd_offload_tgt = 2;
      ff_sd_offload = 1;
      /* NOT into msu_res: that variable carries the pending reset request and is
         the loop's exit condition.  Storing FR_OK (== SNES_RESET_NONE) here threw
         away a SNES_RESET_LONG raised earlier in the SAME iteration -- the SNES
         had already parked itself in nmi_stop and the MCU_CMD was already ACKed,
         so the console stayed frozen on a black screen and never reached the menu. */
      FRESULT refill_res = f_read(&msudata, file_buf, MSU_DATA_BUFSIZE / 2, &msu_data_bytes_read);
      if(f_eof(&msudata)) {
        msu_data_usage = MSU_IDLE;
      }
      DBG_MSU1 printf("data page %d refilled. res=%d page1=%08lx page2=%08lx\n", msu_addr ? 2 : 1, refill_res, msu_page1_start, msu_page2_start);
      (void)refill_res;
    }

    /* Audio buffer refill */
    msu_audio_refill(fpga_status_now);

    if(fpga_status_now & MSU_FPGA_STATUS_AUDIO_START) {
      /* get trackno */
      msu_track = get_msu_track();
      DBG_MSU1 printf("Audio requested! Track=%d\n", msu_track);

      prepare_audio_track(msu_track, (msu_track == resume_msu_track) ? resume_msu_offset : MSU_PCM_OFFSET_WAVEDATA);
      if(msu_track == resume_msu_track) {
        resume_msu_track = -1;
      }
    }

    if(fpga_status_now & MSU_FPGA_STATUS_DATA_START) {
      /* get address */
      msu_offset=get_msu_offset();
      prepare_data(msu_offset);
    }

    if(fpga_status_now & MSU_FPGA_STATUS_CTRL_START) {
      if(fpga_status_now & MSU_FPGA_STATUS_CTRL_RESUME_FLAG_BIT && !(fpga_status_now & MSU_FPGA_STATUS_CTRL_PLAY_FLAG_BIT)) {
        resume_msu_track = msu_track;
        resume_msu_offset = f_tell(&msuaudio);
      }

      if(fpga_status_now & MSU_FPGA_STATUS_CTRL_REPEAT_FLAG_BIT) {
        msu_repeat = 1;
        set_msu_status(MSU_SNES_STATUS_SET_AUDIO_REPEAT);
        DBG_MSU1 printf("Repeat set!\n");
      } else {
        msu_repeat = 0;
        set_msu_status(MSU_SNES_STATUS_CLEAR_AUDIO_REPEAT);
        DBG_MSU1 printf("Repeat clear!\n");
      }

      if(fpga_status_now & MSU_FPGA_STATUS_CTRL_PLAY_FLAG_BIT) {
        DBG_MSU1 printf("PLAY!\n");
        set_msu_status(MSU_SNES_STATUS_SET_AUDIO_PLAY);
        msu_audio_usage = MSU_BUSY;
        dac_play();
      } else {
        DBG_MSU1 printf("PAUSE!\n");
        set_msu_status(MSU_SNES_STATUS_CLEAR_AUDIO_PLAY);
        msu_audio_usage = MSU_IDLE;
        dac_pause();
      }
    }

    fpga_status_prev = fpga_status_now;

    /* handle loop / end */
    msu_audio_end();

    /* check if we can sneak in an SRAM poll / save.  Xeno Crisis: also while the music
       plays -- the CRC and the save keep the DAC buffer filled through xc_service(). */
    if(is_msu_free_to_save() || romprops.has_xc) {
      msu_savecheck(0);
    }
  }
  msu_loop_active = 0;
  dac_pause();
  f_close(&msuaudio);
  msu_audio_usage = MSU_IDLE;
  msu_data_usage = MSU_IDLE;
// TODO have FPGA automatically reset SRTC on detected reset
  fpga_reset_srtc_state();
  DBG_MSU1 printf("Reset ");
  if(msu_res == SNES_RESET_LONG) {
    f_close(&msudata);
    DBG_MSU1 printf("to menu\n");
    return 1;
  }
  save_during_msu_shortreset();
  DBG_MSU1 printf("game\n");
  return 0;
}

/* =======================================================================
   Menu navigation sound effects via the MSU-1 DAC (one-shot).

   Music stays on the SPC700 (menu.spc); the DAC only ever plays a
   short one-shot effect.
   ======================================================================= */

static FIL menusfx_fil;
static uint32_t menusfx_loop_point = 0;
static UINT menusfx_bytes_read = 0;
static uint16_t menusfx_fpga_prev = 0;
static uint8_t menusfx_wraps = 0;
static int menusfx_active = 0;
static int menusfx_open = 0;               /* menusfx_fil currently holds an open effect */
static const char *menusfx_open_name = 0;  /* which effect is open (same static strings) */
static tick_t menusfx_deadline = 0;        /* watchdog: latest tick an effect may run to */
static int menusfx_loop_forever = 0;       /* 1 = FMV music: loop the clip, no one-shot deadline */
static int menusfx_locked = 0;             /* 1 = a caller claimed the DAC exclusively (see
                                              menu_music_lock) */
static uint32_t menusfx_samples = 0;       /* FMV music: stereo samples fed to the DAC since the
                                              loop start -> the FMV video frame clock (sync) */
static uint8_t menusfx_noloop = 0;         /* 1 = play the clip once and stop at EOF (the menu
                                              PCM player moves on to the next track itself) */
static uint8_t menusfx_eof = 0;            /* noloop: EOF read, the DAC is on the last full half */
static uint8_t menusfx_ended = 0;          /* noloop: the clip played out, DAC paused at its end */
static uint8_t menusfx_paused = 0;         /* menu_music_pause: a seek must not unpause */

/* A QOA clip soundtrack (<rom>.qoa) is not streamed through the 2 KB dac_buf like a .pcm.
   The MCU decodes it into the PSRAM ROM area, idle while the menu runs, and the FPGA SFX
   fetcher (sfxdma.v) plays it from there exactly like a nav blip -- the MCU cannot write
   dac_buf itself (the SPI write opcode only reaches PSRAM).  Decoding runs ahead of the
   fetcher from the menu loop (menu_music_work).
   The clip is laid out back to back as many times as the area holds (a 30 s clip twice, a
   6 s one eleven times) and then its first 2 KB once more, so the fetcher runs through the
   whole run on its own; only the end of the run costs a re-kick, whose prime holds the DAC
   ~4 ms (measured on a Mk.III).  Re-kicking at every loop made that a click per loop. */
#define MENUQOA_ADDR     SRAM_ROM_ADDR
#define MENUQOA_PAD      MSU_DAC_BUFSIZE   /* the clip's first 2 KB again after the run */
#define MENUQOA_MAX      (SRAM_DIRINDEX_ADDR - SRAM_ROM_ADDR - MENUQOA_PAD) /* ~70 s; a longer clip is cut */
#define MENUQOA_LEAD     0x8000UL   /* decoded before the fetcher starts (~186 ms, ~64 ms of work) */
#define MENUQOA_GUARD    0x1000UL   /* the fetcher this close to the decoder -> hold the DAC */
#define MENUQOA_RESUME   0x8000UL   /* ... until the decoder is this far ahead again */
#define MENUQOA_STEP     80         /* sample frames per decode step: 320 B of stack */
#define MENUQOA_TICK_B   (176400UL / MS_TO_TICKS(1000))    /* DAC bytes per timer tick */
#define MENUQOA_TICK_HI  (MENUQOA_TICK_B * 65 / 64)         /* ... plus 1.6 %, in case the DAC clock is not exact */
/* The pump knows the DAC's position by one bit, DAC_READ_MSB (the half of dac_buf it reads),
   and a pump later than a half-buffer (~5.8 ms) used to miss two flips at a time: the count
   fell behind the fetcher (~0.5 s in 95 s while an NSF rendered), and at the end of a run the
   fetcher played silence until the late count reached the re-kick.  The bit gives the parity
   of the flips and the cycle counter the time since the last pump, which together give the
   count.  (Not the timer: on the LPC175x a tick is 10.4 ms, the SysTick reload is the 100 MHz
   TENMS calibration.) */
#define MENUQOA_HALF_CYC ((uint32_t)((uint64_t)CONFIG_CPU_FREQUENCY * (MSU_DAC_BUFSIZE / 2) / 176400))
#define MENUQOA_STALL    1          /* menuqoa_hold: the decoder fell behind the fetcher */
#define MENUQOA_PAUSED   2          /* menuqoa_hold: menu_music_pause */
#define MENUQOA_START    4          /* menuqoa_hold: opened, the lead-in not decoded yet */
static qoa_dec_t menuqoa_dec;
static uint8_t   menuqoa = 0;       /* the playing music clip is a QOA decoded into PSRAM */
static uint8_t   menuqoa_hold = 0;  /* MENUQOA_STALL | _PAUSED | _START: DAC held, clock frozen */
static uint8_t   menuqoa_sealed;    /* the run is complete and the pad after it written */
static uint32_t  menuqoa_wr;        /* bytes of the run decoded so far */
static uint32_t  menuqoa_copy;      /* where the copy being decoded starts */
static uint32_t  menuqoa_len;       /* one clip, in bytes; 0 until the first copy is complete */
static uint32_t  menuqoa_end;       /* the run: whole copies (once sealed) */
static uint32_t  menuqoa_pos;       /* run byte the last kick started the fetcher at */
static uint32_t  menuqoa_halves;    /* DAC half-buffers (1 KB) played since the last kick */
static tick_t    menuqoa_t0;        /* tick of the last kick, pushed forward by every hold */
static tick_t    menuqoa_held_at;   /* tick the current hold began */
static uint32_t  menuqoa_cyc;       /* cycle count of the last pump, or of the DAC starting again */
/* An endless generator (the NSF player, nsfhost.c) plays through the same fetcher with the
   area as a RING: menu_music_work has gen fill it ahead of the DAC, wrapping onto bytes the
   DAC already played, and the end-of-run re-kick takes the fetcher back to the top once a
   lap (one ~4 ms seam per lap).  The run machinery is reused as is: the run is "sealed" from
   the start with end = len = the ring, and the generated bytes go into the pad too.
   menuqoa_wr then counts every byte generated, menuqoa_lap is that count at the top of the
   fetcher's current lap. */
static menu_music_gen_fn menuqoa_gen;
static void     *menuqoa_gen_ctx;
static uint32_t  menuqoa_lap;
/* A generator's frames are mono (L = R): when the base core's fetcher can play a mono body
   (FPGA_CMD_SFX_STATUS bit 2), the ring keeps only L, 2 bytes a frame, and the fetcher writes
   each frame to the DAC twice.  The PSRAM writes halve, and they cost ~84 MCU cycles a byte.
   DAC bytes = run bytes << menuqoa_dsh (0 = stereo, 1 = mono); positions in the run (pos, wr,
   lap, end) are run bytes, DAC half-buffers and the tick rate are DAC bytes. */
static uint8_t   menuqoa_dsh;
#define RUNB(dac) ((uint32_t)(dac) >> menuqoa_dsh)
/* The fetcher's length is 24 bits, and on a core with the mono body bit 23 IS that flag:
   a length of 8 MB or more read as "mono" played a stereo clip at half speed (a ~70 s QOA
   run is ~12 MB), and the NSF ring (~11 MB) lost its bit 23 and stopped ~38 s in.  So no
   kick asks for more than MENUQOA_RUNCAP: a longer run is kicked again where the DAC is,
   once it passes menuqoa_kend -- the seam of the loop, one every ~47 s stereo / ~95 s mono.
   The bytes past menuqoa_kend are the run itself, so the fetcher's last PAD is real audio. */
#define MENUQOA_RUNCAP   0x7FF000UL
static uint32_t  menuqoa_kend;      /* run byte to kick again at (0 = the run fits the cap) */
#define MENUQOA_DONE (menuqoa_sealed && !menuqoa_gen)   /* a clip's run is decoded to the end */

/* Cap on how long one effect may stay "active". Real blips are well under a
   second; this only matters if the DAC never toggles (e.g. an abnormal FPGA
   state) - it stops the effect so the menu loop can't busy-spin on it forever. */
#define MENU_SFX_MAX_TICKS MS_TO_TICKS(4000)

static void menusfx_close(void) {
  if(menusfx_open) { f_close(&menusfx_fil); menusfx_open = 0; menusfx_open_name = 0; }
}

/* =======================================================================
   FPGA-autonomous navigation SFX (sfxdma.v).

   The 2 KB dac_buf that MSU-1 / FMV music stream through holds only ~11.6 ms
   and must be refilled by the MCU every ~5.8 ms.  In the menu the MCU blocks
   far longer than that (dir scan, f_open + FAT cluster walk, cover load), which
   used to leave the DAC re-wrapping the last 2 KB -- the audible "frozen loop".
   Nav blips now play from the FPGA sfxdma engine: each effect's PCM body is
   preloaded into PSRAM once, and the FPGA streams it into dac_buf on its own,
   immune to any MCU stall.  (FMV info-screen MUSIC from a .pcm still uses the MCU-fed
   path; a .qoa one is decoded into PSRAM and played by this same fetcher, see menuqoa.)

   The four effects SHARE the 256 KB window as one budget, handed out by a bump
   allocator in first-use order, instead of owning a fixed 64 KB slot each.  At
   44.1 kHz 16-bit stereo (4 bytes per frame) a fixed slot was 0.371 s, and an
   effect that did not fit was REJECTED by menusfx_preload (ready = -1), which
   makes menu_sfx_play return without a sound -- so a long effect went silent
   rather than being cut short, with nothing on screen to say why.  Sharing lets a
   single effect run to ~1.49 s and lets the four coexist in any mix that fits the
   window (e.g. 17K + 36K + 20K + 180K).  Consequence: a slot's base depends on
   what was loaded before it and on its own size, so it can only be fixed inside
   menusfx_preload, once f_size is known -- and an effect may now straddle a PSRAM
   bank boundary, which the fetcher does not care about (sfxdma.v addresses PSRAM
   as base_r + src_off, one flat 24-bit byte address).
   ======================================================================= */
#define MENU_SFX_SLOTS   4
#define MENU_SFX_WINDOW  0x40000UL  /* the whole free 0xCC0000..0xCFFFFF PSRAM window */
typedef struct {
  const char *name;    /* stable static path pointer; 0 = free slot */
  uint32_t    base;    /* PSRAM byte address of the PCM body (set by menusfx_preload) */
  uint32_t    bytelen; /* body length in bytes (frame_count * 4) */
  int8_t      ready;   /* 1 = preloaded ok, -1 = missing/bad, 0 = not loaded yet */
} menusfx_slot_t;
static menusfx_slot_t menusfx_slots[MENU_SFX_SLOTS];  /* .bss: zero-init = all free */
static uint32_t menusfx_next = SRAM_MENU_SFX_ADDR;    /* bump allocator: next free byte */

/* Keep only L of a body whose channels are identical: the fetcher repeats it on R (sfxdma.v,
   mono bodies), so the effect costs half the shared window and sounds exactly the same.
   0 = L != R somewhere, or a read failed: the caller falls back to the stereo path. */
static __attribute__((noinline)) int menusfx_stream_mono(FIL *fp, uint32_t addr, uint32_t size) {
  uint16_t *p = (uint16_t*)file_buf;
  while(size) {
    UINT br = 0, i;
    if(f_read(fp, file_buf, size < 512 ? size : 512, &br) != FR_OK || !br || (br & 3)) return 0;
    for(i = 0; i < br / 4; i++) {
      if(p[2 * i] != p[2 * i + 1]) return 0;
      p[i] = p[2 * i];
    }
    sram_writeblock(file_buf, addr, br / 2);
    addr += br / 2;
    size -= br;
  }
  return 1;
}

/* Stream one effect's PCM body (offset 8..EOF) into its PSRAM slot, once.
   Bounded + fail-safe (never hangs the MCU): any error marks the slot silent.
   The FIL is on the stack: it is open only inside this call, which runs from the idle
   menu loop (menu_main_loop -> menu_sfx_play), far shallower than the deep command
   paths that set the stack peak -- a static one held 552 B of RAM for the whole session.
   noinline keeps the frame out of menu_main_loop. */
static __attribute__((noinline)) void menusfx_preload(menusfx_slot_t *s) {
  FIL menusfx_pre_fil;
  UINT br = 0;
  uint8_t magic[4];
  DWORD fsz;
  uint32_t need, start = 0, region = 0;
  s->ready = -1;                                    /* pessimistic until fully streamed */
  ff_sd_offload = 0; sd_offload = 0;                /* the magic read below is a normal RAM read */
  /* the chosen theme's own effect first; everything below reads the same from either */
  if(theme_sfx_open(&menusfx_pre_fil, s->name, &region)) start = menusfx_pre_fil.fptr;
  else if(f_open(&menusfx_pre_fil, (const TCHAR*)s->name, FA_READ) != FR_OK) return;
  if(f_read(&menusfx_pre_fil, magic, 4, &br) != FR_OK || br != 4 || memcmp(magic, "MSU1", 4)) {
    f_close(&menusfx_pre_fil); return;              /* not a valid MSU-1 PCM */
  }
  fsz = region ? region : f_size(&menusfx_pre_fil);
  if(fsz <= MSU_PCM_OFFSET_WAVEDATA) {
    f_close(&menusfx_pre_fil); return;              /* header only, no body */
  }
  s->bytelen = (uint32_t)(fsz - MSU_PCM_OFFSET_WAVEDATA);
  /* Claim the body out of the shared window, rounded up to a 4-byte DAC frame so the
     NEXT effect still starts on a frame boundary.  Written as "space left" rather than
     "end >= next + need" so it cannot overflow.  No room -> ready stays -1, i.e. this
     effect is silent and the menu is otherwise unaffected (same fail-safe as before,
     only now it takes a genuinely oversized set of effects to hit it).  A body with L == R
     on a core that plays mono bodies takes half of that; bytelen then carries the flag
     fpga_sfx_play wants, so menu_sfx_play needs no second field. */
  need = ((s->bytelen >> 1) + 3) & ~(uint32_t)3;
  if(!(s->bytelen & 3) && (fpga_sfx_status() & FPGA_SFX_ST_MONO)
     && need <= (SRAM_MENU_SFX_ADDR + MENU_SFX_WINDOW) - menusfx_next) {
    f_lseek(&menusfx_pre_fil, start + MSU_PCM_OFFSET_WAVEDATA);
    if(menusfx_stream_mono(&menusfx_pre_fil, menusfx_next, s->bytelen)) {
      f_close(&menusfx_pre_fil);
      s->base = menusfx_next;
      s->bytelen = (s->bytelen >> 1) | FPGA_SFX_MONO;
      menusfx_next += need;
      s->ready = 1;
      return;
    }
  }
  need = (s->bytelen + 3) & ~(uint32_t)3;
  if(need > (SRAM_MENU_SFX_ADDR + MENU_SFX_WINDOW) - menusfx_next) {
    f_close(&menusfx_pre_fil); return;
  }
  s->base = menusfx_next;
  /* Stream SD -> PSRAM the SAME way load_cover does (cover.c cover_stream): f_read into
     file_buf, then sram_writeblock into PSRAM.  NOT sd_offload DMA: sd_offload asserts
     SD_DMA_TO_ROM, which forces ROM_ADDR=MCU_ADDR for the WHOLE transfer.  A nav SFX is
     fired fire-and-forget (snes.c) while the SNES is running the menu FROM PSRAM, so an
     sd_offload preload hijacks the SNES's own opcode fetches -> it reads garbage -> hard
     freeze on the first blip.  sram_writeblock uses MCU writes that interleave in free
     slots (never taking ROM_ADDR from the live SNES), exactly like the cover load that
     already streams to PSRAM on every browse without ever freezing. */
  f_lseek(&menusfx_pre_fil, start + MSU_PCM_OFFSET_WAVEDATA);
  if(!psram_stream(&menusfx_pre_fil, s->base, s->bytelen, 0)) {
    f_close(&menusfx_pre_fil); return;              /* read error -> stay silent */
  }
  f_close(&menusfx_pre_fil);
  menusfx_next += need;                             /* commit only what was fully streamed */
  s->ready = 1;
}

/* Resolve (and lazily preload) the PSRAM slot for a nav-SFX path.  Keyed by the
   stable static string pointer the menu passes, so at most 4 effects are cached. */
static menusfx_slot_t *menusfx_slot_for(const char *filename) {
  int i;
  for(i = 0; i < MENU_SFX_SLOTS; i++)
    if(menusfx_slots[i].name == filename) return &menusfx_slots[i];
  for(i = 0; i < MENU_SFX_SLOTS; i++)
    if(!menusfx_slots[i].name) {                    /* claim a free slot + preload it */
      menusfx_slots[i].name = filename;
      /* No base here: the bodies share one budget, so where this one lands depends on
         its own size, which menusfx_preload learns from f_size. */
      menusfx_preload(&menusfx_slots[i]);
      return &menusfx_slots[i];
    }
  return 0;                                         /* >4 distinct effects (shouldn't happen) */
}

/* Drop the preload cache: the PSRAM slots may be clobbered while a game runs, so
   re-preload on the next blip after returning to the menu.  Public because anything that
   writes over 0xCC0000..0xCFFFFF has to say so -- the cache lives in .bss and survives a
   menu reload, so a stale "ready" slot plays whatever now sits there (memtest.c). */
void menu_sfx_forget(void) {
  int i;
  for(i = 0; i < MENU_SFX_SLOTS; i++) { menusfx_slots[i].name = 0; menusfx_slots[i].ready = 0; }
  /* The bump allocator has to be rewound WITH the table: the caller has just made the
     whole window free again (memtest.c overwrites all of it), and a pointer left where
     it stopped would keep handing out space that no longer exists -- after a couple of
     forget/re-preload rounds nothing would fit and every effect would go silent. */
  menusfx_next = SRAM_MENU_SFX_ADDR;
}

int menu_sfx_active(void) {
  return menusfx_active;   /* MCU-fed FMV music only; nav SFX are autonomous (sfxdma) */
}

void menu_sfx_stop(void) {
  fpga_sfx_disable();   /* abort the FPGA nav-SFX fetcher if one is running */
  dac_pause();          /* freeze the DAC read pointer -> silence (no residual loop; stops music too) */
  menusfx_active = 0;   /* clear the MCU-fed (FMV music) flag */
  menuqoa = 0;          /* and a QOA clip played by the fetcher */
  menuqoa_hold = 0;
  menuqoa_gen = 0;
}

void menu_sfx_shutdown(void) {
  menu_sfx_stop();
  menusfx_close();        /* release the FMV-music handle */
  menu_sfx_forget();      /* PSRAM slots may be clobbered by the game -> re-preload later */
  if(current_features & FEAT_MSU1)
    fpga_set_features(current_features & ~FEAT_MSU1);
}

void menu_sfx_play(const char *filename) {
  /* Resolve the effect's PSRAM slot (lazily preloading its PCM body on first use).
     Absent / bad / no free slot -> stay silent, menu unaffected. */
  menusfx_slot_t *s = menusfx_slot_for(filename);
  if(!s || s->ready != 1) return;

  /* Keep FEAT_MSU1 enabled so the MSU volume register the menu set stays live on
     the DAC (the DAC engine + volume path are unchanged; only the dac_buf source
     moves from the MCU SD stream to the FPGA fetcher). */
  if(!(current_features & FEAT_MSU1))
    fpga_set_features(current_features | FEAT_MSU1);

  /* Hand the effect to the FPGA: reset the DAC read pointer, arm sfxdma with the
     PSRAM base+length and kick it (newest wins), then release play.  sfxdma holds
     play off (prime_hold) until it has primed the whole 2 KB, so no garbage frames.
     From here the FPGA streams the whole one-shot into the DAC on its own -- it
     never depends on the MCU again, so a long blocking SD op can't freeze it. */
  dac_reset(0);
  fpga_sfx_play(s->base, s->bytelen);
  dac_play();
  DBG_MSU1 printf("sfx: %s @%06lx len %lu\n", filename,
                  (unsigned long)s->base, (unsigned long)s->bytelen);
}

/* ---- QOA clip soundtrack (see MENUQOA_ADDR) ---- */

static unsigned menuqoa_read(void *ctx, uint8_t *buf, unsigned len) {
  UINT br = 0;
  (void)ctx;
  ff_sd_offload = 0; sd_offload = 0;   /* a plain RAM read, never the FPGA SD DMA */
  if(f_read(&menusfx_fil, buf, len, &br) != FR_OK) return 0;
  return br;
}

/* Back to the top of the file for the next copy (also the first one); 0 = unusable. */
static uint32_t menuqoa_rewind(void) {
  uint8_t hdr[QOA_HEADER_SIZE];
  f_lseek(&menusfx_fil, 0);
  if(menuqoa_read(0, hdr, sizeof(hdr)) != sizeof(hdr)) return 0;
  return qoa_dec_open(&menuqoa_dec, hdr, 44100);
}

/* Upper bound of where the fetcher reads: the 2 KB prime plus every byte the DAC could
   have played since the kick (two ticks of slack), frozen while the DAC is held. */
static uint32_t menuqoa_fetch_max(void) {
  tick_t now = menuqoa_hold ? menuqoa_held_at : getticks();
  uint32_t played = (uint32_t)(now - menuqoa_t0 + 2) * MENUQOA_TICK_HI;
  uint32_t counted = menuqoa_halves * (MSU_DAC_BUFSIZE / 2);
  return menuqoa_pos + RUNB(MSU_DAC_BUFSIZE + (played > counted ? played : counted));
}

/* Bring menuqoa_halves up to date with the DAC: the half-buffers played since the last
   count, from the cycle counter, with the parity fixed by the flip bit of the status word
   (floor(time / half) or one more, whichever has the parity).  Held, the DAC stands still:
   at most the flip it was in. */
static void menuqoa_count(void) {
  uint16_t now = fpga_status();
  uint32_t cyc = getcycles();
  uint32_t flip = ((now ^ menusfx_fpga_prev) & MSU_FPGA_STATUS_DAC_READ_MSB) ? 1 : 0;
  if(menuqoa_hold) menuqoa_halves += flip;
  else {
    uint32_t n = (cyc - menuqoa_cyc) / MENUQOA_HALF_CYC;
    menuqoa_halves += n + ((n ^ flip) & 1);
  }
  menuqoa_cyc = cyc;
  menusfx_fpga_prev = now;
}

static void menuqoa_hold_set(uint8_t bit, int on) {
  uint8_t was = menuqoa_hold;
  if(on) menuqoa_hold |= bit; else menuqoa_hold &= ~bit;
  if(!was && menuqoa_hold) {
    /* count up to the pause first: the menu loop pumps every ~20 ms (3 half-buffers),
       and the halves between the last pump and here were lost before, so every
       pause/resume left the DAC's position behind by that much */
    menuqoa_count();
    dac_pause();
    menuqoa_held_at = getticks();
  }
  else if(was && !menuqoa_hold) {
    menuqoa_t0 += getticks() - menuqoa_held_at;
    menuqoa_cyc = getcycles();
    dac_play();
  }
}

/* (Re)start the fetcher at run byte `pos`, with the pad after the run in its range.  At the
   end of the run the fetcher reads the pad (the start of the clip), and the first
   half-buffer flip after the DAC crosses the end re-kicks it at the clip byte the DAC has
   reached: the seam is the prime, plus the few samples the pump took to see the flip heard
   twice, never a silence of the leftover half-buffer. */
static void menuqoa_kick(uint32_t pos) {
  uint32_t len = (menuqoa_sealed ? menuqoa_end : MENUQOA_MAX) + MENUQOA_PAD - pos;
  menuqoa_kend = 0;
  if(len > MENUQOA_RUNCAP) {
    len = MENUQOA_RUNCAP;
    menuqoa_kend = pos + MENUQOA_RUNCAP - MENUQOA_PAD;
  }
  dac_pause();
  dac_reset(0);
  fpga_sfx_play(MENUQOA_ADDR + pos, len | (menuqoa_dsh ? FPGA_SFX_MONO : 0));
  menuqoa_pos = pos;
  menuqoa_halves = 0;
  menusfx_fpga_prev = fpga_status();
  menuqoa_cyc = getcycles();
  menuqoa_t0 = getticks();
  if(menuqoa_hold) menuqoa_held_at = menuqoa_t0;   /* paused before the clip even started */
  else dac_play();
  printf("qoa: kick @%lu %lu/%lu\n", (unsigned long)pos, (unsigned long)menuqoa_wr,
         (unsigned long)menuqoa_len);
}

/* The run is complete: its end is `menuqoa_wr`, cut back to whole copies, and the 2 KB
   after it get the start of the clip. */
static void menuqoa_seal(uint8_t *buf, uint16_t bufsize) {
  uint32_t at = 0;
  if(!menuqoa_len) menuqoa_len = menuqoa_wr;          /* the clip did not fit the area */
  menuqoa_end = menuqoa_len ? menuqoa_wr / menuqoa_len * menuqoa_len : 0;
  menuqoa_wr = menuqoa_end;
  menuqoa_sealed = 1;
  while(menuqoa_len && at < MENUQOA_PAD) {
    uint32_t src = at % menuqoa_len;
    uint32_t n = MENUQOA_PAD - at;
    if(n > bufsize) n = bufsize;
    if(n > menuqoa_len - src) n = menuqoa_len - src;
    sram_readblock(buf, MENUQOA_ADDR + src, (uint16_t)n);
    sram_writeblock(buf, MENUQOA_ADDR + menuqoa_end + at, (uint16_t)n);
    at += n;
  }
}

/* Decode up to `bytes` more of the run into PSRAM.  A copy ends where the decoder stops:
   the end of the file, or a broken spot (the first copy then defines the clip, and every
   copy stops at the same place).  The next copy starts only if it fits whole. */
static __attribute__((noinline)) void menuqoa_decode(uint32_t bytes) {
  int16_t pcm[MENUQOA_STEP * 2];
  while(bytes && !menuqoa_sealed) {
    uint32_t room = (MENUQOA_MAX - menuqoa_wr) / 4;
    unsigned want = room < MENUQOA_STEP ? (unsigned)room : MENUQOA_STEP;
    unsigned got = want ? qoa_dec_run(&menuqoa_dec, pcm, want, menuqoa_read, 0) : 0;
    if(got) {
      sram_writeblock(pcm, MENUQOA_ADDR + menuqoa_wr, got * 4);
      menuqoa_wr += got * 4;
    }
    if(!want || got < want) {
      if(!menuqoa_len && want) menuqoa_len = menuqoa_wr;   /* the first copy is the clip */
      if(!menuqoa_len || menuqoa_wr - menuqoa_copy != menuqoa_len
         || menuqoa_wr + menuqoa_len > MENUQOA_MAX || !menuqoa_rewind()) {
        menuqoa_seal((uint8_t *)pcm, sizeof(pcm));
        break;
      }
      menuqoa_copy = menuqoa_wr;
    }
    bytes = bytes > got * 4 ? bytes - got * 4 : 0;
  }
}

/* Open a .qoa clip: decode the lead-in, then hand it to the fetcher.  The clip must be
   44.1 kHz mono/stereo (what the DAC plays); anything else fails like a bad .pcm. */
static int menuqoa_play(void) {
  if(!menuqoa_rewind()) return 0x02;
  menuqoa_dsh = 0;                       /* a clip's run is stereo */
  menuqoa_wr = menuqoa_copy = menuqoa_len = menuqoa_end = 0;
  menuqoa_hold = 0;
  menuqoa_sealed = 0;
  menuqoa_decode(MENUQOA_STEP * 4);
  if(!menuqoa_wr) return 0x02;           /* not even the first slice decoded */
  if(!(current_features & FEAT_MSU1))
    fpga_set_features(current_features | FEAT_MSU1);
  menuqoa = 1;
  /* The lead-in is decoded and the fetcher started from the idle menu loop (menu_music_work),
     not here: the caller is still busy.  The info screen goes on to scan the game's info folder
     for guides, ~300 ms on a big card with nothing decoding, and a clip already playing ran out
     of its lead there -- past the decoded bytes the fetcher played the previous clip's
     leftovers, then stalled -- while decoding the lead here made every Up/Down that much slower. */
  menuqoa_hold = MENUQOA_START;
  /* the FMV frame clock starts with the clip, as on the .pcm path (the pump leaves it alone
     while the DAC is held): the counter kept the previous clip's position, and the new video
     jumped to that frame and froze until the music caught up */
  menusfx_samples = MSU_DAC_BUFSIZE / 4;
  return 0xA0;
}

/* Cheap enough for every pump site (no SD access): follow the DAC, hold it if the decoder
   fell behind, and re-kick at the end of the run. */
static void menuqoa_pump(void) {
  uint32_t at;
  menuqoa_count();
  if(menuqoa_hold) return;
  at = menuqoa_pos + RUNB(menuqoa_halves * (MSU_DAC_BUFSIZE / 2));   /* the DAC's run byte */
  /* the FMV frame clock: samples fed (2 KB ahead of the DAC, like the .pcm path) into
     the current copy of the clip */
  menusfx_samples = (menuqoa_len ? (at + RUNB(MSU_DAC_BUFSIZE)) % menuqoa_len
                                 : at + RUNB(MSU_DAC_BUFSIZE)) >> (2 - menuqoa_dsh);
  if(menuqoa_sealed && menuqoa_end && at >= menuqoa_end) {
    if(menuqoa_gen) menuqoa_lap += menuqoa_end;
    menuqoa_kick((at - menuqoa_end) % menuqoa_len);
  } else if(menuqoa_kend && at >= menuqoa_kend) {
    menuqoa_kick(at);                     /* past the cap: on from where the DAC is */
  } else if(!MENUQOA_DONE && menuqoa_wr < menuqoa_lap + menuqoa_fetch_max() + RUNB(MENUQOA_GUARD)) {
    menuqoa_hold_set(MENUQOA_STALL, 1);
  }
}

/* One generator step into the ring, unless it would land on audio the DAC has yet to play
   (the 2 KB the fetcher primed ahead of the DAC counted as unplayed). */
static __attribute__((noinline)) void menuqoa_ring_fill(void) {
  int16_t pcm[MENUQOA_STEP * 2];
  uint32_t dac = menuqoa_lap + menuqoa_pos + RUNB(menuqoa_halves * (MSU_DAC_BUFSIZE / 2));
  uint32_t off, n, i;
  if(menuqoa_wr + RUNB(MENUQOA_STEP * 4 + MSU_DAC_BUFSIZE) > dac + menuqoa_end) return;
  n = menuqoa_gen(menuqoa_gen_ctx, pcm, MENUQOA_STEP);
  if(menuqoa_dsh)                     /* mono: keep L */
    for(i = 1; i < n; i++) pcm[i] = pcm[2 * i];
  n = RUNB(n * 4);
  off = menuqoa_wr % menuqoa_end;
  sram_writeblock(pcm, MENUQOA_ADDR + off, n);
  if(off < MENUQOA_PAD)               /* the pad: what the fetcher reads past the ring's end */
    sram_writeblock(pcm, MENUQOA_ADDR + menuqoa_end + off, n < MENUQOA_PAD - off ? n : MENUQOA_PAD - off);
  menuqoa_wr += n;
}

/* Start an endless generator on the fetcher (see menuqoa_gen); ring = PSRAM bytes it may
   use from SRAM_ROM_ADDR, pad included.  Starts held: menu_music_work generates the
   lead-in and kicks the fetcher. */
int menu_music_play_gen(menu_music_gen_fn gen, void *ctx, uint32_t ring) {
  menu_sfx_stop();
  menusfx_close();
  menusfx_noloop = menusfx_eof = menusfx_ended = menusfx_paused = 0;
  menuqoa_dsh = (fpga_sfx_status() & FPGA_SFX_ST_MONO) ? 1 : 0;
  ring -= MENUQOA_PAD;
  ring -= ring % RUNB(MENUQOA_STEP * 4);   /* whole steps: a step never straddles the top */
  menuqoa_gen = gen;
  menuqoa_gen_ctx = ctx;
  menuqoa_wr = menuqoa_copy = menuqoa_lap = menuqoa_pos = menuqoa_halves = 0;
  menuqoa_len = menuqoa_end = ring;
  menuqoa_sealed = 1;
  if(!(current_features & FEAT_MSU1))
    fpga_set_features(current_features | FEAT_MSU1);
  menuqoa = 1;
  menuqoa_hold = MENUQOA_START;
  menusfx_samples = MSU_DAC_BUFSIZE / 4;
  menusfx_loop_forever = 1;
  menusfx_active = 1;
  return 0xA0;
}

/* Bytes the DAC has played since the generator started (its clock). */
uint32_t menu_music_gen_played(void) {
  return menuqoa_gen ? ((menuqoa_lap + menuqoa_pos) << menuqoa_dsh) + menuqoa_halves * (MSU_DAC_BUFSIZE / 2) : 0;
}

/* The decoding half of the QOA clip, from the idle menu loop only (it reads the card):
   one short step per call, so a pending command waits at most one step. */
void menu_music_work(void) {
  if(!menuqoa || !menusfx_active) return;
  if(menuqoa_gen) menuqoa_ring_fill();
  else if(!menuqoa_sealed) menuqoa_decode(MENUQOA_STEP * 4);
  if(menuqoa_hold & MENUQOA_START) {
    if(MENUQOA_DONE || menuqoa_wr >= RUNB(MENUQOA_LEAD)) {
      menuqoa_hold &= ~MENUQOA_START;
      menuqoa_kick(0);
    }
    return;
  }
  if((menuqoa_hold & MENUQOA_STALL)
     && (MENUQOA_DONE || menuqoa_wr >= menuqoa_lap + menuqoa_fetch_max() + RUNB(MENUQOA_RESUME)))
    menuqoa_hold_set(MENUQOA_STALL, 0);
}

void menu_sfx_silence(void) {
  fpga_sfx_play(SRAM_MENU_SFX_ADDR, 0);
}

void menu_sfx_pump(void) {
  uint16_t now;
  if(!menusfx_active) return;
  /* Watchdog: terminate if the effect has outlived any real blip. Normal
     stop is driven by DAC half-buffer toggles below; this guards the case
     where the DAC never toggles, so menusfx_active can't latch forever and
     the menu loop can't busy-spin on a stuck effect. */
  if(!menusfx_loop_forever && time_after(getticks(), menusfx_deadline)) { menu_sfx_stop(); return; }
  if(menuqoa) { menuqoa_pump(); return; }
  now = fpga_status();
  /* refill the half the FPGA just finished reading (DAC_READ_MSB toggles) */
  if((now & MSU_FPGA_STATUS_DAC_READ_MSB) != (menusfx_fpga_prev & MSU_FPGA_STATUS_DAC_READ_MSB)) {
    if(menusfx_eof) {
      /* noloop: the DAC just left the last full half for the one EOF cut short.  Stop it
         here: past the bytes that read delivered, that half still holds audio from two
         halves back, and playing it would sound a few ms of stale track at the very end.
         Cutting the partial tail instead loses under 6 ms of what is nearly always the
         track's closing silence. */
      dac_pause();
      menusfx_active = 0;
      menusfx_eof = 0;
      menusfx_ended = 1;
      return;
    }
    set_dac_addr((now & MSU_FPGA_STATUS_DAC_READ_MSB) ? 0 : (MSU_DAC_BUFSIZE / 2));
    ff_sd_offload = 1; sd_offload_tgt = 1;
    f_read(&menusfx_fil, file_buf, MSU_DAC_BUFSIZE / 2, &menusfx_bytes_read);
    if(menusfx_bytes_read < MSU_DAC_BUFSIZE / 2 && menusfx_noloop) {
      menusfx_eof = 1;                        /* stop at the next toggle, see above */
    } else if(menusfx_bytes_read < MSU_DAC_BUFSIZE / 2) {
      /* EOF: seek to the loop point and fill the rest of this half (the FPGA
         write pointer auto-advanced, so do NOT re-set the DAC address). The
         loop region of an effect is pure silence; after two wraps the
         one-shot is over - stop cleanly mid-silence. */
      UINT br2 = 0;
      /* one-shot ends after two wraps into the (silent) loop region; FMV music instead
         loops the clip forever (until menu_music_stop). */
      if(!menusfx_loop_forever && ++menusfx_wraps >= 2) { DBG_MSU1 printf("sfx: done\n"); menu_sfx_stop(); return; }
      ff_sd_offload = 0; sd_offload = 0;
      ff_sd_offload = 1; sd_offload_tgt = 1;
      f_lseek(&menusfx_fil, MSU_PCM_OFFSET_WAVEDATA + (uint32_t)menusfx_loop_point * 4);
      ff_sd_offload = 1; sd_offload_tgt = 1;
      f_read(&menusfx_fil, file_buf, (MSU_DAC_BUFSIZE / 2) - menusfx_bytes_read, &br2);
      if(!br2) { menu_sfx_stop(); return; }   /* unreadable -> go silent, no hang */
      menusfx_samples = br2 / 4;              /* looped: position = the loop-start samples fed */
    } else {
      menusfx_samples += (MSU_DAC_BUFSIZE / 2) / 4;  /* +256 stereo samples this refill */
    }
  }
  menusfx_fpga_prev = now;
}

/* Looping background music (FMV info-screen audio) via the same DAC. Like menu_sfx_play but
   always reopens (no per-blip name cache), loops the whole clip from the header loop point,
   and sets the no-deadline loop_forever mode (menu_sfx_pump then loops it). Silent + harmless
   if the .pcm is absent/bad. Pumped by the same menu_sfx_pump() in the menu loop. */
int menu_music_play(const char *filename) {
  UINT br = 0;
  uint8_t magic[4];

  menu_sfx_stop();                     /* free the DAC from any blip */
  menusfx_close();                     /* always reopen fresh (music isn't retriggered per blip) */
  menusfx_noloop = 0;                  /* loops unless the caller says otherwise afterwards */
  menusfx_eof = 0;
  menusfx_ended = 0;
  menusfx_paused = 0;
  if(f_open(&menusfx_fil, (const TCHAR*)filename, FA_READ) != FR_OK) return 0x01;
  /* No fast-seek linkmap: a multi-MB clip overflows CLTBL_SIZE anyway, and BUILDING it scans
     the whole cluster chain -- a slow open. Stream sequentially without it; the only seek is
     the cheap once-per-loop rewind to the loop point (near the file start). */
  menusfx_fil.cltbl = 0;
  if(f_read(&menusfx_fil, magic, 4, &br) == FR_OK && br == 4 && !memcmp(magic, "qoaf", 4)) {
    int st;
    menusfx_open = 1;
    menusfx_open_name = 0;
    menusfx_loop_forever = 1;
    st = menuqoa_play();
    if(st != 0xA0) { menusfx_close(); menusfx_loop_forever = 0; return st; }
    menusfx_active = 1;
    DBG_MSU1 printf("music (qoa): %s\n", filename);
    return st;
  }
  if(br != 4 || memcmp(magic, "MSU1", 4)) {
    f_close(&menusfx_fil); return 0x02;   /* not a valid MSU-1 PCM -> silent */
  }
  /* loop point (samples) from the header, clamped; default = loop the whole clip */
  f_lseek(&menusfx_fil, MSU_PCM_OFFSET_LOOPPOINT);
  f_read(&menusfx_fil, &menusfx_loop_point, sizeof(menusfx_loop_point), &br);
  {
    DWORD fsz = f_size(&menusfx_fil);
    uint32_t max_lp = (fsz > MSU_PCM_OFFSET_WAVEDATA)
                        ? (uint32_t)((fsz - MSU_PCM_OFFSET_WAVEDATA) / 4) : 0;
    if(br != sizeof(menusfx_loop_point) || menusfx_loop_point > max_lp)
      menusfx_loop_point = 0;
  }
  menusfx_open = 1;
  menusfx_open_name = 0;               /* no name cache for music */
  menusfx_loop_forever = 1;

  if(!(current_features & FEAT_MSU1))
    fpga_set_features(current_features | FEAT_MSU1);
  dac_pause();
  dac_reset(0);
  set_msu_status(MSU_SNES_STATUS_CLEAR_AUDIO_ERROR | MSU_SNES_STATUS_SET_AUDIO_REPEAT);
  set_dac_addr(0);
  ff_sd_offload = 1; sd_offload_tgt = 1;
  f_lseek(&menusfx_fil, MSU_PCM_OFFSET_WAVEDATA);
  ff_sd_offload = 1; sd_offload_tgt = 1;
  f_read(&menusfx_fil, file_buf, MSU_DAC_BUFSIZE, &menusfx_bytes_read);
  menusfx_samples = MSU_DAC_BUFSIZE / 4;   /* 512 stereo samples primed into the DAC buffer */

  menusfx_fpga_prev = fpga_status();
  menusfx_wraps = 0;
  dac_play();
  menusfx_active = 1;
  DBG_MSU1 printf("music: %s\n", filename);
  return 0xA0;
}

int menu_music_active(void) {
  return menusfx_active && menusfx_loop_forever;
}

/* Claim / release the DAC exclusively.
   There is one DAC and three things want it: the info screen's FMV soundtrack, the nav
   blips, and the menu PCM player.  The FMV paths stop "the clip" from two places that know
   nothing about each other -- the 300 ms idle watchdog (gameinfo_fmv_idle_check) and the
   per-command gate (menucmd_fmv_gate) -- so a consumer that wants to survive them has to be
   able to say so.  The flag lives HERE, in the module that owns the DAC, rather than each
   of those sites asking a different module whether some particular screen is up. */
void menu_music_lock(int locked) {
  menusfx_locked = locked;
}

int menu_music_locked(void) {
  return menusfx_locked;
}

/* Stereo samples fed to the DAC since the loop start = the FMV video frame clock. The menu
   drives the displayed frame off this so the video stays locked to the audio (no drift). */
uint32_t menu_music_samples(void) {
  return menusfx_samples;
}

void menu_music_stop(void) {
  if(menusfx_active && menusfx_loop_forever) menu_sfx_stop();
  menusfx_loop_forever = 0;
  menusfx_noloop = 0;
  menusfx_eof = 0;
  menusfx_ended = 0;
  menusfx_paused = 0;
}

/* Play the open .pcm once instead of looping it at the header's loop point: at EOF the DAC
   stops and menu_music_ended() turns true (the menu PCM player then starts the next track
   of the folder).  Call right after menu_music_play.  A QOA clip ignores it. */
void menu_music_noloop(int on) {
  menusfx_noloop = on ? 1 : 0;
}

int menu_music_ended(void) {
  return menusfx_ended;
}

/* Move the open .pcm to byte `pos` of its body (after the 8-byte header) and refill the
   whole DAC buffer from there, so nothing of the old position is left to play.  Same prime
   as menu_music_play: DAC paused, read pointer reset, 2 KB by SD DMA, then play -- unless
   the clip is paused, which a seek keeps.  The target is frame-aligned and kept a whole
   buffer short of EOF, so the prime never runs into the end with stale bytes behind it
   (the pump then plays that last buffer out and stops or loops as usual).  A clip that
   already ended starts again from there.  Returns 0 when nothing could be done: no .pcm
   open (a QOA clip is decoded ahead into PSRAM and has no file position to move), or the
   read failed -- which stops the clip, like a read error in the pump. */
int menu_music_seek(uint32_t pos) {
  uint32_t fsz, body;
  if(menuqoa || !menusfx_open || !menusfx_loop_forever) return 0;
  fsz = (uint32_t)f_size(&menusfx_fil);
  body = (fsz > MSU_PCM_OFFSET_WAVEDATA) ? fsz - MSU_PCM_OFFSET_WAVEDATA : 0;
  if(body <= MSU_DAC_BUFSIZE) pos = 0;
  else if(pos > body - MSU_DAC_BUFSIZE) pos = body - MSU_DAC_BUFSIZE;
  pos &= ~(uint32_t)3;
  fpga_sfx_disable();                  /* a blip may have taken the DAC after an end */
  dac_pause();
  if(!(current_features & FEAT_MSU1))
    fpga_set_features(current_features | FEAT_MSU1);
  dac_reset(0);
  set_dac_addr(0);
  ff_sd_offload = 1; sd_offload_tgt = 1;
  f_lseek(&menusfx_fil, MSU_PCM_OFFSET_WAVEDATA + pos);
  ff_sd_offload = 1; sd_offload_tgt = 1;
  f_read(&menusfx_fil, file_buf, MSU_DAC_BUFSIZE, &menusfx_bytes_read);
  if(!menusfx_bytes_read) { menu_sfx_stop(); return 0; }
  menusfx_samples = MSU_DAC_BUFSIZE / 4;
  menusfx_fpga_prev = fpga_status();
  menusfx_wraps = 0;
  menusfx_eof = 0;
  menusfx_ended = 0;
  menusfx_active = 1;
  if(!menusfx_paused) dac_play();
  return 1;
}

/* Byte position of the clip's READ head, and its size, for a progress display (the menu
   PCM player, src/pcmplay.c).  The handle is static in here, hence the accessors.
   NOT menu_music_samples(): that counter restarts at every loop wrap (it is the FMV frame
   clock, not a file position).  f_tell runs up to one DAC buffer (2 KB = ~11 ms) ahead of
   what is actually audible, which is invisible on a progress bar. */
uint32_t menu_music_tell(void) {
  /* a QOA clip answers in decoded bytes, behind a pretend .pcm header, so the caller's
     arithmetic does not care which kind is playing */
  if(menuqoa) {
    uint32_t played = menuqoa_pos + menuqoa_halves * (MSU_DAC_BUFSIZE / 2);
    return MSU_PCM_OFFSET_WAVEDATA + (menuqoa_len ? played % menuqoa_len : played);
  }
  return menusfx_open ? (uint32_t)f_tell(&menusfx_fil) : 0;
}

uint32_t menu_music_size(void) {
  if(menuqoa) return MSU_PCM_OFFSET_WAVEDATA
                     + (menuqoa_len ? menuqoa_len : menuqoa_wr + menuqoa_dec.left * 4);
  return menusfx_open ? (uint32_t)f_size(&menusfx_fil) : 0;
}



/* Freeze / unfreeze the DAC read pointer, keeping the file open and menusfx_active set.
   Deliberately NOT menu_sfx_stop(): that disarms the FPGA fetcher and clears the active
   flag, which would drop the open handle and the position.  With the DAC paused the
   DAC_READ_MSB bit stops toggling, so menu_sfx_pump() turns into a no-op on its own. */
void menu_music_pause(int paused) {
  if(!menusfx_active || !menusfx_loop_forever) return;
  if(menuqoa) { menuqoa_hold_set(MENUQOA_PAUSED, paused); return; }
  menusfx_paused = paused ? 1 : 0;
  if(paused) dac_pause(); else dac_play();
}

uint8_t msu_readbyte(uint16_t addr) {
  set_msu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0xF5); /* READ */
  //FPGA_WAIT_RDY();
  uint8_t val = FPGA_RX_BYTE();
  FPGA_DESELECT();
  return val;
}

uint16_t msu_readshort(uint16_t addr) {
  set_msu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0xF5);
  //FPGA_WAIT_RDY();
  uint32_t val = FPGA_RX_BYTE();
  //FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<8);
  FPGA_DESELECT();
  return val;
}

uint32_t msu_readlong(uint16_t addr) {
  set_msu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0xF5);
  //FPGA_WAIT_RDY();
  uint32_t val = FPGA_RX_BYTE();
  //FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<8);
  //FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<16);
  //FPGA_WAIT_RDY();
  val |= ((uint32_t)FPGA_RX_BYTE()<<24);
  FPGA_DESELECT();
  return val;
}

void msu_readlongblock(uint32_t* buf, uint16_t addr, uint16_t count) {
  set_msu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0xF5);
  uint16_t i=0;
  while(i<count) {
    //FPGA_WAIT_RDY();
    uint32_t val = (uint32_t)FPGA_RX_BYTE()<<24;
    //FPGA_WAIT_RDY();
    val |= ((uint32_t)FPGA_RX_BYTE()<<16);
    //FPGA_WAIT_RDY();
    val |= ((uint32_t)FPGA_RX_BYTE()<<8);
    //FPGA_WAIT_RDY();
    val |= FPGA_RX_BYTE();
    buf[i++] = val;
  }
  FPGA_DESELECT();
}

uint16_t msu_readblock(void* buf, uint16_t addr, uint16_t size) {
  uint16_t count=size;
  uint8_t* tgt = buf;
  set_msu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0xF5);   /* READ */
  while(count--) {
    //FPGA_WAIT_RDY();
    *(tgt++) = FPGA_RX_BYTE();
  }
  FPGA_DESELECT();
  return size;
}

uint16_t msu_readstrn(void* buf, uint16_t addr, uint16_t size) {
  uint16_t elemcount = 0;
  uint16_t count = size;
  uint8_t* tgt = buf;
  set_msu_addr(addr);
  FPGA_SELECT();
  FPGA_TX_BYTE(0xF5);   /* READ */
  while(count--) {
    //FPGA_WAIT_RDY();
    if(!(*(tgt++) = FPGA_RX_BYTE())) break;
    elemcount++;
  }
  tgt--;
  if(*tgt) *tgt = 0;
  FPGA_DESELECT();
  return elemcount;
}
