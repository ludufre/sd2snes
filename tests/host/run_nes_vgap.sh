#!/usr/bin/env bash
# run_nes_vgap.sh -- the per-strip BG1VOFS of the NES renderer, executed for real.
#
# nes_vgap_cli.c runs nhs_vgap_build (the routine that turns the $11 CMD_SPLITS
# entries into the channel-4 HDMA table) from misc/nes_snes.bin inside the 65816
# interpreter (m65816.c) and prints the table.  This script pins the CONTRACT of
# the `sy` field: it is the nametable line the PPU is drawing on the entry's
# scanline (the raster line), so a status-bar split whose strip starts at
# scanline S with sy == S must be CONTINUOUS with the strip above it -- same
# BG1VOFS.  That is the Super Mario Bros. 1 case (sprite-0 split at 31, $2005
# only).  The control is the value the pre-v3.9 core published for that split,
# sy = 0: the playfield strip then gets a different VOFS (it is re-anchored at
# logical line 0 = the status-bar rows drawn inside the playfield, the 2.16 bug).
#
# Needs misc/nes_snes.bin + misc/nes_snes.map (the build.sh fetch); without them
# the gate SKIPs (exit 0) unless NES_BIN_REQUIRED=1, like run_nes_chr.sh.
set -u
cd "$(dirname "$0")" || exit 1
. ./sanitizers.sh
CC=${CC:-cc}
BIN=../../misc/nes_snes.bin; MAP=../../misc/nes_snes.map
if [ ! -f "$BIN" ] || [ ! -f "$MAP" ]; then
  if [ "${NES_BIN_REQUIRED:-0}" = "1" ]; then echo "*** misc/nes_snes.bin/.map ausentes"; exit 1; fi
  echo "SKIP  run_nes_vgap: misc/nes_snes.bin/.map ausentes"; exit 0
fi
mkdir -p build
$CC -O1 -g -fsanitize=address,undefined nes_vgap_cli.c m65816.c -o build/nes_vgap_cli || exit 1
CLI=./build/nes_vgap_cli
fail=0
# VOFS of block N of the table for a given sy1 (split fixed at 31)
vofs() { $CLI --bin "$BIN" --map "$MAP" --split 31 --sy1 "$1" | sed -n "s/^  \[$2\] .*BG1VOFS= *\([0-9]*\).*/\1/p"; }
v0=$(vofs 31 0); v1=$(vofs 31 1); c1=$(vofs 0 1)
[ -n "$v0" ] && [ -n "$v1" ] && [ -n "$c1" ] || { echo "*** tabela do canal 4 nao lida (CLI mudou de formato?)"; exit 1; }
if [ "$v0" = "$v1" ]; then
  echo "PASS  SMB1: split @31 com sy=31 (linha do raster) -> playfield CONTINUO com a HUD (VOFS $v0 == $v1)"
else
  echo "FAIL  SMB1: split @31 com sy=31 -> VOFS da HUD=$v0, do playfield=$v1 (tinha de ser igual)"; fail=1
fi
if [ "$c1" != "$v0" ]; then
  echo "PASS  controle: sy=0 na mesma faixa -> VOFS $c1 (re-ancora na linha 0: a forma do bug da 2.16)"
else
  echo "FAIL  controle: sy=0 deu o MESMO VOFS ($c1) -- o gate nao discrimina"; fail=1
fi
exit $fail
