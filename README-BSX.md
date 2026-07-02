# BS-X Satellaview — USB broadcast tooling (`utils/`)

Host-side Python tools that turn the Mac into a "St.GIGA satellite": they push a BS-X
(Satellaview) broadcast to a real sd2snes / FXPAK PRO over USB, including the **over-the-air
download** of a program (`.bs`) that the Town receives in a building, saves to the Memory
Pack, and runs. The real Satellaview did exactly this over St.GIGA; the **stock
sd2snes / FXPAK PRO firmware** never had the live-broadcast **receiver** — it only ever
played static `.bs` / `.mpk` files. This fork adds that receiver (in the FPGA); these tools
are the "satellite" that feeds it over USB.

## Where the scripts live and how to run them

The scripts are in **`utils/`** in this repo. They resolve their own paths relative to the
script file, so run them from the repo root:

```bash
python3 utils/bsx_download.py ...
```

**Requirements:**
- **Python 3** + **pyserial** (`pip3 install pyserial`). No numpy/PIL.
- **Firmware with the BS-X receiver** flashed on the device (`bsx.v` receiver + `bsx_dl.c`
  bridge + the fixed TIME opcode) — build the MCU firmware and menu with the repo Makefile
  (see `README.md`) and write `firmware.im3` / `m3nu.bin` to the SD.
- **Core `fpga_base.bi3`** with the BS-X receiver synthesized.
- **The USB serial only exists while the SNES is POWERED ON.** The port is auto-detected by
  VID:PID (override: env `SD2SNES_PORT`).
- **Never share the USB port**: one process at a time. A second reader while the satellite
  transmits kills the transmission ("multiple access on port").

## The scripts

| Script | Role | Executable? |
|---|---|---|
| **`bsx_download.py`** | **The DOWNLOAD satellite** — pushes the catalog + streams the program; the Town receives and saves it. **This is the one that works end-to-end.** | ✅ CLI |
| `usb_bsx.py` | Raw read/write of the BS-X PSRAM + file PUT to the SD, over USB. Base of the others. | ✅ CLI |
| `bsx_broadcast.py` | Library of *builders* (Directory / Town Status / Channel Map / Data-Group / schedule). | 📚 lib |
| `bsx_server.py` | 24h service driven by a CSV grid (fixed pool of slots + live Directory). | ✅ CLI |
| `bsx_soundlink.py` | SoundLink-style push ("about to begin" + scheduled auto-reception). | ✅ CLI |
| `bsx_stage_a.py` | De-risk of the "fixed Channel Map + live Directory" model (install / a / b / empty). | ✅ CLI |
| `bsx_trace_dump.py` | Debug: dump + decode of the silicon bus trace (debug core only). | ✅ (positional) |
| `bsx_receiver_model.py` | Golden model of the receiver (port of bsnes-plus BSXBase). Source of `make_fragments`. | 📚 lib |

---

## Main flow: download a game (`bsx_download.py`)

### Device preconditions (one-time)
1. Firmware + core with the BS-X receiver flashed (above).
2. The SD needs a **streaming-ready `bsxpage.bin`** whose **Channel Map** already contains the
   download service (svc `0x0103`, channel `0x125`, **dest=3** FLASH-Free). The Channel Map is
   **read once at boot and cached for the whole session** — so it must be in the BOOTED image;
   only Directory/Town Status are re-read live. (`bsx_stage_a.py install` generates that image
   and writes it to the SD.)
3. **Boot the Town** from a `.bs` (e.g. any Memory Pack) → you reach the city.

### Step by step (each session)
```bash
# TRANSMIT (continuous-carousel satellite; Ctrl-C ends it and disarms the receiver).
# In the Town, when it asks you to delete saved data to free space, DELETE — the core
# erases the right blocks and the save comes out byte-perfect.
python3 utils/bsx_download.py "/path/Arkanoid - Doh It Again (Japan).bs" --dest 3
```

> **Note — no manual pre-erase needed** (with the erase-fix core, `fpga_base.bi3` ≥
> 2026-07-02): the Town's erase clears the correct pack blocks before programming.
> `usb_bsx fill 0x400000 0x100000 0xFF --force` remains only as an optional manual **format**
> (deliberately wiping the whole pack). **On older cores** (wrong erase block index — only the
> lower half was erased) the `fill` is mandatory, otherwise the upper half of the program keeps
> garbage → **Error 09**.

Once "armed", **in the Town**: wait for the St.GIGA announcement → enter the **News building**
→ receive (the **progress bar** fills, ~1.5 min, 16 fragments) → go back **home** → **execute
saved data** → the game runs. The **city stays alive** after the broadcast (infinite carousel
+ preserved Channel Map).

### What the satellite does under the hood
1. **Sets the device clock** (USB TIME opcode) to the real date — the catalog schedule is
   stamped with today and the Town validates it against the RTC. (`--no-clock` skips it, for
   old firmware.)
2. **Pushes the catalog** (Directory `0x122` / Town Status `0x123` / Channel Map `0x124` /
   data channel `0x125`), with **verified writes** (read-back + retry). The Channel Map is
   **merged/upserted** — the city's own services are preserved (replacing the map kills the
   city: `番組が終了` + buildings offline).
3. **Arms** the receiver and **streams the fragments on demand**: the FPGA requests the next
   one (`dl_seq` bump), the host stages one 32KB fragment into the ring (`0x980000`) + the
   prefix table. Single slot, strictly drain-gated.
4. When done, **re-serves in an infinite carousel** (a real station never goes silent) with an
   **anti-tear gate** (never rewrites the ring while the FPGA may be serving).
5. On exit (Ctrl-C / error / end), it **disarms** the receiver (`ctl=0` descriptor + waits for
   the ACK) — otherwise the channel stays hijacked serving garbage until a power-cycle. The
   firmware also auto-disarms if the host dies.

### Flags (`bsx_download.py --help`)
| Flag | Default | What |
|---|---|---|
| `program` | — | `.bs` image (LoROM; the Town reads the header at `0x7FB0`) |
| `--dest N` | `3` | 0=WRAM · 1=PSRAM(runs once, no save) · 2=full FLASH · **3=FLASH-Free (saves)** |
| `--chan 0xNNN` | `0x125` | data-channel LCI (must be in the booted Channel Map pool) |
| `--feed-boot` | off | feed the boot broadcast to `0x900000` live + arm, so the city can boot with **no `bsxpage.bin` on the SD** (used by the `.sfc` base-unit mode below) |
| `--dirid N` | `0` (auto) | Directory/TownStatus ID; auto-rotates so the Town re-reads |
| `--autostart N` | `0` | 0=list · 1=optional · 2=autoboot |
| `--date YYYY-MM-DD` | today | schedule date |
| `--window HH:MM-HH:MM` | all day | schedule window |
| `--name` / `--desc` / `--folder` / `--fmsg` | — | listing/building metadata |
| `--no-clock` | off | skip the clock sync (firmware without the TIME fix) |
| `--no-catalog` | off | don't push the catalog (assume already in PSRAM) |
| `--watch` | off | **Town lifecycle WRAM telemetry** (debug — below) |
| `--timeout S` | `10` | per-fragment drain timeout warning |

### `--watch` (debug telemetry)
Logs changes only, once/2s, of the Town's lifecycle via the WRAM mirror at `0xF5xxxx`:
`state $13C9` (3 = service lookup FAILED → city offline; `0x18` = broadcast watchdog → Error
22), `result $13C5`, `restart $0B74` (2 = healthy reload, 3 = offline), `wdog/preset
$1433/$1435`. This is the tool that cracked the "dying city" case.

---

## Booting the city from the `.sfc` BIOS — persistent Memory Pack (base-unit mode)

Besides booting a `.bs` (file = pack), this fork's firmware can boot the **BS-X BIOS `.sfc`**
("BS-X - Sore wa Namae o Nusumareta Machi no Monogatari") directly: it detects the BIOS, forces
BS-X mode, and runs it as a live **base unit** with a **dedicated, PERSISTENT Memory Pack**
(`/sd2snes/saves/<sfc-name>.mpk`, blank until you download into it). Downloads saved here — and
the city's own name/settings (`<sfc-name>.srm`) — **survive a power cycle**. (The `.bs` mode is
the "file is the pack" one; the `.sfc` mode is the vanilla city with its own pack.)

Here the broadcast is fed **live** by the satellite (no `bsxpage.bin` on the SD), so pass
**`--feed-boot`** and start the satellite **before** booting the `.sfc` — the Channel Map is
cached once at boot, so the satellite must already be armed when the city comes up:

```bash
# 1) Start the satellite with --feed-boot (feeds the boot broadcast to 0x900000, then arms).
#    WAIT for "armed ... Waiting for the Town to start" BEFORE booting the .sfc.
python3 utils/bsx_download.py "/path/Arkanoid - Doh It Again (Japan).bs" --dest 3 --feed-boot

# 2) In the sd2snes browser, boot the BS-X .sfc  ->  the city comes up (fed live, NPCs, clock).
# 3) News building -> receive (progress bar, 16 fragments) -> back home -> execute saved data.
#    The game runs; the save PERSISTS across reboots (in <sfc-name>.mpk).
```

To verify a save landed byte-perfect, read the pack over USB (BS-X header at `pack+0x7FB0`):

```bash
python3 utils/usb_bsx.py read 0x407FDA 8        # 0x33 at 0x407FDA = magic written = OK
python3 utils/usb_bsx.py read 0x400000 0x100000 -o pack.bin   # full pack; checksum 0x7FDC+0x7FDE == 0xFFFF
```

> The satellite serves **one download per run**. To download **again** after a reboot, **restart
> the satellite** (fresh feed + arm) — a second download over a stale run stays at `dl_seq=0`
> (`受信中` with no data) and eventually hits Error 22.
>
> First boot of a **brand-new** pack: the firmware fills it with `0xFF` (erased flash); the city
> formats it. A fresh `.mpk` / `.srm` is created under `/sd2snes/saves/` on the first reset-save.

---

## `usb_bsx.py` — raw read/write of the PSRAM (+ file PUT)

Base of all the others; handy for manual inspection.

```bash
python3 utils/usb_bsx.py read  0x400000 0x100 -o dump.bin      # raw GET
python3 utils/usb_bsx.py feed  img.bin --addr 0x900000         # PUT the whole image
python3 utils/usb_bsx.py page  5 data.bin                      # PUT at 0x900000 + 5*0x200
python3 utils/usb_bsx.py poke  0x900048 DEADBEEF               # marker bytes
python3 utils/usb_bsx.py fill  0x400000 0x100000 0xFF --force  # blank the pack (read-back)
python3 utils/usb_bsx.py verify img.bin --addr 0x900000        # read-back + diff
```
It also exposes `put_file(local, remote)` (file PUT to the SD, e.g. writing `bsxpage.bin`),
used by `bsx_stage_a.py`.
> **Guard:** `feed`/`page`/`poke`/`fill` **refuse** to write outside `[0x900000, 0xA00000)`
> without `--force`. That's why blanking the pack (`0x400000`) passes `--force`.

---

## Other satellites

- **`bsx_server.py <grid.csv>`** — real-time 24h service. FIXED pool of slots laid down at boot
  (via `bsx_stage_a install`) + live Directory driven by the clock. `--once` (test), `--demo`
  (cycles one program per poll — each swap = one St.GIGA reception + restart; do **not** use
  with a short poll), `--rtc-date`, `--transport {usb,esp}`.
- **`bsx_soundlink.py [title]`** — SoundLink-style push: puts a program "on air now" and lets
  the Town auto-receive ("about to begin" + St.GIGA logo → saves to the pack). `--data`
  (content), `--schedule` (continuous CSV mode), `--slot`, `--rtc-date`.
- **`bsx_stage_a.py {install|a|b|empty}`** — proves the "fixed Channel Map + live Directory"
  model: `install` generates the streaming-ready image and writes it to the SD (then **boot a
  `.bs` once**); `a`/`b` feed programs live into slots (appear/disappear without reboot);
  `empty` clears them. Only touches Directory/Town Status/slot channel — **never** the Channel
  Map.

## `bsx_trace_dump.py` — silicon debug (debug core only)
Dump + decode of the bus trace that a **debug build of the core** writes to `0x990000` (the
trace writer is not in the production core — it was itself a corruption vector). Usage:
`python3 utils/bsx_trace_dump.py [out.bin] [nbytes]`. Run it **with the satellite stopped**
(one user per port); it reads over USB via `usb_bsx`.

---

## Address map (PSRAM, space=SNES)
| Region | Use |
|---|---|
| `0x400000`–`0x4FFFFF` | **Writable pack (1 MB)** — where the download SAVES (dest=3). |
| `0x900000` + `LCI*0x200` | **Broadcast page/channel window** (Time/Directory/Town Status/Channel Map/data). |
| `0x980000` | **Download ring** (the FPGA serves `0x980000 + offset`); prefix table at `+0x8100`. |
| `0x990000` | Silicon bus trace (debug core only). |
| `0x9A0000` | **Descriptor mailbox** (host→MCU, 13 B: `BXDL`+seq+ctl+chan+base+frames). |
| `0x9A0010` | **Status mailbox** (MCU→host, 6 B: `BXDS`+dl_seq@+4+ack@+5). |

Default LCIs: Directory `0x122` · Town Status `0x123` · Channel Map `0x124` · data `0x125+`.
Download service = `0x0103` (type 5, dest=3). SatellaWave fragment = **32 KB** (`CHUNK`).

---

## Troubleshooting (hardware-proven gotchas)
- **No progress bar / infinite wait:** the satellite isn't transmitting (start it), or the
  booted Channel Map lacks svc `0x0103` on the right channel, or the header isn't LoROM.
- **"Error 09" (Memory Pack):** the blocks the program wrote to weren't erased (`0xFF`), so its
  `0xFF` holes kept the previous game. **Fixed in the erase-fix core** (the Town's erase now
  clears the correct blocks). On an older core, work around it with `fill`.
- **"Error 21" (validation):** **HiROM** program (the Town reads the header at `0x7FB0` LoROM)
  — use a **LoROM** `.bs`; or an unadapted header (the satellite adapts it itself on dest=3).
- **"Error 22" (Fragment Interval):** per-fragment gap exceeded the Channel Map timeout — the
  satellite uses `timeout=600` (VBlank-seconds) and stage-on-demand; should not happen.
- **"番組が終了しました" + dead city (sad-eyed buildings):** the live Channel Map lost the
  city's services. The satellite does a **merge/upsert** to avoid it; if it happens, restoring
  the full map revives it **without reboot** (a dead city, however, won't re-read the catalog —
  power-cycle).
- **USB wedge under Town contention:** power-cycle the SNES. Repeated wedge→power-cycle cycles
  can wedge the macOS USB stack (device vanishes on the Mac, fine on Windows) → **reboot
  macOS** (Apple Silicon can't reset USB without a reboot).
- **Reception (St.GIGA "about to begin" logo + city restart) is NORMAL** — inherent to the Town
  ROM, identical to bsnes-plus; controllable only by FREQUENCY (windows of hours), not
  removable.

## Library modules
- **`bsx_broadcast.py`** — builders for the sd2snes page format, by copy-and-patch of a baseline
  `bsxpage.bin`: `build_directory`, `build_townstatus`, `build_channelmap`,
  `channelmap_append_service`, `build_data_channel`, `datagroup`, `pack_sched`, `patch_page`.
- **`bsx_receiver_model.py`** — golden model of the receiver (faithful port of bsnes-plus
  BSXBase); `make_fragments`/`frag_frames`/`PKT` are consumed by `bsx_download.py` to slice the
  program into 32KB fragments.
