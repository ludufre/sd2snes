# Hey Emacs, this is a -*- makefile -*-

#----------------------------------------------------------------------------
# WinAVR Makefile Template written by Eric B. Weddington, Joerg Wunsch, et al.
#
# Released to the Public Domain
#
# Additional material for this makefile was written by:
# Peter Fleury
# Tim Henigan
# Colin O'Flynn
# Reiner Patommel
# Markus Pfaff
# Sander Pool
# Frederik Rouleau
# Carlos Lamas
#
#
# Extensively modified for sd2iec and later adapted for ARM by Ingo Korb
#
# To rebuild project do "make clean" then "make all".
#----------------------------------------------------------------------------

# select programming interface (default: ae-ft2232)
# setting is expanded to filename interface/$INTERFACE.cfg
# available: ae-ft2232 usb-blaster oocdlink usbjtag
export INTERFACE ?= ae-ft2232

# Read configuration file
ifndef CONFIG
$(info ERROR: Please use $(MAKE) CONFIG=... to specify at least one config file.)
$(info Available configs:)
$(foreach cfg,$(wildcard config-*),$(info $(patsubst %,  %,$(cfg))))
$(info )
$(info You may specify a programming interface using INTERFACE=...)
$(info Available INTERFACEs (default/current: $(INTERFACE)):)
$(foreach intf,$(wildcard interface/*),$(info $(patsubst interface/%.cfg,  %,$(intf))))
$(info )
$(error missing CONFIG parameter)
else
 CONFIGSUFFIX = $(CONFIG:config%=%)
endif

# Enable verbose compilation with "make V=1"
ifdef V
 Q :=
 E := @:
else
 Q := @
 E := @echo
endif

# Include the configuration file
include $(CONFIG)

# OpenOCD search path from config
export CONFIG_OPENOCD_SCRIPT_PATH ?= .

# Directory for all generated files
OBJDIR := obj$(CONFIGSUFFIX)
DEPDIR := .dep$(CONFIGSUFFIX)

# Linker script
LINKERSCRIPT = $(CONFIG_LD_SCRIPT)

# Target file name (without extension).
TARGET = $(OBJDIR)/sd2snes

# Environment variables for openocd reuse
export FW_START=$(CONFIG_FW_START)
export FW_FILE=$(OBJDIR)/$(CONFIG_FW_FILE)
export FLASH_SIZE=$(CONFIG_FLASH_SIZE)

# Version
-include VERSION
include version.mk

# List C source files here. (C dependencies are automatically generated.)
SRC  = main.c ff.c ccsbcs.c
SRC += printf.c fileops.c fpga.c fpga_spi.c snes.c smc.c
SRC += memory.c filetypes.c faulthandler.c sort.c crc32.c cic.c patch.c patch_copier.c
SRC += cli.c ymodem.c rle.c zx0.c msu1.c crc16.c
SRC += nesave.c cfg.c cheat.c yaml.c savestate.c sgb.c gbc.c sms.c atari.c nes.c nes_chr.c hwinfo.c gameinfo.c igmenu.c manual.c
# Shared helpers: menu command handlers, file->PSRAM streaming, string/path
# utilities and the YAML sidecar writer.
SRC += menucmd.c psram_io.c strutil.c yamlw.c trainer.c cheatcode.c cheatedit.c scratch.c cjkglyph.c qoa.c
SRC += dirindex.c themereg.c
# Per-game options (the .opt sidecar): today the 1CHIP transient fix per game.
SRC += gameopt.c
# RAM connection test driven from the menu ("Memory test").  Every config: the Mk.II is
# exactly where users need the triage, and it is the board that has no serial console to
# run the standalone diagnostic through.
SRC += memtest.c
# Card modules (module.h): menu features whose code is a file on the card, loaded only while
# their screen is open.  The images (modules/) are built below with every firmware.
SRC += module.c fileman.c
# The SPC7110 RTC-4513 battery backup is a card module (modules/rtc7110/, rtc7110.bin):
# load_rom opens it for SPC7110 games only.

# Sufami Turbo Slot B autosave.  Linked into every config.
SRC += sufami.c

# Xeno Crisis: the soft CPU image loader and the service of the xc cores.  Every config.
SRC += xc.c

SRC += usbdesc.c usbcore.c usbuser.c cdcuser.c usbinterface.c

# C library: newlib-nano (see LDFLAGS), with its malloc-backed strtok replaced.
SRC += strtok.c

# Card overlays (ovl.h): the loader is resident; OVL1_SRC are compiled into the card file
# /sd2snes/ovl1.<img|im3|stm> instead of the firmware (rules at the end of this file).
SRC += ovl.c ovl_stub.c sysloop.c
OVL1_SRC = theme.c sysinfo.c cover.c
# helpers only the System Information screen uses: under LTO they lived inlined in it, and
# left resident they would stay as out-of-line copies (~0.6 KB on the mk2)
OVL1_SRC += sgbbios.c $(CONFIG_ARCH)/sdtacc.c
# the SPC player's load and the random menu-music draw (menu only)
OVL1_SRC += spcload.c
# the info screen's loader and full description (the clip pump and the CRC job stay)
OVL1_SRC += giload.c
# OVL2_SRC go to /sd2snes/ovl2.<img|im3|stm>, loaded in place of ovl1 (one image at a time):
# the patch dialog (scan, list, Slot B picker, the per-ROM patch sidecar) and the patch
# restage of a list launch, the patched-ROM export and a deleted ROM's sidecars, the saves
# screen's actions, the bootleg identification of a game load
OVL2_SRC = patchui.c patchmeta.c patchexp.c savesop.c
# the copy-protected bootleg identification: a game load, after the patch query
OVL2_SRC += bootleg.c
OVL2_RENAME = query_ips_patches stage_patch_from_entry patchmeta_save patch_export_exists \
              patch_export_write patch_export_copy_assets patch_unlink_rom_assets \
              saves_command bootleg_identify_file bootleg_apply
# -Wl,-u list of the firmware symbols the overlay uses (a link prerequisite: defined here)
OVL_ULIST = $(if $(strip $(OVL1_SRC)),$(OBJDIR)/ovl1/ulist)

# Wedge diagnostic instrument (wdiag.h): only a private diagnostic config sets
# GBC_WEDGE_DIAG = y; every shipped config leaves it out.
SRC += $(if $(filter y,$(GBC_WEDGE_DIAG)),wdiag.c)

# List Assembler source files here.
#     Make them always end in a capital .S.  Files ending in a lowercase .s
#     will not be considered source files but generated files (assembler
#     output from the compiler), and will be deleted upon "make clean"!
#     Even though the DOS/Win* filesystem matches both .s and .S the same,
#     it will preserve the spelling of the filenames, and gcc itself does
#     care about how the name is spelled on its command-line.
ASRC = crc.S

# add in platform specific configuration
include $(CONFIG_ARCH)/variables.mk
-include $(CONFIG_ARCH)/debug-variables.mk

# usbcontrol.c usb_hid.c usbhw_lpc.c usbinit.c usbstdreq.c

# Optimization level, can be [0, 1, 2, 3, s].
#     0 = turn off optimization. s = optimize for size.
#     (Note: 3 is not always the best optimization level. See avr-libc FAQ.)
# Use s -mcall-prologues when you really need size...
#OPT = 2
OPT = s

# Debugging format.
DEBUG = dwarf-2

# List any extra directories to look for include files here.
#     Each directory must be seperated by a space.
#     Use forward slashes for directory separators.
#     For a directory that has spaces, enclose it in quotes.
EXTRAINCDIRS =

# Compiler flag to set the C Standard level.
#     c89   = "ANSI" C
#     gnu89 = c89 plus GCC extensions
#     c99   = ISO C99 standard (not yet fully implemented)
#     gnu99 = c99 plus GCC extensions
CSTANDARD = -std=gnu99

# Place -D or -U options here
CDEFS = -DF_OSC=$(CONFIG_MCU_FOSC)UL
# XC_DEBUG=1: Xeno Crisis soft CPU halt report in /sd2snes/xc_debug.txt (xc.c); off by default
CDEFS += $(if $(filter 1,$(XC_DEBUG)),-DXC_DEBUG)
# card overlay load timing probe (ovl.c), published for USB: make OVL_TIMING=1, never a release
CDEFS += $(if $(filter 1,$(OVL_TIMING)),-DOVL_TIMING)

# Place -I options here
CINCS = -Iinclude -Iinclude/arm

# CPU-specific flags
ifndef CPUFLAGS
  CPUFLAGS := -mthumb -mcpu=cortex-m3
endif

ifndef ARCH
  ARCH := arm-none-eabi
endif

# Define programs and commands.
# CC must be defined here to generate the correct CFLAGS
SHELL = sh
CC = $(ARCH)-gcc
OBJCOPY = $(ARCH)-objcopy
OBJDUMP = $(ARCH)-objdump
SIZE = $(ARCH)-size
AR = $(ARCH)-ar
NM = $(ARCH)-nm
REMOVE = rm -f
COPY = cp
AWK = gawk
BIN2C = ../utils/bin2c

#---------------- Compiler Options ----------------
#  -g*:          generate debugging information
#  -O*:          optimization level
#  -f...:        tuning, see GCC manual and avr-libc documentation
#  -Wall...:     warning level
#  -Wa,...:      tell GCC to pass this to the assembler.
#    -adhlns...: create assembler listing
CFLAGS = -g$(DEBUG)
CFLAGS += $(CDEFS) $(CINCS)
CFLAGS += -O$(OPT)
CFLAGS += $(CPUFLAGS) -nostartfiles
#CFLAGS += -funsigned-char -funsigned-bitfields -fpack-struct -fshort-enums
CFLAGS += -Wall -Wstrict-prototypes -Werror -Wno-strict-aliasing
CFLAGS += -I$(OBJDIR) -I$(CONFIG_ARCH)
CFLAGS += $(patsubst %,-I%,$(EXTRAINCDIRS))
CFLAGS += $(CSTANDARD)
# Sections per function (gc-sections drops the unused ones).  Data stays in one section per
# object: the per-variable sections cost padding and literal-pool reach (~0.5 KB measured),
# and the little data gc could still drop is not worth that.
CFLAGS += -ffunction-sections
# Per-function stack frames: the LPC175x main SRAM leaves only a few KB between .bss and
# the stack.  Under LTO the frames are those of the final code, written at link time as
# $(OBJDIR)/*.ltrans.su (the per-object .su files only show the pre-LTO view).
CFLAGS += -fstack-usage

# Link-Time Optimization, every config: ~4 KB on each board (the mk2 would not fit without
# it).  Under -flto the per-C assembler listing (-Wa,-adhlns) must be OFF: the compile emits
# GIMPLE (no asm) and at link $< is an object, so the LTO codegen step would write the
# listing over a .o; `make lss` gives the disassembly of the final image instead.
# (-flto also needs `used` on the .fwhdr placeholder in main.c so it survives whole-program
# GC; KEEP() in the linker script protects only against --gc-sections.  The FPGA_* helpers
# that are functions are noinline on purpose: LTO would inline them back.)
CFLAGS += -flto

# Size-first code generation outside the hot paths: these turn off optimizations that, under
# -Os, still trade bytes for speed (loop-invariant motion, jump threading, sibling calls,
# scheduling, ...) -- 1-1.5 KB of flash per board.  The files that move data byte by byte
# between the SD card, the FPGA/PSRAM, USB and the DAC keep the plain -Os code: there the
# hoisted invariants are register addresses inside per-byte loops, and the transfer rates
# (ROM load, file copy, MSU-1 / QOA streaming) depend on them.
SIZE_CFLAGS = -fno-move-loop-invariants -fno-tree-loop-im -fno-thread-jumps -fno-caller-saves \
  -fno-schedule-insns2 -fno-tree-dominator-opts -fno-optimize-sibling-calls -fno-tree-copy-prop \
  --param max-inline-insns-auto=5
HOT_SRC = fpga_spi.c memory.c psram_io.c msu1.c usbinterface.c ff.c crc32.c patch.c patch_copier.c \
  sort.c qoa.c $(CONFIG_ARCH)/sdnative.c $(CONFIG_ARCH)/spi.c $(CONFIG_ARCH)/usbhw.c
CFLAGS += $(SIZE_CFLAGS)

#---------------- Assembler Options ----------------
#  -Wa,...:   tell GCC to pass this to the assembler.
#  -ahlms:    create listing
#  -gstabs:   have the assembler create line number information; note that
#             for use in COFF files, additional information about filenames
#             and function names needs to be present in the assembler source
#             files -- see avr-libc docs [FIXME: not yet described there]
ASFLAGS = $(CPUFLAGS) -Wa,-adhlns=$(OBJDIR)/$(<:.S=.lst),-gstabs -I$(OBJDIR)

#---------------- Linker Options ----------------
#  -Wl,...:     tell GCC to pass this to linker.
#    -Map:      create map file
#    --cref:    add cross reference to  map file
LDFLAGS = -Wl,-Map=$(TARGET).map,--cref
LDFLAGS += -T$(LINKERSCRIPT)
LDFLAGS += -Wl,--gc-sections
# Print flash/ram/ahbram usage per link (the mk2 flash is the budget to watch).
LDFLAGS += -Wl,--print-memory-usage
# Input sections sorted by alignment: less padding between them.
LDFLAGS += -Wl,--sort-section=alignment -Wl,--sort-common
# newlib-nano: the full newlib's routines are built for speed and its reentrancy block
# (_impure_data + the stdio FILEs) costs ~500 B of RAM; nano saves ~4 KB of flash.  Its
# strtok is replaced (strtok.c), and memcpy/memmove/memset are taken from the full newlib
# of the same multilib (FASTMEM_OBJ): nano's copy a byte at a time, and they move every
# USB packet and SD/PSRAM buffer.
LDFLAGS += --specs=nano.specs

# OpenOCD options
# -s <path>:      set additional script search path
# -f openocd.cfg: load common script
OPENOCDFLAGS = -s $(CONFIG_OPENOCD_SCRIPT_PATH) -f openocd.cfg

#============================================================================

# De-dupe the list of C source files
CSRC := $(sort $(SRC))

# Define all object files.
OBJ := $(patsubst %,$(OBJDIR)/%,$(CSRC:.c=.o) $(ASRC:.S=.o))

# The full newlib's word-at-a-time memcpy/memmove/memset, linked ahead of newlib-nano's
LIBC_FULL := $(shell $(CC) $(CPUFLAGS) -print-file-name=libc.a)
FASTMEM_OBJ := $(addprefix $(OBJDIR)/libc_full/libc_a-,memcpy.o memmove.o memset.o)

# The hot paths keep the plain -Os code (see SIZE_CFLAGS).
$(patsubst %.c,$(OBJDIR)/%.o,$(HOT_SRC)): SIZE_CFLAGS :=

# Define all listing files.
LST := $(patsubst %,$(OBJDIR)/%,$(CSRC:.c=.lst) $(ASRC:.S=.lst))

# Generate list of obj dirs
OBJDIRS := $(sort $(dir $(OBJ)))

# Compiler flags to generate dependency files.
GENDEPFLAGS = -MMD -MP -MF $(DEPDIR)/$(@F).d

# Combine all necessary flags and optional flags.
# Add target processor to flags.
ALL_CFLAGS = -I. $(CFLAGS) $(GENDEPFLAGS)
ALL_ASFLAGS = -I. -x assembler-with-cpp $(ASFLAGS) $(CDEFS)

# Default target.
all: build

build: elf bin hex xc_soc modules modcheck ovl
	$(E) "  SIZE   $(TARGET).elf"
	$(Q)$(ELFSIZE)|grep -v debug
	cp $(TARGET).bin $(OBJDIR)/$(CONFIG_FW_FILE)

# Xeno Crisis: xc_soc.bin, the soft CPU's replacement bootrom and firmware additions (Cortex-M0+
# code, see xc_soc/README.md).  Built with every firmware so the pair on the card always match;
# the same file serves all three boards.
xc_soc:
	$(Q)$(MAKE) -s -C xc_soc OUT=../$(OBJDIR)
.PHONY: xc_soc

# Card module images (modules/Makefile): one image per module, for every board.
modules:
	$(Q)$(MAKE) -s -C modules OUT=../$(OBJDIR) FOP_SPARE_MIN=$(FOP_SPARE_MIN)
.PHONY: modules

# The module block (module.c) is ptrcache up to MOD_END, so every byte the firmware adds to
# .ahbram comes out of it, and a module that loads but has too little spare left fails at
# its start: fileops.bin / rmdir.bin then answer every op with "the card refused a read or a
# write" (FM_ERR_IO), and nothing in a build said so.  This fails the build instead.
# FOP_SPARE_MIN = the spare fopmod.c needs (FOP_NBUF * FOP_PATH + FOP_BUF_MIN, asserted there).
FOP_SPARE_MIN = 1792
ifeq ($(CONFIG_MK3_STM32),y)
MODCHECK_END = 0x$$($(NM) $(TARGET).elf | awk '$$3 == "__ahbram_end__" { print $$1 }')
else
MODCHECK_END = 0x20080000
endif
modcheck: elf modules
	$(Q)start=$$($(NM) $(TARGET).elf | awk '$$3 == "ptrcache" { print $$1 }'); \
	  [ -n "$$start" ] || { echo "modcheck: no ptrcache in $(TARGET).elf"; exit 1; }; \
	  block=$$(( $(MODCHECK_END) - 0x$$start )); \
	  for m in fileops rmdir; do \
	    img=$$(( ($$(wc -c < $(OBJDIR)/$$m.bin) + 3) & ~3 )); \
	    left=$$(( block - img - $(FOP_SPARE_MIN) )); \
	    echo "  MODCHECK $$m.bin: $$img of a $$block B block, $$left B over the spare it needs"; \
	    [ $$left -ge 0 ] || { echo "modcheck: $$m.bin does not fit the module block"; exit 1; }; \
	  done
.PHONY: modcheck

elf: $(TARGET).elf
bin: $(TARGET).bin
hex: $(TARGET).hex
eep: $(TARGET).eep
lss: $(TARGET).lss
sym: $(TARGET).sym

program: build
	utils/lpcchksum $(TARGET).bin
	openocd $(OPENOCDFLAGS) -f flash.cfg

debug: build
	openocd $(OPENOCDFLAGS)

reset:
	openocd $(OPENOCDFLAGS) -f reset.cfg

# Display size of file.
ELFSIZE = $(SIZE) -A $(TARGET).elf

# Generate version.h
.PRECIOUS : $(OBJDIR)/version.h
$(OBJDIR)/version.h: .ARG_VERSION | $(OBJDIR)
	$(E) "  VERSION $(CONFIG_VERSION)"
	$(Q)printf "#ifndef __VERSION_H\n"\
"#define __VERSION_H\n"\
"#define CONFIG_VERSION \"$(CONFIG_VERSION)\"\n"\
"#endif" > $(OBJDIR)/version.h

# Generate autoconf.h from config
.PRECIOUS : $(OBJDIR)/autoconf.h
$(OBJDIR)/autoconf.h: $(CONFIG) | $(OBJDIR)
	$(E) "  CONF2H $(CONFIG)"
	$(Q)$(AWK) -f conf2h.awk $(CONFIG) > $(OBJDIR)/autoconf.h

# Embedded images re-packed as ZX0 (zx0.c): the boot bitstream (mk3/mk3-stm32) and the SNES
# boot program.  Both start from the RLE images the tree has always had (fpga_mini.bi3,
# snesboot.h), expanded by utils/rled exactly as the firmware's old RLE reader did, so the
# FPGA and the SNES receive the same bytes as before.  The decoder's window is ptrcache
# (8 KB), so the compressor must not reach further back than that.
ZX0_WINDOW = 8192
ZX0_PACK = utils/zx0c -f -w $(ZX0_WINDOW)
utils/rled utils/bootimg utils/zx0c:
	$(Q)$(MAKE) -s -C utils $(@F)

.PRECIOUS : $(OBJDIR)/cfgware.h
ifeq ($(CONFIG_MK2),y)
# mk2 loads fpga_mini.bit from the card (fpga.c does not include this)
$(OBJDIR)/cfgware.h: $(CONFIG_CFGWARE) | $(OBJDIR) $(OBJDIR)/autoconf.h
	$(E) "  BIN2C $(CONFIG_CFGWARE)"
	$(Q)$(BIN2C) "$(CONFIG_CFGWARE)" cfgware > $(OBJDIR)/cfgware.h
else
$(OBJDIR)/cfgware.h: $(CONFIG_CFGWARE) utils/rled utils/zx0c | $(OBJDIR) $(OBJDIR)/autoconf.h
	$(E) "  ZX0    $(CONFIG_CFGWARE)"
	$(Q)utils/rled "$(CONFIG_CFGWARE)" $(OBJDIR)/cfgware.raw
	$(Q)$(ZX0_PACK) $(OBJDIR)/cfgware.raw $(OBJDIR)/cfgware.zx0 >/dev/null
	$(Q)$(BIN2C) $(OBJDIR)/cfgware.zx0 cfgware > $(OBJDIR)/cfgware.h
endif

.PRECIOUS : $(OBJDIR)/bootzx0.h
$(OBJDIR)/bootzx0.h: snesboot.h utils/bootimg utils/rled utils/zx0c | $(OBJDIR)
	$(E) "  ZX0    snesboot.h"
	$(Q)utils/bootimg $(OBJDIR)/bootrle.rle
	$(Q)utils/rled $(OBJDIR)/bootrle.rle $(OBJDIR)/bootrle.raw
	$(Q)$(ZX0_PACK) $(OBJDIR)/bootrle.raw $(OBJDIR)/bootrle.zx0 >/dev/null
ifeq ($(CONFIG_MK2),y)
# mk2: the program is the card file /sd2snes/bootscr.img (next to fpga_mini.bit); the
# firmware only knows its size and CRC
	$(Q)cp $(OBJDIR)/bootrle.zx0 $(OBJDIR)/bootscr.img
	$(Q)python3 -c 'import sys, zlib; d = open(sys.argv[1], "rb").read(); \
	  print("#define BOOTSCR_SIZE %dU\n#define BOOTSCR_CRC 0x%08xUL" % (len(d), zlib.crc32(d) & 0xffffffff))' \
	  $(OBJDIR)/bootscr.img > $(OBJDIR)/bootzx0.h
else
	$(Q)$(BIN2C) $(OBJDIR)/bootrle.zx0 bootzx0 > $(OBJDIR)/bootzx0.h
endif

# Create final output files from ELF output file.
# --gap-fill 0xff is important to match the padding values as they end up
# in physical flash; CRC calculation is wrong otherwise.
$(TARGET)-intermediate.bin: $(TARGET)-intermediate.elf
	$(E) "  BINTMP $@"
	$(Q)$(OBJCOPY) --gap-fill 0xff -O binary -R .fwhdr $< $@

$(TARGET).bin: $(OBJDIR)/fw.hdr $(TARGET)-intermediate.bin
	$(E) "  BIN    $@"
	$(Q)cat $^ > $@

$(OBJDIR)/%.hex: $(OBJDIR)/%.elf
	$(E) "  HEX    $@"
	$(Q)$(OBJCOPY) -O ihex $< $@

# Create extended listing file from ELF output file.
$(OBJDIR)/%.lss: $(OBJDIR)/%.elf
	$(E) "  LSS    $<"
	$(Q)$(OBJDUMP) -h -S $< > $@

# Create a symbol table from ELF output file.
$(OBJDIR)/%.sym: $(OBJDIR)/%.elf
	$(E) "  SYM    $<"
	$(E)$(NM) -n $< > $@

# The full newlib's mem* routines, out of its archive (see FASTMEM_OBJ)
$(FASTMEM_OBJ): $(LIBC_FULL) | $(OBJDIRS)
	$(Q)mkdir -p $(OBJDIR)/libc_full
	$(Q)cd $(OBJDIR)/libc_full && $(AR) x $(LIBC_FULL) $(notdir $@)

# Link: create ELF output file from object files.
.SECONDARY : $(TARGET)-intermediate.elf
.PRECIOUS : $(OBJ)
$(TARGET)-intermediate.elf: .EXTRA_PREREQS = $(LINKERSCRIPT)
$(TARGET)-intermediate.elf : $(OBJ) $(FASTMEM_OBJ) $(OVL_ULIST)
	$(E) "  LINK   $@"
	$(Q)$(CC) $(ALL_CFLAGS) $(OBJ) $(FASTMEM_OBJ) $(if $(OVL_ULIST),@$(OVL_ULIST)) --output $@ $(LDFLAGS)

# create final ELF file with embedded firmware header
.SECONDARY : $(TARGET).elf
$(TARGET).elf: $(TARGET)-intermediate.elf $(OBJDIR)/fw.hdr
	$(E) "  ELF    $@"
	$(Q)$(OBJCOPY) --update-section .fwhdr=$(OBJDIR)/fw.hdr $< $@

# firmware header file generated by genhdr
$(OBJDIR)/fw.hdr: $(TARGET)-intermediate.bin
	$(E) "  GENHDR $@"
	$(Q)utils/genhdr -s $(CONFIG_FW_MAGIC) -h $(CONFIG_FW_HEADERSIZE) -o $@ $<

$(OBJDIR)/$(CONFIG_FW_FILE): $(TARGET).elf
	cat $^ > $(OBJDIR)/$(CONFIG_FW_FILE)

# Compile: create object files from C source files.
$(OBJDIR)/%.o : %.c | $(OBJDIRS) $(OBJDIR)/autoconf.h $(OBJDIR)/cfgware.h $(OBJDIR)/bootzx0.h $(OBJDIR)/version.h
	$(E) "  CC     $<"
	$(Q)$(CC) -c $(ALL_CFLAGS) $< -o $@

# Compile: create assembler files from C source files.
$(OBJDIR)/%.s : %.c | $(OBJDIR) $(OBJDIR)/autoconf.h $(OBJDIR)/cfgware.h $(OBJDIR)/bootzx0.h $(OBJDIR)/version.h
	$(CC) -S $(ALL_CFLAGS) $< -o $@

# Assemble: create object files from assembler source files.
$(OBJDIR)/%.o : %.S | $(OBJDIRS) $(OBJDIR)/autoconf.h
	$(E) "  AS     $<"
	$(Q)$(CC) -c $(ALL_ASFLAGS) $< -o $@

# Create preprocessed source for use in sending a bug report.
$(OBJDIR)/%.i : %.c | $(OBJDIR) $(OBJDIR)/autoconf.h $(OBJDIR)/cfgware.h $(OBJDIR)/bootzx0.h $(OBJDIR)/version.h
	$(CC) -E -mmcu=$(MCU) -I. $(CFLAGS) $< -o $@

# Create the output directory
$(OBJDIRS) :
	$(E) "  MKDIR  $(OBJDIRS)"
	$(Q)mkdir -p $(OBJDIRS)

# Target: clean project.
clean: begin clean_list end

clean_list :
	$(E) "  CLEAN"
	$(Q)$(REMOVE) $(TARGET).hex
	$(Q)$(REMOVE) $(TARGET).bin
	$(Q)$(REMOVE) $(TARGET).elf
	$(Q)$(REMOVE) $(TARGET)-intermediate.bin
	$(Q)$(REMOVE) $(TARGET)-intermediate.elf
	$(Q)$(REMOVE) $(TARGET).map
	$(Q)$(REMOVE) $(TARGET).sym
	$(Q)$(REMOVE) $(TARGET).lss
	$(Q)$(REMOVE) $(OBJ)
	$(Q)$(REMOVE) $(OBJDIR)/autoconf.h
	$(Q)$(REMOVE) $(OBJDIR)/cfgware.h $(OBJDIR)/cfgware.raw $(OBJDIR)/cfgware.zx0
	$(Q)$(REMOVE) $(OBJDIR)/bootzx0.h $(OBJDIR)/bootrle.rle $(OBJDIR)/bootrle.raw $(OBJDIR)/bootrle.zx0 $(OBJDIR)/bootscr.img
	$(Q)$(REMOVE) $(OBJDIR)/version.h
	$(Q)$(REMOVE) $(OBJDIR)/${CONFIG_FW_FILE}
	$(Q)$(REMOVE) $(OBJDIR)/fw.hdr
	$(Q)$(REMOVE) -r $(OBJDIR)/xc_soc_build $(OBJDIR)/xc_soc.bin $(OBJDIR)/xc_fw.bin $(OBJDIR)/xc_bootrom.bin \
	  $(OBJDIR)/modules_build $(OBJDIR)/nsfplay.bin $(OBJDIR)/memtest.bin $(OBJDIR)/pcmplay.bin $(OBJDIR)/rtc7110.bin
	$(Q)$(REMOVE) $(OBJDIR)/.DS_Store
	$(Q)$(REMOVE) -r $(OBJDIR)/libc_full
	$(Q)$(REMOVE) -r $(OBJDIR)/ovl1 $(OBJDIR)/ovl1.elf $(OBJDIR)/ovl1.map $(OBJDIR)/ovl1.raw $(OBJDIR)/ovl1.nm $(OBJDIR)/$(OVL1_FILE)
	$(Q)$(REMOVE) -r $(OBJDIR)/ovl2 $(OBJDIR)/ovl2.elf $(OBJDIR)/ovl2.map $(OBJDIR)/ovl2.raw $(OBJDIR)/ovl2.nm $(OBJDIR)/$(OVL2_FILE)
	$(Q)$(REMOVE) $(LST)
	$(Q)$(REMOVE) $(CSRC:.c=.s)
	$(Q)$(REMOVE) $(CSRC:.c=.d)
	$(Q)$(REMOVE) $(DEPDIR)/*
# -fstack-usage drops a .su next to every .o, and the mk2 LTO link adds its own
# obj-mk*/*.ltrans.su.  Sweep the whole object tree rather than $(OBJ:.o=.su), so a
# .su left behind by a deleted source file is not read back as a current frame.
	$(Q)$(REMOVE) $(addsuffix *.su,$(OBJDIRS))
	-$(Q)rmdir --ignore-fail-on-non-empty -p $(OBJDIRS)

# Include the dependency files.
-include $(info $(DEPDIR)) $(shell mkdir $(DEPDIR) 2>/dev/null) $(wildcard $(DEPDIR)/*)

# Listing of phony targets.
.PHONY : all begin finish end sizebefore sizeafter \
build elf hex lss sym clean clean_list

# ---- card overlays (ovl.h) --------------------------------------------------
# An overlay is compiled with the firmware's own flags minus LTO (no -fpic either: it runs
# at a fixed address, ptrcache) and linked against the firmware ELF with --just-symbols, so
# it calls resident functions and reads resident globals directly.  Its public functions are
# renamed OVL_<name> so the resident veneers (ovl_stub.c) keep the real names.  Firmware and
# overlay come out of the same make; the overlay header carries the firmware image CRC
# (fw.hdr + 12), and the loader refuses an image from any other build.
OVL_SFX_mk2 = img
OVL_SFX_mk3 = im3
OVL_SFX_mk3-stm32 = stm
OVL_SFX = $(OVL_SFX_$(patsubst config-%,%,$(notdir $(CONFIG))))
OVL1_FILE = ovl1.$(OVL_SFX)
OVL2_FILE = ovl2.$(OVL_SFX)
ifneq ($(strip $(OVL1_SRC)),)
OVL1_RENAME = theme_apply theme_font_edges_stale theme_font_edges theme_font_file font_select \
              theme_clear theme_pick theme_select sysinfo_open load_cover load_cover_path \
              sysinfo_pass load_spc menu_spc_pick_random gameinfo_load gameinfo_desc_full \
              theme_music_load theme_info
OVL_RENAME = $(OVL1_RENAME) $(OVL2_RENAME)
# bootleg_scan stays resident (ovl_stub.c): the overlay's own definition is renamed away.
# Every overlay sees every rename: an overlay calling another overlay's entry fails to link.
OVL_CFLAGS = $(filter-out -flto,$(ALL_CFLAGS)) $(foreach f,$(OVL_RENAME),-D$(f)=OVL_$(f)) \
             -Dbootleg_scan=ovl_unused_bootleg_scan
OVL_N = 1 $(if $(strip $(OVL2_SRC)),2)
# spcload.c streams 64 KB to the FPGA a byte at a time: the plain -Os code memory.c had
$(OBJDIR)/ovl1/spcload.o: SIZE_CFLAGS :=
# One overlay: $(1) = its number, $(2) = where its .bss starts inside the resident ovl_bss
# array (each overlay keeps its own slice: it survives the other one's loads), $(3) = the
# slice's size in bytes
define OVL_RULES
OVL$(1)_OBJ = $$(patsubst %.c,$$(OBJDIR)/ovl$(1)/%.o,$$(OVL$(1)_SRC)) $$(OBJDIR)/ovl$(1)/ovl_hdr$(1).o
$$(OBJDIR)/ovl$(1)/%.o: %.c | $$(OBJDIRS) $$(OBJDIR)/autoconf.h $$(OBJDIR)/version.h
	@mkdir -p $$(@D)
	$$(E) "  CC     $$< (ovl$(1))"
	$$(Q)$$(CC) $$(OVL_CFLAGS) -MF $$(DEPDIR)/ovl$(1)-$$(@F).d -c $$< -o $$@
$$(OBJDIR)/ovl$(1)/ovl_hdr$(1).o: ovl/ovl_hdr$(1).c | $$(OBJDIRS) $$(OBJDIR)/autoconf.h
	@mkdir -p $$(@D)
	$$(Q)$$(CC) $$(OVL_CFLAGS) -MF $$(DEPDIR)/ovl$(1)-$$(@F).d -c $$< -o $$@
$$(OBJDIR)/ovl$(1)/imports.txt: $$(OVL$(1)_OBJ)
	$$(Q)$$(NM) -u $$(OVL$(1)_OBJ) | awk 'NF==2{print $$$$2}' | sort -u > $$(OBJDIR)/ovl$(1)/undef.txt
	$$(Q)$$(NM) -g --defined-only $$(OVL$(1)_OBJ) | awk 'NF==3{print $$$$3}' | sort -u > $$(OBJDIR)/ovl$(1)/def.txt
	$$(Q)comm -23 $$(OBJDIR)/ovl$(1)/undef.txt $$(OBJDIR)/ovl$(1)/def.txt > $$@
$$(OBJDIR)/ovl$(1).elf: $$(TARGET).elf $$(OVL$(1)_OBJ) ovl/ovl.ld
	$$(E) "  LINK   $$@"
	$$(Q)t=$$$$($$(NM) $$(TARGET).elf | awk '$$$$3=="ptrcache"{print $$$$1}'); \
	  b=$$$$($$(NM) $$(TARGET).elf | awk '$$$$3=="ovl_bss"{print $$$$1}'); \
	  [ -n "$$$$t" ] && [ -n "$$$$b" ] || { echo "ovl: ptrcache/ovl_bss missing from $$(TARGET).elf"; exit 1; }; \
	  b=$$$$(printf %x $$$$(( 0x$$$$b + $(2) ))); \
	  $$(CC) $$(CPUFLAGS) -nostdlib -T ovl/ovl.ld -Wl,--section-start=.text=0x$$$$t -Wl,--section-start=.bss=0x$$$$b \
	    -Wl,--defsym=__ovl_bss_max=$(3) \
	    -Wl,--just-symbols=$$(TARGET).elf -Wl,--gc-sections -Wl,--no-undefined -Wl,--no-warn-rwx-segments \
	    -Wl,-Map=$$(OBJDIR)/ovl$(1).map $$(OVL$(1)_OBJ) -lgcc -o $$@
$$(OBJDIR)/$$(OVL$(1)_FILE): $$(OBJDIR)/ovl$(1).elf $$(OBJDIR)/fw.hdr ovl/ovlpack.py ovl/ovlentries.py
	$$(Q)$$(OBJCOPY) -O binary -j .text $$< $$(OBJDIR)/ovl$(1).raw
	$$(Q)$$(NM) -S $$< > $$(OBJDIR)/ovl$(1).nm
	$$(Q)python3 ovl/ovlentries.py $$(OBJDIR)/ovl$(1).raw $$(OBJDIR)/ovl$(1).nm $(1) ovl_stub.c $$(OVL$(1)_RENAME)
	$$(Q)python3 ovl/ovlpack.py $$(OBJDIR)/ovl$(1).raw $$(OBJDIR)/fw.hdr $$@
	@echo "  OVL$(1)   $$@ ($$$$(wc -c < $$@) bytes)"
# Image + its AHB scratch must leave the loader's FIL room at the end of the block.
ovlcheck$(1): $$(OBJDIR)/$$(OVL$(1)_FILE) elf
	$$(Q)e=$$$$($$(NM) $$(OBJDIR)/ovl$(1).elf | awk '$$$$3=="__ovl_scratch_end"{print $$$$1}'); \
	  room=$$$$(( $$(MODCHECK_END) - $$(OVL_FIL_RESERVE) - 0x$$$$e )); \
	  echo "  OVLCHECK $$(OVL$(1)_FILE): image+scratch end 0x$$$$e, $$$$room B before the loader's FIL"; \
	  [ $$$$room -ge 0 ] || { echo "ovl: $$(OVL$(1)_FILE) does not fit the module block"; exit 1; }
.PHONY: ovlcheck$(1)
endef
# the .bss slices come from ovl.h (OVL1_BSS / OVL2_BSS), which also sizes ovl_bss
OVL1_BSS := $(shell awk '$$2=="OVL1_BSS"{print $$3}' ovl.h)
OVL2_BSS := $(shell awk '$$2=="OVL2_BSS"{print $$3}' ovl.h)
$(if $(OVL1_BSS),,$(error ovl: no OVL1_BSS in ovl.h))
$(if $(OVL2_BSS),,$(error ovl: no OVL2_BSS in ovl.h))
$(eval $(call OVL_RULES,1,0,$(OVL1_BSS)))
$(if $(strip $(OVL2_SRC)),$(eval $(call OVL_RULES,2,$(OVL1_BSS),$(OVL2_BSS))))
# The firmware symbols the overlays use: kept global (and out of LTO's reach) with -u
$(OVL_ULIST): $(foreach n,$(OVL_N),$(OBJDIR)/ovl$(n)/imports.txt)
	$(Q)sort -u $^ > $(OBJDIR)/ovl1/imports_all.txt
	$(Q)(cat $(OBJDIR)/ovl1/imports_all.txt; echo ovl_bss) | sed 's/^/-Wl,-u,/' > $@
# The link check (ovlreach.py), on the imports of every overlay and every veneer.
ovl: $(foreach n,$(OVL_N),ovlcheck$(n))
	$(Q)$(OBJDUMP) -d $(TARGET).elf > $(OBJDIR)/ovl1/fw.dis
	$(Q)python3 ovl/ovlreach.py $(TARGET).elf $(OBJDIR)/ovl1/fw.dis $(OBJDIR)/ovl1/imports_all.txt $(OVL_RENAME) \
	  > $(OBJDIR)/ovl1/reach.txt || { cat $(OBJDIR)/ovl1/reach.txt; exit 1; }
	$(Q)grep -v 'ptrcache users' $(OBJDIR)/ovl1/reach.txt | sed 's/^/  /'
else
ovl:
endif
OVL_FIL_RESERVE = 576
.PHONY: ovl
