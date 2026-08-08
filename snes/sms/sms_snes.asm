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

    jsr SmsApuUnmute    ; open the console's cart-DAC audio path

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


; ===========================================================================
; S-DSP unmute -- opens the console's cartridge-DAC audio path.
;
; WHY: with no program uploaded to the S-SMP, the S-DSP stays in its IPL
; reset/mute state, and in that state the console's analogue path GATES the
; cartridge DAC output -- the FPGA can drive I2S perfectly and nothing reaches
; the jack.  Same mechanic, same cure, same 82-byte payload as snes/sfx.a65
; (menu) and snes/nes/nes_apu.a65 (NES renderer, proven on hardware).
;
; ENTRY CONTRACT (true at the call site in Reset): native mode, DBR=$00,
; DP=$0000, NMI off ($4200=0 since the reset), A 8-bit / X,Y 16-bit.  Returns
; carry clear = stub running, carry set = timed out (boot continues, at worst
; silent).
; ===========================================================================

!SMS_APU_BEEP = 0              ; 1 = instrumented payload (209 bytes): the same
                               ;     unmute plus a 0.5 s 2 kHz beep at boot, so
                               ;     "no sound" can be split into "the S-DSP
                               ;     gate is still shut" (no beep) and "the gate
                               ;     is open, the PSG side is the problem"
                               ;     (beep, then silence).  BRING-UP ONLY.
                               ; 0 = PRODUCTION: 82 bytes, unmute only, silent.
                               ;     Set this back to 0 before shipping.

!APUIO0 = $2140
!APUIO1 = $2141
!APUIO2 = $2142

if !SMS_APU_BEEP == 1
!SMS_APU_STUB_ADDR = $0300     ; upload base: DIR table + BRR + code
!SMS_APU_EXEC_ADDR = $0320     ; code entry (DIR must own $0300)
!SMS_APU_STUB_LEN  = $00D1     ; 209
else
!SMS_APU_STUB_ADDR = $0300
!SMS_APU_EXEC_ADDR = $0300
!SMS_APU_STUB_LEN  = $0052     ; 82
endif

!SMS_APU_WAIT_POLLS  = $0800   ; per-wait ceiling
!SMS_APU_POLL_BUDGET = $FFFF   ; aggregate ceiling across the whole routine

SmsApuUnmute:
    sep #$20
    rep #$10
    ldy.w #!SMS_APU_POLL_BUDGET

    ; --- 1) wait for the IPL to publish $BBAA ---------------------------------
    ; own ceiling ~5x the handshake one: the IPL spends ~2.4 ms clearing page
    ; zero before it answers, and a timeout here is indistinguishable from
    ; "the analogue gate was not the cause".
    ldx.w #$4000
SmsApuHello:
    lda !APUIO0
    cmp.b #$aa
    bne SmsApuHelloNext
    lda !APUIO1
    cmp.b #$bb
    beq SmsApuHelloOk
SmsApuHelloNext:
    dey
    beq SmsApuFail
    dex
    bne SmsApuHello
    bra SmsApuFail
SmsApuHelloOk:

    ; --- 2) open the transfer -------------------------------------------------
    ldx.w #!SMS_APU_STUB_ADDR
    stx !APUIO2                 ; $2142/$2143 = destination
    lda.b #$cc
    sta !APUIO1                 ; non-zero starts the transfer
    sta !APUIO0
    jsr SmsApuWait
    bcs SmsApuFail

    ; --- 3) payload, byte by byte --------------------------------------------
    ; X is both the table index and the protocol index; the payload is < 256
    ; bytes so the index never wraps.
    ldx.w #$0000
SmsApuSend:
    lda.l SmsApuStub,x
    sta !APUIO1
    txa
    sta !APUIO0
    jsr SmsApuWait
    bcs SmsApuFail
    inx
    cpx.w #!SMS_APU_STUB_LEN
    bne SmsApuSend

    ; --- 4) end of transfer: entry address + index+2 --------------------------
    ldx.w #!SMS_APU_EXEC_ADDR
    stx !APUIO2
    stz !APUIO1                 ; 0 = execute, no further block
    lda !APUIO0
    inc a
    inc a
    sta !APUIO0
    jsr SmsApuWait
    bcs SmsApuFail
    clc
    rts

SmsApuFail:
    sec
    rts

; A(8) = expected echo, Y(16) = remaining global budget.
; carry clear = echoed, carry set = timed out.  A and X preserved.
SmsApuWait:
    phx
    ldx.w #!SMS_APU_WAIT_POLLS
SmsApuWaitPoll:
    cmp !APUIO0
    beq SmsApuWaitOk
    dey
    beq SmsApuWaitTo
    dex
    bne SmsApuWaitPoll
SmsApuWaitTo:
    plx
    sec
    rts
SmsApuWaitOk:
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
SmsApuStub:
if !SMS_APU_BEEP == 1
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
