#!/usr/bin/env python3
"""Generate onb_const_lang.a65 -- the onboarding ROM's i18n string pool.

Self-contained generator (the onboarding is a separate image, so it has its own
strtab independent of the menu's const_lang*.a65). Mirrors the menu's resolve_str
dispatch format: each localized label is a ROW of N word pointers living between
`onb_strtab_lo` and `onb_strtab_hi`; onb_resolve_str (onb_ui.a65) recognizes a
pointer in that range and swaps the column by onb_cur_lang (the menu's language
index, 8 columns). The per-language strings live AFTER onb_strtab_hi (outside the
dispatch range) in bank $C1. Accent/Cyrillic encoding is build_const.encode_string's;
a character the font has no tile for aborts the build (_cells), and so does a string
wider than the cells the engine prints it with (_budget).

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

# The cards (13) and the "and more" items (6): their names (the paragraphs are TEXTS below).

# The descriptors in tour order, by name: the base tour's cards (1-11, "and more" last),
# the 2.17 section (12-19: its separator card, then the release's cards), then the
# "and more" items (20-31, ONB_MORE_FIRST in onboarding_main.a65). Per language.
NAMES = [
  # 1 language
  ('Language', 'Idioma', 'Idioma', 'Sprache', 'Langue', 'Lingua', 'Язык', 'Taal'),
  # 2 box art
  ('Box art in the list', 'Capas na lista', 'Carátulas en la lista', 'Cover in der Liste', 'Jaquettes dans la liste', 'Copertine nella lista', 'Обложки в списке', 'Hoezen in de lijst'),
  # 3 game info card
  ('Game info card', 'Ficha do jogo', 'Ficha del juego', 'Spiel-Infokarte', 'Fiche du jeu', 'Scheda del gioco', 'Об игре', 'Spelinfo'),
  # 4 menu music
  ('Menu music', 'Música do menu', 'Música del menú', 'Menümusik', 'Musique du menu', 'Musica del menu', 'Музыка меню', 'Menumuziek'),
  # 5 random music
  ('Random music', 'Música aleatória', 'Música aleatoria', 'Zufallsmusik', 'Musique aléatoire', 'Musica casuale', 'Случайная музыка', 'Willekeurige muziek'),
  # 6 menu sounds
  ('Menu sounds', 'Sons do menu', 'Sonidos del menú', 'Menü-Sounds', 'Sons du menu', 'Suoni del menu', 'Звуки меню', 'Menugeluiden'),
  # 7 in-game menu
  ('In-game menu', 'Menu in-game', 'Menú en el juego', 'Ingame-Menü', 'Menu en jeu', 'Menu in gioco', 'Меню в игре', 'In-game menu'),
  # 8 savestates
  ('Savestates', 'Savestates', 'Savestates', 'Savestates', 'Savestates', 'Savestate', 'Сейвстейты', 'Savestates'),
  # 9 MSU-1 folders
  ('MSU-1 folders', 'Pastas MSU-1', 'Carpetas MSU-1', 'MSU-1-Ordner', 'Dossiers MSU-1', 'Cartelle MSU-1', 'Папки MSU-1', 'MSU-1-mappen'),
  # 10 smart reset
  ('Smart reset', 'Reset inteligente', 'Reset inteligente', 'Smart-Reset', 'Reset intelligent', 'Reset intelligente', 'Умный сброс', 'Slimme reset'),
  # 11 "and more" (the base tour's last card)
  ('And more', 'E mais', 'Y más', 'Und mehr', 'Et plus', 'E altro', 'И ещё', 'En meer'),
  # 12 the 2.17 section: what is new in this release
  ("What's new in 2.17", 'Novidades da 2.17', 'Novedades de la 2.17', 'Neu in 2.17', 'Nouveautés de la 2.17', 'Novità della 2.17', 'Новое в 2.17', 'Nieuw in 2.17'),
  # 13 controller 2 (2.17)
  ('Controller 2 shortcuts', 'Atalhos no controle 2', 'Atajos en el mando 2', 'Kürzel auf Controller 2', 'Raccourcis manette 2', 'Scorciatoie controller 2', 'Комбо на контроллере 2', 'Sneltoetsen controller 2'),
  # 14 Game Boy Color (2.17)
  ('Game Boy Color', 'Game Boy Color', 'Game Boy Color', 'Game Boy Color', 'Game Boy Color', 'Game Boy Color', 'Game Boy Color', 'Game Boy Color'),
  # 15 the in-game shortcut list (2.17)
  ('Shortcut list', 'Lista de atalhos', 'Lista de atajos', 'Kürzel-Liste', 'Liste des raccourcis', 'Elenco scorciatoie', 'Список комбинаций', 'Sneltoetsenlijst'),
  # 16 Seta chips and bootlegs (2.17, cores by M2M)
  ('Seta chips and bootlegs', 'Chips Seta e bootlegs', 'Chips Seta y bootlegs', 'Seta-Chips und Bootlegs', 'Puces Seta et bootlegs', 'Chip Seta e bootleg', 'Чипы Seta и бутлеги', 'Seta-chips en bootlegs'),
  # 17 Super 20 in 1, Gamars Puzzle and .sfrom (2.17, by terminator2k2)
  ('Super 20 in 1, Gamars, .sfrom',)*8,
  # 18 file-type icons in the list (2.17)
  ('Icons in the list', 'Ícones na lista', 'Iconos en la lista', 'Symbole in der Liste', 'Icônes dans la liste', 'Icone nella lista', 'Значки в списке', 'Pictogrammen in de lijst'),
  # 19 the cheat list from the game info card (2.17)
  ('Cheats from the game info', 'Cheats pela ficha do jogo', 'Cheats desde la ficha', 'Cheats aus der Infokarte', 'Cheats depuis la fiche', 'Cheat dalla scheda', 'Читы из окна "Об игре"', 'Cheats vanuit spelinfo'),
  # 20 themes (an "and more" item)
  ('Themes', 'Temas', 'Temas', 'Themes', 'Thèmes', 'Temi', 'Темы', "Thema's"),
  # 21 cheats and trainer
  ('Cheats and trainer', 'Cheats e trainer', 'Cheats y trainer', 'Cheats und Trainer', 'Cheats et trainer', 'Cheats e trainer', 'Читы и трейнер', 'Cheats en trainer'),
  # 22 patches
  ('IPS/BPS patches', 'Patches IPS/BPS', 'Parches IPS/BPS', 'IPS/BPS-Patches', 'Patchs IPS/BPS', 'Patch IPS/BPS', 'Патчи IPS/BPS', 'IPS/BPS-patches'),
  # 23 other consoles
  ('Other consoles', 'Outros consoles', 'Otras consolas', 'Andere Konsolen', 'Autres consoles', 'Altre console', 'Другие консоли', 'Andere consoles'),
  # 24 more special chips
  ('More special chips', 'Mais chips especiais', 'Más chips especiales', 'Weitere Spezialchips', 'Autres puces spéciales', 'Altri chip speciali', 'Ещё спецчипы', 'Meer speciale chips'),
  # 25 memory test
  ('Memory test', 'Teste de memória', 'Prueba de memoria', 'Speichertest', 'Test mémoire', 'Test di memoria', 'Тест памяти', 'Geheugentest'),
  # 26 4 battery saves per game
  ('4 saves per game', '4 saves por jogo', '4 partidas por juego', '4 Spielstände pro Spiel', '4 sauvegardes par jeu', '4 salvataggi per gioco', '4 сохранения на игру', '4 saves per spel'),
  # 27 MSU-1 tracks in the menu
  ('MSU-1 tracks in the menu', 'Trilhas MSU-1 no menu', 'Pistas MSU-1 en el menú', 'MSU-1-Tracks im Menü', 'Pistes MSU-1 au menu', 'Tracce MSU-1 nel menu', 'Треки MSU-1 в меню', 'MSU-1-nummers in menu'),
  # 28 BS-X and the Memory Pack
  ('BS-X and Memory Pack', 'BS-X e Memory Pack', 'BS-X y Memory Pack', 'BS-X und Memory Pack', 'BS-X et Memory Pack', 'BS-X e Memory Pack', 'BS-X и Memory Pack', 'BS-X en Memory Pack'),
  # 29 delete files and saves
  ('Delete files and saves', 'Apagar arquivos e saves', 'Borrar archivos y saves', 'Dateien/Saves löschen', 'Effacer fichiers/saves', 'Eliminare file e save', 'Удаление файлов', 'Bestanden/saves wissen'),
  # 30 missing BIOS warning
  ('Missing BIOS warning', 'Aviso de BIOS faltando', 'Aviso de BIOS que falta', 'Fehlendes BIOS melden', 'Alerte de BIOS manquant', 'Avviso di BIOS mancante', 'Нет файла BIOS', 'Melding: BIOS ontbreekt'),
  # 31 a description for each option
  ('Option descriptions', 'Descrição das opções', 'Descripción de opciones', 'Optionsbeschreibungen', 'Description des options', 'Descrizione opzioni', 'Описание опций', 'Uitleg bij opties'),
]


# Each card's paragraph, per language ({off}, {on}... are the answer labels as the
# list shows them, {cfg}, {saves}... the menu's own words). The generator word-wraps it
# to ONB_TEXT_W cells and pads every language to the same number of lines (the "and
# more" items to the same count as each other), so the answer list below it stays put
# when the language or the item changes. Lines are separated by byte 1, where
# onb_hiprint stops; onb_emit_para prints one per row.
TEXTS = [
  # 1 language
  ('The whole menu, the in-game menu and this tour in 8 languages. Move the bar to try one: the tour switches at once. You can change it later in Configuration.',
   'O menu inteiro, o menu in-game e este tour em 8 idiomas. Mova a barra para experimentar: o tour troca na hora. Dá para mudar depois em Configurações.',
   'Todo el menú, el menú del juego y este tour en 8 idiomas. Mueve la barra para probar: el tour cambia al instante. Puedes cambiarlo luego en Configuración.',
   'Das ganze Menü, das Ingame-Menü und diese Tour in 8 Sprachen. Bewege den Balken zum Testen: die Tour wechselt sofort. Später in den Einstellungen änderbar.',
   'Tout le menu, le menu en jeu et cette visite en 8 langues. Déplace la barre pour essayer : la visite change aussitôt. Modifiable ensuite dans Configuration.',
   'Tutto il menu, il menu in gioco e questo tour in 8 lingue. Sposta la barra per provare: il tour cambia subito. Puoi cambiarla dopo in Configurazione.',
   'Всё меню, "меню в игре" и этот тур на 8-ми языках. Двигай полосу, чтобы попробовать: тур сразу переключится. Потом можно сменить в настройках.',
   'Het hele menu, het in-game menu en deze rondleiding in 8 talen. Beweeg de balk om te proberen: de rondleiding wisselt meteen. Later te wijzigen in Configuratie.'),
  # 2 box art
  ("Shows the game's box art next to the list as you browse. {large} fills the top corner, {small} takes less room. Covers are .cov files: the Web Manager downloads them for your whole collection.",
   'Mostra a capa do jogo ao lado da lista enquanto você navega. {large} ocupa o canto de cima, {small} ocupa menos espaço. As capas são arquivos .cov: o Web Manager baixa para a coleção inteira.',
   'Muestra la carátula del juego junto a la lista mientras navegas. {large} ocupa la esquina superior, {small} ocupa menos. Son archivos .cov: el Web Manager los descarga para toda tu colección.',
   'Zeigt das Spielcover neben der Liste beim Blättern. {large} füllt die obere Ecke, {small} braucht weniger Platz. Cover sind .cov-Dateien: der Web Manager lädt sie für die ganze Sammlung.',
   'Affiche la jaquette du jeu à côté de la liste pendant la navigation. {large} occupe le coin du haut, {small} prend moins de place. Ce sont des fichiers .cov : le Web Manager les télécharge pour toute ta collection.',
   "Mostra la copertina del gioco accanto alla lista mentre navighi. {large} occupa l'angolo in alto, {small} occupa meno spazio. Sono file .cov: il Web Manager li scarica per tutta la collezione.",
   'Показывает обложку игры рядом со списком. {large} = занимает верхний угол, {small} = меньше места. Обложки это файлы .cov: Web Manager скачает их для всей коллекции.',
   'Toont de hoes van het spel naast de lijst tijdens het bladeren. {large} vult de bovenhoek, {small} neemt minder ruimte. Hoezen zijn .cov-bestanden: de Web Manager haalt ze op voor je hele collectie.'),
  # 3 game info card
  ('A card with the cover, screenshot, publisher, year, genre and description before the game starts. {on}: [A] opens it. {ctx}: [A] starts the game and the card is in the [Y] menu. {off}: [A] starts the game.',
   'Uma ficha com capa, screenshot, editora, ano, gênero e descrição antes do jogo começar. {on}: [A] abre a ficha. {ctx}: [A] inicia o jogo e a ficha fica no menu do [Y]. {off}: [A] inicia o jogo.',
   'Una ficha con carátula, captura, editora, año, género y descripción antes de jugar. {on}: [A] la abre. {ctx}: [A] inicia el juego y la ficha está en el menú de [Y]. {off}: [A] inicia el juego.',
   'Eine Karte mit Cover, Screenshot, Publisher, Jahr, Genre und Beschreibung vor dem Start. {on}: [A] öffnet sie. {ctx}: [A] startet das Spiel, die Karte ist im [Y]-Menü. {off}: [A] startet das Spiel.',
   "Une fiche avec jaquette, capture, éditeur, année, genre et description avant de jouer. {on} : [A] l'ouvre. {ctx} : [A] lance le jeu et la fiche est dans le menu [Y]. {off} : [A] lance le jeu.",
   'Una scheda con copertina, schermata, editore, anno, genere e descrizione prima di giocare. {on}: [A] la apre. {ctx}: [A] avvia il gioco e la scheda è nel menu [Y]. {off}: [A] avvia il gioco.',
   'Карточка с обложкой, скриншотом, издателем, годом, жанром и описанием перед запуском. {on}: Открывает её. {ctx}: Запускает игру, карточка в меню кнопка [Y]. {off}: Сразу запускает игру.',
   'Een kaart met hoes, screenshot, uitgever, jaar, genre en beschrijving voor het spelen. {on}: [A] opent hem. {ctx}: [A] start het spel, de kaart zit in het [Y]-menu. {off}: [A] start het spel.'),
  # 4 menu music
  ('An .spc soundtrack plays in the background while you browse and stops when a game starts. To use your own, put it at /sd2snes/menu.spc.',
   'Uma trilha .spc toca ao fundo enquanto você navega e para quando o jogo começa. Para trocar, ponha sua trilha em /sd2snes/menu.spc.',
   'Una pista .spc suena de fondo mientras navegas y se detiene al iniciar un juego. Para cambiarla, pon tu pista en /sd2snes/menu.spc.',
   'Ein .spc-Soundtrack läuft beim Blättern im Hintergrund und stoppt beim Spielstart. Eigenen Titel als /sd2snes/menu.spc ablegen.',
   "Une musique .spc joue en fond pendant la navigation et s'arrête au lancement d'un jeu. Pour la changer, mets ta piste dans /sd2snes/menu.spc.",
   "Una traccia .spc suona in sottofondo mentre navighi e si ferma all'avvio del gioco. Per cambiarla, metti la tua traccia in /sd2snes/menu.spc.",
   'Музыка .spc играет фоном, пока ты листаешь меню, и стихает при запуске игры. Назови свой трек menu.spc и положи в папку /sd2snes.',
   'Een .spc-soundtrack speelt op de achtergrond tijdens het bladeren en stopt als een spel start. Eigen nummer: zet het in /sd2snes/menu.spc.'),
  # 5 random music
  ('Each time the menu starts, a different track is drawn from the /sd2snes/music folder. Put as many .spc files there as you like to build your playlist.',
   'A cada vez que o menu abre, uma trilha diferente é sorteada da pasta /sd2snes/music. Coloque lá quantos .spc quiser para montar sua playlist.',
   'Cada vez que se abre el menú, se elige una pista distinta de la carpeta /sd2snes/music. Pon ahí todos los .spc que quieras para tu lista.',
   'Bei jedem Menüstart wird ein anderer Titel aus dem Ordner /sd2snes/music gewählt. Lege dort beliebig viele .spc-Dateien als Playlist ab.',
   'À chaque ouverture du menu, une piste différente est tirée du dossier /sd2snes/music. Mets-y autant de .spc que tu veux pour ta playlist.',
   'A ogni avvio del menu viene scelta una traccia diversa dalla cartella /sd2snes/music. Mettici tutti gli .spc che vuoi per la tua playlist.',
   'При каждом запуске меню играет случайный трек из папки /sd2snes/music. Положи туда сколько угодно файлов .spc.',
   'Bij elke start van het menu wordt een ander nummer uit de map /sd2snes/music gekozen. Zet er zoveel .spc-bestanden in als je wilt.'),
  # 6 menu sounds
  ("Short sound effects when you move the cursor, confirm, go back or hit an error, played through the cartridge's MSU-1 audio. Make your own set in the Sound Creator on the website.",
   'Efeitos curtos ao mover o cursor, confirmar, voltar ou dar erro, tocados pelo áudio MSU-1 do cartucho. Crie o seu conjunto no Criador de Sons do site.',
   'Efectos cortos al mover el cursor, confirmar, volver o al haber un error, por el audio MSU-1 del cartucho. Crea los tuyos en el Sound Creator de la web.',
   'Kurze Effekte beim Bewegen des Cursors, Bestätigen, Zurückgehen und bei Fehlern, über das MSU-1-Audio des Moduls. Eigene im Sound Creator auf der Website.',
   "Des effets courts quand tu bouges le curseur, valides, reviens ou en cas d'erreur, joués par l'audio MSU-1 de la cartouche. Crée les tiens dans le Sound Creator du site.",
   "Brevi effetti quando muovi il cursore, confermi, torni indietro o c'è un errore, dall'audio MSU-1 della cartuccia. Crea i tuoi con il Sound Creator sul sito.",
   'Короткие звуки при движении курсора, выборе, возврате и ошибке, через звук MSU-1 картриджа. Создай свои в Sound Creator на сайте.',
   'Korte geluiden bij cursor bewegen, bevestigen, terug en fouten, via de MSU-1-audio van de cartridge. Maak je eigen set in de Sound Creator op de website.'),
  # 7 in-game menu
  ('Press [L]+[R]+[Y]+[Left] during a game to pause it and open a menu over it: cheats, savestates, save slots, guides and a RAM trainer. Works with enhancement chips too.',
   'Aperte [L]+[R]+[Y]+[Esquerda] durante o jogo para pausar e abrir um menu por cima: cheats, savestates, slots de save, guias e um trainer de RAM. Funciona também com chips.',
   'Pulsa [L]+[R]+[Y]+[Izquierda] en el juego para pausarlo y abrir un menú encima: cheats, savestates, ranuras de guardado, guías y un trainer de RAM. También con chips.',
   'Drücke [L]+[R]+[Y]+[Links] im Spiel, um es zu pausieren und ein Menü darüber zu öffnen: Cheats, Savestates, Speicherplätze, Anleitungen und ein RAM-Trainer. Auch mit Chips.',
   'Appuie sur [L]+[R]+[Y]+[Gauche] en jeu pour le mettre en pause et ouvrir un menu par-dessus : cheats, savestates, emplacements, guides et un trainer RAM. Marche aussi avec les puces.',
   'Premi [L]+[R]+[Y]+[Sinistra] in gioco per metterlo in pausa e aprire un menu sopra: cheats, savestate, slot di salvataggio, guide e un trainer RAM. Anche con i chip.',
   'В игре нажми [L]+[R]+[Y]+[Влево], чтобы приостановить её и открыть меню: читы, сейвстейты, слоты сохранений, руководства и трейнер RAM. Работает и с чипами.',
   'Druk [L]+[R]+[Y]+[Links] tijdens het spel om te pauzeren en een menu erover te openen: cheats, savestates, opslagplekken, gidsen en een RAM-trainer. Ook met chips.'),
  # 8 savestates
  ('Save the whole game at any moment and load it back later, in 4 slots per game, even in games with special chips. In a game, [Start]+[R] saves and [Start]+[L] loads. Pick the slot in the in-game menu.',
   'Salve o jogo inteiro a qualquer momento e volte depois, em 4 slots por jogo, até em jogos com chips. No jogo, [Start]+[R] salva e [Start]+[L] carrega. O slot se escolhe no menu in-game.',
   'Guarda el juego entero en cualquier momento y vuelve luego, en 4 ranuras por juego, incluso con chips. En el juego, [Start]+[R] guarda y [Start]+[L] carga. La ranura se elige en el menú del juego.',
   'Speichere das ganze Spiel jederzeit und lade es später, in 4 Plätzen pro Spiel, auch mit Spezialchips. Im Spiel sichert [Start]+[R], [Start]+[L] lädt. Den Platz wählst du im Ingame-Menü.',
   "Sauvegarde tout le jeu à tout moment et reviens-y plus tard, 4 emplacements par jeu, même avec les puces. En jeu, [Start]+[R] sauve et [Start]+[L] charge. L'emplacement se choisit dans le menu en jeu.",
   "Salva l'intero gioco in qualsiasi momento e riprendilo dopo, 4 slot per gioco, anche con i chip. In gioco [Start]+[R] salva e [Start]+[L] carica. Lo slot si sceglie nel menu in gioco.",
   'Сохраняй игровой процесс в любой момент и продолжай позже, 4 слота на игру, даже с чипами. В игре [Start]+[R] сохраняет, [Start]+[L] загружает. Слот выбирается в "меню в игре".',
   'Sla het hele spel op elk moment op en laad het later, 4 plekken per spel, ook met speciale chips. In het spel slaat [Start]+[R] op en laadt [Start]+[L]. Kies de plek in het in-game menu.'),
  # 9 MSU-1 folders
  ('A folder with one game and its MSU-1 audio (.msu and .pcm tracks) acts as the game itself: [A] on the folder starts it and its cover shows in the list. The tracks are listed after the ROM.',
   'Uma pasta com um jogo e o áudio MSU-1 dele (.msu e faixas .pcm) vira o próprio jogo: [A] na pasta inicia o jogo e a capa aparece na lista. As faixas ficam listadas depois da ROM.',
   'Una carpeta con un juego y su audio MSU-1 (.msu y pistas .pcm) actúa como el juego: [A] en la carpeta lo inicia y su carátula sale en la lista. Las pistas van tras la ROM.',
   'Ein Ordner mit einem Spiel und seinem MSU-1-Audio (.msu und .pcm-Tracks) verhält sich wie das Spiel: [A] startet es, das Cover steht in der Liste. Tracks kommen nach der ROM.',
   "Un dossier avec un jeu et son audio MSU-1 (.msu et pistes .pcm) devient le jeu : [A] sur le dossier le lance et sa jaquette s'affiche. Les pistes sont listées après la ROM.",
   'Una cartella con un gioco e il suo audio MSU-1 (.msu e tracce .pcm) diventa il gioco: [A] sulla cartella lo avvia e la copertina appare nella lista. Le tracce vanno dopo la ROM.',
   'Папка с игрой и её аудио MSU-1 (.msu и треки .pcm) запускается как игра: кнопка [A] на папке запускает её, обложка видна в списке. Треки идут после ROM.',
   'Een map met een spel en zijn MSU-1-audio (.msu en .pcm-nummers) gedraagt zich als het spel: [A] op de map start het en de hoes staat in de lijst. Nummers staan na de ROM.'),
  # 10 smart reset
  ("What the console's [RESET] button does during a game. {off}: resets the game. {menu}: back to the menu. {folder}: to the game's folder. {rom}: the folder with the game selected. {hold}: a tap resets, holding goes back.",
   'O que o botão [RESET] do console faz durante o jogo. {off}: reinicia o jogo. {menu}: volta ao menu. {folder}: vai para a pasta do jogo. {rom}: a pasta com o jogo selecionado. {hold}: toque reinicia, segurar volta.',
   'Qué hace el botón [RESET] de la consola en un juego. {off}: reinicia el juego. {menu}: vuelve al menú. {folder}: a la carpeta del juego. {rom}: la carpeta con el juego elegido. {hold}: tocar reinicia, mantener vuelve.',
   'Was die [RESET]-Taste im Spiel macht. {off}: startet das Spiel neu. {menu}: zurück ins Menü. {folder}: in den Ordner des Spiels. {rom}: der Ordner, Spiel markiert. {hold}: Tippen startet neu, Halten geht zurück.',
   'Ce que fait le bouton [RESET] de la console en jeu. {off} : relance le jeu. {menu} : retour au menu. {folder} : au dossier du jeu. {rom} : le dossier, jeu sélectionné. {hold} : appui court relance, long revient.',
   'Cosa fa il tasto [RESET] della console in gioco. {off}: riavvia il gioco. {menu}: torna al menu. {folder}: alla cartella del gioco. {rom}: la cartella col gioco scelto. {hold}: tocco riavvia, tenuto torna.',
   'Что делает кнопка [RESET] во время игры. {off}: перезапуск игры. {menu}: выход в меню. {folder}: в папку игры. {rom}: в папку с выбранной игрой. {hold}: короткое нажатие - перезапуск, долгое - выход.',
   'Wat de [RESET]-knop tijdens een spel doet. {off}: herstart het spel. {menu}: terug naar het menu. {folder}: naar de map van het spel. {rom}: de map met het spel gekozen. {hold}: tik herstart, vasthouden gaat terug.'),
  # 11 "and more" (the base tour's last card)
  ("",) * 8,
  # 12 the 2.17 section: what is new in this release
  ("Version 2.17 adds a Game Boy Color core, the ST011 and ST018 chips, copy-protected bootleg carts, Dutch as the menu's eighth language and menu music that now comes with the firmware, playing a random track by default. The next cards show the rest.",
   'A versão 2.17 traz um core de Game Boy Color, os chips ST011 e ST018, cartuchos piratas com proteção, o holandês como oitavo idioma do menu e música de menu que já vem com a firmware, tocando uma trilha aleatória por padrão. Os próximos cards mostram o resto.',
   'La versión 2.17 trae un core de Game Boy Color, los chips ST011 y ST018, cartuchos piratas con protección, el neerlandés como octavo idioma del menú y música de menú que ya viene con el firmware, con una pista aleatoria por defecto. Las próximas pantallas muestran el resto.',
   'Version 2.17 bringt einen Game-Boy-Color-Core, die Chips ST011 und ST018, kopiergeschützte Bootleg-Module, Niederländisch als achte Menüsprache und Menümusik, die jetzt mit der Firmware kommt und standardmäßig zufällig spielt. Die nächsten Karten zeigen den Rest.',
   'La version 2.17 apporte un core Game Boy Color, les puces ST011 et ST018, les cartouches pirates protégées, le néerlandais comme huitième langue du menu et une musique de menu fournie avec le firmware, en lecture aléatoire par défaut. Les écrans suivants montrent le reste.',
   "La versione 2.17 porta un core Game Boy Color, i chip ST011 e ST018, le cartucce pirata protette, l'olandese come ottava lingua del menu e musica del menu inclusa nel firmware, con una traccia casuale di default. Le prossime schede mostrano il resto.",
   'В версии 2.17: ядро Game Boy Color, чипы ST011 и ST018, пиратские картриджи с защитой, нидерландский как восьмой язык меню и музыка меню, которая теперь идёт с прошивкой и по умолчанию играет случайный трек. Дальше остальные новинки.',
   'Versie 2.17 brengt een Game Boy Color-core, de chips ST011 en ST018, beveiligde bootleg-cartridges, Nederlands als achtste menutaal en menumuziek die nu bij de firmware zit, standaard met een willekeurig nummer. De volgende kaarten tonen de rest.'),
  # 13 controller 2 (2.17)
  ('The in-game shortcuts (menu, savestates, back to the menu) also work from controller 2, so the second player can use them too. Controller 1 keeps priority.',
   'Os atalhos in-game (menu, savestates, voltar ao menu) também funcionam no controle 2, então o segundo jogador também pode usar. O controle 1 tem prioridade.',
   'Los atajos del juego (menú, savestates, volver al menú) también funcionan en el mando 2, así el segundo jugador puede usarlos. El mando 1 tiene prioridad.',
   'Die Ingame-Kürzel (Menü, Savestates, zurück zum Menü) gehen auch an Controller 2, so kann sie auch Spieler 2 nutzen. Controller 1 hat Vorrang.',
   'Les raccourcis en jeu (menu, savestates, retour au menu) marchent aussi sur la manette 2, pour le second joueur. La manette 1 reste prioritaire.',
   'Le scorciatoie in gioco (menu, savestate, ritorno al menu) funzionano anche col controller 2, così le usa anche il secondo giocatore. Il controller 1 ha la precedenza.',
   'Игровые комбинации (меню, сейвстейты, выход в меню) работают и на контроллере 2, их может использовать второй игрок. Приоритет у контроллера 1.',
   'De in-game sneltoetsen (menu, savestates, terug naar menu) werken ook op controller 2, dus ook de tweede speler kan ze gebruiken. Controller 1 heeft voorrang.'),
  # 14 Game Boy Color (2.17)
  ('Put a .gbc on the card and it runs on a Game Boy Color core instead of the Super Game Boy, in color and at full speed. {sgbmenu} picks {auto}, {prefsgb} or {prefgbc}. Experimental, FXPAK PRO (Mk.III) only.',
   'Coloque um .gbc no cartão e ele roda num core de Game Boy Color em vez do Super Game Boy, em cores e a toda velocidade. Em {sgbmenu}: {auto}, {prefsgb} ou {prefgbc}. Experimental, só no FXPAK PRO (Mk.III).',
   'Pon un .gbc en la tarjeta y corre en un core de Game Boy Color en vez del Super Game Boy, en color y a toda velocidad. En {sgbmenu}: {auto}, {prefsgb} o {prefgbc}. Experimental, solo en FXPAK PRO (Mk.III).',
   'Lege eine .gbc auf die Karte und sie läuft auf einem Game-Boy-Color-Core statt dem Super Game Boy, in Farbe und mit voller Geschwindigkeit. In {sgbmenu}: {auto}, {prefsgb} oder {prefgbc}. Experimentell, nur FXPAK PRO (Mk.III).',
   'Mets un .gbc sur la carte et il tourne sur un core Game Boy Color au lieu du Super Game Boy, en couleur et à pleine vitesse. Dans {sgbmenu} : {auto}, {prefsgb} ou {prefgbc}. Expérimental, FXPAK PRO (Mk.III) seulement.',
   'Metti un .gbc sulla scheda e gira su un core Game Boy Color invece del Super Game Boy, a colori e a piena velocità. In {sgbmenu}: {auto}, {prefsgb} o {prefgbc}. Sperimentale, solo FXPAK PRO (Mk.III).',
   'Положи .gbc на карту, и он запустится на ядре Game Boy Color вместо Super Game Boy, в цвете и на полной скорости. В {sgbmenu}: {auto}, {prefsgb} или {prefgbc}. Экспериментально, только FXPAK PRO (Mk.III).',
   'Zet een .gbc op de kaart en hij draait op een Game Boy Color-core in plaats van de Super Game Boy, in kleur en op volle snelheid. In {sgbmenu}: {auto}, {prefsgb} of {prefgbc}. Experimenteel, alleen FXPAK PRO (Mk.III).'),
  # 15 the in-game shortcut list (2.17)
  ('In the in-game menu, [SELECT] on the tab bar lists every button shortcut of the game you are playing, with the combos armed for it: open the menu, save and load states, reset, cheats on and off.',
   'No menu in-game, [SELECT] na barra de abas lista todos os atalhos de botão do jogo que você está jogando, com as combinações ativas para ele: abrir o menu, salvar e carregar estados, reset, ligar e desligar cheats.',
   'En el menú del juego, [SELECT] en la barra de pestañas lista todos los atajos del juego actual, con las combinaciones activas para él: abrir el menú, guardar y cargar estados, reset, activar y desactivar cheats.',
   'Im Ingame-Menü zeigt [SELECT] auf der Tab-Leiste alle Tastenkürzel des laufenden Spiels, mit den dafür aktiven Kombinationen: Menü öffnen, Zustände sichern und laden, Reset, Cheats an und aus.',
   "Dans le menu en jeu, [SELECT] sur la barre d'onglets liste tous les raccourcis du jeu en cours, avec les combinaisons actives pour lui : ouvrir le menu, sauver et charger, reset, cheats on et off.",
   'Nel menu in gioco, [SELECT] sulla barra delle schede elenca tutte le scorciatoie del gioco in corso, con le combinazioni attive: aprire il menu, salvare e caricare stati, reset, cheat on e off.',
   'В "меню в игре" [SELECT] на панели вкладок показывает все комбинации кнопок для текущей игры: открыть меню, сохранить и загрузить состояние, сброс, включить и выключить читы.',
   'In het in-game menu toont [SELECT] op de tabbalk alle knopcombinaties voor het huidige spel: menu openen, states opslaan en laden, reset, cheats aan en uit.'),

  # 16 Seta chips and bootlegs (2.17, cores by M2M)
  ('Morita Shougi 1 and 2 now run: the Seta ST011 and ST018 chips need st011.rom and st018.rom in /sd2snes. 18 copy-protected bootleg carts boot from the untouched dump. Cores by M2M.',
   'Morita Shougi 1 e 2 agora rodam: os chips Seta ST011 e ST018 precisam de st011.rom e st018.rom em /sd2snes. 18 bootlegs com proteção contra cópia iniciam do dump original. Cores por M2M.',
   'Morita Shougi 1 y 2 ya funcionan: los chips Seta ST011 y ST018 necesitan st011.rom y st018.rom en /sd2snes. 18 bootlegs con protección anticopia arrancan desde el volcado original. Cores de M2M.',
   'Morita Shougi 1 und 2 laufen jetzt: die Seta-Chips ST011 und ST018 brauchen st011.rom und st018.rom in /sd2snes. 18 kopiergeschützte Bootlegs starten vom unveränderten Dump. Cores von M2M.',
   'Morita Shougi 1 et 2 tournent enfin : les puces Seta ST011 et ST018 demandent st011.rom et st018.rom dans /sd2snes. 18 bootlegs protégés contre la copie démarrent depuis le dump intact. Cores par M2M.',
   'Morita Shougi 1 e 2 ora funzionano: i chip Seta ST011 e ST018 richiedono st011.rom e st018.rom in /sd2snes. 18 bootleg con protezione anticopia partono dal dump originale. Core di M2M.',
   'Morita Shougi 1 и 2 теперь запускаются: чипам Seta ST011 и ST018 нужны st011.rom и st018.rom в /sd2snes. 18 бутлегов с защитой от копирования грузятся из нетронутого дампа. Ядра от M2M.',
   'Morita Shougi 1 en 2 werken nu: de Seta-chips ST011 en ST018 hebben st011.rom en st018.rom in /sd2snes nodig. 18 kopieerbeveiligde bootlegs starten vanaf de originele dump. Cores door M2M.'),
  # 17 Super 20 in 1, Gamars Puzzle and .sfrom (2.17, by terminator2k2)
  ('The Super 20 in 1 multicart opens its own game menu, Gamars Puzzle runs, and .sfrom files from the SNES Classic load like any ROM. By terminator2k2.',
   'O multicart Super 20 in 1 abre o próprio menu de jogos, o Gamars Puzzle roda e arquivos .sfrom do SNES Classic carregam como qualquer ROM. Por terminator2k2.',
   'El multicart Super 20 in 1 abre su propio menú de juegos, Gamars Puzzle funciona y los archivos .sfrom del SNES Classic cargan como cualquier ROM. Por terminator2k2.',
   'Das Multicart Super 20 in 1 öffnet sein eigenes Spielemenü, Gamars Puzzle läuft, und .sfrom-Dateien vom SNES Classic laden wie jede ROM. Von terminator2k2.',
   'Le multicart Super 20 in 1 ouvre son propre menu de jeux, Gamars Puzzle tourne et les fichiers .sfrom de la SNES Classic se lancent comme une ROM. Par terminator2k2.',
   'Il multicart Super 20 in 1 apre il suo menu di giochi, Gamars Puzzle funziona e i file .sfrom dello SNES Classic si caricano come una ROM. Di terminator2k2.',
   'Мультикартридж Super 20 in 1 открывает своё меню игр, Gamars Puzzle работает, а файлы .sfrom от SNES Classic грузятся как обычные ROM. Автор terminator2k2.',
   'De multicart Super 20 in 1 opent zijn eigen spelmenu, Gamars Puzzle draait en .sfrom-bestanden van de SNES Classic laden als elke ROM. Door terminator2k2.'),
  # 18 file-type icons in the list (2.17)
  ('Every row of the file list starts with an icon of its type: SNES game, NES, Master System, Game Boy, Atari 2600, music, theme, folder. A folder that opens as an MSU-1 game shows a yellow controller.',
   'Cada linha da lista começa com um ícone do tipo: jogo de SNES, NES, Master System, Game Boy, Atari 2600, música, tema, pasta. Uma pasta que abre como jogo MSU-1 mostra um controle amarelo.',
   'Cada fila de la lista empieza con un icono de su tipo: juego de SNES, NES, Master System, Game Boy, Atari 2600, música, tema, carpeta. Una carpeta que abre como juego MSU-1 muestra un mando amarillo.',
   'Jede Zeile der Liste beginnt mit einem Symbol ihres Typs: SNES-Spiel, NES, Master System, Game Boy, Atari 2600, Musik, Theme, Ordner. Ein Ordner, der als MSU-1-Spiel startet, zeigt einen gelben Controller.',
   'Chaque ligne de la liste commence par une icône de son type : jeu SNES, NES, Master System, Game Boy, Atari 2600, musique, thème, dossier. Un dossier qui se lance comme un jeu MSU-1 montre une manette jaune.',
   'Ogni riga della lista inizia con una icona del suo tipo: gioco SNES, NES, Master System, Game Boy, Atari 2600, musica, tema, cartella. Una cartella che si apre come gioco MSU-1 mostra un controller giallo.',
   'Каждая строка списка начинается со значка типа: игра SNES, NES, Master System, Game Boy, Atari 2600, музыка, тема, папка. Папка, которая открывается как игра MSU-1, показывает жёлтый контроллер.',
   'Elke regel van de lijst begint met een pictogram van het type: SNES-spel, NES, Master System, Game Boy, Atari 2600, muziek, thema, map. Een map die als MSU-1-spel opent, toont een gele controller.'),
  # 19 the cheat list from the game info card (2.17)
  ("[SELECT] on the game info card opens that game's cheat list: turn codes on and off, add or edit them, then start the game with them already set.",
   '[SELECT] na ficha do jogo abre a lista de cheats dele: ligue e desligue códigos, adicione ou edite, e inicie o jogo com eles já prontos.',
   '[SELECT] en la ficha del juego abre su lista de cheats: activa y desactiva códigos, añade o edita, y empieza el juego con ellos listos.',
   '[SELECT] auf der Infokarte öffnet die Cheat-Liste des Spiels: Codes an- und ausschalten, hinzufügen oder ändern, dann mit ihnen starten.',
   '[SELECT] sur la fiche du jeu ouvre sa liste de cheats : active ou désactive les codes, ajoute ou modifie, puis lance le jeu avec eux.',
   '[SELECT] sulla scheda del gioco apre la sua lista di cheat: attiva e disattiva i codici, aggiungi o modifica, poi avvia il gioco con quelli pronti.',
   '[SELECT] в окне "Об игре" открывает список читов этой игры: включай и выключай коды, добавляй или меняй их и запускай игру уже с ними.',
   '[SELECT] op de spelinfo opent de cheatlijst van het spel: zet codes aan en uit, voeg toe of wijzig, en start het spel er direct mee.'),

  # 20 themes (an "and more" item)
  ("Change the menu's logo, colours and background gradient. Pick a .thm file in the list to apply it. Make your own in the Theme Creator or get one from the online gallery.",
   'Troque o logo, as cores e o gradiente de fundo do menu. Escolha um arquivo .thm na lista para aplicar. Crie o seu no Theme Creator ou pegue um na galeria do site.',
   'Cambia el logo, los colores y el degradado del fondo del menú. Elige un archivo .thm en la lista para aplicarlo. Crea el tuyo en el Theme Creator o baja uno de la galería.',
   'Ändere Logo, Farben und Hintergrundverlauf des Menüs. Wähle eine .thm-Datei in der Liste, um es anzuwenden. Eigene im Theme Creator oder aus der Online-Galerie.',
   "Change le logo, les couleurs et le dégradé de fond du menu. Choisis un fichier .thm dans la liste pour l'appliquer. Crée le tien dans le Theme Creator ou prends-en un dans la galerie.",
   'Cambia logo, colori e sfumatura di sfondo del menu. Scegli un file .thm nella lista per applicarlo. Crea il tuo nel Theme Creator o prendine uno dalla galleria online.',
   'Меняй логотип, цвета и градиент фона меню. Выбери файл .thm в списке, чтобы применить. Создай свою тему в Theme Creator или возьми из галереи на сайте.',
   'Wijzig logo, kleuren en achtergrondverloop van het menu. Kies een .thm-bestand in de lijst om het toe te passen. Maak je eigen in de Theme Creator of haal er een uit de galerij.'),
  # 21 cheats and trainer
  ('Game Genie and Pro Action Replay codes per game: turn them on in the [Y] menu or in the in-game menu, and edit or add codes right on the console. The trainer finds a value in RAM (lives, time) and freezes it.',
   'Códigos Game Genie e Pro Action Replay por jogo: ligue no menu do [Y] ou no menu in-game, e edite ou crie códigos no próprio console. O trainer acha um valor na RAM (vidas, tempo) e congela.',
   'Códigos Game Genie y Pro Action Replay por juego: actívalos en el menú [Y] o en el menú del juego, y edita o crea códigos en la consola. El trainer busca un valor en la RAM (vidas, tiempo) y lo congela.',
   'Game-Genie- und Pro-Action-Replay-Codes pro Spiel: im [Y]-Menü oder Ingame-Menü einschalten, Codes direkt an der Konsole bearbeiten. Der Trainer findet einen Wert im RAM (Leben, Zeit) und friert ihn ein.',
   'Codes Game Genie et Pro Action Replay par jeu : active-les dans le menu [Y] ou le menu en jeu, et modifie ou crée des codes sur la console. Le trainer trouve une valeur en RAM (vies, temps) et la fige.',
   'Codici Game Genie e Pro Action Replay per gioco: attivali dal menu [Y] o dal menu in gioco, e modifica o crea codici sulla console. Il trainer trova un valore nella RAM (vite, tempo) e lo blocca.',
   'Коды Game Genie и Pro Action Replay для каждой игры: включай в меню кнопкой [Y] или в "меню в игре", редактируй и добавляй прямо на консоли. Трейнер находит значение в RAM (жизни, время) и замораживает его.',
   'Game Genie- en Pro Action Replay-codes per spel: zet ze aan in het [Y]-menu of in-game menu, en bewerk of maak codes op de console. De trainer vindt een waarde in het RAM (levens, tijd) en bevriest die.'),
  # 22 patches
  ('IPS and BPS patches next to a ROM are applied when it loads: translations, hacks and fixes, without changing the original file. With more than one, [A] on the game lets you pick.',
   'Patches IPS e BPS ao lado da ROM são aplicados ao carregar: traduções, hacks e correções, sem mudar o arquivo original. Com mais de um, [A] no jogo deixa escolher.',
   'Los parches IPS y BPS junto a una ROM se aplican al cargarla: traducciones, hacks y arreglos, sin cambiar el archivo original. Con varios, [A] en el juego te deja elegir.',
   'IPS- und BPS-Patches neben einer ROM werden beim Laden angewendet: Übersetzungen, Hacks und Fixes, ohne die Originaldatei zu ändern. Bei mehreren wählst du mit [A].',
   "Les patchs IPS et BPS à côté d'une ROM s'appliquent au chargement : traductions, hacks et correctifs, sans modifier le fichier d'origine. S'il y en a plusieurs, [A] sur le jeu permet de choisir.",
   'Le patch IPS e BPS accanto a una ROM si applicano al caricamento: traduzioni, hack e correzioni, senza modificare il file originale. Se sono più di una, [A] sul gioco fa scegliere.',
   'Патчи IPS и BPS рядом с ROM применяются при загрузке: переводы, хаки и исправления без изменения исходного файла. Если их несколько, кнопка [A] на игре даёт выбрать нужный.',
   'IPS- en BPS-patches naast een ROM worden bij het laden toegepast: vertalingen, hacks en fixes, zonder het originele bestand te wijzigen. Bij meerdere kies je met [A].'),
  # 23 other consoles
  ('NES, Master System, Game Boy Color and Atari 2600 games run on their own FPGA cores, on FXPAK PRO (Mk.III) only. Put the ROM on the card and open it in the list like any game.',
   'Jogos de NES, Master System, Game Boy Color e Atari 2600 rodam em cores de FPGA próprios, só no FXPAK PRO (Mk.III). Ponha a ROM no cartão e abra na lista como qualquer jogo.',
   'Juegos de NES, Master System, Game Boy Color y Atari 2600 corren en sus propios cores FPGA, solo en FXPAK PRO (Mk.III). Pon la ROM en la tarjeta y ábrela como cualquier juego.',
   'NES-, Master-System-, Game-Boy-Color- und Atari-2600-Spiele laufen auf eigenen FPGA-Cores, nur auf FXPAK PRO (Mk.III). ROM auf die Karte und wie jedes Spiel öffnen.',
   'Les jeux NES, Master System, Game Boy Color et Atari 2600 tournent sur leurs propres cores FPGA, sur FXPAK PRO (Mk.III) seulement. Mets la ROM sur la carte et ouvre-la comme un jeu.',
   'I giochi NES, Master System, Game Boy Color e Atari 2600 girano su core FPGA dedicati, solo su FXPAK PRO (Mk.III). Metti la ROM sulla scheda e aprila come qualsiasi gioco.',
   'Игры NES, Master System, Game Boy Color и Atari 2600 идут на своих ядрах FPGA, только на FXPAK PRO (Mk.III). Положи ROM на карту и открой как любую игру.',
   'NES-, Master System-, Game Boy Color- en Atari 2600-spellen draaien op eigen FPGA-cores, alleen op FXPAK PRO (Mk.III). Zet de ROM op de kaart en open hem als elk ander spel.'),
  # 24 more special chips
  ('More enhancement chips work: SPC7110 with its clock, Super FX 3, Sufami Turbo and the competition carts (Campus Challenge 92, PowerFest 94). Some need a BIOS file in /sd2snes.',
   'Mais chips especiais funcionam: SPC7110 com relógio, Super FX 3, Sufami Turbo e os cartuchos de competição (Campus Challenge 92, PowerFest 94). Alguns pedem um BIOS em /sd2snes.',
   'Funcionan más chips especiales: SPC7110 con reloj, Super FX 3, Sufami Turbo y los cartuchos de competición (Campus Challenge 92, PowerFest 94). Algunos piden un BIOS en /sd2snes.',
   'Weitere Spezialchips laufen: SPC7110 mit Uhr, Super FX 3, Sufami Turbo und die Turniermodule (Campus Challenge 92, PowerFest 94). Manche brauchen ein BIOS in /sd2snes.',
   "D'autres puces marchent : SPC7110 avec horloge, Super FX 3, Sufami Turbo et les cartouches de tournoi (Campus Challenge 92, PowerFest 94). Certaines demandent un BIOS dans /sd2snes.",
   'Funzionano altri chip speciali: SPC7110 con orologio, Super FX 3, Sufami Turbo e le cartucce da torneo (Campus Challenge 92, PowerFest 94). Alcuni vogliono un BIOS in /sd2snes.',
   'Работают новые спецчипы: SPC7110 с часами, Super FX 3, Sufami Turbo и турнирные картриджи (Campus Challenge 92, PowerFest 94). Некоторым нужен BIOS в /sd2snes.',
   'Meer speciale chips werken: SPC7110 met klok, Super FX 3, Sufami Turbo en de toernooicartridges (Campus Challenge 92, PowerFest 94). Sommige hebben een BIOS in /sd2snes nodig.'),
  # 25 memory test
  ("Checks the cartridge's memory right on the console, from the main menu ([X]). The wiring test is quick, the full test takes about 30 seconds. Both reset the console at the end.",
   'Testa a memória do cartucho no próprio console, pelo menu principal ([X]). O teste de fiação é rápido, o completo leva uns 30 segundos. Os dois reiniciam o console no fim.',
   'Prueba la memoria del cartucho en la consola, desde el menú principal ([X]). La prueba de cableado es rápida, la completa tarda unos 30 segundos. Ambas reinician la consola.',
   'Prüft den Speicher des Moduls direkt an der Konsole, im Hauptmenü ([X]). Der Leitungstest ist schnell, der volle Test dauert etwa 30 Sekunden. Beide starten die Konsole neu.',
   'Teste la mémoire de la cartouche sur la console, depuis le menu principal ([X]). Le test de câblage est rapide, le complet prend environ 30 secondes. Les deux redémarrent la console.',
   'Verifica la memoria della cartuccia sulla console, dal menu principale ([X]). Il test del cablaggio è rapido, quello completo dura circa 30 secondi. Entrambi riavviano la console.',
   'Проверяет память картриджа прямо на консоли, из главного меню кнопкой [X]. Быстрый тест линий - 10 секунд и полный - около 30 секунд. Оба в конце перезапускают консоль.',
   'Test het geheugen van de cartridge op de console, via het hoofdmenu ([X]). De bedradingstest is snel, de volledige duurt ongeveer 30 seconden. Beide herstarten de console.'),
  # 26 4 battery saves per game
  ('Every game has 4 battery-save slots, so two people can each keep their own. Pick the slot in the {saves} tab of the in-game menu: it applies on the next boot of the game.',
   'Cada jogo tem 4 slots de save de bateria, para duas pessoas terem cada uma o seu. O slot se escolhe na aba {saves} do menu in-game e vale no próximo boot do jogo.',
   'Cada juego tiene 4 ranuras de guardado de batería, para que dos personas tengan cada una la suya. La ranura se elige en la pestaña {saves} del menú del juego y vale en el próximo arranque.',
   'Jedes Spiel hat 4 Batterie-Speicherplätze, damit zwei Leute je ihren eigenen haben. Den Platz wählst du im Tab {saves} des Ingame-Menüs, er gilt beim nächsten Spielstart.',
   "Chaque jeu a 4 emplacements de sauvegarde, pour que deux personnes aient chacune la sienne. L'emplacement se choisit dans l'onglet {saves} du menu en jeu et vaut au prochain démarrage.",
   'Ogni gioco ha 4 slot di salvataggio a batteria, così due persone hanno ognuna il suo. Lo slot si sceglie nella scheda {saves} del menu in gioco e vale al prossimo avvio.',
   'У каждой игры 4 слота сохранения с батарейкой, чтобы у двух человек было своё. Слот выбирается на вкладке {saves} "меню в игре" и действует со следующего запуска.',
   'Elk spel heeft 4 batterij-opslagplekken, zodat twee mensen elk hun eigen hebben. Kies de plek in de tab {saves} van het in-game menu, hij geldt bij de volgende start.'),
  # 27 MSU-1 tracks in the menu
  ("The .pcm tracks of MSU-1 games show up in the file list. [A] on one opens a player with a progress bar and the elapsed and total time, [A] pauses, [B] closes. Handy to check a game's soundtrack before playing.",
   'As trilhas .pcm dos jogos MSU-1 aparecem na lista de arquivos. [A] numa delas abre um tocador com barra de progresso e tempo decorrido e total, [A] pausa, [B] fecha. Bom para conferir a trilha antes de jogar.',
   'Las pistas .pcm de los juegos MSU-1 aparecen en la lista. [A] en una abre un reproductor con barra de progreso y tiempo transcurrido y total, [A] pausa, [B] cierra. Útil para revisar la banda sonora antes de jugar.',
   'Die .pcm-Tracks von MSU-1-Spielen stehen in der Dateiliste. [A] auf einem öffnet einen Player mit Fortschrittsbalken und Zeitanzeige, [A] pausiert, [B] schließt. Praktisch, um den Soundtrack vorher zu prüfen.',
   "Les pistes .pcm des jeux MSU-1 apparaissent dans la liste. [A] sur l'une ouvre un lecteur avec barre de progression et temps écoulé et total, [A] met en pause, [B] ferme. Pratique pour vérifier la bande-son.",
   'Le tracce .pcm dei giochi MSU-1 compaiono nella lista. [A] su una apre un lettore con barra di avanzamento e tempo trascorso e totale, [A] mette in pausa, [B] chiude. Utile per controllare la colonna sonora.',
   'Треки .pcm игр MSU-1 видны в списке файлов. Кнопка [А] на треке открывает плеер с полосой прогресса и временем. В плеере кнопка [А] - пауза, а кнопка [В] - закрытие. Удобно проверить саундтрек перед игрой.',
   'De .pcm-nummers van MSU-1-spellen staan in de bestandslijst. [A] op een nummer opent een speler met voortgangsbalk en tijd, [A] pauzeert, [B] sluit. Handig om de soundtrack vooraf te checken.'),
  # 28 BS-X and the Memory Pack
  ('Satellaview games that use the 8M Memory Pack slot read it from /sd2snes/saves as <rom>.mpk: saved sound novels, downloaded races and the like. The game still boots without a pack.',
   'Jogos do Satellaview que usam o slot de Memory Pack de 8M leem o pack de /sd2snes/saves como <rom>.mpk: sound novels salvas, corridas baixadas e afins. O jogo boota mesmo sem o pack.',
   'Los juegos de Satellaview que usan el slot de Memory Pack de 8M lo leen de /sd2snes/saves como <rom>.mpk: sound novels guardadas, carreras descargadas y más. El juego arranca incluso sin pack.',
   'Satellaview-Spiele mit 8M-Memory-Pack-Slot lesen das Pack aus /sd2snes/saves als <rom>.mpk: gespeicherte Sound Novels, geladene Rennen und mehr. Das Spiel startet auch ohne Pack.',
   'Les jeux Satellaview qui utilisent le slot Memory Pack 8M le lisent dans /sd2snes/saves en <rom>.mpk : sound novels sauvegardés, courses téléchargées... Le jeu démarre même sans pack.',
   'I giochi Satellaview che usano lo slot Memory Pack da 8M lo leggono da /sd2snes/saves come <rom>.mpk: sound novel salvate, corse scaricate e simili. Il gioco parte anche senza pack.',
   'Игры Satellaview со слотом Memory Pack 8M читают его из /sd2snes/saves как <rom>.mpk: сохранённые звуковые новеллы, скачанные гонки и прочее. Игра запускается и без Memory Pack.',
   'Satellaview-spellen met een 8M Memory Pack-slot lezen het pack uit /sd2snes/saves als <rom>.mpk: opgeslagen sound novels, gedownloade races en meer. Het spel start ook zonder pack.'),
  # 29 delete files and saves
  ("From the [Y] menu of the file list, {del} erases the selected file and {delsrm} only the ROM's save, right on the console. Deleting a ROM also takes its cover, info card, guides and cheats along.",
   'No menu do [Y] da lista de arquivos, {del} apaga o arquivo selecionado e {delsrm} só o save da ROM, no próprio console. Apagar uma ROM leva junto a capa, a ficha, as guias e os cheats dela.',
   'Desde el menú [Y] de la lista, {del} borra el archivo elegido y {delsrm} solo el save de la ROM, en la consola. Borrar una ROM también se lleva su carátula, ficha, guías y cheats.',
   'Im [Y]-Menü der Dateiliste löscht {del} die gewählte Datei und {delsrm} nur den Spielstand der ROM, direkt an der Konsole. Eine gelöschte ROM nimmt Cover, Infokarte, Anleitungen und Cheats mit.',
   'Depuis le menu [Y] de la liste, {del} efface le fichier choisi et {delsrm} seulement la sauvegarde de la ROM, sur la console. Supprimer une ROM emporte aussi sa jaquette, sa fiche, ses guides et ses cheats.',
   'Dal menu [Y] della lista, {del} cancella il file scelto e {delsrm} solo il salvataggio della ROM, direttamente sulla console. Eliminare una ROM porta via anche copertina, scheda, guide e cheat.',
   'В списке файлов при нажатии кнопки [Y] пункт {del} удаляет выбранный файл игры, а {delsrm} только файл сохранения игры, прямо на консоли. Вместе с игрой удаляются её обложка, карточка, руководства и читы.',
   'Vanuit het [Y]-menu van de lijst wist {del} het gekozen bestand en {delsrm} alleen de save van de ROM, gewoon op de console. Een gewiste ROM neemt ook hoes, spelinfo, gidsen en cheats mee.'),
  # 30 missing BIOS warning
  ('Games with special chips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) need support files on the card. If one is missing, the menu names the file and stays up, instead of freezing or starting a broken game.',
   'Jogos com chips especiais (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) precisam de arquivos de apoio no cartão. Se faltar algum, o menu diz qual é e continua de pé, em vez de travar ou abrir um jogo quebrado.',
   'Los juegos con chips especiales (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) necesitan archivos de apoyo en la tarjeta. Si falta uno, el menú dice cuál y sigue en pie, en vez de colgarse o abrir un juego roto.',
   'Spiele mit Spezialchips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) brauchen Hilfsdateien auf der Karte. Fehlt eine, nennt das Menü die Datei und bleibt stehen, statt zu hängen oder ein kaputtes Spiel zu starten.',
   "Les jeux à puces spéciales (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) ont besoin de fichiers sur la carte. S'il en manque un, le menu dit lequel et reste ouvert, au lieu de planter ou de lancer un jeu cassé.",
   'I giochi con chip speciali (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) vogliono file di supporto sulla scheda. Se ne manca uno, il menu dice quale e resta attivo, invece di bloccarsi o avviare un gioco rotto.',
   'Играм с особыми чипами (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) нужны файлы BIOS на карте. Если какого-то нет, меню показывает его название и остаётся открытым, а не зависает и не запускает сломанную игру.',
   'Spellen met speciale chips (DSP, Super FX, SA-1, S-DD1, CX4, Super Game Boy, BS-X...) hebben hulpbestanden op de kaart nodig. Ontbreekt er een, dan noemt het menu het bestand en blijft het open, in plaats van te hangen.'),
  # 31 a description for each option
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
        if ch in ENCODE or (" " <= ch <= "~"):
            n += 1
        else:
            sys.exit("gen_onb_lang: no font tile for %r in %r" % (ch, text))
    return n


TEXT_W = 30          # == ONB_TEXT_W: cols 1..30
MORE_FIRST = 19      # == ONB_MORE_FIRST: the "and more" items are the descriptors from here
MORE_NAME_W = 24     # an answer row's width (onb_draw_option_value prints labels with 24)
TEXT_MAX_LINES = 11  # the answer list starts 2 rows below the text and the longest
                     # list (8 languages) has to fit above the progress bar
_LABELS = {"off": "onb_text_off", "on": "onb_text_on", "menu": "onb_text_rst_menu", "large": "onb_text_large",
           "small": "onb_text_small", "ctx": "onb_text_gi_ctx", "folder": "onb_text_rst_folder",
           "rom": "onb_text_rst_game", "hold": "onb_text_rst_hold"}
# the menu's own words, as its dictionaries have them (so a paragraph names what the
# menu shows)
_MENU_LABELS = {"cfg": "mtext_mm_cfg", "sgbmenu": "mtext_cfg_sgb", "auto": "text_auto",
                "prefsgb": "text_gbc_prefer_sgb", "prefgbc": "text_gbc_prefer_gbc",
                "saves": "text_igm_tab_saves", "del": "text_filesel_context_delete_file",
                "delsrm": "text_filesel_context_delete_srm"}
_MENU_WORDS = {key: menu_text(lab) for key, lab in _MENU_LABELS.items()}


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
    return ", ".join(toks)


def _wrap(text, width):
    lines, cur = [], ""
    for word in text.split():
        cand = (cur + " " + word) if cur else word
        if _cells(cand) <= width:
            cur = cand
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


def main():
    out_path = "onb_const_lang.a65"
    if "-o" in sys.argv:
        out_path = sys.argv[sys.argv.index("-o") + 1]

    nlang = len(LANGS)
    L = []
    L.append("; ==========================================================================")
    L.append("; AUTO-GENERATED by utils/gen_onb_lang.py -- DO NOT EDIT BY HAND.")
    L.append("; Onboarding i18n string pool (EN/PT/ES/DE/FR/IT/RU/NL). Edit the STRINGS table in the")
    L.append("; generator and re-run `make` instead.")
    L.append("; ==========================================================================")
    L.append(".link page $c1")
    L.append("")
    L.append("onb_strtab_nlang  .byt %d" % nlang)
    L.append("")
    L.append("; ---- dispatch tables (one row of %d word pointers per label) ----" % nlang)
    L.append("onb_strtab_lo:")
    for label in STRINGS:
        L.append("%s:" % label)
        for lang in LANGS:
            L.append("  .word !%s_%s" % (label, lang))
    L.append("onb_strtab_hi:")
    L.append("")
    L.append("; ---- per-language strings (font-encoded; outside the dispatch range) ----")
    for label, vals in STRINGS.items():
        if len(vals) != nlang:
            sys.exit("label %s has %d values, expected %d" % (label, len(vals), nlang))
        for lang, text in zip(LANGS, vals):
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
                L.append("%s_%s  .byt %s" % (label, lang, ", ".join(toks)))
                continue
            if _cells(text) > _budget(label):
                sys.exit("gen_onb_lang: %s[%s] is %d cells, budget %d: %r"
                         % (label, lang, _cells(text), _budget(label), text))
            L.append("%s_%s  .byt %s" % (label, lang, encode_string(text)))

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    print("generated %s: %d labels x %d languages" % (out_path, len(STRINGS), nlang))


if __name__ == "__main__":
    main()
