# Makefile -- builda nes_snes.bin (renderer NES->SNES real, Fase 1b) via
# snescom+sneslink + o slicer LoROM (gen_lorom.py -- ver nota extensa la:
# snescom/sneslink so' produzem bancos lineares de 64KB, nao o janelamento
# LoROM de 32KB; o slicer fatia $8000-$FFFF de cada 'page' e conserta
# romsize/checksum). NAO faz parte do build do menu (snes/Makefile) --
# roda so' quando chamado explicitamente aqui.
#
# snescom aceita #include/#define direto nos .a65 (sem precisar de cpp
# externo alimentando ele -- mesma observacao do Makefile principal de
# snes/, onde "cpp ..." so' gera dependencia p/ o make, nunca e' o que o
# snescom realmente le).
#
# GOTCHA REAL (pego empiricamente, custou uma sessao inteira): ao linkar
# MULTIPLOS .o65 pra' mesma pagina, o sneslink NAO respeita o '*=' de cada
# objeto individual -- ele empacota os segmentos SEQUENCIALMENTE (relink/
# reposiciona), ignorando o endereco absoluto declarado. Conteudo que
# PRECISA ficar num endereco EXATO mesmo linkado junto com outros objetos
# (o header LoROM em $FFB0 e o ponteiro de Direct Page em $0010, usado por
# '[nes_parse_ptr],y') tem que virar IPS (snescom -I -J, "never relocated"
# -- mesmo padrao que o Makefile principal de snes/ ja usa pro header.ips
# do menu). sneslink aceita misturar .o65 (relocavel) e .ips (fixo) no
# mesmo link, resolvendo referencias cruzadas entre os dois normalmente.

O65 = nes_data.o65 nes_render.o65 nes_boot.o65 nes_transport_device.o65 nes_apu.o65 nes_snes.o65
IPS = nes_data_zp.ips nes_header.ips
RAW = nes_snes_raw.sfc
FINAL = ../../misc/nes_snes.bin
# Mapa de simbolos do binario final (ver gen_map.py). Sai ao lado do .bin: o gate
# de host tests/host/run_nes_chr.sh EXECUTA os bytes reais num interpretador 65816
# e precisa dos dois juntos. O mapa carimba o CRC do .bin, entao um par
# dessincronizado e' recusado em vez de virar lixo silencioso.
MAP = $(FINAL:.bin=.map)

all: $(FINAL) $(MAP)

$(FINAL): $(RAW) gen_lorom.py
	@mkdir -p $(@D)
	python3 gen_lorom.py $(RAW) $(FINAL) --pages 1 --romsize 0x05

# O mapa sai DEPOIS do .bin: o CRC carimbado e' o do arquivo final, ja' fatiado e
# com o checksum corrigido pelo gen_lorom.py. link.log e os *.o65.log/*.ips.log ja'
# existem aqui: $(FINAL) depende do $(RAW), que e' quem os produz. A ORDEM dos
# objetos tem que ser a MESMA do sneslink -- e' ela que casa cada objeto com o seu
# 'object_<N>_code' no link.log.
$(MAP): $(FINAL) gen_map.py
	python3 gen_map.py link.log $(FINAL) $@ $(O65) $(IPS)

$(RAW): $(O65) $(IPS)
	sneslink -fsmc -o $@ $(O65) $(IPS) 2>&1 | tee link.log
	@if grep -qE "ERROR:|still undefined|still not defined" link.log; then \
	  echo "*** nes_snes link FAILED (see snes/nes/link.log)"; exit 1; fi

# TRAVA DE BUILD (v2.7, sprites 8x16) -- nes_data.o65 tem regra PROPRIA (a
# explicita vence a pattern) so' por causa dela. O nes_apply_vblank deriva a
# fonte da pagina alta da OAM (slots 64-127) somando $0600 ao PROPRIO
# nes_av_fbo (fb_oam0 $7F4000 -> fb_oam0b $7F4600; fb_oam1 $7F4100 ->
# fb_oam1b $7F4700). Se qualquer um dos quatro blocos mudar de endereco em
# nes_data.a65, a NMI passa a DMAr 256B de WRAM ERRADA direto pros slots
# 64-127 da OAM -- em silencio, e nenhum gate garante pegar. A regra le os
# enderecos REAIS no log de simbolos do snescom e ABORTA o build.
# Fica na compilacao (nao no link) de proposito: assim vale tambem para o
# build do HARNESS, que pede estes .o65 por 'make -C' e nunca linka o
# nes_snes_raw.sfc.
nes_data.o65: nes_data.a65
	snescom -J -Wall -o $@ $< 2>&1 | tee $@.log
	@if grep -q "Error:" $@.log; then \
	  echo "*** snescom FAILED on $< (see $@.log)"; rm -f $@; exit 1; fi
	@a=`sed -n 's/^ *\([0-9A-Fa-f]*\) nes_fb_oam0$$/\1/p' $@.log`; \
	 b=`sed -n 's/^ *\([0-9A-Fa-f]*\) nes_fb_oam0b$$/\1/p' $@.log`; \
	 c=`sed -n 's/^ *\([0-9A-Fa-f]*\) nes_fb_oam1$$/\1/p' $@.log`; \
	 d=`sed -n 's/^ *\([0-9A-Fa-f]*\) nes_fb_oam1b$$/\1/p' $@.log`; \
	 if [ -z "$$a" ] || [ -z "$$b" ] || [ -z "$$c" ] || [ -z "$$d" ]; then \
	   echo "*** trava OAM 8x16: simbolo fb_oam? ausente em $@.log"; rm -f $@; exit 1; fi; \
	 if [ $$((0x$$b - 0x$$a)) -ne $$((0x0600)) ] || [ $$((0x$$d - 0x$$c)) -ne $$((0x0600)) ]; then \
	   echo "*** trava OAM 8x16: fb_oam?b tem de ficar em fb_oam?+\$$0600"; \
	   echo "    fb_oam0=\$$$$a fb_oam0b=\$$$$b  fb_oam1=\$$$$c fb_oam1b=\$$$$d"; \
	   echo "    (ver o 'adc #\$$0600' em nes_apply_vblank e o mapa em nes_data.a65)"; \
	   rm -f $@; exit 1; fi

# Regra generica .a65 -> .o65 (mesmo padrao/guard de snes/Makefile: o
# "| tee" mascara o exit code do snescom, entao grepa o log e falha alto).
%.o65 %.o65.log: %.a65
	snescom -J -Wall -o $(@:.log=) $< 2>&1 | tee $(@:.log=).log
	@if grep -q "Error:" $(@:.log=).log; then \
	  echo "*** snescom FAILED on $< (see $(@:.log=).log)"; rm -f $(@:.log=); exit 1; fi

# .a65 -> .ips (conteudo de endereco FIXO, nunca relocado -- ver nota acima).
%.ips %.ips.log: %.a65
	snescom -I -J -Wall -o $(@:.log=) $< 2>&1 | tee $(@:.log=).log
	@if grep -q "Error:" $(@:.log=).log; then \
	  echo "*** snescom FAILED on $< (see $(@:.log=).log)"; rm -f $(@:.log=); exit 1; fi

# O .bin sai JUNTO com o .map: sem o mapa o gate de host pula o bloco do renderer
# e fica verde sem ter executado nada.
clean:
	rm -f *.o65 *.o65.log *.ips *.ips.log $(RAW) link.log $(FINAL) $(MAP)

.PHONY: all clean
