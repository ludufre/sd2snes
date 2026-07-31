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

O65 = nes_data.o65 nes_render.o65 nes_boot.o65 nes_transport_device.o65 nes_snes.o65
IPS = nes_data_zp.ips nes_header.ips
RAW = nes_snes_raw.sfc
FINAL = ../../misc/nes_snes.bin

all: $(FINAL)

$(FINAL): $(RAW) gen_lorom.py
	@mkdir -p $(@D)
	python3 gen_lorom.py $(RAW) $(FINAL) --pages 1 --romsize 0x05

$(RAW): $(O65) $(IPS)
	sneslink -fsmc -o $@ $(O65) $(IPS) 2>&1 | tee link.log
	@if grep -qE "ERROR:|still undefined|still not defined" link.log; then \
	  echo "*** nes_snes link FAILED (see snes/nes/link.log)"; exit 1; fi

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

clean:
	rm -f *.o65 *.o65.log *.ips *.ips.log $(RAW) link.log

.PHONY: all clean
