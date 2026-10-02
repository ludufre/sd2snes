CORE = basex

# mk2-only variant of the base core: the same source with BASE_EXT defined (this
# project's Verilog Macros), i.e. with the Sufami Turbo map, the Gamars window and the BS
# Memory Pack slot in the address decoder. See address.v.
VSRC_DIR = ../sd2snes_base

VSRC = address.v cheat.v clk_test.v ctx.v dac.v dcm.v dma.v bsx.v rtc.v srtc.v main.v mcu_cmd.v msu.v sd_dma.v spi.v sfxdma.v
VHSRC =

COMMON_IP = dac_buf msu_databuf snescmd_buf

HEADER = config.vh

XIL_IP =
XIL_IPCORE_DIR = ../sd2snes_base/ip/mk2

INT_IP = pll

include ../settings.mk
include ../common.mk
