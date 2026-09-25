#!/usr/bin/env python3
"""gen_map_asar.py -- mapa de simbolos do player GBC (gbc_snes.bin).

POR QUE ELE EXISTE.  O player e' assembly 65816 e o binario final e' um dump
CRU: nenhum endereco sobrevive ao assembly.  Um teste de host que queira
EXECUTAR o codigo real (tests/host/run_gbc_player.sh) precisa saber onde cada
rotina foi parar -- e o unico jeito honesto de saber isso e' derivar do
PROPRIO build, nunca copiar numero para dentro do teste (numero copiado
envelhece em silencio no primeiro reassembly).

DE ONDE VEM CADA ENDERECO.  Aqui o assembler e' o asar (nao o par
snescom/sneslink do snes/nes/), entao nao ha link.log nem .o65.log para
juntar: o asar despeja tudo de uma vez com

    asar --symbols=wla --symbols-path=<f.sym> gbc_snes.asm <f.bin>

O formato WLA e' uma sequencia de secoes '[nome]'; a que interessa e'
'[labels]', uma linha por simbolo:

    00:8154 GbcInit          ->  008154 GbcInit

O arquivo tambem traz '[addr-to-line mapping]', cujas linhas tem a MESMA cara
de um label ('00:fff6 0000:000009e8') -- por isso o parser para no proximo
'[' em vez de varrer o arquivo inteiro.  Varrer tudo encheria o mapa de
simbolos falsos com nome '0000:000009e8', e o consumidor so' descobriria isso
quando pedisse um simbolo que existe de verdade e recebesse o endereco errado.

O cabecalho carimba tamanho + CRC32 do .bin ao lado.  E' o que impede o par
(bin, map) de sair de sincronia: quem consome o mapa confere os dois antes de
executar um unico opcode; mapa velho + binario novo daria lixo silencioso.
Mesma disciplina do snes/nes/gen_map.py.

Uso: gen_map_asar.py <entrada.sym> <rom.bin> <saida.map>
"""
import re
import sys
import zlib

# '00:8154 GbcInit' -- banco:offset e um nome de simbolo de verdade.  O
# ':' no segundo campo e' o que separa um label do addr-to-line mapping,
# mas nao dependemos disso: a secao ja' delimita.
LBL_RE = re.compile(r"^\s*([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})\s+"
                    r"([A-Za-z_.][A-Za-z0-9_.]*)\s*$")


def labels(sym_path):
    """Os simbolos da secao [labels] do .sym do asar."""
    out = {}
    inside = False
    saw_section = False
    for line in open(sym_path):
        s = line.strip()
        if s.startswith("["):
            inside = s.lower().startswith("[labels]")
            saw_section = saw_section or inside
            continue
        if not inside or not s or s.startswith(";"):
            continue
        m = LBL_RE.match(line)
        if m:
            out[m.group(3)] = (int(m.group(1), 16) << 16) | int(m.group(2), 16)
    if not saw_section:
        raise SystemExit(f"*** {sym_path}: sem secao [labels] -- o asar rodou "
                         f"com --symbols=wla?")
    return out


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    sym_path, rom_path, out_path = sys.argv[1:4]

    syms = labels(sym_path)
    if not syms:
        raise SystemExit(f"*** {sym_path}: [labels] vazia")

    rom = open(rom_path, "rb").read()
    crc = zlib.crc32(rom) & 0xFFFFFFFF
    with open(out_path, "w") as f:
        f.write("# gbc_snes.map -- gerado por snes/gbc/gen_map_asar.py; NAO editar\n")
        f.write(f"# rom_bytes {len(rom)}\n")
        f.write(f"# rom_crc32 {crc:08X}\n")
        for name in sorted(syms):
            f.write(f"{syms[name]:06X} {name}\n")
    print(f"{out_path}: {len(syms)} simbolos, rom={len(rom)}B crc32={crc:08X}")


if __name__ == "__main__":
    main()
