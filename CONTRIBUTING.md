# Contributing / Building from source

This repo has three independent pieces that build separately: the **SNES
menu** (65816 assembly, `snes/`), the **MCU firmware** (C, `src/`), and the
**FPGA cores** (Verilog, `verilog/`, needs Xilinx ISE — not covered here).
Most contributions only touch the menu or the firmware, so that's what this
document covers, on both Linux and Windows.

CI (`.github/workflows/ci.yml`) builds both on every push/PR — use it as the
canonical, always-up-to-date recipe if anything below drifts.

## 1. The SNES menu (`snes/`)

Building it needs `snescom`/`sneslink`, a 65816 assembler+linker by
[Bisqwit](https://bisqwit.iki.fi/source/snescom.html). It isn't packaged
anywhere, so it has to be built from source once.

### Linux / WSL

```bash
curl -fsSL -o snescom.tar.gz https://bisqwit.iki.fi/src/arch/snescom-1.8.1.1.tar.gz
tar xzf snescom.tar.gz
make -C snescom-1.8.1.1
sudo make -C snescom-1.8.1.1 install    # installs to /usr/local/bin by default
```

That's it — a stock `gcc`/`g++` (any recent one; the code needs C++17) builds
it cleanly. Despite the old `snes/README`, **Boost is not actually required**
by this version of the source.

### Windows

There is no prebuilt Windows binary, and it does not build with plain
MinGW-w64 (`g++`/`gcc.exe` from most Windows toolchains): `snescom` shells out
to `gcc -E -` for its preprocessing step using raw POSIX `fork()`/`pipe()`/
`execlp()`, none of which exist in the MinGW-w64 CRT.

What *does* work is **MSYS2's own `gcc` package** (the "msys" subsystem
build, not `mingw-w64-x86_64-gcc`) — its runtime (`msys-2.0.dll`) emulates
`fork()` properly:

```bash
# from an MSYS2 shell (or any bash with pacman on PATH, e.g. a devkitPro install)
pacman -S gcc

curl -fsSL -o snescom.tar.gz https://bisqwit.iki.fi/src/arch/snescom-1.8.1.1.tar.gz
tar xzf snescom.tar.gz
cd snescom-1.8.1.1

# One flag needs changing: with plain `-std=c++1z`, libstdc++ hides fileno()/
# ftruncate() (POSIX extensions) unless a feature-test macro is defined first.
# `-std=gnu++1z` turns those macros on by default.
sed -i 's/-std=c++1z/-std=gnu++1z/' Makefile

make
# snescom.exe / sneslink.exe land in this directory -- copy them wherever
# you keep local tools and put that on PATH.
```

If your Windows setup only has MinGW-w64 (`mingw64/bin/gcc.exe`) and no
MSYS2 `gcc`, the build fails at `precompile.cc` with
`fatal error: sys/wait.h: No such file or directory` — that's the signal
you're on the wrong `gcc`.

### Building the menu itself

Once `snescom`/`sneslink` are on `PATH`:

```bash
cd snes
make
```

This produces `menu.bin` (Mk.II), `m3nu.bin` (Mk.III/FXPak Pro) and
`igmenu.bin` (the in-game menu shell). Copy the one matching your hardware to
`<sdcard>/sd2snes/menu.bin` (or `m3nu.bin`) to test.

`snescom`/`sneslink` also need `cpp` and `python3` on `PATH` (the latter
generates the multilingual string tables — `utils/build_const.py` — before
assembling). On Windows, if `python3` resolves to the Microsoft Store stub
instead of a real interpreter, point a wrapper script at your actual
`python.exe` and put that first on `PATH`.

## 2. The MCU firmware (`src/`)

Needs an ARM GCC toolchain (`arm-none-eabi-gcc`) — both the LPC175x boards
(Mk.II, Mk.III) and the STM32F4 board target Cortex-M.

```bash
# Linux: apt-get install gcc-arm-none-eabi (or grab the ARM-hosted release)
# Windows: the "GNU Arm Embedded Toolchain" installer from ARM, or via a
#          package manager (e.g. `choco install gcc-arm-embedded`)

cd src
make -C utils                 # builds genhdr/lpcchksum -- host tools the main
                               # build assumes already exist; easy to miss
make CONFIG=config-mk2         # or config-mk3 / config-mk3-stm32
```

The build embeds the FPGA core's bitstream into the firmware header
(`CONFIG_CFGWARE`, read from `verilog/sd2snes_<core>/...`), so a *complete,
flashable* image needs that core already built with Xilinx ISE, which this
document doesn't cover. If you only want to confirm your C changes compile
and link cleanly (which is most of what a menu/firmware PR needs), any
same-sized placeholder file works as a stand-in for local iteration — CI does
exactly this. Don't ship a build made this way.

The build runs under `-Wall -Wstrict-prototypes -Werror`: a warning fails the
build, by design. If your change makes one appear, fix it rather than
work around `-Werror` — the project relies on it staying at zero.

## 3. Running the test suite

```bash
# ASan/UBSan-instrumented host tests (patch apply/probe, YAML writer, CRC16,
# string utils, cheat trainer, NES CHR conversion, ...):
cd tests/host
for f in run*.sh; do bash "$f" || echo "FAILED: $f"; done

# i18n glyph/accent table parity (snes/font.a65 vs the Python encode tables):
python3 tests/test_i18n_parity.py
```

These need a plain host `cc` with ASan/UBSan support (mainstream Linux/macOS
gcc or clang; MSYS2's `gcc` package on Windows does **not** ship the
sanitizer runtimes, so `tests/host/*.sh` won't link there — use WSL or CI to
run them if you're on Windows).

## 4. Editing the menu font

`snes/utils/fontedit.py` can render any glyph to the terminal
(`python3 fontedit.py show <code>`) or export/import the whole 8x8 tile sheet
as a PNG for pixel-editing (`export`/`import`). See the module docstring at
the top of the file for the full round-trip workflow, including the
transparent/body/outline/mid-tone colour contract the PNG sheet uses.
