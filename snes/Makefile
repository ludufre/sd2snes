IPS = header.ips
# const_lang.o65 is generated from the English base const.a65 + the per-language
# dicts (utils/lang_*.py) by utils/build_const.py -> multilingual const_lang.a65.
O65 = reset.o65 main.o65 font.o65 palette.o65 data.o65 const_lang.o65 logo.o65 dma.o65 filesel.o65 cover.o65 pad.o65 time.o65 sysinfo.o65 spc700.o65 spcplay.o65 menu.o65 menudata.o65 ui.o65 nmihook.o65 common.o65 savestate.o65 near.o65 patch.o65 cheatmenu.o65 # gfx.o65 # vars.o65

OBJS = $(IPS) $(O65)
# stopgap measure to create both files as long as they are identical...

OUT = menu.bin m3nu.bin

LOG = $(OBJS:=.log)
MAP = $(IPS:.ips=.map) $(O65:.o65=.map)

DEPDIR := .dep
GENDEPFLAGS = -MM -MP -MF $(DEPDIR)/$(@F).d -MT $(@F)

all: $(OUT) $(MAP)

# Generate the const source from the English base + language dicts.
const_lang.a65: const.a65 utils/build_const.py utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py
	python3 utils/build_const.py const.a65 utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py -o const_lang.a65

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

# in case link.log has been deleted
link.log: $(OBJS)
	sneslink -fsmc -o /dev/null $^ 2>&1 | tee link.log

# Generic rule to create .o65[.log] out from .a65
%.o65 %.o65.log: %.a65
	cpp $(GENDEPFLAGS) $<
	snescom -J -Wall -o $(@:.log=) $< 2>&1 | tee $(@:.log=).log

# Generic rule to create .ips out from .a65
%.ips %.ips.log: %.a65
	cpp $(GENDEPFLAGS) $<
	snescom -I -J -Wall -o $(@:.log=) $< 2>&1 | tee $(@:.log=).log

clean:
	rm -f -- *~ *.ips *.o65 const_lang.a65 $(OUT) *.o65.log *.ips.log

-include $(info $(DEPDIR)) $(shell mkdir $(DEPDIR) 2>/dev/null) $(wildcard $(DEPDIR)/*)

.PHONY: all smc map clean
