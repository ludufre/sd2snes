CORE = atari

# Atari 2600 for the Mk.II (XC3S400): the STorm2600 core by AndinhoNFS
# (GPL-2.0-or-later, github.com/AndinhoNFS/STorm2600-sd2snes-Chip-mk2), as a
# variant of sd2snes_base.  With ATARI defined (this project's Verilog Macros)
# sd2snes_base/main.v puts a26_top where the FPGA copier was and a26_i2s where
# the DAC was; the modules this mode does not use are port-compatible stubs.
# The SNES side is snes/atari (cc65), the firmware side src/atari.c (CONFIG_MK2).
# The Mk.III runs a different core, sd2snes_a26.
VSRC_DIR = ../sd2snes_base
VSRC = address.v clk_test.v dcm.v main.v mcu_cmd.v sd_dma.v spi.v
VHSRC =

VSRC_LOCAL = a26_top.v a26_i2s.v a26_core.v a26_regs.v a26_mem.v a26_vidwr.v a26_tdict.v \
             m6507.v riot6532.v tia.v tia_hc.v tia_obj.v tia_audio.v \
             stub_sfxdma.v stub_srtc.v stub_rtc.v stub_msu.v stub_ctx.v stub_bsx.v stub_cheat.v

COMMON_IP = snescmd_buf

HEADER = config.vh

XIL_IP =
XIL_IPCORE_DIR = ../sd2snes_base/ip/mk2

INT_IP = pll

include ../settings.mk
include ../common.mk
