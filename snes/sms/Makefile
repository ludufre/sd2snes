# Makefile -- builds sms_snes.bin (the SNES-side player for the experimental SMS
# FPGA core) from sms_snes.asm.
#
# TOOLCHAIN NOTE: this one is assembled with **asar**, not with the snescom/
# sneslink pair that snes/ (menu) and snes/nes/ (NES renderer) use. The player was
# written against asar syntax (`lda.b`/`sta.l` size suffixes, `org`, `lorom`,
# `!defines`), so it is kept as-is instead of being ported -- porting a player that
# already works on hardware buys nothing. prepare.sh provisions asar on the build
# server (built from source; EL9 has no package for it).
#
# asar emits the LoROM image directly (`lorom` + `org $008000`), so unlike
# snes/nes/ there is no gen_lorom.py slicing step here.
#
# Not part of the menu build (snes/Makefile) -- build.sh calls this explicitly.

ASAR ?= asar
OUT   = ../../misc/sms_snes.bin
SRC   = sms_snes.asm

all: $(OUT)

# asar exits non-zero on error, but it also prints errors to stdout while doing so;
# the mkdir keeps a fresh checkout (no misc/) working.
$(OUT): $(SRC)
	@mkdir -p $(@D)
	@rm -f $(OUT)
	$(ASAR) $(SRC) $(OUT)
	@test -s $(OUT) || { echo "*** sms_snes assembly produced no output"; exit 1; }

clean:
	rm -f $(OUT)

.PHONY: all clean
