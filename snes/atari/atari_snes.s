; atari_snes.s -- Atari 2600 video + controls "player" for the sd2snes Mk.II (v19), the SNES half
; of the STorm2600 core (verilog/sd2snes_atari).  By AndinhoNFS; 65816, ca65.  SPDX-License-Identifier: GPL-2.0-or-later
;
; v17: v15 with the DMA diagnostic (L) only in test builds.  The public build (DIAGBLD = 0, the
;   default) ignores L completely.  A test build is made with:  ca65 -D DIAGBLD=1 ...
; v15: 232 picture lines (core Passo 34: TIA lines 27..258, 29 map rows, 233-entry tables, entry
;   232 = black).  Every TIA line stays on the same TV line as before: map row r is shown on
;   V = r + VOFF + 1 (v14: TIA 34 = row 0 on V = 9; v15: TIA 27 = row 0 on V = 2, so TIA 34 is
;   still on V = 9).  V = 1 is black, the picture is on V = 2..233 and the force blank at
;   V = 233 cuts the last row (TIA 258), as v14 cut its last row at V = 232.  The display is
;   switched on at V = 1 (a black line), so the time for the map and tile DMA stays 30 lines
;   (V = 233..261 and 0), as in v14.  HDMA: 1 black + 232 picture + 1 black entries, then stop.
;   DMA diagnostic: hold L to see "N### L###" = the highest tile count and the most lines
;   the DMA ran into the next frame (top lines late) since L was pressed.  L is not an Atari
;   input; it does nothing else.
;
; v14: "PAUSE" on screen for as long as the game is paused (R).  It is shown in the same place
;   and colour as the palette/level names, with no timeout, and is switched off in the very
;   frame where R resumes the game.  Pausing also cancels a palette/level name that was still
;   on screen.  Y (level) is ignored while paused, like X: the game is frozen and the name
;   would replace PAUSE.  BG2 now stays off while a new text row is waiting for its VRAM
;   upload, so the previous name can no longer flash for one frame before the new one.
;
; v6: tile-dictionary frames (FPGA Passo 21b).  The core sends only the DISTINCT 8x8 tiles
; of a frame (N <= 512, 16 bytes each) plus a 28x32 tile map, so the SNES needs ~4 KB of
; DMA per frame instead of 12 KB.  That fits in the non-picture lines and allows the full
; 224 game lines.  SNES overscan mode (239 lines): 224 game lines with VOFF black lines above
; (8) and 7 below.  The black lines come from the HDMA colour
; tables (entry 224 = black), so nothing else is needed.
;   * IRQ at V=232 (phase 0): force blank, lock the newest buffer, DMA map + tiles to VRAM.
;   * IRQ at V=0   (phase 1): display on, pad, hotkeys, core registers.
;   * HDMA (indirect, source bank $70-$72 = the locked PSRAM buffer):
;       ch1-4  mode 3 -> $2121,$2121,$2122,$2122   colour k=0..3 of every line
;       ch5    mode 1 -> $2126,$2127               colour-window edges of every line
;
; PSRAM FRAME LAYOUT: buffer b at $380000 + b*$8000 = $70+b:8000
;   +$0000 header (word 0 = N, number of distinct tiles)   +$0010 BG1 map, 29 x 32 x 2 bytes (v15)
;   +$0800 tiles, 16 bytes each (2bpp)                     +$4000 + k*$400 colour k (4 bytes/line)
;   +$5000 window (2 bytes/line: left, right)              entry 232 of every table is black/default (v15)
;
; v13: the text palette (CGRAM 32..35) is rewritten every frame at the end of the V=232 IRQ,
;   i.e. in vertical blank when no HDMA runs, so a wrong colour cannot stay on screen.
; v12: Y = level Easy -> Normal -> Hard (-> Easy ...), name on screen like the palettes.
;   Easy = both difficulty switches B, Normal = left B + right A, Hard = both A.
; v11: Y key.
; v10: text only (no background), bright green.  v9: X ignored while paused.
; v8: X = next colour palette (Color-1, Color-2, Color-3, Black/White).  The choice goes to core
; register 8 ($2028, bits 1:0) every frame; the name is shown for ~2.5 s on BG2 (2bpp text,
; font in VRAM $1000, map at $1C00, row 26, inside the 160-pixel colour window).
;
; CONTROLS: SNES pad 1 = left joystick (P0), SNES pad 2 = right joystick (P1).
;   On each pad: d-pad = joystick (SWCHA bits 7:4 for pad 1, bits 3:0 for pad 2),
;   B or A = fire (pad 1 -> INPT4, pad 2 -> INPT5).  Pad 1 only: Start = RESET,
;   Select = SELECT, L/R/X/Y as below, L+R+Start+Select = reset the game,
;   L+R+Select+X = back to the menu.
; v19: second joystick.  Pad 2 ($421A/$421B) drives P1 (SWCHA bits 3:0, the same bit
;   order and polarity as P0) and fire 1 (register 2 bit 1), as on the Mk.III player.
;   Both ports go through the controller filter of the menu: a port is used only when
;   its ID nibble is 0 (pad) or 4 (NTT Data keypad, PAD_ID_NOTPAD = $000B), and the
;   nibble is masked off; anything else (mouse, Super Scope, nothing) reads as released.
;   The console switches, the combos and L/R/X/Y stay on pad 1.  The SWCHA table is
;   now written out here (it was an include without its generator).
; v18 (the button layout of the Mk.III player, so both boards play the same):
;   L = Easy/Normal/Hard, R = pause, X = TV type switch Color <-> B/W (SWCHB bit 3),
;   Y = next palette.  L, R, X and Y act on RELEASE, and only when that key was the only
;   one of L/R/X/Y/Select/Start pressed while it was down (CHL/CHH), so the combos never
;   change a setting.  L+R+Start+Select = reset the game (MCU command $80), as on the
;   Mk.III.  While L and R are both held, RESET and SELECT stay released.  The level
;   starts at Easy (both switches B).  Fire = B or A.

.p816
.a8
.i16

.ifndef DIAGBLD
DIAGBLD = 0                 ; 1 = test build with the DMA diagnostic on L
.endif
WIN    = $2020              ; core register window ($2020-$202F)
MAPB   = 29*32*2            ; tile map bytes = 1856 (v15: 29 rows)
VIRQ   = 233                ; V-IRQ line: after the last picture line (232)
VON    = 1                  ; V-IRQ line where the display is switched on (line 0 is black)
VOFF0  = 1                  ; black lines above the picture (screen line 0)
PICL   = 232                ; picture lines; table entry PICL is black/default
TBLS   = $2000              ; WRAM $7E:2000, $20 bytes per channel
VOFS1  = $3FF

; direct page
SBIT   = $13
BASE   = $14
SZW    = $16
NB1    = $18
NP1    = $1A
NP2    = $1C
NB3    = $1E
HDR    = $20
CHN    = $22
KK     = $2C
TMP    = $2E
PTR    = $40
JOYL   = $44
JOYH   = $45
RPREV  = $47
MAPPER = $48
PAUSE  = $5A
SKIPV  = $5B
RUNV   = $5D
PHASE  = $61
VOFF16 = $64                ; black lines above the picture (16-bit)
LDN    = $66                ; 1 = L and Select held (fine adjust active)
UDPREV = $67
DIRTY  = $69
XPREV  = $6B                ; X key state of the previous frame
PAL    = $6C                ; palette 0..3 (core register 8)
TXTCNT = $6D                ; frames left to show the palette name (BG2 on)
TXTUP  = $6E                ; 1 = upload the name row to VRAM at the next V=232 IRQ
TXTSEL = $6F                ; which name row to upload: 0..3 palettes, 4 Easy, 5 Normal, 6 Hard, 7 PAUSE
TXTPAU = 7                  ; name row "PAUSE"
YPREV  = $74                ; Y key state of the previous frame
DIFF   = $75                ; level 0 = Easy, 1 = Normal, 2 = Hard
TXTROW = 26                 ; BG2 map row of the name
TXTFR  = 150                ; frames the name stays on screen (~2.5 s)
HP     = $70                ; long pointer to the locked buffer header
NCUR   = $78                ; tile count N of the frame uploaded last (16-bit)
MAXN   = $7A                ; highest N since L was pressed (16-bit)
MAXL   = $7C                ; most lines the DMA ran into the next frame since L was pressed
LPREV  = $7D                ; L key state of the previous frame
DIAG   = $7E                ; 1 = L held: show the diagnostic row
DUP    = $7F                ; 1 = upload the diagnostic row at the next V=233 IRQ
DSHOWN = $80                ; 1 = the diagnostic row is in VRAM
VNOW   = $82                ; V counter at the end of the DMAs (16-bit)
CHL    = $84                ; X L R pressed while one of L/R/X/Y was down (v18)
CHH    = $85                ; Y Select Start, the same
TVBW   = $86                ; 1 = TV type switch at B/W (SWCHB bit 3 low)
JOY2L  = $88                ; pad 2, $421A after the controller filter (v19)
JOY2H  = $89                ; pad 2, $421B
DROW   = $0300              ; WRAM (bank 0 mirror): the diagnostic row, 32 words

.segment "CODE"

; ---------------------------------------------------------------- reset ----
Reset:
    sei
    clc
    xce
    rep #$10
    sep #$20
    .a8
    .i16
    ldx #$1FFF
    txs
    phk
    plb

    lda #$8F
    sta $2100               ; forced blank
    stz $4200
    stz $420B
    stz $420C
    stz $2101
    stz $2105               ; BG mode 0
    stz $2106
    lda #$18
    sta $2107               ; BG1 map at VRAM $1800, 32x32
    lda #$1C
    sta $2108               ; BG2 map at VRAM $1C00, 32x32
    stz $2109
    stz $210A
    lda #$10
    sta $210B               ; BG1 chr at $0000, BG2 chr at $1000
    stz $210F
    stz $210F               ; BG2 scroll 0
    stz $2110
    stz $2110
    stz $210C
    stz $210D               ; BG1 H scroll 0
    stz $210D
    lda #<VOFS1
    sta $210E               ; BG1 V scroll
    lda #>VOFS1
    sta $210E
    lda #$80
    sta $2115               ; VRAM increment after high byte
    stz $211A
    stz $2123
    stz $2124
    lda #$20
    sta $2125               ; colour window: W1 enabled
    stz $2126               ; window edges come from HDMA every line
    lda #255
    sta $2127
    stz $212A
    stz $212B
    lda #$01
    sta $212C               ; BG1 on
    stz $212D
    stz $212E
    stz $212F
    lda #$40
    sta $2130               ; black outside the colour window (the sides, HMOVE bar)
    stz $2131
    lda #$04
    sta $2133               ; overscan: 239 visible lines

    ; VRAM = 0 (DMA with a fixed zero source, 64 KB)
    stz $2116
    stz $2117
    lda #$09
    sta $4300
    lda #$18
    sta $4301
    ldx #ZeroByte
    stx $4302
    stz $4304
    ldx #$0000
    stx $4305
    lda #$01
    sta $420B

    ; BG2 font -> VRAM $1000 (words), 2bpp tiles
    ldx #$1000
    stx $2116
    lda #$01
    sta $4300
    lda #$18
    sta $4301
    ldx #FontTiles
    stx $4302
    stz $4304
    ldx #FONTLEN
    stx $4305
    lda #$01
    sta $420B
    jsr TextPal

    ; variables
    stz PAUSE
    stz XPREV
    stz PAL
    stz TXTCNT
    stz TXTUP
    stz TXTSEL
    stz YPREV
    stz LPREV
    stz DIAG
    stz DUP
    stz DSHOWN
    stz MAXL
    stz NCUR
    stz NCUR+1
    stz MAXN
    stz MAXN+1
    stz DIFF                ; Easy = both switches B (v18, as on the Mk.III)
    stz CHL
    stz CHH
    stz TVBW
    lda #16
    sta SKIPV               ; register 6 = SKIPV - 16 (signed offset of the first captured line)
    lda #VOFF0
    sta VOFF16
    stz VOFF16+1
    stz LDN
    stz UDPREV
    stz JOYL
    stz JOYH
    stz JOY2L
    stz JOY2H
    stz DIRTY
    lda #$7E
    sta PTR+2
    jsr BuildAll            ; pointer tables (same for the 3 buffers: only the bank changes)
    sep #$20
    .a8

    ; HDMA: ch1-4 indirect mode 3 -> $2121,$2121,$2122,$2122; ch5 indirect mode 1 -> $2126,$2127
    lda #$43
    sta $4310
    sta $4320
    sta $4330
    sta $4340
    lda #$41
    sta $4350
    lda #$21
    sta $4311
    sta $4321
    sta $4331
    sta $4341
    lda #$26
    sta $4351
    lda #$7E
    sta $4314               ; table bank (WRAM)
    sta $4324
    sta $4334
    sta $4344
    sta $4354
    lda #$70
    sta $4317               ; indirect data bank (PSRAM); phase 0 changes it every frame
    sta $4327
    sta $4337
    sta $4347
    sta $4357
    ldx #TBLS
    stx $4312
    ldx #TBLS+$20
    stx $4322
    ldx #TBLS+$40
    stx $4332
    ldx #TBLS+$60
    stx $4342
    ldx #TBLS+$80
    stx $4352
    lda #$3E
    sta $420C               ; HDMA on channels 1-5

    ; mapper from the firmware
    lda #1
    sta MAPPER
    lda $FFF1
    cmp #$A2
    bne @nomagic
    lda $FFF0
    sta MAPPER
@nomagic:
    lda MAPPER
    asl
    ora #$01
    sta WIN+3               ; run = 1, mapper = bits 3:1
    sta RUNV
    stz WIN+6               ; no skipped lines

    jsr SpcUnmute           ; take the S-DSP out of mute so the cartridge audio is heard

    stz PHASE
    lda #VIRQ
    sta $4209
    stz $420A               ; V-IRQ time
    stz $4207
    stz $4208
    lda #$21
    sta $4200               ; V-IRQ on, NMI off, auto joypad on
    cli                     ; the CPU leaves reset with IRQs masked
@idle:
    bra @idle

; ------------------------------------------------------------------ IRQ ----
Nmi:
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8
    lda $4211               ; acknowledge IRQ
    lda PHASE
    beq @ph1
    jmp @ph2
@ph1:
    lda #1
    sta PHASE               ; next IRQ at line VON (it stays pending until this handler returns)
    lda #VON
    sta $4209
    stz $420A
    lda #$8F
    sta $2100               ; forced blank

    lda WIN+0               ; newest complete buffer N (0..2), sampled ONCE
    and #$03
    cmp #$03
    bcc @nok
    lda #$00
@nok:
    sta SBIT
    ora #$80
    sta WIN+7               ; lock it: the core never writes into a locked buffer
    clc
    lda #$70
    adc SBIT
    sta KK                  ; bank $70+N
    sta $4317               ; HDMA indirect data bank = the locked buffer
    sta $4327
    sta $4337
    sta $4347
    sta $4357

    lda #$FF                ; BG vertical scroll = -(VOFF+1)
    sec
    sbc VOFF16
    sta $210E
    lda #$03
    sta $210E

    ; DMA 1: BG1 tile map (29 rows) -> VRAM $1800
    ldx #$1800
    stx $2116
    lda #$01
    sta $4300
    lda #$18
    sta $4301
    ldx #$8010
    stx $4302
    lda KK
    sta $4304
    ldx #MAPB
    stx $4305
    lda #$01
    sta $420B

    ; DMA 2: the N distinct tiles (16 bytes each, N from the header) -> VRAM $0000
    ldx #$8000
    stx HP
    lda KK
    sta HP+2
    rep #$20
    .a16
    lda [HP]
    and #$03FF
    sta NCUR                ; for the diagnostic (flags still from the AND)
    beq @skipt              ; N = 0: nothing valid yet
    cmp #513
    bcc @nl
    lda #512
@nl:
    asl
    asl
    asl
    asl
    tax                     ; N*16 bytes
    sep #$20
    .a8
    ldy #$0000
    sty $2116
    ldy #$8800
    sty $4302
    stx $4305
    lda #$01
    sta $420B               ; the CPU waits here
@skipt:
    sep #$20
    .a8
    lda TXTUP
    beq @notxt
    stz TXTUP
    stz DSHOWN              ; the diagnostic row is being replaced
    ldx #$1C00+TXTROW*32
    stx $2116
    lda #$01
    sta $4300
    lda #$18
    sta $4301
    rep #$30
    .a16
    .i16
    lda TXTSEL
    and #$000F
    asl
    asl
    asl
    asl
    asl
    asl                     ; PAL * 64 bytes per name row
    clc
    adc #TxtRows
    sta $4302
    sep #$20
    .a8
    stz $4304
    ldx #64
    stx $4305
    lda #$01
    sta $420B
@notxt:
.if DIAGBLD
    lda DUP                 ; diagnostic row (built in WRAM by phase 1) -> BG2 name row
    beq @nodup
    stz DUP
    ldx #$1C00+TXTROW*32
    stx $2116
    lda #$01
    sta $4300
    lda #$18
    sta $4301
    ldx #DROW
    stx $4302
    stz $4304               ; bank 0: WRAM mirror
    ldx #64
    stx $4305
    lda #$01
    sta $420B
    lda #$01
    sta DSHOWN
@nodup:
    ; where did the VRAM work end?  V < VIRQ means it ran into the next frame: the display
    ; (switched on by the V=VON IRQ, which waits for this handler) comes on V lines late.
    lda $213F               ; reset the counter read flip-flop
    lda $2137               ; latch H/V
    lda $213D
    sta VNOW
    lda $213D
    and #$01
    sta VNOW+1
    rep #$20
    .a16
    lda VNOW
    cmp #VIRQ
    sep #$20
    .a8
    bcs @ontime
    lda VNOW                ; < 233: fits in 8 bits
    cmp MAXL
    bcc @ontime
    sta MAXL
@ontime:
    rep #$20
    .a16
    lda NCUR
    cmp MAXN
    bcc @nomaxn
    sta MAXN
@nomaxn:
    sep #$20
    .a8
.endif
    jsr TextPal             ; vblank: HDMA is finished, safe to write CGRAM
    jmp @exit
@ph2:
    stz PHASE
    lda #VIRQ
    sta $4209
    stz $420A               ; next IRQ at line VIRQ
    lda #$0F
    sta $2100               ; display on

    ; joypad: wait until the auto read is finished
@padwait:
    lda $4212
    and #$01
    bne @padwait
    ; both ports through the controller filter of the menu (v19): ID nibble 0 (pad) or
    ; 4 (NTT Data keypad) = the 12 buttons, nibble masked off; anything else = released
    rep #$20
    .a16
    lda $4218
    bit #$000B              ; PAD_ID_NOTPAD
    beq @p1pad
    lda #$0000
@p1pad:
    .a16
    and #$FFF0
    sta JOYL                ; JOYL/JOYH
    lda $421A
    bit #$000B
    beq @p2pad
    lda #$0000
@p2pad:
    .a16
    and #$FFF0
    sta JOY2L               ; JOY2L/JOY2H
    sep #$20
    .a8

    ; reset-to-menu combo L+R+Select+X (pad word $2070), same as the sd2snes in-game hook
    lda JOYL
    cmp #$70
    bne @noigr
    lda JOYH
    cmp #$20
    bne @noigr
    lda #$8F
    sta $2100               ; forced blank
    stz $4200               ; NMI and auto-joypad off
    stz $420C               ; HDMA off
    lda #$81
    sta $2A00
@igrwait:
    bra @igrwait            ; the firmware resets the SNES within ~250 ms
@noigr:
    ; reset-the-game combo L+R+Start+Select (pad word $3030), as on the Mk.III player
    lda JOYL
    cmp #$30
    bne @norst
    lda JOYH
    cmp #$30
    bne @norst
    lda #$8F
    sta $2100               ; forced blank
    stz $4200               ; NMI and auto-joypad off
    stz $420C               ; HDMA off
    lda #$80
    sta $2A00
@rstwait:
    bra @rstwait            ; the firmware resets the SNES
@norst:
    ; L, R, X, Y: collect everything pressed while one of them is down, act on release
    lda JOYH
    and #$40                ; Y
    sta TMP
    lda JOYL
    and #$70                ; X L R
    ora TMP
    beq @keyup
    lda JOYL
    and #$70
    ora CHL
    sta CHL
    lda JOYH
    and #$70                ; Y Select Start
    ora CHH
    sta CHH
    jmp @keysdone
@keyup:
    lda CHL
    ora CHH
    bne @keyrel
    jmp @keysdone
@keyrel:
    lda CHH
    beq @lowkey
    cmp #$40
    bne @keyclr             ; Select or Start was in it
    lda CHL
    bne @keyclr
    jmp @kY
@lowkey:
    lda CHL
    cmp #$10
    beq @kR
    cmp #$20
    beq @kL
    cmp #$40
    beq @kX
@keyclr:
    jmp @keyend

@kR:
    stz TXTCNT              ; any palette/level name goes away
    lda PAUSE               ; R button: pause / resume
    eor #$01
    sta PAUSE
    beq @resume
    lda #TXTPAU             ; paused: upload "PAUSE"; it stays on while PAUSE = 1
    sta TXTSEL
    lda #$01
    sta TXTUP
    jmp @keyend
@resume:
    stz TXTUP               ; resumed: nothing to show, BG2 goes off below in this same frame
    jmp @keyend

@kL:
    lda PAUSE
    bne @keyend             ; paused: the level name would replace "PAUSE"
    lda DIFF
    clc
    adc #$01
    cmp #$03
    bcc @lvok
    lda #$00                ; Hard -> Easy
@lvok:
    sta DIFF
    clc
    adc #$04                ; name rows 4 Easy, 5 Normal, 6 Hard
    bra @kshow

@kX:
    lda PAUSE
    bne @keyend
    lda TVBW
    eor #$01
    sta TVBW
    clc
    adc #$09                ; name rows 9 TV Color, 10 TV B/W
    bra @kshow

@kY:
    lda PAUSE
    bne @keyend             ; paused: the frame is frozen, a palette change would not show
    lda PAL
    clc
    adc #$01
    and #$03
    sta PAL                 ; name rows 0..3
@kshow:
    sta TXTSEL
    lda #TXTFR
    sta TXTCNT
    lda #$01
    sta TXTUP
@keyend:
    stz CHL
    stz CHH
@keysdone:
.if DIAGBLD
    ; L = DMA diagnostic (L is not an Atari input).  Pressing it clears the maxima.
    lda JOYL
    and #$20
    sta TMP
    lda LPREV
    eor #$FF
    and TMP
    beq @nole
    stz MAXL
    stz MAXN
    stz MAXN+1
    stz DSHOWN
    stz TXTCNT              ; a palette/level name goes away
@nole:
    lda TMP
    sta LPREV
    stz DIAG
    lda TMP
    beq @nodiag
    lda PAUSE
    bne @nodiag             ; paused: "PAUSE" has priority
    lda #$01
    sta DIAG
    jsr BuildDiag
    lda #$01
    sta DUP
@nodiag:
.endif
    lda PAL
    sta WIN+8               ; core register 8: palette (latched by the core at the next VSYNC)
    lda PAUSE               ; BG2 on for the whole pause ("PAUSE"), once the row is in VRAM
    beq @np
    lda TXTUP
    bne @txoff
    bra @txon
@np:
    lda DIAG                ; L held: diagnostic row, once it is in VRAM
    beq @nd
    lda DSHOWN
    bne @txon
    bra @txoff
@nd:
    lda TXTUP               ; new row not in VRAM yet (upload at the next V=233): keep BG2 off
    bne @txoff
    lda TXTCNT              ; or while a palette/level name is counting down
    beq @txoff
    dec TXTCNT
@txon:
    lda #$03
    bra @txw
@txoff:
    lda #$01
@txw:
    sta $212C

    lda PAUSE               ; register 3: run + mapper + pause (bit 4)
    asl
    asl
    asl
    asl
    ora RUNV
    sta WIN+3

    lda SKIPV
    sec
    sbc #16
    sta WIN+6               ; signed offset -16..+47

    rep #$30
    .a16
    .i16
    lda JOY2H               ; pad 2 -> P1: the table byte, high nibble moved to bits 3:0
    and #$000F
    tax
    sep #$20
    .a8
    lda SwchaLut,x
    lsr
    lsr
    lsr
    lsr
    sta TMP
    rep #$20
    .a16
    lda JOYH                ; pad 1 -> P0, bits 7:4
    and #$000F
    tax
    sep #$20
    .a8
    lda SwchaLut,x
    and #$F0
    ora TMP
    sta WIN+0               ; SWCHA

    lda #$FF
    sta TMP
    lda TVBW
    beq @tvc
    lda #$F7                ; TV type switch at B/W: bit 3 low
    sta TMP
@tvc:
    lda JOYL
    and #$30
    cmp #$30
    beq @nosel              ; L+R held (a combo): RESET and SELECT stay released
    lda JOYH
    and #$10                ; Start -> RESET (bit 0)
    beq @nost
    lda TMP
    and #$FE
    sta TMP
@nost:
    lda JOYH
    and #$20                ; Select -> SELECT (bit 1)
    beq @nosel
    lda TMP
    and #$FD
    sta TMP
@nosel:
    lda DIFF
    cmp #$02
    beq @hardsw             ; Hard: both switches A (bits 7 and 6 high)
    cmp #$01
    beq @normsw
    lda TMP
    and #$3F                ; Easy: both switches B
    sta TMP
    bra @hardsw
@normsw:
    lda TMP
    and #$BF                ; Normal: left switch B (bit 6 low), right switch A
    sta TMP
@hardsw:
    lda TMP
    sta WIN+1               ; SWCHB
    lda #$03                ; register 2: bit 0 = fire 0, bit 1 = fire 1, 1 = released
    sta TMP
    lda JOYH
    ora JOYL
    and #$80                ; pad 1 B or A -> fire 0 (INPT4)
    beq @nof0
    lda TMP
    and #$FE
    sta TMP
@nof0:
    .a8
    lda JOY2H
    ora JOY2L
    and #$80                ; pad 2 B or A -> fire 1 (INPT5)
    beq @nof1
    lda TMP
    and #$FD
    sta TMP
@nof1:
    .a8
    lda TMP
    sta WIN+2

    lda DIRTY               ; picture moved: rebuild the pointer tables
    beq @exit
    stz DIRTY
    jsr BuildAll
@exit:
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    sep #$20
    .a8
    rti

; BG2 palette 0 = CGRAM 32..35: transparent, then bright green x3 (CGRAM has no HDMA user here)
; a8 on entry and exit; call only in force blank / vertical blank (no HDMA writes to $2121).
TextPal:
    .a8
    lda #32
    sta $2121
    stz $2122
    stz $2122               ; colour 0 (transparent)
    ldx #3
@tp:
    lda #$E0
    sta $2122
    lda #$03
    sta $2122               ; colours 1..3: bright green $03E0
    dex
    bne @tp
    rts

; ------------------------------------------------- HDMA pointer tables ----
; For every channel: [header, ptr lo, ptr hi] segments, then $00.
;   black lines on top (header = n, shows entry 224 once and holds)
;   picture 224 lines in two repeat segments of 112 (header = $80|112)
;   black lines below, up to 239 lines in total
BuildAll:
    rep #$30
    .a16
    .i16
    lda VOFF16
    sta NB1                 ; 1 black line on top (screen line 0)
    lda #1
    sta NB3                 ; 1 black line below (233), then the HDMA stops (force blank)
    lda #PICL/2
    sta NP1
    sta NP2
    lda #TBLS
    sta PTR
    lda #$8000+$4000        ; colour tables at +$4000 + k*$400, window at +$5000 of the buffer
    sta BASE
    stz CHN
@ch:
    lda CHN
    lsr
    lsr
    sta SZW                 ; channel 4 (window) has 2-byte entries
    jsr EmitChan
    lda PTR
    clc
    adc #$20
    sta PTR
    lda BASE
    clc
    adc #$400
    sta BASE
    inc CHN
    lda CHN
    cmp #5
    bne @ch
    sep #$20
    .a8
    rts

; writes the segments of one channel at [PTR] (a16, i16)
EmitChan:
    .a16
    .i16
    ldy #0
    lda NB1
    beq @a
    sta HDR
    lda #PICL
    jsr Seg
@a:
    lda NP1
    ora #$80
    sta HDR
    lda #0
    jsr Seg
    lda NP2
    ora #$80
    sta HDR
    lda #PICL/2
    jsr Seg
    lda NB3
    beq @d
    sta HDR
    lda #PICL
    jsr Seg
@d:
    sep #$20
    .a8
    lda #0
    sta [PTR],y
    rep #$20
    .a16
    rts

.if DIAGBLD
; diagnostic row: template "N000 L000" from TxtRows row 8, digits of MAXN and MAXL
; (a8 on entry and exit)
BuildDiag:
    rep #$30
    .a16
    .i16
    ldx #0
@cp:
    lda TxtRows+8*64,x
    sta DROW,x
    inx
    inx
    cpx #64
    bne @cp
    lda MAXN
    ldy #DCOLN*2
    jsr Dec3
    lda MAXL
    and #$00FF
    ldy #DCOLL*2
    jsr Dec3
    sep #$20
    .a8
    rts

; A = value (shown 0..999), Y = byte offset in DROW: 3 digit tiles (a16, i16)
Dec3:
    .a16
    .i16
    cmp #1000
    bcc @ok
    lda #999
@ok:
    ldx #0
@h:
    cmp #100
    bcc @hd
    sbc #100
    inx
    bra @h
@hd:
    pha
    txa
    clc
    adc #DIG0
    sta DROW,y
    pla
    iny
    iny
    ldx #0
@t:
    cmp #10
    bcc @td
    sbc #10
    inx
    bra @t
@td:
    pha
    txa
    clc
    adc #DIG0
    sta DROW,y
    pla
    iny
    iny
    clc
    adc #DIG0
    sta DROW,y
    rts

.endif
; A = entry index, HDR = header; writes 3 bytes at [PTR],y and advances y
Seg:
    .a16
    asl
    ldx SZW
    bne @w
    asl                     ; 4 bytes per entry
@w:
    clc
    adc BASE
    pha
    sep #$20
    .a8
    lda HDR
    sta [PTR],y
    iny
    rep #$20
    .a16
    pla
    sta [PTR],y
    iny
    iny
    rts
    .a8

; ------------------------------------------------------------ audio unmute ----
; The S-DSP leaves reset muted and that mutes the cartridge audio as well.  Upload a
; 8-byte SPC700 program through the boot ROM protocol ($2140-$2143) that writes
; DSP FLG ($6C) = $20 (mute off, echo off) and then spins.  Gives up silently if the
; boot ROM never answers.
SPCLEN = 8
SpcUnmute:
    ldx #$FFFF
@w1:
    lda $2140
    cmp #$AA
    bne @w1n
    lda $2141
    cmp #$BB
    beq @ready
@w1n:
    dex
    bne @w1
    rts
@ready:
    ldx #$0200              ; load address in SPC RAM
    stx $2142
    lda #$01
    sta $2141               ; non-zero = start a transfer block
    lda #$CC
    sta $2140
    ldx #$FFFF
@w2:
    lda $2140
    cmp #$CC
    beq @go
    dex
    bne @w2
    rts
@go:
    ldy #0
@nb:
    lda SpcProg,y
    sta $2141
    tya
    sta $2140               ; byte counter
    ldx #$FFFF
@w3:
    cmp $2140
    beq @ok
    dex
    bne @w3
    rts
@ok:
    iny
    cpy #SPCLEN
    bne @nb
    ldx #$0200              ; execute address
    stx $2142
    stz $2141               ; zero = jump
    lda #SPCLEN+1           ; counter + 2 relative to the last byte index
    sta $2140
    rts

Dummy:
    rti

; -------------------------------------------------------------- data ----
.segment "RODATA"
ZeroByte:
    .byte 0
    .include "font_txt.inc"
SpcProg:
    .byte $8F,$6C,$F2       ; mov $F2,#$6C   (DSP register FLG)
    .byte $8F,$20,$F3       ; mov $F3,#$20   (soft reset off, mute off, echo write off)
    .byte $2F,$FE           ; bra $          (spin)
; SWCHA nibble of one joystick, indexed by the d-pad bits of $4219/$421B (bit 3 Up,
; 2 Down, 1 Left, 0 Right): bits 7:4 = Right Left Down Up, active low, bits 3:0 = 1.
; Pad 1 uses it as is (P0); pad 2 moves the high nibble down (P1, bits 3:0).
SwchaLut:
    .byte $FF,$7F,$BF,$3F,$DF,$5F,$9F,$1F,$EF,$6F,$AF,$2F,$CF,$4F,$8F,$0F

.segment "HEADER"           ; $FFC0
    .byte "ATARI2600 VIDEO PLYR "   ; 21 bytes
    .byte $20               ; LoROM, slow
    .byte $00               ; ROM only
    .byte $05               ; 32 KB
    .byte $00               ; no SRAM
    .byte $01               ; NTSC
    .byte $00
    .byte $00               ; version
    .word $FFFF, $0000      ; complement / checksum, patched by fix_snes_checksum.py
    .res 4,0                ; $FFE0-$FFE3

.segment "VECTORS"          ; $FFE4
    .word Dummy, Dummy, Dummy, Dummy, 0, Nmi        ; native: COP BRK ABORT NMI - IRQ
    .word 0, 0                                      ; $FFF0: mapper, $FFF1: magic (firmware patches)
    .word Dummy, 0, Dummy, Nmi, Reset, Dummy        ; emulation: COP - ABORT NMI RESET IRQ
