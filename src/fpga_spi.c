/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2012 Maximilian Rehkopf <otakon@gmx.net>
   uC firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   fpga_spi.h: functions for SPI ctrl, SRAM interfacing and feature configuration
*/
/*

        SPI commands

        cmd        param                function
   =============================================
        0t        bbhhll        set address to 0xbbhhll
                                t = target
                                target: 0 = RAM
                                        1 = MSU Audio buffer
                                        2 = MSU Data buffer
                                targets 1 & 2 only require 2 address bytes to
                                be written.

        10        bbhhll        set SNES input address mask to 0xbbhhll
        20        bbhhll        set SRAM address mask to 0xbbhhll

        3m        -             set mapper to m
                                0=HiROM, 1=LoROM, 2=ExHiROM, 6=SF96, 7=Menu

        4s        -             trigger SD DMA (512b from SD to memory)
                                s: Bit 2 = partial, Bit 1:0 = target
                                target: see above

        60        xsssyeee      set SD DMA partial transfer parameters
                                x: 0 = read from sector start (skip until
                                       start offset reached)
                                   8 = assume mid-sector position and read
                                       immediately
                                sss = start offset (msb first)
                                y: 0 = skip rest of SD sector
                                   8 = stop mid-sector if end offset reached
                                eee = end offset (msb first)

        8p        -             read (RAM only)
                                p: 0 = no increment after read
                                   8 = increment after read

        9p        {xx}*         write xx
                                p: i-tt
                                tt = target (see above)
                                i = increment (see above)

        E0        ssrr          set MSU-1 status register (=FPGA status [7:0])
                                ss = bits to set in status register (1=set)
                                rr = bits to reset in status register (1=reset)

        E1        -             pause DAC
        E2        -             resume/play DAC
        E3        hhll          set DAC playback pointer
        E4        hhll          set MSU read pointer

        E5        tt{7}         set RTC (SPC7110 format + 1000s of year,
                                         nibbles packed)
                                eg 0x20111210094816 is 2011-12-10, 9:48:16
        E6        ssrr          set/reset BS-X status register [7:0]
        E7        -             reset SRTC state
        E8        -             reset DSP program and data ROM write pointers
        E9        hhmmllxxxx    write+incr. DSP program ROM (xxxx=dummy writes)
        EA        hhllxxxx      write+incr. DSP data ROM (xxxx=dummy writes)
        EB        rr            control DSP reset
        EC        vv            set DAC volume boost
                                  vv[2:0]: 0 = 1x
                                           1 = 1.5x
                                           2 = 2x
                                           3 = 3x
                                           4 = 4x
        ED        -             set feature enable bits (see below)
        EE        -             set $213f override value (0=NTSC, 1=PAL)
        EF        aaaa          set DSP core features (see below)
        EF        aa            set SGB core features (see below)
        F0        -             receive test token (to see if FPGA is alive)
        F1        -             receive status (16bit, MSB first), see below

        F2        -             get MSU data address (32bit, MSB first)
        F3        -             get MSU audio track no. (16bit, MSB first)
        F4        -             get MSU volume (8bit)

        FE        -             get SNES master clock frequency (32bit, MSB first)
                                measured 1x/sec
        FF        {xx}*         echo (returns the sent data in the next byte)

        FPGA status word:
        bit        function
   ==========================================================================
         15        SD DMA busy (0=idle, 1=busy)
         14        DAC read pointer MSB
         13        MSU read pointer MSB
         12        reserved (0)
         11        reserved (0)
         10        reserved (0)
          9        reserved (0)
          8        reserved (0)
          7        reserved (0)
          6        reserved (0)
          5        MSU1 Audio request from SNES
          4        MSU1 Data request from SNES
          3        MSU1 Audio control status: 0=no resume, 1=resume
          2        MSU1 Audio control status: 0=no repeat, 1=repeat
          1        MSU1 Audio control status: 0=pause, 1=play
          0        MSU1 Audio control request

        FPGA feature enable bits:
        bit        function
   ==========================================================================
        12         enable Satellaview base unit emulation
        11         enable DMA1 registers
      10-7         $2100 brightness limit (4 bits)
         6         enable $2100 DAC fix for 1CHIP
         5         enable permanent snescmd unlock (during load handshake)
         4         enable $213F override
         3         enable MSU1 registers
         2         enable SRTC registers
         1         enable ST0010 mapping
         0         enable DSPx mapping

        DSP core features (DSP1-4 / ST0010)
   ==========================================================================
      15-4         -
       3-0         number of additional clocks per DSP cycle (7+x)

        DSP core features (Cx4)
        bit
   ==========================================================================
      15-1         -
         0         speed (0: more faithful; 1: no waitstates)





*/

#include "bits.h"
#include "fpga.h"
#include "config.h"
#include "uart.h"
#include "spi.h"
#include "fpga_spi.h"
#include "timer.h"
#include "sdnative.h"

void fpga_spi_init(void) {
  spi_init();
  GPIO_MODE_IN(FPGA_MCU_RDY_REG, FPGA_MCU_RDY_BIT);
}

void set_msu_addr(uint16_t address) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SETADDR | FPGA_TGT_MSUBUF);
  FPGA_TX_BYTE((address >> 8) & 0xff);
  FPGA_TX_BYTE((address) & 0xff);
  FPGA_DESELECT();
}

void set_dac_addr(uint16_t address) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SETADDR | FPGA_TGT_DACBUF);
  FPGA_TX_BYTE((address >> 8) & 0xff);
  FPGA_TX_BYTE((address) & 0xff);
  FPGA_DESELECT();
}

void set_mcu_addr(uint32_t address) {
  FPGA_SELECT();
  // wait for prior operations to clear out
  FPGA_WAIT_RDY();
  FPGA_TX_BYTE(FPGA_CMD_SETADDR | FPGA_TGT_MEM);
  FPGA_TX_BYTE((address >> 16) & 0xff);
  FPGA_TX_BYTE((address >> 8) & 0xff);
  FPGA_TX_BYTE((address) & 0xff);
  FPGA_DESELECT();
}

void set_saveram_base(uint8_t mask) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SETRAMBASE);
  FPGA_TX_BYTE((mask) & 0xff);
  FPGA_DESELECT();
}

void set_saveram_mask(uint32_t mask) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SETRAMMASK);
  FPGA_TX_BYTE((mask >> 16) & 0xff);
  FPGA_TX_BYTE((mask >> 8) & 0xff);
  FPGA_TX_BYTE((mask) & 0xff);
  FPGA_DESELECT();
}

void set_rom_mask(uint32_t mask) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SETROMMASK);
  FPGA_TX_BYTE((mask >> 16) & 0xff);
  FPGA_TX_BYTE((mask >> 8) & 0xff);
  FPGA_TX_BYTE((mask) & 0xff);
  FPGA_DESELECT();
}

void set_mapper(uint8_t val) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SETMAPPER(val));
  FPGA_DESELECT();
}

uint8_t fpga_test() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_TEST);
  uint8_t result = FPGA_RX_BYTE();
  FPGA_DESELECT();
  return result;
}

uint16_t fpga_status() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_GETSTATUS);
  uint16_t result = (FPGA_RX_BYTE()) << 8;
  result |= FPGA_RX_BYTE();
  FPGA_DESELECT();
  return result;
}

void fpga_set_sddma_range(uint16_t start, uint16_t end) {
  DBG_SD_OFFLOAD printf("FPGA set partial range %u - %u\n", start, end);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SDDMA_RANGE);
  FPGA_TX_BYTE(start >> 8);
  FPGA_TX_BYTE(start & 0xff);
  FPGA_TX_BYTE(end >> 8);
  FPGA_TX_BYTE(end & 0xff);
  FPGA_DESELECT();
}

void fpga_sddma(uint8_t tgt, uint8_t partial) {
  GPIO_MODE_IN(SD_CLKREG, SD_CLKBIT);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SDDMA | (tgt & 3) | (partial ? FPGA_SDDMA_PARTIAL : 0));
  FPGA_TX_BYTE(0x00); /* dummy for falling DMA_EN edge */
  FPGA_DESELECT();
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_GETSTATUS);
  DBG_SD_OFFLOAD printf("FPGA DMA tgt=%u partial=%u, wait for completion...", tgt, partial);
  while(FPGA_RX_BYTE() & 0x80) {
    FPGA_RX_BYTE(); /* eat the 2nd status byte */
  }
  GPIO_MODE_OUT(SD_CLKREG, SD_CLKBIT);
  DBG_SD_OFFLOAD printf("...complete\n");
  FPGA_DESELECT();
}

void dac_play() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DACPLAY);
  FPGA_TX_BYTE(0x00); /* latch reset */
  FPGA_DESELECT();
}

void dac_pause() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DACPAUSE);
  FPGA_TX_BYTE(0x00); /* latch reset */
  FPGA_DESELECT();
}

void dac_reset(uint16_t address) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DACSETPTR);
  FPGA_TX_BYTE((address >> 8) & 0xff); /* address hi */
  FPGA_TX_BYTE(address & 0xff);      /* address lo */
  FPGA_DESELECT();
}

/* Arm the autonomous SFX fetcher (sfxdma.v) with the PSRAM base + body length of a
   preloaded PCM effect and kick playback.  The FPGA then streams it into dac_buf on
   its own -- no MCU refill, so it survives any menu-side SD blocking. */
void fpga_sfx_play(uint32_t base, uint32_t len) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SFX_PLAY);       /* 0xfb */
  FPGA_TX_BYTE((base >> 16) & 0xff);
  FPGA_TX_BYTE((base >> 8) & 0xff);
  FPGA_TX_BYTE((base) & 0xff);
  FPGA_TX_BYTE((len >> 16) & 0xff);
  FPGA_TX_BYTE((len >> 8) & 0xff);
  FPGA_TX_BYTE((len) & 0xff);            /* last byte -> kick */
  FPGA_DESELECT();
}

/* Abort the SFX fetcher and hand the dac_buf write port back to the MCU SD-DMA path
   (in-game MSU-1 / FMV music).  Called at game load. */
void fpga_sfx_disable(void) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SFX_DISABLE);    /* 0xfc */
  FPGA_TX_BYTE(0x00);                    /* one param byte -> disable strobe */
  FPGA_DESELECT();
}

void msu_reset(uint16_t address) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_MSUSETPTR);
  FPGA_TX_BYTE((address >> 8) & 0xff); /* address hi */
  FPGA_TX_BYTE(address & 0xff);      /* address lo */
  FPGA_DESELECT();
}

void set_msu_status(uint16_t status) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_MSUSETBITS);
  FPGA_TX_BYTE(status & 0xff);
  FPGA_TX_BYTE((status >> 8) & 0xff);
  FPGA_TX_BYTE(0x00); /* latch reset */
  FPGA_DESELECT();
}

uint16_t get_msu_track() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_MSUGETTRACK);
  uint16_t result = (FPGA_RX_BYTE()) << 8;
  result |= FPGA_RX_BYTE();
  FPGA_DESELECT();
  return result;
}

uint32_t get_msu_pointer() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_MSUGETSCADDR);
  uint32_t result = (FPGA_RX_BYTE()) << 24;
  result |= (FPGA_RX_BYTE()) << 16;
  result |= (FPGA_RX_BYTE()) <<  8;
  result |= (FPGA_RX_BYTE()) <<  0;
  FPGA_DESELECT();
  return result;
}

uint32_t get_msu_offset() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_MSUGETADDR);
  uint32_t result = (FPGA_RX_BYTE()) << 24;
  result |= (FPGA_RX_BYTE()) << 16;
  result |= (FPGA_RX_BYTE()) << 8;
  result |= (FPGA_RX_BYTE());
  FPGA_DESELECT();
  return result;
}

uint32_t get_snes_sysclk() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_GETSYSCLK);
  FPGA_TX_BYTE(0x00); /* dummy (copy current sysclk count to register) */
  uint32_t result = (FPGA_RX_BYTE()) << 24;
  result |= (FPGA_RX_BYTE()) << 16;
  result |= (FPGA_RX_BYTE()) << 8;
  result |= (FPGA_RX_BYTE());
  FPGA_DESELECT();
  return result;
}

void set_bsx_regs(uint8_t set, uint8_t reset) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_BSXSETBITS);
  FPGA_TX_BYTE(set);
  FPGA_TX_BYTE(reset);
  FPGA_TX_BYTE(0x00); /* latch reset */
  FPGA_DESELECT();
}

uint64_t get_fpga_time() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_RTCGET);
  FPGA_TX_BYTE(0x00); /* dummy (copy current ftime to register) */
  uint64_t result = ((uint64_t)FPGA_RX_BYTE()) << 48;
  result |= ((uint64_t)FPGA_RX_BYTE()) << 40;
  result |= ((uint64_t)FPGA_RX_BYTE()) << 32;
  result |= ((uint64_t)FPGA_RX_BYTE()) << 24;
  result |= ((uint64_t)FPGA_RX_BYTE()) << 16;
  result |= ((uint64_t)FPGA_RX_BYTE()) << 8;
  result |= ((uint64_t)FPGA_RX_BYTE());
  FPGA_DESELECT();
  return result;
}

void set_fpga_time(uint64_t time) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_RTCSET);
  FPGA_TX_BYTE((time >> 48) & 0xff);
  FPGA_TX_BYTE((time >> 40) & 0xff);
  FPGA_TX_BYTE((time >> 32) & 0xff);
  FPGA_TX_BYTE((time >> 24) & 0xff);
  FPGA_TX_BYTE((time >> 16) & 0xff);
  FPGA_TX_BYTE((time >> 8) & 0xff);
  FPGA_TX_BYTE(time & 0xff);
  FPGA_TX_BYTE(0x00);
  FPGA_DESELECT();
}

void fpga_reset_srtc_state() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SRTCRESET);
  FPGA_TX_BYTE(0x00);
  FPGA_TX_BYTE(0x00);
  FPGA_DESELECT();
}

void fpga_reset_dspx_addr() {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DSPRESETPTR);
  FPGA_TX_BYTE(0x00);
  FPGA_TX_BYTE(0x00);
  FPGA_DESELECT();
}

void fpga_write_dspx_pgm(uint32_t data) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DSPWRITEPGM);
  FPGA_TX_BYTE((data >> 16) & 0xff);
  FPGA_TX_BYTE((data >> 8) & 0xff);
  FPGA_TX_BYTE((data) & 0xff);
  FPGA_TX_BYTE(0x00);
  FPGA_TX_BYTE(0x00);
  FPGA_DESELECT();
}

void fpga_write_dspx_dat(uint16_t data) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DSPWRITEDAT);
  FPGA_TX_BYTE((data >> 8) & 0xff);
  FPGA_TX_BYTE((data) & 0xff);
  FPGA_TX_BYTE(0x00);
  FPGA_TX_BYTE(0x00);
  FPGA_DESELECT();
}

void fpga_dspx_reset(uint8_t reset) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DSPRESET);
  FPGA_TX_BYTE(reset);
  FPGA_DESELECT();
}

void fpga_set_dac_boost(uint8_t boost) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DACBOOST);
  FPGA_TX_BYTE(boost);
  FPGA_DESELECT();
}

void fpga_set_features(uint16_t feat) {
  printf("set features: %04x\n", feat);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SETFEATURE);
  FPGA_TX_BYTE((feat >> 8) & 0xff);
  FPGA_TX_BYTE((feat) & 0xff);
  FPGA_DESELECT();
  current_features = feat;
}

void fpga_set_213f(uint8_t data) {
  printf("set 213f: %d\n", data);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SET213F);
  FPGA_TX_BYTE(data);
  FPGA_DESELECT();
}

void fpga_set_snescmd_addr(uint16_t addr) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SNESCMD_SETADDR);
  FPGA_TX_BYTE(addr & 0xff);
  FPGA_TX_BYTE(addr >> 8);
  FPGA_DESELECT();
}

void fpga_write_snescmd(uint8_t data) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SNESCMD_WRITE);
  FPGA_TX_BYTE(data);
  FPGA_TX_BYTE(0x00);
  FPGA_DESELECT();
}

uint8_t fpga_read_snescmd() {
  uint8_t data;
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_SNESCMD_READ);
  data = FPGA_RX_BYTE();
  FPGA_DESELECT();
  return data;
}

void fpga_write_cheat(uint8_t index, uint32_t code) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_CHEAT_WRITE);
  FPGA_TX_BYTE(index);
  FPGA_TX_BYTE(code >> 24);
  FPGA_TX_BYTE((code >> 16) & 0xff);
  FPGA_TX_BYTE((code >> 8) & 0xff);
  FPGA_TX_BYTE(code & 0xff);
  FPGA_DESELECT();
}

/* MCU-driven FPGA copier op: copy len bytes src->dst inside the 24-bit PSRAM space,
   with the SNES held in reset (the arbiter grants the copier full bandwidth, any
   core that carries the copier).  Programs dma_r[0..9] via CMD 0xd4 (last byte =
   opcode COPY + trigger), then polls busy (CMD 0xd5).  Returns 0 on success, 1 if
   busy never cleared within the timeout (core has no copier / wedged) -> caller
   falls back to the byte-by-byte patch. */
static int fpga_copier_op_to(uint32_t src, uint32_t dst, uint32_t len, uint32_t timeout) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DMA_OP);
  FPGA_TX_BYTE((dst >> 16) & 0xff);  /* dma_r[0] dst bank   */
  FPGA_TX_BYTE((src >> 16) & 0xff);  /* dma_r[1] src bank   */
  FPGA_TX_BYTE(src & 0xff);          /* dma_r[2] src[7:0]   */
  FPGA_TX_BYTE((src >> 8) & 0xff);   /* dma_r[3] src[15:8]  */
  FPGA_TX_BYTE(dst & 0xff);          /* dma_r[4] dst[7:0]   */
  FPGA_TX_BYTE((dst >> 8) & 0xff);   /* dma_r[5] dst[15:8]  */
  FPGA_TX_BYTE(len & 0xff);          /* dma_r[6] len[7:0]   */
  FPGA_TX_BYTE((len >> 8) & 0xff);   /* dma_r[7] len[15:8]  */
  FPGA_TX_BYTE((len >> 16) & 0xff);  /* dma_r[8] len[23:16] */
  FPGA_TX_BYTE(0x01);                /* dma_r[9] opcode COPY(0), loop 0, dir 0, trig 1 */
  FPGA_DESELECT();

  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DMA_BUSY);
  while(FPGA_RX_BYTE() & 0x01) {
    if(!--timeout) { FPGA_DESELECT(); return 1; }
  }
  FPGA_DESELECT();
  return 0;
}

int fpga_copier_op(uint32_t src, uint32_t dst, uint32_t len) {
  return fpga_copier_op_to(src, dst, len, 60000000u); /* generous: a 4 MB copy is ~0.3 s of polling */
}

/* Cheap availability probe: same op with a MUCH shorter timeout.  On a core that
   has NO copier the 0xd5 (DMA_BUSY) command is unknown and the MISO can read back
   bit0=1 (phantom busy), so fpga_copier_op's 60M-iteration timeout would spin for
   seconds/tens of seconds -- bad now that the gate defaults ON.  A real copier
   finishes a 4-byte copy in microseconds; 100k SPI iterations (~tens of ms) is a
   huge margin, so the probe fails fast on a core without the copier. */
int fpga_copier_op_probe(uint32_t src, uint32_t dst, uint32_t len) {
  return fpga_copier_op_to(src, dst, len, 100000u);
}

/* Fire a copier op WITHOUT polling busy (#3, 1-trigger queue): the FPGA latches a
   trigger that arrives while it is still copying (op_kick), so small ops stream at
   SPI speed without the per-op busy-poll round-trip.  Use ONLY for small ops (the
   FPGA keeps up); a big op must still poll (fpga_copier_op) or the 1-deep queue
   overruns. */
void fpga_copier_op_nopoll(uint32_t src, uint32_t dst, uint32_t len) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DMA_OP);
  FPGA_TX_BYTE((dst >> 16) & 0xff);
  FPGA_TX_BYTE((src >> 16) & 0xff);
  FPGA_TX_BYTE(src & 0xff);
  FPGA_TX_BYTE((src >> 8) & 0xff);
  FPGA_TX_BYTE(dst & 0xff);
  FPGA_TX_BYTE((dst >> 8) & 0xff);
  FPGA_TX_BYTE(len & 0xff);
  FPGA_TX_BYTE((len >> 8) & 0xff);
  FPGA_TX_BYTE((len >> 16) & 0xff);
  FPGA_TX_BYTE(0x01);
  FPGA_DESELECT();
}

/* Poll the copier status (CMD 0xd5) until idle (bit0=0); returns the last status
   byte -- bit1 = overrun (a queued op was dropped, i.e. the MCU fired faster than
   the 1-deep queue could absorb).  Bounded timeout. */
uint8_t fpga_copier_wait(void) {
  uint32_t timeout = 60000000u;
  uint8_t st = 0;
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_DMA_BUSY);
  while(((st = FPGA_RX_BYTE()) & 0x01)) {
    if(!--timeout) break;
  }
  FPGA_DESELECT();
  return st;
}

void fpga_set_chipfeat(uint16_t feat) {
  printf("chipfeat <= %d\n", feat);
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_CHIPFEAT);
  FPGA_TX_BYTE(feat >> 8);
  FPGA_TX_BYTE(feat & 0xff);
  FPGA_DESELECT();
}

uint8_t fpga_read_config(uint8_t group, uint8_t index) {
  uint8_t data;
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_CONFIG_READ);
  FPGA_TX_BYTE(group);
  FPGA_TX_BYTE(index);
  FPGA_RX_BYTE(); // null read to create delay
  data = FPGA_RX_BYTE();
  FPGA_DESELECT();
  return data;
}

void fpga_write_config(uint8_t group, uint8_t index, uint8_t value, uint8_t invmask) {
  FPGA_SELECT();
  FPGA_TX_BYTE(FPGA_CMD_CONFIG_WRITE);
  FPGA_TX_BYTE(group);
  FPGA_TX_BYTE(index);
  FPGA_TX_BYTE(value);
  FPGA_TX_BYTE(invmask);
  FPGA_TX_BYTE(0x00); // flop reset
  FPGA_DESELECT();
}
