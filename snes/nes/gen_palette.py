#!/usr/bin/env python3
"""gen_palette.py -- gera a LUT NES->BGR555 canonica, fonte UNICA para o renderer
65816 (nes_palette.i65) e o verificador Python do harness (nes_palette.py).

Paleta base: a tabela "2C02" classica (64 cores RGB24), amplamente publicada e
usada como default por varios emuladores (FCEUX, Nestopia, BizHawk) -- NAO e'
uma medicao de hardware, e' a referencia documentada mais comum (ver
NES-BRIDGE-SPEC.md / a doc pede "ex. a do Mesen/2C02 padrao"). O que importa
para o harness e' CONSISTENCIA interna (mesma tabela nos dois lados), nao
fidelidade de cor absoluta -- documentado em nes_render.a65.

Conversao RGB24 -> BGR555 (formato CGRAM do SNES, book1 p.6918: bit14-10=Blue,
bit9-5=Green, bit4-0=Red): trunca cada canal de 8 para 5 bits (>>3).

Uso: python3 gen_palette.py --i65 nes_palette.i65 --py nes_palette_data.py
"""
import argparse

# Tabela 2C02 classica (R,G,B), indices 0x00-0x3F.
NES_RGB24 = [
    (84, 84, 84), (0, 30, 116), (8, 16, 144), (48, 0, 136),
    (68, 0, 100), (92, 0, 48), (84, 4, 0), (60, 24, 0),
    (32, 42, 0), (8, 58, 0), (0, 64, 0), (0, 60, 0),
    (0, 50, 60), (0, 0, 0), (0, 0, 0), (0, 0, 0),
    (152, 150, 152), (8, 76, 196), (48, 50, 236), (92, 30, 228),
    (136, 20, 176), (160, 20, 100), (152, 34, 32), (120, 60, 0),
    (84, 90, 0), (40, 114, 0), (8, 124, 0), (0, 118, 40),
    (0, 102, 120), (0, 0, 0), (0, 0, 0), (0, 0, 0),
    (236, 238, 236), (76, 154, 236), (120, 124, 236), (176, 98, 236),
    (228, 84, 236), (236, 88, 180), (236, 106, 100), (212, 136, 32),
    (160, 170, 0), (116, 196, 0), (76, 208, 32), (56, 204, 108),
    (56, 180, 204), (60, 60, 60), (0, 0, 0), (0, 0, 0),
    (236, 238, 236), (168, 204, 236), (188, 188, 236), (212, 178, 236),
    (236, 174, 236), (236, 174, 212), (236, 180, 176), (228, 196, 144),
    (204, 210, 120), (180, 222, 120), (168, 226, 144), (152, 226, 180),
    (160, 214, 228), (160, 162, 160), (0, 0, 0), (0, 0, 0),
]

assert len(NES_RGB24) == 64


def rgb24_to_bgr555(r, g, b):
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    return (b5 << 10) | (g5 << 5) | r5


BGR555 = [rgb24_to_bgr555(*rgb) for rgb in NES_RGB24]


def write_i65(path):
    # Comentario em bloco C (nao ';') -- arquivos .i65 #included tropecam em
    # comentario ';' logo na 1a linha (gotcha ja documentado em
    # snes/boot/dma.i65: "#if0'd because it's #included, and those don't
    # get parsed properly by snescom"). Confirmado empiricamente aqui: um
    # nes_palette.i65 com ';' na 1a linha dava "Error: What is '; ...'" no
    # snescom real.
    lines = [
        "/* nes_palette.i65 -- LUT NES(0-63) -> BGR555, GERADO por gen_palette.py.",
        " * NAO EDITAR A MAO -- regenerar com: python3 gen_palette.py",
        " * Fonte: paleta 2C02 classica (ver gen_palette.py); mesma tabela em",
        " * nes-tests/renderer-harness/nes_palette_data.py (consistencia harness).",
        " * Fragmento puro (sem .link page) -- inclua de um arquivo que ja",
        " * declarou '.link page $00' (mesma convencao de dma.i65/memmap.i65). */",
        "nes_palette_lut:",
    ]
    for i in range(0, 64, 4):
        vals = ", ".join(f"${BGR555[i + j]:04x}" for j in range(4))
        lines.append(f"  .word {vals}")
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {path}")


def write_py(path):
    lines = [
        '"""nes_palette_data.py -- GERADO por _repo/snes/nes/gen_palette.py.',
        "NAO EDITAR A MAO. Mesma LUT NES->BGR555 usada pelo renderer 65816",
        '(nes_palette.i65) -- consistencia byte-exata entre os dois lados."""',
        "",
        "NES_TO_BGR555 = [",
    ]
    for i in range(0, 64, 8):
        vals = ", ".join(f"0x{BGR555[i + j]:04x}" for j in range(8))
        lines.append(f"    {vals},")
    lines.append("]")
    lines.append("")
    lines.append("assert len(NES_TO_BGR555) == 64")
    lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--i65", default="nes_palette.i65")
    ap.add_argument("--py", default=None)
    args = ap.parse_args()
    write_i65(args.i65)
    if args.py:
        write_py(args.py)


if __name__ == "__main__":
    main()
