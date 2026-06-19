#!/usr/bin/env bash
# Build (against the REAL src/patch.c) and run the IPS/BPS patch CLI.
#
#   tests/host/patch-cli.sh [--header N|--no-header] [--probe] [-q] <rom> <patch> <out>
#
# Runs the exact firmware patch engine over a 16 MB fake SDRAM and writes the
# patched ROM, so you can boot the result in an emulator AND debug divergences
# from the device with the same source. Built with ASan/UBSan by default
# (SAN= to disable, e.g. `SAN= tests/host/patch-cli.sh ...`). Rebuilds only when
# patch.c / the shim / the CLI changed; runs from your current directory so
# relative ROM/patch/out paths work.
#
# Examples:
#   tests/host/patch-cli.sh game.sfc hack.ips game-hack.sfc
#   tests/host/patch-cli.sh game.sfc translation.bps game-en.sfc
#   tests/host/patch-cli.sh --probe game.sfc translation.bps -   # inspect a BPS
set -euo pipefail
SD="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # tests/host
SRC="$SD/../../src"
CC="${CC:-cc}"
SAN="${SAN--fsanitize=address,undefined}"
BIN="$SD/build/patch-cli"

need=0
[ -x "$BIN" ] || need=1
for f in "$SRC/patch.c" "$SD/patch_cli.c" "$SD/shim.c"; do
  [ "$need" = 1 ] || [ "$BIN" -nt "$f" ] || need=1
done

if [ "$need" = 1 ]; then
  mkdir -p "$SD/build"
  # Copy patch.c so the shim headers win over the real firmware headers that sit
  # next to it (quoted #includes resolve in the includer's own dir first).
  cp "$SRC/patch.c" "$SD/build/patch_under_test.c"
  # shellcheck disable=SC2086
  $CC -O1 -g $SAN -I "$SD/shim" -I "$SRC" -I "$SD" \
      "$SD/shim.c" "$SD/patch_cli.c" "$SD/build/patch_under_test.c" -o "$BIN"
fi

exec "$BIN" "$@"
