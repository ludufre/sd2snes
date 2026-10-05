CORE = bootleg

# Variant of the base core for the copy-protected unlicensed bootlegs: the base core source
# with BOOTLEG defined (the Verilog Macros of this .xise, the VERILOG_MACRO of this
# main.qsf), plus this directory's bootleg.v. BOOTLEG adds the protection unit, the CMD
# 0xef chip feature word and the KOF98 ROM bank remap to the base files, and builds them
# with a LoROM-only address decoder (see sd2snes_base/address.v). mk2 AND mk3. main.ucf is
# generated (common.mk) as ucf.extra, the SYSTEM_JITTER the mk2 fit is signed against,
# followed by the base core's main.ucf.
VSRC_DIR = ../sd2snes_base

VSRC = address.v cheat.v clk_test.v ctx.v dac.v dcm.v dma.v bsx.v rtc.v srtc.v main.v mcu_cmd.v msu.v sd_dma.v spi.v sfxdma.v
VSRC_LOCAL = bootleg.v
VHSRC =

COMMON_IP = dac_buf msu_databuf snescmd_buf

HEADER = ../sd2snes_base/config.vh

XIL_IP =
XIL_IPCORE_DIR = ../sd2snes_base/ip/mk2

INT_IP = pll
INT_IP_DIR = ../sd2snes_base/ip/mk3

include ../settings.mk
include ../common.mk
