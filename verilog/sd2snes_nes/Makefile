CORE = nes

# NOTE: this file list is a mechanical adaptation of sd2snes_sgb/Makefile
# (same VSRC-list-driven common.mk build, see ../common.mk / ../settings.mk).
# sgb.v/sgb_cpu.v/sgb_icd2.v/cheat.v are replaced by the vendored+pruned
# fpganes core (nes.v/cpu.v/MicroCode.v/ppu.v/apu.v/mmu.v/compat.v) plus the
# sd2snes plumbing adapter (nes_wrap.v). Not yet run through the real build
# (`make -C _repo/verilog/sd2snes_nes CONFIG=...`) -- Phase -1's gate is the
# iverilog testbench in tb/, not synthesis. See the Phase -1 report for what
# is unvalidated here (COMMON_IP dropped dec_table/vram/oam/hram/row_buf,
# which were the SGB video/VRAM/OAM/HRAM pipeline, not applicable to a
# CPU-only core).
VSRC = address.v dac.v dcm.v main.v mcu_cmd.v msu.v sd_dma.v spi.v \
       nes_wrap.v nes_bridge.v nes_split_capture.v nes_chrsplit_capture.v nes_chrwin_capture.v nes.v cpu.v MicroCode.v ppu.v apu.v mmu.v compat.v
VHSRC =

HEADER = config.vh

COMMON_IP = dac_buf msu_databuf snescmd_buf

XIL_IP =
XIL_IPCORE_DIR = ip/mk2

INT_IP = pll

include ../settings.mk
include ../common.mk
