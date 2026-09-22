CORE = st0018

# Dedicated Seta ST018 (ARMv3 coprocessor) core, derived from sd2snes_st0011.
#   - st018.v / st018_cpu.v replace the uPD96050 (upd77c25*.v, datram, datrom).
#   - No MSU-1, no audio DAC (as in the ST011 core).
#   - No ctx.v / dma.v: savestate-only, and savestate.c never enables
#     savestates on this core. Needed for the mk2 XC3S400 LUT budget.
# The ARM firmware (st018.rom) lives in the Bus 2 SRAM, loaded by the MCU.

VSRC = address.v cheat.v clk_test.v dcm.v main.v mcu_cmd.v sd_dma.v spi.v \
       st018.v st018_cpu.v
VHSRC =

COMMON_IP = snescmd_buf

HEADER = config.vh

XIL_IP =
XIL_IPCORE_DIR = ip/mk2

INT_IP = pll

include ../settings.mk
include ../common.mk
