<h1> sd2snes ❤️ covers</h1>

<img src="gfx/showcase.gif" width="512" alt="Showcase">
<img src="gfx/langs.gif" width="384" alt="The main menu shown in English, Brazilian Portuguese and Spanish">

A friendlier sd2snes/FXPAK firmware experience: languages, game covers, menu music, patches and smarter reset-to-menu behavior.

**🌐 Language:** English · [Português 🇧🇷](README-BR.md) · [Español 🇪🇸](README-ES.md)

**More information:** [sd2snes.ludufre.com](https://sd2snes.ludufre.com) has more details, guides and visual examples for this fork.

> **What is this?**
>
> This is a fork of the [original sd2snes firmware](https://github.com/mrehkopf/sd2snes) by [@mrehkopf](https://github.com/mrehkopf). It keeps the original firmware base and adds user-facing improvements: a menu in Brazilian Portuguese, English and Spanish, game covers, menu music, IPS/BPS patch selection and better reset-to-menu options.
>
> Use this repository for questions or bugs about the **translation**, **language selector**, **covers**, **menu music**, **patch selector** or **theme editor for this fork**. For core firmware issues unrelated to these additions, please use the original project.

## Start Here

If you just want to use this firmware, you do **not** need to compile anything or download the source code.

You need:

- An sd2snes or FXPAK cartridge.
- An SD card already prepared for your cartridge.
- The **full** `.zip` from this repository's [Releases](https://github.com/ludufre/sd2snes/releases) page.

> [!NOTE]
> This project does not include games/ROMs. Use your own legally obtained files.

## What This Fork Adds

- **Languages:** choose Brazilian Portuguese, English or Spanish directly in the menu.
- **Game covers:** show each game's box art while browsing your ROM list.
- **Menu music:** play an `.spc` track in the background while browsing.
- **IPS/BPS patches:** choose translation, hack or fix patches before a game starts, without changing the ROM file on the SD card.
- **Reset to menu improvements:** return to the same folder or even the same ROM after a short reset.
- **Custom themes:** edit the logo, font, palette, background and selector colors with the theme editor made for this fork.

## Installation

For the easiest installation, download the release marked **full**. It already includes the complete firmware files needed by this fork, so you do **not** need to download the equivalent official firmware first.

Each release name shows the matching official firmware version. For example, **"v2.1 (sd2snes v1.11.2)"** is meant for official firmware `v1.11.2`.

1. Download the matching **full** `.zip` from this repository's [Releases](https://github.com/ludufre/sd2snes/releases).
2. Open the `.zip` and copy its contents to the **root of your SD card**.
3. Replace existing files when your computer asks.
4. Put the SD card back into the sd2snes/FXPAK and turn on the console.

If you download a non-full package instead, it contains only the files changed by this fork. In that case, install the matching official firmware from [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads) first, then copy this fork's files into the `/sd2snes` folder over the existing files.

The menu starts in **English by default**. You can change it anytime from the **Language** option in the main menu.

## Get Notified About New Versions

To receive a GitHub notification whenever a new firmware version is released:

1. Open this repository on GitHub.
2. Click **Watch**.
3. Choose **Custom**.
4. Select **Releases** and save.

<p>
<img src="gfx/follow1.png" width="256" alt="GitHub Watch menu with the Custom option">
<img src="gfx/follow2.png" width="256" alt="GitHub custom notifications with Releases selected">
</p>

## Languages

<img src="gfx/langs.gif" width="512" alt="The main menu shown in English, Brazilian Portuguese and Spanish">

The menu can run in three languages:

- **Português:** Brazilian Portuguese translation for menus, messages and screens.
- **English:** the original and default firmware language.
- **Español:** Spanish translation for menus, messages and screens.

Open **Language** in the main menu, choose the language you want, and the menu changes immediately. Your choice is saved for the next time you turn on the console.

## Game Covers

Game covers appear in the menu while you browse your games.

For each ROM, place a cover file in the same folder as the ROM. The cover file must have the same name as the ROM, but with the `.cov` extension:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).cov
```

The easiest way to create `.cov` files is with the cover generator app:

### 👉 [github.com/ludufre/sd2snes-covers](https://github.com/ludufre/sd2snes-covers)

Use **sd2snes-covers v1.1.0 or newer**. If you made covers with an older version, regenerate them with the newer app.

You can turn covers on or off in the menu with **Mostrar capas** / **Show covers**.

## IPS/BPS Patches

This fork can apply **IPS** and **BPS** patches when a game loads. This is useful for fan translations, hacks and fixes.

Your ROM file on the SD card is not changed. The patch is applied only while the game is being loaded.

Put the patch in the same folder as the ROM. Its filename must start with the ROM filename, without the ROM extension, and end in `.ips` or `.bps`:

```text
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).ips
/sd2snes/A/Aladdin (USA) (Hack).bps
```

When you open a game with matching patches, the menu shows a patch selector:

- **`[No patch]`** starts the game normally.
- Choose a patch to use it for this boot.
- Up to **8** patches are shown for each game.

## Menu Music

The menu can play background music while you browse. The file must be an **`.spc`** file named `menu.spc` and placed here:

```text
/sd2snes/menu.spc
```

To add music:

1. Download an `.spc` file.
2. Rename it to `menu.spc`.
3. Copy it into the `/sd2snes/` folder on your SD card.
4. Turn on the console.

Good places to find `.spc` files:

- [snesmusic.org](https://snesmusic.org)
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc)

You can turn menu music on or off in **Configuration → Browser Settings → Menu music**.

> [!TIP]
> Some soundtracks are downloaded as `.rsn` files. An `.rsn` is usually an archive that contains several `.spc` files. Extract it and choose one `.spc` from inside.

## Reset to Menu

The reset button can bring you back to the sd2snes menu instead of simply restarting the game. This fork adds two options that make returning to your game list easier.

Set it in **Configuration → In-game Settings → Reset to menu**:

- **Off:** reset behaves like a normal SNES reset.
- **On:** a short reset returns to the menu.
- **Folder:** returns to the menu and opens the folder of the game you were playing.
- **ROM:** returns to the folder and highlights the ROM you were playing.

The **Folder** and **ROM** options work after a reset back to the menu. A full power-on still starts from the normal top-level menu.

## Custom Theme

You can customize the menu logo, font, palette, background and selector colors.

Use the theme editor made for this fork:

### 👉 [sd2snes.ludufre.com/theme](https://sd2snes.ludufre.com/theme/)

> [!IMPORTANT]
> The official sd2snes theme editor does not support this fork's theme format. Use the editor above when editing `m3nu.bin` for this firmware.

Basic theme workflow:

1. Open the theme editor.
2. Upload your `m3nu.bin`.
3. Upload a **128×56 PNG** if you want to replace the logo.
4. Choose the parts you want to change.
5. Download the edited file and copy it back to your SD card.

## Common Problems

**The menu did not change after installing.**

Check that you used the matching **full** release and copied its files to the root of the SD card. If you used a non-full package, check that the matching official firmware was installed first and that this fork's files were copied into `/sd2snes`.

**Covers do not appear.**

Check that covers are enabled, that each `.cov` file has the same name as its ROM, and that the covers were generated with **sd2snes-covers v1.1.0 or newer**.

**Menu music is silent.**

Check that the file is named exactly `menu.spc`, is placed at `/sd2snes/menu.spc`, and is really an `.spc` file. MP3 and WAV files do not work.

**A patch does not appear.**

Check that the patch is in the same folder as the ROM, starts with the ROM filename, and ends in `.ips` or `.bps`.

## Advanced Notes

This section is mainly for developers, maintainers and advanced users. You do not need it for normal installation.

### Cover Format

This firmware, starting with **v1.11.2-br-2.1**, uses the newer OBJ-sprite `.cov` cover format. Covers generated by older versions of the cover app will not display correctly. Regenerate them with **sd2snes-covers v1.1.0 or newer**.

### BPS Patch Integrity Check

The BPS integrity check can be enabled from **Configuration → Patch Options → Verify Integrity**.

This option is **Off by default**. When enabled, the firmware re-reads the ROM after applying a BPS patch to confirm it was applied correctly. This makes BPS loading slower; for example, a 4 MB BPS patch can add around 15 seconds of loading time on average. IPS patches are not verified by this option.

### Menu Music Limitations

Only `.spc` files are supported. An `.spc` file is not a normal audio recording; it is a snapshot of the SNES sound chip state and is capped at 64 KB. There is no direct MP3-to-SPC conversion.

When music loads at boot, after a reset or after turning the option on, the menu may pause briefly while the file is uploaded to the SNES sound chip. Opening an `.spc` from the file browser pauses the background music and resumes it when you return with the B button.

### Theme Format

The official theme editor expects the original full-width sd2snes logo layout. This fork changed the menu header to make room for game covers:

- Original logo area: **256×56**
- This fork's logo area: **128×56**
- The right side of the header is reserved for the game cover
- The logo uses its own color palette

Because of this, the official editor cannot read or write this fork's `m3nu.bin` correctly.

### Credits

The IPS/BPS patch support and the original reset-to-menu work come from [@Xeroxxx](https://github.com/mrehkopf/sd2snes/pull/293), with changes made in this fork.

Original sd2snes repository contributors listed by GitHub:

- [@mrehkopf](https://github.com/mrehkopf)
- [@RedGuyyyy](https://github.com/RedGuyyyy)
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

### Source Code and License

This project remains licensed under the GNU General Public License v2.0 (GPL-2.0), following the original sd2snes project license.

All original copyrights belong to their respective authors and contributors.

Fork-specific changes:
Copyright (C) 2026 Luan Freitas and contributors

Source code for all distributed binaries/releases is available in this repository and corresponding Git tags/releases, in accordance with GPL requirements.

See the [Original README](https://github.com/mrehkopf/sd2snes/blob/master/README.md).

See [FURiOUS's README](README.Savestates.FURiOUS.md) for information on Save States.
