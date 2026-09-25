# Makefile -- builds gbc_snes.bin (the SNES-side player for the experimental
# Game Boy Color FPGA core) from gbc_snes.asm.
#
# TOOLCHAIN NOTE: this one is assembled with **asar**, not with the snescom/
# sneslink pair that snes/ (menu) and snes/nes/ (NES renderer) use, matching
# snes/sms/ and snes/a26/. prepare.sh provisions asar on the build server
# (built from source; EL9 has no package for it).
#
# asar emits the LoROM image directly (`lorom` + `org $008000`), so unlike
# snes/nes/ there is no gen_lorom.py slicing step here.
#
# Not part of the menu build (snes/Makefile) -- build.sh calls this explicitly.
# The core and the player ship together: /sd2snes/fpga_gbc.bi3 without
# /sd2snes/gbc_snes.bin aborts every .gb/.gbc load with SUPPLFILE, and a pair
# whose wire versions disagree leaves the player in forced blank on purpose.

ASAR ?= asar
OUT   = ../../misc/gbc_snes.bin
SRC   = gbc_snes.asm

# Offline harness image (see the !GBC_HARNESS block at the top of the .asm):
# the same source with the read-only view banks $E0-$E3 and the $EF write
# window relocated into WRAM, so tests/host/run_gbc_player.sh can execute the
# REAL player under the 65816 interpreter.  Separate file on purpose -- the
# device image must never be the one that got the harness define.
HOUT  = ../../misc/gbc_snes_harness.bin
HSYM  = $(HOUT:.bin=.sym)
MAP   = $(OUT:.bin=.map)
HMAP  = $(HOUT:.bin=.map)

all: $(OUT)

# asar exits non-zero on error, but it also prints errors to stdout while doing so;
# the mkdir keeps a fresh checkout (no misc/) working.
$(OUT): $(SRC)
	@mkdir -p $(@D)
	@rm -f $(OUT)
	$(ASAR) $(SRC) $(OUT)
	@test -s $(OUT) || { echo "*** gbc_snes assembly produced no output"; exit 1; }

# --- symbol maps (see gen_map_asar.py) -------------------------------------
# The .map carries the size + CRC32 of the .bin beside it, so a consumer can
# refuse a desynchronised pair instead of executing garbage.  asar writes the
# symbols during the SAME run that writes the image, which is what makes the
# two match by construction; the recipe therefore reassembles rather than
# trying to derive symbols from an image it did not produce.
#
# Deliberately NOT part of `all`, for the same reason `check` is not: the
# build server does not have to grow a python3 dependency for the DEVICE image
# to build.  `harness` does depend on it -- that image exists only to be run by
# the host test, which cannot work without the map anyway.
map: $(MAP)

$(MAP): $(SRC)
	@mkdir -p $(@D)
	$(ASAR) --symbols=wla --symbols-path=$(OUT:.bin=.sym) $(SRC) $(OUT)
	python3 gen_map_asar.py $(OUT:.bin=.sym) $(OUT) $(MAP)

harness: $(HMAP)

$(HOUT): $(SRC)
	@mkdir -p $(@D)
	@rm -f $(HOUT)
	$(ASAR) -DGBC_HARNESS=1 --symbols=wla --symbols-path=$(HSYM) $(SRC) $(HOUT)
	@test -s $(HOUT) || { echo "*** gbc_snes harness assembly produced no output"; exit 1; }

# The map is written from the .sym the rule above just produced, so it is never
# stale with respect to the image it names.
$(HMAP): $(HOUT) gen_map_asar.py
	python3 gen_map_asar.py $(HSYM) $(HOUT) $(HMAP)

# Structural audit of the assembled image: one 8-bit store site into the $EF
# write window, and no V read that is not the latched 9-bit pair (contract
# sec. 13.4 / 13.12).  Deliberately NOT part of `all`: the build server does not
# have to grow a python3 dependency for the player to build.
check: $(OUT)
	python3 selfcheck.py -v $(OUT)

clean:
	rm -f $(OUT) $(MAP) $(OUT:.bin=.sym) $(HOUT) $(HMAP) $(HSYM)

.PHONY: all map harness check clean
