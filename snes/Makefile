IPS = header.ips
# const_lang.o65 is generated from the English base const.a65 + the per-language
# dicts (utils/lang_*.py) by utils/build_const.py -> multilingual const_lang.a65.
O65 = reset.o65 main.o65 font.o65 palette.o65 data.o65 const_lang.o65 const_lang_str.o65 logo.o65 dma.o65 filesel.o65 cover.o65 gameinfo.o65 sfx.o65 pad.o65 time.o65 sysinfo.o65 wifi.o65 spc700.o65 spcplay.o65 menu.o65 menudata.o65 ui.o65 nmihook.o65 common.o65 savestate.o65 cheatoverlay.o65 near.o65 patch.o65 cheatmenu.o65 # gfx.o65 # vars.o65

OBJS = $(IPS) $(O65)
# stopgap measure to create both files as long as they are identical...

OUT = menu.bin m3nu.bin
MENU_SIZE = 131072

LOG = $(OBJS:=.log)
MAP = $(IPS:.ips=.map) $(O65:.o65=.map)

DEPDIR := .dep
GENDEPFLAGS = -MM -MP -MF $(DEPDIR)/$(@F).d -MT $(@F)

all: $(OUT) $(MAP)

# Generate the const source from the English base + language dicts. One run
# emits both files: const_lang.a65 (bank $C0: neutral data) and const_lang_str.a65
# (bank $C1: the localized string pool + dispatch tables) -> m3nu.bin spans 2 banks.
const_lang.a65 const_lang_str.a65 &: const.a65 utils/build_const.py utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py utils/lang_fr.py
	python3 utils/build_const.py const.a65 utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py utils/lang_fr.py -o const_lang.a65

smc: menu.bin
	cat menu.bin sd2snes.rom > menu.smc

map: $(MAP)

$(MAP) &: menu.bin $(LOG) link.log
	utils/mkmap.sh $(OBJS)

# generate menu.bin / m3nu.bin / link.log
# The `| tee` masks sneslink's exit code, so a bank overflow / undefined symbol
# would otherwise produce a BROKEN binary with a 0 exit. Grep link.log and fail
# loudly instead (this exact failure shipped an untranslated m3nu.bin once).
$(OUT): $(OBJS)
	sneslink -fsmc -o $@ $^ 2>&1 | tee link.log
	@if grep -qE "ERROR:|still undefined|still not defined" link.log; then \
	  echo "*** menu link FAILED — bank overflow or undefined symbol (see snes/link.log)"; exit 1; fi
	@python3 -c "from pathlib import Path; p=Path('$@'); d=p.read_bytes(); target=$(MENU_SIZE); assert len(d) <= target, f'{p} too large: {len(d)} > {target}'; p.write_bytes(d + b'\xff' * (target - len(d)))"

# in case link.log has been deleted
link.log: $(OBJS)
	sneslink -fsmc -o /dev/null $^ 2>&1 | tee link.log

# Generic rule to create .o65[.log] out from .a65
# The `| tee` masks snescom's exit code, so an assembler error (e.g. an invalid
# addressing mode) would otherwise produce a broken .o65 with a 0 exit and slip
# past the link step (the link guard only greps the LINKER log). Grep the
# assembler log for "Error:" and fail loudly, deleting the bad object so a
# re-make actually retries. (A broken cheat-overlay object shipped once this way.)
%.o65 %.o65.log: %.a65
	cpp $(GENDEPFLAGS) $<
	snescom -J -Wall -o $(@:.log=) $< 2>&1 | tee $(@:.log=).log
	@if grep -q "Error:" $(@:.log=).log; then \
	  echo "*** snescom FAILED on $< (see $(@:.log=).log)"; rm -f $(@:.log=); exit 1; fi

# Generic rule to create .ips out from .a65
%.ips %.ips.log: %.a65
	cpp $(GENDEPFLAGS) $<
	snescom -I -J -Wall -o $(@:.log=) $< 2>&1 | tee $(@:.log=).log
	@if grep -q "Error:" $(@:.log=).log; then \
	  echo "*** snescom FAILED on $< (see $(@:.log=).log)"; rm -f $(@:.log=); exit 1; fi

clean:
	rm -f -- *~ *.ips *.o65 const_lang.a65 const_lang_str.a65 $(OUT) *.o65.log *.ips.log

-include $(info $(DEPDIR)) $(shell mkdir $(DEPDIR) 2>/dev/null) $(wildcard $(DEPDIR)/*)

.PHONY: all smc map clean
