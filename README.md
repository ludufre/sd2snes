<h1> sd2snes ❤️ covers</h1>

<img src="gfx/showcase.gif" width="512" alt="Showcase">

SD card based multi-purpose cartridge for the SNES

**🌐 Language:** English · [Português 🇧🇷](README-BR.md)

> **Notice**
>
> This fork provides the **Brazilian Portuguese translation** and the **game covers** feature for the sd2snes firmware, based on the [original repository](https://github.com/mrehkopf/sd2snes) by [@mrehkopf](https://github.com/mrehkopf).
>
> For questions and bugs about the **translation** or the **covers**, use this repository. For anything else related to the firmware itself, please use the original repository.

## ✨ Highlights

- 🇧🇷 **Brazilian Portuguese translation** — the firmware menu, messages and screens fully translated.
- 🎮 **Game covers** — shows the game's box-art cover in the menu as you browse the list. See the **Game Covers** section below.
- 🎵 **Menu background music** — plays an `.spc` track while you browse the menu. See the **Menu Music** section below.
- 🩹 **IPS/BPS patches** — apply translation/hack patches to a game at boot, without modifying the ROM. See the **IPS/BPS Patches** section below.

## Installation

The translation is available in the [**Releases**](https://github.com/ludufre/sd2snes/releases) section of this repository. The package contains **only the translated files**, so you must first install the full official firmware, available at [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads).

Each Release name indicates the equivalent official firmware version. For example: the release **"v2.1 (sd2snes v1.11.2)"** corresponds to the official firmware `v1.11.2`.

1. Download and install the official firmware of the matching version from [sd2snes.de/blog/downloads](https://sd2snes.de/blog/downloads).
2. Download the translation `.zip` from [Releases](https://github.com/ludufre/sd2snes/releases), choosing the version that matches the installed firmware.
3. Extract the contents of the `.zip` into the `/sd2snes` folder on your SD card, replacing the existing files.
4. Insert the card into your sd2snes/FXPAK and power on the console. The menu will load in Brazilian Portuguese.

## 🎮 Game Covers

Besides the translation, this fork shows the **game's box-art cover** in the menu as you browse the game list.

For each ROM, just drop a cover file **with the same name** and the **`.cov`** extension in the same folder as the ROM:

```
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).cov   ← this game's cover
```

Covers use a custom format (`.cov`). The easiest way to create them is with the **cross-platform app** (Windows, macOS, and Linux), available in the repository:

### 👉 [github.com/ludufre/sd2snes-covers](https://github.com/ludufre/sd2snes-covers)

The app generates the `.cov` files for your game library. In the menu, cover display can be toggled on/off with the **"Mostrar capas"** option.

> [!IMPORTANT]
> Generate the covers with **[sd2snes-covers](https://github.com/ludufre/sd2snes-covers) v1.1.0 or newer**. This firmware (**v1.11.2-br-2.1 or newer**) uses the new OBJ-sprite `.cov` format — covers created with older app versions won't display correctly; just regenerate them with v1.1.0+.

---

## 🩹 IPS/BPS Patches

This fork can apply **IPS** and **BPS** patches (fan translations, hacks, fixes) to a game **at load time**. The patch is applied to the copy of the ROM in memory only — **your original ROM file on the SD card is never modified**.

Drop the patch file in the **same folder** as the ROM, with a name that **starts with the ROM's name** (without extension) and ends in **`.ips`** or **`.bps`**:

```
/sd2snes/A/Aladdin (USA).sfc
/sd2snes/A/Aladdin (USA).ips          ← a patch for this game
/sd2snes/A/Aladdin (USA) (Hack).bps   ← another patch for the same game
```

When you open a game that has matching patches, the menu shows a **patch selector** before booting:

- **`[No patch]`** is always the first option — boot the game unpatched.
- Pick a patch to apply it for this boot.
- Up to **8** patches per game are listed (sorted by name).

`.ips` and `.bps` are auto-detected. BPS patches may produce a larger ROM (this is handled automatically).

**Verify integrity:** there's an option in the menu at **Configurações → Opções de Patches → Verificar Integridade** (*Settings → Patch Options → Verify integrity*). When on (the default), the firmware re-reads the patched ROM to confirm it was applied correctly. This makes loading noticeably slower (~23 s for a 4 MB BPS), so turn it off if you'd rather have faster loads.

---

## 🎵 Menu Music

The menu can play background music while you browse the game list. The music is any **`.spc`** file (the SNES's native music format) placed at this exact path on the SD card:

```
/sd2snes/menu.spc
```

**How to set or change the music:**

1. Get an `.spc` file (see sources below).
2. Rename it to `menu.spc`.
3. Copy it into the `/sd2snes/` folder on your SD card.
4. Done — the music plays the next time the menu boots.

**Where to get `.spc` files:**

- [snesmusic.org](https://snesmusic.org) — SNES game soundtracks.
- [zophar.net/music](https://www.zophar.net/music/nintendo-snes-spc) — SPC sets per game.

> [!TIP]
> Many sites distribute soundtracks as `.rsn` (a `.rar` archive containing several `.spc` files). In that case, extract the `.rsn` and pick one of the `.spc` files inside.

**Turning it on/off:** there's a switch in the menu at **Configurações → Opções do Navegador → Música do menu** (*Settings → Browser Options → Menu music*). When on (the default), the menu looks for `/sd2snes/menu.spc`; if the file isn't present, the menu simply stays silent (no error).

**Notes:**

- Only the `.spc` format works (not MP3/WAV). An `.spc` isn't an audio recording — it's a snapshot of the SNES sound chip (samples + sequence), capped at 64 KB. **There is no MP3-to-`.spc` conversion**; use ready-made `.spc` tracks.
- When the music loads (at boot, after a reset, or when you switch it on) there's a brief ~1 s pause while the 64 KB are uploaded to the sound chip. This is a hardware limitation (the transfer must run with interrupts disabled).
- Opening an `.spc` from the file browser pauses the background music automatically and resumes it when you go back (B button).

---

See [Original README](https://github.com/mrehkopf/sd2snes/blob/master/README.md)

See [FURiOUS's README](README.Savestates.FURiOUS.md) for information on Save States!
