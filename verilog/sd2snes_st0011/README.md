# sd2snes_st0011 — dedicated ST011 core

uPD96050 core for **ST011 only** (Hayazashi Nidan Morita Shougi).
Targets **mk2 (Spartan-3 XC3S400)** and **mk3 (Cyclone IV EP4CE15)** from one
source tree.

**Status: working on mk2 and mk3.** The game plays through the move/capture. 
Both targets use identical parameters.

**MSU-1 is included** (`msu.v`, `dac.v`, from `sd2snes_base`; `dac.v` has its
volume multiply pipelined for mk2 timing).
MSU-1 not yet tested on hardware.

## ST010 is not supported

ST010 stays on `sd2snes_dsp`, which is untouched by this work. `smc.c`
selects the core: ST010 keeps `fpga_conf = FPGA_DSP`, ST011 gets
`FPGA_ST0011`. The two chips are separated by core, not by featurebit —
there is no free bit (see `fpga_spi.h`), so this core still keys its bus
decode and external-fetch enable off `FEAT_ST0010`, which here means
"uPD96050 present". `load_dspx()` loads a different firmware geometry and
byte order for each chip, so an ST010 cart on this core would get the wrong
image.

## How it works

### Program fetch (`upd77c25_extpgm.v`)

The ST011 program is 16384 24-bit words, too large for on-chip memory. The
MCU downloads it (`PGM_WR`, `$E9`) into the board's Bus 2 SRAM while the DSP
is held in reset. Each fetch is served from the fastest source that has it:

```
loop buffer   8 recent words, flip-flops, zero latency
pinned table  words 0..255, block RAM, prewarmed after the download
cache         512 entries, direct-mapped block RAM, all other words,
              filled on demand
Bus 2 SRAM    ~32 cycles per word (3 bytes, 45 ns part)
```

### Why speed matters: DMA transfers

The game polls SR for commands, but moves bulk data through DR by **DMA**
with no handshake: a byte lands every ~370 ns whether or not the DSP has
consumed the previous one. The transfer loops (words 197–200 inbound,
243–247 outbound) take 4 instructions per byte. A byte that is overwritten
before it is read is lost, the loop counter never reaches zero, and the DSP
waits forever.

At 96 MHz a cached instruction takes 6 cycles (62.5 ns), about 6 per byte
slot. An SRAM fetch takes a whole slot, so the transfer code must never
miss. Four mechanisms ensure that (a table or cache hit costs no stall
cycles):

- **`SKIP_ALU2`, `PC_LOOKAHEAD`** — 6 cycles per instruction; the next PC is
  looked up in the cache one cycle early.
- **Pinned table** — every word executed during a DMA transfer is below 256
  (0–2, 31, 197–200, 243–247). Those words live in their own table, which
  nothing else can write. With a single shared cache, a routine at words
  12485–12490 once evicted the inbound loop by aliasing, and the next
  transfer dropped a byte: this was the "freeze on capturing a piece".
- **Prewarm** — the pinned table is filled before the game first uses the
  DSP, so the first transfer is not cold.
- **Loop buffer** (`LOOPBUF_ENTRIES`, default 0, i.e. off) — an associative
  buffer of recently fetched words. It was there to protect the transfer
  loops from eviction, which the pinned table now does: over the full trace,
  8 entries and 0 give the same 8.0% miss rate. Its compares hang off `pc`
  and feed both `ready` and the opcode decode, and that path failed
  TS_CLK21 on mk2 by 2.655 ns, so it is off.

## MSU-1

`msu.v` comes unchanged from `sd2snes_base`, as does the IP
(`ip/mk2/msu_databuf.xco`, `ip/mk2/dac_buf.xco`, `ip/mk3/*.qip`). The hookup in
`main.v` is the standard one: the MCU's SD DMA fills the DAC buffer when
`SD_DMA_TGT` is `01` and the MSU data buffer when it is `10`; `address.v`
decodes `msu_enable`, and `mcu_cmd.v`'s MSU/DAC registers were already
present.

`dac.v` has one change from `sd2snes_base`: the volume multiply and
saturation are two register stages deep, with both channels computed every
cycle and the channel chosen at load time (the single-cycle path failed
TS_CLK21 on mk2 by 0.76 ns). Compared with the original in simulation, 98.5%
of output samples are bit-identical; the rest are the CIC output sampled
~21 ns earlier, just before an interpolator step.

`sd2snes_base`'s menu-SFX path (`sfxdma.v`) is not included, so the
DAC is fed by SD DMA only.

## Removed relative to `sd2snes_dsp`

| Removed | Saved | Notes |
|---|---|---|
| On-chip program ROM (`upd77c25_pgmrom`) | 4 RAMB16 / 6 M9K | never read; see reset ordering below |
| Savestate context capture (`ctx.v`) | logic | PSRAM arbiter ports tied off |
| Savestate scan port | logic | tied off in `main.v` (`ss_window_en = ss_halt = 0`), not deleted: the `ss_*` signals feed the data RAM write path, and constant folding removes the overlay without touching it |
| ChipScope | — | removed from the `.xise` only; files remain in `ip/mk2/` for `MK2_DEBUG` builds |

**Reset ordering (why no program ROM is safe).** `mcu_cmd.v` powers up with
`dspx_reset_out = 1` and `main.v` wires `.RST(~dspx_reset)`, so the DSP stays
in reset until after the feature write and firmware load. The cold-start
gates in `upd77c25.v` also require `ext_pgm_en & ext_pgm_ready`, so if that
ordering ever changes the core stalls at pc=0 instead of executing garbage.

## Block RAM (mk2, XC3S400, 16 RAMB16)

```
pin_data          256 x 25    1
cache_data        512 x 30    1
upd77c25_datram  2048 x 16    2
upd77c25_datrom  2048 x 16    2
snescmd_buf      1024 x  8    1
msu_databuf     16384 x  8    8
dac_buf          2048 x  8    1
                             16
```

**All 16 are used.** Anything else that infers block RAM will not fit mk2.

Without MSU-1 the last mk2 build reported 7 RAMB16, 2,903 of 3,584 slices
(80%), 4,329 LUTs (60%) and 2,963 flip-flops (41%). Timing for that build had
not been confirmed after the `cache_q_dirty` change, and MSU-1 adds logic on
top; check TS_CLK21 in the next report.

Before the split cache (4096 × 27 bits, 6 RAMB16) the arrays totalled 11 and
the build reported 13 of 16 RAMB16 at 80% slice occupancy. Check the map
report after rebuilding: the two memories must infer as block RAM (they carry
`ram_style = "block"` for XST), not distributed RAM.

Modelled over the full MesenCE trace, the split cache misses on 8.0% of
instructions against 10.2% for the old pinned 4096-entry cache. Both targets
use the same configuration.

## Configuration

```
upd77c25.v         SKIP_ALU2=1  PC_LOOKAHEAD=1  PREWARM_ENABLE=1
                   LOOPBUF_ENTRIES=0  READ_VERIFY=0
                   stack 16 entries, regs_sp 4 bits
upd77c25_extpgm.v  CACHE_BITS=9 (512)  PIN_BITS=8 (256)  PGM_IN_PSRAM=0
mcu/smc.c          ST0011_WAITSTATES 0
```

- `READ_VERIFY=1` doubles the cost of a miss (32 → 62 cycles); keep 0.
- `PGM_IN_PSRAM=1` selects an unfinished PSRAM fetch path that fails its
  own testbench; keep 0.
- `LOOPBUF_ENTRIES` > 0 re-enables the loop buffer. It buys nothing with the
  split cache and costs mk2 timing. At 0 the loop-buffer vectors keep one
  unused bit, because XST rejects a `[-1:0]` declaration.
- Raise `PIN_BITS` only if some firmware's real-time code sits above 255.

## Building

### mk2 (ISE)

- **Regenerate the CoreGen IP.** The `.ngc` netlists are not shipped. `make`
  regenerates them from the `.xco` files (rule in `common.mk`); the ISE GUI
  does not, and `ngdbuild` then fails with `NgdBuild:604 ... could not be
  resolved`. From the GUI, first run
  `coregen -p ip/mk2 -b ip/mk2/<core>.xco -r` for `snescmd_buf`,
  `upd77c25_datram`, `upd77c25_datrom`, `msu_databuf` and `dac_buf`.
- The data RAM/ROM `.xco` files are sized for the uPD96050: `datram`
  2048 x 16 (port B 4096 x 8), `datrom` 2048 x 16.
- `main.ucf`: the Bus 2 SRAM pins (`RAM_ADDR`, `RAM_DATA`, `RAM_OE`,
  `RAM_WE`) are enabled; upstream `sd2snes_dsp` ships them commented out.
- Map option "Map Slice Logic into Unused Block RAMs" (`-bp`) is off, so
  ISE does not spend block RAMs to save a few LUTs. The `.xise` value must
  be `non-default` to take effect; check `-bp` is absent from the command
  line in `main_map.mrp`. Cover mode stays `Area`.
- ISE requires declaration before use; Icarus does not. If XST reports
  "illegal redeclaration", a signal is used above its declaration.

Two changes exist purely for mk2 timing and are behaviour-neutral:
`stack_top` is a registered copy of `stack[regs_sp-1]`, and the SRAM byte
address (`pc_r * 3`) is registered with `pc_r`.

### mk3 (Quartus)

The Altera `altsyncram` parameters are already correct; no regeneration
needed.
