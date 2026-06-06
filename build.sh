#!/usr/bin/env bash
#
# One-shot builder for the ludufre sd2snes fork.
#
#   1. (remote) compiles the MCU firmware (firmware.img/.im3/.stm) and the SNES
#      menu (menu.bin / m3nu.bin) on the build server (arm-none-eabi-gcc + snescom).
#   2. (local) compiles the ESP companion for both targets (esp32.bin / esp8266.bin)
#      with PlatformIO.
#   3. packages two zips into release/:
#        sd2snes_firmware_v<VER>.zip       (fork binaries only)
#        sd2snes_firmware_v<VER>-full.zip  (official base + fork bins)
#
# VER  = RELEASE_VERSION in src/VERSION (e.g. 1.11.2-br-3.0b).
# BASE = the upstream sd2snes release the full zip overlays (OFFICIAL_VER).
#
# Env overrides: SERVER (user@host), SERVER_DIR, OFFICIAL_VER, OFFICIAL_URL, PIO.
# Flags: --no-mcu (skip the remote build), --no-esp (skip the companion), -h/--help.
set -euo pipefail

SERVER="${SERVER:-root@172.29.8.103}"
SERVER_PORT="${SERVER_PORT:-22}"
SERVER_DIR="${SERVER_DIR:-/root/sd2snes}"
OFFICIAL_VER="${OFFICIAL_VER:-1.11.2}"
OFFICIAL_URL="${OFFICIAL_URL:-https://sd2snes.de/files/sd2snes_firmware_v${OFFICIAL_VER}.zip}"
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"

REPO="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# The release version = RELEASE_VERSION in src/VERSION (the same string the MCU
# build stamps into the firmware), e.g. 1.11.2-br-3.0b.
release_version() {
  sed -n 's/.*RELEASE_VERSION[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
      src/VERSION | head -1
}

# name|relative-source-path for every fork-modified binary (one per line; no spaces).
# Listed once; reused for both zips and for the missing-file check.
artifact_list() {
  cat <<'EOF'
firmware.img|bin/firmware.img
firmware.im3|bin/firmware.im3
firmware.stm|bin/firmware.stm
menu.bin|bin/menu.bin
m3nu.bin|bin/m3nu.bin
menu.spc|misc/menu.spc
esp32.bin|companion/.pio/build/esp32dev/firmware.bin
esp8266.bin|companion/.pio/build/nodemcuv2/firmware.bin
EOF
}

# --- 1. MCU firmware + SNES menu, on the build server ------------------------
build_mcu_remote() {
  log "Syncing source to $SERVER:$SERVER_DIR"
  rsync -az --delete --exclude 'obj*/' --exclude '.dep*/' --exclude '*.o' \
        -e "ssh -p $SERVER_PORT" \
        src/ "$SERVER:$SERVER_DIR/src/"
  rsync -az --exclude '*.o65' --exclude '*.lst' --exclude '*.bin' \
        --exclude 'const_lang.a65' --exclude '.dep/' \
        -e "ssh -p $SERVER_PORT" \
        snes/ "$SERVER:$SERVER_DIR/snes/"

  log "Compiling MCU firmware (mk2 / mk3 / mk3-stm32) + menu on $SERVER"
  ssh -p "$SERVER_PORT" "$SERVER" "set -e; cd '$SERVER_DIR'; \
    make -C utils >/dev/null; \
    make -C src/utils >/dev/null; \
    touch src/.ARG_VERSION; \
    for cfg in config-mk2 config-mk3 config-mk3-stm32; do \
      echo \"  [\$cfg]\"; \
      make -C src CONFIG=\$cfg clean >/dev/null 2>&1 || true; \
      make -C src CONFIG=\$cfg >/dev/null; \
    done; \
    echo '  [menu]'; \
    make -C snes clean >/dev/null 2>&1 || true; \
    make -C snes >/dev/null"

  log "Fetching MCU/menu binaries into bin/"
  mkdir -p bin
  scp -P "$SERVER_PORT" -q "$SERVER:$SERVER_DIR/src/obj-mk2/firmware.img"       bin/firmware.img
  scp -P "$SERVER_PORT" -q "$SERVER:$SERVER_DIR/src/obj-mk3/firmware.im3"       bin/firmware.im3
  scp -P "$SERVER_PORT" -q "$SERVER:$SERVER_DIR/src/obj-mk3-stm32/firmware.stm" bin/firmware.stm
  scp -P "$SERVER_PORT" -q "$SERVER:$SERVER_DIR/snes/m3nu.bin"                  bin/m3nu.bin
  scp -P "$SERVER_PORT" -q "$SERVER:$SERVER_DIR/snes/menu.bin"                  bin/menu.bin
}

# --- 2. ESP companion (ESP32 + ESP8266), locally with PlatformIO -------------
build_companion_local() {
  [ -x "$PIO" ] || die "PlatformIO not found at $PIO (set \$PIO)."
  log "Regenerating WebUI assets (companion)"
  ( cd companion && bash tools/gen_www.sh )
  log "Compiling ESP companion (esp32dev + nodemcuv2)"
  ( cd companion && "$PIO" run )
}

# --- 3. package the two zips -------------------------------------------------
package() {
  local ver; ver="$(release_version)"
  [ -n "$ver" ] || die "could not read RELEASE_VERSION from src/VERSION"
  local stem="sd2snes_firmware_v${ver}"
  mkdir -p release dist

  local e n s
  for e in $(artifact_list); do
    s="${e#*|}"
    [ -f "$s" ] || die "missing build artifact: $s  (run without --no-mcu/--no-esp?)"
  done

  # modified-only zip: just the fork binaries, under sd2snes/ (overlay layout)
  local md; md="$(mktemp -d)"; mkdir -p "$md/sd2snes"
  for e in $(artifact_list); do n="${e%%|*}"; s="${e#*|}"; cp "$s" "$md/sd2snes/$n"; done
  rm -f "release/${stem}.zip"
  ( cd "$md" && zip -rq "$REPO/release/${stem}.zip" sd2snes )
  rm -rf "$md"
  log "wrote release/${stem}.zip  (fork binaries only)"

  # full zip: official base + fork binaries overlaid (and esp*/spc added)
  local base="dist/sd2snes_firmware_v${OFFICIAL_VER}.zip"
  if [ ! -s "$base" ]; then
    log "Downloading official base: $OFFICIAL_URL"
    curl -fsSL -o "$base" "$OFFICIAL_URL" || die "download failed: $OFFICIAL_URL"
  fi
  local fd; fd="$(mktemp -d)"
  unzip -q "$base" -d "$fd"
  [ -d "$fd/sd2snes" ] || die "unexpected base zip layout (no sd2snes/ dir)"
  for e in $(artifact_list); do n="${e%%|*}"; s="${e#*|}"; cp "$s" "$fd/sd2snes/$n"; done
  rm -f "release/${stem}-full.zip"
  ( cd "$fd" && zip -rq "$REPO/release/${stem}-full.zip" sd2snes )
  rm -rf "$fd"
  log "wrote release/${stem}-full.zip  (official base + fork binaries)"

  printf '\nOutputs (v%s):\n  release/%s.zip\n  release/%s-full.zip\n' "$ver" "$stem" "$stem"
}

main() {
  local do_mcu=1 do_esp=1 arg
  for arg in "$@"; do case "$arg" in
    --no-mcu) do_mcu=0 ;;
    --no-esp) do_esp=0 ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    *) die "Unknown flag: $arg (try --help)" ;;
  esac; done

  if [ "$do_mcu" = 1 ]; then build_mcu_remote; else warn "Skipping MCU/menu build (--no-mcu); reusing bin/"; fi
  if [ "$do_esp" = 1 ]; then build_companion_local; else warn "Skipping companion build (--no-esp); reusing companion/.pio/"; fi
  package
}

main "$@"
