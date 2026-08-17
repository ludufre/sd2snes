# Makefile -- builds a26_snes.bin (the SNES-side player for the experimental
# Atari 2600 FPGA core) from a26_snes.asm.
#
# TOOLCHAIN NOTE: this one is assembled with **asar**, not with the snescom/
# sneslink pair that snes/ (menu) and snes/nes/ (NES renderer) use, matching
# snes/sms/. prepare.sh provisions asar on the build server (built from source;
# EL9 has no package for it).
#
# asar emits the LoROM image directly (`lorom` + `org $008000`), so unlike
# snes/nes/ there is no gen_lorom.py slicing step here.
#
# Not part of the menu build (snes/Makefile) -- build.sh calls this explicitly.
# The core and the player ship together: /sd2snes/fpga_a26.bi3 without
# /sd2snes/a26_snes.bin aborts every .a26 load with SUPPLFILE.

ASAR ?= asar
OUT   = ../../misc/a26_snes.bin
SRC   = a26_snes.asm

all: $(OUT)

# asar exits non-zero on error, but it also prints errors to stdout while doing so;
# the mkdir keeps a fresh checkout (no misc/) working.
$(OUT): $(SRC)
	@mkdir -p $(@D)
	@rm -f $(OUT)
	$(ASAR) $(SRC) $(OUT)
	@test -s $(OUT) || { echo "*** a26_snes assembly produced no output"; exit 1; }

clean:
	rm -f $(OUT)

.PHONY: all clean
