; sd2snes GBC core -- SNES-side player for the experimental Game Boy Color FPGA
; core, LoROM.  PLAYER v1.6 (phase 5, second read window): every frame it strobes
; the genlock, latches the bridge's dirty bits, folds them into a persistent
; backlog, applies the snapshot's registers (TM, the four scroll pairs), copies
; the bridge's mid-frame log into WRAM and drains as much of the backlog into
; VRAM/CGRAM/OAM as the transfer window affords -- carrying the rest to the next
; frame.  Outside the window, while the display is active and the transfer
; engine has nothing to do, the RASTER COMPILER turns that log into the six
; HDMA tables of contract sec. 11.4, so scroll, window and palette writes that
; the Game Boy made in the middle of ITS frame are replayed line by line here.
; With the genlock LOCKED, a V-IRQ at V=185 (the first line of the bottom
; letterbox) opens the frame early -- COMMIT, status, fold and a first drain
; pass run over lines 185..224 -- and the NMI carries on from where it stopped:
; one transfer window of ~117 lines instead of ~77.  See the TWO WINDOWS note
; below and GbcIrqBody.  (v1.6, phase 5b: the same tables out of a compiler
; ~7x cheaper on a colour entry and ~2x on a register one -- a 512-entry colour
; log in ~3 frames instead of ~20 on the host model; see GbcPass1, GbcPassRegs
; and GbcPassColour.)
;
; Booted by the firmware (FPGA_GBC) as the cart ROM in place of the .gbc itself:
; the ROM is staged to PSRAM 0x880000 and mapped as a LoROM image, exactly like
; the SGB player it replaces.  Everything the GB actually is runs in the FPGA;
; this side only re-renders what the bridge publishes and forwards the pad.
;
; ASSEMBLER: asar (NOT snescom/sneslink like snes/ and snes/nes/) -- see the
; Makefile next to this file.  Built into misc/gbc_snes.bin by build.sh.
;
; COUPLED PAIR: fpga_gbc.bi3 and gbc_snes.bin implement the same wire version
; (!GBC_VER below, GBC-CORE-CONTRACT sec. 13.1) and are never flashed apart.
; The player refuses to release the forced blank when the version it reads back
; is not the one it was built against.
;
; IMPORTANT -- unlike the SMS player, this one initialises the WHOLE $2101-$2133
; block.  The console reset does not clear it and the menu hands over whatever
; it was last using, so a stale colour window, a stale mosaic or a stale $2133
; pseudo-hires bit would land here as a broken picture (contract sec. 11.3).
;
; View banks (address.v: gbc_view_enable = SNES_ADDR[23:18] == 6'b111000):
;   $E0:0000 BG chr (12 KB)   $E0:4000 OBJ chr (16 KB)
;   $E1:0000 the four map views, BY GAME BOY MAP and not by SNES layer:
;            MC0_ $E1:0000 content of $9800   MC1_ $E1:0800 content of $9C00
;            MK0_ $E1:1000 carpet of $9800    MK1_ $E1:1800 carpet of $9C00
;   $E2:0000 CGRAM view (512 B)   $E2:0200 status block   $E2:0400 mid-frame log
;   $E3:0000 OAM view (512 + 32 B)
; Write window $EF00xx: pads, COMMIT, GO, SYNC, CONSUMED -- 8-bit stores ONLY
; (contract sec. 13.4).  Every one of them goes through GbcEfPut, the single
; store site in the file, so the invariant is auditable by grep (and by
; selfcheck.py next to this file).
;
; ---------------------------------------------------------------------------
; TRANSFER MODEL (the premise the drain engine is built on)
; ---------------------------------------------------------------------------
; General DMA moves 1 byte per 8 master cycles.  Window A is the NMI (V~225)
; through V=261 and V=0..40: the letterbox HDMA holds $2100 = $80 over lines
; 1..40, so VRAM/CGRAM/OAM stay writable until line 41 starts -- that line is
; the DEADLINE, and past it writes are dropped SILENTLY (corruption, not a
; glitch), which is why every transfer is gated on the live V counter.
;
; Contract sec. 11.1 gives the ceiling as a formula, not a number:
;   eq_useful = (L*1364 - sum_lines(18 + 8C + 8B + 8H) - prologue) / 8
; With the two channels this phase runs (ch5 $2100 = 1 B/line, ch4 $2126/$2127
; = 2 B/line, both hold-runs so H ~ 0) the per-line HDMA tax is
; 18 + 8*2 + 8*3 = 58 mc, i.e. 163 byte-equivalents survive of the 170 a line
; would otherwise carry.  37 vblank lines * 170 + 41 tail lines * 163 = 12973,
; and the contract's published figure for "window A, 2 channels" is ~12600 --
; that is !GBC_BUDGET.  A DMA is charged bytes + !GBC_DMACOST against it.
;
; COST TABLE (byte-equivalents; every DMA pays its bytes + 384)
;   status block      64 B ->    448   once per frame, unconditional
;   CGRAM            512 B ->    896   when DIRTY_MISC.CRAM and SEQ advanced
;   OAM              512 B ->    896   when DIRTY_MISC.OAM  and SEQ advanced
;   one map class   4096 B ->   4480   64 rows of 64 B, coalesced into 1 DMA
;   both maps       8192 B ->   8960   the MSEL worst case (content + carpet)
;   BG chr         16384 B ->  18688   6 DMAs: the GB's 12 KB with the $8800
;                                      block of each bank sent TWICE, once into
;                                      each chr base (the VRAM map below)
;   OBJ chr        16384 B ->  16768   32 blocks of 512 B (half of it zeros, by
;                                     contract sec. 4.2 -- 1 DMA per 16 tiles)
;   FULL REFRESH   40960 B ->  44416   = 9 DMAs; 3.8 windows -> lands in 4-5
;                                      frames (wire $01 was 37952 B / 41408 eq)
; A full-map frame (8960) leaves 12600 - 448 - 8960 = 3192 eq for chr, so the
; chr classes still advance ~11 blocks even in the worst map storm; the class
; rotation below is what keeps that share from always going to the same class.
;
; VRAM MAP (contract sec. 11.2, wire $02).  The Game Boy's LCDC.4 picks which
; of TWO 512-tile chr bases a tile index means, so the 12 KB chr view is laid
; out twice over -- and the $8800 block, which both bases name, is the one that
; costs two transfers:
;   $0000-$3FFF  OBJ chr, 512 x 32
;   $4000-$5FFF  BG chr base A (LCDC.4 = 1): $8000 b0, $8800 b0, $8000 b1,
;                                            $8800 b1
;   $6000-$7FFF  BG chr base B (LCDC.4 = 0): $9000 b0, $8800 b0 (dup),
;                                            $9000 b1, $8800 b1 (dup)
;   $8000-$8FFF  content maps: $9800 then $9C00   (BG1/BG2 pick with $2107/$2108)
;   $9000-$9FFF  carpet maps:  $9800 then $9C00   (BG3/BG4 pick with $2109/$210A)
;   $A000-$A7FF  the LCD-off white map, written ONCE at init
;   $B000/$B010  the solid tiles 768/769 (carpet chr base $210C = $44)
;
; MEASURED, not modelled (bsnes-plus, gbc/tests/renderer-harness):
;   * the deadline was measured at V=40, H=1148 of 1364 mc back when line 40
;     was still forced blank -- the letterbox entry for line 41 runs in the
;     hblank at the end of line 40.  Line 40 is now $00 (screen on, brightness
;     0) so that the PPU prepares line 41's sprites, which moves the deadline
;     to the START of line 40: !GBC_DEADLINE = 40, exactly a line boundary,
;     with no sub-line correction to fold into the slack.
;   * !GBC_BUDGET is NEVER the binding constraint.  The live capacity at V=225
;     is 37*170 + 6520 - 1024 = 11786 byte-equivalents, below the 12600 model
;     ceiling, so GbcCapacity always answers first.  The budget line ($1E) is
;     kept as a second, cheaper bound and as the place the contract's figure is
;     written down -- not because anything depends on it.
;   * GbcCapacity assumes NTSC (262 lines).  On a PAL console V=225..261 is not
;     the end of the frame, the vblank branch claims zero whole lines (the
;     subtract borrows, see GbcCapVblOk) and only the 40-line tail is offered:
;     conservative, never unsafe.
;   * an IDLE frame body (empty backlog) ends at V=245, i.e. 20 of the 37
;     vblank lines, with the whole 41-line letterbox tail still unspent.  It
;     ended at V=48 -- 85 lines, MORE than the whole of window A -- until
;     GbcClsEmpty gave every class an O(1) exit; 75 of those 85 lines were the
;     six classes probing 208 all-zero block bits, i.e. the entire transfer
;     budget was being spent moving nothing.
;
; The byte-equivalent counter is a MODEL.  The hard backstop is GbcCapacity,
; which re-derives what is left from the LIVE V counter (9-bit read, contract
; sec. 13.12) before EVERY DMA, and !GBC_GUARDSLACK / !GBC_FIRESLACK hold back
; the player-side overhead the model knows nothing about.
;
; WHICH DMAs MAY STRADDLE V=0 (the HDMA init), and why only one may not.
; Capacity is deliberately offered as "the rest of the vblank PLUS the 40-line
; tail", so a transfer started in vblank is ALLOWED to still be running at
; V=0: the letterbox holds $2100 = $80 through line 40 and the destinations
; stay writable.  What a straddle costs is that the HDMA pauses the general DMA
; to take its own slots -- harmless unless the paused DMA shares a POINTER with
; one of the armed channels, because the general DMA sets its pointer once and
; then streams the data port.  Destination by destination:
;   status / log / CGRAM view   $2180-$2183 (WRAM port)   no channel writes it
;   OAM                         $2102-$2104               no channel writes it
;   block classes (chr, maps)   $2115-$2119               no channel writes it
;   CGRAM                       $2121-$2122               ⚠ a COLOUR CHANNEL
;                                                           writes exactly this
; The armed set is ch5 $2100, ch4 $2126/$2127, the scroll pairs $210D-$2114 and
; the colour channels $2121/$2122 (see GbcSetHeader), so CGRAM is the single
; collision -- and GbcDrainCgram is where it is refused, by making the block
; fit inside the vblank rather than merely start inside it.
; Two things this does NOT claim.  (1) It says nothing about the rev.1 5A22,
; which corrupts or hangs on a general DMA overlapping an HDMA at all; that
; exposure is published through !GBC_CTR_CPUREV and its mitigation would be to
; zero !GBC_TAILBYTES, not to move one transfer (see the note at Reset).
; (2) It is about POINTER sharing, not bandwidth: the tail is already charged
; per line at $70 bytes, which is what keeps a straddling DMA inside the
; window.
;
; ---------------------------------------------------------------------------
; TWO WINDOWS (phase 5): the V-IRQ at V=185 and the NMI share ONE frame body
; ---------------------------------------------------------------------------
; Lines 185..224 are the bottom letterbox: ch5 holds $2100 = $80 there, so
; VRAM/OAM are as writable as in the top one, and nothing used them.  A V-IRQ
; on the first of them (!GBC_VIRQ_LINE) OPENS the frame -- COMMIT, the status
; block, the fold, OAM and a first walk over the block classes (GbcIrqBody) --
; and the NMI RESUMES it: registers, the log copy, CGRAM and the rest of the
; same walk, then pads and CONSUMED (GbcFrameResume).  The backlog is
; persistent and every class keeps its own cursor, so "resume" costs nothing
; beyond remembering which class the walk stopped in ($66/$67).  Window A is
; unchanged; window B is added in front of it: ~40 lines, ~5 KB-eq.
;
; What stays in the NMI, and why:
;   * SYNC.  It is the genlock's phase reference (contract sec. 2: ALVO = 43
;     lines before it, LY=0 ~ V=182) and must not move with the workload.
;   * the prologue ($2100, GbcHdmaPublish, $420C).  The HDMA reads the
;     current set until line 224; the flip has to be past it and before V=0.
;   * GbcRegs.  It writes the scroll pairs the HDMA is NOT driving, and a
;     scroll channel whose tail hold is split at 127 lines DOES transfer inside
;     185..224 -- the shared BGOFS latch makes a CPU pair split by such a
;     transfer land wrong.  Vblank has no HDMA transfers at all.
;   * the log copy (GbcRasterArm).  Arming after the publish, as before, keeps
;     the compile cadence identical: arming at V=185 would find the previous
;     compile still waiting for its prologue and skip every other frame.
;   * CGRAM.  With a colour channel armed it is vblank-only (S9) anyway, and
;     deciding it in window B would race the prologue: a "no colour channel"
;     read at V=224 followed by a publish that arms one.
;
; How window B ends.  GbcCapacity, while $16 b0 is up, offers only the lines
; left before V=225 (at the OLD set's $70), so no DMA of window B is still on
; the bus when the NMI is due -- a DMA holds the CPU, and a late NMI is a late
; SYNC, i.e. genlock phase noise.  When nothing more fits, window B PAUSES
; (not an abort: no drop, no deferral counted).
;
; Who resumes.  $15 is a two-owner token, set to 2 by the IRQ; each side
; decrements it when its part is done, and whoever brings it to 0 runs
; GbcFrameResume.  DEC is one instruction and its Z flag survives an
; interrupt, so exactly one of them sees zero whichever order they land in:
; the usual case is the IRQ pausing before V=225 and the NMI resuming; if the
; IRQ part is still scanning at V=225 the NMI does its prologue and returns,
; and the IRQ part resumes the frame itself the moment it stops.
;
; When window B is NOT opened (the IRQ acknowledges and returns, and the NMI
; runs the whole phase-4 body exactly as before):
;   * the genlock is not LOCKED (flags0.b4 of the last status copy), or the
;     last snapshot did not advance, or LY_SYNC < 25 (GbcVirqWanted).  LY=0
;     then wanders through the frame, and every line the read window stays
;     open is a line on which a LY=0 is lost (snap_skipped); window B would
;     add 40 of them.  Locked, LY=0 sits at V~182, three lines before it.
;     ⚠ The Exato mode is NOT caught by LOCKED: the RTL reports it locked by
;     construction (gbc_clk.v), and it is the LY_SYNC test that keeps window B
;     off the frames whose LY=0 drifts into 185..224.
;   * a rev.1 5A22 (contract sec. 12.11): lines 185..224 run HDMA, and a
;     general DMA over an active HDMA is what that revision corrupts or hangs
;     on.  The V-IRQ is never enabled there (GbcVirqInit), so a rev.1 console
;     gets exactly the phase-4 exposure and not a line more.
;   * a PAL console ($213F bit 4): every number here is NTSC.  Never enabled.
;   * no bridge / no screen: the IRQ is a no-op.
; The wire version did not move: nothing here is visible to the core beyond
; COMMIT arriving 40 lines earlier, which contract sec. 6/7 already specified.

; Contract wire version this player implements.  It did NOT move for the
; mid-frame raster: every field that phase added to the status block lives in
; bytes sec. 5 had declared reserved (COMPAT_RAW +$40..+$57, LOG_N +$23..+$24,
; DIRTY_MISC.RASTER), an older core publishes them as $00, and $00 is exactly
; "no raster" -- the compiler then arms ch4 + ch5 and leaves the same letterbox
; and window-1 tables the previous player left.  That is the rule in contract
; sec. 13.1 (additive field in reserved space whose $00 degrades by itself does
; not bump; a change of format, address or meaning does), and the (P4-V13) case
; of tests/host/run_gbc_player.sh is the proof, not the claim.
;
; ⚡ IT DID MOVE TO $02 for LCDC.3/LCDC.4 per line.  The four map views changed
; MEANING at the SAME addresses -- they are now the Game Boy's two maps, each as
; content and carpet, instead of the SNES's two layers -- and the tile index of
; a map entry lost the LCDC.4 term.  A $01 core feeding a $02 player therefore
; draws a plausible and WRONG picture, which is exactly the case sec. 13.1 says
; must bump.  The player refuses the pair and stays in forced blank.
;
; ⚡ AND TO $03 FOR THE FRAMEBUFFER MODE (C6, contract sec. 14).  The bridge
; grew a fifth view bank ($E4: the framebuffer as 8bpp tiles), two write
; registers ($EF0006 C6_CTL, $EF0007 ROW_DONE), 58 live status bytes (+58..+91)
; and a 3-cycle read latency over the WHOLE view window; a $02 bridge answers
; $00 in all of them and would leave a $03 player in the FB mode reading a
; black picture.  Outside the FB mode nothing of $02 changed (the $03 is
; additive), so every table this player publishes with fb_state = 0 is byte
; for byte the v1.6 one -- tests/host/run_gbc_player.sh proves it against the
; phase-4/5 goldens.  See THE FRAMEBUFFER MODE below.
!GBC_VER      = $03

; ---------------------------------------------------------------------------
; Offline harness hook.  Default 0 = the device build; with it the file below
; assembles byte for byte as it did before this hook existed.
;
; A wrapper that sets !GBC_HARNESS = 1 before including this file replaces the
; only two things the FPGA provides -- the read-only view banks $E0-$E3 and the
; $EF write window -- with a golden image linked into bank $7F and a WRAM page,
; so an emulator can exercise the transfer engine against a known set.  The
; views are packed into the single bank $7F in the same order they occupy
; $E0-$E3, and nothing answers at $EF there, so the write window lands in WRAM
; and stays observable.  Everything actually under test (the register init, the
; DMA/HDMA programming, the fold/drain, the budget arithmetic) is the same code
; either way.
;
; HARNESS MAP (views.bin sections -> where the harness must load them):
;   $7F:0000  BGCH  12288    $7F:A000  CGRM    512
;   $7F:4000  OBCH  16384    $7F:A200  STAT    256
;   $7F:8000  MC0_   2048    $7F:A400  LOG_   2048
;   $7F:8800  MC1_   2048    $7F:B000  OAMV    544
;   $7F:9000  MK0_   2048    $7E:1000  $EF write window (6 bytes)
;   $7F:9800  MK1_   2048
; The harness ALSO has to model, at minimum: (a) general DMA (ch7) including
; a WRAM-port destination ($2180 with $2181-$2183, used by the status fetch),
; (b) the H/V latch ($2137 -> $213D twice) -- returning a V outside [41,225)
; or the guard rejects every transfer, and (c) writes to $EF landing in WRAM so
; the strobes can be counted.  To exercise the CARRY-OVER the harness must
; REWRITE the dirty bitmaps in the STAT image between GbcFrameOnce calls: the
; player clears only what it actually copied and the bridge is what would
; normally re-publish the rest.
; ---------------------------------------------------------------------------
!GBC_HARNESS ?= 0

if !GBC_HARNESS == 0
!GBC_BGCHR_A1B  = $E0
!GBC_BGCHR_A16  = $0000
!GBC_OBJCHR_A1B = $E0
!GBC_OBJCHR_A16 = $4000
!GBC_MAP_A1B    = $E1
!GBC_MAP_A16    = $0000
!GBC_CGRAM_A1B  = $E2
!GBC_CGRAM_A16  = $0000
!GBC_STAT_A1B   = $E2
!GBC_STAT_A16   = $0200
!GBC_LOG_A1B    = $E2
!GBC_LOG_A16    = $0400
!GBC_OAM_A1B    = $E3
!GBC_OAM_A16    = $0000
!GBC_EF         = $EF0000
!GBC_FB_A1B     = $E4      ; ⚡ wire $03: the framebuffer view (contract sec. 14.3)
else
!GBC_BGCHR_A1B  = $7F
!GBC_BGCHR_A16  = $0000
!GBC_OBJCHR_A1B = $7F
!GBC_OBJCHR_A16 = $4000
!GBC_MAP_A1B    = $7F
!GBC_MAP_A16    = $8000
!GBC_CGRAM_A1B  = $7F
!GBC_CGRAM_A16  = $A000
!GBC_STAT_A1B   = $7F
!GBC_STAT_A16   = $A200
!GBC_LOG_A1B    = $7F
!GBC_LOG_A16    = $A400
!GBC_OAM_A1B    = $7F
!GBC_OAM_A16    = $B000
!GBC_EF         = $7E1000
; ⚡ The framebuffer view is NOT relocated in the harness build: the A-bus of
; every C6 transfer is computed from this bank ($E4:{row, col, 0}), and a
; harness that moved it to WRAM would be testing a different address
; calculation from the one that reaches the cartridge.  m65816 serves bank $E4
; from a fifth view bank instead (m_set_view_window, 320 KB).
!GBC_FB_A1B     = $E4
endif

!GBC_STAT_LONG  = (!GBC_STAT_A1B<<16)+!GBC_STAT_A16

; --- $EF write window (contract sec. 3), as offsets for GbcEfPut -------------
!GBC_EFO_PADL    = $0000   ; $4218
!GBC_EFO_PADH    = $0001   ; $4219
!GBC_EFO_COMMIT  = $0002   ; latch the dirty bits -- exactly 1x per frame
!GBC_EFO_GO      = $0003   ; bit 0 = 1 releases the GB's reset
!GBC_EFO_SYNC    = $0004   ; genlock phase strobe -- 1x per frame
!GBC_EFO_CONSUME = $0005   ; end of the last read window -- 1x per frame
!GBC_EFO_MBX     = $0010   ; counter mailbox: 5 words, lo then hi, at +$10..+$19
                           ; (FRAMES DEFER DROPS READY REGSLATE).  Data, not
                           ; strobes: the bridge holds the last value for the
                           ; MCU (GBDG +20..+29), since nothing outside the
                           ; 5A22 can read SNES WRAM in this core.  Optional in
                           ; both directions (contract sec. 3/10): an old
                           ; bridge drops the stores, an old player never
                           ; makes them.
                           ; ⚡ wire $03: a sixth word, C6MODE, at +$1A/+$1B
                           ; (lo = fb_state, hi = ROW_DONEs of the frame).
!GBC_MBX_N       = 6
; ⚡ wire $03 (contract sec. 14.2): the two C6 registers.  Both go through
; GbcEfPut like every other store into the window.
!GBC_EFO_C6CTL   = $0006   ; C6_CTL: b0 HOLD, b1 FB_EN, b2 STRETCH_H, b3 CL_LOCK
!GBC_EFO_ROWDONE = $0007   ; ROW_DONE: 0..17 = "I read row r", $FF = every frozen row
!GBC_C6_FBEN     = $02
!GBC_C6_STRETCH  = $04

; --- status block, offsets inside the 256 B at $E2:0200 (contract sec. 5) ---
!GBC_ST_VER     = $00
!GBC_ST_FLAGS0  = $01   ; b0 LCD_ON b1 CGB_MODE b2 COMPAT b3 SPEED2X
                        ; b4 LOCKED b5 FIRST_FRAME b6 LOG_OVF b7 SNAP_VALID
!GBC_ST_LCDC    = $02
!GBC_ST_SCX     = $03
!GBC_ST_SCY     = $04
!GBC_ST_WX      = $05
!GBC_ST_WY      = $06
!GBC_ST_BGP     = $07
!GBC_ST_OBP0    = $08
!GBC_ST_OBP1    = $09
!GBC_ST_OPRI    = $0A
!GBC_ST_KEY1    = $0B
!GBC_ST_LYSYNC  = $0C
!GBC_ST_FRAME   = $0D
!GBC_ST_SEQ     = $0E   ; 2 B; unchanged = the snapshot was skipped (sec. 7)
!GBC_ST_DCHR    = $10   ; 6 B, 48 blocks of 256 B
!GBC_ST_DOBJ    = $16   ; 4 B, 32 blocks of 512 B
!GBC_ST_DMAP    = $1A   ; 8 B, 64 map rows ($9800 rows 0-31, $9C00 rows 32-63)
!GBC_ST_DMISC   = $22   ; b0 OAM b1 CRAM b2 REGS b3 RASTER b4 MSEL
!GBC_ST_LOGN    = $23
; ⚡ wire $03: the C6 bytes, +58..+91 (contract sec. 14.5).  LIVE in the
; bridge, not snapshot fields: ROW_FROZEN, C6_DIRTY and ROW_CLID of rows 0..7
; move INSIDE the read window (those rows freeze with the beam already past
; V=185), which is why GbcFbRow re-reads them from the bridge per row instead
; of trusting the copy the frame took at its start.
!GBC_ST_C6FLAGS = $58   ; b0 ALL_FROZEN b1 HOLD b2 FB_EN b3 STRETCH_H b4 CL_LOCK
                        ; b5 CL_PEND b6 ANY_FROZEN b7 = 1: a C6 bridge
!GBC_ST_C6FRAMES = $59  ; FB frames written, wrap 8 bits (LIVE)
!GBC_ST_ROWF    = $5A   ; 3 B: ROW_FROZEN[17:0]
!GBC_ST_C6CL    = $5D   ; b2:0 CL_CUR, b3 CL_ID, b6:4 CL_PREV
!GBC_ST_C6ROWDROP = $5E ; c6_rows_dropped, 8 bits
!GBC_ST_C6DIRTY = $60   ; 45 B: C6_DIRTY[359:0], bit 20r+c = cell c of row r
!GBC_ST_RCLI    = $8D   ; 3 B: ROW_CLID[17:0]
!GBC_ST_COLWN   = $90   ; 2 B: BCPD/OCPD writes of the last published frame

; status flag bits used below
!GBC_F_LCDON    = $01
!GBC_F_COMPAT   = $04
!GBC_F_LOCKED   = $10   ; genlock locked (LY=0 ~ V=182): the V-IRQ may open
!GBC_F_FIRST    = $20
!GBC_F_LOGOVF   = $40   ; the mid-frame log overflowed (> 512 entries)
!GBC_F_SNAPOK   = $80

; --- WRAM working set ($7E; the same addresses in both harness modes) -------
!GBC_WRAM_A1B     = $7E
; ⚡ wire $03: STILL 88 B, a page lower (it was at $1E00; the page is the FB
; mode's state now, see !GBC_FB_OFF).  None of the C6 bytes (+58..+91) is in
; the copy: they are LIVE in the bridge and every one is read live where it
; is used -- ROW_FROZEN, C6_DIRTY and ROW_CLID per row (GbcFbRow; the copy
; would be stale for rows 0..7 anyway), C6_FLAGS and COLW_N by GbcFbPolicy,
; C6_CL by the prologue and GbcFbRow (it moves only at a locked LY=0, which
; never falls inside a window, advisor A2).  Copying them cost 58 B of DMA in
; EVERY frame, FB mode or not, and that alone moved deferrals and guard
; refusals of the quad-layer against the $02 player (advisor C6-PLAYER).
!GBC_STATCOPY_OFF = $1D00       ; 88 B: the useful head of the status block.
!GBC_STATCOPY_LEN = $0058       ; 64 B would stop just short of COMPAT_RAW
                                ; (+$40..+$57), the twelve RAW compat colours
                                ; the bridge publishes in the block's reserved
                                ; area.  They are the ONLY place a compat BGP/
                                ; OBP write can get its colours from: every BG
                                ; entry of the CGRAM view has already been
                                ; through kB(k) = (BGP >> 2k) & 3, so when the
                                ; BGP in force is not a permutation the raw
                                ; colour a NEW BGP asks for is not in the view
                                ; at all.  24 bytes on a DMA that already pays
                                ; !GBC_DMACOST.
!GBC_SC           = $7E1D00     ; = !GBC_WRAM_A1B:!GBC_STATCOPY_OFF, spelled
                                ; out because asar parses a parenthesised
                                ; long base followed by ,x as an indirection

; ⚡ wire $03: the FRAMEBUFFER MODE's state (C6-SPEC.md b.1).  Right after the
; status copy, reached as bank-$00 low-RAM absolutes (DBR = 0 throughout).
; The direct page has no free byte left below $80 and everything from $80 up
; is the compiler's, so this lives in WRAM: ~4 cycles more per access, and
; only the drain touches it in a loop.
!GBC_FB_OFF       = $1DA0
!GBC_FB_LEN       = $0070
!GBC_FB_STATE     = !GBC_FB_OFF+$00  ; 0 off, 1 entering, 2 on, 3 leaving (C6MODE.lo)
!GBC_FB_HIST      = !GBC_FB_OFF+$01  ; the last 8 frames' press_in, one bit each
!GBC_FB_QUIET     = !GBC_FB_OFF+$02  ; frames in a row with press_out = 0, sat. 255
!GBC_FB_CUR       = !GBC_FB_OFF+$03  ; the round-robin cursor: a POSITION in
                                     ; GbcFbPRow (0 = row 8), PERSISTENT across
                                     ; frames (C6-SPEC b.3, advisor A1)
!GBC_FB_ROWSDONE  = !GBC_FB_OFF+$04  ; ROW_DONEs this frame (C6MODE.hi)
!GBC_FB_CLFIX     = !GBC_FB_OFF+$05  ; the CL_CUR $2132 was written for; $FF = never
!GBC_FB_EXITCNT   = !GBC_FB_OFF+$06  ; prologues spent in state 3
!GBC_FB_FLAGS     = !GBC_FB_OFF+$07  ; b0 the cut happened (the FB is on screen)
                                     ; b1 the white map is owed to window A
                                     ; b2 FB_EN = 0 is owed to window A
!GBC_FB_READY     = !GBC_FB_OFF+$08  ; 3 B: bit r = row r went up whole since FB_EN
!GBC_FB_DONEM     = !GBC_FB_OFF+$0B  ; 3 B: bit r = row r was released THIS frame
!GBC_FB_FRAMES    = !GBC_FB_OFF+$0E  ; 2 B: prologues with the FB on screen
!GBC_FB_MAPCL     = !GBC_FB_OFF+$10  ; 18 B: the CL each row's tilemap entries carry; $FF = never written
!GBC_FB_ENTRIES   = !GBC_FB_OFF+$22  ; times the mode was entered (8 bits)
!GBC_FB_RCDROP    = !GBC_FB_OFF+$23  ; the last completed compile's colour_dropped:
                                     ; b1 > 0 (press_out), b0 >= 32 (press_in)
!GBC_FB_RDTOTAL   = !GBC_FB_OFF+$24  ; 2 B: ROW_DONE strobes, ever
!GBC_FB_DROPS     = !GBC_FB_OFF+$26  ; 2 B: C6 transfers the V guard refused
!GBC_FB_MAPROWS   = !GBC_FB_OFF+$28  ; 2 B: tilemap rows written (CL changes + first uploads)
!GBC_FB_RESUME    = !GBC_FB_OFF+$2A  ; 18 B: first cell of the frozen row still owed
                                     ; after a refusal (0 = the whole row)
!GBC_FB_PEND      = !GBC_FB_OFF+$3C  ; 3 B: bit r = row r was released and may not
                                     ; have been revisited yet (THE PENDING ROWS)
!GBC_FB_VE        = !GBC_FB_OFF+$40  ; 18 words: body clock at which row r's
                                     ; revisit is surely over (GbcFbNow)
!GBC_FB_NOW       = !GBC_FB_OFF+$64  ; 2 B: the body clock the pass last read
!GBC_FB_DL        = !GBC_FB_OFF+$66  ; 2 B: the deadline in force (40; 16 while
                                     ; the second stage is on screen)
!GBC_FB_VL        = !GBC_FB_OFF+$68  ; 2 B: window B's first line (185; 209 ...)
!GBC_FB_POLEND    = !GBC_FB_OFF+$6A  ; 1 = window B of this frame ran the FB
                                     ; body (GbcIrqFbBody): so does the resume.
                                     ; 0 = the $02 body, and GbcFbPolicy runs
                                     ; at the end of the resume
!GBC_FB_CTL       = !GBC_FB_OFF+$6B  ; C6CTL the entry writes: !GBC_FB_CTLON1/2
!GBC_FB_STG       = !GBC_FB_OFF+$6C  ; 2 B: the stage this session runs, 0 =
                                     ; the first, 1 = the second (THE SECOND
                                     ; STAGE); set once at boot, never again.
                                     ; The high byte stays 0 so the flag reads
                                     ; the same with A 8 or 16 bits wide
!GBC_FB_ROWJ      = !GBC_FB_OFF+$6E  ; 2 B: where GbcFbRow goes after its
                                     ; ticket: GbcFbRow1 or GbcFbRow2


; Diagnostic counters, for the offline harness and for a USB read on silicon.
; All little-endian; `bytes` is 32 bits because one frame can move ~12 KB and a
; 16-bit total would wrap in five frames.
!GBC_CTR_OFF      = $1E80
!GBC_CTR          = $7E1E80     ; = !GBC_WRAM_A1B:!GBC_CTR_OFF (see above)
                                ; $1E00/$1E80 and not $1F00/$1F80: the stack
                                ; starts at $1FFF and the deepest call chain
                                ; measured leaves ~78 B between it and $1F80.
                                ; A page lower costs nothing and stops a future
                                ; frame from writing the counters with SP.
!GBC_CTR_LEN      = $0012
!GBC_CTR_FRAMES   = !GBC_CTR+$00   ; NMI bodies that ran to completion
!GBC_CTR_DMAS     = !GBC_CTR+$02   ; general DMAs fired
!GBC_CTR_BYTES    = !GBC_CTR+$04   ; bytes moved (32 bits)
!GBC_CTR_DEFER    = !GBC_CTR+$08   ; frames that ended with work still pending
!GBC_CTR_DROPS    = !GBC_CTR+$0A   ; transfers the V guard refused
!GBC_CTR_READY    = !GBC_CTR+$0C   ; frame number the screen was released on
!GBC_CTR_READYTO  = !GBC_CTR+$0E   ; 1 = it was !GBC_READY_MAX_FRAMES that
                                   ;     released the screen, not convergence
!GBC_CTR_CPUREV   = !GBC_CTR+$0F   ; $4210 bits 3:0, the 5A22 revision -- see
                                   ; the note on rev.1 in the header
!GBC_CTR_REGSLATE = !GBC_CTR+$10   ; frames whose GbcRegs was DEFERRED because
                                   ; it could no longer finish before V=0
                                   ; (GbcRegsSafe).  Appended: $1E90 was free

; Diagnostic counters of the RASTER COMPILER, one compile's worth (they are
; zeroed when a compile starts, not accumulated): the same quantities, under
; the same names, that the phase-4 golden publishes beside every table it
; emits.  All 16-bit little-endian.
!GBC_RCTR_OFF     = $1EA0
!GBC_RCTR         = $7E1EA0     ; = !GBC_WRAM_A1B:!GBC_RCTR_OFF
!GBC_RCTR_LEN     = $0020
!GBC_RC_REQ       = !GBC_RCTR+$00  ; colour_requested   CGRAM entries the log asked for
!GBC_RC_PLACED    = !GBC_RCTR+$02  ; colour_placed      ... that got a channel/line
!GBC_RC_DELAY     = !GBC_RCTR+$04  ; colour_delayed     ... not on the line they were born
!GBC_RC_DROP      = !GBC_RCTR+$06  ; colour_dropped     = requested - placed
!GBC_RC_NOOP      = !GBC_RCTR+$08  ; colour_noop        writes that move no view byte
!GBC_RC_LCDC      = !GBC_RCTR+$0A  ; lcdc_unsupported   per-line LCDC bits the
                                   ;                    tables cannot carry.  ⚡
                                   ;                    wire $02 took .3 and .4
                                   ;                    out of it (the mask is
                                   ;                    $C7 now, was $DF): b5 is
                                   ;                    window 1, b3/b4 the LCDC
                                   ;                    channel, and what is
                                   ;                    left is sec. 12.5
!GBC_RC_AFTER     = !GBC_RCTR+$0C  ; log_after_screen   ly_eff = 144
!GBC_RC_BADK      = !GBC_RCTR+$0E  ; log_bad_kind       kind >= 10
!GBC_RC_SPARE     = !GBC_RCTR+$10  ; spare_channels     ch0-3 the scroll left free
!GBC_RC_CCH       = !GBC_RCTR+$12  ; colour_channels    ... that got an entry
!GBC_RC_LOGN      = !GBC_RCTR+$14  ; entries actually compiled (LOG_N, clamped)
!GBC_RC_LROWS     = !GBC_RCTR+$16  ; lcdc_rows          visible rows whose LCDC
                                   ;                    differs from the
                                   ;                    snapshot in b3/b4
!GBC_RC_EVICT     = !GBC_RCTR+$18  ; win_carpet_scroll_evicted  0 or 1 (S13)
!GBC_RC_LBLANK    = !GBC_RCTR+$1A  ; lcdc_blank         0 or 1: the group would
                                   ;                    have been armed but the
                                   ;                    screen is the white one
!GBC_RC_CEVICT    = !GBC_RCTR+$1C  ; colour_channel_evicted  spares the LCDC
                                   ;                    group took from the
                                   ;                    colour pool (0 or 1)
!GBC_RC_ROWFIX    = !GBC_RCTR+$1E  ; segments a log outside sec. 8 asked to
                                   ;                    walk BACKWARDS, clamped
                                   ;                    to zero length

; The compile runs OUTSIDE the transfer window and may straddle frames, so the
; state it reads has to be a private copy: the live status block is rewritten
; by the next frame's fetch.  8 registers + two decoded flags + LOG_N, then the
; 24 COMPAT_RAW bytes.
!GBC_SNAP_OFF     = $1EC0
!GBC_SNAP         = $7E1EC0
!GBC_SNAP_LEN     = $0030
!GBC_SNAP_REGS    = $00         ; SCX SCY WX WY LCDC BGP OBP0 OBP1 (log-kind order)
!GBC_SNAP_COMPAT  = $08
!GBC_SNAP_BLANK   = $09
!GBC_SNAP_LOGN    = $0A         ; 2 B
!GBC_SNAP_RAW     = $10         ; 24 B: BG[0][0..3], OB[0][0..3], OB[1][0..3]

; HDMA tables live in WRAM because the HDMA reads the A-bus and the compiler
; rewrites them every frame (contract sec. 11.2/11.4: double buffer, each table
; wholly inside one bank).  The letterbox table is static and is copied into
; BOTH sets at boot; everything else is compiled.
;
; A set is a FIXED layout, because the prologue has to find every channel's
; table without knowing what the compile decided.  ch0-ch3 change role from
; frame to frame (scroll pair, window pair, or CGRAM colour), so each of them
; owns a slot big enough for the worst case of ANY role -- 4-byte groups, 144
; distinct lines: 5 (the top letterbox hold) + 1 + 127*4 + 1 + 17*4 + 5 + 1 =
; 589 bytes.  The window-1 table is 2-byte groups and worst-cases at 295.  The
; set header is what the prologue reads: the $420C mask the compile decided and,
; per channel, its B-bus address and the offset of the table it must point at
; (ch0/ch1 and ch2/ch3 carry IDENTICAL bytes when they are a scroll pair, so
; they are pointed at ONE table -- each channel has its own line counter and
; reading the same bytes twice a line is free).
!GBC_SETA_OFF     = $2000
!GBC_SETB_OFF     = $3000
!GBC_SET_LEN      = $1000
!GBC_WRAM_LONG    = $7E0000     ; = !GBC_WRAM_A1B:$0000 (see above)
!GBC_T_INIDISP    = $0000       ; ch5 table offset inside a set ($2100)
!GBC_H_MASK       = $0010       ; set header: the $420C mask this set implies
!GBC_H_PMASK      = $0011       ; ... and the same mask in PHYSICAL channels
                                ; (GbcHdmaMaskTab), what the prologue writes
!GBC_H_CH         = $0014       ; + ch*4: {bbad, table offset lo, hi, 0}
!GBC_T_W1         = $0100       ; ch4 table ($2126/$2127), 2-byte groups
!GBC_T_CH0        = $0300       ; ch0..ch3, 640 B each, 4-byte groups
!GBC_T_CH1        = $0580
!GBC_T_CH2        = $0800
!GBC_T_CH3        = $0A80
!GBC_T_SLOT       = $0280       ; 640: the 589-byte worst case with room to spare

; Working areas of the raster compiler.  All of bank $7E above the player's
; own set is free (the views live in the FPGA, not in WRAM), so these are sized
; for the contract's worst case rather than squeezed.
!GBC_LOG_OFF      = $4000       ; the copy of $E2:0400, 512 x 4 B
!GBC_LOG_MAX      = 512         ; contract sec. 8: past this the bridge drops
!GBC_ROWS_OFF     = $4808       ; digest: {row, SCX, SCY, WX, WY, LCDC} x 144.
                                ; $4800 is where the log's SENTINEL lands when
                                ; the log is full (see GbcPass1)
!GBC_ROWS_LEN     = 6
!GBC_CP_OFF       = $5000       ; the colour walk's per-spare state (4 x 11 B)
!GBC_CP_REQP      = !GBC_WRAM_LONG+!GBC_CP_OFF+$40  ; pass 1 -> colour walk:
!GBC_CP_NOOPP     = !GBC_WRAM_LONG+!GBC_CP_OFF+$42  ; requested/noop up to the
!GBC_CP_REQA      = !GBC_WRAM_LONG+!GBC_CP_OFF+$44  ; first ly >= 144, and over
!GBC_CP_NOOPA     = !GBC_WRAM_LONG+!GBC_CP_OFF+$46  ; the whole log
!GBC_CRAM_OFF     = $6000       ; the CGRAM view, copied for the colour replay

; Frames the boot waits for the bridge to answer before giving up (~3 s).
!GBC_LIFEWAIT     = 180

; ---------------------------------------------------------------------------
; Budget / deadline constants.
;
; The four marked PROVISIONAL are the contract's sec. 11.1 figures, carried
; over from the A26 player where they were measured.  HOW TO RE-MEASURE them
; against this binary: (1) !GBC_DMACOST is what a fired DMA costs the FRAME --
; sample GbcVCount immediately before and after one drain pass with a known
; byte count and solve cost = (dV*1364/8 - bytes)/n_dma; (2) !GBC_DMASTART is
; the engine's own start-up only (~324 mc = 41 eq, hardware constant, leave it);
; (3) !GBC_GUARDSLACK is the guard-accept -> $420B latency, same measurement
; with n_dma = 1 and the guard taken as t=0; (4) !GBC_FIRESLACK is what the run
; builder burns between sizing and firing -- sample V at GbcRunCeil and again
; at GbcRunFire on a 48-block run.  Raise both slacks until the last DMA of a
; worst-case frame is off the bus before line 41 with margin; they are the two
; numbers that trade throughput for the silent-corruption failure mode.
!GBC_BUDGET     = 12600 ; window A, 2 HDMA channels (contract sec. 11.1 table)
!GBC_BUDGETB    = 6280  ; window B (V=185..224, 2 channels): the sec. 11.1
                        ; formula, (40*1364 - 40*58 - 2000)/8.  Same role as
                        ; !GBC_BUDGET -- a model line; the live-V capacity is
                        ; what binds (see GbcCapWinB)
; The V-IRQ (phase 5).  !GBC_VIRQ = 0 assembles the phase-4 player: the vector
; stays a stub and $4200 never gets bit 5.
!GBC_VIRQ       = 1
!GBC_VIRQ_LINE  = 185   ; first line of the bottom letterbox: ch5 wrote $80 for
                        ; it in the hblank of line 184, AFTER the last visible
                        ; pixel of the Game Boy's image (V=41..184).  184 would
                        ; land on a visible line.
!GBC_VBLANK     = 225   ; first vblank line = the NMI = where window B ends
!GBC_REGS_LAST  = 254   ; last line GbcRegs may START on (GbcRegsSafe)
!GBC_VIRQ_LYSYNC = 25   ; lowest LY_SYNC that opens window B: a locked genlock
                        ; reads exactly 25 (ALVO = 11.525 dots), see
                        ; GbcVirqWanted
!GBC_BUILDEQ    = 160   ; byte-equivalents one GbcRunAdd iteration costs in
                        ; TIME (~1240 mc on the clocked host model, see
                        ; GbcRunAdd); window B charges it per block
!GBC_DMASTART   = 41    ; PROVISIONAL: what the DMA engine adds to a transfer
!GBC_CGFITSLACK = 256   ; the CGRAM vblank fit's allowance for the CPU between
                        ; its V latch and the $420B (GbcDrainCgram)
!GBC_DMACOST    = 384   ; PROVISIONAL: what a fired DMA costs the frame
!GBC_GUARDSLACK = 1024  ; MEASURED (bsnes-plus, see the deadline note above):
                        ; 512 left a worst margin of -104 to -136 byte-
                        ; equivalents, i.e. one or two DMAs a frame finishing
                        ; PAST the deadline -- 36 bytes of BG chr silently
                        ; dropped at $066DC, which the VRAM comparison did not
                        ; catch because those bytes happened to be zero and the
                        ; boot had already zeroed VRAM.  1024 puts the worst
                        ; margin at +394 to +448 across every scenario measured
                        ; (including STORM: 806 DMAs, 772 KB, 126 guard
                        ; refusals, ZERO drops).  It is all player-side latency
                        ; the model does not carry: since line 40 became $00
                        ; the deadline is exactly the start of a line, so there
                        ; is no sub-line correction folded in here any more.
!GBC_FIRESLACK  = 640   ; PROVISIONAL: held back on top when SIZING a run
!GBC_V0SPIN     = 2048  ; GbcWaitInitW's bound, in $4212 polls (~18 a line):
                        ; past a PAL vblank -- only a V that never moves (the
                        ; host harness pins it) reaches it
!GBC_WAITW      = $7E1D58 ; GbcWaitInitW is copied here at boot: the gap
                          ; between the status copy and the FB block (both
                          ; asserted below GbcWaitInitWEnd)
; Safety net for the screen release.  GbcReadyCheck normally waits for CGRAM +
; OAM + all four map views to be drained at least once, which every real
; workload reaches within a handful of frames.  It is not GUARANTEED to: a game
; that re-dirties all four maps every single frame keeps the backlog non-empty
; forever (the bsnes-plus STORM scenario does exactly that, artificially), and
; the player would sit in forced blank for good.  A game like that is going to
; show a partial picture in steady state ANYWAY -- the re-render is always one
; frame behind -- so "partial and self-healing" beats "black forever".  After
; this many frames WITH A VALID STATUS BLOCK the release is taken regardless of
; what is still pending, and !GBC_CTR_READYTO records that it was the net and
; not convergence that armed it, so bringup never reads "it converged" into
; "it gave up waiting".  16 frames ~ 267 ms: long enough that no normal boot
; reaches it (measured: cgb-acid2, dmg-acid2, window and lcd_off all arm by
; frame 6), short enough not to look like a hang.
!GBC_READY_MAX_FRAMES = 16
!GBC_DEADLINE   = 40    ; first line the letterbox HDMA hands the screen back
                        ; (line 40 = $00, screen on at brightness 0, so the PPU
                        ; prepares line 41's sprites -- see GbcTblInidisp)
!GBC_BPLVBL     = 170   ; bytes/line with no HDMA running (1364 mc / 8)
; Lines in an NTSC frame.  Everything about the window is NTSC: on a PAL
; console (312 lines) V = 225..261 is not the end of the frame at all, the
; subtract below borrows and the vblank term claims ZERO whole lines, which is
; conservative and never unsafe (see GbcCapVblOk and GbcDrainCgV).
!GBC_LINES      = 262
; Bytes/line over V=0..39 with the HDMA taking its cut.  Contract sec. 11.1
; charges `18 + 8C + 8B` master cycles a line for C active channels moving B
; bytes, which with 8 mc to the byte-equivalent is exactly
;   BPLHDMA = (1364 - 18 - 8*(C + B)) / 8 = 168 - (C + B).
; ch5 contributes C+B = 2, ch4 3 and the ch0 decoy 2 (one channel, one byte,
; charged on every line like the others), so the always-on set gives
; 168 - 7 = 161.  Every logical channel 0..3 the raster compiler arms adds 5
; more (one channel, four bytes), so the tail is worth 161, 156, 151, 146 or
; 141 bytes a line depending on how many of the four are up.  It is a CEILING in the pessimistic direction: the model charges B on
; every line, while the top letterbox is one hold run per channel and a hold
; entry transfers only on the first line of its run (contract sec. 11.4).
; $70/$71 carry the pair the published set implies; these two are the boot
; values and the row 0 of the tables below.
!GBC_BPLHDMA    = 161
!GBC_TAILBYTES  = 6440  ; = !GBC_DEADLINE * !GBC_BPLHDMA
; The FB mode's ch4 + ch5 set has no decoy: ch4 is its lowest channel and its
; table (GbcTblW1, the window shut on every line) means the same thing frozen
; or not, so it takes the hit at no cost.  168 - 5.
!GBC_BPLHDMA_FB = 163
!GBC_TAILBYTES_FB = 6520 ; = !GBC_DEADLINE * !GBC_BPLHDMA_FB
; The physical $420C mask last written, so a general DMA can put it back (see
; THE 5A22 TAKES THE LOWEST HDMA CHANNEL).  A free byte between the FB block
; ($1DA0+$70) and the counters ($1E80).
!GBC_HDMAEN_SH  = $7E1E10
; Frames the white screen is still held after the LCD came back (GbcWhiteHold).
!GBC_WHITE_CNT  = $7E1E11
!GBC_WHITE_HOLD = 8     ; the cap: ~133 ms, then the picture whatever is owed
!GBC_WHITE_FIRST = $7E1E12 ; F_FIRST while held, else 0: OR'd into the flags

; ---------------------------------------------------------------------------
; The framebuffer mode's policy (contract sec. 14.10, advisor A3).
; ---------------------------------------------------------------------------
!GBC_FB_COLW_IN  = 256  ; press_in : LOG_OVF | COLW_N >= this | RC_DROP > 0.  96
                        ; would switch 47 of the 409 clean/cosmetic titles of
                        ; the census into 3:3:2; 256 leaves 3 (advisor A3)
!GBC_FB_COLW_OUT = 96   ; press_out: LOG_OVF | COLW_N >= this | RC_DROP > 0
!GBC_FB_RCDROP_IN = 32  ; press_in's colour_dropped threshold.  ⚡ The literal
                        ; sec. 14.10 "RC_DROP > 0" switched 28 of the 409
                        ; clean/cosmetic titles of the census into 3:3:2; 32
                        ; leaves 17 (C6-GOLDEN, coordinator's decision).
                        ; press_out keeps "> 0": it only extends a session
!GBC_FB_PRESS_N  = 6    ; enter on press_in in >= 6 of the last 8 frames
!GBC_FB_LYSYNC   = 25   ; ⚡ advisor C6-PLAYER: the FB mode needs LY_SYNC ==
                        ; 25, not the V-IRQ's ">= 25".  ">=" only says LY=0
                        ; fell at or before V~182; in Exato (LOCKED = 1 by
                        ; construction) LY=0 drifts ~1.6 lines a frame through
                        ; the whole frame and ">= 25" holds for ~85 % of it,
                        ; while the pending rows (GbcFbRevisit) and the row
                        ; order assume LY=0 at V 182.35 to the line: on the
                        ; host (--ly0-drift 1.6 --locked 1) the mode came on
                        ; and ROW_DONE landed on OPEN rows (fresh-mod,
                        ; cpu-static).  A locked genlock reads exactly 25
                        ; (contract sec. 5 +0C, ALVO = line 25 + 125 dots
                        ; +-64); Exato meets it ~1 frame per drift cycle, too
                        ; short for the cut (every row up once).
!GBC_FB_QUIET_N  = 90   ; leave after this many frames with press_out = 0 (1.5 s)
!GBC_FB_EXIT_MAX = 2    ; state 3 lasts at most this many prologues: the FB
                        ; stays on screen while OAM/CGRAM catch up (advisor A7)
!GBC_FB_GAP      = 6    ; clean cells a span may bridge (64 B each; the DMA
                        ; it saves costs !GBC_DMACOST = 6 x 64): C6-SPEC b.3
!GBC_FB_SPANEQ   = 256  ; what a span charges the ticket on top of its bytes
                        ; (GbcFbDrain): the DMA's start-up and the ~50
                        ; instructions between two spans, ~1300 mc on the host
                        ; clock -- 2048 mc of room, i.e. 1.5x CPU cost
!GBC_FB_CTLON1   = $02  ; C6CTL with the mode on, first stage: FB_EN
!GBC_FB_CTLON2   = $06  ; ... second stage: FB_EN | STRETCH_H.  BOTH stages
                        ; are in every image; which one a session runs is
                        ; the GBCF block's +5 (GbcCfg, written by the MCU),
                        ; read ONCE by GbcFbInit into !GBC_FB_STG/!GBC_FB_CTL
                        ; (see THE SECOND STAGE)
!GBC_FB_VIRQ2    = 209  ; second stage: the letterbox of 16/16 starts here
!GBC_FB_DEADLINE2 = 16  ; ... and ends here (C6-SPEC a.3, advisor A5)
!GBC_FB_BPL2     = 160  ; ch6 (mode 2) + ch4 + ch5: 168 - (3 + 5)
!GBC_FB_TAIL2    = 2560 ; = !GBC_FB_DEADLINE2 * !GBC_FB_BPL2
!GBC_FB_MAPWORD2 = $6800 ; second stage: the tilemap (byte $D000)
!GBC_FB_MAPEQ2   = 432  ; its 32-byte DMA + 32 CPU stores
!GBC_FB_MAPEQ    = 276  ; what GbcFbMapRow asks the guard for: the 20-byte
                        ; DMA plus the 20 CPU stores of the high bytes, which
                        ; are VRAM writes too (~1250 mc on the host clock)
!GBC_FB_MAPWORD  = $5000 ; VRAM word of the FB tilemap AND of tile 0 of its
                        ; chr base (byte $A000, contract sec. 14.8); cell c is
                        ; tile k(c) = c + 18 + [c >= 46] -- the first 18 tiles
                        ; ARE the tilemap and tile 64 is the solid tiles

; ---------------------------------------------------------------------------
; Transfer classes.  Every one of them is "N fixed-size blocks, contiguous in
; the view AND contiguous in VRAM", which is what lets one generic run builder
; drive all six.  See GbcClsTab for the numbers.
; ⚡ wire $02 split the BG chr view into SIX of them.  The view itself did not
; change -- it is still the Game Boy's 12 KB in the bridge's block order
; ($8000, $8800, $9000 of bank 0, then of bank 1) -- but the VRAM it goes to
; is now two 512-tile chr bases, and the $8800 block of each bank belongs to
; BOTH of them.  A class has to be contiguous in the view AND in VRAM, and
; these are exactly the six pieces that are:
;
;   class      GB blocks   view bytes    VRAM bytes   what it is
;   CHRA0       0..15      $0000-$0FFF   $4000-$4FFF  base A, tiles   0..255
;   CHRA1      24..39      $1800-$27FF   $5000-$5FFF  base A, tiles 256..511
;   CHRB0      16..23      $1000-$17FF   $6000-$67FF  base B, tiles   0..127
;   CHRD0       8..15      $0800-$0FFF   $6800-$6FFF  base B, tiles 128..255
;   CHRB1      40..47      $2800-$2FFF   $7000-$77FF  base B, tiles 256..383
;   CHRD1      32..39      $2000-$27FF   $7800-$7FFF  base B, tiles 384..511
;
; CHRD0/CHRD1 are the SECOND copy of the $8800 block.  They carry their own
; backlog byte ($3B/$3C) instead of sharing the main one, because a class
; clears the bits it sent and two classes sharing bits would race: whichever
; ran first would clear them and the other would never transfer.  The fold ORs
; the same incoming DIRTY_CHR bits into both.
!GBC_CLS_CHRA0  = 0     ; BG chr base A, blocks  0..15 -> VRAM $4000
!GBC_CLS_CHRA1  = 1     ; BG chr base A, blocks 24..39 -> VRAM $5000
!GBC_CLS_CHRB0  = 2     ; BG chr base B, blocks 16..23 -> VRAM $6000
!GBC_CLS_CHRD0  = 3     ; BG chr base B, blocks  8..15 -> VRAM $6800 (duplicate)
!GBC_CLS_CHRB1  = 4     ; BG chr base B, blocks 40..47 -> VRAM $7000
!GBC_CLS_CHRD1  = 5     ; BG chr base B, blocks 32..39 -> VRAM $7800 (duplicate)
!GBC_CLS_OBJ    = 6     ; OBJ chr, 32 blocks of 512 B ($E0:4000 -> VRAM $0000)
!GBC_CLS_MAPC0  = 7     ; content of map $9800, 32 rows -> VRAM $8000
!GBC_CLS_MAPC1  = 8     ; content of map $9C00, 32 rows -> VRAM $8800
!GBC_CLS_MAPK0  = 9     ; carpet of map $9800,  32 rows -> VRAM $9000
!GBC_CLS_MAPK1  = 10    ; carpet of map $9C00,  32 rows -> VRAM $9800
!GBC_CLS_N      = 11
; ⚠ A CLASS IS CAPPED AT 32 BLOCKS, AND THAT IS A TIMING RULE, NOT TIDINESS.
; The run builder is O(blocks) in INSTRUCTIONS -- GbcRunAdd walks one block at
; a time to apply the coalescing rule, and GbcRunTrim and GbcRunFireTry walk
; back down the same way -- and on this CPU ~25 instructions is about two
; thirds of a scanline.  The content maps ($9800's rows then $9C00's) are
; contiguous in the view AND in VRAM, so ONE class of 64 rows would be legal;
; measured in bsnes-plus, its build loop alone ran 42 scanlines, the capacity
; it had been sized against was gone by the time the run was ready, and the
; class shrank itself to nothing and transferred NOTHING, every frame, for
; ever.  32 blocks is what wire $01 ran and what the window affords.

; ---------------------------------------------------------------------------
; Direct page map (DP = 0 throughout)
; ---------------------------------------------------------------------------
;   $00      the PROLOGUE's: $73 b2 of the set it published last
;   $01      the PROLOGUE's: how many times a colour channel left the bus
;   $02      the body's: the $01 GbcFold last answered with a CGRAM re-send
;   $03      NMI re-entrancy latch (0 = not inside the body)
;   $04      IGR edge latch (1 = a combo already fired during this hold)
;   $05      1 = the bridge answers with our wire version
;   $06      screen release: 0 = not yet, 1 = decided, the next prologue
;            arms $420C, 2 = armed
;   $07      previous WX (window-1 table change detector).  ⚡ wire $02 freed
;            it: the white screen has its own map in free VRAM now, written
;            once at init, so there is no "requested / painted" state left
;   $08-$09  per-frame budget ceiling, byte-equivalents
;   $0A      1 = the next fold marks EVERYTHING dirty
;   $0B-$0C  previous SEQ
;   $0D      class rotation start, advanced every frame (was $16, which the
;            seventh class cursor took)
;   $0E-$0F  pad shadow {$4218, $4219} as one 16-bit word (= {A X L R ....}
;            in the low byte, {B Y Sel St Up Dn Lf Rt} in the high), which is
;            the IGR's compare operand
;   $10-$14  scratch of the NMI prologue and of GbcRasterArm, which never
;            overlap in time (the arm runs after the prologue has returned):
;            $10-$11 the base of the set being published / $10 = GbcRasterArm's
;            "something the tables depend on moved" flag, $12 the B-bus address
;            of the channel being armed, $13 the channel the loop is on, $14
;            how many of ch0..ch3 the set arms -- the index into the budget
;            tables.  ⚠ The prologue may now interrupt the V-IRQ's half of the
;            frame (see TWO WINDOWS), so nothing GbcIrqBody runs may own
;            $10-$14 -- it does not: the fold, OAM and the block walk live in
;            $17-$6F, and GbcRasterArm (the other owner) stays in the NMI half
;   $15      V-IRQ/NMI handoff token: 0 = the frame was not opened by the
;            V-IRQ, 2 = opened and neither half done, 1 = one half done; the
;            half that decrements it to 0 runs GbcFrameResume
;   $16      V-IRQ half state: b0 = window B is running (GbcCapacity offers
;            only the lines before V=225, and running out is a pause, not a
;            drop), b1 = it got as far as the fold, so the resume owes the
;            registers, the log, CGRAM and the rest of the walk
;   $17      drain abort flag
;   $18      HDMA set being written this frame (0 = A, 1 = B)
;   $19      parked IGR command byte
;   $1A-$1B  scanline scratch for the 9-bit V read / capacity arithmetic
;   $1C-$1D  bytes of the DMA the guard is being asked about
;   $1E-$1F  budget left this frame
;   $20-$25  backlog: BG chr, 48 bits (the bridge's own block numbering, and
;            the four base-A/base-B chr classes index into it at their own
;            offsets: $20, $23, $22, $25)
;   $26-$29  backlog: OBJ chr, 32 bits
;   $2A-$2D  backlog: content of map $9800   $2E-$31  ... of map $9C00
;   $32-$35  backlog: carpet of map $9800   $36-$39  ... of map $9C00
;            (the carpets carry the SAME DIRTY_MAP bits as their content twin)
;   $3A      backlog: b0 OAM, b1 CGRAM
;   $3B      backlog: the duplicate of the $8800 chr block of bank 0
;   $3C      backlog: ... and of bank 1 (8 bits each; see !GBC_CLS_CHRD0)
;   $3D      previous LCDC (window-1 table change detector; WX is $07, WY $6F)
;   $3E      b0 = re-send OAM/CGRAM this frame (the snapshot advanced, or a
;            forced refresh), b1 = the snapshot REALLY advanced (SEQ moved) --
;            the V-IRQ gate reads b1 (GbcVirqWanted)
;   $3F      1 = something was deferred this frame
;   $40-$41  class: backlog DP base (word)
;   $42-$43  class: cursor DP address (word)
;   $44      class: block count
;   $45      bit mask scratch (GbcBlockBit output)
;   $46-$47  class: block size in bytes
;   $48-$49  class: source 16-bit base
;   $4A      class: source bank
;   $4B      class: clean blocks that may be bridged inside one run
;   $4C-$4D  class: VRAM word base
;   $4E      class: block -> source byte shift
;   $4F      class: block -> VRAM word shift
;   $50      scan cursor (block index)
;   $51      last block of the segment being scanned
;   $52      first block of the run being built
;   $53      blocks in the run
;   $54-$55  bytes in the run
;   $56-$57  ceiling the run is being sized against
;   $58-$59  16-bit scratch -- CLOBBERED BY EVERY DMA (GbcClsDma's shift
;            operand, GbcDebit's byte count), so nothing that has to outlive a
;            transfer may live here
;   $5A      trailing clean blocks in the run / clear-run counter
;   $5B-$5E  scratch of GbcRegs: the snapshot's LCDC, the TM value, and the two
;            window scroll offsets GbcScrollWin hands to its two halves
;   $5F-$60  DMA source 16-bit offset
;   $61      block the current class walk began at (survives every DMA of
;            segment 1; segment 2 is [0, $61-1])
;   $62-$63  backlog byte address the segment scan is holding
;   $64-$65  DMA VRAM word destination
;   $66      class loop counter: classes the frame's walk has still to visit
;   $67      class walker (the class the rotation is on).  Both survive from
;            window B to window A: the NMI's resume carries on with the same
;            walk (GbcDrainWalk), it does not start a second one
;   $68-$69  window-1 table write cursor / emit scratch
;   $6A-$6B  window-1 emit data bytes
;   $6C      GbcClsEmpty's backlog byte counter
;   $6E      previous white/live state, for the window-1 change detector
;   $6D      frames with a valid status block since boot, capped at
;            !GBC_READY_MAX_FRAMES (the screen-release safety net)
;   $6F      previous WY (window-1 table change detector; see $07)
;
; --- $74-$7E: ONE round-robin cursor per transfer class, persistent ---------
; ⚡ Eleven of them, one per class, never shared.  A duplicate chr class
; sharing the cursor of the class it duplicates looked free -- the two carry
; the same bits -- but they have different block counts, and GbcDrainCls
; writes a clamped cursor BACK, so every visit of the eight-block duplicate
; reset the fairness phase of the sixteen-block class.
; $7F  the $4200 value with the V-IRQ ($A1) when this console may have it,
;      0 when it may not (rev.1 5A22, PAL, !GBC_VIRQ = 0) -- GbcVirqInit
;
; --- published HDMA state: what the LAST prologue put on the bus ------------
; = !GBC_DP_HDMA, exported through GbcHarnessMap.
;   $70      bytes/line the tail is worth with the channels now armed
;   $71-$72  !GBC_DEADLINE * $70, the tail term of GbcCapacity
;   $73      b0 = ch0/ch1 are the BG scroll pair, b1 = ch2 is driving BG1 (the
;            window's CONTENT), b2 = at least one channel is writing CGRAM,
;            b3 = ch3 is driving BG3 (the window's CARPET), b4 = a channel is
;            driving the LCDC group.  ⚡ b1 and b3 were ONE bit in wire $01:
;            the window pair is always armed together EXCEPT when the LCDC
;            channel evicts ch3 (TABLE SPEC S13), and then BG3 has to be
;            written by GbcRegsScroll while BG1 must not be
;
; --- the raster compiler ----------------------------------------------------
; Everything from $80 up belongs to the compiler, which runs OUTSIDE the NMI
; and may be interrupted by it half way through: the frame body must not touch
; any of it (the one exception is GbcRasterArm, which only runs when the state
; below says no compile is in flight).
;   $80      state: 0 idle, 1 armed, 2 running, 3 done (waiting for a prologue)
;   $81      the set the compile is writing (0 = A, 1 = B).  Written by
;            GbcRasterRun AFTER it has put 2 in $80, which is what freezes
;            $18 -- see GbcRrGo
;   $82      the HDMA sets that still owe the current tables: b0 = set A,
;            b1 = set B (a MASK, not a count -- see GbcRrGo)
;   $83      1 = the compile needs the CGRAM view copied into WRAM
;   $84      1 = ... and this frame's window copied it
;   $85      1 = the previous frame's log was not empty
;   $86      the channel the LCDC group got, $FF = none this frame (S12/S13)
;   $87      the $2109 byte of the LCDC group: the SNAPSHOT's LCDC.6, constant
;            for the whole frame (S12 -- it is in the group only because mode 4
;            needs four adjacent registers)
;   $88-$8F  the live registers of the walk, INDEXED BY LOG KIND:
;            SCX SCY WX WY LCDC BGP OBP0 OBP1
;   $90      compat, $91 blank screen (LCD off or FIRST_FRAME)
;   $94-$95  rows in the digest
;   $96-$97  the set being written
;   $98-$99  pass 1: the row pending in the digest / later: a segment's start
;   $9A-$9D  the group being offered to an encoder
;   $A8      spare channels (after GbcP1Spares)
;   $A9      b0 the log carries SCX/SCY, b1 it carries WX/WY, b2 BCPD, b3 OCPD,
;            b4 any palette write, b5 the window pair lost ch3 to the LCDC
;            group (S13), b6 some visible row moves LCDC.3/.4
;   $B0-$DF  pass 2 (GbcPassRegs): four RUN-LENGTH ENCODERS, 12 bytes each
;            (see !ENC_*): W1, the BG pair, the window pair, the LCDC group
;   $E0-$E3  the channel number of spare 0..3
;   $E4-$E7  1 = that spare received at least one entry
;   $F0-$F2  a long pointer to $7E:0000, so the encoders can store through
;            [dp],y with the 16-bit table cursor in Y (there is no
;            absolute-long,Y addressing mode).  The frame body reads it too:
;            nothing may ever write it but the boot
; ⚡ P5B: everything else from $87 up is REUSED pass by pass -- pass 1 (!P1_*),
; pass 2 (!PR_*) and the colour walk (!CP_*/!CS_*, which also takes $B0-$DB
; for its four spares once pass 2 has closed its encoders, and $F4-$FF, which
; pass 1 ($F4) and pass 2 ($F8-$FD, S14) also borrow and leave dead) each name
; what they own next to their code.  Only what is
; listed above outlives the pass that wrote it: the state $80-$86, the two
; masks, the set, the spare list and the digest's length.

; The published HDMA state is the one block of direct page the OFFLINE HARNESS
; has to find (it is the budget the V guard divides the window with), so it is
; named and exported through GbcHarnessMap instead of being spelled $70 at
; every site -- a raw number there and a copy of it in the harness would drift.
!GBC_DP_HDMA = $70      ; +0 bytes/line, +1..+2 tail, +3 published roles

; One run-length encoder.  X holds slot*12 throughout, so every field below is
; reached as `dp,x`; slot 0 is also the scratch the single-table passes use.
!ENC_BASE  = $B0
!ENC_STRIDE = 12
!ENC_CUR   = !ENC_BASE+$00      ; 2 B: where the next byte of the table goes
!ENC_HDR   = !ENC_BASE+$02      ; 2 B: the header byte of the open $80|k batch
!ENC_K     = !ENC_BASE+$04      ; groups in that batch; 0 = no batch open
!ENC_GSZ   = !ENC_BASE+$05      ; 1, 2 or 4 bytes per group
!ENC_RUN   = !ENC_BASE+$06      ; lines of the run being accumulated
!ENC_GRP   = !ENC_BASE+$07      ; 4 B: the group that run carries
!GBC_PTR   = $F0

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

    ; The menu leaves WRAM as it found it, so every gate byte that is read
    ; before it is written has to be zeroed by hand.
    ldx.w #$0000
    lda.w #$0000
GbcZeroDp:
    sta.b $00,x
    inx
    inx
    cpx.w #$0100
    bne GbcZeroDp
    sep #$20

    ; The encoders store through [dp],y because the 65816 has no
    ; absolute-long,Y: the table cursor has to be the 16-bit index, and only
    ; the indirect-long form takes Y.
    lda.b #$00
    sta.b !GBC_PTR
    sta.b !GBC_PTR+1
    lda.b #!GBC_WRAM_A1B
    sta.b !GBC_PTR+2

    ; GbcWaitInitW runs from WRAM (see GbcDmaGuard)
    ldx.w #$0000
GbcWaitWCopy:
    lda.l GbcWaitInitWSrc,x
    sta.l !GBC_WAITW,x
    inx
    cpx.w #(GbcWaitInitWEnd-GbcWaitInitWSrc)
    bne GbcWaitWCopy

    lda.b #$01
    sta.b $04           ; IGR edge latch, born ARMED: a combo still held
                        ; through the $80 reset must be released before it
                        ; can fire again (see the IGR block in the NMI)
    lda.b #$01
    sta.b $0A           ; the first fold is a full refresh
    lda.b #$03
    sta.b $82           ; and the raster tables are owed to both HDMA sets
    lda.b #!GBC_BPLHDMA
    sta.b !GBC_DP_HDMA  ; the boot set has ch4 + ch5 only
    rep #$20
    lda.w #!GBC_BUDGET
    sta.b $08
    lda.w #!GBC_TAILBYTES
    sta.b !GBC_DP_HDMA+1
    sep #$20

    jsr GbcCtrClear
    jsr GbcFbInit       ; the framebuffer mode starts OFF, every row unmapped
    lda.b #$00          ; the V-IRQ gates on the LOCKED bit of the last status
    sta.l !GBC_SC+!GBC_ST_FLAGS0 ; copy, and WRAM holds whatever the menu left
                        ; ($55 has bit 4 set): no copy yet = not locked
    ; 5A22 revision, for the silicon report.  A general DMA that runs while
    ; HDMA is active -- which is every transfer this player makes over
    ; V=0..39 -- and the HDMA init at V=0 are the pair that a rev.1 CPU
    ; corrupts or hangs on; rev.2 and the 1-CHIP pause the DMA and are fine.
    ; Same exposure as the A26 player.  Nothing is done conditionally on it
    ; yet: it is published so a field report can say which silicon it came
    ; from before anyone starts guessing.  Mitigation if it ever bites: offer
    ; vblank-only capacity (!GBC_TAILBYTES = 0) when rev == 1.
    lda.w $4210
    and.b #$0F
    sta.l !GBC_CTR_CPUREV
    jsr GbcInit         ; the whole PPU + VRAM/OAM/CGRAM + HDMA tables + APU

    jsr GbcLifeWait     ; carry clear = the bridge published our wire version
    bcs GbcResetDead

    lda.b #$01
    sta.b $05
    jsr GbcGo           ; release the GB's reset (contract sec. 10)

GbcResetDead:
    ; The degraded path deliberately falls through with $420C = 0 and $2100
    ; still $8F: no core answering means every view reads back as $00, and a
    ; picture drawn out of that would be indistinguishable from a core bug.
    ; The NMI is enabled either way -- it costs nothing when the writes go
    ; nowhere, and it is what keeps the IGR combo alive so the user can get
    ; back to the menu instead of power-cycling.
    ;
    ; NOTE that $420C stays 0 even on the GOOD path: the letterbox HDMA is what
    ; would turn the screen on at line 41, and it is armed only once the first
    ; full CGRAM + OAM + map drain has landed (GbcReadyCheck).  Until then the
    ; picture would be a half-built one.
    jsr GbcVirqInit     ; A(8) = $81, or $A1 with the V-IRQ of phase 5
    sta.w $4200         ; NMI + auto-joypad (+ V-IRQ)
    cli                 ; only the V-IRQ can pull /IRQ: the core ties the cart
                        ; line off (main.v: SNES_IRQ = 0), and with $4200 b5
                        ; clear nothing asserts it at all

; The frame body owns the NMI (and, with the genlock locked, the V-IRQ that
; opens it); the RASTER COMPILER owns everything else.
; It runs here, in the main loop, and not at the end of the body on purpose:
; a compile of the contract's worst case (512 log entries, four colour
; channels) is longer than one frame, and an NMI that had not returned by the
; time the next one arrives loses that frame's SYNC/COMMIT/CONSUMED -- the
; genlock's phase reference among them.  Out here the NMI simply interrupts
; it: the compiler keeps its whole state in direct page $80-$FF and writes
; only the HDMA set the body is NOT publishing, so the work resumes where it
; left off and the picture keeps the tables it already had.  The V-IRQ half
; of the body obeys the same rules (it never touches $80-$FF, the log, the
; CGRAM copy or the HDMA sets).
MainLoop:
    wai
    jsr GbcRasterRun
    bra MainLoop

; ===========================================================================
; NMI -- one frame.  The body is a subroutine so the offline harness can drive
; it directly; everything that must happen even on a re-entered frame (the
; acknowledge) lives out here.
; ===========================================================================
NMI:
    rep #$30
    pha
    phx
    phy
    sep #$20

    jsr GbcNmiBody

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
; IRQ -- the V-IRQ at !GBC_VIRQ_LINE (phase 5).  The only IRQ source there is:
; the core never asserts the cartridge /IRQ, and the H-IRQ is off ($4200 b4).
; $4211 is read FIRST: TIMEUP holds /IRQ low until it is read, and an IRQ
; returned from with the flag still up is taken again on the RTI.
; ===========================================================================
IRQ:
    rep #$30
    pha
    phx
    phy
    sep #$20

    lda.w $4211         ; ack TIMEUP
    jsr GbcIrqBody

    rep #$30
    ply
    plx
    pla
    sep #$20
    rti

; ---------------------------------------------------------------------------
; GbcVirqInit -- A(8) = the $4200 value the boot leaves ($81: the V-IRQ is
; never armed at boot -- there is no locked status copy yet).  On a console
; that may have it, programs VTIME and leaves $7F = $A1, the value the end of
; every frame body writes to $4200 while the genlock is locked (GbcVirqArm).
; See TWO WINDOWS for the two consoles that never get it ($7F = 0).
; ---------------------------------------------------------------------------
GbcVirqInit:
    stz.b $7F
if !GBC_VIRQ == 1
    lda.l !GBC_CTR_CPUREV
    cmp.b #$01
    beq GbcVirqOff      ; rev.1 5A22: general DMA over an active HDMA (sec. 12.11)
    lda.w $213F
    and.b #$10
    bne GbcVirqOff      ; PAL: the window arithmetic is NTSC
    lda.b #(!GBC_VIRQ_LINE&$FF)
    sta.w $4209
    lda.b #(!GBC_VIRQ_LINE>>8)
    sta.w $420A         ; VTIME; the H-IRQ stays off, so it fires at H ~ 0
    lda.w $4211
    lda.b #$A1          ; NMI + V-IRQ + auto-joypad
    sta.b $7F
GbcVirqOff:
endif
    lda.b #$81
    rts

; Carry set = the status copy says the genlock is LOCKED, i.e. the next LY=0
; lands ~3 lines before !GBC_VIRQ_LINE and window B may open.  The one
; predicate both the arm (end of a frame) and GbcIrqBody (start of the next)
; ask, so they cannot disagree.
;
; ⚡ AND THE SNAPSHOT HAS TO BE FRESH ($3E b1).  LOCKED is a field OF THE
; SNAPSHOT (contract sec. 5/10), so it only changes when a snapshot is taken --
; and window B is what stops one from being taken when LY=0 lands inside it.
; An LCD toggle that throws LY=0 into 185..224 would therefore leave a stale
; LOCKED = 1 in every status copy, window B would open on every frame, and
; every LY=0 would be skipped until the loop dragged it out at 2.6 lines a
; frame: measured on the clocked host model, 12 lost snapshots per relock
; that the phase-4 body does not lose.  With the freshness test the first
; skipped snapshot closes window B for the next frame, the next LY=0 goes
; through, and it carries the LOCKED = 0 that keeps it closed.
;
; ⚡ AND LY_SYNC >= 25, BECAUSE "LOCKED" IS 1 IN THE EXATO MODE.  gbc_clk.v
; reports `locked = exact_mode | locked_r`: with the loop off the clock is at
; its target rate "by construction" -- and LY=0 then walks 1.6 SNES lines a
; frame through the whole frame, into 185..224 as well.  LOCKED alone opened
; window B on every Exato frame with a fresh snapshot (measured on the clocked
; host model: 115 snapshots skipped in 298 GB frames against 96 with window B
; closed).  LY_SYNC (status +0C, live in the bridge: the GB line the last SYNC
; landed on) is the phase itself: a locked genlock holds it at 25 (ALVO =
; 11.525 dots = line 25 + 125 dots, +-64), and >= 25 means LY=0 fell at least
; 25 GB lines = 42.5 SNES lines before SYNC, i.e. at or before V~182.8.
GbcVirqWanted:
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #!GBC_F_LOCKED
    beq GbcVirqNo
    lda.b $3E
    and.b #$02
    beq GbcVirqNo
    lda.l !GBC_SC+!GBC_ST_LYSYNC
    cmp.b #!GBC_VIRQ_LYSYNC
    bcc GbcVirqNo
    rts                 ; carry set by the cmp
GbcVirqNo:
    clc
    rts

; End of every frame body: the V-IRQ is ENABLED only while it is going to be
; used.  Unlocked (a genlock re-acquiring after an LCD toggle), or with
; LY=0 drifting past V~183 (Exato), it is switched off in $4200, so such a
; frame is the phase-4 frame to the cycle -- no 40-instruction IRQ entry
; stolen from the compiler at V=185.  Runs before
; the IGR, whose exit hygiene zeroes $4200 and must have the last word.
GbcVirqArm:
    lda.b $7F
    beq GbcVirqArmOut   ; rev.1 / PAL / !GBC_VIRQ = 0: $4200 stays $81
    jsr GbcVirqWanted
    lda.b #$81
    bcc GbcVirqArmSt
    lda.b $7F
GbcVirqArmSt:
    sta.w $4200
GbcVirqArmOut:
    rts

; ---------------------------------------------------------------------------
; GbcNmiBody / GbcFrameOnce -- entry: native mode, DBR = $00, DP = $0000,
; A 8-bit, X/Y 16-bit.  GbcFrameOnce is the name the offline harness calls.
;
; ORDER IS THE CONTRACT (sec. 5/6/7/13.5): SYNC first (it is the genlock's
; phase reference and must not drift with how much work the rest of the frame
; does), then COMMIT before ANY view read, then the reads, then CONSUMED once
; the last of them is done.  COMMIT and CONSUMED are strobes that may appear
; exactly once per frame, which is why the whole body sits behind the
; re-entrancy latch instead of only the parts that own direct-page state.
; ⚡ Phase 5: when the V-IRQ opened the frame ($15 != 0) COMMIT already went
; out at V=185 -- still the first thing of the FIRST window, still before any
; view read -- and the NMI only does its prologue and resumes (TWO WINDOWS).
; ---------------------------------------------------------------------------
; One whole frame as the offline harness sees it: the body the NMI runs plus
; the compile the main loop would have run between this NMI and the next.  The
; harness has no clock, so "run it to completion" is the honest model of the
; 185 display lines the compiler has; on silicon the split is what keeps the
; NMI short (see MainLoop).
GbcFrameOnce:
    jsr GbcNmiBody
    jsr GbcRasterRun
    rts

GbcNmiBody:
    lda.b $15
    bne GbcNmiIrqFrame  ; the V-IRQ opened this frame: see below
    lda.b $03
    beq GbcNmiRun
    rts                 ; re-entered inside our own frame: the strobes below
                        ; may appear exactly once each, and everything under
                        ; them keeps its state in the direct page
GbcNmiRun:
    inc.b $03

    jsr GbcNmiPrologue

    stz.b $3F           ; nothing deferred yet this frame

    ; --- 2) latch the dirty bits, before the first view read ---------------
    ldx.w #!GBC_EFO_COMMIT
    lda.b #$00
    jsr GbcEfPut

    rep #$20
    lda.b $08
    sta.b $1E           ; budget for this frame
    lda.l !GBC_CTR_FRAMES
    inc a
    sta.l !GBC_CTR_FRAMES
    sep #$20
    stz.b $17

    ; --- 3) the status block, by DMA into fast RAM -------------------------
    jsr GbcStatusFetch
    bcs GbcNmiHaveStat
    ; The COMMIT above already latched (and cleared) the bridge's accumulator.
    ; Not reading what it published would lose those changes for good, so the
    ; next fold is promoted to a full refresh instead.
    lda.b #$01
    sta.b $0A
    sta.b $3F
    bra GbcNmiTail
GbcNmiHaveStat:

    jsr GbcCheckVer
    lda.b $05
    bne GbcNmiHaveVer
    jsr GbcFbNoSnap     ; a bridge that stopped answering takes the FB mode
    bra GbcNmiTail      ; down with it: pads and IGR only from here
GbcNmiHaveVer:

    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #!GBC_F_SNAPOK
    bne GbcNmiSnapOk
    lda.b #$01
    sta.b $0A           ; same reasoning as the no-status path above
    sta.b $3F
    jsr GbcFbNoSnap
    bra GbcNmiTail
GbcNmiSnapOk:

    ; ⚡ wire $03, THE MODE OFF COSTS THE WINDOW NOTHING.  With fb_state = 0
    ; and nothing owed by the last exit (FB_FLAGS = 0 there), the body is the
    ; $02 player's, call for call, and the policy runs LAST, after the
    ; frame's transfers: every cycle it spent in front of the drain used to
    ; come out of the guard's capacity and moved deferrals and refusals of
    ; the quad-layer (advisor C6-PLAYER).  An entry decided there takes
    ; effect exactly when one decided at the start would: FB_EN is latched
    ; at the bridge's next LY=0 either way.
    lda.w !GBC_FB_STATE
    ora.w !GBC_FB_FLAGS
    beq GbcNmiBody02
    jsr GbcNmiFbBody    ; the mode on, entering, leaving or owed
    bra GbcNmiTail
GbcNmiBody02:
    jsr GbcFold
    jsr GbcRegsSafe
    jsr GbcRasterArm    ; the log copy, inside the window and before CONSUMED
    jsr GbcDrain
    jsr GbcReadyCheck
    jsr GbcFbPolicy     ; enter?  (off the window, before CONSUMED)

GbcNmiTail:
    jmp GbcFrameTail

; The V-IRQ opened this frame.  The prologue is owed whatever state the IRQ
; half is in (it may still be running underneath us: the NMI is not
; maskable), and then the token decides who resumes.
GbcNmiIrqFrame:
    jsr GbcNmiPrologue
    dec.b $15
    bne GbcNmiIrqOut    ; the IRQ half is still running; it resumes itself
    jmp GbcFrameResume  ; it paused before V=225: carry on from here
GbcNmiIrqOut:
    rts

; ---------------------------------------------------------------------------
; The part of the NMI that is owed EVERY frame at the same place, whoever owns
; the rest: the genlock strobe, forced blank for V=0 and the HDMA publish.
;
; ⚡ SYNC IS THE FIRST THING, AND THAT IS A TIMING FIX, NOT TIDINESS.  The
; genlock puts LY=0 exactly ALVO = 43 SNES lines before SYNC (contract sec. 2),
; and the contract's "LY=0 ~ V=182" assumes SYNC at the NMI.  It used to go out
; AFTER GbcHdmaPublish, whose length depends on the set it publishes: on the
; clocked host model that put SYNC 3 to 5 lines past V=225 -- and wandering by
; up to 4.5 lines between frames whenever the published set changed, which is
; phase noise the genlock's lock window (64 dots = 0.24 line) cannot absorb.
; Harmless while COMMIT was at V=225 too, fatal for the V-IRQ: LY=0 then fell
; at V=185-187, inside window B, and 33 of 40 snapshots of cgb-acid2 (38 of 40
; of p4-parallax) were SKIPPED.  Here SYNC leaves ~8 instructions after the NMI
; is taken, the same number on every path.
; ---------------------------------------------------------------------------
GbcNmiPrologue:
    ; --- 1) genlock phase strobe -------------------------------------------
    ldx.w #!GBC_EFO_SYNC
    lda.b #$00
    jsr GbcEfPut

    lda.b #$8F
    sta.w $2100         ; V=0 belongs to no HDMA entry.  From line 1 on, ch5
                        ; owns $2100; the body never writes it again, because
                        ; an end-of-body $0F would un-blank the top letterbox
                        ; on any frame whose drain ran past V=0.

    ; The only $420C write of the whole player (contract sec. 13.11) and the
    ; only place the HDMA channels are pointed at a table.  Here, and only
    ; here, we are certainly before the V=0 HDMA init, so a channel that comes
    ; up now gets its line counter loaded from the pointer this routine wrote.
    ; ⚡ wire $03: with the framebuffer mode anywhere but OFF the cut-over,
    ; the FB's own channel set and the way back are decided here too (they
    ; are register writes that have to land before V=0, like the publish).
    ; Carry set = the FB set went on the bus; the quad-layer's publish is
    ; skipped this frame.
    lda.w !GBC_FB_STATE
    beq GbcNmiProNormal
    jsr GbcFbPrologue
    bcs GbcNmiArmed
GbcNmiProNormal:
    jsr GbcHdmaPublish
    lda.b $06
    cmp.b #$01
    bne GbcNmiArmed
    lda.b #$02
    sta.b $06           ; GbcHdmaPublish armed $420C for the first time above
    rep #$20
    lda.l !GBC_CTR_FRAMES
    sta.l !GBC_CTR_READY
    sep #$20
GbcNmiArmed:
GbcIrqEarlyOut:         ; GbcIrqBody's early exits: an rts within reach
    rts

; ---------------------------------------------------------------------------
; GbcIrqBody -- window B: open the frame at V=!GBC_VIRQ_LINE.
;
; Everything here is the first half of GbcNmiRun above, in the same order, up
; to and including a first drain pass -- minus CGRAM, which window A owns (see
; TWO WINDOWS), and with a budget and a capacity that end at V=225.
; ---------------------------------------------------------------------------
GbcIrqBody:
    lda.b $03
    bne GbcIrqEarlyOut       ; a body is still running (not reachable on NTSC
                        ; timing: the resume ends near V=40)
    lda.b $05
    beq GbcIrqEarlyOut       ; no bridge, or not our wire version
    jsr GbcVirqWanted
    bcc GbcIrqEarlyOut       ; genlock not locked: the NMI runs the whole frame
                        ; (GbcVirqArm normally keeps the IRQ off then; this is
                        ; the IRQ that was armed by the frame before it)
    lda.b #$02
    sta.b $15           ; two owners left (see TWO WINDOWS).  Before the $03
    inc.b $03           ; latch: an NMI landing between the two stores then
                        ; takes the IRQ-frame path (prologue + token) instead
                        ; of the "re-entered" rts, which would drop the SYNC
    lda.b #$01
    sta.b $16           ; window B: capacity ends at V=225
    stz.b $3F

    ldx.w #!GBC_EFO_COMMIT
    lda.b #$00
    jsr GbcEfPut

    rep #$20
    lda.w #!GBC_BUDGETB
    sta.b $1E
    lda.l !GBC_CTR_FRAMES
    inc a
    sta.l !GBC_CTR_FRAMES
    sep #$20
    stz.b $17

    jsr GbcStatusFetch
    bcs GbcIrqHaveStat
    lda.b #$01          ; as in GbcNmiRun: the COMMIT above cleared the
    sta.b $0A           ; bridge's accumulator, so the next fold is a full
    sta.b $3F           ; refresh
    bra GbcIrqHandoff
GbcIrqHaveStat:
    jsr GbcCheckVer
    lda.b $05
    bne GbcIrqHaveVer
    jsr GbcFbNoSnap
    bra GbcIrqHandoff
GbcIrqHaveVer:
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #!GBC_F_SNAPOK
    bne GbcIrqSnapOk
    lda.b #$01
    sta.b $0A
    sta.b $3F
    jsr GbcFbNoSnap
    bra GbcIrqHandoff
GbcIrqSnapOk:
    lda.w !GBC_FB_STATE ; ⚡ wire $03: the $02 body when the mode is OFF and
    ora.w !GBC_FB_FLAGS ; nothing is owed (see GbcNmiSnapOk); the resume
    beq GbcIrqBody02    ; follows the choice made here (!GBC_FB_POLEND)
    jsr GbcIrqFbBody
    bra GbcIrqHandoff
GbcIrqBody02:
    jsr GbcFold
    lda.b #$03
    sta.b $16           ; b1: the resume owes regs, log, CGRAM and the walk
    jsr GbcDrainOpen
    jsr GbcDrainB

GbcIrqHandoff:
    lda.b #$01
    trb.b $16           ; the capacity is the normal windows' again
    stz.b $17           ; running out of window B is a pause, not an abort
    dec.b $15
    bne GbcIrqOut       ; the NMI has not come yet: its prologue resumes
    jmp GbcFrameResume  ; it came while this half ran: resume right here
GbcIrqOut:
    rts

; ---------------------------------------------------------------------------
; GbcFrameResume -- the second half of a frame the V-IRQ opened, run by
; whichever of the two handlers is last (TWO WINDOWS).  Same calls, same order
; as the tail of GbcNmiRun, with the drain CONTINUING window B's walk.
; ---------------------------------------------------------------------------
GbcFrameResume:
    lda.b $16
    and.b #$02
    beq GbcFrameResTail ; window B never reached the fold: pads and CONSUMED
    stz.b $16
    rep #$20
    lda.b $08
    sta.b $1E           ; window A's budget line
    sep #$20
    lda.w !GBC_FB_POLEND
    beq GbcFrameRes02   ; window B ran the $02 body: so does window A
    jsr GbcResumeFbBody ; window B ran the FB body (wire $03)
    bra GbcFrameResTail
GbcFrameRes02:
    jsr GbcRegsSafe
    jsr GbcRasterArm
    jsr GbcDrainResume
    jsr GbcReadyCheck
    jsr GbcFbPolicy     ; enter?  (off the window, before CONSUMED)
GbcFrameResTail:
    stz.b $16

; ---------------------------------------------------------------------------
; The end of every frame body, whichever way it was run.
; ---------------------------------------------------------------------------
GbcFrameTail:
    jsr GbcWhiteHold    ; after the drain: is the NEXT frame still owed white?
    lda.b $3F
    beq GbcNmiNoDefer
    rep #$20
    lda.l !GBC_CTR_DEFER
    inc a
    sta.l !GBC_CTR_DEFER
    sep #$20
GbcNmiNoDefer:

    jsr GbcVirqArm

    ; --- 4) pads ------------------------------------------------------------
    ; $4218 mid auto-read is garbage, and a reset must never fire off a
    ; misread.  A frame with an empty backlog gets here early enough that the
    ; wait actually waits.
GbcPadWait:
    lda.w $4212
    lsr a
    bcs GbcPadWait
    lda.w $4218
    sta.b $0E
    ldx.w #!GBC_EFO_PADL
    jsr GbcEfPut            ; $4218 = the LOW byte of the 16-bit pad word,
                            ; i.e. {A X L R - - - -}.  (The contract's sec. 9
                            ; table names the two halves the other way round;
                            ; the 5A22 is what it is, and the bridge is being
                            ; fixed to match.  The stores here have always been
                            ; $4218 -> $EF0000 and $4219 -> $EF0001.)
    lda.w $4219
    sta.b $0F
    ldx.w #!GBC_EFO_PADH
    jsr GbcEfPut            ; $4219 = the HIGH byte, {B Y Sel St Up Dn Lf Rt}

    jsr GbcIgr

    ; --- 5) done reading for this frame ------------------------------------
    ldx.w #!GBC_EFO_CONSUME
    lda.b #$00
    jsr GbcEfPut

    ; --- 6) the counters, for a USB read on silicon ------------------------
    ; AFTER CONSUMED on purpose: the ~350 cycles are then paid by the main
    ; loop, not by the read window.  Placed before it they lengthened busy,
    ; and every LY=0 that lands in a longer window is one more skipped
    ; snapshot (+12 in 298 GB frames of Exato on the clocked host model).
    ; Every counter of the frame has moved by now (DEFER is the last, above).
    jsr GbcMbxPublish

    stz.b $03           ; body finished: the next NMI may own the direct page
    rts

; ===========================================================================
; GbcEfPut -- A(8) = value, X(16) = offset inside the $EF write window.
;
; THE ONLY store into $EF in the whole player.  Contract sec. 13.4 makes 8-bit
; stores an invariant (data and strobe are decoded separately, so a 16-bit
; store would fire the neighbouring strobe as well); funnelling every strobe
; through one site turns "all $EF stores are 8-bit" from a rule that has to be
; re-checked at every call site into one instruction preceded by one sep, which
; selfcheck.py can then assert mechanically.  Six calls a frame, 12 cycles each,
; plus the ten mailbox bytes of GbcMbxPublish.
; ===========================================================================
GbcEfPut:
    sep #$20
    sta.l !GBC_EF,x
    rts

; ===========================================================================
; GbcMbxPublish -- entry A 8-bit, X/Y 16-bit; any DBR.  Copies the five
; diagnostic counters into the bridge's mailbox ($EF0010-$EF0019, GBDG
; +20..+29).  Low byte first, then high: the bridge only updates a word on the
; high byte that follows its own low byte, so an MCU read between the two
; stores sees last frame's word, never a torn one.  Every store still goes
; through GbcEfPut (one 8-bit store site into $EF, sec. 13.4).
; ===========================================================================
GbcMbxPublish:
    ldx.w #!GBC_EFO_MBX
    rep #$20
    lda.l !GBC_CTR_FRAMES
    jsr GbcMbxPut16
    lda.l !GBC_CTR_DEFER
    jsr GbcMbxPut16
    lda.l !GBC_CTR_DROPS
    jsr GbcMbxPut16
    lda.l !GBC_CTR_READY
    jsr GbcMbxPut16
    lda.l !GBC_CTR_REGSLATE
    jsr GbcMbxPut16
    ; ⚡ wire $03, word 5 = C6MODE: lo = fb_state, hi = the ROW_DONEs of this
    ; frame (GBDG +2A).  Two 8-bit loads: the two bytes are not adjacent.
    sep #$20
    lda.w !GBC_FB_ROWSDONE
    xba
    lda.w !GBC_FB_STATE
    rep #$20
    jsr GbcMbxPut16
    sep #$20
    rts

; A(16) = the word, X = the $EF offset of its low byte.  Leaves A 16-bit and
; X on the next word.
GbcMbxPut16:
    jsr GbcEfPut        ; sep #$20 inside: the low byte
    xba
    inx
    jsr GbcEfPut        ; the high byte
    inx
    rep #$20
    rts

; ===========================================================================
; GbcCheckVer -- $05 = 1 while the bridge answers with our wire version.
; Re-read every frame, not just at boot: a core that stops answering must stop
; the re-render rather than paint whatever $00 decodes to.
; ===========================================================================
GbcCheckVer:
    lda.l !GBC_SC+!GBC_ST_VER
    cmp.b #!GBC_VER
    beq GbcCheckVerOk
    stz.b $05
    rts
GbcCheckVerOk:
    lda.b $05
    bne GbcCheckVerDone
    lda.b #$01
    sta.b $05
    ; The bridge answered for the FIRST time, which means GbcLifeWait timed out
    ; at boot and GbcGo was never reached: the GB is still in reset, and the
    ; body below would fold, apply registers and drain views of a machine that
    ; is not running.  GO is idempotent by contract (sec. 10), so releasing it
    ; here costs nothing on any path that already did.
    jsr GbcGo
GbcCheckVerDone:
    rts

; ===========================================================================
; FOLD -- merge what the bridge published into the persistent backlog.
;
; The bridge clears its accumulator on COMMIT (contract sec. 6), so a block we
; could not afford last frame is OURS to remember: backlog |= DIRTY, and only
; what actually went out is cleared.
;
; ⚡ wire $02 made the map half a PLAIN OR, and that is the whole point of the
; phase: DIRTY_MAP bit r names a row of GB VRAM ($9800 for r < 32, $9C00 for
; r >= 32) and the map views are now the Game Boy's maps, so bit r is block r
; of the content class and block r of the carpet class, whatever LCDC says.
; Wire $01 had to pick a half with LCDC.3 / LCDC.6 and re-send 4 KB every time
; one of them moved; here a game that alternates b3 between frames re-sends
; NOTHING, and a map both layers use is transferred once instead of twice.
;
; The chr half grew the opposite way: the $8800 block of each bank now lives in
; VRAM twice (once per chr base), and the second copy is its own class with its
; own backlog byte, so the same incoming bits are ORed into two places.
; ===========================================================================
GbcFold:
    ; --- did the snapshot advance? (contract sec. 7) -----------------------
    ; SEQ unchanged means the bridge SKIPPED the snapshot, so OAM/CRAM/regs
    ; still hold what we already copied -- re-sending them would be pure
    ; bandwidth.  The dirty bits stay in the backlog and are honoured on the
    ; frame SEQ finally moves.
    stz.b $3E
    rep #$20
    lda.l !GBC_SC+!GBC_ST_SEQ
    cmp.b $0B
    beq GbcFoldSeqSame
    sta.b $0B
    sep #$20
    lda.b #$03          ; b0 re-send, b1 the snapshot really moved
    sta.b $3E
    bra GbcFoldSeqDone
GbcFoldSeqSame:
    sep #$20
GbcFoldSeqDone:

    lda.b $0A
    beq GbcFoldNormal
    stz.b $0A
    jsr GbcMarkAll
    lda.b #$01
    tsb.b $3E           ; a forced refresh always re-copies OAM and CGRAM (b1,
                        ; "the snapshot moved", is left as the SEQ said)
    rts

GbcFoldNormal:
    rep #$20
    lda.l !GBC_SC+!GBC_ST_DCHR+0
    ora.b $20
    sta.b $20
    lda.l !GBC_SC+!GBC_ST_DCHR+2
    ora.b $22
    sta.b $22
    lda.l !GBC_SC+!GBC_ST_DCHR+4
    ora.b $24
    sta.b $24
    lda.l !GBC_SC+!GBC_ST_DOBJ+0
    ora.b $26
    sta.b $26
    lda.l !GBC_SC+!GBC_ST_DOBJ+2
    ora.b $28
    sta.b $28
    sep #$20

    ; The $8800 block of each bank is sent to BOTH chr bases, so its bits are
    ; ORed into the duplicate's own backlog as well: DIRTY_CHR byte 1 is
    ; blocks 8..15 (bank 0) and byte 4 is blocks 32..39 (bank 1).
    lda.l !GBC_SC+!GBC_ST_DCHR+1
    ora.b $3B
    sta.b $3B
    lda.l !GBC_SC+!GBC_ST_DCHR+4
    ora.b $3C
    sta.b $3C

    ; The maps: bit r of DIRTY_MAP is row r of a CONTENT class and row r of the
    ; CARPET class of the same Game Boy map.  No LCDC anywhere (see the header
    ; of this routine).  The four backlogs are contiguous and in the same order
    ; as the eight DIRTY_MAP bytes twice over, so one walk does both halves:
    ;   $2A content $9800   $2E content $9C00   $32 carpet $9800   $36 carpet $9C00
    ldx.w #$0000
    rep #$20
GbcFoldMapLoop:
    lda.l !GBC_SC+!GBC_ST_DMAP,x
    ora.b $2A,x
    sta.b $2A,x
    lda.l !GBC_SC+!GBC_ST_DMAP,x
    ora.b $32,x
    sta.b $32,x
    inx
    inx
    cpx.w #$0008
    bne GbcFoldMapLoop
    sep #$20

    lda.l !GBC_SC+!GBC_ST_DMISC
    sta.b $5A
    and.b #$10          ; MSEL: LCDC.0 or the CGB/compat mode moved, so every
                        ; map row's ATTRIBUTES are stale.  ⚡ wire $02 took
                        ; LCDC.3/.4/.6 out of MSEL in the bridge -- they are
                        ; registers now, not view content
    beq GbcFoldNoMsel
    jsr GbcMarkMaps
GbcFoldNoMsel:
    lda.b $5A
    and.b #$03          ; b0 OAM, b1 CRAM -- same bit positions as the backlog
    ora.b $3A
    sta.b $3A
    ; ⚡ A COLOUR CHANNEL THAT LEAVES THE BUS LEAVES ITS WRITES IN CGRAM.  The
    ; SNES keeps the last value written to a CGRAM entry, and CGRAM is only
    ; re-sent when the snapshot's CRAM moves -- so the last frames a colour
    ; channel ran (tables one or more frames behind the view: the compile's
    ; latency, a deferred frame) stay on a static screen for good.  Re-send the
    ; view once, on the first frame whose published set has no colour channel
    ; after one that had.
    ; The prologue counts those departures in $01 (it sees every flip, a
    ; deferred frame's too -- this body does not run on one, and a colour set
    ; that only ever reached the bus on deferred frames would go unseen); $02
    ; is the last count this body answered.
    lda.b $01
    cmp.b $02
    beq GbcFoldColDone
    sta.b $02
    lda.b #$02
    tsb.b $3A           ; ... and has just left it: the view goes back up
GbcFoldColDone:
    rts

; Everything dirty: the boot refresh, and the recovery path for any frame whose
; COMMIT was latched but never read.  $3B/$3C (the duplicates of the $8800 chr
; block) are outside the contiguous run because $3A, the OAM/CRAM byte, sits
; between them and has bits that must stay clear.
GbcMarkAll:
    lda.b #$FF
    ldx.w #$0020
GbcMarkAllLoop:
    sta.b $00,x
    inx
    cpx.w #$003A
    bne GbcMarkAllLoop
    sta.b $3B
    sta.b $3C
    lda.b $3A
    ora.b #$03
    sta.b $3A
    rts

; Only the four map classes (MSEL: LCDC.0 or the compat mode moved, so every
; map entry's attributes are stale).
GbcMarkMaps:
    lda.b #$FF
    ldx.w #$002A
GbcMarkMapsLoop:
    sta.b $00,x
    inx
    cpx.w #$003A
    bne GbcMarkMapsLoop
    rts

; ===========================================================================
; ⚡ GbcHdmaDropLcdc -- take the LCDC group off the bus for THIS frame.
;
; TABLE SPEC S12 says the group is never armed on the white screen, and the
; compiler obeys it -- but a compile only reaches the bus in the PROLOGUE OF
; THE NEXT FRAME, and the prologue runs before the status block of this one has
; even been read.  So the first frame in which the LCD goes off publishes the
; set compiled from the PREVIOUS snapshot, with the group armed, and from line
; 1 on it writes $48/$4C over the $210A = $50 that GbcRegs is about to set:
; instead of a white screen the whole frame shows the previous scene's carpet.
; Two frames of it when that compile has to be redone for the CGRAM copy.
;
; So the white branch takes the channel off $420C itself.  It is the one write
; of $420C outside GbcHdmaPublish, and it is still inside the NMI and still
; before V=0, where contract sec. 13.11 wants it; it only CLEARS a bit, so no
; channel is ever enabled outside the prologue.  Which channel it is comes from
; the header of the set the prologue published -- the same bytes it loaded --
; so nothing has to be remembered in the direct page.
;
; $73 b4 is the flag the prologue publishes for exactly this, and clearing it
; here is what makes GbcRegs write the four registers of the group again.
; ===========================================================================
GbcHdmaDropLcdc:
    lda.b $73
    and.b #$10
    bne GbcHdlGo
    rts
GbcHdlGo:
    rep #$20
    lda.b $18
    and.w #$00FF
    beq GbcHdlSetA
    lda.w #!GBC_SETB_OFF
    bra GbcHdlBase
GbcHdlSetA:
    lda.w #!GBC_SETA_OFF
GbcHdlBase:
    sta.b $5B           ; the set the prologue put on the bus
    sep #$20
    ldx.w #$0000
GbcHdlLoop:
    rep #$20
    txa
    asl a
    asl a
    clc
    adc.b $5B
    adc.w #!GBC_H_CH
    tay
    sep #$20
    lda [!GBC_PTR],y
    cmp.b #$08          ; the B-bus address of the group
    beq GbcHdlFound
    inx
    cpx.w #$0004
    bne GbcHdlLoop
    bra GbcHdlDone
GbcHdlFound:
    lda.b #$01
GbcHdlBit:
    cpx.w #$0000
    beq GbcHdlBitDone
    asl a
    dex
    bra GbcHdlBit
GbcHdlBitDone:
    cmp.b #$01
    bne GbcHdlBitPhys
    lda.b #$40          ; logical channel 0 is ch6
GbcHdlBitPhys:
    eor.b #$FF
    sta.b $5D           ; ~(1 << channel)
    rep #$20
    lda.b $5B
    clc
    adc.w #!GBC_H_MASK
    tay
    sep #$20
    lda [!GBC_PTR],y    ; the mask the prologue loaded into $420C
    jsr GbcHdmaMask
    and.b $5D
    sta.w $420C
    sta.l !GBC_HDMAEN_SH
GbcHdlDone:
    lda.b $73
    and.b #$EF
    sta.b $73
    rts

; ===========================================================================
; GbcRegsSafe -- GbcRegs, but only if it can still END before V=0.
;
; WHY.  Everything GbcRegs writes is also on the HDMA's bus from line 0 on:
; the four write-twice scroll pairs $210D-$2114 share ONE latch (BGnOFS
; "prev") with every scroll channel's four-byte group, and the LCDC channel
; writes $2108-$210B itself.  HDMA initialises at V=0 and makes its first
; B-bus transfer in the hblank of line 0 (SNES manual, HDMA; contract sec.
; 11.4 "entry i covers line i+1"), so a GbcRegs still running then can see a
; channel's byte land between its own low and high write -- a scroll register
; with the wrong byte for the whole frame, no crash, nothing any gate reads.
; Vblank (V=225..261) has no HDMA transfer at all; that is the only safe place.
;
; WHO IS LATE.  The phase-4 body calls GbcRegs ~5 lines into the vblank.  A
; frame the V-IRQ half closes ITSELF (it was still scanning clean blocks when
; the NMI came, TWO WINDOWS) calls it when that scan stops: 13-22 lines past
; V=225 on the host clock, 29-31 with the CPU 30 % slower.  Nothing bounded
; that before this routine.
;
; THE LIMIT.  !GBC_REGS_LAST is the last line GbcRegs may START on.  Its
; longest path (LCD on, no scroll pair on the HDMA: all eight scroll writes)
; measures 4.3 K mc = 3.2 lines from its first store to its last on the host
; clock (tests/host --clock, regs_dur_max; 6.5 K at 1.5x CPU cost).  The V
; read below can happen anywhere inside line N, so a START of "line N" is up
; to 1 line later; budgeting GbcRegs at TWICE the host figure (6.4 lines --
; the bsnes-plus harness measured this code ~13 % slower than the host model)
; gives 254 + 1 + 6.4 = 261.4 < 262: done before V=0, and the first HDMA
; transfer is later still, in the hblank of line 0.
;
; WHAT A DEFERRED FRAME LOOKS LIKE.  The registers keep the previous frame's
; values (TM, the map/chr bases the HDMA does not drive, the undriven scroll
; pairs) and the NEXT frame's GbcRegs writes the new ones: one frame late,
; never torn.  The one visible corner: a frame whose LCD just went off keeps
; the LCDC channel armed (GbcHdmaDropLcdc is inside GbcRegs) and shows the
; previous scene for one more frame instead of white.  Counted in
; !GBC_CTR_REGSLATE for the silicon report; on the host clock it stays 0 in
; every scenario, including 1.5x CPU cost.
; ===========================================================================
GbcRegsSafe:
    jsr GbcVCount       ; A(16) = V
    cmp.w #!GBC_VBLANK
    bcc GbcRegsLate     ; 0..224: the HDMA has already initialised
    cmp.w #!GBC_REGS_LAST+1
    bcs GbcRegsLate     ; too close to V=0 to finish (or a PAL line)
    sep #$20
    jmp GbcRegs
GbcRegsLate:
    lda.l !GBC_CTR_REGSLATE
    inc a
    sta.l !GBC_CTR_REGSLATE
    sep #$20
    rts

; ---------------------------------------------------------------------------
; ⚡ GbcWhiteHold -- keep the white screen after the LCD comes back until what
; changed while it was off has been uploaded.
;
; A game switching scenes turns the LCD off, rewrites the tiles and maps, and
; turns it back on; the GB shows nothing for the first frame after that and a
; finished picture from the next.  Here the first frame is white (F_FIRST) but
; the uploads take a few frames, and the frame after it showed the new scene
; half sent: the new map over the old tiles, one frame of garbage on every
; death and level change of Super Mario Bros. Deluxe (Mk.III, 2026-10-01).
; So, decided at the END of each frame (after its drain: exactly what is still
; owed), the NEXT frame gets F_FIRST through !GBC_WHITE_FIRST while the block
; backlog or OAM/CGRAM is owed, and every reader of the pair (GbcRegs' white
; branch and its LCDC drop, GbcW1Blank) keeps the white frame -- for at most
; !GBC_WHITE_HOLD frames, so a game that never lets the backlog empty still
; gets its picture.  Off the FB mode only (its exit has a white screen of its
; own).  ⚡ Here and not in front of GbcRegs: the FB freshness gate moved with
; ~45 cycles more in the body; GbcRegs pays one ORA.  A(8) out.
; ---------------------------------------------------------------------------
GbcWhiteHold:
    sep #$20
    lda.b #$00
    sta.l !GBC_WHITE_FIRST
    lda.w !GBC_FB_STATE
    bne GbcWhOut
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #(!GBC_F_LCDON|!GBC_F_FIRST)
    cmp.b #!GBC_F_LCDON
    beq GbcWhOn
    lda.b #!GBC_WHITE_HOLD  ; LCD off or its first frame back: arm the hold,
    sta.l !GBC_WHITE_CNT    ; and judge it now -- the frame after F_FIRST is
GbcWhOn:                    ; the first one it covers
    lda.l !GBC_WHITE_CNT
    beq GbcWhOut
    dec a
    sta.l !GBC_WHITE_CNT
    beq GbcWhOut        ; the cap
    jsr GbcFbBacklogEmpty
    bne GbcWhKeep
    lda.b $3A
    and.b #$03          ; OAM / CGRAM owed
    beq GbcWhRelease
GbcWhKeep:
    lda.b #!GBC_F_FIRST
    sta.l !GBC_WHITE_FIRST
    rts
GbcWhRelease:
    lda.b #$00
    sta.l !GBC_WHITE_CNT
GbcWhOut:
    rts

; ===========================================================================
; REGISTERS -- everything the snapshot decides that is not a transfer.
; Cheap enough to redo unconditionally every frame (contract sec. 11.3).
; ===========================================================================
GbcRegs:
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    ora.l !GBC_WHITE_FIRST  ; GbcWhiteHold
    sta.b $5A
    and.b #(!GBC_F_LCDON|!GBC_F_FIRST)
    cmp.b #!GBC_F_LCDON
    beq GbcRegsHaveLcd
    jsr GbcHdmaDropLcdc ; ⚡ errata E2: NO white frame may go out with the LCDC
                        ; group on the bus -- see the routine
GbcRegsHaveLcd:

    ; --- ⚡ the six map/chr base registers, from the snapshot's LCDC --------
    ; Contract sec. 11.3, wire $02.  LCDC.3 (which map the background reads),
    ; LCDC.6 (which map the window reads) and LCDC.4 (which of the two chr
    ; bases a tile index means) are REGISTERS here, not something baked into a
    ; view -- which is what lets the HDMA follow b3 and b4 per line later.
    ;   $2107 BG1 map = the window's content   $2109 BG3 map = its carpet
    ;   $2108 BG2 map = the background's       $210A BG4 map = its carpet
    ;   $210B BG1+BG2 chr base ($4000 or $6000), $210C the carpets' ($8000)
    ;
    ; ⚠ FOUR OF THE SIX ARE SKIPPED WHEN THE HDMA IS DRIVING THEM ($73 b4),
    ; for the same reason GbcRegsScroll skips a scroll pair: the group carries
    ; its own value on EVERY line from line 1 on, so a CPU write here only
    ; decides line 0 -- which is inside the top letterbox and is not shown --
    ; and writing it invites exactly the bug the E2 above is about.  $2107,
    ; the window's CONTENT map, is outside the group and is always written.
    lda.l !GBC_SC+!GBC_ST_LCDC
    sta.b $5B
    and.b #$40
    beq GbcRegsWin0
    lda.b #$44
    sta.w $2107
    lda.b #$4C
    bra GbcRegsWinSt
GbcRegsWin0:
    lda.b #$40
    sta.w $2107
    lda.b #$48
GbcRegsWinSt:
    sta.b $5C           ; the $2109 this frame's snapshot asks for
    lda.b $73
    and.b #$10
    bne GbcRegsGrpHdma
    lda.b $5C
    sta.w $2109
    lda.b $5B
    and.b #$08
    beq GbcRegsBg0
    lda.b #$44
    sta.w $2108
    lda.b #$4C
    sta.w $210A
    bra GbcRegsChr
GbcRegsBg0:
    lda.b #$40
    sta.w $2108
    lda.b #$48
    sta.w $210A
GbcRegsChr:
    lda.b $5B
    and.b #$10
    beq GbcRegsChrB
    lda.b #$22          ; $210B per frame: base A, VRAM $4000
    bra GbcRegsChrSt
GbcRegsChrB:
    lda.b #$33          ; $210B per frame: base B, VRAM $6000
GbcRegsChrSt:
    sta.w $210B
GbcRegsGrpHdma:

    lda.b $5A
    and.b #(!GBC_F_LCDON|!GBC_F_FIRST)
    cmp.b #!GBC_F_LCDON
    beq GbcRegsTm

    ; --- LCD off, or the first frame after it came back: WHITE -------------
    ; Contract sec. 11.3: $212C = $08 (BG4 alone) over a static map of
    ; {t = 769, pal = 0, pri = 0}; tile 769 is solid colour 2 and CGRAM[98] is
    ; the fixed $7FFF the view guarantees, so the viewport reads white while
    ; the letterbox and pillars stay black.
    ;
    ; ⚡ wire $02: that map lives in FREE VRAM ($A000) and is written ONCE at
    ; init, so this is now two register writes and nothing else.  It used to be
    ; painted over the carpet map's VRAM every time the LCD went off, which
    ; cost a 2 KB DMA, made the carpet class unsendable while it was up, and
    ; forced every map row to be marked dirty again on the way back.
    ; ⚡ wire $03: unless the framebuffer mode's exit still owes that map
    ; ($A000 carried the FB tilemap): the backdrop for this one frame, the
    ; white screen from the next (C6-SPEC a.5 "LCD off: 1 black -> white").
    lda.w !GBC_FB_FLAGS
    and.b #$02
    eor.b #$02
    beq GbcRegsWhiteOwed
    lda.b #$08
GbcRegsWhiteOwed:
    sta.w $212C
    lda.b #$50
    sta.w $210A         ; BG4 reads the static white map instead of a carpet.
                        ; GbcHdmaDropLcdc above guarantees no channel is going
                        ; to write over it from line 1 on
    bra GbcRegsScroll

GbcRegsTm:
    ; TM: everything, except that in compat with LCDC.0 = 0 the BG and window
    ; content layers must go and the carpets must stay -- the carpets paint
    ; BG[0][BGP&3], which is exactly what the hardware shows there.  In CGB
    ; mode LCDC.0 = 0 only changes priority, and that is already in the view.
    lda.b #$1F
    sta.b $5B
    lda.b $5A
    and.b #!GBC_F_COMPAT
    beq GbcRegsTmGo
    lda.l !GBC_SC+!GBC_ST_LCDC
    and.b #$01
    bne GbcRegsTmGo
    lda.b #$1C
    sta.b $5B
GbcRegsTmGo:
    lda.b $5B
    sta.w $212C

GbcRegsScroll:
    ; The GB image occupies V = 41..184, X = 48..207.  BG2/BG4 carry the GB
    ; background, BG1/BG3 the window.  Every one of these is a write-twice
    ; register sharing ONE latch across all eight, so each is written low byte
    ; then high byte back to back -- interleaving two registers leaves the
    ; second one holding the first one's high byte.
    ;
    ; A PAIR THE HDMA IS DRIVING IS NOT WRITTEN HERE.  The BGOFS latch is
    ; shared by all eight registers, and an HDMA scroll channel transfers its
    ; four bytes {HOFS lo, HOFS hi, VOFS lo, VOFS hi} back to back on the
    ; strength of exactly that: a write from the CPU landing between two of
    ; them leaves the second register holding the first one's byte.  The body
    ; runs in vblank, where no HDMA transfers happen, so today this is belt
    ; and braces -- but a frame body that ever ran past V=0 would corrupt the
    ; picture in a way no gate here could see.  The channel carries the
    ; snapshot's value on every line anyway (rows 0..39 are its letterbox
    ; hold), so skipping the write loses nothing.
    lda.b $73
    lsr a               ; b0 -> carry: the BG pair is on the HDMA
    bcs GbcScrollWin
    rep #$20
    lda.l !GBC_SC+!GBC_ST_SCX
    and.w #$00FF
    sec
    sbc.w #48
    and.w #$03FF
    ldx.w #$210F
    jsr GbcScrollW      ; BG2HOFS
    ldx.w #$2113
    jsr GbcScrollW      ; BG4HOFS
    lda.l !GBC_SC+!GBC_ST_SCY
    and.w #$00FF
    sec
    sbc.w #41
    and.w #$03FF
    ldx.w #$2110
    jsr GbcScrollW      ; BG2VOFS
    ldx.w #$2114
    jsr GbcScrollW      ; BG4VOFS
    sep #$20
GbcScrollWin:
    ; ⚡ THE TWO HALVES OF THE WINDOW PAIR ARE GATED SEPARATELY (wire $02).
    ; They are armed together except in the one case TABLE SPEC S13 describes:
    ; with both scroll pairs and an LCDC change in the same frame the LCDC
    ; group takes ch3, and then BG3 -- the window's CARPET -- is not on the
    ; HDMA at all and has to carry the snapshot's value, written HERE.  ch2
    ; keeps moving BG1, the window's content, per line.
    rep #$20
    lda.l !GBC_SC+!GBC_ST_WX
    and.w #$00FF
    clc
    adc.w #41
    eor.w #$FFFF
    inc a               ; -(WX + 41), mod 1024
    and.w #$03FF
    sta.b $5B
    lda.l !GBC_SC+!GBC_ST_WY
    and.w #$00FF
    clc
    adc.w #41
    eor.w #$FFFF
    inc a               ; -(WY + 41)
    and.w #$03FF
    sta.b $5D
    sep #$20
    lda.b $73
    and.b #$02          ; ch2 is driving BG1
    bne GbcScrollW3
    rep #$20
    lda.b $5B
    ldx.w #$210D
    jsr GbcScrollW      ; BG1HOFS
    lda.b $5D
    ldx.w #$210E
    jsr GbcScrollW      ; BG1VOFS
    sep #$20
GbcScrollW3:
    lda.b $73
    and.b #$08          ; ch3 is driving BG3
    bne GbcScrollDone
    rep #$20
    lda.b $5B
    ldx.w #$2111
    jsr GbcScrollW      ; BG3HOFS
    lda.b $5D
    ldx.w #$2112
    jsr GbcScrollW      ; BG3VOFS
    sep #$20
GbcScrollDone:
    rts

; A(16) = 10-bit offset, X = register address.  A survives the call.
GbcScrollW:
    pha
    sep #$20
    sta.w $0000,x
    xba
    sta.w $0000,x
    rep #$20
    pla
    rts

; ===========================================================================
; THE RASTER COMPILER -- contract sec. 8 (the mid-frame log) into sec. 11.4
; (the six HDMA tables).
;
; WHAT IT IS.  The bridge logs every write the Game Boy's CPU made to SCX, SCY,
; WX, WY, LCDC, BGP, OBP0, OBP1, BCPD and OCPD while LY was on screen, with the
; line each one took effect on.  The snapshot alone -- which is all phase 2 had
; -- shows the state at the TOP of the frame, so a game that moves the scroll
; half way down the picture (every parallax, every status bar, every HUD split)
; came out with the split applied to the whole screen.  This compiles that log
; into run-length tables the SNES's own HDMA replays line by line.
;
; ⚡ wire $02 added one more table to it, and it is the one that needed the map
; views to change shape: LCDC.3 (which of the Game Boy's two maps the
; background reads) and LCDC.4 (which of two chr bases a tile index means) are
; SNES registers now, four adjacent ones, so ONE mode-4 channel replays both of
; them per line.  A HUD drawn by swapping maps half way down the frame -- the
; Oracle games, Metal Gear Solid, Dragon Warrior III -- used to come out as the
; wrong map over the whole screen, or as a flat colour when the map the
; snapshot named was not the one being shown.
;
; WHERE IT RUNS.  Not in the NMI.  The worst case the contract allows -- 512 log
; entries and four colour channels -- is longer than one frame, and an NMI that
; had not returned by the time the next one arrives would lose that frame's
; SYNC, COMMIT and CONSUMED, the genlock's phase reference among them.  So the
; frame body only COPIES the log (by DMA, inside the transfer window, before
; CONSUMED, charged 4*LOG_N + !GBC_DMACOST like everything else) and the main
; loop compiles from that copy while the display is active and the transfer
; engine has nothing left to do.  The NMI is free to interrupt it:
;   * every byte of compiler state lives in direct page $80-$FF, which the
;     frame body never touches;
;   * the compile writes the HDMA set the prologue is NOT publishing;
;   * while a compile is in flight the body does not re-copy the log or the
;     CGRAM view, because those are the buffers being read.
; A compile that does not finish inside one frame simply finishes in the next,
; and the picture keeps the tables it already had -- the same 1-frame transient
; contract sec. 12.10 already allows when a raster pattern changes.
;
; THE THREE BANDS (sec. 11.4 / the phase-4 golden's TABLE SPEC):
;   rows   0.. 39  the top letterbox: every channel HOLDS the snapshot's value
;   rows  40..183  the Game Boy's 144 lines: the log plays out here, ly_eff = k
;                  landing on row 40+k and standing until something else moves
;   rows 184..223  the bottom letterbox: every channel HOLDS what row 183 left
; Entry i of a table drives visible line i+1, so entry i IS framebuffer row i.
;
; A LOG ENTRY WITH ly_eff = 144 IS DROPPED: it is not visible this frame and it
; is already inside the next snapshot (sec. 7), so applying it would show it
; twice.
; ===========================================================================

; ---------------------------------------------------------------------------
; GbcRasterArm -- the frame body's half: decide whether the tables have to be
; rebuilt, and if so copy the inputs into WRAM while the window is still open.
; ---------------------------------------------------------------------------
GbcRasterArm:
    lda.b $80
    cmp.b #$02
    bne GbcRaGo         ; a compile in flight owns those buffers; leave them
    rts
GbcRaGo:
    stz.b $84           ; the CGRAM copy is only this frame's if made below

    ; --- has anything the tables depend on moved? --------------------------
    ; The phase-2 detector (WX/WY/LCDC.5/white) widened by the log, which is
    ; an input like any other.  A log that is not empty rebuilds every frame;
    ; one that JUST went empty rebuilds once more, so the empty tables reach
    ; both sets.
    stz.b $10
    lda.l !GBC_SC+!GBC_ST_LOGN
    ora.l !GBC_SC+!GBC_ST_LOGN+1
    beq GbcRaLogEmpty
    lda.b #$01
    sta.b $10
    sta.b $85
    bra GbcRaW1
GbcRaLogEmpty:
    lda.b $85
    beq GbcRaW1
    stz.b $85
    lda.b #$01
    sta.b $10
GbcRaW1:
    lda.l !GBC_SC+!GBC_ST_WX
    cmp.b $07
    bne GbcRaChanged
    lda.l !GBC_SC+!GBC_ST_WY
    cmp.b $6F
    bne GbcRaChanged
    lda.l !GBC_SC+!GBC_ST_LCDC
    eor.b $3D
    and.b #$20          ; only LCDC.5 reaches the window table
    bne GbcRaChanged
    jsr GbcW1Blank
    cmp.b $6E
    beq GbcRaStore      ; white<->live flips the table between "as computed"
GbcRaChanged:           ; and "empty", so it is part of the change test
    lda.b #$01
    sta.b $10
GbcRaStore:
    lda.l !GBC_SC+!GBC_ST_WX
    sta.b $07
    lda.l !GBC_SC+!GBC_ST_WY
    sta.b $6F
    lda.l !GBC_SC+!GBC_ST_LCDC
    sta.b $3D
    jsr GbcW1Blank
    sta.b $6E
    lda.b $10
    beq GbcRaOwed
    lda.b #$03
    sta.b $82           ; owed to BOTH sets: the HDMA alternates between them,
GbcRaOwed:              ; so a table written once is two frames stale on the
    lda.b $82           ; set that did not get it
    beq GbcRaOut

    ; --- copy the inputs ---------------------------------------------------
    stz.b $80           ; ⚡ disarmed while they are rewritten: a claim still
                        ; waiting on the last inputs ($80 = 1: a set that does
                        ; not owe, or the prologue's re-arm) must not compile a
                        ; new snapshot against the old log if the V guard below
                        ; refuses -- the debt stands and the next arm retries
    jsr GbcRasterSnap
    jsr GbcLogFetch
    bcc GbcRaOut        ; the V guard refused: nothing is armed, retry next
    lda.b $83           ; frame
    beq GbcRaNoCram
    jsr GbcCramFetch
GbcRaNoCram:
    lda.b #$01
    sta.b $80           ; armed.  The TARGET SET is not chosen here: the main
                        ; loop picks it when it claims the compile, out of the
                        ; $18 it has just frozen (see GbcRrGo)
GbcRaOut:
    rts

; A(8) = 1 when the screen is the LCD-off / first-frame white one.  Reads the
; status block rather than $07 so the window compiler does not depend on having
; been reached through GbcRegs' white branch.
GbcW1Blank:
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    ora.l !GBC_WHITE_FIRST  ; GbcWhiteHold
    and.b #(!GBC_F_LCDON|!GBC_F_FIRST)
    cmp.b #!GBC_F_LCDON
    beq GbcW1BlankNo
    lda.b #$01
    rts
GbcW1BlankNo:
    lda.b #$00
    rts

; ---------------------------------------------------------------------------
; GbcRasterSnap -- freeze everything the compile reads out of the status block.
; The live copy is overwritten by the next frame's fetch, and a compile is
; allowed to straddle frames.
; ---------------------------------------------------------------------------
GbcRasterSnap:
    lda.l !GBC_SC+!GBC_ST_SCX
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+0
    lda.l !GBC_SC+!GBC_ST_SCY
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+1
    lda.l !GBC_SC+!GBC_ST_WX
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+2
    lda.l !GBC_SC+!GBC_ST_WY
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+3
    lda.l !GBC_SC+!GBC_ST_LCDC
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+4
    lda.l !GBC_SC+!GBC_ST_BGP
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+5
    lda.l !GBC_SC+!GBC_ST_OBP0
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+6
    lda.l !GBC_SC+!GBC_ST_OBP1
    sta.l !GBC_SNAP+!GBC_SNAP_REGS+7
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #!GBC_F_COMPAT
    beq GbcRsCgb
    lda.b #$01
GbcRsCgb:
    sta.l !GBC_SNAP+!GBC_SNAP_COMPAT
    jsr GbcW1Blank
    sta.l !GBC_SNAP+!GBC_SNAP_BLANK
    ; the twelve raw compat colours, +$40..+$57 of the status block
    ldx.w #$0000
GbcRsRaw:
    lda.l !GBC_SC+$40,x
    sta.l !GBC_SNAP+!GBC_SNAP_RAW,x
    inx
    cpx.w #$0018
    bne GbcRsRaw
    rts

; ---------------------------------------------------------------------------
; GbcLogFetch -- the log, by DMA, into WRAM.  Carry set = the compile may go
; ahead (LOG_N = 0 costs nothing and is a legitimate "go ahead": the tables
; still have to be built out of the snapshot alone).
; ---------------------------------------------------------------------------
GbcLogFetch:
    rep #$20
    lda.l !GBC_SC+!GBC_ST_LOGN
    cmp.w #!GBC_LOG_MAX+1
    bcc GbcLfClamped
    lda.w #!GBC_LOG_MAX  ; sec. 8: past 512 the bridge sets LOG_OVF and drops,
GbcLfClamped:            ; and what it kept is what we compile
    sta.l !GBC_SNAP+!GBC_SNAP_LOGN
    asl a
    asl a
    sta.b $1C            ; 4 bytes an entry
    sep #$20
    lda.b $1C
    ora.b $1D
    beq GbcLfNone
    jsr GbcDmaGuard
    bcc GbcLfNo
    lda.b #(!GBC_LOG_OFF&$FF)
    sta.w $2181
    lda.b #(!GBC_LOG_OFF>>8)
    sta.w $2182
    stz.w $2183          ; WRAM port -> $7E:!GBC_LOG_OFF (bit 0 = bank select)
    lda.b #$00
    sta.w $4370          ; ch7 DMAP: A->B, increment, mode 0
    lda.b #$80
    sta.w $4371          ; BBAD = $2180 (WMDATA)
    ldx.w #!GBC_LOG_A16
    stx.w $4372
    lda.b #!GBC_LOG_A1B
    sta.w $4374
    rep #$20
    lda.b $1C
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.b $1C
    jsr GbcDebit
    sep #$20
GbcLfNone:
    sec
    rts
GbcLfNo:
    jsr GbcDropCount
    clc
    rts

; ---------------------------------------------------------------------------
; GbcCramFetch -- the CGRAM view into WRAM, for the CGB colour replay.
;
; A BCPD write carries ONE byte of a 15-bit colour, so the other byte has to
; come from the state.  In CGB mode the view IS that state byte for byte: entry
; 4p+k of the CGRAM view is BG[p][k] and entry 128+16q+k is OB[q][k] (contract
; sec. 4.4), so the compiler replays its own copy of the view exactly as the
; hardware replays CRAM.  The ONE hole is entry 0, which sec. 4.4 pins at $0000
; for the backdrop: BG[0][0] is not in the view, and it is patched in from the
; raw compat colours the status block carries.
;
; ⚠ WHICH MAKES COMPAT_RAW +$40..+$41 NORMATIVE IN CGB TOO, not just in compat
; -- the field's name is what misleads.  BG[0][0] is the colour BOTH carpets
; take for a BCPD write with k = 0, and the status block is the only place the
; player can get it.  Contract sec. 5, sec. 4.4 and invariant 13.19 say so; a
; bridge that zeroed the field in CGB "because only compat reads it" would take
; the carpets' colour away and nothing would report it.
;
; Copied only when a CGB palette write can actually be PLACED -- compat takes
; its colours from the status block instead, and with no spare channel the
; compile only needs the counters.  The frame after a game STARTS writing
; palettes mid-frame therefore compiles one frame late (GbcPass1 gives up and
; asks for the copy); from then on it is exact.
; ---------------------------------------------------------------------------
GbcCramFetch:
    rep #$20
    lda.w #512
    sta.b $1C
    sep #$20
    jsr GbcDmaGuard
    bcc GbcCfNo
    lda.b #(!GBC_CRAM_OFF&$FF)
    sta.w $2181
    lda.b #(!GBC_CRAM_OFF>>8)
    sta.w $2182
    stz.w $2183
    lda.b #$00
    sta.w $4370
    lda.b #$80
    sta.w $4371
    ldx.w #!GBC_CGRAM_A16
    stx.w $4372
    lda.b #!GBC_CGRAM_A1B
    sta.w $4374
    rep #$20
    lda.b $1C
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.b $1C
    jsr GbcDebit
    sep #$20
    ; entry 0 of the view is the backdrop, not BG[0][0]: patch it back in
    lda.l !GBC_SNAP+!GBC_SNAP_RAW+0
    sta.l !GBC_WRAM_LONG+!GBC_CRAM_OFF+0
    lda.l !GBC_SNAP+!GBC_SNAP_RAW+1
    sta.l !GBC_WRAM_LONG+!GBC_CRAM_OFF+1
    lda.b #$01
    sta.b $84
    rts
GbcCfNo:
    jsr GbcDropCount
    rts

; ---------------------------------------------------------------------------
; GbcHdmaPublish -- the NMI prologue's half: hand the finished set to the HDMA.
;
; Contract sec. 11.4/13.11: the channels re-read $43x2/$43x3 at the V=0 init and
; $420C may only be written here, so this is the one place a table or a channel
; role can move.  A compile that is still running keeps the CURRENT set on the
; bus -- its own set is half written.
; ---------------------------------------------------------------------------
; ⚡ THE 5A22 TAKES THE LOWEST HDMA CHANNEL.
;
; Measured on a Mk.III (Super Mario Bros. Deluxe, Genlock AND Exato): about
; one frame in 75, the HDMA channel with the LOWEST number that is active
; stopped dead somewhere around lines 9..33 -- line counter frozen, table
; address still on entry 0, from there to the end of the frame -- and the
; whole picture showed the HUD's SCY.  Nothing wrote $420C or the channel's
; registers in those frames (an FPGA snoop counted exactly one $420C write per
; frame, the prologue's), so it is the CPU itself.  It follows the general
; DMAs: with none of them after V=0 the rate fell by ~3x, and writing $420C
; again right after each one cut it by ~9x.  Moving the channel showed who it
; is: with logical channel 0 on ch7 the victim became ch1, the new lowest one.
;
; So ch0 is a DECOY: a channel with nothing to say (one byte to $21FF, which
; no device on the B bus decodes, every 127 lines) that is armed whenever
; anything is, and is therefore always the lowest active channel.  Logical
; channel 0 -- the compiler's first raster channel and the FB's VOFS stretch
; -- lives on ch6 instead, and every general DMA runs on ch7.  The FB mode's
; own sets need no decoy: their lowest channel is ch4 and its table is the
; window shut on every line (GbcTblW1), the same frozen or not.  Every general
; DMA is followed by the mask again (!GBC_HDMAEN_SH into $420C, two
; instructions inline: the FB mode fires many small DMAs, and a call per DMA
; cost it a starved row in the freshness gate), so a decoy that was hit is
; armed again before the next DMA and a second hit in the same frame still
; lands on it.  Re-writing a bit that is already set is a no-op, and a
; channel that terminated stays terminated (termination is not $420C).
; Measured with the decoy: 0 real channels stopped in 7 000 frames.
; The cost is one channel's line overhead (2 byte-equivalents a line in the
; model, see !GBC_BPLHDMA) and the tail of the window shrinking by 80 B.
;
; A(8) = a $420C mask in LOGICAL channels (the compiler's 0..3, the window 4,
; the letterbox 5) -> the PHYSICAL mask: logical 0 lives on ch6 and ch0 is
; the decoy, armed whenever anything is.  Z from the caller's load.
GbcHdmaMask:
    beq GbcHmOut        ; nothing armed stays nothing
    lsr a
    bcc GbcHmNo0
    ora.b #$20          ; logical 0 -> bit 6 (before the shift back)
GbcHmNo0:
    asl a
    ora.b #$01          ; the decoy
GbcHmOut:
    rts

; $43x0 of logical channels 0..3.
GbcHpChReg:
    dw $4360, $4310, $4320, $4330

; GbcHdmaMask as a table, for the compile (!GBC_H_PMASK).  ⚡ Not just speed:
; the FB mode's freshness gate (run_gbc_player.sh, C6-FRESH "DKC language
; selector") sits between two stable schedules, and ~25 cycles more in the
; prologue were enough to tip it into the one whose worst row waits 4 frames
; instead of 3.  The prologue therefore reads a mask the compile already
; converted and costs what it cost before the decoy.
GbcHdmaMaskTab:
!i = 0
while !i < 64
    if !i == 0
        db $00
    else
        db (!i&$3E)|((!i&$01)<<6)|$01
    endif
    !i #= !i+1
endwhile

; The decoy's table: three 127-line holds, one byte each, so ch0 is active on
; every line of every frame and the terminator is never reached.
GbcTblDecoy:
    db $7F,$00,$7F,$00,$7F,$00,$00

GbcHdmaPublish:
    lda.b $80
    cmp.b #$02
    bne GbcHpLive
    rts                 ; a compile is half way through the other set
GbcHpLive:
    cmp.b #$03
    bne GbcHpFlip
    stz.b $80           ; the compile finished: its set goes live now
    ; ⚡ ... and if the OTHER set still owes these tables, the inputs the
    ; compile read are still in WRAM (only GbcRasterArm rewrites them), so it
    ; is armed again right here.  Without this the second set was served only
    ; by the NEXT arm, and an arm always lands one flip after a publish: when
    ; every other frame is deferred (no status, the arm never runs) with a log
    ; that re-owes both sets on every arm, every compile landed in the same set
    ; and the other kept the previous scene's tables for good.
    lda.b $82
    beq GbcHpFlip
    inc.b $80           ; 1 = armed, the same inputs
GbcHpFlip:
    lda.b $18
    eor.b #$01
    sta.b $18
    beq GbcHpSetA       ; tested here, not after the rep: $18/$19 are separate
    rep #$20            ; bytes and a 16-bit load would drag the parked IGR
    lda.w #!GBC_SETB_OFF ; command in with it
    bra GbcHpSetGo
GbcHpSetA:
    rep #$20
    lda.w #!GBC_SETA_OFF
GbcHpSetGo:
    sta.b $10
    clc
    adc.w #!GBC_T_W1
    sta.w $4342         ; ch4 A1T = window 1
    lda.b $10
    clc
    adc.w #!GBC_T_INIDISP
    sta.w $4352         ; ch5 A1T = the letterbox (static, re-pointed anyway)
    sep #$20
    lda.b #!GBC_WRAM_A1B
    sta.w $4344
    sta.w $4354

    stz.b $73           ; rebuild the published roles
    stz.b $13           ; channel
    stz.b $14           ; channels armed among ch0..ch3
GbcHpChLoop:
    rep #$20
    lda.b $13
    and.w #$00FF
    asl a
    asl a
    clc
    adc.b $10
    adc.w #!GBC_H_CH
    tay                 ; Y = the header entry of this channel
    lda.b $13
    and.w #$00FF
    asl a
    tax
    lda.l GbcHpChReg,x  ; logical channel 0 lives on ch6 (see GbcHdmaMask)
    tax                 ; X = its $43x0
    sep #$20
    lda [!GBC_PTR],y
    sta.b $12           ; B-bus address; 0 = the compile left it disarmed
    beq GbcHpChNext
    sta.w $0001,x       ; $43x1
    iny
    rep #$20
    lda [!GBC_PTR],y
    clc
    adc.b $10
    sta.w $0002,x       ; $43x2/$43x3 = A1T
    sep #$20
    iny
    iny                 ; +3 of the header entry: the DMAP the compile chose.
    lda [!GBC_PTR],y    ; ⚡ mode 3 (two registers, four bytes) for a scroll or
    sta.w $0000,x       ; colour channel, mode 4 (FOUR registers) for the LCDC
                        ; group, which is why it is carried and not assumed
    lda.b #!GBC_WRAM_A1B
    sta.w $0004,x       ; $43x4 = A1B
    inc.b $14
    lda.b $12
    jsr GbcHpRole
    ora.b $73
    sta.b $73
GbcHpChNext:
    inc.b $13
    lda.b $13
    cmp.b #$04
    bne GbcHpChLoop

    ; ⚡ A colour channel leaving the bus: count it for GbcFold, which re-sends
    ; the CGRAM view (the channel's writes stay in CGRAM).  Counted HERE and
    ; not in the body because only the prologue sees every flip; $00/$01 are
    ; written by nothing else.
    lda.b $73
    and.b #$04
    cmp.b $00
    beq GbcHpColSame
    sta.b $00           ; this set's colour bit, for the next flip
    and.b #$04          ; (a store sets no flag: test the bit again)
    bne GbcHpColSame    ; colour came ON: nothing to repair
    inc.b $01           ; colour went OFF
GbcHpColSame:

    ; The tail of window A is worth less per line the more channels are up
    ; (contract sec. 11.1); see the note on !GBC_BPLHDMA.
    rep #$20
    lda.b $14
    and.w #$00FF
    tax
    sep #$20
    lda.l GbcHpBplTab,x
    sta.b !GBC_DP_HDMA
    rep #$20
    txa
    asl a
    tax
    lda.l GbcHpTailTab,x
    sta.b !GBC_DP_HDMA+1
    sep #$20

    lda.b $06
    bne GbcHpArm
    stz.b $73           ; the screen has not been released yet: $420C stays 0,
    rts                 ; so no channel is on the bus and no role is published
GbcHpArm:
    rep #$20
    lda.b $10
    clc
    adc.w #!GBC_H_PMASK ; the physical mask, which the compile worked out (see
    tay                 ; GbcHdmaMaskTab: a few cycles here move the FB drain)
    sep #$20
    lda [!GBC_PTR],y
    sta.w $420C
    sta.l !GBC_HDMAEN_SH
GbcHpOut:
    rts

; A(8) = the B-bus address of an armed channel -> the $73 role bit it means.
; The frame body reads $73 to know what it must NOT write (the BGOFS latch is
; shared by all eight scroll registers) and when CGRAM traffic has to stay in
; true vblank, so every armed channel has to name itself here.  ⚡ wire $02:
; $0D and $11 are separate bits, because the LCDC group can take ch3 and leave
; ch2 -- see TABLE SPEC S13 and GbcScrollWin.
GbcHpRole:
    cmp.b #$21
    beq GbcHpRoleCol
    cmp.b #$0F
    beq GbcHpRoleBg
    cmp.b #$13
    beq GbcHpRoleBg
    cmp.b #$0D
    beq GbcHpRoleW1
    cmp.b #$11
    beq GbcHpRoleW3
    cmp.b #$08
    beq GbcHpRoleLc
    lda.b #$00
    rts
GbcHpRoleCol:
    lda.b #$04          ; a channel is writing CGRAM every line it is armed
    rts
GbcHpRoleBg:
    lda.b #$01          ; ch0/ch1 are the BG scroll pair
    rts
GbcHpRoleW1:
    lda.b #$02          ; ch2 drives BG1, the window's content
    rts
GbcHpRoleW3:
    lda.b #$08          ; ch3 drives BG3, the window's carpet
    rts
GbcHpRoleLc:
    lda.b #$10          ; a channel is driving $2108-$210B per line
    rts

; 168 - (C + B) with ch5 (1 channel, 1 byte), ch4 (1, 2) and n of ch0..ch3
; (1, 4 each), and that times !GBC_DEADLINE.  The LCDC group is mode 4 and
; still four bytes a line, so it costs exactly what a scroll channel costs and
; the table below did not have to move.
GbcHpBplTab:
    db 161, 156, 151, 146, 141
GbcHpTailTab:
    dw 6440, 6240, 6040, 5840, 5640

; ---------------------------------------------------------------------------
; GbcRasterRun -- the main loop's half.  One armed compile, run to completion
; (interrupted by however many NMIs it takes).
; ---------------------------------------------------------------------------
GbcRasterRun:
    sep #$20
    rep #$10
    lda.b $80
    cmp.b #$01
    beq GbcRrGo
    rts
GbcRrGo:
    ; CLAIM FIRST, then read $18.  From the moment $80 is 2 the prologue
    ; neither flips $18 nor re-arms, so the set on the bus is frozen and the
    ; target can be derived from it.  Choosing the target in GbcRasterArm
    ; instead left a window -- three instructions wide, but real -- in which an
    ; NMI landing between the test above and this store flipped $18 under an
    ; $81 that had already been written, and the compile then wrote the set
    ; the HDMA was reading.
    lda.b #$02
    sta.b $80
    lda.b $18
    eor.b #$01
    sta.b $81           ; compile into the set the prologue is NOT publishing
    ; ⚡ THE DEBT IS PER SET, NOT A COUNT.  It used to be "two compiles owed",
    ; which is only the same thing when two consecutive compiles land in two
    ; different sets -- i.e. when exactly ONE prologue flip separates them.  A
    ; frame whose body never reached GbcRasterArm (a deferred frame: no status,
    ; SNAP_VALID = 0) still flips, so the second compile landed in the set the
    ; first one had just written, the debt reached zero, and the other set kept
    ; the PREVIOUS scene's tables for good: the HDMA alternated between the two
    ; every frame (Dragon Warrior III: the fade's colour channels coming back
    ; on every other frame of a static menu).  So the target has to be one
    ; that still owes; if it is not, the set that owes is the one on the bus,
    ; and the next prologue flips it out.  (What guarantees the owed set is
    ; served at all is the prologue's re-arm in GbcHdmaPublish; this wait only
    ; saves the compile of a set that owes nothing.)
    inc a               ; 1 = set A, 2 = set B: its bit in $82
    and.b $82
    bne GbcRrOwed
    lda.b #$01
    sta.b $80           ; still armed: claim again after the next flip
    rts
GbcRrOwed:
    jsr GbcCompile
    bcc GbcRrAbort
    ; The debt is paid BEFORE the state leaves 2, for the same reason: at $80
    ; = 3 the prologue consumes the compile and GbcRasterArm runs again, and an
    ; arm that owed both sets again in that window would lose one of them to
    ; the clear below -- the picture would alternate between this raster and
    ; the last one.
    lda.b $81
    inc a
    trb.b $82           ; this set now carries these tables
GbcRrPub:
    lda.b #$03
    sta.b $80           ; done: the next prologue publishes it
    rts
GbcRrAbort:
    stz.b $80           ; the compile needs something this frame did not copy;
    rts                 ; the debt stands and the next frame tries again

; ---------------------------------------------------------------------------
; GbcCompile -- snapshot + log -> the six tables of one set.  Carry set = done.
; ---------------------------------------------------------------------------
GbcCompile:
    jsr GbcRctrClear
    lda.b $81
    beq GbcCoSetA
    rep #$20
    lda.w #!GBC_SETB_OFF
    bra GbcCoSetGo
GbcCoSetA:
    rep #$20
    lda.w #!GBC_SETA_OFF
GbcCoSetGo:
    sta.b $96           ; the set being written
    sep #$20
    jsr GbcPass1
    bcc GbcCoAbort
    jsr GbcPassRegs     ; ⚡ before the colour pass, whose state takes the
                        ; encoders' direct page
    jsr GbcPassColour
    jsr GbcSetHeader
    sec
    rts
GbcCoAbort:
    clc
    rts

; ---------------------------------------------------------------------------
; PASS 1 -- digest the log.
;
; Out of 512 entries it builds at most 144 rows of {row, SCX, SCY, WX, WY,
; LCDC}: the register state as of every line the log touches one of them on.
; Every later pass walks THAT, so the 512-entry worst case is paid once instead
; of once per table.  Kinds that carry no raster register (the palettes) still
; run through here for the counters and for the kind mask the channel
; allocation is made of.
;
; ⚡ It also COUNTS what the colour walk will be asked for (P5B).  How many
; CGRAM pairs an entry is worth depends only on its kind, the mode and -- for
; OCPD -- two bits of the index (TABLE SPEC S8), so the counters colour_
; requested and colour_noop are sums over the log, and the colour walk no longer
; has to visit the entries it can no longer place just to count them: it stops
; the moment its row cursor passes 183 (see GbcPassColour).  Two sums, because
; the two ways the walk ends count different prefixes of the log:
;   * with spares, ingestion stops at the FIRST entry with ly_eff >= 144, of any
;     kind -- that entry parks the chronological cursor for the rest of the
;     frame (the note at GbcPassColour);
;   * with no spare at all every entry with ly_eff < 144 is counted, past such
;     an entry too.
; A BCPD in CGB -- the whole of a hi-colour log -- runs through a four-
; instruction test in a loop of its own (GbcP1FastB), because on a 512-entry
; log this pass is walked once more than the colour pass is.
;
; A LOG SENTINEL is written right after the last entry: ly_eff = $FF, which no
; entry of this frame can carry past the checks below.  It is what lets the
; tight loops here and in the colour walk run without an end-of-log test --
; the sentinel fails their fast test and the general path finds the end.
; ---------------------------------------------------------------------------
!P1_REQ  = $9A          ; requested pairs so far (all entries with ly < 144)
!P1_NOOP = $9C          ; colour_noop so far
!P1_CNTB = $9E          ; where the fast loop's run began / its length
!P1_FK   = $F4          ; ... the kind it runs on, << 8
!P1_FC   = $AE          ; ... and what each of its entries is worth
!P1_PFXR = $A0          ; P1_REQ at the first ly >= 144; bit 15 = not reached
!P1_PFXN = $A2          ; P1_NOOP at the same point
!P1_DCUR = $A4          ; digest write cursor
!P1_LCDC34 = $A6        ; non-zero = some LCDC write moves b3/b4 off the
                        ; snapshot's: only then can GbcLcdcScan find a row
GbcPass1:
    jsr GbcLoadSnapRegs
    lda.l !GBC_SNAP+!GBC_SNAP_COMPAT
    sta.b $90
    lda.l !GBC_SNAP+!GBC_SNAP_BLANK
    sta.b $91
    stz.b $A9
    lda.b #$FF
    sta.b $98           ; no row pending (rows are 0..143)
    rep #$20
    lda.l !GBC_SNAP+!GBC_SNAP_LOGN
    sta.l !GBC_RC_LOGN
    asl a
    asl a               ; 4 bytes an entry
    clc
    adc.w #!GBC_LOG_OFF
    sta.b $92           ; the end of the log copy
    tay
    sep #$20
    lda.b #$FF
    sta [!GBC_PTR],y    ; the sentinel: ly_eff = $FF right after the last entry
    rep #$20
    stz.b $94           ; rows in the digest
    lda.w #!GBC_ROWS_OFF
    sta.b !P1_DCUR      ; where the next one goes
    stz.b !P1_LCDC34
    stz.b !P1_REQ
    stz.b !P1_NOOP
    lda.w #$FFFF
    sta.b !P1_PFXR
    ldy.w #!GBC_LOG_OFF
GbcP1Loop:              ; A 16-bit
    cpy.b $92
    bcc GbcP1Entry
    brl GbcP1End
GbcP1Entry:
    lda [!GBC_PTR],y    ; ly_eff | kind << 8
    cmp.w #$0A00
    bcs GbcP1BadK       ; kind >= 10, whatever the line
    sta.b $AA           ; $AA = ly_eff, $AB = kind
    and.w #$00FF
    cmp.w #144
    bcs GbcP1After
    lda.b $AB
    and.w #$00FF
    cmp.w #$0005
    bcc GbcP1Reg
    brl GbcP1Colour
GbcP1BadK:
    and.w #$00FF
    cmp.w #144
    bcc GbcP1BadKCnt
    jsr GbcP1Pfx        ; it still parks the colour walk: the line is tested first
GbcP1BadKCnt:
    ldx.w #(!GBC_RC_BADK-!GBC_RCTR)
    jsr GbcRctrInc
    rep #$20
    bra GbcP1Next
GbcP1After:
    jsr GbcP1Pfx
    ldx.w #(!GBC_RC_AFTER-!GBC_RCTR)
    jsr GbcRctrInc      ; sec. 7: already inside the next snapshot
    rep #$20
GbcP1Next:
    tya
    clc
    adc.w #$0004
    tay
    bra GbcP1Loop

GbcP1Reg:               ; A 16-bit = kind 0..4
    sep #$20
    cmp.b #$02
    bcs GbcP1NotBg
    lda.b $A9
    ora.b #$01          ; the log carries SCX or SCY
    sta.b $A9
    bra GbcP1Row
GbcP1NotBg:
    cmp.b #$04
    bcs GbcP1Row        ; 4 = LCDC, which belongs to no pair
    lda.b $A9
    ora.b #$02          ; the log carries WX or WY
    sta.b $A9
GbcP1Row:
    lda.b $98
    cmp.b #$FF
    beq GbcP1RowKeep
    cmp.b $AA
    beq GbcP1RowKeep
    phy
    jsr GbcRowEmit      ; the row we were on is finished
    ply
GbcP1RowKeep:
    lda.b $AA
    sta.b $98
    iny
    iny
    iny
    lda [!GBC_PTR],y
    sta.b $AC           ; value
    iny                 ; Y = the next entry
    lda.b $AB
    cmp.b #$04
    bne GbcP1RowStore
    lda.b $AC
    eor.b $8C
    and.b #$C7          ; ⚡ b5 is window 1 and b3/b4 are the LCDC group; what
                        ; is left (b0 b1 b2 b6 b7) is sec. 12.5, not a table
    beq GbcP1Lcdc34
    ldx.w #(!GBC_RC_LCDC-!GBC_RCTR)
    jsr GbcRctrInc
GbcP1Lcdc34:
    lda.b $AC
    eor.l !GBC_SNAP+!GBC_SNAP_REGS+4
    and.b #$18
    tsb.b !P1_LCDC34
GbcP1RowStore:
    rep #$20
    lda.b $AB
    and.w #$00FF
    tax
    sep #$20
    lda.b $AC
    sta.b $88,x         ; the live registers are indexed by log kind
    rep #$20
    brl GbcP1Loop

; A 16-bit = kind 5..9, ly_eff < 144.  BGP/OBP0/OBP1 are registers too, but no
; table follows them per line; what is kept is the kind mask and the counts.
GbcP1Colour:
    sep #$20
    lda.b $A9
    ora.b #$10
    sta.b $A9
    lda.b $AB
    cmp.b #$08
    bcc GbcP1Pal        ; 5..7
    beq GbcP1Bcpd
    lda.b $A9
    ora.b #$08          ; 9 = OCPD
    sta.b $A9
    lda.b $90
    bne GbcP1Noop       ; sec. 8: in compat the hardware ignores OCPD
    iny
    iny
    lda [!GBC_PTR],y    ; idx
    dey
    dey
    rep #$20
    ; OCPD in CGB is the one kind whose worth depends on the entry: one pair,
    ; or nothing for k = 0 (sec. 4.4 pins that entry at $0000).  Its run
    ; tests k per entry.
GbcP1Ocpd:
    iny
    iny
    lda [!GBC_PTR],y    ; idx | value << 8
    iny
    iny
    and.w #$0006
    beq GbcP1OcZero
    inc.b !P1_REQ
    bra GbcP1OcNext
GbcP1OcZero:
    inc.b !P1_NOOP
GbcP1OcNext:
    lda [!GBC_PTR],y
    eor.w #$0900
    cmp.w #144
    bcc GbcP1Ocpd
    brl GbcP1Loop
GbcP1Pal:
    lda.b $90
    beq GbcP1Noop       ; in CGB kB(k) = k: BGP/OBP move no view byte
    lda.b $AB
    cmp.b #$05
    bne GbcP1Obp
    lda.b #8            ; BGP, compat: eight pairs
    bra GbcP1Run
GbcP1Obp:
    lda.b #3            ; OBP0/OBP1, compat: three
    bra GbcP1Run
GbcP1NoopOne:
    rep #$20
    inc.b !P1_NOOP
    brl GbcP1Next
GbcP1Bcpd:
    lda.b $A9
    ora.b #$04
    sta.b $A9
    lda.b $90
    bne GbcP1Noop       ; sec. 8: in compat the hardware ignores BCPD
    lda.b #2            ; two pairs, whatever k is
    bra GbcP1Run
GbcP1Noop:
    lda.b #0            ; worth nothing: counts colour_noop instead
    ; A(8) = what this entry is worth in pairs, 0 = a noop.  Every entry of the
    ; SAME kind that follows, with ly_eff < 144, is worth exactly the same --
    ; worth depends on the kind and the mode only (OCPD in CGB, above, is the
    ; exception) -- so the run is counted by how far Y gets, and the tight loop
    ; below is all a hi-colour log costs here.  The EOR leaves the line when
    ; the kind matches and a number >= 256 when it does not, so ONE compare
    ; tests both.  Two entries a turn: the code is fetched from slow ROM.
GbcP1Run:
    rep #$20
    and.w #$00FF
    sta.b !P1_FC
    lda.b $AB
    and.w #$00FF
    xba
    sta.b !P1_FK        ; kind << 8
    sty.b !P1_CNTB      ; the run starts at this entry
    tya
    clc
    adc.w #$0004
    tay
GbcP1Fast:
    lda [!GBC_PTR],y
    eor.b !P1_FK
    cmp.w #144
    bcs GbcP1FastOut
    tya
    adc.w #$0004        ; carry is clear: the bcs above was not taken
    tay
    lda [!GBC_PTR],y
    eor.b !P1_FK
    cmp.w #144
    bcs GbcP1FastOut
    tya
    adc.w #$0004
    tay
    bra GbcP1Fast
GbcP1FastOut:
    tya
    sec
    sbc.b !P1_CNTB
    lsr a
    lsr a               ; entries in the run
    ldx.b !P1_FC
    bne GbcP1FoReq
    clc
    adc.b !P1_NOOP
    sta.b !P1_NOOP
    brl GbcP1Loop
GbcP1FoReq:
    sta.b !P1_CNTB
    lda.b !P1_REQ
GbcP1FoMul:             ; + entries * worth (2, 3 or 8)
    clc
    adc.b !P1_CNTB
    dex
    bne GbcP1FoMul
    sta.b !P1_REQ
    brl GbcP1Loop       ; the general path re-reads the entry that broke the
                        ; run -- or finds the sentinel, i.e. the end

; A 16-bit.  The colour walk stops at the first entry whose ly_eff is past the
; picture, so what it will have been asked for is what was counted up to here.
GbcP1Pfx:
    lda.b !P1_PFXR
    bpl GbcP1PfxOut     ; only the first one counts
    lda.b !P1_REQ
    sta.b !P1_PFXR
    lda.b !P1_NOOP
    sta.b !P1_PFXN
GbcP1PfxOut:
    rts

GbcP1End:
    rep #$20
    jsr GbcP1Pfx        ; no such entry: the prefix is the whole log
    lda.b !P1_PFXR
    sta.l !GBC_CP_REQP
    lda.b !P1_PFXN
    sta.l !GBC_CP_NOOPP
    lda.b !P1_REQ
    sta.l !GBC_CP_REQA
    lda.b !P1_NOOP
    sta.l !GBC_CP_NOOPA
    sep #$20
    lda.b $98
    cmp.b #$FF
    beq GbcP1Scan
    jsr GbcRowEmit
GbcP1Scan:
    lda.b !P1_LCDC34
    beq GbcP1Spares     ; every row carries the snapshot's b3/b4: nothing to count
    jsr GbcLcdcScan
GbcP1Spares:
    ; sec. 11.4 / TABLE SPEC S13: scroll beats everything, the LCDC group comes
    ; next, colour takes what is left.  ch0/ch1 go to the BG pair and ch2/ch3
    ; to the window pair when the log CARRIES that kind -- a redundant write
    ; arms the pair just as well -- and the rest is handed out in ASCENDING
    ; channel order.  Ascending is not decoration: the HDMA serves channels
    ; 0..7 in order within a line, so the pair queued last is written last and
    ; two writes to one CGRAM index on one line land in chronological order.
    ; It is also what keeps a frame that does NOT move b3/b4 byte-identical to
    ; what wire $01 produced -- the LCDC group takes the LOWEST free channel,
    ; so when it takes none the spare list is exactly the old one.
    stz.b $A8
    lda.b $A9
    lsr a
    bcs GbcP1SpWin
    lda.b #$00
    sta.b $E0
    lda.b #$01
    sta.b $E1
    lda.b #$02
    sta.b $A8
GbcP1SpWin:
    lda.b $A9
    and.b #$02
    bne GbcP1SpLcdc
    rep #$20
    lda.b $A8
    and.w #$00FF
    tax
    sep #$20
    lda.b #$02
    sta.b $E0,x
    inx
    lda.b #$03
    sta.b $E0,x
    lda.b $A8
    clc
    adc.b #$02
    sta.b $A8
GbcP1SpLcdc:
    ; ⚡ THE LCDC GROUP (S12/S13).  Armed only when some VISIBLE row's LCDC
    ; differs from the snapshot in b3/b4 -- GbcLcdcScan has just measured that
    ; -- and never on the white screen, where $210A points at the static white
    ; map and a channel writing a carpet base over it would turn the white
    ; screen into a carpet (S12; the picture is one flat colour, so nothing is
    ; lost by leaving it disarmed).
    lda.b #$FF
    sta.b $86           ; no LCDC channel this frame
    lda.b $A9
    and.b #$40
    beq GbcP1SpDone
    lda.b $91
    bne GbcP1SpBlank
    lda.b $A8
    beq GbcP1SpEvict
    lda.b $E0
    sta.b $86           ; the lowest free channel
    ldx.w #(!GBC_RC_CEVICT-!GBC_RCTR)
    jsr GbcRctrInc      ; ... which is one colour channel the frame will not get
    ldx.w #$0000
GbcP1SpShift:
    lda.b $E1,x
    sta.b $E0,x
    inx
    cpx.w #$0003
    bne GbcP1SpShift
    dec.b $A8
    bra GbcP1SpDone
GbcP1SpEvict:
    ; Both pairs armed and nothing free: the group takes ch3 and the frame gets
    ; NO colour channel.  ch2 and ch3 carry identical bytes, so ch2 alone still
    ; moves BG1 -- the window's CONTENT -- per line; what stops following is
    ; BG3, its carpet, a flat tile whose only per-cell content is the palette.
    ; Taking ch2 instead would leave the window's content on the snapshot's WX
    ; while its edge (ch4, W1) kept moving, which draws the window in the wrong
    ; place; that was measured at 3840 px on this phase's anchor scene where
    ; this rule costs 0.
    lda.b #$03          ; ch3, the half of the window pair that drives BG3
    sta.b $86
    lda.b $A9
    ora.b #$20          ; GbcSetHeader must not arm ch3 as the window's carpet
    sta.b $A9
    ldx.w #(!GBC_RC_EVICT-!GBC_RCTR)
    jsr GbcRctrInc
    bra GbcP1SpDone
GbcP1SpBlank:
    ldx.w #(!GBC_RC_LBLANK-!GBC_RCTR)
    jsr GbcRctrInc
GbcP1SpDone:
    lda.b $A8
    sta.l !GBC_RC_SPARE
    ; Does this compile need the CGRAM view?  Only a CGB BCPD/OCPD reads it,
    ; and only when there is a spare channel to put the colour on.
    stz.b $83
    lda.b $90
    bne GbcP1Ok         ; compat reads the raw colours out of the status block
    lda.b $A8
    beq GbcP1Ok         ; no spare: the counters need no colour VALUE at all
    lda.b $A9
    and.b #$0C
    beq GbcP1Ok
    lda.b #$01
    sta.b $83
    lda.b $84
    bne GbcP1Ok
    clc                 ; ask the window for it and come back next frame
    rts
GbcP1Ok:
    sec
    rts

; Append {row, SCX, SCY, WX, WY, LCDC} for the row in $98, at !P1_DCUR.  Y is
; clobbered; leaves A 8-bit.
GbcRowEmit:
    rep #$20
    lda.b $94
    cmp.w #144
    bcs GbcReFull       ; 144 rows is every row the log can name
    inc a
    sta.b $94
    ldy.b !P1_DCUR
    sep #$20
    lda.b $98
    sta [!GBC_PTR],y
    rep #$20
    iny
    lda.b $88
    sta [!GBC_PTR],y    ; SCX, SCY
    iny
    iny
    lda.b $8A
    sta [!GBC_PTR],y    ; WX, WY
    iny
    iny
    lda.b $8C
    sta [!GBC_PTR],y    ; LCDC (and a byte of the next row, written over
    iny                 ; when it comes; the digest area has room past 144)
    sty.b !P1_DCUR
GbcReFull:
    sep #$20
    rts

; ---------------------------------------------------------------------------
; ⚡ GbcLcdcScan -- how many VISIBLE rows the Game Boy spends with a different
; LCDC.3 / LCDC.4 than the snapshot's, and therefore whether the LCDC group is
; armed at all (TABLE SPEC S12).
;
; It is a row count and not a write count on purpose: a game that writes LCDC
; twice on the same line, or writes back the value it started from, has moved
; nothing and must not cost a channel.  The walk is the same segment walk every
; other pass makes -- the digest row r starts a segment at framebuffer row
; 40 + r and it runs to the next digest row, or to the bottom -- with the two
; ends clamped to the picture: the top letterbox always carries the snapshot
; (S2) and rows 184..223 are not visible.
;
; Enter and leave with A 8-bit, X/Y 16-bit.
; ---------------------------------------------------------------------------
GbcLcdcScan:
    lda.l !GBC_SNAP+!GBC_SNAP_REGS+4
    and.b #$18
    sta.b $AC           ; the snapshot's b3/b4, the value every row is read
    lda.l !GBC_SNAP+!GBC_SNAP_REGS+4
    sta.b $8C           ; against; the walk carries LCDC alone
    rep #$20
    stz.b $98           ; the row the current segment starts on
    lda.b $94
    sta.b $9E           ; digest rows left
    ldy.w #!GBC_ROWS_OFF
GbcLsSeg:
    lda.b $9E
    beq GbcLsLast
    lda [!GBC_PTR],y
    and.w #$00FF
    clc
    adc.w #40
    bra GbcLsGo
GbcLsLast:
    lda.w #224
GbcLsGo:
    sta.b $A0
    sep #$20
    lda.b $8C
    and.b #$18
    cmp.b $AC
    rep #$20
    beq GbcLsNext       ; this segment is the snapshot's: it costs nothing
    lda.b $A0
    cmp.w #184
    bcc GbcLsClamp
    lda.w #184          ; the bottom letterbox is not visible
GbcLsClamp:
    sec
    sbc.b $98
    bcc GbcLsNext       ; the segment starts past the picture
    beq GbcLsNext
    clc
    adc.l !GBC_RC_LROWS
    sta.l !GBC_RC_LROWS
    sep #$20
    lda.b $A9
    ora.b #$40          ; ... and that is what arms the group
    sta.b $A9
    rep #$20
GbcLsNext:
    lda.b $9E
    beq GbcLsDone
    dec.b $9E
    sep #$20
    iny
    iny
    iny
    iny
    iny
    lda [!GBC_PTR],y    ; this digest row's LCDC
    sta.b $8C
    iny                 ; the next row
    rep #$20
    lda.b $A0
    sta.b $98
    bra GbcLsSeg
GbcLsDone:
    sep #$20
    rts

; ---------------------------------------------------------------------------
; PASS 2 -- window 1, the two scroll pairs and the LCDC group, in ONE walk
; over the digest.
;
; ⚡ P5B: these were three passes (four walks: W1, the BG pair, the window
; pair, the LCDC group), each re-reading every digest row and each paying a
; multiply to find it.  They visit the SAME segments -- digest row r starts one
; at framebuffer row 40 + ly_r and it runs to the next -- so one walk feeds all
; four run-length encoders (slots 0..3), and every table gets exactly the calls
; it got before, in the same order: the only difference between the walks was
; where their first and last segments start and end, and that is kept (W1
; starts at row 40 after an empty hold of 40 and ends at 184 with a hold of 40;
; the others run 0..224).  A clamp (below) is counted once per table built, as
; the four walks each counted it.
;
; WINDOW 1 (ch4, $2126/$2127, two bytes a line).  Per row: (255, 0) when the
; window is not shown -- with W1-OUT selected for BG1/BG3 the whole line reads
; as "outside" -- and (48 + max(WX-7, 0), 207) when it is.  Shown iff, with
; THAT row's registers, LCDC.5 and WX < 167 and ly >= WY and the screen is not
; the LCD-off/first-frame white one (sec. 11.3/12.12: white first, or W1-IN
; punches a black hole in BG4).  WX 0..6 clamps at 7; the real glitch is
; deviation sec. 12.8.  This is the only thing per-line LCDC.5 can do.
;
; THE SCROLL PAIRS (mode 3, four bytes a line).  The group is {HOFS lo, HOFS
; hi, VOFS lo, VOFS hi} and mode 3 spreads it over $21xx, $21xx, $21xx+1,
; $21xx+1.  Writing a BGnHOFS twice, low byte then high, lands exactly
; (hi<<8)|lo -- but only because the four bytes of ONE channel go out back to
; back: the BGOFS latch is shared by all eight registers, so two pairs may never
; interleave and nothing else may write $210D..$2114 while these are armed (see
; GbcRegsScroll).  ch0/ch1 (BG2/BG4, the Game Boy's background) and ch2/ch3
; (BG1/BG3, its window) carry IDENTICAL bytes, so each pair is compiled ONCE
; and both channels are pointed at that one table.
;
; THE LCDC GROUP (TABLE SPEC S12), MODE 4, four bytes a line -- see GbcGrpLcdc
; for the bytes.  The channel is whatever GbcP1Spares gave it ($86), so the
; table goes into that channel's own slot.
;
; ⚠ sec. 8 promises ly_eff non-decreasing, and the walk is built on it.  A log
; that breaks it would make a segment end land BEFORE the row the walk is on,
; the subtract would borrow, and an encoder would be handed a 255-line segment
; -- 144 times over, which is a table longer than the slot it lives in and,
; from the last slot, longer than the SET.  The bridge is the only producer and
; is the coupled pair, so this is a promise and not a risk; what the clamp buys
; is that a broken promise stays inside the slot instead of writing over the
; tables the HDMA is reading.  It is counted in row_clamped.
; ---------------------------------------------------------------------------
!PR_WALKS = $A2         ; tables this walk builds (row_clamped counts per table)
!PR_DCUR  = $A4         ; digest read cursor
!PR_LEFT  = $A6         ; digest rows still to step past
!PR_SS    = $9E         ; where the 0..224 tables' segment starts ($98 is W1's)
!PR_LEN   = $EA         ; ... and how long it is
!PR_WLY   = $F8         ; ⚡ S14: window lines DRAWN so far this frame (WLY)
!PR_WSK   = $FA         ; ... and the window pair's VOFS correction (16-bit)
!PR_WFR   = $FC         ; ... scratch: the first drawn row of a segment
GbcPassRegs:
    jsr GbcLoadSnapRegs
    rep #$20
    lda.w #$0001
    sta.b !PR_WALKS
    stz.b !PR_WLY
    stz.b !PR_WSK
    ; slot 0: window 1, which is always built
    lda.b $96
    clc
    adc.w #!GBC_T_W1
    tay
    sep #$20
    ldx.w #$0000
    lda.b #$02
    jsr GbcEncInit
    jsr GbcW1Empty
    lda.b #40
    jsr GbcEncAdd       ; rows 0..39: above the picture there is no window
    ; slot 1: the BG pair, when the log carries SCX or SCY
    lda.b $A9
    lsr a
    bcc GbcPrNoBg
    rep #$20
    lda.b $96
    clc
    adc.w #!GBC_T_CH0
    tay
    inc.b !PR_WALKS
    sep #$20
    ldx.w #12
    lda.b #$04
    jsr GbcEncInit
GbcPrNoBg:
    ; slot 2: the window pair, when it carries WX or WY
    lda.b $A9
    and.b #$02
    beq GbcPrNoWin
    rep #$20
    lda.b $96
    clc
    adc.w #!GBC_T_CH2
    tay
    inc.b !PR_WALKS
    sep #$20
    ldx.w #24
    lda.b #$04
    jsr GbcEncInit
GbcPrNoWin:
    ; slot 3: the LCDC group, on the channel GbcP1Spares gave it
    lda.b $86
    cmp.b #$FF
    beq GbcPrNoLcdc
    lda.l !GBC_SNAP+!GBC_SNAP_REGS+4
    and.b #$40
    beq GbcPrWin0
    lda.b #$4C
    bra GbcPrWinSt
GbcPrWin0:
    lda.b #$48
GbcPrWinSt:
    sta.b $87           ; the constant $2109 byte of every group this frame
    lda.b $86
    jsr GbcChSlotY      ; Y = the table of the channel the group got
    rep #$20
    inc.b !PR_WALKS
    sep #$20
    ldx.w #36
    lda.b #$04
    jsr GbcEncInit
GbcPrNoLcdc:
    rep #$20
    lda.w #40
    sta.b $98           ; W1's first segment starts at row 40 ...
    stz.b !PR_SS        ; ... the others' at row 0
    lda.b $94
    sta.b !PR_LEFT
    lda.w #!GBC_ROWS_OFF
    sta.b !PR_DCUR
GbcPrSeg:               ; A 16-bit
    lda.b !PR_LEFT
    beq GbcPrLast
    ldy.b !PR_DCUR
    lda [!GBC_PTR],y
    and.w #$00FF
    clc
    adc.w #40           ; the row this digest row starts on
    cmp.b $98
    bcs GbcPrSegOk
    lda.l !GBC_RC_ROWFIX ; the clamp -- see above
    clc
    adc.b !PR_WALKS
    sta.l !GBC_RC_ROWFIX
    lda.b $98
GbcPrSegOk:
    sta.b $A0
    sep #$20
    ldx.w #$0000        ; W1Segment adds to the encoder in X
    jsr GbcW1Segment
    jsr GbcPrFour
    ; step past the digest row: its registers hold from here on
    rep #$20
    dec.b !PR_LEFT
    ldy.b !PR_DCUR
    iny
    lda [!GBC_PTR],y
    sta.b $88           ; SCX, SCY
    iny
    iny
    lda [!GBC_PTR],y
    sta.b $8A           ; WX, WY
    iny
    iny
    sep #$20
    lda [!GBC_PTR],y
    sta.b $8C           ; LCDC
    rep #$20
    iny
    sty.b !PR_DCUR
    lda.b $A0
    sta.b $98
    sta.b !PR_SS
    bra GbcPrSeg
GbcPrLast:
    lda.w #184
    sta.b $A0
    sep #$20
    ldx.w #$0000
    jsr GbcW1Segment    ; W1 ends with the picture ...
    lda.b #40
    jsr GbcEncHold      ; ... rows 184..223 hold what row 183 left
    jsr GbcEncEnd
    rep #$20
    lda.w #224
    sta.b $A0
    sep #$20
    jsr GbcPrFour       ; the others run to the bottom
    lda.b $A9
    lsr a
    bcc GbcPrEndBg
    ldx.w #12
    jsr GbcEncEnd
GbcPrEndBg:
    lda.b $A9
    and.b #$02
    beq GbcPrEndWin
    ldx.w #24
    jsr GbcEncEnd
GbcPrEndWin:
    lda.b $86
    cmp.b #$FF
    beq GbcPrEndLcdc
    ldx.w #36
    jsr GbcEncEnd
GbcPrEndLcdc:
    rts

; Rows [!PR_SS, $A0) into the three four-byte tables that are armed, with the
; registers as they now stand.  A 8-bit.
GbcPrFour:
    rep #$20
    lda.b $A0
    sec
    sbc.b !PR_SS
    sta.b !PR_LEN
    sep #$20
    lda.b $A9
    lsr a
    bcc GbcPfNoBg
    jsr GbcGrpBg
    ldx.w #12
    lda.b !PR_LEN
    jsr GbcEncAdd
GbcPfNoBg:
    lda.b $A9
    and.b #$02
    beq GbcPfNoWin
    jsr GbcGrpWin
    ldx.w #24
    lda.b !PR_LEN
    jsr GbcEncAdd
GbcPfNoWin:
    lda.b $86
    cmp.b #$FF
    beq GbcPfNoLcdc
    jsr GbcGrpLcdc
    ldx.w #36
    lda.b !PR_LEN
    jsr GbcEncAdd
GbcPfNoLcdc:
    rts

; Emit rows [$98, $A0) with the registers as they now stand.
GbcW1Segment:
    rep #$20
    lda.b $A0
    sec
    sbc.b $98
    beq GbcW1SegNone
    sep #$20
    lda.b $91
    bne GbcW1SegEmpty   ; the white screen forces the window empty
    lda.b $8C
    and.b #$20
    beq GbcW1SegEmpty   ; LCDC.5 = 0
    lda.b $8B
    cmp.b #144
    bcs GbcW1SegEmpty   ; WY > 143
    lda.b $8A
    cmp.b #167
    bcs GbcW1SegEmpty   ; WX >= 167
    rep #$20
    lda.b $8B
    and.w #$00FF
    clc
    adc.w #40           ; the row ly = WY lands on
    sta.b $E8
    cmp.b $98
    bcc GbcW1SegShown
    beq GbcW1SegShown
    cmp.b $A0
    bcs GbcW1SegEmptyR
    jsr GbcW1Wly        ; ⚡ S14, from the row ly = WY lands on
    sep #$20
    jsr GbcW1Empty
    rep #$20
    lda.b $E8
    sec
    sbc.b $98
    sep #$20
    jsr GbcEncAdd       ; ... the empty part
    jsr GbcW1Shown
    rep #$20
    lda.b $A0
    sec
    sbc.b $E8
    sep #$20
    jsr GbcEncAdd       ; ... and the shown part
    rts
GbcW1SegShown:
    lda.b $98
    jsr GbcW1Wly        ; ⚡ S14, from the segment's first row
    sep #$20
    jsr GbcW1Shown
    bra GbcW1SegLen
GbcW1SegEmptyR:
    lda.w #$0000        ; ⚡ S14: the whole segment is above WY, so it carries
    sec                 ; what the row ly = WY will: D = -WLY
    sbc.b !PR_WLY
    sta.b !PR_WSK
    sep #$20
GbcW1SegEmpty:
    jsr GbcW1Empty
GbcW1SegLen:
    rep #$20
    lda.b $A0
    sec
    sbc.b $98
    sep #$20
    jsr GbcEncAdd
    rts
GbcW1SegNone:
    sep #$20
    rts

; ⚡ TABLE SPEC S14 -- the window line counter.  A(16) = the first row of this
; segment the window is DRAWN on ($E8 = the row ly = WY lands on); the drawn
; rows run to $A0.  The window's VOFS for the segment is -(WY + 41 + D) with
; D = (first - $E8) - WLY, WLY being the lines drawn before it -- constant over
; the segment because WLY grows with the row -- and WLY then counts the
; segment's drawn rows.  A segment where the window is not drawn (white screen,
; LCDC.5 = 0, WX >= 167, WY > 143) leaves D alone.  Leaves A 16-bit.
GbcW1Wly:
    sta.b !PR_WFR
    sec
    sbc.b $E8
    sec
    sbc.b !PR_WLY
    sta.b !PR_WSK
    lda.b $A0
    sec
    sbc.b !PR_WFR
    clc
    adc.b !PR_WLY
    sta.b !PR_WLY
    rts

GbcW1Empty:
    lda.b #255
    sta.b $9A
    lda.b #0
    sta.b $9B
    rts

GbcW1Shown:
    lda.b $8A
    cmp.b #7
    bcs GbcW1ShWxOk
    lda.b #7            ; WX 0..6: the contract clamps, the real glitch is a
GbcW1ShWxOk:            ; documented deviation (sec. 12.8)
    sec
    sbc.b #7
    clc
    adc.b #48
    sta.b $9A           ; left edge
    lda.b #207
    sta.b $9B           ; right edge = the viewport's
    rts

; sec. 11.3: the Game Boy's picture sits at (48, 40) in a 0-based framebuffer.
GbcGrpBg:
    rep #$20
    lda.b $88
    and.w #$00FF
    sec
    sbc.w #48
    and.w #$03FF
    sta.b $9A           ; SCX - 48, ten bits
    lda.b $89
    and.w #$00FF
    sec
    sbc.w #41
    and.w #$03FF
    sta.b $9C           ; SCY - 41
    sep #$20
    rts

GbcGrpWin:
    rep #$20
    lda.b $8A
    and.w #$00FF
    clc
    adc.w #41
    eor.w #$FFFF
    inc a
    and.w #$03FF
    sta.b $9A           ; -(WX + 41)
    lda.b $8B
    and.w #$00FF
    clc
    adc.w #41
    clc
    adc.b !PR_WSK       ; ⚡ S14: + the window line counter's correction
    eor.w #$FFFF
    inc a
    and.w #$03FF
    sta.b $9C           ; -(WY + 41 + D)
    sep #$20
    rts

; ---------------------------------------------------------------------------
; ⚡ THE LCDC GROUP (TABLE SPEC S12), MODE 4, four bytes a line.
;
; Mode 4 writes FOUR ADJACENT registers from four bytes, which is the whole
; reason one channel can carry both halves of this phase:
;   byte 0 -> $2108  BG2 map base = LCDC.3 ? $44 : $40   the background's map
;   byte 1 -> $2109  BG3 map base = LCDC.6 ? $4C : $48   the window's carpet
;   byte 2 -> $210A  BG4 map base = LCDC.3 ? $4C : $48   the background's
;   byte 3 -> $210B  BG1+BG2 chr  = LCDC.4 ? $22 : $33   which chr base a tile
;                                                        index means
; with the LCDC that is EFFECTIVE on that line -- the snapshot's, walked
; forward by the log, exactly like SCX in pass 3.
;
; ⚠ $2109 IS NOT FOLLOWED.  It carries the SNAPSHOT's LCDC.6 on every line,
; letterbox included; it is in the group only because mode 4 needs four
; consecutive registers and $2108 and $210B are four apart.  Writing it costs
; nothing (the value never changes) and changes nothing (it is what the frame
; init already put there).  $2107, the window's CONTENT map, is outside the
; group -- carrying it would need a fifth register -- so a frame that moves
; LCDC.6 mid-picture draws the window's content AND carpet from the snapshot's
; map while the background follows b3.  That is contract sec. 12.5 and
; lcdc_unsupported counts it.
;
; Built by GbcPassRegs, encoder slot 3.
; ---------------------------------------------------------------------------
; The four bytes, out of the effective LCDC in $8C and the frame constant $87.
GbcGrpLcdc:
    lda.b $8C
    and.b #$08
    beq GbcGlBg0
    lda.b #$44
    sta.b $9A
    lda.b #$4C
    sta.b $9C
    bra GbcGlChr
GbcGlBg0:
    lda.b #$40
    sta.b $9A
    lda.b #$48
    sta.b $9C
GbcGlChr:
    lda.b $87
    sta.b $9B
    lda.b $8C
    and.b #$10
    beq GbcGlB0
    lda.b #$22          ; $210B per line: base A, VRAM $4000
    bra GbcGlSt
GbcGlB0:
    lda.b #$33          ; $210B per line: base B, VRAM $6000
GbcGlSt:
    sta.b $9D
    rts

; A(8) = channel -> Y = the table that channel owns inside the set being
; written.  Enter with A 8-bit, leave with A 8-bit; X is preserved.
GbcChSlotY:
    rep #$20
    and.w #$00FF
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a
    asl a               ; ch * 128
    sta.b $AE
    asl a
    asl a               ; ch * 512
    clc
    adc.b $AE           ; ch * 640 = ch * !GBC_T_SLOT
    clc
    adc.w #!GBC_T_CH0
    clc
    adc.b $96
    tay
    sep #$20
    rts

; ---------------------------------------------------------------------------
; PASS 4 -- colour, on whatever channels the scroll left over.
;
; Every palette write the picture can show becomes a queue of (CGRAM index,
; 15-bit colour) pairs.  The queue is FIFO and strictly streaming: on each row
; 40..183 the pairs that became ready there are appended, then the armed colour
; channels take ONE each, in ascending channel number.  A pair that has to wait
; counts colour_delayed; one still queued after row 183 is never shown.  No
; coalescing, no reordering, no priority -- the tables are a byte cursor over
; that queue and nothing else.
;
; What one write is worth, in CGRAM entries (contract sec. 11.4):
;   BCPD, CGB      2   k != 0 -> (4p+k) and (32+4p+k), the BG1 and BG2 regions
;                      k == 0 -> (64+4p+1) and (96+4p+1), the two carpets
;   OCPD, CGB      1   (128+16q+k); k == 0 costs NOTHING, sec. 4.4 pins that
;                      entry at $0000
;   BGP, compat    8   paired BG1/BG2 per colour so that four channels apply
;                      two COMPLETE colours a line rather than four halves
;   OBP0/1, compat 3   (128+16q+k) for k = 1..3
;   BGP/OBP in CGB, BCPD/OCPD in compat: nothing -- they move no view byte.
; A channel holding the neutral group {0,0,0,0} writes CGRAM[0] = $0000, which
; sec. 4.4 and invariant 7 pin there forever: provably a no-op.
;
; ⚡ P5B -- THE QUEUE IS NOT STORED ANY MORE.  It used to be: pushed per pair,
; popped per row and per spare, with the run-length encoder asked about every
; spare on every row -- 390 to 625 instructions per log entry, 14 to 20 frames
; for a 512-entry log.  Because the queue is FIFO and every row takes the same
; number of pairs, WHERE a pair lands is fully decided by the pairs before it:
;
;     pair n goes to the first free slot (row, spare), rows in order and spares
;     ascending inside a row, whose row is not below the row n was ingested on
;
; so the walk keeps just that slot cursor -- !CP_R and the spare in X -- and
; writes each pair into its table the moment the log produces it.  The
; "ingested on" row is the one the old walk's single forward cursor gave it:
; the row of the first entry, of ANY kind, that has not been passed yet, i.e.
; the running maximum of the lines seen so far.  That is why a register entry
; on a later line moves the cursor too (GbcCpJump from GbcCpOther): the old
; walk could not ingest anything behind it until its row came round.  With a
; chronological log (sec. 8) the running maximum IS the entry's own line.
; IDENTITY WITH THE QUEUE, the invariant everything here rests on: the i-th pair
; placed on (row r, spare s) is the one the FIFO would have popped there,
; because (1) pairs are produced in log order, as they were pushed; (2) a row
; takes spares 0..S-1 in order before the next row starts, as the pops did;
; (3) the cursor never moves back and never skips a free slot on a row at or
; after the ingestion row, which is exactly "the queue is not empty"; (4) the
; slots of a row the cursor jumps OVER are the ones on which the old queue was
; empty and every spare held.  colour_delayed is "placed on another row than
; its own line", counted per stretch rather than per pair (!CP_FO); requested
; and noop come from pass 1; dropped = requested - placed, as before.
;
; ENCODING -- the same bytes as the old run-length encoder, built in place.
; Every spare has a table cursor and the group it last received, which sits in
; the table already, in one of two shapes:
;   BATCH  the last element of an open `$80|k` batch of singletons, whose
;          header byte is only written when the batch closes;
;   RUN    the data of a hold entry whose count byte, just before it, is
;          written when the run ends (split at 127 on the way out).
; A new group on the NEXT row of a spare in BATCH is appended to the batch --
; the fast path, four stores into the table and nothing else.  Anything else
; (a hold between two placements, the same group twice, a batch of 127, the
; first placement) goes through GbcCpSlow, which moves the last group from the
; batch into a hold when it turns out not to be a singleton after all.  The
; partition into runs and singletons is the one TABLE SPEC S4 describes, run
; by run, so the bytes are the golden's.
;
; THE FAST PATH INDEXES WITH THE ROW.  Inside one batch the group of row r sits
; at base + 4r, so with Y = 4 * !CP_R -- one value for every spare on the row --
; each spare only needs three long pointers into its own table (the previous
; group's colour, the new group's index and colour) and the row it expects
; next.  A spare whose expected row does not match, because of a gap, a run, a
; full batch or the first placement, has it tagged with bit 15 and falls into
; the slow path.
; ---------------------------------------------------------------------------

; Colour-walk direct page.  Everything from $87 to $DF that the earlier passes
; used is free by now, and so is $F4-$FF (pass 1 and pass 2's S14 counter
; borrow parts of it, and neither reads it back after its pass); $86,
; $90, $91, $96-$97, $A8, $A9 and $E0-$E7 are still read by GbcSetHeader and
; stay untouched, and $F0-$F2 is the frame body's too (read-only here).
!CP_R     = $88   ; LINE of the next free slot, 0..144 (row = 40 + line); every
                  ; row the colour walk keeps is a Game Boy line, so the log's
                  ; ly_eff compares with it as it is
!CP_R4    = $8A   ; 4 * !CP_R: the Y every placement indexes the tables with
!CP_W0    = $8C   ; {idx, idx} of the pair being placed
!CP_W1    = $8E   ; its colour, bit 15 clear
!CP_XS    = $92   ; the cursor's X while the general path needs X
!CP_LOGY  = $94   ; log cursor
!CP_FO    = $98   ; X at which this row's on-time stretch began; bit 15 = none
!CP_ONT2  = $9A   ; pairs placed on their own line, x2 (closed stretches)
!CP_SKIP2 = $9C   ; slots the cursor jumped over, x2: placed = slots - skipped
!CP_TRAP  = $9E   ; first row on which an open batch would take its 128th group
!CP_TBASE = $A0   ; X of spare 0 in GbcCpTab for this frame's spare count
!CP_ET    = $A2   ; the entry's {idx, value}
!CP_SEND  = $A4   ; 11 * spares: the end of the spare blocks
!CP_XEND  = $F4   ; !CP_TBASE + 2 * spares: X on a full row (the wrap entry)
!CP_NEXT  = $A6   ; the walk loop the general path returns to (CGB / compat)
!CS_ROW   = $AA   ; slow path: the row the spare expected
!CS_GAP   = $AC   ; ... the lines between that row and this one
!CS_G0    = $AE   ; ... the group it holds, as it sits in the table
!CS_G1    = $EE
!CS_T     = $DC   ; scratch
!CP_V     = $DE   ; the entry's row, then the general path's loop index
!CP_P2    = $E8   ; long pointer $7E:0002: the second word of a table group or
                  ; of a log entry
!CP_PCR   = $EB   ; long pointer to the CGRAM copy, $7E:!GBC_CRAM_OFF
!CP_C     = $F6   ; 4 words: the four colours of a compat BGP/OBP write
; Per spare k, 11 bytes at $B0 + 11k, so the slow path reaches them as dp,x:
!CP_NX    = $B0   ; +0  the row the fast path expects next; bit 15 = go slow
!CP_PP    = $B2   ; +2  long pointer: base - 2 (the colour of the row above)
!CP_PC    = $B5   ; +5  long pointer: base + 2 (the colour of this row)
!CP_PI    = $B8   ; +8  long pointer: base     ({idx, idx} of this row)
; ... and 11 bytes of WRAM a spare, same stride, that only the slow path reads.
!CS_MODE  = !GBC_WRAM_LONG+!GBC_CP_OFF+0  ; 0 = BATCH, 1 = RUN
!CS_LP    = !GBC_WRAM_LONG+!GBC_CP_OFF+2  ; where the last group sits
!CS_HDR   = !GBC_WRAM_LONG+!GBC_CP_OFF+4  ; the open batch's header byte; 0 =
                                          ; never placed on (stays DISARMED)
!CS_R0    = !GBC_WRAM_LONG+!GBC_CP_OFF+6  ; the open batch's first row
!CS_RUN   = !GBC_WRAM_LONG+!GBC_CP_OFF+8  ; lines of the run (RUN)

GbcPassColour:
    ; ⚡ The four "spare received an entry" flags are THIS compile's, and
    ; GbcSetHeader arms a colour channel for every one still set.  They are
    ; cleared here, before either early return, because only GbcPcInit below
    ; used to clear them: a log without a visible palette write skipped it,
    ; kept the flags of the last colour compile, and re-armed that set's stale
    ; colour tables on every later frame (a hi-colour title repainting every
    ; screen after it with its own palettes).
    rep #$20
    stz.b $E4
    stz.b $E6
    sep #$20
    lda.b $A9
    and.b #$10
    bne GbcPcAny
    rts                 ; no palette write on a visible line: nothing to count
GbcPcAny:               ; or place, and every counter stays at the 0 it was
    rep #$30            ; cleared to
    lda.b $A8
    and.w #$00FF
    bne GbcPcPlace
    ; No spare at all: every request is dropped.  Pass 1 counted them over the
    ; whole log, which is what the old walk replayed here -- past a ly >= 144.
    lda.l !GBC_CP_REQA
    sta.l !GBC_RC_REQ
    sta.l !GBC_RC_DROP
    lda.l !GBC_CP_NOOPA
    sta.l !GBC_RC_NOOP
    sep #$20
    rts
GbcPcPlace:             ; A = S, the spare count (1..4)
    sta.b !CS_T
    asl a
    tax
    lda.l GbcCpTBase,x
    sta.b !CP_TBASE
    lda.b !CS_T
    asl a
    sta.b !CP_SEND
    asl a
    asl a
    clc
    adc.b !CP_SEND      ; 10 S
    adc.b !CS_T         ; 11 S
    sta.b !CP_SEND
    lda.l !GBC_CP_REQP
    sta.l !GBC_RC_REQ
    lda.l !GBC_CP_NOOPP
    sta.l !GBC_RC_NOOP
    lda.w #$0002        ; the pointers the walk reads through
    sta.b !CP_P2
    lda.w #!GBC_CRAM_OFF
    sta.b !CP_PCR
    sep #$20
    lda.b #!GBC_WRAM_A1B
    sta.b !CP_P2+2
    sta.b !CP_PCR+2
    rep #$20
    ; Every spare starts virgin: a RUN of 40 lines of the neutral group (rows
    ; 0..39), whose data is written at slot + 1 now and whose count byte, at
    ; slot + 0, is written when the run ends.  A spare that is never placed on
    ; is left DISARMED by GbcSetHeader, so those bytes are never read.
    stz.b !CS_T         ; k
    ldx.w #$0000        ; 11 k
GbcPcInit:
    cpx.b !CP_SEND
    bcs GbcPcInitDone
    phx
    ldx.b !CS_T
    sep #$20
    stz.b $E4,x         ; not armed until something lands on it
    lda.b $E0,x         ; the channel spare k was given
    jsr GbcChSlotY      ; Y = its table
    rep #$20
    plx
    tya
    inc a
    sta.l !CS_LP,x      ; the group sits at slot + 1 ...
    tay
    lda.w #$0000
    sta [!GBC_PTR],y
    sta [!CP_P2],y      ; ... and it is {0, 0, 0, 0}
    sta.l !CS_HDR,x     ; never placed on
    inc a
    sta.l !CS_MODE,x    ; RUN ...
    lda.w #40
    sta.l !CS_RUN,x     ; ... of 40 lines,
    lda.w #$8000
    sta.b !CP_NX,x      ; expecting line 0 (row 40), through the slow path
    sep #$20
    lda.b #!GBC_WRAM_A1B
    sta.b !CP_PP+2,x
    sta.b !CP_PC+2,x
    sta.b !CP_PI+2,x
    rep #$20
    inc.b !CS_T
    txa
    clc
    adc.w #11
    tax
    bra GbcPcInit
GbcPcInitDone:
    stz.b !CP_R         ; line 0 = row 40
    stz.b !CP_R4
    lda.w #$FFFF
    sta.b !CP_FO        ; no on-time stretch open
    lda.w #$7FFF
    sta.b !CP_TRAP
    stz.b !CP_ONT2
    stz.b !CP_SKIP2
    lda.b !CP_SEND      ; 11 S ...
    ldx.w #$0000
GbcPcXend:              ; ... -> 2 S, without a divide
    inx
    inx
    sec
    sbc.w #11
    bne GbcPcXend
    txa
    clc
    adc.b !CP_TBASE
    sta.b !CP_XEND
    lda.w #!GBC_LOG_OFF
    sta.b !CP_LOGY
    ldx.b !CP_TBASE     ; the cursor: row 40, spare 0
    lda.b $90
    and.w #$00FF
    beq GbcPcCgb
    lda.w #GbcCpLoopCompat
    sta.b !CP_NEXT
    jmp GbcCpLoopCompat
GbcPcCgb:
    lda.w #GbcCpLoopCgb
    sta.b !CP_NEXT
    ; fall through

; ---------------------------------------------------------------------------
; The walk.  A, X, Y 16-bit; X = the slot cursor's spare, as an index into
; GbcCpTab (!CP_TBASE + 2 * spare); Y = the log cursor between entries and
; !CP_R4 during a placement.  Leaves through GbcCpEnd -- at the sentinel or at
; the first entry with ly_eff >= 144 -- or GbcCpEndSat, when the cursor runs
; past row 183 and nothing more can be placed.
;
; THE FAST PATH is a BCPD in CGB mode, which is the whole of a hi-colour log;
; the EOR test is the one pass 1 uses.  Everything else -- another kind, a line
; past the picture, the sentinel -- goes through GbcCpOther.
; ---------------------------------------------------------------------------
GbcCpLoopCgb:
    ldy.b !CP_LOGY
    lda [!GBC_PTR],y    ; ly_eff | kind << 8
    eor.w #$0800
    cmp.w #144
    bcs GbcCpNotBcpd
    cmp.b !CP_R         ; A = its line
    bne GbcCpBNotR
    lda.b !CP_FO        ; on its own line: this row's on-time stretch starts
    bpl GbcCpBody       ; at this slot, unless it already has
    stx.b !CP_FO
GbcCpBody:
    tya
    clc
    adc.w #$0004
    sta.b !CP_LOGY
    lda [!CP_P2],y      ; idx | value << 8
    sta.b !CP_ET
    ; cram_bg[idx & $3F] = value, then the whole colour is read back (S10);
    ; entry 0 of the copy was patched to BG[0][0] by GbcCramFetch
    and.w #$003F
    tay
    sep #$20
    lda.b !CP_ET+1
    sta [!CP_PCR],y
    rep #$20
    tya
    and.w #$003E
    tay
    lda [!CP_PCR],y
    and.w #$7FFF
    sta.b !CP_W1
    lda.w GbcCpBcpdIdx,y ; {i, i}: the BG1 region, or the first carpet; the
    sta.b !CP_W0        ; second pair is 32 entries on (+$2020)
    ldy.b !CP_R4
    jmp (GbcCpPairTab,x) ; both pairs, inline, for this spare count and spare
GbcCpBNotR:
    bcs GbcCpBJump      ; a row the cursor has not reached: jump to it
    ; A row the cursor is already past: every pair of this entry is late, and
    ; this row's on-time stretch -- only a log out of sec. 8 order has one open
    ; here -- ends before it.
    lda.b !CP_FO
    bmi GbcCpBody
    jsr GbcCpFoClose
    bra GbcCpBody
GbcCpBJump:
    jsr GbcCpJump
    stx.b !CP_FO        ; its own row, from spare 0
    bra GbcCpBody

; --- OCPD in CGB: the same shape, one pair, and none at all for k = 0 ----------
; cram_ob[i] with i = 8q + r lives at view byte 256 + 32q + r (the OBJ half is
; NOT contiguous in the view); the pair is entry 128 + 16q + k.  A k = 0 write
; is replayed but places nothing (sec. 4.4 pins that entry at $0000) -- and
; still moves the cursor like any entry, which the jump above has done.
GbcCpNotBcpd:
    eor.w #$0100        ; raw ^ $0900: kind 9 leaves the line
    cmp.w #144
    bcs GbcCpOther
    cmp.b !CP_R
    beq GbcCpOcR
    bcc GbcCpOcLate
    jsr GbcCpJump
    stx.b !CP_FO        ; its own row, from spare 0 (harmless if k = 0: an
    bra GbcCpOcBody     ; empty stretch counts nothing)
GbcCpOcLate:
    lda.b !CP_FO
    bmi GbcCpOcBody
    jsr GbcCpFoClose
    bra GbcCpOcBody
GbcCpOcR:
    lda.b !CP_FO
    bpl GbcCpOcBody
    stx.b !CP_FO
GbcCpOcBody:
    tya
    clc
    adc.w #$0004
    sta.b !CP_LOGY
    lda [!CP_P2],y      ; idx | value << 8
    sta.b !CP_ET
    and.w #$003F
    asl a
    tay
    lda.w GbcCpObByte,y ; 256 + 32q + r
    tay
    sep #$20
    lda.b !CP_ET+1
    sta [!CP_PCR],y
    rep #$20
    lda.b !CP_ET
    and.w #$0006
    beq GbcCpOcNone     ; k = 0: nothing to place (pass 1 counted the noop)
    tya
    and.w #$FFFE
    tay
    lda [!CP_PCR],y
    and.w #$7FFF
    sta.b !CP_W1
    lda.b !CP_ET
    and.w #$003E
    tay
    lda.w GbcCpOcpdIdx,y ; {128 + 16q + k} twice
    sta.b !CP_W0
    ldy.b !CP_R4
    jsr (GbcCpTab,x)
GbcCpOcNone:
    jmp GbcCpLoopCgb

; Every entry the fast tests above do not take, and every entry in compat.
GbcCpLoopCompat:
    ldy.b !CP_LOGY
GbcCpOther:
    lda [!GBC_PTR],y
    and.w #$00FF
    cmp.w #144
    bcc GbcCpOtherGo
    jmp GbcCpEnd        ; ly_eff >= 144, or the sentinel.  Either way the old
                        ; walk's forward cursor stopped here for good.
GbcCpOtherGo:
    sta.b !CP_V         ; its line
    cmp.b !CP_R
    beq GbcCpOtherKind
    bcc GbcCpOtherKind
    jsr GbcCpJump       ; ANY kind moves the cursor to its row (see above)
GbcCpOtherKind:
    lda [!GBC_PTR],y
    xba
    and.w #$00FF        ; the kind
    sec
    sbc.w #$0005
    cmp.w #$0005
    bcs GbcCpSkip       ; 0..4 wrap round to >= 5 too: no colour, and pass 1
    stx.b !CP_XS        ; counted the kinds past 9
    asl a
    tax
    lda [!CP_P2],y
    sta.b !CP_ET        ; idx | value << 8
    tya
    clc
    adc.w #$0004
    sta.b !CP_LOGY
    jmp (GbcCpKindTab,x)
GbcCpSkip:
    tya
    clc
    adc.w #$0004
    sta.b !CP_LOGY
    jmp (!CP_NEXT)

GbcCpKindTab:
    dw GbcCpBgp, GbcCpObp, GbcCpObp, GbcCpNoop, GbcCpOcpd

; BCPD only gets here in compat, where the hardware ignores it (sec. 8), and
; BGP/OBP/OCPD come back here when the mode or k = 0 makes them move nothing.
; colour_noop was counted by pass 1.
GbcCpNoop:
    ldx.b !CP_XS
    jmp (!CP_NEXT)

; A general-path entry that WILL place pairs: settle this row's on-time
; stretch against the entry's own row (!CP_V), as the fast path does, and
; point Y at the row.  X = the cursor.
GbcCpRowIn:
    lda.b !CP_V
    cmp.b !CP_R
    bne GbcCpRiLate
    lda.b !CP_FO
    bpl GbcCpRiY
    stx.b !CP_FO
    bra GbcCpRiY
GbcCpRiLate:
    lda.b !CP_FO
    bmi GbcCpRiY
    jsr GbcCpFoClose
GbcCpRiY:
    ldy.b !CP_R4
    rts

; --- OCPD, CGB.  cram_ob[i] with i = 8q + r lives at view byte 256 + 32q + r
; (the OBJ half is NOT contiguous in the view), and the pair is entry
; 128 + 16q + k.  k = 0 is replayed but costs nothing (sec. 4.4).
GbcCpOcpd:
    lda.b $90
    and.w #$00FF
    bne GbcCpNoop       ; compat
    lda.b !CP_ET
    and.w #$0038
    asl a
    asl a               ; 32q
    sta.b !CS_T
    lda.b !CP_ET
    and.w #$0007
    clc
    adc.b !CS_T
    adc.w #256
    tay
    sep #$20
    lda.b !CP_ET+1
    sta [!CP_PCR],y
    rep #$20
    lda.b !CP_ET
    and.w #$0006
    beq GbcCpNoop
    lsr a               ; k
    sta.b !CS_T
    tya
    and.w #$FFFE
    tay
    lda [!CP_PCR],y
    and.w #$7FFF
    sta.b !CP_W1
    lda.b !CP_ET
    and.w #$0038
    asl a               ; 16q
    adc.b !CS_T         ; + k (carry clear: 16q < $80)
    adc.w #128
    sta.b !CS_T
    xba
    ora.b !CS_T
    sta.b !CP_W0        ; {128 + 16q + k} twice
    ldx.b !CP_XS
    jsr GbcCpRowIn
    jsr (GbcCpTab,x)
    jmp (!CP_NEXT)

; --- BGP, compat.  Palette 0 is the only one a compat map can name; the four
; colours come from the RAW ones in the status block (S11).  Eight pairs,
; BG1/BG2 per colour: (1,c1) (33,c1) (2,c2) (34,c2) (3,c3) (35,c3) (65,c0)
; (97,c0).
GbcCpBgp:
    lda.b $90
    and.w #$00FF
    beq GbcCpNoop       ; CGB: kB(k) = k, BGP moves no view byte
    lda.w #!GBC_SNAP_RAW
    jsr GbcCpRawFour    ; !CP_C = BG[0][(BGP >> 2j) & 3], j = 0..3
    ldx.b !CP_XS
    jsr GbcCpRowIn
    stz.b !CP_V
GbcCpBgpLoop:
    ldy.b !CP_V
    lda.w GbcCpBgpTab,y
    sta.b !CP_W0
    lda.w GbcCpBgpTab+2,y
    tay
    lda.w $0000,y       ; one of !CP_C (DBR = 0, direct page = bank $00)
    sta.b !CP_W1
    ldy.b !CP_R4
    jsr (GbcCpTab,x)
    lda.b !CP_V
    clc
    adc.w #$0004
    sta.b !CP_V
    cmp.w #32
    bcc GbcCpBgpLoop
    jmp (!CP_NEXT)

GbcCpBgpTab:
    dw $0101, !CP_C+2, $2121, !CP_C+2, $0202, !CP_C+4, $2222, !CP_C+4
    dw $0303, !CP_C+6, $2323, !CP_C+6, $4141, !CP_C+0, $6161, !CP_C+0

; --- OBP0/OBP1, compat: k = 1..3 of one OBJ palette, (128+16q+k,
; OB[q][(OBPq >> 2k) & 3]).  X = 2 for OBP0, 4 for OBP1 (the kind table).
GbcCpObp:
    lda.b $90
    and.w #$00FF
    bne GbcCpObpGo
    jmp GbcCpNoop       ; CGB
GbcCpObpGo:
    stx.b !CS_ROW       ; 2 (q = 0) or 4 (q = 1); the slow path is not running
    txa
    asl a
    asl a               ; 8 (q = 0) or 16 (q = 1)
    adc.w #!GBC_SNAP_RAW
    jsr GbcCpRawFour    ; !CP_C = OB[q][(OBPq >> 2j) & 3]
    lda.b !CS_ROW
    asl a
    asl a
    asl a               ; 16 (q = 0) or 32 (q = 1) = 16q + 16
    adc.w #113          ; 128 + 16q + 1 (carry clear)
    sta.b !CP_W0
    xba
    ora.b !CP_W0
    sta.b !CP_W0        ; {128 + 16q + 1} twice
    ldx.b !CP_XS
    jsr GbcCpRowIn
    lda.w #!CP_C+2
    sta.b !CP_V
GbcCpObpLoop:
    ldy.b !CP_V
    lda.w $0000,y
    sta.b !CP_W1
    ldy.b !CP_R4
    jsr (GbcCpTab,x)
    lda.b !CP_W0
    clc
    adc.w #$0101        ; k + 1
    sta.b !CP_W0
    lda.b !CP_V
    inc a
    inc a
    sta.b !CP_V
    cmp.w #!CP_C+8
    bcc GbcCpObpLoop
    jmp (!CP_NEXT)

; A = the offset of a RAW palette inside the snapshot (+!GBC_SNAP_RAW); the
; entry's value picks four of its colours: !CP_C+2j = raw[(value >> 2j) & 3],
; bit 15 clear.  X is clobbered (the callers keep the cursor in !CP_XS).
GbcCpRawFour:
    sta.b !CS_T
    lda.b !CP_ET+1
    and.w #$00FF
    sta.b !CS_G0        ; the value, shifted two bits at a time below
    ldy.w #$0000
GbcCpRfLoop:
    lda.b !CS_G0
    and.w #$0003
    asl a
    adc.b !CS_T         ; carry clear
    tax
    lda.l !GBC_SNAP,x
    and.w #$7FFF
    sta.w !CP_C,y
    lsr.b !CS_G0
    lsr.b !CS_G0
    iny
    iny
    cpy.w #$0008
    bcc GbcCpRfLoop
    rts

; --- the cursor ---------------------------------------------------------------
; Close this row's on-time stretch: the slots from !CP_FO up to the cursor
; were filled by pairs placed on their own line.
GbcCpFoClose:
    txa
    sec
    sbc.b !CP_FO
    clc
    adc.b !CP_ONT2
    sta.b !CP_ONT2
    lda.w #$FFFF
    sta.b !CP_FO
    rts

; A = a row past !CP_R: the cursor jumps to spare 0 of it.  The row it leaves
; is closed -- its placed slots counted, its on-time stretch ended -- and the
; spares it did not reach hold, which is what the old walk did on the rows its
; queue was empty on.  Y is preserved.
GbcCpJump:
    sta.b !CS_T         ; the new line
    lda.b !CP_FO
    bmi GbcCpJmpFo
    jsr GbcCpFoClose
GbcCpJmpFo:
    lda.b !CS_T
    dec a
    cmp.b !CP_R
    bne GbcCpJmpFar     ; not the next line
    cpx.b !CP_XEND
    beq GbcCpJmpGo      ; the next line after a full row: nothing is skipped
GbcCpJmpFar:
    ; the slots it skips: the rest of this row, and every row in between
    stx.b !CS_GAP
    lda.b !CP_XEND
    sec
    sbc.b !CS_GAP
    clc
    adc.b !CP_SKIP2
    sta.b !CP_SKIP2
    lda.b !CS_T
    clc                 ; lines in between = new - old - 1
    sbc.b !CP_R
    beq GbcCpJmpGo
    tax
GbcCpJmpRow:
    lda.b !CP_XEND
    sec
    sbc.b !CP_TBASE     ; 2 S
    clc
    adc.b !CP_SKIP2
    sta.b !CP_SKIP2
    dex
    bne GbcCpJmpRow
GbcCpJmpGo:
    lda.b !CS_T
    sta.b !CP_R
    asl a
    asl a
    sta.b !CP_R4
    ldx.b !CP_TBASE
    lda.b !CP_R
    cmp.b !CP_TRAP
    bcc GbcCpJmpOut
    jsr GbcCpTrap
GbcCpJmpOut:
    rts

; The dispatch entry after the last spare: the row is full, the pair in
; !CP_W0/!CP_W1 goes to spare 0 of the next one.  (No slot was skipped, so
; nothing is counted: GbcCpEnd derives "placed" from where the cursor is.)
GbcCpWrap:
    lda.b !CP_FO
    bmi GbcCpWrFo
    jsr GbcCpFoClose
GbcCpWrFo:
    lda.b !CP_R4
    clc
    adc.w #$0004
    sta.b !CP_R4
    tay
    inc.b !CP_R
    lda.b !CP_R
    cmp.w #144
    bcs GbcCpSat
    cmp.b !CP_TRAP
    bcc GbcCpWrGo
    jsr GbcCpTrap
GbcCpWrGo:
    ldx.b !CP_TBASE
    jmp (GbcCpTab,x)
GbcCpSat:
    ; Past row 183: this pair and every one after it falls off the picture,
    ; which pass 1 has already counted as requested.  The walk is over, and
    ; the cursor stands at spare 0 of line 144 -- exactly as many slots as
    ; the picture has.
    pla                 ; the placement's return address
    ldx.b !CP_TBASE
    jmp GbcCpEndSat

; ⚡ A batch holds at most 127 groups (`$80|127`).  Inside one, the group of a
; row is appended by the fast path without any count, so the limit is watched
; per ROW instead: !CP_TRAP is the first row on which some open batch would
; take its 128th, and on reaching it every such spare is sent to the slow path,
; which closes the batch and opens the next one.  X and Y are preserved.
GbcCpTrap:
    phx
    phy
    lda.w #$7FFF
    sta.b !CP_TRAP
    ldx.w #$0000
GbcCpTrLoop:
    cpx.b !CP_SEND
    bcs GbcCpTrDone
    lda.l !CS_MODE,x
    bne GbcCpTrNext     ; only an open batch
    lda.l !CS_HDR,x
    beq GbcCpTrNext
    lda.l !CS_R0,x
    clc
    adc.w #127
    cmp.b !CP_R
    beq GbcCpTrFull
    bcc GbcCpTrFull
    cmp.b !CP_TRAP
    bcs GbcCpTrNext
    sta.b !CP_TRAP
    bra GbcCpTrNext
GbcCpTrFull:
    lda.b !CP_NX,x
    ora.w #$8000
    sta.b !CP_NX,x
GbcCpTrNext:
    txa
    clc
    adc.w #11
    tax
    bra GbcCpTrLoop
GbcCpTrDone:
    ply
    plx
    rts

; --- placement ----------------------------------------------------------------
; One entry of GbcCpTab per spare, called with Y = !CP_R4 and the pair in
; !CP_W0/!CP_W1; each leaves X on the next spare and Y = !CP_R4.  The fast path
; is the whole of a batch: the spare expected exactly this row (so the group
; above sits at base + 4(r-1) and the new one goes to base + 4r) and the colour
; differs from the one above -- an equal colour may still be an equal GROUP,
; and that is the slow path's to find out.
macro GbcCpSpare(k)
GbcCpSp<k>:
    lda.b !CP_R
    cmp.b !CP_NX+(11*<k>)
    bne GbcCpSl<k>
    lda.b !CP_W1
    cmp [!CP_PP+(11*<k>)],y
    beq GbcCpEq<k>
GbcCpAp<k>:
    sta [!CP_PC+(11*<k>)],y
    lda.b !CP_W0
    sta [!CP_PI+(11*<k>)],y
    inc.b !CP_NX+(11*<k>)
    inx
    inx
    rts
GbcCpEq<k>:             ; the same colour: the same GROUP only if the index is
    dey                 ; too -- {idx, idx} of the row above is two bytes below
    dey
    lda.b !CP_W0
    cmp [!CP_PP+(11*<k>)],y
    beq GbcCpSm<k>
    iny
    iny
    lda.b !CP_W1
    bra GbcCpAp<k>
GbcCpSm<k>:
    iny
    iny
GbcCpSl<k>:
    phx
    ldx.w #11*<k>
    jsr GbcCpSlow
    plx
    inx
    inx
    rts
endmacro
%GbcCpSpare(0)
%GbcCpSpare(1)
%GbcCpSpare(2)
%GbcCpSpare(3)

; X = 11 * spare.  Places {!CP_W0, !CP_W1} on row !CP_R, the long way: the
; group this spare holds (G, in the table at !CS_LP) is read back and the
; partition into holds and singletons decided exactly as TABLE SPEC S4 does.
; Leaves Y = !CP_R4.
GbcCpSlow:
    jsr GbcCsLoad
    lda.b !CP_R
    sec
    sbc.b !CS_ROW
    sta.b !CS_GAP       ; lines G holds before this row
    lda.l !CS_MODE,x
    bne GbcCsRun
    ; --- BATCH: G is the last singleton of the open batch
    lda.b !CS_GAP
    bne GbcCsBRet
    jsr GbcCsSame
    bcs GbcCsBRet       ; the same group on the next row: G is a run after all
    lda.b !CS_ROW
    sec
    sbc.l !CS_R0,x
    cmp.w #127
    bcs GbcCsFull
    ; a new group right after G, with room in the batch: append it (the fast
    ; path only declined because the colour was equal)
    lda.l !CS_LP,x
    clc
    adc.w #$0004
    sta.l !CS_LP,x
    tay
    lda.b !CP_W0
    sta [!GBC_PTR],y
    lda.b !CP_W1
    sta [!CP_P2],y
    lda.b !CP_R
    inc a
    sta.b !CP_NX,x
    ldy.b !CP_R4
    rts
GbcCsFull:
    lda.l !CS_HDR,x     ; 127 singletons: close the batch as $80|127 ...
    tay
    sep #$20
    lda.b #$FF
    sta [!GBC_PTR],y
    rep #$20
    lda.l !CS_LP,x
    clc
    adc.w #$0004        ; ... and open the next one right after it
    bra GbcCsOpen
GbcCsBRet:
    jsr GbcCsRetract
    bra GbcCsRunCmp
GbcCsRun:
    ; --- RUN: G is the data of a hold whose count is still to be written
    lda.l !CS_RUN,x
    clc
    adc.b !CS_GAP
    sta.l !CS_RUN,x
GbcCsRunCmp:
    jsr GbcCsSame
    bcc GbcCsFlush
    lda.l !CS_RUN,x     ; the same group once more: the run grows by a line
    inc a
    sta.l !CS_RUN,x
    lda.b !CP_R
    inc a
    ora.w #$8000
    sta.b !CP_NX,x
    ldy.b !CP_R4
    rts
GbcCsFlush:
    jsr GbcCsFlushRun   ; A = the byte after G
GbcCsOpen:
    ; A = where the header of a new batch goes; the pair is its first group
    sta.l !CS_HDR,x
    inc a
    sta.l !CS_LP,x
    tay
    lda.b !CP_W0
    sta [!GBC_PTR],y
    lda.b !CP_W1
    sta [!CP_P2],y
    lda.b !CP_R
    sta.l !CS_R0,x
    lda.w #$0000
    sta.l !CS_MODE,x
    lda.l !CS_LP,x      ; base: row r's group of this batch is at base + 4r
    sec
    sbc.b !CP_R4
    sta.b !CP_PI,x
    inc a
    inc a
    sta.b !CP_PC,x
    sec
    sbc.w #$0004
    sta.b !CP_PP,x
    lda.b !CP_R
    inc a
    sta.b !CP_NX,x      ; the fast path takes the next row
    lda.b !CP_R
    clc
    adc.w #127          ; the line its 128th group would land on
    cmp.b !CP_TRAP
    bcs GbcCsOpenOut
    sta.b !CP_TRAP
GbcCsOpenOut:
    ldy.b !CP_R4
    rts

; X = 11 * spare: !CS_ROW = the row it expects, G = the group it holds, read
; back from the table into !CS_G0/!CS_G1.  In a BATCH the fast path appends
; without keeping !CS_LP, so there it is recomputed from the row: the group of
; row r sits at base + 4r, and the last one is on the row before !CS_ROW.
GbcCsLoad:
    lda.b !CP_NX,x
    and.w #$7FFF
    sta.b !CS_ROW
    lda.l !CS_MODE,x
    bne GbcCsLdHave
    lda.b !CS_ROW
    dec a
    asl a
    asl a
    clc
    adc.b !CP_PI,x
    sta.l !CS_LP,x
GbcCsLdHave:
    lda.l !CS_LP,x
    tay
    lda [!GBC_PTR],y
    sta.b !CS_G0
    lda [!CP_P2],y
    sta.b !CS_G1
    rts

; Carry set = {!CP_W0, !CP_W1} is the group G the spare holds.
GbcCsSame:
    lda.b !CP_W0
    cmp.b !CS_G0
    bne GbcCsSameNo
    lda.b !CP_W1
    cmp.b !CS_G1
    bne GbcCsSameNo
    sec
    rts
GbcCsSameNo:
    clc
    rts

; G was the last singleton of the open batch, and it turns out to hold for
; 1 + !CS_GAP lines: it leaves the batch and becomes a RUN.  If it was the only
; one, the batch's header byte, right before it, simply becomes its count
; byte; otherwise the batch is closed on the others and G moves up one byte to
; make room for its count.
GbcCsRetract:
    lda.b !CS_ROW
    sec
    sbc.l !CS_R0,x      ; the batch's groups, G included
    dec a
    beq GbcCsRetOnly
    cmp.w #$0001
    beq GbcCsRetHdr     ; an isolated line is a hold of one, not $81 (S4)
    ora.w #$0080
GbcCsRetHdr:
    sta.b !CS_T
    lda.l !CS_HDR,x
    tay
    sep #$20
    lda.b !CS_T
    sta [!GBC_PTR],y
    rep #$20
    lda.l !CS_LP,x
    inc a
    sta.l !CS_LP,x
    tay
    lda.b !CS_G0
    sta [!GBC_PTR],y
    lda.b !CS_G1
    sta [!CP_P2],y
GbcCsRetOnly:
    lda.b !CS_GAP
    inc a
    sta.l !CS_RUN,x
    lda.w #$0001
    sta.l !CS_MODE,x
    rts

; Write the count of the RUN of G (at !CS_LP, its count byte right before it),
; split in 127s first and the remainder last as S4 says.  A = the byte after
; the last copy of G.
GbcCsFlushRun:
    lda.l !CS_LP,x
    tay
    dey
GbcCsFrLoop:
    lda.l !CS_RUN,x
    cmp.w #128
    bcc GbcCsFrLast
    sbc.w #127          ; carry set
    sta.l !CS_RUN,x
    sep #$20
    lda.b #127
    sta [!GBC_PTR],y
    rep #$20
    tya
    clc
    adc.w #$0005        ; the next hold entry of the same group
    tay
    iny
    lda.b !CS_G0
    sta [!GBC_PTR],y
    lda.b !CS_G1
    sta [!CP_P2],y
    dey
    bra GbcCsFrLoop
GbcCsFrLast:
    sep #$20
    sta [!GBC_PTR],y
    rep #$20
    tya
    clc
    adc.w #$0005
    rts

; --- the end of the walk ---------------------------------------------------------
GbcCpEnd:
    lda.b !CP_FO
    bmi GbcCpEndSat
    jsr GbcCpFoClose
GbcCpEndSat:
    ; placed = the slots up to the cursor (2 S a line, x2) - the skipped ones.
    ; 2 S * line by shift-and-add over the four bits of 2 S (it is 2..8).
    lda.b !CP_XEND
    sec
    sbc.b !CP_TBASE
    sta.b !CS_GAP       ; 2 S
    lda.b !CP_R
    sta.b !CS_ROW       ; line, doubled as the bits of 2 S are consumed
    txa
    sec
    sbc.b !CP_TBASE
    sec
    sbc.b !CP_SKIP2
GbcCpPlcLine:
    lsr.b !CS_GAP
    bcc GbcCpPlcBit
    clc
    adc.b !CS_ROW
GbcCpPlcBit:
    asl.b !CS_ROW
    ldy.b !CS_GAP
    bne GbcCpPlcLine
GbcCpPlcHave:
    sta.b !CP_SKIP2     ; now: placed, x2
    ; Close every spare that was placed on: whatever it holds holds to row 223
    ; (rows 184..223 hold what row 183 left, S2), then the terminator.
    stz.b !CP_V         ; colour channels
    ldx.w #$0000
GbcCpFin:
    cpx.b !CP_SEND
    bcs GbcCpFinDone
    lda.l !CS_HDR,x
    beq GbcCpFinNext    ; never placed on: stays disarmed
    inc.b !CP_V
    phx
    txa                 ; 11 k -> k, for the armed flag
    ldy.w #$0000
GbcCpFinK:
    cmp.w #11
    bcc GbcCpFinKHave
    sbc.w #11
    iny
    bra GbcCpFinK
GbcCpFinKHave:
    tyx
    sep #$20
    lda.b #$01
    sta.b $E4,x         ; this spare has earned its channel
    rep #$20
    plx
    jsr GbcCsLoad
    lda.w #184          ; row 224 = line 184
    sec
    sbc.b !CS_ROW
    sta.b !CS_GAP
    lda.l !CS_MODE,x
    bne GbcCpFinRun
    jsr GbcCsRetract    ; the last singleton holds to the bottom: a RUN
    bra GbcCpFinFlush
GbcCpFinRun:
    lda.l !CS_RUN,x
    clc
    adc.b !CS_GAP
    sta.l !CS_RUN,x
GbcCpFinFlush:
    jsr GbcCsFlushRun
    tay
    sep #$20
    lda.b #$00
    sta [!GBC_PTR],y    ; the terminator
    rep #$20
GbcCpFinNext:
    txa
    clc
    adc.w #11
    tax
    bra GbcCpFin
GbcCpFinDone:
    lda.b !CP_V
    sta.l !GBC_RC_CCH
    lda.b !CP_SKIP2     ; placed, x2
    lsr a
    sta.l !GBC_RC_PLACED
    sta.b !CS_T
    lda.b !CP_ONT2
    lsr a
    sta.b !CS_G0
    lda.b !CS_T
    sec
    sbc.b !CS_G0
    sta.l !GBC_RC_DELAY ; placed off their own line
    lda.l !GBC_RC_REQ
    sec
    sbc.b !CS_T
    sta.l !GBC_RC_DROP  ; requested and never placed
    sep #$20
    rts

; --- the fast path's placement: BOTH pairs of a CGB BCPD, inline -----------------
; One block per (spare count S, spare k the first pair lands on), reached by a
; single `jmp (GbcCpPairTab,x)` with the same X the per-pair table takes --
; so a hi-colour entry pays one dispatch instead of two calls and two returns,
; and the spares' direct-page fields are constants in the code.  k = S means
; the row is already full: both pairs go to the next line.  Every block ends
; with the cursor in X, as the walk expects.

; Place {!CP_W0, !CP_W1} on spare <k> at Y = !CP_R4 (Y is kept; X is not).
macro GbcCpPut(k)
    lda.b !CP_R
    cmp.b !CP_NX+(11*(<k>))
    bne ?slow
    lda.b !CP_W1
    cmp [!CP_PP+(11*(<k>))],y
    beq ?eq
?ap:
    sta [!CP_PC+(11*(<k>))],y
    lda.b !CP_W0
    sta [!CP_PI+(11*(<k>))],y
    inc.b !CP_NX+(11*(<k>))
    bra ?done
?eq:
    dey
    dey
    lda.b !CP_W0
    cmp [!CP_PP+(11*(<k>))],y
    beq ?same
    iny
    iny
    lda.b !CP_W1
    bra ?ap
?same:
    iny
    iny
?slow:
    ldx.w #11*(<k>)
    jsr GbcCpSlow
?done:
endmacro

macro GbcCpNextW0()
    lda.b !CP_W0
    clc
    adc.w #$2020
    sta.b !CP_W0
endmacro

; <tb> = !CP_TBASE for <s> spares.
macro GbcCpPair(s, k, tb)
GbcCpPr<s>_<k>:
if <k> < <s>
    %GbcCpPut(<k>)
    %GbcCpNextW0()
if <k>+1 < <s>
    %GbcCpPut(<k>+1)
    ldx.w #<tb>+(2*(<k>+2))
else
    ldx.w #<tb>+(2*<s>)
    jsr GbcCpWrapRow
    %GbcCpPut(0)
    ldx.w #<tb>+2
endif
else
    ldx.w #<tb>+(2*<s>)
    jsr GbcCpWrapRow
    %GbcCpPut(0)
    %GbcCpNextW0()
if <s> > 1
    %GbcCpPut(1)
    ldx.w #<tb>+4
else
    ldx.w #<tb>+2
    jsr GbcCpWrapRow
    %GbcCpPut(0)
    ldx.w #<tb>+2
endif
endif
    jmp GbcCpLoopCgb
endmacro

%GbcCpPair(1, 0, 0)
%GbcCpPair(1, 1, 0)
%GbcCpPair(2, 0, 4)
%GbcCpPair(2, 1, 4)
%GbcCpPair(2, 2, 4)
%GbcCpPair(3, 0, 10)
%GbcCpPair(3, 1, 10)
%GbcCpPair(3, 2, 10)
%GbcCpPair(3, 3, 10)
%GbcCpPair(4, 0, 18)
%GbcCpPair(4, 1, 18)
%GbcCpPair(4, 2, 18)
%GbcCpPair(4, 3, 18)
%GbcCpPair(4, 4, 18)

GbcCpPairTab:
    dw GbcCpPr1_0, GbcCpPr1_1
    dw GbcCpPr2_0, GbcCpPr2_1, GbcCpPr2_2
    dw GbcCpPr3_0, GbcCpPr3_1, GbcCpPr3_2, GbcCpPr3_3
    dw GbcCpPr4_0, GbcCpPr4_1, GbcCpPr4_2, GbcCpPr4_3, GbcCpPr4_4

; X = the wrap entry (a full row): the cursor moves to the next line, Y =
; !CP_R4.  Called only from the blocks above, which the walk reached by a
; jump -- so running out of lines unwinds exactly this one call.
GbcCpWrapRow:
    lda.b !CP_FO
    bmi GbcCpWrrFo
    jsr GbcCpFoClose
GbcCpWrrFo:
    lda.b !CP_R4
    clc
    adc.w #$0004
    sta.b !CP_R4
    tay
    inc.b !CP_R
    lda.b !CP_R
    cmp.w #144
    bcs GbcCpWrrSat
    cmp.b !CP_TRAP
    bcc GbcCpWrrOut
    jsr GbcCpTrap
GbcCpWrrOut:
    rts
GbcCpWrrSat:
    pla                 ; this call's return address: the walk is over
    ldx.b !CP_TBASE
    jmp GbcCpEndSat

; X = !CP_TBASE + 2 * spare.  After the last spare of the frame's count comes
; GbcCpWrap, which moves the cursor to spare 0 of the next row.
GbcCpTab:
    dw GbcCpSp0, GbcCpWrap                                  ; 1 spare: X from 0
    dw GbcCpSp0, GbcCpSp1, GbcCpWrap                        ; 2: from 4
    dw GbcCpSp0, GbcCpSp1, GbcCpSp2, GbcCpWrap              ; 3: from 10
    dw GbcCpSp0, GbcCpSp1, GbcCpSp2, GbcCpSp3, GbcCpWrap    ; 4: from 18
GbcCpTBase:
    dw 0, 0, 4, 10, 18

; The view byte an OCPD write lands on, indexed by 2 * (idx & $3F): with
; i = 8q + r, 256 + 32q + r.
GbcCpObByte:
!i = 0
while !i < 64
    dw 256+(32*(!i>>3))+(!i&7)
!i #= !i+1
endwhile

; {128 + 16q + k} twice for a CGB OCPD, indexed by idx & $3E = 2(4q + k) --
; only read for k != 0.
GbcCpOcpdIdx:
!i = 0
while !i < 32
    dw (128+(16*(!i>>2))+(!i&3))*$0101
!i #= !i+1
endwhile

; {i, i} for the first pair of a CGB BCPD, indexed by idx & $3E = 2i, i = 4p+k:
; k != 0 -> the BG1 region, entry i; k = 0 -> the first carpet, 64 + 4p + 1.
; The second pair is 32 entries on in both cases (+$2020).
GbcCpBcpdIdx:
!i = 0
while !i < 32
if !i&3 == 0
    dw (!i+65)*$0101
else
    dw !i*$0101
endif
!i #= !i+1
endwhile

; ---------------------------------------------------------------------------
; The set header: what the prologue reads to point the channels.
; ---------------------------------------------------------------------------
GbcSetHeader:
    lda.b #$30
    sta.b $E9           ; ch5 (letterbox) and ch4 (window 1) are always armed
    stz.b $E8
    stz.b $EA           ; the DMAP a disarmed channel gets is irrelevant
GbcShClear:
    stz.b $AE           ; B-bus address 0 = the channel stays disarmed
    rep #$20
    lda.w #$0000
    sta.b $98
    sep #$20
    lda.b $E8
    jsr GbcHdrSet
    inc.b $E8
    lda.b $E8
    cmp.b #$04
    bne GbcShClear
    lda.b #$03
    sta.b $EA           ; mode 3: two registers, four bytes a line
    lda.b $A9
    lsr a
    bcc GbcShWin
    lda.b #$0F
    sta.b $AE
    rep #$20
    lda.w #!GBC_T_CH0
    sta.b $98
    sep #$20
    lda.b #$00
    jsr GbcHdrSet       ; ch0 -> BG2HOFS
    lda.b #$13
    sta.b $AE
    lda.b #$01
    jsr GbcHdrSet       ; ch1 -> BG4HOFS, pointed at the SAME table
    lda.b $E9
    ora.b #$03
    sta.b $E9
GbcShWin:
    lda.b $A9
    and.b #$02
    beq GbcShLcdc
    lda.b #$0D
    sta.b $AE
    rep #$20
    lda.w #!GBC_T_CH2
    sta.b $98
    sep #$20
    lda.b #$02
    jsr GbcHdrSet       ; ch2 -> BG1HOFS
    lda.b $E9
    ora.b #$04
    sta.b $E9
    lda.b $A9
    and.b #$20
    bne GbcShLcdc       ; ⚡ S13: the LCDC group took ch3.  BG3 keeps the
                        ; snapshot's scroll for the whole frame, written by
                        ; GbcRegsScroll because $73 b3 is clear
    lda.b #$11
    sta.b $AE
    rep #$20
    lda.w #!GBC_T_CH2
    sta.b $98
    sep #$20
    lda.b #$03
    jsr GbcHdrSet       ; ch3 -> BG3HOFS, the SAME table as ch2
    lda.b $E9
    ora.b #$08
    sta.b $E9
GbcShLcdc:
    ; ⚡ the LCDC group: mode 4, $2108 and the three registers after it.
    lda.b $86
    cmp.b #$FF
    beq GbcShColour
    jsr GbcChSlotY      ; FIRST: it owns $AE, which the B-bus address lives in
    rep #$20
    tya
    sec
    sbc.b $96
    sta.b $98
    sep #$20
    lda.b #$08          ; BBAD = $2108, and mode 4 takes the three after it
    sta.b $AE
    lda.b #$04          ; mode 4: FOUR registers, four bytes a line
    sta.b $EA
    lda.b $86
    jsr GbcHdrSet
    lda.b $86
    jsr GbcShMaskBit
    lda.b #$03
    sta.b $EA           ; back to mode 3 for the colour channels below
GbcShColour:
    stz.b $E8
GbcShColLoop:
    lda.b $E8
    cmp.b $A8
    bcs GbcShDone
    rep #$20
    lda.b $E8
    and.w #$00FF
    tax
    sep #$20
    lda.b $E4,x
    beq GbcShColNext
    lda.b $E0,x
    sta.b $EB           ; the channel this spare was given
    jsr GbcChSlotY      ; Y = the table it wrote
    rep #$20
    tya
    sec
    sbc.b $96
    sta.b $98
    sep #$20
    lda.b #$21
    sta.b $AE           ; $2121: CGADD, CGADD, CGDATA, CGDATA
    lda.b $EB
    jsr GbcHdrSet
    lda.b $EB
    jsr GbcShMaskBit
GbcShColNext:
    inc.b $E8
    bra GbcShColLoop
GbcShDone:
    rep #$20
    lda.b $96
    clc
    adc.w #!GBC_H_MASK
    tay
    sep #$20
    lda.b $E9
    sta [!GBC_PTR],y
    rep #$20
    and.w #$003F
    tax
    sep #$20
    lda.l GbcHdmaMaskTab,x
    iny                 ; !GBC_H_PMASK: worked out here, off the NMI
    sta [!GBC_PTR],y
    rts

; A(8) = channel -> $E9 |= 1 << channel.  X is scratch.
GbcShMaskBit:
    rep #$20
    and.w #$00FF
    tax
    sep #$20
    lda.b #$01
GbcShMbLoop:
    cpx.w #$0000
    beq GbcShMbDone
    asl a
    dex
    bra GbcShMbLoop
GbcShMbDone:
    ora.b $E9
    sta.b $E9
    rts

; A(8) = channel, $AE = B-bus address, $98 = the table's offset inside the set,
; $EA = the DMAP the prologue must program ($43x0).  The mode is carried rather
; than assumed because ⚡ the LCDC group is mode 4 and everything else is 3.
GbcHdrSet:
    rep #$20
    and.w #$00FF
    asl a
    asl a
    clc
    adc.b $96
    adc.w #!GBC_H_CH
    tay
    sep #$20
    lda.b $AE
    sta [!GBC_PTR],y
    iny
    rep #$20
    lda.b $98
    sta [!GBC_PTR],y
    sep #$20
    iny
    iny
    lda.b $EA
    sta [!GBC_PTR],y
    rts

; ---------------------------------------------------------------------------
; THE RUN-LENGTH ENCODER (contract sec. 11.4, the golden's TABLE SPEC S4).
;
; {count, data}: count 1..127 repeats the group for `count` lines and costs
; only its header -- a hold entry TRANSFERS only on the first line of its run;
; count | $80 means `count` lines each with their own group; $00 terminates.
; A run longer than 127 is split into 127 first and the remainder after.
;
; The one thing the format leaves open is a line that is on its own: it can be
; written `1, <group>` or `$81, <group>`, and both transfer the same bytes on
; the same line.  It is `1, <group>` here -- the encoding that reproduces the
; static letterbox table byte for byte, line 40 being an isolated line between
; two long runs.  It costs nothing to get right: the header byte of an open
; $80|k batch is written back at the END of the batch, so k == 1 just patches a
; different value into a byte that was already reserved.
;
; X = slot*12 throughout.  The 65816 has no absolute-long,Y, so the table
; cursor goes in Y and the bytes are stored through [$F0],y.
; ---------------------------------------------------------------------------
GbcEncInit:
    sta.b !ENC_GSZ,x
    rep #$20
    tya
    sta.b !ENC_CUR,x
    lda.w #$0000
    sta.b !ENC_HDR,x
    sep #$20
    stz.b !ENC_K,x
    stz.b !ENC_RUN,x
    rts

; A(8) = the byte to append.
GbcEncPut:
    ldy.b !ENC_CUR,x
    sta [!GBC_PTR],y
    iny
    sty.b !ENC_CUR,x
    rts

; ⚡ P5B: the group goes out as 16-bit words -- a group is 2 bytes (window 1)
; or 4 (everything else); no table of the compiler has 1-byte groups (the
; letterbox's, ch5, is built once at boot).  A 8-bit in and out.
GbcEncPutGroup:
    rep #$20
    ldy.b !ENC_CUR,x
    lda.b !ENC_GRP,x
    sta [!GBC_PTR],y
    iny
    iny
    lda.b !ENC_GSZ,x
    and.w #$00FF
    cmp.w #$0004
    bcc GbcEpgDone
    lda.b !ENC_GRP+2,x
    sta [!GBC_PTR],y
    iny
    iny
GbcEpgDone:
    sty.b !ENC_CUR,x
    sep #$20
    rts

; Carry set = the group in $9A.. is the one the encoder is holding.  A 8-bit in
; and out.
GbcEncGrpEq:
    rep #$20
    lda.b $9A
    cmp.b !ENC_GRP,x
    bne GbcEgeNo
    lda.b !ENC_GSZ,x
    and.w #$00FF
    cmp.w #$0004
    bcc GbcEgeYes
    lda.b $9C
    cmp.b !ENC_GRP+2,x
    bne GbcEgeNo
GbcEgeYes:
    sep #$20
    sec
    rts
GbcEgeNo:
    sep #$20
    clc
    rts

GbcEncSetGrp:
    rep #$20
    lda.b $9A
    sta.b !ENC_GRP,x
    lda.b $9C
    sta.b !ENC_GRP+2,x
    sep #$20
    rts

; A(8) = lines carrying the group in $9A..
GbcEncAdd:
    pha
    lda.b !ENC_RUN,x
    beq GbcEaNew
    jsr GbcEncGrpEq
    bcc GbcEaNew
    pla
    clc
    adc.b !ENC_RUN,x
    sta.b !ENC_RUN,x
    rts
GbcEaNew:
    jsr GbcEncFlushRun
    jsr GbcEncSetGrp
    pla
    sta.b !ENC_RUN,x
    rts

; A(8) = more lines of whatever the encoder is already holding.
GbcEncHold:
    clc
    adc.b !ENC_RUN,x
    sta.b !ENC_RUN,x
    rts

GbcEncEnd:
    jsr GbcEncFlushRun
    jsr GbcEncFlushBatch
    lda.b #$00
    jsr GbcEncPut
    rts

GbcEncFlushBatch:
    lda.b !ENC_K,x
    beq GbcEfbDone
    cmp.b #$01
    beq GbcEfbOne
    ora.b #$80
    bra GbcEfbPatch
GbcEfbOne:
    lda.b #$01          ; an isolated line is a hold of one, not $81
GbcEfbPatch:
    ldy.b !ENC_HDR,x
    sta [!GBC_PTR],y
    stz.b !ENC_K,x
GbcEfbDone:
    rts

GbcEncFlushRun:
    lda.b !ENC_RUN,x
    beq GbcEfrDone
    cmp.b #$02
    bcc GbcEfrSingle
    jsr GbcEncFlushBatch
GbcEfrLoop:
    lda.b !ENC_RUN,x
    beq GbcEfrDone
    cmp.b #128
    bcc GbcEfrLast
    lda.b #127
GbcEfrLast:
    pha
    jsr GbcEncPut
    jsr GbcEncPutGroup
    pla
    sta.b $AE
    lda.b !ENC_RUN,x
    sec
    sbc.b $AE
    sta.b !ENC_RUN,x
    bra GbcEfrLoop
GbcEfrSingle:
    lda.b !ENC_K,x
    cmp.b #127
    bne GbcEfrNoFlush
    jsr GbcEncFlushBatch
GbcEfrNoFlush:
    lda.b !ENC_K,x
    bne GbcEfrHaveBatch
    rep #$20
    lda.b !ENC_CUR,x
    sta.b !ENC_HDR,x    ; reserve the header; its value is decided at the end
    sep #$20
    lda.b #$00
    jsr GbcEncPut
GbcEfrHaveBatch:
    jsr GbcEncPutGroup
    inc.b !ENC_K,x
    stz.b !ENC_RUN,x
GbcEfrDone:
    rts

; ---------------------------------------------------------------------------
; Small shared helpers.
; ---------------------------------------------------------------------------
GbcLoadSnapRegs:
    ldx.w #$0000
GbcLsrLoop:
    lda.l !GBC_SNAP+!GBC_SNAP_REGS,x
    sta.b $88,x
    inx
    cpx.w #$0008
    bne GbcLsrLoop
    rts

GbcRctrClear:
    ldx.w #$0000
    lda.b #$00
GbcRcClrLoop:
    sta.l !GBC_RCTR,x
    inx
    cpx.w #!GBC_RCTR_LEN
    bne GbcRcClrLoop
    rts

; X = the counter's offset inside the block.
GbcRctrInc:
    rep #$20
    lda.l !GBC_RCTR,x
    inc a
    sta.l !GBC_RCTR,x
    sep #$20
    rts

; ===========================================================================
; The LCD-off screen: a whole 32x32 map of {t = 769, pal = 0, pri = 0}.
;
; ⚡ wire $02 moved it to FREE VRAM ($A000-$A7FF) and writes it ONCE, here, in
; the forced blank of the bring-up.  BG4 is pointed at it with $210A = $50 for
; as long as the LCD is off and pointed back at a carpet when it comes on, so
; nothing of the picture is destroyed: the LCD going off costs two register
; writes instead of a 2 KB DMA, the carpet class is transferable the whole
; time, and coming back no longer has to mark every map row dirty again.
;
; ⚠ THIS CANNOT BE A FIXED-SOURCE DMA over a two-byte `dw $0301`, which is what
; it was until the bsnes-plus harness measured the result.  There is no
; word-wide fixed mode on the 5A22: with the A-bus held still, mode 1 feeds the
; SAME byte to $2118 and to $2119, so the word that lands is $0101 -- tile 257,
; whose pixels are whatever the game happens to have there (all zeros in
; cgb-acid2) -- instead of $0301 = tile 769, the solid colour-2 tile that makes
; CGRAM[98] = $7FFF show as white.  Measured: the map full of 01 01 01 01 and
; the whole 23040-pixel viewport black where the contract wants white.
; Spelling the pattern out in ROM keeps it to ONE ordinary incrementing DMA
; (the two-DMA alternative -- low bytes with VMAIN incrementing on $2118, high
; bytes on $2119 -- costs a VMAIN mode nothing else in this file uses).
; 2 KB of a 32 KB image that is otherwise ~90% empty.
; ===========================================================================
GbcWhiteMapUp:
    lda.b #$80
    sta.w $2115         ; VMAIN: +1 word after $2119
    rep #$20
    lda.w #$5000        ; VRAM word $5000 = byte $A000
    sta.w $2116
    sep #$20
    lda.b #$01
    sta.w $4370         ; ch7 DMAP: A->B, INCREMENT, mode 1 ($2118/$2119)
    lda.b #$18
    sta.w $4371
    rep #$20
    lda.w #GbcWhiteMapData
    sta.w $4372
    sep #$20
    stz.w $4374         ; the pattern lives in this ROM, bank $00
    rep #$20
    lda.w #2048
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B         ; fire ch7
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rts

; 1024 x {t = 769, pal 0, pri 0}: byte 0 = t[7:0] = $01, byte 1 = t[9:8] = %11.
GbcWhiteMapData:
!i = 0
while !i < 1024
    dw $0301
!i #= !i+1
endwhile
GbcWhiteMapDataEnd:
assert GbcWhiteMapDataEnd-GbcWhiteMapData == 2048, "the LCD-off map must be a whole 32x32 screen"

; ===========================================================================
; DRAIN -- send as much of the backlog as the budget and the clock allow.
;
; CGRAM and OAM first: both are single 512 B transfers whose content is
; all-or-nothing (a half-copied OAM is a screen full of misplaced sprites,
; a half-copied CGRAM is wrong colours), and both are skipped outright when the
; snapshot did not advance.  Everything else is blocks, and the six block
; classes are walked in a ROTATING order.
;
; Why rotate rather than keep a fixed priority: the budget is a hard 12600 and
; four dirty map views alone are 9728 of it, so a fixed order would let the
; same class eat the frame every time and starve whatever sits behind it -- the
; A26 player's field report in miniature.  Rotating the entry point costs four
; instructions and makes every class advance under overload.  Nothing here
; tears as a result: a block that misses this frame is re-sent next frame and
; the picture converges (contract sec. 12.6/12.7 already assume that).
; ===========================================================================
; ⚡ wire $03: the framebuffer's own drain goes in FRONT of everything while
; the mode is entering or on (states 1, 2: it is the picture, or about to be),
; and BEHIND CGRAM/OAM while it is leaving (state 3: the way back needs those
; two current, advisor A7).  Window A of the frame the mode left owes the
; bridge FB_EN = 0 and VRAM the white map (GbcFbOwed).  See GbcDrainFront /
; GbcDrainBack.
GbcDrainFb:
    jsr GbcDrainOpen
    jsr GbcDrainFront
    jsr GbcDrainCgOam
    jsr GbcDrainBack
    jsr GbcDrainWalkFb
    jmp GbcDrainEnd

; The $02 drain, byte for byte: the body runs it with the FB mode OFF and
; nothing owed (GbcNmiSnapOk).
GbcDrain:
    jsr GbcDrainOpen
    jsr GbcDrainCgOam
    jsr GbcDrainWalk
    jmp GbcDrainEnd

; The frame's walk starts here -- once per frame, in window B when the V-IRQ
; opened the frame and in window A otherwise.
GbcDrainOpen:
    stz.b $17
    lda.b $0D
    sta.b $67           ; where this frame's walk starts, defined before the
                        ; CGRAM/OAM transfers below so that an abort there
                        ; leaves the entry point exactly where it was
    lda.b #!GBC_CLS_N
    sta.b $66           ; classes still to visit this frame
    rts

; CGRAM then OAM, each only when the snapshot advanced and the backlog has it.
GbcDrainCgOam:
    lda.b $3E
    beq GbcDrainCgOamOut
    lda.b $3A
    and.b #$02
    beq GbcDrainNoCg
    jsr GbcDrainCgram
GbcDrainNoCg:
    lda.b $17
    bne GbcDrainCgOamOut
    lda.b $3A
    and.b #$01
    beq GbcDrainCgOamOut
    jsr GbcDrainOam
GbcDrainCgOamOut:
    rts

; ⚡ wire $03: window B's share with the FB mode on or owed (GbcDrainB is the
; $02 one).
GbcDrainBFb:
    jsr GbcDrainFront   ; ⚡ wire $03: the FB first (states 1, 2)
    lda.b $3E
    beq GbcDrainBBack
    lda.b $3A
    and.b #$01
    beq GbcDrainBBack
    jsr GbcDrainOam     ; (one that does not fit leaves $17 up: no walk)
GbcDrainBBack:
    jsr GbcDrainBack    ; ⚡ ... and after OAM while leaving (state 3)
; The walk with the FB mode anywhere but OFF: an EMPTY backlog skips it.
GbcDrainWalkFb:
    lda.b $17
    bne GbcDrainWalkOut
    lda.w !GBC_FB_STATE         ; ⚡ wire $03: with the FB mode anywhere but
    beq GbcDrainClsLoop         ; OFF, an EMPTY backlog skips the walk: eleven
    cmp.b #$03
    bcs GbcDrainWalkFbE         ; (leaving: the walk runs in both stages)
    lda.w !GBC_FB_STG
    bne GbcDrainWalkOut         ; second stage: the FB owns the classes' VRAM
GbcDrainWalkFbE:
    jsr GbcFbBacklogEmpty       ; empty classes cost ~16 lines, twice a frame,
    beq GbcDrainWalkOut         ; all of it off the FB's window
    bra GbcDrainClsLoop

; Window B's share: OAM (no channel writes $2102-$2104) and the walk.  CGRAM is
; left to the resume -- see TWO WINDOWS.
GbcDrainB:
    lda.b $3E
    beq GbcDrainWalk
    lda.b $3A
    and.b #$01
    beq GbcDrainWalk
    jsr GbcDrainOam
    ; fall through: an OAM that did not fit leaves $17 up and the walk waits

; Visit the classes still owed this frame, from $67 on, until $66 runs out or
; a transfer does not fit ($17).  The class that stopped the walk is NOT
; stepped past: its cursor is persistent, and the resume starts right there.
GbcDrainWalk:
    lda.b $17
    bne GbcDrainWalkOut
GbcDrainClsLoop:
    lda.b $66
    beq GbcDrainWalkOut
    ; ⚡ No class is ever skipped now.  Wire $01 had to hold the carpet map
    ; back while the LCD was off, because the white screen was painted over
    ; that very region; the white map has its own VRAM here.
    lda.b $67
    jsr GbcDrainCls
    lda.b $17
    bne GbcDrainWalkOut
    lda.b $67
    inc a
    cmp.b #!GBC_CLS_N
    bcc GbcDrainClsNext
    lda.b #$00
GbcDrainClsNext:
    sta.b $67
    dec.b $66
    bra GbcDrainClsLoop
GbcDrainWalkOut:
    rts

; ⚡ wire $03: window A's share with the FB mode on or owed.
GbcDrainResumeFb:
    stz.b $17
    jsr GbcDrainFront   ; ⚡ wire $03 (see GbcDrainFb)
    jsr GbcDrainCgOam
    jsr GbcDrainBack
    jsr GbcDrainWalkFb
    bra GbcDrainEnd

; Window A's share of a frame the V-IRQ opened: CGRAM (and OAM if window B
; could not take it), then the SAME walk carried on, then the frame's close.
GbcDrainResume:
    stz.b $17
    jsr GbcDrainCgOam
    jsr GbcDrainWalk

GbcDrainEnd:
    ; ⚡ WHERE THE NEXT FRAME'S WALK STARTS -- AND WHY IT IS A BLIND `+1`.
    ;
    ; THE PROPERTY: with every class permanently dirty, EVERY class transfers
    ; more than zero bytes in any window of !GBC_CLS_N consecutive frames.
    ; THE PROOF: the head advances by exactly one, modulo the class count, and
    ; depends on NOTHING else -- so over any N consecutive frames it takes all
    ; N values, and the class that is head is walked FIRST, with the whole of
    ; window A in front of it (the status block costs 472 of the ~11800
    ; byte-equivalents GbcCapacity offers at V=225, and one block of the
    ; largest class costs 896).  A head class with anything pending therefore
    ; always sends at least one block.  K = N = !GBC_CLS_N frames, and it is
    ; the class count that sets it, nothing else.
    ; ⚡ Phase 5: in a frame the V-IRQ opened, the head is walked first in
    ; WINDOW B, and the same argument holds there -- window B offers ~4.7 K eq
    ; at V=185, the status block and OAM take 1.4 K of it, and 896 still fits.
    ; Picking the walk up at the class window B stopped in (GbcDrainResume) is
    ; NOT the "resume at the class that aborted" below: that one is about which
    ; class HEADS THE NEXT FRAME; this is the same walk of the same frame, and
    ; this close runs once per frame either way.
    ;
    ; ⚠ THE RULE MAY NOT LOOK AT WHERE THE BUDGET DIED.  That is feedback, and
    ; feedback can lock: making `$0D` the class AFTER the one that aborted
    ; sends precisely the starved class to the END of the next lap, and with
    ; eleven classes that closes a LIMIT CYCLE -- measured, the possible heads
    ; collapsed to {1, 5, 7} and two classes (chr base A among them)
    ; transferred once in frame 1 and never again, for ever.  Resuming AT the
    ; class that aborted is worse still: the big class monopolises and the map
    ; classes get zero.  What `+1` costs is convergence LATENCY -- a class
    ; waits up to N frames to become head -- and latency is a picture that
    ; arrives late, not a picture that is permanently wrong.
    lda.b $0D
    inc a
    cmp.b #!GBC_CLS_N
    bcc GbcDrainRotOk
    lda.b #$00
GbcDrainRotOk:
    sta.b $0D
    lda.b $17
    beq GbcDrainNoDefer
    lda.b #$01
    sta.b $3F
GbcDrainNoDefer:
    rts

; --- CGRAM: 512 B, $E2:0000 -> $2122 ---------------------------------------
; The view already carries the two colours the contract fixes (entry 0 = $0000
; backdrop, entry 98 = $7FFF for the LCD-off screen), so a whole-block copy is
; also what keeps them right; nothing is re-planted afterwards.
GbcDrainCgram:
    rep #$20
    lda.w #512
    sta.b $1C
    sep #$20
    ; ⚠ A COLOUR CHANNEL AND A CGRAM DMA CANNOT SHARE A LINE.  An armed colour
    ; channel writes $2121/$2122 in the hblank of EVERY line it is armed for,
    ; rows 0..40 included -- which is exactly the letterbox tail this transfer
    ; runs in.  A general DMA to CGRAM sets $2121 ONCE and then streams $2122,
    ; so an HDMA $2121 landing in the middle of it moves the CGRAM pointer and
    ; the rest of the block goes to the wrong entries: 512 bytes of wrong
    ; colour, silently.  With a colour channel up, CGRAM traffic is therefore
    ; confined to TRUE vblank, where no HDMA transfers at all.
    ;
    ; ⚠ AND "V >= 225" IS NOT THAT TEST.  512 bytes are ~3 scanlines, so a DMA
    ; started at V = 259..261 is still on the bus at V = 0, where the HDMA
    ; re-initialises and every armed colour channel makes its FIRST transfer of
    ; the letterbox hold -- $2121 among them.  The block has to FIT IN WHAT IS
    ; LEFT OF THE VBLANK, which is the vblank term of GbcCapacity ALONE: the
    ; capacity that routine returns adds the 40-line tail ($71, ~5.7 KB-eq with
    ; four colour channels up), and it is precisely the tail that is off limits
    ; here.  Comparing against it would accept V = 261.
    ;
    ; The same question was asked of every other general DMA this player fires,
    ; and this is the only one that has to answer it (the audit is in the
    ; header's TRANSFER MODEL note): the status, log and CGRAM-view fetches go
    ; to the WRAM port ($2180-$2183), OAM to $2102-$2104 and the block classes
    ; to $2115-$2119 -- no armed channel writes any of those, so an HDMA slot
    ; taken in the middle of one of them only pauses it.
    ;
    ; ⚡ advisor C6-PLAYER: AND THE FIT COUNTS ONLY THE LINES AFTER THE ONE IT
    ; READ, WITH !GBC_CGFITSLACK HELD BACK.  "262 - V" counted the line the
    ; latch landed in as whole (it may be at its last dot) and charged the ~40
    ; instructions between the latch and the $420B (~0.8 line, 1.1 at the
    ; 1.3x CPU) nothing but the 41 of !GBC_DMASTART: the P5-VIRQ
    ; p4-cgb-colour case hits S9 with the $02 player itself once ~20-40
    ; cycles are added anywhere in front of the drain (measured: nops in the
    ; $02 body), i.e. its V=258 edge was never safe, only lucky.  The last V
    ; that fits is now 256 (5 x 170 = 850 >= 512 + 41 + 256).
    ;
    ; ⚡ THE GUARD FIRST, THE VBLANK FIT LAST (wire $03).  The fit below is
    ; only as good as the V it read, and it used to be read BEFORE the guard:
    ; GbcDmaGuard -> GbcCapacity (a second V read, a multiply) is ~1.5 lines on
    ; the host clock, all of it between "fits in what is left of the vblank"
    ; and the $420B.  The 41 byte-equivalents of !GBC_DMASTART cover a DMA
    ; set-up, not that -- measured, a block accepted at V=258 started at V=260
    ; and ran into V=0 with a colour channel armed (P5-VIRQ p4-cgb-colour, S9)
    ; as soon as the status copy grew by the 58 C6 bytes.  Now the fit's V read
    ; is the last thing before the transfer's own set-up; at a pinned V
    ; (tests/host, P4-CG) the answer is the same as before.
    jsr GbcDmaGuard
    bcc GbcDrainCgNo
    lda.b $73
    and.b #$04
    beq GbcDrainCgV
    jsr GbcVCount       ; A(16) = live scanline, also left in $1A
    cmp.w #225
    bcc GbcDrainCgNoW   ; active display or the letterbox tail
    lda.w #!GBC_LINES-1
    sec
    sbc.b $1A           ; whole vblank lines left before V=0 AFTER this one
    bcc GbcDrainCgNoW   ; V past the last line: PAL, overscan or a stale latch
    sep #$20            ; -- claim nothing, exactly as GbcCapVblOk does
    sta.w $4202
    lda.b #!GBC_BPLVBL
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.w $4216         ; byte-equivalents left in THIS vblank
    sec
    sbc.b $1C
    bcc GbcDrainCgNoW
    sec
    sbc.w #!GBC_DMASTART+!GBC_CGFITSLACK
    bcc GbcDrainCgNoW   ; it would still be running when the HDMA inits
    sep #$20
GbcDrainCgV:
    stz.w $2121         ; CGADD = 0
    lda.b #$00
    sta.w $4370         ; ch7 DMAP: A->B, increment, mode 0
    lda.b #$22
    sta.w $4371         ; BBAD = $2122
    rep #$20
    lda.w #!GBC_CGRAM_A16
    sta.w $4372
    sep #$20
    lda.b #!GBC_CGRAM_A1B
    sta.w $4374
    rep #$20
    lda.b $1C
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.b $1C
    jsr GbcDebit
    sep #$20
    lda.b $3A
    and.b #$FD
    sta.b $3A
    rts
GbcDrainCgNoW:
    sep #$20            ; the vblank-fit test above rejects with A 16-bit
GbcDrainCgNo:
    jsr GbcDropCount
    lda.b #$01
    sta.b $17
    rts

; --- OAM: 512 B, $E3:0000 -> $2104 -----------------------------------------
; Low table only.  The 32-byte high table is all zeros and was written once at
; init (contract sec. 4.5/13.9): X bit 8 = 0 and size = small for all 128
; sprites, which is what makes Y = $F0 hide a sprite instead of wrapping it.
GbcDrainOam:
    rep #$20
    lda.w #512
    sta.b $1C
    sep #$20
    jsr GbcDmaGuard
    bcc GbcDrainOamNo
    stz.w $2102
    stz.w $2103         ; OAM word address 0, priority rotation off
    lda.b #$00
    sta.w $4370
    lda.b #$04
    sta.w $4371         ; BBAD = $2104
    rep #$20
    lda.w #!GBC_OAM_A16
    sta.w $4372
    sep #$20
    lda.b #!GBC_OAM_A1B
    sta.w $4374
    rep #$20
    lda.b $1C
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.b $1C
    jsr GbcDebit
    sep #$20
    lda.b $3A
    and.b #$FE
    sta.b $3A
    rts
GbcDrainOamNo:
    jsr GbcDropCount
    lda.b #$01
    sta.b $17
    rts

; ===========================================================================
; One block class.  Round-robin: the scan starts at the class's PERSISTENT
; cursor, never at block 0, and because a run has to be contiguous in block
; number while the wrap from the last block to block 0 is not, the walk is done
; as two straight segments instead of modular arithmetic in the inner loop.
; ===========================================================================
GbcDrainCls:
    jsr GbcClsLoad
    jsr GbcClsEmpty
    beq GbcDrainClsOut  ; O(1) exit for a class with nothing pending.  Without
                        ; it an IDLE frame still walks all 208 blocks of the
                        ; six classes probing bits that are all zero, which the
                        ; bsnes-plus harness measured at 75 of the 78 lines of
                        ; window A -- the whole transfer budget spent moving
                        ; nothing, and the frame body ending 85 lines past the
                        ; start of vblank.
    ldx.b $42
    lda.b $00,x
    cmp.b $44
    bcc GbcDrainClsCur
    lda.b #$00
    sta.b $00,x
GbcDrainClsCur:
    ; $61 and NOT the 16-bit scratch at $58: this value has to survive the
    ; whole of segment 1, and every DMA fired inside it goes through GbcClsDma
    ; (shift operand) and GbcDebit (byte count), both of which own $58.  What
    ; came back from $58 was the low byte of the last DMA's length: 0 skipped
    ; segment 2 harmlessly, anything else made $51 = (bytes & $FF) - 1, a
    ; segment end far past the class's block count -- the scan then walked off
    ; the end of this class's bitmap into the next one (the backlogs are
    ; contiguous in the direct page) and fired DMAs for blocks >= nblk, reading
    ; the wrong view and writing outside the class's VRAM region.  Found by
    ; tests/host/run_gbc_player.sh (map:0-2, map:2-4); runs whose byte count is
    ; a multiple of 256 hid it.
    sta.b $61           ; where this walk began
    sta.b $50
    lda.b $44
    dec a
    sta.b $51           ; segment 1 = cursor .. N-1
    jsr GbcDrainSeg
    lda.b $17
    bne GbcDrainClsOut
    lda.b $61
    beq GbcDrainClsOut  ; the walk began at 0: segment 1 was everything
    stz.b $50
    dec a
    sta.b $51           ; segment 2 = 0 .. cursor-1
    jsr GbcDrainSeg
GbcDrainClsOut:
    rts

; Scan blocks $50..$51.  $45/$62 carry the current block's bit mask and backlog
; byte so the scan costs an AND per block instead of a GbcBlockBit call.
GbcDrainSeg:
    lda.b $50
    jsr GbcBlockBit
    stx.b $62
GbcSegScan:
    lda.b $50
    cmp.b $51
    beq GbcSegTest
    bcs GbcSegOut
GbcSegTest:
    ldx.b $62
    lda.b $00,x
    and.b $45
    bne GbcSegRun
    jsr GbcSegNext
    bra GbcSegScan
GbcSegOut:
    rts

GbcSegNext:
    inc.b $50
    asl.b $45
    bne GbcSegNextDone
    lda.b #$01
    sta.b $45
    inc.b $62
GbcSegNextDone:
    rts

; ---------------------------------------------------------------------------
; Build one run starting at the dirty block $50.
;
; COALESCENCE (contract sec. 6: never one DMA per block; the HYBRID rule of the
; A26 player).  The choice there was "one DMA over the whole extent" against
; "one DMA per dirty piece", i.e. bridging g clean blocks costs g*blocksize
; bytes while splitting costs one more !GBC_DMACOST.  Applied locally that is
; simply: bridge a gap while g*blocksize < DMACOST, which is a per-class
; constant -- 5 clean rows for a 64 B map row, 1 clean block for 256 B of BG
; chr, 0 for 512 B of OBJ chr.  Same decision, no second costing pass, and the
; run always begins and ends on a dirty block because the clean tail is trimmed
; off before it is sent.
; ---------------------------------------------------------------------------
GbcSegRun:
    lda.b $50
    sta.b $52
    stz.b $53
    stz.b $5A
    rep #$20
    lda.w #$0000
    sta.b $54
    sep #$20
    jsr GbcRunCeil      ; $56 = min(budget left, live capacity)

GbcRunAdd:
    rep #$20
    lda.b $54
    clc
    adc.b $46
    clc
    adc.w #!GBC_DMACOST
    cmp.b $56
    sep #$20
    bcs GbcRunEnd       ; one more block would not fit
    rep #$20
    lda.b $54
    clc
    adc.b $46
    sta.b $54
    sep #$20
    inc.b $53
    ; ⚡ WINDOW B CHARGES THE BUILD ITSELF.  Walking one block here costs
    ; about as much time as DMAing 64 bytes, and window B is short: a run sized
    ; against the capacity measured BEFORE the build is, for 64-byte map rows,
    ; a run the clock can no longer afford AFTER it (GbcRunTrim then shrinks it
    ; to nothing and the resume builds it all over again).  So in window B
    ; every block added also shrinks the ceiling by what its walk costs.
    ; Window A keeps the phase-4 rule untouched.
    lda.b $16
    lsr a
    bcc GbcRunAddCh
    rep #$20
    lda.b $56
    sec
    sbc.w #!GBC_BUILDEQ
    bcs GbcRunAddCeil
    lda.w #$0000
GbcRunAddCeil:
    sta.b $56
    sep #$20
GbcRunAddCh:

    ldx.b $62
    lda.b $00,x
    and.b $45
    beq GbcRunClean
    stz.b $5A
    bra GbcRunStep
GbcRunClean:
    inc.b $5A
    lda.b $5A
    cmp.b $4B
    beq GbcRunStep
    bcs GbcRunEnd       ; bridging further costs more than a second DMA
GbcRunStep:
    lda.b $50
    cmp.b $51
    beq GbcRunEnd
    jsr GbcSegNext
    bra GbcRunAdd

GbcRunEnd:
    lda.b $5A
    beq GbcRunTrim
GbcRunTail:
    lda.b $53
    beq GbcRunTrim
    dec.b $53
    rep #$20
    lda.b $54
    sec
    sbc.b $46
    sta.b $54
    sep #$20
    dec.b $5A
    bne GbcRunTail

GbcRunTrim:
    lda.b $53
    bne GbcRunTrimGo
    jmp GbcRunStop
GbcRunTrimGo:
    ; The run was sized against the capacity as it was BEFORE the build loop,
    ; and the loop itself burns scanlines.  Rather than let the guard reject
    ; the finished run -- which would send nothing at all, frame after frame,
    ; at exactly the workload the carry-over exists for -- shrink it to what
    ; the clock still allows and send that.
    jsr GbcCapacity
    sec
    sbc.w #!GBC_FIRESLACK
    bcs GbcRunTrimCeil
    lda.w #$0000
GbcRunTrimCeil:
    sta.b $56
    sep #$20
GbcRunTrimLoop:
    rep #$20
    lda.b $54
    clc
    adc.w #!GBC_DMASTART
    cmp.b $56
    sep #$20
    bcc GbcRunFireTry
    dec.b $53
    beq GbcRunStop
    rep #$20
    lda.b $54
    sec
    sbc.b $46
    sta.b $54
    sep #$20
    bra GbcRunTrimLoop

GbcRunFireTry:
    rep #$20
    lda.b $54
    sta.b $1C
    sep #$20
    jsr GbcDmaGuard
    bcs GbcRunFire
    jsr GbcDropCount
    dec.b $53
    beq GbcRunStop
    rep #$20
    lda.b $54
    sec
    sbc.b $46
    sta.b $54
    sep #$20
    bra GbcRunFireTry

GbcRunFire:
    jsr GbcClsDma
    jsr GbcClearRun
    lda.b $52
    clc
    adc.b $53
    sta.b $50           ; resume just past the run
    ldx.b $42
    cmp.b $44
    bcc GbcRunCurOk
    lda.b #$00
GbcRunCurOk:
    sta.b $00,x         ; and park the class cursor there for the next frame
    lda.b $50
    cmp.b $51
    beq GbcRunResume
    bcc GbcRunResume
    rts
GbcRunResume:
    lda.b $50
    jsr GbcBlockBit
    stx.b $62
    jmp GbcSegScan

GbcRunStop:
    lda.b #$01
    sta.b $17           ; out of budget or out of clock: the rest stays pending
    rts

; Program ch7 for the run and fire it.  Source offset and VRAM destination both
; derive from the SAME block number, so a run can never land on a byte the
; tilemap does not point at.  $4375 is rewritten for EVERY DMA: it counts down
; during the transfer, and a stale value means a 64 KB DMA over the whole of
; VRAM (the failure the SMS player documents).
GbcClsDma:
    rep #$20
    lda.b $52
    and.w #$00FF
    sta.b $58
    sep #$20
    lda.b $4F
    jsr GbcShl58        ; block -> VRAM words
    rep #$20
    lda.b $58
    clc
    adc.b $4C
    sta.b $64
    lda.b $52
    and.w #$00FF
    sta.b $58
    sep #$20
    lda.b $4E
    jsr GbcShl58        ; block -> source bytes
    rep #$20
    lda.b $58
    clc
    adc.b $48
    sta.b $5F
    sep #$20

    lda.b #$80
    sta.w $2115         ; VMAIN: +1 word after $2119
    rep #$20
    lda.b $64
    sta.w $2116
    sep #$20
    lda.b #$01
    sta.w $4370         ; ch7 DMAP: A->B, increment, mode 1 ($2118/$2119)
    lda.b #$18
    sta.w $4371
    rep #$20
    lda.b $5F
    sta.w $4372
    sep #$20
    lda.b $4A
    sta.w $4374
    rep #$20
    lda.b $54
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B         ; fire ch7 (every general DMA; ch0-6 are HDMA)
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.b $54
    jsr GbcDebit
    sep #$20
    rts

; Drop blocks $52..$52+$53-1 from the backlog.  The blocks are contiguous, so
; the mask is rolled instead of GbcBlockBit being called per block.
GbcClearRun:
    lda.b $52
    jsr GbcBlockBit
    lda.b $53
    sta.b $5A
GbcClearLoop:
    lda.b $45
    eor.b #$FF
    and.b $00,x
    sta.b $00,x
    dec.b $5A
    beq GbcClearDone
    asl.b $45
    bne GbcClearLoop
    lda.b #$01
    sta.b $45
    inx
    bra GbcClearLoop
GbcClearDone:
    rts

; A(8) = block -> X = DP address of its backlog byte, $45 = its bit mask.
GbcBlockBit:
    rep #$20
    and.w #$00FF
    pha
    and.w #$0007
    tax
    sep #$20
    lda.l GbcBitMask,x
    sta.b $45
    rep #$20
    pla
    lsr a
    lsr a
    lsr a
    clc
    adc.b $40
    tax
    sep #$20
    rts

GbcBitMask:
    db $01,$02,$04,$08,$10,$20,$40,$80

; $58-$59 <<= A(8).  Y is scratch here, which is why nothing in the drain keeps
; state in Y.  Enter and leave with A 8-bit.
GbcShl58:
    rep #$20
    and.w #$00FF
    tay
GbcShl58Loop:
    cpy.w #$0000
    beq GbcShl58Done
    asl.b $58
    dey
    bra GbcShl58Loop
GbcShl58Done:
    sep #$20
    rts

; Z = 1 when the loaded class has nothing pending.  ceil(N/8) bytes ORed
; together -- 4 or 6 of them, against the 32 or 48 per-block probes the scan
; would otherwise cost.  X is scratch (the caller reloads it from $42).
GbcClsEmpty:
    lda.b $44
    dec a
    lsr a
    lsr a
    lsr a
    inc a               ; bytes of bitmap = ceil(N / 8)
    sta.b $6C
    ldx.b $40
    lda.b #$00
GbcClsEmptyLoop:
    ora.b $00,x
    inx
    dec.b $6C
    bne GbcClsEmptyLoop
    cmp.b #$00          ; the dec/bne above clobbered the OR's flags
    rts

; A(8) = class -> the whole descriptor in $40-$4F.
GbcClsLoad:
    rep #$30
    and.w #$00FF
    asl a
    asl a
    asl a
    asl a
    tax
    lda.l GbcClsTab+$00,x
    sta.b $40
    lda.l GbcClsTab+$02,x
    sta.b $42
    lda.l GbcClsTab+$06,x
    sta.b $46
    lda.l GbcClsTab+$08,x
    sta.b $48
    lda.l GbcClsTab+$0C,x
    sta.b $4C
    sep #$20
    lda.l GbcClsTab+$04,x
    sta.b $44
    lda.l GbcClsTab+$05,x
    sta.b $4B
    lda.l GbcClsTab+$0A,x
    sta.b $4A
    lda.l GbcClsTab+$0E,x
    sta.b $4E
    lda.l GbcClsTab+$0F,x
    sta.b $4F
    rts

; ---------------------------------------------------------------------------
; The eleven classes.  16 bytes each so the index is a shift, not a multiply.
;
;   +$0 backlog DP base      +$8 source 16-bit base
;   +$2 cursor DP address    +$A source bank
;   +$4 block count          +$B (reserved)
;   +$5 clean blocks that    +$C VRAM word base
;       may be bridged       +$E block -> source byte shift
;   +$6 block size, bytes    +$F block -> VRAM word shift
;
; The block numbering is the bridge's, unchanged: DIRTY_CHR bit i is exactly
; byte i*256 of the BG chr view (bank 0 fills bits 0-23 = view $0000-$17FF,
; bank 1 fills 24-47 = $1800-$2FFF), and DIRTY_OBJ bit i is exactly byte i*512
; of the OBJ chr view (16 GB tiles = one 512 B SNES block, in both banks).  No
; permutation anywhere, which is what makes source and destination the same
; shift of the same block number.
;
; ⚡ WHAT A CLASS IS, AND WHY THERE ARE ELEVEN.  A class is the unit the generic
; run builder can coalesce over: N blocks contiguous in the view AND contiguous
; in VRAM.  Wire $02 kept every block numbered as the bridge numbers it and
; simply gave each contiguous PIECE its own row here, with the backlog DP base
; pointing at the byte its first block lives in -- so the chr backlog is still
; the same 48 bits at $20-$25 and four of the rows below index into it at $20,
; $23, $22 and $25.  The two DUPLICATE rows are the second copy of the $8800
; block that base B also names; they carry their own backlog byte, because a
; class clears the bits it sent and two rows sharing bits would race.
; ⚡ Every row has its OWN round-robin cursor ($74-$7E).  Sharing one between a
; duplicate and the class it duplicates looked free, since they carry the same
; bits, but the block counts differ and GbcDrainCls writes a clamped cursor
; back -- so the eight-block duplicate kept resetting the fairness phase of the
; sixteen-block class.
; ---------------------------------------------------------------------------
GbcClsTab:
; CHRA0: blocks 0..15, 16 x 256 B -> base A tiles 0..255, VRAM $4000 (word $2000)
    dw $0020, $0074
    db 16, 1
    dw 256, !GBC_BGCHR_A16+$0000
    db !GBC_BGCHR_A1B, 0
    dw $2000
    db 8, 7
; CHRA1: blocks 24..39 -> base A tiles 256..511, VRAM $5000 (word $2800)
    dw $0023, $0075
    db 16, 1
    dw 256, !GBC_BGCHR_A16+$1800
    db !GBC_BGCHR_A1B, 0
    dw $2800
    db 8, 7
; CHRB0: blocks 16..23 ($9000 b0) -> base B tiles 0..127, VRAM $6000 (word $3000)
    dw $0022, $0076
    db 8, 1
    dw 256, !GBC_BGCHR_A16+$1000
    db !GBC_BGCHR_A1B, 0
    dw $3000
    db 8, 7
; CHRD0: blocks 8..15 ($8800 b0) AGAIN -> base B tiles 128..255, VRAM $6800
    dw $003B, $0077
    db 8, 1
    dw 256, !GBC_BGCHR_A16+$0800
    db !GBC_BGCHR_A1B, 0
    dw $3400
    db 8, 7
; CHRB1: blocks 40..47 ($9000 b1) -> base B tiles 256..383, VRAM $7000
    dw $0025, $0078
    db 8, 1
    dw 256, !GBC_BGCHR_A16+$2800
    db !GBC_BGCHR_A1B, 0
    dw $3800
    db 8, 7
; CHRD1: blocks 32..39 ($8800 b1) AGAIN -> base B tiles 384..511, VRAM $7800
    dw $003C, $0079
    db 8, 1
    dw 256, !GBC_BGCHR_A16+$2000
    db !GBC_BGCHR_A1B, 0
    dw $3C00
    db 8, 7
; OBJ chr: 32 x 512 B -> VRAM bytes $0000.. (word $0000)
    dw $0026, $007A
    db 32, 0
    dw 512, !GBC_OBJCHR_A16
    db !GBC_OBJCHR_A1B, 0
    dw $0000
    db 9, 8
; Content of map $9800: DIRTY_MAP rows 0..31 -> VRAM $8000 (word $4000)
    dw $002A, $007B
    db 32, 5
    dw 64, !GBC_MAP_A16+$0000
    db !GBC_MAP_A1B, 0
    dw $4000
    db 6, 5
; Content of map $9C00: DIRTY_MAP rows 32..63 -> VRAM $8800 (word $4400)
    dw $002E, $007C
    db 32, 5
    dw 64, !GBC_MAP_A16+$0800
    db !GBC_MAP_A1B, 0
    dw $4400
    db 6, 5
; Carpet of map $9800: the SAME rows 0..31 -> VRAM $9000 (word $4800)
    dw $0032, $007D
    db 32, 5
    dw 64, !GBC_MAP_A16+$1000
    db !GBC_MAP_A1B, 0
    dw $4800
    db 6, 5
; Carpet of map $9C00: rows 32..63 -> VRAM $9800 (word $4C00)
    dw $0036, $007E
    db 32, 5
    dw 64, !GBC_MAP_A16+$1800
    db !GBC_MAP_A1B, 0
    dw $4C00
    db 6, 5

; ===========================================================================
; Budget and the overrun guard.
;
; $1E is the frame's byte-equivalent budget line: a MODEL, and an optimistic
; one.  GbcCapacity is the hard backstop -- it re-derives what is left from the
; LIVE scanline before every transfer, because the player's own per-DMA
; overhead is not in the model and the error would otherwise accumulate across
; a block-heavy frame.  Past line 41 the letterbox HDMA has handed the screen
; back and VRAM writes are dropped SILENTLY: the failure mode is corruption,
; not a glitch, so the engine gives transfers up rather than risk one.
; ===========================================================================
GbcRunCeil:
    jsr GbcCapacity
    cmp.b $1E
    bcc GbcRunCeilOk
    lda.b $1E
GbcRunCeilOk:
    sta.b $56
    ; ⚡ Window B sizes the run against what GbcRunTrim will allow AFTER the
    ; build, not before it: the FIRESLACK the trim subtracts comes off here
    ; too, and GbcRunAdd charges each block's walk (!GBC_BUILDEQ).  Without
    ; both, a run built to the start-of-build ceiling is trimmed one block at
    ; a time -- and each trim step costs more lines than the block it drops,
    ; so near V=225 the loop chases the deadline and sends nothing (measured on
    ; the clocked host model: a 17-row map run, 16 lines of build, zero bytes).
    sep #$20
    lda.b $16
    lsr a
    bcc GbcRunCeilOut
    rep #$20
    lda.b $56
    sec
    sbc.w #!GBC_FIRESLACK
    bcs GbcRunCeilB
    lda.w #$0000
GbcRunCeilB:
    sta.b $56
GbcRunCeilOut:
    sep #$20
    rts

; A(16) = bytes the DMA just fired moved.  Debits the budget and the counters.
; Enter and leave with A 16-bit.
GbcDebit:
    sta.b $58
    lda.b $1E
    sec
    sbc.b $58
    sec
    sbc.w #!GBC_DMACOST
    bcs GbcDebitOk
    lda.w #$0000
GbcDebitOk:
    sta.b $1E
    lda.l !GBC_CTR_DMAS
    inc a
    sta.l !GBC_CTR_DMAS
    lda.l !GBC_CTR_BYTES
    clc
    adc.b $58
    sta.l !GBC_CTR_BYTES
    lda.l !GBC_CTR_BYTES+2
    adc.w #$0000
    sta.l !GBC_CTR_BYTES+2
    rts

; $1C = bytes the next DMA would move.  Carry set = it can still finish before
; the deadline -- and, started in window A's vblank, before the HDMA init of
; line 0.  Enter A 8-bit, leave A 8-bit.
;
; ⚡ NO TRANSFER MAY STRADDLE V=0.  The HDMA initialises every enabled channel
; at the start of line 0, and when that init preempts a general DMA that is
; reading the bridge (a cartridge bank), the FIRST byte the init fetches -- the
; line count of ch0's entry 0, ch0 being first in the init order -- can come
; back wrong.  Measured on a Mk.III (Super Mario Bros. Deluxe, whose HUD split
; is a SCY write at GB line 8): about one frame in 44, ch0 was still on its
; entry 0 at V=84 with a line counter of 39, i.e. it had loaded ~127 lines
; instead of 48, while the table in WRAM read back correct and ch5 had
; initialised normally.  The scroll pair never reached the split and the whole
; frame showed the HUD's SCY.  The capacity counts the vblank AND the tail, so
; a run accepted near V=261 used to run straight through the init; which frames
; it hit depended on where the drain stood at V=0, so any change of timing
; anywhere moved the beat (the C6 player's extra cycles made it visible).  A
; transfer that fits the budget but would still be on the bus at V=0 now waits
; for the init to pass (GbcWaitInitW) and is weighed again against the tail;
; one that no longer fits is refused like any other, and the run builder trims.
GbcDmaGuard:
    jsr GbcGuardFit
    bcc GbcGuardOut     ; over the budget, or past the deadline
    jsr GbcGuardV0
    bcs GbcGuardOut     ; it ends before V=0, or V=0 is behind us
    jsl GbcWaitInitW    ; it would straddle the init: let it pass first
    bcc GbcGuardHold    ; V never moved (the host pins it): as before
    bra GbcGuardFit     ; and ask again, now against the tail
GbcGuardHold:
    sec
GbcGuardOut:
    rts

GbcGuardFit:
    jsr GbcCapacity
    sta.b $1A
    lda.b $1C
    clc
    adc.w #!GBC_DMASTART ; clock question: the player-side overhead around this
    cmp.b $1A            ; transfer is already inside !GBC_GUARDSLACK
    sep #$20
    bcs GbcGuardNo
    sec
    rts
GbcGuardNo:
    clc
    rts

; Carry set = the transfer of $1C bytes is clear of V=0: window B (whose
; capacity ends at V=225), any line before the vblank, or a vblank with room
; for the transfer plus the guard's own slack before line 262.  A 8-bit in and
; out; $1A is clobbered.
GbcGuardV0:
    lda.b $16
    lsr a               ; b0: window B is running
    bcs GbcGv0Ok
    jsr GbcVCount       ; A(16) = V
    cmp.w #!GBC_VBLANK
    bcc GbcGv0Ok16      ; the init of this frame is behind us
    sta.b $1A
    lda.w #!GBC_LINES
    sec
    sbc.b $1A           ; whole vblank lines left before V=0
    bcc GbcGv0No16      ; a PAL line past 261: not before its own V=0
    beq GbcGv0No16
    sep #$20
    sta.w $4202
    lda.b #!GBC_BPLVBL
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.b $1C
    clc
    adc.w #!GBC_DMASTART+!GBC_GUARDSLACK
    cmp.w $4216
    bcs GbcGv0No16      ; would still be on the bus at the init
GbcGv0Ok16:
    sep #$20
GbcGv0Ok:
    sec
    rts
GbcGv0No16:
    sep #$20
    clc
    rts

; GbcWaitInitW -- wait until the HDMA init of line 0 is over.  Carry set =
; done (A 8-bit, X/Y as they came), carry clear = the bound ran out (a V that
; never moves).  It runs FROM WRAM: the CPU is exactly what must not be on the
; cartridge bus when the init fetches ch0's first byte (see GbcDmaGuard), so the
; loop fetches its own opcodes from WRAM and reads nothing but $4212, a CPU
; register -- no cartridge, no B-bus.  Vblank flag down = V=0 has begun; the
; init runs a few dots into it, so the loop then waits for the hblank flag to
; drop (the one carried over from line 261) and to rise again at the end of
; line 0.  Copied to !GBC_WAITW at boot; reached with jsl.
GbcWaitInitWSrc:
base !GBC_WAITW
GbcWaitInitW:
    php
    sep #$20
    rep #$10
    phx
    ldx.w #!GBC_V0SPIN
GbcWiwVbl:
    lda.w $4212
    bpl GbcWiwH0        ; bit 7 low: the vblank is over, V=0 has begun
    dex
    bne GbcWiwVbl
    bra GbcWiwFail
GbcWiwH0:
    lda.w $4212
    and.b #$40
    beq GbcWiwH1        ; the hblank carried over from line 261 is over
    dex
    bne GbcWiwH0
    bra GbcWiwFail
GbcWiwH1:
    lda.w $4212
    and.b #$40
    bne GbcWiwDone      ; line 0's own hblank: the init is behind us
    dex
    bne GbcWiwH1
GbcWiwFail:
    plx
    plp
    clc
    rtl
GbcWiwDone:
    plx
    plp
    sec
    rtl
base off
GbcWaitInitWEnd:
assert !GBC_STATCOPY_OFF+!GBC_STATCOPY_LEN <= (!GBC_WAITW&$FFFF), "GbcWaitInitW overlaps the status copy"
assert (!GBC_WAITW&$FFFF)+(GbcWaitInitWEnd-GbcWaitInitWSrc) <= !GBC_FB_OFF, "GbcWaitInitW overlaps the FB block"

GbcDropCount:
    sep #$20
    lda.b $16
    lsr a
    bcs GbcDropPause    ; window B running out is a pause (see TWO WINDOWS)
    rep #$20
    lda.l !GBC_CTR_DROPS
    inc a
    sta.l !GBC_CTR_DROPS
    sep #$20
    lda.b #$01
    sta.b $3F
GbcDropPause:
    rts

; Byte-equivalents that still fit before the deadline, from the LIVE scanline.
; Enter with A 8-bit, leave with A 16-bit holding the capacity (0 = past it):
; !GBC_BPLVBL for every whole vblank line still left, plus the 40-line tail at
; $70 bytes a line, where the armed HDMA channels take their cut out of every
; one of them.  $70 and $71 are rewritten by the prologue from the number of
; channels the published set arms (see !GBC_BPLHDMA); they are not constants
; any more, because the raster compiler can put four more channels on the bus.
GbcCapacity:
    jsr GbcVCount       ; A(16) = scanline, 9 valid bits (also in $1A)
    sep #$20
    lda.b $16
    lsr a               ; b0: window B is running
    rep #$20
    bcs GbcCapWinB
    lda.b $1A
    cmp.w #225
    bcs GbcCapVbl
    cmp.w !GBC_FB_DL    ; !GBC_DEADLINE, or 16 with the second stage on
    bcs GbcCapNone      ; between the deadline and the next vblank: too late
    sta.b $1A
    lda.w !GBC_FB_DL
    sec
    sbc.b $1A
    sep #$20
    sta.w $4202
    lda.b !GBC_DP_HDMA
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.w $4216
    bra GbcCapTrim
GbcCapVbl:
    sta.b $1A
    lda.w #!GBC_LINES
    sec
    sbc.b $1A           ; whole vblank lines left
    bcs GbcCapVblOk     ; V past 261 -- a PAL console (312 lines), overscan or
    lda.w #$0000        ; a stale H/V latch.  The subtract borrowed, and only
GbcCapVblOk:            ; the LOW BYTE reaches $4202: 262-263 = $FFFF would be
                        ; multiplied as 255 lines and hand the guard a capacity
                        ; of ~45000, i.e. a blank cheque to overrun the screen.
                        ; Claim zero vblank lines instead; the tail below still
                        ; lets small transfers through.
    sep #$20
    sta.w $4202
    lda.b #!GBC_BPLVBL
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.w $4216
    clc
    adc.b !GBC_DP_HDMA+1
    bra GbcCapTrim
; ⚡ Window B (phase 5): the lines left before V=225 and nothing else, at $70
; bytes a line -- the bottom letterbox runs the SAME channels the frame just
; showed, and $70 is theirs until the prologue publishes the next set.  Not the
; vblank, not the tail: a DMA still on the bus at V=225 holds the CPU and
; delays the NMI, i.e. the SYNC strobe the genlock measures its phase against.
; Outside [!GBC_VIRQ_LINE, 225) it is zero, which the walk reads as "pause".
GbcCapWinB:
    lda.b $1A
    cmp.w !GBC_FB_VL    ; !GBC_VIRQ_LINE, or 209 with the second stage on
    bcc GbcCapNone
    cmp.w #!GBC_VBLANK
    bcs GbcCapNone
    lda.w #!GBC_VBLANK
    sec
    sbc.b $1A           ; whole lines left before the NMI
    sep #$20
    sta.w $4202
    lda.b !GBC_DP_HDMA
    sta.w $4203
    nop
    nop
    nop
    nop
    rep #$20
    lda.w $4216
    bra GbcCapTrim
GbcCapNone:
    lda.w #$0000
GbcCapTrim:
    sec
    sbc.w #!GBC_GUARDSLACK
    bcs GbcCapDone
    lda.w #$0000        ; borrowed: nothing left at all
GbcCapDone:
    rts

; Current scanline -> A (16 bits).  $2137 latches H/V; $213D returns the low
; byte on the first read and bit 8 on the second, so $213F is read first to put
; that toggle in a known state.  $1A/$1B are adjacent so the 16-bit load at the
; end returns the whole 9-bit value -- an 8-bit read would fold V=256..261 onto
; V=0..5 and hand a mid-screen transfer a full window's worth of capacity.
GbcVCount:
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
; Copy the useful head of the status block into WRAM.
;
; Carry set = it landed.  A skipped fetch is not survivable in silence: the
; COMMIT that preceded it already cleared the bridge's accumulator, so the
; caller promotes the next fold to a full refresh.
; ===========================================================================
GbcStatusFetch:
    rep #$20
    lda.w #!GBC_STATCOPY_LEN
    sta.b $1C
    sep #$20
    jsr GbcDmaGuard
    bcc GbcStatusSkip
    lda.b #(!GBC_STATCOPY_OFF&$FF)
    sta.w $2181
    lda.b #(!GBC_STATCOPY_OFF>>8)
    sta.w $2182
    stz.w $2183         ; WRAM port -> $7E:1F00 (bit 0 = bank select)

    lda.b #$00
    sta.w $4370         ; ch7 DMAP: A->B, increment, mode 0
    lda.b #$80
    sta.w $4371         ; BBAD = $2180 (WMDATA)
    ldx.w #!GBC_STAT_A16
    stx.w $4372
    lda.b #!GBC_STAT_A1B
    sta.w $4374
    rep #$20
    lda.b $1C
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.b $1C
    jsr GbcDebit
    sep #$20
    sec
    rts
GbcStatusSkip:
    jsr GbcDropCount
    clc
    rts

; ===========================================================================
; Release the screen, once.
;
; The letterbox HDMA is what turns the picture on at line 41, so arming it is
; the same decision as "is there a picture yet".  The bar is CGRAM + OAM + all
; four map views drained at least once: with those in place the carpets paint
; the GB's colour 0 across the viewport and any chr still missing reads as
; pixel 0, i.e. transparent, i.e. that same colour -- a legible partial image
; that fills in.  Arming with a half-built CGRAM instead would flash.
;
; ⚠ This only DECIDES; the $420C write itself is owed to the next prologue
; ($06 = 1 -> armed there, $06 = 2).  Enabling a channel after the V=0 HDMA
; init has passed leaves its line counter unset for the rest of the frame, and
; the decision is by construction reached at the END of a frame body -- which,
; for any frame that actually transferred something, is well past V=0.  Testing
; "am I still in vblank?" here instead was the first shape of this routine and
; it deadlocked outright: the body ended at V=48 (bsnes-plus, measured), the
; test was false in every frame, and the screen never left forced blank.  A
; busy game would have kept it false even after the idle body was made cheap,
; so the fix is not a faster body -- it is arming where the contract says to
; (sec. 13.11: $420C only in the prologue).
; ===========================================================================
GbcReadyCheck:
    lda.b $06
    bne GbcReadyDone            ; already decided
    lda.b $6D
    cmp.b #!GBC_READY_MAX_FRAMES
    bcs GbcReadyCounted         ; hold at the ceiling: this is a frame count,
    inc.b $6D                   ; not a wrapping counter
GbcReadyCounted:
    lda.b $3A
    bne GbcReadyLate
    ; ⚡ No special case for the LCD-off screen any more.  Wire $01 needed one
    ; -- the white map was painted over the carpet map's VRAM, so the carpet
    ; class was held back and its backlog never emptied -- and here the white
    ; map has its own region, so both map classes drain normally whether the
    ; LCD is on or off and the loop below converges by itself.
    ldx.w #$002A
GbcReadyMapLoop:
    lda.b $00,x
    bne GbcReadyLate
    inx
    cpx.w #$003A
    bne GbcReadyMapLoop
GbcReadyArm:
    lda.b #$01
    sta.b $06           ; owed: the NEXT prologue arms it
    rts

; Nothing has converged.  Release anyway once the net expires -- see
; !GBC_READY_MAX_FRAMES for why a permanently busy game must not stay black.
GbcReadyLate:
    lda.b $6D
    cmp.b #!GBC_READY_MAX_FRAMES
    bcc GbcReadyDone
    lda.b #$01
    sta.b $06
    sta.l !GBC_CTR_READYTO      ; armed by the net, not by convergence
GbcReadyDone:
    rts

; ===========================================================================
; IGR pad combos (chassis parity).  The NMI hook that feeds cheat.v's combo
; detector never runs under a player, so the match lives here.  Exact 16-bit
; compare of {$4219,$4218} as one word, same values as cheat.v's case():
;   $3030 = L+R+Start+Select -> $80 (reset the game)
;   $2070 = L+R+Select+X     -> $81 (back to the menu)
; What keeps these from colliding with gameplay is L+R: the GB pad translation
; (contract sec. 9) maps only A|X, B|Y, Select, Start and the d-pad, so neither
; shoulder button has a GB equivalent and no game can ask for both of them.
; Select/Start DO reach the GB and are forwarded above as usual -- harmless,
; since a combo match means we are leaving this frame.  The command byte goes
; to the fork's MCU_CMD mailbox ($2A00, snescmd BRAM -- write window opened for
; this core in main.v) and the MCU game loop serves it.  DP $04 = edge latch:
; fire only on release->press (armed at Reset).
;
; The operand is the pad word the NMI already forwarded, so the $4212 guard
; above covers this read too.
; ===========================================================================
GbcIgr:
    rep #$20
    lda.b $0E
    cmp.w #$3030
    beq GbcIgrReset
    cmp.w #$2070
    beq GbcIgrMenu
    sep #$20
    stz.b $04           ; no combo held -> re-arm the edge
    rts
GbcIgrReset:
    sep #$20
    lda.b #$80
    bra GbcIgrFire
GbcIgrMenu:
    sep #$20
    lda.b #$81
GbcIgrFire:
    sta.b $19           ; park the command
    lda.b $04
    bne GbcIgrDone      ; already fired during this hold
    lda.b #$01
    sta.b $04
    jsr GbcExitHygiene  ; hand the $21xx block back the way iris_out gives it
    lda.b $19
    sta.l $002A00       ; MCU_CMD
GbcIgrDone:
    rts

; Leaving for the menu / a game reset: the menu inherits this $21xx block, so
; put it back the way iris_out is expected to hand it over -- forced blank, no
; HDMA, no NMI, nothing armed.
GbcExitHygiene:
    lda.b #$8F
    sta.w $2100
    stz.w $420C
    pha
    lda.b #$00
    sta.l !GBC_HDMAEN_SH
    pla
    stz.w $4200         ; the V-IRQ goes with the NMI
    lda.w $4211         ; and a TIMEUP already latched is dropped
    rts

; ===========================================================================
; GbcGo -- release the GB's reset (contract sec. 10).
;
; Idempotent by contract, and deliberately NOT guarded by a watchdog: an
; auto-GO would mask exactly the bringup failure it would be papering over.
; Writing $00 here instead re-asserts the reset AND re-arms BOOTROM_ACTIVE,
; i.e. a full GB reset.
; ===========================================================================
GbcGo:
    sep #$20
    lda.b #$01
    ldx.w #!GBC_EFO_GO
    jsr GbcEfPut
    rts

; ===========================================================================
; GbcLifeWait -- wait for the bridge to publish our wire version.
;
; Returns carry CLEAR when status.ver == !GBC_VER.  Carry SET means either
; $00 (no bridge behind the view banks: an old or wrong .bi3, or no core at
; all) or a version this player does not speak -- both of which end the boot
; in forced blank rather than in a screen drawn out of undefined reads
; (contract sec. 10).  Nothing is pumped while waiting: ver is a constant of
; the bridge, published whether or not the GB has been released.
; ===========================================================================
GbcLifeWait:
    sep #$20
    rep #$10
    ldx.w #!GBC_LIFEWAIT
GbcLifeLoop:
    lda.l !GBC_STAT_LONG+!GBC_ST_VER
    beq GbcLifeTick     ; $00 = nothing answering (yet) -- keep waiting
    cmp.b #!GBC_VER
    beq GbcLifeOk
    sec                 ; a bridge answered in a version this player does not
    rts                 ; speak: a definitive mismatch, not a slow boot, so
                        ; there is nothing to wait for
GbcLifeTick:
    jsr GbcFrameWait
    dex
    bne GbcLifeLoop
    sec
    rts
GbcLifeOk:
    clc
    rts

; Wait one full frame on the vblank flag (the NMI is still off up here).
GbcFrameWait:
GbcFrameWaitOut:
    lda.w $4212
    bmi GbcFrameWaitOut ; leave the vblank we may be sitting in
GbcFrameWaitIn:
    lda.w $4212
    bpl GbcFrameWaitIn  ; and wait for the next one to start
    rts

; Zero the diagnostic counters (WRAM comes up as whatever the menu left).
GbcCtrClear:
    rep #$30
    ldx.w #$0000
    lda.w #$0000
GbcCtrLoop:
    sta.l !GBC_CTR,x
    inx
    inx
    cpx.w #!GBC_CTR_LEN
    bne GbcCtrLoop
    sep #$20
    rts

; ===========================================================================
; GbcInit -- the complete video bring-up (contract sec. 11.2/11.3).
;
; Entry: native mode, DBR = $00, DP = $0000, A 8-bit, X/Y 16-bit, interrupts
; off.  Leaves the screen in forced blank; the caller decides whether the
; bridge earned the right to have it released.
; ===========================================================================
GbcInit:
    sep #$20
    rep #$10

    lda.b #$8F
    sta.w $2100         ; forced blank for the whole bring-up
    stz.w $4200         ; no NMI, no auto-joypad yet
    stz.w $420C         ; no HDMA yet: the tables are still garbage
    pha
    lda.b #$00
    sta.l !GBC_HDMAEN_SH
    sta.l !GBC_WHITE_CNT
    sta.l !GBC_WHITE_FIRST
    pla
    stz.w $420B
    stz.w $4300         ; ch0 = the decoy: mode 0, direct, A->B
    lda.b #$FF
    sta.w $4301         ; BBAD $FF = $21FF, a B-bus address nothing decodes
    rep #$20
    lda.w #GbcTblDecoy
    sta.w $4302
    sep #$20
    stz.w $4304         ; this ROM, bank $00
    lda.b #$FF
    sta.w $4201         ; WRIO high.  Reading $2137 latches the H/V counters
                        ; ONLY while $4201 bit 7 is set, and the DMA guard is a
                        ; function of that latch: with the pin low $213D would
                        ; return one frozen scanline forever.  The console reset
                        ; leaves $FF here; the guard is too load-bearing to
                        ; inherit it rather than state it.

    ; --- 1) wipe the inherited $21xx block ---------------------------------
    ; The menu hands this block over as it last used it and the console reset
    ; does not clear it, so every register is stated below.  The sweep first,
    ; so anything NOT restated afterwards is known to be zero rather than
    ; whatever the menu was doing; the write-only data ports it touches
    ; ($2104/$2118/$2119/$2122) are all re-initialised further down anyway.
    ldx.w #$2101
GbcInitZero:
    stz.w $0000,x
    inx
    cpx.w #$2134
    bne GbcInitZero

    ; --- 2) layers, bases and the two view geometries ----------------------
    ; Mode 0, four 2bpp layers: BG1 = GB window, BG2 = GB background,
    ; BG3 = window carpet (priority 1), BG4 = background carpet (priority 0),
    ; OBJ 8x8 only.  Bit 3 of $2105 stays 0 (contract sec. 13.7): BG3 must not
    ; be promoted, the whole priority table depends on it.
    stz.w $2105         ; BGMODE 0, no 16x16 tiles, BG3 priority off
    stz.w $2106         ; mosaic off (an inherited mosaic deforms everything)
    stz.w $2101         ; OBSEL: OBJ chr base word $0000, name select 0, 8x8
    stz.w $2102
    stz.w $2103         ; OAM address 0; bit 7 = 0 -> no priority rotation, which
                        ; is what makes "first 32 sprites of the line" follow OAM
                        ; order (contract sec. 12.1)
    ; ⚡ The four map bases and the BG chr base are per-FRAME state now
    ; (GbcRegs writes them out of the snapshot's LCDC every frame), so what is
    ; stated here is only the consistent starting point the first frame
    ; overwrites: the Game Boy's map $9800 for both layers, chr base A.
    lda.b #$40
    sta.w $2107         ; BG1SC: window content = map $9800, base word $4000
    lda.b #$40
    sta.w $2108         ; BG2SC: BG content     = map $9800, base word $4000
    lda.b #$48
    sta.w $2109         ; BG3SC: window carpet  = map $9800, base word $4800
    lda.b #$48
    sta.w $210A         ; BG4SC: BG carpet      = map $9800, base word $4800
    lda.b #$22
    sta.w $210B         ; BG12NBA: chr base A, word $2000 (= byte $4000)
    lda.b #$44
    sta.w $210C         ; BG34NBA: the carpets read their own pool at word
                        ; $4000 (= byte $8000), where tile 768 falls on byte
                        ; $B000 -- free VRAM past both chr bases and both map
                        ; regions.  They cannot share the BG pool any more: it
                        ; is only 512 tiles wide now and 768 is outside it
    lda.b #$80
    sta.w $2115         ; VMAIN: +1 word after $2119

    ; scroll: every register here is write-twice, so the sweep above only got
    ; half of each one.
    ldx.w #$210D
GbcInitScroll:
    stz.w $0000,x
    stz.w $0000,x
    inx
    cpx.w #$2115
    bne GbcInitScroll

    ; --- 3) windows (contract sec. 11.3) -----------------------------------
    ; W2 = the 160-pixel viewport (X 48..207); W1 is driven per line by HDMA
    ; and carries the GB's window layer.  With every layer masked OUTSIDE the
    ; viewport and no colour math, what shows there is the backdrop, CGRAM[0].
    lda.b #$EF
    sta.w $2123         ; BG1: W1-OUT+EN, W2-OUT+EN / BG2: W1-IN+EN, W2-OUT+EN
    lda.b #$EF
    sta.w $2124         ; BG3/BG4 the same pairing
    lda.b #$0C
    sta.w $2125         ; OBJ: W2-OUT+EN only; the colour window stays off
    lda.b #$FF
    sta.w $2126         ; W1 left  \ seeded EMPTY (left > right).  HDMA ch4 owns
    stz.w $2127         ; W1 right / this pair from line 1 on; the seed only
                        ; decides what line 0 -- which belongs to no HDMA entry
                        ; -- would look like, and keeps the register state
                        ; consistent with the table built below.
    lda.b #48
    sta.w $2128         ; W2 left
    lda.b #207
    sta.w $2129         ; W2 right
    stz.w $212A         ; BG window mask logic: OR
    stz.w $212B         ; OBJ/colour window mask logic: OR
    lda.b #$1F
    sta.w $212E         ; window masking on for BG1-4 + OBJ, main screen
    stz.w $212F         ; nothing masked on the subscreen (nothing is on it)

    ; --- 4) screens and colour math ----------------------------------------
    lda.b #$1F
    sta.w $212C         ; TM: BG1-4 + OBJ
    stz.w $212D         ; TS: subscreen empty
    stz.w $2130         ; no colour math, no clip-to-black: with every layer
    stz.w $2131         ; masked outside the viewport the backdrop is enough
    lda.b #$E0
    sta.w $2132         ; fixed colour = black (B/G/R selected, value 0)
    stz.w $2133         ; 224 lines: no overscan, no interlace, no pseudo-hires

    jsr GbcClearVram
    jsr GbcSolidTilesUp
    jsr GbcWhiteMapUp   ; ⚡ the LCD-off map, once, into free VRAM
    jsr GbcClearCgram
    jsr GbcClearOam
    jsr GbcHdmaBuild
    jsr GbcApuUnmute    ; open the console's cart-DAC audio path
    rts

; Zero all 64 KB of VRAM.  Before the first drain lands, every tile has to read
; back as pixel 0 (= transparent = backdrop = black) instead of as whatever the
; menu left in there.  Fixed A-bus source, ch7 (the general DMA channel; ch0-6
; belong to the HDMA).  ~24 ms, all of it in forced blank.
GbcClearVram:
    lda.b #$80
    sta.w $2115         ; VMAIN: +1 word after $2119
    ldx.w #$0000
    stx.w $2116
    lda.b #$09
    sta.w $4370         ; ch7 DMAP: A->B, FIXED source, mode 1 ($2118/$2119)
    lda.b #$18
    sta.w $4371
    ldx.w #GbcZero
    stx.w $4372
    lda.b #$00
    sta.w $4374         ; this ROM lives in bank $00
    ldx.w #$0000
    stx.w $4375         ; 0 = 65536 bytes
    lda.b #$80
    sta.w $420B         ; fire ch7
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rts

GbcZero:
    dw $0000

; The two solid tiles the carpet layers are made of (contract sec. 4.1/13.10):
; tile 768 = every pixel colour 1, tile 769 = every pixel colour 2.  They are
; the only tiles the player writes itself -- the view never emits them.
; ⚡ wire $02 gave the carpets a chr base of their own ($210C = $44, byte
; $8000), so tile 768 lands at byte $B000: past both BG chr bases and past the
; four map regions, in VRAM nothing else uses.
GbcSolidTilesUp:
    lda.b #$80
    sta.w $2115
    ldx.w #$5800        ; word address of byte $B000
    stx.w $2116
    lda.b #$01
    sta.w $4370         ; ch7 DMAP: A->B, increment, mode 1 ($2118/$2119)
    lda.b #$18
    sta.w $4371
    ldx.w #GbcSolidTileData
    stx.w $4372
    lda.b #$00
    sta.w $4374
    ldx.w #$0020        ; two 2bpp tiles
    stx.w $4375
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rts

; 2bpp, one word per row: low byte = bitplane 0, high byte = bitplane 1.
GbcSolidTileData:
    dw $00FF, $00FF, $00FF, $00FF, $00FF, $00FF, $00FF, $00FF   ; tile 768, col 1
    dw $FF00, $FF00, $FF00, $FF00, $FF00, $FF00, $FF00, $FF00   ; tile 769, col 2

; Zero all 256 CGRAM entries, then plant the one fixed colour the contract
; reserves (sec. 4.4/13.7): entry 98 = $7FFF, the white BG4 pal-0 colour 2 that
; the LCD-off / first-frame screen is painted with.  Entry 0 = $0000 (the
; backdrop, i.e. the letterbox and the pillars) is already satisfied by the
; wipe.  From the first CGRAM drain on, the view carries both of them itself.
GbcClearCgram:
    stz.w $2121         ; CGADD = 0
    ldx.w #$0100
GbcClearCgramLoop:
    stz.w $2122
    stz.w $2122
    dex
    bne GbcClearCgramLoop
    lda.b #98
    sta.w $2121
    lda.b #$FF
    sta.w $2122
    lda.b #$7F
    sta.w $2122
    rts

; Park every sprite off-screen and state the high table once.
;
; Y = $F0 with 8x8 objects covers lines 241..248 -- past the 224 visible ones
; and without wrapping -- which is exactly why "OBJ only 8x8" is an invariant
; (contract sec. 4.5/13.9): the same Y on a 16x16 object would show its top
; rows again at the bottom of the screen.  The high table is 32 bytes of $00
; (X bit 8 = 0, size = small) and is written HERE and nowhere else; the OAM
; view only ever supplies the low table.
GbcClearOam:
    stz.w $2102
    stz.w $2103         ; OAM word address 0, priority rotation off
    ldx.w #$0080        ; 128 sprites
GbcClearOamLoop:
    stz.w $2104         ; X = 0
    lda.b #$F0
    sta.w $2104         ; Y = $F0
    stz.w $2104         ; tile = 0
    stz.w $2104         ; attr = 0
    dex
    bne GbcClearOamLoop
    ldx.w #$0020        ; 32 bytes of high table
GbcClearOamHi:
    stz.w $2104
    dex
    bne GbcClearOamHi
    rts

; ===========================================================================
; HDMA tables.
;
; The letterbox table is static, so it is copied into BOTH sets at boot and
; never touched again; the window-1 table is compiled per frame by GbcW1Frame
; into the set the HDMA is NOT reading, and both channels' pointers are
; rewritten in the NMI prologue (contract sec. 11.4).  Both sets live inside
; bank $7E, because the A-bus of an HDMA wraps at $FFFF and a table that
; straddled a bank would read garbage for its tail.
;
; Format: {count, data...}; count 1..127 repeats the same data for `count`
; lines (hold: it costs only the header), count | $80 means `count` lines each
; with their own data; $00 terminates.  Table entry i drives the VISIBLE line
; i+1, so entry 0 is line 1 and entry 223 is line 224.
; ===========================================================================
GbcHdmaBuild:
    ldx.w #$0000
GbcHdmaCopyIniA:
    lda.l GbcTblInidisp,x
    sta.l !GBC_WRAM_LONG+!GBC_SETA_OFF+!GBC_T_INIDISP,x
    sta.l !GBC_WRAM_LONG+!GBC_SETB_OFF+!GBC_T_INIDISP,x
    inx
    cpx.w #(GbcTblInidispEnd-GbcTblInidisp)
    bne GbcHdmaCopyIniA
    ldx.w #$0000
GbcHdmaCopyW1:
    lda.l GbcTblW1,x
    sta.l !GBC_WRAM_LONG+!GBC_SETA_OFF+!GBC_T_W1,x
    sta.l !GBC_WRAM_LONG+!GBC_SETB_OFF+!GBC_T_W1,x
    inx
    cpx.w #(GbcTblW1End-GbcTblW1)
    bne GbcHdmaCopyW1

    ; The set headers: ch4 + ch5 armed, ch0..ch3 disarmed.  A set whose header
    ; the compiler has not written yet still has to be publishable, because the
    ; prologue reads it every frame from the very first one.
    ldx.w #$0000
GbcHdmaCopyHdr:
    lda.l GbcHdrSeed,x
    sta.l !GBC_WRAM_LONG+!GBC_SETA_OFF+!GBC_H_MASK,x
    sta.l !GBC_WRAM_LONG+!GBC_SETB_OFF+!GBC_H_MASK,x
    inx
    cpx.w #(GbcHdrSeedEnd-GbcHdrSeed)
    bne GbcHdmaCopyHdr

    ; --- ch5: $2100, the letterbox (fixed channel, contract sec. 11.4) -----
    stz.w $4350         ; DMAP: A->B, direct table, mode 0 (one register)
    stz.w $4351         ; BBAD = $2100 (INIDISP)

    ; --- ch4: $2126/$2127, window 1 (fixed channel) ------------------------
    lda.b #$01
    sta.w $4340         ; DMAP: A->B, direct table, mode 1 (two registers)
    lda.b #$26
    sta.w $4341         ; BBAD = $2126 (WH0), so $2127 (WH1) gets the second byte

    ; The A1T/A1B of every channel, and the role of ch0..ch3, belong to
    ; GbcHdmaPublish: they are frame state, and the contract allows them to
    ; move only in the prologue.
    rts

; !GBC_H_MASK then four {bbad, offset lo, offset hi, 0} entries.
GbcHdrSeed:
    db $30, $31, $00, $00        ; logical ch4 + ch5; physical + the ch0 decoy
    db $00, $00, $00, $00
    db $00, $00, $00, $00
    db $00, $00, $00, $00
    db $00, $00, $00, $00
GbcHdrSeedEnd:

; Letterbox: the GB's 144 lines sit on visible lines 41..184; everything above
; and below is forced blank so nothing of the SNES frame shows through and the
; sprites parked at Y = $F0 stay invisible.
;
; ⚠ LINE 40 IS $00 -- screen ON at brightness 0 -- AND NOT $80.  The PPU
; evaluates the sprites of line N during line N-1 and fetches the first BG
; tiles of line N in the hblank of N-1; in FORCED blank it does neither.  With
; line 40 blanked, the GB's very first line (V=41) came up with no sprites on
; it at all.  Brightness 0 is black on the screen but the PPU is rendering, so
; the evaluation and the fetch happen.  The bottom letterbox stays $80: there
; is no line 225 to prepare.
;
; That is also why the transfer deadline is line 40 and not 41 (!GBC_DEADLINE):
; the screen comes back on at the START of line 40, so VRAM is ours only
; through line 39.  It costs one line, ~170 byte-equivalents of window A.
GbcTblInidisp:
    db 39,  $80         ; lines 1..39    forced blank
    db 1,   $00         ; line 40        screen ON, brightness 0 -- see below
    db 127, $0F         ; lines 41..167  screen on, full brightness
    db 17,  $0F         ; lines 168..184 (a run is capped at 127 lines)
    db 40,  $80         ; lines 185..224 forced blank
    db $00              ; terminator
GbcTblInidispEnd:

; Boot seed for window 1: EMPTY everywhere (left = 255, right = 0).  With
; W1-OUT selected for BG1/BG3 the whole line reads as "outside", so the two
; window layers are masked away until the first compiled table lands.
GbcTblW1:
    db 127, 255, 0      ; lines 1..127
    db 97,  255, 0      ; lines 128..224
    db $00              ; terminator
GbcTblW1End:

; ===========================================================================
; THE FRAMEBUFFER MODE (C6, wire $03) -- contract sec. 14, C6-SPEC.md sec. b.
;
; WHAT IT IS.  For the scenes the raster compiler cannot follow (hi-colour:
; a palette rewritten every line, 512-4096 CGRAM writes a frame, more colours
; per line than four HDMA channels carry) the FPGA composes the Game Boy's
; picture itself -- BG, window and sprites with the CGB priority, the CRAM
; read live -- and writes it, quantised to the SNES's direct-colour byte
; (Book I A-17: BBGGGRRR), into a 160x144x8 framebuffer that the bridge
; serves as 8bpp tiles in bank $E4.  This side then shows that framebuffer in
; Mode 3 (BG1 8bpp, CG direct select) and uploads only the 8x8 cells that
; changed.  Scene by scene: the player decides to enter and to leave from the
; pressure the status block reports (GbcFbPolicy), and everything the
; quad-layer does (fold, backlog, OAM, CGRAM) keeps running underneath so the
; way back is seamless.
;
; STATES (!GBC_FB_STATE, published in the mailbox as C6MODE.lo):
;   0 OFF       the v1.6 player, byte for byte
;   1 ENTERING  FB_EN written; the 18 rows of cells are uploaded UNDER the
;               quad-layer (VRAM the quad-layer never displays with the LCD
;               on: the FB tilemap at $A000 and its tiles from $A480 up,
;               skipping $B000-$B03F where the carpets' solid tiles live) --
;               no black frame, the cut-over is one prologue (GbcFbCut)
;   2 ON        Mode 3 on the FB; GbcRegs and the compiler are idle; the
;               drain serves the FB first, then OAM/CGRAM/blocks with what
;               is left
;   3 LEAVING   the FB stays on screen 1-2 frames while OAM/CGRAM catch up,
;               then a prologue puts the quad-layer's registers back
;               (GbcFbRestore); window A of that frame writes FB_EN = 0 and
;               restores the LCD-off white map the FB tilemap sat on
;
; THE ROW MACHINE (contract sec. 14.6).  The bridge freezes a row of cells
; (8 GB lines) the moment the beam finishes writing it, and only reopens it
; at the beam's next visit after a ROW_DONE from us.  While frozen its 20
; dirty bits and its content are stable, so the uploader reads them straight
; from the bridge, writes the row's 20 tilemap entries if the row's CL (the
; 3 palette bits of direct colour, per frame by vote) moved, sends the dirty
; spans, and only then strobes ROW_DONE -- never with a bit still pending
; (invariant 40).  A row whose transfer does not fit stays frozen: nothing is
; lost, the spans already sent are remembered (!GBC_FB_RESUME) and the
; cursor comes back to it.
;
; ORDER: round-robin with a PERSISTENT cursor (advisor A1).  The cyclic order
; is 8..17, 0..7 (rows 8..17 froze during the previous frame's picture and
; are ready when window B opens; rows 0..7 freeze INSIDE the window, at V ~
; 195 + 13.6 r) but the cursor is never reset per frame.  A pass visits the
; 18 positions once from the cursor; a row not frozen yet is skipped without
; moving the cursor past it, and the cursor ends on the row that did not fit
; or right after the last row the pass completed (the freeze_model's rule,
; C6-SPEC c.1), so a storm serves every row within two frames instead of
; starving the top third.
;
; DIRECT PAGE.  The pass borrows the block walk's $40-$57 and $5F-$65 (never
; live at the same time: the walk restarts every class from its persistent
; cursor) and stays out of $58-$59 (every DMA's GbcDebit) and $5A-$5E
; (GbcRegs, which the exit prologue may run).  Words whose high byte is
; zeroed at the pass entry, so `ldx.b` loads them as 16-bit indices:
;   $40-$41 position in the cycle     $42-$43 the row r
;   $44 i (positions visited)         $45 last position completed ($FF none)
;   $46-$47 r's byte in the 18-bit maps   $48 r's bit in that byte
;   $49 CL(r)                         $4B/$4D a nibble's last / first cell
;   $4C the dirty byte being scanned  $4E the first cell of that nibble
;   $4F last dirty cell of the open span ($FF: none)
;   $52 first cell of the span        $53 cells in it     $54-$55 its bytes
;   $56-$57 nibble value (index)      $5F-$61 the row's 20 dirty bits
;   $62-$63 dirty byte index 0..2     $64-$65 VRAM word
; ===========================================================================

; ---------------------------------------------------------------------------
; GbcFbInit -- boot: state OFF, cursor at row 8, no row mapped, and the
; STAGE this session runs, out of the GBCF block (GbcCfg): +5 = 1 is the
; second stage, anything else the first.  It is read here and only here --
; before any FB state exists, never again mid-game -- and every stage-
; dependent value is decided now: the C6CTL of the entry (!GBC_FB_CTL), the
; row pass's jump (!GBC_FB_ROWJ, an indirect jmp: the hot path pays no test)
; and the flag (!GBC_FB_STG) the prologue and the drain branch on.
; ---------------------------------------------------------------------------
GbcFbInit:
    rep #$30
    ldx.w #$0000
    lda.w #$0000
GbcFbInitLoop:
    sta.w !GBC_FB_OFF,x
    inx
    inx
    cpx.w #!GBC_FB_LEN
    bne GbcFbInitLoop
    lda.w #!GBC_DEADLINE
    sta.w !GBC_FB_DL
    lda.w #!GBC_VIRQ_LINE
    sta.w !GBC_FB_VL
    lda.w #GbcFbRow1
    sta.w !GBC_FB_ROWJ
    sep #$20
    lda.b #!GBC_FB_CTLON1
    sta.w !GBC_FB_CTL
    lda.l GbcCfgStretch
    cmp.b #$01
    bne GbcFbInitStg            ; 0 (or an unknown value): the first stage
    sta.w !GBC_FB_STG
    lda.b #!GBC_FB_CTLON2
    sta.w !GBC_FB_CTL
    rep #$20
    lda.w #GbcFbRow2
    sta.w !GBC_FB_ROWJ
    sep #$20
GbcFbInitStg:
    lda.b #$FF
    sta.w !GBC_FB_CLFIX
    ldx.w #$0011
GbcFbInitCl:
    sta.w !GBC_FB_MAPCL,x
    dex
    bpl GbcFbInitCl
    rts

; ---------------------------------------------------------------------------
; GbcFbPolicy -- entry: A 8-bit, X/Y 16-bit, this frame's status copy valid
; (our wire version, SNAP_VALID).  The state machine of C6-SPEC b.2 / contract
; sec. 14.10, run once per frame right after the status copy and before the
; fold, from whichever half opened the frame.
;
;   press_in  = LOG_OVF | COLW_N >= !GBC_FB_COLW_IN  | RC_DROP >= !GBC_FB_RCDROP_IN
;   press_out = LOG_OVF | COLW_N >= !GBC_FB_COLW_OUT | RC_DROP > 0
;   enter when press_in held in >= 6 of the last 8 frames, with the genlock
;   LOCKED, the LCD on, not in compat, not FIRST_FRAME, and the screen
;   released; leave at once on LCD off / FIRST_FRAME / unlocked / no C6 in
;   the bridge, or after 90 frames in a row with press_out = 0.
;
; RC_DROP is the colour_dropped of the last compile that COMPLETED
; (!GBC_FB_RCDROP, latched here whenever no compile is running: a compile in
; flight has zeroed its counters at its start).  No compile runs while the FB
; is on screen, so the latch is cleared at the cut-over and the quiet count
; there reads LOG_OVF and COLW_N alone -- the two the bridge keeps publishing.
; RC_LCDC is NOT a pressure (advisor A3/sec. 14.10: a decision for the
; maintainer; the default is press_in without it).
; ---------------------------------------------------------------------------
GbcFbPolicy:
    stz.w !GBC_FB_ROWSDONE      ; this frame's ROW_DONEs (C6MODE.hi)
    stz.w !GBC_FB_DONEM         ; ... and which rows got one
    stz.w !GBC_FB_DONEM+1
    stz.w !GBC_FB_DONEM+2
    lda.l !GBC_STAT_LONG+!GBC_ST_C6FLAGS ; live (not in the copy)
    bmi GbcFbPolC6
    jmp GbcFbNoSnap             ; no C6 in this bridge: never on
GbcFbPolC6:
    ; --- the last completed compile's colour_dropped, latched -----------------
    lda.w !GBC_FB_STATE
    cmp.b #$02
    bcs GbcFbPolLatched         ; FB on screen: no compile, the latch is 0
    lda.b $80
    cmp.b #$02
    beq GbcFbPolLatched         ; a compile is half way: its counters too
    stz.w !GBC_FB_RCDROP
    rep #$20
    lda.l !GBC_RC_DROP
    beq GbcFbPolNoDrop          ; b1 = press_out's "> 0"
    cmp.w #!GBC_FB_RCDROP_IN
    sep #$20
    lda.b #$02
    bcc GbcFbPolDropSt
    lda.b #$03                  ; b0 = press_in's ">= !GBC_FB_RCDROP_IN"
GbcFbPolDropSt:
    sta.w !GBC_FB_RCDROP
GbcFbPolNoDrop:
    sep #$20
GbcFbPolLatched:
    ; --- this frame's pressure: $4C b0 = press_in, b1 = press_out -------------
    lda.w !GBC_FB_RCDROP
    sta.b $4C
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #!GBC_F_LOGOVF
    beq GbcFbPolColw
    lda.b #$03
    sta.b $4C
GbcFbPolColw:
    rep #$20
    lda.l !GBC_STAT_LONG+!GBC_ST_COLWN ; live: not in the copy (see
    sta.b $56                   ; !GBC_STATCOPY_LEN); it only moves with a
    cmp.w #!GBC_FB_COLW_OUT     ; snapshot, and none is taken before CONSUMED
    bcc GbcFbPolPress
    sep #$20
    lda.b #$02
    tsb.b $4C
    rep #$20
    lda.b $56
    cmp.w #!GBC_FB_COLW_IN
    bcc GbcFbPolPress
    sep #$20
    lda.b #$01
    tsb.b $4C
GbcFbPolPress:
    sep #$20
    ; hist = (hist << 1) | press_in ; quiet = press_out ? 0 : min(255, quiet + 1)
    lda.b $4C
    lsr a                       ; carry = press_in
    rol.w !GBC_FB_HIST
    lsr a                       ; carry = press_out
    bcc GbcFbPolQuietInc
    stz.w !GBC_FB_QUIET
    bra GbcFbPolState
GbcFbPolQuietInc:
    lda.w !GBC_FB_QUIET
    inc a
    beq GbcFbPolState           ; saturated at 255
    sta.w !GBC_FB_QUIET
GbcFbPolState:
    lda.w !GBC_FB_STATE
    beq GbcFbPolOff
    cmp.b #$03
    beq GbcFbPolOut             ; already on the way out
    ; --- ENTERING / ON: an immediate exit? ------------------------------------
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #(!GBC_F_LOCKED|!GBC_F_FIRST|!GBC_F_LCDON)
    cmp.b #(!GBC_F_LOCKED|!GBC_F_LCDON)
    bne GbcFbPolLeave           ; LCD off, FIRST_FRAME, unlocked: at once
    lda.l !GBC_SC+!GBC_ST_LYSYNC
    cmp.b #!GBC_FB_LYSYNC
    bne GbcFbPolLeave           ; ⚡ LY=0 is not where the row order and
                                ; the pending rows assume it (the Exato mode reports
                                ; LOCKED by construction; LY_SYNC does not lie)
    lda.w !GBC_FB_STATE
    cmp.b #$02
    bne GbcFbPolOut             ; entering: no quiet exit before the cut
    lda.w !GBC_FB_QUIET
    cmp.b #!GBC_FB_QUIET_N
    bcc GbcFbPolOut
GbcFbPolLeave:
    jmp GbcFbLeave
GbcFbPolOut:
    rts
GbcFbPolOff:
    ; --- OFF with FB_EN still IN FORCE in the bridge, and no FB_EN = 0 owed:
    ; ⚡ advisor C6-PLAYER.  A console reset reboots the player (state OFF,
    ; nothing owed) and may leave the bridge running with the last session's
    ; FB_EN = 1.  Entering from there writes 1 over 1: no 0 -> 1 edge at the
    ; next LY=0, so no restart -- C6_DIRTY is NOT all ones, the rows the old
    ; session had sent are clean, and the cut would show tiles this boot's
    ; VRAM never received (tests/host, scene key stale=1).  Write the 0 and
    ; do not enter this frame: an LY=0 has to see it first.
    lda.l !GBC_STAT_LONG+!GBC_ST_C6FLAGS
    and.b #$04                  ; b2: FB_EN in force
    beq GbcFbPolEnter
    lda.w !GBC_FB_FLAGS
    and.b #$04
    bne GbcFbPolOut             ; the exit's FB_EN = 0 is owed: it goes out
    lda.b #$00                  ; in window A (GbcFbOwed)
    ldx.w #!GBC_EFO_C6CTL
    jmp GbcEfPut
GbcFbPolEnter:
    ; --- OFF: enter? -------------------------------------------------------
    lda.b $06
    cmp.b #$02
    bne GbcFbPolOut             ; the screen is not released yet: nothing to
                                ; cut over from
    lda.w !GBC_FB_FLAGS
    and.b #$06
    bne GbcFbPolOut             ; the last exit's white map / FB_EN = 0 are
                                ; still owed: the footprint is not ours yet
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #(!GBC_F_LOCKED|!GBC_F_COMPAT|!GBC_F_FIRST|!GBC_F_LCDON)
    cmp.b #(!GBC_F_LOCKED|!GBC_F_LCDON)
    bne GbcFbPolOut
    lda.l !GBC_SC+!GBC_ST_LYSYNC
    cmp.b #!GBC_FB_LYSYNC
    bne GbcFbPolOut             ; the locked phase EXACTLY (see !GBC_FB_LYSYNC)
    lda.w !GBC_FB_HIST
    and.b #$0F
    sta.b $56
    stz.b $57
    ldx.b $56
    lda.w GbcFbPop4,x
    sta.b $4C
    lda.w !GBC_FB_HIST
    lsr a
    lsr a
    lsr a
    lsr a
    sta.b $56
    ldx.b $56
    lda.w GbcFbPop4,x
    clc
    adc.b $4C
    cmp.b #!GBC_FB_PRESS_N
    bcc GbcFbPolOut
    ; --- OFF -> ENTERING: FB_EN to the bridge (in force at its next LY=0),
    ; every row unmapped and none ready.  The cursor is NOT reset: it is
    ; persistent across frames by design, and across sessions it costs
    ; nothing.
    lda.b #$01
    sta.w !GBC_FB_STATE
    stz.w !GBC_FB_READY
    stz.w !GBC_FB_READY+1
    stz.w !GBC_FB_READY+2
    stz.w !GBC_FB_PEND
    stz.w !GBC_FB_PEND+1
    stz.w !GBC_FB_PEND+2
    stz.w !GBC_FB_FLAGS
    lda.b #$FF
    sta.w !GBC_FB_CLFIX
    ldx.w #$0011
GbcFbEnterCl:
    sta.w !GBC_FB_MAPCL,x
    stz.w !GBC_FB_RESUME,x
    dex
    bpl GbcFbEnterCl
    inc.w !GBC_FB_ENTRIES       ; 8 bits, wraps
    lda.w !GBC_FB_CTL           ; FB_EN [| STRETCH_H], decided at boot
    ldx.w #!GBC_EFO_C6CTL
    jmp GbcEfPut

; OFF (or LEAVING) is where a frame without a usable picture leaves the mode:
; no status, wrong version, SNAP_VALID = 0, or a bridge without C6
; (contract sec. 14.10's immediate exits).
GbcFbNoSnap:
    stz.w !GBC_FB_ROWSDONE
    lda.w !GBC_FB_STATE
    beq GbcFbNoSnapOut
    cmp.b #$03
    beq GbcFbNoSnapOut
GbcFbLeave:
    lda.b #$03
    sta.w !GBC_FB_STATE
    stz.w !GBC_FB_EXITCNT
GbcFbNoSnapOut:
    rts

; The frame body with the mode on, entering, leaving, or owed by the last
; exit (GbcNmiSnapOk / GbcIrqSnapOk / GbcFrameResume): the policy first, the
; FB's drain in front of the quad-layer's.
GbcNmiFbBody:
    jsr GbcFbPolicy     ; enter / stay / leave the FB mode
    jsr GbcFold
    jsr GbcFbRegsSafe   ; GbcRegsSafe, unless the FB is on screen
    jsr GbcFbRasterArm  ; the log copy, inside the window and before CONSUMED
                        ; -- unless the FB is on screen (no compile runs then)
    jsr GbcDrainFb
    jmp GbcReadyCheck
GbcIrqFbBody:
    lda.b #$01
    sta.w !GBC_FB_POLEND        ; the resume follows (GbcFrameResume)
    jsr GbcFbPolicy
    jsr GbcFold         ; (GbcIrqBody's order)
    lda.b #$03
    sta.b $16           ; b1: the resume owes regs, log, CGRAM and the walk
    jsr GbcDrainOpen
    jmp GbcDrainBFb
GbcResumeFbBody:
    stz.w !GBC_FB_POLEND
    jsr GbcFbRegsSafe   ; both skipped while the FB is on screen
    jsr GbcFbRasterArm
    jsr GbcDrainResumeFb
    jmp GbcReadyCheck

; GbcRegs / GbcRasterArm, unless the FB is on the screen (states 2 and 3):
; the registers would fight the FB's, and a compile would be wasted.  State
; 1 keeps both -- the quad-layer is still what is shown.
GbcFbRegsSafe:
    lda.w !GBC_FB_STATE
    cmp.b #$02
    bcs GbcFbSkip
    jmp GbcRegsSafe
GbcFbRasterArm:
    lda.w !GBC_FB_STATE
    cmp.b #$02
    bcs GbcFbSkip
    jmp GbcRasterArm
GbcFbSkip:
    rts

; Z set = the block backlog is empty: every class bit ($20-$39) and the two
; duplicate chr bytes ($3B/$3C).  OAM/CGRAM ($3A) are not the walk's.
GbcFbBacklogEmpty:
    rep #$20
    lda.b $20
    ora.b $22
    ora.b $24
    ora.b $26
    ora.b $28
    ora.b $2A
    ora.b $2C
    ora.b $2E
    ora.b $30
    ora.b $32
    ora.b $34
    ora.b $36
    ora.b $38
    ora.b $3B
    sep #$20                    ; (Z survives the sep)
    rts

; The drain's two hooks (see GbcDrain).  FRONT: the FB while entering or on,
; and what the last exit still owes while off.  BACK: the FB while leaving
; with it on screen (after OAM/CGRAM, advisor A7).
GbcDrainFront:
    lda.w !GBC_FB_STATE
    beq GbcDrainFrontOwed
    cmp.b #$03
    bne GbcDrainFrontOn
    lda.w !GBC_FB_STG
    bne GbcDrainFrontOwed       ; the second stage's way back owes it all
    rts
GbcDrainFrontOn:
    jmp GbcFbDrain
GbcDrainFrontOwed:
    lda.w !GBC_FB_FLAGS
    and.b #$0E
    beq GbcDrainFrontOut
    jmp GbcFbOwed
GbcDrainFrontOut:
    rts
GbcDrainBack:
    lda.w !GBC_FB_STG
    bne GbcDrainFrontOut        ; the second stage leaves under blank
    lda.w !GBC_FB_STATE
    cmp.b #$03
    bne GbcDrainFrontOut
    lda.w !GBC_FB_FLAGS
    lsr a                       ; b0: the FB is on screen
    bcc GbcDrainFrontOut
    lda.l !GBC_SC+!GBC_ST_FLAGS0
    and.b #!GBC_F_LCDON
    beq GbcDrainFrontOut        ; leaving because the LCD went off: the GB is
                                ; not drawing, nothing will be revisited
    jmp GbcFbDrain

; Window A after the way back: FB_EN = 0 to the bridge and the white map
; (2 KB, ROM -> $A000, the region the FB tilemap and its first tiles took)
; back into VRAM.  The LCD-off screen reads it, and until it is back GbcRegs
; shows the backdrop instead of it (C6-SPEC a.5: one black frame, then white).
GbcFbOwed:
    lda.w !GBC_FB_FLAGS
    and.b #$04
    beq GbcFbOwedMap
    lda.b #$00
    ldx.w #!GBC_EFO_C6CTL
    jsr GbcEfPut
    lda.b #$04
    trb.w !GBC_FB_FLAGS
GbcFbOwedMap:
    lda.w !GBC_FB_FLAGS
    and.b #$08                  ; b3: the solid tiles (only the second
    beq GbcFbOwedWhite          ; stage's way back ever owes them)
    jsr GbcFbOwedSolid
    bcc GbcFbOwedOut
GbcFbOwedWhite:
    lda.w !GBC_FB_FLAGS
    and.b #$02
    beq GbcFbOwedOut
    rep #$20
    lda.w #2048
    sta.b $1C
    sep #$20
    jsr GbcDmaGuard
    bcc GbcFbOwedNo
    jsr GbcWhiteMapUp
    rep #$20
    lda.w #2048
    jsr GbcDebit
    sep #$20
    lda.b #$02
    trb.w !GBC_FB_FLAGS
    rts
GbcFbOwedNo:
    jsr GbcDropCount
    lda.b #$01
    sta.b $17
GbcFbOwedOut:
    rts

; ---------------------------------------------------------------------------
; GbcFbPrologue -- the NMI prologue's half, with the mode anywhere but OFF.
; Entry: A = fb_state (8-bit).  Carry set = the FB's channel set was put on
; the bus and the quad-layer's publish is to be skipped this frame.
; ---------------------------------------------------------------------------
GbcFbPrologue:
    cmp.b #$01
    beq GbcFbProEnter
    cmp.b #$02
    beq GbcFbProOn
    ; --- LEAVING -----------------------------------------------------------
    lda.w !GBC_FB_STG
    beq GbcFbProLeave1
    jmp GbcFbProLeave2          ; the second stage leaves under blank
GbcFbProLeave1:
    lda.w !GBC_FB_FLAGS
    lsr a                       ; b0: the cut happened
    bcc GbcFbProNoCut
    inc.w !GBC_FB_EXITCNT
    lda.b $80
    cmp.b #$02
    beq GbcFbProOn              ; a compile is writing a set: the way back
                                ; re-seeds both, so it waits for it to end
    lda.b $3A
    and.b #$03
    beq GbcFbProRestore         ; OAM and CGRAM are current: the way back
    lda.w !GBC_FB_EXITCNT
    cmp.b #!GBC_FB_EXIT_MAX
    bcc GbcFbProOn              ; one more frame of the FB while they land
GbcFbProRestore:
    jsr GbcFbRestore
    clc
    rts
GbcFbProNoCut:
    ; the FB never reached the screen: nothing to put back but the white
    ; map its tilemap sat on, and the bridge's FB_EN
    lda.b #$06
    tsb.w !GBC_FB_FLAGS
    stz.w !GBC_FB_STATE
    clc
    rts
GbcFbProEnter:
    ; --- ENTERING: every row up at least once?  Then cut over now. --------
    lda.w !GBC_FB_READY
    and.w !GBC_FB_READY+1
    cmp.b #$FF
    bne GbcFbProNo
    lda.w !GBC_FB_READY+2
    cmp.b #$03
    bne GbcFbProNo
    lda.w !GBC_FB_STG
    bne GbcFbProCut2
    jsr GbcFbCut
    sec
    rts
GbcFbProCut2:
    jsr GbcFbCut2
    sec
    rts
GbcFbProNo:
    lda.w !GBC_FB_STG
    bne GbcFbProNoBlank
    clc                         ; first stage: the quad-layer stays on screen
    rts
GbcFbProNoBlank:
    jsr GbcFbPubBlank           ; the FB is being built over the quad-layer's
    sec                         ; VRAM: nothing of it may be shown meanwhile
    rts
GbcFbProOn:
    lda.w !GBC_FB_STG
    bne GbcFbProOn2
    jsr GbcFbPublish
    bra GbcFbProOnCtr
GbcFbProOn2:
    jsr GbcFbPublish2
GbcFbProOnCtr:
    inc.w !GBC_FB_FRAMES
    bne GbcFbProOnOut
    inc.w !GBC_FB_FRAMES+1
GbcFbProOnOut:
    sec
    rts

; ---------------------------------------------------------------------------
; GbcFbCut -- the cut-over (C6-SPEC a.5 step 3, contract sec. 14.8): in the
; prologue, before any HDMA of V=0, no DMA in between.  One video frame
; separates the quad-layer from the FB; no black frame.
;
;   $2105 = $03   Mode 3: BG1 8bpp (CG direct select through $2130.b0)
;   $2107 = $50   BG1 tilemap at word $5000 (byte $A000), 32x32
;   $210B = $05   BG1 chr base at word $5000: tile k at word $5000 + 32k
;   $212C = $01   main screen = BG1 only
;   $2123 = $0C   BG1: W2 enabled, OUTSIDE -> masked off the pillars
;   $2125 = $8C   colour window = W2, INSIDE; OBJ stays W2-OUT (not shown)
;   $212B = $00   OR
;   $2130 = $11   b0 direct colour; b5:4 = 01: the fixed colour is added
;                 INSIDE the colour window only; b1 = 0: fixed colour, not
;                 the subscreen; b7:6 = 00: never clip the main screen
;   $2131 = $20   add, full, on the BACKDROP only
;   $2132         the darkest colour CL_CUR can name (GbcFbColdata)
;   BG1 HOFS/VOFS = -48 / -41: the FB at X 48..207, V 41..184, as today
; Effect: inside the viewport a transparent pixel (byte $00) shows CGRAM[0]
; + the fixed colour = exactly the direct colour of byte 0 with this CL;
; outside it the colour window turns the sum off and the backdrop stays
; black.  The codings of $2130.b5:4 and $2125.b7:6 were read on the manual's
; page images (2-27-16, 2-27-12, A-17) and in bsnes-plus by the spec's
; advisor; the offline gate proves the values, the picture itself is the
; bsnes-plus harness's.
; ---------------------------------------------------------------------------
GbcFbCut:
    lda.b #$03
    sta.w $2105
    lda.b #$50
    sta.w $2107
    lda.b #$05
    sta.w $210B
    lda.b #$01
    sta.w $212C
    lda.b #$0C
    sta.w $2123
    lda.b #$8C
    sta.w $2125
    stz.w $212B
    lda.b #$11
    sta.w $2130
    lda.b #$20
    sta.w $2131
    jsr GbcFbColdata
    rep #$20
    lda.w #$03D0                ; -48 & $3FF
    ldx.w #$210D
    jsr GbcScrollW              ; BG1HOFS
    lda.w #$03D7                ; -41 & $3FF
    ldx.w #$210E
    jsr GbcScrollW              ; BG1VOFS
    sep #$20
    lda.b #$02
    sta.w !GBC_FB_STATE
    lda.b #$01
    tsb.w !GBC_FB_FLAGS         ; the FB is what is shown from V=1 on
    stz.w !GBC_FB_RCDROP        ; no compile under the FB (see GbcFbPolicy)
    ; the compiler: idle, both sets owed on the way back.  A compile that is
    ; armed or finished is dropped (its tables are the scene the FB replaces);
    ; one in flight finishes on its own and waits in "done" -- the FB's
    ; prologue never publishes a set -- until GbcFbRestore drops it too.
    lda.b #$03
    sta.b $82
    lda.b $80
    cmp.b #$02
    beq GbcFbPublish
    stz.b $80
    ; fall through
; ---------------------------------------------------------------------------
; GbcFbPublish -- the FB's channel set (contract sec. 14.8 "HDMA no modo FB"):
; ch5 the letterbox, ch4 an EMPTY window 1, both static and both read from
; this ROM (the same bytes the boot seeded the WRAM sets with), ch0..ch3
; off, $420C = $30.  No roles: nothing drives a scroll pair, CGRAM or the
; LCDC group, so GbcDrainCgram may run anywhere in the window and the tail
; is worth the two-channel figure.  Re-pointed every frame like the normal
; publish does (the pointers are static, the habit is cheap).
; ---------------------------------------------------------------------------
GbcFbPublish:
    rep #$20
    lda.w #GbcTblInidisp
    sta.w $4352                 ; ch5 A1T = the letterbox
    lda.w #GbcTblW1
    sta.w $4342                 ; ch4 A1T = window 1 empty everywhere
    sep #$20
    stz.w $4354                 ; A1B = $00: this ROM
    stz.w $4344
    stz.b $73
    lda.b #!GBC_BPLHDMA_FB
    sta.b !GBC_DP_HDMA
    rep #$20
    lda.w #!GBC_TAILBYTES_FB
    sta.b !GBC_DP_HDMA+1
    sep #$20
    ; a colour channel that was on the bus has just left it: GbcFold reads
    ; $01 to re-send the CGRAM view once (see GbcHdmaPublish)
    lda.b $00
    beq GbcFbPubCol
    stz.b $00
    inc.b $01
GbcFbPubCol:
    ; $2132 follows CL_CUR (C6-SPEC b.3: the rows still on the old
    ; quantisation are <= 2/31 off on their byte-0 pixels for <= 3 frames)
    lda.l !GBC_STAT_LONG+!GBC_ST_C6CL
    and.b #$07
    cmp.w !GBC_FB_CLFIX
    beq GbcFbPubMask
    jsr GbcFbColdata
GbcFbPubMask:
    lda.b #$30                  ; ch5 + ch4 (ch4 is the decoy here)
    sta.w $420C                 ; the screen was released before the entry
    sta.l !GBC_HDMAEN_SH
    rts

; $2132 = {2 CL0, 2 CL1, 4 CL2}, the three components written separately;
; remembers the CL it was written for.  2*CL1 and 4*CL2 are the bits of CL as
; they sit, only CL0 has to move up.
GbcFbColdata:
    lda.l !GBC_STAT_LONG+!GBC_ST_C6CL
    and.b #$07
    sta.w !GBC_FB_CLFIX
    and.b #$01
    asl a
    ora.b #$20
    sta.w $2132                 ; R = 2 CL0
    lda.w !GBC_FB_CLFIX
    and.b #$02
    ora.b #$40
    sta.w $2132                 ; G = 2 CL1
    lda.w !GBC_FB_CLFIX
    and.b #$04
    ora.b #$80
    sta.w $2132                 ; B = 4 CL2
    rts

; ---------------------------------------------------------------------------
; GbcFbRestore -- the way back (C6-SPEC a.5 "Saida"), in the prologue: the
; sec. 11.3 registers, the quad-layer's channel set with ch4/ch5 static until
; the first compile lands (both set headers back to the boot seed: a stale
; colour channel would otherwise come back for a frame, and with it the S9
; hazard), and GbcRegs out of the last snapshot -- which, with the LCD off,
; puts the backdrop up for one frame, because the white map is not back yet
; (the owed flag set here is what GbcRegs' white branch reads).  Never
; reached with a compile underneath (GbcFbPrologue waits for it).
; ---------------------------------------------------------------------------
GbcFbRestore:
    lda.w !GBC_FB_STG
    beq GbcFbResMode            ; the first stage never moved W2
    lda.b #48
    sta.w $2128
    lda.b #207
    sta.w $2129                 ; W2 back on the 160-pixel viewport
GbcFbResMode:
    stz.w $2105                 ; Mode 0
    lda.b #$EF
    sta.w $2123
    lda.b #$0C
    sta.w $2125
    stz.w $2130
    stz.w $2131
    lda.b #$E0
    sta.w $2132
    ldx.w #$0000
GbcFbResHdr:
    lda.l GbcHdrSeed,x
    sta.l !GBC_WRAM_LONG+!GBC_SETA_OFF+!GBC_H_MASK,x
    sta.l !GBC_WRAM_LONG+!GBC_SETB_OFF+!GBC_H_MASK,x
    inx
    cpx.w #(GbcHdrSeedEnd-GbcHdrSeed)
    bne GbcFbResHdr
    ldx.w #$0000
GbcFbResW1:
    lda.l GbcTblW1,x
    sta.l !GBC_WRAM_LONG+!GBC_SETA_OFF+!GBC_T_W1,x
    sta.l !GBC_WRAM_LONG+!GBC_SETB_OFF+!GBC_T_W1,x
    inx
    cpx.w #(GbcTblW1End-GbcTblW1)
    bne GbcFbResW1
    lda.b #$03
    sta.b $82                   ; both sets owe the next compile
    stz.b $80                   ; and whatever the last one left is dropped
    stz.w !GBC_FB_STATE
    lda.w !GBC_FB_STG
    bne GbcFbResOwed            ; the second stage's way back paid them already
    lda.b #$06
    tsb.w !GBC_FB_FLAGS         ; window A: FB_EN = 0 and the white map
GbcFbResOwed:
    lda.b #$01
    trb.w !GBC_FB_FLAGS
    ; GbcRegs: TM, the six bases, the scroll pairs (no channel drives
    ; anything: $73 = 0).  Its scratch $5A-$5E is saved around it: this is
    ; the prologue, and the V-IRQ half may be underneath in the middle of a
    ; block class (the walk keeps a counter in $5A) -- the frame body never
    ; runs GbcRegs itself while that half is live, the way back has to.
    rep #$20
    lda.b $5A
    pha
    lda.b $5C
    pha
    sep #$20
    lda.b $5E
    pha
    jsr GbcRegs
    pla
    sta.b $5E
    rep #$20
    pla
    sta.b $5C
    pla
    sta.b $5A
    sep #$20
    jmp GbcHdmaPublish

; ---------------------------------------------------------------------------
; GbcFbDrain -- one pass over the 18 rows from the cursor (C6-SPEC b.3).
; Entry/exit: A 8-bit, X/Y 16-bit.  See the direct-page note above.
;
; THE TICKET ($50-$51).  A live-V guard costs ~40 instructions (GbcCapacity:
; the latched 9-bit read, a multiply), as much as everything else a span
; needs, so it is asked once per ROW that has something to send, and its
; answer -- byte-equivalents that still fit before the deadline, with
; !GBC_GUARDSLACK already held back -- is the row's ticket: every span of the
; row charges it bytes + !GBC_FB_SPANEQ (more than the DMA plus the CPU
; between two spans take on the host clock even at 1.3x CPU, ~1300 mc against
; 2048), and a span that does not fit in what is left is refused.  What is
; left is a LOWER bound of what the guard would answer at that instant, and it
; never outlives its row.  Contract sec. 13.12's guard, amortised over a row:
; "every transfer finishes before the deadline" holds for each one (⚡ player:
; one live read per row, not per DMA).  A first version kept the ticket
; across rows and charged the pass's other work to it: the pending-row tests
; were enough to under-charge it, and with the CPU 30 % dearer a 1280-byte
; row landed on V=40 (tests/host, C6-CPU x1.3).
; ---------------------------------------------------------------------------
GbcFbDrain:
    lda.b $17
    beq GbcFbDrGo
    rts                         ; this window is already out of room
GbcFbDrGo:
    stz.b $41
    stz.b $43
    stz.b $47
    stz.b $4B
    stz.b $53
    stz.b $57
    stz.b $63
    stz.b $44                   ; i
    lda.b #$FF
    sta.b $45                   ; last position completed: none
    jsr GbcFbNow                ; the pass's clock (refreshed at every release)
    sta.w !GBC_FB_NOW
    sep #$20
    jsr GbcFbDmaSetup
GbcFbDrLoop:
    lda.w !GBC_FB_CUR
    clc
    adc.b $44
    sta.b $40                   ; p = CUR + i, 0..34: the position tables are
    ldx.b $40                   ; the cycle written out twice (no mod 18)
    lda.w GbcFbPByte,x
    sta.b $46
    lda.w GbcFbPMask,x
    sta.b $48
    ldx.b $46
    and.w !GBC_FB_DONEM,x
    bne GbcFbDrNext             ; released this frame: nothing new before the
                                ; window closes (rows 0..7 thaw next frame,
                                ; 8..17 refreeze past V=40)
    lda.b $48
    and.l !GBC_STAT_LONG+!GBC_ST_ROWF,x
    beq GbcFbDrNext             ; not frozen yet: skipped, the cursor keeps it
    ldy.b $40
    lda.w GbcFbPRow,y
    sta.b $42                   ; the row
    asl a
    sta.b $4A                   ; 2 r, for the word tables
    lda.b $48
    and.w !GBC_FB_PEND,x
    beq GbcFbDrGoRow
    ; --- pending: skipped until its revisit is over, then the bit drops.
    ;     The clock is the pass's last reading (its start, or the last
    ;     release): never later than now, so a revisit it calls unfinished may
    ;     only be over already -- the row waits for the next pass, it is never
    ;     released early.
    rep #$20
    lda.w !GBC_FB_NOW
    ldy.b $4A
    sec
    sbc.w !GBC_FB_VE,y
    sep #$20
    bmi GbcFbDrNext             ; released, revisit still to come: skipped
    lda.b $48
    eor.b #$FF
    and.w !GBC_FB_PEND,x
    sta.w !GBC_FB_PEND,x
GbcFbDrGoRow:
    jsr GbcFbRow
    bcc GbcFbDrStop
    ; --- the whole row is up: ROW_DONE(r) --------------------------------
    lda.b $42
    ldx.w #!GBC_EFO_ROWDONE
    jsr GbcEfPut
    ldx.b $46
    lda.b $48
    ora.w !GBC_FB_DONEM,x
    sta.w !GBC_FB_DONEM,x
    lda.b $48
    ora.w !GBC_FB_PEND,x
    sta.w !GBC_FB_PEND,x
    lda.b $48
    ora.w !GBC_FB_READY,x
    sta.w !GBC_FB_READY,x
    ldx.b $42
    stz.w !GBC_FB_RESUME,x
    jsr GbcFbRevisit
    inc.w !GBC_FB_ROWSDONE
    inc.w !GBC_FB_RDTOTAL
    bne GbcFbDrDone16
    inc.w !GBC_FB_RDTOTAL+1
GbcFbDrDone16:
    lda.b $40
    sta.b $45
GbcFbDrNext:
    inc.b $44
    lda.b $44
    cmp.b #18
    beq GbcFbDrEnd
    jmp GbcFbDrLoop
GbcFbDrEnd:
    lda.b $45
    bmi GbcFbDrOut              ; no row completed: the cursor stays
    inc a                       ; p + 1, 1..35
    cmp.b #18
    bcc GbcFbDrCur
    sbc.b #18                   ; (carry set) mod 18
    cmp.b #18
    bcc GbcFbDrCur
    lda.b #$00                  ; p = 35: 36 mod 18
GbcFbDrCur:
    sta.w !GBC_FB_CUR           ; right after the last row completed
GbcFbDrOut:
    rts
GbcFbDrStop:
    lda.b $40
    cmp.b #18
    bcc GbcFbDrStCur
    sbc.b #18                   ; (carry set) p mod 18
GbcFbDrStCur:
    sta.w !GBC_FB_CUR           ; resume at the row that did not fit
    lda.b #$01
    sta.b $17                   ; out of room: the rest of the window's drain
    rts                         ; is skipped, the frame counts as deferred

; ---------------------------------------------------------------------------
; THE PENDING ROWS.  A ROW_DONE does not open a row: the bridge keeps it
; frozen until the beam's next visit to its first scan (sec. 14.6.3).  A row
; the pass finds frozen may therefore be one it already released and whose
; visit has not come yet -- rows 1..7 released in one window are revisited
; INSIDE the next one (V ~ 196..278), row 8 released late in window A's tail
; past V=29 -- and that row holds nothing to send (its content and dirty
; bits are those of the release).  Handling it again would cost a pass of
; CPU for nothing and, worse, a second ROW_DONE that raced its thaw would
; land on an OPEN row (ignored, c6_rowdone_bad++).  So a released row is
; PENDING until its revisit is surely over, and a pending row is skipped like
; an open one.  The revisit is at a fixed phase with the genlock locked (LY=0
; at V 182.35, one GB line = 1.7011 SNES lines: C6-SPEC c.1, the same time
; base as the bridge model and freeze_model.py), so its end is computed at
; the release and kept per row (!GBC_FB_VE).  The FB mode is never on without
; LOCKED and LY_SYNC >= 25.  Frozen and not pending = never released (the
; beam finds it frozen and drops that visit) or released and rewritten since:
; either way a ROW_DONE is safe whenever it lands.
;
; GbcFbNow -- the BODY CLOCK: A16 = frame << 8 | tau, where frame is the low
; byte of !GBC_CTR_FRAMES (it moves at the body's start: V=185 with the
; V-IRQ, 225 without) and tau = (V - 185) mod 262, the lines into the body
; (0..117 inside a pass; a revisit end is at most 243).  Only 16-bit
; DIFFERENCES are ever used, so the wrap of the frame byte is harmless.
; Enter A 8-bit; leaves A 16-bit; clobbers $1A/$1B and $64-$65 ($64 = tau).
; ---------------------------------------------------------------------------
GbcFbNow:
    jsr GbcVCount               ; A16 = V
    sec
    sbc.w #185
    bcs GbcFbNowTau
    adc.w #262                  ; V < 185: past the frame's V=0
GbcFbNowTau:
    sta.b $64                   ; tau (high byte 0)
    sep #$20
    lda.w !GBC_CTR_FRAMES&$FFFF
    xba
    lda.b $64
    rep #$20
    rts

; At row $42's ROW_DONE: when its revisit is surely over.  The visit after
; the release is this body's if the release comes before its first scan (one
; line of margin), else the next body's: VE = {frame [+1], !GbcFbVisEnd[r]}
; (the freeze + 1 line, ceiled).
GbcFbRevisit:
    jsr GbcVCount               ; (GbcFbNow, inlined)
    sec
    sbc.w #185
    bcs GbcFbRevTau
    adc.w #262                  ; V < 185: past the frame's V=0
GbcFbRevTau:
    sta.b $64                   ; tau (high byte 0)
    sep #$20
    lda.w !GBC_CTR_FRAMES&$FFFF
    xba
    lda.b $64
    rep #$20
    sta.w !GBC_FB_NOW
    ldx.b $4A
    lda.b $64
    sec
    sbc.w GbcFbVisLo,x          ; borrow = tau < thaw - 1: this body's visit
    lda.w !GBC_FB_NOW           ; (the lda leaves the carry alone)
    bcc GbcFbRevThis
    clc
    adc.w #$0100                ; the next body's visit
GbcFbRevThis:
    and.w #$FF00
    ora.w GbcFbVisEnd,x
    sta.w !GBC_FB_VE,x
    sep #$20
    rts

; ch7 for the tile transfers of a pass: A->B, mode 1 ($2118/$2119), from the
; FB view; VMAIN steps a word after $2119.  GbcFbMapRow changes DMAP / A1B /
; VMAIN for its own transfer and calls this again.
GbcFbDmaSetup:
    lda.b #$80
    sta.w $2115
    lda.b #$01
    sta.w $4370
    lda.b #$18
    sta.w $4371
    lda.b #!GBC_FB_A1B
    sta.w $4374
    rts

; ---------------------------------------------------------------------------
; GbcFbRow -- one frozen row $42: its CL and its 20 dirty bits read LIVE from
; the bridge (a row that froze after the frame's status copy is not in the
; copy; +5D, the CL pair, is read live too -- it moves at LY=0 and a locked
; LY=0 never falls in a window, advisor A2), the tilemap entries if
; the CL the row carries is not the one its entries were written with, then
; every dirty span not sent yet.  Entry: X = the row's byte in the 18-bit
; maps (the pass's $46).  Carry set = the whole row is up and ROW_DONE may go
; out; clear = a transfer did not fit, the row stays frozen and
; !GBC_FB_RESUME remembers the first cell still owed.
; ---------------------------------------------------------------------------
GbcFbRow:
    ; --- CL(r) = (ROW_CLID[r] == CL_ID) ? CL_CUR : CL_PREV -----------------
    lda.l !GBC_STAT_LONG+!GBC_ST_RCLI,x
    and.b $48
    beq GbcFbRowId0
    lda.b #$08
GbcFbRowId0:
    eor.l !GBC_STAT_LONG+!GBC_ST_C6CL ; live, like the rest of the row's C6 bytes
    bit.b #$08
    beq GbcFbRowCur             ; same id: the row carries CL_CUR
    lsr a                       ; CL_PREV = b6:4 (b7 is 0, b3 falls off)
    lsr a
    lsr a
    lsr a
    bra GbcFbRowCl
GbcFbRowCur:
    and.b #$07
GbcFbRowCl:
    sta.b $49
    ldx.b $42
    cmp.w !GBC_FB_MAPCL,x
    beq GbcFbRowBits
    jsr GbcFbMapRow
    bcs GbcFbRowMapped
    rts                         ; carry clear: refused
GbcFbRowMapped:
GbcFbRowBits:
    ; --- the 20 dirty bits: bits 20r..20r+19 of C6_DIRTY, three bytes from
    ;     byte floor(5r/2), an odd row starting at the high nibble.  Masked
    ;     in place (the neighbours' nibbles out), NOT shifted: the scan starts
    ;     an odd row's cell count at -4 instead.
    ldy.b $42
    lda.w GbcFbRowByte,y
    sta.b $62
    ldx.b $62
    lda.l !GBC_STAT_LONG+!GBC_ST_C6DIRTY,x
    and.w GbcFbM0,y
    sta.b $5F
    lda.l !GBC_STAT_LONG+!GBC_ST_C6DIRTY+1,x
    sta.b $60
    lda.l !GBC_STAT_LONG+!GBC_ST_C6DIRTY+2,x
    and.w GbcFbM2,y
    sta.b $61
    ora.b $60
    ora.b $5F
    bne GbcFbRowSome
    sec                         ; nothing dirty: only the ROW_DONE
    rts
GbcFbRowSome:
    ; --- the row's ticket: the live V guard, once per row with something
    ;     to send (see GbcFbDrain) -------------------------------------------
    jsr GbcCapacity             ; A16 = what still fits, slack held back
    sta.b $50
    sep #$20
    ldy.b $42
    jmp (!GBC_FB_ROWJ)          ; GbcFbRow1 or GbcFbRow2 (GbcFbInit)
GbcFbRow1:
    ; --- the cells already sent from this frozen content (a pass that ran
    ;     out of room part way through the row): not again -----------------
    lda.w !GBC_FB_RESUME,y
    bne GbcFbRowResume
    ; --- every cell dirty (hi-colour, a scroll): one span, no scan ---------
    lda.b $60
    cmp.b #$FF
    bne GbcFbScan
    lda.b $5F
    cmp.w GbcFbM0,y
    bne GbcFbScan
    lda.b $61
    cmp.w GbcFbM2,y
    bne GbcFbScan
    stz.b $52
    lda.b #19
    sta.b $4F
    jmp GbcFbSpan
GbcFbRowResume:
    clc
    adc.w GbcFbBit0,y           ; the cell's bit in the 24-bit window
    rep #$20
    and.w #$00FF
    sta.b $64
    asl a
    adc.b $64                   ; x3 (carry clear: < 24)
    tax
    lda.b $5F
    and.l GbcFbKeep,x
    sta.b $5F
    sep #$20
    lda.b $61
    and.l GbcFbKeep+2,x
    sta.b $61
    ora.b $60
    ora.b $5F
    bne GbcFbScan
    sec                         ; everything was already sent (a held
    rts                         ; release): only the ROW_DONE
    ; the scan: carry out as GbcFbScan says

; ---------------------------------------------------------------------------
; GbcFbScan -- the spans of the dirty bits $5F-$61 of row $42 (Y = r), a BYTE
; at a time, unrolled: {first, last} dirty CELL of each byte by table (one
; table per byte position and row parity, so the cell arithmetic is in the
; ROM).  A set bit more than !GBC_FB_GAP clean cells after the open span's
; last one closes it (bridging g clean cells costs 64g bytes, splitting costs
; a DMA of !GBC_DMACOST = 384 = 6 x 64: C6-SPEC b.3).  Inside a byte the
; widest gap is 6, so only byte boundaries decide.  ⚡ player: spans only --
; the HYBRID "whole row or spans" alternative of C6-SPEC b.3 never wins with
; this gap rule (a whole row always costs at least as much as its spans; the
; golden proved the rule dead, WORKER-C6-GOLDEN).  Every span starts and ends
; on a dirty cell.  Carry set = every span went out.
; ---------------------------------------------------------------------------
macro FbScB(src, tlast, tfirst)
    lda.b <src>
    beq ?skip
    sta.b $56
    ldx.b $56
    lda.w <tlast>,x
    pha                         ; the byte's last dirty cell
    lda.w <tfirst>,x
    sta.b $4D                   ; ... and its first
    lda.b $4F
    bmi ?new                    ; no span open
    lda.b $4D
    sec
    sbc.b $4F                   ; first - last = the gap + 1
    cmp.b #!GBC_FB_GAP+2
    bcc ?ext                    ; gap <= !GBC_FB_GAP: one span
    jsr GbcFbSpan               ; $52..$4F out
    bcs ?new
    pla
    clc
    rts                         ; refused (RESUME says where)
?new:
    lda.b $4D
    sta.b $52
?ext:
    pla
    sta.b $4F
?skip:
endmacro

macro FbScB2(src, tlast, tfirst)
    lda.b <src>
    beq ?skip
    sta.b $56
    ldx.b $56
    lda.w <tlast>,x
    pha
    lda.w <tfirst>,x
    sta.b $4D
    lda.b $4F
    bmi ?new
    lda.b $4D
    sec
    sbc.b $4F
    cmp.b #!GBC_FB_GAP+2
    bcc ?ext
    jsr GbcFbSpan2
    bcs ?new
    pla
    clc
    rts
?new:
    lda.b $4D
    sta.b $52
?ext:
    pla
    sta.b $4F
?skip:
endmacro

GbcFbScan:
    lda.b #$FF
    sta.b $4F                   ; no span open
    lda.w GbcFbBit0,y
    bne GbcFbScOdd
    jmp GbcFbScEven
GbcFbScOdd:
    %FbScB($5F, GbcFbCL04, GbcFbCF04)
    %FbScB($60, GbcFbCL14, GbcFbCF14)
    %FbScB($61, GbcFbCL24, GbcFbCF24)
    jmp GbcFbSpan               ; the last span (one is always open here)
GbcFbScEven:
    %FbScB($5F, GbcFbCL00, GbcFbCF00)
    %FbScB($60, GbcFbCL10, GbcFbCF10)
    %FbScB($61, GbcFbCL20, GbcFbCF20)
    jmp GbcFbSpan

; ---------------------------------------------------------------------------
; GbcFbSpan -- cells $52..$4F of row $42 into their tiles.  Row 2 is the one
; that straddles tile 64, the solid tiles' (cell 46 = col 6): a span across
; cols 5|6 goes out as two DMAs (C6-SPEC a.2 -- never a DMA into $B000-$B03F).
; Carry set = all of it went out; clear = refused, and !GBC_FB_RESUME[r] =
; the first cell still owed.
; ---------------------------------------------------------------------------
GbcFbSpan:
    lda.b $42
    cmp.b #$02
    bne GbcFbSpOne
    lda.b $52
    cmp.b #$06
    bcs GbcFbSpOne
    lda.b $4F
    cmp.b #$06
    bcc GbcFbSpOne
    pha
    lda.b #$05
    sta.b $4F
    jsr GbcFbSpOne              ; cols $52..5
    pla
    sta.b $4F
    bcs GbcFbSpSecond
    rts                         ; refused, RESUME already says col $52
GbcFbSpSecond:
    lda.b #$06
    sta.b $52                   ; then cols 6..
GbcFbSpOne:
    lda.b $4F
    sec
    sbc.b $52
    inc a                       ; n cells
    rep #$20
    and.w #$00FF
    xba
    lsr a
    lsr a
    sta.b $54                   ; 64 n bytes
    ; --- the frame's budget line: bytes + DMACOST, taken here -------------
    lda.b $1E
    sec
    sbc.b $54
    bcc GbcFbSpNoRoomJ
    sbc.w #!GBC_DMACOST
    bcs GbcFbSpLine
GbcFbSpNoRoomJ:
    jmp GbcFbSpNoRoom
GbcFbSpLine:
    sta.b $64                   ; what the line will be if this goes
    ; --- the ticket, or the live guard ------------------------------------
    lda.b $54
    adc.w #!GBC_FB_SPANEQ-1     ; carry set by the sbc: + SPANEQ
    cmp.b $50
    bcs GbcFbSpNo               ; not in what is left of the row's ticket
    eor.w #$FFFF
    sec
    adc.b $50
    sta.b $50                   ; ticket -= bytes + SPANEQ
    lda.b $64
    sta.b $1E                   ; budget line -= bytes + DMACOST
    ; --- VRAM word: the row's first tile + 32 per cell -- and on row 2, past
    ;     its col 6 = cell 46, one tile more (tile 64 is the solid tiles');
    ;     source: $E4:{r[4:0], col0[4:0], 000000}.  Both out of tables: X = 2 r,
    ;     Y = 2 col0 ($53 and $57 stay 0 for the whole pass).
    sep #$20
    lda.b $52
    asl a
    sta.b $56
    rep #$20
    ldx.b $4A
    ldy.b $56
    lda.w GbcFbColW,y
    cpx.w #$0004
    bne GbcFbSpWord
    lda.w GbcFbColW2,y
GbcFbSpWord:
    clc
    adc.w GbcFbTileWord,x
    sta.w $2116
    lda.w GbcFbColSrc,y
    ora.w GbcFbSrcRow,x
    sta.w $4372
    lda.b $54
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B                 ; fire ch7
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    inc.w !GBC_CTR_DMAS&$FFFF   ; the counters GbcDebit keeps (bank-$00
    lda.w !GBC_CTR_BYTES&$FFFF  ; mirror of $7E)
    clc
    adc.b $54
    sta.w !GBC_CTR_BYTES&$FFFF
    bcc GbcFbSpCtr
    inc.w (!GBC_CTR_BYTES+2)&$FFFF
GbcFbSpCtr:
    sep #$20
    sec
    rts
GbcFbSpNo:
    sep #$20
    jsr GbcDropCount
    inc.w !GBC_FB_DROPS
    bne GbcFbSpRefused
    inc.w !GBC_FB_DROPS+1
    bra GbcFbSpRefused
GbcFbSpNoRoom:
    sep #$20
GbcFbSpRefused:
    ldx.b $42
    lda.b $52
    sta.w !GBC_FB_RESUME,x      ; everything before this cell went out
    clc
    rts

; ---------------------------------------------------------------------------
; GbcFbMapRow -- the 20 tilemap entries of row $42 with CL $49: word
; {v=0, h=0, pri=0, pal=CL, t=k(c)}, i.e. byte 0 = k[7:0], byte 1 = CL << 2
; | k[9:8].  k is fixed per row, so the low bytes are a ROM table sent by
; DMA (VMAIN stepping after $2118, 20 B), and the high bytes -- the only
; place the CL lives -- are 20 CPU stores of one or two constants with
; VMAIN stepping after $2119 (the two bytes of a VRAM word are written
; independently).  The guard is asked for the DMA AND the stores' time (the
; stores are VRAM writes too).  Carry clear = refused.
; ---------------------------------------------------------------------------
GbcFbMapRow:
    lda.w !GBC_FB_STG
    beq GbcFbMapRow1
    jmp GbcFbMapRow2
GbcFbMapRow1:
    rep #$20
    lda.w #!GBC_FB_MAPEQ
    sta.b $1C
    clc
    adc.w #!GBC_DMACOST
    cmp.b $1E
    sep #$20
    beq GbcFbMapBud
    bcc GbcFbMapBud
    jmp GbcFbMapNoRoom          ; over the frame's budget line
GbcFbMapBud:
    jsr GbcDmaGuard
    bcs GbcFbMapGo
    jmp GbcFbMapNo
GbcFbMapGo:
    stz.w $2115                 ; VMAIN: +1 word after $2118
    rep #$20
    lda.b $42
    asl a
    asl a
    asl a
    asl a
    asl a
    adc.w #!GBC_FB_MAPWORD      ; carry clear
    sta.b $64                   ; word $5000 + 32 r
    sta.w $2116
    lda.b $42
    asl a
    tax
    lda.w GbcFbMapLoOff,x       ; words: 20 r reaches 340
    adc.w #GbcFbMapLo           ; carry clear: offsets < 360
    sta.w $4372
    lda.w #20
    sta.w $4375
    sep #$20
    stz.w $4370                 ; ch7 DMAP: A->B, increment, mode 0 ($2118)
    stz.w $4374                 ; A1B = $00: this ROM
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.w #20
    jsr GbcDebit
    lda.b $64
    sta.w $2116
    sep #$20
    lda.b #$80
    sta.w $2115                 ; VMAIN: +1 word after $2119
    rep #$20
    lda.b $42
    asl a
    tax                         ; X = 2 r: the two tables below hold words
    sep #$20
    lda.b $49
    asl a
    asl a                       ; CL << 2 | k[9:8] = 0
    ldy.w GbcFbMapHiN,x         ; cells of the row with k < 256
    beq GbcFbMapHi1
GbcFbMapHi0:
    sta.w $2119
    dey
    bne GbcFbMapHi0
GbcFbMapHi1:
    ldy.w GbcFbMapHiM,x         ; ... and with k >= 256
    beq GbcFbMapDone
    ora.b #$01
GbcFbMapHi1L:
    sta.w $2119
    dey
    bne GbcFbMapHi1L
GbcFbMapDone:
    ldx.b $42
    lda.b $49
    sta.w !GBC_FB_MAPCL,x
    inc.w !GBC_FB_MAPROWS
    bne GbcFbMapSetup
    inc.w !GBC_FB_MAPROWS+1
GbcFbMapSetup:
    jsr GbcFbDmaSetup           ; ch7 / VMAIN back to the tile transfers'
    sec
    rts
GbcFbMapNo:
    jsr GbcDropCount
    inc.w !GBC_FB_DROPS
    bne GbcFbMapNoRoom
    inc.w !GBC_FB_DROPS+1
GbcFbMapNoRoom:
    clc
    rts

; ===========================================================================
; THE SECOND STAGE (C6-SPEC a.3/a.4, contract sec. 14.8): the FB stretched to
; 256 x 192 -- STRETCH_H in the bridge (5 -> 8 columns, $E4 serves 32 tiles a
; row) and BG1VOFS by HDMA (ch0, mode 2, [1, 2, 1]).  A USER OPTION, chosen at
; RUN TIME: every image carries both stages, and the GBCF block at the end of
; the bank (GbcCfg, image offset $7F80) says which one a session runs -- its
; +5 is 0 as assembled, and the MCU writes CFG GbcStretch there after staging
; the image (an old MCU leaves the 0: first stage; an old player has no block
; and the MCU writes nothing).  GbcFbInit reads it ONCE at boot into
; !GBC_FB_STG (and the entry's C6CTL into !GBC_FB_CTL); the few places the
; stages differ branch on that flag.  OFF by default: the golden measured ~2.3
; rows a frame for it (D1 71.7 % for the moderate titles) and silicon agrees
; (VERIFY-C6, stage 2): a still hi-colour screen is perfect, an animated one
; comes out in bands.  Only the FB mode stretches; the quad-layer stays 160x144.
;
; What differs from the first stage:
;   * footprint $4000-$CFFF (k(c) = c = 32 r + tc) and the tilemap at $D000:
;     it covers the quad-layer's chr, maps, white map and solid tiles, so
;     the entry and the way back run under forced blank (GbcFbPubBlank) and
;     the quad-layer's block walk is held off while the FB owns that VRAM;
;   * the letterbox is 16/16 (GbcTblInidisp16), window B opens at V=209 and
;     the tail ends at line 16 (!GBC_FB_DL / !GBC_FB_VL, read by GbcCapacity);
;   * the row's 20 dirty bits name SOURCE cells; the stretched columns they
;     touch (tc from floor(8c/5) to floor((8c+7)/5)) come out of GbcFbExp;
;   * the way back re-sends everything (GbcFold marks every class dirty,
;     the solid tiles and the white map are owed) before the registers go
;     back -- ~3 frames of black (C6-SPEC a.5).
; ===========================================================================

; The FB set of the second stage, in the prologue: ch0 BG1VOFS (mode 2, the
; a.4 table), ch4 W1 empty, ch5 the 16/16 letterbox; $420C = $31.
GbcFbPublish2:
    rep #$20
    lda.w #GbcTblInidisp16
    sta.w $4352
    lda.w #GbcTblW1
    sta.w $4342
    lda.w #GbcTblVofs
    sta.w $4362
    sep #$20
    stz.w $4354
    stz.w $4344
    stz.w $4364
    lda.b #$02
    sta.w $4360                 ; logical ch0 = ch6: mode 2, one register written twice
    lda.b #$0E
    sta.w $4361                 ; $210E BG1VOFS.  ⚡ SPEC: C6-SPEC a.4 and
                                ; contract sec. 14.8 say BBAD = $10, which is
                                ; $2110 = BG2VOFS; bsnes-plus shows the FB
                                ; unscrolled with it (renderer-harness fb_ppu)
    stz.b $73
    lda.b #!GBC_FB_BPL2
    sta.b !GBC_DP_HDMA
    rep #$20
    lda.w #!GBC_FB_TAIL2
    sta.b !GBC_DP_HDMA+1
    sep #$20
    lda.b $00
    beq GbcFbPub2Col
    stz.b $00
    inc.b $01
GbcFbPub2Col:
    lda.l !GBC_STAT_LONG+!GBC_ST_C6CL
    and.b #$07
    cmp.w !GBC_FB_CLFIX
    beq GbcFbPub2Mask
    jsr GbcFbColdata
GbcFbPub2Mask:
    lda.b #$70                  ; ch6 VOFS + ch5 + ch4 (ch4 is the decoy here)
    sta.w $420C
    sta.l !GBC_HDMAEN_SH
    rts

; Forced blank on the whole frame (entry and way back of the second stage):
; ch5 all $80, ch4 W1 empty, the first stage's budget figures.
GbcFbPubBlank:
    rep #$20
    lda.w #GbcTblBlank
    sta.w $4352
    lda.w #GbcTblW1
    sta.w $4342
    sep #$20
    stz.w $4354
    stz.w $4344
    stz.b $73
    lda.b #!GBC_BPLHDMA_FB
    sta.b !GBC_DP_HDMA
    rep #$20
    lda.w #!GBC_TAILBYTES_FB
    sta.b !GBC_DP_HDMA+1
    sep #$20
    lda.b $00
    beq GbcFbPubBlOut
    stz.b $00
    inc.b $01
GbcFbPubBlOut:
    lda.b #$30                  ; ch5 + ch4 (ch4 is the decoy here)
    sta.w $420C
    sta.l !GBC_HDMAEN_SH
    rts

; The cut-over of the second stage (C6-SPEC a.3, contract sec. 14.8).
GbcFbCut2:
    lda.b #$03
    sta.w $2105
    lda.b #$68
    sta.w $2107                 ; tilemap word $6800 (byte $D000)
    lda.b #$02
    sta.w $210B                 ; chr base word $2000 (byte $4000): k = c
    lda.b #$01
    sta.w $212C
    lda.b #$0C
    sta.w $2123
    lda.b #$8C
    sta.w $2125
    stz.w $212B
    lda.b #$11
    sta.w $2130
    lda.b #$20
    sta.w $2131
    stz.w $2128                 ; W2 = (0, 255): the whole width
    lda.b #255
    sta.w $2129
    jsr GbcFbColdata
    stz.w $210D
    stz.w $210D                 ; BG1HOFS = 0 (VOFS is the HDMA's)
    lda.b #(!GBC_FB_VIRQ2&$FF)
    sta.w $4209
    lda.b #(!GBC_FB_VIRQ2>>8)
    sta.w $420A                 ; window B from V=209 (the letterbox starts there)
    rep #$20
    lda.w #!GBC_FB_VIRQ2
    sta.w !GBC_FB_VL
    lda.w #!GBC_FB_DEADLINE2
    sta.w !GBC_FB_DL
    sep #$20
    lda.b #$02
    sta.w !GBC_FB_STATE
    lda.b #$01
    tsb.w !GBC_FB_FLAGS
    stz.w !GBC_FB_RCDROP
    lda.b #$03
    sta.b $82
    lda.b $80
    cmp.b #$02
    beq GbcFbCut2Pub
    stz.b $80
GbcFbCut2Pub:
    jmp GbcFbPublish2

; The LEAVING prologue of the second stage: the first one blanks the screen
; and makes the quad-layer owe everything; the ones after keep it blank until
; nothing is owed, then the registers go back (GbcFbRestore).
GbcFbProLeave2:
    lda.w !GBC_FB_FLAGS
    and.b #$10
    bne GbcFbPL2Wait
    lda.b #$1E                  ; b1 white map, b2 FB_EN = 0, b3 solid tiles,
    tsb.w !GBC_FB_FLAGS         ; b4 the way back has begun
    lda.b #$01
    trb.w !GBC_FB_FLAGS         ; the FB is no longer what is shown
    sta.b $0A                   ; the next fold marks every class dirty
    lda.b #(!GBC_VIRQ_LINE&$FF)
    sta.w $4209
    lda.b #(!GBC_VIRQ_LINE>>8)
    sta.w $420A
    rep #$20
    lda.w #!GBC_VIRQ_LINE
    sta.w !GBC_FB_VL
    lda.w #!GBC_DEADLINE
    sta.w !GBC_FB_DL
    sep #$20
    bra GbcFbPL2Blank
GbcFbPL2Wait:
    lda.b $0A
    bne GbcFbPL2Blank           ; the fold has not re-marked yet
    lda.w !GBC_FB_FLAGS
    and.b #$0E
    bne GbcFbPL2Blank           ; FB_EN = 0, the solids or the white map owed
    lda.b $3A
    and.b #$03
    bne GbcFbPL2Blank           ; OAM / CGRAM
    lda.b $80
    cmp.b #$02
    beq GbcFbPL2Blank           ; a compile is writing a set
    jsr GbcFbBacklogEmpty
    bne GbcFbPL2Blank           ; chr / maps still owed
    lda.b #$10
    trb.w !GBC_FB_FLAGS
    jsr GbcFbRestore
    clc
    rts
GbcFbPL2Blank:
    jsr GbcFbPubBlank
    sec
    rts

; The solid tiles, owed by the way back of the second stage (the FB's cells
; 448/449 sat on them).
GbcFbOwedSolid:
    rep #$20
    lda.w #32
    sta.b $1C
    sep #$20
    jsr GbcDmaGuard
    bcc GbcFbOwSolNo
    jsr GbcSolidTilesUp
    rep #$20
    lda.w #32
    jsr GbcDebit
    sep #$20
    lda.b #$08
    trb.w !GBC_FB_FLAGS
    sec
    rts
GbcFbOwSolNo:
    jsr GbcDropCount
    lda.b #$01
    sta.b $17
    clc
    rts

; ---------------------------------------------------------------------------
; GbcFbRow2 -- the second stage's half of GbcFbRow, from its ticket on: the
; row's 20 SOURCE dirty bits ($5F-$61, masked in place) become 32 stretched
; ones ($5F-$62) through GbcFbExp, then resume / full-row / scan / spans as
; in the first stage, 4 bytes wide.  Y = r on entry.
; ---------------------------------------------------------------------------
GbcFbRow2:
    lda.b $5F
    sta.b $4C
    lda.b $60
    sta.b $4E
    lda.b $61
    sta.b $64                   ; the three raw bytes
    stz.b $5F
    stz.b $60
    stz.b $61
    stz.b $62
    lda.w GbcFbBit0,y
    bne GbcFbR2Odd
    lda.b $4C                   ; even row: b0 lo, b0 hi, b1 lo, b1 hi, b2 lo
    ldx.w #$0000
    jsr GbcFbExpNib
    lda.b $4C
    lsr a
    lsr a
    lsr a
    lsr a
    ldx.w #$0040
    jsr GbcFbExpNib
    lda.b $4E
    ldx.w #$0080
    jsr GbcFbExpNib
    lda.b $4E
    lsr a
    lsr a
    lsr a
    lsr a
    ldx.w #$00C0
    jsr GbcFbExpNib
    lda.b $64
    ldx.w #$0100
    jsr GbcFbExpNib
    bra GbcFbR2Have
GbcFbR2Odd:
    lda.b $4C                   ; odd row: b0 hi, b1 lo, b1 hi, b2 lo, b2 hi
    lsr a
    lsr a
    lsr a
    lsr a
    ldx.w #$0000
    jsr GbcFbExpNib
    lda.b $4E
    ldx.w #$0040
    jsr GbcFbExpNib
    lda.b $4E
    lsr a
    lsr a
    lsr a
    lsr a
    ldx.w #$0080
    jsr GbcFbExpNib
    lda.b $64
    ldx.w #$00C0
    jsr GbcFbExpNib
    lda.b $64
    lsr a
    lsr a
    lsr a
    lsr a
    ldx.w #$0100
    jsr GbcFbExpNib
GbcFbR2Have:
    jsr GbcCapacity             ; the ticket again: the expansion above is
    sta.b $50                   ; ~100 instructions the first one never saw
    sep #$20
    ldy.b $42
    lda.w !GBC_FB_RESUME,y
    beq GbcFbR2Full
    rep #$20
    and.w #$00FF
    asl a
    asl a
    tax                         ; 4 x the resume column
    lda.b $5F
    and.l GbcFbKeep32,x
    sta.b $5F
    lda.b $61
    and.l GbcFbKeep32+2,x
    sta.b $61
    ora.b $5F
    sep #$20
    bne GbcFbR2Scan
    sec                         ; all sent already: only the ROW_DONE
    rts
GbcFbR2Full:
    rep #$20
    lda.b $5F
    and.b $61
    cmp.w #$FFFF
    sep #$20
    bne GbcFbR2Scan
    stz.b $52
    lda.b #31
    sta.b $4F
    jmp GbcFbSpan2              ; every column: one span of 2 KB
GbcFbR2Scan:
    lda.b #$FF
    sta.b $4F
    %FbScB2($5F, GbcFbCL00, GbcFbCF00)
    %FbScB2($60, GbcFbCL10, GbcFbCF10)
    %FbScB2($61, GbcFbCL20, GbcFbCF20)
    %FbScB2($62, GbcFbCL30, GbcFbCF30)
    jmp GbcFbSpan2

; A = a nibble (low 4 bits), X = its row position * 64: OR its stretched
; columns into $5F-$62.
GbcFbExpNib:
    and.b #$0F
    beq GbcFbExpOut
    rep #$20
    and.w #$000F
    asl a
    asl a
    sta.b $1A
    txa
    clc
    adc.b $1A
    tax
    lda.b $5F
    ora.l GbcFbExp,x
    sta.b $5F
    lda.b $61
    ora.l GbcFbExp+2,x
    sta.b $61
    sep #$20
GbcFbExpOut:
    rts

; cells $52..$4F (stretched columns) of row $42: one DMA, no seam.
GbcFbSpan2:
    lda.b $4F
    sec
    sbc.b $52
    inc a
    rep #$20
    and.w #$00FF
    xba
    lsr a
    lsr a
    sta.b $54                   ; 64 n
    lda.b $1E
    sec
    sbc.b $54
    bcc GbcFbS2NoRoomJ
    sbc.w #!GBC_DMACOST
    bcs GbcFbS2Line
GbcFbS2NoRoomJ:
    jmp GbcFbSpNoRoom
GbcFbS2Line:
    sta.b $64
    lda.b $54
    adc.w #!GBC_FB_SPANEQ-1
    cmp.b $50
    bcc GbcFbS2Tk
    jmp GbcFbSpNo
GbcFbS2Tk:
    eor.w #$FFFF
    sec
    adc.b $50
    sta.b $50
    lda.b $64
    sta.b $1E
    sep #$20
    lda.b $52
    asl a
    sta.b $56
    rep #$20
    ldx.b $4A
    ldy.b $56
    lda.w GbcFbColW,y
    clc
    adc.w GbcFbTileWord2,x
    sta.w $2116
    lda.w GbcFbColSrc,y
    ora.w GbcFbSrcRow,x
    sta.w $4372
    lda.b $54
    sta.w $4375
    sep #$20
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    inc.w !GBC_CTR_DMAS&$FFFF
    lda.w !GBC_CTR_BYTES&$FFFF
    clc
    adc.b $54
    sta.w !GBC_CTR_BYTES&$FFFF
    bcc GbcFbS2Ctr
    inc.w (!GBC_CTR_BYTES+2)&$FFFF
GbcFbS2Ctr:
    sep #$20
    sec
    rts

; The 32 tilemap entries of row $42 with CL $49 at word $6800 + 32 r: k = 32 r
; + tc, so the low bytes are 32 consecutive values (a ROM ramp) and every high
; byte is the same, CL << 2 | r >> 3.
GbcFbMapRow2:
    rep #$20
    lda.w #!GBC_FB_MAPEQ2
    sta.b $1C
    clc
    adc.w #!GBC_DMACOST
    cmp.b $1E
    sep #$20
    beq GbcFbM2Bud
    bcc GbcFbM2Bud
    jmp GbcFbMapNoRoom
GbcFbM2Bud:
    jsr GbcDmaGuard
    bcs GbcFbM2Go
    jmp GbcFbMapNo
GbcFbM2Go:
    stz.w $2115
    rep #$20
    lda.b $42
    asl a
    asl a
    asl a
    asl a
    asl a
    adc.w #!GBC_FB_MAPWORD2
    sta.b $64
    sta.w $2116
    lda.b $42
    and.w #$0007
    xba
    lsr a
    lsr a
    lsr a                       ; (r & 7) << 5
    adc.w #GbcFbRamp
    sta.w $4372
    lda.w #32
    sta.w $4375
    sep #$20
    stz.w $4370
    stz.w $4374
    lda.b #$80
    sta.w $420B
    lda.l !GBC_HDMAEN_SH ; the mask again: THE 5A22 TAKES THE LOWEST HDMA CHANNEL
    sta.w $420C
    rep #$20
    lda.w #32
    jsr GbcDebit
    lda.b $64
    sta.w $2116
    sep #$20
    lda.b #$80
    sta.w $2115
    lda.b $42
    lsr a
    lsr a
    lsr a
    sta.b $4C
    lda.b $49
    asl a
    asl a
    ora.b $4C
    ldy.w #32
GbcFbM2Hi:
    sta.w $2119
    dey
    bne GbcFbM2Hi
    ldx.b $42
    lda.b $49
    sta.w !GBC_FB_MAPCL,x
    jsr GbcFbDmaSetup
    sec
    rts

; --- second-stage tables -----------------------------------------------------
GbcTblInidisp16:
    db 15,  $80         ; lines 1..15   forced blank
    db 1,   $00         ; line 16       on, brightness 0 (line 17's tiles)
    db 127, $0F         ; lines 17..143
    db 65,  $0F         ; lines 144..208
    db 16,  $80         ; lines 209..224
    db $00
GbcTblBlank:
    db 127, $80         ; the whole frame forced blank
    db 97,  $80
    db $00
; the stretched columns a SOURCE nibble touches: row position j (0..4) x
; value (0..15), 4 bytes of the 32-bit mask each.  Source cell c covers pixels
; 8c..8c+7, stretched column tc covers 5tc..5tc+4 (sec. 14.3).
GbcFbExp:
!j = 0
while !j < 5
!v = 0
while !v < 16
!m = 0
!b = 0
while !b < 4
if (!v>>!b)&1 == 1
!c #= (4*!j)+!b
!t = 0
while !t < 32
if (5*!t) <= ((8*!c)+7)
if ((5*!t)+4) >= (8*!c)
!m #= !m|(1<<!t)
endif
endif
!t #= !t+1
endwhile
endif
!b #= !b+1
endwhile
    db !m&$FF, (!m>>8)&$FF, (!m>>16)&$FF, (!m>>24)&$FF
!v #= !v+1
endwhile
!j #= !j+1
endwhile
GbcFbKeep32:
!c = 0
while !c < 32
    db ($FFFFFFFF<<!c)&$FF, (($FFFFFFFF<<!c)>>8)&$FF, (($FFFFFFFF<<!c)>>16)&$FF, (($FFFFFFFF<<!c)>>24)&$FF
!c #= !c+1
endwhile
GbcFbTileWord2:
!i = 0
while !i < 18
    dw $2000+(1024*!i)
!i #= !i+1
endwhile
GbcFbRamp:
!i = 0
while !i < 256
    db !i
!i #= !i+1
endwhile

; --- tables ------------------------------------------------------------------
; The cyclic order of the rows: 8..17 first (frozen before window B opens),
; then 0..7 (they freeze inside it), by position p = CUR + i (0..34), written
; out twice so the pass indexes it without a mod 18: the row, and its byte
; (r >> 3) and bit (1 << (r & 7)) in the three 18-bit maps.
GbcFbPRow:
    db 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0, 1, 2, 3, 4, 5, 6, 7
    db 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0, 1, 2, 3, 4, 5, 6, 7
GbcFbPByte:
    db 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0
    db 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0
GbcFbPMask:
    db $01, $02, $04, $08, $10, $20, $40, $80, $01, $02, $01, $02, $04, $08, $10, $20, $40, $80
    db $01, $02, $04, $08, $10, $20, $40, $80, $01, $02, $01, $02, $04, $08, $10, $20, $40, $80
; The byte of C6_DIRTY the 20 bits of row r start in: floor(5r / 2).
GbcFbRowByte:
    db 0, 2, 5, 7, 10, 12, 15, 17, 20, 22, 25, 27, 30, 32, 35, 37, 40, 42
; First / last dirty CELL of a dirty byte, by byte position (0..2) and row
; parity (the odd rows' bit 0 is cell -4): bit + 8 b - 4 p.  Index 0 is
; never asked (the scan skips a clean byte).
macro FbCellTbl(name, base, dir)
<name>:
!i = 0
while !i < 256
if <dir> == 0
!b = 0
while !b < 7 && ((!i>>!b)&1) == 0
!b #= !b+1
endwhile
else
!b = 7
while !b > 0 && ((!i>>!b)&1) == 0
!b #= !b-1
endwhile
endif
    db (!b+<base>)&$FF
!i #= !i+1
endwhile
endmacro
%FbCellTbl(GbcFbCF00, 0, 0)
%FbCellTbl(GbcFbCL00, 0, 1)
%FbCellTbl(GbcFbCF10, 8, 0)
%FbCellTbl(GbcFbCL10, 8, 1)
%FbCellTbl(GbcFbCF20, 16, 0)
%FbCellTbl(GbcFbCL20, 16, 1)
%FbCellTbl(GbcFbCF04, -4, 0)
%FbCellTbl(GbcFbCL04, -4, 1)
%FbCellTbl(GbcFbCF14, 4, 0)
%FbCellTbl(GbcFbCL14, 4, 1)
%FbCellTbl(GbcFbCF24, 12, 0)
%FbCellTbl(GbcFbCL24, 12, 1)
%FbCellTbl(GbcFbCF30, 24, 0)    ; the second stage's fourth byte
%FbCellTbl(GbcFbCL30, 24, 1)
; Bits set in a nibble, for the entry threshold.
GbcFbPop4:
    db 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
; The masks that keep bits >= p (p = 0..24) of the 24-bit window a row's
; 20 dirty bits sit in, 3 bytes each: a row resumed after a refusal does not
; re-send the cells before its resume cell.
GbcFbKeep:
!c = 0
while !c < 25
    db ($FFFFFF<<!c)&$FF, (($FFFFFF<<!c)>>8)&$FF, (($FFFFFF<<!c)>>16)&$FF
!c #= !c+1
endwhile
; A row's 20 bits in its 3-byte window: an even row owns bits 0..19 (byte 2's
; high nibble is the next row's), an odd row bits 4..23 (byte 0's low nibble
; is the previous row's).  Masks of bytes 0 and 2, and the bit of cell 0.
GbcFbM0:
    db $FF, $F0, $FF, $F0, $FF, $F0, $FF, $F0, $FF, $F0, $FF, $F0, $FF, $F0, $FF, $F0, $FF, $F0
GbcFbM2:
    db $0F, $FF, $0F, $FF, $0F, $FF, $0F, $FF, $0F, $FF, $0F, $FF, $0F, $FF, $0F, $FF, $0F, $FF
GbcFbBit0:
    db 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4, 0, 4
; The beam's visit of row r in the BODY CLOCK (lines after V=185; words,
; X = 2 r): a line before its thaw t(0, 8r), floored, and a line after its
; freeze t(159, 8r+7), ceiled, with C6-SPEC c.1's t(x, y) = 182.35 + ((456 y
; + 92 + x) / 456) * 1.7011.  Row 0's thaw is 2.3 lines BEFORE the body, so
; any release inside one is after it: its first line is written as 0 (no
; tau is below it) and its end belongs to the next body.
GbcFbVisLo:
    dw 0, 10, 23, 37, 51, 64, 78, 91, 105, 119, 132, 146, 159, 173, 187, 200, 214, 228
GbcFbVisEnd:
    dw 12, 25, 39, 53, 66, 80, 93, 107, 121, 134, 148, 161, 175, 189, 202, 216, 229, 243
; 32 col (words, Y = 2 col), and row 2's version with the tile-64 skip.
GbcFbColW:
!i = 0
while !i < 32
    dw 32*!i
!i #= !i+1
endwhile
GbcFbColW2:
!i = 0
while !i < 20
if !i >= 6
    dw 32*(!i+1)
else
    dw 32*!i
endif
!i #= !i+1
endwhile
; col << 6, the cell's offset in its row of $E4 (words, Y = 2 col).
GbcFbColSrc:
!i = 0
while !i < 32
    dw !i<<6
!i #= !i+1
endwhile
; r << 11, the row's base in $E4 (words, X = 2 r).
GbcFbSrcRow:
!i = 0
while !i < 18
    dw !i<<11
!i #= !i+1
endwhile
; The VRAM word of the FIRST cell's tile of row r: $5000 + 32 k(20 r), with
; k(c) = c + 18 + [c >= 46] (parenthesised: asar evaluates left to right).
GbcFbTileWord:
!i = 0
while !i < 18
if !i >= 3
    dw !GBC_FB_MAPWORD+(32*((20*!i)+19))
else
    dw !GBC_FB_MAPWORD+(32*((20*!i)+18))
endif
!i #= !i+1
endwhile
; k(c)[7:0] for the 20 cells of every row (the tilemap's low bytes), and
; where each row's 20 start in it.
GbcFbMapLo:
!r = 0
while !r < 18
!c = 0
while !c < 20
!k #= (20*!r)+18+!c
if (20*!r)+!c >= 46
!k #= !k+1
endif
    db !k&$FF
!c #= !c+1
endwhile
!r #= !r+1
endwhile
GbcFbMapLoOff:
!r = 0
while !r < 18
    dw 20*!r
!r #= !r+1
endwhile
; Cells of row r whose k(c) < 256 (tilemap high byte k[9:8] = 0) and the
; rest (k[9:8] = 1): 20/0 for rows 0..10, 17/3 for row 11, 0/20 from 12 on.
; Words, so a 16-bit ldy can take them straight.
GbcFbMapHiN:
!r = 0
while !r < 18
!n = 0
!c = 0
while !c < 20
!k #= (20*!r)+18+!c
if (20*!r)+!c >= 46
!k #= !k+1
endif
if !k < 256
!n #= !n+1
endif
!c #= !c+1
endwhile
    dw !n
!r #= !r+1
endwhile
GbcFbMapHiM:
!r = 0
while !r < 18
!n = 0
!c = 0
while !c < 20
!k #= (20*!r)+18+!c
if (20*!r)+!c >= 46
!k #= !k+1
endif
if !k >= 256
!n #= !n+1
endif
!c #= !c+1
endwhile
    dw !n
!r #= !r+1
endwhile

; ---------------------------------------------------------------------------
; The stretch-V table of the SECOND stage (C6-SPEC a.4, contract sec. 14.8):
; BG1VOFS per line by HDMA mode 2 (BBAD = $0E -- the spec's $10 is BG2VOFS,
; see GbcFbPublish2 -- two bytes into the same write-twice register), so that
; visible line V = 17 + j shows FB scanline
; j - floor((j + 2) / 4): the [1, 2, 1] pattern, 144 -> 192.  Static, built
; here at assembly time; lines 1..16 carry line 17's value, 209..224 line
; 208's.  Armed by GbcFbPublish2 only, i.e. only in a session the GBCF block
; put in the second stage (see THE SECOND STAGE); the fixture that fixes its
; bytes (tests/host, c6-vofs) checks it in every image.
; ---------------------------------------------------------------------------
GbcTblVofs:
    db 18, (1024-17)&$FF, ((1024-17)>>8)&$FF       ; lines 1..18: -17
!v = 18
!g = 0
while !g < 47
    db 4, (1024-!v)&$FF, ((1024-!v)>>8)&$FF        ; four lines each: -18 .. -64
!v #= !v+1
!g #= !g+1
endwhile
    db 2, (1024-65)&$FF, ((1024-65)>>8)&$FF        ; lines 207..208: -65
    db 16, (1024-65)&$FF, ((1024-65)>>8)&$FF       ; lines 209..224: as 208
    db $00
GbcTblVofsEnd:

if !GBC_HARNESS == 1
; Where the offline harness finds the player's WRAM working set.  Only emitted
; in the harness build, so the device image is unchanged.
GbcHarnessMap:
    db "GBHM"
    dw !GBC_STATCOPY_OFF        ; status copy
    dw !GBC_CTR_OFF             ; counters, 16 B (see !GBC_CTR_* above)
    dw !GBC_SETA_OFF            ; HDMA set A
    dw !GBC_SETB_OFF            ; HDMA set B
    dw (!GBC_EF&$FFFF)          ; write window
    ; Appended, never inserted: a reader that only knows the five words above
    ; still finds them where it always did.
    dw !GBC_RCTR_OFF            ; raster counters, !GBC_RCTR_LEN bytes
    dw !GBC_LOG_OFF             ; the WRAM copy of the mid-frame log
    dw !GBC_H_MASK              ; set header: the $420C mask it implies
    dw !GBC_H_CH                ; ... then {bbad, offset lo, hi, DMAP} x 4
    dw !GBC_SNAP_OFF            ; the compile's private register snapshot
    dw !GBC_CRAM_OFF            ; the WRAM copy of the CGRAM view
    dw !GBC_DP_HDMA             ; direct page: bytes/line, tail, published roles
    dw $003A                    ; direct page: the OAM/CRAM backlog byte.
                                ; wire $02 broke the old "the byte right after
                                ; the last bitmap" rule -- the two duplicate
                                ; chr classes live at $3B/$3C, past it
    dw !GBC_FB_OFF              ; ⚡ wire $03: the framebuffer mode's state
                                ; block, !GBC_FB_LEN bytes (see !GBC_FB_*)
endif

; ===========================================================================
; S-DSP unmute -- opens the console's cartridge-DAC audio path.
;
; WHY: with no program uploaded to the S-SMP, the S-DSP stays in its IPL
; reset/mute state, and in that state the console's analogue path GATES the
; cartridge DAC output -- the FPGA can drive I2S perfectly and nothing reaches
; the jack.  Same mechanic, same cure, same 82-byte payload as snes/sfx.a65
; (menu), snes/nes/nes_apu.a65 and snes/sms/sms_snes.asm.  MIRROR, not a
; source: touched snes/sfx.a65, touch this too.
;
; ENTRY CONTRACT (true at the call site in GbcInit): native mode, DBR=$00,
; DP=$0000, NMI off ($4200=0 since the reset), A 8-bit / X,Y 16-bit.  Returns
; carry clear = stub running, carry set = timed out (boot continues, at worst
; silent).
; ===========================================================================

!GBC_APU_BEEP = 0              ; 1 = instrumented payload (209 bytes): the same
                               ;     unmute plus a 0.5 s 2 kHz beep at boot, so
                               ;     "no sound" can be split into "the S-DSP
                               ;     gate is still shut" (no beep) and "the gate
                               ;     is open, the FPGA side is the problem"
                               ;     (beep, then silence).  BRING-UP ONLY.
                               ; 0 = PRODUCTION: 82 bytes, unmute only, silent.
                               ;     Set this back to 0 before shipping.

!APUIO0 = $2140
!APUIO1 = $2141
!APUIO2 = $2142

if !GBC_APU_BEEP == 1
!GBC_APU_STUB_ADDR = $0300     ; upload base: DIR table + BRR + code
!GBC_APU_EXEC_ADDR = $0320     ; code entry (DIR must own $0300)
!GBC_APU_STUB_LEN  = $00D1     ; 209
else
!GBC_APU_STUB_ADDR = $0300
!GBC_APU_EXEC_ADDR = $0300
!GBC_APU_STUB_LEN  = $0052     ; 82
endif

!GBC_APU_WAIT_POLLS  = $0800   ; per-wait ceiling
!GBC_APU_POLL_BUDGET = $FFFF   ; aggregate ceiling across the whole routine

GbcApuUnmute:
    sep #$20
    rep #$10
    ldy.w #!GBC_APU_POLL_BUDGET

    ; --- 1) wait for the IPL to publish $BBAA ---------------------------------
    ; own ceiling ~5x the handshake one: the IPL spends ~2.4 ms clearing page
    ; zero before it answers, and a timeout here is indistinguishable from
    ; "the analogue gate was not the cause".
    ldx.w #$4000
GbcApuHello:
    lda !APUIO0
    cmp.b #$aa
    bne GbcApuHelloNext
    lda !APUIO1
    cmp.b #$bb
    beq GbcApuHelloOk
GbcApuHelloNext:
    dey
    beq GbcApuFail
    dex
    bne GbcApuHello
    bra GbcApuFail
GbcApuHelloOk:

    ; --- 2) open the transfer -------------------------------------------------
    ldx.w #!GBC_APU_STUB_ADDR
    stx !APUIO2                 ; $2142/$2143 = destination
    lda.b #$cc
    sta !APUIO1                 ; non-zero starts the transfer
    sta !APUIO0
    jsr GbcApuWait
    bcs GbcApuFail

    ; --- 3) payload, byte by byte --------------------------------------------
    ; X is both the table index and the protocol index; the payload is < 256
    ; bytes so the index never wraps.
    ldx.w #$0000
GbcApuSend:
    lda.l GbcApuStub,x
    sta !APUIO1
    txa
    sta !APUIO0
    jsr GbcApuWait
    bcs GbcApuFail
    inx
    cpx.w #!GBC_APU_STUB_LEN
    bne GbcApuSend

    ; --- 4) end of transfer: entry address + index+2 --------------------------
    ldx.w #!GBC_APU_EXEC_ADDR
    stx !APUIO2
    stz !APUIO1                 ; 0 = execute, no further block
    lda !APUIO0
    inc a
    inc a
    sta !APUIO0
    jsr GbcApuWait
    bcs GbcApuFail
    clc
    rts

GbcApuFail:
    sec
    rts

; A(8) = expected echo, Y(16) = remaining global budget.
; carry clear = echoed, carry set = timed out.  A and X preserved.
GbcApuWait:
    phx
    ldx.w #!GBC_APU_WAIT_POLLS
GbcApuWaitPoll:
    cmp !APUIO0
    beq GbcApuWaitOk
    dey
    beq GbcApuWaitTo
    dex
    bne GbcApuWaitPoll
GbcApuWaitTo:
    plx
    sec
    rts
GbcApuWaitOk:
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
GbcApuStub:
if !GBC_APU_BEEP == 1
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


; ---- GBCF: the player's config block (image offset $7F80, 16 bytes) ----
; The MCU patches it in PSRAM after staging the image, before the reset:
;   +0..+3 "GBCF"   +4 block version ($01)
;   +5     stretch: 0 = the FB mode's first stage (160x144), 1 = the second
;          (256x192, THE SECOND STAGE) -- CFG GbcStretch
;   +6..+15 reserved, $00
; The MCU writes +5 only when it finds the magic and version 1, so an old
; player (no block) is never touched, and an old MCU leaves the 0 assembled
; here.  No wire bump: the bridge never sees any of it.  Read once, by
; GbcFbInit.
assert pc() <= $00FF80, "the player grew into the GBCF block"
org $00FF80
GbcCfg:
    db "GBCF"
    db $01
GbcCfgStretch:
    db $00
    db $00, $00, $00, $00, $00, $00, $00, $00, $00, $00

; ---- LoROM header (so smc_id detects LoROM) ----
; NO SaveRAM: sgb_update_romprops refuses a player image that declares one, and
; the cart's own RAM is the FPGA's business (PSRAM $E00000), never the SNES's.
org $00FFC0
    db "GBC PLAYER           "    ; 21-byte title
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
if !GBC_VIRQ == 1
    dw IRQ
else
    dw Stub
endif
org $00FFF4
    dw Stub
    dw Stub
    dw Stub
    dw Stub
    dw Reset
    dw Stub
