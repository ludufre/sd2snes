# sd2snes / FXPAK PRO - ESP Companion (ESP32 / ESP8266)

A companion ESP wired to the cartridge's MCU over a private UART turns the sd2snes
into a network-managed device: browse and transfer the SD card from a phone/PC
over Wi-Fi, configure Wi-Fi from the SNES menu itself, with the protocol designed
so the SNES is **never blocked** during transfers.

One Arduino codebase in `companion/` builds both targets; the only per-chip
differences live in `companion/src/platform.h`. The protocol, WebUI and MCU/menu
code are identical (the MCU does not know or care which one is connected):

| Target  | Chip | PlatformIO env |
|---------|------|----------------|
| ESP32   | ESP32-WROOM-32   | `esp32dev`  |
| ESP8266 | ESP-12E (NodeMCU) | `nodemcuv2` |

```
[SNES menu (m3nu)] --SNES_CMD/$55--> [MCU] <==UART 921600 8N1==> [ESP] <--Wi-Fi/HTTP--> [browser]
                                       LPC1756 UART0 (P0.2/P0.3)                          SoftAP 192.168.4.1
```

The MCU side lives in `src/` (the C firmware) and `snes/` (the m3nu menu) and is the
same firmware you already build. Only the ESP-side pins differ between the two chips.

---

## Hardware and wiring

MCU side (identical for both ESP targets):

| MCU (board) | Link peripheral | TX pin | RX pin | Status |
|-------------|-----------------|--------|--------|--------|
| LPC1756 (mk3 / FXPAK PRO) | UART0 | P0.2 = pin 79 | P0.3 = pin 80 | active, tested |
| LPC1754 (mk2) | UART0 | P0.2 = pin 79 | P0.3 = pin 80 | active, build-only |
| STM32F401 (FXPAK PRO STM32) | USART6 | PC6 | PC7 | driver built but OFF (pins unconfirmed) |

ESP side (the chip determines which pins/UART):

| ESP target | UART | ESP RX (<- MCU TX P0.2) | ESP TX (-> MCU RX P0.3) | Debug console |
|------------|------|-------------------------|--------------------------|---------------|
| ESP32   | UART2 | GPIO16 | GPIO17 | UART0 over USB (full `ESP_LOGx`) |
| ESP8266 | UART0 (swapped) | GPIO13 = D7 | GPIO15 = D8 | UART1 TX = GPIO2, one-way only |

Everything is 3.3 V, no level shifter. Common GND. Power the ESP from its own USB
on the bench; a permanent install needs a solid 3.3 V supply (Wi-Fi current peaks).

**Use one companion at a time** on the MCU UART0 lines (ESP32 OR ESP8266, not both
wired to P0.2/P0.3 at once).

Why UART0 on the LPC: it is the LPC's ISP bootloader UART, so it is broken out and
free; using it from the running firmware is safe (ISP entry is not triggered on
this board, and SD-based firmware flashing does not use it).

Why the swap on the ESP8266: the ESP8266 has only one full UART (UART0), and its
default pins (GPIO1/GPIO3) are the ones wired to the USB-serial chip. `Serial.swap()`
remaps UART0 to GPIO13/GPIO15 so the USB stays usable for flashing. The ESP8266's
second UART (UART1) is TX-only (GPIO2), so it can only be a one-way debug log.

---

## The link

- 921600 8N1, verified clean on the ESP32. Arch-neutral byte transport
  (`src/esplink.h`), implemented per MCU: `src/lpc175x/uart0.c` (UART0) and
  `src/stm32f4xx/usart_stm32.c` (USART6). Gated by `ESPLINK_ENABLE` (1 on LPC,
  0 on STM32).
- Compact framed protocol, byte-identical on both sides (`src/uart_proto.c` on the
  MCU == `companion/src/proto.h` on the ESP):
  `SOF(0x7E) | TYPE | OPCODE | SEQ | LEN | PAYLOAD | CRC16`. CRC16/ARC.
- Non-blocking by design (modeled on the cover state machine): the MCU is a pure
  server, advancing one bounded step per main-loop poll, and never times out
  waiting on the ESP (all retries live on the ESP side). File ops reuse FatFs with
  a private `FIL`, so the SNES stays responsive during transfers.
- Opcodes: `0x00-0x0F` file ops (PING/LS/STAT/GET/PUT/RM/MV/MKDIR/ABORT),
  `0x10-0x12` Wi-Fi bridge (POLL/REPORT/SCAN) for the in-menu Wi-Fi config.
  Reserved ranges for later phases: `0x20` firmware update, `0x30` cover/TFT,
  `0x40` theme/config.
- ESP8266 note: at 921600 with Wi-Fi active the ESP8266 (128-byte FIFO, no DMA)
  can drop a byte occasionally; the protocol's CRC + retries cover it, at the cost
  of a few more retries than the ESP32.

---

## WebUI (shared)

The WebUI is in `companion/src/www/` (`index.html` + `style.css` + `app.js`).
`companion/tools/gen_www.sh` gzips them into `companion/src/assets.cpp` as PROGMEM
byte arrays (kept in flash; PROGMEM is harmless on the ESP32), served with
`send_P`. Edit the WebUI there and re-run the script.

Connect to Wi-Fi `sd2snes-XXXX` (password `sd2snes0`) and open `http://192.168.4.1/`
(or the LAN IP once it joins your router).

- File manager: browse folders, download and upload with a live progress bar
  (speed and ETA; the transfer is baud-limited), rename, delete, new folder,
  `..` to go up, loading spinner for slow cards.
- Firmware update (OTA): open a release `.zip` in the browser, pick the files
  (pre-selected per device_name), write them to `/sd2snes/`; a `firmware.*` prompts
  to power-cycle.
- Multi-language (EN/PT/ES), auto-selected from the sd2snes menu language.
- Wi-Fi setup: scan, connect, status, forget. Runs AP+STA, persists credentials,
  auto-connects on boot, and drops the SoftAP once joined to a router.
- Hides OS junk (`.fseventsd`, `.Spotlight-V100`, `.Trashes`, `._*`, etc.) and is
  mobile-friendly (zoom locked, rows reflow, drag-and-drop hidden on touch).

---

## ESP self-update (from the SD)

The companion updates itself from the SD card over the same UART link - **flash if
different**, no version numbers, exactly like the sd2snes MCU bootloader re-flashes
`firmware.imX` when the content differs. After the first install you never re-attach
USB. It needs just one file: `/sd2snes/espXX.bin` (`esp32.bin` or `esp8266.bin`,
per chip).

How it works:
- The build appends a 16-byte trailer to `firmware.bin` (`tools/append_fwtrailer.py`):
  `"SD2ESPFW" + crc32(image)`. The ESP bootloader and the Update library use the
  image's own declared length and ignore those trailing bytes, so the trailered
  `.bin` still flashes/boots normally over USB.
- At boot the ESP reads just the last 16 bytes of `/sd2snes/espXX.bin` (the MCU GET
  seeks, so it does not read the whole file) and compares that CRC to the one it
  last applied (kept in EEPROM). If they **differ**, it pulls the image over the
  link, self-flashes with the Arduino `Update` library, stores the new CRC, and
  restarts. Same CRC -> nothing happens (no loop). No `espXX.bin` -> nothing.
- `FW_VERSION_STR` in `companion/src/version.h` is **only** the label shown on the
  SNES "System Information" screen (`Companion: <str> (ESP32)`). It does NOT affect
  updating - it is purely cosmetic.

Release a new ESP firmware:
1. (Optional) set `FW_VERSION_STR` in `companion/src/version.h` for the displayed
   label. You do NOT need to bump anything for the update to trigger.
2. Build (the CRC trailer is added automatically):
   ```sh
   cd companion
   ~/.platformio/penv/bin/pio run            # both, or -e esp32dev / -e nodemcuv2
   ```
3. Copy the image onto the card as `/sd2snes/espXX.bin`:
   - ESP32:   `.pio/build/esp32dev/firmware.bin`  -> `/sd2snes/esp32.bin`
   - ESP8266: `.pio/build/nodemcuv2/firmware.bin` -> `/sd2snes/esp8266.bin`

   via a card reader, the WebUI file manager (upload into `/sd2snes/`), or USB:
   ```sh
   python3 utils/sd2snes-usb/usb_put.py put \
       companion/.pio/build/esp32dev/firmware.bin /sd2snes/esp32.bin
   ```
4. Reboot the ESP (reset/power-cycle). It sees the different CRC, self-flashes, and
   restarts. Verify with `curl http://<esp-ip>/api/ping` (`"fw"`) or on the SNES
   System Information.

First install (one time): there is no firmware to self-update from yet, so the
first flash is over USB - `pio run -e esp32dev -t upload` (ESP32) or
NodeMCU-PyFlasher with `.pio/build/nodemcuv2/firmware.bin` (ESP8266). After that,
use the SD procedure above.

Safety: the `Update` library validates the image (magic + size + MD5) and writes
the **inactive** OTA slot, so a corrupt/partial `.bin` or a power loss mid-flash
leaves the running firmware intact (it just keeps running and tries again next
boot). A `.bin` without the trailer (wrong magic) is ignored, and the per-chip
filenames keep an ESP32 image from landing on an ESP8266. Note: right after a USB
install, if a `.bin` is also on the card, the first boot re-applies it once (the
EEPROM record does not match yet) - harmless; remove the SD `.bin` to avoid it.

---

## Prerequisites

ESP side (PlatformIO Core):

```sh
# 1) Install PlatformIO Core (pick one):
pip3 install -U platformio
#   or the official installer:
# python3 -c "$(curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py)"

# 2) Locate the CLI. pip puts `pio` on PATH; the installer / VS Code extension
#    put it under ~/.platformio. The full path used in this repo is:
#       ~/.platformio/penv/bin/pio
#    Add it to PATH if you prefer plain `pio`:
# export PATH="$HOME/.platformio/penv/bin:$PATH"

# 3) Sanity check:
~/.platformio/penv/bin/pio --version
```

The first `pio run` downloads the right platform automatically (no manual SDK
install): `espressif32` + Arduino-ESP32 for the `esp32dev` env, `espressif8266` +
Arduino-ESP8266 for the `nodemcuv2` env. `gen_www.sh` also needs `gzip` and `xxd`
(standard on macOS/Linux).

MCU side: the standard sd2snes toolchain (`arm-none-eabi-gcc`, plus `snescom` for
the menu). In this fork the MCU is compiled on a remote build server; the `make`
commands below assume that toolchain is on PATH.

---

## Build and flash

### ESP companion (companion/)

One Arduino project, two PlatformIO envs (`esp32dev` / `nodemcuv2`):

```sh
cd companion

# Regenerate the embedded WebUI after editing src/www/{index.html,style.css,app.js}:
./tools/gen_www.sh

# Build both targets (first run downloads each platform/toolchain):
~/.platformio/penv/bin/pio run

# ...or build one firmware at a time:
~/.platformio/penv/bin/pio run -e esp32dev     # -> .pio/build/esp32dev/firmware.bin
~/.platformio/penv/bin/pio run -e nodemcuv2    # -> .pio/build/nodemcuv2/firmware.bin

# Flash over USB (pick the env / port for the connected board):
~/.platformio/penv/bin/pio run -e esp32dev  -t upload --upload-port /dev/cu.usbserial-110
~/.platformio/penv/bin/pio run -e nodemcuv2 -t upload
```

Each build writes a flashable image at `companion/.pio/build/<env>/firmware.bin`:

| Firmware | Build command | Output `.bin` |
|----------|---------------|---------------|
| ESP32    | `pio run -e esp32dev`  | `companion/.pio/build/esp32dev/firmware.bin`  |
| ESP8266  | `pio run -e nodemcuv2` | `companion/.pio/build/nodemcuv2/firmware.bin` |

List ports with `ls /dev/cu.*` (macOS), `ls /dev/ttyUSB*` (Linux), or `pio device list`.

ESP32 debug log: `pio device monitor -e esp32dev` (115200, over USB). ESP8266 debug
log: `Serial1` on GPIO2 (TX-only), since UART0 is the MCU link; the USB serial is
free for flashing but carries no runtime log.

ESP8266 without `pio` upload: flash `companion/.pio/build/nodemcuv2/firmware.bin`
with **NodeMCU-PyFlasher** (select the `.bin`, Flash mode DIO, "Erase flash" on the
first time; the single `.bin` goes at offset `0x00000`).

### MCU (src/ + snes/)

```sh
make -C src  CONFIG=config-mk3      # produces src/obj-mk3/firmware.im3
make -C snes                        # produces snes/m3nu.bin (the menu)
# stage both to the SD over USB, then ONE physical cold-boot flashes the firmware:
#   usb_put.py put firmware.im3 /sd2snes/firmware.im3
#   usb_put.py put m3nu.bin     /sd2snes/m3nu.bin
# (menu-only changes can be reloaded with usb_cmd.py MENU_RESET, no cold-boot)
```

(`config-mk2` produces `firmware.img`, `config-mk3-stm32` produces `firmware.stm`.)
Firmware and menu must come from the same build (shared SRAM layout).

---

## Status

### Done
- [x] Non-blocking UART link (LPC UART0 @ 921600), verified clean
- [x] Compact CRC-framed protocol, arch-neutral (`esplink`)
- [x] MCU file server over UART (PING/LS/STAT/GET/PUT/RM/MV/MKDIR/ABORT), reusing FatFs, non-blocking
- [x] Companion firmware: one Arduino codebase for ESP32 and ESP8266 (SoftAP + STA + HTTP server)
- [x] ESP8266 single-UART support (UART0 swapped to GPIO13/15, USB free for flashing)
- [x] WebUI file manager (browse / download / upload with progress / rename / delete / mkdir / up-dir)
- [x] WebUI polish (mobile reflow, zoom lock, loading spinner, hidden OS junk files)
- [x] WebUI firmware update (open release zip in the browser, write selected files)
- [x] Multi-language WebUI (EN/PT/ES, auto-selected from the sd2snes menu language)
- [x] Wi-Fi configuration via WebUI (scan / connect / status / forget, AP+STA, auto-connect)
- [x] Wi-Fi configuration from the m3nu menu (on-screen keyboard on the SNES, no second device)
- [x] ESP self-update from the SD (boot-time `/sd2snes/espXX.bin`; flash-if-different via a CRC32 in the `.bin` trailer, no version numbers)
- [x] Builds for all three MCUs (mk2 / mk3 / mk3-stm32); STM32 USART6 driver written

### To do
- [ ] Test on mk2 and mk3-stm32. Currently build-validated only (mk3/LPC1756 is the tested unit; mk2 has no hardware here; the STM32 link is `ESPLINK_ENABLE=0` and PC6/PC7 are not confirmed broken out on any PCB).
- [ ] Wi-Fi status icon in m3nu (show connected/IP on the SNES menu).
- [ ] Cover (`.cov`) on an IPS/TFT wired to the ESP (decode the cover and mirror the highlighted game). Protocol range `0x30`.
- [ ] Trigger the ESP self-update from the m3nu menu (it is automatic at boot today).
- [ ] Update the sd2snes from the WebUI, OTA-style for the MCU flash trigger. Protocol range `0x20`.
- [ ] Optional ESP8266 OLED (show IP / status). Confirm it is I2C (GPIO4/5) so it does not clash with the UART on GPIO13/15.

---

## Notes and caveats
- Flashing the MCU firmware needs one physical cold-boot (SD-based bootloader); it cannot be triggered over USB.
- ESP8266 at 921600 under Wi-Fi load can drop the odd byte (no UART DMA); CRC + retries keep transfers correct but a bit slower than the ESP32. If transfers get unreliable, a lower baud is possible but needs a matching MCU build.
- STM32: `USART1` is impossible (all its pins are taken); `USART6` (PC6/PC7) is the only free pair, but accessibility on real PCBs is unconfirmed. Verify the pins and the BRR on a scope before flipping `ESPLINK_ENABLE` to 1.
