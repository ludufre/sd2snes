; sd2snes SMS core (M7.4b) — SNES-side player for the REAL FPGA core, LoROM.
;
; Booted by the firmware (FPGA_SMS) as the cart ROM, in place of the .sms itself
; (sms_update_file swaps the filename; the .sms is staged separately to PSRAM
; 0x300000). Each vblank it DMAs the FPGA-translated SMS frame from the $E0-$E3
; buffer banks (PSRAM 0x380000, mapped by address.v) into VRAM/CGRAM/OAM, so the
; SNES PPU draws the live SMS frame; it also forwards the pad to $EF and swaps the
; FPGA's double buffer.
;
; ASSEMBLER: asar (NOT snescom/sneslink like snes/ and snes/nes/) -- see the
; Makefile next to this file. Built into misc/sms_snes.bin by build.sh.
;
; IMPORTANT -- this player initialises only what it needs (BGMODE, BG1/OBJ bases,
; TM, scroll). Everything else in $21xx it INHERITS from whatever the menu left
; behind, because the console reset does not clear that block. That is why
; iris_out (snes/iris.a65) must hand over a disarmed PPU: a stale colour window or
; a stale $2133 pseudo-hires bit lands here as a broken/blurry picture.
;
; Buffer banks (match address.v SMS_BUF_HIT):
;   $E0 tiles(16KB) $E1 tilemap(2KB) $E2 cgram(512) $E3 oam(544)

!TILES_BANK   = $E0
!TILEMAP_BANK = $E1
!CGRAM_BANK   = $E2
!OAM_BANK     = $E3
!BG1_CHAR = $0000
!BG1_MAP  = $2000

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
    sep #$20

    lda.b #$8F
    sta $2100
    stz $4200
    stz $420C
    stz $210D           ; BG1HOFS = 0
    stz $210D
    stz $210E           ; BG1VOFS = 0
    stz $210E

    lda.b #$01
    sta $2105           ; BGMODE 1 (BG1 4bpp)
    lda.b #$20
    sta $2107           ; BG1SC: map base word $2000, 32x32
    lda.b #$00
    sta $210B           ; BG1 char base word $0000
    lda.b #$00
    sta $2101           ; OBSEL: OBJ char base $0000
    lda.b #$11
    sta $212C           ; TM: BG1 + OBJ

    lda.b #$81
    sta $4200           ; NMI + auto-joypad
    lda.b #$0F
    sta $2100

MainLoop:
    wai
    bra MainLoop

NMI:
    rep #$30
    pha
    phx
    phy
    sep #$20

    lda.b #$8F
    sta $2100           ; force blank for DMA

    ; CGRAM 512B ($E2) -> CGRAM 0
    stz $2121
    lda.b #$00
    sta $4300
    lda.b #$22
    sta $4301
    ldx.w #$0000
    stx $4302
    lda.b #!CGRAM_BANK
    sta $4304
    ldx.w #$0200
    stx $4305
    lda.b #$01
    sta $420B

    ; VMAIN +1 word (used by both the tiles and tilemap DMA)
    lda.b #$80
    sta $2115

    ; tiles: two changed-tile spans -> BG (low half 0..255) + sprites (high half 256..511).
    ; A single [min..max] spans both halves whenever BG and sprite tiles both change in a
    ; frame, re-uploading the whole 16KB -> vblank overrun that grows with sprite count.
    ; FPGA emits each half's range at $E2:0204 lo_min / 0205 lo_max / 0206 hi_min /
    ; 0207 hi_max (8-bit; hi is an index, absolute tile = 256+idx). carry-clear = empty.
    sep #$20
    lda.l $E20205            ; lo_max
    sec
    sbc.l $E20204            ; lo_max - lo_min ; carry clear -> empty
    bcc NoLoTiles
    rep #$20
    and.w #$00FF
    inc a                    ; count
    tay
    lda.l $E20204
    and.w #$00FF             ; abs tile = lo_min
    tax
    jsr DmaTileSpan
NoLoTiles:
    sep #$20
    lda.l $E20207            ; hi_max
    sec
    sbc.l $E20206            ; hi_max - hi_min ; carry clear -> empty
    bcc NoHiTiles
    rep #$20
    and.w #$00FF
    inc a                    ; count
    tay
    lda.l $E20206
    and.w #$00FF
    clc
    adc.w #$0100             ; abs tile = 256 + hi_min
    tax
    jsr DmaTileSpan
NoHiTiles:
    sep #$20

    ; tilemap: per-column vertical-increment DMA of only the DIRTY columns. The FPGA emits the
    ; tilemap COLUMN-MAJOR (column c = 64 bytes at $E1:(c*64)) and publishes a 32-bit dirty-column
    ; bitmap at $E2:0208-020B. $2115=$81 (+32-word increment) lays a column down the screen; clean
    ; columns keep their VRAM value (the FPGA didn't re-emit them) -> the 2KB DMA collapses to the
    ; few changed columns, cutting both the FPGA write AND this read out of the PSRAM contention.
    lda.b #$81
    sta $2115                ; VMAIN: +32-word vertical increment (column-down)
    lda.b #$01
    sta $4300                ; DMA mode 1 ($2118/$2119)
    lda.b #$18
    sta $4301
    lda.b #!TILEMAP_BANK
    sta $4304
    rep #$20
    lda.l $E20208
    sta $00                  ; dirty columns 0..15
    lda.l $E2020A
    sta $02                  ; dirty columns 16..31
    sep #$20
    ldx.w #$0000             ; X = column 0
TmCol:
    rep #$20
    lsr $02                  ; 32-bit bitmap >> 1: column X -> carry
    ror $00
    bcc TmColSkip            ; clean column -> skip DMA
    txa
    clc
    adc.w #!BG1_MAP
    sta $2116                ; VRAM dest = BG1_MAP + X (column start)
    txa
    asl
    asl
    asl
    asl
    asl
    asl                      ; X*64 = column source offset in $E1
    sta $4302
    lda.w #$0040
    sta $4305                ; 64 bytes — reload PER column: $4305 DECREMENTS to 0
                             ; during the DMA (a "constant" here = 64KB DMA on the
                             ; 2nd dirty column, wiping VRAM = the black-screen bug)
    sep #$20
    lda.b #$01
    sta $420B                ; trigger DMA ch0
    rep #$20
TmColSkip:
    inx
    cpx.w #$0020             ; 32 columns
    bne TmCol
    sep #$20

    ; OAM 544B ($E3) -> OAM
    stz $2102
    stz $2103
    lda.b #$00
    sta $4300
    lda.b #$04
    sta $4301
    ldx.w #$0000
    stx $4302
    lda.b #!OAM_BANK
    sta $4304
    ldx.w #$0220
    stx $4305
    lda.b #$01
    sta $420B

    ; --- scroll: read hofs/vofs from $E2:0200 (FPGA-emitted) -> BG1 scroll ---
    lda.l $E20200
    sta $210D           ; BG1HOFS low
    lda.l $E20201
    sta $210D           ; BG1HOFS high
    lda.l $E20202
    sta $210E           ; BG1VOFS low
    lda.l $E20203
    sta $210E           ; BG1VOFS high

    ; --- forward SNES pad (auto-joypad JOY1) to the FPGA control window $EF ---
    lda $4218
    sta.l $EF0000       ; JOY1L (A X L R)
    lda $4219
    sta.l $EF0001       ; JOY1H (B Y Sel St U D L R)

    ; --- double-buffer: done reading the front -> swap if the FPGA has a ready back ---
    sta.l $EF0005

    lda.b #$0F
    sta $2100           ; screen on
    lda $4210           ; ack NMI

    rep #$30
    ply
    plx
    pla
    sep #$20
    rti

; X = first absolute tile (0..511), Y = tile count (>=1); A/X/Y 16-bit on entry.
; DMA Y tiles (32B each) $E0:(tile*32) -> VRAM word (tile*16). $2115 = +1 word, ch0.
DmaTileSpan:
    tya
    asl
    asl
    asl
    asl
    asl                      ; count*32 = byte length
    sta $4305
    txa
    asl
    asl
    asl
    asl                      ; tile*16 = VRAM word dest
    sta $2116
    txa
    asl
    asl
    asl
    asl
    asl                      ; tile*32 = src byte offset
    sta $4302
    sep #$20
    lda.b #$01
    sta $4300                ; DMA mode 1 ($2118/$2119)
    lda.b #$18
    sta $4301
    lda.b #!TILES_BANK
    sta $4304                ; src bank $E0
    lda.b #$01
    sta $420B                ; trigger ch0
    rep #$20
    rts

Stub:
    rti

; ---- LoROM header (so smc_id detects LoROM) ----
org $00FFC0
    db "SMS PLAYER           "    ; 21-byte title
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
