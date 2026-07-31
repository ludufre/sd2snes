#!/usr/bin/env python3
"""Conversor de referência CHR NES -> formato de tile SNES (contrato da Fase 1 do core NES).

Este arquivo é o CONTRATO canônico da conversão de tiles entre o firmware MCU (src/nes.c,
implementação em C, grava as regiões pré-convertidas na PSRAM no load) e o harness do
renderer 65816 (nes-tests/renderer-harness/, gera fixtures). Qualquer implementação em
outra linguagem TEM que produzir bytes idênticos aos desta.

Formatos (ver NES-BRIDGE-SPEC.md §2.4 e errata §13.2):
- Tile NES 2bpp (16 B): bytes 0..7 = plano 0 (linhas 0..7), bytes 8..15 = plano 1.
- Tile SNES 2bpp (16 B): por linha r, byte[2r] = plano 0, byte[2r+1] = plano 1 (intercalado).
  Usado pelas BGs em Mode 0 (BG 2bpp — 1:1 em tamanho com o NES).
- Tile SNES 4bpp (32 B): bytes 0..15 = pares plano0/plano1 por linha (igual ao 2bpp),
  bytes 16..31 = pares plano2/plano3 por linha, SEMPRE ZERO (o NES só tem 2 planos).
  Usado pelos OBJ (sprites SNES são sempre 4bpp — 2x o tamanho do tile NES).

Regiões PSRAM de destino (gravadas pelo MCU no load; ver NES-CORE-CONTRACT.md §10):
  0x500000+ = CHR inteira convertida p/ SNES 2bpp (BG)   [tamanho == CHR NES]
  0x600000+ = CHR inteira convertida p/ SNES 4bpp (OBJ)  [tamanho == 2x CHR NES]
(0x400000 é o breadcrumb NESL da Fase 0 — NÃO usar; supersede o "ex. 0x400000+" da spec §3.2.)

Uso CLI:
  nes_chr_convert.py entrada.chr saida.bin --bpp 2|4
  nes_chr_convert.py rom.nes saida.bin --bpp 2 --from-ines   (extrai a CHR do .nes antes)
"""
import argparse
import sys


def nes_tile_to_snes2(tile: bytes) -> bytes:
    """Tile NES 16 B -> tile SNES 2bpp 16 B (intercala plano 0/1 por linha)."""
    assert len(tile) == 16
    out = bytearray(16)
    for r in range(8):
        out[2 * r] = tile[r]          # plano 0, linha r
        out[2 * r + 1] = tile[8 + r]  # plano 1, linha r
    return bytes(out)


def nes_tile_to_snes4(tile: bytes) -> bytes:
    """Tile NES 16 B -> tile SNES 4bpp 32 B (planos 2/3 zerados)."""
    out = bytearray(32)               # 16..31 ficam 0x00 (planos 2/3)
    out[0:16] = nes_tile_to_snes2(tile)
    return bytes(out)


def convert_chr(data: bytes, bpp: int) -> bytes:
    """Converte um blob CHR NES inteiro (multiplo de 16 B) pro formato SNES pedido."""
    if len(data) % 16 != 0:
        raise ValueError(f"CHR de {len(data)} bytes nao e multiplo de 16")
    conv = nes_tile_to_snes2 if bpp == 2 else nes_tile_to_snes4
    out = bytearray()
    for off in range(0, len(data), 16):
        out += conv(data[off:off + 16])
    return bytes(out)


def chr_from_ines(rom: bytes) -> bytes:
    """Extrai a CHR-ROM de um .nes (iNES; pula trainer se presente)."""
    if rom[:4] != b"NES\x1a":
        raise ValueError("nao e um arquivo iNES")
    prg = rom[4] * 16384
    chr_size = rom[5] * 8192
    off = 16 + (512 if rom[6] & 0x04 else 0) + prg
    if chr_size == 0:
        raise ValueError("ROM usa CHR-RAM (chr=0), nada a converter no load")
    return rom[off:off + chr_size]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--bpp", type=int, choices=(2, 4), required=True)
    ap.add_argument("--from-ines", action="store_true",
                    help="entrada e um .nes; extrai a CHR-ROM antes de converter")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    if args.from_ines:
        data = chr_from_ines(data)
    out = convert_chr(data, args.bpp)
    open(args.output, "wb").write(out)
    print(f"{len(data)} B CHR NES -> {len(out)} B SNES {args.bpp}bpp")
    return 0


# Autoteste do contrato: tile de exemplo com todos os padroes de linha distintos.
_T = bytes(range(0x00, 0x08)) + bytes(range(0x80, 0x88))
assert nes_tile_to_snes2(_T) == bytes(
    [0x00, 0x80, 0x01, 0x81, 0x02, 0x82, 0x03, 0x83,
     0x04, 0x84, 0x05, 0x85, 0x06, 0x86, 0x07, 0x87])
assert nes_tile_to_snes4(_T)[16:] == bytes(16)

if __name__ == "__main__":
    sys.exit(main())
