# sd2snes bootleg core (fpga_bootleg)

Dedicated core for copy-protected unlicensed LoROM bootlegs: the games in fullsnes
"SNES Cart Unlicensed Variants" (https://problemkaputt.de/fullsnes.htm) plus the ones nocash,
Revenant and others documented on nesdev (forum t=15510, 2017).  It is `sd2snes_base`
plus `bootleg.v` and a small address remap in `main.v`; everything else (MSU-1, DMA,
cheats, in-game hooks, SFX fetcher) is unchanged.

## Protection variants

The firmware picks the variant from the ROM's CRC32 and sends it as chipfeat[2:0].

| variant | games | hardware |
|---|---|---|
| 1 BITSWAP   | Aladdin 2000, Digimon Adventure, KOF2000, Pocket Monster (Picachu), Pokemon Gold Silver, Pokemon Stadium, Soul Edge Vs Samurai, X-Men vs SF, Squirrel | write latch at 88:xxxx, read back at 80:xxxx with bits reordered 0,6,7,1,2,3,4,5; decoded in banks A23=1 / A18-16=000 (80, 88, 90 ...) |
| 2 CONSTANT  | Soul Blade, Hercules, Dragon Ball Z - Final Bout | 80-BF:8000-FFFF read as the repeating pattern 55,0F,AA,F0; C0-FF open bus |
| 3 ALU       | Tekken 2, Street Fighter EX Plus Alpha | 80-BF:8000-87FF: set/clear/count/shift unit, result read at 81xx |
| 4 PORT6     | A Bug's Life, Bananas de Pijamas | 00-3F/80-BF:6000-6FFF; real function undocumented, the core returns one answer set that passes both games' checks (61=2 while 60xx armed, 63=4, 65=F, 67=0, 6F=3) |
| 5 BITSWAP40 | Marvel Super Heroes vs Street Fighter | BITSWAP latch and bit order, decoded at 40-4F:8000-FFFF (game writes 4x:xxx2, reads 4x:xxx0) |
| 6 KOF98     | King of Fighters '98 | BITSWAP, plus a bank register at C0-CF:8000-FFFF (game: C0:8788 = 82/00); while bit 7 is set, ROM accesses see A19..A16 replaced by its low nibble (remap in main.v, before address.v) |

## Supported files

Headerless images; a 512-byte copier header is skipped automatically.

| game | size | CRC32 | variant | checked |
|---|---|---|---|---|
| Aladdin 2000 | 2 MB | 752A25D3 | 1 | boot check |
| Digimon Adventure | 2 MB | 4F660972 | 1 | boot check |
| King of Fighters 2000 | 3 MB | A7813943 | 1 | boot check |
| Pocket Monster (Picachu) | 2 MB | 892C6765 | 1 | boot check |
| Pokemon Gold Silver | 2 MB | 7C0B798D | 1 | boot check |
| Pokemon Stadium | 2 MB | F863C642 | 1 | boot check |
| Soul Edge Vs Samurai | 2 MB | 5E4ADA04 | 1 | boot check |
| X-Men vs. Street Fighter | 2 MB | 40242231 | 1 | boot check |
| Squirrel | 2 MB | BAD1D9B8 | 1 | boot check |
| Soul Blade | 3 MB | C97D1D7B | 2 | boot check |
| Hercules | 2 MB | 45874D3D | 2 | boot check |
| Dragon Ball Z - Final Bout (dump, banks 07-0A blank) | 2 MB | 5BBA4EB3 | 2 | boot check, identical to its crack; sound partly missing |
| Dragon Ball Z - Final Bout (sound restored, see below) | 2 MB | DD7AFCB9 | 2 | boot check |
| Tekken 2 | 2 MB | 066687CA | 3 | boot check  |
| Street Fighter EX Plus Alpha | 2 MB | DAD59B9F | 3 | boot check |
| A Bug's Life | 2 MB | 014F0FCF | 4 | boot check |
| Bananas de Pijamas | 1 MB | 52B0D84B | 4 | boot check |
| Marvel Super Heroes vs Street Fighter | 2 MB | CDB590E4 | 5 | boot check |
| King of Fighters '98 | 2 MB | 6C303FC9 | 6 | boot check |

Cracked versions of these games carry no protection and are left alone: they load with
the normal base core.

## Layout
- `verilog/sd2snes_bootleg/` – the core.  Put it next to `sd2snes_base`; `CORE = bootleg`
  builds `fpga_bootleg.bit` (mk2) / `fpga_bootleg.bi3` (mk3); copy those to `/sd2snes/`.
- `src/` – changed firmware files (full copies). 
- `src/utils/bootleg_fp.py` – prints table rows (CRC + 64 KB fingerprint) for ROM files;
  `--fix OUTDIR` converts doubled-bank overdumps to the clean image.
- `src/utils/repair_dbz_sound.py` – restores Dragon Ball Z - Final Bout's missing sound banks.

## How a game gets here
`load_identify()` enables `bootleg_scan` for normal SNES game loads; `smc_id()` then
checks the headerless image only if its size is 1, 2 or 3 MB.  The CRC32 of the first
64 KB is compared first; only on a fingerprint hit is the full image CRC'd, so other games
of those sizes cost a single 64 KB read.  A match sets `fpga_conf = FPGA_BOOTLEG`,
`mapper_id = 1`, `fpga_dspfeat = variant`, clears any chip the copied header claimed and
sizes the ROM from the file.  `fpga_dspfeat` goes to the FPGA as CMD 0xEF; the core's
`mcu_cmd.v` decodes it as chipfeat.

## Overdumps
Some circulating files are overdumps with every 32 KB bank stored twice: Picachu, Pokemon
Stadium and Tekken 2 (8 MB, the doubled image mirrored once more) and Dragon Ball Z -
Final Bout (4 MB).  They are not recognised as they are; convert them with
  python3 src/utils/bootleg_fp.py --fix OUTDIR *.sfc
which writes the clean images, whose CRCs match the table.

## Dragon Ball Z - Final Bout: restoring the missing sound
Every circulating DBZ Final Bout file (the 2 MB dump, the 4 MB overdump and the cracks)
comes from one dump (CRC 5BBA4EB3) with four blank 32 KB banks, ROM $038000-$057FFF.  They
hold sound data only: the upload table at $05:8000 (76 entries) points into them from entry
#32 on.  A blank entry is a valid empty upload, so the dump runs but goes silent (e.g. from
character select on).  Nothing else references the hole; the protection reads at banks
$87-$8A overlap its ROM offsets, but return the pattern on the real cart.

Source of the data: DVS copied Street Fighter II (Japan) (CRC 5556C5C9) ROM banks $0A-$0F
verbatim into DBZ banks $05-$0A and rebased the table by -5 banks.  Verified: DBZ's intact
65,308 bytes equal SF II at +$28000, the table matches with every bank byte -5, all 76
entries are valid upload chains in SF II ending exactly on DBZ's table boundaries, and the
copy ends at the bank boundary.  So the hole is SF II $060000-$07FFFF:

  python3 src/utils/repair_dbz_sound.py DBZ_DUMP SF2_DUMP DBZ_restored.sfc   -> CRC DD7AFCB9

It accepts the 2 MB dump or the 4 MB overdump and refuses any other input.  The result is
still the protected cart (the core provides the protection) and runs with full sound on
hardware. 

## Open issues
- No savestates on this core (it is not in savestate.c's core list).
- tbd

## Corrections to the fullsnes list
Per nocash (nesdev t=15510, 2017) and confirmed by tracing: A Bug's Life and Bananas de
Pijamas use a "port 6xxx" protection (their bitswap code is dead), and SF EX Plus Alpha uses
the Tekken 2 ALU, not bitswap.  Squirrel, Marvel vs SF, KOF98, Hercules and DBZ Final Bout
were added from the same thread, each checked against its crack.
