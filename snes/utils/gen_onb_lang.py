#!/usr/bin/env python3
"""Generate onb_const_lang.a65 -- the onboarding ROM's i18n string pool.

Self-contained generator (the onboarding is a separate image, so it has its own
strtab independent of the menu's const_lang*.a65). Mirrors the menu's resolve_str
dispatch format: each localized label is a ROW of N word pointers living between
`onb_strtab_lo` and `onb_strtab_hi`; onb_resolve_str (onb_ui.a65) recognizes a
pointer in that range and swaps the column by onb_cur_lang (the menu's language
index, 8 columns). The per-language strings live outside the dispatch range, in a
pool split by language over two banks (POOL_B_LANGS): pool A (en/pt/es/it) after
onb_strtab_hi in $C1, pool B (de/fr/ru/nl) in onb_const_lang_b.a65, in the code bank
$C0; onb_strpool_bank gives each column its bank. Accent/Cyrillic encoding is
build_const.encode_string's; a character the font has no tile for aborts the build
(_cells), and so does a string wider than the cells the engine prints it with (_budget).

CONTENT MODEL: per feature, name + desc (2 lines) + howto, shown in ONE language
(onb_cur_lang, cycled by X). The rest of a card (its answer kind, CFG byte, demo pictures) lives in
onboarding_const.a65 ($C0).

Usage: gen_onb_lang.py -o onb_const_lang.a65
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_const import encode_string, ENCODE

# Column order == onb_cur_lang index == the menu's CFG_LANGUAGE.
LANGS = ["en", "ptbr", "es", "de", "fr", "it", "ru", "nl"]

from pathlib import Path
from build_const import parse_base, load_dict, decode_args
_UTILS = Path(os.path.dirname(os.path.abspath(__file__)))
_EN = parse_base(_UTILS.parent / "const.a65")[1]
_DICTS = {l: load_dict(_UTILS / ("lang_%s.py" % l)) for l in LANGS[1:]}


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
    "onb_ui_more":      ("SEE ALSO", "VEJA TAMBÉM", "VER TAMBIÉN", "SIEHE AUCH", "VOIR AUSSI", "VEDI ANCHE", "СМ. ТАКЖЕ", "ZIE OOK"),
    "onb_ui_next":      ("NEXT", "AVANÇAR", "AVANZAR", "WEITER", "SUIVANT", "AVANTI", "ДАЛЕЕ", "VERDER"),
    "onb_ui_back":      ("BACK", "VOLTAR", "ATRÁS", "ZURÜCK", "RETOUR", "TORNA", "НАЗАД", "TERUG"),
    "onb_ui_exit":      ("EXIT", "SAIR", "SALIR", "ENDE", "QUITTER", "ESCI", "ВЫХОД", "STOPPEN"),
    "onb_ui_menu":      ("MENU", "MENU", "MENÚ", "MENÜ", "MENU", "MENU", "МЕНЮ", "MENU"),
    "onb_ui_done_title":("ALL SET!", "TUDO PRONTO!", "¡TODO LISTO!", "ALLES FERTIG!",
                         "C'EST PRÊT !", "TUTTO PRONTO!", "ВСЁ ГОТОВО!", "KLAAR!"),
    "onb_ui_done_l1":   ("Your choices will be saved.", "Suas escolhas serão salvas.",
                         "Tus elecciones se guardarán.", "Deine Auswahl wird gespeichert.",
                         "Tes choix seront enregistrés.", "Le tue scelte saranno salvate.",
                         "Твой выбор будет сохранён.", "Je keuzes worden opgeslagen."),
    "onb_ui_done_l2":   ("Change them later in Configuration.", "Mude depois em Configurações.",
                         "Cámbialas luego en Configuración.", "Später in Einstellungen änderbar.",
                         "Modifie-les dans Configuration.", "Cambiale poi in Configurazione.",
                         "Их можно менять в настройках.", "Wijzig ze later in Configuratie."),
    "onb_ui_done_go":   ("PRESS A FOR THE MENU", "APERTE A PARA O MENU", "PULSA A PARA EL MENÚ",
                         "A DRÜCKEN FÜRS MENÜ", "APPUIE SUR A : MENU", "PREMI A PER IL MENU",
                         "НАЖМИ A ДЛЯ МЕНЮ", "DRUK OP A: MENU"),
}

# the Competition Cart round, one answer per value: 0..15 = 3..18 minutes (cfg.h cc_time_limit)
for _m in range(3, 19):
    STRINGS["onb_text_cc_%d" % _m] = tuple("%d %s" % (_m, "мин" if l == "ru" else "min") for l in LANGS)

# The descriptors in tour order, by name: the base tour's cards (1-32, "and more" last),
# the 2.17 section (33-40: its separator card, then the release's cards), then the
# "and more" items (41-48, ONB_MORE_FIRST in onboarding_main.a65). Per language.
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
  ('Covers in Recent/Favorites', 'Capas em Recentes/Favoritos', 'Carátulas en las listas', 'Cover in Letzte/Favoriten', 'Jaquettes dans Récents/Favoris', 'Copertine in Recenti/Preferiti', 'Обложки в недавних/избранном', 'Hoezen in Recent/Favorieten'),
  # 7 game info card
  ('Game info card', 'Ficha do jogo', 'Ficha del juego', 'Spiel-Infokarte', 'Fiche du jeu', 'Scheda del gioco', 'Об игре', 'Spelinfo'),
  # 8 the video clip on the game info card (needs the card)
  ('Video in the game info', 'Vídeo na ficha', 'Vídeo en la ficha', 'Video in der Infokarte', 'Vidéo dans la fiche', 'Video nella scheda', 'Видео в карточке', 'Video in de spelinfo'),
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
  ('The cheat list', 'Lista de cheats', 'Lista de cheats', 'Cheat-Liste', 'Liste des cheats', 'Elenco dei cheat', 'Список читов', 'Cheatlijst'),
  # 18 in-game menu
  ('In-game menu', 'Menu in-game', 'Menú en el juego', 'Ingame-Menü', 'Menu en jeu', 'Menu in gioco', 'Меню в игре', 'In-game menu'),
  # 19 savestates
  ('Savestates', 'Savestates', 'Savestates', 'Savestates', 'Savestates', 'Savestate', 'Сейвстейты', 'Savestates'),
  # 20 4 battery saves per game, the in-game menu's SAVES tab (needs the in-game menu)
  ('4 saves per game', '4 saves por jogo', '4 partidas por juego', '4 Spielstände pro Spiel', '4 sauvegardes par jeu', '4 salvataggi per gioco', '4 сохранения на игру', '4 saves per spel'),
  # 21 the in-game menu's RAM trainer (needs the in-game menu)
  ('RAM trainer', 'Treinador de RAM', 'Entrenador de RAM', 'RAM-Trainer', 'Trainer RAM', 'Trainer RAM', 'Трейнер памяти', 'RAM-trainer'),
  # 22 IPS/BPS patches
  ('IPS/BPS patches', 'Patches IPS/BPS', 'Parches IPS/BPS', 'IPS/BPS-Patches', 'Patchs IPS/BPS', 'Patch IPS/BPS', 'Патчи IPS/BPS', 'IPS/BPS-patches'),
  # 23 creating the patched ROM from the [Y] menu of a patch
  menu_text("mtext_patch_create_rom"),
  # 24 more special chips (its own card)
  ('More special chips', 'Mais chips especiais', 'Más chips especiales', 'Weitere Spezialchips', 'Autres puces spéciales', 'Altri chip speciali', 'Ещё специальные чипы', 'Meer speciale chips'),
  # 25 Sufami Turbo: the Slot B picker
  ('Sufami Turbo Slot B', 'Slot B do Sufami Turbo', 'Ranura B del Sufami Turbo', 'Sufami Turbo: Slot B', 'Sufami Turbo : port B', 'Slot B del Sufami Turbo', 'Слот B Sufami Turbo', 'Sufami Turbo: slot B'),
  # 26 Competition Carts: the round's minutes (3..18, the events used 6)
  ('Competition Cart round', 'Round dos Competition Carts', 'Ronda de los Competition Carts', 'Runde der Competition Carts', 'Manche des Competition Carts', 'Round dei Competition Cart', 'Раунд Competition Cart', 'Ronde van de Competition Carts'),
  # 27 other consoles: NES, Master System, Game Boy Color, Atari 2600
  ('Other consoles', 'Outros consoles', 'Otras consolas', 'Andere Konsolen', 'Autres consoles', 'Altre console', 'Другие консоли', 'Andere consoles'),
  # 28 the Atari 2600's console switches on the pad, and its picture width
  ('Atari 2600 controls', 'Controles do Atari 2600', 'Controles del Atari 2600', 'Atari-2600-Steuerung', 'Commandes Atari 2600', 'Comandi Atari 2600', 'Управление Atari 2600', 'Atari 2600-bediening'),
  # 29 the card's folders: 2-letter buckets, console folders, Organize
  ('Folders on the card', 'Pastas no cartão', 'Carpetas en la tarjeta', 'Ordner auf der Karte', 'Dossiers sur la carte', 'Cartelle sulla scheda', 'Папки на карте', 'Mappen op de kaart'),
  # 30 the memory test of the main menu
  ('Memory test', 'Teste de memória', 'Prueba de memoria', 'Speichertest', 'Test mémoire', 'Test di memoria', 'Тест памяти', 'Geheugentest'),
  # 31 the Mk.II LED codes for a boot without a picture
  ('Mk.II boot errors on the LED', 'Erros de boot no LED do Mk.II', 'Errores de arranque en el LED', 'Startfehler an der Mk.II-LED', 'LED du Mk.II au démarrage', 'Errori di avvio sul LED Mk.II', 'Ошибки запуска на LED Mk.II', 'Opstartfouten op de Mk.II-LED'),
  # 32 "and more" (the base tour's last card)
  ('And more', 'E mais', 'Y más', 'Und mehr', 'Et plus', 'E altro', 'И ещё', 'En meer'),
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
  ('Seta chips and bootlegs', 'Chips Seta e bootlegs', 'Chips Seta y bootlegs', 'Seta-Chips und Bootlegs', 'Puces Seta et bootlegs', 'Chip Seta e bootleg', 'Чипы Seta и бутлеги', 'Seta-chips en bootlegs'),
  # 38 Super 20 in 1, Gamars Puzzle and .sfrom (2.17)
  ('Super 20 in 1, Gamars, .sfrom',)*8,
  # 39 file-type icons in the list (2.17)
  ('Icons in the list', 'Ícones na lista', 'Iconos en la lista', 'Symbole in der Liste', 'Icônes dans la liste', 'Icone nella lista', 'Значки в списке', 'Pictogrammen in de lijst'),
  # 40 the cheat list from the game info card (2.17)
  ('Cheats from the game info', 'Cheats pela ficha do jogo', 'Cheats desde la ficha', 'Cheats aus der Infokarte', 'Cheats depuis la fiche', 'Cheat dalla scheda', 'Читы в окне Об игре', 'Cheats vanuit spelinfo'),
  # 41 clear the PPU before a patched game boots
  ('Clear PPU on boot', 'Limpar PPU no boot', 'Limpiar PPU al arrancar', 'PPU beim Start löschen', 'Effacer le PPU au boot', "Pulisci PPU all'avvio", 'Очистка PPU при старте', 'Leeg PPU bij starten'),
  # 42 bus timing compat
  ('Bus timing compat', 'Compat. de barramento', 'Compat. de bus', 'Bus-Timing-Kompat.', 'Compat. timing bus', 'Compat. timing bus', 'Совместимость шины', 'Compat. bustiming'),
  # 43 the hardware model in System Information
  ('Hardware model', 'Modelo do hardware', 'Modelo de hardware', 'Hardware-Modell', 'Modèle du matériel', "Modello dell'hardware", 'Модель устройства', 'Hardwaremodel'),
  # 44 BS-X and the Memory Pack
  ('BS-X and Memory Pack', 'BS-X e Memory Pack', 'BS-X y Memory Pack', 'BS-X und Memory Pack', 'BS-X et Memory Pack', 'BS-X e Memory Pack', 'BS-X и Memory Pack', 'BS-X en Memory Pack'),
  # 45 delete files and saves
  ('Delete files and saves', 'Apagar arquivos e saves', 'Borrar archivos y saves', 'Dateien/Saves löschen', 'Effacer fichiers/saves', 'Eliminare file e save', 'Удаление файлов', 'Bestanden/saves wissen'),
  # 46 missing BIOS warning
  ('Missing BIOS warning', 'Aviso de BIOS faltando', 'Aviso de BIOS que falta', 'Fehlendes BIOS melden', 'Alerte de BIOS manquant', 'Avviso di BIOS mancante', 'Нет файла BIOS', 'Melding: BIOS ontbreekt'),
  # 47 the clock question on boot and the date format
  ('Clock and date', 'Relógio e data', 'Reloj y fecha', 'Uhr und Datum', 'Horloge et date', 'Orologio e data', 'Часы и дата', 'Klok en datum'),
  # 48 a description for each option
  ('Option descriptions', 'Descrição das opções', 'Descripción de opciones', 'Optionsbeschreibungen', 'Description des options', 'Descrizione opzioni', 'Описание опций', 'Uitleg bij opties'),
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
  ('The whole menu, the in-game menu and this tour in 8 languages. Move the bar to try one: the tour switches at once. You can change it later in Configuration.',
   'O menu inteiro, o menu in-game e este tour em 8 idiomas. Mova a barra para experimentar: o tour troca na hora. Dá para mudar depois em Configurações.',
   'Todo el menú, el menú del juego y este tour en 8 idiomas. Mueve la barra para probar: el tour cambia al instante. Puedes cambiarlo luego en Configuración.',
   'Das ganze Menü, das Ingame-Menü und diese Tour in 8 Sprachen. Bewege den Balken zum Testen: die Tour wechselt sofort. Später in den Einstellungen änderbar.',
   'Tout le menu, le menu en jeu et cette visite en 8 langues. Déplace la barre pour essayer : la visite change aussitôt. Modifiable ensuite dans Configuration.',
   'Tutto il menu, il menu in gioco e questo tour in 8 lingue. Sposta la barra per provare: il tour cambia subito. Puoi cambiarla dopo in Configurazione.',
   'Всё меню, Меню в игре и этот тур на 8-ми языках. Двигай полосу, чтобы попробовать: тур сразу переключится. Потом можно сменить в настройках.',
   'Het hele menu, het in-game menu en deze rondleiding in 8 talen. Beweeg de balk om te proberen: de rondleiding wisselt meteen. Later te wijzigen in Configuratie.'),
  # 2 themes: the sd2snes+ theme, a .thm from the list, restoring it or the classic one
  ("Change the menu's logo, colours and gradient: pick a .thm in the list to apply it. Make your own in the Theme Creator or take one from the site's gallery. In {browser}, {restoretheme} brings back sd2snes+, the factory theme, and {restoreclassic} the old teal look (classic.thm).",
   'Troque o logo, as cores e o gradiente do menu: escolha um .thm na lista para aplicar. Crie o seu no Theme Creator ou pegue um na galeria do site. Em {browser}, {restoretheme} volta ao sd2snes+, o tema de fábrica, e {restoreclassic} ao visual verde-azulado antigo (classic.thm).',
   'Cambia el logo, los colores y el degradado del menú: elige un .thm en la lista para aplicarlo. Crea el tuyo en el Theme Creator o baja uno de la galería del sitio. En {browser}, {restoretheme} vuelve a sd2snes+, el tema de fábrica, y {restoreclassic} al aspecto turquesa de antes (classic.thm).',
   'Ändere Logo, Farben und Verlauf des Menüs: eine .thm in der Liste wählen wendet sie an. Eigene im Theme Creator oder aus der Galerie der Website. In {browser} holt {restoretheme} das Werks-Theme sd2snes+ zurück, {restoreclassic} den alten türkisen Look (classic.thm).',
   "Change logo, couleurs et dégradé du menu avec un .thm choisi dans la liste. Crée le tien dans le Theme Creator ou pioche dans la galerie du site. {restoretheme} ({browser}) remet sd2snes+, le thème d'origine, et {restoreclassic} l'ancien look turquoise (classic.thm).",
   'Cambia logo, colori e sfumatura del menu: scegli un .thm nella lista per applicarlo. Crea il tuo nel Theme Creator o prendine uno dalla galleria del sito. In {browser}, {restoretheme} torna a sd2snes+, il tema di fabbrica, e {restoreclassic} al vecchio aspetto turchese (classic.thm).',
   'Меняй логотип, цвета и градиент меню: выбери файл .thm в списке, чтобы применить. Создай свою тему в Theme Creator или возьми из галереи на сайте. В разделе {browser} пункт {restoretheme} вернёт заводскую sd2snes+, а {restoreclassic} старый бирюзовый вид (classic.thm).',
   'Wijzig logo, kleuren en verloop van het menu: een .thm in de lijst kiezen past het toe. Maak je eigen in de Theme Creator of haal er een op de site. {restoretheme} ({browser}) zet het standaardthema sd2snes+ terug, {restoreclassic} de oude turquoise look (classic.thm).'),
  # 3 text outline: theme / on / off
  ('The dark edge around each letter of the menu. {theme} follows what the theme asks for, {on} always draws it, {off} drops it and lets the background show through. Move the bar to try one: this text changes at once, and [A] keeps it for the menu.',
   'A borda escura em volta de cada letra do menu. {theme} segue o que o tema pede, {on} sempre desenha, {off} tira a borda e deixa o fundo aparecer. Mova a barra para experimentar: este texto muda na hora, e [A] grava a escolha para o menu.',
   'El borde oscuro alrededor de cada letra del menú. {theme} sigue lo que pide el tema, {on} siempre lo dibuja, {off} lo quita y deja ver el fondo. Mueve la barra para probar: este texto cambia al instante y [A] lo guarda para el menú.',
   'Der dunkle Rand um jeden Buchstaben des Menüs. {theme} folgt dem Theme, {on} zeichnet ihn immer, {off} lässt ihn weg und den Hintergrund durchscheinen. Bewege den Balken zum Testen: dieser Text ändert sich sofort, [A] übernimmt es fürs Menü.',
   "Le bord sombre autour de chaque lettre du menu. {theme} suit ce que veut le thème, {on} le dessine toujours, {off} l'enlève et laisse voir le fond. Déplace la barre pour essayer : ce texte change aussitôt, [A] le garde pour le menu.",
   'Il bordo scuro attorno a ogni lettera del menu. {theme} segue il tema, {on} lo disegna sempre, {off} lo toglie e lascia vedere lo sfondo. Sposta la barra per provare: questo testo cambia subito, [A] lo salva per il menu.',
   'Тёмная обводка вокруг каждой буквы меню. {theme}: как задано в теме, {on}: всегда, {off}: без обводки, сквозь буквы виден фон. Двигай полосу, чтобы попробовать: этот текст сразу изменится, а [A] сохранит выбор для меню.',
   'De donkere rand rond elke letter van het menu. {theme} volgt het thema, {on} tekent hem altijd, {off} laat hem weg zodat de achtergrond doorschijnt. Beweeg de balk om te proberen: deze tekst verandert meteen, [A] bewaart het voor het menu.'),
  # 4 text anti-aliasing: theme / on / off
  ('The half-tone step that smooths the curves of each letter. {theme} follows what the theme asks for, {on} always smooths, {off} draws the letters with hard pixels. Move the bar to try one: this text changes at once, and [A] keeps it for the menu.',
   'O meio-tom que suaviza as curvas de cada letra. {theme} segue o que o tema pede, {on} sempre suaviza, {off} desenha as letras com pixels duros. Mova a barra para experimentar: este texto muda na hora, e [A] grava a escolha para o menu.',
   'El medio tono que suaviza las curvas de cada letra. {theme} sigue lo que pide el tema, {on} siempre suaviza, {off} dibuja las letras con píxeles duros. Mueve la barra para probar: este texto cambia al instante y [A] lo guarda para el menú.',
   'Die Halbtonstufe, die die Rundungen jedes Buchstabens glättet. {theme} folgt dem Theme, {on} glättet immer, {off} zeigt harte Pixel. Bewege den Balken zum Testen: dieser Text ändert sich sofort, [A] übernimmt es fürs Menü.',
   'Le demi-ton qui adoucit les courbes de chaque lettre. {theme} suit ce que veut le thème, {on} lisse toujours, {off} dessine les lettres en pixels nets. Déplace la barre pour essayer : ce texte change aussitôt, [A] le garde pour le menu.',
   'Il mezzo tono che ammorbidisce le curve di ogni lettera. {theme} segue il tema, {on} ammorbidisce sempre, {off} disegna le lettere a pixel netti. Sposta la barra per provare: questo testo cambia subito, [A] lo salva per il menu.',
   'Полутон, который сглаживает изгибы каждой буквы. {theme}: как задано в теме, {on}: всегда, {off}: буквы из резких пикселей. Двигай полосу, чтобы попробовать: этот текст сразу изменится, а [A] сохранит выбор для меню.',
   'De halftoon die de rondingen van elke letter gladstrijkt. {theme} volgt het thema, {on} strijkt altijd glad, {off} tekent de letters met harde pixels. Beweeg de balk om te proberen: deze tekst verandert meteen, [A] bewaart het voor het menu.'),
  # 5 box art
  ("Shows the game's box art next to the list as you browse. {large} fills the top corner, {small} takes less room. Covers are .cov files: the Web Manager downloads them for your whole collection.",
   'Mostra a capa do jogo ao lado da lista enquanto você navega. {large} ocupa o canto de cima, {small} ocupa menos espaço. As capas são arquivos .cov: o Web Manager baixa para a coleção inteira.',
   'Muestra la carátula del juego junto a la lista mientras navegas. {large} ocupa la esquina superior, {small} ocupa menos. Son archivos .cov: el Web Manager los descarga para toda tu colección.',
   'Zeigt das Spielcover neben der Liste beim Blättern. {large} füllt die obere Ecke, {small} braucht weniger Platz. Cover sind .cov-Dateien: der Web Manager lädt sie für die ganze Sammlung.',
   'Affiche la jaquette du jeu à côté de la liste pendant la navigation. {large} occupe le coin du haut, {small} prend moins de place. Ce sont des fichiers .cov : le Web Manager les télécharge pour toute ta collection.',
   "Mostra la copertina del gioco accanto alla lista mentre navighi. {large} occupa l'angolo in alto, {small} occupa meno spazio. Sono file .cov: il Web Manager li scarica per tutta la collezione.",
   'Показывает обложку игры рядом со списком. {large} = занимает верхний угол, {small} = меньше места. Обложки это файлы .cov: Web Manager скачает их для всей коллекции.',
   'Toont de hoes van het spel naast de lijst tijdens het bladeren. {large} vult de bovenhoek, {small} neemt minder ruimte. Hoezen zijn .cov-bestanden: de Web Manager haalt ze op voor je hele collectie.'),
  # 6 covers in the Recent / Favorites lists (needs covers on)
  ('The box art of the game under the bar also shows in the Recent and Favorites lists, at the size chosen on the previous card. Only asked when covers are on.',
   'A capa do jogo sob a barra aparece também nas listas de Recentes e Favoritos, no tamanho escolhido no card anterior. Só aparece com as capas ligadas.',
   'La carátula del juego bajo la barra también aparece en las listas de Recientes y Favoritos, con el tamaño elegido antes. Solo con las carátulas activadas.',
   'Das Cover des markierten Spiels erscheint auch in den Listen Letzte und Favoriten, in der zuvor gewählten Größe. Nur mit eingeschalteten Covern.',
   "La jaquette du jeu sous la barre s'affiche aussi dans les listes Récents et Favoris, à la taille choisie avant. Seulement si les jaquettes sont activées.",
   'La copertina del gioco sotto la barra appare anche negli elenchi Recenti e Preferiti, con la misura scelta prima. Solo con le copertine attive.',
   'Обложка игры под курсором видна и в списках недавних и избранных игр, в размере, выбранном раньше. Только если обложки включены.',
   'De hoes van het spel onder de balk verschijnt ook in de lijsten Recent en Favorieten, op de eerder gekozen grootte. Alleen als hoezen aan staan.'),
  # 7 game info card
  ('A card with the cover, screenshot, publisher, year, genre and description before the game starts. {on}: [A] opens it. {ctx}: [A] starts the game and the card is in the [Y] menu. {off}: [A] starts the game. On the card, [Up] and [Down] jump to the previous or next game of the list.',
   'Uma ficha com capa, screenshot, editora, ano, gênero e descrição antes do jogo começar. {on}: [A] abre a ficha. {ctx}: [A] inicia o jogo e a ficha fica no menu do [Y]. {off}: [A] inicia o jogo. Na ficha, [Cima] e [Baixo] pulam para o jogo anterior ou o próximo da lista.',
   'Una ficha con carátula, captura, editora, año, género y descripción antes de jugar. {on}: [A] la abre. {ctx}: [A] inicia el juego y la ficha está en el menú de [Y]. {off}: [A] inicia el juego. En la ficha, [Arriba] y [Abajo] saltan al juego anterior o al siguiente.',
   'Eine Karte mit Cover, Screenshot, Publisher, Jahr, Genre und Beschreibung vor dem Start. {on}: [A] öffnet sie. {ctx}: [A] startet das Spiel, die Karte ist im [Y]-Menü. {off}: [A] startet das Spiel. Auf der Karte springen [Oben] und [Unten] zum vorigen oder nächsten Spiel.',
   "Une fiche avec jaquette, capture, éditeur, année, genre et description avant de jouer. {on} : [A] l'ouvre. {ctx} : [A] lance le jeu et la fiche est dans le menu [Y]. {off} : [A] lance le jeu. Sur la fiche, [Haut] et [Bas] passent au jeu précédent ou suivant.",
   'Una scheda con copertina, schermata, editore, anno, genere e descrizione prima di giocare. {on}: [A] la apre. {ctx}: [A] avvia il gioco e la scheda è nel menu [Y]. {off}: [A] avvia il gioco. Sulla scheda, [Su] e [Giù] passano al gioco precedente o successivo.',
   'Карточка с обложкой, скриншотом, издателем, годом, жанром и описанием перед запуском. {on}: [A] открывает её. {ctx}: [A] запускает игру, карточка в меню [Y]. {off}: [A] сразу запускает игру. В карточке [Вверх] и [Вниз] переходят к предыдущей или следующей игре.',
   'Een kaart met hoes, screenshot, uitgever, jaar, genre en beschrijving voor het spelen. {on}: [A] opent hem. {ctx}: [A] start het spel, de kaart zit in het [Y]-menu. {off}: [A] start het spel. Op de kaart springen [Omhoog] en [Omlaag] naar het vorige of volgende spel.'),
  # 8 the video clip on the game info card (needs the card)
  ('When the game has a video clip (.fmv in /sd2snes/info), the game info card plays it in place of the still screenshot. With {no}, the card keeps the screenshot. Only asked when the game info card is on.',
   'Quando o jogo tem um clipe de vídeo (.fmv em /sd2snes/info), a ficha do jogo toca o clipe no lugar do screenshot parado. Com {no}, a ficha mostra o screenshot. Só aparece com a ficha ligada.',
   'Si el juego tiene un clip de vídeo (.fmv en /sd2snes/info), la ficha lo reproduce en lugar de la captura fija. Con {no}, la ficha muestra la captura. Solo con la ficha activada.',
   'Hat das Spiel einen Videoclip (.fmv in /sd2snes/info), spielt die Infokarte ihn statt des Standbilds ab. Mit {no} bleibt das Standbild. Nur mit eingeschalteter Infokarte.',
   'Si le jeu a un clip vidéo (.fmv dans /sd2snes/info), la fiche le joue à la place de la capture fixe. Avec {no}, la fiche garde la capture. Seulement si la fiche est activée.',
   'Se il gioco ha un video (.fmv in /sd2snes/info), la scheda lo riproduce al posto della schermata fissa. Con {no}, la scheda mostra la schermata. Solo con la scheda attiva.',
   'Если у игры есть видеоролик (.fmv в /sd2snes/info), карточка показывает его вместо скриншота. При {no} остаётся скриншот. Только если карточка включена.',
   'Heeft het spel een videoclip (.fmv in /sd2snes/info), dan speelt de spelinfo die af in plaats van de stilstaande screenshot. Met {no} blijft de screenshot. Alleen als de spelinfo aan staat.'),
  # 9 the clip's soundtrack (needs the video and the card)
  ("The clip's soundtrack (.pcm next to the .fmv) plays with it through the cartridge's MSU-1 audio. With {no}, the clip plays silent. Only asked when the video is on.",
   'A trilha do clipe (.pcm ao lado do .fmv) toca junto pelo áudio MSU-1 do cartucho. Com {no}, o clipe roda mudo. Só aparece com o vídeo ligado.',
   'La banda sonora del clip (.pcm junto al .fmv) suena con él por el audio MSU-1 del cartucho. Con {no}, el clip va mudo. Solo con el vídeo activado.',
   'Der Ton des Clips (.pcm neben der .fmv) läuft über das MSU-1-Audio des Moduls mit. Mit {no} bleibt der Clip stumm. Nur mit eingeschaltetem Video.',
   "La bande-son du clip (.pcm à côté du .fmv) joue avec lui par l'audio MSU-1 de la cartouche. Avec {no}, le clip est muet. Seulement si la vidéo est activée.",
   "La colonna sonora del video (.pcm accanto al .fmv) suona insieme tramite l'audio MSU-1 della cartuccia. Con {no}, il video è muto. Solo con il video attivo.",
   'Звук ролика (.pcm рядом с .fmv) играет вместе с ним через звук MSU-1 картриджа. При {no} ролик идёт без звука. Только если видео включено.',
   'De soundtrack van de clip (.pcm naast de .fmv) speelt mee via de MSU-1-audio van de cartridge. Met {no} speelt de clip zonder geluid. Alleen als de video aan staat.'),
  # 10 menu music
  ('An .spc soundtrack plays in the background while you browse and stops when a game starts. To use your own, put it at /sd2snes/menu.spc. Or press [Y] on any .spc in the list: {setbgm}. {restoremusic} in {browser} goes back to menu.spc.',
   'Uma trilha .spc toca ao fundo enquanto você navega e para quando o jogo começa. Para trocar, ponha sua trilha em /sd2snes/menu.spc. Ou aperte [Y] sobre qualquer .spc da lista: {setbgm}. {restoremusic} em {browser} volta ao menu.spc.',
   'Una pista .spc suena de fondo mientras navegas y se detiene al iniciar un juego. Para cambiarla, pon tu pista en /sd2snes/menu.spc. O pulsa [Y] sobre cualquier .spc de la lista: {setbgm}. {restoremusic} en {browser} vuelve a menu.spc.',
   'Ein .spc-Soundtrack läuft beim Blättern im Hintergrund und stoppt beim Spielstart. Eigenen Titel als /sd2snes/menu.spc ablegen. Oder [Y] auf einer .spc in der Liste: {setbgm}. {restoremusic} in {browser} kehrt zu menu.spc zurück.',
   "Une musique .spc joue en fond pendant la navigation et s'arrête au lancement d'un jeu. Pour la changer, mets ta piste dans /sd2snes/menu.spc. Ou appuie sur [Y] sur un .spc de la liste : {setbgm}. {restoremusic} dans {browser} revient à menu.spc.",
   "Una traccia .spc suona in sottofondo mentre navighi e si ferma all'avvio del gioco. Per cambiarla, metti la tua traccia in /sd2snes/menu.spc. Oppure premi [Y] su un .spc della lista: {setbgm}. {restoremusic} in {browser} torna a menu.spc.",
   'Музыка .spc играет фоном, пока ты листаешь меню, и стихает при запуске игры. Назови свой трек menu.spc и положи в папку /sd2snes. Или нажми [Y] на любом .spc в списке: {setbgm}. {restoremusic} в {browser} возвращает menu.spc.',
   'Een .spc-soundtrack speelt op de achtergrond tijdens het bladeren en stopt als een spel start. Eigen nummer: zet het in /sd2snes/menu.spc. Of druk [Y] op een .spc in de lijst: {setbgm}. {restoremusic} in {browser} gaat terug naar menu.spc.'),
  # 11 random music
  ('Each time the menu starts, a different track is drawn from the /sd2snes/music folder. Put as many .spc files there as you like to build your playlist.',
   'A cada vez que o menu abre, uma trilha diferente é sorteada da pasta /sd2snes/music. Coloque lá quantos .spc quiser para montar sua playlist.',
   'Cada vez que se abre el menú, se elige una pista distinta de la carpeta /sd2snes/music. Pon ahí todos los .spc que quieras para tu lista.',
   'Bei jedem Menüstart wird ein anderer Titel aus dem Ordner /sd2snes/music gewählt. Lege dort beliebig viele .spc-Dateien als Playlist ab.',
   'À chaque ouverture du menu, une piste différente est tirée du dossier /sd2snes/music. Mets-y autant de .spc que tu veux pour ta playlist.',
   'A ogni avvio del menu viene scelta una traccia diversa dalla cartella /sd2snes/music. Mettici tutti gli .spc che vuoi per la tua playlist.',
   'При каждом запуске меню играет случайный трек из папки /sd2snes/music. Положи туда сколько угодно файлов .spc.',
   'Bij elke start van het menu wordt een ander nummer uit de map /sd2snes/music gekozen. Zet er zoveel .spc-bestanden in als je wilt.'),
  # 12 menu sounds
  ("Short sound effects when you move the cursor, confirm, go back or hit an error, played through the cartridge's MSU-1 audio. Make your own set in the Sound Creator on the website.",
   'Efeitos curtos ao mover o cursor, confirmar, voltar ou dar erro, tocados pelo áudio MSU-1 do cartucho. Crie o seu conjunto no Criador de Sons do site.',
   'Efectos cortos al mover el cursor, confirmar, volver o al haber un error, por el audio MSU-1 del cartucho. Crea los tuyos en el Sound Creator de la web.',
   'Kurze Effekte beim Bewegen des Cursors, Bestätigen, Zurückgehen und bei Fehlern, über das MSU-1-Audio des Moduls. Eigene im Sound Creator auf der Website.',
   "Des effets courts quand tu bouges le curseur, valides, reviens ou en cas d'erreur, joués par l'audio MSU-1 de la cartouche. Crée les tiens dans le Sound Creator du site.",
   "Brevi effetti quando muovi il cursore, confermi, torni indietro o c'è un errore, dall'audio MSU-1 della cartuccia. Crea i tuoi con il Sound Creator sul sito.",
   'Короткие звуки при движении курсора, выборе, возврате и ошибке, через звук MSU-1 картриджа. Создай свои в Sound Creator на сайте.',
   'Korte geluiden bij cursor bewegen, bevestigen, terug en fouten, via de MSU-1-audio van de cartridge. Maak je eigen set in de Sound Creator op de website.'),
  # 13 MSU-1 folders
  ('A folder with one game and its MSU-1 audio (.msu and .pcm tracks) acts as the game itself: [A] on the folder starts it and its cover shows in the list. The tracks are listed after the ROM.',
   'Uma pasta com um jogo e o áudio MSU-1 dele (.msu e faixas .pcm) vira o próprio jogo: [A] na pasta inicia o jogo e a capa aparece na lista. As faixas ficam listadas depois da ROM.',
   'Una carpeta con un juego y su audio MSU-1 (.msu y pistas .pcm) actúa como el juego: [A] en la carpeta lo inicia y su carátula sale en la lista. Las pistas van tras la ROM.',
   'Ein Ordner mit einem Spiel und seinem MSU-1-Audio (.msu und .pcm-Tracks) verhält sich wie das Spiel: [A] startet es, das Cover steht in der Liste. Tracks kommen nach der ROM.',
   "Un dossier avec un jeu et son audio MSU-1 (.msu et pistes .pcm) devient le jeu : [A] sur le dossier le lance et sa jaquette s'affiche. Les pistes sont listées après la ROM.",
   'Una cartella con un gioco e il suo audio MSU-1 (.msu e tracce .pcm) diventa il gioco: [A] sulla cartella lo avvia e la copertina appare nella lista. Le tracce vanno dopo la ROM.',
   'Папка с игрой и её аудио MSU-1 (.msu и треки .pcm) запускается как игра: кнопка [A] на папке запускает её, обложка видна в списке. Треки идут после ROM.',
   'Een map met een spel en zijn MSU-1-audio (.msu en .pcm-nummers) gedraagt zich als het spel: [A] op de map start het en de hoes staat in de lijst. Nummers staan na de ROM.'),
  # 14 the .pcm track player of the file list
  ("The .pcm tracks of MSU-1 games show up in the file list. [A] on one opens a player with a progress bar and the elapsed and total time, [A] pauses, [B] closes. Handy to check a game's soundtrack before playing.",
   'As trilhas .pcm dos jogos MSU-1 aparecem na lista de arquivos. [A] numa delas abre um tocador com barra de progresso e tempo decorrido e total, [A] pausa, [B] fecha. Bom para conferir a trilha antes de jogar.',
   'Las pistas .pcm de los juegos MSU-1 aparecen en la lista. [A] en una abre un reproductor con barra de progreso y tiempo transcurrido y total, [A] pausa, [B] cierra. Útil para revisar la banda sonora antes de jugar.',
   'Die .pcm-Tracks von MSU-1-Spielen stehen in der Dateiliste. [A] auf einem öffnet einen Player mit Fortschrittsbalken und Zeitanzeige, [A] pausiert, [B] schließt. Praktisch, um den Soundtrack vorher zu prüfen.',
   "Les pistes .pcm des jeux MSU-1 apparaissent dans la liste. [A] sur l'une ouvre un lecteur avec barre de progression et temps écoulé et total, [A] met en pause, [B] ferme. Pratique pour vérifier la bande-son.",
   'Le tracce .pcm dei giochi MSU-1 compaiono nella lista. [A] su una apre un lettore con barra di avanzamento e tempo trascorso e totale, [A] mette in pausa, [B] chiude. Utile per controllare la colonna sonora.',
   'Треки .pcm игр MSU-1 видны в списке файлов. Кнопка [A] на треке открывает плеер с полосой прогресса и временем. В плеере кнопка [A] - пауза, а кнопка [B] - закрытие. Удобно проверить саундтрек перед игрой.',
   'De .pcm-nummers van MSU-1-spellen staan in de bestandslijst. [A] op een nummer opent een speler met voortgangsbalk en tijd, [A] pauzeert, [B] sluit. Handig om de soundtrack vooraf te checken.'),
  # 15 show the sd2snes folder in the file list
  ('Lists the hidden /sd2snes folder in the file list: look at saves, states and info files, apply a theme from it, or delete data you no longer need with [Y].',
   'Mostra a pasta oculta /sd2snes na lista de arquivos, para ver saves, states e fichas, aplicar um tema que esteja lá ou apagar com [Y] dados que não servem mais.',
   'Muestra la carpeta oculta /sd2snes en la lista, para ver saves, states y fichas, aplicar un tema que esté allí o borrar con [Y] datos que ya no sirven.',
   'Zeigt den versteckten Ordner /sd2snes in der Dateiliste: Saves, States und Infodateien ansehen, ein Theme von dort anwenden oder alte Daten mit [Y] löschen.',
   "Affiche le dossier caché /sd2snes dans la liste, pour voir saves, states et fiches, appliquer un thème qui s'y trouve ou effacer avec [Y] les données inutiles.",
   "Mostra la cartella nascosta /sd2snes nella lista, per vedere save, state e schede, applicare un tema che c'è dentro o cancellare con [Y] i dati che non servono più.",
   'Показывает скрытую папку /sd2snes в списке файлов: можно смотреть сохранения, сейвстейты и карточки, применить тему оттуда или удалить кнопкой [Y] ненужные данные.',
   'Toont de verborgen map /sd2snes in de bestandslijst: saves, states en spelinfo bekijken, een thema daaruit toepassen of oude gegevens wissen met [Y].'),
  # 16 smart reset
  ("What the console's [RESET] button does during a game. {off}: resets the game. {menu}: back to the menu. {folder}: to the game's folder. {rom}: the folder with the game selected. {hold}: a tap resets, holding goes back. The [L]+[R]+[Select]+[X] shortcut/hook back to the menu follows {folder}, {rom} and {hold} too.",
   'O que o botão [RESET] do console faz durante o jogo. {off}: reinicia o jogo. {menu}: volta ao menu. {folder}: vai para a pasta do jogo. {rom}: a pasta com o jogo selecionado. {hold}: toque reinicia, segurar volta. O atalho/hook [L]+[R]+[Select]+[X], que volta ao menu, segue {folder}, {rom} e {hold} também.',
   'Qué hace el botón [RESET] de la consola en un juego. {off}: reinicia el juego. {menu}: vuelve al menú. {folder}: a la carpeta del juego. {rom}: la carpeta con el juego elegido. {hold}: tocar reinicia, mantener vuelve. El atajo/hook [L]+[R]+[Select]+[X], que vuelve al menú, también sigue {folder}, {rom} y {hold}.',
   'Was die [RESET]-Taste im Spiel macht. {off}: startet das Spiel neu. {menu}: zurück ins Menü. {folder}: in den Ordner des Spiels. {rom}: der Ordner, Spiel markiert. {hold}: Tippen startet neu, Halten geht zurück. Das Kürzel/Hook [L]+[R]+[Select]+[X] zurück ins Menü folgt auch {folder}, {rom} und {hold}.',
   'Ce que fait le bouton [RESET] de la console en jeu. {off} : relance le jeu. {menu} : retour au menu. {folder} : au dossier du jeu. {rom} : le dossier, jeu sélectionné. {hold} : appui court relance, long revient. Le raccourci/hook [L]+[R]+[Select]+[X] de retour au menu suit aussi {folder}, {rom} et {hold}.',
   'Cosa fa il tasto [RESET] della console in gioco. {off}: riavvia il gioco. {menu}: torna al menu. {folder}: alla cartella del gioco. {rom}: la cartella col gioco scelto. {hold}: tocco riavvia, tenuto torna. La scorciatoia/hook [L]+[R]+[Select]+[X] per tornare al menu segue anche {folder}, {rom} e {hold}.',
   'Что делает кнопка [RESET] во время игры. {off}: перезапуск игры. {menu}: выход в меню. {folder}: в папку игры. {rom}: в папку с выбранной игрой. {hold}: короткое нажатие - перезапуск, долгое - выход. Комбинация/перехват [L]+[R]+[Select]+[X] для выхода в меню работает так же в режимах {folder}, {rom} и {hold}.',
   'Wat de [RESET]-knop tijdens een spel doet. {off}: herstart het spel. {menu}: terug naar het menu. {folder}: naar de map van het spel. {rom}: de map met het spel gekozen. {hold}: tik herstart, vasthouden gaat terug. De sneltoets/hook [L]+[R]+[Select]+[X] terug naar het menu volgt ook {folder}, {rom} en {hold}.'),
  # 17 the cheat list: where it opens, the code formats, the pages, the keys
  ("Open it with [Y] on a game in the file list ({cheats}) or in the {cheatstab} tab of the in-game menu. It holds the game's Game Genie and Pro Action Replay codes, page by page: [Left] and [Right] turn the page, a long name scrolls on its row. [A] turns a code on or off, [Y] edits it and [SELECT] adds one.",
   'Abra com [Y] num jogo da lista de arquivos ({cheats}) ou na aba {cheatstab} do menu in-game. Ela traz os códigos Game Genie e Pro Action Replay do jogo, página por página: [Esquerda] e [Direita] trocam a página e um nome longo rola na linha. [A] liga ou desliga um código, [Y] edita e [SELECT] adiciona.',
   'Se abre con [Y] sobre un juego de la lista ({cheats}) o en la pestaña {cheatstab} del menú del juego. Tiene los códigos Game Genie y Pro Action Replay del juego por páginas: [Izquierda] y [Derecha] pasan de página y un nombre largo se desplaza. [A] activa o desactiva un código, [Y] lo edita y [SELECT] añade uno.',
   'Öffne sie mit [Y] auf einem Spiel in der Dateiliste ({cheats}) oder im Tab {cheatstab} des Ingame-Menüs. Sie zeigt die Game-Genie- und Pro-Action-Replay-Codes seitenweise: [Links] und [Rechts] blättern, ein langer Name läuft durch. [A] schaltet einen Code an oder aus, [Y] bearbeitet ihn, [SELECT] fügt einen hinzu.',
   "Ouvre-la avec [Y] sur un jeu de la liste ({cheats}) ou dans l'onglet {cheatstab} du menu en jeu. Elle montre les codes Game Genie et Pro Action Replay du jeu, page par page : [Gauche] et [Droite] tournent la page, un nom long défile. [A] active ou coupe un code, [Y] le modifie et [SELECT] en ajoute un.",
   'Si apre con [Y] su un gioco della lista dei file ({cheats}) o nella scheda {cheatstab} del menu in gioco. Mostra i codici Game Genie e Pro Action Replay del gioco, pagina per pagina: [Sinistra] e [Destra] cambiano pagina, un nome lungo scorre. [A] attiva o disattiva un codice, [Y] lo modifica e [SELECT] ne aggiunge uno.',
   'Открывается кнопкой [Y] на игре в списке файлов ({cheats}) или на вкладке {cheatstab} меню в игре. Там коды Game Genie и Pro Action Replay по страницам: [Влево] и [Вправо] листают, длинное имя прокручивается. [A] включает или выключает код, [Y] редактирует, [SELECT] добавляет новый.',
   'Open hem met [Y] op een spel in de bestandslijst ({cheats}) of in de tab {cheatstab} van het in-game menu. Hij toont de Game Genie- en Pro Action Replay-codes van het spel per pagina: [Links] en [Rechts] bladeren, een lange naam schuift door. [A] zet een code aan of uit, [Y] bewerkt hem en [SELECT] voegt er een toe.'),
  # 18 in-game menu (a yes turns the in-game hook on too: onboarding_const.a65 ONB_FF_HOOK)
  ('Press [L]+[R]+[Y]+[Left] in a game to pause it and open a menu over it: cheats, savestates, save slots, guides and a RAM trainer. Works with special chips too. {yes} also turns on {hook}: if a game shows graphics glitches, turn it off in {cfg} > {ingame}.',
   'Aperte [L]+[R]+[Y]+[Esquerda] no jogo para pausar e abrir um menu por cima: cheats, savestates, slots de save, guias e treinador de RAM. Funciona também em jogos com chips especiais. {yes} liga também o {hook}: se um jogo der problema gráfico, desligue em {cfg} > {ingame}.',
   'Pulsa [L]+[R]+[Y]+[Izquierda] en el juego para pausarlo y abrir un menú encima: cheats, savestates, ranuras, guías y entrenador de RAM. También en juegos con chips especiales. {yes} activa también el {hook}: si un juego falla en los gráficos, desactívalo en {cfg} > {ingame}.',
   'Drücke [L]+[R]+[Y]+[Links] im Spiel für ein Menü darüber: Cheats, Savestates, Speicherplätze, Anleitungen und RAM-Trainer. Geht auch mit Spezialchips. {yes} schaltet auch {hook} ein: bei Grafikfehlern in einem Spiel in {cfg} > {ingame} ausschalten.',
   'Appuie sur [L]+[R]+[Y]+[Gauche] en jeu pour le mettre en pause sous un menu : cheats, savestates, emplacements, guides et trainer RAM. Marche aussi avec les puces spéciales. {yes} active aussi {hook} : si un jeu a des bugs graphiques, coupe-le dans {cfg} > {ingame}.',
   'Premi [L]+[R]+[Y]+[Sinistra] in gioco per metterlo in pausa con un menu sopra: cheats, savestate, slot, guide e trainer RAM. Anche nei giochi con chip speciali. Con {yes} si attiva anche {hook}: se un gioco ha difetti grafici, disattivalo in {cfg} > {ingame}.',
   'В игре нажми [L]+[R]+[Y]+[Влево], чтобы открыть меню поверх неё: читы, сейвстейты, слоты сохранений, руководства и трейнер памяти. Работает и со специальными чипами. {yes} включает и {hook}: при сбоях графики в игре выключи его в {cfg} > {ingame}.',
   'Druk [L]+[R]+[Y]+[Links] in een spel om te pauzeren met een menu erover: cheats, savestates, opslagplekken, gidsen en RAM-trainer. Werkt ook met speciale chips. {yes} zet ook {hook} aan: bij grafische fouten in een spel zet je het uit in {cfg} > {ingame}.'),
  # 19 savestates
  ('Save the whole game at any moment and load it back later, in 4 slots per game, even in games with special chips. In a game, [Start]+[R] saves and [Start]+[L] loads. Pick the slot in the in-game menu. {yes} also turns on {hook}.',
   'Salve o jogo inteiro a qualquer momento e volte depois, em 4 slots por jogo, até em jogos com chips especiais. No jogo, [Start]+[R] salva e [Start]+[L] carrega. O slot se escolhe no menu in-game. {yes} liga também o {hook}.',
   'Guarda el juego entero en cualquier momento y vuelve luego, en 4 ranuras por juego, incluso con chips especiales. En el juego, [Start]+[R] guarda y [Start]+[L] carga. La ranura se elige en el menú del juego. {yes} activa también el {hook}.',
   'Speichere das ganze Spiel jederzeit und lade es später, in 4 Plätzen pro Spiel, auch mit Spezialchips. Im Spiel sichert [Start]+[R], [Start]+[L] lädt. Den Platz wählst du im Ingame-Menü. {yes} schaltet auch {hook} ein.',
   "Sauvegarde tout le jeu à tout moment et reviens-y plus tard, 4 emplacements par jeu, même avec les puces spéciales. En jeu, [Start]+[R] sauve et [Start]+[L] charge. L'emplacement se choisit dans le menu en jeu. {yes} active aussi {hook}.",
   "Salva l'intero gioco in qualsiasi momento e riprendilo dopo, 4 slot per gioco, anche con i chip speciali. In gioco [Start]+[R] salva e [Start]+[L] carica. Lo slot si sceglie nel menu in gioco. Con {yes} si attiva anche {hook}.",
   'Сохраняй игровой процесс в любой момент и продолжай позже, 4 слота на игру, даже со специальными чипами. В игре [Start]+[R] сохраняет, [Start]+[L] загружает. Слот выбирается в меню в игре. {yes} включает и {hook}.',
   'Sla het hele spel op elk moment op en laad het later, in 4 plekken per spel, ook met speciale chips. In het spel slaat [Start]+[R] op en laadt [Start]+[L]. Kies de plek in het in-game menu. {yes} zet ook {hook} aan.'),
  # 20 4 battery saves per game, the in-game menu's SAVES tab (needs the in-game menu)
  ('Every game has 4 battery-save slots, so two people can each keep their own. Pick the slot in the {saves} tab of the in-game menu: it applies on the next boot of the game.',
   'Cada jogo tem 4 slots de save de bateria, para duas pessoas terem cada uma o seu. O slot se escolhe na aba {saves} do menu in-game e vale no próximo boot do jogo.',
   'Cada juego tiene 4 ranuras de guardado de batería, para que dos personas tengan cada una la suya. La ranura se elige en la pestaña {saves} del menú del juego y vale en el próximo arranque.',
   'Jedes Spiel hat 4 Batterie-Speicherplätze, damit zwei Leute je ihren eigenen haben. Den Platz wählst du im Tab {saves} des Ingame-Menüs, er gilt beim nächsten Spielstart.',
   "Chaque jeu a 4 emplacements de sauvegarde, pour que deux personnes aient chacune la sienne. L'emplacement se choisit dans l'onglet {saves} du menu en jeu et vaut au prochain démarrage.",
   'Ogni gioco ha 4 slot di salvataggio a batteria, così due persone hanno ognuna il suo. Lo slot si sceglie nella scheda {saves} del menu in gioco e vale al prossimo avvio.',
   'У каждой игры 4 слота сохранения с батарейкой, чтобы у двух человек было своё. Слот выбирается на вкладке {saves} меню в игре и действует со следующего запуска.',
   'Elk spel heeft 4 batterij-opslagplekken, zodat twee mensen elk hun eigen hebben. Kies de plek in de tab {saves} van het in-game menu, hij geldt bij de volgende start.'),
  # 21 the in-game menu's RAM trainer (needs the in-game menu)
  ("The {trainer} tab of the in-game menu finds a value in RAM (lives, time, coins): search the number on screen, or {unknown}, and filter as it changes: {changed}, {increased}, {decreased}. Then write the value or freeze it, up to 4 at once, with no cheat made. {savecheat} adds the address to the cheat list.",
   'A aba {trainer} do menu in-game acha um valor na RAM (vidas, tempo, moedas): busque o número da tela, ou {unknown}, e filtre conforme muda: {changed}, {increased}, {decreased}. Depois grave o valor ou congele, até 4 de uma vez, sem criar cheat. {savecheat} põe o endereço na lista de cheats.',
   'La pestaña {trainer} del menú del juego halla un valor en la RAM (vidas, tiempo, monedas): busca el número de la pantalla, o {unknown}, y filtra según cambia: {changed}, {increased}, {decreased}. Luego escribe el valor o congélalo, hasta 4 a la vez, sin crear cheat. {savecheat} lo pone en la lista de cheats.',
   'Der Tab {trainer} im Ingame-Menü findet einen Wert im RAM (Leben, Zeit, Münzen): suche die Zahl vom Bildschirm oder {unknown} und filtere, wenn sie sich ändert: {changed}, {increased}, {decreased}. Dann Wert setzen oder einfrieren, bis zu 4 zugleich, ohne Cheat. {savecheat} legt die Adresse in die Cheat-Liste.',
   "L'onglet {trainer} du menu en jeu trouve une valeur en RAM (vies, temps, pièces) : cherche le nombre affiché, ou {unknown}, puis filtre quand il change : {changed}, {increased}, {decreased}. Écris ensuite la valeur ou fige-la, 4 au plus, sans créer de cheat. {savecheat} la met dans la liste des cheats.",
   "La scheda {trainer} del menu in gioco trova un valore nella RAM (vite, tempo, monete): cerca il numero sullo schermo, o {unknown}, e filtra quando cambia: {changed}, {increased}, {decreased}. Poi scrivi il valore o bloccalo, fino a 4 insieme, senza creare cheat. {savecheat} lo mette nella lista dei cheat.",
   'Вкладка {trainer} меню в игре находит значение в RAM (жизни, время, монеты): ищи число с экрана или {unknown}, затем фильтруй по изменению: {changed}, {increased}, {decreased}. Потом запиши значение или заморозь, до 4 сразу, без создания чита. {savecheat} добавляет адрес в список читов.',
   'De tab {trainer} van het in-game menu vindt een waarde in het RAM (levens, tijd, munten): zoek het getal op het scherm, of {unknown}, en filter als het verandert: {changed}, {increased}, {decreased}. Stel dan de waarde in of zet hem vast, tot 4 tegelijk, zonder cheat. {savecheat} zet het adres in de cheatlijst.'),
  # 22 IPS/BPS patches, and the header mode of each patch ([Y] in the patch list)
  ('IPS and BPS patches next to a ROM bring translations, hacks and fixes without changing the original: [A] on the game asks which one to apply, or none. In that list, [Y] on a patch sets its {hdrmode} (the 512-byte header): {hdrauto}, {hdron} or {hdroff}, remembered per patch.',
   'Patches IPS e BPS ao lado da ROM trazem traduções, hacks e correções sem mudar o original: [A] no jogo pergunta qual aplicar, ou nenhum. Nessa lista, [Y] num patch escolhe o {hdrmode} (header de 512 bytes): {hdrauto}, {hdron} ou {hdroff}, lembrado por patch.',
   'Los parches IPS y BPS junto a una ROM traen traducciones, hacks y arreglos sin cambiar el original: [A] en el juego pregunta cuál aplicar, o ninguno. Ahí, [Y] en un parche elige el {hdrmode} (cabecera de 512 bytes): {hdrauto}, {hdron} o {hdroff}, recordado por parche.',
   'IPS- und BPS-Patches neben einer ROM bringen Übersetzungen, Hacks und Fixes, ohne sie zu ändern: [A] auf dem Spiel fragt, welcher. Dort wählt [Y] auf einem Patch den {hdrmode} (512-Byte-Header): {hdrauto}, {hdron} oder {hdroff}, pro Patch gemerkt.',
   "Les patchs IPS et BPS à côté d'une ROM apportent traductions, hacks et correctifs sans toucher l'original : [A] sur le jeu demande lequel, ou aucun. Là, [Y] sur un patch règle son {hdrmode} (en-tête de 512 octets) : {hdrauto}, {hdron} ou {hdroff}, mémorisé par patch.",
   "Le patch IPS e BPS accanto a una ROM portano traduzioni, hack e correzioni senza toccare l'originale: [A] sul gioco chiede quale applicare, o nessuna. Lì, [Y] su una patch sceglie il {hdrmode} (header di 512 byte): {hdrauto}, {hdron} o {hdroff}, ricordato per patch.",
   'Патчи IPS и BPS рядом с ROM дают переводы, хаки и исправления без изменения оригинала: [A] на игре спросит, какой применить, или никакой. Там [Y] на патче задаёт {hdrmode} (заголовок 512 байт): {hdrauto}, {hdron} или {hdroff}, для каждого патча.',
   'IPS- en BPS-patches naast een ROM brengen vertalingen, hacks en fixes zonder het origineel te wijzigen: [A] op het spel vraagt welke, of geen. Daar kiest [Y] op een patch de {hdrmode} (512-byte header): {hdrauto}, {hdron} of {hdroff}, per patch onthouden.'),
  # 23 creating the patched ROM from the [Y] menu of a patch
  ('In the patch list, [Y] on a patch and {createrom} save a copy of the ROM with the patch already applied, next to the original. Its cover, info card, guides, cheats, saves and states go along with it.',
   'Na lista de patches, [Y] sobre um patch e {createrom} gravam ao lado da original uma cópia da ROM já com o patch. A capa, a ficha, as guias, os cheats, os saves e os states vão junto.',
   'En la lista de parches, [Y] sobre un parche y {createrom} guardan junto a la original una copia de la ROM ya parcheada. Su carátula, ficha, guías, cheats, saves y states van con ella.',
   'In der Patch-Liste legen [Y] auf einem Patch und {createrom} neben dem Original eine Kopie der ROM mit dem Patch an. Cover, Infokarte, Anleitungen, Cheats, Saves und States kommen mit.',
   "Dans la liste des patchs, [Y] sur un patch puis {createrom} enregistre à côté de l'original une copie de la ROM déjà patchée. Jaquette, fiche, guides, cheats, saves et states suivent.",
   "Nell'elenco delle patch, [Y] su una patch e poi {createrom} salva accanto all'originale una copia della ROM già patchata. Copertina, scheda, guide, cheat, save e state la seguono.",
   'В списке патчей [Y] на патче и пункт {createrom} сохраняют рядом с оригиналом копию ROM уже с патчем. Обложка, карточка, руководства, читы и сохранения переносятся вместе с ней.',
   'In de patchlijst zetten [Y] op een patch en {createrom} naast het origineel een kopie van de ROM met de patch erin. Hoes, spelinfo, gidsen, cheats, saves en states gaan mee.'),
  # 24 more special chips (its own card)
  ('Besides DSP, Super FX, SA-1, S-DD1 and CX4, more special chips run: the SPC7110 with its clock, saved on the card, Super FX 3, the Sufami Turbo adapter and the competition carts Campus Challenge 92 and PowerFest 94. Some need a BIOS file in /sd2snes.',
   'Além de DSP, Super FX, SA-1, S-DD1 e CX4, mais chips especiais rodam: o SPC7110 com o relógio, salvo no cartão, o Super FX 3, o adaptador Sufami Turbo e os cartuchos de competição Campus Challenge 92 e PowerFest 94. Alguns pedem um arquivo de BIOS em /sd2snes.',
   'Además de DSP, Super FX, SA-1, S-DD1 y CX4, funcionan más chips especiales: el SPC7110 con su reloj, guardado en la tarjeta, el Super FX 3, el adaptador Sufami Turbo y los cartuchos de competición Campus Challenge 92 y PowerFest 94. Algunos piden un BIOS en /sd2snes.',
   'Neben DSP, Super FX, SA-1, S-DD1 und CX4 laufen weitere Spezialchips: der SPC7110 mit Uhr, auf der Karte gespeichert, Super FX 3, der Sufami-Turbo-Adapter und die Turniermodule Campus Challenge 92 und PowerFest 94. Manche brauchen ein BIOS in /sd2snes.',
   "En plus des DSP, Super FX, SA-1, S-DD1 et CX4, d'autres puces spéciales marchent : le SPC7110 avec son horloge, gardée sur la carte, le Super FX 3, l'adaptateur Sufami Turbo et les cartouches de tournoi Campus Challenge 92 et PowerFest 94. Certaines demandent un BIOS dans /sd2snes.",
   "Oltre a DSP, Super FX, SA-1, S-DD1 e CX4 funzionano altri chip speciali: lo SPC7110 con il suo orologio, salvato sulla scheda, il Super FX 3, l'adattatore Sufami Turbo e le cartucce da torneo Campus Challenge 92 e PowerFest 94. Alcuni vogliono un BIOS in /sd2snes.",
   'Кроме DSP, Super FX, SA-1, S-DD1 и CX4 работают и другие специальные чипы: SPC7110 с часами, которые хранятся на карте, Super FX 3, адаптер Sufami Turbo и турнирные картриджи Campus Challenge 92 и PowerFest 94. Некоторым нужен BIOS в /sd2snes.',
   'Naast DSP, Super FX, SA-1, S-DD1 en CX4 werken meer speciale chips: de SPC7110 met zijn klok, bewaard op de kaart, Super FX 3, de Sufami Turbo-adapter en de toernooicartridges Campus Challenge 92 en PowerFest 94. Sommige hebben een BIOS in /sd2snes nodig.'),
  # 25 Sufami Turbo: the Slot B picker
  ("Starting a Sufami Turbo cart (.st) asks which minicart goes in Slot B: the other .st files of the same folder, or none. The pair is remembered for next time. Needs the adapter's BIOS as /sd2snes/stbios.bin.",
   'Ao iniciar um cartucho Sufami Turbo (.st), o menu pergunta qual minicartucho vai no Slot B: os outros .st da mesma pasta, ou nenhum. O par fica lembrado para a próxima vez. Precisa do BIOS do adaptador em /sd2snes/stbios.bin.',
   'Al iniciar un cartucho Sufami Turbo (.st), el menú pregunta qué minicartucho va en la ranura B: los otros .st de la misma carpeta, o ninguno. El par se recuerda para la próxima vez. Necesita la BIOS del adaptador en /sd2snes/stbios.bin.',
   'Beim Start eines Sufami-Turbo-Moduls (.st) fragt das Menü, welches Minimodul in Slot B kommt: die anderen .st im selben Ordner oder keins. Das Paar wird gemerkt. Braucht das BIOS des Adapters als /sd2snes/stbios.bin.',
   "Au lancement d'une cartouche Sufami Turbo (.st), le menu demande quelle minicartouche va dans le port B : les autres .st du même dossier, ou aucune. La paire est mémorisée. Il faut le BIOS de l'adaptateur en /sd2snes/stbios.bin.",
   "All'avvio di una cartuccia Sufami Turbo (.st), il menu chiede quale minicartuccia va nello Slot B: gli altri .st della stessa cartella, o nessuna. La coppia viene ricordata. Serve il BIOS dell'adattatore in /sd2snes/stbios.bin.",
   'При запуске картриджа Sufami Turbo (.st) меню спрашивает, какой мини-картридж вставить в слот B: другие .st из той же папки или никакой. Пара запоминается. Нужен BIOS адаптера в /sd2snes/stbios.bin.',
   'Bij het starten van een Sufami Turbo-cartridge (.st) vraagt het menu welke minicartridge in slot B gaat: de andere .st in dezelfde map, of geen. Het paar wordt onthouden. De BIOS van de adapter moet in /sd2snes/stbios.bin.'),
  # 26 Competition Carts: the round's minutes (3..18, the events used 6)
  ('The Campus Challenge 92 and PowerFest 94 carts play a timed round, like at the original events. Choose how long it lasts, from 3 to 18 minutes: the events used 6. Also in {chipopts}.',
   'Os cartuchos Campus Challenge 92 e PowerFest 94 jogam um round cronometrado, como nos eventos originais. Escolha quanto tempo ele dura, de 3 a 18 minutos: os eventos usavam 6. Também em {chipopts}.',
   'Los cartuchos Campus Challenge 92 y PowerFest 94 juegan una ronda cronometrada, como en los eventos originales. Elige cuánto dura, de 3 a 18 minutos: los eventos usaban 6. También en {chipopts}.',
   'Die Module Campus Challenge 92 und PowerFest 94 spielen eine Runde auf Zeit, wie bei den echten Turnieren. Wähle die Dauer, 3 bis 18 Minuten: die Turniere nutzten 6. Auch unter {chipopts}.',
   "Les cartouches Campus Challenge 92 et PowerFest 94 jouent une manche chronométrée, comme aux tournois d'origine. Choisis sa durée, de 3 à 18 minutes : les tournois utilisaient 6. Aussi dans {chipopts}.",
   'Le cartucce Campus Challenge 92 e PowerFest 94 giocano un round a tempo, come ai tornei originali. Scegli quanto dura, da 3 a 18 minuti: i tornei usavano 6. Anche in {chipopts}.',
   'Картриджи Campus Challenge 92 и PowerFest 94 играют раунд на время, как на настоящих турнирах. Выбери его длину, от 3 до 18 минут: на турнирах было 6. Также в {chipopts}.',
   'De cartridges Campus Challenge 92 en PowerFest 94 spelen een ronde op tijd, zoals op de echte toernooien. Kies hoe lang die duurt, 3 tot 18 minuten: de toernooien gebruikten 6. Ook in {chipopts}.'),
  # 27 other consoles: NES, Master System, Game Boy Color, Atari 2600 (Mk.III only, experimental)
  ('NES, Master System, Game Boy Color and Atari 2600 games run on their own FPGA cores, FXPAK PRO (Mk.III) only, and open like any ROM. These cores are experimental: some games may glitch or not run. On the NES, [L]+[R]+[Start]+[Up] repaints the palette if the colours look wrong.',
   'Jogos de NES, Master System, Game Boy Color e Atari 2600 rodam em cores de FPGA próprios, só no FXPAK PRO (Mk.III), e abrem como qualquer ROM. Esses cores são experimentais: alguns jogos podem ter falhas ou não rodar. No NES, [L]+[R]+[Start]+[Cima] repinta a paleta se as cores saírem erradas.',
   'Los juegos de NES, Master System, Game Boy Color y Atari 2600 corren en sus propios cores FPGA, solo en FXPAK PRO (Mk.III), y abren como cualquier ROM. Estos cores son experimentales: algunos juegos pueden fallar o no arrancar. En la NES, [L]+[R]+[Start]+[Arriba] repinta la paleta si los colores salen mal.',
   'NES-, Master-System-, Game-Boy-Color- und Atari-2600-Spiele laufen auf eigenen FPGA-Cores, nur auf FXPAK PRO (Mk.III). Die Cores sind experimentell: manche Spiele haben Fehler oder laufen nicht. Beim NES malt [L]+[R]+[Start]+[Oben] die Palette neu, wenn Farben falsch sind.',
   "Les jeux NES, Master System, Game Boy Color et Atari 2600 tournent sur leurs propres cores FPGA, sur FXPAK PRO (Mk.III) seulement. Ces cores sont expérimentaux : certains jeux peuvent bugger ou ne pas démarrer. Sur NES, [L]+[R]+[Start]+[Haut] redessine la palette si les couleurs sont fausses.",
   'I giochi NES, Master System, Game Boy Color e Atari 2600 girano su core FPGA dedicati, solo su FXPAK PRO (Mk.III), e si aprono come una ROM. Questi core sono sperimentali: alcuni giochi possono avere difetti o non partire. Sul NES, [L]+[R]+[Start]+[Su] ridisegna la tavolozza se i colori sono sbagliati.',
   'Игры NES, Master System, Game Boy Color и Atari 2600 идут на своих ядрах FPGA, только на FXPAK PRO (Mk.III), и открываются как любой ROM. Эти ядра экспериментальные: некоторые игры могут сбоить или не запускаться. На NES [L]+[R]+[Start]+[Вверх] перерисовывает палитру, если цвета неверные.',
   'NES-, Master System-, Game Boy Color- en Atari 2600-spellen draaien op eigen FPGA-cores, alleen op FXPAK PRO (Mk.III), en openen als elke ROM. Deze cores zijn experimenteel: sommige spellen haperen of starten niet. Op de NES tekent [L]+[R]+[Start]+[Omhoog] het palet opnieuw als de kleuren fout zijn.'),
  # 28 the Atari 2600's console switches on the pad, and its picture width
  ("The console's switches are on controller 1: [Start] is RESET and [Select] is SELECT while held, [X] flips Color and B/W, [L] and [R] flip the two difficulty switches. {a26w} in {chipopts} picks 160 px (1:1) or 256 px (wide), FXPAK PRO (Mk.III) only.",
   'As chaves do console ficam no controle 1: [Start] é o RESET e [Select] o SELECT enquanto segurados, [X] alterna Cor e P/B, [L] e [R] alternam as duas chaves de dificuldade. {a26w} em {chipopts} escolhe 160 px (1:1) ou 256 px (esticado), só no FXPAK PRO (Mk.III).',
   'Los interruptores de la consola están en el mando 1: [Start] es RESET y [Select] es SELECT mientras se mantienen, [X] cambia Color y B/N, [L] y [R] los dos de dificultad. {a26w} en {chipopts} elige 160 px (1:1) o 256 px (ancho), solo en FXPAK PRO (Mk.III).',
   'Die Schalter der Konsole liegen auf Controller 1: [Start] ist RESET und [Select] SELECT, solange gehalten, [X] wechselt Farbe und S/W, [L] und [R] die zwei Schwierigkeitsschalter. {a26w} in {chipopts}: 160 px (1:1) oder 256 px (breit), nur FXPAK PRO (Mk.III).',
   "Sur la manette 1, [Start] est RESET et [Select] SELECT tant qu'ils sont tenus, [X] bascule Couleur et N/B, [L] et [R] les deux interrupteurs de difficulté. {a26w} ({chipopts}) choisit 160 px (1:1) ou 256 px (large), FXPAK PRO (Mk.III) seulement.",
   'Gli interruttori della console sono sul controller 1: [Start] è RESET e [Select] è SELECT finché tenuti, [X] alterna Colore e B/N, [L] e [R] i due interruttori di difficoltà. {a26w} in {chipopts}: 160 px (1:1) o 256 px (largo), solo FXPAK PRO (Mk.III).',
   'Переключатели консоли на контроллере 1: [Start] это RESET, а [Select] это SELECT, пока зажаты, [X] переключает цвет и Ч/Б, [L] и [R] два переключателя сложности. {a26w} в {chipopts}: 160 px (1:1) или 256 px (широко), только FXPAK PRO (Mk.III).',
   'De schakelaars van de console zitten op controller 1: [Start] is RESET en [Select] SELECT zolang ingedrukt, [X] wisselt Kleur en Z/W, [L] en [R] de twee moeilijkheidsschakelaars. {a26w} in {chipopts}: 160 px (1:1) of 256 px (breed), alleen FXPAK PRO (Mk.III).'),
  # 29 the card's folders: 2-letter buckets, console folders, Organize
  ('Saves, states, cheats, info cards and patch settings of each game live in /sd2snes/<area>/<2 letters>/, plus a console folder (sgb, sft, nes, sms, a26). Updating only the firmware? Run Organize in the Web Manager, or old saves seem to vanish.',
   'Saves, states, cheats, fichas e ajustes de patch de cada jogo ficam em /sd2snes/<área>/<2 letras>/, mais uma pasta por console (sgb, sft, nes, sms, a26). Vai atualizar só a firmware? Rode o Organize do Web Manager, senão os saves antigos parecem sumir.',
   'Saves, states, cheats, fichas y ajustes de parche de cada juego están en /sd2snes/<área>/<2 letras>/, más una carpeta por consola (sgb, sft, nes, sms, a26). ¿Solo actualizas el firmware? Usa Organize en el Web Manager, o los saves viejos parecerán perdidos.',
   'Saves, States, Cheats, Infokarten und Patch-Einstellungen jedes Spiels liegen in /sd2snes/<Bereich>/<2 Buchstaben>/, dazu ein Konsolenordner (sgb, sft, nes, sms, a26). Nur die Firmware aktualisiert? Organize im Web Manager starten, sonst wirken alte Saves verschwunden.',
   'Saves, states, cheats, fiches et réglages de patch de chaque jeu sont dans /sd2snes/<zone>/<2 lettres>/, plus un dossier par console (sgb, sft, nes, sms, a26). Tu mets à jour seulement le firmware ? Lance Organize dans le Web Manager, sinon les anciens saves semblent perdus.',
   'Save, state, cheat, schede e impostazioni delle patch di ogni gioco stanno in /sd2snes/<area>/<2 lettere>/, più una cartella per console (sgb, sft, nes, sms, a26). Aggiorni solo il firmware? Usa Organize nel Web Manager, o i vecchi save sembreranno spariti.',
   'Сохранения, сейвстейты, читы, карточки и настройки патчей каждой игры лежат в /sd2snes/<раздел>/<2 буквы>/, плюс папка консоли (sgb, sft, nes, sms, a26). Обновляешь только прошивку? Запусти Organize в Web Manager, иначе старые сохранения пропадут из виду.',
   'Saves, states, cheats, spelinfo en patch-instellingen van elk spel staan in /sd2snes/<gebied>/<2 letters>/, plus een consolemap (sgb, sft, nes, sms, a26). Werk je alleen de firmware bij? Draai Organize in de Web Manager, anders lijken oude saves weg.'),
  # 30 the memory test of the main menu
  ("{memtest}, in the main menu, checks the cartridge's memory right on the console: [A] runs the full test, the wiring test included, in about 30 seconds. The console resets at the end.",
   '{memtest}, no menu principal, testa a memória do cartucho no próprio console: [A] roda o teste completo, que já inclui o de fiação, em uns 30 segundos. O console reinicia no fim.',
   '{memtest}, en el menú principal, prueba la memoria del cartucho en la consola: [A] hace la prueba completa, que ya incluye la del cableado, en unos 30 segundos. La consola se reinicia al final.',
   '{memtest} im Hauptmenü prüft den Speicher des Moduls direkt an der Konsole: [A] startet den vollen Test, Leitungstest inklusive, etwa 30 Sekunden. Danach startet die Konsole neu.',
   '{memtest}, dans le menu principal, teste la mémoire de la cartouche sur la console : [A] lance le test complet, câblage compris, en 30 secondes environ. La console redémarre à la fin.',
   '{memtest}, nel menu principale, verifica la memoria della cartuccia sulla console: [A] avvia il test completo, cablaggio incluso, in circa 30 secondi. Alla fine la console si riavvia.',
   '{memtest} в главном меню проверяет память картриджа прямо на консоли: [A] запускает полный тест, вместе с тестом линий, около 30 секунд. В конце консоль перезапускается.',
   '{memtest} in het hoofdmenu test het geheugen van de cartridge op de console: [A] start de volledige test, bedrading inbegrepen, in ongeveer 30 seconden. Daarna herstart de console.'),
  # 31 the Mk.II LED codes for a boot without a picture (Mk.II only)
  ('To make room for more features on the Mk.II (SD2SNES), its boot error screen was replaced by blinking LED patterns. What is missing:\nSD card: green/red\nfpga_mini.bit: green/yellow\nThat file goes in /sd2snes.',
   'Para abrir espaço para mais recursos no Mk.II (SD2SNES), a tela de erro do boot virou combinações de LEDs piscando. O que falta:\nCartão SD: verde/vermelho\nfpga_mini.bit: verde/amarelo\nEsse arquivo fica em /sd2snes.',
   'Para hacer sitio a más funciones en el Mk.II (SD2SNES), la pantalla de error de arranque se cambió por combinaciones de LED parpadeando. Lo que falta:\nTarjeta SD: verde/rojo\nfpga_mini.bit: verde/amarillo\nEse archivo va en /sd2snes.',
   'Um auf dem Mk.II (SD2SNES) Platz für mehr Funktionen zu schaffen, wurde der Startfehler-Bildschirm durch blinkende LED-Muster ersetzt. Was fehlt:\nSD-Karte: grün/rot\nfpga_mini.bit: grün/gelb\nDie Datei gehört nach /sd2snes.',
   "Pour faire de la place à plus de fonctions sur le Mk.II (SD2SNES), l'écran d'erreur au démarrage a été remplacé par des combinaisons de LED qui clignotent. Ce qui manque :\nCarte SD : vert/rouge\nfpga_mini.bit : vert/jaune\nCe fichier va dans /sd2snes.",
   "Per fare spazio a più funzioni sul Mk.II (SD2SNES), la schermata di errore all'avvio è stata sostituita da combinazioni di LED lampeggianti. Cosa manca:\nScheda SD: verde/rosso\nfpga_mini.bit: verde/giallo\nIl file va in /sd2snes.",
   'Чтобы освободить место для новых функций на Mk.II (SD2SNES), экран ошибок запуска заменён миганием светодиодов. Чего не хватает:\nSD-карта: зелёный/красный\nfpga_mini.bit: зелёный/жёлтый\nЭтот файл лежит в /sd2snes.',
   'Om op de Mk.II (SD2SNES) ruimte te maken voor meer functies, is het foutscherm bij het opstarten vervangen door knipperende LED-patronen. Wat ontbreekt:\nSD-kaart: groen/rood\nfpga_mini.bit: groen/geel\nDat bestand hoort in /sd2snes.'),
  # 32 "and more" (the base tour's last card)
  ("",) * 8,
  # 33 the 2.17 section: what is new in this release
  ("Version 2.17 adds a Game Boy Color core, FXPAK PRO (Mk.III) only, the ST011 and ST018 special chips, copy-protected bootleg carts, Dutch as the menu's eighth language and menu music that comes with the firmware, a random track by default. The next cards show the rest.",
   'A versão 2.17 traz um core de Game Boy Color, só no FXPAK PRO (Mk.III), os chips especiais ST011 e ST018, cartuchos piratas com proteção, o holandês como oitavo idioma e música de menu que vem com a firmware, aleatória por padrão. Os próximos cards mostram o resto.',
   'La versión 2.17 trae un core de Game Boy Color, solo en FXPAK PRO (Mk.III), los chips especiales ST011 y ST018, cartuchos piratas con protección, el neerlandés como octavo idioma y música de menú incluida en el firmware, aleatoria por defecto. Lo demás, en las próximas pantallas.',
   'Version 2.17 bringt einen Game-Boy-Color-Core, nur für FXPAK PRO (Mk.III), die Spezialchips ST011 und ST018, kopiergeschützte Bootleg-Module, Niederländisch als achte Sprache und Menümusik mit der Firmware, standardmäßig zufällig. Die nächsten Karten zeigen den Rest.',
   'La version 2.17 apporte un core Game Boy Color, sur FXPAK PRO (Mk.III) seulement, les puces spéciales ST011 et ST018, les cartouches pirates protégées, le néerlandais en huitième langue et une musique de menu fournie, aléatoire par défaut. La suite dans les écrans suivants.',
   "La versione 2.17 porta un core Game Boy Color, solo per FXPAK PRO (Mk.III), i chip speciali ST011 e ST018, le cartucce pirata protette, l'olandese come ottava lingua e musica del menu inclusa nel firmware, casuale di default. Le prossime schede mostrano il resto.",
   'В версии 2.17: ядро Game Boy Color, только для FXPAK PRO (Mk.III), специальные чипы ST011 и ST018, пиратские картриджи с защитой, нидерландский как восьмой язык и музыка меню в комплекте с прошивкой, по умолчанию случайная. Дальше остальные новинки.',
   'Versie 2.17 brengt een Game Boy Color-core, alleen voor FXPAK PRO (Mk.III), de speciale chips ST011 en ST018, beveiligde bootleg-cartridges, Nederlands als achtste taal en menumuziek bij de firmware, standaard willekeurig. De volgende kaarten tonen de rest.'),
  # 34 controller 2 (2.17)
  ('The in-game shortcuts/hooks (menu, savestates, back to the menu) also work from controller 2, so the second player can use them too. Controller 1 keeps priority. {yes} also turns on {hook} and {igbuttons}.',
   'Os atalhos/hooks in-game (menu, savestates, voltar ao menu) também funcionam no segundo controle, então o jogador 2 também pode usar. O controle 1 tem prioridade. {yes} liga também o {hook} e os {igbuttons}.',
   'Los atajos/hooks del juego (menú, savestates, volver al menú) también funcionan en el segundo mando, así el otro jugador puede usarlos. El mando 1 tiene prioridad. {yes} activa también el {hook} y los {igbuttons}.',
   'Die Ingame-Kürzel/Hooks (Menü, Savestates, zurück zum Menü) gehen auch an Controller 2, so kann sie auch Spieler 2 nutzen. Controller 1 hat Vorrang. {yes} schaltet auch {hook} und {igbuttons} ein.',
   'Les raccourcis/hooks en jeu (menu, savestates, retour au menu) marchent aussi sur la deuxième manette, pour le joueur 2. La manette 1 reste prioritaire. {yes} active aussi {hook} et {igbuttons}.',
   'Le scorciatoie/hook in gioco (menu, savestate, ritorno al menu) funzionano anche col controller 2, così le usa anche il secondo giocatore. Il controller 1 ha la precedenza. Con {yes} si attivano anche {hook} e {igbuttons}.',
   'Игровые комбинации/перехваты (меню, сейвстейты, выход в меню) работают и на контроллере 2, их может использовать второй игрок. У контроллера 1 приоритет. {yes} включает и {hook}, и {igbuttons}.',
   'De in-game sneltoetsen/hooks (menu, savestates, terug naar menu) werken ook op controller 2, dus ook de tweede speler kan ze gebruiken. Controller 1 heeft voorrang. {yes} zet ook {hook} en {igbuttons} aan.'),
  # 35 Game Boy Color (2.17)
  ('Put a .gbc on the card and it runs on a Game Boy Color core instead of the Super Game Boy, in color and at full speed. {sgbmenu} picks {auto}, {prefsgb} or {prefgbc}. Experimental, FXPAK PRO (Mk.III) only.',
   'Coloque um .gbc no cartão e ele roda num core de Game Boy Color em vez do Super Game Boy, em cores e a toda velocidade. Em {sgbmenu}: {auto}, {prefsgb} ou {prefgbc}. Experimental, só no FXPAK PRO (Mk.III).',
   'Pon un .gbc en la tarjeta y corre en un core de Game Boy Color en vez del Super Game Boy, en color y a toda velocidad. En {sgbmenu}: {auto}, {prefsgb} o {prefgbc}. Experimental, solo en FXPAK PRO (Mk.III).',
   'Lege eine .gbc auf die Karte und sie läuft auf einem Game-Boy-Color-Core statt dem Super Game Boy, in Farbe und mit voller Geschwindigkeit. In {sgbmenu}: {auto}, {prefsgb} oder {prefgbc}. Experimentell, nur FXPAK PRO (Mk.III).',
   'Mets un .gbc sur la carte et il tourne sur un core Game Boy Color au lieu du Super Game Boy, en couleur et à pleine vitesse. Dans {sgbmenu} : {auto}, {prefsgb} ou {prefgbc}. Expérimental, FXPAK PRO (Mk.III) seulement.',
   'Metti un .gbc sulla scheda e gira su un core Game Boy Color invece del Super Game Boy, a colori e a piena velocità. In {sgbmenu}: {auto}, {prefsgb} o {prefgbc}. Sperimentale, solo FXPAK PRO (Mk.III).',
   'Положи .gbc на карту, и он запустится на ядре Game Boy Color вместо Super Game Boy, в цвете и на полной скорости. В {sgbmenu}: {auto}, {prefsgb} или {prefgbc}. Экспериментально, только FXPAK PRO (Mk.III).',
   'Zet een .gbc op de kaart en hij draait op een Game Boy Color-core in plaats van de Super Game Boy, in kleur en op volle snelheid. In {sgbmenu}: {auto}, {prefsgb} of {prefgbc}. Experimenteel, alleen FXPAK PRO (Mk.III).'),
  # 36 the in-game shortcut list (2.17)
  ('In the in-game menu, [SELECT] on the tab bar lists every button shortcut/hook of the game you are playing, with the combos armed for it: open the menu, save and load states, reset, cheats on and off.',
   'No menu in-game, [SELECT] na barra de abas lista todos os atalhos/hooks de botão do jogo que você está jogando, com as combinações ativas para ele: abrir o menu, salvar e carregar estados, reset, ligar e desligar cheats.',
   'En el menú del juego, [SELECT] en la barra de pestañas lista todos los atajos/hooks del juego actual, con las combinaciones activas para él: abrir el menú, guardar y cargar estados, reset, activar y desactivar cheats.',
   'Im Ingame-Menü zeigt [SELECT] auf der Tab-Leiste alle Tastenkürzel/Hooks des laufenden Spiels, mit den dafür aktiven Kombinationen: Menü öffnen, Zustände sichern und laden, Reset, Cheats an und aus.',
   "Dans le menu en jeu, [SELECT] sur la barre d'onglets liste tous les raccourcis/hooks du jeu en cours, avec les combinaisons actives pour lui : ouvrir le menu, sauver et charger, reset, cheats on et off.",
   'Nel menu in gioco, [SELECT] sulla barra delle schede elenca tutte le scorciatoie/hook del gioco in corso, con le combinazioni attive: aprire il menu, salvare e caricare stati, reset, cheat on e off.',
   'В меню в игре [SELECT] на панели вкладок показывает все комбинации/перехваты кнопок для текущей игры: открыть меню, сохранить и загрузить состояние, сброс, включить и выключить читы.',
   'In het in-game menu toont [SELECT] op de tabbalk alle sneltoetsen/hooks voor het huidige spel: menu openen, states opslaan en laden, reset, cheats aan en uit.'),
  # 37 Seta chips and bootlegs (2.17)
  ('Morita Shougi 1 and 2 now run: the Seta ST011 and ST018 special chips need st011.rom and st018.rom in /sd2snes. Copy-protected bootleg carts boot from the untouched dump.',
   'Morita Shougi 1 e 2 agora rodam: os chips especiais Seta ST011 e ST018 precisam de st011.rom e st018.rom em /sd2snes. Bootlegs com proteção contra cópia iniciam do dump original.',
   'Morita Shougi 1 y 2 ya funcionan: los chips especiales Seta ST011 y ST018 necesitan st011.rom y st018.rom en /sd2snes. Los bootlegs con protección anticopia arrancan desde el volcado original.',
   'Morita Shougi 1 und 2 laufen jetzt: die Seta-Spezialchips ST011 und ST018 brauchen st011.rom und st018.rom in /sd2snes. Kopiergeschützte Bootlegs starten vom unveränderten Dump.',
   'Morita Shougi 1 et 2 tournent enfin : les puces spéciales Seta ST011 et ST018 demandent st011.rom et st018.rom dans /sd2snes. Les bootlegs protégés contre la copie démarrent depuis le dump intact.',
   'Morita Shougi 1 e 2 ora funzionano: i chip speciali Seta ST011 e ST018 richiedono st011.rom e st018.rom in /sd2snes. I bootleg con protezione anticopia partono dal dump originale.',
   'Morita Shougi 1 и 2 теперь запускаются: специальным чипам Seta ST011 и ST018 нужны st011.rom и st018.rom в /sd2snes. Бутлеги с защитой от копирования грузятся из нетронутого дампа.',
   'Morita Shougi 1 en 2 werken nu: de speciale Seta-chips ST011 en ST018 hebben st011.rom en st018.rom in /sd2snes nodig. Kopieerbeveiligde bootlegs starten vanaf de originele dump.'),
  # 38 Super 20 in 1, Gamars Puzzle and .sfrom (2.17)
  ('The Super 20 in 1 multicart opens its own game menu, Gamars Puzzle runs, and .sfrom files from the SNES Classic load like any ROM.',
   'O multicart Super 20 in 1 abre o próprio menu de jogos, o Gamars Puzzle roda e arquivos .sfrom do SNES Classic carregam como qualquer ROM.',
   'El multicart Super 20 in 1 abre su propio menú de juegos, Gamars Puzzle funciona y los archivos .sfrom del SNES Classic cargan como cualquier ROM.',
   'Das Multicart Super 20 in 1 öffnet sein eigenes Spielemenü, Gamars Puzzle läuft, und .sfrom-Dateien vom SNES Classic laden wie jede ROM.',
   'Le multicart Super 20 in 1 ouvre son propre menu de jeux, Gamars Puzzle tourne et les fichiers .sfrom de la SNES Classic se lancent comme une ROM.',
   'Il multicart Super 20 in 1 apre il suo menu di giochi, Gamars Puzzle funziona e i file .sfrom dello SNES Classic si caricano come una ROM.',
   'Мультикартридж Super 20 in 1 открывает своё меню игр, Gamars Puzzle работает, а файлы .sfrom от SNES Classic грузятся как обычные ROM.',
   'De multicart Super 20 in 1 opent zijn eigen spelmenu, Gamars Puzzle draait en .sfrom-bestanden van de SNES Classic laden als elke ROM.'),
  # 39 file-type icons in the list (2.17)
  ('Every row of the file list starts with an icon of its type: SNES game, NES, Master System, Game Boy, Atari 2600, music, theme, folder. A folder that opens as an MSU-1 game shows a yellow controller.',
   'Cada linha da lista começa com um ícone do tipo: jogo de SNES, NES, Master System, Game Boy, Atari 2600, música, tema, pasta. Uma pasta que abre como jogo MSU-1 mostra um controle amarelo.',
   'Cada fila de la lista empieza con un icono de su tipo: juego de SNES, NES, Master System, Game Boy, Atari 2600, música, tema, carpeta. Una carpeta que abre como juego MSU-1 muestra un mando amarillo.',
   'Jede Zeile der Liste beginnt mit einem Symbol ihres Typs: SNES-Spiel, NES, Master System, Game Boy, Atari 2600, Musik, Theme, Ordner. Ein Ordner, der als MSU-1-Spiel startet, zeigt einen gelben Controller.',
   'Chaque ligne de la liste commence par une icône de son type : jeu SNES, NES, Master System, Game Boy, Atari 2600, musique, thème, dossier. Un dossier qui se lance comme un jeu MSU-1 montre une manette jaune.',
   'Ogni riga della lista inizia con una icona del suo tipo: gioco SNES, NES, Master System, Game Boy, Atari 2600, musica, tema, cartella. Una cartella che si apre come gioco MSU-1 mostra un controller giallo.',
   'Каждая строка списка начинается со значка типа: игра SNES, NES, Master System, Game Boy, Atari 2600, музыка, тема, папка. Папка, которая открывается как игра MSU-1, показывает жёлтый контроллер.',
   'Elke regel van de lijst begint met een pictogram van het type: SNES-spel, NES, Master System, Game Boy, Atari 2600, muziek, thema, map. Een map die als MSU-1-spel opent, toont een gele controller.'),
  # 40 the cheat list from the game info card (2.17)
  ("[SELECT] on the game info card opens that game's cheat list: turn codes on and off, add or edit them, then start the game with them already set.",
   '[SELECT] na ficha do jogo abre a lista de cheats dele: ligue e desligue códigos, adicione ou edite, e inicie o jogo com eles já prontos.',
   '[SELECT] en la ficha del juego abre su lista de cheats: activa y desactiva códigos, añade o edita, y empieza el juego con ellos listos.',
   '[SELECT] auf der Infokarte öffnet die Cheat-Liste des Spiels: Codes an- und ausschalten, hinzufügen oder ändern, dann mit ihnen starten.',
   '[SELECT] sur la fiche du jeu ouvre sa liste de cheats : active ou désactive les codes, ajoute ou modifie, puis lance le jeu avec eux.',
   '[SELECT] sulla scheda del gioco apre la sua lista di cheat: attiva e disattiva i codici, aggiungi o modifica, poi avvia il gioco con quelli pronti.',
   '[SELECT] в окне Об игре открывает список читов этой игры: включай и выключай коды, добавляй или меняй их и запускай игру уже с ними.',
   '[SELECT] op de spelinfo opent de cheatlijst van het spel: zet codes aan en uit, voeg toe of wijzig, en start het spel er direct mee.'),
  # 41 clear the PPU before a patched game boots
  ("Some romhacks draw their intro without clearing the video memory, and on a real console the menu's leftovers show as garbage. {clearppu} in {patchopts} wipes it before a patched game starts.",
   'Alguns romhacks desenham a abertura sem limpar a memória de vídeo, e num console de verdade as sobras do menu aparecem como lixo. {clearppu} em {patchopts} limpa tudo antes de um jogo com patch começar.',
   'Algunos romhacks dibujan su intro sin limpiar la memoria de vídeo, y en una consola real los restos del menú salen como basura. {clearppu} en {patchopts} la borra antes de que empiece un juego parcheado.',
   'Manche Romhacks zeichnen ihr Intro, ohne den Videospeicher zu leeren, und auf echter Hardware erscheinen Menüreste als Müll. {clearppu} in {patchopts} löscht ihn, bevor ein gepatchtes Spiel startet.',
   "Certains romhacks dessinent leur intro sans vider la mémoire vidéo, et sur une vraie console les restes du menu s'affichent en vrac. {clearppu} dans {patchopts} la vide avant le lancement d'un jeu patché.",
   "Alcune romhack disegnano l'intro senza pulire la memoria video, e su una console vera gli avanzi del menu appaiono come spazzatura. {clearppu} in {patchopts} la pulisce prima che parta un gioco patchato.",
   'Некоторые ромхаки рисуют заставку, не очищая видеопамять, и на настоящей консоли остатки меню видны как мусор. {clearppu} в {patchopts} очищает её перед запуском пропатченной игры.',
   'Sommige romhacks tekenen hun intro zonder het videogeheugen te wissen, en op een echte console verschijnen restjes van het menu als rommel. {clearppu} in {patchopts} wist het voordat een gepatcht spel start.'),
  # 42 bus timing compat
  ('A game that freezes or glitches right after the logo on some consoles, mostly 1-CHIP ones? Try {cfg} > {ingame} > {buscompat}: it frees the cartridge bus a little earlier, like firmware 1.11.0 did.',
   'Um jogo trava ou dá glitch logo depois do logo em alguns consoles, quase sempre 1-CHIP? Tente {cfg} > {ingame} > {buscompat}: ela libera o barramento do cartucho um pouco antes, como a firmware 1.11.0 fazia.',
   '¿Un juego se cuelga o falla justo tras el logo en algunas consolas, casi siempre 1-CHIP? Prueba {cfg} > {ingame} > {buscompat}: libera el bus del cartucho un poco antes, como hacía el firmware 1.11.0.',
   'Hängt oder flackert ein Spiel direkt nach dem Logo auf manchen Konsolen, meist 1-CHIP? Versuche {cfg} > {ingame} > {buscompat}: es gibt den Modulbus etwas früher frei, wie Firmware 1.11.0.',
   'Un jeu qui plante ou bugue juste après le logo sur certaines consoles, souvent des 1-CHIP ? Essaie {cfg} > {ingame} > {buscompat} : il libère le bus de la cartouche un peu plus tôt, comme le firmware 1.11.0.',
   "Un gioco si blocca o dà glitch subito dopo il logo su alcune console, quasi sempre 1-CHIP? Prova {cfg} > {ingame} > {buscompat}: libera il bus della cartuccia un po' prima, come il firmware 1.11.0.",
   'Игра зависает или сбоит сразу после логотипа на некоторых консолях, чаще 1-CHIP? Попробуй {cfg} > {ingame} > {buscompat}: шина картриджа освобождается чуть раньше, как в прошивке 1.11.0.',
   'Loopt een spel vast of hapert het net na het logo op sommige consoles, meestal 1-CHIP? Probeer {cfg} > {ingame} > {buscompat}: de cartridgebus wordt iets eerder vrijgegeven, zoals firmware 1.11.0 deed.'),
  # 43 the hardware model in System Information
  ('{sysinfo} in the main menu ([X]) now also shows which hardware it runs on: sd2snes Mk.II, sd2snes Mk.III or FXPAK PRO STM32.',
   '{sysinfo} no menu principal ([X]) agora mostra também em que hardware ele roda: sd2snes Mk.II, sd2snes Mk.III ou FXPAK PRO STM32.',
   '{sysinfo} en el menú principal ([X]) ahora también muestra en qué hardware corre: sd2snes Mk.II, sd2snes Mk.III o FXPAK PRO STM32.',
   '{sysinfo} im Hauptmenü ([X]) zeigt jetzt auch, auf welcher Hardware es läuft: sd2snes Mk.II, sd2snes Mk.III oder FXPAK PRO STM32.',
   '{sysinfo} dans le menu principal ([X]) montre aussi sur quel matériel il tourne : sd2snes Mk.II, sd2snes Mk.III ou FXPAK PRO STM32.',
   '{sysinfo} nel menu principale ([X]) ora mostra anche su quale hardware gira: sd2snes Mk.II, sd2snes Mk.III o FXPAK PRO STM32.',
   '{sysinfo} в главном меню ([X]) теперь показывает и модель устройства: sd2snes Mk.II, sd2snes Mk.III или FXPAK PRO STM32.',
   '{sysinfo} in het hoofdmenu ([X]) toont nu ook op welke hardware het draait: sd2snes Mk.II, sd2snes Mk.III of FXPAK PRO STM32.'),
  # 44 BS-X and the Memory Pack
  ('Satellaview games that use the 8M Memory Pack slot read it from /sd2snes/saves as <rom>.mpk: saved sound novels, downloaded races and the like. The game still boots without a pack.',
   'Jogos do Satellaview que usam o slot de Memory Pack de 8M leem o pack de /sd2snes/saves como <rom>.mpk: sound novels salvas, corridas baixadas e afins. O jogo boota mesmo sem o pack.',
   'Los juegos de Satellaview que usan el slot de Memory Pack de 8M lo leen de /sd2snes/saves como <rom>.mpk: sound novels guardadas, carreras descargadas y más. El juego arranca incluso sin pack.',
   'Satellaview-Spiele mit 8M-Memory-Pack-Slot lesen das Pack aus /sd2snes/saves als <rom>.mpk: gespeicherte Sound Novels, geladene Rennen und mehr. Das Spiel startet auch ohne Pack.',
   'Les jeux Satellaview qui utilisent le slot Memory Pack 8M le lisent dans /sd2snes/saves en <rom>.mpk : sound novels sauvegardés, courses téléchargées... Le jeu démarre même sans pack.',
   'I giochi Satellaview che usano lo slot Memory Pack da 8M lo leggono da /sd2snes/saves come <rom>.mpk: sound novel salvate, corse scaricate e simili. Il gioco parte anche senza pack.',
   'Игры Satellaview со слотом Memory Pack 8M читают его из /sd2snes/saves как <rom>.mpk: сохранённые звуковые новеллы, скачанные гонки и прочее. Игра запускается и без Memory Pack.',
   'Satellaview-spellen met een 8M Memory Pack-slot lezen het pack uit /sd2snes/saves als <rom>.mpk: opgeslagen sound novels, gedownloade races en meer. Het spel start ook zonder pack.'),
  # 45 delete files and saves
  ("From the [Y] menu of the file list, {del} erases the selected file and {delsrm} only the game's saves, all 4 slots. Deleting a ROM also takes its cover, info card, guides and cheats along.",
   'No menu do [Y] da lista de arquivos, {del} apaga o arquivo selecionado e {delsrm} só os saves do jogo, nos 4 slots. Apagar uma ROM leva junto a capa, a ficha, as guias e os cheats dela.',
   'Desde el menú [Y] de la lista, {del} borra el archivo elegido y {delsrm} solo los saves del juego, las 4 ranuras. Borrar una ROM también se lleva su carátula, ficha, guías y cheats.',
   'Im [Y]-Menü der Dateiliste löscht {del} die gewählte Datei und {delsrm} nur die Spielstände, alle 4 Plätze. Eine gelöschte ROM nimmt Cover, Infokarte, Anleitungen und Cheats mit.',
   'Depuis le menu [Y] de la liste, {del} efface le fichier et {delsrm} les 4 emplacements de sauvegarde du jeu. Supprimer une ROM emporte aussi jaquette, fiche, guides et cheats.',
   'Dal menu [Y] della lista, {del} cancella il file scelto e {delsrm} solo i salvataggi del gioco, tutti e 4 gli slot. Eliminare una ROM porta via anche copertina, scheda, guide e cheat.',
   'В списке файлов по кнопке [Y] пункт {del} удаляет выбранный файл, а {delsrm} только сохранения игры, все 4 слота. Вместе с игрой удаляются её обложка, карточка, руководства и читы.',
   'Vanuit het [Y]-menu van de lijst wist {del} het gekozen bestand en {delsrm} alleen de saves van het spel, alle 4 plekken. Een gewiste ROM neemt ook hoes, spelinfo, gidsen en cheats mee.'),
  # 46 missing BIOS warning
  ('Games with special chips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) need support files on the card. If one is missing, the menu names the file and stays up, instead of freezing or starting a broken game.',
   'Jogos com chips especiais (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) precisam de arquivos de apoio no cartão. Se faltar algum, o menu diz qual é e continua de pé, em vez de travar ou abrir um jogo quebrado.',
   'Los juegos con chips especiales (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) necesitan archivos de apoyo en la tarjeta. Si falta uno, el menú dice cuál y sigue en pie, en vez de colgarse o abrir un juego roto.',
   'Spiele mit Spezialchips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) brauchen Hilfsdateien auf der Karte. Fehlt eine, nennt das Menü die Datei und bleibt stehen, statt zu hängen oder ein kaputtes Spiel zu starten.',
   "Les jeux à puces spéciales (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) ont besoin de fichiers sur la carte. S'il en manque un, le menu dit lequel et reste ouvert, au lieu de planter ou de lancer un jeu cassé.",
   'I giochi con chip speciali (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) vogliono file di supporto sulla scheda. Se ne manca uno, il menu dice quale e resta attivo, invece di bloccarsi o avviare un gioco rotto.',
   'Играм со специальными чипами (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) нужны файлы BIOS на карте. Если какого-то нет, меню показывает его название и остаётся открытым, а не зависает и не запускает сломанную игру.',
   'Spellen met speciale chips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) hebben hulpbestanden op de kaart nodig. Ontbreekt er een, dan noemt het menu het bestand en blijft het open, in plaats van te hangen.'),
  # 47 the clock question on boot and the date format
  ("When the cartridge's clock is not set, the menu asks for the time on start. {askclock} in {browser} turns the question off. The date at the bottom of the list follows the language: month first in English, day first in the others.",
   'Quando o relógio do cartucho não está acertado, o menu pede a hora ao iniciar. {askclock} em {browser} desliga a pergunta. A data no rodapé da lista segue o idioma: mês primeiro em inglês, dia primeiro nos outros.',
   'Si el reloj del cartucho no está en hora, el menú la pide al iniciar. {askclock} en {browser} quita la pregunta. La fecha al pie de la lista sigue el idioma: primero el mes en inglés, el día en los demás.',
   'Ist die Uhr des Moduls nicht gestellt, fragt das Menü beim Start nach der Zeit. {askclock} in {browser} schaltet die Frage ab. Das Datum unter der Liste folgt der Sprache: Monat zuerst auf Englisch, sonst der Tag.',
   "Si l'horloge de la cartouche n'est pas réglée, le menu demande l'heure au démarrage. {askclock} dans {browser} coupe la question. La date en bas de la liste suit la langue : le mois d'abord en anglais, le jour sinon.",
   "Se l'orologio della cartuccia non è impostato, il menu chiede l'ora all'avvio. {askclock} in {browser} toglie la domanda. La data in fondo alla lista segue la lingua: prima il mese in inglese, il giorno nelle altre.",
   'Если часы картриджа не выставлены, меню спрашивает время при старте. {askclock} в {browser} отключает вопрос. Дата внизу списка следует языку: в английском сначала месяц, в остальных день.',
   'Is de klok van de cartridge niet ingesteld, dan vraagt het menu bij de start om de tijd. {askclock} in {browser} zet dat uit. De datum onder de lijst volgt de taal: in het Engels eerst de maand, anders de dag.'),
  # 48 a description for each option
  ("Every option in {cfg} shows a sentence in a box on screen explaining what it does, in the menu's language, so each setting makes sense without a manual.",
   'Cada opção em {cfg} mostra uma frase numa caixa na tela explicando o que ela faz, no idioma do menu, para cada ajuste fazer sentido sem manual.',
   'Cada opción de {cfg} muestra una frase en un recuadro explicando lo que hace, en el idioma del menú, para entender cada ajuste sin manual.',
   'Jede Option in {cfg} zeigt in einem Kasten einen Satz, der erklärt, was sie tut, in der Menüsprache, damit jede Einstellung ohne Handbuch verständlich ist.',
   "Chaque option de {cfg} affiche dans un cadre une phrase qui explique ce qu'elle fait, dans la langue du menu, pour comprendre chaque réglage sans manuel.",
   'Ogni opzione in {cfg} mostra in un riquadro una frase che spiega cosa fa, nella lingua del menu, così ogni impostazione si capisce senza manuale.',
   'Каждая опция в разделе {cfg} показывает в рамке информацию о том, что она делает, на языке меню, так что всё понятно без руководства.',
   'Elke optie in {cfg} toont in een kader een zin die uitlegt wat ze doet, in de taal van het menu, zodat elke instelling zonder handleiding duidelijk is.'),
]

def _cells(text):
    """Screen cells of a string; aborts on a character the font has no tile for
    (build_const.encode_string would pass it through as raw UTF-8 bytes, which
    draw as two tiles of garbage -- nothing downstream catches that)."""
    n = 0
    for ch in text:
        if ch in "[]":                  # a button's green markup: no cell (see _encode_line)
            continue
        if ch in '";':
            # '"' closes encode_string's literal early (the rest of the line came out as
            # byte lists and commas on screen), and the font has no ';' (it draws as //)
            sys.exit("gen_onb_lang: %r is not allowed in %r" % (ch, text))
        if ch in ENCODE or (" " <= ch <= "~"):
            n += 1
        else:
            sys.exit("gen_onb_lang: no font tile for %r in %r" % (ch, text))
    return n


TEXT_W = 30          # == ONB_TEXT_W: cols 1..30
MORE_FIRST = 40      # == ONB_MORE_FIRST: the "and more" items are the descriptors from here
MORE_NAME_W = 24     # an answer row's width (onb_draw_option_value prints labels with 24)
TEXT_MAX_LINES = 11  # the answer list starts 2 rows below the text and the longest
                     # list (8 languages) has to fit above the progress bar
_LABELS = {"off": "onb_text_off", "on": "onb_text_on", "menu": "onb_text_rst_menu", "large": "onb_text_large",
           "small": "onb_text_small", "ctx": "onb_text_gi_ctx", "folder": "onb_text_rst_folder",
           "rom": "onb_text_rst_game", "hold": "onb_text_rst_hold", "theme": "onb_text_theme",
           "yes": "onb_text_yes", "no": "onb_text_no"}
# the menu's own words, as its dictionaries have them (so a paragraph names what the
# menu shows)
_MENU_LABELS = {"cfg": "mtext_mm_cfg", "sgbmenu": "mtext_cfg_sgb", "auto": "text_auto",
                "prefsgb": "text_gbc_prefer_sgb", "prefgbc": "text_gbc_prefer_gbc",
                "saves": "text_igm_tab_saves", "del": "text_filesel_context_delete_file",
                "delsrm": "text_filesel_context_delete_srm", "browser": "mtext_cfg_browser",
                "restoretheme": "mtext_browser_restoretheme",
                "restoreclassic": "mtext_browser_restoreclassic",
                "hdrmode": "mtext_patch_header_mode", "hdrauto": "text_patch_hdrsel_auto",
                "hdron": "text_patch_hdrsel_on", "hdroff": "text_patch_hdrsel_off",
                "createrom": "mtext_patch_create_rom", "setbgm": "text_filesel_set_as_bgm",
                "restoremusic": "mtext_browser_restoremusic", "ingame": "mtext_cfg_ingame",
                "buscompat": "mtext_scic_buscompat", "askclock": "mtext_cfg_ask_clock",
                "clearppu": "mtext_patch_clearppu", "patchopts": "mtext_cfg_patch",
                "chipopts": "mtext_cfg_chip", "a26w": "mtext_a26_width",
                "sysinfo": "mtext_mm_sysinfo", "memtest": "mtext_mm_memtest",
                "trainer": "text_igm_tab_trainer", "unknown": "text_igm_tr_unknown",
                "changed": "text_igm_tr_changed", "increased": "text_igm_tr_increased",
                "decreased": "text_igm_tr_decreased", "savecheat": "text_igm_tr_savecheat",
                "cheats": "text_filesel_context_cheats", "cheatstab": "text_igm_tab_cheats",
                "hook": "mtext_ingame_enable"}
_MENU_WORDS = {key: menu_text(lab) for key, lab in _MENU_LABELS.items()}
# a sub-option's label without the menu's indent and arrow glyph (" {129}")
_MENU_WORDS["igbuttons"] = tuple(w.replace("{129}", "").strip() for w in menu_text("mtext_ingame_buttons"))


# A button named in a paragraph is written [A], [L]+[R], [Start]... and drawn green like
# every key hint: each bracket becomes BTN_TOGGLE, a byte onb_hiprint reads as "flip
# between the line's palette and green" without printing or advancing a column.
BTN_TOGGLE = 2


def _encode_line(line):
    """encode_string for a paragraph line, the button brackets turned into BTN_TOGGLE."""
    if line.count("[") != line.count("]"):
        sys.exit("gen_onb_lang: unbalanced button markup in %r" % line)
    toks = []
    for k, seg in enumerate(line.replace("]", "[").split("[")):
        if k:
            toks.append(str(BTN_TOGGLE))
        if seg:
            enc = encode_string(seg)
            toks.append(enc[:-3])           # drop the ", 0"
    # A toggle that closes the line does nothing (every line starts in its own colour),
    # and on a full line it would be the byte after the last cell: onb_hiprint stops on
    # its cell count before reading it, and onb_emit_para would take it for "another
    # line" and print an empty one.
    if toks and toks[-1] == str(BTN_TOGGLE):
        toks.pop()
    return ", ".join(toks)


def _wrap(text, width):
    """Word-wrap to width cells; a "\n" starts a new line (each piece wraps on its own)."""
    if "\n" in text:
        return [l for piece in text.split("\n") for l in _wrap(piece, width)]
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
        _t = _t.format(**{key: STRINGS[lab][_k] for key, lab in _LABELS.items()},
                       **{key: words[_k] for key, words in _MENU_WORDS.items()})
        cols.append(_wrap(_t, TEXT_W))
    _wrapped.append(cols)
# pad: every language of a card to its longest, and the "and more" items to each other
_MORE = range(MORE_FIRST, len(_wrapped))
_more_h = max(len(c) for i in _MORE for c in _wrapped[i])
for _i, cols in enumerate(_wrapped):
    h = _more_h if _i in _MORE else max(len(c) for c in cols)
    if h > TEXT_MAX_LINES:
        sys.exit("gen_onb_lang: feature %d text is %d lines, max %d" % (_i + 1, h, TEXT_MAX_LINES))
    STRINGS["onb_f%d_text" % (_i + 1)] = tuple(c + [""] * (h - len(c)) for c in cols)


# The string pool is split by language over two banks: the dispatch tables and pool A in
# $C1 (onb_const_lang.a65), pool B in the free half of the code bank $C0
# (onb_const_lang_b.a65). A table entry is the string's 16-bit address; onb_resolve_str
# (onb_ui.a65) takes its bank from onb_strpool_bank[language]. One column lives in one
# pool, so a language costs one byte of lookup. Pick the pools by the measured sizes the
# generator prints (each bank also holds code or the font).
POOL_B_LANGS = ("de", "fr", "ru", "nl")


def main():
    out_path = "onb_const_lang.a65"
    if "-o" in sys.argv:
        out_path = sys.argv[sys.argv.index("-o") + 1]
    out_b = os.path.join(os.path.dirname(out_path), "onb_const_lang_b.a65")

    nlang = len(LANGS)
    head = ["; ==========================================================================",
            "; AUTO-GENERATED by utils/gen_onb_lang.py -- DO NOT EDIT BY HAND.",
            "; Onboarding i18n string pool (EN/PT/ES/DE/FR/IT/RU/NL). Edit the STRINGS table in the",
            "; generator and re-run `make` instead.",
            "; =========================================================================="]
    L = head + [".link page $c1", ""]
    B = head + ["; pool B: the columns of " + ", ".join(POOL_B_LANGS) + " (onb_strpool_bank)",
                ".link page $c0", "", "onb_pool_b:"]
    L.append("onb_strtab_nlang  .byt %d" % nlang)
    L.append("; the bank of each language's strings, by onb_cur_lang")
    L.append("onb_strpool_bank  .byt " + ", ".join("^onb_pool_b" if l in POOL_B_LANGS else "^onb_pool_a"
                                                     for l in LANGS))
    L.append("")
    L.append("; ---- dispatch tables (one row of %d word pointers per label) ----" % nlang)
    L.append("onb_strtab_lo:")
    for label in STRINGS:
        L.append("%s:" % label)
        for lang in LANGS:
            L.append("  .word !%s_%s" % (label, lang))
    L.append("onb_strtab_hi:")
    L.append("")
    L.append("; ---- pool A: per-language strings (font-encoded; outside the dispatch range) ----")
    L.append("onb_pool_a:")
    size = dict.fromkeys(LANGS, 0)
    for label, vals in STRINGS.items():
        if len(vals) != nlang:
            sys.exit("label %s has %d values, expected %d" % (label, len(vals), nlang))
        for lang, text in zip(LANGS, vals):
            dst = B if lang in POOL_B_LANGS else L
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
                dst.append("%s_%s  .byt %s" % (label, lang, ", ".join(toks)))
                size[lang] += sum(_cells(x) + x.count("[") + x.count("]") + 1 for x in text)
                continue
            if _cells(text) > _budget(label):
                sys.exit("gen_onb_lang: %s[%s] is %d cells, budget %d: %r"
                         % (label, lang, _cells(text), _budget(label), text))
            enc = encode_string(text)
            dst.append("%s_%s  .byt %s" % (label, lang, enc))
            size[lang] += _cells(text) + 1

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    with open(out_b, "w", encoding="utf-8") as f:
        f.write("\n".join(B) + "\n")
    pool_b = sum(size[l] for l in POOL_B_LANGS)
    print("generated %s + %s: %d labels x %d languages; pool A %d B, pool B %d B (%s)"
          % (out_path, os.path.basename(out_b), len(STRINGS), nlang, sum(size.values()) - pool_b,
             pool_b, " ".join("%s=%d" % kv for kv in size.items())))


if __name__ == "__main__":
    main()
