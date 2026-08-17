CORE = a26

VSRC = a26.v a26_audio.v a26_core.v a26_tia.v a26_video.v address.v arlet/ALU.v arlet/cpu.v bsx.v cheat.v clk_test.v ctx.v dac.v dcm.v dma.v main.v mcu_cmd.v msu.v rtc.v sd_dma.v spi.v srtc.v
# a26_palette.vh is a GENERATED include (gen_palette_vh.py, lockstep with the
# a26sim palette) -- a dependency of a26_video.v, not a source of its own
VHSRC = a26_palette.vh

COMMON_IP = dac_buf msu_databuf snescmd_buf

HEADER = config.vh

XIL_IP =
XIL_IPCORE_DIR = ip/mk2

INT_IP = pll

include ../settings.mk
include ../common.mk
