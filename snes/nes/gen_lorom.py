#!/usr/bin/env python3
"""gen_lorom.py -- fatia o dump linear que sneslink produz (snescom/sneslink
so' suportam bancos lineares de 64KB por 'page' -- nao tem o janelamento
LoROM de 32KB nativamente; comprovado empiricamente: uma pagina '$00' com
conteudo so em $8000-$FFFF ainda sai como um .sfc de 65536 bytes, endereco
== offset de arquivo direto, i.e. HiROM-style) e produz um LoROM classico
de verdade: cada 'pagina' de 64KB do dump vira 32KB (a metade $8000-$FFFF,
que e' onde TODO o codigo/dados do renderer mora -- as fontes nunca usam
enderecos <$8000 dentro de uma pagina, ver nes_snes.a65/nes_render.a65).
Isso e' exatamente o mesmo truque que utils/gen_nes_snes_stub.py faz na
mao (monta o LoROM por offset de arquivo direto) -- aqui e' so' automatizado
em cima da saida real do snescom/sneslink.

sneslink tambem escreve romsize/checksum ERRADOS (calculados sobre os
64KB inteiros do dump, nao sobre os 32KB finais que sobram por pagina) --
corrigidos aqui, mesma formula do stub (cchk^chk = 0xFFFF fixo).

Uso: gen_lorom.py entrada.sfc saida.bin [--pages N] [--romsize BYTE]
"""
import argparse

HDR = 0x7FB0  # offset relativo ao INICIO do banco 0 fatiado (32KB)


def slice_lorom(data: bytes, pages: int) -> bytes:
    out = bytearray()
    for p in range(pages):
        page_start = p * 0x10000
        half = data[page_start + 0x8000: page_start + 0x10000]
        if len(half) != 0x8000:
            raise ValueError(f"pagina {p}: esperava 32768B, achei {len(half)}")
        out += half
    return bytes(out)


def fix_header(rom: bytearray, romsize_byte: int) -> None:
    rom[HDR + 0x27] = romsize_byte
    rom[HDR + 0x2C] = 0xFF
    rom[HDR + 0x2D] = 0xFF
    rom[HDR + 0x2E] = 0x00
    rom[HDR + 0x2F] = 0x00
    chk = sum(rom) & 0xFFFF
    cchk = chk ^ 0xFFFF
    rom[HDR + 0x2C] = cchk & 0xFF
    rom[HDR + 0x2D] = (cchk >> 8) & 0xFF
    rom[HDR + 0x2E] = chk & 0xFF
    rom[HDR + 0x2F] = (chk >> 8) & 0xFF


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--pages", type=int, default=1,
                     help="numero de paginas de 64KB no dump de entrada "
                          "(1 banco SNES 'page $NN' cada, ver .link page nos .a65)")
    ap.add_argument("--romsize", type=lambda s: int(s, 0), default=0x05,
                     help="byte de romsize LoROM p/ o tamanho final (default 0x05 = 32KB)")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    expected = args.pages * 0x10000
    if len(data) != expected:
        raise SystemExit(f"entrada tem {len(data)}B, esperava {expected}B "
                          f"({args.pages} pagina(s) de 64KB) -- confira --pages")

    rom = bytearray(slice_lorom(data, args.pages))
    fix_header(rom, args.romsize)

    open(args.output, "wb").write(rom)
    chk = rom[HDR + 0x2E] | (rom[HDR + 0x2F] << 8)
    reset = rom[0x7FFC] | (rom[0x7FFD] << 8)
    print(f"{args.output}: {len(rom)} bytes, chk=0x{chk:04x}, reset=${reset:04x}")


if __name__ == "__main__":
    main()
