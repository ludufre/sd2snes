#!/usr/bin/env python3
"""
Rewrite the english strings in snes/const.a65 to Brazilian Portuguese.

Accented chars are encoded as raw byte values that match the slots added
to font.a65 by fontedit.py addaccents:

    á=130 à=131 â=132 ã=133 é=134 ê=135 í=136 ó=137 ô=138 õ=139 ú=140 ç=141
    Á=142 À=143 Â=144 Ã=145 É=146 Ê=147 Í=148 Ó=149 Ô=150 Õ=151 Ú=152 Ç=153

Each `.byt` directive that we want to keep building must end with `, 0`
to preserve the null terminator.
"""

import re
from pathlib import Path

CONST = Path(__file__).resolve().parent.parent / "const.a65"

ACCENTS = {
    "á": 130, "à": 131, "â": 132, "ã": 133, "é": 134, "ê": 135,
    "í": 136, "ó": 137, "ô": 138, "õ": 139, "ú": 140, "ç": 141,
    "Á": 142, "À": 143, "Â": 144, "Ã": 145, "É": 146, "Ê": 147,
    "Í": 148, "Ó": 149, "Ô": 150, "Õ": 151, "Ú": 152, "Ç": 153,
}

# label -> (translated text, optional trailing fragments preserved verbatim)
# A None translation means "leave alone".
#
# Special placeholders inside the text:
#   "{129}" -> insert raw byte 129 (sub-menu icon)
#   "{127}{128}" -> insert "..." control bytes
TRANSLATIONS = {
    "text_clkset":                                "Ajuste a hora",
    "text_mainmenu":                              "Menu Principal",
    "text_mm_file":                               "Navegador de Arquivos",
    "text_mm_last":                               "Jogos Recentes",
    "text_mm_favorites":                          "Jogos Favoritos",
    "mtext_mm_cfg":                               "Configurações",
    "mtext_mm_sysinfo":                           "Informações do Sistema",
    "mtext_cfg_time":                             "Ajustar Relógio",
    "mtext_cfg_bsx":                              "Opções BS-X",
    "mtext_cfg_browser":                          "Opções do Navegador",
    "mtext_cfg_chip":                             "Opções de Chip",
    "mtext_cfg_sgb":                              "Opções SGB",
    "mtext_cfg_ingame":                           "Opções no Jogo",
    "mtext_cfg_savestates":                       "Opções de Savestates",
    "mtext_cfg_scic":                             "Opções SuperCIC",
    "mtext_bsx_timesource":                       "Relógio BS-X",
    "mtext_bsx_time":                             "Hora customizada",
    "mtext_scic_enable":                          "Ativar SuperCIC",
    "mtext_scic_vmode_menu":                      " {129}Modo vídeo do menu",
    "mtext_scic_vmode_game":                      " {129}Modo vídeo do jogo",
    "mtext_browser_sort":                         "Ordenar diretórios",
    "mtext_browser_hide_ext":                     "Ocultar extensões",
    "mtext_browser_screensaver":                  "Protetor de tela",
    "mtext_browser_screensaver_timeout":          "Escurecer tela após",
    "mtext_browser_ledbright":                    "Brilho dos LEDs",
    "mtext_ingame_cheats":                        "Iniciar com trapaças",
    "mtext_ingame_shortreset":                    "Reset para o menu",
    "mtext_ingame_enable":                        "Hook no jogo",
    "mtext_ingame_buttons":                       " {129}Botões no jogo",
    "mtext_ingame_holdoff":                       " {129}Espera inicial",
    "mtext_ingame_savestate":                     "Savestates no jogo",
    "mtext_savestate_slots":                      " {129}Slots de savestate",
    "mtext_loadstate_delay":                      " {129}Atraso ao carregar",
    "mtext_sgb_enable_hooks":                     "Hooks no jogo",
    "mtext_sgb_enable_state":                     "Savestates (XR, XL)",
    "mtext_sgb_enh_override":                     "Recursos do SGB",
    "mtext_sgb_spr_increase":                     "Limite máx. sprites (40)",
    "mtext_sgb_volume_boost":                     "Boost de volume",
    "mtext_sgb_clock":                            "Clock (Timing)",
    "mtext_sgb_bios":                             "Versão do BIOS",
    "mtext_ingame_regionpatch":                   "Patch auto. de região",
    "mtext_ingame_1chiptransientfixes":           "Correções 1CHIP",
    "mtext_ingame_brightlimit":                   "Limite de brilho",
    "mtext_ingame_resetpatch":                    "Patch de reset (clock)",
    "mtext_ingame_autosave":                      "Auto-salvar",
    "mtext_ingame_autosave_msu1":                 " {129}Auto-salvar MSU-1",
    "mtext_chip_cx4_speed":                       "Velocidade Cx4",
    "mtext_chip_gsu_speed":                       "Velocidade SuperFX",
    "mtext_chip_msu1_volume_boost":               "Boost volume MSU-1",

    "mdesc_mm_last":                              "Mostra até 10 jogos recentes",
    "mdesc_mm_favorites":                         "Mostra até 10 jogos favoritos",
    "mdesc_mm_cfg":                               "Configurar o sd2snes",
    "mdesc_mm_sysinfo":                           "Info de FW, SD, SNES, CIC{127}{128}",
    "mdesc_cfg_time":                             "Ajustar data e hora",
    "mdesc_cfg_bsx":                              "Opções específicas BS-X/Satellaview",
    "mdesc_cfg_browser":                          "Alterar opções do navegador",
    "mdesc_cfg_chip":                             "Recursos especiais de chip",
    "mdesc_cfg_ingame":                           "Recursos durante o jogo",
    "mdesc_cfg_sgb":                              "Recursos do Super Game Boy",
    "mdesc_cfg_savestates":                       "Recursos de savestates",
    "mdesc_cfg_scic":                             "Ativar SuperCIC e modos de vídeo",
    "mdesc_bsx_timesource":                       "Data/hora vistas pelos jogos BS-X",
    "mdesc_bsx_time":                             "Hora e data customizadas para BS-X",
    "mdesc_scic_enable":                          "Necessário para recursos SuperCIC",
    "mdesc_scic_vmode_menu":                      "Modo de vídeo do menu",
    "mdesc_scic_vmode_game":                      "Modo de vídeo dos jogos",
    "mdesc_browser_sort":                         "Ordenar listas de arquivos",
    "mdesc_browser_hide_ext":                     "Ocultar extensões de arquivos",
    "mdesc_browser_screensaver":                  "Escurecer a tela após um tempo",
    "mdesc_browser_screensaver_timeout":          "Tempo para escurecer a tela",
    "mdesc_browser_ledbright":                    "Alterar brilho dos LEDs",
    "mdesc_ingame_cheats":                        "Iniciar com trapaças ativadas",
    "mdesc_ingame_shortreset":                    "Volta ao menu com reset curto",
    "mdesc_ingame_enable":                        "Necessário para botões e trapaças 7E/7F",
    "mdesc_ingame_buttons":                       "Botões no jogo (reset, trapaças{127}{128}). Requer hook ativo.",
    "mdesc_ingame_holdoff":                       "Espera 10s antes de ativar o hook",
    "mdesc_ingame_savestate":                     "Savestates no jogo. Padrão: Salvar Start+L, Carregar Start+R",
    "mdesc_savestate_slots":                      "Slots de savestate. Select+Dpad escolhe o slot",
    "mdesc_loadstate_delay":                      "Ajustar atraso ao carregar state (frames)",
    "mdesc_sgb_enable_hooks":                     "Necessário para botões no jogo",
    "mdesc_sgb_enable_state":                     "Savestates internos no SGB2 (XR, XL)",
    "mdesc_sgb_enh_override":                     "Ativar/desativar recursos SGB",
    "mdesc_sgb_spr_increase":                     "Aumentar número de sprites visíveis",
    "mdesc_sgb_volume_boost":                     "Aumentar volume do áudio SGB",
    "mdesc_sgb_clock":                            "Ajustar clock do SGB (timing)",
    "mdesc_sgb_bios":                             "Selecionar versão de BIOS do SGB",
    "mdesc_ingame_regionpatch":                   "Ignora proteção simples de região",
    "mdesc_ingame_1chiptransientfixes":           "Corrige glitches de brilho em 1CHIP/Jr.",
    "mdesc_ingame_brightlimit":                   "Limitar brilho geral (RGB em 1CHIP)",
    "mdesc_ingame_resetpatch":                    "Patch de reset para alinhar clocks",
    "mdesc_ingame_autosave":                      "Salvar SRM automaticamente quando muda",
    "mdesc_ingame_autosave_msu1":                 "MSU-1: salva SRM em pausas. Pode travar.",
    "mdesc_chip_cx4_speed":                       "Velocidade do soft core Cx4",
    "mdesc_chip_gsu_speed":                       "Velocidade do soft core SuperFX",
    "mdesc_chip_msu1_volume_boost":               "Boost de volume MSU-1 (Rev. E-G)",
    "mdesc_cx4_speed_fast":                       "Rodar o mais rápido possível",
    "mdesc_cx4_speed_normal":                     "Rodar aprox. na velocidade original",
    "mdesc_gsu_speed_fast":                       "Rodar o mais rápido possível",
    "mdesc_gsu_speed_normal":                     "Rodar aprox. na velocidade original",

    "text_filesel_selected_file":                 "Arquivo selecionado",
    "text_filesel_context_add_to_favorites":      "Adicionar aos favoritos",
    "mdesc_filesel_context_add_to_favorites":     "Adiciona o arquivo aos favoritos",
    "text_filesel_context_remove_from_favorites": "Remover dos favoritos",
    "mdesc_filesel_context_remove_from_favorites":"Remove o arquivo dos favoritos",
    "text_recent_context_remove_from_recent":     "Remover dos recentes",
    "mdesc_recent_context_remove_from_recent":    "Remove o arquivo dos jogos recentes",
    "text_filesel_set_as_autoboot":               "Definir como autoboot",
    "mdesc_filesel_set_as_autoboot":              "Inicia esta ROM no próximo power-on (START cancela)",

    "text_statusbar_play":                        "A:Jogar B:Voltar X:Menu",
    "text_statusbar_keys":                        "A:OK B:Voltar X:Menu Y:Contexto",
    "text_statusbar_spc":                         "B:Voltar",
    "text_statusbar_menu":                        "A:OK B:Voltar",
    "text_last":                                  "Jogos recentes",
    "text_favorite":                              "Jogos favoritos",

    "text_on_p1":                                 "Lig: P1",
    "text_on_p2":                                 "Lig: P2",
    "text_on":                                    "Lig",
    "text_off":                                   "Des",
    "text_enabled":                               "Ativado",
    "text_disabled":                              "Desativ.",
    "text_normal":                                "Normal",
    "text_fast":                                  "Rápido",
    "text_rtc":                                   "Tempo real",
    "text_custom":                                "Custom.",
    "text_yes":                                   "Sim",
    "text_no":                                    "Não",
    "text_auto":                                  "Auto",

    "text_add":                                   "Adicionar",
    "text_remove":                                "Remover",
    "text_edit":                                  "Editar",
    "text_save":                                  "Salvar",

    "text_1min":                                  "1 min",
    "text_2min":                                  "2 mins",
    "text_3min":                                  "3 mins",
    "text_4min":                                  "4 mins",
    "text_5min":                                  "5 mins",
    "text_10min":                                 "10 mins",
    "text_15min":                                 "15 mins",
    "text_20min":                                 "20 mins",
    "text_30min":                                 "30 mins",
    "text_45min":                                 "45 mins",
    "text_60min":                                 "1 hora",

    "text_spcplay":                               "Tocador de SPC",
    "text_spcload":                               "Carregando SPC no SPC700{127}{128}",
    "text_spcstarta":                             "**** Tocando SPC ****",
    "text_spcstartb":                             "Nome:  ",
    "text_spcstartc":                             "Música:",
    "text_spcstartd":                             "Artista:",
    "text_spc_statusbar":                         "B:Voltar",
    "text_loading":                               "Carregando{127}{128}                  ",
    "text_saving":                                "Salvando{127}{128}                    ",
    "text_cheat":                                 "Trapaças para ",
    "cheat_tab_head":                             "Nome                         # códigos   Ativado",
    "cheat_detail_head":                          "PAR       GG        Endereço Valor",
}


def encode_string(text):
    """Convert PT-BR string with accents and {N} placeholders into a
    sequence of `.byt` arguments."""
    pieces = []
    current = ""
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "{":
            end = text.index("}", i)
            if current:
                pieces.append(f'"{current}"')
                current = ""
            pieces.append(text[i+1:end])
            i = end + 1
            continue
        if ch in ACCENTS:
            if current:
                pieces.append(f'"{current}"')
                current = ""
            pieces.append(str(ACCENTS[ch]))
        else:
            current += ch
        i += 1
    if current:
        pieces.append(f'"{current}"')
    pieces.append("0")
    return ", ".join(pieces)


# Match `LABEL .byt "...", 0` (and variants with embedded commas/byte values).
LINE_RE = re.compile(r'^(\s*)(\S+)(\s+\.byt\s+)(.*)$')


def main():
    out = []
    for line in CONST.read_text().splitlines():
        m = LINE_RE.match(line)
        if not m:
            out.append(line); continue
        indent, label, prefix, rest = m.groups()
        if label in TRANSLATIONS and TRANSLATIONS[label] is not None:
            new_rest = encode_string(TRANSLATIONS[label])
            out.append(f"{indent}{label}{prefix}{new_rest}")
        else:
            out.append(line)
    CONST.write_text("\n".join(out) + "\n")
    print(f"Translated {sum(1 for v in TRANSLATIONS.values() if v)} strings into {CONST}")


if __name__ == "__main__":
    main()
