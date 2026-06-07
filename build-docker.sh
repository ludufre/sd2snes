#!/usr/bin/env bash
#
# Local Docker build of the full ludufre sd2snes fork - no server, no VPN.
#
# Same outputs as build.sh, but everything compiles locally inside a container
# (docker/build/Dockerfile) instead of on the remote build server over SSH:
#   1. MCU firmware (firmware.img/.im3/.stm) + SNES menu (menu.bin/m3nu.bin)
#   2. ESP companion (esp32.bin / esp8266.bin) with PlatformIO
#   3. two zips in release/:
#        sd2snes_firmware_v<VER>.zip       (fork binaries only)
#        sd2snes_firmware_v<VER>-full.zip  (official base + fork bins)
#
# VER  = RELEASE_VERSION in src/VERSION (e.g. 1.11.2-br-3.0b).
# BASE = the upstream sd2snes release the full zip overlays (OFFICIAL_VER).
#
# FPGA bootstrap cores (cfgware) embedded in the firmware, both synthesized in
# Docker - the fork never changes the FPGA, so each is built once:
#   mk3/STM32 -> fpga_mini.bi3 (Cyclone IV): Quartus, baked in the main image.
#   mk2       -> fpga_mini.bit (Spartan-3):  Xilinx ISE, via --with-ise (heavy,
#               emulated ~15 min; needs the ISE installer + license in XILINX_SRC).
# Without --with-ise the existing verilog/sd2snes_mini/fpga_mini.bit is reused.
# The runtime cores (fpga_base.* ...) are unchanged and come from the official base.
#
# The repo is bind-mounted into the container and built in place, so obj-*/ and
# companion/.pio land straight in the working tree (gitignored). On Docker
# Desktop for Mac those files come back owned by you, not root.
#
# Env overrides: IMAGE, ISE_IMAGE, PIO_VOL, XILINX_SRC, OFFICIAL_VER, SNESCOM_VER.
# Flags: --with-ise (synthesize the mk2 cfgware with ISE in Docker), --rebuild-image
#        (force docker build), --reuse (just repackage existing artifacts), -h/--help.
set -euo pipefail

IMAGE="${IMAGE:-sd2snes-build:local}"
ISE_IMAGE="${ISE_IMAGE:-sd2snes-ise:14.7}" # ISE WebPACK image (mk2 Spartan-3 cfgware)
PIO_VOL="${PIO_VOL:-sd2snes-pio}"          # named volume caching /root/.platformio
PLATFORM="${PLATFORM:-linux/amd64}"        # base image is amd64-only (emulated on Apple Silicon)
SNESCOM_VER="${SNESCOM_VER:-1.8.1.1}"
# ISE 14.7 installer parts + WebPACK license live here (login-gated; user-provided):
#   Xilinx_ISE_DS_14.7_1015_1-1.tar + -2/-3/-4.zip.xz + Xilinx.lic
XILINX_SRC="${XILINX_SRC:-$HOME/Downloads}"
QUARTUS_VER="${QUARTUS_VER:-23.1std.0.991}"   # Quartus Lite for the mk3 FPGA bootstrap synth
QUARTUS_BASE="${QUARTUS_BASE:-https://downloads.intel.com/akdlm/software/acdsinst/23.1std/991/ib_installers}"
# Relocatable Python for the PlatformIO venv (the base image's 3.6 is too old for
# the current esptool's cryptography dependency).
PYBS_TAG="${PYBS_TAG:-20260602}"
PYBS_VER="${PYBS_VER:-3.10.20}"
PYBS_URL="${PYBS_URL:-https://github.com/astral-sh/python-build-standalone/releases/download/${PYBS_TAG}/cpython-${PYBS_VER}+${PYBS_TAG}-x86_64-unknown-linux-gnu-install_only.tar.gz}"
OFFICIAL_VER="${OFFICIAL_VER:-1.11.2}"
OFFICIAL_URL="${OFFICIAL_URL:-https://sd2snes.de/files/sd2snes_firmware_v${OFFICIAL_VER}.zip}"

REPO="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# The release version = RELEASE_VERSION in src/VERSION (same string the MCU build
# stamps into the firmware), e.g. 1.11.2-br-3.0b.
release_version() {
  sed -n 's/.*RELEASE_VERSION[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
      src/VERSION | head -1
}

# name|relative-source-path for every fork-modified binary (one per line; no spaces).
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

# --- fetch the Quartus installer on the HOST (the old base image can't validate
#     Intel's CDN cert chain; the host can). Cached in dist/quartus/. ----------
ensure_quartus() {
  mkdir -p dist/quartus
  local run="dist/quartus/QuartusLiteSetup-${QUARTUS_VER}-linux.run"
  local qdz="dist/quartus/cyclone-${QUARTUS_VER}.qdz"
  if [ ! -s "$run" ]; then
    log "Downloading Quartus Lite installer (~2GB, one time): $run"
    curl -fL -o "$run" "${QUARTUS_BASE}/QuartusLiteSetup-${QUARTUS_VER}-linux.run" \
      || die "Quartus installer download failed (set QUARTUS_BASE/QUARTUS_VER)."
  fi
  if [ ! -s "$qdz" ]; then
    log "Downloading Cyclone IV device support (~1.5GB, one time): $qdz"
    curl -fL -o "$qdz" "${QUARTUS_BASE}/cyclone-${QUARTUS_VER}.qdz" \
      || die "Cyclone device file download failed (set QUARTUS_BASE/QUARTUS_VER)."
  fi
}

# --- fetch a relocatable Python for the PlatformIO venv (host TLS). Cached. ----
ensure_python() {
  mkdir -p dist/python
  local tgz="dist/python/cpython-${PYBS_VER}-x86_64-linux.tar.gz"
  if [ ! -s "$tgz" ]; then
    log "Downloading standalone Python ${PYBS_VER} (for PlatformIO): $tgz"
    curl -fL -o "$tgz" "$PYBS_URL" || die "Python download failed (set PYBS_URL)."
  fi
}

# --- reassemble the ISE installer (host) into dist/xilinx/ from XILINX_SRC -----
ensure_xilinx() {
  local inst="dist/xilinx/installer" v="Xilinx_ISE_DS_14.7_1015_1"
  if [ ! -x "$inst/bin/lin64/batchxsetup" ]; then
    [ -f "$XILINX_SRC/${v}-1.tar" ] || die "ISE installer not found: $XILINX_SRC/${v}-1.tar (set XILINX_SRC)"
    log "Reassembling ISE installer into $inst (part-1 tree + parts 2-4 alongside)"
    rm -rf "$inst"; mkdir -p "$inst"
    tar xf "$XILINX_SRC/${v}-1.tar" -C "$inst"
    local p
    for p in 2 3 4; do
      [ -f "$XILINX_SRC/${v}-${p}.zip.xz" ] || die "missing ISE installer part: $XILINX_SRC/${v}-${p}.zip.xz"
      cp "$XILINX_SRC/${v}-${p}.zip.xz" "$inst/"
    done
  fi
  mkdir -p dist/xilinx
  [ -s dist/xilinx/Xilinx.lic ] || cp "$XILINX_SRC/Xilinx.lic" dist/xilinx/Xilinx.lic \
    || die "missing WebPACK license: $XILINX_SRC/Xilinx.lic"
}

# --- synthesize the mk2 (Spartan-3) cfgware fpga_mini.bit with ISE in Docker ---
# Heavy + emulated (~15 min); opt-in via --with-ise. Writes the .bit into the tree.
build_ise_bit() {
  command -v docker >/dev/null 2>&1 || die "docker not found on PATH."
  ensure_xilinx
  log "Building ISE image $ISE_IMAGE + synthesizing fpga_mini.bit (Spartan-3, emulated - slow)"
  DOCKER_BUILDKIT=1 docker build --platform "$PLATFORM" -f docker/ise14.7/Dockerfile -t "$ISE_IMAGE" .
  log "Extracting fpga_mini.bit into verilog/sd2snes_mini/"
  local cid; cid="$(docker create --platform "$PLATFORM" "$ISE_IMAGE")"
  docker cp "$cid:/opt/cfgware/fpga_mini.bit" verilog/sd2snes_mini/fpga_mini.bit
  docker rm "$cid" >/dev/null
}

# --- build the image (only if missing, unless --rebuild-image) ----------------
ensure_image() {
  local force="$1"
  command -v docker >/dev/null 2>&1 || die "docker not found on PATH."
  if [ "$force" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    ensure_quartus
    ensure_python
    log "Building Docker image $IMAGE ($PLATFORM) - includes a one-time Quartus FPGA synth"
    docker build --platform "$PLATFORM" \
      --build-arg "SNESCOM_VER=$SNESCOM_VER" --build-arg "QUARTUS_VER=$QUARTUS_VER" \
      -f docker/build/Dockerfile -t "$IMAGE" .
  else
    log "Reusing Docker image $IMAGE (use --rebuild-image to force)"
  fi
}

# --- compile MCU + menu + companion inside the container ----------------------
container_build() {
  log "Compiling MCU (mk2 / mk3 / mk3-stm32) + menu + companion in Docker"
  docker run --rm \
    --platform "$PLATFORM" \
    -v "$REPO:/work" \
    -v "$PIO_VOL:/root/.platformio" \
    -w /work \
    "$IMAGE" bash -euo pipefail -c '
      # drop the prebuilt mk3 cfgware (synthesized into the image) into the tree
      mkdir -p verilog/sd2snes_mini
      cp /opt/cfgware/fpga_mini.bi3 verilog/sd2snes_mini/fpga_mini.bi3
      make -C utils >/dev/null
      make -C src/utils >/dev/null
      touch src/.ARG_VERSION
      for cfg in config-mk2 config-mk3 config-mk3-stm32; do
        echo "  [$cfg]"
        make -C src CONFIG=$cfg clean >/dev/null 2>&1 || true
        make -C src CONFIG=$cfg >/dev/null
      done
      echo "  [menu]"
      make -C snes clean >/dev/null 2>&1 || true
      make -C snes >/dev/null
      echo "  [companion]"
      cd companion
      bash tools/gen_www.sh
      pio run
    '
}

# --- gather MCU/menu binaries into bin/ (built in-tree by the container) ------
collect() {
  log "Collecting MCU/menu binaries into bin/"
  mkdir -p bin
  cp src/obj-mk2/firmware.img       bin/firmware.img
  cp src/obj-mk3/firmware.im3       bin/firmware.im3
  cp src/obj-mk3-stm32/firmware.stm bin/firmware.stm
  cp snes/m3nu.bin                  bin/m3nu.bin
  cp snes/menu.bin                  bin/menu.bin
}

# --- package the two zips (identical layout to build.sh) ----------------------
package() {
  local ver; ver="$(release_version)"
  [ -n "$ver" ] || die "could not read RELEASE_VERSION from src/VERSION"
  local stem="sd2snes_firmware_v${ver}"
  mkdir -p release dist

  local e s
  for e in $(artifact_list); do
    s="${e#*|}"
    [ -f "$s" ] || die "missing build artifact: $s  (run without --reuse?)"
  done

  # modified-only zip: just the fork binaries, under sd2snes/ (overlay layout)
  local md n; md="$(mktemp -d)"; mkdir -p "$md/sd2snes"
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
  local rebuild_image=0 reuse=0 with_ise=0 arg
  for arg in "$@"; do case "$arg" in
    --rebuild-image) rebuild_image=1 ;;
    --reuse|--no-build) reuse=1 ;;
    --with-ise) with_ise=1 ;;
    -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
    *) die "Unknown flag: $arg (try --help)" ;;
  esac; done

  if [ "$reuse" = 1 ]; then
    warn "Skipping Docker build (--reuse); repackaging existing artifacts"
  else
    # mk2 cfgware: synthesize fpga_mini.bit with ISE (--with-ise), else reuse the tree's.
    if [ "$with_ise" = 1 ]; then
      build_ise_bit
    elif [ ! -s verilog/sd2snes_mini/fpga_mini.bit ]; then
      die "verilog/sd2snes_mini/fpga_mini.bit missing - run with --with-ise to synthesize it (ISE in Docker)."
    fi
    ensure_image "$rebuild_image"   # the mk3 cfgware (fpga_mini.bi3) is baked here via Quartus
    container_build
    collect
  fi
  package
}

main "$@"
