#!/usr/bin/env bash
# Golden test for the Fase 1c CHR NES -> CHR SNES tile conversion contract.
#
# Compiles the REAL src/nes_chr.c (the firmware's conversion functions,
# zero hardware deps) into a small CLI, mirrors it against the canonical
# reference utils/nes_chr_convert.py over a corpus of tiles/CHR blobs, and
# requires byte-identical output (cmp) for both --bpp 2 and --bpp 4 on
# every corpus item. See NES-CORE-CONTRACT.md Sec. 10.5 and src/nes_chr.h.
#
# Fase 2.2-lite (CHR-RAM) adds a SECOND path to the same contract: the
# renderer no longer receives pre-converted CHR for mapper 2/7 -- it takes raw
# NES bytes in CMD_CHR_RUN ($41), keeps a shadow of the CHR-RAM, swizzles the
# dirty tiles into packed BG/OBJ arrays during decode, and blits them with two
# block DMAs in the NMI.  nes_chr_stage_cli.c is a transcription of that whole
# path (shadow -> tile expansion -> swizzle -> $2118/$2119 DMA into a VRAM
# model) and dumps the resulting VRAM in the SAME layout nes_chr_convert.py
# emits; PASS is again a byte-exact `cmp`.  The second block below hammers the
# parts that only the composition can get wrong: runs that are NOT tile
# aligned (the shadow is what makes partial tiles work) and runs that fall
# outside / straddle the 8KB CHR-RAM ceiling (the clamp).
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
PY="${PYTHON:-python3}"
REPO_ROOT="../.."
CONVERTER="$REPO_ROOT/utils/nes_chr_convert.py"

echo "== build =="
mkdir -p build corpus_chr
$CC -O1 -g -fsanitize=address,undefined -I ../../src \
    nes_chr_convert_cli.c ../../src/nes_chr.c -o build/nes_chr_convert_cli || exit 1
CLI=./build/nes_chr_convert_cli
# As constantes de layout sao EXTRAIDAS do nes_equates.i65 e passadas por -D:
# nenhum numero de layout fica duplicado no host (mata o drift de lockstep --
# se o renderer mover a base de CHR na VRAM, o gate move junto ou nao compila).
EQ=../../snes/nes/nes_equates.i65
eq_def() { # <NOME> -> valor em C (converte $HEX -> 0xHEX)
  awk -v n="$1" '$1=="#define" && $2==n { v=$3; sub(/^\$/, "0x", v); print v; exit }' "$EQ"
}
BGW=$(eq_def VRAM_BG_CHR_WORD)
OBW=$(eq_def VRAM_OBJ_CHR_WORD)
RAMB=$(eq_def NES_CHR_RAM_BYTES)
NTIL=$(eq_def NES_CHR_TILES)
for v in BGW OBW RAMB NTIL; do
  eval "[ -n \"\$$v\" ]" || { echo "*** equate de layout ausente ($v) em $EQ"; exit 1; }
done
echo "  layout (de $EQ): BG=$BGW OBJ=$OBW CHR-RAM=$RAMB tiles=$NTIL"
$CC -O1 -g -fsanitize=address,undefined \
    -DVRAM_BG_CHR_WORD=$BGW -DVRAM_OBJ_CHR_WORD=$OBW \
    -DNES_CHR_RAM_BYTES=$RAMB -DNES_CHR_TILES=$NTIL \
    nes_chr_stage_cli.c -o build/nes_chr_stage_cli || exit 1
PDCLI=./build/nes_chr_stage_cli

echo "== corpus =="
$PY - "$REPO_ROOT" <<'EOF' || exit 1
import os
import random
import sys

repo_root = sys.argv[1]
out = "corpus_chr"
os.makedirs(out, exist_ok=True)

# 1) The exact autoteste tile from nes_chr_convert.py (all 16 line patterns
#    distinct: 0x00..0x07 plane 0, 0x80..0x87 plane 1).
autoteste = bytes(range(0x00, 0x08)) + bytes(range(0x80, 0x88))
with open(f"{out}/autoteste_tile.chr", "wb") as f:
    f.write(autoteste)

# 2) Edge tiles: all-zero and all-0xFF (degenerate bit patterns).
with open(f"{out}/zero_tile.chr", "wb") as f:
    f.write(bytes(16))
with open(f"{out}/ff_tile.chr", "wb") as f:
    f.write(bytes([0xFF] * 16))

# 3) All 256 possible byte values as plane 0 of tile 0, replicated pattern
#    across many tiles (256 tiles = 4096 B) -- exercises every byte value
#    in every bit position of the interleave, deterministic (no RNG).
buf = bytearray()
for v in range(256):
    buf += bytes([v]) * 8 + bytes([(v ^ 0xFF)]) * 8
with open(f"{out}/allbytes_256tiles.chr", "wb") as f:
    f.write(bytes(buf))

# 4) Pseudo-random multi-tile blob, fixed seed for determinism (1024 tiles
#    = 16 KiB -- big enough to catch an off-by-one in a tile-loop).
rnd = random.Random(0xC0FFEE)
with open(f"{out}/random_1024tiles.chr", "wb") as f:
    f.write(bytes(rnd.randrange(256) for _ in range(1024 * 16)))

# 4b) Synthetic 32 KiB CHR (2048 tiles = 4 * 8KB CNROM/MMC1-sized banks) --
#     the v2.0b target size (Tetris USA CHR-ROM is exactly 32 KiB, 4 banks
#     of CNROM-style 8KB CHR). Distinct fixed seed from item 4 so it isn't
#     just a truncation of that blob; big enough to exercise multiple
#     8KB-bank-sized chunks through the SAME single tile-loop (nes_convert_chr
#     has no per-bank special-casing -- this corpus item is what proves that
#     a multi-bank CHR converts byte-identically to N independent 8KB blobs).
rnd2 = random.Random(0x7E7E5713)
with open(f"{out}/synthetic_32k_4bank.chr", "wb") as f:
    f.write(bytes(rnd2.randrange(256) for _ in range(2048 * 16)))

# 5) Real CHR-ROM from a small .nes: reuse nes-tests/nestest.nes (read-only;
#    owned by another agent's fixtures, never modified here). NROM, 1x8KB
#    CHR bank -- exercised via --from-ines on both sides so the iNES
#    extraction logic itself is cross-checked too, not just the tile math.
nestest = os.path.join(repo_root, "..", "nes-tests", "nestest.nes")
if os.path.exists(nestest):
    with open(f"{out}/nestest_source.txt", "w") as f:
        f.write(os.path.abspath(nestest) + "\n")
else:
    print(f"WARNING: {nestest} not found, skipping real-ROM corpus item")

print("corpus generated in corpus_chr/")
EOF

pass=0; fail=0

check_bpp() { # <name.chr> <bpp> [--from-ines]
  local src=$1 bpp=$2 extra=${3:-}
  local base pyout ciout
  base=$(basename "$src" | sed 's/\.[^.]*$//')
  pyout="corpus_chr/${base}.py.bpp${bpp}.bin"
  ciout="corpus_chr/${base}.c.bpp${bpp}.bin"

  $PY "$CONVERTER" "$src" "$pyout" --bpp "$bpp" $extra >/tmp/py_$$.log 2>&1
  local pyrc=$?
  $CLI "$src" "$ciout" --bpp "$bpp" $extra >/tmp/c_$$.log 2>&1
  local circ=$?

  if [ "$pyrc" -ne 0 ] || [ "$circ" -ne 0 ]; then
    echo "FAIL  $base bpp=$bpp: python rc=$pyrc c rc=$circ"
    echo "      python: $(cat /tmp/py_$$.log)"
    echo "      c:      $(cat /tmp/c_$$.log)"
    fail=$((fail+1))
  elif cmp -s "$pyout" "$ciout"; then
    echo "PASS  $base bpp=$bpp ($(wc -c < "$pyout" | tr -d ' ') B, byte-exact)"
    pass=$((pass+1))
  else
    echo "FAIL  $base bpp=$bpp: outputs differ"
    cmp "$pyout" "$ciout" | sed 's/^/      /'
    fail=$((fail+1))
  fi
  rm -f /tmp/py_$$.log /tmp/c_$$.log
}

echo "== run =="
for chr_file in corpus_chr/*.chr; do
  [ -e "$chr_file" ] || continue
  # os *.8k.chr sao truncagens geradas pelo bloco plane-DMA (abaixo) a partir
  # destes mesmos itens -- pular mantem a contagem do gate DETERMINISTICA
  # entre uma rodada limpa e uma re-rodada com corpus_chr/ ja povoado.
  case "$chr_file" in *.8k.chr) continue;; esac
  check_bpp "$chr_file" 2
  check_bpp "$chr_file" 4
done

if [ -f corpus_chr/nestest_source.txt ]; then
  nestest_path=$(cat corpus_chr/nestest_source.txt)
  check_bpp "$nestest_path" 2 --from-ines
  check_bpp "$nestest_path" 4 --from-ines
fi

# ============================================================
# Fase 2.2-lite -- caminho SHADOW+SWIZZLE+BLOCO (renderer, CHR-RAM)
# ============================================================
# A CHR-RAM de mapper 2/7 tem 8KB fixos, entao o corpus e' truncado nesse teto
# (o renderer clampa igual: nes_chr_handle_run recusa off >= $2000 e trunca o
# len).  Cada item vira um blob <= 8192 B, convertido pelos DOIS lados:
#   python nes_chr_convert.py  (contrato canonico)
#   nes_chr_planedma_cli       (simulacao de $2115/$2116/$2118/$2119 + GP-DMA)
# PASS = cmp byte-exato, pra bpp 2 (BG) e bpp 4 (OBJ -- prova de quebra que os
# planos 2/3 continuam ZERO, porque o dump le a VRAM em vez de assumir).
#
# MATRIZ DE FATIAMENTO (o ponto do teste): o mesmo blob e' aplicado como runs
# de tamanhos diferentes.  240 = 15 tiles = o corte natural do RTL (<=255 B);
# 16 = 1 tile; 13/7/1 e o modo aleatorio produzem runs DESALINHADOS, que
# comecam e terminam no MEIO de um tile -- so' passam porque o shadow tem os
# bytes que o run nao trouxe.  O `clamp` injeta runs FORA do teto de 8KB e um
# que ATRAVESSA o teto.  O resultado tem que ser IDENTICO em todos: a VRAM
# final nao pode depender de como o RTL picou o fluxo.
echo "== shadow+swizzle+bloco (CHR-RAM, Fase 2.2-lite) =="

check_planedma() { # <name.chr> <bpp> <split-desc> <split-args...>
  local src=$1 bpp=$2 desc=$3; shift 3
  local base pyout pdout
  base=$(basename "$src" | sed 's/\.[^.]*$//')
  pyout="corpus_chr/${base}.py.bpp${bpp}.bin"
  pdout="corpus_chr/${base}.pd${desc}.bpp${bpp}.bin"

  $PY "$CONVERTER" "$src" "$pyout" --bpp "$bpp" >/tmp/py_$$.log 2>&1
  local pyrc=$?
  $PDCLI "$src" "$pdout" --bpp "$bpp" "$@" >/tmp/pd_$$.log 2>&1
  local pdrc=$?

  if [ "$pyrc" -ne 0 ] || [ "$pdrc" -ne 0 ]; then
    echo "FAIL  $base bpp=$bpp split=$desc: python rc=$pyrc planedma rc=$pdrc"
    echo "      python:   $(cat /tmp/py_$$.log)"
    echo "      planedma: $(cat /tmp/pd_$$.log)"
    fail=$((fail+1))
  elif cmp -s "$pyout" "$pdout"; then
    echo "PASS  $base bpp=$bpp split=$desc ($(wc -c < "$pyout" | tr -d ' ') B, byte-exact)"
    pass=$((pass+1))
  else
    echo "FAIL  $base bpp=$bpp split=$desc: outputs differ"
    cmp "$pyout" "$pdout" | sed 's/^/      /'
    fail=$((fail+1))
  fi
  rm -f /tmp/py_$$.log /tmp/pd_$$.log
}

# corpus truncado no teto da CHR-RAM (8KB)
for chr_file in corpus_chr/*.chr; do
  [ -e "$chr_file" ] || continue
  case "$chr_file" in *.8k.chr) continue;; esac
  b=$(basename "$chr_file" .chr)
  head -c 8192 "$chr_file" > "corpus_chr/${b}.8k.chr"
done

for chr_file in corpus_chr/*.8k.chr; do
  [ -e "$chr_file" ] || continue
  for bpp in 2 4; do
    check_planedma "$chr_file" "$bpp" 240 --split 240   # corte natural do RTL
    check_planedma "$chr_file" "$bpp" 16  --split 16    # 1 tile por run
    check_planedma "$chr_file" "$bpp" 13  --split 13    # DESALINHADO
    check_planedma "$chr_file" "$bpp" 7   --split 7     # nunca cruza plano
    check_planedma "$chr_file" "$bpp" 1   --split 1     # byte a byte (pior caso)
    check_planedma "$chr_file" "$bpp" rnd --split-rand 0xA53C17   # comprimentos mistos
    check_planedma "$chr_file" "$bpp" clamp --split 13 --clamp-probe  # fora/atravessa o teto
  done
done

echo "== summary: $pass pass, $fail fail =="
[ "$fail" -eq 0 ] || exit 1
