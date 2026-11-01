#!/usr/bin/env python3
"""Generate onb_const_lang.a65 -- the onboarding ROM's i18n string pool.

Self-contained generator (the onboarding is a separate image, so it has its own
strtab independent of the menu's const_lang*.a65). Mirrors the menu's resolve_str
dispatch idea: each localized label is a ROW of N long pointers (word + bank) living
between `onb_strtab_lo` and `onb_strtab_hi`; onb_resolve_str (onb_ui.a65) recognizes a
pointer in that range and swaps in the column of onb_cur_lang (the menu's language
index, 10 columns). The tables live in a picture bank of their own (onb_const_lang_t.a65,
TABLE_BANK). Each string is placed on its own, first fit, in the banks with room
(STR_BANKS / STR_OTHER), one object per bank (str_file). Accent/Cyrillic encoding is
build_const.encode_string's; a character the font has no tile for aborts the build
(_cells), and so does a string wider than the cells the engine prints it with (_budget).

CONTENT MODEL: per feature, name + desc (2 lines) + howto, shown in ONE language
(onb_cur_lang, cycled by X). The rest of a card (its answer kind, CFG byte, demo pictures) lives in
onboarding_const.a65 ($C0).

Usage: gen_onb_lang.py -o onb_const_lang.a65
"""
import sys, os, json, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_const as bc
from build_const import encode_string, ENCODE

# Column order == onb_cur_lang index == the menu's CFG_LANGUAGE. The CJK languages come
# last (build_const.CJK_LANGS): their text is drawn from a glyph cache on BG2 (onb_cjk.a65).
LANGS = ["en", "ptbr", "es", "de", "fr", "it", "ru", "nl", "ja", "zh"]
CJK_LANGS = ("ja", "zh")
if os.environ.get("ONB_CJK_ONLY"):          # a translator's check of one CJK column alone
    CJK_LANGS = (os.environ["ONB_CJK_ONLY"],)
    LANGS = LANGS[:8] + list(CJK_LANGS)

from pathlib import Path
from build_const import parse_base, load_dict, decode_args
_UTILS = Path(os.path.dirname(os.path.abspath(__file__)))
_EN = parse_base(_UTILS.parent / "const.a65")[1]
_DICTS = {l: load_dict(_UTILS / ("lang_%s.py" % l)) for l in LANGS[1:]}
# The tour's own words in a CJK language (onb_lang_<code>.json): "name_N" / "text_N" for card N,
# and the STRINGS labels. A key it lacks falls back to English.
_TOUR = {}
for _l in CJK_LANGS:
    _p = _UTILS / ("onb_lang_%s.json" % _l)
    _TOUR[_l] = json.loads(_p.read_text(encoding="utf-8")) if _p.exists() else {}


def _cjk_cols(key, vals):
    """vals in the 8 non-CJK languages -> all LANGS, the CJK columns from the tour dicts."""
    vals = tuple(vals)
    if len(vals) == len(LANGS):
        return vals
    return vals + tuple(_TOUR[l].get(key, vals[0]) for l in CJK_LANGS)


def menu_text(label):
    """A menu string in the 8 languages: English from const.a65, the rest from the
    menu's lang_*.py (English where a dict lacks it, as the menu build does)."""
    en = decode_args(_EN[label])
    return (en,) + tuple(_DICTS[l].get(label, en) for l in LANGS[1:])

# Width budget, in font cells (an accent or a Cyrillic letter is one cell) =
# the onb_print_count the engine prints each label with (onboarding_main.a65).
# name rows are cols 1..30; the paragraphs wrap at TEXT_W (below).
_BUDGET = {
    "onb_ui_enable": 10, "onb_ui_choose": 10, "onb_ui_next": 8,
    "onb_ui_more": 24, "onb_ui_pick": 8,
    # the card footer's B word sits at col 40 and START at 48: 7 cells, one blank
    "onb_ui_back": 7, "onb_ui_exit": 8,
    "onb_ui_menu": 8, "onb_ui_done_title": 20, "onb_ui_done_l1": 44, "onb_ui_done_l2": 44,
    "onb_ui_done_go": 24,
}
def _budget(label):
    if label in _BUDGET:
        return _BUDGET[label]
    if label.endswith("_name"):
        return 30
    return 24          # onb_text_*: an answer of the list, printed with 24 (cols 3..26)

# label -> (EN, PT-BR, ES, DE, FR, IT, RU, NL)
STRINGS = {
    # ---- option values: the MENU's own words (menu_text), so an answer reads the
    #      same here as on the Configuration row it sets ----
    "onb_text_yes":        menu_text("text_yes"),
    "onb_text_no":         menu_text("text_no"),
    # the tour's list has room the menu's value column does not: whole words, not "Des"/"Lig"
    "onb_text_off":        ("Off", "Desligado", "Desactivado", "Aus", "Désactivé", "Disattivato", "Выключено", "Uit"),
    "onb_text_on":         ("On", "Ligado", "Activado", "Ein", "Activé", "Attivato", "Включено", "Aan"),
    "onb_text_large":      menu_text("text_cover_large"),
    "onb_text_small":      menu_text("text_cover_small"),
    "onb_text_gi_ctx":     menu_text("text_gameinfo_context"),
    "onb_text_rst_menu":   ("Menu", "Menu", "Menú", "Menü", "Menu", "Menu", "Меню", "Menu"),
    "onb_text_rst_folder": menu_text("text_nav_folder"),
    "onb_text_rst_game":   menu_text("text_nav_rom"),
    "onb_text_rst_hold":   menu_text("text_nav_duration"),
    # the font-edge cards (Text outline / Text anti-aliasing): Theme / On / Off, the menu's
    # kv_text_edge order
    "onb_text_theme":      menu_text("text_theme"),

    # ---- shared UI ----
    "onb_ui_enable":    ("ENABLE?", "ATIVAR?", "¿ACTIVAR?", "AKTIV?", "ACTIVER ?", "ATTIVARE?", "ВКЛЮЧИТЬ?", "AANZETTEN?"),
    "onb_ui_choose":    ("CHOOSE", "ESCOLHA", "ELIGE", "WÄHLEN", "CHOIX", "SCEGLI", "ВЫБОР", "KIES"),
    "onb_ui_pick":      ("SELECT", "ESCOLHER", "ELEGIR", "WÄHLEN", "CHOISIR", "SCEGLI", "ВЫБОР", "KIEZEN"),
    "onb_ui_more":      ("SEE ALSO", "VEJA TAMBÉM", "VER TAMBIÉN", "SIEHE AUCH", "VOIR AUSSI", "VEDI ANCHE", "СМОТРИ ТАКЖЕ", "ZIE OOK"),
    "onb_ui_next":      ("NEXT", "AVANÇAR", "AVANZAR", "WEITER", "SUIVANT", "AVANTI", "ДАЛЕЕ", "VERDER"),
    "onb_ui_back":      ("BACK", "VOLTAR", "ATRÁS", "ZURÜCK", "RETOUR", "TORNA", "НАЗАД", "TERUG"),
    "onb_ui_exit":      ("EXIT", "SAIR", "SALIR", "ENDE", "QUITTER", "ESCI", "ВЫХОД", "STOPPEN"),
    "onb_ui_menu":      ("MENU", "MENU", "MENÚ", "MENÜ", "MENU", "MENU", "МЕНЮ", "MENU"),
    "onb_ui_done_title":("ALL SET!", "TUDO PRONTO!", "¡TODO LISTO!", "ALLES FERTIG!",
                         "C'EST PRÊT !", "TUTTO PRONTO!", "ВСЁ ГОТОВО!", "KLAAR!"),
    "onb_ui_done_l1":   ("Your choices will be saved.", "Suas escolhas serão salvas.",
                         "Tus elecciones se guardarán.", "Deine Auswahl wird gespeichert.",
                         "Tes choix seront enregistrés.", "Le tue scelte saranno salvate.",
                         'Ваши настройки будут сохранены.', "Je keuzes worden opgeslagen."),
    "onb_ui_done_l2":   ("Change them later in Configuration.", "Mude depois em Configurações.",
                         "Cámbialas luego en Configuración.", "Später in Einstellungen änderbar.",
                         "Modifie-les dans Configuration.", "Cambiale poi in Configurazione.",
                         'Вы можете изменить их в настройках sd2snes+.', "Wijzig ze later in Configuratie."),
    "onb_ui_done_go":   ("PRESS A FOR THE MENU", "APERTE A PARA O MENU", "PULSA A PARA EL MENÚ",
                         "A DRÜCKEN FÜRS MENÜ", "APPUIE SUR A : MENU", "PREMI A PER IL MENU",
                         "НАЖМИТЕ A ДЛЯ МЕНЮ", "DRUK OP A: MENU"),
}

STRINGS = {k: _cjk_cols(k, v) for k, v in STRINGS.items()}

# the Competition Cart round, one answer per value: 0..15 = 3..18 minutes (cfg.h cc_time_limit)
_MIN = {"ru": "мин", "ja": "分", "zh": "分钟"}
for _m in range(3, 19):
    STRINGS["onb_text_cc_%d" % _m] = tuple(("%d%s" if l in CJK_LANGS else "%d %s") % (_m, _MIN.get(l, "min"))
                                           for l in LANGS)

# The descriptors in tour order, by name: the base tour's cards (1-32, "and more" last),
# the 2.17, 2.18 and 2.19 sections (33-40, 41-55 and 56-60: each its separator card, then the release's
# cards), then the "and more" items (61-68, ONB_MORE_FIRST in onboarding_main.a65). Per language.
NAMES = [
  # 1 language
  ('Language', 'Idioma', 'Idioma', 'Sprache', 'Langue', 'Lingua', 'Язык', 'Taal'),
  # 2 themes: the sd2snes+ theme, a .thm from the list, restoring it or the classic one
  ('Themes', 'Temas', 'Temas', 'Themes', 'Thèmes', 'Temi', 'Темы', "Thema's"),
  # 3 text outline: theme / on / off
  ('Text outline', 'Contorno do texto', 'Contorno del texto', 'Textkontur', 'Contour du texte', 'Contorno del testo', 'Контур текста', 'Tekstcontour'),
  # 4 text anti-aliasing: theme / on / off
  ('Text anti-aliasing', 'Suavização do texto', 'Suavizado del texto', 'Kantenglättung', 'Lissage du texte', 'Anti-aliasing del testo', 'Сглаживание текста', 'Tekst anti-aliasing'),
  # 5 box art
  ('Box art in the list', 'Capas na lista', 'Carátulas en la lista', 'Cover in der Liste', 'Jaquettes dans la liste', 'Copertine nella lista', 'Обложки в списке', 'Hoezen in de lijst'),
  # 6 covers in the Recent / Favorites lists (needs covers on)
  ('Covers in Recent/Favorites', 'Capas em Recentes/Favoritos', 'Carátulas en las listas', 'Cover in Letzte/Favoriten', 'Jaquettes dans Récents/Favoris', 'Copertine in Recenti/Preferiti', 'Обложки в Недавние/Избранное', 'Hoezen in Recent/Favorieten'),
  # 7 game info card
  ('Game info card', 'Ficha do jogo', 'Ficha del juego', 'Spiel-Infokarte', 'Fiche du jeu', 'Scheda del gioco', 'Информация об игре', 'Spelinfo'),
  # 8 the video clip on the game info card (needs the card)
  ('Video in the game info', 'Vídeo na ficha', 'Vídeo en la ficha', 'Video in der Infokarte', 'Vidéo dans la fiche', 'Video nella scheda', 'Видео в карточке игры', 'Video in de spelinfo'),
  # 9 the clip's soundtrack (needs the video and the card)
  ('Music of the video', 'Música do vídeo', 'Música del vídeo', 'Musik des Videos', 'Musique de la vidéo', 'Musica del video', 'Музыка ролика', 'Muziek van de video'),
  # 10 menu music
  ('Menu music', 'Música do menu', 'Música del menú', 'Menümusik', 'Musique du menu', 'Musica del menu', 'Музыка меню', 'Menumuziek'),
  # 11 random music
  ('Random music', 'Música aleatória', 'Música aleatoria', 'Zufallsmusik', 'Musique aléatoire', 'Musica casuale', 'Случайная музыка', 'Willekeurige muziek'),
  # 12 menu sounds
  ('Menu sounds', 'Sons do menu', 'Sonidos del menú', 'Menü-Sounds', 'Sons du menu', 'Suoni del menu', 'Звуки меню', 'Menugeluiden'),
  # 13 MSU-1 folders
  ('MSU-1 folders', 'Pastas MSU-1', 'Carpetas MSU-1', 'MSU-1-Ordner', 'Dossiers MSU-1', 'Cartelle MSU-1', 'Папки MSU-1', 'MSU-1-mappen'),
  # 14 the .pcm track player of the file list
  ('MSU-1 track player', 'Tocador de trilhas MSU-1', 'Reproductor de pistas MSU-1', 'MSU-1-Track-Player', 'Lecteur de pistes MSU-1', 'Lettore tracce MSU-1', 'Плеер треков MSU-1', 'MSU-1-nummerspeler'),
  # 15 show the sd2snes folder in the file list
  ('Show sd2snes folder', 'Mostrar pasta sd2snes', 'Mostrar carpeta sd2snes', 'sd2snes-Ordner zeigen', 'Afficher le dossier sd2snes', 'Mostra cartella sd2snes', 'Показывать папку sd2snes', 'sd2snes-map tonen'),
  # 16 smart reset
  ('Smart reset', 'Reset inteligente', 'Reset inteligente', 'Smart-Reset', 'Reset intelligent', 'Reset intelligente', 'Умный сброс', 'Slimme reset'),
  # 17 the cheat list: where it opens, the code formats, the pages, the keys
  ('The cheat list', 'Lista de cheats', 'Lista de cheats', 'Cheat-Liste', 'Liste des cheats', 'Elenco dei cheat', 'Список чит-кодов', 'Cheatlijst'),
  # 18 in-game menu
  ('In-game menu', 'Menu in-game', 'Menú en el juego', 'Ingame-Menü', 'Menu en jeu', 'Menu in gioco', 'Меню в игре', 'In-game menu'),
  # 19 savestates
  ('Savestates', 'Savestates', 'Savestates', 'Savestates', 'Savestates', 'Savestate', 'Сейвстейты', 'Savestates'),
  # 20 4 battery saves per game, the in-game menu's SAVES tab (needs the in-game menu)
  ('4 saves per game', '4 saves por jogo', '4 partidas por juego', '4 Spielstände pro Spiel', '4 sauvegardes par jeu', '4 salvataggi per gioco', '4 сохранения на игру', '4 saves per spel'),
  # 21 the in-game menu's RAM trainer (needs the in-game menu)
  ('RAM trainer', 'Treinador de RAM', 'Entrenador de RAM', 'RAM-Trainer', 'Trainer RAM', 'Trainer RAM', 'Трейнер памяти', 'RAM-trainer'),
  # 22 IPS/BPS patches
  ('IPS/BPS patches', 'Patches IPS/BPS', 'Parches IPS/BPS', 'IPS/BPS-Patches', 'Patchs IPS/BPS', 'Patch IPS/BPS', 'IPS/BPS Патчи', 'IPS/BPS-patches'),
  # 23 creating the patched ROM from the [Y] menu of a patch
  menu_text("mtext_patch_create_rom"),
  # 24 more special chips (its own card)
  ('More special chips', 'Mais chips especiais', 'Más chips especiales', 'Weitere Spezialchips', 'Autres puces spéciales', 'Altri chip speciali', 'Другие специальные чипы', 'Meer speciale chips'),
  # 25 Sufami Turbo: the Slot B picker
  ('Sufami Turbo Slot B', 'Slot B do Sufami Turbo', 'Ranura B del Sufami Turbo', 'Sufami Turbo: Slot B', 'Sufami Turbo : port B', 'Slot B del Sufami Turbo', 'Слот "B" у Sufami Turbo', 'Sufami Turbo: slot B'),
  # 26 Competition Carts: the round's minutes (3..18, the events used 6)
  ('Competition Cart round', 'Round dos Competition Carts', 'Ronda de los Competition Carts', 'Runde der Competition Carts', 'Manche des Competition Carts', 'Round dei Competition Cart', 'Раунд в Competition Cart', 'Ronde van de Competition Carts'),
  # 27 other consoles: NES, Master System, Game Boy Color, Atari 2600
  ('Other consoles', 'Outros consoles', 'Otras consolas', 'Andere Konsolen', 'Autres consoles', 'Altre console', 'Другие консоли', 'Andere consoles'),
  # 28 the Atari 2600's console switches on the pad, and its picture width
  ('Atari 2600 controls', 'Controles do Atari 2600', 'Controles del Atari 2600', 'Atari-2600-Steuerung', 'Commandes Atari 2600', 'Comandi Atari 2600', 'Управление для Atari 2600', 'Atari 2600-bediening'),
  # 29 the card's folders: 2-letter buckets, console folders, Organize
  ('Folders on the card', 'Pastas no cartão', 'Carpetas en la tarjeta', 'Ordner auf der Karte', 'Dossiers sur la carte', 'Cartelle sulla scheda', 'Папки на карте памяти', 'Mappen op de kaart'),
  # 30 the memory test of the main menu
  ('Memory test', 'Teste de memória', 'Prueba de memoria', 'Speichertest', 'Test mémoire', 'Test di memoria', 'Тест памяти', 'Geheugentest'),
  # 31 the Mk.II LED codes for a boot without a picture
  ('Mk.II boot errors on the LED', 'Erros de boot no LED do Mk.II', 'Errores de arranque en el LED', 'Startfehler an der Mk.II-LED', 'LED du Mk.II au démarrage', 'Errori di avvio sul LED Mk.II', 'Индикация ошибок запуска Mk.II', 'Opstartfouten op de Mk.II-LED'),
  # 32 "and more" (the base tour's last card)
  ('And more', 'E mais', 'Y más', 'Und mehr', 'Et plus', 'E altro', 'И многое другое', 'En meer'),
  # 33 the 2.17 section: what is new in this release
  ("What's new in 2.17", 'Novidades da 2.17', 'Novedades de la 2.17', 'Neu in 2.17', 'Nouveautés de la 2.17', 'Novità della 2.17', 'Новое в 2.17', 'Nieuw in 2.17'),
  # 34 shortcuts/hooks on controller 2 (2.17)
  ('Controller 2 shortcuts/hooks',
   'Atalhos/hooks no controle 2',
   'Atajos/hooks en el mando 2',
   'Kürzel/Hooks auf Controller 2',
   'Raccourcis/hooks manette 2',
   'Scorciatoie/hook controller 2',
   'Комбо/перехваты: контроллер 2',
   'Sneltoetsen/hooks controller 2'),
  # 35 Game Boy Color (2.17)
  ('Game Boy Color',)*8,
  # 36 the in-game shortcut/hook list (2.17)
  ('Shortcut/hook list',
   'Lista de atalhos/hooks',
   'Lista de atajos/hooks',
   'Kürzel/Hook-Liste',
   'Liste des raccourcis/hooks',
   'Elenco scorciatoie/hook',
   'Список комбинаций/перехватов',
   'Sneltoetsen/hooks-lijst'),
  # 37 Seta chips and bootlegs (2.17)
  ('Seta chips and bootlegs', 'Chips Seta e bootlegs', 'Chips Seta y bootlegs', 'Seta-Chips und Bootlegs', 'Puces Seta et bootlegs', 'Chip Seta e bootleg', 'Чипы Seta, нелицензионные игры', 'Seta-chips en bootlegs'),
  # 38 Super 20 in 1, Gamars Puzzle and .sfrom (2.17)
  ('Super 20 in 1, Gamars, .sfrom',)*8,
  # 39 file-type icons in the list (2.17)
  ('Icons in the list', 'Ícones na lista', 'Iconos en la lista', 'Symbole in der Liste', 'Icônes dans la liste', 'Icone nella lista', 'Значки в списке', 'Pictogrammen in de lijst'),
  # 40 the cheat list from the game info card (2.17)
  ('Cheats from the game info', 'Cheats pela ficha do jogo', 'Cheats desde la ficha', 'Cheats aus der Infokarte', 'Cheats depuis la fiche', 'Cheat dalla scheda', 'Чит-коды из карточки игры', 'Cheats vanuit spelinfo'),
  # 41 the 2.18 section: what is new in this release
  ("What's new in 2.18", 'Novidades da 2.18', 'Novedades de la 2.18', 'Neu in 2.18', 'Nouveautés de la 2.18', 'Novità della 2.18', 'Новое в 2.18', 'Nieuw in 2.18'),
  # 42 the cartridge with an RP2040 chip (2.18)
  ('Xeno Crisis',)*8,
  # 43 the SNES Mouse in the menu (2.18)
  ('SNES Mouse',)*8,
  # 44 the music players: .pcm (seek, playlist) and .spc (2.18)
  ('Music players', 'Tocadores de música', 'Reproductores de música', 'Musik-Player', 'Lecteurs de musique', 'Lettori musicali', 'Музыкальные плееры', 'Muziekspelers'),
  # 45 the NES music (.nsf) player (2.18)
  ('NES music (.nsf)', 'Música de NES (.nsf)', 'Música de NES (.nsf)', 'NES-Musik (.nsf)', 'Musique NES (.nsf)', 'Musica del NES (.nsf)', 'Музыка NES (.nsf)', 'NES-muziek (.nsf)'),
  # 46 the saves screen: each save and savestate slot, the one the game starts with (2.18)
  ('Saves screen', 'Tela de saves', 'Pantalla de partidas', 'Spielstand-Übersicht', 'Écran des sauvegardes', 'Schermata salvataggi', 'Экран сохранений', 'Saves-overzicht'),
  # 47 the file manager: copy, cut, paste, rename, new folder, select several, view a text file (2.18)
  ('File manager', 'Gerenciador de arquivos', 'Gestor de archivos', 'Dateimanager', 'Gestionnaire de fichiers', 'Gestione file', 'Файловый менеджер', 'Bestandsbeheer'),
  # 48 delete a folder and all it holds (2.18)
  ('Delete folders', 'Excluir pastas', 'Borrar carpetas', 'Ordner löschen', 'Supprimer des dossiers', 'Eliminare cartelle', 'Удаление папок', 'Mappen verwijderen'),
  # 49 game options: settings for one game only, over the global ones, laid out like Settings (2.18)
  ('Game options', 'Opções do jogo', 'Opciones del juego', 'Spieloptionen', 'Options du jeu', 'Opzioni del gioco', 'Настройки игры', 'Spelopties'),
  # 50 Repeat in the .spc / .pcm / .nsf players (2.18)
  ('Repeat in the players', 'Repetir nos tocadores', 'Repetir en reproductores', 'Wiederholen in den Playern', 'Répéter dans les lecteurs', 'Ripeti nei lettori', 'Повтор в плеерах', 'Herhalen in de spelers'),
  # 51 stop the menu music on the game info card (2.18; needs the menu music)
  ('Stop music on game info', 'Parar música na ficha', 'Parar música en la ficha', 'Musik aus in der Infokarte', 'Couper la musique (fiche)', 'Ferma musica nella scheda', 'Без музыки в карточке игры', 'Muziek uit bij spelinfo'),
  # 52 the ROM's CRC32 on the game info card (2.18)
  ('CRC32 on the game info', 'CRC32 na ficha', 'CRC32 en la ficha', 'CRC32 in der Infokarte', 'CRC32 dans la fiche', 'CRC32 nella scheda', 'CRC32 в карточке игры', 'CRC32 in de spelinfo'),
  # 53 rotated Game Boy buttons, on the SGB and the GBC core (2.18)
  ('Rotated Game Boy buttons', 'Botões do Game Boy girados', 'Botones de Game Boy girados', 'Gedrehte Game-Boy-Tasten', 'Boutons Game Boy pivotés', 'Tasti Game Boy ruotati', 'Повёрнутые кнопки Game Boy', 'Gedraaide Game Boy-knoppen'),
  # 54 the NTT Data Keypad (NDK10) as a standard pad (2.18)
  ('NTT Data Keypad',)*8,
  # 55 the parental lock: a code of 4 buttons before what changes the card or the settings (2.18)
  ('Parental lock', 'Bloqueio parental', 'Bloqueo parental', 'Kindersicherung', 'Contrôle parental', 'Blocco parentale', 'Родительский контроль', 'Kinderslot'),
  # 56 the 2.19 section: what is new in this release
  ("What's new in 2.19", 'Novidades da 2.19', 'Novedades de la 2.19', 'Neu in 2.19', 'Nouveautés de la 2.19', 'Novità della 2.19', 'Новое в 2.19', 'Nieuw in 2.19'),
  # 57 the animated menu background, a theme's own, off in Japanese and Chinese (2.19)
  ('Animated background', 'Fundo animado', 'Fondo animado', 'Animierter Hintergrund', 'Fond animé', 'Sfondo animato', 'Анимированный фон', 'Bewegende achtergrond'),
  # 58 a theme's font, music, effects and preview picture, the Apply font / Apply sounds window (2.19)
  ('Font and sounds in themes', 'Fonte e sons no tema', 'Fuente y sonidos en temas', 'Schrift und Sounds im Theme', 'Police et sons du thème', 'Font e suoni nel tema', 'Шрифт и звуки в теме', 'Lettertype en geluid in thema'),
  # 59 font files (.sfn), picked in the browser like a theme (2.19)
  ('Font files', 'Arquivos de fonte', 'Archivos de fuente', 'Schriftdateien', 'Fichiers de police', 'File di font', 'Файлы шрифтов', 'Lettertypebestanden'),
  # 60 the Atari 2600 on the Mk.II too, and the same pad keys on both boards (2.19)
  ('Atari 2600 on the Mk.II', 'Atari 2600 no Mk.II', 'Atari 2600 en el Mk.II', 'Atari 2600 auf dem Mk.II', 'Atari 2600 sur le Mk.II', 'Atari 2600 sul Mk.II', 'Atari 2600 на Mk.II', 'Atari 2600 op de Mk.II'),
  # 61 the PPU is cleared before every game boots
  ('Clear PPU on boot', 'Limpar PPU no boot', 'Limpiar PPU al arrancar', 'PPU beim Start löschen', 'Effacer le PPU au boot', "Pulisci PPU all'avvio", 'Очистка PPU при загрузке', 'Leeg PPU bij starten'),
  # 62 bus timing compat
  ('Bus timing compat', 'Compat. de barramento', 'Compat. de bus', 'Bus-Timing-Kompat.', 'Compat. timing bus', 'Compat. timing bus', 'Совместимость шины', 'Compat. bustiming'),
  # 63 the hardware model in System Information
  ('Hardware model', 'Modelo do hardware', 'Modelo de hardware', 'Hardware-Modell', 'Modèle du matériel', "Modello dell'hardware", 'Модель картриджа', 'Hardwaremodel'),
  # 64 BS-X and the Memory Pack
  ('BS-X and Memory Pack', 'BS-X e Memory Pack', 'BS-X y Memory Pack', 'BS-X und Memory Pack', 'BS-X et Memory Pack', 'BS-X e Memory Pack', 'BS-X и Memory Pack', 'BS-X en Memory Pack'),
  # 65 delete files and saves
  ('Delete files and saves', 'Apagar arquivos e saves', 'Borrar archivos y saves', 'Dateien/Saves löschen', 'Effacer fichiers/saves', 'Eliminare file e save', 'Удалить файлы/сохранения', 'Bestanden/saves wissen'),
  # 66 missing BIOS warning
  ('Missing BIOS warning', 'Aviso de BIOS faltando', 'Aviso de BIOS que falta', 'Fehlendes BIOS melden', 'Alerte de BIOS manquant', 'Avviso di BIOS mancante', 'Инфо: BIOS не найден', 'Melding: BIOS ontbreekt'),
  # 67 the clock question on boot and the date format
  ('Clock and date', 'Relógio e data', 'Reloj y fecha', 'Uhr und Datum', 'Horloge et date', 'Orologio e data', 'Дата и время', 'Klok en datum'),
  # 68 a description for each option
  ('Option descriptions', 'Descrição das opções', 'Descripción de opciones', 'Optionsbeschreibungen', 'Description des options', 'Descrizione opzioni', 'Описание опций настроек', 'Uitleg bij opties'),
]


# Each card's paragraph, per language ({off}, {on}... are the answer labels as the
# list shows them, {cfg}, {saves}... the menu's own words). The generator word-wraps it
# to ONB_TEXT_W cells and pads every language to the same number of lines (the "and
# more" items to the same count as each other), so the answer list below it stays put
# when the language or the item changes. Lines are separated by byte 1, where
# onb_hiprint stops; onb_emit_para prints one per row. A "\n" in a text forces a new line
# there (each piece is wrapped on its own), for a list of short lines under a sentence.
TEXTS = [
  # 1 language
  ('The menu and this tour in 10 languages, the in-game menu in 8. Move the bar to try one: the tour switches at once. You can change it later in |Configuration|.',
   'O menu e este tour em 10 idiomas, o menu in-game em 8. Mova a barra para experimentar: o tour troca na hora. Dá para mudar depois em |Configurações|.',
   'El menú y este tour en 10 idiomas, el menú del juego en 8. Mueve la barra para probar: el tour cambia al instante. Puedes cambiarlo luego en |Configuración|.',
   'Das Menü und diese Tour in 10 Sprachen, das Ingame-Menü in 8. Bewege den Balken zum Testen: die Tour wechselt sofort. Später in den |Einstellungen| änderbar.',
   'Le menu et cette visite en 10 langues, le menu en jeu en 8. Déplace la barre pour essayer : la visite change aussitôt. Modifiable ensuite dans |Configuration|.',
   'Il menu e questo tour in 10 lingue, il menu in gioco in 8. Sposta la barra per provare: il tour cambia subito. Puoi cambiarla dopo in |Configurazione|.',
   'Меню и этот тур на 10-ти языках, Меню в игре на 8-ми. Переместите курсор, чтобы попробовать: тур переключится мгновенно. Позже это можно изменить в разделе |Настройки|.',
   'Het menu en deze rondleiding in 10 talen, het in-game menu in 8. Beweeg de balk om te proberen: de rondleiding wisselt meteen. Later te wijzigen in |Configuratie|.'),
  # 2 themes: the |sd2snes+| theme, a |.thm| from the list, restoring it or the classic one
  ("Change the menu's logo, colours and gradient: pick a |.thm| in the list to apply it. Make your own in the |Theme Creator| or take one from the site's gallery. In {browser}, {restoretheme} brings back |sd2snes+|, the factory theme, and {restoreclassic} the same with a still background (|classic.thm|).",
   'Troque o logo, as cores e o gradiente do menu: escolha um |.thm| na lista para aplicar. Crie o seu no |Theme Creator| ou pegue um na galeria do site. Em {browser}, {restoretheme} volta ao |sd2snes+|, o tema de fábrica, e {restoreclassic} ao mesmo sem o fundo animado (|classic.thm|).',
   'Cambia el logo, los colores y el degradado del menú: elige un |.thm| en la lista para aplicarlo. Crea el tuyo en el |Theme Creator| o baja uno de la galería del sitio. En {browser}, {restoretheme} vuelve a |sd2snes+|, el tema de fábrica, y {restoreclassic} al mismo sin fondo animado (|classic.thm|).',
   'Ändere Logo, Farben und Verlauf des Menüs: eine |.thm| in der Liste wählen wendet sie an. Eigene im |Theme Creator| oder aus der Galerie der Website. In {browser} holt {restoretheme} das Werks-Theme |sd2snes+| zurück, {restoreclassic} dasselbe ohne bewegten Hintergrund (|classic.thm|).',
   "Change logo, couleurs et dégradé du menu avec un |.thm| choisi dans la liste. Crée le tien dans le |Theme Creator| ou pioche dans la galerie du site. {restoretheme} ({browser}) remet |sd2snes+|, le thème d'origine, et {restoreclassic} le même sans fond animé (|classic.thm|).",
   'Cambia logo, colori e sfumatura del menu: scegli un |.thm| nella lista per applicarlo. Crea il tuo nel |Theme Creator| o prendine uno dalla galleria del sito. In {browser}, {restoretheme} torna a |sd2snes+|, il tema di fabbrica, e {restoreclassic} allo stesso senza sfondo animato (|classic.thm|).',
   'Меняй логотип, цвета и градиент меню: выбери файл |.thm| в списке, чтобы применить. Создай свою тему в |Theme Creator| или возьми из галереи на сайте. В разделе {browser} пункт {restoretheme} вернёт заводскую тему |sd2snes+|, а {restoreclassic} её же без живого фона (|classic.thm|).',
   'Wijzig logo, kleuren en verloop van het menu: een |.thm| in de lijst kiezen past het toe. Maak je eigen in de |Theme Creator| of haal er een op de site. {restoretheme} ({browser}) zet het standaardthema |sd2snes+| terug, {restoreclassic} hetzelfde zonder animatie (|classic.thm|).'),
  # 3 text outline: theme / on / off
  ('The dark edge around each letter of the menu. {theme} follows what the theme asks for, {on} always draws it, {off} drops it and lets the background show through. Move the bar to try one: this text changes at once, and [A] keeps it for the menu.',
   'A borda escura em volta de cada letra do menu. {theme} segue o que o tema pede, {on} sempre desenha, {off} tira a borda e deixa o fundo aparecer. Mova a barra para experimentar: este texto muda na hora, e [A] grava a escolha para o menu.',
   'El borde oscuro alrededor de cada letra del menú. {theme} sigue lo que pide el tema, {on} siempre lo dibuja, {off} lo quita y deja ver el fondo. Mueve la barra para probar: este texto cambia al instante y [A] lo guarda para el menú.',
   'Der dunkle Rand um jeden Buchstaben des Menüs. {theme} folgt dem Theme, {on} zeichnet ihn immer, {off} lässt ihn weg und den Hintergrund durchscheinen. Bewege den Balken zum Testen: dieser Text ändert sich sofort, [A] übernimmt es fürs Menü.',
   "Le bord sombre autour de chaque lettre du menu. {theme} suit ce que veut le thème, {on} le dessine toujours, {off} l'enlève et laisse voir le fond. Déplace la barre pour essayer : ce texte change aussitôt, [A] le garde pour le menu.",
   'Il bordo scuro attorno a ogni lettera del menu. {theme} segue il tema, {on} lo disegna sempre, {off} lo toglie e lascia vedere lo sfondo. Sposta la barra per provare: questo testo cambia subito, [A] lo salva per il menu.',
   'Тёмная обводка вокруг каждой буквы меню. {theme}: как задано в теме, {on}: всегда включено, {off}: без обводки, сквозь буквы виден фон. Переместите курсор, чтобы попробовать: этот текст сразу изменится, а кнопка [A] сохранит выбор для меню.',
   'De donkere rand rond elke letter van het menu. {theme} volgt het thema, {on} tekent hem altijd, {off} laat hem weg zodat de achtergrond doorschijnt. Beweeg de balk om te proberen: deze tekst verandert meteen, [A] bewaart het voor het menu.'),
  # 4 text anti-aliasing: theme / on / off
  ('The half-tone step that smooths the curves of each letter. {theme} follows what the theme asks for, {on} always smooths, {off} draws the letters with hard pixels. Move the bar to try one: this text changes at once, and [A] keeps it for the menu.',
   'O meio-tom que suaviza as curvas de cada letra. {theme} segue o que o tema pede, {on} sempre suaviza, {off} desenha as letras com pixels duros. Mova a barra para experimentar: este texto muda na hora, e [A] grava a escolha para o menu.',
   'El medio tono que suaviza las curvas de cada letra. {theme} sigue lo que pide el tema, {on} siempre suaviza, {off} dibuja las letras con píxeles duros. Mueve la barra para probar: este texto cambia al instante y [A] lo guarda para el menú.',
   'Die Halbtonstufe, die die Rundungen jedes Buchstabens glättet. {theme} folgt dem Theme, {on} glättet immer, {off} zeigt harte Pixel. Bewege den Balken zum Testen: dieser Text ändert sich sofort, [A] übernimmt es fürs Menü.',
   'Le demi-ton qui adoucit les courbes de chaque lettre. {theme} suit ce que veut le thème, {on} lisse toujours, {off} dessine les lettres en pixels nets. Déplace la barre pour essayer : ce texte change aussitôt, [A] le garde pour le menu.',
   'Il mezzo tono che ammorbidisce le curve di ogni lettera. {theme} segue il tema, {on} ammorbidisce sempre, {off} disegna le lettere a pixel netti. Sposta la barra per provare: questo testo cambia subito, [A] lo salva per il menu.',
   'Полутон, который сглаживает изгибы каждой буквы. {theme}: как задано в теме, {on}: всегда включено, {off}: буквы из резких пикселей. Переместите курсор, чтобы попробовать: этот текст сразу изменится, а кнопка [A] сохранит выбор для меню.',
   'De halftoon die de rondingen van elke letter gladstrijkt. {theme} volgt het thema, {on} strijkt altijd glad, {off} tekent de letters met harde pixels. Beweeg de balk om te proberen: deze tekst verandert meteen, [A] bewaart het voor het menu.'),
  # 5 box art
  ("Shows the game's box art next to the list as you browse. {large} fills the top corner, {small} takes less room. Covers are |.cov| files: the |Web Manager| downloads them for your whole collection.",
   'Mostra a capa do jogo ao lado da lista enquanto você navega. {large} ocupa o canto de cima, {small} ocupa menos espaço. As capas são arquivos |.cov|: o |Web Manager| baixa para a coleção inteira.',
   'Muestra la carátula del juego junto a la lista mientras navegas. {large} ocupa la esquina superior, {small} ocupa menos. Son archivos |.cov|: el |Web Manager| los descarga para toda tu colección.',
   'Zeigt das Spielcover neben der Liste beim Blättern. {large} füllt die obere Ecke, {small} braucht weniger Platz. Cover sind |.cov|-Dateien: der |Web Manager| lädt sie für die ganze Sammlung.',
   'Affiche la jaquette du jeu à côté de la liste pendant la navigation. {large} occupe le coin du haut, {small} prend moins de place. Ce sont des fichiers |.cov| : le |Web Manager| les télécharge pour toute ta collection.',
   "Mostra la copertina del gioco accanto alla lista mentre navighi. {large} occupa l'angolo in alto, {small} occupa meno spazio. Sono file |.cov|: il |Web Manager| li scarica per tutta la collezione.",
   'Показывает обложку игры рядом со списком. {large} - занимает верхний правый угол, {small} - меньше места на экране. Обложки это файлы |.cov|. Используй |Web Manager| на сайте, он скачает их для всей вашей коллекции.',
   'Toont de hoes van het spel naast de lijst tijdens het bladeren. {large} vult de bovenhoek, {small} neemt minder ruimte. Hoezen zijn |.cov|-bestanden: de |Web Manager| haalt ze op voor je hele collectie.'),
  # 6 covers in the Recent / Favorites lists (needs covers on)
  ('The box art of the game under the bar also shows in the Recent and Favorites lists, at the size chosen on the previous card. Only asked when covers are on.',
   'A capa do jogo sob a barra aparece também nas listas de Recentes e Favoritos, no tamanho escolhido no card anterior. Só aparece com as capas ligadas.',
   'La carátula del juego bajo la barra también aparece en las listas de Recientes y Favoritos, con el tamaño elegido antes. Solo con las carátulas activadas.',
   'Das Cover des markierten Spiels erscheint auch in den Listen Letzte und Favoriten, in der zuvor gewählten Größe. Nur mit eingeschalteten Covern.',
   "La jaquette du jeu sous la barre s'affiche aussi dans les listes Récents et Favoris, à la taille choisie avant. Seulement si les jaquettes sont activées.",
   'La copertina del gioco sotto la barra appare anche negli elenchi Recenti e Preferiti, con la misura scelta prima. Solo con le copertine attive.',
   'Если в настройках включено отображение обложек, то при наведении курсора на игру её обложка появится в списках |Недавние| и |Избранные|. Размер обложки соответствует вашим сохранённым настройкам.',
   'De hoes van het spel onder de balk verschijnt ook in de lijsten Recent en Favorieten, op de eerder gekozen grootte. Alleen als hoezen aan staan.'),
  # 7 game info card
  ('A card with the cover, screenshot, publisher, year, genre and description before the game starts. {on}: [A] opens it. {ctx}: [A] starts the game and the card is in the [Y] menu. {off}: [A] starts the game. On the card, [Up] and [Down] jump to the previous or next game of the list.',
   'Uma ficha com capa, screenshot, editora, ano, gênero e descrição antes do jogo começar. {on}: [A] abre a ficha. {ctx}: [A] inicia o jogo e a ficha fica no menu do [Y]. {off}: [A] inicia o jogo. Na ficha, [Cima] e [Baixo] pulam para o jogo anterior ou o próximo da lista.',
   'Una ficha con carátula, captura, editora, año, género y descripción antes de jugar. {on}: [A] la abre. {ctx}: [A] inicia el juego y la ficha está en el menú de [Y]. {off}: [A] inicia el juego. En la ficha, [Arriba] y [Abajo] saltan al juego anterior o al siguiente.',
   'Eine Karte mit Cover, Screenshot, Publisher, Jahr, Genre und Beschreibung vor dem Start. {on}: [A] öffnet sie. {ctx}: [A] startet das Spiel, die Karte ist im [Y]-Menü. {off}: [A] startet das Spiel. Auf der Karte springen [Oben] und [Unten] zum vorigen oder nächsten Spiel.',
   "Une fiche avec jaquette, capture, éditeur, année, genre et description avant de jouer. {on} : [A] l'ouvre. {ctx} : [A] lance le jeu et la fiche est dans le menu [Y]. {off} : [A] lance le jeu. Sur la fiche, [Haut] et [Bas] passent au jeu précédent ou suivant.",
   'Una scheda con copertina, schermata, editore, anno, genere e descrizione prima di giocare. {on}: [A] la apre. {ctx}: [A] avvia il gioco e la scheda è nel menu [Y]. {off}: [A] avvia il gioco. Sulla scheda, [Su] e [Giù] passano al gioco precedente o successivo.',
   'Карточка с обложкой, скриншотом, издателем, годом, жанром и описанием перед запуском игры. {on} - кнопка [A] открывает её. {ctx} - кнопка [A] запускает игру, а кнопка [Y] открывает карточку в меню. {off} - кнопка [A] сразу запускает игру. В карточке кнопки [Вверх] и [Вниз] - переход к предыдущей или следующей игре.',
   'Een kaart met hoes, screenshot, uitgever, jaar, genre en beschrijving voor het spelen. {on}: [A] opent hem. {ctx}: [A] start het spel, de kaart zit in het [Y]-menu. {off}: [A] start het spel. Op de kaart springen [Omhoog] en [Omlaag] naar het vorige of volgende spel.'),
  # 8 the video clip on the game info card (needs the card)
  ('When the game has a video clip (|.fmv| in |/sd2snes/info|), the game info card plays it in place of the still screenshot. With {no}, the card keeps the screenshot. Only asked when the game info card is on.',
   'Quando o jogo tem um clipe de vídeo (|.fmv| em |/sd2snes/info|), a ficha do jogo toca o clipe no lugar do screenshot parado. Com {no}, a ficha mostra o screenshot. Só aparece com a ficha ligada.',
   'Si el juego tiene un clip de vídeo (|.fmv| en |/sd2snes/info|), la ficha lo reproduce en lugar de la captura fija. Con {no}, la ficha muestra la captura. Solo con la ficha activada.',
   'Hat das Spiel einen Videoclip (|.fmv| in |/sd2snes/info|), spielt die Infokarte ihn statt des Standbilds ab. Mit {no} bleibt das Standbild. Nur mit eingeschalteter Infokarte.',
   'Si le jeu a un clip vidéo (|.fmv| dans |/sd2snes/info|), la fiche le joue à la place de la capture fixe. Avec {no}, la fiche garde la capture. Seulement si la fiche est activée.',
   'Se il gioco ha un video (|.fmv| in |/sd2snes/info|), la scheda lo riproduce al posto della schermata fissa. Con {no}, la scheda mostra la schermata. Solo con la scheda attiva.',
   'Если у игры есть видеоролик (|.fmv| в |/sd2snes/info|), он воспроизводится в карточке вместо скриншота. Если выбрано {no}, остаётся скриншот. Но только если карточка в |Информация об игре| включена.',
   'Heeft het spel een videoclip (|.fmv| in |/sd2snes/info|), dan speelt de spelinfo die af in plaats van de stilstaande screenshot. Met {no} blijft de screenshot. Alleen als de spelinfo aan staat.'),
  # 9 the clip's soundtrack (needs the video and the card)
  ("The clip's soundtrack (|.pcm| next to the |.fmv|) plays with it through the cartridge's |MSU-1| audio. With {no}, the clip plays silent. Only asked when the video is on.",
   'A trilha do clipe (|.pcm| ao lado do |.fmv|) toca junto pelo áudio |MSU-1| do cartucho. Com {no}, o clipe roda mudo. Só aparece com o vídeo ligado.',
   'La banda sonora del clip (|.pcm| junto al |.fmv|) suena con él por el audio |MSU-1| del cartucho. Con {no}, el clip va mudo. Solo con el vídeo activado.',
   'Der Ton des Clips (|.pcm| neben der |.fmv|) läuft über das |MSU-1|-Audio des Moduls mit. Mit {no} bleibt der Clip stumm. Nur mit eingeschaltetem Video.',
   "La bande-son du clip (|.pcm| à côté du |.fmv|) joue avec lui par l'audio |MSU-1| de la cartouche. Avec {no}, le clip est muet. Seulement si la vidéo est activée.",
   "La colonna sonora del video (|.pcm| accanto al |.fmv|) suona insieme tramite l'audio |MSU-1| della cartuccia. Con {no}, il video è muto. Solo con il video attivo.",
   'Звук ролика (|.pcm| рядом с |.fmv|) воспроизводится вместе с ним как |MSU-1| аудио картриджа. Если выбрано {no}, ролик идёт без звука. Но только если включён параметр |Показывать видео|.',
   'De soundtrack van de clip (|.pcm| naast de |.fmv|) speelt mee via de |MSU-1|-audio van de cartridge. Met {no} speelt de clip zonder geluid. Alleen als de video aan staat.'),
  # 10 menu music
  ('An |.spc| soundtrack plays in the background while you browse and stops when a game starts. To use your own, put it at |/sd2snes/menu.spc|. Or press [Y] on any |.spc| in the list: {setbgm}. {restoremusic} in {browser} goes back to |menu.spc|.',
   'Uma trilha |.spc| toca ao fundo enquanto você navega e para quando o jogo começa. Para trocar, ponha sua trilha em |/sd2snes/menu.spc|. Ou aperte [Y] sobre qualquer |.spc| da lista: {setbgm}. {restoremusic} em {browser} volta ao |menu.spc|.',
   'Una pista |.spc| suena de fondo mientras navegas y se detiene al iniciar un juego. Para cambiarla, pon tu pista en |/sd2snes/menu.spc|. O pulsa [Y] sobre cualquier |.spc| de la lista: {setbgm}. {restoremusic} en {browser} vuelve a |menu.spc|.',
   'Ein |.spc|-Soundtrack läuft beim Blättern im Hintergrund und stoppt beim Spielstart. Eigenen Titel als |/sd2snes/menu.spc| ablegen. Oder [Y] auf einer |.spc| in der Liste: {setbgm}. {restoremusic} in {browser} kehrt zu |menu.spc| zurück.',
   "Une musique |.spc| joue en fond pendant la navigation et s'arrête au lancement d'un jeu. Pour la changer, mets ta piste dans |/sd2snes/menu.spc|. Ou appuie sur [Y] sur un |.spc| de la liste : {setbgm}. {restoremusic} dans {browser} revient à |menu.spc|.",
   "Una traccia |.spc| suona in sottofondo mentre navighi e si ferma all'avvio del gioco. Per cambiarla, metti la tua traccia in |/sd2snes/menu.spc|. Oppure premi [Y] su un |.spc| della lista: {setbgm}. {restoremusic} in {browser} torna a |menu.spc|.",
   'Музыка |.spc| играет фоном, пока ты листаешь меню, и стихает при запуске игры. Назови свой трек |menu.spc| и запиши в папку |/sd2snes|. Или нажми кнопку [Y] на любом |.spc| в списке: {setbgm}. {restoremusic} в {browser} возвращает музыку |menu.spc|.',
   'Een |.spc|-soundtrack speelt op de achtergrond tijdens het bladeren en stopt als een spel start. Eigen nummer: zet het in |/sd2snes/menu.spc|. Of druk [Y] op een |.spc| in de lijst: {setbgm}. {restoremusic} in {browser} gaat terug naar |menu.spc|.'),
  # 11 random music
  ('Each time the menu starts, a different track is drawn from the |/sd2snes/music| folder. Put as many |.spc| files there as you like to build your playlist.',
   'A cada vez que o menu abre, uma trilha diferente é sorteada da pasta |/sd2snes/music|. Coloque lá quantos |.spc| quiser para montar sua playlist.',
   'Cada vez que se abre el menú, se elige una pista distinta de la carpeta |/sd2snes/music|. Pon ahí todos los |.spc| que quieras para tu lista.',
   'Bei jedem Menüstart wird ein anderer Titel aus dem Ordner |/sd2snes/music| gewählt. Lege dort beliebig viele |.spc|-Dateien als Playlist ab.',
   'À chaque ouverture du menu, une piste différente est tirée du dossier |/sd2snes/music|. Mets-y autant de |.spc| que tu veux pour ta playlist.',
   'A ogni avvio del menu viene scelta una traccia diversa dalla cartella |/sd2snes/music|. Mettici tutti gli |.spc| che vuoi per la tua playlist.',
   'При каждом запуске меню играет случайный трек из папки |/sd2snes/music|. Запишите туда сколько угодно файлов |.spc|.',
   'Bij elke start van het menu wordt een ander nummer uit de map |/sd2snes/music| gekozen. Zet er zoveel |.spc|-bestanden in als je wilt.'),
  # 12 menu sounds
  ("Short sound effects when you move the cursor, confirm, go back or hit an error, played through the cartridge's |MSU-1| audio. Make your own set in the |Sound Creator| on the website.",
   'Efeitos curtos ao mover o cursor, confirmar, voltar ou dar erro, tocados pelo áudio |MSU-1| do cartucho. Crie o seu conjunto no |Criador de Sons| do site.',
   'Efectos cortos al mover el cursor, confirmar, volver o al haber un error, por el audio |MSU-1| del cartucho. Crea los tuyos en el |Sound Creator| de la web.',
   'Kurze Effekte beim Bewegen des Cursors, Bestätigen, Zurückgehen und bei Fehlern, über das |MSU-1|-Audio des Moduls. Eigene im |Sound Creator| auf der Website.',
   "Des effets courts quand tu bouges le curseur, valides, reviens ou en cas d'erreur, joués par l'audio |MSU-1| de la cartouche. Crée les tiens dans le |Sound Creator| du site.",
   "Brevi effetti quando muovi il cursore, confermi, torni indietro o c'è un errore, dall'audio |MSU-1| della cartuccia. Crea i tuoi con il |Sound Creator| sul sito.",
   'Короткие звуки при движении курсора, выборе, возврате и ошибке - через |MSU-1| аудио картриджа. Создай свои в |Sound Creator| на сайте.',
   'Korte geluiden bij cursor bewegen, bevestigen, terug en fouten, via de |MSU-1|-audio van de cartridge. Maak je eigen set in de |Sound Creator| op de website.'),
  # 13 |MSU-1| folders
  ('A folder with one game and its |MSU-1| audio (|.msu| and |.pcm| tracks) acts as the game itself: [A] on the folder starts it and its cover shows in the list. The tracks are listed after the ROM.',
   'Uma pasta com um jogo e o áudio |MSU-1| dele (|.msu| e faixas |.pcm|) vira o próprio jogo: [A] na pasta inicia o jogo e a capa aparece na lista. As faixas ficam listadas depois da ROM.',
   'Una carpeta con un juego y su audio |MSU-1| (|.msu| y pistas |.pcm|) actúa como el juego: [A] en la carpeta lo inicia y su carátula sale en la lista. Las pistas van tras la ROM.',
   'Ein Ordner mit einem Spiel und seinem |MSU-1|-Audio (|.msu| und |.pcm|-Tracks) verhält sich wie das Spiel: [A] startet es, das Cover steht in der Liste. Tracks kommen nach der ROM.',
   "Un dossier avec un jeu et son audio |MSU-1| (|.msu| et pistes |.pcm|) devient le jeu : [A] sur le dossier le lance et sa jaquette s'affiche. Les pistes sont listées après la ROM.",
   'Una cartella con un gioco e il suo audio |MSU-1| (|.msu| e tracce |.pcm|) diventa il gioco: [A] sulla cartella lo avvia e la copertina appare nella lista. Le tracce vanno dopo la ROM.',
   '|MSU-1| папка с игрой и её аудио (|.msu| и треки |.pcm|) запускается как игра: кнопка [A] на папке запускает её, обложка видна в списке. Если выбрано {no}, открывается папка с игрой. Треки |.pcm| идут после ROM-файла игры.',
   'Een map met een spel en zijn |MSU-1|-audio (|.msu| en |.pcm|-nummers) gedraagt zich als het spel: [A] op de map start het en de hoes staat in de lijst. Nummers staan na de ROM.'),
  # 14 the |.pcm| track player of the file list
  ("The |.pcm| tracks of |MSU-1| games show up in the file list. [A] on one opens a player with a progress bar and the elapsed and total time, [A] pauses, [B] closes. Handy to check a game's soundtrack before playing.",
   'As trilhas |.pcm| dos jogos |MSU-1| aparecem na lista de arquivos. [A] numa delas abre um tocador com barra de progresso e tempo decorrido e total, [A] pausa, [B] fecha. Bom para conferir a trilha antes de jogar.',
   'Las pistas |.pcm| de los juegos |MSU-1| aparecen en la lista. [A] en una abre un reproductor con barra de progreso y tiempo transcurrido y total, [A] pausa, [B] cierra. Útil para revisar la banda sonora antes de jugar.',
   'Die |.pcm|-Tracks von |MSU-1|-Spielen stehen in der Dateiliste. [A] auf einem öffnet einen Player mit Fortschrittsbalken und Zeitanzeige, [A] pausiert, [B] schließt. Praktisch, um den Soundtrack vorher zu prüfen.',
   "Les pistes |.pcm| des jeux |MSU-1| apparaissent dans la liste. [A] sur l'une ouvre un lecteur avec barre de progression et temps écoulé et total, [A] met en pause, [B] ferme. Pratique pour vérifier la bande-son.",
   'Le tracce |.pcm| dei giochi |MSU-1| compaiono nella lista. [A] su una apre un lettore con barra di avanzamento e tempo trascorso e totale, [A] mette in pausa, [B] chiude. Utile per controllare la colonna sonora.',
   'У игр |MSU-1| треки |.pcm| видны в списке файлов. Кнопка [A] на треке открывает плеер с полосой прогресса и временем. В плеере кнопка [A] - пауза, а кнопка [B] - выход. Удобно проверить саундтрек перед игрой.',
   'De |.pcm|-nummers van |MSU-1|-spellen staan in de bestandslijst. [A] op een nummer opent een speler met voortgangsbalk en tijd, [A] pauzeert, [B] sluit. Handig om de soundtrack vooraf te checken.'),
  # 15 show the sd2snes folder in the file list
  ('Lists the hidden |/sd2snes| folder in the file list: look at saves, states and info files, apply a theme from it, or delete data you no longer need with [Y].',
   'Mostra a pasta oculta |/sd2snes| na lista de arquivos, para ver saves, states e fichas, aplicar um tema que esteja lá ou apagar com [Y] dados que não servem mais.',
   'Muestra la carpeta oculta |/sd2snes| en la lista, para ver saves, states y fichas, aplicar un tema que esté allí o borrar con [Y] datos que ya no sirven.',
   'Zeigt den versteckten Ordner |/sd2snes| in der Dateiliste: Saves, States und Infodateien ansehen, ein Theme von dort anwenden oder alte Daten mit [Y] löschen.',
   "Affiche le dossier caché |/sd2snes| dans la liste, pour voir saves, states et fiches, appliquer un thème qui s'y trouve ou effacer avec [Y] les données inutiles.",
   "Mostra la cartella nascosta |/sd2snes| nella lista, per vedere save, state e schede, applicare un tema che c'è dentro o cancellare con [Y] i dati che non servono più.",
   'Показывает скрытую папку |/sd2snes| в списке файлов: можно смотреть сохранения, сейвстейты и карточки, применить тему оттуда или удалить ненужные данные кнопкой [Y].',
   'Toont de verborgen map |/sd2snes| in de bestandslijst: saves, states en spelinfo bekijken, een thema daaruit toepassen of oude gegevens wissen met [Y].'),
  # 16 smart reset
  ("What the console's [RESET] button does during a game. {off}: resets the game. {menu}: back to the menu. {folder}: to the game's folder. {rom}: the folder with the game selected. {hold}: a tap resets, holding goes back. The [L]+[R]+[Select]+[X] shortcut/hook back to the menu follows {folder}, {rom} and {hold} too.",
   'O que o botão [RESET] do console faz durante o jogo. {off}: reinicia o jogo. {menu}: volta ao menu. {folder}: vai para a pasta do jogo. {rom}: a pasta com o jogo selecionado. {hold}: toque reinicia, segurar volta. O atalho/hook [L]+[R]+[Select]+[X], que volta ao menu, segue {folder}, {rom} e {hold} também.',
   'Qué hace el botón [RESET] de la consola en un juego. {off}: reinicia el juego. {menu}: vuelve al menú. {folder}: a la carpeta del juego. {rom}: la carpeta con el juego elegido. {hold}: tocar reinicia, mantener vuelve. El atajo/hook [L]+[R]+[Select]+[X], que vuelve al menú, también sigue {folder}, {rom} y {hold}.',
   'Was die [RESET]-Taste im Spiel macht. {off}: startet das Spiel neu. {menu}: zurück ins Menü. {folder}: in den Ordner des Spiels. {rom}: der Ordner, Spiel markiert. {hold}: Tippen startet neu, Halten geht zurück. Das Kürzel/Hook [L]+[R]+[Select]+[X] zurück ins Menü folgt auch {folder}, {rom} und {hold}.',
   'Ce que fait le bouton [RESET] de la console en jeu. {off} : relance le jeu. {menu} : retour au menu. {folder} : au dossier du jeu. {rom} : le dossier, jeu sélectionné. {hold} : appui court relance, long revient. Le raccourci/hook [L]+[R]+[Select]+[X] de retour au menu suit aussi {folder}, {rom} et {hold}.',
   'Cosa fa il tasto [RESET] della console in gioco. {off}: riavvia il gioco. {menu}: torna al menu. {folder}: alla cartella del gioco. {rom}: la cartella col gioco scelto. {hold}: tocco riavvia, tenuto torna. La scorciatoia/hook [L]+[R]+[Select]+[X] per tornare al menu segue anche {folder}, {rom} e {hold}.',
   'Что делает кнопка [RESET] во время игры. {off} - перезапуск игры. {menu} - выход в меню. {folder} - в папку игры. {rom} - в папку с выбранной игрой. {hold}: короткое нажатие - перезапуск игры, долгое - выход в меню. Комбинация/перехват кнопками [L]+[R]+[Select]+[X] для выхода в меню работает так же в режимах {folder}, {rom} и {hold}.',
   'Wat de [RESET]-knop tijdens een spel doet. {off}: herstart het spel. {menu}: terug naar het menu. {folder}: naar de map van het spel. {rom}: de map met het spel gekozen. {hold}: tik herstart, vasthouden gaat terug. De sneltoets/hook [L]+[R]+[Select]+[X] terug naar het menu volgt ook {folder}, {rom} en {hold}.'),
  # 17 the cheat list: where it opens, the code formats, the pages, the keys
  ("Open it with [Y] on a game in the file list ({cheats}) or in the {cheatstab} tab of the in-game menu. It holds the game's Game Genie and Pro Action Replay codes, page by page: [Left] and [Right] turn the page, a long name scrolls on its row. [A] turns a code on or off, [Y] edits it and [SELECT] adds one.",
   'Abra com [Y] num jogo da lista de arquivos ({cheats}) ou na aba {cheatstab} do menu in-game. Ela traz os códigos Game Genie e Pro Action Replay do jogo, página por página: [Esquerda] e [Direita] trocam a página e um nome longo rola na linha. [A] liga ou desliga um código, [Y] edita e [SELECT] adiciona.',
   'Se abre con [Y] sobre un juego de la lista ({cheats}) o en la pestaña {cheatstab} del menú del juego. Tiene los códigos Game Genie y Pro Action Replay del juego por páginas: [Izquierda] y [Derecha] pasan de página y un nombre largo se desplaza. [A] activa o desactiva un código, [Y] lo edita y [SELECT] añade uno.',
   'Öffne sie mit [Y] auf einem Spiel in der Dateiliste ({cheats}) oder im Tab {cheatstab} des Ingame-Menüs. Sie zeigt die Game-Genie- und Pro-Action-Replay-Codes seitenweise: [Links] und [Rechts] blättern, ein langer Name läuft durch. [A] schaltet einen Code an oder aus, [Y] bearbeitet ihn, [SELECT] fügt einen hinzu.',
   "Ouvre-la avec [Y] sur un jeu de la liste ({cheats}) ou dans l'onglet {cheatstab} du menu en jeu. Elle montre les codes Game Genie et Pro Action Replay du jeu, page par page : [Gauche] et [Droite] tournent la page, un nom long défile. [A] active ou coupe un code, [Y] le modifie et [SELECT] en ajoute un.",
   'Si apre con [Y] su un gioco della lista dei file ({cheats}) o nella scheda {cheatstab} del menu in gioco. Mostra i codici Game Genie e Pro Action Replay del gioco, pagina per pagina: [Sinistra] e [Destra] cambiano pagina, un nome lungo scorre. [A] attiva o disattiva un codice, [Y] lo modifica e [SELECT] ne aggiunge uno.',
   'Открывается кнопкой [Y] на игре в списке файлов ({cheats}) или на вкладке {cheatstab} в |Меню в игре|. Там коды Game Genie и Pro Action Replay по страницам: кнопки [Влево] и [Вправо] - листают, длинное имя прокручивается, кнопка [A] - включает или выключает код, кнопка [Y] - редактирует, а кнопка [SELECT] - добавляет новый.',
   'Open hem met [Y] op een spel in de bestandslijst ({cheats}) of in de tab {cheatstab} van het in-game menu. Hij toont de Game Genie- en Pro Action Replay-codes van het spel per pagina: [Links] en [Rechts] bladeren, een lange naam schuift door. [A] zet een code aan of uit, [Y] bewerkt hem en [SELECT] voegt er een toe.'),
  # 18 in-game menu (a yes turns the in-game hook on too: onboarding_const.a65 ONB_FF_HOOK)
  ('Press [L]+[R]+[Y]+[Left] in a game to pause it and open a menu over it: cheats, savestates, save slots, guides and a RAM trainer. Works with special chips too. {yes} also turns on {hook}: if a game shows graphics glitches, turn it off in {cfg} > {ingame}.',
   'Aperte [L]+[R]+[Y]+[Esquerda] no jogo para pausar e abrir um menu por cima: cheats, savestates, slots de save, guias e treinador de RAM. Funciona também em jogos com chips especiais. {yes} liga também o {hook}: se um jogo der problema gráfico, desligue em {cfg} > {ingame}.',
   'Pulsa [L]+[R]+[Y]+[Izquierda] en el juego para pausarlo y abrir un menú encima: cheats, savestates, ranuras, guías y entrenador de RAM. También en juegos con chips especiales. {yes} activa también el {hook}: si un juego falla en los gráficos, desactívalo en {cfg} > {ingame}.',
   'Drücke [L]+[R]+[Y]+[Links] im Spiel für ein Menü darüber: Cheats, Savestates, Speicherplätze, Anleitungen und RAM-Trainer. Geht auch mit Spezialchips. {yes} schaltet auch {hook} ein: bei Grafikfehlern in einem Spiel in {cfg} > {ingame} ausschalten.',
   'Appuie sur [L]+[R]+[Y]+[Gauche] en jeu pour le mettre en pause sous un menu : cheats, savestates, emplacements, guides et trainer RAM. Marche aussi avec les puces spéciales. {yes} active aussi {hook} : si un jeu a des bugs graphiques, coupe-le dans {cfg} > {ingame}.',
   'Premi [L]+[R]+[Y]+[Sinistra] in gioco per metterlo in pausa con un menu sopra: cheats, savestate, slot, guide e trainer RAM. Anche nei giochi con chip speciali. Con {yes} si attiva anche {hook}: se un gioco ha difetti grafici, disattivalo in {cfg} > {ingame}.',
   'В игре нажми комбинацию кнопок [L]+[R]+[Y]+[Влево], чтобы приостановить игру и открыть |Меню в игре|: чит-коды, сейвстейты, слоты сохранений, руководства и трейнер памяти. Работает и в играх со специальными чипами. {yes} - включает и {hook}: при сбоях графики в игре выключи его в {cfg} > {ingame}.',
   'Druk [L]+[R]+[Y]+[Links] in een spel om te pauzeren met een menu erover: cheats, savestates, opslagplekken, gidsen en RAM-trainer. Werkt ook met speciale chips. {yes} zet ook {hook} aan: bij grafische fouten in een spel zet je het uit in {cfg} > {ingame}.'),
  # 19 savestates
  ('Save the whole game at any moment and load it back later, in 4 slots per game, even in games with special chips. In a game, [Start]+[R] saves and [Start]+[L] loads. Pick the slot in the in-game menu. {yes} also turns on {hook}.',
   'Salve o jogo inteiro a qualquer momento e volte depois, em 4 slots por jogo, até em jogos com chips especiais. No jogo, [Start]+[R] salva e [Start]+[L] carrega. O slot se escolhe no menu in-game. {yes} liga também o {hook}.',
   'Guarda el juego entero en cualquier momento y vuelve luego, en 4 ranuras por juego, incluso con chips especiales. En el juego, [Start]+[R] guarda y [Start]+[L] carga. La ranura se elige en el menú del juego. {yes} activa también el {hook}.',
   'Speichere das ganze Spiel jederzeit und lade es später, in 4 Plätzen pro Spiel, auch mit Spezialchips. Im Spiel sichert [Start]+[R], [Start]+[L] lädt. Den Platz wählst du im Ingame-Menü. {yes} schaltet auch {hook} ein.',
   "Sauvegarde tout le jeu à tout moment et reviens-y plus tard, 4 emplacements par jeu, même avec les puces spéciales. En jeu, [Start]+[R] sauve et [Start]+[L] charge. L'emplacement se choisit dans le menu en jeu. {yes} active aussi {hook}.",
   "Salva l'intero gioco in qualsiasi momento e riprendilo dopo, 4 slot per gioco, anche con i chip speciali. In gioco [Start]+[R] salva e [Start]+[L] carica. Lo slot si sceglie nel menu in gioco. Con {yes} si attiva anche {hook}.",
   'Сохраняй игровой процесс в любой момент и продолжай позже, 4 слота на игру, даже со специальными чипами. В игре комбинация кнопок [Start]+[R] сохраняет, а [Start]+[L] загружает. Слот выбирается в |Меню в игре|. {yes} - включает и {hook}.',
   'Sla het hele spel op elk moment op en laad het later, in 4 plekken per spel, ook met speciale chips. In het spel slaat [Start]+[R] op en laadt [Start]+[L]. Kies de plek in het in-game menu. {yes} zet ook {hook} aan.'),
  # 20 4 battery saves per game, the in-game menu's SAVES tab (needs the in-game menu)
  ('Every game has 4 battery-save slots, so two people can each keep their own. Pick the slot in the {saves} tab of the in-game menu: it applies on the next boot of the game.',
   'Cada jogo tem 4 slots de save de bateria, para duas pessoas terem cada uma o seu. O slot se escolhe na aba {saves} do menu in-game e vale no próximo boot do jogo.',
   'Cada juego tiene 4 ranuras de guardado de batería, para que dos personas tengan cada una la suya. La ranura se elige en la pestaña {saves} del menú del juego y vale en el próximo arranque.',
   'Jedes Spiel hat 4 Batterie-Speicherplätze, damit zwei Leute je ihren eigenen haben. Den Platz wählst du im Tab {saves} des Ingame-Menüs, er gilt beim nächsten Spielstart.',
   "Chaque jeu a 4 emplacements de sauvegarde, pour que deux personnes aient chacune la sienne. L'emplacement se choisit dans l'onglet {saves} du menu en jeu et vaut au prochain démarrage.",
   'Ogni gioco ha 4 slot di salvataggio a batteria, così due persone hanno ognuna il suo. Lo slot si sceglie nella scheda {saves} del menu in gioco e vale al prossimo avvio.',
   'У каждой игры 4 слота сохранения с батарейкой, чтобы у нескольких человек было своё. Слот выбирается на вкладке {saves} в |Меню в игре| и действует со следующего запуска игры.',
   'Elk spel heeft 4 batterij-opslagplekken, zodat twee mensen elk hun eigen hebben. Kies de plek in de tab {saves} van het in-game menu, hij geldt bij de volgende start.'),
  # 21 the in-game menu's RAM trainer (needs the in-game menu)
  ("The {trainer} tab of the in-game menu finds a value in RAM (lives, time, coins): search the number on screen, or {unknown}, and filter as it changes: {changed}, {increased}, {decreased}. Then write the value or freeze it, up to 4 at once, with no cheat made. {savecheat} adds the address to the cheat list.",
   'A aba {trainer} do menu in-game acha um valor na RAM (vidas, tempo, moedas): busque o número da tela, ou {unknown}, e filtre conforme muda: {changed}, {increased}, {decreased}. Depois grave o valor ou congele, até 4 de uma vez, sem criar cheat. {savecheat} põe o endereço na lista de cheats.',
   'La pestaña {trainer} del menú del juego halla un valor en la RAM (vidas, tiempo, monedas): busca el número de la pantalla, o {unknown}, y filtra según cambia: {changed}, {increased}, {decreased}. Luego escribe el valor o congélalo, hasta 4 a la vez, sin crear cheat. {savecheat} lo pone en la lista de cheats.',
   'Der Tab {trainer} im Ingame-Menü findet einen Wert im RAM (Leben, Zeit, Münzen): suche die Zahl vom Bildschirm oder {unknown} und filtere, wenn sie sich ändert: {changed}, {increased}, {decreased}. Dann Wert setzen oder einfrieren, bis zu 4 zugleich, ohne Cheat. {savecheat} legt die Adresse in die Cheat-Liste.',
   "L'onglet {trainer} du menu en jeu trouve une valeur en RAM (vies, temps, pièces) : cherche le nombre affiché, ou {unknown}, puis filtre quand il change : {changed}, {increased}, {decreased}. Écris ensuite la valeur ou fige-la, 4 au plus, sans créer de cheat. {savecheat} la met dans la liste des cheats.",
   "La scheda {trainer} del menu in gioco trova un valore nella RAM (vite, tempo, monete): cerca il numero sullo schermo, o {unknown}, e filtra quando cambia: {changed}, {increased}, {decreased}. Poi scrivi il valore o bloccalo, fino a 4 insieme, senza creare cheat. {savecheat} lo mette nella lista dei cheat.",
   'Вкладка {trainer} в |Меню в игре| находит значение в RAM (жизни, время, монеты): ищи число с экрана или {unknown}, затем используй фильтр по изменению: {changed}, {increased}, {decreased}. Потом запиши значение или заморозь, до 4 сразу, без создания чит-кода. {savecheat} добавляет адрес в список чит-кодов.',
   'De tab {trainer} van het in-game menu vindt een waarde in het RAM (levens, tijd, munten): zoek het getal op het scherm, of {unknown}, en filter als het verandert: {changed}, {increased}, {decreased}. Stel dan de waarde in of zet hem vast, tot 4 tegelijk, zonder cheat. {savecheat} zet het adres in de cheatlijst.'),
  # 22 IPS/BPS patches, and the header mode of each patch ([Y] in the patch list)
  ('IPS and BPS patches next to a ROM bring translations, hacks and fixes without changing the original: [A] on the game asks which one to apply, or none. In that list, [Y] on a patch sets its {hdrmode} (the 512-byte header): {hdrauto}, {hdron} or {hdroff}, remembered per patch.',
   'Patches IPS e BPS ao lado da ROM trazem traduções, hacks e correções sem mudar o original: [A] no jogo pergunta qual aplicar, ou nenhum. Nessa lista, [Y] num patch escolhe o {hdrmode} (header de 512 bytes): {hdrauto}, {hdron} ou {hdroff}, lembrado por patch.',
   'Los parches IPS y BPS junto a una ROM traen traducciones, hacks y arreglos sin cambiar el original: [A] en el juego pregunta cuál aplicar, o ninguno. Ahí, [Y] en un parche elige el {hdrmode} (cabecera de 512 bytes): {hdrauto}, {hdron} o {hdroff}, recordado por parche.',
   'IPS- und BPS-Patches neben einer ROM bringen Übersetzungen, Hacks und Fixes, ohne sie zu ändern: [A] auf dem Spiel fragt, welcher. Dort wählt [Y] auf einem Patch den {hdrmode} (512-Byte-Header): {hdrauto}, {hdron} oder {hdroff}, pro Patch gemerkt.',
   "Les patchs IPS et BPS à côté d'une ROM apportent traductions, hacks et correctifs sans toucher l'original : [A] sur le jeu demande lequel, ou aucun. Là, [Y] sur un patch règle son {hdrmode} (en-tête de 512 octets) : {hdrauto}, {hdron} ou {hdroff}, mémorisé par patch.",
   "Le patch IPS e BPS accanto a una ROM portano traduzioni, hack e correzioni senza toccare l'originale: [A] sul gioco chiede quale applicare, o nessuna. Lì, [Y] su una patch sceglie il {hdrmode} (header di 512 byte): {hdrauto}, {hdron} o {hdroff}, ricordato per patch.",
   'IPS и BPS патчи рядом с игрой дают переводы, хаки и исправления без изменения исходного файла: кнопка [A] на игре спросит, какой патч применить или запустить её без патча. Кнопка [Y] на патче задаёт режим {hdrmode} (заголовок 512 байт): {hdrauto}, {hdron} или {hdroff}.',
   'IPS- en BPS-patches naast een ROM brengen vertalingen, hacks en fixes zonder het origineel te wijzigen: [A] op het spel vraagt welke, of geen. Daar kiest [Y] op een patch de {hdrmode} (512-byte header): {hdrauto}, {hdron} of {hdroff}, per patch onthouden.'),
  # 23 creating the patched ROM from the [Y] menu of a patch
  ('In the patch list, [Y] on a patch and {createrom} save a copy of the ROM with the patch already applied, next to the original. Its cover, info card, guides, cheats, saves and states go along with it.',
   'Na lista de patches, [Y] sobre um patch e {createrom} gravam ao lado da original uma cópia da ROM já com o patch. A capa, a ficha, as guias, os cheats, os saves e os states vão junto.',
   'En la lista de parches, [Y] sobre un parche y {createrom} guardan junto a la original una copia de la ROM ya parcheada. Su carátula, ficha, guías, cheats, saves y states van con ella.',
   'In der Patch-Liste legen [Y] auf einem Patch und {createrom} neben dem Original eine Kopie der ROM mit dem Patch an. Cover, Infokarte, Anleitungen, Cheats, Saves und States kommen mit.',
   "Dans la liste des patchs, [Y] sur un patch puis {createrom} enregistre à côté de l'original une copie de la ROM déjà patchée. Jaquette, fiche, guides, cheats, saves et states suivent.",
   "Nell'elenco delle patch, [Y] su una patch e poi {createrom} salva accanto all'originale una copia della ROM già patchata. Copertina, scheda, guide, cheat, save e state la seguono.",
   'В списке патчей нажатие кнопки [Y] на патче и выбор пункта {createrom} создают в той же папке новый пропатченный ROM-файл. Обложка, карточка, руководства, чит-коды и сохранения копируются с оригинала.',
   'In de patchlijst zetten [Y] op een patch en {createrom} naast het origineel een kopie van de ROM met de patch erin. Hoes, spelinfo, gidsen, cheats, saves en states gaan mee.'),
  # 24 more special chips (its own card)
  ('Besides DSP, Super FX, SA-1, S-DD1 and CX4, more special chips run: the SPC7110 with its clock, saved on the card, Super FX 3, the Sufami Turbo adapter and the competition carts Campus Challenge 92 and PowerFest 94. Some need a BIOS file in |/sd2snes|.',
   'Além de DSP, Super FX, SA-1, S-DD1 e CX4, mais chips especiais rodam: o SPC7110 com o relógio, salvo no cartão, o Super FX 3, o adaptador Sufami Turbo e os cartuchos de competição Campus Challenge 92 e PowerFest 94. Alguns pedem um arquivo de BIOS em |/sd2snes|.',
   'Además de DSP, Super FX, SA-1, S-DD1 y CX4, funcionan más chips especiales: el SPC7110 con su reloj, guardado en la tarjeta, el Super FX 3, el adaptador Sufami Turbo y los cartuchos de competición Campus Challenge 92 y PowerFest 94. Algunos piden un BIOS en |/sd2snes|.',
   'Neben DSP, Super FX, SA-1, S-DD1 und CX4 laufen weitere Spezialchips: der SPC7110 mit Uhr, auf der Karte gespeichert, Super FX 3, der Sufami-Turbo-Adapter und die Turniermodule Campus Challenge 92 und PowerFest 94. Manche brauchen ein BIOS in |/sd2snes|.',
   "En plus des DSP, Super FX, SA-1, S-DD1 et CX4, d'autres puces spéciales marchent : le SPC7110 avec son horloge, gardée sur la carte, le Super FX 3, l'adaptateur Sufami Turbo et les cartouches de tournoi Campus Challenge 92 et PowerFest 94. Certaines demandent un BIOS dans |/sd2snes|.",
   "Oltre a DSP, Super FX, SA-1, S-DD1 e CX4 funzionano altri chip speciali: lo SPC7110 con il suo orologio, salvato sulla scheda, il Super FX 3, l'adattatore Sufami Turbo e le cartucce da torneo Campus Challenge 92 e PowerFest 94. Alcuni vogliono un BIOS in |/sd2snes|.",
   'Кроме DSP, Super FX, SA-1, S-DD1 и CX4 работают и другие специальные чипы: |SPC7110| с часами реального времени, которые хранятся на карте памяти, |Super FX3|, адаптер |Sufami Turbo| и турнирные картриджи |Campus Challenge 92| и |PowerFest 94|. Некоторым нужен |BIOS| в папке |/sd2snes|.',
   'Naast DSP, Super FX, SA-1, S-DD1 en CX4 werken meer speciale chips: de SPC7110 met zijn klok, bewaard op de kaart, Super FX 3, de Sufami Turbo-adapter en de toernooicartridges Campus Challenge 92 en PowerFest 94. Sommige hebben een BIOS in |/sd2snes| nodig.'),
  # 25 Sufami Turbo: the Slot B picker
  ("Starting a Sufami Turbo cart (|.st|) asks which minicart goes in Slot B: the other |.st| files of the same folder, or none. The pair is remembered for next time. Needs the adapter's BIOS as |/sd2snes/stbios.bin|.",
   'Ao iniciar um cartucho Sufami Turbo (|.st|), o menu pergunta qual minicartucho vai no Slot B: os outros |.st| da mesma pasta, ou nenhum. O par fica lembrado para a próxima vez. Precisa do BIOS do adaptador em |/sd2snes/stbios.bin|.',
   'Al iniciar un cartucho Sufami Turbo (|.st|), el menú pregunta qué minicartucho va en la ranura B: los otros |.st| de la misma carpeta, o ninguno. El par se recuerda para la próxima vez. Necesita la BIOS del adaptador en |/sd2snes/stbios.bin|.',
   'Beim Start eines Sufami-Turbo-Moduls (|.st|) fragt das Menü, welches Minimodul in Slot B kommt: die anderen |.st| im selben Ordner oder keins. Das Paar wird gemerkt. Braucht das BIOS des Adapters als |/sd2snes/stbios.bin|.',
   "Au lancement d'une cartouche Sufami Turbo (|.st|), le menu demande quelle minicartouche va dans le port B : les autres |.st| du même dossier, ou aucune. La paire est mémorisée. Il faut le BIOS de l'adaptateur en |/sd2snes/stbios.bin|.",
   "All'avvio di una cartuccia Sufami Turbo (|.st|), il menu chiede quale minicartuccia va nello Slot B: gli altri |.st| della stessa cartella, o nessuna. La coppia viene ricordata. Serve il BIOS dell'adattatore in |/sd2snes/stbios.bin|.",
   'При запуске картриджа Sufami Turbo (|.st|) система предложит выбрать мини-картридж для слота "B": другой файл |.st| из той же папки или пустой слот. Пара запоминается автоматически. Нужен файл BIOS адаптера |stbios.bin| в папке |/sd2snes|.',
   'Bij het starten van een Sufami Turbo-cartridge (|.st|) vraagt het menu welke minicartridge in slot B gaat: de andere |.st| in dezelfde map, of geen. Het paar wordt onthouden. De BIOS van de adapter moet in |/sd2snes/stbios.bin|.'),
  # 26 Competition Carts: the round's minutes (3..18, the events used 6)
  ('The Campus Challenge 92 and PowerFest 94 carts play a timed round, like at the original events. Choose how long it lasts, from 3 to 18 minutes: the events used 6. Also in {chipopts}.',
   'Os cartuchos Campus Challenge 92 e PowerFest 94 jogam um round cronometrado, como nos eventos originais. Escolha quanto tempo ele dura, de 3 a 18 minutos: os eventos usavam 6. Também em {chipopts}.',
   'Los cartuchos Campus Challenge 92 y PowerFest 94 juegan una ronda cronometrada, como en los eventos originales. Elige cuánto dura, de 3 a 18 minutos: los eventos usaban 6. También en {chipopts}.',
   'Die Module Campus Challenge 92 und PowerFest 94 spielen eine Runde auf Zeit, wie bei den echten Turnieren. Wähle die Dauer, 3 bis 18 Minuten: die Turniere nutzten 6. Auch unter {chipopts}.',
   "Les cartouches Campus Challenge 92 et PowerFest 94 jouent une manche chronométrée, comme aux tournois d'origine. Choisis sa durée, de 3 à 18 minutes : les tournois utilisaient 6. Aussi dans {chipopts}.",
   'Le cartucce Campus Challenge 92 e PowerFest 94 giocano un round a tempo, come ai tornei originali. Scegli quanto dura, da 3 a 18 minuti: i tornei usavano 6. Anche in {chipopts}.',
   'В картриджах |Campus Challenge 92| и |PowerFest 94| раунд идёт по внутреннему таймеру, как на настоящих турнирах. Выбери его продолжительность от 3 до 18 минут (на турнирах было 6). Настраивается в {chipopts}.',
   'De cartridges Campus Challenge 92 en PowerFest 94 spelen een ronde op tijd, zoals op de echte toernooien. Kies hoe lang die duurt, 3 tot 18 minuten: de toernooien gebruikten 6. Ook in {chipopts}.'),
  # 27 other consoles: NES, Master System, Game Boy Color (Mk.III only), Atari 2600 (both boards), experimental
  ('NES, Master System, Game Boy Color and Atari 2600 games run on their own FPGA cores. They are experimental: some games may glitch or not run. On the NES, [L]+[R]+[Start]+[Up] repaints the palette if the colours look wrong. Only on |FXPAK PRO (Mk.III)|, except the Atari 2600: available on the |sd2snes Mk.II| from version 2.19.',
   'Jogos de NES, Master System, Game Boy Color e Atari 2600 rodam em cores de FPGA próprios. São experimentais: alguns jogos podem ter falhas ou não rodar. No NES, [L]+[R]+[Start]+[Cima] repinta a paleta se as cores saírem erradas. Só no |FXPAK PRO (Mk.III)|, menos o Atari 2600: disponível no |sd2snes Mk.II| a partir da versão 2.19.',
   'Los juegos de NES, Master System, Game Boy Color y Atari 2600 corren en sus propios cores FPGA. Son experimentales: algunos juegos pueden fallar o no arrancar. En la NES, [L]+[R]+[Start]+[Arriba] repinta la paleta si los colores salen mal. Solo en |FXPAK PRO (Mk.III)|, salvo el Atari 2600: disponible en |sd2snes Mk.II| desde la versión 2.19.',
   'NES-, Master-System-, Game-Boy-Color- und Atari-2600-Spiele laufen auf eigenen FPGA-Cores. Sie sind experimentell: manche Spiele haben Fehler oder laufen nicht. Beim NES malt [L]+[R]+[Start]+[Oben] die Palette neu, wenn Farben falsch sind. Nur auf |FXPAK PRO (Mk.III)|, außer Atari 2600: auf |sd2snes Mk.II| ab Version 2.19 verfügbar.',
   "Les jeux NES, Master System, Game Boy Color et Atari 2600 tournent sur leurs propres cores FPGA. Ils sont expérimentaux : des jeux peuvent bugger ou ne pas démarrer. Sur NES, [L]+[R]+[Start]+[Haut] redessine la palette si les couleurs sont fausses. Seulement sur |FXPAK PRO (Mk.III)|, sauf l'Atari 2600 : disponible sur |sd2snes Mk.II| dès la version 2.19.",
   "I giochi NES, Master System, Game Boy Color e Atari 2600 girano su core FPGA dedicati. Sono sperimentali: alcuni giochi possono avere difetti o non partire. Sul NES, [L]+[R]+[Start]+[Su] ridisegna la tavolozza se i colori sono sbagliati. Solo su |FXPAK PRO (Mk.III)|, tranne l'Atari 2600: disponibile su |sd2snes Mk.II| dalla versione 2.19.",
   'Игры NES, Master System, Game Boy Color и Atari 2600 идут на своих ядрах FPGA. Ядра экспериментальные: некоторые игры могут сбоить или не запускаться. У NES кнопки [L]+[R]+[Start]+[Вверх] перерисовывают палитру, если цвета неверны. Только на |FXPAK PRO (Mk.III)|, кроме Atari 2600: доступен на |sd2snes Mk.II| начиная с версии 2.19.',
   'NES-, Master System-, Game Boy Color- en Atari 2600-spellen draaien op eigen FPGA-cores. Ze zijn experimenteel: sommige spellen haperen of starten niet. Op de NES tekent [L]+[R]+[Start]+[Omhoog] het palet opnieuw als de kleuren fout zijn. Alleen op |FXPAK PRO (Mk.III)|, behalve de Atari 2600: op |sd2snes Mk.II| beschikbaar vanaf versie 2.19.'),
  # 28 the Atari 2600's console switches and player keys on the pad (both boards), and its picture width (Mk.III)
  ("The console's switches are on controller 1: [Start] is RESET and [Select] is SELECT while held, [L] steps the difficulty (Easy, Normal, Hard), [R] pauses, [X] flips Color and B/W, [Y] changes the palette. {a26w} in {chipopts} picks 160 px (1:1) or 256 px (wide), |FXPAK PRO (Mk.III)| only.",
   'As chaves do console ficam no controle 1: [Start] é o RESET e [Select] o SELECT enquanto segurados, [L] passa a dificuldade (Easy, Normal, Hard), [R] pausa, [X] alterna Cor e P/B, [Y] troca a paleta. {a26w} em {chipopts} escolhe 160 px (1:1) ou 256 px (esticado), só no |FXPAK PRO (Mk.III)|.',
   'Los interruptores de la consola están en el mando 1: [Start] es RESET y [Select] es SELECT mientras se mantienen, [L] cambia la dificultad (Easy, Normal, Hard), [R] pausa, [X] cambia Color y B/N, [Y] la paleta. {a26w} en {chipopts} elige 160 px (1:1) o 256 px (ancho), solo en |FXPAK PRO (Mk.III)|.',
   'Die Schalter der Konsole liegen auf Controller 1: [Start] ist RESET und [Select] SELECT, solange gehalten, [L] wechselt die Schwierigkeit (Easy, Normal, Hard), [R] pausiert, [X] wechselt Farbe und S/W, [Y] die Palette. {a26w} in {chipopts}: 160 px (1:1) oder 256 px (breit), nur |FXPAK PRO (Mk.III)|.',
   "Sur la manette 1, [Start] est RESET et [Select] SELECT tant qu'ils sont tenus, [L] change la difficulté (Easy, Normal, Hard), [R] met en pause, [X] bascule Couleur et N/B, [Y] change la palette. {a26w} ({chipopts}) choisit 160 px (1:1) ou 256 px (large), |FXPAK PRO (Mk.III)| seulement.",
   'Gli interruttori della console sono sul controller 1: [Start] è RESET e [Select] è SELECT finché tenuti, [L] cambia la difficoltà (Easy, Normal, Hard), [R] mette in pausa, [X] alterna Colore e B/N, [Y] cambia la tavolozza. {a26w} in {chipopts}: 160 px (1:1) o 256 px (largo), solo |FXPAK PRO (Mk.III)|.',
   'Переключатели консоли на контроллере 1: кнопка [Start] выполняет функцию RESET, а кнопка [Select] - функцию SELECT (пока кнопка удерживается). [L] меняет сложность (Easy, Normal, Hard), [R] ставит паузу, [X] переключает цветной и чёрно-белый режим, [Y] меняет палитру. {a26w} в {chipopts}: 160 пикселей (1:1) или 256 (широко), только для |FXPAK PRO (Mk.III)|.',
   'De schakelaars van de console zitten op controller 1: [Start] is RESET en [Select] SELECT zolang ingedrukt, [L] wisselt de moeilijkheid (Easy, Normal, Hard), [R] pauzeert, [X] wisselt Kleur en Z/W, [Y] het palet. {a26w} in {chipopts}: 160 px (1:1) of 256 px (breed), alleen |FXPAK PRO (Mk.III)|.'),
  # 29 the card's folders: 2-letter buckets, console folders, |Organize|
  ('Saves, states, cheats, info cards and patch settings of each game live in |/sd2snes/<area>/<2 letters>/|, plus a console folder (|sgb, sft, nes, sms, a26|). Updating only the firmware? Run |Organize| in the |Web Manager|, or old saves seem to vanish.',
   'Saves, states, cheats, fichas e ajustes de patch de cada jogo ficam em |/sd2snes/<área>/<2 letras>/|, mais uma pasta por console (|sgb, sft, nes, sms, a26|). Vai atualizar só a firmware? Rode o |Organize| do |Web Manager|, senão os saves antigos parecem sumir.',
   'Saves, states, cheats, fichas y ajustes de parche de cada juego están en |/sd2snes/<área>/<2 letras>/|, más una carpeta por consola (|sgb, sft, nes, sms, a26|). ¿Solo actualizas el firmware? Usa |Organize| en el |Web Manager|, o los saves viejos parecerán perdidos.',
   'Saves, States, Cheats, Infokarten und Patch-Einstellungen jedes Spiels liegen in |/sd2snes/<Bereich>/<2 Buchstaben>/|, dazu ein Konsolenordner (|sgb, sft, nes, sms, a26|). Nur die Firmware aktualisiert? |Organize| im |Web Manager| starten, sonst wirken alte Saves verschwunden.',
   'Saves, states, cheats, fiches et réglages de patch de chaque jeu sont dans |/sd2snes/<zone>/<2 lettres>/|, plus un dossier par console (|sgb, sft, nes, sms, a26|). Tu mets à jour seulement le firmware ? Lance |Organize| dans le |Web Manager|, sinon les anciens saves semblent perdus.',
   'Save, state, cheat, schede e impostazioni delle patch di ogni gioco stanno in |/sd2snes/<area>/<2 lettere>/|, più una cartella per console (|sgb, sft, nes, sms, a26|). Aggiorni solo il firmware? Usa |Organize| nel |Web Manager|, o i vecchi save sembreranno spariti.',
   'Сохранения, сейвстейты, чит-коды, карточки и настройки патчей каждой игры лежат в |/sd2snes/<раздел>/<2 буквы>/|, плюс папка консоли (|sgb, sft, nes, sms, a26|). Обновляете только прошивку? Откройте |Web Manager| на сайте, укажите ему карту памяти и запустите |Organize|, иначе старые сохранения пропадут из виду.',
   'Saves, states, cheats, spelinfo en patch-instellingen van elk spel staan in |/sd2snes/<gebied>/<2 letters>/|, plus een consolemap (|sgb, sft, nes, sms, a26|). Werk je alleen de firmware bij? Draai |Organize| in de |Web Manager|, anders lijken oude saves weg.'),
  # 30 the memory test of the main menu
  ("{memtest}, in the main menu, checks the cartridge's memory right on the console: [A] runs the full test, the wiring test included, in about 60 seconds. The console resets at the end.",
   '{memtest}, no menu principal, testa a memória do cartucho no próprio console: [A] roda o teste completo, que já inclui o de fiação, em uns 60 segundos. O console reinicia no fim.',
   '{memtest}, en el menú principal, prueba la memoria del cartucho en la consola: [A] hace la prueba completa, que ya incluye la del cableado, en unos 60 segundos. La consola se reinicia al final.',
   '{memtest} im Hauptmenü prüft den Speicher des Moduls direkt an der Konsole: [A] startet den vollen Test, Leitungstest inklusive, etwa 60 Sekunden. Danach startet die Konsole neu.',
   '{memtest}, dans le menu principal, teste la mémoire de la cartouche sur la console : [A] lance le test complet, câblage compris, en 60 secondes environ. La console redémarre à la fin.',
   '{memtest}, nel menu principale, verifica la memoria della cartuccia sulla console: [A] avvia il test completo, cablaggio incluso, in circa 60 secondi. Alla fine la console si riavvia.',
   '{memtest} в главном меню позволяет проверить память картриджа непосредственно на консоли. При нажатии кнопки [A] запускается полный тест (включая проверку соединений), который длится около 60 секунд. По завершении теста консоль перезагружается.',
   '{memtest} in het hoofdmenu test het geheugen van de cartridge op de console: [A] start de volledige test, bedrading inbegrepen, in ongeveer 60 seconden. Daarna herstart de console.'),
  # 31 the Mk.II LED codes for a boot without a picture (Mk.II only)
  ('To make room for more features on the |Mk.II (SD2SNES)|, its boot error screen was replaced by blinking LED patterns. What is missing:\nSD card: green/red\n|fpga_mini.bit|: green/yellow\nThat file goes in |/sd2snes|.',
   'Para abrir espaço para mais recursos no |Mk.II (SD2SNES)|, a tela de erro do boot virou combinações de LEDs piscando. O que falta:\nCartão SD: verde/vermelho\n|fpga_mini.bit|: verde/amarelo\nEsse arquivo fica em |/sd2snes|.',
   'Para hacer sitio a más funciones en el |Mk.II (SD2SNES)|, la pantalla de error de arranque se cambió por combinaciones de LED parpadeando. Lo que falta:\nTarjeta SD: verde/rojo\n|fpga_mini.bit|: verde/amarillo\nEse archivo va en |/sd2snes|.',
   'Um auf dem |Mk.II (SD2SNES)| Platz für mehr Funktionen zu schaffen, wurde der Startfehler-Bildschirm durch blinkende LED-Muster ersetzt. Was fehlt:\nSD-Karte: grün/rot\n|fpga_mini.bit|: grün/gelb\nDie Datei gehört nach |/sd2snes|.',
   "Pour faire de la place à plus de fonctions sur le |Mk.II (SD2SNES)|, l'écran d'erreur au démarrage a été remplacé par des combinaisons de LED qui clignotent. Ce qui manque :\nCarte SD : vert/rouge\n|fpga_mini.bit| : vert/jaune\nCe fichier va dans |/sd2snes|.",
   "Per fare spazio a più funzioni sul |Mk.II (SD2SNES)|, la schermata di errore all'avvio è stata sostituita da combinazioni di LED lampeggianti. Cosa manca:\nScheda SD: verde/rosso\n|fpga_mini.bit|: verde/giallo\nIl file va in |/sd2snes|.",
   'Чтобы освободить место для новых функций на |Mk.II (SD2SNES)|, экран ошибок запуска заменён миганием светодиодов. Что отсутствует:\nSD-карта: зелёный/красный\n|fpga_mini.bit|: зелёный/жёлтый\nЭтот файл лежит в |/sd2snes|.',
   'Om op de |Mk.II (SD2SNES)| ruimte te maken voor meer functies, is het foutscherm bij het opstarten vervangen door knipperende LED-patronen. Wat ontbreekt:\nSD-kaart: groen/rood\n|fpga_mini.bit|: groen/geel\nDat bestand hoort in |/sd2snes|.'),
  # 32 "and more" (the base tour's last card)
  ("",) * 8,
  # 33 the 2.17 section: what is new in this release
  ("Version 2.17 adds a Game Boy Color core, |FXPAK PRO (Mk.III)| only, the ST011 and ST018 special chips, copy-protected bootleg carts, Dutch as the menu's eighth language and menu music that comes with the firmware, a random track by default. The next cards show the rest.",
   'A versão 2.17 traz um core de Game Boy Color, só no |FXPAK PRO (Mk.III)|, os chips especiais ST011 e ST018, cartuchos piratas com proteção, o holandês como oitavo idioma e música de menu que vem com a firmware, aleatória por padrão. Os próximos cards mostram o resto.',
   'La versión 2.17 trae un core de Game Boy Color, solo en |FXPAK PRO (Mk.III)|, los chips especiales ST011 y ST018, cartuchos piratas con protección, el neerlandés como octavo idioma y música de menú incluida en el firmware, aleatoria por defecto. Lo demás, en las próximas pantallas.',
   'Version 2.17 bringt einen Game-Boy-Color-Core, nur für |FXPAK PRO (Mk.III)|, die Spezialchips ST011 und ST018, kopiergeschützte Bootleg-Module, Niederländisch als achte Sprache und Menümusik mit der Firmware, standardmäßig zufällig. Die nächsten Karten zeigen den Rest.',
   'La version 2.17 apporte un core Game Boy Color, sur |FXPAK PRO (Mk.III)| seulement, les puces spéciales ST011 et ST018, les cartouches pirates protégées, le néerlandais en huitième langue et une musique de menu fournie, aléatoire par défaut. La suite dans les écrans suivants.',
   "La versione 2.17 porta un core Game Boy Color, solo per |FXPAK PRO (Mk.III)|, i chip speciali ST011 e ST018, le cartucce pirata protette, l'olandese come ottava lingua e musica del menu inclusa nel firmware, casuale di default. Le prossime schede mostrano il resto.",
   'В версии 2.17: ядро Game Boy Color, только для |FXPAK PRO (Mk.III)|, специальные чипы |ST011| и |ST018|, нелицензионные игры с защитой, нидерландский как восьмой язык и музыка меню в комплекте с прошивкой, по умолчанию случайная. |ДАЛЕЕ| - остальные новинки.',
   'Versie 2.17 brengt een Game Boy Color-core, alleen voor |FXPAK PRO (Mk.III)|, de speciale chips ST011 en ST018, beveiligde bootleg-cartridges, Nederlands als achtste taal en menumuziek bij de firmware, standaard willekeurig. De volgende kaarten tonen de rest.'),
  # 34 controller 2 (2.17)
  ('The in-game shortcuts/hooks (menu, savestates, back to the menu) also work from controller 2, so the second player can use them too. Controller 1 keeps priority. {yes} also turns on {hook} and {igbuttons}.',
   'Os atalhos/hooks in-game (menu, savestates, voltar ao menu) também funcionam no segundo controle, então o jogador 2 também pode usar. O controle 1 tem prioridade. {yes} liga também o {hook} e os {igbuttons}.',
   'Los atajos/hooks del juego (menú, savestates, volver al menú) también funcionan en el segundo mando, así el otro jugador puede usarlos. El mando 1 tiene prioridad. {yes} activa también el {hook} y los {igbuttons}.',
   'Die Ingame-Kürzel/Hooks (Menü, Savestates, zurück zum Menü) gehen auch an Controller 2, so kann sie auch Spieler 2 nutzen. Controller 1 hat Vorrang. {yes} schaltet auch {hook} und {igbuttons} ein.',
   'Les raccourcis/hooks en jeu (menu, savestates, retour au menu) marchent aussi sur la deuxième manette, pour le joueur 2. La manette 1 reste prioritaire. {yes} active aussi {hook} et {igbuttons}.',
   'Le scorciatoie/hook in gioco (menu, savestate, ritorno al menu) funzionano anche col controller 2, così le usa anche il secondo giocatore. Il controller 1 ha la precedenza. Con {yes} si attivano anche {hook} e {igbuttons}.',
   'Игровые комбинации/перехваты (меню, сейвстейты, выход в меню) работают и на контроллере 2, их может использовать второй игрок. У контроллера 1 приоритет. {yes} - включает и {hook}, и {igbuttons}.',
   'De in-game sneltoetsen/hooks (menu, savestates, terug naar menu) werken ook op controller 2, dus ook de tweede speler kan ze gebruiken. Controller 1 heeft voorrang. {yes} zet ook {hook} en {igbuttons} aan.'),
  # 35 Game Boy Color (2.17)
  ('Put a |.gbc| on the card and it runs on a Game Boy Color core instead of the Super Game Boy, in color and at full speed. {sgbmenu} picks {auto}, {prefsgb} or {prefgbc}. Experimental, |FXPAK PRO (Mk.III)| only.',
   'Coloque um |.gbc| no cartão e ele roda num core de Game Boy Color em vez do Super Game Boy, em cores e a toda velocidade. Em {sgbmenu}: {auto}, {prefsgb} ou {prefgbc}. Experimental, só no |FXPAK PRO (Mk.III)|.',
   'Pon un |.gbc| en la tarjeta y corre en un core de Game Boy Color en vez del Super Game Boy, en color y a toda velocidad. En {sgbmenu}: {auto}, {prefsgb} o {prefgbc}. Experimental, solo en |FXPAK PRO (Mk.III)|.',
   'Lege eine |.gbc| auf die Karte und sie läuft auf einem Game-Boy-Color-Core statt dem Super Game Boy, in Farbe und mit voller Geschwindigkeit. In {sgbmenu}: {auto}, {prefsgb} oder {prefgbc}. Experimentell, nur |FXPAK PRO (Mk.III)|.',
   'Mets un |.gbc| sur la carte et il tourne sur un core Game Boy Color au lieu du Super Game Boy, en couleur et à pleine vitesse. Dans {sgbmenu} : {auto}, {prefsgb} ou {prefgbc}. Expérimental, |FXPAK PRO (Mk.III)| seulement.',
   'Metti un |.gbc| sulla scheda e gira su un core Game Boy Color invece del Super Game Boy, a colori e a piena velocità. In {sgbmenu}: {auto}, {prefsgb} o {prefgbc}. Sperimentale, solo |FXPAK PRO (Mk.III)|.',
   'Запишите игру |.gbc| на карту памяти, и она запустится на ядре Game Boy Color вместо Super Game Boy, в цвете и на полной скорости. В {sgbmenu}: {auto}, {prefsgb} или {prefgbc}. Экспериментально, только |FXPAK PRO (Mk.III)|.',
   'Zet een |.gbc| op de kaart en hij draait op een Game Boy Color-core in plaats van de Super Game Boy, in kleur en op volle snelheid. In {sgbmenu}: {auto}, {prefsgb} of {prefgbc}. Experimenteel, alleen |FXPAK PRO (Mk.III)|.'),
  # 36 the in-game shortcut list (2.17)
  ('In the in-game menu, [SELECT] on the tab bar lists every button shortcut/hook of the game you are playing, with the combos armed for it: open the menu, save and load states, reset, cheats on and off.',
   'No menu in-game, [SELECT] na barra de abas lista todos os atalhos/hooks de botão do jogo que você está jogando, com as combinações ativas para ele: abrir o menu, salvar e carregar estados, reset, ligar e desligar cheats.',
   'En el menú del juego, [SELECT] en la barra de pestañas lista todos los atajos/hooks del juego actual, con las combinaciones activas para él: abrir el menú, guardar y cargar estados, reset, activar y desactivar cheats.',
   'Im Ingame-Menü zeigt [SELECT] auf der Tab-Leiste alle Tastenkürzel/Hooks des laufenden Spiels, mit den dafür aktiven Kombinationen: Menü öffnen, Zustände sichern und laden, Reset, Cheats an und aus.',
   "Dans le menu en jeu, [SELECT] sur la barre d'onglets liste tous les raccourcis/hooks du jeu en cours, avec les combinaisons actives pour lui : ouvrir le menu, sauver et charger, reset, cheats on et off.",
   'Nel menu in gioco, [SELECT] sulla barra delle schede elenca tutte le scorciatoie/hook del gioco in corso, con le combinazioni attive: aprire il menu, salvare e caricare stati, reset, cheat on e off.',
   'В |Меню в игре| кнопка [SELECT] на панели вкладок показывает все комбинации/перехваты кнопок для текущей игры: открыть меню, сохранить и загрузить сейвстейты, сбросить, включить и выключить чит-коды.',
   'In het in-game menu toont [SELECT] op de tabbalk alle sneltoetsen/hooks voor het huidige spel: menu openen, states opslaan en laden, reset, cheats aan en uit.'),
  # 37 Seta chips and bootlegs (2.17)
  ('Morita Shougi 1 and 2 now run: the Seta ST011 and ST018 special chips need |st011.rom| and |st018.rom| in |/sd2snes|. Copy-protected bootleg carts boot from the untouched dump.',
   'Morita Shougi 1 e 2 agora rodam: os chips especiais Seta ST011 e ST018 precisam de |st011.rom| e |st018.rom| em |/sd2snes|. Bootlegs com proteção contra cópia iniciam do dump original.',
   'Morita Shougi 1 y 2 ya funcionan: los chips especiales Seta ST011 y ST018 necesitan |st011.rom| y |st018.rom| en |/sd2snes|. Los bootlegs con protección anticopia arrancan desde el volcado original.',
   'Morita Shougi 1 und 2 laufen jetzt: die Seta-Spezialchips ST011 und ST018 brauchen |st011.rom| und |st018.rom| in |/sd2snes|. Kopiergeschützte Bootlegs starten vom unveränderten Dump.',
   'Morita Shougi 1 et 2 tournent enfin : les puces spéciales Seta ST011 et ST018 demandent |st011.rom| et |st018.rom| dans |/sd2snes|. Les bootlegs protégés contre la copie démarrent depuis le dump intact.',
   'Morita Shougi 1 e 2 ora funzionano: i chip speciali Seta ST011 e ST018 richiedono |st011.rom| e |st018.rom| in |/sd2snes|. I bootleg con protezione anticopia partono dal dump originale.',
   'Теперь работают Morita Shougi и Morita Shougi 2! Играм на чипах Seta ST011 и ST018 нужны файлы |st011.rom| и |st018.rom| в папке |/sd2snes|. Нелицензионные игры с защитой от копирования грузятся из чистого дампа.',
   'Morita Shougi 1 en 2 werken nu: de speciale Seta-chips ST011 en ST018 hebben |st011.rom| en |st018.rom| in |/sd2snes| nodig. Kopieerbeveiligde bootlegs starten vanaf de originele dump.'),
  # 38 Super 20 in 1, Gamars Puzzle and |.sfrom| (2.17)
  ('The Super 20 in 1 multicart opens its own game menu, Gamars Puzzle runs, and |.sfrom| files from the SNES Classic load like any ROM.',
   'O multicart Super 20 in 1 abre o próprio menu de jogos, o Gamars Puzzle roda e arquivos |.sfrom| do SNES Classic carregam como qualquer ROM.',
   'El multicart Super 20 in 1 abre su propio menú de juegos, Gamars Puzzle funciona y los archivos |.sfrom| del SNES Classic cargan como cualquier ROM.',
   'Das Multicart Super 20 in 1 öffnet sein eigenes Spielemenü, Gamars Puzzle läuft, und |.sfrom|-Dateien vom SNES Classic laden wie jede ROM.',
   'Le multicart Super 20 in 1 ouvre son propre menu de jeux, Gamars Puzzle tourne et les fichiers |.sfrom| de la SNES Classic se lancent comme une ROM.',
   'Il multicart Super 20 in 1 apre il suo menu di giochi, Gamars Puzzle funziona e i file |.sfrom| dello SNES Classic si caricano come una ROM.',
   'Мультиигровой картридж Super 20 in 1 работает через своё меню игр, запускается Gamars Puzzle, а файлы |.sfrom| от SNES Classic грузятся как обычные игры.',
   'De multicart Super 20 in 1 opent zijn eigen spelmenu, Gamars Puzzle draait en |.sfrom|-bestanden van de SNES Classic laden als elke ROM.'),
  # 39 file-type icons in the list (2.17)
  ('Every row of the file list starts with an icon of its type: SNES game, NES, Master System, Game Boy, Atari 2600, music, theme, folder. A folder that opens as an |MSU-1| game shows a yellow controller. With {no}, the list shows the names only, as before.',
   'Cada linha da lista começa com um ícone do tipo: jogo de SNES, NES, Master System, Game Boy, Atari 2600, música, tema, pasta. Uma pasta que abre como jogo |MSU-1| mostra um controle amarelo. Com {no}, a lista mostra só os nomes, como antes.',
   'Cada fila de la lista empieza con un icono de su tipo: juego de SNES, NES, Master System, Game Boy, Atari 2600, música, tema, carpeta. Una carpeta que abre como juego |MSU-1| muestra un mando amarillo. Con {no}, la lista muestra solo los nombres, como antes.',
   'Jede Zeile der Liste beginnt mit einem Symbol ihres Typs: SNES-Spiel, NES, Master System, Game Boy, Atari 2600, Musik, Theme, Ordner. Ein Ordner, der als |MSU-1|-Spiel startet, zeigt einen gelben Controller. Mit {no} zeigt die Liste nur die Namen, wie früher.',
   'Chaque ligne de la liste commence par une icône de son type : jeu SNES, NES, Master System, Game Boy, Atari 2600, musique, thème, dossier. Un dossier qui se lance comme un jeu |MSU-1| montre une manette jaune. Avec {no}, la liste ne montre que les noms, comme avant.',
   'Ogni riga della lista inizia con una icona del suo tipo: gioco SNES, NES, Master System, Game Boy, Atari 2600, musica, tema, cartella. Una cartella che si apre come gioco |MSU-1| mostra un controller giallo. Con {no}, la lista mostra solo i nomi, come prima.',
   'Начало каждой строки списка в браузере имеет свой тип значка: Игра (SNES, NES, Master System, Game Boy, Atari 2600), Музыка, Тема и Папка. Папка |MSU-1|, которая открывается как игра, обозначена жёлтым контроллером. Если выбрать {no}, в списке будут только имена файлов и папок, как раньше.',
   'Elke regel van de lijst begint met een pictogram van het type: SNES-spel, NES, Master System, Game Boy, Atari 2600, muziek, thema, map. Een map die als |MSU-1|-spel opent, toont een gele controller. Met {no} toont de lijst alleen de namen, zoals vroeger.'),
  # 40 the cheat list from the game info card (2.17)
  ("[SELECT] on the game info card opens that game's cheat list: turn codes on and off, add or edit them, then start the game with them already set.",
   '[SELECT] na ficha do jogo abre a lista de cheats dele: ligue e desligue códigos, adicione ou edite, e inicie o jogo com eles já prontos.',
   '[SELECT] en la ficha del juego abre su lista de cheats: activa y desactiva códigos, añade o edita, y empieza el juego con ellos listos.',
   '[SELECT] auf der Infokarte öffnet die Cheat-Liste des Spiels: Codes an- und ausschalten, hinzufügen oder ändern, dann mit ihnen starten.',
   '[SELECT] sur la fiche du jeu ouvre sa liste de cheats : active ou désactive les codes, ajoute ou modifie, puis lance le jeu avec eux.',
   '[SELECT] sulla scheda del gioco apre la sua lista di cheat: attiva e disattiva i codici, aggiungi o modifica, poi avvia il gioco con quelli pronti.',
   'Кнопка [SELECT] в окне карточки |Информация об игре| открывает список чит-кодов этой игры: включай и выключай коды, добавляй или меняй их и запускай игру уже с ними.',
   '[SELECT] op de spelinfo opent de cheatlijst van het spel: zet codes aan en uit, voeg toe of wijzig, en start het spel er direct mee.'),
  # 41 the 2.18 section: what is new in this release
  ('Version 2.18 is out. The next card shows what is new.',
   'A versão 2.18 chegou. O próximo card mostra o que há de novo.',
   'Ya está aquí la versión 2.18. La siguiente pantalla muestra las novedades.',
   'Version 2.18 ist da. Die nächste Karte zeigt, was neu ist.',
   'La version 2.18 est là. Le prochain écran montre les nouveautés.',
   'La versione 2.18 è arrivata. La prossima scheda mostra le novità.',
   'Вышла версия 2.18. На следующей странице показано, что нового.',
   'Versie 2.18 is er. De volgende kaart toont wat er nieuw is.'),
  # 42 the cartridge with an RP2040 chip (2.18)
  ("Xeno Crisis, the 2019 cartridge with an RP2040 chip, now runs. It needs the dump of your cartridge's RP2040 flash, saved as |/sd2snes/xenocrisis_rp2040.bin| on the card. Music comes from an |MSU-1| pack next to the ROM. Without one, the game plays with sound effects only.",
   'Xeno Crisis, o cartucho de 2019 com um chip RP2040, agora roda. Ele precisa do dump da flash RP2040 do seu cartucho, gravado como |/sd2snes/xenocrisis_rp2040.bin| no cartão. A música vem de um pack |MSU-1| ao lado da ROM. Sem ele, o jogo toca só com efeitos sonoros.',
   'Xeno Crisis, el cartucho de 2019 con un chip RP2040, ya funciona. Necesita el volcado de la flash RP2040 de tu cartucho, guardado como |/sd2snes/xenocrisis_rp2040.bin| en la tarjeta. La música viene de un pack |MSU-1| junto a la ROM. Sin él, el juego suena solo con efectos de sonido.',
   'Xeno Crisis, das Modul von 2019 mit RP2040-Chip, läuft jetzt. Es braucht den Dump des RP2040-Flashs deines Moduls, gespeichert als |/sd2snes/xenocrisis_rp2040.bin| auf der Karte. Die Musik kommt aus einem |MSU-1|-Pack neben der ROM. Ohne Pack hat das Spiel nur Soundeffekte.',
   "Xeno Crisis, la cartouche de 2019 avec une puce RP2040, tourne désormais. Elle demande le dump de la flash RP2040 de ta cartouche, enregistré sous |/sd2snes/xenocrisis_rp2040.bin| sur la carte. La musique vient d'un pack |MSU-1| à côté de la ROM. Sans pack, le jeu n'a que les effets sonores.",
   'Xeno Crisis, la cartuccia del 2019 con un chip RP2040, ora funziona. Serve il dump della flash RP2040 della tua cartuccia, salvato come |/sd2snes/xenocrisis_rp2040.bin| sulla scheda. La musica arriva da un pack |MSU-1| accanto alla ROM. Senza pack, il gioco ha solo gli effetti sonori.',
   'Теперь запускается Xeno Crisis - картридж 2019 года с чипом RP2040. Нужен дамп флеш-памяти RP2040 твоего картриджа, сохранённый как |xenocrisis_rp2040.bin| в папке |/sd2snes| на карте. Музыка берётся из |MSU-1| пака в папке рядом с ROM-файлом. Без него в игре будут только звуковые эффекты.',
   'Xeno Crisis, de cartridge uit 2019 met een RP2040-chip, draait nu. Hij heeft de RP2040-flashdump van je cartridge nodig, bewaard als |/sd2snes/xenocrisis_rp2040.bin| op de kaart. De muziek komt uit een |MSU-1|-pack naast de ROM. Zonder pack heeft het spel alleen geluidseffecten.'),
  # 43 the SNES Mouse in the menu (2.18); [ML] [MR] [MB] [MRH] [MLH] draw the mouse glyphs
  ('Plug a SNES Mouse into either port and the menu shows a pointer. A left click [ML] is [A], a right click [MR] is [B], both together [MB] are [X] and holding the right one [MRH] is [Y]. In the music players, holding the left one [MLH] plays the next track and [MRH] the previous one. Push the pointer against the top or bottom edge to scroll the list.',
   'Ligue um SNES Mouse em qualquer porta e o menu mostra um ponteiro. O clique esquerdo [ML] é [A], o direito [MR] é [B], os dois juntos [MB] são [X] e segurar o direito [MRH] é [Y]. Nos tocadores de música, segurar o esquerdo [MLH] passa para a próxima faixa e [MRH] volta para a anterior. Encoste o ponteiro na borda de cima ou de baixo para rolar a lista.',
   'Conecta un SNES Mouse en cualquier puerto y el menú muestra un puntero. El clic izquierdo [ML] es [A], el derecho [MR] es [B], los dos juntos [MB] son [X] y mantener el derecho [MRH] es [Y]. En los reproductores de música, mantener el izquierdo [MLH] pasa a la pista siguiente y [MRH] vuelve a la anterior. Lleva el puntero al borde de arriba o de abajo para desplazar la lista.',
   'Steck eine SNES Mouse in einen der beiden Ports, und das Menü zeigt einen Zeiger. Linksklick [ML] ist [A], Rechtsklick [MR] ist [B], beide zusammen [MB] sind [X], rechts gedrückt halten [MRH] ist [Y]. In den Musik-Playern springt links gedrückt halten [MLH] zum nächsten Titel und [MRH] zum vorherigen. Schieb den Zeiger an den oberen oder unteren Rand, um die Liste zu blättern.',
   "Branche une SNES Mouse sur l'un des ports et le menu affiche un pointeur. Le clic gauche [ML] fait [A], le clic droit [MR] fait [B], les deux ensemble [MB] font [X] et le droit maintenu [MRH] fait [Y]. Dans les lecteurs de musique, le gauche maintenu [MLH] passe à la piste suivante et [MRH] revient à la précédente. Pousse le pointeur contre le bord haut ou bas pour faire défiler la liste.",
   'Collega un SNES Mouse a una delle due porte e il menu mostra un puntatore. Il clic sinistro [ML] è [A], il destro [MR] è [B], i due insieme [MB] sono [X] e tenere premuto il destro [MRH] è [Y]. Nei lettori musicali, tenere premuto il sinistro [MLH] passa alla traccia successiva e [MRH] torna alla precedente. Spingi il puntatore contro il bordo in alto o in basso per scorrere la lista.',
   'Подключи мышь для SNES к любому порту, и в меню появится курсор. Левая кнопка [ML] работает как [A], правая [MR] - как [B], обе [MB] - как [X], а удержание правой [MRH] - как [Y]. В проигрывателях удержание левой [MLH] включает следующий трек, а удержание правой [MRH] - предыдущий. Сдвинь курсор до упора в верхний или нижний край, чтобы прокрутить список.',
   'Sluit een SNES Mouse aan op een van beide poorten en het menu toont een aanwijzer. Linksklikken [ML] is [A], rechtsklikken [MR] is [B], beide samen [MB] is [X] en rechts ingedrukt houden [MRH] is [Y]. In de muziekspelers gaat links ingedrukt houden [MLH] naar het volgende nummer en [MRH] naar het vorige. Duw de aanwijzer tegen de boven- of onderrand om door de lijst te scrollen.'),
  # 44 the music players: |.pcm| (seek, playlist) and |.spc| (2.18)
  ("A |.pcm| or |.spc| in the list opens its player, and the folder is a playlist: the next track plays when one ends, [L]/[R] skip tracks. On a |.pcm|, [Left]/[Right] jump 5 s (hold for more) and a click on the bar seeks. An |.spc| shows its length, then moves on.",
   'Um |.pcm| ou |.spc| na lista abre o tocador, e a pasta vira playlist: quando uma faixa acaba toca a próxima, [L]/[R] trocam de faixa. No |.pcm|, [Esquerda]/[Direita] pulam 5 s (segure para mais) e um clique na barra pula para lá. O |.spc| mostra a duração e depois segue.',
   'Un |.pcm| o |.spc| de la lista abre su reproductor y la carpeta es una lista: al acabar una pista suena la siguiente, [L]/[R] cambian de pista. En un |.pcm|, [Izquierda]/[Derecha] saltan 5 s (mantén para más) y un clic en la barra salta ahí. Un |.spc| muestra su duración y luego sigue.',
   'Eine |.pcm| oder |.spc| in der Liste öffnet ihren Player, der Ordner wird zur Playlist: nach einem Titel kommt der nächste, [L]/[R] wechseln. Bei |.pcm| springen [Links]/[Rechts] 5 s (halten für mehr), ein Klick auf den Balken springt dorthin. Eine |.spc| zeigt ihre Länge und geht dann weiter.',
   "Un |.pcm| ou |.spc| de la liste ouvre son lecteur, le dossier devient une playlist : la piste suivante démarre à la fin, [L]/[R] changent de piste. Sur un |.pcm|, [Gauche]/[Droite] sautent de 5 s (maintenir pour plus) et un clic sur la barre y va. Un |.spc| affiche sa durée puis enchaîne.",
   'Un |.pcm| o |.spc| nella lista apre il suo lettore e la cartella diventa una playlist: finita una traccia parte la successiva, [L]/[R] cambiano traccia. In un |.pcm|, [Sinistra]/[Destra] saltano 5 s (tieni premuto per di più) e un clic sulla barra va lì. Un |.spc| mostra la durata e poi prosegue.',
   'При выборе файла |.pcm| или |.spc| в списке открывается соответствующий проигрыватель, а папка превращается в плейлист: по завершении одного трека автоматически начинается следующий, переключаться между ними можно кнопками [L]/[R]. В случае с |.pcm| кнопки [Влево]/[Вправо] позволяют переместиться на 5 секунд (при удержании кнопки - на большее время), а щелчок по полосе прогресса - перейти в указанное место. Для файла |.spc| отображается его длительность, после чего воспроизведение продолжается дальше.',
   'Een |.pcm| of |.spc| in de lijst opent de speler, de map wordt een afspeellijst: na een nummer start het volgende, [L]/[R] wisselen. Bij een |.pcm| springen [Links]/[Rechts] 5 s (vasthouden voor meer) en een klik op de balk springt daarheen. Een |.spc| toont zijn lengte en gaat dan door.'),
  # 45 the NES music (.nsf) player (2.18)
  ("An |.nsf| in the list plays NES music on the console itself, all five channels of its sound chip. [Left]/[Right] change the song, [A] pauses, [L]/[R] change the file.",
   'Um |.nsf| na lista toca a música do NES no próprio console, nos cinco canais do chip de som. [Esquerda]/[Direita] trocam a música, [A] pausa, [L]/[R] trocam o arquivo.',
   'Un |.nsf| de la lista toca la música de NES en la propia consola, los cinco canales de su chip de sonido. [Izquierda]/[Derecha] cambian la canción, [A] pausa, [L]/[R] el archivo.',
   'Eine |.nsf| in der Liste spielt NES-Musik auf der Konsole selbst, alle fünf Kanäle des Soundchips. [Links]/[Rechts] wechseln das Lied, [A] pausiert, [L]/[R] die Datei.',
   "Un |.nsf| de la liste joue la musique NES sur la console elle-même, les cinq canaux de sa puce son. [Gauche]/[Droite] changent le morceau, [A] met en pause, [L]/[R] le fichier.",
   'Un |.nsf| nella lista suona la musica del NES sulla console stessa, tutti e cinque i canali del chip audio. [Sinistra]/[Destra] cambiano brano, [A] mette in pausa, [L]/[R] il file.',
   'Файл |.nsf| из списка позволяет воспроизводить музыку NES на реальной игровой консоли, задействуя пять каналов её звукового чипа. Кнопки [Влево]/[Вправо] - переключают композиции, кнопка [A] - ставит воспроизведение на паузу, а кнопки [L]/[R] - позволяют переходить между файлами.',
   'Een |.nsf| in de lijst speelt NES-muziek op de console zelf, alle vijf kanalen van de geluidschip. [Links]/[Rechts] wisselen het nummer, [A] pauzeert, [L]/[R] het bestand.'),
  # 46 the saves screen (2.18)
  ('On a game, [Y] opens the context menu: {savesrow} there (or [R] on the info card) lists each save and savestate slot with its date. Pick the slot the game starts with, duplicate it or delete it.',
   'Num jogo, [Y] abre o menu de contexto: {savesrow} nele (ou [R] na ficha) lista cada slot de save e de savestate com a data. Escolha o slot com que o jogo inicia, duplique ou apague.',
   'En un juego, [Y] abre el menú contextual: {savesrow} (o [R] en la ficha) muestra cada ranura de partida y de savestate con su fecha. Elige con cuál empieza el juego, duplícala o bórrala.',
   'Auf einem Spiel öffnet [Y] das Kontextmenü: {savesrow} dort (oder [R] auf der Infokarte) zeigt jeden Spielstand- und Savestate-Slot mit Datum. Wähle den Startslot, dupliziere oder lösche ihn.',
   'Sur un jeu, [Y] ouvre le menu contextuel : {savesrow} (ou [R] sur la fiche) liste chaque emplacement de sauvegarde et de savestate avec sa date. Choisissez celui de démarrage, dupliquez-le ou supprimez-le.',
   'Su un gioco, [Y] apre il menu contestuale: {savesrow} (o [R] sulla scheda) elenca ogni slot di salvataggio e savestate con la data. Scegli lo slot di avvio, duplicalo o eliminalo.',
   'Кнопка [Y] на игре открывает контекстное меню: пункт {savesrow} (или кнопка [R] в карточке) показывает каждый слот сохранений и сейвстейтов с датой. Выберите слот для запуска, скопируйте или удалите его.',
   'Op een spel opent [Y] het contextmenu: {savesrow} daarin (of [R] op de spelinfo) toont elk save- en savestate-slot met de datum. Kies het startslot, dupliceer of verwijder het.'),
  # 47 the file manager (2.18)
  ('In the file list, [Y] on any entry opens the context menu with |Copy|, |Cut|, |Paste here|, |Rename| and |New folder|. |Select several| marks entries with [A]. A renamed game keeps its saves, cheats and covers, and [A] on a |.txt| opens it.',
   'Na lista de arquivos, [Y] em qualquer item abre o menu de contexto com |Copiar|, |Recortar|, |Colar aqui|, |Renomear| e |Nova pasta|. |Selecionar vários| marca itens com [A]. Um jogo renomeado leva junto saves, cheats e capas, e [A] num |.txt| abre o texto.',
   'En la lista, [Y] en cualquier entrada abre el menú contextual con |Copiar|, |Cortar|, |Pegar aquí|, |Renombrar| y |Nueva carpeta|. |Seleccionar varios| marca entradas con [A]. Un juego renombrado conserva sus partidas, trucos y portadas, y [A] en un |.txt| lo abre.',
   'In der Dateiliste öffnet [Y] auf einem Eintrag das Kontextmenü mit |Kopieren|, |Ausschneiden|, |Hier einfügen|, |Umbenennen| und |Neuer Ordner|. |Mehrere auswählen| markiert Einträge mit [A]. Ein umbenanntes Spiel behält Spielstände, Cheats und Cover, und [A] auf einer |.txt| öffnet sie.',
   "Dans la liste, [Y] sur une entrée ouvre le menu contextuel avec |Copier|, |Couper|, |Coller ici|, |Renommer| et |Nouveau dossier|. |Sélection multiple| marque des entrées avec [A]. Un jeu renommé garde ses sauvegardes, cheats et jaquettes, et [A] sur un |.txt| l'ouvre.",
   'Nella lista, [Y] su una voce apre il menu contestuale con |Copia|, |Taglia|, |Incolla qui|, |Rinomina| e |Nuova cartella|. |Selezione multipla| segna le voci con [A]. Un gioco rinominato tiene salvataggi, cheat e copertine, e [A] su un |.txt| lo apre.',
   'Нажатие кнопки [Y] на любом элементе списка файлов открывает контекстное меню: |Удалить|, |Копировать|, |Вырезать|, |Вставить сюда|, |Переименовать| и |Новая папка|. Функция |Выбрать несколько| позволяет пометить элементы с помощью кнопки [A]. При переименовании игры также автоматически переименовываются связанные с ней файлы сохранений, чит-коды и обложки, а нажатие кнопки [A] на файле с расширением |.txt| открывает его содержимое.',
   'In de lijst opent [Y] op een item het contextmenu met |Kopiëren|, |Knippen|, |Hier plakken|, |Hernoemen| en |Nieuwe map|. |Meerdere selecteren| markeert items met [A]. Een hernoemd spel houdt zijn saves, cheats en hoesjes, en [A] op een |.txt| opent het.'),
  # 48 delete a folder and all it holds, and the marked entries in the selection mode (2.18)
  ('[Y] on a folder offers {rmdir}: it counts files, folders and size, asks ({no} comes selected) and removes it all with a progress bar ([B] cancels). In {fmsel}, {del} does the same for the marked entries. Saves and cheats the |sd2snes| folder keeps stay, system folders are refused.',
   '[Y] numa pasta oferece {rmdir}: conta arquivos, pastas e tamanho, pergunta ({no} já marcado) e apaga tudo com barra de progresso ([B] cancela). Em {fmsel}, {del} faz o mesmo com os itens marcados. Saves e cheats da pasta |sd2snes| ficam, e as pastas do sistema são recusadas.',
   '[Y] en una carpeta ofrece {rmdir}: cuenta archivos, carpetas y tamaño, pregunta ({no} ya elegido) y la borra con barra de progreso ([B] cancela). En {fmsel}, {del} hace lo mismo con lo marcado. Partidas y trucos de la carpeta |sd2snes| se quedan, las carpetas del sistema se rechazan.',
   '[Y] auf einem Ordner bietet {rmdir}: zählt Dateien, Ordner und Größe, fragt ({no} vorgewählt) und löscht alles mit Fortschrittsbalken ([B] bricht ab). In {fmsel} macht {del} das mit den markierten Einträgen. Spielstände und Cheats im Ordner |sd2snes| bleiben, Systemordner sind gesperrt.',
   "[Y] sur un dossier propose {rmdir} : il compte fichiers, dossiers et taille, demande ({no} présélectionné) et efface tout avec une barre ([B] annule). En {fmsel}, {del} fait de même avec les entrées marquées. Sauvegardes et cheats de |sd2snes| restent, dossiers système refusés.",
   '[Y] su una cartella offre {rmdir}: conta file, cartelle e dimensione, chiede ({no} già scelto) e cancella tutto con una barra ([B] annulla). In {fmsel}, {del} fa lo stesso con le voci segnate. Salvataggi e cheat di |sd2snes| restano, cartelle di sistema rifiutate.',
   'При нажатии кнопки [Y] на папке доступна функция {rmdir}: она подсчитывает количество файлов и папок, а также их общий размер, запрашивает подтверждение (по умолчанию {no}) и удаляет всё содержимое, отображая индикатор прогресса ([B] - отмена). В режиме {fmsel} команда {del} выполняет аналогичное действие для отмеченных элементов. Сохранения и файлы чит-кодов в папке |/sd2snes| остаются нетронутыми, а удаление системных папок заблокировано.',
   '[Y] op een map biedt {rmdir}: telt bestanden, mappen en grootte, vraagt het ({no} voorgeselecteerd) en wist alles met een balk ([B] annuleert). In {fmsel} doet {del} hetzelfde met de gemarkeerde items. Saves en cheats in |sd2snes| blijven, systeemmappen geweigerd.'),
  # 49 game options (2.18): Settings' own options, per game, only those the game uses
  ("{goptrow} keeps settings for one game only, laid out like {cfg} with just what that game uses. {gdefault} follows the global option. Open it with [Y] on a game, [L] on the info card or [Y] on the in-game tab bar, where what cannot change mid-game is dimmed.",
   '{goptrow} guarda ajustes só para um jogo, organizados como em {cfg} e só com o que vale para ele. {gdefault} segue a opção global. Abra com [Y] num jogo, [L] na ficha ou [Y] na barra de abas do menu in-game, onde o que não muda com o jogo rodando fica apagado.',
   '{goptrow} guarda ajustes solo para un juego, como en {cfg} y solo con lo que ese juego usa. {gdefault} sigue la opción global. Ábrelo con [Y] en un juego, [L] en la ficha o [Y] en la barra de pestañas del juego, donde lo que no cambia jugando sale atenuado.',
   '{goptrow} speichert Einstellungen für ein Spiel, aufgebaut wie {cfg}, nur mit dem, was das Spiel nutzt. {gdefault} folgt der globalen Option. Öffnen mit [Y] auf einem Spiel, [L] auf der Infokarte oder [Y] auf der Ingame-Tab-Leiste, dort ist im Spiel Gesperrtes grau.',
   "{goptrow} garde des réglages pour un seul jeu, rangés comme {cfg}, avec seulement ce que le jeu utilise. {gdefault} suit l'option globale. Ouvrez avec [Y] sur un jeu, [L] sur la fiche ou [Y] sur les onglets du menu en jeu, où ce qui est bloqué en jeu est grisé.",
   "{goptrow} salva impostazioni per un solo gioco, come in {cfg}, solo con ciò che il gioco usa. {gdefault} segue l'opzione globale. Apri con [Y] su un gioco, [L] sulla scheda o [Y] sulle schede del menu in gioco, dove ciò che è bloccato in gioco è grigio.",
   'Пункт {goptrow} содержит настройки, относящиеся к конкретной игре. Они организованы так же, как в {cfg}, и включают только те параметры, которые применимы к ней. {gdefault} соответствует общим настройкам. Открыть этот раздел можно нажатием кнопки [Y] на выбранной игре, кнопка [L] - в карточке игры или кнопка [Y] - на панели вкладок |Меню в игре|, при этом заблокированные для игры параметры отображаются серым цветом.',
   '{goptrow} bewaart instellingen voor één spel, ingedeeld zoals {cfg}, met alleen wat dat spel gebruikt. {gdefault} volgt de algemene optie. Open met [Y] op een spel, [L] op de spelinfo of [Y] op de tabbalk van het in-game menu, waar wat vastligt grijs is.'),
  # 50 Repeat in the .spc / .pcm / .nsf players (2.18)
  ('In the |.spc|, |.pcm| and |.nsf| players, [X] turns {repeat} on: when the track ends it starts again instead of moving to the next one. An |.nsf| song ends after 3 s of silence. {repeat} starts off and the player remembers it.',
   'Nos tocadores de |.spc|, |.pcm| e |.nsf|, [X] liga {repeat}: quando a faixa acaba, ela recomeça em vez de ir para a próxima. Uma música de |.nsf| acaba depois de 3 s de silêncio. {repeat} começa desligado e o tocador lembra a escolha.',
   'En los reproductores de |.spc|, |.pcm| y |.nsf|, [X] activa {repeat}: al terminar, la pista vuelve a empezar en vez de pasar a la siguiente. Una canción de |.nsf| termina tras 3 s de silencio. {repeat} empieza desactivado y el reproductor lo recuerda.',
   'In den Playern für |.spc|, |.pcm| und |.nsf| schaltet [X] {repeat} ein: am Ende beginnt der Titel von vorn, statt zum nächsten zu gehen. Ein |.nsf|-Lied endet nach 3 s Stille. {repeat} ist anfangs aus, der Player merkt es sich.',
   "Dans les lecteurs |.spc|, |.pcm| et |.nsf|, [X] active {repeat} : à la fin, la piste recommence au lieu de passer à la suivante. Un morceau |.nsf| se termine après 3 s de silence. {repeat} est désactivé au départ et le lecteur s'en souvient.",
   'Nei lettori di |.spc|, |.pcm| e |.nsf|, [X] attiva {repeat}: alla fine la traccia ricomincia invece di passare alla successiva. Un brano |.nsf| finisce dopo 3 s di silenzio. {repeat} parte disattivato e il lettore lo ricorda.',
   'В плеерах |.spc|, |.pcm| и |.nsf| кнопка [X] включает {repeat}: в конце трек начинается заново, а не переходит к следующему. Мелодия |.nsf| заканчивается после 3 секунд тишины. {repeat} изначально выключен, плеер запоминает выбор.',
   'In de spelers voor |.spc|, |.pcm| en |.nsf| zet [X] {repeat} aan: aan het eind begint het nummer opnieuw in plaats van het volgende. Een |.nsf|-nummer eindigt na 3 s stilte. {repeat} staat eerst uit en de speler onthoudt het.'),
  # 51 stop the menu music on the game info card (2.18; needs the menu music). The user asked for
  # the cost to be plain: opening the card takes about a second longer, and both ways fade
  ('{yes}: the menu music stops while a game info card is open and starts over when you leave. It takes an internal console reset: opening the card gets about 1 s slower, and both ways the screen fades out and back. The clip and the menu sounds still play.',
   '{yes}: a música do menu para enquanto a ficha de um jogo está aberta e recomeça quando você sai. Isso exige reiniciar o console por dentro: abrir a ficha fica cerca de 1 s mais lento, e na ida e na volta a tela escurece e volta. O vídeo e os sons do menu continuam.',
   '{yes}: la música del menú se detiene mientras la ficha de un juego está abierta y vuelve a empezar al salir. Requiere reiniciar la consola por dentro: abrir la ficha tarda cerca de 1 s más, y al entrar y al salir la pantalla se oscurece y vuelve. El vídeo y los sonidos siguen.',
   '{yes}: Die Menümusik stoppt, solange eine Infokarte offen ist, und beginnt beim Verlassen von vorn. Dafür braucht es einen internen Konsolen-Reset: Das Öffnen dauert etwa 1 s länger, und hin wie zurück blendet das Bild aus und wieder ein. Video und Menü-Sounds bleiben.',
   "{yes} : la musique du menu s'arrête tant que la fiche d'un jeu est ouverte et reprend du début en sortant. Il faut un reset interne de la console : ouvrir la fiche prend environ 1 s de plus, et à l'aller comme au retour l'écran s'éteint puis revient. La vidéo et les sons restent.",
   "{yes}: la musica del menu si ferma mentre la scheda di un gioco è aperta e riparte quando esci. Serve un reset interno della console: aprire la scheda richiede circa 1 s in più, e all'andata e al ritorno lo schermo si oscura e torna. Il video e i suoni restano.",
   '{yes} - музыка в меню прекращается, когда открыта карточка с информацией об игре, и начинает звучать заново, когда вы её закрываете. Это требует внутреннего перезапуска консоли: открытие карточки происходит примерно на 1 секунду медленнее, а при её открытии и закрытии экран плавно гаснет и снова загорается. Видеоролик и звуки меню продолжают воспроизводиться.',
   "{yes}: de menumuziek stopt zolang de spelinfo open is en begint opnieuw als je teruggaat. Daarvoor is een interne reset van de console nodig: de spelinfo openen duurt zo'n 1 s langer, en heen en terug dooft het beeld uit en weer in. Video en menugeluiden blijven."),
  # 52 the ROM's CRC32 on the game info card (2.18)
  ("The game info card shows the ROM's CRC32 right of the title, the number the catalogs use: no 512-byte copier header and, for NES, no iNES header. A patched entry shows its base ROM. It is computed in the background and shows up in seconds. A long title scrolls.",
   'A ficha do jogo mostra o CRC32 da ROM à direita do título, o número que os catálogos usam: sem o cabeçalho de 512 bytes de copiadora e, no NES, sem o iNES. Uma entrada com patch mostra a ROM base. Ele é calculado em segundo plano e aparece em segundos. Um título longo rola.',
   'La ficha del juego muestra el CRC32 de la ROM a la derecha del título, el número de los catálogos: sin la cabecera de 512 bytes de copiadora y, en NES, sin la iNES. Una entrada con parche muestra la ROM base. Se calcula en segundo plano y aparece en segundos. Un título largo se desplaza.',
   'Die Infokarte zeigt rechts vom Titel die CRC32 der ROM, die Zahl der Kataloge: ohne 512-Byte-Kopiererheader und beim NES ohne iNES-Header. Ein Eintrag mit Patch zeigt die Basis-ROM. Sie wird im Hintergrund berechnet und erscheint nach Sekunden. Ein langer Titel läuft durch.',
   "La fiche du jeu affiche le CRC32 de la ROM à droite du titre, le nombre des catalogues : sans l'en-tête de copieur de 512 octets et, pour la NES, sans l'iNES. Une entrée patchée montre sa ROM de base. Il est calculé en arrière-plan et arrive en quelques secondes. Un titre long défile.",
   "La scheda del gioco mostra il CRC32 della ROM a destra del titolo, il numero dei cataloghi: senza l'header di 512 byte del copiatore e, per il NES, senza l'iNES. Una voce con patch mostra la ROM di base. Viene calcolato in background e appare in pochi secondi. Un titolo lungo scorre.",
   'В карточке игры справа от названия отображается контрольная сумма CRC32 (используемая в каталогах), которая рассчитывается без учета 512-байтового заголовка, добавляемого устройствами для копирования (или заголовка iNES - в случае с консолью NES). Для модифицированных (патченных) версий игр указывается CRC32 исходного ROM-файла. Это значение вычисляется в фоновом режиме и появляется через несколько секунд. Длинные названия отображаются в виде бегущей строки.',
   'De spelinfo toont rechts van de titel de CRC32 van de ROM, het getal van de catalogi: zonder 512-byte copier-header en bij NES zonder iNES-header. Een item met patch toont de basis-ROM. Hij wordt op de achtergrond berekend en verschijnt binnen enkele seconden. Een lange titel scrolt.'),
  # 53 rotated Game Boy buttons, on the SGB and the GBC core (2.18)
  ("{yes}: SNES [B] plays the Game Boy's A and [Y] its B, the two side by side as on the Game Boy. [A] still plays A. Works on the Super Game Boy and on the Game Boy Color core. It lives in {cfg} > {sgbmenu} and can be set per game too.",
   '{yes}: o [B] do SNES faz o A do Game Boy e o [Y] faz o B, os dois lado a lado como no Game Boy. O [A] continua sendo A. Vale no Super Game Boy e no núcleo Game Boy Color. Fica em {cfg} > {sgbmenu} e também pode ser ajustado por jogo.',
   '{yes}: el [B] del SNES hace de A del Game Boy y el [Y] de B, uno al lado del otro como en el Game Boy. [A] sigue siendo A. Funciona en el Super Game Boy y en el núcleo Game Boy Color. Está en {cfg} > {sgbmenu} y también se ajusta por juego.',
   '{yes}: SNES-[B] ist das A des Game Boy und [Y] sein B, nebeneinander wie am Game Boy. [A] bleibt A. Gilt für den Super Game Boy und den Game-Boy-Color-Kern. Zu finden unter {cfg} > {sgbmenu}, auch pro Spiel einstellbar.',
   '{yes} : le [B] du SNES fait le A du Game Boy et le [Y] son B, côte à côte comme sur le Game Boy. [A] reste A. Vaut pour le Super Game Boy et le noyau Game Boy Color. Dans {cfg} > {sgbmenu}, aussi réglable par jeu.',
   '{yes}: il [B] del SNES fa la A del Game Boy e la [Y] la B, affiancati come sul Game Boy. [A] resta A. Vale per il Super Game Boy e il core Game Boy Color. Si trova in {cfg} > {sgbmenu} e si può impostare anche per gioco.',
   '{yes} - На SNES кнопка [B] соответствует кнопке [A] на Game Boy, а кнопка [Y] - кнопке [B], они расположены рядом, как и на самом Game Boy. Кнопка [A] по-прежнему соответствует кнопке [A]. Функция работает в Super Game Boy и в ядре Game Boy Color. Настройка находится в разделе {cfg} > {sgbmenu}, её также можно задать отдельно для каждой игры.',
   '{yes}: SNES-[B] is de A van de Game Boy en [Y] de B, naast elkaar zoals op de Game Boy. [A] blijft A. Werkt op de Super Game Boy en op de Game Boy Color-kern. Te vinden in {cfg} > {sgbmenu}, ook per spel in te stellen.'),
  # 54 the NTT Data Keypad (NDK10) as a standard pad (2.18)
  ('The |NTT Data Keypad| (NDK10) works as a standard controller in the menu, the in-game menu, the guide viewer and the in-game shortcuts. Its number pad is left to the games that use it.',
   'O |NTT Data Keypad| (NDK10) funciona como um controle comum no menu, no menu in-game, no visualizador de guias e nos atalhos dos jogos. O teclado numérico fica para os jogos que o usam.',
   'El |NTT Data Keypad| (NDK10) funciona como un mando normal en el menú, el menú del juego, el visor de guías y los atajos en el juego. Su teclado numérico queda para los juegos que lo usan.',
   'Das |NTT Data Keypad| (NDK10) arbeitet wie ein normaler Controller im Menü, im Ingame-Menü, in der Anleitungsansicht und bei den Ingame-Kürzeln. Sein Ziffernblock bleibt den Spielen, die ihn nutzen.',
   "Le |NTT Data Keypad| (NDK10) marche comme une manette normale dans le menu, le menu en jeu, le lecteur de guides et les raccourcis en jeu. Son pavé numérique reste aux jeux qui l'utilisent.",
   "L'|NTT Data Keypad| (NDK10) funziona come un controller normale nel menu, nel menu in gioco, nel visore delle guide e nelle scorciatoie in gioco. Il tastierino numerico resta ai giochi che lo usano.",
   '|NTT Data Keypad| (NDK10) работает как обычный контроллер в |Меню|, в |Меню в игре|, в просмотре |РУКОВОДСТВО К ИГРЕ| и в |КОМБИНАЦИИ В ИГРЕ|. Цифровой блок остаётся играм, которые его используют.',
   'Het |NTT Data Keypad| (NDK10) werkt als een gewone controller in het menu, het in-game menu, de handleidingviewer en de in-game sneltoetsen. Het cijferblok blijft voor de spellen die het gebruiken.'),
  # 55 the parental lock (2.18)
  ('{lockrow} in {cfg} asks for a code of 4 to 10 buttons (wait 3 s to end one shorter than 10) before anything that changes the card or the settings: {cfg}, deleting, renaming and moving files, saves, {goptrow}, editing cheats, autoboot and themes, in the in-game menu too. Holding [B] for 3 s cancels. Forgot the code? Delete LockCode in |/sd2snes/config.yml|.',
   '{lockrow}, em {cfg}, pede um código de 4 a 10 botões (espere 3 s para terminar antes dos 10) antes do que muda o cartão ou os ajustes: {cfg}, apagar, renomear e mover arquivos, saves, {goptrow}, editar cheats, autoboot e temas, também no menu in-game. Segurar [B] por 3 s cancela. Esqueceu o código? Apague LockCode no |/sd2snes/config.yml|.',
   '{lockrow}, en {cfg}, pide un código de 4 a 10 botones (espera 3 s para acabar antes de 10) antes de lo que cambia la tarjeta o los ajustes: {cfg}, borrar, renombrar y mover archivos, partidas, {goptrow}, editar trucos, autoarranque y temas, también en el menú del juego. Mantener [B] 3 s cancela. Si olvidas el código, borra LockCode en |/sd2snes/config.yml|.',
   '{lockrow} in {cfg} fragt vor allem, was die Karte oder die Einstellungen ändert, einen Code aus 4 bis 10 Tasten ab (3 s warten beendet einen kürzeren): {cfg}, Dateien löschen, umbenennen und verschieben, Spielstände, {goptrow}, Cheats bearbeiten, Autoboot und Themes, auch im Ingame-Menü. [B] 3 s halten bricht ab. Code vergessen? LockCode in |/sd2snes/config.yml| löschen.',
   '{lockrow}, dans {cfg}, demande un code de 4 à 10 boutons (attendez 3 s pour finir avant 10) avant ce qui change la carte ou les réglages : {cfg}, effacer, renommer et déplacer des fichiers, sauvegardes, {goptrow}, éditer les cheats, autoboot et thèmes, aussi dans le menu en jeu. Maintenir [B] 3 s annule. Code oublié ? Effacez LockCode dans |/sd2snes/config.yml|.',
   '{lockrow}, in {cfg}, chiede un codice da 4 a 10 tasti (aspetta 3 s per finire prima di 10) prima di ciò che cambia la scheda o le impostazioni: {cfg}, cancellare, rinominare e spostare file, salvataggi, {goptrow}, modificare i cheat, autoboot e temi, anche nel menu in gioco. Tenere [B] 3 s annulla. Codice dimenticato? Cancella LockCode in |/sd2snes/config.yml|.',
   '{lockrow} в {cfg} запрашивает код, состоящий из комбинации 4-10 кнопок, перед любым действием, меняющим данные на карте памяти или настройки: {cfg}, удалением, переименованием и перемещением файлов, сохранениями, {goptrow}, правкой чит-кодов, автозапуском и темами, в том числе в |Меню в игре|. Если в коде менее 10 кнопок, подождите 3 секунды, чтобы закончить ввод раньше. Удержание кнопки [B] в течение 3 секунд - отмена. Забыли код? Удалите строку LockCode в файле |/sd2snes/config.yml|.',
   "{lockrow} in {cfg} vraagt een code van 4 tot 10 knoppen (wacht 3 s om eerder te stoppen) voor alles wat de kaart of de instellingen wijzigt: {cfg}, bestanden wissen, hernoemen en verplaatsen, saves, {goptrow}, cheats bewerken, autoboot en thema's, ook in het in-game menu. [B] 3 s vasthouden annuleert. Code vergeten? Wis LockCode in |/sd2snes/config.yml|."),
  # 56 the 2.19 section: what is new in this release
  ('Version 2.19 is out. The next cards show what is new.',
   'A versão 2.19 chegou. Os próximos cards mostram o que há de novo.',
   'Ya está aquí la versión 2.19. Las siguientes pantallas muestran las novedades.',
   'Version 2.19 ist da. Die nächsten Karten zeigen, was neu ist.',
   'La version 2.19 est là. Les prochains écrans montrent les nouveautés.',
   'La versione 2.19 è arrivata. Le prossime schede mostrano le novità.',
   'Вышла версия 2.19. На следующих страницах показано, что нового.',
   'Versie 2.19 is er. De volgende kaarten tonen wat er nieuw is.'),
  # 57 the animated menu background (2.19): the header stays still, a theme may bring its own
  # (made in the Theme Creator), a theme without one shows only its gradient, and the CJK
  # languages turn it off (their glyph cache uses the same video memory)
  ('The menu has an animated background that slides behind the list while the logo stays still. A |.thm| theme can bring its own, made in the |Theme Creator|. A theme without one shows only its gradient. Japanese and Chinese have no background: their text uses the same video memory.',
   'O menu tem um fundo animado, que desliza atrás da lista com o logo parado. Um tema |.thm| pode trazer o dele, criado no |Theme Creator|. Tema sem fundo mostra só o degradê. Em japonês e chinês não há fundo: o texto usa a mesma memória de vídeo.',
   'El menú tiene un fondo animado que se desliza tras la lista con el logo fijo. Un tema |.thm| puede traer el suyo, creado en el |Theme Creator|. Un tema sin fondo muestra solo su degradado. En japonés y chino no hay fondo: el texto usa la misma memoria de vídeo.',
   'Das Menü hat einen animierten Hintergrund, der hinter der Liste gleitet, das Logo steht still. Ein |.thm|-Theme kann seinen eigenen mitbringen, erstellt im |Theme Creator|. Ohne Hintergrund zeigt es nur seinen Verlauf. Japanisch und Chinesisch haben keinen: ihr Text nutzt denselben Videospeicher.',
   "Le menu a un fond animé qui glisse derrière la liste, le logo restant fixe. Un thème |.thm| peut apporter le sien, créé dans le |Theme Creator|. Un thème sans fond montre seulement son dégradé. Le japonais et le chinois n'ont pas de fond : leur texte utilise la même mémoire vidéo.",
   'Il menu ha uno sfondo animato che scorre dietro la lista, con il logo fermo. Un tema |.thm| può portare il suo, creato nel |Theme Creator|. Un tema senza sfondo mostra solo la sfumatura. Giapponese e cinese non hanno sfondo: il testo usa la stessa memoria video.',
   'В меню есть анимированный фон: он движется за списком, а логотип стоит на месте. Тема |.thm| может содержать свой фон, созданный в |Theme Creator|. Тема без фона показывает только градиент. В японском и китайском фона нет: их текст использует ту же видеопамять.',
   'Het menu heeft een bewegende achtergrond die achter de lijst schuift terwijl het logo stilstaat. Een |.thm|-thema kan een eigen meebrengen, gemaakt in de |Theme Creator|. Zonder achtergrond toont het alleen zijn verloop. Japans en Chinees hebben er geen: hun tekst gebruikt hetzelfde videogeheugen.'),
  # 58 a theme's font, music, effects and preview picture (2.19): picking it opens a window with
  # the picture and the Apply font / Apply sounds options; nothing is extracted, changing the
  # theme undoes it all, and the last choice wins (a .sfn picked later replaces the theme's font,
  # a .spc its music; the theme's effects stay)
  ("A |.thm| theme can also bring a font, music, effects and a preview. When you pick it, the preview appears with {thmfont} and {thmsound}. Nothing goes to the card, and changing the theme undoes it all. A |.sfn| or |.spc| picked later replaces the theme's font or music.",
   'Um tema |.thm| também pode trazer fonte, música, efeitos e uma prévia. Ao escolhê-lo, a prévia aparece com {thmfont} e {thmsound}. Nada vai para o cartão, e trocar de tema desfaz tudo. Um |.sfn| ou |.spc| escolhido depois substitui a fonte ou a música do tema.',
   'Un tema |.thm| también puede traer fuente, música, efectos y una vista previa. Al elegirlo, la vista previa sale con {thmfont} y {thmsound}. Nada va a la tarjeta, y otro tema lo deshace todo. Un |.sfn| o |.spc| elegido después sustituye la fuente o la música del tema.',
   'Ein |.thm|-Theme kann auch Schrift, Musik, Effekte und eine Vorschau mitbringen. Beim Wählen erscheint die Vorschau mit {thmfont} und {thmsound}. Nichts kommt auf die Karte, ein anderes Theme macht alles rückgängig. Eine später gewählte |.sfn| oder |.spc| ersetzt Schrift oder Musik des Themes.',
   "Un thème |.thm| peut aussi apporter police, musique, effets et un aperçu. Quand tu le choisis, l'aperçu s'affiche avec {thmfont} et {thmsound}. Rien ne va sur la carte, et un autre thème annule tout. Un |.sfn| ou |.spc| choisi ensuite remplace la police ou la musique du thème.",
   "Un tema |.thm| può portare anche font, musica, effetti e un'anteprima. Quando lo scegli, l'anteprima appare con {thmfont} e {thmsound}. Niente va sulla scheda, e un altro tema annulla tutto. Un |.sfn| o |.spc| scelto dopo sostituisce il font o la musica del tema.",
   'Тема |.thm| может содержать также шрифт, музыку, эффекты и превью. При выборе превью видно с {thmfont} и {thmsound}. На карту ничего не копируется, другая тема отменяет всё. Выбранные позже |.sfn| или |.spc| заменяют шрифт или музыку темы.',
   'Een |.thm|-thema kan ook een lettertype, muziek, effecten en een voorbeeld meebrengen. Bij het kiezen verschijnt het voorbeeld met {thmfont} en {thmsound}. Niets gaat naar de kaart, en een ander thema maakt alles ongedaan. Een later gekozen |.sfn| of |.spc| vervangt lettertype of muziek van het thema.'),
  # 59 font files (.sfn), picked in the browser like a theme (2.19)
  ("Change the menu's letters with a |.sfn| font file: pick it in the list, as with a theme, and only the letters change. Draw your own in the |Font Editor| on the site or take one from its font gallery. {restorefont} in {browser} brings back the built-in font.",
   'Troque as letras do menu com um arquivo de fonte |.sfn|: escolha na lista, como um tema, e só as letras mudam. Desenhe a sua no |Font Editor| do site ou pegue uma na galeria de fontes. {restorefont} em {browser} volta à fonte do próprio menu.',
   'Cambia las letras del menú con un archivo de fuente |.sfn|: elígelo en la lista, como un tema, y solo cambian las letras. Dibuja la tuya en el |Font Editor| del sitio o toma una de su galería de fuentes. {restorefont} en {browser} vuelve a la fuente del menú.',
   'Ändere die Buchstaben des Menüs mit einer Schriftdatei |.sfn|: in der Liste wählen wie ein Theme, nur die Buchstaben ändern sich. Zeichne deine eigene im |Font Editor| der Website oder nimm eine aus der Schriftgalerie. {restorefont} in {browser} holt die eingebaute Schrift zurück.',
   "Change les lettres du menu avec un fichier de police |.sfn| : choisis-le dans la liste, comme un thème, et seules les lettres changent. Dessine la tienne dans le |Font Editor| du site ou prends-en une dans sa galerie de polices. {restorefont} ({browser}) remet la police d'origine.",
   'Cambia le lettere del menu con un file di font |.sfn|: sceglilo nella lista, come un tema, e cambiano solo le lettere. Disegna il tuo nel |Font Editor| del sito o prendine uno dalla galleria dei font. {restorefont} in {browser} torna al font del menu.',
   'Меняйте буквы меню файлом шрифта |.sfn|: выберите его в списке, как тему, и изменятся только буквы. Нарисуйте свой в |Font Editor| на сайте или возьмите готовый из галереи шрифтов. {restorefont} в разделе {browser} вернёт встроенный шрифт.',
   'Verander de letters van het menu met een lettertypebestand |.sfn|: kies het in de lijst, zoals een thema, en alleen de letters veranderen. Teken je eigen in de |Font Editor| op de site of neem er een uit de lettertypegalerij. {restorefont} in {browser} zet het ingebouwde lettertype terug.'),
  # 60 the Atari 2600 on the Mk.II too, and the same pad keys on both boards (2.19)
  ('Atari 2600 games now run on the |sd2snes Mk.II| too, on a core of its own. And on both boards the pad works the same: [L] steps the difficulty (Easy, Normal, Hard), [R] pauses, [X] flips Color and B/W and [Y] changes the palette, each with its name on screen. They act when released and only alone, so the combos never change a setting.',
   'Os jogos de Atari 2600 agora rodam também no |sd2snes Mk.II|, num core próprio. E nas duas placas o controle funciona igual: [L] passa a dificuldade (Easy, Normal, Hard), [R] pausa, [X] alterna Cor e P/B e [Y] troca a paleta, cada um com o nome na tela. Eles agem ao soltar e só sozinhos, então os combos nunca mudam um ajuste.',
   'Los juegos de Atari 2600 ahora corren también en el |sd2snes Mk.II|, con un core propio. Y en las dos placas el mando funciona igual: [L] cambia la dificultad (Easy, Normal, Hard), [R] pausa, [X] cambia Color y B/N e [Y] la paleta, cada uno con su nombre en pantalla. Actúan al soltarlos y solo solos, así los combos nunca cambian un ajuste.',
   'Atari-2600-Spiele laufen jetzt auch auf dem |sd2snes Mk.II|, mit einem eigenen Core. Und auf beiden Boards funktioniert der Controller gleich: [L] wechselt die Schwierigkeit (Easy, Normal, Hard), [R] pausiert, [X] wechselt Farbe und S/W, [Y] die Palette, jeweils mit Namen auf dem Bild. Sie wirken beim Loslassen und nur allein, Kombos ändern also nie eine Einstellung.',
   "Les jeux Atari 2600 tournent maintenant aussi sur le |sd2snes Mk.II|, avec un core à lui. Et sur les deux cartes la manette marche pareil : [L] change la difficulté (Easy, Normal, Hard), [R] met en pause, [X] bascule Couleur et N/B, [Y] change la palette, chacun avec son nom à l'écran. Ils agissent au relâchement et seuls, donc les combos ne changent jamais un réglage.",
   "I giochi Atari 2600 ora girano anche sul |sd2snes Mk.II|, con un core tutto suo. E su entrambe le schede il controller funziona allo stesso modo: [L] cambia la difficoltà (Easy, Normal, Hard), [R] mette in pausa, [X] alterna Colore e B/N, [Y] cambia la tavolozza, ognuno con il nome sullo schermo. Agiscono al rilascio e solo da soli, quindi le combo non cambiano mai un'impostazione.",
   'Игры Atari 2600 теперь идут и на |sd2snes Mk.II|, на собственном ядре. А на обеих платах кнопки работают одинаково: [L] меняет сложность (Easy, Normal, Hard), [R] ставит паузу, [X] переключает цветной и чёрно-белый режим, [Y] меняет палитру, название каждой настройки видно на экране. Кнопки срабатывают при отпускании и только поодиночке, поэтому комбинации никогда не меняют настройки.',
   "Atari 2600-spellen draaien nu ook op de |sd2snes Mk.II|, met een eigen core. En op beide borden werkt de controller hetzelfde: [L] wisselt de moeilijkheid (Easy, Normal, Hard), [R] pauzeert, [X] wisselt Kleur en Z/W, [Y] het palet, elk met de naam op het scherm. Ze werken bij loslaten en alleen los, dus combo's veranderen nooit een instelling."),
  # 61 the PPU is cleared before every game boots
  ("Some romhacks draw their intro without clearing the video memory, and on a real console the menu's leftovers show as garbage. The |sd2snes+| wipes it before every game starts.",
   'Alguns romhacks desenham a abertura sem limpar a memória de vídeo, e num console de verdade as sobras do menu aparecem como lixo. O |sd2snes+| limpa tudo antes de qualquer jogo começar.',
   'Algunos romhacks dibujan su intro sin limpiar la memoria de vídeo, y en una consola real los restos del menú salen como basura. El |sd2snes+| la borra antes de que empiece cualquier juego.',
   'Manche Romhacks zeichnen ihr Intro, ohne den Videospeicher zu leeren, und auf echter Hardware erscheinen Menüreste als Müll. Der |sd2snes+| löscht ihn vor jedem Spielstart.',
   "Certains romhacks dessinent leur intro sans vider la mémoire vidéo, et sur une vraie console les restes du menu s'affichent en vrac. Le |sd2snes+| la vide avant le lancement de chaque jeu.",
   "Alcune romhack disegnano l'intro senza pulire la memoria video, e su una console vera gli avanzi del menu appaiono come spazzatura. L'|sd2snes+| la pulisce prima che parta qualsiasi gioco.",
   'Некоторые ромхаки рисуют заставку, не очищая видеопамять, и на настоящей консоли остатки меню видны как мусор. |sd2snes+| очищает её перед запуском любой игры.',
   'Sommige romhacks tekenen hun intro zonder het videogeheugen te wissen, en op een echte console verschijnen restjes van het menu als rommel. De |sd2snes+| wist het voordat elk spel start.'),
  # 62 bus timing compat
  ('A game that freezes or glitches right after the logo on some consoles, mostly 1-CHIP ones? Try {cfg} > {ingame} > {buscompat}: it frees the cartridge bus a little earlier, like firmware 1.11.0 did.',
   'Um jogo trava ou dá glitch logo depois do logo em alguns consoles, quase sempre 1-CHIP? Tente {cfg} > {ingame} > {buscompat}: ela libera o barramento do cartucho um pouco antes, como a firmware 1.11.0 fazia.',
   '¿Un juego se cuelga o falla justo tras el logo en algunas consolas, casi siempre 1-CHIP? Prueba {cfg} > {ingame} > {buscompat}: libera el bus del cartucho un poco antes, como hacía el firmware 1.11.0.',
   'Hängt oder flackert ein Spiel direkt nach dem Logo auf manchen Konsolen, meist 1-CHIP? Versuche {cfg} > {ingame} > {buscompat}: es gibt den Modulbus etwas früher frei, wie Firmware 1.11.0.',
   'Un jeu qui plante ou bugue juste après le logo sur certaines consoles, souvent des 1-CHIP ? Essaie {cfg} > {ingame} > {buscompat} : il libère le bus de la cartouche un peu plus tôt, comme le firmware 1.11.0.',
   "Un gioco si blocca o dà glitch subito dopo il logo su alcune console, quasi sempre 1-CHIP? Prova {cfg} > {ingame} > {buscompat}: libera il bus della cartuccia un po' prima, come il firmware 1.11.0.",
   'Игра зависает или работает со сбоями сразу после логотипа на некоторых консолях, чаще всего 1-CHIP? Попробуйте {cfg} > {ingame} > {buscompat}: шина картриджа освобождается немного раньше, как в прошивке 1.11.0.',
   'Loopt een spel vast of hapert het net na het logo op sommige consoles, meestal 1-CHIP? Probeer {cfg} > {ingame} > {buscompat}: de cartridgebus wordt iets eerder vrijgegeven, zoals firmware 1.11.0 deed.'),
  # 63 the hardware model in System Information
  ('{sysinfo} in the main menu ([X]) now also shows which hardware it runs on: |sd2snes Mk.II|, |sd2snes Mk.III| or |FXPAK PRO STM32|.',
   '{sysinfo} no menu principal ([X]) agora mostra também em que hardware ele roda: |sd2snes Mk.II|, |sd2snes Mk.III| ou |FXPAK PRO STM32|.',
   '{sysinfo} en el menú principal ([X]) ahora también muestra en qué hardware corre: |sd2snes Mk.II|, |sd2snes Mk.III| o |FXPAK PRO STM32|.',
   '{sysinfo} im Hauptmenü ([X]) zeigt jetzt auch, auf welcher Hardware es läuft: |sd2snes Mk.II|, |sd2snes Mk.III| oder |FXPAK PRO STM32|.',
   '{sysinfo} dans le menu principal ([X]) montre aussi sur quel matériel il tourne : |sd2snes Mk.II|, |sd2snes Mk.III| ou |FXPAK PRO STM32|.',
   '{sysinfo} nel menu principale ([X]) ora mostra anche su quale hardware gira: |sd2snes Mk.II|, |sd2snes Mk.III| o |FXPAK PRO STM32|.',
   '{sysinfo} в главном меню ([X]) теперь показывает и модель картриджа: |sd2snes Mk.II|, |sd2snes Mk.III| или |FXPAK PRO STM32|.',
   '{sysinfo} in het hoofdmenu ([X]) toont nu ook op welke hardware het draait: |sd2snes Mk.II|, |sd2snes Mk.III| of |FXPAK PRO STM32|.'),
  # 64 BS-X and the Memory Pack
  ('Satellaview games that use the 8M Memory Pack slot read it from |/sd2snes/saves| as |<rom>.mpk|: saved sound novels, downloaded races and the like. The game still boots without a pack.',
   'Jogos do Satellaview que usam o slot de Memory Pack de 8M leem o pack de |/sd2snes/saves| como |<rom>.mpk|: sound novels salvas, corridas baixadas e afins. O jogo boota mesmo sem o pack.',
   'Los juegos de Satellaview que usan el slot de Memory Pack de 8M lo leen de |/sd2snes/saves| como |<rom>.mpk|: sound novels guardadas, carreras descargadas y más. El juego arranca incluso sin pack.',
   'Satellaview-Spiele mit 8M-Memory-Pack-Slot lesen das Pack aus |/sd2snes/saves| als |<rom>.mpk|: gespeicherte Sound Novels, geladene Rennen und mehr. Das Spiel startet auch ohne Pack.',
   'Les jeux Satellaview qui utilisent le slot Memory Pack 8M le lisent dans |/sd2snes/saves| en |<rom>.mpk| : sound novels sauvegardés, courses téléchargées... Le jeu démarre même sans pack.',
   'I giochi Satellaview che usano lo slot Memory Pack da 8M lo leggono da |/sd2snes/saves| come |<rom>.mpk|: sound novel salvate, corse scaricate e simili. Il gioco parte anche senza pack.',
   'Игры Satellaview со слотом Memory Pack 8M читают его из |/sd2snes/saves| как |<rom>.mpk|: сохранённые звуковые новеллы, скачанные гонки и прочее. Игра запускается и без Memory Pack.',
   'Satellaview-spellen met een 8M Memory Pack-slot lezen het pack uit |/sd2snes/saves| als |<rom>.mpk|: opgeslagen sound novels, gedownloade races en meer. Het spel start ook zonder pack.'),
  # 65 delete files and saves
  ("From the [Y] menu of the file list, {del} erases the selected file and {savesrow} opens its saves and savestates, slot by slot. Deleting a ROM also takes its cover, info card, guides and cheats along.",
   'No menu do [Y] da lista de arquivos, {del} apaga o arquivo selecionado e {savesrow} abre os saves e savestates dele, slot a slot. Apagar uma ROM leva junto a capa, a ficha, as guias e os cheats dela.',
   'Desde el menú [Y] de la lista, {del} borra el archivo elegido y {savesrow} abre sus partidas y savestates, ranura por ranura. Borrar una ROM también se lleva su carátula, ficha, guías y cheats.',
   'Im [Y]-Menü der Dateiliste löscht {del} die gewählte Datei, {savesrow} öffnet ihre Spielstände und Savestates, Slot für Slot. Eine gelöschte ROM nimmt Cover, Infokarte, Anleitungen und Cheats mit.',
   'Depuis le menu [Y] de la liste, {del} efface le fichier et {savesrow} ouvre ses sauvegardes et savestates, emplacement par emplacement. Supprimer une ROM emporte aussi jaquette, fiche, guides et cheats.',
   'Dal menu [Y] della lista, {del} cancella il file scelto e {savesrow} apre i suoi salvataggi e savestate, slot per slot. Eliminare una ROM porta via anche copertina, scheda, guide e cheat.',
   'В меню кнопки [Y], доступном в списке файлов, пункт {del} удаляет выбранный файл, а {savesrow} открывает список его сохранений и сейвстейтов - по отдельным слотам. При удалении ROM-файла также удаляются сопутствующие ему обложка, карточка, руководства и чит-коды.',
   'Vanuit het [Y]-menu van de lijst wist {del} het gekozen bestand en opent {savesrow} de saves en savestates ervan, per slot. Een gewiste ROM neemt ook hoes, spelinfo, gidsen en cheats mee.'),
  # 66 missing BIOS warning
  ('Games with special chips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) need support files on the card. If one is missing, the menu names the file and stays up, instead of freezing or starting a broken game.',
   'Jogos com chips especiais (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) precisam de arquivos de apoio no cartão. Se faltar algum, o menu diz qual é e continua de pé, em vez de travar ou abrir um jogo quebrado.',
   'Los juegos con chips especiales (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) necesitan archivos de apoyo en la tarjeta. Si falta uno, el menú dice cuál y sigue en pie, en vez de colgarse o abrir un juego roto.',
   'Spiele mit Spezialchips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) brauchen Hilfsdateien auf der Karte. Fehlt eine, nennt das Menü die Datei und bleibt stehen, statt zu hängen oder ein kaputtes Spiel zu starten.',
   "Les jeux à puces spéciales (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) ont besoin de fichiers sur la carte. S'il en manque un, le menu dit lequel et reste ouvert, au lieu de planter ou de lancer un jeu cassé.",
   'I giochi con chip speciali (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) vogliono file di supporto sulla scheda. Se ne manca uno, il menu dice quale e resta attivo, invece di bloccarsi o avviare un gioco rotto.',
   'Играм со специальными чипами (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X и другим) нужны файлы BIOS в папке |/sd2snes|. Если какого-то из них нет, меню показывает его название и остаётся открытым, а не зависает и не запускает нерабочую игру.',
   'Spellen met speciale chips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) hebben hulpbestanden op de kaart nodig. Ontbreekt er een, dan noemt het menu het bestand en blijft het open, in plaats van te hangen.'),
  # 67 the clock question on boot and the date format (snes/time.a65 time_date_sep: month first
  # in English and Chinese, the year first in Japanese, the day first in the others)
  ("When the cartridge's clock is not set, the menu asks for the time on start. {askclock} in {browser} turns the question off. The date below the list follows the language: month first in English and Chinese, year first in Japanese, day first in the others.",
   'Quando o relógio do cartucho não está acertado, o menu pede a hora ao iniciar. {askclock} em {browser} desliga a pergunta. A data no rodapé da lista segue o idioma: mês primeiro em inglês e chinês, ano primeiro em japonês, dia primeiro nos outros.',
   'Si el reloj del cartucho no está en hora, el menú la pide al iniciar. {askclock} en {browser} quita la pregunta. La fecha al pie de la lista sigue el idioma: primero el mes en inglés y chino, el año en japonés, el día en los demás.',
   'Ist die Uhr des Moduls nicht gestellt, fragt das Menü beim Start nach der Zeit. {askclock} in {browser} schaltet die Frage ab. Das Datum unter der Liste folgt der Sprache: Monat zuerst auf Englisch und Chinesisch, Jahr zuerst auf Japanisch, sonst der Tag.',
   "Si l'horloge de la cartouche n'est pas réglée, le menu demande l'heure au démarrage. {askclock} dans {browser} coupe la question. La date en bas de la liste suit la langue : le mois d'abord en anglais et en chinois, l'année en japonais, le jour sinon.",
   "Se l'orologio della cartuccia non è impostato, il menu chiede l'ora all'avvio. {askclock} in {browser} toglie la domanda. La data in fondo alla lista segue la lingua: prima il mese in inglese e cinese, l'anno in giapponese, il giorno nelle altre.",
   'Если часы картриджа не выставлены, меню запрашивает время при старте. {askclock} в {browser} отключает этот запрос. Дата внизу списка соответствует языку: в английском и китайском сначала месяц, в японском - год, в остальных - день.',
   'Is de klok van de cartridge niet ingesteld, dan vraagt het menu bij de start om de tijd. {askclock} in {browser} zet dat uit. De datum onderaan volgt de taal: eerst de maand in het Engels en Chinees, het jaar in het Japans, anders de dag.'),
  # 68 a description for each option
  ("Every option in {cfg} shows a sentence in a box on screen explaining what it does, in the menu's language, so each setting makes sense without a manual.",
   'Cada opção em {cfg} mostra uma frase numa caixa na tela explicando o que ela faz, no idioma do menu, para cada ajuste fazer sentido sem manual.',
   'Cada opción de {cfg} muestra una frase en un recuadro explicando lo que hace, en el idioma del menú, para entender cada ajuste sin manual.',
   'Jede Option in {cfg} zeigt in einem Kasten einen Satz, der erklärt, was sie tut, in der Menüsprache, damit jede Einstellung ohne Handbuch verständlich ist.',
   "Chaque option de {cfg} affiche dans un cadre une phrase qui explique ce qu'elle fait, dans la langue du menu, pour comprendre chaque réglage sans manuel.",
   'Ogni opzione in {cfg} mostra in un riquadro una frase che spiega cosa fa, nella lingua del menu, così ogni impostazione si capisce senza manuale.',
   'Каждая опция в разделе {cfg} снабжена встроенной рамкой-подсказкой на языке меню, поэтому назначение функций понятно без руководства.',
   'Elke optie in {cfg} toont in een kader een zin die uitlegt wat ze doet, in de taal van het menu, zodat elke instelling zonder handleiding duidelijk is.'),
]

# The SNES Mouse's gestures in a paragraph, [ML] [MR] [MB] [MRH] [MLH] (build_const.py
# MOUSE_TOKENS), are drawn as the menu legends' mouse glyph pairs, green like a key. Their
# codes (176, 250-255) are the CJK marker and leads to onb_hiprint, so each glyph goes behind
# MOUSE_ESC (onb_memmap.i65 ONB_MOUSE_ESC: "the next byte is a plain cell", no column of its
# own). From the format to the encode a gesture is ONE private-use character, two cells wide,
# so the wrap never splits it.
MOUSE_ESC = 4
_MOUSE_PUA = {k: chr(0xE000 + i) for i, k in enumerate(bc.MOUSE_TOKENS)}
_PUA_GLYPHS = {_MOUSE_PUA[k]: g for k, (_, g) in bc.MOUSE_TOKENS.items()}


def _mouse(text):
    """A paragraph's [M..] gestures -> [<one private-use character>] (still green)."""
    def sub(m):
        if m.group(1) not in bc.MOUSE_TOKENS:
            sys.exit("gen_onb_lang: unknown mouse gesture [%s] in %r (known: %s)"
                     % (m.group(1), text, ", ".join("[%s]" % k for k in bc.MOUSE_TOKENS)))
        return "[%s]" % _MOUSE_PUA[m.group(1)]
    return bc.MOUSE_RE.sub(sub, text)


def _mouse_bytes(text):
    """The private-use characters -> their escaped glyph bytes ({N} raw bytes)."""
    return "".join("".join("{%d}{%d}" % (MOUSE_ESC, c) for c in _PUA_GLYPHS[ch])
                   if ch in _PUA_GLYPHS else ch for ch in text)


def _has_cjk(text):
    return any(bc.is_cjk(ch) for ch in text)


def _cjk_cells(text):
    """Cells of a string with CJK, as encode_string lays it out: the marker, 2 per glyph at
    an even offset after it (a blank pads an odd one), 1 per other character; the colour
    markup ([button], |blue|) takes none."""
    off, i = 0, 0
    while i < len(text):
        ch = text[i]
        if ch == "{":                           # a raw byte (a menu word's special glyph)
            i = text.index("}", i) + 1
            off += 1
            continue
        if ch in "[]|":
            pass
        elif ch in _PUA_GLYPHS:                 # a mouse gesture: two plain cells
            off += 2
        elif ch == ';':
            sys.exit("gen_onb_lang: %r is not allowed in %r" % (ch, text))
        elif bc.is_cjk(ch):
            off += 2 + (off & 1)
        elif ch in ENCODE or (" " <= ch <= "~"):
            off += 1
        else:
            sys.exit("gen_onb_lang: no font tile for %r (U+%04X) in %r" % (ch, ord(ch), text))
        i += 1
    return off + 1


def _cells(text):
    """Screen cells of a string; aborts on a character the font has no tile for
    (build_const.encode_string would pass it through as raw UTF-8 bytes, which
    draw as two tiles of garbage -- nothing downstream catches that)."""
    if _has_cjk(text):
        return _cjk_cells(text)
    n = 0
    for ch in text:
        if ch in "[]|":                 # colour markup ([green], |blue|): no cell (see _encode_line)
            continue
        if ch in _PUA_GLYPHS:           # a mouse gesture: its glyph pair
            n += 2
            continue
        if ch == ';':
            # the font has no ';' (it draws as //)
            sys.exit("gen_onb_lang: %r is not allowed in %r" % (ch, text))
        if ch in ENCODE or (" " <= ch <= "~"):
            n += 1
        else:
            sys.exit("gen_onb_lang: no font tile for %r in %r" % (ch, text))
    return n


TEXT_W = 30          # == ONB_TEXT_W: cols 1..30
MORE_FIRST = 60      # == ONB_MORE_FIRST: the "and more" items are the descriptors from here
MORE_NAME_W = 24     # an answer row's width (onb_draw_option_value prints labels with 24)
# How tall a card's paragraph may be depends on what the card shows under it, so the cap
# is worked out per card from the engine's own layout (onboarding_main.a65) and the card's
# answer kind (onb_feat_table): the paragraph starts on ONB_TEXT_ROW; an answer box starts
# a blank row and its top border under it, its answers end on ONB_LIST_LAST; an info card
# (no box) may run down to the row above the progress bar. A list longer than
# LIST_MIN_VIS answers scrolls, and keeps at least that many rows. The "and more" card's
# item paragraphs go under its item box instead (ONB_MORE_ROWS, onb_more_body).
LIST_MIN_VIS = 7
_ONB_DIR = _UTILS.parent / "onboarding"
_ONB_DEF = {m.group(1): int(m.group(2)) for f in ("onboarding_main.a65", "onb_memmap.i65")
            for m in re.finditer(r"^#define\s+(ONB_\w+)\s+(\d+)\b",
                                 (_ONB_DIR / f).read_text(encoding="utf-8"), re.M)}
_onb_const = (_ONB_DIR / "onboarding_const.a65").read_text(encoding="utf-8")
_OPT_COUNT = [int(v) for v in
              re.search(r"^onb_opt_count\s+\.byt\s+([\d, ]+)", _onb_const, re.M).group(1).split(",")]
_FEAT_KIND = [_ONB_DEF[k] for k in re.findall(
    r"\.byt\s+ONB_PAL_\w+\s*,\s*(ONB_OPT_\w+)",
    _onb_const[_onb_const.index("onb_feat_table:"):])]


def _text_max_lines(i, n_items):
    """Rows the paragraph of descriptor i may take (see LIST_MIN_VIS)."""
    kind = _FEAT_KIND[i] if i < MORE_FIRST else _ONB_DEF["ONB_OPT_MORE"]
    if kind == _ONB_DEF["ONB_OPT_MORE"]:
        return _ONB_DEF["ONB_MORE_ROWS"] - min(n_items, LIST_MIN_VIS)
    if kind == _ONB_DEF["ONB_OPT_INFO"]:
        return _ONB_DEF["ONB_PROG_ROW"] - _ONB_DEF["ONB_TEXT_ROW"]
    answers = len(LANGS) if kind == _ONB_DEF["ONB_OPT_LANG"] else _OPT_COUNT[kind]
    return (_ONB_DEF["ONB_LIST_LAST"] + 1 - (_ONB_DEF["ONB_TEXT_ROW"] + 2)
            - min(answers, LIST_MIN_VIS))
_LABELS = {"off": "onb_text_off", "on": "onb_text_on", "menu": "onb_text_rst_menu", "large": "onb_text_large",
           "small": "onb_text_small", "ctx": "onb_text_gi_ctx", "folder": "onb_text_rst_folder",
           "rom": "onb_text_rst_game", "hold": "onb_text_rst_hold", "theme": "onb_text_theme",
           "yes": "onb_text_yes", "no": "onb_text_no"}
# the menu's own words, as its dictionaries have them (so a paragraph names what the
# menu shows)
_MENU_LABELS = {"cfg": "mtext_mm_cfg", "sgbmenu": "mtext_cfg_sgb", "auto": "text_auto",
                "prefsgb": "text_gbc_prefer_sgb", "prefgbc": "text_gbc_prefer_gbc",
                "saves": "text_igm_tab_saves", "del": "text_filesel_context_delete_file",
                "savesrow": "text_filesel_context_saves", "browser": "mtext_cfg_browser",
                "restoretheme": "mtext_browser_restoretheme",
                "restoreclassic": "mtext_browser_restoreclassic",
                "hdrmode": "mtext_patch_header_mode", "hdrauto": "text_patch_hdrsel_auto",
                "hdron": "text_patch_hdrsel_on", "hdroff": "text_patch_hdrsel_off",
                "createrom": "mtext_patch_create_rom", "setbgm": "text_filesel_set_as_bgm",
                "restoremusic": "mtext_browser_restoremusic", "ingame": "mtext_cfg_ingame",
                "buscompat": "mtext_scic_buscompat", "askclock": "mtext_cfg_ask_clock",
                "chipopts": "mtext_cfg_chip", "a26w": "mtext_a26_width",
                "sysinfo": "mtext_mm_sysinfo", "memtest": "mtext_mm_memtest",
                "trainer": "text_igm_tab_trainer", "unknown": "text_igm_tr_unknown",
                "changed": "text_igm_tr_changed", "increased": "text_igm_tr_increased",
                "decreased": "text_igm_tr_decreased", "savecheat": "text_igm_tr_savecheat",
                "cheats": "text_filesel_context_cheats", "cheatstab": "text_igm_tab_cheats",
                "hook": "mtext_ingame_enable",
                "goptrow": "mtext_gopt_row", "lockrow": "mtext_lock_row", "onechip": "mtext_ingame_1chiptransientfixes",
                "gdefault": "text_gopt_default", "rmdir": "mtext_fm_rmdir",
                "fmsel": "mtext_fm_select", "restorefont": "mtext_sfn_restore",
                "thmfont": "text_thm_font", "thmsound": "text_thm_sound"}
_MENU_WORDS = {key: menu_text(lab) for key, lab in _MENU_LABELS.items()}
# a sub-option's label without the menu's indent and arrow glyph (" {129}")
_MENU_WORDS["igbuttons"] = tuple(w.replace("{129}", "").strip() for w in menu_text("mtext_ingame_buttons"))
# the players' Repeat key hint ("X: Repeat") without its key
_MENU_WORDS["repeat"] = tuple(w.split(":", 1)[1].strip() if ":" in w else w for w in menu_text("text_media_repeat"))


# A button named in a paragraph is written [A], [L]+[R], [Start]... and drawn green like
# every key hint: each bracket becomes BTN_TOGGLE, a byte onb_hiprint reads as "flip
# between the line's palette and green" without printing or advancing a column.
# |words| is the same for blue (palette 3, the window border's): each pipe becomes
# BLUE_TOGGLE. The two never nest; a |blue| run may span words, and a line break
# inside it closes it at the line's end and opens it again on the next (_carry_blue).
BTN_TOGGLE = 2
BLUE_TOGGLE = 3
_TOGGLES = {"[": BTN_TOGGLE, "]": BTN_TOGGLE, "|": BLUE_TOGGLE}


def _check_markup(line):
    """Abort on unbalanced or nested colour markup."""
    green = blue = False
    for ch in line:
        if ch in "[]":
            if blue or green == (ch == "["):
                sys.exit("gen_onb_lang: bad button markup in %r" % line)
            green = not green
        elif ch == "|":
            if green:
                sys.exit("gen_onb_lang: |blue| inside a [button] in %r" % line)
            blue = not blue
    if green or blue:
        sys.exit("gen_onb_lang: unclosed colour markup in %r" % line)


def _carry_blue(lines):
    """A |blue| run cut by the wrap: closed at the end of its line, reopened on the next."""
    out, carry = [], False
    for l in lines:
        if carry:
            l = "|" + l
        carry = l.count("|") % 2 == 1
        out.append(l + "|" if carry else l)
    return out


def _encode_line(line):
    """encode_string for a paragraph line, the colour markup turned into toggle bytes."""
    _check_markup(line)
    if _has_cjk(line):
        # one encode for the whole line: a CJK string carries ONE marker, at its start, and
        # the toggles (and the mouse escapes) take no column in its alignment
        t = "".join("{%d}" % _TOGGLES[ch] if ch in _TOGGLES else ch for ch in line)
        while t.endswith(("{%d}" % BTN_TOGGLE, "{%d}" % BLUE_TOGGLE)):
            t = t[:-3]
        return encode_string(_mouse_bytes(t), zero_width=(BTN_TOGGLE, BLUE_TOGGLE, MOUSE_ESC))[:-3]
    line = _mouse_bytes(line)
    toks, seg = [], ""
    for ch in line + "\0":
        if ch in _TOGGLES or ch == "\0":
            if seg:
                toks.append(encode_string(seg)[:-3])    # drop the ", 0"
            seg = ""
            if ch != "\0":
                toks.append(str(_TOGGLES[ch]))
        else:
            seg += ch
    # A toggle that closes the line does nothing (every line starts in its own colour),
    # and on a full line it would be the byte after the last cell: onb_hiprint stops on
    # its cell count before reading it, and onb_emit_para would take it for "another
    # line" and print an empty one.
    while toks and toks[-1] in (str(BTN_TOGGLE), str(BLUE_TOGGLE)):
        toks.pop()
    return ", ".join(toks)


# Japanese/Chinese line breaking: a line may break between any two characters, except
# before these (closing punctuation, small kana, the long-vowel mark) or after the openers.
_NO_START = set("、。，．・：；！？）」』】〕〉》ー々ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ”’)]!?,.:%")
_NO_END = set("（「『【〔〈《“‘([")


def _cjk_units(text):
    """A CJK text as unbreakable units: one per CJK character, a run of other characters
    (a Latin word, a number, [A]) as one, and the blanks between them as break points."""
    units, cur = [], ""
    for ch in text:
        if bc.is_cjk(ch) or ch == " ":
            if cur:
                units.append(cur)
                cur = ""
            units.append(ch)
        else:
            cur += ch
    if cur:
        units.append(cur)
    return units


def _wrap_cjk(text, width):
    lines, cur = [], []
    for u in _cjk_units(text):
        if u == " " and not cur:
            continue
        cand = "".join(cur + [u]).rstrip()
        if not cur or _cells(cand) <= width:
            cur.append(u)
            continue
        # break: never start the line with closing punctuation, never end one on an opener
        nxt = [u]
        while cur and (nxt[0][0] in _NO_START or cur[-1][-1] in _NO_END):
            nxt.insert(0, cur.pop())
        if not cur:
            sys.exit("gen_onb_lang: cannot break %r in %d cells" % (text, width))
        lines.append("".join(cur).strip())
        cur = nxt if nxt[0] != " " else nxt[1:]
    if cur:
        lines.append("".join(cur).strip())
    for l in lines:
        if _cells(l) > width:
            sys.exit("gen_onb_lang: line %r is %d cells, max %d" % (l, _cells(l), width))
    return lines


def _wrap(text, width):
    """Word-wrap to width cells; a "\n" starts a new line (each piece wraps on its own)."""
    if "\n" in text:
        return [l for piece in text.split("\n") for l in _wrap(piece, width)]
    if _has_cjk(text):
        return _wrap_cjk(text, width)
    lines, cur = [], ""
    for word in text.split():
        cand = (cur + " " + word) if cur else word
        if _cells(cand) <= width:
            cur = cand
        elif word in (":", "?", "!") and " " in cur:
            # French puts a blank before these: never start a line with one, take the
            # word it belongs to along
            head, last = cur.rsplit(" ", 1)
            lines.append(head)
            cur = last + " " + word
        else:
            if cur:
                lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


if len(NAMES) != len(TEXTS):
    sys.exit("gen_onb_lang: %d names, %d texts" % (len(NAMES), len(TEXTS)))
NAMES = [_cjk_cols("name_%d" % (i + 1), v) for i, v in enumerate(NAMES)]
TEXTS = [_cjk_cols("text_%d" % (i + 1), v) for i, v in enumerate(TEXTS)]
_wrapped = []
for _n, (_names, _texts) in enumerate(zip(NAMES, TEXTS), 1):
    if len(_names) != len(LANGS) or len(_texts) != len(LANGS):
        sys.exit("feature %d: expected %d languages" % (_n, len(LANGS)))
    STRINGS["onb_f%d_name" % _n] = _names
    if _n > MORE_FIRST:             # an "and more" item: its name is a row of that card's list
        for _nm in _names:
            if _cells(_nm) > MORE_NAME_W:
                sys.exit("gen_onb_lang: feature %d name %r is %d cells, the list row holds %d"
                         % (_n, _nm, _cells(_nm), MORE_NAME_W))
    cols = []
    for _k, _t in enumerate(_texts):
        # the menu's own words and the answers the text names come out blue (|...|): blue
        # is the tour's highlight
        if "|{" in _t or "}|" in _t:
            # a {placeholder} is blue on its own: |{cfg}| would flip it back to white
            sys.exit("gen_onb_lang: {placeholder} inside |blue| in %r" % _t)
        _t = _t.format(**{key: "|%s|" % STRINGS[lab][_k] for key, lab in _LABELS.items()},
                       **{key: "|%s|" % words[_k] for key, words in _MENU_WORDS.items()})
        cols.append(_carry_blue(_wrap(_mouse(_t), TEXT_W)))
    _wrapped.append(cols)
# pad: every language of a card to its longest, and the "and more" items to each other
_MORE = range(MORE_FIRST, len(_wrapped))
_more_h = max(len(c) for i in _MORE for c in _wrapped[i])
if len(_FEAT_KIND) != len(_wrapped):
    sys.exit("gen_onb_lang: onb_feat_table has %d descriptors, the texts %d"
             % (len(_FEAT_KIND), len(_wrapped)))
for _i, cols in enumerate(_wrapped):
    h = _more_h if _i in _MORE else max(len(c) for c in cols)
    _max = _text_max_lines(_i, len(_MORE))
    if h > _max:
        sys.exit("gen_onb_lang: feature %d text is %d lines, max %d" % (_i + 1, h, _max))
    STRINGS["onb_f%d_text" % (_i + 1)] = tuple(c + [""] * (h - len(c)) for c in cols)


# The strings: every (label, language) string is placed on its own, first fit, in the banks
# that have room (STR_BANKS, in order), and the dispatch tables (onb_const_lang_t.a65, in a
# picture bank of their own: TABLE_BANK) hold a LONG pointer per entry (word + bank byte), so
# onb_resolve_str (onb_ui.a65) takes the bank from the entry, not from the language. Until the
# 2.18 cards a language's whole column had to fit in one bank (pool A en/pt/es/it in $C1, pool B
# de/fr/ru/nl in $C0, Japanese in $CB, Chinese in $C1), and none of them had room left; the
# ~36 KB the picture banks have free past their 8 pictures were out of reach.
# STR_ROOM: what each bank has for strings = 65536 minus what the link puts there besides them
# (the code, the pictures, the font; measured from the objects, with room for $C0's code to
# grow). The CJK sheets and the tables are taken off below, from their real size. A wrong
# number shows up as an overflow at the link, never as a silent error.
TABLE_BANK = "$c4"
CJK_SHEET_BANK = {"ja": "$ca", "zh": "$c7"}
_PICS = 8 * (7168 + 256)                      # a bank of 8 demo pictures (gen_onb_demo.py PER_BANK)
STR_OTHER = {
    "$c1": 64,                                # onb_strtab_nlang and the CJK sheet pointers
    "$c0": 11700,                             # the engine's code, palette, spc700 (~11.2 KB) + room to grow
    "$cb": 6 * (7168 + 256),                  # the last 6 pictures
    "$c5": _PICS, "$c6": _PICS,
    "$c3": _PICS + 16 * 64,                   # + onb_demo_map (16 bytes a card)
    "$c8": _PICS + 1800,                      # + onb_welcome.a65's code
    "$c9": _PICS + 1900,                      # + onb_cjk.a65's code
    "$ca": _PICS,                             # + the Japanese sheet (below)
    "$c7": _PICS,                             # + the Chinese sheet (below)
    "$c2": _PICS + 474 + 4096,                # + the picture index table and the tour's font copy
}
STR_BANKS = ["$c1", "$c0", "$cb", "$c5", "$c6", "$c3", "$c8", "$c9", "$ca", "$c7", "$c2"]
STR_MARGIN = 64
# the language list of the tour's language card: onboarding_const.a65's onb_langname_tbl is
# .word, in $C0, so the CJK languages' own names live in $C0 (onb_const_lang_b.a65)
LANGNAME_BANK = "$c0"


def str_file(bank):
    """The object a bank's strings go into: $C1 and $C0 keep their old names."""
    return {"$c1": "onb_const_lang.a65", "$c0": "onb_const_lang_b.a65"}.get(bank, "onb_str_%s.a65" % bank[1:])


def sheet_file(lang):
    """The sheet file of a CJK language: the first one's also holds the common glyphs."""
    return "onb_cjk_sheets.a65" if lang == CJK_LANGS[0] else "onb_cjk_sheets_%s.a65" % lang
CJK_COMMON_MAX = 128          # glyphs 0..127: shown in every language (the languages' names)
CJK_LANG_FIRST = 128          # 128..767: the active CJK language's
CJK_LANG_MAX = 640
LANG_NAME_LABELS = {"ja": "text_lang_ja", "zh": "text_lang_zh"}   # the menu's (const.a65)


def _glyph_2bpp(rows):
    t = bc.cjk_glyph_tiles(rows)            # 4bpp: left tile 32 B, right tile 32 B
    return t[0:16] + t[32:48]               # planes 0/1 of each: BG2 is 2bpp


def _texts_of(vals):
    for v in vals:
        if isinstance(v, list):
            yield from v
        else:
            yield v


def cjk_maps():
    """The glyph registries: (common_map, lang_maps, fonts, common) -- char -> glyph index for
    what every language shows (the CJK languages' own names, in their own fonts) and for each
    CJK language; the fonts by language; the font each common glyph comes from."""
    fonts = {l: bc.load_cjk_font(_UTILS.parent / "fonts" / bc.CJK_LANGS[l]) for l in CJK_LANGS}
    langnames = {l: decode_args(_EN[LANG_NAME_LABELS[l]]) for l in CJK_LANGS}
    common = {}                              # char -> font
    for l, name in langnames.items():
        for ch in name:
            if bc.is_cjk(ch):
                common.setdefault(ch, l)
    for label, vals in STRINGS.items():
        for k, text in enumerate(vals):
            if LANGS[k] not in CJK_LANGS:
                for t in _texts_of([text]):
                    for ch in t:
                        if bc.is_cjk(ch):
                            common.setdefault(ch, CJK_LANGS[0])
    if len(common) > CJK_COMMON_MAX:
        sys.exit("gen_onb_lang: %d common CJK glyphs, room for %d" % (len(common), CJK_COMMON_MAX))
    common_map = {ch: i for i, ch in enumerate(sorted(common))}
    lang_maps = {}
    for l in CJK_LANGS:
        k = LANGS.index(l)
        used = sorted({ch for vals in STRINGS.values() for t in _texts_of([vals[k]]) for ch in t
                       if bc.is_cjk(ch)})
        missing = [ch for ch in used if ch not in fonts[l]]
        if missing:
            sys.exit("gen_onb_lang: [%s] no glyph in %s for %s" % (l, bc.CJK_LANGS[l], "".join(missing)))
        if len(used) > CJK_LANG_MAX:
            sys.exit("gen_onb_lang: [%s] %d CJK glyphs, room for %d" % (l, len(used), CJK_LANG_MAX))
        lang_maps[l] = {ch: CJK_LANG_FIRST + i for i, ch in enumerate(used)}

    return common_map, lang_maps, fonts, common


def main():
    out_path = "onb_const_lang.a65"
    if "-o" in sys.argv:
        out_path = sys.argv[sys.argv.index("-o") + 1]
    out_dir = os.path.dirname(out_path)
    out_t = os.path.join(out_dir, "onb_const_lang_t.a65")

    common_map, lang_maps, fonts, common = cjk_maps()
    langnames = {l: decode_args(_EN[LANG_NAME_LABELS[l]]) for l in CJK_LANGS}

    def use(lang):
        bc.CJK = lang_maps.get(lang, common_map)

    nlang = len(LANGS)
    head = ["; ==========================================================================",
            "; AUTO-GENERATED by utils/gen_onb_lang.py -- DO NOT EDIT BY HAND.",
            "; Onboarding i18n string pool (" + "/".join(l.upper()[:2] for l in LANGS) + "). Edit the",
            "; STRINGS table in the generator and re-run `make` instead.",
            "; =========================================================================="]
    # one object per bank that holds strings (str_file); $C1's also carries the tables below
    F = {b: head + ["; the tour's strings placed in bank %s (gen_onb_lang.py STR_BANKS)" % b,
                    ".link page %s" % b, "", "onb_str_%s:" % b[1:]] for b in STR_BANKS}
    S = {l: head + ["; the %s glyph sheet (onb_cjk.a65): 8 bytes a glyph, its rows top-down, bit 7 the" % l,
                    "; leftmost pixel", ".link page %s" % CJK_SHEET_BANK[l], ""] for l in CJK_LANGS}
    L = F["$c1"]
    L.append("onb_strtab_nlang  .byt %d" % nlang)
    L.append("; per language: its CJK glyph sheet (0 = a language without one), glyphs 128 up")
    L.append("onb_cjk_sheet     .word " + ", ".join("!onb_sheet_" + l if l in CJK_LANGS else "0"
                                                      for l in LANGS))
    L.append("onb_cjk_sheet_bank .byt " + ", ".join("^onb_sheet_" + l if l in CJK_LANGS else "0"
                                                     for l in LANGS))
    L.append("")
    # The dispatch tables: a row per label, a long pointer (word + bank) per language. Every
    # reader takes their bank from ^onb_strtab_lo, and no string shares it, so no string falls
    # in the table range.
    T = head + [".link page %s" % TABLE_BANK, "",
         "; ---- dispatch tables (one row of %d long pointers per label) ----" % nlang,
         "onb_strtab_lo:"]
    for label in STRINGS:
        T.append("%s:" % label)
        for lang in LANGS:
            T.append("  .word !%s_%s : .byt ^%s_%s" % (label, lang, label, lang))
    T.append("onb_strtab_hi:")
    T.append("")
    table_size = len(STRINGS) * nlang * 3
    S0 = S[CJK_LANGS[0]]
    S0.append("; the common glyphs (0..%d), in every language" % (len(common_map) - 1))
    S0.append("onb_cjk_common:")
    for ch, i in sorted(common_map.items(), key=lambda kv: kv[1]):
        S0.append("  .byt " + ", ".join("$%02x" % b for b in fonts[common[ch]][ch]) + "   ; %s" % ch)

    sheets = {}
    for l in CJK_LANGS:
        S[l].append("")
        S[l].append("; %s: %d glyphs, from %d" % (l, len(lang_maps[l]), CJK_LANG_FIRST))
        S[l].append("onb_sheet_%s:" % l)
        for ch, i in sorted(lang_maps[l].items(), key=lambda kv: kv[1]):
            S[l].append("  .byt " + ", ".join("$%02x" % b for b in fonts[l][ch]) + "   ; %s" % ch)
        sheets[l] = 8 * len(lang_maps[l])
    sheets[CJK_LANGS[0]] += 8 * len(common_map)

    # the CJK languages' own names, for the language list (in $C0, see LANGNAME_BANK)
    use(None)
    room = {b: 65536 - STR_OTHER[b] - STR_MARGIN for b in STR_BANKS}
    for l in CJK_LANGS:
        enc = encode_string(langnames[l])
        F[LANGNAME_BANK].append("onb_langname_%d  .byt %s" % (LANGS.index(l), enc))
        room[LANGNAME_BANK] -= len(bc.args_to_bytes(enc))
    for l in CJK_LANGS:
        room[CJK_SHEET_BANK[l]] -= sheets[l]
    if TABLE_BANK in room:
        room[TABLE_BANK] -= table_size
    if 65536 - _PICS - table_size < STR_MARGIN:
        sys.exit("gen_onb_lang: the tables (%d B) do not fit in %s" % (table_size, TABLE_BANK))

    # every string, encoded, then placed first fit
    items = []
    size = dict.fromkeys(LANGS, 0)
    for label, vals in STRINGS.items():
        if len(vals) != nlang:
            sys.exit("label %s has %d values, expected %d" % (label, len(vals), nlang))
        for lang, text in zip(LANGS, vals):
            use(lang)
            if isinstance(text, list):          # a paragraph: lines joined by byte 1
                toks = []
                for k, line in enumerate(text):
                    if _cells(line) > TEXT_W:
                        sys.exit("gen_onb_lang: %s[%s] line is %d cells, max %d: %r"
                                 % (label, lang, _cells(line), TEXT_W, line))
                    enc = _encode_line(line)
                    if enc:
                        toks.append(enc)
                    toks.append("1" if k < len(text) - 1 else "0")
                args = ", ".join(toks)
            else:
                if any(ch in text for ch in "[]|"):
                    # colour markup is a paragraph thing: here it would print as is
                    sys.exit("gen_onb_lang: %s[%s] has [ ] or | outside a paragraph: %r"
                             % (label, lang, text))
                if _cells(text) > _budget(label):
                    sys.exit("gen_onb_lang: %s[%s] is %d cells, budget %d: %r"
                             % (label, lang, _cells(text), _budget(label), text))
                args = encode_string(text)
            n = len(bc.args_to_bytes(args))
            size[lang] += n
            items.append(("%s_%s" % (label, lang), args, n))
    used = dict.fromkeys(STR_BANKS, 0)
    for name, args, n in items:
        for b in STR_BANKS:
            if used[b] + n <= room[b]:
                F[b].append("%s  .byt %s" % (name, args))
                used[b] += n
                break
        else:
            sys.exit("gen_onb_lang: no bank has room for %s (%d B): %s" % (name, n,
                     ", ".join("%s %d free" % (b, room[b] - used[b]) for b in STR_BANKS)))

    for b in STR_BANKS:
        with open(os.path.join(out_dir, str_file(b)), "w", encoding="utf-8") as f:
            f.write("\n".join(F[b]) + "\n")
    with open(out_t, "w", encoding="utf-8") as f:
        f.write("\n".join(T) + "\n")
    for l in CJK_LANGS:
        with open(os.path.join(out_dir, sheet_file(l)), "w", encoding="utf-8") as f:
            f.write("\n".join(S[l]) + "\n")
    print("generated %s: %d labels x %d languages, %d B of strings (%s); tables %d B in %s; %s; %d common glyphs"
          % (os.path.basename(out_path), len(STRINGS), nlang, sum(size.values()),
             " ".join("%s=%d" % kv for kv in size.items()), table_size, TABLE_BANK,
             ", ".join("%s %d glyphs %d B in %s" % (l, len(lang_maps[l]), sheets[l], CJK_SHEET_BANK[l])
                       for l in CJK_LANGS), len(common_map)))
    print("gen_onb_lang: strings per bank: " + ", ".join("%s %d (%d free)" % (b, used[b], room[b] - used[b])
                                                         for b in STR_BANKS))


if __name__ == "__main__":
    main()
