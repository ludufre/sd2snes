<p align="center"><a href="https://sd2snes.ludufre.com"><img src="https://sd2snes.ludufre.com/assets/logo.png" alt="sd2snes+" width="320"></a></p>

**🌐 Language:** English · [Português 🇧🇷](https://sd2snes.ludufre.com/?lang=pt) · [Español 🇪🇸](https://sd2snes.ludufre.com/?lang=es) · [Deutsch 🇩🇪](https://sd2snes.ludufre.com/?lang=de) · [Français 🇫🇷](https://sd2snes.ludufre.com/?lang=fr)

> **What is this?** A fork of the [original sd2snes firmware](https://github.com/mrehkopf/sd2snes) by [@mrehkopf](https://github.com/mrehkopf) that keeps the firmware base and adds user-facing extras. Use this repo only for issues about **those additions** — for core firmware issues, use the original project.

## 👉 Full guides, downloads, tools and FAQ: **[sd2snes.ludufre.com](https://sd2snes.ludufre.com)**

Everything below is a quick overview. **The site has the step-by-step guides, screenshots and the web tools** — head there for anything beyond the basics.

## Get it running

1. Download the **full** `.zip` from [**Releases**](https://github.com/ludufre/sd2snes/releases).
2. Copy its contents to the **root of your SD card** (replace when asked).
3. **Fill your library:** with the SD card still in your computer, open the **[Web Manager](https://sd2snes.ludufre.com/manager/)** in your browser and connect the card — it scans your ROMs and downloads **box art, game-info cards (with animated video clips) and cheats** straight onto the card. Runs in the browser, nothing to install.
4. Put the card back and power on. Change the language anytime in the **Language** menu.

> [!NOTE]
> No games/ROMs are included — use your own legally obtained files.

Downloads, the step-by-step install and troubleshooting live on the site:
[Downloads](https://sd2snes.ludufre.com/#downloads) · [Install guide](https://sd2snes.ludufre.com/#instalar) · [FAQ](https://sd2snes.ludufre.com/#faq) — e.g. [Which firmware version do I need?](https://sd2snes.ludufre.com/#faq-versao) · [Black screen / blinking LED on Mk.II?](https://sd2snes.ludufre.com/#faq-tela-preta)

## What This Fork Adds

| Preview | Feature |
| :-----: | :------ |
| <img src="https://sd2snes.ludufre.com/assets/diffs/languages.gif" width="320" alt="Menu in Portuguese, English, Spanish, German & French"> | **Menu in Portuguese, English, Spanish, German & French**<br>The whole firmware menu, messages and screens translated to Brazilian Portuguese, Spanish, German and French. The original English stays available — just pick one. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/covers.gif" width="320" alt="Box-art in the game list"> | **Box-art in the game list**<br>As you browse, the game's official cover shows up on screen. Just drop a .cov file with the same name as the ROM in the same folder. To build the whole collection at once, the Web Manager downloads box art in bulk (and the cheats too). |
| <img src="https://sd2snes.ludufre.com/assets/diffs/gameinfo.gif" width="320" alt="See the game's info card before playing"> | **See the game's info card before playing**<br>Before a ROM boots, a full-screen card shows the cover, a screenshot (or an animated clip with sound) and the game's metadata — developer, year, players, genre and special chip — plus a short description. In the menu you can choose whether the clip plays (with its soundtrack) or just a static snapshot shows. The Web Manager downloads each game's info card automatically. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/sounds.gif" width="320" alt="Menu music and sounds"> | **Menu music and sounds**<br>Plays an .spc track in the background while you browse — plus 4 navigation sound effects (cursor, confirm, back, error). Build it all in the Sound Creator: pick the .spc, preview it in the browser, swap the effects and download them ready for the card. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/cheat-overlay.gif" width="320" alt="Cheat menu over the game"> | **Cheat menu over the game**<br>Pause the running game with L + R + Y + ← and toggle cheats on the fly, without going back to the menu. On by default; turn it off in Configuration › In-game Settings › In-game hook › Cheat menu. Tip: the Web Manager also downloads each game's cheats automatically. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/themes.gif" width="320" alt="Switch the menu theme on the console"> | **Switch the menu theme on the console**<br>From firmware 2.9 on, pick the menu theme right on the SNES — logo, colours, background and selection bar. Drop .thm files in any folder on the card and choose one in the browser. Grab ready-made themes in the gallery or build your own in the web editor. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/cheats.gif" width="320" alt="Cheat manager in the menu"> | **Cheat manager in the menu**<br>The sd2snes already applies cheats per game — what this fork adds is the in-menu manager: from the context menu (Y) you turn each code on or off on the console, without editing the YAML on a PC. The files live in /sd2snes/cheats; grab ready-made cheats at gamehacking.org, exporting as FXPak Pro 1.7 (.yml), or download them automatically with the Web Manager. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/memorypack.gif" width="320" alt="BS-X carts with a Memory Pack slot"> | **BS-X carts with a Memory Pack slot**<br>Added support for the Satellaview 8M Memory Pack slot for the Japanese games that use it — drop the pack dump at /sd2snes/saves/&lt;rom&gt;.mpk and the game reads the Memory Pack contents (saved sound novels, downloaded races…). On top of that, Derby Stallion 96 and Sound Novel Tsukuru now work. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/patches.gif" width="320" alt="IPS/BPS patch at boot"> | **IPS/BPS patch at boot**<br>The heart of rom-hacking: apply fan translations, hacks and fixes without altering the ROM. Drop the .ips/.bps patch in the ROM's folder using the game name as a prefix; at launch the firmware lists the patches and applies it in memory, leaving the original ROM untouched. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/descriptions.gif" width="320" alt="A description for every menu option"> | **A description for every menu option**<br>From firmware 2.11 on, every settings option shows a line explaining what it does, right on screen — in Portuguese, English, Spanish, German and French. It's easy to understand each setting without a manual. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/chips.gif" width="320" alt="Warns when a chip's BIOS is missing"> | **Warns when a chip's BIOS is missing**<br>Some games use special chips (DSP in Super Mario Kart, Super FX in Star Fox, SA-1, S-DD1, OBC1, CX4, Super Game Boy, BS-X…) that need support files on the card. From firmware 2.11 on, if one is missing, instead of freezing or booting a broken game, the menu shows which file is missing and returns to the menu without resetting the console. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/delete.gif" width="320" alt="Delete a file and its savegame"> | **Delete a file and its savegame**<br>From the browser's context menu (Y), delete the selected file or just the ROM's save (.srm) — right on the console, without pulling the card out or turning on a PC. |
| <img src="https://sd2snes.ludufre.com/assets/diffs/reset.gif" width="320" alt="Reset brings you back where you were"> | **Reset brings you back where you were**<br>A short reset can drop you back in the menu already inside the folder of your last game — or right on the ROM you were playing. Pick it in In-game Settings → Reset to menu: Off, On, Folder or ROM. |

Each feature has a full guide, screenshots and web tools on the site — start at **[Features & guides](https://sd2snes.ludufre.com/#recursos)**.
Tools: [Theme gallery](https://sd2snes.ludufre.com/gallery/) · [Theme Creator](https://sd2snes.ludufre.com/theme/) · [Sound Creator](https://sd2snes.ludufre.com/sounds/) · [Web Manager — covers, info & cheats](https://sd2snes.ludufre.com/manager/) · [GamesDB](https://sd2snes.ludufre.com/gamesdb/).

## Credits & license

- IPS/BPS patching and the original reset-to-menu work: [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293) (adapted here).
- In-game cheat menu: [@Relikk](https://github.com/Relikk).
- Fork, translation, covers and web tools: [@ludufre](https://github.com/ludufre).
- Original [sd2snes project](https://github.com/mrehkopf/sd2snes) and its contributors: [@mrehkopf](https://github.com/mrehkopf), [@RedGuyyyy](https://github.com/RedGuyyyy), [@Relikk](https://github.com/Relikk), [@furious](https://github.com/furious), [@redacted173](https://github.com/redacted173), [@francois-berder](https://github.com/francois-berder), [@Godzil](https://github.com/Godzil), [@mlarouche](https://github.com/mlarouche), [@devinacker](https://github.com/devinacker), [@Xeroxxx](https://github.com/Xeroxxx), [@tcprescott](https://github.com/tcprescott), [@freelancer42](https://github.com/freelancer42), [@LuigiBlood](https://github.com/LuigiBlood), [@DevLaTron](https://github.com/DevLaTron), [@gasparitiago](https://github.com/gasparitiago).

Licensed under **GPL-2.0**, following the original sd2snes project. Fork-specific changes © 2026 Luan Freitas and contributors. Source for every released binary is in this repository and its Git tags/releases. See also the [original README](https://github.com/mrehkopf/sd2snes/blob/master/README.md) and [Save States (FURiOUS)](https://github.com/mrehkopf/sd2snes/blob/master/README.Savestates.FURiOUS.md).
