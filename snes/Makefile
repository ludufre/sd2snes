IPS = header.ips
# const_lang.o65 is generated from the English base const.a65 + the per-language
# dicts (utils/lang_*.py) by utils/build_const.py -> multilingual const_lang.a65.
# One run emits three objects, in three different banks: const_lang ($C0, neutral
# labels), const_lang_str ($C2, the string pool) and const_lang_tab ($C1, the
# dispatch tables). The tables sit apart from the pool because $C2 is the scarce
# bank and $C1 has tens of KB free. A fourth file, const_lang_cjk ($C1), is the glyph
# sheet of the CJK cache (cjk.o65): the ideographs any translation uses, drawn from
# fonts/misaki_gothic_2nd.hex. The CJK languages (ja, zh: LAST on the command line) do not
# go into the image: each column, with its glyphs, is a lang_<code>.bin that ships in
# /sd2snes/lang/ and that the menu loads into WRAM when the language is selected.
O65 = reset.o65 main.o65 font.o65 palette.o65 data.o65 const_lang.o65 const_lang_str.o65 const_lang_tab.o65 const_lang_cjk.o65 cjk.o65 logo.o65 dma.o65 filesel.o65 cover.o65 gameinfo.o65 sfx.o65 pad.o65 time.o65 sysinfo.o65 sysinfo_render.o65 memtest.o65 pcmplay.o65 spcview.o65 nsfplay.o65 msufolder.o65 diricon.o65 gicrcres.o65 fmres.o65 spc700.o65 spcplay.o65 menu.o65 menudata.o65 contextdata.o65 fileman.o65 const_lang_fm.o65 gameopt.o65 saves.o65 savesgate.o65 gicrc.o65 kvdata.o65 ui.o65 nmihook.o65 hookxlate.o65 mouse.o65 mouselegend.o65 common.o65 savestate.o65 savestate_cx4.o65 cheatoverlay.o65 manhost.o65 near.o65 patch.o65 stdlg.o65 ghlaunch.o65 expwarn.o65 cheatmenu.o65 cheatedit.o65 iris.o65 bootgate.o65 ovlwarn.o65 bootintro.o65 ovlwtext.o65 gimusic.o65 gimenu.o65 lock.o65 kbdcur.o65 onboarding/onboarding_gate.o65 # gfx.o65 # vars.o65

OBJS = $(IPS) $(O65)
# stopgap measure to create both files as long as they are identical...

OUT = menu.bin m3nu.bin
# igmenu.bin: the in-game TAB menu shell, linked SEPARATELY from the menu (org bank $C8,
# loaded by the MCU to PSRAM $C80000 at game-load). NOT part of m3nu.bin.
IGMENU = igmenu.bin
# 4 banks ($C0-$C3). The font and the theme regions stay in $C0, which is what
# THEME_MENU_SIZE in src/theme.c (3 banks) bounds. $C3 is the file manager's bank
# (fileman.a65 + its strings, const_lang_fm): it is loaded with the rest at boot, but the
# Ficha's guide viewer borrows $C3 for its tiles, so nothing outside $C3 may depend on it
# being there -- fm_bank_ok (fmres.a65) checks its magic and the firmware restages it
# (FM_OP_BANK). Before the file manager the menu was 3 banks: $C2 carries the
# i18n string pool (const_lang_str, build_const.py; the dispatch tables that name
# those strings live in $C1, const_lang_tab) and stays RESIDENT in-game
# (igmenu moved to $C8, the dir buffer to $DB), so in-game readers like the cheat
# overlay's (no name) lookup keep working.
MENU_SIZE = 262144

# onboarding.bin: the first-boot tour ROM (sources in onboarding/). A SEPARATE HiROM
# image with its own $C0-$C2 namespace, booted by the firmware IN PLACE OF the menu
# (SNES_CMD_LOAD_ONBOARDING) -- not linked into m3nu.bin. Only the gate that asks
# the question (onboarding/onboarding_gate.o65, bank $C1) is part of the menu, in
# O65 above. The tour reuses the menu's font.o65 (same format) and has its own
# header/reset/engine/ui/const/data, and the menu's palette; its strings come
# from utils/gen_onb_lang.py.
# snescom resolves #include relative to the make CWD (snes/), so tour sources
# include "onboarding/onb_memmap.i65".
ONB_O65 = onboarding/onb_header.ips onboarding/onb_reset.o65 onboarding/onboarding_main.o65 onboarding/onb_ui.o65 \
          onboarding/onboarding_const.o65 onboarding/onb_const_lang.o65 onboarding/onb_data.o65 \
          onboarding/onb_demo_a.o65 onboarding/onb_demo_b.o65 onboarding/onb_demo_c.o65 onboarding/onb_demo_d.o65 \
          onboarding/onb_demo_e.o65 onboarding/onb_demo_f.o65 onboarding/onb_demo_g.o65 onboarding/onb_demo_h.o65 \
          onboarding/onb_demo_i.o65 onboarding/onb_demo_j.o65 onboarding/onb_spc.o65 onboarding/onb_welcome.o65 \
          onboarding/onb_font.o65 palette.o65 spc700.o65 onboarding/onb_const_lang_b.o65 onboarding/onb_const_lang_t.o65 \
          onboarding/onb_cjk.o65 onboarding/onb_cjk_sheets.o65 onboarding/onb_cjk_sheets_zh.o65 \
          onboarding/onb_demomap.o65 onboarding/onb_str_c2.o65 onboarding/onb_str_c3.o65 onboarding/onb_str_c5.o65 onboarding/onb_str_c6.o65 onboarding/onb_str_c7.o65 onboarding/onb_str_c8.o65 onboarding/onb_str_c9.o65 onboarding/onb_str_ca.o65 onboarding/onb_str_cb.o65
ONB_OUT = onboarding.bin
# 12 banks ($C0 code + the menu's palette + half of the string pool, $C1 the string
# tables and the other half, $C2-$CB the demo pictures -- utils/gen_onb_demo.py). The
# image is loaded over the menu's own PSRAM ($C0..$CB), all of it idle while the tour
# runs: $C8 (the igmenu.bin staging) is restaged on every game load, and $C9-$CB (the
# cover and game info staging) are only written when the menu asks for a cover or a
# game info card, which the tour never does. $CC-$CF hold the menu sounds the tour
# plays: the image must stop below them. NB: no inline comment on the next line --
# make would fold it into the value and break the Python pad expr.
ONB_SIZE = 786432

LOG = $(OBJS:=.log)
MAP = $(IPS:.ips=.map) $(O65:.o65=.map)

DEPDIR := .dep
# -I. lets cpp's dep-gen resolve the snes/-relative includes of the sources in
# onboarding/ (cpp otherwise looks next to the SOURCE file; snescom uses the CWD).
GENDEPFLAGS = -MM -MP -MF $(DEPDIR)/$(@F).d -MT $(@F) -I.

all: $(OUT) $(IGMENU) $(ONB_OUT) $(MAP) lang_ja.fnt lang_zh.fnt

# the tour's own string pool, EN + the same 7 languages as the menu, split by language
# over two banks: tables + en/pt/es/it in $C1 (onb_const_lang.a65), de/fr/ru/nl in $C0
# (onb_const_lang_b.a65; gen_onb_lang.py POOL_B_LANGS). The
# answers' value words come from the menu's const.a65 / lang_*.py (menu_text).
# Japanese and Chinese: their tour words in utils/onb_lang_<code>.json, their strings placed with
# every other language's (onb_str_<bank>.a65) and a glyph sheet each (onb_cjk_sheets.a65 = ja + the
# common glyphs, onb_cjk_sheets_zh.a65).
# The tour's copy of the menu font, in the picture bank $C2: the tour's $C1 is full with pool A and
# the Chinese pool. The same data as font.a65 but its .link page and the mouse glyphs (it is read
# bank-agnostically, #^font / @font,x / the _GFXPTR_ bank byte). It must stay in $C0-$C2: the
# firmware remaps a theme's font edges only inside the first THEME_MENU_SIZE bytes (src/theme.c).
# The menu's own font stays in $C1. The copy draws the mouse glyphs for text: one pixel
# further right than the menu's legend glyphs (utils/fontedit.py tour_font).
onboarding/onb_font.a65: font.a65 utils/fontedit.py
	python3 utils/fontedit.py tourfont $< $@

onboarding/onb_const_lang.a65 onboarding/onb_const_lang_b.a65 onboarding/onb_const_lang_t.a65 onboarding/onb_str_c2.a65 onboarding/onb_str_c3.a65 onboarding/onb_str_c5.a65 onboarding/onb_str_c6.a65 onboarding/onb_str_c7.a65 onboarding/onb_str_c8.a65 onboarding/onb_str_c9.a65 onboarding/onb_str_ca.a65 onboarding/onb_str_cb.a65 onboarding/onb_cjk_sheets.a65 onboarding/onb_cjk_sheets_zh.a65 &: utils/gen_onb_lang.py utils/build_const.py const.a65 utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py utils/lang_fr.py utils/lang_it.py utils/lang_ru.py utils/lang_nl.py utils/lang_ja.py utils/lang_zh.py utils/onb_lang_ja.json utils/onb_lang_zh.json fonts/misaki_gothic_2nd.hex fonts/fusion8_zh_hans.hex
	python3 utils/gen_onb_lang.py -o onboarding/onb_const_lang.a65

# Same loud guard as the menu link (the `| tee` masks sneslink's exit code).
$(ONB_OUT): $(ONB_O65)
	sneslink -fsmc -o $@ $^ 2>&1 | tee onb_link.log
	@if grep -qE "ERROR:|still undefined|still not defined" onb_link.log; then \
	  echo "*** onboarding link FAILED -- bank overflow or undefined symbol (see snes/onb_link.log)"; exit 1; fi
	@python3 -c "from pathlib import Path; p=Path('$@'); d=p.read_bytes(); target=$(ONB_SIZE); assert len(d) <= target, f'{p} too large: {len(d)} > {target}'; p.write_bytes(d + b'\xff' * (target - len(d)))"

# Generate the const source from the English base + language dicts. One run emits
# three files: const_lang.a65 (bank $C0: neutral data), const_lang_str.a65 (bank $C2:
# the localized string pool) and const_lang_tab.a65 (bank $C1: the dispatch tables
# resolve_str walks) -> m3nu.bin spans 3 banks.
const_lang.a65 const_lang_str.a65 const_lang_tab.a65 const_lang_cjk.a65 const_lang_fm.a65 lang_ja.bin lang_zh.bin &: const.a65 utils/build_const.py fonts/misaki_gothic_2nd.hex fonts/fusion8_zh_hans.hex utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py utils/lang_fr.py utils/lang_it.py utils/lang_ru.py utils/lang_nl.py utils/lang_ja.py utils/lang_zh.py
	python3 utils/build_const.py const.a65 utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py utils/lang_fr.py utils/lang_it.py utils/lang_ru.py utils/lang_nl.py utils/lang_ja.py utils/lang_zh.py -o const_lang.a65

# The firmware's CJK fonts (src/gameinfo.c draws a game's Japanese/Chinese title and
# description with them): /sd2snes/lang/<code>.fnt.
lang_ja.fnt: fonts/misaki_gothic_2nd.hex utils/gen_cjk_font.py
	python3 utils/gen_cjk_font.py fonts/misaki_gothic_2nd.hex -o lang_ja.fnt
lang_zh.fnt: fonts/fusion8_zh_hans.hex utils/gen_cjk_font.py
	python3 utils/gen_cjk_font.py fonts/fusion8_zh_hans.hex -o lang_zh.fnt

# igmenu_lang.i65: the in-game HELP tab's font-encoded i18n string tables, generated
# from the English base (const.a65) + the SAME language dicts as const_lang, by
# utils/gen_igmenu_lang.py. Included by igmenu.a65 -> linked into igmenu.bin (bank $C2),
# SEPARATE from m3nu.bin (which can't reach it). Explicit prereq of igmenu.o65 so it
# exists before cpp scans igmenu.a65's #include for dependencies.
igmenu_lang.i65: const.a65 utils/gen_igmenu_lang.py utils/build_const.py utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py utils/lang_fr.py utils/lang_it.py utils/lang_ru.py utils/lang_nl.py
	python3 utils/gen_igmenu_lang.py const.a65 utils/lang_ptbr.py utils/lang_es.py utils/lang_de.py utils/lang_fr.py utils/lang_it.py utils/lang_ru.py utils/lang_nl.py -o igmenu_lang.i65

igmenu.o65: igmenu_lang.i65

smc: menu.bin
	cat menu.bin sd2snes.rom > menu.smc

map: $(MAP)

$(MAP) &: menu.bin $(LOG) link.log
	utils/mkmap.sh $(OBJS)
	@python3 utils/hdrcheck.py $(MAP)

# generate menu.bin / m3nu.bin / link.log
# The `| tee` masks sneslink's exit code, so a bank overflow / undefined symbol
# would otherwise produce a BROKEN binary with a 0 exit. Grep link.log and fail
# loudly instead (this exact failure shipped an untranslated m3nu.bin once).
$(OUT): $(OBJS)
	sneslink -fsmc -o $@ $^ 2>&1 | tee link.log
	@if grep -qE "ERROR:|still undefined|still not defined" link.log; then \
	  echo "*** menu link FAILED — bank overflow or undefined symbol (see snes/link.log)"; exit 1; fi
	@python3 -c "from pathlib import Path; p=Path('$@'); d=p.read_bytes(); target=$(MENU_SIZE); assert len(d) <= target, f'{p} too large: {len(d)} > {target}'; p.write_bytes(d + b'\xff' * (target - len(d)))"
	@python3 utils/bankcheck.py $@ $(MENU_SIZE)
	@python3 -c "import sys; d=open('$@','rb').read(); sys.exit(0 if d[0x30000:0x30004]==b'FMB1' else 'bank \$$C3 must start with fileman.a65 (fm_bank_magic, FMB1): an object linked into \$$C3 sits before it in O65')"

# igmenu.bin: separate $C2 link -> HiROM smc -> slice from the "IGMN" magic + patch the
# crc16(body). Same loud guard as the menu (the `| tee` masks sneslink's exit code, so a
# bank overflow / undefined symbol would otherwise ship a broken bin with a 0 exit; the
# undefined-symbol case is ALSO the extraction guard for cheatlist.i65 -- if a routine
# moved into the shared include still referenced a $C0-only label, it fails loudly here).
$(IGMENU): igmenu.o65
	sneslink -fsmc -o igmenu_raw.smc $^ 2>&1 | tee igmenu.link.log
	@if grep -qE "ERROR:|still undefined|still not defined" igmenu.link.log; then \
	  echo "*** igmenu link FAILED — bank overflow or undefined symbol (see snes/igmenu.link.log)"; exit 1; fi
	python3 utils/pack_igmenu.py igmenu_raw.smc $(IGMENU)

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
	rm -f -- *~ *.ips *.o65 const_lang.a65 const_lang_str.a65 const_lang_tab.a65 const_lang_cjk.a65 const_lang_fm.a65 lang_*.bin lang_*.fnt igmenu_lang.i65 $(OUT) *.o65.log *.ips.log $(IGMENU) igmenu_raw.smc igmenu.link.log
	rm -f -- onboarding/*.o65 onboarding/*.ips onboarding/*.o65.log onboarding/*.ips.log onboarding/onb_const_lang.a65 onboarding/onb_const_lang_b.a65 onboarding/onb_const_lang_t.a65 onboarding/onb_str_*.a65 onboarding/onb_cjk_sheets.a65 onboarding/onb_cjk_sheets_zh.a65 onboarding/onb_font.a65 $(ONB_OUT) onb_link.log

-include $(info $(DEPDIR)) $(shell mkdir $(DEPDIR) 2>/dev/null) $(wildcard $(DEPDIR)/*)

.PHONY: all smc map clean
