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
# block DMAs in the NMI.  That path lives in 65816 (snes/nes/nes_render.a65), and
# is checked here twice:
#
#   * block 2 -- nes_chr_stage_cli.c is a C MODEL of the design.  It proves that
#     shadow -> expand -> swizzle -> DMA meets the contract, and does not see the
#     renderer: it stays green if the assembly changes.  Oracle, not gate.
#   * block 3 -- nes_render_cli.c loads misc/nes_snes.bin into a 65816 interpreter
#     (m65816.c) and EXECUTES the real routines against a VRAM/DMA model.  Any
#     change to the swizzle, the tile range, the descriptor, the clamp or the DMA
#     address/length changes the bytes that come out.
#
# Both dump VRAM in the SAME layout nes_chr_convert.py emits, and PASS is a
# byte-exact cmp against it, over the two things only composition can get wrong:
# runs that are NOT tile-aligned (the shadow is what makes a partial tile work)
# and runs that fall outside or cross the 8KB CHR-RAM ceiling (the clamp).
set -u
cd "$(dirname "$0")"
CC="${CC:-cc}"
. ./sanitizers.sh   # ASAN_OPTIONS/UBSAN_OPTIONS + san_report(); see the file
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
  eval "[ -n \"\$$v\" ]" || { echo "*** layout equate missing ($v) in $EQ"; exit 1; }
done
echo "  layout (de $EQ): BG=$BGW OBJ=$OBW CHR-RAM=$RAMB tiles=$NTIL"
$CC -O1 -g -fsanitize=address,undefined \
    -DVRAM_BG_CHR_WORD=$BGW -DVRAM_OBJ_CHR_WORD=$OBW \
    -DNES_CHR_RAM_BYTES=$RAMB -DNES_CHR_TILES=$NTIL \
    nes_chr_stage_cli.c -o build/nes_chr_stage_cli || exit 1
PDCLI=./build/nes_chr_stage_cli
# The executor of the REAL renderer (65816 interpreter + VRAM/DMA model).  Same
# layout constants, by the same -D, for the same reason.
$CC -O1 -g -fsanitize=address,undefined \
    -DVRAM_BG_CHR_WORD=$BGW -DVRAM_OBJ_CHR_WORD=$OBW \
    -DNES_CHR_RAM_BYTES=$RAMB -DNES_CHR_TILES=$NTIL \
    nes_render_cli.c m65816.c -o build/nes_render_cli || exit 1
RCLI=./build/nes_render_cli

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

pass=0; fail=0; skip=0

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
echo "== block 2: C MODEL of the CHR-RAM path (oracle, never runs the renderer) =="

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

# Corpus truncated at the 8KB CHR-RAM ceiling.  The .8k.chr are DERIVED: erase
# before regenerating, so an item removed from the corpus stops being tested and
# the case count stays the same between a clean run and a re-run.
rm -f corpus_chr/*.8k.chr
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


# ============================================================
# BLOCK 3 -- the REAL renderer, executed
# ============================================================
# nes_render_cli.c loads the bytes of misc/nes_snes.bin into a 65816 interpreter
# (m65816.c, with WRAM $7E/$7F, the LoROM in bank $00, the VRAM registers and
# general purpose DMA) and CALLS the renderer routines -- nes_boot_init,
# nes_chr_handle_run ($41), nes_chrq_publish, nes_chrq_service.  What comes out is
# the VRAM the renderer's own DMAs wrote.
#
# ADDRESSES COME FROM THE BUILD: misc/nes_snes.map is generated by
# snes/nes/gen_map.py from the sneslink link.log plus the snescom symbol logs, and
# stamps the .bin's size and CRC32 beside it, so the CLI refuses a desynchronised
# pair.  No routine or buffer address appears here or in C.
#
# SKIP: misc/nes_snes.bin is not versioned (it comes out of the remote build), so
# a clean clone skips this block and the gate exits 0.  NES_BIN_REQUIRED=1 turns
# every reason for checking nothing into a failure.
echo "== block 3: REAL renderer (nes_snes.bin under a 65816 interpreter) =="

# The model self-tests first: everything below is premised on an interpreter that
# decodes correctly.  It does not need the .bin, so a clean clone still checks it.
if out=$($RCLI --selftest 2>&1); then
  echo "PASS  m65816 selftest (modelo do 65816 + barramento)"
  pass=$((pass+1))
else
  echo "FAIL  m65816 selftest:"; echo "$out" | sed 's/^/      /'
  fail=$((fail+1))
fi

NES_BIN="${NES_SNES_BIN:-$REPO_ROOT/misc/nes_snes.bin}"
NES_MAP="${NES_SNES_MAP:-${NES_BIN%.bin}.map}"
NES_REQUIRED="${NES_BIN_REQUIRED:-0}"

# How many cases this block WOULD check, so a SKIP is never mute: the summary
# line says how many were left unchecked.
render_planned=$(( 5 * 2 * $(ls corpus_chr/*.8k.chr 2>/dev/null | wc -l | tr -d ' ') + 3 * 2 * 2 + 2 * 2 ))

render_skip() { # <motivo>
  if [ "$NES_REQUIRED" != "0" ]; then
    echo "FAIL: $1" >&2
    echo "      NES_BIN_REQUIRED is set, so not executing the renderer is a failure." >&2
    echo "      Rode ./build.sh (que builda snes/nes no servidor e traz misc/nes_snes.{bin,map})," >&2
    echo "      ou aponte \$NES_SNES_BIN/\$NES_SNES_MAP para um par valido." >&2
    fail=$((fail+1))
    return 1
  fi
  echo "SKIP  block 3 ($render_planned cases not checked): $1"
  skip=$render_planned
  return 1
}

run_render_block() {
  [ -f "$NES_BIN" ] || { render_skip "misc/nes_snes.bin missing ($NES_BIN) -- not tracked, comes out of the remote build"; return; }
  [ -f "$NES_MAP" ] || { render_skip "$NES_MAP missing -- produced by 'make -C snes/nes' and fetched with the .bin"; return; }
  echo "  renderer: $NES_BIN ($(wc -c < "$NES_BIN" | tr -d ' ') B) + $(basename "$NES_MAP")"

  check_render() { # <name.chr> <bpp> <desc> <args...>
    local src=$1 bpp=$2 desc=$3; shift 3
    local base pyout rout
    base=$(basename "$src" | sed 's/\.[^.]*$//')
    pyout="corpus_chr/${base}.py.bpp${bpp}.bin"
    rout="corpus_chr/${base}.rr${desc}.bpp${bpp}.bin"

    $PY "$CONVERTER" "$src" "$pyout" --bpp "$bpp" >/tmp/py_$$.log 2>&1
    local pyrc=$?
    $RCLI "$src" "$rout" --rom "$NES_BIN" --map "$NES_MAP" --bpp "$bpp" "$@" >/tmp/rr_$$.log 2>&1
    local rrc=$?

    if [ "$pyrc" -ne 0 ] || [ "$rrc" -ne 0 ]; then
      echo "FAIL  $base bpp=$bpp regime=$desc: python rc=$pyrc renderer rc=$rrc"
      echo "      python:   $(cat /tmp/py_$$.log)"
      echo "      renderer: $(cat /tmp/rr_$$.log)"
      fail=$((fail+1))
    elif cmp -s "$pyout" "$rout"; then
      echo "PASS  $base bpp=$bpp regime=$desc ($(wc -c < "$pyout" | tr -d ' ') B, byte-exact)"
      pass=$((pass+1))
    else
      echo "FAIL  $base bpp=$bpp regime=$desc: outputs differ"
      cmp "$pyout" "$rout" | sed 's/^/      /'
      fail=$((fail+1))
    fi
    rm -f /tmp/py_$$.log /tmp/rr_$$.log
  }

  # SLICING -- depends on the PAYLOAD (an unaligned run only closes because the
  # shadow holds the bytes the run did not bring), so it runs over the whole corpus.
  for chr_file in corpus_chr/*.8k.chr; do
    [ -e "$chr_file" ] || continue
    for bpp in 2 4; do
      check_render "$chr_file" "$bpp" 240   --split 240              # corte natural do RTL
      check_render "$chr_file" "$bpp" 16    --split 16               # 1 tile por run
      check_render "$chr_file" "$bpp" 13    --split 13               # DESALINHADO
      check_render "$chr_file" "$bpp" rnd   --split-rand 0xA53C17    # comprimentos mistos
      check_render "$chr_file" "$bpp" clamp --split 13 --clamp-probe # fora/atravessa o teto
    done
  done

  # REGIME -- exercises the renderer's POLICY (which of the $41 dispatcher's three
  # paths, when the NMI drains, when a full rebuild fires).  Policy looks at size
  # and rhythm, never at the payload, and the CLI ABORTS if the requested path did
  # not actually run, so two representatives are enough.
  # (a) regimes valid at ANY size -- including the smallest possible, 1 tile.
  for chr_file in corpus_chr/autoteste_tile.8k.chr corpus_chr/random_1024tiles.8k.chr; do
    [ -e "$chr_file" ] || continue
    for bpp in 2 4; do
      check_render "$chr_file" "$bpp" fast     --path fast --split 240      # despejo rapido (tela apagada)
      check_render "$chr_file" "$bpp" rebuild  --split 240 --rebuild-at 5   # rebuild total dirigido
      check_render "$chr_file" "$bpp" byte     --split 1 --rpf 32           # byte a byte (pior caso)
    done
  done
  # (b) regimes that only EXIST with volume: a 1-tile payload never overflows the
  #     queue (192 tiles/buffer) nor survives a deferred vblank.  CHR-RAM full only.
  for chr_file in corpus_chr/random_1024tiles.8k.chr; do
    [ -e "$chr_file" ] || continue
    for bpp in 2 4; do
      # Declaring the overflow is mandatory: without --expect-drop the CLI refuses
      # a drop, so no other regime can pass on a full rebuild hiding a broken drain.
      check_render "$chr_file" "$bpp" overflow --split 240 --rpf 0 --expect-drop
      # V that WALKS between vblanks: half the values make the NMI defer, and the
      # CLI requires having seen one.  With V fixed this regime never defers.
      check_render "$chr_file" "$bpp" vdefer   --split 240 --rpf 1 --v-walk
    done
  done
}
run_render_block

echo "== summary: $pass pass, $fail fail, $skip skipped =="
[ "$fail" -eq 0 ] || exit 1
