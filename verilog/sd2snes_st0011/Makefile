CORE = st0011

# Dedicated ST011 (uPD96050) core, with MSU-1.

VSRC = address.v cheat.v clk_test.v dac.v dcm.v dma.v msu.v upd77c25.v \
       upd77c25_extpgm.v main.v mcu_cmd.v sd_dma.v spi.v
VHSRC =

COMMON_IP = dac_buf msu_databuf snescmd_buf upd77c25_datram upd77c25_datrom

HEADER = config.vh

XIL_IP =
XIL_IPCORE_DIR = ip/mk2

INT_IP = pll

include ../settings.mk
include ../common.mk
