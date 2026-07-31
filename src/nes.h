/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

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

   nes.h: NES (.nes / iNES) file structures -- Phase 0 loader, mirrors sgb.h.

   Architecture (see NES-PLANO.md / NES-CORE-CONTRACT.md at the workspace root):
   a foreign-console "cartridge" boots the SAME way SGB does -- the MCU swaps the
   filename for a tiny SNES-side LoROM stub (NES_SNES_STUB), reconfigures the FPGA
   to fpga_nes, then streams the *.nes PRG/CHR data straight into PSRAM at the
   fixed offsets the vendored fpganes core expects (see nes_wrap.v's header
   comment): PRG at NES_PSRAM_PRG_ADDR, CHR at NES_PSRAM_CHR_ADDR. mk3-only (no
   fpga_nes.bit exists or is planned for the mk2 Spartan-3 -- see nes_id()). */

#ifndef NES_H
#define NES_H

/* SNES-side stub program the game_handshake boots while the real NES core
   (fpga_nes) grinds on the *.nes image in PSRAM.  Unlike SGB's sgbN_boot.bin/
   sgbN_snes.bin (proprietary Nintendo binaries the user must supply), this is
   entirely fork-authored and versionless -- see utils/gen_nes_snes_stub.py. */
#define NES_SNES_STUB ((const uint8_t*)"/sd2snes/nes_snes.bin")

/* PSRAM layout the vendored NES core (nes_wrap.v) expects -- see its header
   comment and NES-CORE-CONTRACT.md.  Lockstep with nes_wrap.v; do not move
   without updating both sides. */
#define NES_PSRAM_PRG_ADDR   (0x000000L)  /* PRG-ROM, <=1MB */
#define NES_PSRAM_CHR_ADDR   (0x200000L)  /* CHR-ROM/RAM, <=1MB, formato NES NATIVO (core-owned) */
#define NES_PSRAM_CIRAM_ADDR (0x300000L)  /* 2KB nametable RAM (core-owned, MCU never writes it) */
#define NES_PSRAM_WRAM_ADDR  (0x380000L)  /* 2KB CPU work RAM (core-owned) */
#define NES_PSRAM_CARTRAM_ADDR (0x3C0000L) /* 8KB PRG-RAM window (core-owned; no save support yet) */

/* CHR pre-convertida p/ SNES no load (Fase 1c) -- ver src/nes_chr.h pro
   contrato byte-a-byte da conversao e NES-CORE-CONTRACT.md Sec. 10.5 pro
   mapa completo.  0x400000 (breadcrumb, abaixo) fica ENTRE 0x3FFFFF (fim
   do espaco de 22 bits do core) e estas regioes -- nao move.  Tamanho ==
   CHR NES (2bpp) / 2x CHR NES (4bpp); CHR max 1MB => janelas de ate 1MB/2MB,
   ambas MUITO antes de 0x880000 (stub SNES-side no Bus-2 SRAM, endereco
   MCU_ADDR diferente -- nunca colide).  CHR-RAM (chr=0 no header): estas
   regioes NAO sao escritas no load (nao ha conteudo pra converter ainda;
   ver NES_BREADCRUMB_VERSION 2, campo chr_ram). */
#define NES_PSRAM_CHR_SNES2_ADDR (0x500000L)  /* CHR->SNES 2bpp (BG), tam == CHR NES */
#define NES_PSRAM_CHR_SNES4_ADDR (0x600000L)  /* CHR->SNES 4bpp (OBJ), tam == 2x CHR NES */

/* Verification breadcrumb for the Phase 0 gate (NES-PLANO.md): a small, fixed,
   documented block the orchestrator reads back with usb_read.py (GET
   space=SNES) after a load, to confirm the PSRAM layout + the exact
   mapper_flags word sent to the FPGA without needing video.  Placed well past
   the core's own 22-bit address decode (0x000000-0x3FFFFF, see nes_wrap.v) so
   it can never collide with a PRG/CHR/CIRAM/WRAM/CART-RAM access the core
   itself makes.  Layout documented in NES-CORE-CONTRACT.md -- keep both in
   lockstep. */
#define NES_PSRAM_BREADCRUMB_ADDR (0x400000L)
#define NES_BREADCRUMB_VERSION    (2)  /* v2 (Fase 1c): + campos chr_snes2/4_*, chr_ram, chr_converted */

/* Phase log for the hardware-wedge investigation: a small SD-card file written
   with one f_open/f_write/f_sync/f_close round-trip per milestone of the NES
   load path (PRE_PGM/POST_PGM/PRE_STUB/PRE_STREAM/STREAM_xx%/POST_STREAM/
   POST_CONVERT/POST_CHIPFEAT/DONE + PGM_FAIL/FPGA_TIMEOUT error marks).
   Survives a power-cycle (PSRAM/BSRAM breadcrumbs do not if the FPGA is the
   thing that died), so reading it back after a wedge pins the exact phase.
   NES-load-only (no-op when has_nes==0), cheap, removable later. */
#define NES_DBG_LOG ((const char*)"/sd2snes/nesdbg.log")

/* In-game NES debug snapshot ("NDBG") -- freeze diagnosis (e.g. DK freezing
   on the barrel/Mario collision): while a .nes is in-game, the MCU main loop
   polls the FPGA config-bus group 0x04 (opcode 0xf9 CONFIG_READ via the
   existing fpga_read_config(); nes_wrap.v `bc_r[]`/counter mapping: idx 0/1 =
   6502 PC lo/hi snapshotted at every instruction-fetch start, 2..6 =
   A/X/Y/P/SP, 7 = free-running CPU-cycle counter low byte, 8..13 = bridge
   band counters bytes_last/frames/overruns, each LE16; v3: idx 14/15 = APU
   audio evidence; v3+: idx 16/17 = DAC-side audio evidence) and publishes a
   snapshot at PSRAM 0x400100, read back over USB (usb_read.py GET space=SNES
   @ 0x400100, 24 bytes).  Layout (packed LE, nes_ndbg_t in nes.c -- keep in
   lockstep):
     +0  magic[4]        "NDBG"
     +4  version         NES_NDBG_VERSION
     +5  seq             ++ each publish (proof the publisher is alive)
     +6  pc              (LE16) 6502 PC at last instruction-fetch start
     +8  a   +9 x   +10 y   +11 p   +12 sp
     +13 cyc_lo          free-running cycle counter low byte: advancing
                         between USB samples = CPU is clocked (PC frozen +
                         cyc moving = tight loop/spin; both frozen = dead ce)
     +14 band_bytes_last (LE16) bridge: mailbox bytes of last frame
     +16 band_frames     (LE16) bridge: frames serialized
     +18 band_overruns   (LE16) bridge: frame overruns
     +20 apu_max         (v3) config-bus idx 14: STICKY max-hold of |APU_DAT|
                         (8-bit) -- nonzero once ANY nonzero sample has ever
                         left the core (mute hunt: silicon-side evidence that
                         audio flowed at some point since reset)
     +21 apu_nz_ctr      (v3) config-bus idx 15: rolling counter of nonzero
                         samples -- advancing between USB samples = audio is
                         flowing NOW (vs apu_max = flowed at some point)
     +22 dac_cic_max     (v3+) config-bus idx 16: max-hold of |CIC output| in
                         dac.v, cleared by every load's dac_reset pulse --
                         audio survived the DAC's filter for THIS game (vs
                         apu_max: pre-filter, sticky since core reset)
     +23 dac_lrck_ctr    (v3+) config-bus idx 17: rolling counter of LRCK
                         edges (~2.4 wraps/s); frozen between USB samples =
                         the DAC's own I2S clock is dead
     +24 i2s_dat_max     (v3++) config-bus idx 18: max-hold of
                         |vol_sample_sat| (dbg_dat_max in dac.v -- the exact
                         16-bit word the I2S shifter reloads on every LRCK
                         edge, top 8 bits).  0xFF pinned = saturated/DC
                         output (garbage-in-the-mix signature); 0 = data
                         dies between CIC and shifter; musical value varying
                         + mute at the jack = data IS on the wire, mystery
                         is off-chip (analog)
     +25 nt_wr_ctr       (v3++++) config-bus idx 19: rolling counter of
                         tapped NT-writes -- nametable-loss-in-transition
                         trap ("ORLD" leftover in SMB1 / missing BEST digit
                         in Excitebike): on a caught leftover, compare this
                         counter against the trace's expected writes over
                         the same window
     +26..+27 reserved   always 0
     +28 pal_sum         (v3+++) config-bus idx 22: palette fingerprint
                         (sum mod 256 of the bridge's pal array) --
                         bad-palette-boot trap: compare clean vs dirty boot
                         cheaply, no full dump needed
     +29 pal_wr_ctr      (v3+++) config-bus idx 23: counter of tapped
                         palette writes
   The 22 config-bus reads are NOT atomic vs the running core -- the snapshot
   is only self-consistent when the CPU is stuck (the use case; multi-byte
   fields can tear while running).  0x400100 sits past the NESL breadcrumb
   (0x400000, 52 B) inside the MCU-owned window; the core's 22-bit decode
   (0x000000-0x3FFFFF) can never touch it.
   PHASE GATE (USB reader beware): the PSRAM write is gated on
   band_frames != 0 -- publishing during the renderer's boot window (its CHR
   DMA from the 0x500000/0x600000 windows) collided on the PSRAM bus and
   baked corrupted OBJ tiles into VRAM (main.v operational rule: no
   MCU->PSRAM traffic during CHR DMA).  So the NDBG block only appears/
   updates after the first bridge frame; before that the region holds stale/
   absent data.  16-bit wrap (1 frame ==0 every 65536, ~18min at 60fps)
   skips a single publish -- benign. */
#define NES_PSRAM_NDBG_ADDR (0x400100L)
#define NES_NDBG_VERSION    (3)  /* v3: + apu_max/apu_nz_ctr @ +20/+21 (idx 14/15);
                                    v3+ acrescenta dac_cic_max/dac_lrck_ctr @ +22/+23
                                    (idx 16/17); v3++ acrescenta i2s_dat_max @ +24
                                    (idx 18); v3+++ acrescenta pal_sum/pal_wr_ctr @
                                    +28/+29 (idx 22/23); v3++++ acrescenta nt_wr_ctr
                                    @ +25 (idx 19; +26..+27 reservados = 0).
                                    SEM bump (layout so' cresce/preenche gaps) */

typedef struct __attribute__ ((__packed__)) _nes_header {
  uint8_t magic[4];        /* 0x00: "NES\x1A" */
  uint8_t prg_16k_banks;   /* 0x04: PRG-ROM size, 16KB units (iNES 1.0 low byte) */
  uint8_t chr_8k_banks;    /* 0x05: CHR-ROM size, 8KB units; 0 = CHR-RAM */
  uint8_t flags6;          /* 0x06: mapper low nibble, mirroring, battery, trainer, four-screen */
  uint8_t flags7;          /* 0x07: mapper high nibble, NES 2.0 signature (bits 3:2 == 10b) */
  uint8_t flags8;          /* 0x08: PRG-RAM size (iNES 1.0) / mapper MSB + submapper (NES 2.0) */
  uint8_t flags9;          /* 0x09: TV system (iNES 1.0) / PRG+CHR size MSB (NES 2.0) */
  uint8_t flags10;         /* 0x0A: unofficial (PRG-RAM/bus conflicts); ignored */
  uint8_t padding[5];      /* 0x0B-0x0F: reserved, normally zero */
} nes_header_t;

typedef struct __attribute__ ((__packed__)) _nes_romprops {
  nes_header_t header;
  uint8_t  has_nes;          /* .nes presence + valid iNES magic */
  uint8_t  mapper_id;        /* iNES mapper number (8-bit; classic-iNES derivation, see nes.c) */
  uint8_t  prg_size_class;   /* 3-bit bank-count class -> mapper_flags[10:8] (fpganes GameLoader table) */
  uint8_t  chr_size_class;   /* 3-bit bank-count class -> mapper_flags[13:11] */
  uint8_t  mirror_vertical;  /* iNES flags6 bit0 -> mapper_flags[14] */
  uint8_t  four_screen;      /* iNES flags6 bit3 -- NOT implemented by the pruned mmu.v -> NOIMPL */
  uint8_t  has_battery;      /* iNES flags6 bit1 -- informational only, no PRG-RAM save support yet */
  uint8_t  has_trainer;      /* iNES flags6 bit2 -- 512B trainer skipped before PRG when set */
  uint8_t  has_chr_ram;      /* chr_8k_banks == 0 -> mapper_flags[15] */
  uint8_t  is_nes20;         /* NES 2.0 header (flags7 bits 3:2 == 10b); sizes still read as classic iNES */
  uint8_t  supported;        /* mapper_id in the Phase 0 set {0,1,2,3,7,28} AND !four_screen */
  uint32_t prgsize_bytes;    /* PRG bytes actually streamed to NES_PSRAM_PRG_ADDR */
  uint32_t chrsize_bytes;    /* CHR bytes actually streamed to NES_PSRAM_CHR_ADDR (0 if CHR-RAM) */
  /* Exact 16-bit word written via fpga_set_chipfeat() (opcode 0xef, CHIPFEAT) and
     wired in main.v to nes_wrap's mapper_flags_in[15:0] (mapper_flags[31:16] is
     always 0 -- see the vendored fpganes GameLoader). Bit layout (matches
     NES_Nexys4.v's GameLoader.mapper_flags / mmu.v's MultiMapper `flags` 1:1):
       [7:0]   mapper number
       [10:8]  prg_size class
       [13:11] chr_size class
       [14]    mirroring (0=horizontal, 1=vertical)
       [15]    has_chr_ram
     See NES-CORE-CONTRACT.md for the full derivation + rationale. */
  uint16_t mapper_flags16;
  uint8_t  error;            /* MENU_ERR_NOIMPL on unsupported mapper / four-screen, else MENU_ERR_OK */
  const uint8_t *error_param;
} nes_romprops_t;

void nes_id(nes_romprops_t*, uint8_t *filename);
uint8_t nes_update_file(uint8_t **filename_ref);
uint8_t nes_update_romprops(snes_romprops_t*, uint8_t *nes_filename);
uint32_t nes_load_prg(uint8_t *nes_filename);

/* Wedge-debug phase log (see NES_DBG_LOG above).  nes_dbg_log appends one
   "tag\n" line (open/write/sync/close per call; no-op unless a .nes load is
   in flight).  nes_dbg_post_pgm logs the fpga_pgm outcome (DONE pin + whether
   fpga_config was actually updated -- fpga_pgm returns void and fails
   SILENTLY on file-open error, see fpga.c) and, on failure, latches the NES
   FPGA-error flag so nes_load_prg skips cleanly instead of wedging on the
   first unbounded FPGA_WAIT_RDY.  Called from memory.c's load_rom. */
void nes_dbg_log(const char *tag);
void nes_dbg_post_pgm(const uint8_t *conf);

/* In-game NDBG publisher (see NES_PSRAM_NDBG_ADDR above): called once per
   iteration of the in-game main loop (main.c); no-op unless has_nes.  Fully
   bounded: config-bus reads only wait on the LPC's own SPI FIFO (never the
   MCU_RDY pin) and the PSRAM write goes through the timeout-latched wrapper. */
void nes_dbg_publish(void);

#endif
