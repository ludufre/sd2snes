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

See [Original README](https://github.com/mrehkopf/sd2snes/blob/master/README.md)

See [FURiOUS's README](README.Savestates.FURiOUS.md) for information on Save States!
