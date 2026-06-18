#ifndef MSU1_H
#define MSU1_H

#include <stdint.h>

#ifdef DEBUG_MSU1
#define DBG_MSU1
#else
#define DBG_MSU1 while(0)
#endif

#define MSU_FPGA_STATUS_SD_DMA_BUSY  (0x8000)
#define MSU_FPGA_STATUS_DAC_READ_MSB (0x4000)
#define MSU_FPGA_STATUS_MSU_READ_MSB (0x2000)
#define MSU_FPGA_STATUS_AUDIO_START  (0x40)
#define MSU_FPGA_STATUS_DATA_START   (0x20)
#define MSU_FPGA_STATUS_CTRL_START   (0x01)

#define MSU_FPGA_STATUS_CTRL_RESUME_FLAG_BIT (0x8)
#define MSU_FPGA_STATUS_CTRL_REPEAT_FLAG_BIT (0x4)
#define MSU_FPGA_STATUS_CTRL_PLAY_FLAG_BIT   (0x2)

#define MSU_INT_STATUS_SET_CTRL_PENDING    (0x0001)
#define MSU_SNES_STATUS_SET_AUDIO_PLAY     (0x0002)
#define MSU_SNES_STATUS_SET_AUDIO_REPEAT   (0x0004)
#define MSU_SNES_STATUS_SET_AUDIO_ERROR    (0x0008)
#define MSU_SNES_STATUS_SET_DATA_BUSY      (0x0010)
#define MSU_SNES_STATUS_SET_AUDIO_BUSY     (0x0020)
#define MSU_INT_STATUS_CLEAR_CTRL_PENDING  (0x0100)
#define MSU_SNES_STATUS_CLEAR_AUDIO_PLAY   (0x0200)
#define MSU_SNES_STATUS_CLEAR_AUDIO_REPEAT (0x0400)
#define MSU_SNES_STATUS_CLEAR_AUDIO_ERROR  (0x0800)
#define MSU_SNES_STATUS_CLEAR_DATA_BUSY    (0x1000)
#define MSU_SNES_STATUS_CLEAR_AUDIO_BUSY   (0x2000)

#define MSU_DAC_BUFSIZE	(2048)
#define MSU_DATA_BUFSIZE (16384)

#define MSU_PCM_OFFSET_WAVEDATA  (8L)
#define MSU_PCM_OFFSET_LOOPPOINT (4L)

int msu1_check(uint8_t*);
int msu1_loop(void);

/* Menu navigation sound effects via the MSU-1 DAC, one-shot (see msu1.c). */
void menu_sfx_play(const char *filename); /* play one MSU-1 PCM once; silent if absent/bad */
void menu_sfx_pump(void);                 /* top up the DAC; call from menu-owning loops */
void menu_sfx_stop(void);                 /* pause + close (keeps FEAT_MSU1 for instant retrigger) */
void menu_sfx_shutdown(void);             /* stop + drop FEAT_MSU1 (game load / console reset) */
int  menu_sfx_active(void);               /* nonzero while an effect is playing */

/* Looping background music via the same DAC (FMV info-screen audio): like menu_sfx but
   no one-shot deadline -- loops the whole clip from sample 0 until menu_music_stop. Shares
   the DAC, so the caller suppresses nav SFX while it plays (snes.c). Pumped by the same
   menu_sfx_pump(). */
int  menu_music_play(const char *filename); /* 0xA0 playing / 0x01 open-fail / 0x02 bad-magic */
void menu_music_stop(void);
int  menu_music_active(void);             /* nonzero while a looping clip is playing */
uint32_t menu_music_samples(void);        /* DAC samples since loop start (FMV frame clock) */

uint8_t msu_readbyte(uint16_t addr);
uint16_t msu_readshort(uint16_t addr);
uint32_t msu_readlong(uint16_t addr);
uint16_t msu_readblock(void* buf, uint16_t addr, uint16_t size);
uint16_t msu_readstrn(void* buf, uint16_t addr, uint16_t size);
void msu_readlongblock(uint32_t* buf, uint16_t addr, uint16_t count);
	
#endif
