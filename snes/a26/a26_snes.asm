; sd2snes Atari 2600 core -- SNES-side player for the FPGA core, LoROM.
;
; Booted by the firmware (FPGA_A26) as the cart ROM, in place of the .a26 itself
; (a26_update_file swaps the filename; the .a26 is staged separately to PSRAM
; 0x300000, from where the core copies it into BRAM at reset).  The core packs
; the TIA frame into one of two PSRAM sets (0x380000 / 0x390000); the SNES sees
; the set the core is NOT writing through banks $E0-$E3 (address.v).  Each NMI
; this player swaps the double buffer, DMAs the tile-rows the core marked dirty
; into VRAM, and forwards the pads + the console-switch shadow to $EF.
;
; The per-scanline colours are NOT touched here: six HDMA channels, programmed
; ONCE at boot against fixed offsets in bank $E2, feed CGRAM 0-3 and $2100 from
; tables the core rewrites every frame.  That is also what makes the transfer
; window bigger than a plain vblank -- see "VIDEO BUDGET" below.
;
; ASSEMBLER: asar (NOT snescom/sneslink like snes/ and snes/nes/) -- see the
; Makefile next to this file.  Built into misc/a26_snes.bin by build.sh.
;
; IMPORTANT -- this player initialises only what it needs and, unlike the SMS
; one, it also DISARMS the rest of the $21xx block at boot (windows, colour
; math, pseudo-hires): the console reset does not clear it, and a stale colour
; window would fight the CGRAM HDMA in ways that look like a broken core.  It
; hands the block back the same way before an IGR exit.
;
; ---------------------------------------------------------------------------
; VIDEO BUDGET (the premise the whole file is built on)
; ---------------------------------------------------------------------------
; General DMA moves 1 byte per 8 master cycles.  A 224-line vblank is only
; 38 lines = 6479 B, so neither a 12288 B (256-wide) nor comfortably a 7680 B
; (160-wide) frame fits in it.
;
; HDMA channel 5 drives $2100 with $80 (forced blank) on lines 1-16 and
; 209-224 and $0F on line 17; the player ALSO forces blank on NMI entry, which
; is what covers V=0.  The usable, contiguous VRAM window is therefore
;
;     NMI (V=225) .. V=261 .. V=0 .. V=16   =  54 lines = 73656 mc
;
; Normative model (contract sec. 0): fixed cost ~3664 mc, then every DMA costs
; 324 + 8*bytes mc.  Expressed in "byte-equivalents" (mc/8) that is a ceiling of
; !A26_BUDGET = 8749 with every fired DMA charged bytes + 41.  Rows that do not
; fit are left in the backlog and drained next frame, ROUND-ROBIN, so nothing
; starves.
;
; The byte-equivalent counter is a MODEL, and an optimistic one: 41 covers the
; DMA engine's start-up but not the ~350 cycles of guard, addressing and
; bookkeeping this player spends around each transfer, nor the fold.  The hard
; backstop is therefore A26Capacity, which re-derives the remaining capacity
; from the LIVE V counter -- 170 B for each whole line still in vblank, 146 B
; for each of the 17 lines over V=0..16 where the six HDMA channels steal
; ~194 mc.  It is consulted three times per run: to size it, to trim it after
; the sizing loop has itself burned lines, and once more per DMA as the final
; veto.  Past line 17 ch5 has re-enabled the screen and VRAM writes are dropped
; SILENTLY -- the failure mode is corruption, not a glitch, so the whole engine
; is built to give up transfers rather than risk one.
;
; What that costs, measured against the cycle counts of this file: a frame in
; which all 24 rows change and the core publishes spans spends ~17 lines in the
; fold alone, so it moves about half the frame and finishes the rest in the
; next NMI.  With no span table (ver < $02) the fold collapses to a 32-bit OR
; and the same repaint moves ~5.4 KB in one go.  Steady-state frames -- a
; handful of dirty rows -- never come close to the deadline.  Worst observed
; carry-over across the modelled workloads is 3 frames, with no overrun.
;
; ---------------------------------------------------------------------------
; PSRAM set layout as the SNES sees it (address.v a26_sel_off + contract sec. 2)
; ---------------------------------------------------------------------------
;   set 0x0000  = $E0:0000  2bpp bitmap tiles, linear, tile n = 16 B at n*16,
;                           height_rows * cols tiles, row-major
;   set 0x4200  = $E2:0200  control block (16 B)
;   set 0x4210  = $E2:0210  span table, 2 B/row (first_tile, last_tile)
;   set 0x4400  = $E2:0400  six HDMA tables
; The banks are sugar over ONE set: sel_off is 0x0000/0x3000/0x4000/0x6000 and
; the 16-bit bank offset is added on top, so $E2:0200 and $E0:4200 are the same
; byte.  This file always uses the $E2 spelling.
;
; Control block $E2:0200:
;   +$00 flags        bit0 = width (0 = 160, 1 = 256), bit1 = tv, bit2 = parity
;   +$01 height_rows  (v0: 24)
;   +$04..+$07 dirty tile-row bitmap, 32 bits LE (bit N = tile-row N changed
;              since the last CONSUMED swap; accumulative on the core side)
;   +$0F contract_ver, and the core's sign of life
;
; Write window (main.v): $EF0000/1 JOY1 L/H, $EF0002/3 JOY2 L/H, $EF0004 SWCHB
; shadow, $EF0005 buffer swap strobe.  Data and strobe are decoded separately,
; so every store here MUST be 8 bits: a 16-bit store to $EF0004 would also hit
; $EF0005 and fire a phantom swap.

!A26_FORMAT   = 0       ; 0 = Mode 0, 2bpp bitmap + per-line CGRAM HDMA (v0)
                        ; 1 = RESERVE: Mode 3, 8bpp bitmap, fixed 128-colour
                        ;     CGRAM, no colour HDMA.  Structural only -- the
                        ;     core's packer does not emit an 8bpp set, and at
                        ;     4x the bytes per row it cannot keep up.  Kept
                        ;     compilable so the fallback is one define away.

!TILES_BANK   = $E0     ; bitmap tiles
!META_BANK    = $E2     ; control block + span table + HDMA tables

!A26_CTRL     = $0200   ; control block offset inside !META_BANK
!A26_SPAN     = $0210   ; span table  offset inside !META_BANK
!A26_HDMA     = $0400   ; first HDMA table offset inside !META_BANK

; ---------------------------------------------------------------------------
; Offline harness hook.  Default 0 = the device build; with it the file below
; assembles byte for byte as it did before this hook existed.
;
; A wrapper that sets !A26_HARNESS = 1 before including this file replaces the
; only two things the FPGA provides -- the framebuffer set behind banks $E0/$E2
; and the $EF write window -- with a set image linked into the ROM and a WRAM
; page, so an emulator can exercise the transfer engine against a golden set.
; Everything that is actually under test (tilemap, DMA/HDMA programming, the
; budget and overrun arithmetic, the fold/drain) is the same code either way.
; ---------------------------------------------------------------------------
!A26_HARNESS ?= 0

if !A26_HARNESS == 0
!META_LONG    = (!META_BANK<<16) ; long base of the control block / span table
!META_A1B     = !META_BANK       ; bank + 16-bit base the HDMA tables are read
!META_A16     = $0000            ; from (the bank offset IS the set offset)
!TILES_A1B    = !TILES_BANK      ; ... and the same pair for the tile DMA
!TILES_A16    = $0000
!A26_EF       = $EF0000          ; write window: pads, SWCHB, swap strobe
else
; The golden set image is linked at $02:8000 (a whole 32 KB LoROM bank), so set
; offset X reads back at $028000+X: the tiles ($E0:0000 = set 0x0000) at
; $028000 and the meta block ($E2:0000 = set 0x4000) at $02C000.  Nothing
; answers at $EF here, so the write window lands in WRAM and stays observable.
!META_LONG    = $02C000
!META_A1B     = $02
!META_A16     = $C000
!TILES_A1B    = $02
!TILES_A16    = $8000
!A26_EF       = $7E1000
endif

!A26_VER      = $02     ; contract wire version this player implements
!A26_ROWSMAX  = 28      ; structural cap on height_rows (contract sec. 2)
!A26_ROWSDEF  = 24      ; v0 height_rows, and the fallback for a bogus field
!A26_ROW0     = 2       ; first tilemap row of content (2 rows = 16 px letterbox)
!A26_COLS160  = 20      ; 160 px / 8
!A26_COLS256  = 32      ; 256 px / 8
!A26_LIFEWAIT = 120     ; frames (~2 s) to wait for the core's sign of life

; Byte-equivalent ceilings.  !A26_BUDGET is the contract figure; !A26_BUDGETDEG
; is what a plain vblank affords, used when there is no core to drive ch5 and
; the NMI has to blank the screen itself (19 rows * 320 B).
!A26_BUDGET    = 8749
!A26_BUDGETDEG = 6080
; TWO per-DMA costs, because the two questions are different.
;
; !A26_DMASTART is what the DMA engine itself adds to a transfer (contract
; sec. 0: 324 mc).  That is the right addend when asking "will this DMA be off
; the bus before the deadline", because the player's own work around it happens
; either before the scanline is sampled or inside !A26_GUARDSLACK.
;
; !A26_DMACOST is what a fired DMA actually costs the FRAME: the engine's
; start-up plus the ~350 cycles (~2800 mc) this player spends per transfer on
; the capacity call, the address arithmetic and the bookkeeping, all out of
; slow ROM at 8 mc/cycle.  That is the right figure for the budget line and for
; choosing between the two transfer shapes.
;
; Charging the budget the contract's 41 was the bug behind the field report:
; a Freeway frame is a median of 11 dirty rows with spans ~3.5 tiles wide, so
; the span shape looks nearly free (41 each) and the budget happily authorises
; a dozen tiny DMAs -- which really cost ~4500 byte-equivalents of fixed
; overhead, blow the window, and leave the overrun guard to cut the frame in
; half.  Rows deferred in a game where everything moves every frame means two
; frames of content on screen at once: permanent tearing.  With an honest debit
; the coalesced whole-row shape wins the comparison and the frame closes.
; 384 = 256+128, so n*cost stays two shifted adds and errs 2% conservative
; against the ~375 measured.
!A26_DMASTART  = 41
!A26_DMACOST   = 384

; Overrun guard: the last scanline that is still forced blank.  ch5 writes $0F
; on line 17, so everything must be on the bus before line 17 starts.
!A26_DEADLINE  = 17
!A26_BPLVBL    = 170    ; bytes/line with no HDMA running (1364 mc / 8)
!A26_BPLHDMA   = 146    ; bytes/line over V=0..16 ((1364 - 194) mc / 8)
!A26_TAILBYTES = 2482   ; = 17 * !A26_BPLHDMA, the V=0..16 tail
; Held back from every capacity answer.  The scanline is sampled BEFORE the
; player computes addresses and writes $420B, and none of that work is in the
; capacity arithmetic -- without the reserve the last DMA of a frame finishes
; one or two lines past the deadline.  256 is the smallest value that keeps
; every modelled workload (full-frame 160/256, 90%-dirty rows, 3-tile spans)
; strictly inside the window.
!A26_GUARDSLACK = 512   ; measured: ch5 turns the screen back on in line 16's
                        ; hblank (~112 mc before line 17) and the guard-accept ->
                        ; DMA-start latency is ~3000 mc (~375 byte-equivalents);
                        ; 256 left only 0.38 line of margin, 512 gives 0.9-1.65
; Held back on top of that when SIZING a run, because everything between the
; sizing and the trigger -- the trim loop, the hybrid decision, the guard, the
; address arithmetic -- is another ~690 cycles that the capacity answer knows
; nothing about.  Without it the trim is stale by the time the guard runs, the
; guard rejects, and the frame sends nothing.
!A26_FIRESLACK  = 640

; Core capability, decided once at boot from $E2:020F (contract sec. 3):
!A26_MODE_SPAN = 0      ; ver >= $02: dirty bitmap + span table
!A26_MODE_ROWS = 1      ; ver == $01, or an unknown ver: dirty bitmap, full rows
!A26_MODE_DEAD = 2      ; ver == $00 after the timeout: no core answering

; Fast path, with hysteresis so a middling load cannot oscillate between the
; two drains frame after frame.  Engage once this many tile-rows are pending,
; let go again below the lower figure.  See A26FastDrain for what it does and
; why the per-row span fold has to be skipped for it to pay.
!A26_FASTPATH_ON  = 8
!A26_FASTPATH_OFF = 4

; Pillarbox for the 160-wide mode: the content occupies tilemap columns 6..25,
; i.e. screen x = 48..207.
!A26_PILLAR_L = 48
!A26_PILLAR_R = 207

if !A26_FORMAT == 0
!BG1_MAP        = $2000  ; map base, VRAM word.  2bpp: 768 tiles = words
                         ; $0000-$17FF, so $2000 clears the bitmap with room
!A26_TILE_WSH   = 3      ; tile -> VRAM word shift (16 B = 8 words per tile)
!A26_TILE_BSH   = 4      ; tile -> source byte shift
else
!BG1_MAP        = $6000  ; 8bpp: 768 tiles = words $0000-$5FFF.  This reserve
                         ; assumes height_rows <= 24; a taller frame would run
                         ; the bitmap into the map and needs a new base.
!A26_TILE_WSH   = 5      ; 64 B = 32 words per tile
!A26_TILE_BSH   = 6
endif

; Empty tile (contract sec. 4: an index OUTSIDE the content range, not "white").
; Tile $3FF is at word $1FF8 (2bpp, below the $2000 map) and at word $7FE0
; (8bpp, above the $6400 map end) -- free in both formats.  Boot clears all of
; VRAM, so it reads back as pixel 0 = transparent = backdrop, which is exactly
; what the contract specifies for the 160-wide side bars (COLUBK of the line).
!A26_EMPTY_TILE = $03FF

; ---------------------------------------------------------------------------
; Direct page map (DP = 0 throughout)
; ---------------------------------------------------------------------------
;   $03      NMI re-entrancy latch (0 = not inside the NMI body)
;   $04      IGR edge latch (born armed)
;   $05      cols (20 or 32)
;   $06      SWCHB persistent bits (COLOR + the two difficulty switches)
;   $07      previous JOY1L, for the switch edge detector
;   $08-$09  budget ceiling in byte-equivalents
;   $0B      core mode (!A26_MODE_*)
;   $0C      height_rows
;   $0E-$0F  JOY1L / JOY1H  (read as one word = the IGR combo)
;   $10      round-robin drain cursor -- PERSISTENT ACROSS NMIs (contract
;            sec. 3: restarting at row 0 starves the bottom of the screen)
;   $11      1 = ignore the core's span table and treat every row as full
;   $12-$13  current run: first row, row count
;   $14-$15  cols as a 16-bit value (the span loop adds it per row)
;   $17      switch edge byte
;   $18      SWCHB output byte
;   $19      IGR command parking slot
;   $1A-$1B  scratch (V counter / guard arithmetic)
;   $1C-$1D  bytes of the DMA the guard is being asked about
;   $1E-$1F  budget left this NMI, byte-equivalents
;   $20-$23  PENDING dirty tile-rows, 32 bits -- the transfer backlog lives
;            here, not in the core: the core clears its bits on the consumed
;            swap, so a row we could not afford is ours to remember
;   $24-$27  incoming dirty bitmap (fold phase only)
;   $28      row cursor (fold: row being folded; drain: row being scanned)
;   $29      last row of the segment being drained
;   $2A      drain abort flag
;   $2B-$2C  index of the bitmap byte holding the cursor row (high byte 0)
;   $2D      bit mask of the cursor row within that byte
;   $2E      round-robin cursor as it was on entry (segment 2 end)
;   $2F      fold scratch: row was already pending
;   $30-$31  run: bytes of the full-row option
;   $32-$33  run: sum of span tile counts
;   $34-$35  bytes per full tile-row (cols << !A26_TILE_BSH)
;   $36-$37  run: cost of the span option
;   $38-$39  run: cost of the full-row option
;   $3A-$3B  helper row / helper count
;   $3E      A26RowBit mask output (separate from $2D on purpose: the helpers
;            that use it run inside loops that are still holding $2D)
;   $40-$77  PENDING span, 2 B per row (first_tile, last_tile), up to 28 rows
;   $78-$79  ceiling for the run being built = min(budget left, time left)
;   $7A      popcount accumulator / result
;   $7B      popcount byte scratch
;   $7C      bitmap bytes actually in use = ceil(height_rows / 8)
;   $7D-$7E  popcount byte index (high byte kept 0 for ldx)
;   $7F      1 = fast path engaged this NMI (hysteresis, see A26FastSelect)

lorom

org $008000
Reset:
    sei
    clc
    xce
    rep #$38
    ldx.w #$1FFF
    txs
    lda.w #$0000
    tcd
    phk
    plb                 ; DBR = 0: every $21xx/$42xx below is absolute
    sep #$20

    lda.b #$01
    sta.b $04           ; IGR edge latch, born ARMED: a combo still held
                        ; through the $80 reset must be released before it
                        ; can fire again (see the IGR block in the NMI)

    lda.b #$3F
    sta.b $06           ; SWCHB shadow: RESET/SELECT released, COLOR set, both
                        ; difficulty switches at B.  Matches the RTL's power-on
                        ; a26_swchb = 8'h3F.  Set up here, before anything reads
                        ; the direct page 16 bits at a time.
    sta.l !A26_EF+$04
    lda.b #$00
    sta.b $07           ; no buttons held last frame
    sta.b $17
    sta.b $10           ; round-robin cursor starts at row 0 exactly once
    sta.b $03           ; NMI re-entrancy latch: not inside the body.  The menu
                        ; leaves WRAM as it found it, so a gate byte that is
                        ; read before it is written has to be zeroed by hand.

    lda.b #$8F
    sta.w $2100         ; forced blank, held until the core signs $E2:020F
    stz.w $4200         ; no NMI, no auto-joypad yet
    stz.w $420C         ; no HDMA yet: the tables are still garbage
    stz.w $420B
    lda.b #$FF
    sta.w $4201         ; WRIO high.  Reading $2137 latches the H/V counters
                        ; ONLY while $4201 bit7 is set, and the entire overrun
                        ; guard is a function of that latch: with the pin low
                        ; $213D would return one frozen scanline forever, and
                        ; the guard would either veto every transfer or wave
                        ; every one of them past line 17.  The console reset
                        ; leaves $FF here; the guard is too load-bearing to
                        ; inherit it rather than state it.

    jsr A26PpuDisarm    ; windows / colour math / pseudo-hires off

    stz.w $210D         ; BG1HOFS = 0 (160-wide is pillarboxed by the tilemap,
    stz.w $210D         ; not by scrolling -- contract sec. 4)
    lda.b #$FF
    sta.w $210E         ; BG1VOFS = $FFFF.  The SNES displays BG line VOFS+n+1,
    sta.w $210E         ; so $FFFF puts BG line 16 (tile-row 2, the first
                        ; content line) on visible line 17 -- exactly where
                        ; HDMA entry 16 puts its colour.  VOFS=0 would shift
                        ; every colour one line above its pixels and drop the
                        ; first content line into the blanked line 16.
                        ; (Off-by-one proven on hardware in this repo:
                        ;  snes/nes/nes_render.a65:7629.)

if !A26_FORMAT == 0
    lda.b #$00
    sta.w $2105         ; BGMODE 0 (four 2bpp layers; BG1 owns CGRAM 0-31)
    lda.b #$20
    sta.w $2107         ; BG1SC: map base word $2000, 32x32
else
    lda.b #$03
    sta.w $2105         ; BGMODE 3 (BG1 8bpp)
    lda.b #$60
    sta.w $2107         ; BG1SC: map base word $6000, 32x32
endif
    lda.b #$00
    sta.w $210B         ; BG12NBA: BG1 char base word $0000
    lda.b #$01
    sta.w $212C         ; TM: BG1 only
    stz.w $212D         ; TS: nothing on the subscreen

    jsr A26ClearVram    ; ~24 ms; the empty tile and every not-yet-painted tile
                        ; must read back as pixel 0

if !A26_FORMAT == 1
    jsr A26LoadPalette  ; 8bpp reserve: CGRAM is static, uploaded once
endif

    jsr A26ApuUnmute    ; open the console's cart-DAC audio path

    ; --- sign of life -------------------------------------------------------
    ; The core writes the wire version to +$0F of the set it is PACKING, and
    ; that set only becomes visible to us through a swap.  So the wait has to
    ; pump $EF0005 exactly like the NMI does -- main.v gates the strobe on
    ; a26_ready, so it is a no-op until the first FRAME_DONE and a real swap
    ; right after it.  Without the pump this deadlocks (contract sec. 3).
    ; Release the forced blank on ANY non-zero version; only the FEATURES are
    ; version-gated.
    ldx.w #!A26_LIFEWAIT
A26LifeWait:
    lda.b #$00
    sta.l !A26_EF+$05
    lda.l !META_LONG+!A26_CTRL+$0F
    bne A26LifeOk
    jsr A26FrameWait
    dex
    bne A26LifeWait

    ; timed out: no core is answering.
    lda.b #!A26_MODE_DEAD
    sta.b $0B
    lda.b #!A26_COLS160
    sta.b $05
    lda.b #!A26_ROWSDEF
    sta.b $0C
    rep #$20
    lda.w #!A26_BUDGETDEG
    sta.b $08
    sep #$20
    bra A26LifeDone

A26LifeOk:
    cmp.b #!A26_VER
    bcc A26LifeOld      ; ver < $02: dirty bitmap only, no span table
    beq A26LifeSpan
    ; ver > $02: a core newer than this player.  The dirty bitmap is the only
    ; field whose meaning is guaranteed, so take the conservative width too.
    lda.b #!A26_MODE_ROWS
    sta.b $0B
    lda.b #!A26_COLS160
    sta.b $05
    bra A26LifeGeom
A26LifeOld:
    lda.b #!A26_MODE_ROWS
    sta.b $0B
    bra A26LifeWidth
A26LifeSpan:
    lda.b #!A26_MODE_SPAN
    sta.b $0B
A26LifeWidth:
    lda.l !META_LONG+!A26_CTRL
    and.b #$01          ; flags bit0 = video_width (echo of feat16[5])
    beq A26LifeW160
    lda.b #!A26_COLS256
    sta.b $05
    bra A26LifeGeom
A26LifeW160:
    lda.b #!A26_COLS160
    sta.b $05
A26LifeGeom:
    lda.l !META_LONG+!A26_CTRL+$01
    beq A26LifeRowsDef  ; 0 is not a legal height
    cmp.b #(!A26_ROWSMAX+1)
    bcc A26LifeRowsOk
A26LifeRowsDef:
    lda.b #!A26_ROWSDEF
A26LifeRowsOk:
    sta.b $0C
    rep #$20
    lda.w #!A26_BUDGET
    sta.b $08
    sep #$20
A26LifeDone:

    ; cols as a word, and bytes per full tile-row = cols * bytes-per-tile
    rep #$20
    lda.b $05
    and.w #$00FF
    sta.b $14
!i = 0
while !i < !A26_TILE_BSH
    asl a
!i #= !i+1
endwhile
    sta.b $34
    sep #$20

    ; bitmap bytes actually in use, for the pending-row count
    lda.b $0C
    clc
    adc.b #$07
    lsr a
    lsr a
    lsr a
    sta.b $7C
    lda.b #$00
    sta.b $7D
    sta.b $7E           ; popcount index, kept 16-bit clean
    sta.b $7F           ; fast path starts disengaged

    jsr A26BuildMap     ; static tilemap; nothing rewrites it after this
    jsr A26ArmPillar    ; black side bars in the 160-wide mode

    ; Seed the backlog with every row at full width: VRAM is blank, so the
    ; first frames have to paint everything no matter what the core reports.
    jsr A26FillSpans
    lda.b #$01
    sta.b $11
    jsr A26AllDirty
    jsr A26Fold

    ; from here on, only a core that publishes spans gets to use them
    lda.b $0B
    cmp.b #!A26_MODE_SPAN
    bne A26SpanOff
    lda.b #$00
    sta.b $11
    bra A26SpanDone
A26SpanOff:
    lda.b #$01
    sta.b $11
A26SpanDone:

    lda.b $0B
    cmp.b #!A26_MODE_DEAD
    beq A26NoHdma       ; no core: the tables are zeros, and a zero table
                        ; terminates on its first byte -- ch5 would never
                        ; blank anything, so run HDMA-less and self-blank
    jsr A26FrameWait    ; arm the channels in vblank, not mid-frame
    jsr A26HdmaSetup
if !A26_FORMAT == 0
    lda.b #$3F          ; HDMA ch0-5: CGADD reset, 4 colours, letterbox blank
else
    lda.b #$20          ; 8bpp reserve: only the letterbox blank channel
endif
    sta.w $420C
A26NoHdma:

    lda.b #$81
    sta.w $4200         ; NMI + auto-joypad
    lda.b #$0F
    sta.w $2100         ; screen on; from here ch5 owns $2100 during the frame

MainLoop:
    wai
    bra MainLoop

; ===========================================================================
; NMI -- one frame.
; ===========================================================================
NMI:
    rep #$30
    pha
    phx
    phy
    sep #$20

    lda.b #$8F
    sta.w $2100         ; force blank for the whole transfer window.  ch5 keeps
                        ; lines 1-16 and 209-224 blanked, but V=0 belongs to no
                        ; HDMA entry -- this write is what covers it, and ch5
                        ; hands the screen back on line 17.  The NMI never
                        ; writes $2100 again unless there is no core at all.

    ; --- 1) swap the double buffer FIRST ------------------------------------
    ; Contract sec. 5: the HDMA reads the colour tables out of the FRONT set
    ; during the display.  Swapping at the END of the NMI (the SMS/NES pattern)
    ; would put the colours of frame N over the pixels of frame N-1 for good.
    ; The strobe is unconditional; main.v ignores it unless a26_ready, so a
    ; frame the core has not finished simply leaves the front alone and we
    ; re-read (and re-fold) a bitmap we already consumed -- idempotent.
    lda.b #$00
    sta.l !A26_EF+$05

    ; --- 1b) re-entrancy latch ----------------------------------------------
    ; The 65816 takes NMI with I set, so a handler that outlives its own frame
    ; is re-entered at the next vblank edge -- and everything below keeps its
    ; state in the direct page, which is NOT re-entrant: the inner run would
    ; rewrite $12/$13/$28-$3F under the outer one and the outer would then
    ; clear the wrong backlog bits and park the cursor on the wrong row.
    ; The transfer engine is deadline-bounded so this should not happen, but
    ; "should not" is exactly what a wedge costs the least to defend against.
    ; NOTE the ORDER: the swap strobe above is deliberately OUTSIDE the latch.
    ; The one invariant the core depends on is that $EF0005 keeps arriving
    ; (main.v holds a26_ready = 1 until a strobe consumes it, and stops
    ; announcing frames while it is held), so a re-entered NMI still strobes
    ; and only skips the part that owns direct-page state.
    lda.b $03
    bne A26NmiBusy
    inc.b $03

    ; --- 2) fold the core's dirty bitmap + spans into the backlog -----------
    lda.b $0B
    cmp.b #!A26_MODE_DEAD
    beq A26NmiFoldAll
    jsr A26ReadDirty    ; incoming <- the core's bitmap
    bra A26NmiSelect
A26NmiFoldAll:
    jsr A26AllDirty     ; no core: repaint everything, every frame
A26NmiSelect:
    jsr A26FastSelect   ; heavy load -> skip the span fold entirely
    jsr A26Fold

    ; --- 3) drain the backlog into VRAM -------------------------------------
    lda.b $7F
    bne A26NmiFast
    jsr A26Drain
    bra A26NmiDrained
A26NmiFast:
    jsr A26FastDrain
A26NmiDrained:

    lda.b $0B
    cmp.b #!A26_MODE_DEAD
    bne A26NmiNoShow
    lda.b #$0F
    sta.w $2100         ; no ch5 to hand the screen back
A26NmiNoShow:

    ; --- 4) pads ------------------------------------------------------------
    ; $4218 mid auto-read is garbage; the transfer above normally outlasts it,
    ; but a frame with nothing dirty would get here early.
A26PadWait:
    lda.w $4212
    lsr a
    bcs A26PadWait
    lda.w $4218
    sta.b $0E
    sta.l !A26_EF+$00       ; JOY1L (A X L R - - - -)
    lda.w $4219
    sta.b $0F
    sta.l !A26_EF+$01       ; JOY1H (B Y Sel St Up Dn Lf Rt)
    lda.w $421A
    sta.l !A26_EF+$02       ; JOY2L
    lda.w $421B
    sta.l !A26_EF+$03       ; JOY2H

    jsr A26Switches
    jsr A26Igr

    stz.b $03           ; body finished: the next NMI may own the direct page
A26NmiBusy:
    lda.w $4210         ; ack NMI

    rep #$30
    ply
    plx
    pla
    sep #$20
    rti

Stub:
    rti

; ===========================================================================
; FOLD -- merge what the core reports into the player's backlog.
;
; The core clears its dirty bits on the consumed swap, so a row we could not
; afford last frame is ours to remember; and because a deferred row keeps
; accumulating changes, its pending span must be the UNION of every span the
; core has published for it since we last sent it (contract sec. 3).  Sending
; only the newest span would leave older columns permanently stale.
; ===========================================================================
A26ReadDirty:
    rep #$20
    lda.l !META_LONG+!A26_CTRL+$04
    sta.b $24
    lda.l !META_LONG+!A26_CTRL+$06
    sta.b $26
    sep #$20
    rts

A26AllDirty:
    lda.b #$FF
    sta.b $24
    sta.b $25
    sta.b $26
    sta.b $27
    rts

A26Fold:
    ; No span table in play -> every row is full width, the stored spans were
    ; filled once at boot and never change, so the whole fold collapses to a
    ; 32-bit OR.  That is the ver<$02 path, the no-core path, the boot seed --
    ; and any NMI the fast path has taken over, because the per-row span fold
    ; is the single most expensive thing in a heavy frame (~120 cycles a dirty
    ; row, over 17 lines when all 24 change) and the fast path does not use
    ; spans at all.
    lda.b $11
    ora.b $7F
    beq A26FoldRows
    rep #$20
    lda.b $24
    ora.b $20
    sta.b $20
    lda.b $26
    ora.b $22
    sta.b $22
    sep #$20
    rts

A26FoldRows:
    lda.b #$00
    sta.b $28           ; row
    sta.b $2B
    sta.b $2C           ; bitmap byte index = 0 (kept 16-bit clean for ldx)
    lda.b #$01
    sta.b $2D           ; mask
A26FoldRow:
    ldx.b $2B
    lda.b $2D
    cmp.b #$01
    bne A26FoldBit
    lda.b $24,x
    bne A26FoldBit
    lda.b $28           ; whole byte clean and we are on its first row:
    clc                 ; step over all eight instead of testing them
    adc.b #$08
    sta.b $28
    inc.b $2B
    cmp.b $0C
    bcc A26FoldRow
    rts
A26FoldBit:
    lda.b $24,x
    and.b $2D
    beq A26FoldNext
    jsr A26FoldSpan
A26FoldNext:
    inc.b $28
    asl.b $2D
    bne A26FoldSame
    lda.b #$01
    sta.b $2D
    inc.b $2B
A26FoldSame:
    lda.b $28
    cmp.b $0C
    bcc A26FoldRow
    rts

; Row $28 changed.  X = index of its bitmap byte, $2D = its mask.
A26FoldSpan:
    lda.b $20,x
    and.b $2D
    sta.b $2F           ; 0 = the row was not pending yet
    lda.b $20,x
    ora.b $2D
    sta.b $20,x

    rep #$20
    lda.b $28
    and.w #$00FF
    asl a
    tax                 ; X = row * 2, the span index
    sep #$20

    ; --- sanitise the pair BEFORE it can reach the pending table ------------
    ; This is the ONE place the core's span bytes enter the player, so it is
    ; the only place they have to be checked: everything downstream (the run
    ; sizing, the trim, the span DMA) consumes a span as "last - first + 1"
    ; in EIGHT BITS.  A pair with last == first-1 makes that count 256, the
    ; inc wraps it to 0, and 0 in $4365 is a 65536-BYTE DMA -- the whole of
    ; VRAM overwritten with 1.5 frames of halted CPU behind it.  That is the
    ; same failure the SMS player documents for a stale $4305, reached here
    ; through the COUNT instead of the length.  first > last and either end
    ; past cols-1 are cheap to catch in the same breath (they also walk the
    ; DMA source out of the tile region and the destination into the tilemap),
    ; so any pair that is not a legal sub-range of the row is widened to the
    ; whole row: the worst a broken core can now cost is one extra full-row
    ; transfer.  With this the invariant $40,x <= $41,x <= cols-1 holds for
    ; every entry (A26FillSpans seeds it, this is the only other writer), so
    ; the count is in [1, cols] by construction and can never wrap.
    lda.l !META_LONG+!A26_SPAN,x
    sta.b $7A           ; first
    lda.l !META_LONG+!A26_SPAN+$01,x
    sta.b $7B           ; last
    cmp.b $05
    bcs A26FoldWide     ; last >= cols
    cmp.b $7A
    bcs A26FoldSane     ; first <= last <= cols-1
A26FoldWide:
    lda.b #$00
    sta.b $7A
    lda.b $05
    dec a
    sta.b $7B
A26FoldSane:

    lda.b $2F           ; (only reached with a span-capable core: A26Fold takes
    beq A26FoldNew      ;  the flat path otherwise)
    lda.b $7A
    cmp.b $40,x
    bcs A26FoldKeepLo
    sta.b $40,x         ; union: lower first tile wins
A26FoldKeepLo:
    lda.b $7B
    cmp.b $41,x
    bcc A26FoldKeepHi
    sta.b $41,x         ; union: higher last tile wins
A26FoldKeepHi:
    rts
A26FoldNew:
    lda.b $7A
    sta.b $40,x
    lda.b $7B
    sta.b $41,x
    rts

; Every row's span = the whole row.  Called once at boot; in full-span mode
; nothing ever rewrites the table, which is what lets A26Fold skip it.
A26FillSpans:
    ldx.w #$0000
A26FillLoop:
    lda.b #$00
    sta.b $40,x
    lda.b $05
    dec a
    sta.b $41,x         ; the whole row, cols-1 being the last tile
    inx
    inx
    cpx.w #(!A26_ROWSMAX*2)
    bne A26FillLoop
    rts

; ===========================================================================
; FAST PATH SELECTOR
;
; The span machinery is worth its cost only while the picture is mostly still.
; Under a heavy load -- a screen transition, or a game like Freeway where every
; lane moves every frame and 20-24 rows are dirty CONTINUOUSLY -- it is the
; thing standing between the player and a full repaint per frame: the fold
; costs ~120 cycles per dirty row (over 17 lines at 24 rows) and the round-robin
; hybrid then splits what is left into pieces, so a repaint takes 4-5 frames and
; the screen is visibly torn the whole time.
;
; Above !A26_FASTPATH_ON pending rows the span fold is skipped outright and the
; drain sweeps whole contiguous rows instead (A26FastDrain).  That buys back the
; ~17 lines, which is exactly what a 160-wide full repaint needs: 24 rows in one
; 7680-byte DMA is 7721 byte-equivalents, ~88% of the ceiling, and it lands
; around V=13 with the fold out of the way.
;
; Hysteresis (release only below !A26_FASTPATH_OFF) keeps a middling load from
; flipping between the two drains on alternate frames, which would be worse than
; either: the span table is not maintained while the fast path is engaged, so
; every crossing costs an A26FillSpans.
; ===========================================================================
A26FastSelect:
    jsr A26TodoCount    ; rows that will be pending once the fold is done
    sta.b $7B
    lda.b $7F
    bne A26FastStay
    lda.b $7B
    cmp.b #!A26_FASTPATH_ON
    bcc A26FastSelDone
    lda.b #$01
    sta.b $7F           ; engage
    rts
A26FastStay:
    lda.b $7B
    cmp.b #!A26_FASTPATH_OFF
    bcs A26FastSelDone
    lda.b #$00
    sta.b $7F           ; release
    ; The rows still pending carry spans the fast path never maintained, so
    ; widen every one of them to the whole row before the span drain can read
    ; them.  A superset is always safe; a stale narrow span silently drops the
    ; columns that changed while the fast path was running.
    jsr A26FillSpans
A26FastSelDone:
    rts

; Set bits in (pending | incoming), over the bytes that can hold a valid row.
; Nibble lookup rather than a bit loop: this runs every NMI, including the ones
; that have no line to spare.
A26TodoCount:
    lda.b #$00
    sta.b $7A
    sta.b $7D
A26TodoLoop:
    ldx.b $7D
    lda.b $20,x
    ora.b $24,x
    sta.b $7B
    rep #$20
    and.w #$000F
    tax
    sep #$20
    lda.l A26BitCount,x
    clc
    adc.b $7A
    sta.b $7A
    rep #$20
    lda.b $7B
    and.w #$00F0
    lsr a
    lsr a
    lsr a
    lsr a
    tax
    sep #$20
    lda.l A26BitCount,x
    clc
    adc.b $7A
    sta.b $7A
    inc.b $7D
    lda.b $7D
    cmp.b $7C
    bne A26TodoLoop
    lda.b $7A
    rts

A26BitCount:
    db 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4

; ===========================================================================
; FAST DRAIN -- one contiguous block of whole rows, straight down the frame.
;
; No hybrid and no run-by-run walk: one pass finds the first and last pending
; row of the segment and everything between them goes out as a single whole-row
; DMA.  Clean rows caught inside that extent are re-sent -- they cost bandwidth
; and nothing else, and one transfer of 8x the bytes still beats a dozen tiny
; ones once each firing is charged what it really costs.
;
; That is the shape the field report needed.  A Freeway frame is a median of 11
; dirty rows (p95 19, max 24) in one to five runs, with spans averaging 3.5 of
; 20 columns -- so the span shape produces a dozen minuscule DMAs whose fixed
; cost alone overruns the window, the guard cuts the frame in half, and a game
; where every lane moves every frame ends up showing two frames at once.  Over
; the 600-frame trace this path drains the backlog COMPLETELY in every single
; frame at 160, in two DMAs, where the span path left a median of 4.3 rows
; behind in 69% of frames.
;
; The sizing step reads the live V counter and shrinks the block until it fits
; before the deadline, so it IS the overrun guard for this path -- there is no
; second check because there is no run-building loop in between to invalidate
; the first one (which is exactly what !A26_FIRESLACK exists to absorb on the
; slow path, and why the fast path must not pay it).
;
; 160-wide: the whole frame is 7680 B, so the block is the whole frame and the
; repaint closes every NMI.  256-wide: 24 rows are 12288 B and simply do not
; fit in one window, so the block is trimmed to ~15 rows and the cursor carries
; the remainder into the next NMI -- sequentially, top to bottom, never
; round-robin, so the seam is a single moving edge rather than a scramble.
; ===========================================================================
A26FastDrain:
    rep #$20
    lda.b $08
    sta.b $1E           ; budget line kept for parity with the slow path; on
    sep #$20            ; this path the clock is what actually binds
    lda.b #$00
    sta.b $2A
    lda.b $10
    sta.b $2E           ; where the sweep began

    ; Two straight segments, because a block has to be contiguous in row number
    ; and the wrap from the last row to row 0 is not.  BOTH are attempted: a
    ; segment with nothing pending is simply a no-op, and skipping the second
    ; one whenever the first found nothing would wedge the sweep -- the cursor
    ; would never move off a clean tail.
    lda.b $2E
    sta.b $28
    lda.b $0C
    dec a
    sta.b $29
    jsr A26FastSeg
    lda.b $2A
    bne A26FastDrainDone
    lda.b $2E
    beq A26FastDrainDone ; the sweep began at row 0: segment 1 was everything
    lda.b #$00
    sta.b $28
    lda.b $2E
    dec a
    sta.b $29
    jsr A26FastSeg
A26FastDrainDone:
    rts

; One coalesced DMA covering the pending EXTENT inside rows $28..$29 -- from
; the first pending row to the last, clean rows in between included.  A single
; upward pass finds both ends; scanning down from the top separately would cost
; an A26RowBit per row.
A26FastSeg:
    lda.b $28
    cmp.b $29
    beq A26FastSegScan
    bcs A26FastSegNone  ; empty segment
A26FastSegScan:
    lda.b #$00
    sta.b $3B           ; nothing found yet
    lda.b $28
    jsr A26RowBit       ; X = bitmap byte index, $3E = mask
A26FastScan:
    lda.b $20,x
    and.b $3E
    beq A26FastScanNext
    lda.b $3B
    bne A26FastScanHi
    lda.b $28
    sta.b $12           ; first pending row of the extent
    lda.b #$01
    sta.b $3B
A26FastScanHi:
    lda.b $28
    sta.b $3A           ; last pending row so far
A26FastScanNext:
    lda.b $28
    cmp.b $29
    beq A26FastScanEnd
    inc.b $28
    asl.b $3E
    bne A26FastScan
    lda.b #$01
    sta.b $3E
    inx
    bra A26FastScan
A26FastScanEnd:
    lda.b $3B
    beq A26FastSegNone  ; nothing pending in this segment
    lda.b $3A
    sec
    sbc.b $12
    inc a
    sta.b $13           ; rows in the extent
    lda.b $13
    jsr A26RowTile      ; A(16) = rows * cols
    rep #$20
!i = 0
while !i < !A26_TILE_BSH
    asl a
!i #= !i+1
endwhile
    sta.b $30           ; bytes for the whole block
    sep #$20

    jsr A26Capacity     ; live scanline count: this is the overrun guard here
    sta.b $78
    sep #$20
A26FastTrim:
    rep #$20
    lda.b $30
    clc
    adc.w #!A26_DMASTART    ; clock question: only the engine's own start-up
    cmp.b $78               ; rides on top of the transfer here
    sep #$20
    bcc A26FastFire
    dec.b $13
    beq A26FastSegStop
    rep #$20
    lda.b $30
    sec
    sbc.b $34
    sta.b $30
    sep #$20
    bra A26FastTrim
A26FastFire:
    jmp A26FireGo       ; shared with the slow path: addresses, length, trigger,
                        ; budget line, backlog clear, cursor advance
A26FastSegStop:
    lda.b #$01
    sta.b $2A           ; no room for even one row
A26FastSegNone:
    rts

; ===========================================================================
; DRAIN -- send as much of the backlog as the budget and the clock allow.
;
; Round-robin: the scan starts at the PERSISTENT cursor $10, never at row 0.
; A run has to be contiguous in row number for one DMA to cover it, and the
; wrap from the last row back to row 0 is not contiguous, so the round-robin
; walk is done as two straight segments -- [cursor..height-1] then
; [0..cursor-1] -- instead of modular arithmetic in the inner loop.
; ===========================================================================
A26Drain:
    rep #$20
    lda.b $08
    sta.b $1E           ; budget for this NMI
    sep #$20
    lda.b #$00
    sta.b $2A
    lda.b $10
    sta.b $2E           ; remember where the walk began

    lda.b $0C
    dec a
    sta.b $29           ; segment 1 = cursor .. height-1
    lda.b $2E
    sta.b $28
    jsr A26DrainSeg
    lda.b $2A
    bne A26DrainDone

    lda.b $2E
    beq A26DrainDone    ; the walk began at row 0: segment 1 covered everything
    dec a
    sta.b $29           ; segment 2 = 0 .. cursor-1
    lda.b #$00
    sta.b $28
    jsr A26DrainSeg
A26DrainDone:
    rts

; Scan rows $28..$29 inclusive.  $2A is set when the drain must stop for good.
A26DrainSeg:
    lda.b $28
    jsr A26RowBit
    stx.b $2B
    lda.b $3E
    sta.b $2D
A26SegScan:
    lda.b $28
    cmp.b $29
    beq A26SegTest
    bcs A26SegOut       ; walked past the end of the segment
A26SegTest:
    ldx.b $2B
    lda.b $20,x
    and.b $2D
    bne A26SegRun
    jsr A26SegNext
    bra A26SegScan
A26SegOut:
    rts

; --- build one run of contiguous pending rows -------------------------------
; The run is extended while the FULL-ROW cost of the extended run still fits
; the budget.  That is a safe bound for both options, because the hybrid choice
; below can only ever be cheaper than the full-row one.
A26SegRun:
    lda.b $28
    sta.b $12
    lda.b #$00
    sta.b $13
    rep #$20
    lda.w #$0000
    sta.b $30           ; bytes of the full-row option
    sta.b $32           ; sum of span tile counts
    sep #$20

    ; Ceiling for this run = min(budget left, scanlines left before the
    ; deadline).  Sizing by the budget alone builds runs that no longer fit the
    ; time left -- the guard then rejects them and NOTHING is ever sent: a
    ; 256-wide full frame folds 24 rows, the fold itself eats ~6 lines, and the
    ; 17-row run the budget allows no longer fits in what is left of the
    ; window.  Deadlock, at exactly the workload the carry-over exists for.
    jsr A26Capacity
    cmp.b $1E
    bcc A26SegCeil
    lda.b $1E
A26SegCeil:
    sta.b $78
    sep #$20

A26SegRunAdd:
    rep #$20
    lda.b $30
    clc
    adc.b $34
    clc
    adc.w #!A26_DMACOST
    cmp.b $78
    sep #$20
    bcs A26SegRunEnd    ; one more full row would not fit

    rep #$20
    lda.b $30
    clc
    adc.b $34
    sta.b $30
    lda.b $28
    and.w #$00FF
    asl a
    tax
    sep #$20
    lda.b $41,x
    sec
    sbc.b $40,x
    inc a               ; tiles in this row's span
    rep #$20
    and.w #$00FF
    clc
    adc.b $32
    sta.b $32
    sep #$20
    inc.b $13

    lda.b $28
    cmp.b $29
    beq A26SegRunEnd    ; the segment ends here, so the run does too
    jsr A26SegNext
    ldx.b $2B
    lda.b $20,x
    and.b $2D
    bne A26SegRunAdd

A26SegRunEnd:
    lda.b $13
    beq A26SegStop      ; not even one row fits: stop the whole drain
    jsr A26SegTrim      ; building the run also burned scanlines
    lda.b $13
    beq A26SegStop
    jsr A26SegFire
    lda.b $2A
    bne A26SegOut2
    jmp A26SegScan      ; too far for a relative branch
A26SegStop:
    lda.b #$01
    sta.b $2A
A26SegOut2:
    rts

; The run was sized against the capacity as it was BEFORE the build loop, and
; the build loop itself costs scanlines -- roughly 35 cycles a row, which on a
; 24-row run is over four lines.  Rather than let the guard reject the finished
; run (which would send nothing at all, frame after frame, at exactly the
; workload the carry-over exists for), shrink it to what the clock still allows
; and send that.  The rows dropped here stay pending and the cursor lands on
; the first of them, so the next NMI resumes exactly where this one stopped.
A26SegTrim:
    jsr A26Capacity
    sec
    sbc.w #!A26_FIRESLACK
    bcs A26TrimCeil
    lda.w #$0000
A26TrimCeil:
    sta.b $78
    ; X = span index of the LAST row of the run; rows are dropped off that end,
    ; so the span sum can be unwound row by row along with the byte count.
    lda.b $12
    and.w #$00FF
    sta.b $3C
    lda.b $13
    and.w #$00FF
    clc
    adc.b $3C
    dec a
    asl a
    tax
    sep #$20
A26TrimLoop:
    rep #$20
    lda.b $30
    clc
    adc.w #!A26_DMASTART ; clock question, not the budget one
    cmp.b $78
    sep #$20
    bcc A26TrimDone     ; what is left of the run fits
    dec.b $13
    beq A26TrimDone     ; nothing of it fits at all
    rep #$20
    lda.b $30
    sec
    sbc.b $34
    sta.b $30
    sep #$20
    lda.b $41,x
    sec
    sbc.b $40,x
    inc a
    rep #$20
    and.w #$00FF
    sta.b $3C
    lda.b $32
    sec
    sbc.b $3C
    sta.b $32           ; keep the span sum honest for the hybrid decision
    sep #$20
    dex
    dex
    bra A26TrimLoop
A26TrimDone:
    rts

; advance the scan cursor one row (row, mask, bitmap byte index)
A26SegNext:
    inc.b $28
    asl.b $2D
    bne A26SegNextDone
    lda.b #$01
    sta.b $2D
    inc.b $2B
A26SegNextDone:
    rts

; --- HYBRID: one DMA over the whole run, or one span DMA per row ------------
; A single DMA can only move a contiguous byte range, and the bitmap is
; row-major, so covering columns c1..c2 of several rows is impossible in one
; go.  The two candidates are therefore: the full byte range of the run
; (1 DMA, all columns), or each row's span (n DMAs, only the changed columns).
; Contract sec. 3 says take the cheaper.
A26SegFire:
    rep #$20
    lda.b $13
    and.w #$00FF
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a               ; n * 128
    sta.b $36
    asl a               ; n * 256
    clc
    adc.b $36           ; n * !A26_DMACOST (384 = 256 + 128)
    sta.b $36
    lda.b $32
!i = 0
while !i < !A26_TILE_BSH
    asl a
!i #= !i+1
endwhile
    clc
    adc.b $36
    sta.b $36           ; cost of n span DMAs
    lda.b $30
    clc
    adc.w #!A26_DMACOST
    sta.b $38           ; cost of one full-row DMA over the whole run
    cmp.b $36
    sep #$20
    bcc A26FireFull
    beq A26FireFull
    jmp A26FireSpans

; --- one DMA, every column of every row in the run --------------------------
A26FireFull:
    rep #$20
    lda.b $30
    sta.b $1C
    sep #$20
A26FireTry:
    jsr A26DmaGuard
    bcs A26FireGo
    ; Still short: give back one row and ask again.  The trim above normally
    ; makes this a no-op, but it is what guarantees the drain can never talk
    ; itself into sending nothing at all -- each pass frees a whole row of
    ; capacity and costs only the guard.
    dec.b $13
    beq A26FireNoTime
    rep #$20
    lda.b $30
    sec
    sbc.b $34
    sta.b $30
    sta.b $1C
    sep #$20
    bra A26FireTry

A26FireGo:
    lda.b $12
    jsr A26RowTile      ; A(16) = first tile of the run
    jsr A26SetTileAddr
    rep #$20
    lda.b $30
    sta.w $4365         ; $4305-style length, rewritten for EVERY DMA: this
                        ; register counts DOWN during the transfer, and reusing
                        ; a stale value is the documented 64 KB-DMA bug
    lda.b $1E
    sec
    sbc.b $30
    sec
    sbc.w #!A26_DMACOST ; the budget is debited what the DMA really costs the
    sta.b $1E           ; frame, not just the engine's start-up
    sep #$20
    lda.b #$40
    sta.w $420B         ; fire ch6

    jsr A26ClearRun
    lda.b $12
    clc
    adc.b $13
    jsr A26SetCursor
    rts

A26FireNoTime:
    lda.b #$01
    sta.b $2A           ; out of scanlines: everything left stays pending
    rts

; --- one DMA per row, covering only that row's span -------------------------
; X is held at row*2 and $3C at row*cols for the whole loop -- neither the
; guard nor A26SetTileAddr touches X, so the per-row index and the per-row base
; tile are advanced by an add instead of being recomputed (the multiply and the
; second index calculation were the two most expensive things in here).
A26FireSpans:
    lda.b $12
    sta.b $3A           ; current row
    lda.b $13
    sta.b $3B           ; rows left in the run
    lda.b $12
    jsr A26RowTile      ; A(16) = row * cols, once for the whole run
    rep #$20
    sta.b $3C
    lda.b $3A
    and.w #$00FF
    asl a
    tax                 ; X = row * 2, the span index
    sep #$20

A26SpanLoop:
    lda.b $41,x
    sec
    sbc.b $40,x
    inc a               ; tiles in this row's span
    bne A26SpanCount    ; belt AND braces with the fold's sanitiser: a count of
    lda.b $05           ; 256 wraps to 0 here and 0 in $4365 is a 65536-byte
A26SpanCount:           ; DMA.  Four bytes, on the one path that could wipe
                        ; VRAM, is cheaper than trusting an invariant.
    rep #$20
    and.w #$00FF
!i = 0
while !i < !A26_TILE_BSH
    asl a
!i #= !i+1
endwhile
    sta.b $1C           ; bytes of this row's span
    sep #$20
    jsr A26DmaGuard
    bcc A26SpanStop

    lda.b $40,x
    rep #$20
    and.w #$00FF
    clc
    adc.b $3C           ; absolute tile = row base + the span's first tile
    jsr A26SetTileAddr  ; (A stays 16 bits into the call -- it takes a tile)
    rep #$20
    lda.b $1C
    sta.w $4365         ; length, rewritten for EVERY DMA (see A26FireFull)
    lda.b $1E
    sec
    sbc.b $1C
    sec
    sbc.w #!A26_DMACOST
    sta.b $1E
    sep #$20
    lda.b #$40
    sta.w $420B         ; fire ch6

    rep #$20
    lda.b $3C
    clc
    adc.b $14
    sta.b $3C           ; next row's base tile
    sep #$20
    inx
    inx                 ; next row's span entry
    inc.b $3A
    dec.b $3B
    bne A26SpanLoop

    ; every row of the run went out.  The backlog bits and the cursor are
    ; settled here rather than per row: both cost ~110 cycles a row inside the
    ; loop and nothing between the DMAs depends on them.
    jsr A26ClearRun
    lda.b $3A
    jsr A26SetCursor
    rts

; Out of scanlines part-way through a run: $3A is the first row that did NOT go
; out, so it is both the count of what did and the right place to resume.
A26SpanStop:
    lda.b $3A
    sec
    sbc.b $12
    beq A26SpanStopNone
    sta.b $13
    jsr A26ClearRun
A26SpanStopNone:
    lda.b $3A
    jsr A26SetCursor
    lda.b #$01
    sta.b $2A
    rts

; A(8) = row -> A(16) = row * cols, the first tile of that row
A26RowTile:
    sta.w $4202
    lda.b $05
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.w $4216
    rts

; A(16) = absolute tile index -> VRAM word address + DMA source offset.
; Both derive from the same tile number so a patched row can never land on a
; different byte than the tilemap points at.  Enter with A 16-bit, leave 8-bit.
A26SetTileAddr:
    pha
!i = 0
while !i < !A26_TILE_WSH
    asl a
!i #= !i+1
endwhile
    sta.w $2116         ; VMADD counts WORDS
    pla
!i = 0
while !i < !A26_TILE_BSH
    asl a
!i #= !i+1
endwhile
if !A26_HARNESS == 1
    clc                 ; the set is not at offset 0 of its bank here
    adc.w #!TILES_A16
endif
    sta.w $4362         ; A1T counts bytes
    sep #$20
    rts

; Drop rows $12..$12+$13-1 from the backlog.  The rows are contiguous, so the
; mask is rolled instead of A26RowBit being called per row -- that call is ~90
; cycles and this loop runs once per row of a 24-row run.
A26ClearRun:
    ; Whole frame in one block -- the fast path's steady state -- is just a
    ; zeroed bitmap, and skipping the 24-iteration walk there is worth the
    ; four instructions it costs to notice.
    lda.b $12
    bne A26ClearPart
    lda.b $13
    cmp.b $0C
    bne A26ClearPart
    rep #$20
    lda.w #$0000
    sta.b $20
    sta.b $22
    sep #$20
    rts
A26ClearPart:
    lda.b $12
    jsr A26RowBit       ; X = bitmap byte index, $3E = mask, once
    lda.b $13
    sta.b $3B
A26ClearLoop:
    lda.b $3E
    eor.b #$FF
    and.b $20,x
    sta.b $20,x
    dec.b $3B
    beq A26ClearDone
    asl.b $3E
    bne A26ClearLoop
    lda.b #$01
    sta.b $3E
    inx
    bra A26ClearLoop
A26ClearDone:
    rts

; A(8) = row that should be drained first next time
A26SetCursor:
    cmp.b $0C
    bcc A26SetCursorOk
    lda.b #$00
A26SetCursorOk:
    sta.b $10
    rts

; A(8) = row -> X = index of its bitmap byte, $3E = its bit mask.  ($3E and not
; $2D: the callers run inside loops that are still using $2D.)
A26RowBit:
    rep #$20
    and.w #$00FF
    pha
    and.w #$0007
    tax
    sep #$20
    lda.l A26BitMask,x
    sta.b $3E
    rep #$20
    pla
    lsr a
    lsr a
    lsr a
    tax
    sep #$20
    rts

A26BitMask:
    db $01,$02,$04,$08,$10,$20,$40,$80

; ===========================================================================
; Overrun guard (contract sec. 4).
;
; $1C = bytes the next DMA would move.  Returns carry set when it can still
; finish before line 17, carry clear when it cannot -- in which case the caller
; leaves the rows pending and stops.  Overrunning is not a glitch: once ch5 has
; handed the screen back on line 17, VRAM writes are dropped silently and the
; picture corrupts.
;
; Capacity is measured in the same byte-equivalents the budget uses, but
; re-anchored on the live scanline: 170 B for every whole line still left in
; vblank, plus 146 B for each of the 17 lines over V=0..16, where the six HDMA
; channels take ~194 mc out of every line.
; ===========================================================================
A26DmaGuard:
    jsr A26Capacity
    sta.b $1A
    lda.b $1C
    clc
    adc.w #!A26_DMASTART ; clock question: the player-side overhead around this
    cmp.b $1A            ; transfer is already inside !A26_GUARDSLACK
    sep #$20
    bcs A26GuardNo
    sec
    rts
A26GuardNo:
    clc
    rts

; Byte-equivalents that still fit before the deadline, from the LIVE scanline.
; Enter with A 8-bit, leave with A 16-bit holding the capacity (0 = past it).
; Re-reading V per DMA is what keeps the estimate honest: the player's own
; per-DMA overhead is not in this arithmetic, so an error would otherwise
; accumulate across a span-heavy frame.
A26Capacity:
    jsr A26VCount       ; A(16) = V
    cmp.w #225
    bcs A26CapVbl
    cmp.w #!A26_DEADLINE
    bcs A26CapNone      ; between the deadline and the next vblank: too late
    sta.b $1A
    lda.w #!A26_DEADLINE
    sec
    sbc.b $1A           ; whole blanked lines left
    sep #$20
    sta.w $4202
    lda.b #!A26_BPLHDMA
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.w $4216
    bra A26CapTrim
A26CapVbl:
    sta.b $1A
    lda.w #262
    sec
    sbc.b $1A           ; whole vblank lines left
    bcs A26CapVblOk     ; V past 261 -- a PAL console (312 lines), overscan, or
    lda.w #$0000        ; a stale H/V latch.  The subtract borrowed, and only
A26CapVblOk:            ; the LOW BYTE reaches $4202: 262-263 = $FFFF would be
                        ; multiplied as 255 lines and hand the guard a capacity
                        ; of ~45000, i.e. a blank cheque to overrun the screen.
                        ; Claim zero vblank lines instead; the 17-line tail
                        ; below still lets small transfers through.
    sep #$20
    sta.w $4202
    lda.b #!A26_BPLVBL
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.w $4216
    clc
    adc.w #!A26_TAILBYTES
    bra A26CapTrim
A26CapNone:
    lda.w #$0000
A26CapTrim:
    sec
    sbc.w #!A26_GUARDSLACK
    bcs A26CapDone
    lda.w #$0000        ; borrowed: nothing left at all
A26CapDone:
    rts

; Current scanline -> A (16 bits).  $2137 latches H/V; $213D returns the low
; byte on the first read and bit 8 on the second, so $213F is read first to put
; that toggle in a known state.
A26VCount:
    sep #$20
    lda.w $213F
    lda.w $2137
    lda.w $213D
    sta.b $1A
    lda.w $213D
    and.b #$01
    sta.b $1B
    rep #$20
    lda.b $1A
    rts

; ===========================================================================
; Console switches -> SWCHB shadow at $EF0004 (contract sec. 6).
;
; The polarity is deliberately not uniform: RESET/SELECT are active LOW levels,
; COLOR and the two difficulty switches are latching toggles driven by button
; EDGES.  Directions and fire are not our business -- the RTL derives SWCHA and
; INPT4/5 straight from the raw pads we forwarded above.  Console switches come
; from pad 1 only.
;
; Combo guard: while L+R are both held on pad 1, no edge is taken and RESET /
; SELECT read as released, so the IGR combos ($3030 = L+R+Start+Select,
; $2070 = L+R+Select+X) cannot also poke the game.
; ===========================================================================
A26Switches:
    lda.b $0E
    and.b #$30          ; L | R
    cmp.b #$30
    beq A26SwIdle

    lda.b $0E
    eor.b $07
    and.b $0E           ; bits that went released -> pressed this frame
    sta.b $17

    lda.b $17
    and.b #$40          ; X -> COLOR / B&W (D3)
    beq A26SwNoX
    lda.b $06
    eor.b #$08
    sta.b $06
A26SwNoX:
    lda.b $17
    and.b #$20          ; L -> player 0 difficulty (D6)
    beq A26SwNoL
    lda.b $06
    eor.b #$40
    sta.b $06
A26SwNoL:
    lda.b $17
    and.b #$10          ; R -> player 1 difficulty (D7)
    beq A26SwNoR
    lda.b $06
    eor.b #$80
    sta.b $06
A26SwNoR:

    lda.b $06
    sta.b $18
    lda.b $0F
    and.b #$10          ; Start held -> RESET pressed (D0 = 0)
    beq A26SwNoReset
    lda.b $18
    and.b #$FE
    sta.b $18
A26SwNoReset:
    lda.b $0F
    and.b #$20          ; Select held -> SELECT pressed (D1 = 0)
    beq A26SwNoSelect
    lda.b $18
    and.b #$FD
    sta.b $18
A26SwNoSelect:
    bra A26SwOut

A26SwIdle:
    lda.b $06
    sta.b $18           ; switches frozen, RESET/SELECT released

A26SwOut:
    lda.b $18
    sta.l !A26_EF+$04       ; 8-bit store ONLY: $EF0005 next door is the swap strobe
    lda.b $0E
    sta.b $07           ; edge reference for the next frame -- updated on the
                        ; guarded path too, so releasing L+R is not an edge
    rts

; ===========================================================================
; IGR pad combos (chassis parity).  This core keeps cheat.v, but the NMI hook
; that feeds its combo detector never runs under the player, so the match lives
; here.  Exact 16-bit compare of {JOY1H,JOY1L}, same values as cheat.v's case():
;   $3030 = L+R+Start+Select -> $80 (reset the game)
;   $2070 = L+R+Select+X     -> $81 (back to the menu)
; The combo guard above keeps those five buttons out of the Atari side while
; they are held, so a combo cannot also press RESET/SELECT on the console.
; The command byte goes straight to the fork's MCU_CMD mailbox ($2A00, snescmd
; BRAM -- write window opened for this core in main.v) and the MCU game loop
; serves it.  DP $04 = edge latch: fire only on release->press (armed at Reset).
; ===========================================================================
A26Igr:
    rep #$20
    lda.b $0E           ; {JOY1H,JOY1L} as one word, exactly what we forwarded
    cmp.w #$3030
    beq A26IgrReset
    cmp.w #$2070
    beq A26IgrMenu
    sep #$20
    stz.b $04           ; no combo held -> re-arm the edge
    rts
A26IgrReset:
    sep #$20
    lda.b #$80
    bra A26IgrFire
A26IgrMenu:
    sep #$20
    lda.b #$81
A26IgrFire:
    sta.b $19           ; park the command
    lda.b $04
    bne A26IgrDone      ; already fired during this hold
    lda.b #$01
    sta.b $04
    jsr A26ExitHygiene  ; hand the $21xx block back the way the iris gave it
    lda.b $19
    sta.l $002A00       ; MCU_CMD
A26IgrDone:
    rts

; Leaving for the menu / a game reset: the menu inherits this $21xx block, so
; put it back the way iris_out is expected to hand it over -- forced blank, no
; HDMA, no NMI, nothing armed (contract sec. 6).
A26ExitHygiene:
    lda.b #$8F
    sta.w $2100
    stz.w $420C
    stz.w $4200
    ; fall through

; ===========================================================================
; Pillarbox for the 160-wide mode (black side bars).
;
; The bars cannot come from the tilemap.  The empty tile is pixel 0, and pixel
; 0 is the backdrop -- which on this core is CGRAM entry 0, i.e. the line's
; COLUBK, rewritten every scanline by HDMA ch1.  So the bars faithfully show
; the game's background colour and the picture appears to spill past the real
; 160-pixel frame (green either side of Pitfall).  Masking BG1 with a window
; does not help either: clipping the layer just exposes that same backdrop.
;
; The cut has to happen after the layers are composed, which is what the colour
; window does (book1 p.130, 2-27-16, CGSWSEL):
;   $2126/$2127  window 1 spans x = 48..207, the 160 content columns
;   $2125 = $20  colour window: W1 ENABLE (D5), IN/OUT (D4) = 0 = "IN", so the
;                colour window area IS the inside of window 1
;                (book1 p.128, 2-27-11: D5/D4 are the colour window's
;                 W1_EN / IN-OUT pair, and IN/OUT 0 = IN)
;   $212B = $00  colour window mask logic OR (only W1 participates)
;   $2130 = $40  MAIN SW (D7:D6) = 01 = "ON (Inside window only)" -- the main
;                screen is displayed inside the window and forced BLACK outside
;                it, whatever layer or backdrop would have been there
;   $2131 = $00  no colour math anywhere; the clip is independent of it
; 256-wide fills the screen, so it arms nothing and A26PpuDisarm's zeros stand.
; ===========================================================================
A26ArmPillar:
    lda.b $05
    cmp.b #!A26_COLS256
    beq A26ArmPillarNone
    lda.b #!A26_PILLAR_L
    sta.w $2126
    lda.b #!A26_PILLAR_R
    sta.w $2127
    lda.b #$20
    sta.w $2125
    stz.w $212B
    lda.b #$40
    sta.w $2130
    stz.w $2131
A26ArmPillarNone:
    rts

; Disarm everything in $21xx this player does not use.  The console reset does
; not clear the block, so a stale colour window or a stale $2133 from the menu
; would land here as a broken picture -- and on the way out the menu would
; inherit whatever we left.
A26PpuDisarm:
    stz.w $2106         ; mosaic off (console reset does NOT clear $21xx; an
                        ; inherited mosaic deforms the whole picture)
    stz.w $2123         ; window mask settings BG1/BG2
    stz.w $2124         ; BG3/BG4
    stz.w $2125         ; OBJ/backdrop
    stz.w $2126         ; window 1 left
    stz.w $2127         ; window 1 right
    stz.w $2128         ; window 2 left
    stz.w $2129         ; window 2 right
    stz.w $212A         ; window mask logic BG
    stz.w $212B         ; window mask logic OBJ/backdrop
    stz.w $212E         ; window area main screen
    stz.w $212F         ; window area subscreen
    stz.w $2130         ; colour math control A
    stz.w $2131         ; colour math control B
    lda.b #$E0
    sta.w $2132         ; fixed colour = black (B/G/R selected, value 0)
    stz.w $2133         ; no pseudo-hires, no interlace, no overscan
    rts

; ===========================================================================
; Boot helpers
; ===========================================================================

; Wait one full frame using the vblank flag (NMI is still off up here).
A26FrameWait:
A26FrameWaitOut:
    lda.w $4212
    bmi A26FrameWaitOut ; leave the vblank we may be sitting in
A26FrameWaitIn:
    lda.w $4212
    bpl A26FrameWaitIn  ; and wait for the next one to start
    rts

; Zero all 64 KB of VRAM: the empty tile, and every tile the core has not
; painted yet, must read back as pixel 0 (= transparent = backdrop).  Fixed
; A-bus source, ch7 (ch0-5 are the HDMA's).  ~24 ms, all of it in forced blank.
A26ClearVram:
    lda.b #$80
    sta.w $2115         ; VMAIN: +1 word after $2119
    ldx.w #$0000
    stx.w $2116
    lda.b #$09
    sta.w $4370         ; DMAP ch7: A->B, FIXED source, mode 1 ($2118/$2119)
    lda.b #$18
    sta.w $4371
    ldx.w #A26Zero
    stx.w $4372
    lda.b #$00
    sta.w $4374         ; this ROM lives in bank $00
    ldx.w #$0000
    stx.w $4375         ; 0 = 65536 bytes
    lda.b #$80
    sta.w $420B         ; fire ch7
    rts

A26Zero:
    dw $0000

; Build the static tilemap.  Content occupies tile-rows !A26_ROW0..+height-1,
; i.e. scanlines 17..(16+height*8), matching the HDMA letterbox: entry i drives
; line i+1, so entries 0-15 = lines 1-16 blank, 16-207 = content, 208-223 blank.
; 160-wide is pillarboxed into columns 6..25; those bars are pixel 0, i.e. the
; line's COLUBK, which is what the contract specifies.  Nothing rewrites this
; map afterwards -- only tile DATA moves per frame.
A26BuildMap:
    rep #$30
    lda.w #!BG1_MAP
    sta.w $2116
    lda.b $05
    and.w #$00FF
    sta.b $3C           ; cols
    lda.w #$0020
    sec
    sbc.b $3C
    lsr a
    sta.b $3A           ; left margin = (32 - cols) / 2 (also the right one)
    lda.b $0C
    and.w #$00FF
    clc
    adc.w #!A26_ROW0
    sta.b $38           ; first tilemap row past the content
    lda.w #$0000
    sta.b $36           ; running tile number
    ldy.w #$0000
A26MapRow:
    cpy.w #!A26_ROW0
    bcc A26MapEmptyRow
    cpy.b $38
    bcs A26MapEmptyRow

    ldx.b $3A
    beq A26MapNoLeft
A26MapLeft:
    lda.w #!A26_EMPTY_TILE
    sta.w $2118
    dex
    bne A26MapLeft
A26MapNoLeft:
    ldx.b $3C
A26MapCells:
    lda.b $36
    sta.w $2118         ; palette 0, priority 0 -> the entry IS the tile number
    inc.b $36
    dex
    bne A26MapCells
    ldx.b $3A
    beq A26MapRowDone
A26MapRight:
    lda.w #!A26_EMPTY_TILE
    sta.w $2118
    dex
    bne A26MapRight
    bra A26MapRowDone

A26MapEmptyRow:
    ldx.w #$0020
A26MapEmptyCell:
    lda.w #!A26_EMPTY_TILE
    sta.w $2118
    dex
    bne A26MapEmptyCell

A26MapRowDone:
    iny
    cpy.w #$0020
    bne A26MapRow
    sep #$20

    ; ch6 is the transfer channel for the rest of the run; its constant fields
    ; never change, so program them once here.  ch0-5 belong to the HDMA:
    ; writing $43x0-$43x4 of a live HDMA channel breaks it silently.
    lda.b #$80
    sta.w $2115         ; VMAIN: +1 word after $2119
    lda.b #$01
    sta.w $4360         ; DMAP ch6: A->B, increment, mode 1 ($2118/$2119)
    lda.b #$18
    sta.w $4361         ; BBAD = $2118
    lda.b #!TILES_A1B
    sta.w $4364         ; A1B = $E0 (front set, redirected by address.v)
    rts

; ===========================================================================
; HDMA setup -- programmed ONCE, here, and never touched again.
;
; The tables live at fixed offsets in bank $E2 and the core rewrites their
; CONTENTS every frame; address.v redirects the bank to whichever set is front,
; so the channels never need reprogramming.  Every table is 224 entries of
; "$01 (one line, no repeat) + data" plus a $00 terminator, and all six share
; that shape on purpose: an unequal one would desynchronise CGADD and shift the
; colours of the rest of the frame.
;
;   ch0  mode 0 -> $2121  +$0400  CGADD = 0 (also resets the $2122 low/high
;                                 flip-flop, so it self-heals every line)
;   ch1  mode 2 -> $2122  +$0600  colour 0 = COLUBK of the line (the backdrop,
;                                 and therefore the 160-wide side bars)
;   ch2  mode 2 -> $2122  +$0900  colour 1 = COLUPF
;   ch3  mode 2 -> $2122  +$0C00  colour 2 = COLUP0
;   ch4  mode 2 -> $2122  +$0F00  colour 3 = COLUP1
;   ch5  mode 0 -> $2100  +$1200  forced blank: $80 lines 1-16 and 209-224,
;                                 $0F line 17
;
; 10 HDMA bytes per line, ~194 mc, about 57% of the hblank+border window.
; Sizes: ch0/ch5 = 224*2+1 = 449 B (slots of $200); ch1-4 = 224*3+1 = 673 B
; (slots of $300).  Last table ends at $0F00+673 = $11A1, ch5 at $1200+449 =
; $13C1 -- both inside the $0400..$13FF region.
; ===========================================================================
A26HdmaSetup:
if !A26_FORMAT == 0
    lda.b #$00
    sta.w $4300         ; ch0 DMAP: mode 0, A->B, direct table
    lda.b #$21
    sta.w $4301         ; BBAD = $2121 (CGADD)
    ldx.w #(!META_A16+!A26_HDMA+$0000)
    stx.w $4302
    lda.b #!META_A1B
    sta.w $4304

    lda.b #$02
    sta.w $4310         ; ch1 DMAP: mode 2 (write $2122 twice)
    lda.b #$22
    sta.w $4311
    ldx.w #(!META_A16+!A26_HDMA+$0200)
    stx.w $4312
    lda.b #!META_A1B
    sta.w $4314

    lda.b #$02
    sta.w $4320         ; ch2
    lda.b #$22
    sta.w $4321
    ldx.w #(!META_A16+!A26_HDMA+$0500)
    stx.w $4322
    lda.b #!META_A1B
    sta.w $4324

    lda.b #$02
    sta.w $4330         ; ch3
    lda.b #$22
    sta.w $4331
    ldx.w #(!META_A16+!A26_HDMA+$0800)
    stx.w $4332
    lda.b #!META_A1B
    sta.w $4334

    lda.b #$02
    sta.w $4340         ; ch4
    lda.b #$22
    sta.w $4341
    ldx.w #(!META_A16+!A26_HDMA+$0B00)
    stx.w $4342
    lda.b #!META_A1B
    sta.w $4344
endif

    lda.b #$00
    sta.w $4350         ; ch5 DMAP: mode 0
    lda.b #$00
    sta.w $4351         ; BBAD = $2100 (INIDISP)
    ldx.w #(!META_A16+!A26_HDMA+$0E00)
    stx.w $4352
    lda.b #!META_A1B
    sta.w $4354
    rts

if !A26_FORMAT == 1
; ---------------------------------------------------------------------------
; 8bpp reserve: CGRAM is static, so it is uploaded once instead of being driven
; per line.  128 entries = the Atari colour byte (4 bits hue, 3 bits luma; D0
; is ignored by the TIA), index = hue*8 + luma.
; ---------------------------------------------------------------------------
A26LoadPalette:
    stz.w $2121         ; CGADD = 0
    lda.b #$00
    sta.w $4370         ; DMAP ch7: mode 0, A->B, increment
    lda.b #$22
    sta.w $4371         ; BBAD = $2122
    ldx.w #A26Palette
    stx.w $4372
    lda.b #$00
    sta.w $4374         ; bank $00
    ldx.w #$0100
    stx.w $4375         ; 128 entries * 2 B
    lda.b #$80
    sta.w $420B
    rts

; MIRROR, not a source.  The canonical palette is atari/tests/a26sim/palette.py
; (contract sec. 4), which also emits the core's a26_palette.vh; re-dump this
; table from it rather than editing the numbers here, because a reserve format
; that disagrees with the live one about colour is worse than no reserve.
; These values follow the classic NTSC composite model, reproducible from this
; comment alone -- linear luma ramp, same deliberate deviation the contract
; records for the canonical table:
;   Y = 0.0625 + (luma/7) * 0.9375
;   hue 0            -> R = G = B = Y (greyscale column)
;   hue h (1..15)    -> phase = 26.2 - (h-1) * 25.7 degrees, saturation 0.30
;                       I = S*cos(phase), Q = S*sin(phase)
;                       R = Y + 0.956I + 0.621Q
;                       G = Y - 0.272I - 0.647Q
;                       B = Y - 1.106I + 1.703Q
;   each channel clamped to [0,1] and rounded to 5 bits, word = B<<10|G<<5|R.
; Hue $F landing next to hue $1 is the real wrap of the subcarrier, not a bug.
A26Palette:
    dw $0842,$18C6,$294A,$39CE,$4E73,$5EF7,$6F7B,$7FFF   ; hue $0
    dw $000C,$1031,$20B5,$3139,$41DD,$525F,$66DF,$775F   ; hue $1
    dw $000B,$008F,$0113,$1197,$221B,$369F,$471F,$579F   ; hue $2
    dw $0048,$00CC,$0170,$01F4,$0A78,$1EFC,$2F7F,$3FFF   ; hue $3
    dw $00A3,$0127,$01AB,$0230,$02D4,$1358,$23DC,$33FF   ; hue $4
    dw $00E0,$0163,$0207,$028B,$070F,$1793,$27F7,$37FB   ; hue $5
    dw $0100,$01A0,$0223,$06A7,$172B,$27AF,$3BF3,$4BF7   ; hue $6
    dw $0100,$0180,$1220,$22A4,$3329,$47AD,$57F1,$67F5   ; hue $7
    dw $10E0,$2160,$31E0,$4664,$56E8,$678C,$77F0,$7FF4   ; hue $8
    dw $30A0,$4120,$51A1,$6625,$76AA,$7F2E,$7FB2,$7FF6   ; hue $9
    dw $4840,$58C0,$6945,$79C9,$7E4D,$7ED1,$7F75,$7FF9   ; hue $A
    dw $5401,$6465,$74E9,$7D6D,$7DF1,$7E96,$7F1A,$7F9E   ; hue $B
    dw $5005,$602A,$70AE,$7D32,$7DB6,$7E3A,$7EDE,$7F5F   ; hue $C
    dw $3C09,$4C0E,$6092,$7116,$7D9A,$7E1E,$7E9F,$7F3F   ; hue $D
    dw $200C,$3010,$4094,$5518,$659D,$761F,$7EBF,$7F3F   ; hue $E
    dw $000C,$1031,$20B5,$3139,$41DD,$565F,$66DF,$775F   ; hue $F
endif


; ===========================================================================
; S-DSP unmute -- opens the console's cartridge-DAC audio path.
;
; WHY: with no program uploaded to the S-SMP, the S-DSP stays in its IPL
; reset/mute state, and in that state the console's analogue path GATES the
; cartridge DAC output -- the FPGA can drive I2S perfectly and nothing reaches
; the jack.  Same mechanic, same cure, same 82-byte payload as snes/sfx.a65
; (menu), snes/nes/nes_apu.a65 (NES renderer) and snes/sms/sms_snes.asm.
;
; ENTRY CONTRACT (true at the call site in Reset): native mode, DBR=$00,
; DP=$0000, NMI off ($4200=0 since the reset), A 8-bit / X,Y 16-bit.  Returns
; carry clear = stub running, carry set = timed out (boot continues, at worst
; silent).
; ===========================================================================

!A26_APU_BEEP = 0              ; 1 = instrumented payload (209 bytes): the same
                               ;     unmute plus a 0.5 s 2 kHz beep at boot, so
                               ;     "no sound" can be split into "the S-DSP
                               ;     gate is still shut" (no beep) and "the gate
                               ;     is open, the TIA side is the problem"
                               ;     (beep, then silence).  BRING-UP ONLY.
                               ; 0 = PRODUCTION: 82 bytes, unmute only, silent.
                               ;     Set this back to 0 before shipping.

!APUIO0 = $2140
!APUIO1 = $2141
!APUIO2 = $2142

if !A26_APU_BEEP == 1
!A26_APU_STUB_ADDR = $0300     ; upload base: DIR table + BRR + code
!A26_APU_EXEC_ADDR = $0320     ; code entry (DIR must own $0300)
!A26_APU_STUB_LEN  = $00D1     ; 209
else
!A26_APU_STUB_ADDR = $0300
!A26_APU_EXEC_ADDR = $0300
!A26_APU_STUB_LEN  = $0052     ; 82
endif

!A26_APU_WAIT_POLLS  = $0800   ; per-wait ceiling
!A26_APU_POLL_BUDGET = $FFFF   ; aggregate ceiling across the whole routine

A26ApuUnmute:
    sep #$20
    rep #$10
    ldy.w #!A26_APU_POLL_BUDGET

    ; --- 1) wait for the IPL to publish $BBAA ---------------------------------
    ; own ceiling ~5x the handshake one: the IPL spends ~2.4 ms clearing page
    ; zero before it answers, and a timeout here is indistinguishable from
    ; "the analogue gate was not the cause".
    ldx.w #$4000
A26ApuHello:
    lda !APUIO0
    cmp.b #$aa
    bne A26ApuHelloNext
    lda !APUIO1
    cmp.b #$bb
    beq A26ApuHelloOk
A26ApuHelloNext:
    dey
    beq A26ApuFail
    dex
    bne A26ApuHello
    bra A26ApuFail
A26ApuHelloOk:

    ; --- 2) open the transfer -------------------------------------------------
    ldx.w #!A26_APU_STUB_ADDR
    stx !APUIO2                 ; $2142/$2143 = destination
    lda.b #$cc
    sta !APUIO1                 ; non-zero starts the transfer
    sta !APUIO0
    jsr A26ApuWait
    bcs A26ApuFail

    ; --- 3) payload, byte by byte --------------------------------------------
    ; X is both the table index and the protocol index; the payload is < 256
    ; bytes so the index never wraps.
    ldx.w #$0000
A26ApuSend:
    lda.l A26ApuStub,x
    sta !APUIO1
    txa
    sta !APUIO0
    jsr A26ApuWait
    bcs A26ApuFail
    inx
    cpx.w #!A26_APU_STUB_LEN
    bne A26ApuSend

    ; --- 4) end of transfer: entry address + index+2 --------------------------
    ldx.w #!A26_APU_EXEC_ADDR
    stx !APUIO2
    stz !APUIO1                 ; 0 = execute, no further block
    lda !APUIO0
    inc a
    inc a
    sta !APUIO0
    jsr A26ApuWait
    bcs A26ApuFail
    clc
    rts

A26ApuFail:
    sec
    rts

; A(8) = expected echo, Y(16) = remaining global budget.
; carry clear = echoed, carry set = timed out.  A and X preserved.
A26ApuWait:
    phx
    ldx.w #!A26_APU_WAIT_POLLS
A26ApuWaitPoll:
    cmp !APUIO0
    beq A26ApuWaitOk
    dey
    beq A26ApuWaitTo
    dex
    bne A26ApuWaitPoll
A26ApuWaitTo:
    plx
    sec
    rts
A26ApuWaitOk:
    plx
    clc
    rts

; ===========================================================================
; SPC700 payload.  BYTE-IDENTICAL to the blobs already proven on hardware --
; do not "improve" it here; change snes/sfx.a65 and re-mirror.
;
; Order is the whole point: the DSP is kept MUTED while every voice and the
; whole echo path are scrubbed, MVOL is restored, and FLG's unmute is the LAST
; DSP write.  Unmuting first lets the power-on garbage in the voice registers
; out as continuous clicking (observed on the Mk.II).
; ===========================================================================
A26ApuStub:
if !A26_APU_BEEP == 1
; ---- instrumented: DIR table ($0300) + BRR ($0310) + code ($0320) ----------
; entry 0 = { start $0310, loop $0310 }; the BRR block is one looping block,
; header $c3 = shift 12 / filter 0 / loop+end, nibbles 7777777788888888 =
; a 16-sample square -> 32000/16 = 2000 Hz at pitch $1000.
    db $10,$03,$10,$03,$00,$00,$00,$00
    db $00,$00,$00,$00,$00,$00,$00,$00
    db $c3,$77,$77,$77,$77,$88,$88,$88
    db $88,$00,$00,$00,$00,$00,$00,$00
; ---- code (identical to the production 82 bytes up to the MVOL block, then
;      voice 0 setup, KON, ~0.5 s delay, KOFF, voice volumes back to 0) ------
    db $8f,$6c,$f2,$8f,$60,$f3,$8f,$5c
    db $f2,$8f,$ff,$f3,$8f,$4c,$f2,$8f
    db $00,$f3,$8f,$4d,$f2,$8f,$00,$f3
    db $8f,$2c,$f2,$8f,$00,$f3,$8f,$3c
    db $f2,$8f,$00,$f3,$8f,$0d,$f2,$8f
    db $00,$f3,$8d,$00,$e8,$00,$c4,$f2
    db $cb,$f3,$bc,$c4,$f2,$cb,$f3,$60
    db $88,$0f,$68,$80,$d0,$f0,$8f,$5c
    db $f2,$8f,$00,$f3,$8f,$5d,$f2,$8f
    db $03,$f3,$8f,$04,$f2,$8f,$00,$f3
    db $8f,$02,$f2,$8f,$00,$f3,$8f,$03
    db $f2,$8f,$10,$f3,$8f,$05,$f2,$8f
    db $00,$f3,$8f,$06,$f2,$8f,$00,$f3
    db $8f,$07,$f2,$8f,$7f,$f3,$8f,$00
    db $f2,$8f,$40,$f3,$8f,$01,$f2,$8f
    db $40,$f3,$8f,$0c,$f2,$8f,$7f,$f3
    db $8f,$1c,$f2,$8f,$7f,$f3,$8f,$6c
    db $f2,$8f,$20,$f3,$8f,$4c,$f2,$8f
    db $01,$f3,$cd,$00,$8d,$00,$00,$dc
    db $d0,$fc,$1d,$d0,$f7,$8f,$5c,$f2
    db $8f,$ff,$f3,$8f,$00,$f2,$8f,$00
    db $f3,$8f,$01,$f2,$8f,$00,$f3,$2f
    db $fe
else
; ---- production: 82 bytes, byte-identical to sfx_dsp_stub_code -------------
    db $8f,$6c,$f2,$8f,$60,$f3   ; FLG   = $60  soft-reset off, MUTE ON
    db $8f,$5c,$f2,$8f,$ff,$f3   ; KOFF  = $ff
    db $8f,$4c,$f2,$8f,$00,$f3   ; KON   = $00
    db $8f,$4d,$f2,$8f,$00,$f3   ; EON   = $00
    db $8f,$2c,$f2,$8f,$00,$f3   ; EVOL_L= $00
    db $8f,$3c,$f2,$8f,$00,$f3   ; EVOL_R= $00
    db $8f,$0d,$f2,$8f,$00,$f3   ; EFB   = $00
    db $8d,$00,$e8,$00           ; y=0, a=0
    db $c4,$f2,$cb,$f3,$bc       ; loop: VOL_L=0, next reg
    db $c4,$f2,$cb,$f3           ;       VOL_R=0
    db $60,$88,$0f,$68,$80,$d0,$f0 ;     a+=15, until a==$80
    db $8f,$0c,$f2,$8f,$7f,$f3   ; MVOL_L= $7f
    db $8f,$1c,$f2,$8f,$7f,$f3   ; MVOL_R= $7f
    db $8f,$6c,$f2,$8f,$20,$f3   ; FLG   = $20  MUTE OFF (last DSP write)
    db $2f,$fe                   ; bra *
endif


; ---- LoROM header (so smc_id detects LoROM) ----
org $00FFC0
    db "A26 PLAYER           "    ; 21-byte title
org $00FFD5
    db $20                        ; map mode: LoROM
    db $00                        ; cart type: ROM only
    db $08                        ; rom size
    db $00                        ; ram size: none
    db $00                        ; country
    db $00                        ; developer
    db $00                        ; version
    dw $0000                      ; checksum complement
    dw $FFFF                      ; checksum

; ---- vectors ----
org $00FFE4
    dw Stub
    dw Stub
    dw Stub
    dw NMI
    dw Stub
    dw Stub
org $00FFF4
    dw Stub
    dw Stub
    dw Stub
    dw Stub
    dw Reset
    dw Stub
