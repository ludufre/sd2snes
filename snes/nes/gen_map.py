#!/usr/bin/env python3
"""gen_map.py -- mapa de simbolos do renderer NES (nes_snes.bin).

POR QUE ELE EXISTE.  O renderer e' assembly 65816 e o binario final
(misc/nes_snes.bin) e' um dump CRU: nenhum endereco sobrevive ao link.  Um
teste de host que queira EXECUTAR o codigo real (tests/host/run_nes_chr.sh)
precisa saber onde cada rotina/buffer foi parar -- e o unico jeito honesto de
saber isso e' derivar do PROPRIO build, nunca copiar numero para dentro do
teste (numero copiado envelhece em silencio no primeiro relink).

DE ONDE VEM CADA ENDERECO.  snescom -J despeja os simbolos de cada objeto em
<obj>.log ("Labels in the TEXT segment"); sneslink despeja em link.log o
endereco final de cada objeto como 'object_<N>_code', na ORDEM em que os
objetos foram passados na linha de comando.  As duas metades se juntam assim:

  * objeto .o65 (RELOCAVEL): endereco final = base do objeto + label, EXCETO
    label >= $10000, que ja' e' absoluto (o '*= $7Fxxxx' de nes_data.a65 --
    dado em WRAM, que o linker nao reposiciona).  Mesma regra do
    snes/utils/mkmap.sh, que faz o mapa do menu.
  * objeto .ips (NUNCA RELOCADO -- nes_data_zp/nes_header): os labels JA sao
    absolutos e a "base" que o linker reporta e' so' o inicio do bloco.
    Somar aqui e' o erro obvio (nes_parse_ptr sairia em $0020 em vez de
    $0010) -- por isso o tipo do objeto entra na conta.

O cabecalho carimba tamanho + CRC32 do .bin ao lado.  E' o que impede o par
(bin, map) de sair de sincronia: quem consome o mapa confere os dois antes de
executar um unico opcode; mapa velho + binario novo daria lixo silencioso.

Uso: gen_map.py <link.log> <rom.bin> <saida.map> <obj1> <obj2> ...
     (a ordem dos objetos TEM que ser a mesma passada ao sneslink)
"""
import re
import sys
import zlib

OBJ_RE = re.compile(r"^\s*([0-9A-Fa-f]+)\s+object_(\d+)_code\s*$")
LBL_RE = re.compile(r"^\s*([0-9A-Fa-f]+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")


def object_bases(link_log):
    bases = {}
    for line in open(link_log):
        m = OBJ_RE.match(line)
        if m:
            bases[int(m.group(2))] = int(m.group(1), 16)
    return bases


def labels(obj_log):
    """Labels da secao TEXT do log do snescom (para em 'Externs')."""
    out = []
    seen_labels = False
    for line in open(obj_log):
        if line.startswith("Labels"):
            seen_labels = True
            continue
        if line.startswith("Externs"):
            break
        if not seen_labels or line.startswith("Warning"):
            continue
        m = LBL_RE.match(line)
        if m:
            out.append((int(m.group(1), 16), m.group(2)))
    return out


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__.strip().splitlines()[-2])
    link_log, rom_path, out_path = sys.argv[1:4]
    objs = sys.argv[4:]

    bases = object_bases(link_log)
    if not bases:
        raise SystemExit(f"*** {link_log}: nenhum 'object_<N>_code' -- "
                         f"o link nao rodou ou o formato do sneslink mudou")

    syms = {}
    for idx, obj in enumerate(objs, start=1):
        if idx not in bases:
            raise SystemExit(f"*** {obj}: sem object_{idx}_code em {link_log} "
                             f"(a ordem dos objetos tem que casar com o sneslink)")
        base = bases[idx]
        relocatable = not obj.endswith(".ips")
        for addr, name in labels(obj + ".log"):
            if relocatable and addr < 0x10000:
                addr += base
            syms[name] = addr

    rom = open(rom_path, "rb").read()
    with open(out_path, "w") as f:
        f.write("# nes_snes.map -- gerado por snes/nes/gen_map.py; NAO editar\n")
        f.write(f"# rom_bytes {len(rom)}\n")
        f.write(f"# rom_crc32 {zlib.crc32(rom) & 0xFFFFFFFF:08X}\n")
        for name in sorted(syms):
            f.write(f"{syms[name]:06X} {name}\n")
    print(f"{out_path}: {len(syms)} simbolos, rom={len(rom)}B "
          f"crc32={zlib.crc32(rom) & 0xFFFFFFFF:08X}")


if __name__ == "__main__":
    main()
