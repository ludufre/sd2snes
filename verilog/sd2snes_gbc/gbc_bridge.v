`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Module Name:    gbc_bridge
// Project Name:   sd2snes_gbc
// Target Devices: EP4CE15 (Mk.III)
// Description:    SNES <-> Game Boy bridge.
//
// This is the piece that replaces the ICD2.  Where the ICD2 handed the SNES
// four row buffers of 2bpp pixels through $6000/$7000, this hands the SNES a
// set of read-only "views" over the GB's own state in banks $E0-$E3 and takes
// commands as byte writes in $EF00xx.  The SNES side re-renders the picture;
// the FPGA never composes one.
//
// What exists now:
//   - the $EF00xx command decode (joypad, COMMIT, GO, SYNC, CONSUMED)
//   - the player's counter mailbox at $EF0010-$EF0019 (GBDG +20..+29)
//   - the joypad -> P1 mapping and the GB reset (CPU_RST = ~go_r)
//   - ALL SIX VIEWS: BG chr, OBJ chr, the four maps, CGRAM, OAM, status
//     (the four map windows are the GB's TWO MAPS x content/carpet, not the
//     SNES's two layers -- see "4.3 maps" below and contract section 4.3)
//   - the dirty accumulator behind COMMIT ("set wins")
//   - the LY=0 snapshot with double-buffered OAM/CRAM/registers, the busy flag
//     CONSUMED clears, and the publish an LY=0 inside the read window defers
//   - the mid-frame write log at $E2:0400, three buffers rotated at LY=0
//   - COMPAT_RAW, the three uncomposed DMG palettes at status +$40..+$57
//   - GBDG telemetry for the MCU at PSRAM 0x810000
//   - the frame dot counter the genlock loop steers against
//   - wire $03 (contract section 14): the framebuffer view in bank $E4 and the
//     C6 control/ROW_DONE registers at $EF0006/$EF0007 (c6_fb.v), the live C6
//     status bytes +58..+91, COLW_N, the sixth mailbox word and GBDG +2A..+35
//
// -----------------------------------------------------------------------------
// THE READ PIPELINE (contract section 3, the rule marked as adversarial)
//
//   cycle N    : SNES_ADDR is stable.  Combinational decode drives the VRAM
//                port B addresses and the snapshot RAM read addresses.  Nothing
//                waits for a strobe -- a DMA read's /RD is only ~190 ns wide and
//                the deglitched address is already several cycles old when it
//                arrives, so a design that armed on SNES_RD_start would have no
//                margin left.  (That is a real bug class: the NES core's
//                renderer lost a frame to it.)
//   cycle N+1  : the RAMs' registered outputs are valid and the stage-1 flags
//                (which view, which byte, which bank) have been registered
//                alongside them, so the composition never has to re-derive the
//                address it belongs to.
//   cycle N+2  : the composed byte is in view_data_r, which is DATA_OUT.
//
// Two cycles for the views of $E0-$E3.  The STATUS BLOCK is the one exception
// below that and takes ONE cycle: it reads no RAM, so there is nothing to wait
// for, and $E2:0200 is the byte every liveness check reads.  The FRAMEBUFFER
// view in bank $E4 (wire $03, c6_fb.v) is the one exception above it and takes
// THREE: its RAM read is registered (after the plane pick), which is what
// closed 84 MHz with the chunky + stretch read -- and since its byte enters
// view_data_r FIRST (the deepest path), a $E0-$E3 read that follows a $E4
// address within two cycles also settles only on the third.  The contract
// therefore says "<= 3 cycles for the whole window" (section 3).  Mixing the
// latencies is safe because the SNES holds an address for eight cycles or more
// before it latches: the settled value is always the byte belonging to the
// address currently on the bus, whichever branch produced it.
//
// -----------------------------------------------------------------------------
// WHY THE SNAPSHOT IS DOUBLE BUFFERED
//
// The copy engine needs 161 cycles to move OAM (160 B) and CRAM (128 B) into
// the snapshot buffers.  The contract forbids a view from ever returning a
// half-copied snapshot, so the buffers are duplicated and a single "published"
// pointer flips on the cycle the last byte lands.  During the copy the views
// read the OTHER buffer, i.e. the previous frame's snapshot -- a valid answer,
// and the one the player is allowed to see.  The alternative (stalling the
// SNES) does not exist on this bus.
//
// -----------------------------------------------------------------------------
// THE MID-FRAME LOG (contract section 8)
//
// Everything above describes ONE instant per frame.  A game that changes SCX,
// a palette or LCDC halfway down the screen is not describable that way at all:
// the snapshot can only say what the register was at LY=0, and the SNES side
// re-renders the whole frame from it.  The log is the missing half -- every
// write the re-render cannot express as a per-frame constant, with the line it
// takes effect on, so the player can turn it back into per-line HDMA.
//
// It is TWO 512-entry buffers and a pointer, not a queue:
//
//   - the GB writes into the LIVE buffer, four bytes per entry
//     ({ly_eff, kind, idx, value});
//   - at LY=0 -- the same event the snapshot uses -- the live buffer becomes
//     the PUBLISHED one and the other becomes live and is emptied;
//   - the SNES only ever reads the published buffer, so it reads a whole
//     frame's log, frozen, while the next frame is being recorded.
//
// The pointer flip happens AT LY=0 when the read window is closed: the log
// costs zero cycles to "copy" -- the flip IS the copy.  An LY=0 that lands
// INSIDE the read window (COMMIT .. CONSUMED) still takes its snapshot: OAM,
// CRAM and the registers go into the hidden half as always and their publish
// waits for CONSUMED, and the log uses a THIRD buffer for it -- the frame that
// just ended becomes PENDING (published together with the snapshot), the next
// frame starts recording in the free buffer, and the published one the player
// may be reading is not touched.  It used to be a skip instead, and in the
// Exato mode (no genlock, LY=0 drifting ~1.6 SNES lines a frame) LY=0 spent
// ~50 frames in a row inside the window: ~48 consecutive snapshots skipped
// every 2.7 s, a 0.8 s freeze and a jump, measured on a Mk.III.
//
// The write port only ever addresses the live half and the read port only ever
// the published half, so unlike a general M9K there is no read-during-write
// case here to define: the buffer bit makes them disjoint.
//
// Entries past LOG_N read back $00 rather than whatever the previous frame left
// in that slot.  A stale entry from two frames ago is indistinguishable from a
// real one if the player miscounts, and $00 is both a deterministic answer for
// a byte-exact golden and a harmless one (ly_eff = 0, kind = SCX, value = 0).
//
// Reference: GBC-CORE-CONTRACT.md sections 3, 4, 5, 6, 7, 8, 9, 10.
//////////////////////////////////////////////////////////////////////////////////
`include "config.vh"

module gbc_bridge(
  input         RST,             // cold reset (SNES reset strobe)
  input         CLK,             // 84 MHz base clock

  // SNES cartridge bus, deglitched by main.v
  input         SNES_WR_end,
  input  [23:0] SNES_ADDR,
  input  [7:0]  DATA_IN,
  input         VIEW_ENABLE,     // $E0-$E3
  input         EF_ENABLE,       // $EF00xx
  output [7:0]  DATA_OUT,

  // GB core
  input         CE1,             // 1x clock enable (free-running liveness counter)
  input         BUS_EDGE,        // GB M-cycle edge, for reset alignment
  input  [15:0] STARVATION,      // sgb_cpu: bus edges that caught a PSRAM access still in flight (GBDG +0C)
  input  [15:0] DILATION,        // sgb_cpu: M-cycles the GB clock was stretched to fit one (GBDG +1C)
  output        CPU_RST,
  input  [1:0]  P1I,             // GB P1[5:4]: {P15 buttons, P14 d-pad}, active low
  output [3:0]  P1O,             // GB P1[3:0], active low
  input         PPU_DOT_EDGE,
  input         PPU_VSYNC_EDGE,
  input         LCD_ON,          // LCDC.7

  // CGB state and taps out of sgb_cpu.v (contract sections 5-8).
  input         COMPAT,          // KEY0: DMG compatibility mode
  // C6 (contract section 14): the final LCD pixel stream out of sgb_cpu.v,
  // one pixel per dot, CGB colour, priority resolved -- what the FB stores.
  input         C6_PX_VALID,
  input  [7:0]  C6_PX_X,
  input  [7:0]  C6_PX_Y,
  input  [14:0] C6_PX_BGR,

  input         TAP_VRAM_WE,
  input         TAP_VRAM_BANK,
  input  [12:0] TAP_VRAM_ADDR,
  input         TAP_OAM_WE,
  input         TAP_CRAM_WE,
  input  [6:0]  TAP_CRAM_IDX,
  input  [7:0]  TAP_CRAM_DATA,

  input         TAP_LY0,         // first dot of mode 2 on line 0
  input         TAP_VBLANK,      // entry into LY=144

  input  [7:0]  TAP_LY,          // live LY, for LY_SYNC
  input  [7:0]  TAP_LCDC,
  input  [7:0]  TAP_SCX,
  input  [7:0]  TAP_SCY,
  input  [7:0]  TAP_WX,
  input  [7:0]  TAP_WY,
  input  [7:0]  TAP_BGP,
  input  [7:0]  TAP_OBP0,
  input  [7:0]  TAP_OBP1,
  input  [7:0]  TAP_OPRI,
  input  [7:0]  TAP_KEY0,
  input  [7:0]  TAP_KEY1,
  input  [7:0]  TAP_VBK,
  input  [7:0]  TAP_SVBK,
  input         TAP_SPEED_REQ,

  input         TAP_MF_VALID,
  input  [3:0]  TAP_MF_KIND,
  input  [5:0]  TAP_MF_IDX,
  input  [7:0]  TAP_MF_VALUE,
  input  [7:0]  TAP_MF_LY,
  input         TAP_MF_BEFORE_MODE3,

  // VRAM / OAM / CRAM port B.  The bridge wins the cycle it asks for; the MCU
  // debug pipe gets what is left, which outside HLT_REQ is what the contract
  // says (section 1).
  output        VRAM0_B_REQ,
  output [12:0] VRAM0_B_ADDR,
  input  [7:0]  VRAM0_B_DATA,
  output        VRAM1_B_REQ,
  output [12:0] VRAM1_B_ADDR,
  input  [7:0]  VRAM1_B_DATA,
  output        OAM_RD_REQ,
  output [7:0]  OAM_RD_ADDR,
  input  [7:0]  OAM_RD_DATA,
  output        CRAM_RD_REQ,
  output [6:0]  CRAM_RD_ADDR,
  input  [7:0]  CRAM_RD_DATA,

  // genlock
  output        SYNC_STROBE,     // $EF0004 write, one CLK pulse
  output [16:0] FRAME_DOT_CTR,   // 0..70223, the PI loop's error input
  input         LOCKED,
  input  [15:0] SYNC_ERR_LAST,   // gbc_clk's last phase error, dots, two's compl.

  // MCU debug read path (GBDG at PSRAM 0x810000-0x81003F)
  input  [11:0] DBG_ADDR,
  output [7:0]  DBG_DATA_OUT
);

// Wire version.  Bumped whenever a format, address or semantic in the
// contract changes; the .bi3 and gbc_snes.bin are never shipped apart.
parameter [7:0] GBC_WIRE_VER = 8'h03;
// GBDG layout revision (GBDG +1E).  Independent of the wire version: the GBDG
// is read by the MCU, not by the player, so growing it never forces a reflash
// of the pair.  $01 = the counter mailbox at +20..+29; $02 = the sixth mailbox
// word (+2A, C6MODE) and the C6 counters at +2C..+35.
parameter [7:0] GBDG_REV = 8'h02;

//-------------------------------------------------------------------
// $EF00xx command decode
//-------------------------------------------------------------------

// The player always uses 8-bit stores here (contract invariant 4), so a
// register is one address and there is no word case to consider.
wire        ef_wr = EF_ENABLE & SNES_WR_end;

wire        wr_pad_lo   = ef_wr & (SNES_ADDR[7:0] == 8'h00);
wire        wr_pad_hi   = ef_wr & (SNES_ADDR[7:0] == 8'h01);
wire        wr_commit   = ef_wr & (SNES_ADDR[7:0] == 8'h02);
wire        wr_go       = ef_wr & (SNES_ADDR[7:0] == 8'h03);
wire        wr_sync     = ef_wr & (SNES_ADDR[7:0] == 8'h04);
wire        wr_consumed = ef_wr & (SNES_ADDR[7:0] == 8'h05);
wire        wr_c6ctl    = ef_wr & (SNES_ADDR[7:0] == 8'h06);   // C6_CTL  (section 14.2)
wire        wr_c6rowdn  = ef_wr & (SNES_ADDR[7:0] == 8'h07);   // ROW_DONE (section 14.2)

// Counter mailbox, $EF0010-$EF001B: six 16-bit words, word i at $EF0010+2i
// (lo) / +2i+1 (hi).  Not a strobe -- plain data the bridge holds for the MCU
// (GBDG +20..+2B, contract section 10), because the player's own counters live
// in SNES WRAM and nothing in this core can read that.  Optional in both
// directions: an old player never writes here and the words read $0000; an
// old bridge decodes none of these addresses and drops the stores.  Word 5
// ($EF001A/1B, GBDG +2A) is C6MODE, wire $03: the player's FB-mode state and
// the rows it released last frame (section 14.9).
wire        wr_mbx      = ef_wr & (SNES_ADDR[7:4] == 4'h1) & (SNES_ADDR[3:0] <= 4'hB);
wire [2:0]  mbx_idx     = SNES_ADDR[3:1];

reg  [7:0]  pad_lo_r;   // $4218
reg  [7:0]  pad_hi_r;   // $4219
reg         go_r;

reg  [7:0]  go_seen_r;
reg  [15:0] commit_ctr_r;
reg  [15:0] consumed_ctr_r;
reg  [15:0] snap_skipped_r;
reg  [15:0] commit_busy_ctr_r;   // COMMITs ignored because the window was open
reg  [7:0]  ly_sync_r;
reg         busy_r;

// Mailbox storage.  The low byte waits in mbx_lo_r, tagged with its word, and
// the word only changes on the matching high-byte write, so an MCU read that
// lands between the two stores sees last frame's value, never half of each.
// That makes the WORD atomic, not the MCU's read of it: the DBG pipe serves
// one byte per request, so the MCU's own +20 and +21 reads can straddle a
// commit (old low byte, new high byte; contract section 3).
// A high byte whose low byte did not come first (a lost store, a reordered
// publish) changes nothing and drops the pending low byte.
// Six named words rather than an array: flops only (no M9K inference), and
// an out-of-range index has nowhere to land.
reg  [15:0] mbx0_r, mbx1_r, mbx2_r, mbx3_r, mbx4_r, mbx5_r;
reg  [7:0]  mbx_lo_r;
reg  [2:0]  mbx_lo_idx_r;
reg         mbx_lo_ok_r;

assign SYNC_STROBE = wr_sync;

//-------------------------------------------------------------------
// Snapshot state (contract section 7)
//-------------------------------------------------------------------

// Published set: what every view and the status block read.
reg  [7:0]  sn_lcdc_r, sn_scx_r, sn_scy_r, sn_wx_r, sn_wy_r;
reg  [7:0]  sn_bgp_r, sn_obp0_r, sn_obp1_r, sn_opri_r, sn_key1_r;
reg         sn_compat_r, sn_lcd_on_r, sn_locked_r, sn_first_frame_r;

// Pending set: latched at the snapshot instant, published when the copy ends.
// The registers are captured in one cycle but must not become visible until
// OAM and CRAM have finished copying, or a player would read the new scroll
// against the old sprites.
reg  [7:0]  pd_lcdc_r, pd_scx_r, pd_scy_r, pd_wx_r, pd_wy_r;
reg  [7:0]  pd_bgp_r, pd_obp0_r, pd_obp1_r, pd_opri_r, pd_key1_r;
reg         pd_compat_r, pd_lcd_on_r, pd_locked_r, pd_first_frame_r;

reg         snap_valid_r;
reg         snap_buf_r;        // which half of the snapshot RAMs is published
reg         snap_run_r;        // copy engine busy
reg         snap_pub_pend_r;   // copy finished inside the read window
reg  [7:0]  snap_cnt_r;        // 0..160, the byte being addressed
reg  [7:0]  snap_cnt_d1_r;     // the byte whose data is arriving
reg         snap_rd_d1_r;      // a read was issued last cycle
reg  [23:0] snap_oam_word_r;   // {tile, X, Y} being assembled
reg         first_frame_arm_r; // set by LCD off->on, consumed by the next LY=0
reg         lcd_off_pend_r;    // an LCDC.7 1->0 still waiting for its snapshot

// Did the OAM / CRAM the views expose actually CHANGE?  DIRTY_MISC bits 0 and 1
// are about the published snapshot, not about live writes: the views are the
// snapshot, so a write that happened between LY=0 and the COMMIT belongs to the
// NEXT frame's word.  *_wr_pend_r accumulates live writes since the last
// capture; *_snap_diff_r is that accumulator frozen at the capture, i.e. "the
// buffer being published differs from the one it replaces".
reg         oam_wr_pend_r,  cram_wr_pend_r;
reg         oam_snap_diff_r, cram_snap_diff_r;

reg  [15:0] seq_r;

wire        snap_wbuf = ~snap_buf_r;

//-------------------------------------------------------------------
// Snapshot control (contract section 7)
//-------------------------------------------------------------------

reg  lcd_on_d1_r;
wire lcd_on_fall = lcd_on_d1_r & ~LCD_ON;
wire lcd_on_rise = ~lcd_on_d1_r & LCD_ON;

// CONSUMED closes the window on the cycle it is written, so an LY=0 landing on
// that same cycle is NOT inside it: it swaps the log on the spot instead of
// parking it as pending, and the copy that follows publishes as it finishes.
wire busy_eff   = busy_r & ~wr_consumed;

// The engine can accept a capture only when nothing is in flight AND nothing is
// waiting to be published: the pending buffer is the one a new capture would
// overwrite.  (So a second LY=0 inside one read window is still a skip -- the
// window would have to outlast a whole Game Boy frame.)
wire snap_ready = ~snap_run_r & ~snap_pub_pend_r;

// The instant is the first dot of mode 2 on line 0, plus the LCDC.7 1->0
// transition so that "the LCD went off" is published at all.
//
// The LCD-off trigger is a PENDING FLAG rather than the edge itself, and that
// is not tidiness.  With the LCD off there is no LY=0 to come back on: an edge
// that arrived while the player's read window was open would be dropped, and
// the status block would keep saying LCD_ON = 1 until the game switched the LCD
// back on -- the player would miss the white screen entirely.
//
// The read window does NOT block an LY=0 capture: the copy fills the hidden
// half, and the publish (snap_publish) and the log's pending buffer wait for
// CONSUMED.  It does block the deferred LCD-off one, which publishes the moment
// it is taken.
wire snap_req   = TAP_LY0 | (lcd_off_pend_r & ~busy_eff);
wire snap_take  = snap_req & snap_ready;

// EVERY LY=0 that does not start a capture is a skip, whatever stopped it --
// a copy still running or a publish still waiting.  A lost LY=0 is a lost
// FIRST_FRAME.
wire snap_skip  = TAP_LY0 & ~snap_take;

// The copy engine has moved its last byte...
wire        snap_done    = snap_run_r & snap_rd_d1_r & (snap_cnt_d1_r == 8'd159);
// ...but publishing it INSIDE the read window would hand the player a snapshot
// half of its own frame is already built on: SEQ would move under it and the
// OAM it copied twenty lines ago would no longer be the one the status block
// describes.  A copy that lands inside the window waits for CONSUMED.
wire        snap_publish = (snap_done & ~busy_eff) | (snap_pub_pend_r & wr_consumed);

//-------------------------------------------------------------------
// Snapshot RAMs
//-------------------------------------------------------------------

// OAM is stored ONE SPRITE PER WORD, not one byte per byte: a single OAM view
// byte needs Y, X, tile and attribute together (visibility depends on Y and X,
// the tile number on the attribute), and a byte-wide RAM would need four reads
// where the contract allows two cycles in total.  40 sprites x 2 buffers.
reg  [31:0] oam_snap_r[0:127];
reg  [31:0] oam_snap_q_r;

// CRAM stays byte-wide: one CGRAM view byte is one palette byte, and which one
// is pure index arithmetic (the compatibility remap included).
reg  [7:0]  cram_snap_r[0:255];
reg  [7:0]  cram_snap_q_r;

// An M9K powers up at zero (power_up_uninitialized defaults to FALSE), so this
// is what the hardware does; writing it down keeps RTL simulation from reading
// X out of a buffer nobody has snapshotted into yet.
integer snap_i;
initial begin
  for (snap_i = 0; snap_i < 128; snap_i = snap_i + 1) oam_snap_r[snap_i]  = 32'd0;
  for (snap_i = 0; snap_i < 256; snap_i = snap_i + 1) cram_snap_r[snap_i] =  8'd0;
end

//-------------------------------------------------------------------
// Mid-frame log -- write side (contract section 8)
//-------------------------------------------------------------------

// 3 buffers x 512 entries x 32 bits, as ONE 1536x32 simple dual-port array:
// the buffer number is the top two address bits.  Thirty-two bits wide rather
// than four byte-wide arrays because an entry is written as a unit and read as
// a unit -- the SNES picks its byte out of the registered word, which costs a
// mux on the data instead of a second address decode.  49152 bits = 6 M9K.
// The three roles rotate: PUBLISHED (what the SNES reads), LIVE (what the GB is
// writing) and, only between an LY=0 inside the read window and the publish at
// CONSUMED, PENDING (the frame that ended there).
//
// The inference template is the one cram_snap_r/oam_snap_r above already use
// and that sgb_cpu.v's cram_r explains: ONE write port, ONE read whose result
// lands in a register, and no combinational read anywhere.  Reading the array
// combinationally here would ask the fitter for 32768 flip-flops.
// max_depth 256: six 256x32 M9Ks.  Left to itself the fitter rounds the depth
// up to 2048 and asks for eight, which is one more than the device has left.
(* ramstyle = "no_rw_check", max_depth = 256 *) reg  [31:0] log_ram_r[0:1535];
reg  [31:0] log_q_r;

reg  [1:0]  log_pub_r;        // the buffer the SNES reads
reg  [1:0]  log_live_r;       // the buffer the GB writes
reg  [1:0]  log_pend_r;       // a finished frame waiting for its publish
reg         log_pend_v_r;     // ... and whether there is one
reg  [9:0]  log_n_pend_r;     // its LOG_N, LOG_OVF and COLW_N
reg         log_ovf_pend_r;
reg  [15:0] colw_pend_r;
reg  [9:0]  log_wr_idx_r;     // 0..512, saturates at 512 (= overflow)
reg         log_ovf_live_r;   // the live buffer dropped at least one entry
reg  [9:0]  log_n_pub_r;      // published LOG_N, status +23..24
reg         log_ovf_pub_r;    // published LOG_OVF, flags0 bit 6
reg  [15:0] log_ovf_ctr_r;    // GBDG +0E: frames whose log overflowed
// COLW_N (wire $03, contract invariant 46): the BCPD/OCPD writes of a frame
// that the log WOULD take -- counted before the 512 cap, so it keeps counting
// where LOG_N saturates.  Same publication rule as LOG_N: it turns over with
// the log flip at LY=0 and restarts with the live buffer.  Saturating.
reg  [15:0] colw_live_r;
reg  [15:0] colw_pub_r;       // status +90..91

integer log_i;
initial begin
  for (log_i = 0; log_i < 1536; log_i = log_i + 1) log_ram_r[log_i] = 32'd0;
end

// Byte 0, ly_eff: the line the write takes effect on.  A write that lands
// before this line's mode 3 still changes what mode 3 fetches, so it counts for
// LY; from mode 3 onwards the line is already being drawn and the first line it
// can change is LY+1.  The tap only fires for LY <= 143, so LY+1 is at most 144
// and the 8-bit add never wraps.
wire [7:0]  log_ly_eff = TAP_MF_BEFORE_MODE3 ? TAP_MF_LY : (TAP_MF_LY + 8'd1);

// Bytes 1..3: kind, the pre-increment BCPS/OCPS index (zero for the eight plain
// registers), and the byte written.  Little endian like everything on this
// wire, so byte 0 is bits [7:0].
wire [31:0] log_word = {TAP_MF_VALUE, 2'b00, TAP_MF_IDX, 4'h0, TAP_MF_KIND,
                        log_ly_eff};

// A write on the very cycle of LY=0 is NOT logged.  Contract section 7: "a
// write that completes in that same dot is in the snapshot and not in the log;
// from the next dot on it enters the log with ly_eff = 0".  One CLK is the
// finest grain the bridge has to resolve that boundary with, and dropping is
// the side the contract names.  (A write one cycle later is logged normally,
// with ly_eff = 0, which is the overwhelmingly more likely case: a dot is about
// twenty CLK wide.)
//
// The rule is enforced twice: here, so the entry is not written at all, and in
// the counter below, where the LY=0 branch wins over the write branch.  The
// second one is what a player can observe; the first keeps the RAM from taking
// a write into a slot nothing will ever read -- cheap insurance against a later
// change that makes that slot readable.
//
// WITH THE LCD OFF NOTHING IS LOGGED EITHER, and that is not an optimisation.
// The log exists to say "this register changed PART WAY DOWN a frame that is
// being drawn"; with LCDC.7 clear nothing is being drawn, and the snapshot at
// the next LY=0 already carries the final value of every register.  Worse, the
// tap keeps firing there -- it only gates on LY <= 143, and the hardware parks
// LY at 0 with the LCD off -- while the PPU is stopped and there is therefore
// no LY=0 to restart the live buffer.  A game that initialises for a while with
// the screen off would fill the log with writes that describe no line, and
// could reach 512 and raise LOG_OVF over nothing.
wire        log_wr   = TAP_MF_VALID & ~TAP_LY0 & LCD_ON;
wire        log_full = (log_wr_idx_r == 10'd512);
wire        log_take = log_wr & ~log_full;
wire        colw_wr  = log_wr & ((TAP_MF_KIND == 4'd8) | (TAP_MF_KIND == 4'd9));

wire [10:0] log_wa   = {log_live_r, log_wr_idx_r[8:0]};

// The swap is the LY=0 that actually takes a snapshot.  Outside the read window
// the live buffer is published on the spot and the old published one becomes
// live; inside it the live buffer becomes PENDING and the third, free buffer
// becomes live, and the pending one is published with the snapshot
// (the "pending log goes public" block below).  A skipped LY=0 (a copy or a publish still in flight) leaves
// the published log and LOG_N exactly as they were, but the live buffer
// restarts anyway, so the skipped frame simply loses its log (section 7).
wire        log_swap       = TAP_LY0 & snap_take & ~busy_eff;
wire        log_swap_defer = TAP_LY0 & snap_take &  busy_eff;
// With no pending buffer, published and live are two of {0,1,2}: the third.
wire [1:0]  log_free       = 2'd3 - log_pub_r - log_live_r;

always @(posedge CLK) begin
  if (RST) begin
    log_pub_r      <= 2'd0;
    log_live_r     <= 2'd1;
    log_pend_r     <= 2'd2;
    log_pend_v_r   <= 1'b0;
    log_n_pend_r   <= 10'd0;
    log_ovf_pend_r <= 1'b0;
    colw_pend_r    <= 16'd0;
    log_wr_idx_r   <= 10'd0;
    log_ovf_live_r <= 1'b0;
    log_n_pub_r    <= 10'd0;
    log_ovf_pub_r  <= 1'b0;
    log_ovf_ctr_r  <= 16'd0;
    colw_live_r    <= 16'd0;
    colw_pub_r     <= 16'd0;
  end
  else if (TAP_LY0 | lcd_on_fall) begin
    colw_live_r    <= 16'd0;
    // LCDC.7 1->0 empties the live buffer as well as LY=0 does.  The frame it
    // was recording was abandoned half way through, so its entries describe
    // lines that were never drawn; and since nothing accumulates while the LCD
    // is off, this is what makes the FIRST LY=0 after the LCD comes back on
    // publish an empty log instead of a fragment from before the blackout.
    // (The 1->0 edge never swaps: log_swap needs TAP_LY0, and the deferred
    // LCD-off snapshot is not one.)
    log_wr_idx_r   <= 10'd0;
    log_ovf_live_r <= 1'b0;
    if (log_swap) begin
      log_pub_r     <= log_live_r;
      log_live_r    <= log_pub_r;
      log_n_pub_r   <= log_wr_idx_r;
      log_ovf_pub_r <= log_ovf_live_r;
      colw_pub_r    <= colw_live_r;
    end
    if (log_swap_defer) begin
      log_pend_r     <= log_live_r;
      log_live_r     <= log_free;
      log_pend_v_r   <= 1'b1;
      log_n_pend_r   <= log_wr_idx_r;
      log_ovf_pend_r <= log_ovf_live_r;
      colw_pend_r    <= colw_live_r;
    end
  end
  else if (log_wr) begin
    if (colw_wr & ~(&colw_live_r)) colw_live_r <= colw_live_r + 16'd1;
    if (log_full) begin
      log_ovf_live_r <= 1'b1;
      // GBDG +0E counts FRAMES whose log overflowed, not entries dropped: the
      // contract's trigger is "passing 512", which happens once per buffer, and
      // a per-entry count would wrap in a couple of minutes of a game that
      // overflows every frame.  It saturates for the same reason -- the phase
      // gate is "this reads zero", and a wrap could make 65536 overflows look
      // like none.
      if (~log_ovf_live_r & ~(&log_ovf_ctr_r)) log_ovf_ctr_r <= log_ovf_ctr_r + 16'd1;
    end
    else log_wr_idx_r <= log_wr_idx_r + 10'd1;
  end

  // The deferred LCD-off publish (a snap_take that is not an LY=0) is the one
  // place outside the read window where "the screen went dark" becomes visible,
  // so the published log retires there too.  With nothing being drawn, LOG_N > 0
  // would describe a frame that is no longer on the screen -- and would keep
  // describing it across a reset of the Game Boy, which clears LCDC.7 but not
  // this module -- and the player would go on compiling its colour writes over
  // the white screen.  lcd_off_pend_r is a registered copy of the 1->0 edge, so
  // this never lands on the cycle of the branch above; and it needs ~busy_eff
  // like every snap_take, so LOG_N never moves under a player that is reading.
  if (~RST & snap_take & ~TAP_LY0) begin
    log_n_pub_r   <= 10'd0;
    log_ovf_pub_r <= 1'b0;
    colw_pub_r    <= 16'd0;
  end

  // The pending log goes public on the cycle its snapshot does, so the pair the
  // player reads is still (state at LY=0, log of the frame that ended there).
  // snap_ready keeps any other capture out while it waits, so the next publish
  // IS this one; and a publish never coincides with the LCD-off take above.
  if (~RST & log_pend_v_r & snap_publish) begin
    log_pub_r     <= log_pend_r;
    log_pend_v_r  <= 1'b0;
    log_n_pub_r   <= log_n_pend_r;
    log_ovf_pub_r <= log_ovf_pend_r;
    colw_pub_r    <= colw_pend_r;
  end
end

//-------------------------------------------------------------------
// Stage 0: decode, straight out of SNES_ADDR, every cycle
//-------------------------------------------------------------------

wire [1:0]  vbank = SNES_ADDR[17:16];    // 0 = $E0 ... 3 = $E3
wire [15:0] vaddr = SNES_ADDR[15:0];
// VIEW_ENABLE now also covers $E4 (address.v); bit 18 tells the FB bank apart.
wire        view_lo = VIEW_ENABLE & ~SNES_ADDR[18];
wire        sel_fb  = VIEW_ENABLE &  SNES_ADDR[18];
wire        view_chr = view_lo;

wire sel_bgch  = view_chr & (vbank == 2'd0) & (vaddr <  16'h3000);
wire sel_obch  = view_chr & (vbank == 2'd0) & (vaddr[15:14] == 2'b01);
wire sel_map   = view_chr & (vbank == 2'd1) & (vaddr <  16'h2000);
wire sel_cgram = view_lo & (vbank == 2'd2) & (vaddr <  16'h0200);
wire sel_stat  = view_lo & (vbank == 2'd2) & (vaddr[15:8] == 8'h02);
wire sel_oam   = view_lo & (vbank == 2'd3) & (vaddr <  16'h0200);
// $E2:0400-$0BFF, i.e. the two 1 KB pages with exactly one of bits 11/10 set.
// $0C00-$0FFF (both set) is reserved and falls through to $00 like any other
// hole in the window.
wire sel_log   = view_lo & (vbank == 2'd2) & (vaddr[15:12] == 4'h0)
                             & (vaddr[11] ^ vaddr[10]);

// ---- 4.1 BG chr: t = addr[13:4], bank = t >= 384, offset = (t mod 384)*16 ----
wire [9:0]  bg_t    = vaddr[13:4];
wire        bg_bank = (bg_t >= 10'd384);
wire [9:0]  bg_toff = bg_bank ? (bg_t - 10'd384) : bg_t;
wire [12:0] va_bgch = {bg_toff[8:0], vaddr[3:0]};

// ---- 4.2 OBJ chr: s = addr[13:5], bank = s[8], bytes 16..31 read $00 --------
wire [8:0]  ob_s    = vaddr[13:5];
wire        ob_bank = ob_s[8];
wire [12:0] va_obch = {1'b0, ob_s[7:0], vaddr[3:0]};
wire        ob_zero = vaddr[4];

// ---- 4.3 maps: both banks at the same offset, read in the same cycle -------
// Region from addr[12:11]: bit 12 picks content from carpet, bit 11 picks the
// GAME BOY MAP itself -- 0 = $9800, 1 = $9C00.  The four windows describe the
// GB's two maps, not the SNES's two layers: which map a layer draws is LCDC.3
// (BG) or LCDC.6 (window), and on the SNES side that is a tilemap base
// register, $2108 / $210A, which the player can even drive per line out of the
// mid-frame log.  A game that flips LCDC.3 halfway down the screen used to be
// undescribable; now it costs the player one HDMA entry and costs the bridge
// nothing.
//
// The consequence here is that the map address reads no snapshot register at
// all: it is a pure wire pick, one 2:1 mux shorter than the layer form it
// replaces, on the path that has to settle within the contract's two cycles.
wire        map_carpet = vaddr[12];
wire        map_msel   = vaddr[11];
wire [12:0] va_map     = {2'b11, map_msel, vaddr[10:1]};

// The two chr views need one bank each and the map view needs both, so both
// ports are driven with the same address and the bank is resolved on the data
// side.  Asking for both costs nothing: sgb_cpu.v's debug pipe stalls on either
// request, so one request and two are the same thing to it.
assign VRAM0_B_REQ  = sel_bgch | sel_obch | sel_map;
assign VRAM1_B_REQ  = VRAM0_B_REQ;
assign VRAM0_B_ADDR = sel_map  ? va_map
                    : sel_obch ? va_obch
                    :            va_bgch;
assign VRAM1_B_ADDR = VRAM0_B_ADDR;

// ---- 4.4 CGRAM: index arithmetic, including the compatibility remap --------
// c and the low address bit pick a palette byte; everything else about the
// entry (black backdrop, white $7FFF, the carpet zeros) is a mux on the data.
wire [7:0]  cg_c      = vaddr[8:1];
wire        cg_hi     = vaddr[0];

// BG half (c = 0..127).  c[6] tells content (0) from carpet (1); the low five
// bits are the same p/k pair in both the BG1 and the BG2 copy.
wire [2:0]  cg_p      = cg_c[4:2];
wire [1:0]  cg_k      = cg_c[1:0];
wire        cg_carpet = cg_c[6];
wire [1:0]  cg_bgp_k  = (cg_k == 2'd0) ? sn_bgp_r[1:0]
                      : (cg_k == 2'd1) ? sn_bgp_r[3:2]
                      : (cg_k == 2'd2) ? sn_bgp_r[5:4]
                      :                  sn_bgp_r[7:6];
wire [1:0]  cg_kb     = sn_compat_r ? cg_bgp_k     : cg_k;
wire [1:0]  cg_kb0    = sn_compat_r ? sn_bgp_r[1:0] : 2'd0;
wire [5:0]  cg_ibg    = {cg_p, (cg_carpet ? cg_kb0 : cg_kb), cg_hi};
wire        cg_bgzero = cg_carpet & (cg_k != 2'd1);

// OBJ half (c = 128..255): q = 3 bits of palette, k = 4 bits of colour, and
// only k = 1..3 is a real colour.
wire [2:0]  cg_q      = cg_c[6:4];
wire [3:0]  cg_k4     = cg_c[3:0];
wire [7:0]  cg_obp    = (cg_q != 3'd0) ? sn_obp1_r : sn_obp0_r;
wire [1:0]  cg_obp_k  = (cg_k4 == 4'd1) ? cg_obp[3:2]
                      : (cg_k4 == 4'd2) ? cg_obp[5:4]
                      :                   cg_obp[7:6];
wire [1:0]  cg_ko     = sn_compat_r ? cg_obp_k : cg_k4[1:0];
wire [5:0]  cg_iob    = {cg_q, cg_ko, cg_hi};
wire        cg_obzero = (cg_k4 == 4'd0) | (cg_k4 > 4'd3);

wire        cg_zero   = cg_c[7] ? cg_obzero : cg_bgzero;
wire        cg_black  = (cg_c == 8'd0);    // backdrop: letterbox and pillars
wire        cg_white  = (cg_c == 8'd98);   // BG4 pal 0 colour 2 = the white screen

// ---- 5 COMPAT_RAW: status +$40..+$57, the three DMG palettes UNCOMPOSED ----
// Twelve BGR555 colours -- BG palette 0, OBJ palette 0, OBJ palette 1 -- little
// endian, bit 15 clear, straight out of the snapshot's CRAM.  The CGRAM view
// above shows the same memory REMAPPED through BGP/OBP0/OBP1 in compatibility
// mode, which is what the screen needs and what a player cannot undo; this is
// the raw table, for the frame-by-frame work the composition throws away.
//
// It is served THROUGH THE CRAM SNAPSHOT'S OWN READ PORT, by steering the
// address, rather than out of 24 shadow registers filled during the copy.  That
// keeps 192 flip-flops out of the design, and it can do so because the port is
// free whenever it is needed: only one address is on the SNES bus at a time, so
// a status read and a CGRAM view read never collide.  The cost is that these
// bytes take the TWO cycle path like every other RAM-backed view, while the
// rest of the status block answers in one -- which is why sel_craw has to be
// subtracted from the status branch of the output mux, further down.
//
// CRAM layout (sgb_cpu.v: cram_a_index = {obj, BCPS/OCPS[5:0]}): palette p
// colour k lives at p*8 + k*2, BG in 0..63 and OBJ in 64..127.  So the three
// palettes wanted here are 0..7, 64..71 and 72..79 -- and with the offset from
// $40 in vaddr[4:0], that is a bit pick: bit 6 = "OBJ" = |off[4:3], bit 3 =
// "palette 1" = off[4], bits 2:0 = the byte within the palette.
wire        sel_craw  = sel_stat & (vaddr[7:5] == 3'b010) & (vaddr[4:3] != 2'b11);
wire [6:0]  craw_idx  = {|vaddr[4:3], 2'b00, vaddr[4], vaddr[2:0]};

wire [7:0]  cram_snap_ra = {snap_buf_r,
                            sel_craw ? craw_idx
                                     : {cg_c[7], (cg_c[7] ? cg_iob : cg_ibg)}};

// ---- 4.5 OAM: entry j = 2i (+1 for the bottom half of an 8x16 sprite) ------
wire [5:0]  oa_i         = vaddr[8:3];
wire [6:0]  oam_snap_ra  = {snap_buf_r, oa_i};

// ---- 8 log: k = (addr - $400) / 4, which is a WIRE PICK, not a subtract ----
// $0400 + k*4 has k[8] in address bit 11 (the $0400 page is k = 0..255, the
// $0800 page k = 256..511) and k[7:0] in bits 9:2, so the entry index needs no
// arithmetic at all.  Byte within the entry = bits 1:0.
wire [8:0]  log_k   = {vaddr[11], vaddr[9:2]};
wire [10:0] log_ra  = {log_pub_r, log_k};
// Past the end of the published log the answer is $00 (see the header).  The
// compare is ten bits because LOG_N reaches 512, which k cannot.
wire        log_oor = ({1'b0, log_k} >= log_n_pub_r);

//-------------------------------------------------------------------
// The snapshot RAMs themselves (1 write + 1 read = simple dual port)
//-------------------------------------------------------------------

// Written by the copy engine into ~snap_buf_r, read by the views out of
// snap_buf_r: the two never touch the same half, so there is no
// read-during-write case to define.
wire        snap_oam_we = snap_rd_d1_r & (snap_cnt_d1_r[1:0] == 2'd3);
wire [6:0]  snap_oam_wa = {snap_wbuf, snap_cnt_d1_r[7:2]};
wire [31:0] snap_oam_wd = {OAM_RD_DATA, snap_oam_word_r};

wire        snap_cram_we = snap_rd_d1_r & ~snap_cnt_d1_r[7];
wire [7:0]  snap_cram_wa = {snap_wbuf, snap_cnt_d1_r[6:0]};

always @(posedge CLK) begin
  if (snap_oam_we) oam_snap_r[snap_oam_wa] <= snap_oam_wd;
  oam_snap_q_r <= oam_snap_r[oam_snap_ra];
end

always @(posedge CLK) begin
  if (snap_cram_we) cram_snap_r[snap_cram_wa] <= CRAM_RD_DATA;
  cram_snap_q_r <= cram_snap_r[cram_snap_ra];
end

// The log RAM, same shape.  The write port addresses the live buffer and the
// read port the published one, which are never the same buffer, so the two can
// never meet on one address and the read-during-write behaviour of the M9K
// never matters (no_rw_check on the declaration tells the fitter so, and saves
// it the bypass).
always @(posedge CLK) begin
  if (log_take) log_ram_r[log_wa] <= log_word;
  log_q_r <= log_ram_r[log_ra];
end

//-------------------------------------------------------------------
// C6: the framebuffer (c6_fb.v, contract section 14)
//-------------------------------------------------------------------
wire [7:0]  c6_view_byte;
wire [7:0]  c6_stat_byte;
wire        c6_stat_hit;
wire [7:0]  c6_flags, c6_cl;
wire [15:0] c6_rows_dropped, c6_rowdone_bad, c6_cl_switches, c6_frames;
// The FB view answers in THREE cycles (the RAM read is registered inside
// c6_fb.v, see the header), so its select flag gets one more stage than the
// other views' and lines up with the byte.
reg         s1_fb_r;
reg         s1_fb_a_r;
always @(posedge CLK) begin s1_fb_a_r <= sel_fb; s1_fb_r <= s1_fb_a_r; end

c6_fb c6fb (
  .CLK(CLK), .RST(RST),
  .PX_VALID(C6_PX_VALID), .PX_X(C6_PX_X), .PX_Y(C6_PX_Y), .PX_BGR(C6_PX_BGR),
  .TAP_LY0(TAP_LY0), .TAP_VBLANK(TAP_VBLANK),
  .CTL_WR(wr_c6ctl), .ROWDONE_WR(wr_c6rowdn), .WR_DATA(DATA_IN),
  .SEL(sel_fb), .VADDR(vaddr), .VIEW_BYTE(c6_view_byte),
  .STAT_OFF(SNES_ADDR[7:0]), .STAT_BYTE(c6_stat_byte), .STAT_HIT(c6_stat_hit),
  .COLW_N(colw_pub_r),
  .FLAGS(c6_flags), .CL_BYTE(c6_cl),
  .ROWS_DROPPED(c6_rows_dropped), .ROWDONE_BAD(c6_rowdone_bad),
  .CL_SWITCHES(c6_cl_switches), .FRAMES(c6_frames)
);

//-------------------------------------------------------------------
// Stage 1: what the composition needs to know about the address
//-------------------------------------------------------------------

// The derived flags are registered rather than the address, because rederiving
// them from a registered address would cost a second copy of the decode.
reg  s1_bgch_r, s1_obch_r, s1_map_r, s1_cgram_r, s1_oam_r, s1_log_r;
reg  s1_bg_bank_r, s1_ob_bank_r, s1_ob_zero_r;
reg  s1_map_carpet_r, s1_map_hi_r;
reg  s1_cg_hi_r, s1_cg_zero_r, s1_cg_black_r, s1_cg_white_r;
reg  [5:0] s1_oam_i_r;
reg        s1_oam_half_r;
reg  [1:0] s1_oam_byte_r;
reg  [1:0] s1_log_byte_r;
reg        s1_log_oor_r;
reg        s1_craw_r, s1_craw_hi_r;

always @(posedge CLK) begin
  s1_bgch_r       <= sel_bgch;
  s1_obch_r       <= sel_obch;
  s1_map_r        <= sel_map;
  s1_cgram_r      <= sel_cgram;
  s1_oam_r        <= sel_oam;
  s1_log_r        <= sel_log;
  s1_log_byte_r   <= vaddr[1:0];
  s1_log_oor_r    <= log_oor;
  s1_craw_r       <= sel_craw;
  s1_craw_hi_r    <= vaddr[0];
  s1_bg_bank_r    <= bg_bank;
  s1_ob_bank_r    <= ob_bank;
  s1_ob_zero_r    <= ob_zero;
  s1_map_carpet_r <= map_carpet;
  s1_map_hi_r     <= vaddr[0];
  s1_cg_hi_r      <= cg_hi;
  s1_cg_zero_r    <= cg_zero;
  s1_cg_black_r   <= cg_black;
  s1_cg_white_r   <= cg_white;
  s1_oam_i_r      <= oa_i;
  s1_oam_half_r   <= vaddr[2];
  s1_oam_byte_r   <= vaddr[1:0];
end

//-------------------------------------------------------------------
// Stage 1: composition
//-------------------------------------------------------------------

// ---- chr ------------------------------------------------------------------
wire [7:0] bgch_byte = s1_bg_bank_r ? VRAM1_B_DATA : VRAM0_B_DATA;
wire [7:0] obch_byte = s1_ob_zero_r ? 8'h00
                     : s1_ob_bank_r ? VRAM1_B_DATA : VRAM0_B_DATA;

// ---- maps -----------------------------------------------------------------
// In compatibility mode the attribute plane is $00 by definition (the boot ROM
// zeroes bank 1 and VBK is latched), so it is forced rather than read: a game
// that left something in bank 1 before KEY0 latched must not colour the screen.
wire [7:0] m_v  = VRAM0_B_DATA;
wire [7:0] m_a  = sn_compat_r ? 8'h00 : VRAM1_B_DATA;
// t = v + 256*a.3, ten bits, and that is the WHOLE index (wire $02).  LCDC.4 is
// no longer baked in: the player holds two chr bases 256 tiles apart -- the
// $8000 method in one, the $8800 method in the other -- and picks between them
// with $210B, so the bit follows the same road as LCDC.3 and can move per line.
// The VRAM bank therefore sits 256 tiles up, not 384, because each base is 512
// tiles wide.
//
// What matters in the concatenation is WHERE the bank bit sits, not how wide
// the expression is: a narrower concatenation is zero-extended on the left and
// means the same thing, but moving m_a[3] to t[9] silently doubles its weight
// and nothing downstream complains, because a 512-tile base only ever uses
// t[8:0] and the map word carries t[9:8] regardless.
//
// ⚠️ This `t` is NOT the `t` of the BG chr view above.  That one runs 0..767
// with a bank step of 384 (contract sec.4.1, bg_t/bg_toff); this one runs
// 0..511 with a bank step of 256 (sec.4.3).  Two different index spaces that
// happen to share a letter -- confusing them yields a picture that looks
// plausible and is wrong.
wire [9:0] m_t  = {1'b0, m_a[3], m_v};
wire [9:0] m_tt = s1_map_carpet_r ? 10'd768 : m_t;
// Priority 0 on BOTH carpets.  The carpet is the GB's colour 0 painted under
// the content, and it cannot win a pixel from anything: BG3 and BG4 are
// spatially exclusive (contract sec.11.3, W1 inside/outside) and OBJ0 is never
// used, so nothing sits between them in the mode 0 order.  The window carpet's
// old priority 1 decided no pixel; dropping it makes the two carpets the same
// object and takes the layer bit out of the composition entirely.
wire       m_pri = s1_map_carpet_r ? 1'b0
                                   : (m_a[7] & sn_lcdc_r[0] & ~sn_compat_r);
wire       m_h   = s1_map_carpet_r ? 1'b0 : m_a[5];
wire       m_vf  = s1_map_carpet_r ? 1'b0 : m_a[6];
wire [7:0] map_byte = s1_map_hi_r ? {m_vf, m_h, m_pri, m_a[2:0], m_tt[9:8]}
                                  : m_tt[7:0];

// ---- CGRAM ----------------------------------------------------------------
// Bit 15 of a BGR555 entry is not a colour bit; the contract publishes it as 0
// rather than whatever the game left in the palette RAM's high byte.
wire [7:0] cg_raw    = s1_cg_hi_r ? {1'b0, cram_snap_q_r[6:0]} : cram_snap_q_r;
wire [7:0] cgram_byte = s1_cg_black_r ? 8'h00
                      : s1_cg_white_r ? (s1_cg_hi_r ? 8'h7F : 8'hFF)
                      : s1_cg_zero_r  ? 8'h00
                      :                 cg_raw;

// ---- OAM ------------------------------------------------------------------
wire [7:0] o_y    = oam_snap_q_r[7:0];
wire [7:0] o_x    = oam_snap_q_r[15:8];
wire [7:0] o_n    = oam_snap_q_r[23:16];
wire [7:0] o_a    = oam_snap_q_r[31:24];
wire       o_h16  = sn_lcdc_r[2];

// Visibility, contract 4.5: OBJ enable, the Y band (which differs between 8x8
// and 8x16), the X band, the bottom half only in 8x16, and entries 80..127
// never.  Everything invisible is parked at Y = $F0, which with 8x8 sprites
// covers lines 241..248 -- off the 224-line screen and with no wrap.  That is
// what makes "OBJ only 8x8" an invariant rather than a preference.
wire       o_yok  = o_h16 ? (o_y >= 8'd1) : (o_y >= 8'd9);
wire       o_vis  = sn_lcdc_r[1] & o_yok & (o_y <= 8'd159)
                  & (o_x >= 8'd1) & (o_x <= 8'd167)
                  & (s1_oam_half_r ? o_h16 : 1'b1)
                  & (s1_oam_i_r < 6'd40);

// 8x16: the top half takes the even tile and the bottom half the odd one, and a
// vertical flip swaps them.  XOR of the half and attribute bit 6 says which.
wire [7:0] o_tile = ~o_h16               ? o_n
                  : (s1_oam_half_r ^ o_a[6]) ? {o_n[7:1], 1'b1}
                  :                            {o_n[7:1], 1'b0};
wire [1:0] o_pri  = (o_a[7] & (sn_compat_r | sn_lcdc_r[0])) ? 2'b01 : 2'b10;
wire [2:0] o_pal  = sn_compat_r ? {2'b00, o_a[4]} : o_a[2:0];
// Bit 0 is the SNES name select, i.e. the GB's VRAM bank.  In compatibility
// mode the hardware IGNORES attribute bit 3 (SameBoy display.c:1121 gates it on
// cgb_mode), and it has to be ignored here too: the boot ROM zeroes bank 1 and
// VBK is latched, so bank 1 is empty and tile n+256 is transparent.  A DMG game
// with junk in the low nibble of an OAM attribute -- which is common, those
// bits mean nothing to it -- would have the sprite simply disappear.
wire [7:0] o_attr = {o_a[6], o_a[5], o_pri, o_pal, o_a[3] & ~sn_compat_r};
wire [7:0] o_ypos = o_y + (s1_oam_half_r ? 8'd32 : 8'd24);

wire [7:0] oam_byte = ~o_vis                    ? ((s1_oam_byte_r == 2'd1) ? 8'hF0 : 8'h00)
                    : (s1_oam_byte_r == 2'd0)   ? (o_x + 8'd40)
                    : (s1_oam_byte_r == 2'd1)   ? o_ypos
                    : (s1_oam_byte_r == 2'd2)   ? o_tile
                    :                             o_attr;

// ---- log (contract section 8) ---------------------------------------------
// The entry came out of the RAM as one word; which of its four bytes the SNES
// asked for is the low two address bits, registered alongside it.
wire [7:0] log_ebyte = (s1_log_byte_r == 2'd0) ? log_q_r[7:0]
                     : (s1_log_byte_r == 2'd1) ? log_q_r[15:8]
                     : (s1_log_byte_r == 2'd2) ? log_q_r[23:16]
                     :                           log_q_r[31:24];
wire [7:0] log_byte  = s1_log_oor_r ? 8'h00 : log_ebyte;

// ---- COMPAT_RAW (contract section 5, +$40..+$57) --------------------------
// The same "bit 15 is not a colour bit" rule the CGRAM view applies: the high
// byte of a BGR555 entry is published with bit 7 clear rather than with
// whatever the game left in palette RAM.
wire [7:0] craw_byte = s1_craw_hi_r ? {1'b0, cram_snap_q_r[6:0]} : cram_snap_q_r;

wire [7:0] view_byte = s1_bgch_r  ? bgch_byte
                     : s1_obch_r  ? obch_byte
                     : s1_map_r   ? map_byte
                     : s1_cgram_r ? cgram_byte
                     : s1_oam_r   ? oam_byte
                     : s1_log_r   ? log_byte
                     : s1_craw_r  ? craw_byte
                     : s1_fb_r    ? c6_view_byte
                     :              8'h00;
wire       view_sel_d1 = s1_bgch_r | s1_obch_r | s1_map_r | s1_cgram_r | s1_oam_r
                       | s1_log_r | s1_craw_r
                       | s1_fb_r
                       ;

//-------------------------------------------------------------------
// Dirty accumulator (contract section 6)
//-------------------------------------------------------------------

reg  [47:0] dirty_chr_acc_r,  dirty_chr_pub_r;
reg  [31:0] dirty_obj_acc_r,  dirty_obj_pub_r;
reg  [63:0] dirty_map_acc_r,  dirty_map_pub_r;
reg  [7:0]  dirty_misc_acc_r, dirty_misc_pub_r;

// One decoder per class rather than a 48/32/64-bit variable shift: the bank is
// a fixed offset (24 blocks, 16 blocks) so it is a concatenation, not an adder.
wire        d_chr_hit = TAP_VRAM_WE & (TAP_VRAM_ADDR < 13'h1800);
wire [31:0] d_chr_dec = 32'd1 << TAP_VRAM_ADDR[12:8];
wire [47:0] set_chr   = ~d_chr_hit    ? 48'd0
                      : TAP_VRAM_BANK ? {d_chr_dec[23:0], 24'd0}
                      :                 {24'd0, d_chr_dec[23:0]};

wire        d_obj_hit = TAP_VRAM_WE & ~TAP_VRAM_ADDR[12];
wire [15:0] d_obj_dec = 16'd1 << TAP_VRAM_ADDR[11:8];
wire [31:0] set_obj   = ~d_obj_hit    ? 32'd0
                      : TAP_VRAM_BANK ? {d_obj_dec, 16'd0}
                      :                 {16'd0, d_obj_dec};

wire        d_map_hit = TAP_VRAM_WE & (TAP_VRAM_ADDR[12:11] == 2'b11);

// snap_done / snap_publish are up in "Snapshot control" (the log needs them).

// Snapshot-derived dirt (contract section 5): EVERY bit here is "the published
// snapshot differs from the one it replaces", never a live write.  The views
// are the snapshot, so a write between LY=0 and the COMMIT belongs to the next
// frame's word -- publishing it now tells the player to re-copy a buffer that
// has not changed yet, and then the frame it DID change in reports nothing.
// Wire $02: the only LCDC bit still baked into a map entry is LCDC.0, through
// the priority bit, and the only other thing that rewrites every entry is
// compat (the attribute plane becomes $00).  LCDC.3/.4/.6 used to be in this
// mask because they chose the map page and the chr base; they are SNES
// registers now, so a frame that moves only those three has not moved one byte
// of any view and must not cost 4 KB of map traffic.  They still count as REGS.
wire        snap_msel   = snap_publish & ((|((pd_lcdc_r ^ sn_lcdc_r) & 8'h01))
                                       |  (pd_compat_r != sn_compat_r));
wire        snap_regs   = snap_publish & ((pd_lcdc_r != sn_lcdc_r) | (pd_scx_r  != sn_scx_r)
                                    |  (pd_scy_r  != sn_scy_r)  | (pd_wx_r   != sn_wx_r)
                                    |  (pd_wy_r   != sn_wy_r)   | (pd_bgp_r  != sn_bgp_r)
                                    |  (pd_obp0_r != sn_obp0_r) | (pd_obp1_r != sn_obp1_r)
                                    |  (pd_opri_r != sn_opri_r) | (pd_key1_r != sn_key1_r)
                                    |  (pd_compat_r != sn_compat_r)
                                    |  (pd_lcd_on_r != sn_lcd_on_r)
                                    |  (pd_first_frame_r != sn_first_frame_r));
// The OAM view also moves when LCDC.0/.1/.2 or compat move, because visibility,
// the 8x16 tile pairing and the priority/palette encoding all read them.
wire        snap_oam    = snap_publish & (oam_snap_diff_r
                                    |  (|((pd_lcdc_r ^ sn_lcdc_r) & 8'h07))
                                    |  (pd_compat_r != sn_compat_r));
// The CGRAM view is composed through BGP/OBP0/OBP1 in compatibility mode, so
// those count as palette changes there.
wire        snap_cram   = snap_publish & (cram_snap_diff_r
                                    |  (pd_compat_r != sn_compat_r)
                                    |  (pd_compat_r & ((pd_bgp_r  != sn_bgp_r)
                                                    |  (pd_obp0_r != sn_obp0_r)
                                                    |  (pd_obp1_r != sn_obp1_r))));

wire [63:0] set_map  = (d_map_hit ? (64'd1 << TAP_VRAM_ADDR[10:5]) : 64'd0)
                     | (snap_msel ? {64{1'b1}} : 64'd0);
wire [7:0]  set_misc = {3'b000, snap_msel, 1'b0, snap_regs, snap_cram, snap_oam};

// Contract section 6, verbatim: on COMMIT the accumulator is PUBLISHED and then
// reloaded with this cycle's set bits.  The forbidden variant
// (pub <= acc | set; acc <= 0) loses a write that lands after the player has
// already copied the block, which is a corrupted tile that never heals.
//
// Only the FIRST COMMIT of a frame swaps.  A second one (the contract forbids
// it, but a player bug is not the wire's problem) would publish an accumulator
// that has been collecting for twenty lines instead of a frame, and silently
// drop everything the first COMMIT had already handed over.  Ignoring it and
// counting it in GBDG turns a lost tile into a number somebody can read.
wire        commit_swap = wr_commit & ~busy_r;

always @(posedge CLK) begin
  if (RST) begin
    dirty_chr_acc_r  <= 48'd0;  dirty_chr_pub_r  <= 48'd0;
    dirty_obj_acc_r  <= 32'd0;  dirty_obj_pub_r  <= 32'd0;
    dirty_map_acc_r  <= 64'd0;  dirty_map_pub_r  <= 64'd0;
    dirty_misc_acc_r <=  8'd0;  dirty_misc_pub_r <=  8'd0;
  end
  else if (commit_swap) begin
    dirty_chr_pub_r  <= dirty_chr_acc_r;   dirty_chr_acc_r  <= set_chr;
    dirty_obj_pub_r  <= dirty_obj_acc_r;   dirty_obj_acc_r  <= set_obj;
    dirty_map_pub_r  <= dirty_map_acc_r;   dirty_map_acc_r  <= set_map;
    dirty_misc_pub_r <= dirty_misc_acc_r;  dirty_misc_acc_r <= set_misc;
  end
  else begin
    dirty_chr_acc_r  <= dirty_chr_acc_r  | set_chr;
    dirty_obj_acc_r  <= dirty_obj_acc_r  | set_obj;
    dirty_map_acc_r  <= dirty_map_acc_r  | set_map;
    dirty_misc_acc_r <= dirty_misc_acc_r | set_misc;
  end
end

//-------------------------------------------------------------------
// Snapshot engine (contract section 7)
//-------------------------------------------------------------------

// The trigger, the skip rule and the publish condition are all up in "Snapshot
// control", because the dirty accumulator needs them too.
assign OAM_RD_REQ   = snap_run_r & (snap_cnt_r < 8'd160);
assign OAM_RD_ADDR  = snap_cnt_r;
assign CRAM_RD_REQ  = snap_run_r & (snap_cnt_r < 8'd128);
assign CRAM_RD_ADDR = snap_cnt_r[6:0];

always @(posedge CLK) begin
  if (RST) begin
    sn_lcdc_r <= 8'h00; sn_scx_r  <= 8'h00; sn_scy_r  <= 8'h00;
    sn_wx_r   <= 8'h00; sn_wy_r   <= 8'h00; sn_bgp_r  <= 8'h00;
    sn_obp0_r <= 8'h00; sn_obp1_r <= 8'h00; sn_opri_r <= 8'h00;
    sn_key1_r <= 8'h00;
    sn_compat_r <= 1'b0; sn_lcd_on_r <= 1'b0; sn_locked_r <= 1'b0;
    sn_first_frame_r <= 1'b0;

    pd_lcdc_r <= 8'h00; pd_scx_r  <= 8'h00; pd_scy_r  <= 8'h00;
    pd_wx_r   <= 8'h00; pd_wy_r   <= 8'h00; pd_bgp_r  <= 8'h00;
    pd_obp0_r <= 8'h00; pd_obp1_r <= 8'h00; pd_opri_r <= 8'h00;
    pd_key1_r <= 8'h00;
    pd_compat_r <= 1'b0; pd_lcd_on_r <= 1'b0; pd_locked_r <= 1'b0;
    pd_first_frame_r <= 1'b0;

    snap_valid_r      <= 1'b0;
    snap_buf_r        <= 1'b0;
    snap_run_r        <= 1'b0;
    snap_pub_pend_r   <= 1'b0;
    oam_wr_pend_r     <= 1'b0;
    cram_wr_pend_r    <= 1'b0;
    oam_snap_diff_r   <= 1'b0;
    cram_snap_diff_r  <= 1'b0;
    snap_cnt_r        <= 8'd0;
    snap_cnt_d1_r     <= 8'd0;
    snap_rd_d1_r      <= 1'b0;
    snap_oam_word_r   <= 24'd0;
    first_frame_arm_r <= 1'b1;
    lcd_off_pend_r    <= 1'b0;
    snap_skipped_r    <= 16'd0;
    seq_r             <= 16'd0;
    lcd_on_d1_r       <= 1'b0;
  end
  else begin
    lcd_on_d1_r <= LCD_ON;
    if (lcd_on_rise) first_frame_arm_r <= 1'b1;

    // Clear first, set second: an edge arriving on the cycle a snapshot starts
    // still has to be published, and the value it wants published is the one
    // that snapshot is about to capture anyway.
    if (snap_take)   lcd_off_pend_r <= 1'b0;
    if (lcd_on_rise) lcd_off_pend_r <= 1'b0;
    if (lcd_on_fall) lcd_off_pend_r <= 1'b1;

    // ---- did the snapshot's OAM / CRAM actually change? -----------------
    // The accumulator restarts AT the capture with this cycle's write, so a
    // write during the 161-cycle copy is attributed to the next snapshot.  That
    // can over-report by one frame (the byte may already have been copied) and
    // never under-reports, which is the safe direction for a dirty bit.
    oam_wr_pend_r  <= snap_take ? TAP_OAM_WE  : (oam_wr_pend_r  | TAP_OAM_WE);
    cram_wr_pend_r <= snap_take ? TAP_CRAM_WE : (cram_wr_pend_r | TAP_CRAM_WE);
    if (snap_take) begin
      oam_snap_diff_r  <= oam_wr_pend_r;
      cram_snap_diff_r <= cram_wr_pend_r;
    end

    // ---- read pipeline -------------------------------------------------
    snap_rd_d1_r  <= snap_run_r & (snap_cnt_r < 8'd160);
    snap_cnt_d1_r <= snap_cnt_r;
    if (snap_run_r & (snap_cnt_r < 8'd160)) snap_cnt_r <= snap_cnt_r + 8'd1;
    // Bytes arrive Y, X, tile, attribute; the first three are shifted down and
    // the fourth is concatenated on the way into the RAM.
    if (snap_rd_d1_r) snap_oam_word_r <= {OAM_RD_DATA, snap_oam_word_r[23:8]};

    // ---- trigger -------------------------------------------------------
    // A skipped LY=0 means "reuse what you have": SEQ stays put and the player
    // keeps the OAM and CGRAM it already copied.
    if (snap_skip) snap_skipped_r <= snap_skipped_r + 16'd1;

    if (snap_take) begin
      pd_lcdc_r <= TAP_LCDC; pd_scx_r  <= TAP_SCX;  pd_scy_r  <= TAP_SCY;
      pd_wx_r   <= TAP_WX;   pd_wy_r   <= TAP_WY;   pd_bgp_r  <= TAP_BGP;
      pd_obp0_r <= TAP_OBP0; pd_obp1_r <= TAP_OBP1; pd_opri_r <= TAP_OPRI;
      pd_key1_r <= TAP_KEY1;
      pd_compat_r <= COMPAT;
      pd_lcd_on_r <= LCD_ON;
      pd_locked_r <= LOCKED;
      pd_first_frame_r  <= TAP_LY0 & first_frame_arm_r;
      if (TAP_LY0) first_frame_arm_r <= 1'b0;
      snap_run_r <= 1'b1;
      snap_cnt_r <= 8'd0;
    end

    // ---- copy finished --------------------------------------------------
    // The buffer is complete here.  Whether it becomes VISIBLE here is a
    // separate question: inside the read window it has to wait.
    if (snap_done) begin
      snap_run_r <= 1'b0;
      if (busy_eff) snap_pub_pend_r <= 1'b1;
    end

    // ---- publish -------------------------------------------------------
    if (snap_publish) begin
      snap_pub_pend_r <= 1'b0;
      sn_lcdc_r <= pd_lcdc_r; sn_scx_r  <= pd_scx_r;  sn_scy_r  <= pd_scy_r;
      sn_wx_r   <= pd_wx_r;   sn_wy_r   <= pd_wy_r;   sn_bgp_r  <= pd_bgp_r;
      sn_obp0_r <= pd_obp0_r; sn_obp1_r <= pd_obp1_r; sn_opri_r <= pd_opri_r;
      sn_key1_r <= pd_key1_r;
      sn_compat_r      <= pd_compat_r;
      sn_lcd_on_r      <= pd_lcd_on_r;
      sn_locked_r      <= pd_locked_r;
      sn_first_frame_r <= pd_first_frame_r;
      snap_buf_r   <= ~snap_buf_r;
      snap_valid_r <= 1'b1;
      seq_r        <= seq_r + 16'd1;
    end
  end
end

//-------------------------------------------------------------------
// Command state
//-------------------------------------------------------------------

always @(posedge CLK) begin
  if (RST) begin
    // "Nothing pressed" until the first $EF0000: the SNES pad registers are
    // active high, so all-zero is the released state.
    pad_lo_r       <= 8'h00;
    pad_hi_r       <= 8'h00;
    go_r           <= 1'b0;
    go_seen_r      <= 8'h00;
    commit_ctr_r      <= 16'h0000;
    consumed_ctr_r    <= 16'h0000;
    commit_busy_ctr_r <= 16'h0000;
    busy_r            <= 1'b0;
    ly_sync_r         <= 8'h00;
    mbx0_r            <= 16'h0000;
    mbx1_r            <= 16'h0000;
    mbx2_r            <= 16'h0000;
    mbx3_r            <= 16'h0000;
    mbx4_r            <= 16'h0000;
    mbx5_r            <= 16'h0000;
    mbx_lo_r          <= 8'h00;
    mbx_lo_idx_r      <= 3'd0;
    mbx_lo_ok_r       <= 1'b0;
  end
  else begin
    if (wr_pad_lo) pad_lo_r <= DATA_IN;
    if (wr_pad_hi) pad_hi_r <= DATA_IN;

    // GO is idempotent.  Writing $00 re-asserts the GB reset, and because
    // REG_BOOT_r is cleared from the CPU's own cpu_ireset_r (sgb_cpu.v), that
    // also re-arms BOOTROM_ACTIVE -- a full GB reset, no separate control.
    if (wr_go) begin
      go_r      <= DATA_IN[0];
      go_seen_r <= go_seen_r + 8'd1;
    end

    // busy spans the read window: up at COMMIT, down at CONSUMED.  It is what
    // makes an LY=0 inside the window skip its snapshot instead of tearing it.
    if (wr_commit) begin
      busy_r       <= 1'b1;
      commit_ctr_r <= commit_ctr_r + 16'd1;
      // The dirty swap is gated on ~busy_r up in the accumulator; this counts
      // the writes that gate threw away, so "the player committed twice" is a
      // number in GBDG rather than a tile that never refreshes.
      if (busy_r) commit_busy_ctr_r <= commit_busy_ctr_r + 16'd1;
    end
    if (wr_consumed) begin
      busy_r         <= 1'b0;
      consumed_ctr_r <= consumed_ctr_r + 16'd1;
    end

    if (wr_sync) ly_sync_r <= TAP_LY;

    if (wr_mbx) begin
      if (~SNES_ADDR[0]) begin
        mbx_lo_r     <= DATA_IN;
        mbx_lo_idx_r <= mbx_idx;
        mbx_lo_ok_r  <= 1'b1;
      end
      else begin
        if (mbx_lo_ok_r & (mbx_lo_idx_r == mbx_idx)) begin
          case (mbx_idx)
            3'd0:    mbx0_r <= {DATA_IN, mbx_lo_r};
            3'd1:    mbx1_r <= {DATA_IN, mbx_lo_r};
            3'd2:    mbx2_r <= {DATA_IN, mbx_lo_r};
            3'd3:    mbx3_r <= {DATA_IN, mbx_lo_r};
            3'd4:    mbx4_r <= {DATA_IN, mbx_lo_r};
            default: mbx5_r <= {DATA_IN, mbx_lo_r};   // wr_mbx caps the index at 5
          endcase
        end
        mbx_lo_ok_r <= 1'b0;
      end
    end
  end
end

//-------------------------------------------------------------------
// GB reset
//-------------------------------------------------------------------

// Same shape as the ICD2's cpu_ireset_r (sgb_icd2.v:172), except that the
// release comes from GO rather than from a $6003 write: hold while reset or
// !GO, then keep holding until the next bus edge so the GB starts on an
// M-cycle boundary.  The ICD2's extra 65536-bus-clock cold-start delay is NOT
// reproduced -- it existed so the SNES could win the race for the row buffers,
// and there are no row buffers here.
reg  gb_ireset_r;
always @(posedge CLK) gb_ireset_r <= RST | ~go_r | (gb_ireset_r & ~BUS_EDGE);

assign CPU_RST = gb_ireset_r;

//-------------------------------------------------------------------
// Joypad
//-------------------------------------------------------------------

// SNES auto-joypad, active high.  The 16-bit JOY1 word is
// BYsSudlr AXLR0000, and $4219 is its HIGH byte -- so the buttons that look
// like they belong to the "low" register do not:
//   $4218 (JOY1L, -> $EF0000) = {b7 A, b6 X, b5 L, b4 R, 0, 0, 0, 0}
//   $4219 (JOY1H, -> $EF0001) = {b7 B, b6 Y, b5 Select, b4 Start,
//                                b3 Up, b2 Down, b1 Left, b0 Right}
// Evidence in this tree, both silicon-validated: sd2snes_sms/main.v:929-935
// reads Up/Down/Left/Right out of sms_pad_raw[11:8] (the $EF0001 half) and A
// out of [7] (the $EF0000 half); and the IGR combos in sd2snes_sgb/cheat.v:192
// spell "L+R+Select+X" as $2070 -- $20 = Select in the high byte, $70 = X+L+R
// in the low one.  The player writes $4218 -> $EF0000 and $4219 -> $EF0001,
// the same way the SMS and A26 players do, so the mapping is fixed HERE.
//
// GB P1, active low, one pad only (the ICD2's four-pad multiplex at
// sgb_icd2.v:485 is gone -- the SGB needed it, a GBC does not).
wire gb_a      = pad_lo_r[7] | pad_lo_r[6];   // A or X
wire gb_b      = pad_hi_r[7] | pad_hi_r[6];   // B or Y
wire gb_select = pad_hi_r[5];
wire gb_start  = pad_hi_r[4];
wire gb_up     = pad_hi_r[3];
wire gb_down   = pad_hi_r[2];
wire gb_left   = pad_hi_r[1];
wire gb_right  = pad_hi_r[0];

wire [3:0] pad_btn  = ~{gb_start, gb_select, gb_b,    gb_a    };
wire [3:0] pad_dpad = ~{gb_down,  gb_up,     gb_left, gb_right};

// P1I[1] = P15 (buttons select), P1I[0] = P14 (d-pad select), both active low.
// With neither selected this yields $F, which is what the hardware returns.
assign P1O = ({4{P1I[1]}} | pad_btn) & ({4{P1I[0]}} | pad_dpad);

//-------------------------------------------------------------------
// Frame counters
//-------------------------------------------------------------------

reg  [7:0]  frame_ctr_r;
reg  [15:0] free_ctr_r;

always @(posedge CLK) begin
  if (RST) begin
    frame_ctr_r <= 8'h00;
    free_ctr_r  <= 16'h0000;
  end
  else begin
    // FRAME counts the snapshot instant, not the vblank edge, so that it and
    // SEQ describe the same moment: a player that sees FRAME move and SEQ stand
    // still knows exactly one thing, that its window was still open at LY=0.
    if (TAP_LY0) frame_ctr_r <= frame_ctr_r + 8'd1;

    // Free-running liveness counter: ticks at the GB dot rate (4.19 MHz), so
    // two USB reads of GBDG a moment apart differ iff the clock is running.
    if (CE1) free_ctr_r <= free_ctr_r + 16'd1;
  end
end

//-------------------------------------------------------------------
// Frame dot counter -- contract section 2
//-------------------------------------------------------------------

// Position inside the GB frame, in dots (1 CE = 1 dot = 1 T-cycle), 0..70223.
// Zeroed by the LY=0 snapshot instant, not by the vblank edge, and frozen with
// the LCD off -- with no LCD there is no frame to be in phase with, which is
// also why the genlock loop freezes there.
reg [16:0] frame_dot_ctr_r;

always @(posedge CLK) begin
  if (RST) frame_dot_ctr_r <= 17'd0;
  else if (TAP_LY0) frame_dot_ctr_r <= 17'd0;
  else if (PPU_DOT_EDGE & LCD_ON)
    frame_dot_ctr_r <= (frame_dot_ctr_r == 17'd70223) ? 17'd0 : frame_dot_ctr_r + 17'd1;
end

assign FRAME_DOT_CTR = frame_dot_ctr_r;

//-------------------------------------------------------------------
// Status block -- $E2:0200-$02FF
//-------------------------------------------------------------------

//  +01 flags0: b0 LCD_ON . b1 CGB_MODE . b2 COMPAT . b3 SPEED2X
//              b4 LOCKED . b5 FIRST_FRAME . b6 LOG_OVF . b7 SNAP_VALID
// Everything here is the SNAPSHOT, including LOCKED: the whole block describes
// one instant, and a player that mixed a snapshot register with a live flag
// would have no way to say which frame it was looking at.  LOG_OVF is the
// published buffer's flag for the same reason -- it describes the log the
// player is about to read, not the one being recorded.
wire [7:0] status_flags0 = {snap_valid_r, log_ovf_pub_r, sn_first_frame_r, sn_locked_r,
                            sn_key1_r[7], sn_compat_r, ~sn_compat_r, sn_lcd_on_r};

// +30..3F mirrors the first sixteen bytes of GBDG so a player can print the
// bridge's own telemetry without the MCU in the loop.
reg [7:0] gbdg_mirror;
always @(*) begin
  case (SNES_ADDR[3:0])
    4'h0:    gbdg_mirror = "G";
    4'h1:    gbdg_mirror = "B";
    4'h2:    gbdg_mirror = "D";
    4'h3:    gbdg_mirror = "G";
    4'h4:    gbdg_mirror = GBC_WIRE_VER;
    4'h5:    gbdg_mirror = status_flags0;
    4'h6:    gbdg_mirror = frame_ctr_r;
    4'h7:    gbdg_mirror = go_seen_r;
    4'h8:    gbdg_mirror = seq_r[7:0];
    4'h9:    gbdg_mirror = seq_r[15:8];
    4'hA:    gbdg_mirror = snap_skipped_r[7:0];
    4'hB:    gbdg_mirror = snap_skipped_r[15:8];
    4'hC:    gbdg_mirror = STARVATION[7:0];
    4'hD:    gbdg_mirror = STARVATION[15:8];
    4'hE:    gbdg_mirror = log_ovf_ctr_r[7:0];
    default: gbdg_mirror = log_ovf_ctr_r[15:8];   // 4'hF
  endcase
end

reg [7:0] status_byte;
always @(*) begin
  case (SNES_ADDR[7:0])
    8'h00:   status_byte = GBC_WIRE_VER;
    8'h01:   status_byte = status_flags0;
    8'h02:   status_byte = sn_lcdc_r;
    8'h03:   status_byte = sn_scx_r;
    8'h04:   status_byte = sn_scy_r;
    8'h05:   status_byte = sn_wx_r;
    8'h06:   status_byte = sn_wy_r;
    8'h07:   status_byte = sn_bgp_r;
    8'h08:   status_byte = sn_obp0_r;
    8'h09:   status_byte = sn_obp1_r;
    8'h0A:   status_byte = sn_opri_r;
    8'h0B:   status_byte = sn_key1_r;
    8'h0C:   status_byte = ly_sync_r;
    8'h0D:   status_byte = frame_ctr_r;
    8'h0E:   status_byte = seq_r[7:0];
    8'h0F:   status_byte = seq_r[15:8];
    8'h10:   status_byte = dirty_chr_pub_r[7:0];
    8'h11:   status_byte = dirty_chr_pub_r[15:8];
    8'h12:   status_byte = dirty_chr_pub_r[23:16];
    8'h13:   status_byte = dirty_chr_pub_r[31:24];
    8'h14:   status_byte = dirty_chr_pub_r[39:32];
    8'h15:   status_byte = dirty_chr_pub_r[47:40];
    8'h16:   status_byte = dirty_obj_pub_r[7:0];
    8'h17:   status_byte = dirty_obj_pub_r[15:8];
    8'h18:   status_byte = dirty_obj_pub_r[23:16];
    8'h19:   status_byte = dirty_obj_pub_r[31:24];
    8'h1A:   status_byte = dirty_map_pub_r[7:0];
    8'h1B:   status_byte = dirty_map_pub_r[15:8];
    8'h1C:   status_byte = dirty_map_pub_r[23:16];
    8'h1D:   status_byte = dirty_map_pub_r[31:24];
    8'h1E:   status_byte = dirty_map_pub_r[39:32];
    8'h1F:   status_byte = dirty_map_pub_r[47:40];
    8'h20:   status_byte = dirty_map_pub_r[55:48];
    8'h21:   status_byte = dirty_map_pub_r[63:56];
    // RASTER (bit 3) is NOT an accumulator bit.  Contract section 5 lists
    // OAM/CRAM/REGS/MSEL as snapshot differences and RASTER separately as "the
    // log is not empty", which is a property of the log the player is reading
    // right now -- the same published LOG_N that sits three bytes further down
    // this block, so the two can never disagree inside one status DMA.
    8'h22:   status_byte = {dirty_misc_pub_r[7:4], |log_n_pub_r, dirty_misc_pub_r[2:0]};
    8'h23:   status_byte = log_n_pub_r[7:0];
    8'h24:   status_byte = {6'h00, log_n_pub_r[9:8]};
    // +40..57 (COMPAT_RAW) never arrives here: sel_craw steers it onto the
    // CRAM snapshot's read port and the stage-1 mux serves it.  +58..91 are
    // the LIVE C6 bytes out of c6_fb.v (wire $03, section 14.5); everything
    // else reserved falls through to $00.
    default: status_byte = (SNES_ADDR[7:4] == 4'h3) ? gbdg_mirror
                         : c6_stat_hit                ? c6_stat_byte
                         :                              8'h00;
  endcase
end

// The status block answers in ONE cycle because it reads no RAM -- except for
// COMPAT_RAW, which does.  Those twenty-four bytes are subtracted from the
// status branch below so they settle on the stage-1 path with the views; the
// SNES holds an address for eight cycles or more, so the settled value is the
// right one either way (see the header, THE READ PIPELINE).
reg [7:0] view_data_r;

always @(posedge CLK) begin
  view_data_r <= 8'h00;
  // The FB byte goes FIRST: it is the deepest path into this register (the
  // registered M9K output, plane pick, stretch) and at the end of the chain it
  // missed 84 MHz by 0.85 ns in the first pilot fit.  A settled address
  // selects one branch only, so the order is not observable -- except as the
  // "three cycles for the whole window" of the header.
  if (s1_fb_r)                   view_data_r <= c6_view_byte;
  else if (sel_stat & ~sel_craw) view_data_r <= status_byte;
  else if (view_sel_d1)          view_data_r <= view_byte;
end

assign DATA_OUT = view_data_r;

//-------------------------------------------------------------------
// GBDG -- MCU telemetry at PSRAM 0x810000-0x81003F
//-------------------------------------------------------------------

// Read through the CPU's DBG pipe (sgb_cpu.v), which addresses this block with
// DBG_ADDR[7:0], so 0x810040-0x8100FF reads back $00 instead of mirroring the
// block.  Little endian, same as everything else on the wire.
reg [7:0] dbg_data_r;

always @(posedge CLK) begin
  case (DBG_ADDR[7:0])
    8'h00:   dbg_data_r <= "G";
    8'h01:   dbg_data_r <= "B";
    8'h02:   dbg_data_r <= "D";
    8'h03:   dbg_data_r <= "G";
    8'h04:   dbg_data_r <= GBC_WIRE_VER;
    8'h05:   dbg_data_r <= status_flags0;
    8'h06:   dbg_data_r <= frame_ctr_r;
    8'h07:   dbg_data_r <= go_seen_r;
    8'h08:   dbg_data_r <= seq_r[7:0];
    8'h09:   dbg_data_r <= seq_r[15:8];
    8'h0A:   dbg_data_r <= snap_skipped_r[7:0];
    8'h0B:   dbg_data_r <= snap_skipped_r[15:8];
    // +0C starvation is sgb_cpu's own counter (M-cycles whose PSRAM access
    // was still in flight at the next bus edge), passed through like +10: it
    // has to read 0, and a copy here would only be a second place for it to
    // go stale.
    8'h0C:   dbg_data_r <= STARVATION[7:0];
    8'h0D:   dbg_data_r <= STARVATION[15:8];
    // +0E: how many frames lost entries to a full log.  Saturating, so "0"
    // really means "never" -- it is the number the phase gate reads.
    8'h0E:   dbg_data_r <= log_ovf_ctr_r[7:0];
    8'h0F:   dbg_data_r <= log_ovf_ctr_r[15:8];
    // +10 is gbc_clk's own number, passed through unregistered: it changes
    // once per SYNC strobe.
    8'h10:   dbg_data_r <= SYNC_ERR_LAST[7:0];
    8'h11:   dbg_data_r <= SYNC_ERR_LAST[15:8];
    8'h12:   dbg_data_r <= {7'h00, sn_locked_r};
    8'h13:   dbg_data_r <= {7'h00, sn_key1_r[7]};   // speed (KEY1), phase 3
    8'h14:   dbg_data_r <= commit_ctr_r[7:0];
    8'h15:   dbg_data_r <= commit_ctr_r[15:8];
    8'h16:   dbg_data_r <= consumed_ctr_r[7:0];
    8'h17:   dbg_data_r <= consumed_ctr_r[15:8];
    8'h18:   dbg_data_r <= free_ctr_r[7:0];
    8'h19:   dbg_data_r <= free_ctr_r[15:8];
    8'h1A:   dbg_data_r <= commit_busy_ctr_r[7:0];
    8'h1B:   dbg_data_r <= commit_busy_ctr_r[15:8];
    // +1C is sgb_cpu's clock-dilation counter, passed through like +0C: how
    // many M-cycles the GB's clock was held so that a CPU access could retire
    // before the edge that consumes it (double speed, OAM DMA and a busy
    // arbiter).  Non-zero is not an error -- +0C staying 0 beside it is the
    // proof the stretch did its job.
    8'h1C:   dbg_data_r <= DILATION[7:0];
    8'h1D:   dbg_data_r <= DILATION[15:8];
    // +1E is the layout revision of THIS block, not the wire version (+04):
    // $01 = the mailbox below exists, $02 = its sixth word and the C6 counters
    // at +2C..+35.  A core without it reads $00 here, which is how a reader
    // tells "the player published zero" from "no mailbox".
    8'h1E:   dbg_data_r <= GBDG_REV;
    // +20..+2B: the player's counters, as it last published them through
    // $EF0010-$EF001B (FRAMES, DEFER, DROPS, READY, REGSLATE, C6MODE).
    8'h20:   dbg_data_r <= mbx0_r[7:0];
    8'h21:   dbg_data_r <= mbx0_r[15:8];
    8'h22:   dbg_data_r <= mbx1_r[7:0];
    8'h23:   dbg_data_r <= mbx1_r[15:8];
    8'h24:   dbg_data_r <= mbx2_r[7:0];
    8'h25:   dbg_data_r <= mbx2_r[15:8];
    8'h26:   dbg_data_r <= mbx3_r[7:0];
    8'h27:   dbg_data_r <= mbx3_r[15:8];
    8'h28:   dbg_data_r <= mbx4_r[7:0];
    8'h29:   dbg_data_r <= mbx4_r[15:8];
    8'h2A:   dbg_data_r <= mbx5_r[7:0];
    8'h2B:   dbg_data_r <= mbx5_r[15:8];
    // +2C..+35: the C6 counters (contract section 10 / 14.9), saturating,
    // zeroed by FB_EN 0->1 and by RST; +34/+35 mirror status +58/+5D.
    8'h2C:   dbg_data_r <= c6_rows_dropped[7:0];
    8'h2D:   dbg_data_r <= c6_rows_dropped[15:8];
    8'h2E:   dbg_data_r <= c6_rowdone_bad[7:0];
    8'h2F:   dbg_data_r <= c6_rowdone_bad[15:8];
    8'h30:   dbg_data_r <= c6_cl_switches[7:0];
    8'h31:   dbg_data_r <= c6_cl_switches[15:8];
    8'h32:   dbg_data_r <= c6_frames[7:0];
    8'h33:   dbg_data_r <= c6_frames[15:8];
    8'h34:   dbg_data_r <= c6_flags;
    8'h35:   dbg_data_r <= c6_cl;
    default: dbg_data_r <= 8'h00;
  endcase
end

assign DBG_DATA_OUT = dbg_data_r;

// CGB registers the status block does not publish, plus the two PPU edges the
// bridge does not need.  Named here so the synthesiser reports them as read and
// a later phase does not have to re-thread sgb.v to reach them.
// verilator lint_off UNUSED
wire _unused_ok = &{1'b0, PPU_VSYNC_EDGE, TAP_KEY0, TAP_VBK,
                    TAP_SVBK, TAP_SPEED_REQ, TAP_CRAM_IDX, TAP_CRAM_DATA, 1'b0};
// verilator lint_on UNUSED

endmodule
