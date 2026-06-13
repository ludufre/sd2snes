<h1> sd2snes+</h1>

<img src="gfx/showcase.gif" width="512" alt="Vorführung">
<img src="gfx/langs.gif" width="384" alt="Das Hauptmenü auf Englisch, Brasilianischem Portugiesisch, Spanisch und Deutsch">

Ein freundlicheres Firmware-Erlebnis für sd2snes/FXPAK: Sprachen, Spielecover, Menümusik, Patches und ein praktischerer Reset zurück ins Menü.

**🌐 Sprache:** [English 🇺🇸](README.md) · [Português 🇧🇷](README-BR.md) · [Español 🇪🇸](README-ES.md) · Deutsch

**Mehr Informationen:** Die Website [sd2snes.ludufre.com](https://sd2snes.ludufre.com) enthält weitere Details, Anleitungen und visuelle Beispiele zu diesem Fork.

> **Was ist das?**
>
> Dies ist ein Fork der [originalen sd2snes-Firmware](https://github.com/mrehkopf/sd2snes) von [@mrehkopf](https://github.com/mrehkopf). Er behält die originale Firmware-Basis bei und ergänzt nutzerfreundliche Verbesserungen: ein Menü auf Brasilianischem Portugiesisch, Englisch, Spanisch und Deutsch, Spielecover, Menümusik, IPS/BPS-Patchauswahl und bessere Optionen für den Reset zurück ins Menü.
>
> Nutze dieses Repository für Fragen oder Fehler zur **Übersetzung**, zum **Sprachwähler**, zu **Covern**, zur **Menümusik**, zum **Patchwähler** oder zum **Theme-Editor dieses Forks**. Für Kernprobleme der Firmware, die nichts mit diesen Ergänzungen zu tun haben, nutze bitte das Originalprojekt.

## Hier anfangen

Wenn du diese Firmware nur benutzen möchtest, musst du **nichts** kompilieren und keinen Quellcode herunterladen.

Du brauchst:

- Ein sd2snes- oder FXPAK-Modul.
- Eine SD-Karte, die bereits für dein Modul vorbereitet ist.
- Das **full** `.zip` von der [Releases](https://github.com/ludufre/sd2snes/releases)-Seite dieses Repositorys.

> [!NOTE]
> Dieses Projekt enthält keine Spiele/ROMs. Verwende deine eigenen legal erworbenen Dateien.

## Was dieser Fork ergänzt

- **Sprachen:** Wähle Brasilianisches Portugiesisch, Englisch, Spanisch oder Deutsch direkt im Menü.
- **Spielecover:** Zeige das Cover jedes Spiels beim Durchsuchen deiner ROM-Liste.
- **Menümusik:** Spiele einen `.spc`-Titel im Hintergrund ab, während du im Menü navigierst.
- **Menü-Sounds:** optionale Navigations-Soundeffekte (Cursor, Bestätigen, Zurück, Fehler), die über den Audio-DAC des Moduls abgespielt werden, unabhängig von der Musik.
- **IPS/BPS-Patches:** Wähle Übersetzungen, Hacks oder Fix-Patches vor dem Start eines Spiels aus, ohne die ROM-Datei auf der SD-Karte zu verändern.
- **Cheat-Manager:** Die originale sd2snes-Firmware kann Cheats bereits anwenden - dieser Fork ergänzt ein Menü, um Codes eines Spiels direkt auf der Konsole zu **aktivieren und deaktivieren** (aus `/sd2snes/cheats/<rom>.yml`), ohne die YAML-Datei am PC zu bearbeiten. Fertige Cheats können bei [gamehacking.org](https://gamehacking.org/system/snes) als "FXPak Pro 1.7 (.yml)" exportiert oder von der App **sd2snes Covers** automatisch heruntergeladen werden (per CRC32 erkannt).
- **Datei und Spielstand löschen:** Lösche die ausgewählte Datei oder nur ihren Spielstand (`.srm`) direkt aus dem Menü, ohne die SD-Karte zu entfernen.
- **Verbesserter Reset ins Menü:** Kehre nach einem kurzen Reset in denselben Ordner oder sogar zur selben ROM zurück.
- **Themes (Firmware 2.9+):** Wähle ein Menü-Theme - Logo, Farben, Hintergrund und Auswahlbalken - **direkt auf der Konsole** aus jedem Ordner der Karte. Lade fertige Themes aus der [Galerie](https://sd2snes.ludufre.com/gallery/) herunter oder erstelle dein eigenes im [Web-Editor](https://sd2snes.ludufre.com/theme/).

## Installation

Für die einfachste Installation lade die Release herunter, die als **full** markiert ist. Sie enthält bereits alle Firmware-Dateien, die dieser Fork benötigt, daher musst du die entsprechende offizielle Firmware **nicht** vorher herunterladen.

Der Name jeder Release zeigt die passende offizielle Firmware-Version. Zum Beispiel ist **"v2.1 (sd2snes v1.11.2)"** für die offizielle Firmware `v1.11.2` gedacht.

1. Lade das passende **full** `.zip` von der [Releases](https://github.com/ludufre/sd2snes/releases)-Seite dieses Repositorys herunter.
2. Öffne das `.zip` und kopiere seinen Inhalt in das **Hauptverzeichnis deiner SD-Karte**.
3. Ersetze vorhandene Dateien, wenn dein Computer danach fragt.
4. Stecke die SD-Karte wieder in sd2snes/FXPAK und schalte die Konsole ein.

Wenn du stattdessen ein Paket ohne `full` herunterlädst, enthält es nur die Dateien, die dieser Fork geändert hat. Installiere in diesem Fall zuerst die passende offizielle Firmware von [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads) und kopiere danach die Dateien dieses Forks in den Ordner `/sd2snes`, wobei vorhandene Dateien ersetzt werden.

Das Menü startet standardmäßig auf **Englisch**. Du kannst die Sprache jederzeit über die Option **Sprache** im Hauptmenü ändern.

## Benachrichtigungen über neue Versionen

So erhältst du eine GitHub-Benachrichtigung, wenn eine neue Firmware-Version veröffentlicht wird:

1. Öffne dieses Repository auf GitHub.
2. Klicke auf **Watch**.
3. Wähle **Custom**.
4. Wähle **Releases** aus und speichere.

<p>
<img src="gfx/follow1.png" width="256" alt="GitHub-Watch-Menü mit der Option Custom">
<img src="gfx/follow2.png" width="256" alt="Benutzerdefinierte GitHub-Benachrichtigungen mit ausgewählten Releases">
</p>

## Sprachen

<img src="gfx/langs.gif" width="512" alt="Das Hauptmenü auf Englisch, Brasilianischem Portugiesisch, Spanisch und Deutsch">

Das Menü kann in vier Sprachen laufen:

- **Português:** Brasilianisch-portugiesische Übersetzung für Menüs, Meldungen und Bildschirme.
- **English:** die originale und standardmäßige Firmware-Sprache.
- **Español:** Spanische Übersetzung für Menüs, Meldungen und Bildschirme.
- **Deutsch:** Deutsche Übersetzung für Menüs, Meldungen und Bildschirme.

Öffne **Sprache** im Hauptmenü, wähle die gewünschte Sprache, und das Menü wechselt sofort. Deine Auswahl wird für den nächsten Konsolenstart gespeichert.

## Spielecover

Spielecover erscheinen im Menü, während du deine Spiele durchsuchst.

Lege für jede ROM eine Cover-Datei in denselben Ordner wie die ROM. Die Cover-Datei muss denselben Namen wie die ROM haben, aber die Erweiterung `.cov` verwenden:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).cov
```

Der einfachste Weg, `.cov`-Dateien zu erstellen, ist die Cover-Generator-App:

### 👉 [github.com/ludufre/sd2snes-covers](https://github.com/ludufre/sd2snes-covers)

Verwende **sd2snes-covers v1.1.0 oder neuer**. Wenn du Cover mit einer älteren Version erstellt hast, erzeuge sie mit der neuen App erneut.

Im Menü hat **Cover anzeigen** drei Einstellungen: **Gross** (vollständige Box-Art), **Klein** (halbe Größe - praktisch, wenn ein Cover die Dateiliste überdeckt) und **Aus**. **Gross** ist die Voreinstellung, sodass bestehende Setups unverändert bleiben.

## IPS/BPS-Patches

Dieser Fork kann **IPS**- und **BPS**-Patches anwenden, wenn ein Spiel geladen wird. Das ist nützlich für Fan-Übersetzungen, Hacks und Fixes.

Die ROM-Datei auf der SD-Karte wird nicht verändert. Der Patch wird nur beim Laden des Spiels angewendet.

Lege den Patch in denselben Ordner wie die ROM. Der Dateiname muss mit dem ROM-Dateinamen ohne ROM-Erweiterung beginnen und auf `.ips` oder `.bps` enden:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).ips
/sd2snes/A/Aladdin (USA) (Hack).bps
```

Wenn du ein Spiel mit passenden Patches öffnest, zeigt das Menü eine Patchauswahl:

- **`[Kein Patch]`** startet das Spiel normal.
- Wähle einen Patch, um ihn für diesen Start zu verwenden.
- Pro Spiel werden bis zu **8** Patches angezeigt.

## Menümusik und Menü-Sounds

Das Menü kann **Hintergrundmusik** abspielen, während du navigierst - dazu vier optionale **Navigations-Soundeffekte** (Cursor, Bestätigen, Zurück, Fehler). Sie spielen nur im Menü und beeinflussen deine Spiele nie.

Am einfachsten richtest du beides mit dem **Sound Creator** im Web ein: Musik auswählen, Effekte erstellen und die fertigen Dateien für die Karte herunterladen. Alles läuft in deinem Browser - es wird nichts hochgeladen.

### 👉 [sd2snes.ludufre.com/sounds](https://sd2snes.ludufre.com/sounds/)

### Hintergrundmusik (`menu.spc`)

Die Musik ist eine **`.spc`**-Datei mit dem Namen `menu.spc`, hier abgelegt:

```text
/sd2snes/menu.spc
```

So fügst du Musik von Hand hinzu:

1. Lade eine `.spc`-Datei herunter.
2. Benenne sie in `menu.spc` um.
3. Kopiere sie in den Ordner `/sd2snes/` auf deiner SD-Karte.
4. Schalte die Konsole ein.

Gute Orte, um `.spc`-Dateien zu finden:

- [snesmusic.org](https://snesmusic.org)
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc) - mit einer MP3-Vorschau für jeden Titel, sodass du vor dem Herunterladen reinhören kannst.

Du kannst die Musik unter **Einstellungen → Browser-Optionen → Menue-Musik** ein- oder ausschalten.

Du kannst die Musik auch **ohne Umbenennen** wählen: Markiere im Dateibrowser eine beliebige **`.spc`**, drücke **Y** für das Kontextmenü und wähle **Als Menuemusik setzen**. Das Menü lädt mit diesem Titel als neuer Hintergrundmusik neu und merkt sie sich über Neustarts hinweg; `/sd2snes/menu.spc` bleibt als Reserve. Um dorthin zurückzukehren, nutze **Einstellungen → Browser-Optionen → Musik zuruecksetzen**.

> [!TIP]
> Manche Soundtracks werden als `.rsn`-Dateien heruntergeladen. Eine `.rsn` ist normalerweise ein Archiv mit mehreren `.spc`-Dateien. Entpacke sie und wähle eine `.spc` daraus.

### Navigations-Sounds (Effekte)

Vier kurze, optionale Effekte spielen, während du dich durchs Menü bewegst. Jeder ist eine eigene Datei in `/sd2snes/`:

| Datei | Spielt bei |
| --- | --- |
| `sfx_cursor.pcm` | Cursorbewegung |
| `sfx_confirm.pcm` | Öffnen / Bestätigen (A) |
| `sfx_back.pcm` | Zurückgehen (B) |
| `sfx_error.pcm` | einer nicht erlaubten Aktion |

Das sind **MSU‑1-PCM**-Dateien (16 Bit Stereo, 44,1 kHz). Sie werden über den Audio-DAC des Moduls abgespielt und unterbrechen die `.spc`-Musik nie. Ein Standardsatz ist in der Firmware enthalten, das Menü hat also von Haus aus Sounds - nutze den Sound Creator oben, um sie anzupassen oder zu ersetzen. (Eine fehlende Datei bedeutet einfach, dass dieser Effekt stumm bleibt.)

Du kannst die Effekte unter **Einstellungen → Browser-Optionen → Menue-Sounds** ein- oder ausschalten.

## Cheats

Die originale sd2snes-Firmware kann Cheats pro Spiel bereits **anwenden**. Dieser Fork ergänzt einen **Cheat-Manager im Menü**, damit du einzelne Codes direkt auf der Konsole aktivieren und deaktivieren kannst - ohne die YAML-Datei am PC zu bearbeiten.

Cheats werden aus einer **YAML**-Datei (`.yml`) im Ordner `/sd2snes/cheats/` gelesen. Sie ist nach der ROM benannt, wobei die Erweiterung durch `.yml` ersetzt wird:

```text
/sd2snes/A/Aladdin (USA).sfc        ← die ROM (in einem beliebigen Ordner)
/sd2snes/cheats/Aladdin (USA).yml   ← ihre Cheats
```

Zum Verwalten markierst du eine ROM im Dateibrowser, drückst **Y** für das Kontextmenü und wählst **Cheats**. Die Liste zeigt jeden Code aus der Datei:

- **A** aktiviert oder deaktiviert den markierten Code.
- **B** speichert die Änderungen und beendet das Menü.

Aktivierte Codes werden beim nächsten Start dieses Spiels angewendet.

So bekommst du fertige Cheat-Dateien:

1. Öffne [gamehacking.org/system/snes](https://gamehacking.org/system/snes) und suche dein Spiel.
2. Exportiere die Codes im Format **FXPak Pro 1.7 (.yml)**.
3. Benenne die Datei passend zur ROM um und lege sie auf der SD-Karte in `/sd2snes/cheats/` ab.

> [!TIP]
> Die App **[sd2snes Covers](https://github.com/ludufre/sd2snes-covers)** kann fertige Cheats automatisch abrufen - sie erkennt jede ROM per CRC32 und speichert `<rom>.yml`-Dateien in einem `cheats/`-Ordner, bereit zum Kopieren nach `/sd2snes/cheats/`.

> [!NOTE]
> Wenn eine ROM keine `.yml` in `/sd2snes/cheats/` hat oder die Datei keine Codes enthält, zeigt das Menü eine Meldung, dass keine Cheats für diese ROM vorhanden sind.

## Datei und Spielstand löschen

Du kannst Dateien und Spielstände direkt aus dem Menü löschen, ohne die SD-Karte zu entfernen oder einen Computer zu benutzen.

Markiere eine Datei im Browser und drücke **Y**, um das Kontextmenü zu öffnen:

- **Loeschen:** entfernt die ausgewählte Datei.
- **Save loeschen:** entfernt nur den `.srm`-Spielstand dieser ROM und behält die ROM selbst.

> [!WARNING]
> Das Löschen ist dauerhaft - auf der SD-Karte gibt es keinen Papierkorb. Prüfe die ausgewählte Datei vor dem Bestätigen genau.

## Reset ins Menü

Der Reset-Knopf kann dich zurück zum sd2snes-Menü bringen, statt nur das Spiel neu zu starten. Dieser Fork ergänzt zwei Optionen, die die Rückkehr zu deiner Spieleliste erleichtern.

Stelle es unter **Einstellungen → Im-Spiel Optionen → Reset zum Menue** ein:

- **Aus:** Reset verhält sich wie ein normaler SNES-Reset.
- **Ein:** ein kurzer Reset kehrt zum Menü zurück.
- **Ordner:** kehrt zum Menü zurück und öffnet den Ordner des Spiels, das du gespielt hast.
- **ROM:** kehrt zum Ordner zurück und markiert die ROM, die du gespielt hast.

Die Optionen **Ordner** und **ROM** funktionieren nach einem Reset zurück ins Menü. Ein vollständiges Einschalten startet weiterhin im normalen Hauptmenü.

## Themes

Ab Firmware **2.9** kannst du das gesamte Aussehen des Menüs ändern - **Logo, Farben, Hintergrund und Auswahlbalken** - direkt auf der Konsole, ohne PC.

1. Lege `.thm`-Theme-Dateien in **einen beliebigen Ordner** auf deiner SD-Karte. Jeder Name ist möglich, nur der versteckte Ordner `/sd2snes` ist nicht erlaubt.
2. Öffne diesen Ordner im Menü und drücke **A** auf einem Theme. Das Menü lädt mit dem Theme neu.
3. Um zum Standarddesign zurückzukehren, nutze **Einstellungen → Browser-Optionen → Theme zuruecksetzen** im Menü.

Themes bekommst du auf zwei Arten:

**Galerie - fertige Themes mit einem Klick herunterladen:**

### 👉 [sd2snes.ludufre.com/gallery](https://sd2snes.ludufre.com/gallery/)

**Theme Creator - eigenes Theme erstellen:** Lade ein Logo hoch (mit Transparenz), wähle die Farben und lade eine `.thm` herunter.

### 👉 [sd2snes.ludufre.com/theme](https://sd2snes.ludufre.com/theme/)

> [!NOTE]
> Fortgeschritten: Es gibt auch einen [`m3nu.bin`-Editor](https://sd2snes.ludufre.com/theme/), der eine vollständige Menü-Binärdatei patcht (der ältere Ablauf). Der offizielle sd2snes-Theme-Editor unterstützt das Format dieses Forks nicht.

## Häufige Probleme

**Das Menü hat sich nach der Installation nicht geändert.**

Prüfe, ob du die passende **full**-Release verwendet und die Dateien in das Hauptverzeichnis der SD-Karte kopiert hast. Wenn du ein Paket ohne `full` verwendet hast, prüfe, ob zuerst die passende offizielle Firmware installiert wurde und die Dateien dieses Forks nach `/sd2snes` kopiert wurden.

**Cover erscheinen nicht.**

Prüfe, ob Cover aktiviert sind, ob jede `.cov`-Datei denselben Namen wie ihre ROM hat und ob die Cover mit **sd2snes-covers v1.1.0 oder neuer** erzeugt wurden.

**Die Menümusik bleibt stumm.**

Prüfe, ob die Datei exakt `menu.spc` heißt, unter `/sd2snes/menu.spc` liegt und wirklich eine `.spc`-Datei ist. MP3- und WAV-Dateien funktionieren nicht.

**Die Navigations-Sounds bleiben stumm.**

Prüfe, ob die Option **Menue-Sounds** eingeschaltet ist und ob `sfx_cursor.pcm`, `sfx_confirm.pcm`, `sfx_back.pcm` und `sfx_error.pcm` in `/sd2snes/` liegen und **MSU‑1-PCM**-Dateien sind. Sie sind in der Firmware enthalten; falls du sie gelöscht hast, kopiere sie aus dem Release-Paket zurück oder erstelle sie neu mit dem [Sound Creator](https://sd2snes.ludufre.com/sounds/).

**Ein Patch erscheint nicht.**

Prüfe, ob der Patch im selben Ordner wie die ROM liegt, mit dem Namen der ROM beginnt und auf `.ips` oder `.bps` endet.

## Fortgeschrittene Hinweise

Dieser Abschnitt ist hauptsächlich für Entwickler, Maintainer und fortgeschrittene Nutzer gedacht. Für die normale Installation brauchst du ihn nicht.

### Cover-Format

Diese Firmware verwendet ab Version **v1.11.2-br-2.1** das neuere `.cov`-Format mit OBJ-Sprites. Cover, die mit älteren Versionen der Cover-App erzeugt wurden, werden nicht korrekt angezeigt. Erzeuge sie erneut mit **sd2snes-covers v1.1.0 oder neuer**.

### Integritätsprüfung für BPS-Patches

Die Integritätsprüfung für BPS kann unter **Einstellungen → Patch-Optionen → Integritaet pruefen** aktiviert werden.

Diese Option ist standardmäßig **deaktiviert**. Wenn sie aktiviert ist, liest die Firmware die ROM nach dem Anwenden eines BPS-Patches erneut ein, um zu bestätigen, dass er korrekt angewendet wurde. Dadurch wird das Laden von BPS langsamer; ein BPS-Patch mit 4 MB kann zum Beispiel im Durchschnitt etwa 15 Sekunden zusätzliche Ladezeit verursachen. IPS-Patches werden von dieser Option nicht geprüft.

### Einschränkungen von Menümusik und Sounds

Für die Musik werden nur `.spc`-Dateien unterstützt. Eine `.spc`-Datei ist keine normale Audioaufnahme, sondern ein Snapshot des Zustands des SNES-Soundchips und auf 64 KB begrenzt. Es gibt keine direkte Umwandlung von MP3 nach SPC - der Sound Creator lässt dich eine `.spc` auswählen und anhören, erzeugt aber keine aus anderem Audio.

Wenn die Musik beim Start, nach einem Reset oder nach dem Aktivieren der Option geladen wird, kann das Menü kurz pausieren, während die Datei an den Soundchip des SNES gesendet wird. Das Öffnen einer `.spc` im Dateibrowser pausiert die Hintergrundmusik und setzt sie fort, wenn du mit B zurückkehrst.

Die Navigations-Effekte sind getrennt davon: Es sind kurze **MSU‑1-PCM**-Clips, die über den Audio-DAC des Moduls abgespielt werden (16 Bit Stereo, 44,1 kHz), sodass die Musik auf dem SNES-Soundchip weiterläuft, während ein Effekt ertönt. Halte sie kurz (deutlich unter einer Sekunde), damit sie sich knackig anfühlen.

### Theme-Format

Der offizielle Theme-Editor erwartet das originale sd2snes-Logo-Layout mit voller Breite. Dieser Fork hat den Menükopf geändert, um Platz für Spielecover zu schaffen:

- Originaler Logo-Bereich: **256×56**
- Logo-Bereich dieses Forks: **128×56**
- Die rechte Seite des Kopfs ist für das Spielecover reserviert
- Das Logo verwendet eine eigene Farbpalette

Deshalb kann der offizielle Editor die `m3nu.bin` dieses Forks nicht korrekt lesen oder schreiben.

### Aus dem Quellcode bauen

Alle Release-Binaries werden lokal mit Docker gebaut. Die einzige Voraussetzung auf deinem Rechner ist Docker selbst:

```
./build-docker.sh
```

Das kompiliert alles in einem Lauf: die MCU-Firmware (`firmware.img` für Mk.II, `firmware.im3` für Mk.III, `firmware.stm` für FXPAK PRO STM32), das SNES-Menü (`menu.bin` / `m3nu.bin`) und den ESP Companion (`esp32.bin` / `esp8266.bin`). Danach werden zwei ZIP-Dateien in `release/` gepackt:

- `sd2snes_firmware_v<VER>.zip`: nur die von diesem Fork geänderten Binärdateien
- `sd2snes_firmware_v<VER>-full.zip`: die offizielle sd2snes-Basis plus die Fork-Binärdateien

`<VER>` kommt aus `RELEASE_VERSION` in `src/VERSION`.

Für die tägliche Arbeit reicht `./build-docker.sh`. Das Build-Image und die FPGA-Cores werden gecacht, daher kompiliert jeder Lauf nur MCU, Menü und Companion neu. Weitere Flags:

- `--reuse`: Kompilierung überspringen und nur das neu verpacken, was bereits in `bin/` liegt
- `--rebuild-image`: das Docker-Build-Image neu erstellen

Die Firmware enthält den FPGA-Bootstrap-Core (cfgware). Der Mk.III/STM32-Core (`fpga_mini.bi3`, Cyclone IV) wird mit Intel Quartus synthetisiert und automatisch in das Build-Image eingebettet. Der Mk.II-Core (`fpga_mini.bit`, Spartan-3) wird standardmäßig aus dem Baum wiederverwendet. Um ihn mit Xilinx ISE innerhalb von Docker neu zu synthetisieren:

```
./build-docker.sh --with-ise
```

Dieser Schritt ist aufwendig und benötigt den Xilinx-ISE-14.7-Installer sowie eine WebPACK-Lizenz in `XILINX_SRC` (Standard: `~/Downloads`). Da der Fork das FPGA nicht ändert, brauchst du das nur selten.

### Credits

Die IPS/BPS-Patch-Unterstützung und die ursprüngliche Reset-zum-Menü-Arbeit stammen von [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293), mit Änderungen in diesem Fork.

Die Arbeit am In-Game-Cheat-Menü stammt von [@Relikk](https://github.com/Relikk).

Mitwirkende am originalen sd2snes-Repository, wie von GitHub gelistet:

- [@mrehkopf](https://github.com/mrehkopf)
- [@RedGuyyyy](https://github.com/RedGuyyyy)
- [@Relikk](https://github.com/Relikk)
- [@github-user-name](https://github.com/github-user-name)
- [@furious](https://github.com/furious)
- [@redacted173](https://github.com/redacted173)
- [@francois-berder](https://github.com/francois-berder)
- [@Godzil](https://github.com/Godzil)
- [@mlarouche](https://github.com/mlarouche)
- [@devinacker](https://github.com/devinacker)
- [@Xeroxxx](https://github.com/Xeroxxx)
- [@tcprescott](https://github.com/tcprescott)
- [@freelancer42](https://github.com/freelancer42)
- [@LuigiBlood](https://github.com/LuigiBlood)
- [@DevLaTron](https://github.com/DevLaTron)
- [@gasparitiago](https://github.com/gasparitiago)

### Quellcode und Lizenz

Dieses Projekt bleibt unter der GNU General Public License v2.0 (GPL-2.0) lizenziert, entsprechend der Lizenz des originalen sd2snes-Projekts.

Alle ursprünglichen Urheberrechte gehören den jeweiligen Autoren und Mitwirkenden.

Fork-spezifische Änderungen:
Copyright (C) 2026 Luan Freitas and contributors

Der Quellcode für alle verteilten Binärdateien/Releases ist in diesem Repository und in den entsprechenden Git-Tags/Releases verfügbar, gemäß den GPL-Anforderungen.

Siehe das [Original README](https://github.com/mrehkopf/sd2snes/blob/master/README.md).

Siehe [FURiOUS's README](README.Savestates.FURiOUS.md) für Informationen zu Save States.
