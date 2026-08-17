// a26_video.v -- TIA raster -> SNES framebuffer set (PSRAM writer).
//
// Turns the TIA's per-colour-clock 2-bit code stream into exactly what the
// SNES-side player expects to find in a buffer set, and writes it to PSRAM
// through the arbiter's free-slot write port (BUF_*, same handshake shape the
// SMS core uses).  Set layout, byte for byte (offsets relative to the base of
// the set; the front/back base is added by main.v, this module only emits
// BUF_BASE + offset):
//
//   0x0000  2bpp tiles, linear bitmap    height_rows * cols * 16, max 0x4000
//                                        160 px: 20x24 = 7680 B
//                                        256 px: 32x24 = 12288 B
//   0x4000  GAP, 0x200 B reserved zero   where the saturating clamp aims
//   0x4200  control block, 16 B
//   0x4210  SPAN table, 0x40 B           2 B/row: first/last CHANGED tile
//   0x4250  free (zeros)
//   0x4400  HDMA ch0  $2121  2 B/line    CGADD = 0            449 B
//   0x4600  HDMA ch1  $2122  3 B/line    colour 0 = COLUBK    673 B
//   0x4900  HDMA ch2  $2122  3 B/line    colour 1 = COLUPF    673 B
//   0x4C00  HDMA ch3  $2122  3 B/line    colour 2 = COLUP0    673 B
//   0x4F00  HDMA ch4  $2122  3 B/line    colour 3 = COLUP1    673 B
//   0x5200  HDMA ch5  $2100  2 B/line    letterbox forced blank 449 B
//
// A tile is 16 B, two bytes per tile line, stride 16: byte 2k is plane 0 of
// line k and 2k+1 is plane 1, bit 7 = leftmost pixel; the tile index is
// tile_row*cols + col.  The TIA code IS the colour index, and the four line
// colours land in CGRAM 0..3 by HDMA, so nothing here knows about RGB except
// the palette lookup that fills the colour tables.
//
// All six tables carry 224 entries plus a terminator with IDENTICAL $01
// headers.  Unequal tables would desynchronise CGADD and shift the colours of
// the rest of the frame.  Entry i drives visible line i+1, so entries 0..15
// and 208..223 are the letterbox (black colours, ch5 forced blank) and 16..207
// carry content lines 0..191.
//
// FRAMING
//   Line 0 of the content is the first line with VBLANK off after VSYNC; the
//   content line counter SATURATES at height_rows*8 (taller kernels are
//   cropped) and short frames are PADDED to a full set of tile rows with code
//   0 and colour $00, so a set always holds complete tile rows and never
//   depends on what the previous frame left in this set.  A frame watchdog
//   closes the frame if the line count runs past WATCHDOG_LINES, so a kernel
//   that never pulses VSYNC cannot stall the double buffer.
//
// CHANGE TRACKING (dirty bitmap + span)
//   One digest PER TILE, compared against the LAST EMITTED frame (N-1, not the
//   back set: diffing against N-2 would erase the single-frame sprite flicker
//   that is universal on this machine).  Per tile row that yields the first
//   and last changed tile -- the SPAN -- and the dirty bit is simply "the span
//   is not empty", so the two fields can never disagree.
//
//   The digest is CRC32 per tile.  The contract floor is 16 bits (8 would give
//   a ~20%/frame chance of shrinking a span and leaving an 8 px column of the
//   previous frame on screen); 32 bits takes the residual from 2^-16 to 2^-32,
//   which also makes the byte-exact comparison against an exact-compare model
//   deterministic instead of flaky.  The cost is block RAM, not logic, and it
//   narrows to 16 bits by changing the digest width and the two RAM widths.
//
//   Both fields are ACCUMULATIVE over the same window and the accounting is
//   FRAME-ATOMIC: the frame being written owns spancur, which a swap cannot
//   touch, while spancarry holds what earlier frames published and is voided
//   by SWAP_CONSUMED.  What gets published is the union.  A single accumulator
//   would drop the rows of the in-flight frame whenever the swap landed
//   mid-render, and the player would never be told to fetch them again.
//
// THROUGHPUT
//   Per visible line the writer emits 2*cols tile bytes plus 4 colour entries
//   (52 B in 160 mode), and at frame end 1366 B of constant tables, span and
//   control block.  With one byte every two cycles that is ~120 cycles per
//   line against ~6100 cycles of line time, so the ping-pong line buffer is
//   never at risk; the only lines that can be dropped are those arriving while
//   the frame-end burst runs (counted in DBG_OVERRUN), which in a real kernel
//   is inside vertical blank.

`timescale 1ns / 1ps

module a26_video (
  input             CLK,
  input             RST,
  input             TIA_CE,
  input             WIDTH_256,        // feat16[5]: 0 = 160 px, 1 = 256 px

  // TIA raster
  input             VID_VISIBLE,
  input      [1:0]  VID_CODE,
  input             LINE_START,
  input             LINE_VIS_START,
  input      [6:0]  COLUBK,
  input      [6:0]  COLUPF,
  input      [6:0]  COLUP0,
  input      [6:0]  COLUP1,
  input             VSYNC_EDGE,

  // PSRAM write port (free-slot client of the main.v arbiter)
  output reg        BUF_WRQ,
  output reg [23:0] BUF_ADDR,
  output reg [7:0]  BUF_DATA,
  input             BUF_WACK,

  input             TR_GATE,          // hold FRAME_DONE while the back set is unconsumed
  input             SWAP_CONSUMED,    // player took the swap -> drop the carry
  output reg        FRAME_DONE,

  output     [7:0]  DBG_LINES,
  output     [7:0]  DBG_OVERRUN,
  output     [31:0] DBG_DIRTY
);

  // ------------------------------------------------------------- constants
  localparam [23:0] BUF_BASE = 24'h200000;   // main.v adds 0x180000 / 0x190000

  localparam [15:0] OFF_CTRL = 16'h4200;
  localparam [15:0] OFF_SPAN = 16'h4210;
  localparam [15:0] OFF_CH0  = 16'h4400;
  localparam [15:0] OFF_CH1  = 16'h4600;
  localparam [15:0] OFF_CH2  = 16'h4900;
  localparam [15:0] OFF_CH3  = 16'h4C00;
  localparam [15:0] OFF_CH4  = 16'h4F00;
  localparam [15:0] OFF_CH5  = 16'h5200;

  localparam [7:0]  TILE_ROWS      = 8'd24;  // height_rows (v0; structural max 28)
  localparam [5:0]  SPAN_ROWS      = 6'd32;  // rows the span table always covers
  localparam [7:0]  CONTENT_LINES  = 8'd192; // TILE_ROWS * 8
  localparam [8:0]  WATCHDOG_LINES = 9'd320;
  localparam [7:0]  CONTRACT_VER   = 8'h02;

  localparam [15:0] LB_TAIL_OFF = 16'd576;   // 624 - 48: second letterbox run
  localparam [15:0] LB_TERM_OFF = 16'd672;   // 224*3

  localparam [8:0]  TAB2_LAST = 9'd448;      // last byte of a 2 B/entry table

  // An empty span is the IDENTITY of min/max: first = 0xFF, last = 0x00.  That
  // makes the union a blind min/max with no special case, and it is also the
  // value that goes on the wire for a row with nothing to send (a row whose
  // dirty bit is clear), so the emit path has no special case either.
  localparam [15:0] SPAN_EMPTY = {8'hFF, 8'h00};

  // ---------------------------------------------------------- palette ROM
  // Synchronous, dedicated read port (house M9K rule): the table goes in a
  // block RAM instead of ~600 LEs of 128:1 combinational mux.
  `include "a26_palette.vh"
  reg  [14:0] pal_q;

  // ---------------------------------------------------------- line capture
  reg         cap_bank;
  reg  [7:0]  sh_p0, sh_p1;
  reg  [2:0]  gcnt;
  reg  [5:0]  col;
  reg         pix_active;
  reg         line_vis;
  reg  [6:0]  snap_bk [0:1];
  reg  [6:0]  snap_pf [0:1];
  reg  [6:0]  snap_p0 [0:1];
  reg  [6:0]  snap_p1 [0:1];
  reg  [7:0]  overrun;

  reg         job_pend;
  reg         job_bank;
  // The index a captured line occupies in the PICTURE, which is not the same
  // thing as how many lines we managed to emit.  It advances on every visible
  // line, captured or not, so a line lost to backpressure leaves a HOLE where
  // it belongs instead of pulling everything below it up the screen.
  reg  [7:0]  vline;
  reg  [7:0]  line_idx;      // index of the line currently being captured
  reg  [7:0]  job_line;      // ...of the line handed to the emitter
  reg         starved;          // a line was lost to backpressure this frame
  // Lines dropped while the PREVIOUS frame was still being flushed belong to
  // the frame that is only just starting, and they are lost before that frame's
  // starved flag is cleared -- so they have to be parked in their own bit and
  // handed over at the reset, otherwise the shortfall looks like a legitimately
  // short frame and the packer pads the tail with a band of zeros.
  reg         starved_next;
  reg         pad_ok_r;        // this frame's shortfall is a real short frame
  reg         cap_hold;         // both banks claimed: skip this line entirely

  // 2 banks x 32 columns of {plane1, plane0}
  (* ramstyle = "M9K" *) reg [15:0] lbuf [0:63];
  reg  [15:0] lb_q;

  wire [2:0]  glast  = WIDTH_256 ? 3'd4 : 3'd7;
  wire [5:0]  colmax = WIDTH_256 ? 6'd31 : 6'd19;

  wire        dopix = TIA_CE & (LINE_VIS_START | pix_active);
  wire [2:0]  g = LINE_VIS_START ? 3'd0 : gcnt;
  wire [5:0]  c = LINE_VIS_START ? 6'd0 : col;

  // 256 px mode stretches every 5 source pixels to 8 with the pattern
  // [2,1,2,1,2], applied to the CODES as they are shifted in -- one tile
  // column is exactly one group of five, so no repacking is needed later.
  // The pattern is SYMMETRIC on purpose: REFPx mirrors sprites, and an
  // asymmetric nearest-neighbour spread would not survive the mirror.
  wire        two = WIDTH_256 & ~g[0];
  wire [7:0]  shp0_n = two ? {sh_p0[5:0], VID_CODE[0], VID_CODE[0]}
                           : {sh_p0[6:0], VID_CODE[0]};
  wire [7:0]  shp1_n = two ? {sh_p1[5:0], VID_CODE[1], VID_CODE[1]}
                           : {sh_p1[6:0], VID_CODE[1]};
  wire        grp_done  = (g == glast);
  wire        line_done = dopix & grp_done & (c == colmax);

  // -------------------------------------------------------------- emitter
  localparam [4:0] ST_IDLE   = 5'd0,  ST_TRD    = 5'd1,  ST_TRD2 = 5'd2,
                   ST_TW0    = 5'd3,  ST_TW1    = 5'd4,  ST_HRD  = 5'd5,
                   ST_HB0    = 5'd6,  ST_HB1    = 5'd7,  ST_HB2  = 5'd8,
                   ST_PADCK  = 5'd9,  ST_LB     = 5'd10, ST_CH0  = 5'd11,
                   ST_CH5    = 5'd12, ST_SPANRD = 5'd13, ST_SPANRD2 = 5'd14,
                   ST_SPANB0 = 5'd15, ST_SPANB1 = 5'd16,
                   ST_CTRL   = 5'd17, ST_DONE   = 5'd18, ST_TDIG = 5'd19,
                   ST_GAP    = 5'd20;

  reg  [4:0]  st;
  reg  [7:0]  cur_line;
  reg         cur_bank, cur_pad;
  reg  [5:0]  tcol;
  reg  [1:0]  chan;
  reg  [15:0] tile_addr, hd_addr, hd_off;
  reg  [8:0]  cnt;                    // table byte counter
  reg  [6:0]  lbc;                    // letterbox byte counter 0..96
  reg  [1:0]  m3;                     // mod-3 phase of a 3 B entry
  reg  [7:0]  cline;                  // content lines emitted this frame
  reg         capturing;
  reg  [8:0]  frame_lines;
  reg         frame_end_req;
  reg         first_frame;
  reg  [7:0]  frame_ctr;

  reg  [15:0] a_r;
  reg  [7:0]  d_r;
  reg         v_r;
  wire        slot_free = ~v_r | ~BUF_WRQ;

  // per-tile digests: accumulators for the row in flight, and the whole
  // previous frame's values
  (* ramstyle = "M9K" *) reg [31:0] tiledig [0:31];
  (* ramstyle = "M9K" *) reg [31:0] prevdig [0:1023];
  reg  [31:0] td_q, pd_q, dig_acc, dig_r;

  // span store: what this frame found, and what earlier frames published and
  // the player has not consumed yet
  (* ramstyle = "M9K" *) reg [15:0] spancur   [0:31];
  (* ramstyle = "M9K" *) reg [15:0] spancarry [0:31];
  reg  [15:0] spc_q, spy_q;
  reg  [7:0]  span_first, span_last;
  reg  [4:0]  sp_row;
  reg         carry_void;
  reg  [31:0] dirty_pub;

  assign DBG_LINES   = cline;
  assign DBG_OVERRUN = overrun;
  assign DBG_DIRTY   = dirty_pub;

  // Reflected CRC-32 (0xEDB88320).  The loop is unrolled at elaboration and
  // the result is a linear XOR function of {crc, byte}, i.e. a flat XOR tree
  // a couple of LUT levels deep -- not eight levels of logic.
  function [31:0] crc32_byte;
    input [31:0] cc;
    input [7:0]  bb;
    integer k;
    reg [31:0] x;
    begin
      x = cc ^ {24'd0, bb};
      for (k = 0; k < 8; k = k + 1)
        x = x[0] ? ((x >> 1) ^ 32'hEDB88320) : (x >> 1);
      crc32_byte = x;
    end
  endfunction

  // line geometry of the line that is about to start (cline, not cur_line)
  // The line about to be set up sits at job_line when the emitter picks a
  // captured line, and at cline when it is filling a short frame's tail.
  wire [7:0]  geo_line = (st == ST_IDLE) ? job_line : cline;
  wire [15:0] trow16 = {11'd0, geo_line[7:3]};
  wire [15:0] rowoff = WIDTH_256 ? (trow16 << 9) : ((trow16 << 8) + (trow16 << 6));
  wire [15:0] tbase  = rowoff + {12'd0, geo_line[2:0], 1'b0};
  wire [15:0] hoff   = 16'd48 + {8'd0, geo_line} + {7'd0, geo_line, 1'b0};

  // colour table base of the channel being emitted
  reg [15:0] chbase;
  always @(*) case (chan)
    2'd0:    chbase = OFF_CH1;
    2'd1:    chbase = OFF_CH2;
    2'd2:    chbase = OFF_CH3;
    default: chbase = OFF_CH4;
  endcase

  // palette index for the current channel of the current line
  wire [6:0] sel_bk = cur_bank ? snap_bk[1] : snap_bk[0];
  wire [6:0] sel_pf = cur_bank ? snap_pf[1] : snap_pf[0];
  wire [6:0] sel_p0 = cur_bank ? snap_p0[1] : snap_p0[0];
  wire [6:0] sel_p1 = cur_bank ? snap_p1[1] : snap_p1[0];
  reg [6:0] pal_sel;
  always @(*) case (chan)
    2'd0:    pal_sel = sel_bk;
    2'd1:    pal_sel = sel_pf;
    2'd2:    pal_sel = sel_p0;
    default: pal_sel = sel_p1;
  endcase
  wire [6:0] pal_ra = cur_pad ? 7'd0 : pal_sel;

  // tile byte about to be emitted and the digest it produces
  wire [7:0]  tbyte    = cur_pad ? 8'h00 : ((st == ST_TW0) ? lb_q[7:0] : lb_q[15:8]);
  wire        row_head = (cur_line[2:0] == 3'd0);   // first line of a tile row
  wire        row_tail = (cur_line[2:0] == 3'd7);   // last line: digests complete
  wire        row_end  = row_tail & (tcol == colmax);
  // The digest is completed in ST_TW1 and PARKED in dig_r; the comparison
  // against the previous frame happens a cycle later, in ST_TDIG.  Keeping the
  // CRC tree and the 32-bit compare in the same cycle put the block RAM array
  // delay, the CRC and the compare in one cone, which is what made this the
  // slowest path in the core -- and it could not be relaxed with a multicycle
  // because the dig_acc -> span_first leg is genuinely one cycle long.
  wire [31:0] dig_n    = crc32_byte(dig_acc, tbyte);
  wire        tile_chg = row_tail & (first_frame | (dig_r != pd_q));
  wire [7:0]  tcol8    = {2'd0, tcol};
  // blind min/max: tcol only ever grows across the sweep, so this collapses to
  // "first changed" and "last changed"
  wire [7:0]  sf_n     = (tile_chg & (tcol8 < span_first)) ? tcol8 : span_first;
  wire [7:0]  sl_n     = (tile_chg & (tcol8 > span_last))  ? tcol8 : span_last;

  // read addresses (registered inside the RAM blocks; the sources are stable
  // for the whole state sequence that consumes the data)
  wire [5:0] lb_ra = {cur_bank, tcol[4:0]};
  wire [4:0] td_ra = tcol[4:0];
  wire [9:0] pd_ra = {cur_line[7:3], tcol[4:0]};
  wire [4:0] sp_ra = sp_row;

  // span union of this frame with the unconsumed carry
  // sp_row is 5 bits, so it always addresses inside the 32-row table; the only
  // real test is whether the row carries content at this height_rows.
  wire        sp_in_range = ({3'd0, sp_row} < TILE_ROWS);
  wire [15:0] sp_cur = sp_in_range ? spc_q : SPAN_EMPTY;
  wire [15:0] sp_car = (sp_in_range & ~carry_void) ? spy_q : SPAN_EMPTY;
  wire [7:0]  u_first = (sp_cur[15:8] < sp_car[15:8]) ? sp_cur[15:8] : sp_car[15:8];
  wire [7:0]  u_last  = (sp_cur[7:0]  > sp_car[7:0])  ? sp_cur[7:0]  : sp_car[7:0];
  wire        u_ne    = (u_first <= u_last);   // only the dirty bit needs this

  // ch5: forced blank over the letterbox, full brightness over the content
  wire [7:0]  ch5_entry = cnt[8:1];
  wire [7:0]  ch5_val = ((ch5_entry < 8'd16) || (ch5_entry > 8'd207)) ? 8'h80 : 8'h0F;

  // control block (contract v3, wire version $02)
  reg [7:0] ctrl_byte;
  always @(*) case (cnt[3:0])
    4'd0:    ctrl_byte = {5'd0, frame_ctr[0], 1'b0, WIDTH_256};
    4'd1:    ctrl_byte = TILE_ROWS;
    4'd4:    ctrl_byte = dirty_pub[7:0];
    4'd5:    ctrl_byte = dirty_pub[15:8];
    4'd6:    ctrl_byte = dirty_pub[23:16];
    4'd7:    ctrl_byte = dirty_pub[31:24];
    4'd15:   ctrl_byte = CONTRACT_VER;
    default: ctrl_byte = 8'h00;
  endcase

  // Rows skipped ENTIRELY by a hole must be reported unchanged, so the emitter
  // walks cline up to the job's row first, marking each one empty on the way.
  wire gap_needed = job_pend & (job_line < CONTENT_LINES)
                  & (cline[7:3] != job_line[7:3]);
  wire job_take   = (st == ST_IDLE) & job_pend & ~gap_needed;

  // The line buffer is a two-bank ping-pong and the emitter reads its bank
  // LAZILY, one tile column at a time, for as long as the write port makes it
  // wait.  So the bank holding a queued or in-flight line is OWNED by the
  // emitter and the capture must not touch it: with the SNES DMAing the front
  // set for milliseconds at a time, a stalled job easily outlives a couple of
  // scanlines, and a capture that recycled the bank underneath it would feed
  // the emitter whatever the newer line left there -- zeros, if that newer
  // line was blanked.
  // TWO lines can be outstanding at once -- one still draining and one already
  // queued behind it -- so BOTH banks can be claimed at the same time.  Each
  // bank is tracked on its own; naming only one of them left the in-flight
  // bank open to being recycled underneath the emitter.
  wire tile_reading = (st == ST_TRD) | (st == ST_TRD2) | (st == ST_TW0)
                    | (st == ST_TW1) | (st == ST_TDIG);
  // The emitter also reads the per-line COLOUR snapshot, and it does that in
  // the HDMA states -- after it is done with the tile bytes.  The lock has to
  // span both or the capture can overwrite the colours of the line being
  // emitted.
  wire hdma_reading = (st == ST_HRD) | (st == ST_HB0) | (st == ST_HB1) | (st == ST_HB2);
  wire emit_lock    = (tile_reading | hdma_reading) & ~cur_pad;
  wire bank0_locked = (emit_lock & ~cur_bank) | (job_pend & ~job_bank);
  wire bank1_locked = (emit_lock &  cur_bank) | (job_pend &  job_bank);
  wire next_locked  = (~cap_bank) ? bank1_locked : bank0_locked;
  wire this_locked  = ( cap_bank) ? bank1_locked : bank0_locked;
  wire wd_trip  = LINE_START & (frame_lines == (WATCHDOG_LINES - 9'd1));
  wire tile_we  = (st == ST_TDIG);
  wire frame_reset = (st == ST_DONE) & ~v_r & ~BUF_WRQ;
  // rows that were never captured because the write port starved us: their
  // bytes in PSRAM stay as the previous frame left them, so they are reported
  // UNCHANGED and nothing is emitted for them
  // starved_next counts here too: when the burst covers the frame CLOSE, the
  // lines of the frame that is starting are lost before this frame's flag is
  // handed over, and testing only `starved` let that shortfall look like a
  // legitimately short frame -- which pads the whole tail with zeros.  Erring
  // towards skipping only ever costs old content staying up for a frame.
  // Padding a shortfall with zeros is only ever right for a frame that really
  // WAS short: it ran to its VSYNC with nothing lost, and it did capture
  // something.  A frame that captured NOTHING is not a short frame, it is a
  // frame the close sequence swallowed whole -- emitting it as 24 rows of black
  // is strictly worse than leaving the previous picture up.
  // Decided ONCE, when the frame closes: the skip path advances cline itself,
  // so a live expression would flip half-way through and start padding again.
  wire span_skip = (st == ST_PADCK) & (cline < CONTENT_LINES) & ~pad_ok_r;

  // ------------------------------------------------------------ RAM ports
  // One block per memory, each written and read on CLK with a registered read
  // address, so block RAM inference is unambiguous.
  always @(posedge CLK) begin
    if (dopix & grp_done) lbuf[{cap_bank, c[4:0]}] <= {shp1_n, shp0_n};
    lb_q <= lbuf[lb_ra];
  end

  always @(posedge CLK) pal_q <= a26_pal_rom[pal_ra];

  always @(posedge CLK) begin
    if (tile_we) tiledig[td_ra] <= dig_r;
    td_q <= tiledig[td_ra];
  end

  always @(posedge CLK) begin
    if (tile_we & row_tail) prevdig[pd_ra] <= dig_r;
    pd_q <= prevdig[pd_ra];
  end

  always @(posedge CLK) begin
    if (tile_we & row_end)          spancur[cur_line[7:3]] <= {sf_n, sl_n};
    else if (span_skip)             spancur[cline[7:3]]    <= SPAN_EMPTY;
    else if (st == ST_GAP)          spancur[cline[7:3]]    <= SPAN_EMPTY;
    spc_q <= spancur[sp_ra];
  end

  always @(posedge CLK) begin
    if ((st == ST_SPANB1) & slot_free & sp_in_range)
      spancarry[sp_ra] <= {u_first, u_last};
    spy_q <= spancarry[sp_ra];
  end

  // -------------------------------------------------------- capture domain
  always @(posedge CLK) begin
    if (RST) begin
      cap_bank <= 1'b0; sh_p0 <= 8'd0; sh_p1 <= 8'd0;
      gcnt <= 3'd0; col <= 6'd0; pix_active <= 1'b0; line_vis <= 1'b0;
      job_pend <= 1'b0; job_bank <= 1'b0; overrun <= 8'd0; starved <= 1'b0;
      cap_hold <= 1'b0; starved_next <= 1'b0;
      vline <= 8'd0; line_idx <= 8'd0; job_line <= 8'd0;
    end else begin
      if (job_take) job_pend <= 1'b0;
      if (frame_reset) begin
        starved      <= starved_next;
        starved_next <= 1'b0;
      end

      if (LINE_START) begin
        // Take the other bank if it is free; otherwise re-use this one if IT
        // is free; if both are claimed, capture NOTHING this line.  Dropping a
        // line costs the old content staying on screen for a frame, which is
        // invisible -- writing into a bank the emitter is still reading is what
        // put a black band on the screen.
        if (!next_locked)      cap_bank <= ~cap_bank;
        cap_hold   <= next_locked & this_locked;
        pix_active <= 1'b0;
      end

      if (VSYNC_EDGE | wd_trip) vline <= 8'd0;

      if (LINE_VIS_START) begin
        if (VID_VISIBLE) begin
          line_idx <= vline;              // this line's place in the picture
          // SATURATES: a kernel that never pulses VSYNC can run past 256
          // visible lines, and a wrapping index would drag those lines back
          // into the valid range and overwrite rows already emitted.
          vline    <= (vline == 8'hFF) ? 8'hFF : (vline + 8'd1);
        end
        // Do not shift blanked lines into the buffer at all.  The TIA forces
        // VID_CODE to 0 while blanked, so capturing them filled whole banks
        // with zeros -- which is what a stalled job used to emit.
        pix_active <= VID_VISIBLE & ~cap_hold;
        line_vis   <= VID_VISIBLE & ~cap_hold;
        if (VID_VISIBLE & cap_hold) starved <= 1'b1;
        // While holding, cap_bank still points at the emitter's bank -- do not
        // scribble this line's colours over the ones it is about to send.
        if (~cap_hold) begin
          snap_bk[cap_bank] <= COLUBK;
          snap_pf[cap_bank] <= COLUPF;
          snap_p0[cap_bank] <= COLUP0;
          snap_p1[cap_bank] <= COLUP1;
        end
      end

      if (dopix) begin
        sh_p0 <= shp0_n;
        sh_p1 <= shp1_n;
        gcnt  <= grp_done ? 3'd0 : (g + 3'd1);
        col   <= grp_done ? (c + 6'd1) : c;
      end

      if (line_done) begin
        pix_active <= 1'b0;
        // A line past the content height is dropped HERE, so the crop costs
        // nothing at all downstream: no job, no bank latch, and nothing for the
        // overrun counter to blame.  Kernels taller than the content height are
        // the normal case (many titles run 220+ visible lines), so the excess
        // must not be able to perturb any end-of-frame state.
        if (capturing & line_vis & (line_idx < CONTENT_LINES)) begin
          if (job_pend & ~job_take) begin
            overrun <= overrun + 8'd1;
            starved <= 1'b1;         // the shortfall is backpressure, not a short frame
          end
          else begin
            job_pend <= 1'b1;
            job_bank <= cap_bank;
            job_line <= line_idx;
          end
        end else if (~capturing & line_vis) begin
          // the frame-close flush is still running and this visible line has
          // nowhere to go: it belongs to the NEXT frame's shortfall
          starved_next <= 1'b1;
        end
      end
    end
  end

  // ------------------------------------------------------- PSRAM write port
  always @(posedge CLK) begin
    if (RST) begin
      BUF_WRQ <= 1'b0; BUF_ADDR <= 24'd0; BUF_DATA <= 8'd0;
    end else if (BUF_WRQ) begin
      if (BUF_WACK) BUF_WRQ <= 1'b0;
    end else if (v_r) begin
      BUF_WRQ  <= 1'b1;
      BUF_ADDR <= BUF_BASE + {8'd0, a_r};
      BUF_DATA <= d_r;
    end
  end

  // -------------------------------------------------------- emitter domain
  always @(posedge CLK) begin
    FRAME_DONE <= 1'b0;

    if (RST) begin
      st <= ST_IDLE; v_r <= 1'b0; a_r <= 16'd0; d_r <= 8'd0;
      cur_line <= 8'd0; cur_bank <= 1'b0; cur_pad <= 1'b0;
      tcol <= 6'd0; chan <= 2'd0; tile_addr <= 16'd0; hd_addr <= 16'd0;
      hd_off <= 16'd0; cnt <= 9'd0; lbc <= 7'd0; m3 <= 2'd0;
      cline <= 8'd0; capturing <= 1'b1; frame_lines <= 9'd0;
      frame_end_req <= 1'b0; first_frame <= 1'b1; frame_ctr <= 8'd0; pad_ok_r <= 1'b0;
      dig_acc <= 32'hFFFFFFFF; dig_r <= 32'hFFFFFFFF;
      span_first <= SPAN_EMPTY[15:8]; span_last <= SPAN_EMPTY[7:0];
      sp_row <= 5'd0; carry_void <= 1'b1; dirty_pub <= 32'd0;
    end else begin
      if (~BUF_WRQ & v_r) v_r <= 1'b0;      // handed over to the write port
      // A consumed swap drops everything earlier frames published; the span of
      // the frame in flight lives in spancur and is untouched.
      if (SWAP_CONSUMED) carry_void <= 1'b1;
      if (LINE_START & (frame_lines != 9'd511)) frame_lines <= frame_lines + 9'd1;

      case (st)
        // ---- idle: take a captured line, or start closing the frame ----
        ST_IDLE: begin
          if (gap_needed) begin
            st <= ST_GAP;
          end else if (job_pend) begin
            if (job_line < CONTENT_LINES) begin
              cur_line <= job_line;
              cur_bank <= job_bank;
              cur_pad  <= 1'b0;
              cline    <= job_line + 8'd1;
              tile_addr <= tbase;
              hd_off    <= hoff;
              tcol      <= 6'd0;
              span_first <= SPAN_EMPTY[15:8]; span_last <= SPAN_EMPTY[7:0];
              st <= ST_TRD;
            end
            // else: past the content height -> cropped, drop the line
          end else if (frame_end_req) begin
            frame_end_req <= 1'b0;
            capturing     <= 1'b0;
            pad_ok_r      <= ~starved & ~starved_next & (cline != 8'd0);
            // a muted frame emitted nothing, so there is nothing to flush and
            // nothing to announce -- go straight back to capturing
            st            <= ST_PADCK;
          end
        end

        // ---- one line of tiles (two block RAM latency cycles, then bytes) ----
        ST_TRD:  st <= ST_TRD2;
        ST_TRD2: st <= ST_TW0;

        ST_TW0: if (slot_free) begin
          v_r <= 1'b1; a_r <= tile_addr; d_r <= tbyte;
          // the digest of a tile covers its 16 bytes and nothing else, so the
          // accumulator restarts from the seed on the row's first line
          dig_acc <= crc32_byte(row_head ? 32'hFFFFFFFF : td_q, tbyte);
          tile_addr <= tile_addr + 16'd1;
          st <= ST_TW1;
        end

        ST_TW1: if (slot_free) begin
          v_r <= 1'b1; a_r <= tile_addr; d_r <= tbyte;
          tile_addr <= tile_addr + 16'd15;
          dig_r <= dig_n;                 // park the finished tile digest
          st <= ST_TDIG;
        end

        // Digest compare / span update.  One extra cycle per tile column (20
        // per line in 160 mode) against ~6100 cycles of line time, and it is
        // what keeps the CRC tree out of the same cone as the previous-frame
        // compare.  Emits nothing, so it never stalls on the write port; the
        // RAM write-backs above are gated on this state.
        ST_TDIG: begin
          span_first <= sf_n;
          span_last  <= sl_n;
          if (tcol == colmax) begin
            chan <= 2'd0;
            st   <= ST_HRD;
          end else begin
            tcol <= tcol + 6'd1;
            st   <= ST_TRD;
          end
        end

        // ---- the four colour-table entries of this line ----
        ST_HRD: begin hd_addr <= chbase + hd_off; st <= ST_HB0; end

        ST_HB0: if (slot_free) begin
          v_r <= 1'b1; a_r <= hd_addr; d_r <= 8'h01;
          hd_addr <= hd_addr + 16'd1;
          st <= ST_HB1;
        end
        ST_HB1: if (slot_free) begin
          v_r <= 1'b1; a_r <= hd_addr; d_r <= pal_q[7:0];
          hd_addr <= hd_addr + 16'd1;
          st <= ST_HB2;
        end
        ST_HB2: if (slot_free) begin
          v_r <= 1'b1; a_r <= hd_addr; d_r <= {1'b0, pal_q[14:8]};
          if (chan == 2'd3) st <= cur_pad ? ST_PADCK : ST_IDLE;
          else begin chan <= chan + 2'd1; st <= ST_HRD; end
        end

        // ---- pad the frame out to a full set of tile rows ----
        // walk over a whole row nobody captured: nothing is written, the row is
        // reported unchanged, and the bytes already in PSRAM stay put
        ST_GAP: begin
          cline <= {cline[7:3] + 5'd1, 3'b000};
          st    <= ST_IDLE;
        end

        ST_PADCK: begin
          if (span_skip) begin
            // Skip a whole tile row without writing a single byte.  Padding it
            // with zeros is what turned a starved frame into a black band; the
            // old content is still on screen and still correct-looking, and the
            // row self-heals next frame because its digest is compared against
            // the frame that actually produced those bytes.
            cline <= {cline[7:3] + 5'd1, 3'b000};
          end else if (cline < CONTENT_LINES) begin
            cur_line <= cline;
            cur_pad  <= 1'b1;
            cline    <= cline + 8'd1;
            tile_addr <= tbase;
            hd_off    <= hoff;
            tcol      <= 6'd0;
            span_first <= SPAN_EMPTY[15:8]; span_last <= SPAN_EMPTY[7:0];
            st <= ST_TRD;
          end else begin
            chan <= 2'd0; lbc <= 7'd0; m3 <= 2'd0;
            st <= ST_LB;
          end
        end

        // ---- letterbox entries (0..15, 208..223) + terminator, 4 tables ----
        ST_LB: if (slot_free) begin
          v_r <= 1'b1;
          a_r <= chbase + ((lbc < 7'd48)  ? {9'd0, lbc}
                        : (lbc < 7'd96) ? ({9'd0, lbc} + LB_TAIL_OFF)
                                        : LB_TERM_OFF);
          d_r <= ((lbc >= 7'd96) || (m3 != 2'd0)) ? 8'h00 : 8'h01;
          m3  <= (m3 == 2'd2) ? 2'd0 : (m3 + 2'd1);
          if (lbc == 7'd96) begin
            lbc <= 7'd0; m3 <= 2'd0;
            if (chan == 2'd3) begin cnt <= 9'd0; st <= ST_CH0; end
            else chan <= chan + 2'd1;
          end else lbc <= lbc + 7'd1;
        end

        // ---- ch0: CGADD = 0 on every line ----
        ST_CH0: if (slot_free) begin
          v_r <= 1'b1;
          a_r <= OFF_CH0 + {7'd0, cnt};
          d_r <= (cnt == TAB2_LAST) ? 8'h00 : (cnt[0] ? 8'h00 : 8'h01);
          if (cnt == TAB2_LAST) begin cnt <= 9'd0; st <= ST_CH5; end
          else cnt <= cnt + 9'd1;
        end

        // ---- ch5: $2100 forced blank over the letterbox ----
        ST_CH5: if (slot_free) begin
          v_r <= 1'b1;
          a_r <= OFF_CH5 + {7'd0, cnt};
          d_r <= (cnt == TAB2_LAST) ? 8'h00 : (cnt[0] ? ch5_val : 8'h01);
          if (cnt == TAB2_LAST) begin
            sp_row <= 5'd0; dirty_pub <= 32'd0;
            st <= ST_SPANRD;
          end else cnt <= cnt + 9'd1;
        end

        // ---- span table: publish the union and fold it into the carry ----
        // This runs BEFORE the control block on purpose: the dirty bitmap is
        // built here, row by row, as "the published span is not empty", so the
        // two fields are consistent by construction.
        ST_SPANRD:  st <= ST_SPANRD2;
        ST_SPANRD2: st <= ST_SPANB0;

        ST_SPANB0: if (slot_free) begin
          v_r <= 1'b1;
          a_r <= OFF_SPAN + {10'd0, sp_row, 1'b0};
          d_r <= u_first;
          dirty_pub[sp_row] <= u_ne;
          st <= ST_SPANB1;
        end

        ST_SPANB1: if (slot_free) begin
          v_r <= 1'b1;
          a_r <= OFF_SPAN + {10'd0, sp_row, 1'b0} + 16'd1;
          d_r <= u_last;
          if (sp_row == 5'd31) begin        // SPAN_ROWS - 1
            carry_void <= 1'b0;             // every row has been rewritten
            cnt <= 9'd0;
            st  <= ST_CTRL;
          end else begin
            sp_row <= sp_row + 5'd1;
            st     <= ST_SPANRD;
          end
        end

        // ---- control block, then the frame is announced ----
        ST_CTRL: if (slot_free) begin
          v_r <= 1'b1;
          a_r <= OFF_CTRL + {7'd0, cnt};
          d_r <= ctrl_byte;
          if (cnt == 9'd15) st <= ST_DONE;
          else cnt <= cnt + 9'd1;
        end

        // The frame is only announced once the LAST byte has been taken by the
        // arbiter: the control block has to be complete in PSRAM before the
        // player is told the set is ready, or it can read a stale
        // contract_ver / dirty bitmap.
        ST_DONE: if (~v_r & ~BUF_WRQ) begin
          FRAME_DONE  <= ~TR_GATE;
          frame_ctr   <= frame_ctr + 8'd1;
          first_frame <= 1'b0;
          cline       <= 8'd0;
          frame_lines <= 9'd0;
          capturing   <= 1'b1;
          st          <= ST_IDLE;
        end

        default: st <= ST_IDLE;
      endcase

      // Frame close request wins over the clear above, so an edge that lands
      // on the same cycle as the hand-off is not lost.
      if (VSYNC_EDGE | wd_trip) frame_end_req <= 1'b1;
    end
  end

endmodule
