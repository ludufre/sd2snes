# sd2snes_st0018 — Seta ST018 core (mk2 + mk3)

The ST018 is the 32-bit ARM coprocessor in *Hayazashi Nidan Morita Shougi 2*
(cart type `$F5`, LoROM map `$30`). This core adds it to sd2snes: an ARMv3 CPU
written from scratch (`st018_cpu.v`), 16 KB of work RAM, a ROM cache and the
host mailbox (`st018.v`). The chip's 160 KB firmware (`st018.rom`) is loaded
by the MCU into the Bus 2 SRAM.

Derived from `sd2snes_st0011`. Everything is GPL-2.0 like the rest of the
project; no third-party HDL is used.

## Files on the SD card

| file | contents |
|---|---|
| `/sd2snes/fpga_st0018.bit` (mk2) / `.bi3` (mk3) | this core |
| `/sd2snes/st018.rom` | 163 840 bytes: 128 KB program ROM followed by 32 KB data ROM, the image Ares uses (known good: md5 `dafae0e0c71c924075811c595c61a30e`) |

A missing file is reported by the menu before anything is loaded
(`load_check_prereqs`). With the known-good image the MCU log shows
`ST018 firmware loaded and verified (sum 00eae5da)`.

## What was removed, and why

| removed | why it can go | frees |
|---|---|---|
| uPD96050 (`upd77c25*.v`, datram, datrom) | replaced by the ST018 | 1 998 LUT, 12 RAMB16 (ST011 mk2 build) |
| MSU-1, audio DAC | No ST018 cart uses them | 9 RAMB16 / 18 M9K |
| `ctx.v` (WRAM/APU shadow) | only feeds full savestates; `savestate.c` never enables savestates or the in-game handler on a core outside `core_has_snapshot`, and `FPGA_ST0018` is not on that list | 869 LUT (mk2) |
| `dma.v` (`$2020` copier) | only used by that handler code; `address.v` ties `dma_enable` low, so `$2020-$202F` is ordinary open bus | 475 LUT (mk2) |

Removing `ctx`/`dma` is what makes the mk2 fit (see Budget). Their outputs are
tied to constants in `main.v`, so the PSRAM-arbiter branches that served them
constant-fold away.

## Design

### CPU (`st018_cpu.v`)
Multicycle, not pipelined, not cycle-accurate. The full 32-bit ARMv3
instruction set: all data-processing ops with complete barrel-shifter carry
semantics, MUL/MLA, LDR/STR/LDRB/STRB in all addressing modes (rotated
unaligned word loads), LDM/STM in all modes (S bit, user bank, PC in list,
empty list), SWP/SWPB, B/BL, SWI, MRS/MSR, the undefined-instruction trap and
register banking for every mode. No IRQ/FIQ/abort inputs (the ST018 has no
source for them) and no 26-bit modes. Where the architecture leaves room,
behaviour follows the reference interpreter used for verification (e.g. R15+12
for register-specified shifts and for a stored PC).

Each cycle is kept shallow for CLK2 = 96 MHz: operand fetch, shift, ALU and
write-back are separate states, and a one-entry prefetch buffer fetches the
next instruction during execution. The register file is two block RAMs, which
saves ~350 LUTs on the mk2, where LUTs are scarce and block RAM is not.

### ARM address map (`st018.v`)
| region | contents |
|---|---|
| `0x0xxxxxxx` | program ROM 128 KB → SRAM `0x00000`, cached |
| `0xAxxxxxxx` | data ROM 32 KB → SRAM `0x20000`, cached |
| `0xExxxxxxx` | work RAM 16 KB, block RAM |
| `0x4xxxxxxx` | I/O: `+00` W byte to host; `+10` R byte from host, W signal; `+20` R status. The timer registers `+20..+2C` are accepted and ignored (they have no observable effect in either published emulator implementation). |

Everything else reads 0. Instruction fetches from the I/O region read 0 and
have no side effects, so speculative prefetch can never disturb the mailbox.

### ROM cache
Direct-mapped, read-only, 16-byte lines: **4 KB on mk2, 16 KB on mk3**
(`ST018_CACHE_LB` in `main.v`). Line fills are address-controlled read bursts
from the 8-bit SRAM at one byte per 8 CLK2 cycles. Each byte is taken 62.5 ns
after its address edge (budget: FPGA Tco + tAA 45 ns + Tsu) through a two-stage
synchroniser. The SRAM interface registers go into the I/O blocks (mk2: the
project's "pack I/O registers" setting; mk3: `FAST_*_REGISTER` assignments in
`main.qsf`), so Tco is short and fixed.

### Host registers (`$00-3F/$80-BF:3800-38FF`, decoded as `addr & $FF06`)
| addr | read | write |
|---|---|---|
| `$3800` | byte from ARM (clears status bit 0) | — |
| `$3802` | clears status bit 2; **open bus** | byte to ARM (sets bit 3) |
| `$3804` | status | non-zero holds the ARM in reset |
| `$3806` | open bus | — |

`status = {~reset, 0, 0, 0, host→ARM full, signal, 0, ARM→host full}`.
Bit 5 must read 0: the firmware jumps to `0x60000000` if it is set. Bit 7
reflects both reset sources (`$3804` and the MCU hold). A console reset
(`SNES_reset_strobe`) resets the ARM and clears the mailbox.

## SaveRAM

The board (ares calls it ARM-LOROM-RAM) puts its 8 KB battery SRAM at
**banks `$68-$6F` and `$F0-$FF`, offset `$0000-$7FFF`** — not only at the
`$70-$7D/$F0-$FF` window the generic LoROM rule in `address.v` decodes. The
game uses the `$68` window exclusively: its save and load routines at
`$00:CCB8` and `$00:CCD2` copy `$0E80` bytes between `$68:0180` and `$7E:4180`
with long addressing. With only the generic rule the writes go nowhere and the
reads return ROM, so the game appears to save and then finds no file.
`address.v` therefore adds the `$68-$6F` window for this core (bit 23
qualified, so the `$E8-$EF` mirror stays out).

Nothing is needed on the MCU side: the header's RAM size (`$03` = 8 KB) is
used as-is, and the mapped offset (`address & $1FFF`) matches what other
emulators write, so `.srm` files are interchangeable.

## What the game needs (from its SNES code, `$00:E717-$F0A0`)

* Power-on (`$EFBD-$F09D`): wait for status bit 7, reset the ARM through
  `$3804` (`00`, `FF`, `00`, then wait for bit 7), send `$F1` and `$F2`.
  `$F1` (`0x5e4`: program/data-ROM checksums against values stored in the
  firmware, work-RAM pattern tests) must answer with bit 2 clear, `$F2`
  (`0x658`: further memory tests) with `$00`; otherwise the game prints
  `E1`/`E2` plus the failing address and hangs. They take 45.6 ms / 11.9 ms
  on this core.
* Before every transfer (`$E892`) status bit 4 must be 0.
* Status bit 6 is only waited for if the ROM byte `$FF41` is non-zero; it is
  `$00` in this game, and neither published emulator implements bit 6.
* Every SNES-side wait is an unbounded status poll: the ARM's speed affects
  thinking time only, never correctness.
* The ARM reads status as a 32-bit word at `0x120` and in its send routine
  (`0x764`, `0x774`), but each read tests exactly one low bit (`TST #32`,
  `TST #1`), so the upper 24 bits are never observed. This core returns the
  status byte zero-extended.

## MCU side

| cmd | this core | other cores |
|---|---|---|
| `$EB nn` | hold (1) / release (0) the ARM; powers up held | DSP reset |
| `$E8 00 00` | load pointer = 0, invalidate the cache | DSP pointer reset |
| `$E9 b0 b1 …` | one image byte per parameter byte into SRAM, paced by `MCU_RDY` | DSP 24-bit word write |
| `$E5 00 00` | start the checksum sweep of the 160 KB image (~14 ms) | RTC set |
| `$F5` + 5 reads | `{busy, sum[31:24] … sum[7:0]}` | MSU read |

`$E8`, `$E9` and `$E5` are ignored unless `$EB` holds the ARM, so a stray
command can never rewrite or sweep the SRAM under a running game.

Firmware changes: `smc.c` (detection sets `has_st0018`, `FPGA_ST0018`,
`DSPFW_ST0018`), `smc.h`, `fpga.h`, `fpga_spi.c/.h`
(`fpga_st018_vsum_start/read`) and `memory.c` (`load_st018()`: size check,
stream, verify, one retry; prerequisite check). The load sequence is the usual
one: reconfigure → stream the game ROM → `load_stage_bios()` loads and verifies
`st018.rom` → `assert_reset()` → `init()` → `deassert_reset()` releases the ARM.

## Budget

mk2 (XC3S400: 7 168 LUT4, 16 RAMB16). The base figure comes from the ST011 mk2
map report's utilisation-by-hierarchy table: 5 401 LUT total − 1 998 for the
uPD96050 = 3 403.

| | LUT4 | RAMB16 |
|---|---|---|
| base (ST011 build minus uPD96050) | 3 403 | 1 (`snescmd_buf`) |
| − `ctx`, `dma` | −1 344 | — |
| ST018 subsystem (yosys, scaled by the XST/yosys ratio measured on the uPD96050) | ≈ 3 100–3 500 | 13 (work RAM 8, cache 2 + 1, register file 2) |
| **estimate** | **≈ 5 200–5 600 (73–78 %)** | **14 of 16** |

The LUT figure is an open-source-synthesis estimate calibrated against XST, not
an ISE result; the ISE map report is the real number.

mk3 (EP4CE15, 15 408 LE, 56 M9K) has ample room; the 16 KB cache takes 16 + 1 M9K.

## Performance

With gameplay commands and the real firmware in simulation: **≈ 17.6 MIPS** on
mk2 (4 KB cache) and 17.8 MIPS on mk3, with cache fills taking 2 % of cycles.
The real chip is an ARM6 at 21.47 MHz where branches and loads take 3 cycles,
so this should be roughly on par. The self-test command `$F1` checksums the
whole 160 KB ROM (compulsory misses, ~15 ms); games issue it at boot.

## Notes

* **`spi.v` differs from upstream in one place.** `cmd_ready_r2` and
  `param_ready_r2` use non-blocking `<=`. Synthesis builds the same flip-flop
  either way, but with the upstream blocking `=` the evaluation order of the
  always blocks decides whether a one-cycle ready pulse is seen once, twice or
  not at all, which drops SPI parameter bytes in any top-level simulation.
* Self-modifying code: the core invalidates its prefetch on a store, so code
  always sees the new bytes; a real ARM executes the two instructions it had
  already prefetched. The firmware relies on neither.
* MSR's register operand is Rm unshifted (bits 11:4 are SBZ), even for
  encodings with non-zero SBZ bits.
* The event-board (`CC92`/`PF94`) and cheat logic inherited from the base core
  are untouched.

## Building

Nothing to regenerate: the only IP is `snescmd_buf` (mk2 CoreGen, mk3
megafunction) and `pll` (mk3), both copied unchanged from the ST011 core.
Build with `make mk2` / `make mk3` in this directory, or `make` at the top
level (the mk3 core list includes `st0018`; the top-level mk2 list is empty in
this tree, as before).
