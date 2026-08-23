/* sd2snes - SD card based universal cartridge for the SNES
   Copyright (C) 2009-2010 Maximilian Rehkopf <otakon@gmx.net>
   AVR firmware portion

   Inspired by and based on code from sd2iec, written by Ingo Korb et al.
   See sdcard.c|h, config.h.

   FAT file system access based on code by ChaN, Jim Brain, Ingo Korb,
   see ff.c|h.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License only.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

   nes.c: carregador de .nes (iNES) -- Fase 0 (+ Fase 1c: pre-conversao de
   CHR) do core NES, espelho do sgb.c.

   Fluxo (identico ao SGB em memory.c/load_rom):
     1. nes_id()             -- detecta a extensao .nes + parseia o header iNES
                                (16 bytes) e valida o mapper contra o conjunto
                                v0 {0,1,2,3,7,28} do mmu.v podado; mapper fora
                                do conjunto / four-screen / header invalido =>
                                romprops.error = MENU_ERR_NOIMPL (popup limpo
                                no menu, nunca trava).
     2. nes_update_file()    -- troca o filename pelo stub SNES-side
                                (/sd2snes/nes_snes.bin), que vira "a ROM" que o
                                load_rom stream normalmente pra 0x880000.
     3. nes_update_romprops()-- valida o stub (LoROM <=512KB sem SaveRAM, igual
                                ao sgb_update_romprops) e aponta
                                fpga_conf = FPGA_NES / load_address = 0x880000.
     4. nes_load_prg()       -- stream do PRG pra PSRAM 0x000000 e do CHR-ROM
                                pra 0x200000 EM FORMATO NES NATIVO; CHR-RAM =>
                                regiao zerada; CIRAM/WRAM/CART-RAM zeradas p/
                                determinismo; breadcrumb "NESL" em 0x400000
                                (gate da Fase 0, lido por USB).
     5. nes_convert_chr()    -- (Fase 1c) SO' se houver CHR-ROM (nao CHR-RAM):
                                converte tile a tile o que acabou de ser
                                escrito em 0x200000 e grava as DUAS regioes
                                pre-convertidas p/ SNES que o renderer 65816
                                vai consumir: 0x500000 (2bpp, BG) e 0x600000
                                (4bpp, OBJ).  Contrato byte-a-byte em
                                nes_chr.h / utils/nes_chr_convert.py.  Le de
                                volta da PSRAM (nao do SD) -- ver comentario
                                em nes_convert_chr() pro motivo.

   mapper_flags: a palavra de 16 bits (nes_romprops.mapper_flags16) segue
   byte-a-byte o GameLoader do fpganes original (NES_Nexys4.v:122):
     {has_chr_ram, mirroring, chr_size[2:0], prg_size[2:0], mapper[7:0]}
   e chega ao FPGA via FPGA_CMD_CHIPFEAT (0xef -> nes_feat_out em mcu_cmd.v ->
   mapper_flags_in[15:0] do nes_wrap; [31:16] = 0 como no fpganes).  INVARIANTE
   (main.sdc false-path): o firmware escreve o chipfeat ANTES do
   deassert_reset() -- nunca reprograma com o core NES rodando.

   mk3-only: no mk2 (LPC1754, flash apertada + sem fpga_nes.bit pro Spartan-3)
   tudo compila como stubs no-op (has_nes fica 0) -- ver #ifdef CONFIG_MK2. */

#include <ctype.h>

#include "fileops.h"
#include "config.h"
#include "uart.h"
#include "smc.h"
#include "nes.h"
#include "nes_chr.h"
#include "string.h"
#include "fpga_spi.h"
#include "snes.h"
#include "fpga.h"
#include "cfg.h"
#include "memory.h"

/* ~64B como o sgb_romprops: pequeno demais pra justificar AHB (o gotcha de
   .bss mira buffers de centenas de bytes); espelha o precedente do SGB. */
nes_romprops_t nes_romprops;

#ifndef CONFIG_MK2

/* Estado do anti-wedge/nesdbg (definidos aqui em cima porque nes_id ja' os
   reseta; a mecanica completa esta' documentada no bloco "Anti-wedge" antes
   de nes_stream_window, mais abaixo). */
static uint8_t  nes_fpga_err;       /* latch de timeout do FPGA (espelho do patch_io_err) */
static uint32_t nes_dbg_total, nes_dbg_done;  /* progresso p/ STREAM_xx% */
static uint32_t nes_dbg_conv_total, nes_dbg_conv_done;  /* progresso p/ CONVERT_xx% (v2.0b) */
static uint8_t  nes_dbg_conv_quarter;                   /* proximo quartil (1..3), zerado por load */
static uint8_t  nes_dbg_quarter;    /* proximo quartil a anunciar (1..3) */
static uint8_t  nes_dbg_truncate;   /* 1 = proximo marco recria o nesdbg.log */

/* Classe de tamanho em bancos -> campo de 3 bits do mapper_flags.  Tabela
   identica ao GameLoader do fpganes (prg: banks de 16KB; chr: banks de 8KB):
   <=1 -> 0, <=2 -> 1, <=4 -> 2, <=8 -> 3, <=16 -> 4, <=32 -> 5, <=64 -> 6,
   senao 7.  O MultiMapper (mmu.v) converte a classe em prg_mask/chr_mask. */
static uint8_t nes_size_class(uint8_t banks) {
  uint8_t cls = 0;
  uint8_t cap = 1;
  while(cls < 7 && banks > cap) {
    cls++;
    cap <<= 1;
  }
  return cls;
}

/* Mappers suportados pelo mmu.v podado (MultiMapper): case 1 -> MMC1;
   case 4 -> MMC3 (TxROM: SMB3, Mega Man 3, Kirby...);
   case 0,2,3,7,28 -> Mapper28 (que emula NROM/UNROM/CNROM/AOROM alem do 28);
   qualquer outro cairia no default MMC0 e rodaria ERRADO -> NOIMPL limpo.

   O `case 4` so' podia entrar AGORA (design Sec. 8.1, invariante duro): ate' o
   gate 2.3 o tap de CHR do mapper 4 publicava um SENTINELA constante, entao um
   jogo MMC3 carregaria com "banco 0" em toda a CHR -- falha SILENCIOSA, a pior
   classe.  O que destrava e' o CMD_CHR_STATE8 $14 (vetor de 8 janelas de 1KB)
   existir no nes_bridge.v/mmu.v; ele existe desde a v2.5.  Se algum dia o $14
   for removido/gated, ESTE case volta a sair junto. */
static uint8_t nes_mapper_supported(uint8_t mapper) {
  switch(mapper) {
    case 0: case 1: case 2: case 3: case 4: case 7: case 28:
      return 1;
    default:
      return 0;
  }
}

void nes_id(nes_romprops_t* props, uint8_t *filename) {
  nes_header_t* header = &(props->header);

  props->has_nes = 0;
  props->mapper_id = 0;
  props->prg_size_class = 0;
  props->chr_size_class = 0;
  props->mirror_vertical = 0;
  props->four_screen = 0;
  props->has_battery = 0;
  props->has_trainer = 0;
  props->has_chr_ram = 0;
  props->is_nes20 = 0;
  props->supported = 0;
  props->prgsize_bytes = 0;
  props->chrsize_bytes = 0;
  props->mapper_flags16 = 0;
  props->error = MENU_ERR_OK;
  props->error_param = NULL;

  /* check for NES ROM.  match case-insensitive <name>.nes */
  char *ext = strrchr((char*)filename, (int)'.');
  if(!ext || strcasecmp(ext + 1, "nes")) return;

  printf("Loading NES\n");
  props->has_nes = 1;
  /* novo load NES: limpa o latch de timeout do FPGA e recomeca o nesdbg.log
     (o 1o marco gravado recria o arquivo -- cada load deixa so' a sua trilha) */
  nes_fpga_err = 0;
  nes_dbg_truncate = 1;
  nes_dbg_total = 0;

  /* o arquivo ja esta aberto pelo load_rom (mesmo contrato do sgb_id) */
  if(file_readblock(header, 0, sizeof(nes_header_t)) < sizeof(nes_header_t)
     || file_res
     || header->magic[0] != 'N' || header->magic[1] != 'E'
     || header->magic[2] != 'S' || header->magic[3] != 0x1A) {
    printf("NES: bad iNES magic\n");
    props->error = MENU_ERR_NOIMPL;
    props->error_param = (const uint8_t*)"iNES header";
    return;
  }

  props->mirror_vertical = (header->flags6 >> 0) & 1;
  props->has_battery     = (header->flags6 >> 1) & 1;
  props->has_trainer     = (header->flags6 >> 2) & 1;
  props->four_screen     = (header->flags6 >> 3) & 1;
  props->is_nes20        = ((header->flags7 & 0x0C) == 0x08);

  /* mapper number, derivacao iNES classica: nibble alto de flags7 + nibble
     baixo de flags6.  Headers antigos contaminados ("DiskDude!" nos bytes
     12-15) tem lixo no flags7 -> heuristica padrao de emulador: se os 4
     ultimos bytes nao sao zero, confia so no nibble baixo. */
  props->mapper_id = (header->flags7 & 0xF0) | (header->flags6 >> 4);
  if(header->padding[1] || header->padding[2]
     || header->padding[3] || header->padding[4]) {
    props->mapper_id &= 0x0F;
  }

  /* NES 2.0: os campos que MENTEM tamanho/mapper rejeitam; SUBMAPPER e'
     tratado por valor (regressao real de campo: o Mega Man (USA) do cartao
     declara submapper em mapper 2 e a rejeicao cega barrava um jogo que o
     core roda IDENTICO ao dump iNES 1.0).

       flags8[3:0] = mapper MSB (bits 11:8)  -> mapper >255: NOIMPL
       flags9[7:0] = PRG/CHR size MSB        -> tamanho real > bytes 4/5:
                     prgsize/chrsize MENTIRIAM pro stream -> NOIMPL
       flags8[7:4] = SUBMAPPER: ACEITO E IGNORADO como regra (as variantes de
                     mapper 1/2/3/7 sao bus-conflict/board trivia que este core
                     nao emula em NENHUMA variante -- carregar = mesmo
                     comportamento do dump iNES 1.0 do mesmo jogo).  EXCECAO:
                     mapper 4 com submapper 1 (MMC6) ou 3 (MC-ACC) e' hardware
                     genuinamente diferente -> NOIMPL. */
  if(props->is_nes20 && ((header->flags8 & 0x0F) || header->flags9)) {
    props->error = MENU_ERR_NOIMPL;
    props->error_param = (const uint8_t*)"NES 2.0";
    return;
  }
  if(props->is_nes20 && props->mapper_id == 4) {
    uint8_t submapper = header->flags8 >> 4;
    if(submapper == 1 || submapper == 3) {
      props->error = MENU_ERR_NOIMPL;
      props->error_param = (const uint8_t*)"MMC6/MC-ACC";
      return;
    }
  }

  /* tamanhos (classico): PRG em bancos de 16KB, CHR em bancos de 8KB.
     0 bancos de CHR = CHR-RAM.  Janelas da PSRAM = 1MB cada (nes_wrap.v). */
  props->prgsize_bytes = (uint32_t)header->prg_16k_banks * 16384;
  props->chrsize_bytes = (uint32_t)header->chr_8k_banks * 8192;
  props->has_chr_ram   = (header->chr_8k_banks == 0);
  if(header->prg_16k_banks == 0 || header->prg_16k_banks > 64
     || header->chr_8k_banks > 128) {
    props->error = MENU_ERR_NOIMPL;
    props->error_param = (const uint8_t*)"PRG/CHR size";
    return;
  }
  props->prg_size_class = nes_size_class(header->prg_16k_banks);
  props->chr_size_class = nes_size_class(header->chr_8k_banks);

  /* Limites ESPECIFICOS do mapper 4 (design Sec. 6): o check generico acima
     aceita PRG ate' 1MB e CHR ate' 1MB, o que EXCEDE a largura dos seletores do
     MMC3 -- `prgsel[5:0]` enderecca 64 bancos de 8KB = 512KB e `chrsel[7:0]`
     256 bancos de 1KB = 256KB (mmu.v, module MMC3).  Passar disso nao da erro
     em lugar nenhum: os bits que faltam simplesmente somem no
     `prg_aout & prg_mask` / `chr_aout & chr_mask` e a ROM ALIASA em silencio.
     32 bancos de 16KB = 512KB de PRG; 32 bancos de 8KB = 256KB de CHR. */
  if(props->mapper_id == 4
     && (header->prg_16k_banks > 32 || header->chr_8k_banks > 32)) {
    props->error = MENU_ERR_NOIMPL;
    props->error_param = (const uint8_t*)"MMC3 size";
    return;
  }

  /* NES 2.0 byte 11 (= header->padding[0], offset 0x0B) declara o TAMANHO da
     CHR-RAM: nibble baixo = CHR-RAM volatil, nibble alto = CHR-NVRAM (com
     bateria); cada um e' um SHIFT, bytes = 64 << shift, 0 = ausente.  O iNES
     classico nao carrega esse campo -- la' chr_8k_banks==0 sempre significou
     8KB -- e o check de NES 2.0 acima so' olha flags8/flags9, entao ate' aqui
     o byte 11 passava direto.

     Por que ele TEM que ser checado (NES-FASE2-MMC3-DESIGN.md Sec. 5.1/7): o
     core so' enxerga 8KB de CHR-RAM.  Com chr_8k_banks==0 o chr_size_class e'
     0 -> chr_mask = 0 no mmu.v -> chr_aout[19:13] zerado, isto e', uma
     CHR-RAM de 32KB (Haunted Halloween '86) ALIASA EM SILENCIO pros mesmos
     8KB; e o bitmap de sujeira da bridge tem CHR_TILES=512 indexado por
     chr_tile[12:0], entao indice > 511 escreveria FORA do array.  Falha
     silenciosa em cima de falha silenciosa -> NOIMPL limpo.

     Aceita-se exatamente um caso: CHR-RAM volatil de ate' 8KB com
     chr_8k_banks==0.  Qualquer outra combinacao (NVRAM, > 8KB, ou CHR-RAM
     declarada JUNTO com CHR-ROM, que o core nao sabe rotear) e' rejeitada.
     (A recomendacao 7.3 do design dizia "flags11 != 0 -> NOIMPL"; isso
     reprovaria tambem o caso COMUM byte11=0x07 = 8KB, que e' justamente o
     alvo do gate 2.2-lite -- por isso aqui a regra e' pelo TAMANHO.) */
  if(props->is_nes20 && header->padding[0]) {
    uint8_t  ram_shift = header->padding[0] & 0x0F;
    uint8_t  nv_shift  = header->padding[0] >> 4;
    uint32_t ram_bytes = ram_shift ? (64UL << ram_shift) : 0;
    if(nv_shift || ram_bytes > 8192 || !props->has_chr_ram) {
      props->error = MENU_ERR_NOIMPL;
      props->error_param = (const uint8_t*)"CHR-RAM size";
      return;
    }
  }

  /* BATTERY (flags6 bit 1) e' registrado mas NAO rejeita (design Sec. 6/8.4):
     a CART-RAM de 8KB em 0x3C0000 existe e funciona -- o mapper 4 usa o mesmo
     encoding do MMC1/Mapper28 -- so' nao ha' PERSISTENCIA em fase nenhuma.  O
     efeito e' "joga sem salvar" (Crystalis, Blade Buster, Kirby), que e' util,
     e nao "roda errado", que seria motivo de NOIMPL.  A persistencia e' fase
     propria; nao transformar isto num filtro sem mudar o design. */

  /* four-screen nao e' representavel no mapper_flags do fpganes -> NOIMPL */
  if(props->four_screen) {
    props->error = MENU_ERR_NOIMPL;
    props->error_param = (const uint8_t*)"four-screen";
    return;
  }
  if(!nes_mapper_supported(props->mapper_id)) {
    /* o numero do mapper vai no popup: buffer estatico pequeno (persistente
       ate o proximo load, igual ao error_param de string literal) */
    static char nes_mapper_msg[16];
    snprintf(nes_mapper_msg, sizeof(nes_mapper_msg), "mapper %d", props->mapper_id);
    props->error = MENU_ERR_NOIMPL;
    props->error_param = (const uint8_t*)nes_mapper_msg;
    return;
  }
  props->supported = 1;

  /* palavra exata enviada via CHIPFEAT -> mapper_flags_in[15:0] do nes_wrap
     (mapper_flags[31:16] = 0, como no GameLoader do fpganes) */
  props->mapper_flags16 =
      ((uint16_t)(props->has_chr_ram    & 1) << 15)
    | ((uint16_t)(props->mirror_vertical & 1) << 14)
    | ((uint16_t)(props->chr_size_class & 7) << 11)
    | ((uint16_t)(props->prg_size_class & 7) <<  8)
    |  (uint16_t) props->mapper_id;

  printf("NES:  mapper=%d prg=%ldKB chr=%ldKB chr_ram=%d mirror=%c trainer=%d"
         " battery=%d nes2.0=%d flags=0x%04x\n",
         props->mapper_id,
         props->prgsize_bytes / 1024, props->chrsize_bytes / 1024,
         props->has_chr_ram, props->mirror_vertical ? 'V' : 'H',
         props->has_trainer, props->has_battery, props->is_nes20,
         props->mapper_flags16);
}

uint8_t nes_update_file(uint8_t **filename_ref) {
  if (nes_romprops.has_nes) {
    file_close();

    *filename_ref = (uint8_t *)NES_SNES_STUB;
    file_open(*filename_ref, FA_READ);
    if(file_res) {
      uart_putc('?');
      uart_putc(0x30 + file_res);
      nes_romprops.has_nes = 0;
      return 0;
    }
  }

  return 1;
}

uint8_t nes_update_romprops(snes_romprops_t *romprops, uint8_t *nes_filename) {
  if (nes_romprops.has_nes) {
    /* confirm properties of the SNES stub image (espelha o SGB) */
    if (  !(romprops->mapper_id == 1)                                      /* LOROM */
       || !(0 < romprops->header.romsize && romprops->header.romsize < 10) /* <=512KB */
       || !(file_handle.fsize <= (512 * 1024))
       || !(romprops->sramsize_bytes == 0)                                 /* no SaveRAM */
       ) {
      nes_romprops.has_nes = 0;
      printf("NES SNES stub does not meet requirements: mapper=0x%02x, romsize=0x%02x, filesize=%ld, sramsize_bytes=%ld.\n",
        romprops->mapper_id,
        romprops->header.romsize,
        file_handle.fsize,
        romprops->sramsize_bytes);
      return 0;
    }

    romprops->fpga_conf = FPGA_NES;
    romprops->load_address = 0x880000;
  }

  return 1;
}

/* ------------------------------------------------------------------
 * Anti-wedge (investigacao do 1o teste em hardware): o load de um .nes
 * WEDGOU a MCU (USB morto, so power-cycle).  O RTL foi exonerado por
 * simulacao (tb_mcu_path dirige o protocolo SPI real contra o main.v e
 * passa); os suspeitos firmware-side sao (a) fpga_pgm(fpga_nes.bi3)
 * falhando SILENCIOSAMENTE (retorna void; em erro de open ele da return
 * sem reconfigurar nem atualizar fpga_config -- ver fpga.c) e o load
 * seguir pro stream achando que o core NES subiu, e (b) o core
 * configurar mas nao dirigir o pino MCU_RDY -> o primeiro FPGA_WAIT_RDY
 * (espera UNBOUNDED pelo pino) gira pra sempre.  Duas defesas:
 *
 * 1. TODA espera de MCU_RDY do caminho NES e' BOUNDED: FPGA_WAIT_RDY_TO
 *    (fpga_spi.h; ~5M iteracoes de busy-loop ~ fracao de segundo) com o
 *    latch nes_fpga_err, espelho exato do patch_io_err do patch.c: no 1o
 *    estouro o latch trava, todos os helpers abaixo viram no-op, e
 *    nes_load_prg aborta LIMPO (MCU viva, USB vivo).  NAO da pra NACKar o
 *    menu aqui: o ACK $55 do game_handshake acontece ANTES do fpga_pgm no
 *    load_rom (memory.c), entao nesta janela o SNES ja foi solto -- abort
 *    limpo = parar de tocar o FPGA + logar + retornar, nunca travar.  Os
 *    helpers globais (sram_*block de memory.c) ficam intactos: wrappers
 *    locais pra nao mudar o comportamento dos outros consumidores.
 *
 * 2. Log de fases em ARQUIVO no SD (nes_dbg_log / NES_DBG_LOG): sobrevive
 *    a power-cycle, entao mesmo que a MCU wedgue num caminho ainda nao
 *    coberto, o ultimo marco gravado crava a fase.  f_sync a cada marco
 *    (o wedge pode vir logo depois).  2o FIL, LOCAL na stack (~600B de
 *    frame, precedente: FIL dst do cfg.c no MESMO contexto de load; nada
 *    de FIL residente no .bss apertado) -- nunca toca o file_handle
 *    global que o load usa pro .nes/stub (_FS_LOCK=0/_FS_TINY=0: cada FIL
 *    tem buffer proprio; padrao listed_game_commit/gi_fmv_fil).  Todos os
 *    marcos ficam FORA de qualquer transferencia sd_offload em andamento.
 * ------------------------------------------------------------------ */

/* (nes_fpga_err / nes_dbg_* estao declarados no topo do arquivo -- nes_id
   os reseta a cada load novo.  Semantica: nes_fpga_err e' o latch de
   timeout do FPGA, 0 = ok, 1 = alguma espera de MCU_RDY estourou ou o
   proprio fpga_pgm falhou (nes_dbg_post_pgm).) */

void nes_dbg_log(const char *tag) {
  FIL fil;                          /* 2o FIL na STACK (ver comentario acima) */
  UINT wr;
  if(!nes_romprops.has_nes) return;
  printf("nesdbg: %s\n", tag);
  if(f_open(&fil, (const TCHAR*)NES_DBG_LOG,
            FA_WRITE | (nes_dbg_truncate ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS))
     != FR_OK) return;               /* log e' best-effort, nunca bloqueia o load */
  nes_dbg_truncate = 0;
  f_lseek(&fil, fil.fsize);          /* append (FatFs antigo: sem FA_OPEN_APPEND) */
  f_write(&fil, tag, strlen(tag), &wr);
  f_write(&fil, "\n", 1, &wr);
  f_sync(&fil);                      /* essencial: o wedge pode vir logo depois */
  f_close(&fil);
}

/* Marco POST_PGM + deteccao da falha SILENCIOSA do fpga_pgm: fpga_pgm e'
   void, mas (a) so' atualiza fpga_config no SUCESSO (erro de open retorna
   ANTES -- fpga_config fica apontando o core anterior) e (b) o pino DONE
   diz se ha' bitstream valido no FPGA agora.  cfg_ok compara PONTEIRO
   (fpga_pgm guarda o proprio ptr que recebeu).  Em falha, latcha
   nes_fpga_err pra nes_load_prg pular o stream limpo -- senao o 1o
   FPGA_WAIT_RDY contra um core errado/ausente e' exatamente o wedge
   observado em hardware. */
void nes_dbg_post_pgm(const uint8_t *conf) {
  char m[48];
  int done, cfg_ok;
  if(!nes_romprops.has_nes) return;
  done   = fpga_get_done();
  cfg_ok = (fpga_config == conf);
  snprintf(m, sizeof(m), "POST_PGM done=%d cfg_ok=%d", done, cfg_ok);
  nes_dbg_log(m);
  if(!done || !cfg_ok) {
    nes_fpga_err = 1;
    nes_dbg_log("PGM_FAIL (fpga_pgm silent failure -> skip NES load)");
  }
}

static void nes_dbg_progress(uint32_t add) {
  char m[16];
  if(!nes_dbg_total) return;
  nes_dbg_done += add;
  while(nes_dbg_quarter <= 3
        && nes_dbg_done >= (nes_dbg_total / 4) * nes_dbg_quarter) {
    snprintf(m, sizeof(m), "STREAM_%d%%", 25 * nes_dbg_quarter);
    nes_dbg_log(m);
    nes_dbg_quarter++;
  }
}

/* v2.0b: espelho de nes_dbg_progress(), mas pro loop de CONVERSAO de CHR
   (nes_convert_chr, mais abaixo) -- estado PROPRIO (nes_dbg_conv_*), nunca
   mistura com o contador STREAM_xx% do stream cru (fases diferentes, custo
   por byte diferente -- ver comentario de nes_convert_chr). Existe porque a
   conversao de uma CHR grande (pior caso realista: MMC1 128KB/8192 tiles)
   custa ~4x o tempo do stream cru do mesmo volume (1 read + 2 write de 16B
   por tile) e antes so' havia 1 log ANTES + 1 DEPOIS de todo o loop -- pra
   um jogo com CHR grande isso deixava um gap de log de alguns segundos sem
   nenhum marco intermediario. So' um log a cada 25%, custo desprezivel
   frente as centenas/milhares de transacoes SPI byte-a-byte do loop. */
static void nes_dbg_conv_progress(uint32_t add) {
  char m[16];
  if(!nes_dbg_conv_total) return;
  nes_dbg_conv_done += add;
  while(nes_dbg_conv_quarter <= 3
        && nes_dbg_conv_done >= (nes_dbg_conv_total / 4) * nes_dbg_conv_quarter) {
    snprintf(m, sizeof(m), "CONVERT_%d%%", 25 * nes_dbg_conv_quarter);
    nes_dbg_log(m);
    nes_dbg_conv_quarter++;
  }
}

/* Wrappers BOUNDED dos helpers de PSRAM (espelham set_mcu_addr/
   sram_writeblock/sram_readblock/sram_memset de fpga_spi.c/memory.c, so'
   trocando a espera por FPGA_WAIT_RDY_TO_INLINE(nes_fpga_err) -- a forma
   macro, porque sao lacos por byte).  Uma vez
   latched, tudo vira no-op (padrao patch.c) -- o abort inteiro custa UM
   timeout (~fracao de segundo), nao um por byte. */
static void nes_set_mcu_addr_to(uint32_t addr) {
  if(nes_fpga_err) return;
  FPGA_SELECT();
  FPGA_WAIT_RDY_TO(nes_fpga_err);    /* espelha o "wait prior ops" do original */
  if(nes_fpga_err) { FPGA_DESELECT(); return; }
  FPGA_TX_BYTE(FPGA_CMD_SETADDR | FPGA_TGT_MEM);
  FPGA_TX_BYTE((addr >> 16) & 0xff);
  FPGA_TX_BYTE((addr >> 8) & 0xff);
  FPGA_TX_BYTE(addr & 0xff);
  FPGA_DESELECT();
}

static void nes_sram_writeblock_to(const void *buf, uint32_t addr, uint16_t size) {
  const uint8_t *src = buf;
  if(nes_fpga_err) return;
  nes_set_mcu_addr_to(addr);
  if(nes_fpga_err) return;
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);                /* WRITE with autoincrement */
  while(size--) {
    FPGA_TX_BYTE(*src++);
    FPGA_WAIT_RDY_TO_INLINE(nes_fpga_err);
    if(nes_fpga_err) break;
  }
  FPGA_DESELECT();
}

static void nes_sram_readblock_to(void *buf, uint32_t addr, uint16_t size) {
  uint8_t *tgt = buf;
  if(nes_fpga_err) return;
  nes_set_mcu_addr_to(addr);
  if(nes_fpga_err) return;
  FPGA_SELECT();
  FPGA_TX_BYTE(0x88);                /* READ */
  while(size--) {
    FPGA_WAIT_RDY_TO_INLINE(nes_fpga_err);
    if(nes_fpga_err) break;
    *(tgt++) = FPGA_RX_BYTE();
  }
  FPGA_DESELECT();
}

static void nes_sram_memset_to(uint32_t addr, uint32_t len, uint8_t val) {
  if(nes_fpga_err) return;
  nes_set_mcu_addr_to(addr);
  if(nes_fpga_err) return;
  FPGA_SELECT();
  FPGA_TX_BYTE(0x98);
  for(uint32_t i = 0; i < len; i++) {
    FPGA_TX_BYTE(val);
    FPGA_WAIT_RDY_TO_INLINE(nes_fpga_err);
    if(nes_fpga_err) break;
  }
  FPGA_DESELECT();
}

/* ------------------------------------------------------------------
 * Publisher NDBG (debug in-game do freeze): 1x por iteracao do laco
 * in-game (main.c, while(fpga_test()==FPGA_TEST_TOKEN)), gated has_nes.
 * Le o grupo 0x04 da config-bus (opcode 0xf9 via fpga_read_config -- que
 * so' espera o FIFO do SSP local do LPC, NUNCA o pino MCU_RDY, entao e'
 * inerentemente bounded) e publica o snapshot em PSRAM 0x400100 pro
 * host ler por USB.  Mapa indice->sinal CONFERIDO no RTL
 * (nes_wrap.v, `assign config_data_out` + o always do bc_r):
 *   0/1   = PC lo/hi do 6502, snapshotado a cada INICIO de fetch de
 *           instrucao (bc_cpu_state==0, fora de reset)
 *   2..6  = A, X, Y, P, SP
 *   7     = bc_cyc_r[7:0], contador de ciclos de CPU FREE-RUNNING
 *           (incrementa a cada apu_ce independente da CPU -- distingue
 *           "CPU sem clock/ce morto" de "CPU rodando em loop apertado")
 *   8/9   = band_bytes_last LE (bridge: bytes de mailbox do ultimo frame)
 *   10/11 = band_frames LE     (bridge: frames serializados)
 *   12/13 = band_overruns LE   (bridge: overruns de frame)
 *   14    = (v3) max-hold STICKY de |APU_DAT| (8 bits) -- caca ao mudo:
 *           != 0 se ALGUMA amostra nao-zero ja' saiu do core desde o reset
 *   15    = (v3) contador ROLANTE de amostras nao-zero -- andando entre
 *           amostras USB = audio fluindo AGORA (14 = fluiu em algum momento)
 *   16    = (v3+) max-hold de |saida do CIC| do dac.v, zerado pelo pulso
 *           dac_reset de todo load -- audio sobreviveu ao filtro do DAC
 *           NESTE jogo (!= idx 14, que e' pre-filtro e sticky desde reset)
 *   17    = (v3+) contador rolante de bordas do LRCK (~2.4 voltas/s):
 *           parado entre amostras USB = o proprio clock I2S do DAC morreu
 *   18    = (v3++) max-hold de |vol_sample_sat| (dbg_dat_max no dac.v: a
 *           palavra EXATA que o shifter I2S recarrega a cada borda de
 *           LRCK, top 8 bits).  0xFF cravado = saida saturada/DC-pinned
 *           (assinatura de lixo no mix); 0 = dado morre entre CIC e
 *           shifter; valor musical variando + mudo no jack = o dado ESTA'
 *           no fio, o misterio e' off-chip (analogico).
 *   19    = (v3++++) contador rolante de NT-writes tapeados -- armadilha
 *           da perda de nametable em transicoes (leftover vs trace)
 *   22    = (v3+++) fingerprint da paleta: soma mod 256 do array pal da
 *           bridge -- armadilha do boot ruim de paleta (compara boot
 *           limpo vs sujo sem precisar de dump completo)
 *   23    = (v3+++) contador de writes de paleta tapeados
 * Layout do bloco NDBG documentado em nes.h (NES_PSRAM_NDBG_ADDR).
 * A escrita PSRAM usa o wrapper bounded (latch nes_fpga_err): FPGA morto
 * -> vira no-op e o laco segue vivo.  Snapshot NAO-atomico vs o core
 * rodando (22 leituras separadas) -- auto-consistente exatamente no caso
 * de uso (CPU presa).
 * ------------------------------------------------------------------ */
typedef struct __attribute__ ((__packed__)) _nes_ndbg {
  uint8_t  magic[4];         /* +0  "NDBG" */
  uint8_t  version;          /* +4  NES_NDBG_VERSION */
  uint8_t  seq;              /* +5  ++ a cada publish (prova de vida) */
  uint16_t pc;               /* +6  LE, PC do 6502 no ultimo fetch */
  uint8_t  a;                /* +8  */
  uint8_t  x;                /* +9  */
  uint8_t  y;                /* +10 */
  uint8_t  p;                /* +11 */
  uint8_t  sp;               /* +12 */
  uint8_t  cyc_lo;           /* +13 contador free-running (anda = CPU clocked) */
  uint16_t band_bytes_last;  /* +14 LE */
  uint16_t band_frames;      /* +16 LE */
  uint16_t band_overruns;    /* +18 LE */
  uint8_t  apu_max;          /* +20 (v3) max-hold sticky de |APU_DAT| */
  uint8_t  apu_nz_ctr;       /* +21 (v3) contador rolante de amostras != 0 */
  uint8_t  dac_cic_max;      /* +22 (v3+) max-hold |saida do CIC| (dac_reset zera) */
  uint8_t  dac_lrck_ctr;     /* +23 (v3+) contador rolante de bordas do LRCK */
  uint8_t  i2s_dat_max;      /* +24 (v3++) max-hold |vol_sample_sat| (shifter I2S) */
  uint8_t  nt_wr_ctr;        /* +25 (v3++++) idx 19: contador rolante de
                                NT-writes tapeados -- armadilha da perda de
                                nametable em transicoes (leftover "ORLD" no
                                SMB1 / digito sumido no BEST do Excitebike:
                                comparar vs o esperado do trace na janela) */
  uint8_t  reserved[2];      /* +26..+27 sempre 0 (reservado) */
  uint8_t  pal_sum;          /* +28 (v3+++) idx 22: fingerprint da paleta
                                (soma mod 256 do array pal da bridge) --
                                armadilha do boot ruim de paleta */
  uint8_t  pal_wr_ctr;       /* +29 (v3+++) idx 23: contador de writes de
                                paleta tapeados */
} nes_ndbg_t;                /* 30 bytes @ NES_PSRAM_NDBG_ADDR */

#define NES_NDBG_GROUP (0x04)

void nes_dbg_publish(void) {
  static uint8_t ndbg_seq;   /* 1 byte de estado; nao precisa reset por load
                                (so' a VARIACAO entre amostras importa) */
  nes_ndbg_t d;
  if(!nes_romprops.has_nes) return;

  d.magic[0] = 'N'; d.magic[1] = 'D'; d.magic[2] = 'B'; d.magic[3] = 'G';
  d.version = NES_NDBG_VERSION;
  d.seq     = ++ndbg_seq;
  d.pc      = (uint16_t)fpga_read_config(NES_NDBG_GROUP, 0)
            | ((uint16_t)fpga_read_config(NES_NDBG_GROUP, 1) << 8);
  d.a       = fpga_read_config(NES_NDBG_GROUP, 2);
  d.x       = fpga_read_config(NES_NDBG_GROUP, 3);
  d.y       = fpga_read_config(NES_NDBG_GROUP, 4);
  d.p       = fpga_read_config(NES_NDBG_GROUP, 5);
  d.sp      = fpga_read_config(NES_NDBG_GROUP, 6);
  d.cyc_lo  = fpga_read_config(NES_NDBG_GROUP, 7);
  d.band_bytes_last = (uint16_t)fpga_read_config(NES_NDBG_GROUP, 8)
                    | ((uint16_t)fpga_read_config(NES_NDBG_GROUP, 9) << 8);
  d.band_frames     = (uint16_t)fpga_read_config(NES_NDBG_GROUP, 10)
                    | ((uint16_t)fpga_read_config(NES_NDBG_GROUP, 11) << 8);
  d.band_overruns   = (uint16_t)fpga_read_config(NES_NDBG_GROUP, 12)
                    | ((uint16_t)fpga_read_config(NES_NDBG_GROUP, 13) << 8);
  d.apu_max         = fpga_read_config(NES_NDBG_GROUP, 14);  /* v3 */
  d.apu_nz_ctr      = fpga_read_config(NES_NDBG_GROUP, 15);  /* v3 */
  d.dac_cic_max     = fpga_read_config(NES_NDBG_GROUP, 16);  /* v3+ */
  d.dac_lrck_ctr    = fpga_read_config(NES_NDBG_GROUP, 17);  /* v3+ */
  d.i2s_dat_max     = fpga_read_config(NES_NDBG_GROUP, 18);  /* v3++ */
  d.nt_wr_ctr       = fpga_read_config(NES_NDBG_GROUP, 19);  /* v3++++ */
  d.reserved[0] = d.reserved[1] = 0;
  d.pal_sum         = fpga_read_config(NES_NDBG_GROUP, 22);  /* v3+++ */
  d.pal_wr_ctr      = fpga_read_config(NES_NDBG_GROUP, 23);  /* v3+++ */

  /* GATE DE FASE (bug REAL de hardware -- nao remover): escrever na PSRAM
     durante a janela de BOOT do renderer (o DMA da CHR pre-convertida das
     janelas 0x500000/0x600000 -> VRAM) viola a regra operacional documentada
     no main.v ("sem trafego MCU->PSRAM durante DMA de CHR; +MCU em voo
     ~380ns estoura a janela do /RD") e corrompia bytes NO MEIO do DMA ->
     CHR de OBJ assada com lixo ate' o proximo load (bloco branco na moto do
     Excitebike, lixo no ceu; boot alternava limpo/sujo = race de fase).
     band_frames so' comeca a avancar DEPOIS do GO (upload de CHR concluido),
     entao `!= 0` segura a primeira escrita ate' a janela critica fechar.
     As LEITURAS SPI acima sao config-bus puro (nao tocam a PSRAM) e podem
     rodar incondicionais.  Residual aceitavel: band_frames e' 16 bits e
     wrapa -- 1 frame "==0" a cada 65536 (~18min a 60fps) pula um publish;
     como nesse ponto o DMA de boot ja' passou ha' muito, pular a escrita
     (nunca arriscar corromper) e' o lado seguro do erro. */
  if(d.band_frames != 0) {
    /* bounded (latch nes_fpga_err): FPGA morto -> no-op, laco segue vivo */
    nes_sram_writeblock_to(&d, NES_PSRAM_NDBG_ADDR, sizeof(d));
  }
}

/* Stream de uma janela [file_off, file_off+len) do arquivo aberto pra PSRAM
   em psram_addr.  Byte-a-byte via SPI (padrao do load_sram): o sd_offload DMA
   exige leituras alinhadas a bloco de 512B e o payload iNES comeca no offset
   16 (ou 528 com trainer) -- desalinhado.  Bounded: len e' finito, cada
   f_read/f_lseek retorna erro em vez de pendurar, e toda espera de MCU_RDY
   e' FPGA_WAIT_RDY_TO com o latch nes_fpga_err (anti-wedge acima); um soluco
   de SD ou um timeout do FPGA apenas encerram o stream (contagem real vai
   pro breadcrumb/log).  ~1MB leva poucos segundos, aceitavel pra Fase 0
   (otimizar = fase futura). */
static uint32_t nes_stream_window(uint32_t file_off, uint32_t len, uint32_t psram_addr) {
  UINT bytes_read;
  uint32_t remaining = len;
  uint32_t done = 0;

  if(nes_fpga_err) return 0;
  file_res = f_lseek(&file_handle, file_off);
  if(file_res || file_handle.fptr != file_off) return 0;
  nes_set_mcu_addr_to(psram_addr);
  while(remaining && !nes_fpga_err) {
    bytes_read = file_read();
    if(file_res || !bytes_read) break;
    if(bytes_read > remaining) bytes_read = remaining;
    FPGA_SELECT();
    FPGA_TX_BYTE(0x98);   /* WRITE with autoincrement */
    for(UINT j = 0; j < bytes_read; j++) {
      FPGA_TX_BYTE(file_buf[j]);
      FPGA_WAIT_RDY_TO_INLINE(nes_fpga_err);
      if(nes_fpga_err) break;
    }
    FPGA_DESELECT();
    if(nes_fpga_err) break;
    remaining -= bytes_read;
    done += bytes_read;
    nes_dbg_progress(bytes_read);
  }
  return done;
}

/* Fase 1c: converte a CHR-ROM que acabou de ser escrita em 0x200000 (formato
   NES nativo) pras duas regioes pre-convertidas que o renderer 65816 vai
   consumir (0x500000 2bpp/BG, 0x600000 4bpp/OBJ).  Contrato byte-a-byte:
   nes_chr.h (== utils/nes_chr_convert.py, validado pelo teste host
   tests/host/run_nes_chr.sh).

   Fonte = PSRAM 0x200000 (RE-LEITURA), nao um segundo pass pelo cartao SD:
   o offset da CHR dentro do arquivo .nes (prg_offset + prgsize_bytes) nao e'
   necessariamente alinhado a bloco de 512B do SD, entao os chunks que
   file_read() devolve NAO caem em fronteira de tile de 16B -- precisaria
   de um "carry" de bytes entre chunks pra converter em linha (streaming) com
   o stream cru.  A regiao 0x200000, ao contrario, comeca EXATAMENTE no
   tile 0 (nes_stream_window escreveu a CHR inteira ali, contigua, sem gap
   inicial), entao ler de volta de la e' trivialmente tile-alinhado e reusa
   sram_readblock/sram_writeblock (os mesmos helpers que o resto do firmware
   ja usa pra PSRAM<->MCU, ver memory.c) em vez de reabrir/re-seekar o
   arquivo .nes.  Mais simples E nao reonera o SD com uma segunda leitura.

   Custo: 1 sram_readblock(16B) + 2 sram_writeblock(16B+32B) por tile, i.e.
   4x os bytes/tile do stream cru original (16B) -- mesmo mecanismo
   byte-a-byte com FPGA_WAIT_RDY ja aceito p/ o stream cru ("~1MB leva
   poucos segundos", nes_stream_window acima); a conversao adiciona ~4x esse
   tempo, mas SO para os bytes de CHR de fato convertidos (nao PRG). Pra uma
   CHR tipica (8-256KB / 512-16384 tiles) fica em ~0.1-4s; o pior caso
   teorico (1MB / 65536 tiles, acima de qualquer jogo real dos mappers v0)
   fica em torno de ~15-20s -- aceitavel pro load (roda 1x por boot de jogo,
   nao em loop) mas documentado aqui pra o caso de precisar de um orcamento
   mais apertado numa fase futura (a saida seria um copier PSRAM->PSRAM no
   FPGA, como o das BPS do core `base` -- fora de escopo
   desta fase, que e' MCU-only, sem tocar verilog/).

   Bounded: a contagem de tiles vem de bytes REALMENTE streamados
   (chr_done, nao o tamanho declarado no header) -- um stream curto (soluco
   de SD) trunca a conversao no ultimo tile completo, nunca le/escreve
   lixo; o loop e' um `for` de contagem fixa, sem dependencia de dado
   externo durante a iteracao; todo acesso a PSRAM usa os wrappers bounded
   (nes_sram_*_to, anti-wedge acima) -- um timeout do FPGA encerra o loop
   no tile corrente em vez de pendurar a MCU.

   Feedback de fase (v2.0b): o loop chama nes_dbg_conv_progress() (espelho
   de nes_dbg_progress(), estado PROPRIO nes_dbg_conv_*) e grava marcos
   "CONVERT_25/50/75/100%" no nesdbg.log -- antes so' havia "POST_STREAM"
   antes e "POST_CONVERT" depois de TODO o loop, deixando ate' ~4s (pior
   caso realista dos mappers v0, MMC1 128KB/8192 tiles) sem nenhum marco
   intermediario. Custo desprezivel (4 f_sync extras no pior caso) frente
   as ~8192*3=24576 transacoes SPI byte-a-byte do proprio loop. */
static uint32_t nes_convert_chr(uint32_t chr_done) {
  uint32_t tiles = chr_done / NES_CHR_TILE_BYTES;
  uint32_t t;
  uint8_t nes_tile[NES_CHR_TILE_BYTES];
  uint8_t snes2[NES_CHR_TILE_SNES2_BYTES];
  uint8_t snes4[NES_CHR_TILE_SNES4_BYTES];

  /* feedback de fase (v2.0b, ver nes_dbg_conv_progress) -- estado proprio,
     reiniciado a cada chamada (1x por load, chamada unica em nes_load_prg). */
  nes_dbg_conv_total   = chr_done;
  nes_dbg_conv_done    = 0;
  nes_dbg_conv_quarter = 1;

  for(t = 0; t < tiles && !nes_fpga_err; t++) {
    uint32_t nes_off = t * NES_CHR_TILE_BYTES;
    nes_sram_readblock_to(nes_tile, NES_PSRAM_CHR_ADDR + nes_off, NES_CHR_TILE_BYTES);
    if(nes_fpga_err) break;            /* tile lido pela metade: nao gravar lixo */
    nes_chr_tile_to_snes2(nes_tile, snes2);
    nes_chr_tile_to_snes4(nes_tile, snes4);
    nes_sram_writeblock_to(snes2, NES_PSRAM_CHR_SNES2_ADDR + nes_off,
                           NES_CHR_TILE_SNES2_BYTES);
    nes_sram_writeblock_to(snes4, NES_PSRAM_CHR_SNES4_ADDR + t * NES_CHR_TILE_SNES4_BYTES,
                           NES_CHR_TILE_SNES4_BYTES);
    nes_dbg_conv_progress(NES_CHR_TILE_BYTES);
  }
  return t * NES_CHR_TILE_BYTES;   /* bytes de CHR NES efetivamente cobertos */
}

/* Breadcrumb "NESL" -- gate de verificacao da Fase 0/1c, lido por USB
   (usb_read.py GET space=SNES @ 0x400000).  Layout documentado no
   NES-CORE-CONTRACT.md; manter em lockstep.  v2 (Fase 1c) SO ACRESCENTA
   campos no fim (+34..+51); o prefixo de 34 bytes da v1 e' byte-a-byte
   identico -- um leitor v1 continua lendo os primeiros 34 bytes certos. */
typedef struct __attribute__ ((__packed__)) _nes_breadcrumb {
  uint8_t  magic[4];        /* +0  "NESL" */
  uint8_t  version;         /* +4  NES_BREADCRUMB_VERSION */
  uint8_t  mapper_id;       /* +5  */
  uint8_t  prg_size_class;  /* +6  */
  uint8_t  chr_size_class;  /* +7  */
  uint32_t prgsize_bytes;   /* +8  LE, tamanho declarado no header */
  uint32_t chrsize_bytes;   /* +12 LE */
  uint8_t  has_chr_ram;     /* +16 */
  uint8_t  mirror_vertical; /* +17 */
  uint8_t  four_screen;     /* +18 */
  uint8_t  has_battery;     /* +19 */
  uint8_t  has_trainer;     /* +20 */
  uint8_t  is_nes20;        /* +21 */
  uint16_t mapper_flags16;  /* +22 LE, a palavra exata enviada via CHIPFEAT */
  uint32_t prg_streamed;    /* +24 LE, bytes realmente escritos em 0x000000 */
  uint32_t chr_streamed;    /* +28 LE, bytes realmente escritos em 0x200000 */
  uint8_t  supported;       /* +32 */
  uint8_t  stream_ok;       /* +33 1 = PRG e CHR streamados por completo */
  /* --- v2 (Fase 1c): CHR pre-convertida p/ SNES --- */
  uint32_t chr_snes2_addr;  /* +34 LE, NES_PSRAM_CHR_SNES2_ADDR (0x500000) */
  uint32_t chr_snes2_bytes; /* +38 LE, bytes realmente escritos em chr_snes2_addr (tiles*16) */
  uint32_t chr_snes4_addr;  /* +42 LE, NES_PSRAM_CHR_SNES4_ADDR (0x600000) */
  uint32_t chr_snes4_bytes; /* +46 LE, bytes realmente escritos em chr_snes4_addr (tiles*32) */
  uint8_t  chr_ram;         /* +50 1 = CHR-RAM (chr=0 no header); 0x500000/0x600000 NAO tocadas */
  uint8_t  chr_converted;   /* +51 1 = conversao rodou e cobriu TODO o chr_streamed; 0 = CHR-RAM ou stream curto */
} nes_breadcrumb_t;

uint32_t nes_load_prg(uint8_t *nes_filename) {
  if (!nes_romprops.has_nes) return 0;

  uint32_t prg_offset = sizeof(nes_header_t)
                        + (nes_romprops.has_trainer ? 512 : 0);
  uint32_t chr_offset = prg_offset + nes_romprops.prgsize_bytes;
  uint32_t prg_done = 0, chr_done = 0, chr_conv_done = 0;

  /* fpga_pgm falhou (PGM_FAIL latched em nes_dbg_post_pgm)?  Nao ha' core
     NES no FPGA -> qualquer FPGA_WAIT_RDY seria o wedge.  Pula tudo limpo;
     o log ja' diz o porque. */
  if(nes_fpga_err) {
    nes_dbg_log("SKIP_LOAD (pgm failed, nothing streamed)");
    return 0;
  }

  /* progresso p/ os marcos STREAM_xx% */
  nes_dbg_total   = nes_romprops.prgsize_bytes + nes_romprops.chrsize_bytes;
  nes_dbg_done    = 0;
  nes_dbg_quarter = 1;

  printf("attempting to load NES ROM %s...\n", nes_filename);
  file_open(nes_filename, FA_READ);
  if(file_res) {
    printf("nes_load_prg: could not open %s, res=%d\n", nes_filename, file_res);
    nes_dbg_log("PRE_STREAM open FAILED");
    return 0;
  }
  nes_dbg_log("PRE_STREAM");

  /* PRG -> 0x000000 (formato nativo NES; nada de conversao aqui) */
  prg_done = nes_stream_window(prg_offset, nes_romprops.prgsize_bytes,
                               NES_PSRAM_PRG_ADDR);
  /* CHR-ROM -> 0x200000 em formato NES nativo; CHR-RAM: nada a copiar, zera
     os 8KB iniciais p/ determinismo E as janelas convertidas 0x500000/0x600000
     (o boot do renderer DMAa as duas -- sem o memset o jogo herdaria a CHR
     convertida do jogo anterior; os tiles vivos chegam pelo CMD_CHR_RUN). */
  if(nes_romprops.chrsize_bytes) {
    chr_done = nes_stream_window(chr_offset, nes_romprops.chrsize_bytes,
                                 NES_PSRAM_CHR_ADDR);
  } else {
    nes_sram_memset_to(NES_PSRAM_CHR_ADDR, 8192, 0x00);
    /* CHR-RAM: as janelas de CHR pre-convertida ficam sem conversao (nao ha
     * CHR-ROM), mas o boot do renderer DMAa as duas -- sem zera-las aqui o
     * jogo herda a CHR convertida do jogo ANTERIOR na VRAM. */
    nes_sram_memset_to(NES_PSRAM_CHR_SNES2_ADDR,  8192, 0x00);
    nes_sram_memset_to(NES_PSRAM_CHR_SNES4_ADDR, 16384, 0x00);
  }
  file_close();

  if(nes_fpga_err) {
    nes_dbg_log("FPGA_TIMEOUT during STREAM (MCU_RDY never came back)");
  } else {
    char m[48];
    snprintf(m, sizeof(m), "POST_STREAM prg=%lu chr=%lu",
             (unsigned long)prg_done, (unsigned long)chr_done);
    nes_dbg_log(m);
  }

  /* Fase 1c: pre-converte a CHR-ROM (nao CHR-RAM) pras duas regioes SNES
     que o renderer consome.  So' roda p/ CHR-ROM -- ver nes_convert_chr(). */
  if(!nes_romprops.has_chr_ram && chr_done) {
    chr_conv_done = nes_convert_chr(chr_done);
  }

  if(nes_fpga_err) {
    nes_dbg_log("FPGA_TIMEOUT during CONVERT");
  } else {
    char m[48];
    snprintf(m, sizeof(m), "POST_CONVERT tiles=%lu%s",
             (unsigned long)(chr_conv_done / NES_CHR_TILE_BYTES),
             nes_romprops.has_chr_ram ? " (chr-ram: skipped)" : "");
    nes_dbg_log(m);
  }

  /* regioes do proprio core, zeradas p/ boot deterministico (baratas):
     CIRAM 2KB, CPU-RAM 2KB, CART-RAM 8KB */
  nes_sram_memset_to(NES_PSRAM_CIRAM_ADDR,   2048, 0x00);
  nes_sram_memset_to(NES_PSRAM_WRAM_ADDR,    2048, 0x00);
  nes_sram_memset_to(NES_PSRAM_CARTRAM_ADDR, 8192, 0x00);

  /* breadcrumb de layout (criterio do gate da Fase 0/1c) */
  nes_breadcrumb_t bc;
  bc.magic[0] = 'N'; bc.magic[1] = 'E'; bc.magic[2] = 'S'; bc.magic[3] = 'L';
  bc.version         = NES_BREADCRUMB_VERSION;
  bc.mapper_id       = nes_romprops.mapper_id;
  bc.prg_size_class  = nes_romprops.prg_size_class;
  bc.chr_size_class  = nes_romprops.chr_size_class;
  bc.prgsize_bytes   = nes_romprops.prgsize_bytes;
  bc.chrsize_bytes   = nes_romprops.chrsize_bytes;
  bc.has_chr_ram     = nes_romprops.has_chr_ram;
  bc.mirror_vertical = nes_romprops.mirror_vertical;
  bc.four_screen     = nes_romprops.four_screen;
  bc.has_battery     = nes_romprops.has_battery;
  bc.has_trainer     = nes_romprops.has_trainer;
  bc.is_nes20        = nes_romprops.is_nes20;
  bc.mapper_flags16  = nes_romprops.mapper_flags16;
  bc.prg_streamed    = prg_done;
  bc.chr_streamed    = chr_done;
  bc.supported       = nes_romprops.supported;
  bc.stream_ok       = (prg_done == nes_romprops.prgsize_bytes)
                       && (chr_done == nes_romprops.chrsize_bytes);
  bc.chr_snes2_addr  = NES_PSRAM_CHR_SNES2_ADDR;
  bc.chr_snes2_bytes = chr_conv_done;
  bc.chr_snes4_addr  = NES_PSRAM_CHR_SNES4_ADDR;
  bc.chr_snes4_bytes = chr_conv_done * 2;
  bc.chr_ram         = nes_romprops.has_chr_ram;
  bc.chr_converted   = (!nes_romprops.has_chr_ram) && (chr_conv_done == chr_done)
                       && (chr_conv_done == nes_romprops.chrsize_bytes);
  /* bounded: com o FPGA morto (latch setado) vira no-op -- o registro que
     sobrevive nesse caso e' o nesdbg.log no SD, nao a PSRAM */
  nes_sram_writeblock_to(&bc, NES_PSRAM_BREADCRUMB_ADDR, sizeof(bc));

  printf("NES:  PRG %ld/%ld  CHR %ld/%ld  CHR->SNES %ld/%ld tiles  breadcrumb @0x%06lx (%s, %s)\n",
         prg_done, nes_romprops.prgsize_bytes,
         chr_done, nes_romprops.chrsize_bytes,
         (unsigned long)(chr_conv_done / NES_CHR_TILE_BYTES),
         (unsigned long)(chr_done / NES_CHR_TILE_BYTES),
         (unsigned long)NES_PSRAM_BREADCRUMB_ADDR,
         bc.stream_ok ? "ok" : "STREAM SHORT",
         nes_romprops.has_chr_ram ? "chr-ram, no convert"
                                   : (bc.chr_converted ? "converted" : "convert SHORT"));
  return prg_done + chr_done;
}

#else /* CONFIG_MK2: stubs no-op -- suporte NES e' mk3-only (sem fpga_nes.bit
         pro Spartan-3 e a flash de 128KB do LPC1754 nao paga o loader) */

void nes_id(nes_romprops_t* props, uint8_t *filename) {
  (void)filename;
  props->has_nes = 0;
  props->error = MENU_ERR_OK;
  props->error_param = NULL;
}

uint8_t nes_update_file(uint8_t **filename_ref) {
  (void)filename_ref;
  return 1;
}

uint8_t nes_update_romprops(snes_romprops_t *romprops, uint8_t *nes_filename) {
  (void)romprops; (void)nes_filename;
  return 1;
}

uint32_t nes_load_prg(uint8_t *nes_filename) {
  (void)nes_filename;
  return 0;
}

void nes_dbg_log(const char *tag) {
  (void)tag;
}

void nes_dbg_post_pgm(const uint8_t *conf) {
  (void)conf;
}

void nes_dbg_publish(void) {
}

#endif /* CONFIG_MK2 */
