# sd2snes / FXPAK PRO - ESP32 Companion

A companion **ESP32-WROOM-32** wired to the cartridge's MCU over a private UART
turns the sd2snes into a network-managed device: browse and transfer the SD card
from a phone/PC over Wi-Fi, with the protocol designed so the SNES is **never
blocked** during transfers.

```
[SNES menu (m3nu)] --SNES_CMD/$55--> [MCU] <==UART 921600 8N1==> [ESP32] <--Wi-Fi/HTTP--> [browser]
                                       LPC1756 UART0 (P0.2/P0.3)   UART2 (GPIO16/17)        SoftAP 192.168.4.1
```

The ESP firmware lives in `esp32/` (PlatformIO + ESP-IDF). The MCU side lives in
`src/` (the C firmware) and is the same `m3nu.bin` / firmware you already build.

---

## Hardware and wiring

| MCU (board) | Link peripheral | TX pin | RX pin | Status |
|-------------|-----------------|--------|--------|--------|
| LPC1756 (mk3 / FXPAK PRO) | UART0 | P0.2 = pin 79 | P0.3 = pin 80 | active, tested |
| LPC1754 (mk2) | UART0 | P0.2 = pin 79 | P0.3 = pin 80 | active, build-only |
| STM32F401 (FXPAK PRO STM32) | USART6 | PC6 | PC7 | driver built but OFF (pins unconfirmed) |

Wiring (LPC): MCU pin 79 (TX) goes to ESP GPIO16 (RX2), MCU pin 80 (RX) goes to
ESP GPIO17 (TX2), plus a common GND. Everything is 3.3 V, no level shifter.
Power the ESP separately for now (its own USB); Wi-Fi current peaks need a solid
3.3 V supply if you wire it permanently.

Why UART0 on the LPC: it is the LPC's ISP bootloader UART, so it is broken out and
free; using it from the running firmware is safe (ISP entry is not triggered on
this board, and SD-based firmware flashing does not use it).

---

## The link

- 921600 8N1, verified clean. Arch-neutral byte transport (`src/esplink.h`),
  implemented per MCU: `src/lpc175x/uart0.c` (UART0) and
  `src/stm32f4xx/usart_stm32.c` (USART6). Gated by `ESPLINK_ENABLE` (1 on LPC,
  0 on STM32).
- Compact framed protocol (`src/uart_proto.c` and `esp32/src/proto.h`):
  `SOF(0x7E) | TYPE | OPCODE | SEQ | LEN | PAYLOAD | CRC16`. CRC16/ARC.
- Non-blocking by design (modeled on the cover state machine): the MCU is a pure
  server, advancing one bounded step per main-loop poll, and never times out
  waiting on the ESP (all retries live on the ESP side). File ops reuse FatFs with
  a private `FIL`, so the SNES stays responsive during transfers.
- Opcodes: `0x00-0x0F` file ops (PING/LS/STAT/GET/PUT/RM/MV/MKDIR/ABORT).
  Reserved ranges for later phases: `0x10` Wi-Fi, `0x20` firmware update,
  `0x30` cover/TFT, `0x40` theme/config.

---

## WebUI (ESP SoftAP)

Connect to Wi-Fi `sd2snes-XXXX` (password `sd2snes0`) and open
`http://192.168.4.1/`.

- File manager: browse folders, download and upload with a live progress bar
  (speed and ETA; the transfer is baud-limited), rename, delete, new folder,
  `..` to go up, loading spinner for slow cards.
- Hides OS junk (`.fseventsd`, `.Spotlight-V100`, `.Trashes`, `._*`, `Thumbs.db`,
  `System Volume Information`, and so on).
- Mobile-friendly: zoom locked, rows reflow (name on its own line), drag-and-drop
  zone hidden on touch.
- Wi-Fi setup: scan networks, connect with password, status, forget. Runs AP+STA
  (the SoftAP stays up), persists credentials in NVS, auto-connects on boot, and
  redirects to the new LAN IP once connected.

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

The first `pio run` automatically downloads the `espressif32` platform, the
ESP-IDF framework, and the Xtensa toolchain into `~/.platformio` (no manual SDK
install). `gen_www.sh` also needs `gzip` and `xxd` (standard on macOS/Linux).

MCU side: the standard sd2snes toolchain (`arm-none-eabi-gcc`, plus `snescom`
for the menu). In this fork the MCU is compiled on a remote build server; see
`prepare.sh` / `build.sh` for provisioning. The `make` commands below assume that
toolchain is on PATH.

---

## Build and flash

### ESP (esp32/)

```sh
cd esp32

# Regenerate the embedded WebUI after editing www/index.html, www/style.css or www/app.js:
./tools/gen_www.sh

# Build (first run downloads the toolchain/framework):
~/.platformio/penv/bin/pio run

# Flash over USB:
~/.platformio/penv/bin/pio run -t upload

# Serial debug log (115200):
~/.platformio/penv/bin/pio device monitor
```

(If `pio` is on your PATH, drop the `~/.platformio/penv/bin/` prefix.)
The USB port is set in `esp32/platformio.ini` (`upload_port` / `monitor_port`,
default `/dev/cu.usbserial-110`); change it to your device. List ports with
`ls /dev/cu.*` (macOS), `ls /dev/ttyUSB*` (Linux), or `pio device list`.

The WebUI sources live in `esp32/src/www/` (`index.html` + `style.css` + `app.js`);
each is gzipped and embedded as a C array in `www_assets.c` and served from `/`,
`/style.css`, `/app.js`. Run `tools/gen_www.sh` whenever you edit any of them.

### MCU (src/)

```sh
make -C src CONFIG=config-mk3 VERSION='*'      # produces src/obj-mk3/firmware.im3
# stage to the SD over USB, then ONE physical cold-boot flashes it:
#   usb_cmd.py MENU_RESET ; usb_put.py put firmware.im3 /sd2snes/firmware.im3
```

(`config-mk2` produces `firmware.img`, `config-mk3-stm32` produces `firmware.stm`.)
The ESP file-manager code is config-independent; no menu (`m3nu.bin`) change is
required for the WebUI.

---

## Status

### TO DO
- [x] Non-blocking UART link (LPC UART0 to ESP UART2 @ 921600), verified clean
- [x] Compact CRC-framed protocol, arch-neutral (`esplink`)
- [x] MCU file server over UART (PING/LS/STAT/GET/PUT/RM/MV/MKDIR/ABORT), reusing FatFs, non-blocking
- [x] ESP SoftAP + HTTP server
- [x] WebUI file manager (browse / download / upload with progress / rename / delete / mkdir / up-dir)
- [x] WebUI polish (mobile reflow, zoom lock, loading spinner, hidden OS junk files)
- [x] Wi-Fi configuration via WebUI (scan / connect / status / forget, AP+STA, NVS auto-connect)
- [x] Builds for all three MCUs (mk2 / mk3 / mk3-stm32); STM32 USART6 driver written
- [ ] Test on mk2 and mk3-stm32. Currently build-validated only (mk3/LPC1756 is the tested unit; mk2 has no hardware here; the STM32 link is `ESPLINK_ENABLE=0` and PC6/PC7 are not confirmed broken out on any PCB).
- [ ] Configure Wi-Fi from the m3nu menu (on-screen keyboard on the SNES, no second device needed). Protocol range `0x10`.
- [ ] Wi-Fi status icon in m3nu (show connected/IP on the SNES menu).
- [x] Multi-language WebUI (EN/PT/ES, auto-selected from the sd2snes menu language reported in PING).
- [ ] Cover (`.cov`) on an IPS/TFT wired to the ESP (decode the cover and mirror the highlighted game). Protocol range `0x30`.
- [ ] Update the ESP32 from m3nu (push/flash ESP firmware from the SNES menu).
- [ ] Update the sd2snes from the WebUI, OTA-style (fetch a firmware release over Wi-Fi, write `firmware.im3` / `.stm` to the SD, then trigger the flash). Protocol range `0x20`.

---

## Notes and caveats
- Flashing the MCU firmware needs one physical cold-boot (SD-based bootloader); it cannot be triggered over USB.
- STM32: `USART1` is impossible (all its pins are taken); `USART6` (PC6/PC7) is the only free pair, but accessibility on real PCBs is unconfirmed. Verify the pins and the BRR on a scope before flipping `ESPLINK_ENABLE` to 1.
```
