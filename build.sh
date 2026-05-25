#!/usr/bin/env bash
#
# One-shot builder for the sd2snes firmware bundle.
#
# When run on the host (no Docker around the script), it:
#   1. Builds the sd2snes-build Docker image (if missing or --rebuild-image).
#   2. Re-invokes itself inside the container.
#
# When run inside the container, it:
#   - Compiles host utilities and the SNES menu from source.
#   - Generates bsxpage.bin.
#   - Downloads the official release zip (cached in dist/).
#   - Overlays the freshly built artifacts and repackages the zip.
#
# Strategy note: the firmware.img/.im3/.stm and fpga_*.bit/.bi3 in the output
# come from the official release. Rebuilding them requires Xilinx ISE / Quartus
# and the embedded fpga_mini cfgware, which aren't part of this repo.
#
# Env overrides:
#   OFFICIAL_VER=1.11.2     target release version
#   OFFICIAL_URL=...        full URL to the release zip
#
# Flags:
#   --rebuild-image         force docker image rebuild before running
#   --shell                 drop into an interactive shell inside the container

set -euo pipefail

OFFICIAL_VER="${OFFICIAL_VER:-1.11.2}"
OFFICIAL_URL="${OFFICIAL_URL:-https://sd2snes.de/files/sd2snes_firmware_v${OFFICIAL_VER}.zip}"
IMAGE_NAME="sd2snes-build:latest"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }

# ---------------------------------------------------------------------------
# Host side: orchestrate Docker.
# ---------------------------------------------------------------------------
run_on_host() {
    local rebuild_image=0
    local drop_shell=0
    for arg in "$@"; do
        case "$arg" in
            --rebuild-image) rebuild_image=1 ;;
            --shell)         drop_shell=1 ;;
            -h|--help)
                sed -n '2,30p' "$0"; exit 0 ;;
            *)
                warn "Unknown flag: $arg"; exit 2 ;;
        esac
    done

    if ! command -v docker >/dev/null 2>&1; then
        warn "docker not found in PATH. Install Docker Desktop first."
        exit 1
    fi

    local need_build=0
    if [ "$rebuild_image" = 1 ]; then
        need_build=1
    elif ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
        need_build=1
    fi

    if [ "$need_build" = 1 ]; then
        log "Building Docker image ($IMAGE_NAME)"
        docker compose build
    else
        log "Reusing existing Docker image ($IMAGE_NAME); pass --rebuild-image to force rebuild"
    fi

    if [ "$drop_shell" = 1 ]; then
        log "Launching interactive shell inside container"
        exec docker compose run --rm --entrypoint bash build
    fi

    log "Running build inside container"
    exec docker compose run --rm \
        -e OFFICIAL_VER="$OFFICIAL_VER" \
        -e OFFICIAL_URL="$OFFICIAL_URL" \
        -e SD2SNES_IN_CONTAINER=1 \
        build bash build.sh
}

# ---------------------------------------------------------------------------
# Container side: actually build.
# ---------------------------------------------------------------------------
run_in_container() {
    local repo_dir
    repo_dir="$(cd "$(dirname "$0")" && pwd)"
    local dist_dir="${repo_dir}/dist"
    local release_dir="${repo_dir}/release/v${OFFICIAL_VER}"
    local staging_dir="${release_dir}/sd2snes"
    local zip_name="sd2snes_firmware_v${OFFICIAL_VER}.zip"

    cd "$repo_dir"

    log "Cleaning previous build outputs"
    make -C utils clean >/dev/null 2>&1 || true
    make -C snes  clean >/dev/null 2>&1 || true
    rm -rf "$release_dir"

    log "Building host utilities (utils/)"
    make -C utils

    log "Building SNES menu (menu.bin / m3nu.bin)"
    make -C snes

    log "Generating bsxpage.bin"
    ( cd bin && "${repo_dir}/utils/genbsxpage" )

    mkdir -p "$dist_dir"
    if [ ! -s "${dist_dir}/${zip_name}" ]; then
        log "Downloading official release: ${OFFICIAL_URL}"
        wget -q --show-progress -O "${dist_dir}/${zip_name}" "$OFFICIAL_URL"
    else
        log "Using cached official release at dist/${zip_name}"
    fi

    log "Staging official release into ${staging_dir#${repo_dir}/}"
    mkdir -p "$staging_dir"
    unzip -joq "${dist_dir}/${zip_name}" -d "$staging_dir"

    log "Overlaying freshly built artifacts"
    install -m644 snes/menu.bin                  "${staging_dir}/menu.bin"
    install -m644 snes/m3nu.bin                  "${staging_dir}/m3nu.bin"
    install -m644 bin/bsxpage.bin                "${staging_dir}/bsxpage.bin"
    install -m644 README.md                      "${staging_dir}/README.md"
    install -m644 README.Savestates.FURiOUS.md   "${staging_dir}/README.Savestates.FURiOUS.md"

    log "Repackaging zip"
    rm -f "${release_dir}/${zip_name}"
    ( cd "$release_dir" && zip -rq "$zip_name" sd2snes )

    log "Done."
    printf '\nOutputs:\n'
    printf '  %s\n'  "${release_dir}/${zip_name}"
    printf '  %s/\n' "$staging_dir"
}

if [ "${SD2SNES_IN_CONTAINER:-0}" = "1" ]; then
    run_in_container
else
    run_on_host "$@"
fi
