// sd2snes SMS core — SMS->SNES translation engine (M7.3).
//
// RTL port of the validated C `translate_all` (ciclone sms_model.cpp). Streams the
// SMS VDP state into SNES-native buffers that the SNES-side player DMAs:
//   - CRAM (6-bit BGR) -> CGRAM (15-bit BGR555): BG subpals 0/1 + OBJ pal 0 + backdrop
//   - pattern table     -> SNES 4bpp tiles (byte rearrange; bit7=leftmost both sides)
//   - name table        -> SNES BG tilemap (bit relocation; 24 rows centered, VOFF=2)
// Sprites (SAT->OAM) are the next sub-step (M7.3b). The engine is a sequential FSM:
// it reads VRAM via a registered 1-cycle port and CRAM combinationally, and emits one
// output byte per cycle on out_* (out_sel picks the tiles/tilemap/cgram window). In
// the FPGA the output goes to the PSRAM buffers via the arbiter (M7.4); the Verilator
// test compares it against the same formulas computed in C.
//
// TODO_SMS_COMPAT: sprites, vertical-scroll wrap, scroll-lock; matches the C model.
module sms_translate (
  input             CLK,
  input             RST,
  input             START,        // pulse to run one full translation pass
  input             STALL,        // hold the FSM (PSRAM write backpressure)
  output reg        BUSY,
  output reg        DONE,

  // SMS VDP registers needed for translation
  input      [7:0]  REG0,         // bit3 = shift sprites left 8px
  input      [7:0]  REG1,         // bit1 = 8x16 sprites
  input      [7:0]  REG2,         // name table base (bits 1-3)
  input      [7:0]  REG5,         // sprite attribute table base (bits 1-6)
  input      [7:0]  REG6,         // bit2 = sprite pattern +256
  input      [7:0]  REG7,         // backdrop color (bits 0-3 -> sprite palette)
  input      [7:0]  REG8,         // horizontal scroll
  input      [7:0]  REG9,         // vertical scroll
  input      [7:0]  DIRTY_LO_MIN, // changed-tile range, LOW half (tiles 0..255 = BG)
  input      [7:0]  DIRTY_LO_MAX,
  input      [7:0]  DIRTY_HI_MIN, // changed-tile range, HIGH half (tiles 256..511 = sprites)
  input      [7:0]  DIRTY_HI_MAX,
  input      [31:0] DIRTY_COLS,   // dirty name-table columns (re-emit only these; clean stay)

  // VRAM read port (registered, 1-cycle latency)
  output reg [13:0] VRAM_ADDR,
  input      [7:0]  VRAM_DATA,
  // CRAM read (combinational, 32 bytes)
  output reg [4:0]  CRAM_ADDR,
  input      [7:0]  CRAM_DATA,

  // output stream to the SNES-format buffers
  output reg        OUT_WE,
  output reg [1:0]  OUT_SEL,      // 0=tiles($E0) 1=tilemap($E1) 2=cgram($E2) 3=oam($E3)
  output reg [15:0] OUT_ADDR,
  output reg [7:0]  OUT_DATA
);

  localparam BLANK_TILE = 10'd511;   // reserved transparent tile (matches C model)

  // 2-bit -> 5-bit channel expand (matches C: {0,10,21,31})
  function [4:0] e2(input [1:0] v);
    case (v) 2'd0: e2 = 5'd0; 2'd1: e2 = 5'd10; 2'd2: e2 = 5'd21; default: e2 = 5'd31; endcase
  endfunction
  function [15:0] bgr555(input [7:0] c);   // SMS 00BBGGRR -> SNES 0BBBBBGGGGGRRRRR
    bgr555 = {1'b0, e2(c[5:4]), e2(c[3:2]), e2(c[1:0])};
  endfunction

  // phases
  localparam S_IDLE=0, S_PAL=1, S_BACK=2, S_TILES_A=3, S_TILES_D=4, S_TZERO=5,
             S_NT_A=6, S_NT_D=7, S_OAM_INIT=8, S_OAM_SCAN=9, S_OAM_EMIT=10,
             S_OAM_HITAB=11, S_DONE=12, S_SCROLL=13;
  reg [3:0]  st;
  // VDP register SNAPSHOT, latched at START: the SMS now RUNS during the pass
  // (no CE freeze), so geometry/scroll regs must be pass-consistent even if the
  // game rewrites them mid-pass (the change lands in the next pass).
  reg [7:0]  r0, r1, r2, r5, r6, r7, r8, r9;
  reg [5:0]  pi;       // palette color index (0..47)
  reg        ph;       // palette byte half (0=lo,1=hi)
  reg [1:0]  bh;       // backdrop byte half
  reg [3:0]  sc;       // scroll/dirty/columns control-block byte counter (0..11)
  wire [9:0] hofs = 10'd0 - {2'd0, r8};     // SNES BG1 H-scroll = (0-reg8)&0x3FF
  wire [9:0] vofs = {7'd0, r9[2:0]} - 10'd1;     // BG1VOFS = fine V-scroll(reg9[2:0]) - 1; coarse is in nt_src
  reg [14:0] ti;       // tile src offset (0..16383)
  reg        tile_hi;  // 0 = uploading the LOW dirty range, 1 = the HIGH range
  reg [4:0]  tz;       // tile-511 zero counter
  reg [10:0] nci;      // name-table cell index (0..1023)
  reg        nbyte;    // which name-table source byte (0=lo,1=hi)
  reg [7:0]  nt_lo;    // captured low byte of the SMS name-table entry
  reg [1:0]  nstep;    // name-table per-cell step
  // sprites (SAT -> OAM)
  reg [9:0]  oi;       // OAM init byte counter (0..511)
  reg [6:0]  si;       // SMS sprite index (0..63)
  reg [7:0]  so;       // SNES OAM sprite index (0..127)
  reg [2:0]  sstep;    // SAT read sub-step
  reg [3:0]  ec;       // emit byte counter
  reg [4:0]  hidx;     // high-table write counter (0..31)
  reg [8:0]  ex_r, ey0_r, ey1_r;
  reg [9:0]  et0_r, et1_r;
  reg        tall_r;
  reg [7:0]  hi_tab [0:31];   // OAM high-table accumulator
  integer    ki;

  wire [13:0] nt_base  = {r2[3:1], 11'd0};     // (REG2 & 0x0E) << 10 = REG2[3:1] << 11
  wire [13:0] sat_base = {r5[6:1], 8'd0};      // (REG5 & 0x7E) << 7
  wire [13:0] sat_xt   = sat_base + 14'h80 + {si, 1'b0};  // X/tile pair base for sprite si
  wire [8:0]  so_cur   = so + {8'd0, ec[2]};              // target SNES sprite (top/bottom)
  // cgram dst index for palette color pi: 0..31 -> 0..31, 32..47 -> 128..143
  wire [8:0] cg_idx = (pi < 6'd32) ? {3'd0, pi} : (9'd128 + {4'd0, (pi - 6'd32)});
  wire [15:0] pal_col = bgr555(CRAM_DATA);

  // CRAM address is combinational (CRAM read is 0-cycle): tracks pi, or backdrop in S_BACK.
  always @* begin
    if (st == S_BACK) CRAM_ADDR = {1'b1, r7[3:0]};
    else              CRAM_ADDR = (pi < 6'd32) ? pi[4:0] : (5'd16 + pi[3:0]);
  end

  // VRAM_ADDR is combinational: the address is PRESENT in the state before the read,
  // so a 1-cycle registered VRAM read (BRAM) delivers the byte in the read state.
  // tiles: ti (S_TILES_A latch -> S_TILES_D read); NT: nt_src (S_NT_A) then nt_src+1
  // (S_NT_D); OAM: Y/X/tile addr per sstep.
  always @* begin
    case (st)
      S_TILES_A, S_TILES_D: VRAM_ADDR = ti[13:0];
      S_NT_A:               VRAM_ADDR = nt_src;
      S_NT_D:               VRAM_ADDR = nt_src + 14'd1;
      S_OAM_SCAN:           VRAM_ADDR = (sstep == 3'd0) ? (sat_base + {7'd0, si})
                                      : (sstep == 3'd1) ? sat_xt
                                      : (sat_xt + 14'd1);
      default:              VRAM_ADDR = ti[13:0];
    endcase
  end

  // tile dst within the 512-tile buffer: n*32 + (k<2 ? r*2+k : 16+r*2+(k-2))
  wire [8:0]  t_n = ti[14:5];          // tile number (0..511) -> 9 bits used
  wire [2:0]  t_r = ti[4:2];           // row 0..7
  wire [1:0]  t_k = ti[1:0];           // plane 0..3
  wire [4:0]  t_off = t_k[1] ? (5'd16 + {t_r,1'b0} + {4'd0,t_k[0]}) : ({t_r,1'b0} + {4'd0,t_k[0]});
  wire [15:0] t_dst = {t_n[8:0], 5'd0} + {11'd0, t_off};
  wire        lo_ne = (DIRTY_LO_MAX >= DIRTY_LO_MIN);   // low dirty range non-empty
  wire        hi_ne = (DIRTY_HI_MAX >= DIRTY_HI_MIN);   // high dirty range non-empty

  // name-table cell -> snes tilemap row/col; content = SNES rows 2..26 (VOFF=2). 24
  // rows cover the static window; the 25th (row 26 = source coarse+24) is needed because
  // a fine V-scroll (vofs=reg9[2:0]-1) shifts the sampling window down by reg9[2:0]px, so
  // the bottom 1..7px reveal the next source row — without it that strip shows blank tile 511.
  // nci iterates COLUMN-major (nci = col*32 + row) so the buffer offset {nci,1'b0} = nci*2 =
  // col*64 + row*2 is column-major: each column is 64 contiguous bytes the player DMAs with a
  // vertical (+32-word) increment. Row is the low 5 bits, column the high 5.
  wire [4:0] sr  = nci[4:0];           // snes row 0..31
  wire [4:0] col = nci[9:5];           // snes column 0..31
  wire       content = (sr >= 5'd2) && (sr <= 5'd26);
  // V-scroll: the visible SMS rows are [reg9_row .. reg9_row+23] wrapping at 28, not
  // a fixed 0..23. coarse = reg9[7:3]; the source row = (coarse + (sr-2)) mod 28.
  wire [4:0]  reg9_row = r9[7:3];
  wire [5:0]  win_sum  = {1'b0, reg9_row} + {2'b0, (sr - 5'd2)};
  wire [5:0]  win_mod  = (win_sum >= 6'd28) ? (win_sum - 6'd28) : win_sum;
  wire [13:0] nt_src   = nt_base + (({win_mod[4:0], col}) << 1);

  always @(posedge CLK) begin
    if (RST) begin
      st <= S_IDLE; BUSY <= 0; DONE <= 0; OUT_WE <= 0;
      pi <= 0; ph <= 0; bh <= 0; sc <= 0; ti <= 0; tile_hi <= 0; tz <= 0; nci <= 0; nbyte <= 0; nstep <= 0;
      OUT_SEL <= 0; OUT_ADDR <= 0; OUT_DATA <= 0;
    end else if (!STALL) begin   // STALL freezes the FSM + holds OUT_* (PSRAM write backpressure)
      OUT_WE <= 0; DONE <= 0;
      case (st)
        S_IDLE: if (START) begin
                  BUSY <= 1; pi <= 0; ph <= 0; st <= S_PAL;
                  r0 <= REG0; r1 <= REG1; r2 <= REG2; r5 <= REG5;   // pass-consistent
                  r6 <= REG6; r7 <= REG7; r8 <= REG8; r9 <= REG9;   // reg snapshot
                  for (ki = 0; ki < 32; ki = ki + 1) hi_tab[ki] <= 8'd0;
                end

        // ---- palette: 32 BG (cgram 0..31) + 16 OBJ (cgram 128..143) ----
        S_PAL: begin
          OUT_SEL <= 2'd2; OUT_WE <= 1'b1;
          OUT_ADDR <= {cg_idx, 1'b0} + {15'd0, ph};
          OUT_DATA <= ph ? pal_col[15:8] : pal_col[7:0];
          if (ph) begin
            ph <= 1'b0;
            if (pi == 6'd47) begin bh <= 0; st <= S_BACK; end
            else pi <= pi + 1'b1;
          end else ph <= 1'b1;
        end

        // ---- backdrop: CGRAM[0] = conv(CRAM[16 + reg7&0xF]) ----
        S_BACK: begin
          OUT_SEL <= 2'd2; OUT_WE <= 1'b1;
          OUT_ADDR <= {15'd0, bh[0]};
          OUT_DATA <= bh[0] ? pal_col[15:8] : pal_col[7:0];
          if (bh[0]) begin ti <= 0; sc <= 0; st <= S_SCROLL; end
          bh <= bh + 1'b1;
        end

        // ---- scroll: emit hofs/vofs to cgram region offset 0x200 (player reads $E2:0200) ----
        S_SCROLL: begin
          OUT_SEL <= 2'd2; OUT_WE <= 1'b1;
          OUT_ADDR <= 16'h0200 + {12'd0, sc};
          case (sc)
            4'd0: OUT_DATA <= hofs[7:0];
            4'd1: OUT_DATA <= {6'd0, hofs[9:8]};
            4'd2: OUT_DATA <= vofs[7:0];
            4'd3: OUT_DATA <= {6'd0, vofs[9:8]};
            4'd4: OUT_DATA <= DIRTY_LO_MIN;     // $E2:0204 low-half min  (tile 0..255)
            4'd5: OUT_DATA <= DIRTY_LO_MAX;     // $E2:0205 low-half max
            4'd6: OUT_DATA <= DIRTY_HI_MIN;     // $E2:0206 high-half min (absolute = 256 + idx)
            4'd7: OUT_DATA <= DIRTY_HI_MAX;     // $E2:0207 high-half max
            4'd8: OUT_DATA <= DIRTY_COLS[7:0];   // $E2:0208 dirty name-table columns 0..7
            4'd9: OUT_DATA <= DIRTY_COLS[15:8];  // $E2:0209 columns 8..15
            4'd10: OUT_DATA <= DIRTY_COLS[23:16];// $E2:020A columns 16..23
            default: OUT_DATA <= DIRTY_COLS[31:24]; // $E2:020B columns 24..31
          endcase
          if (sc == 4'd11) begin                 // start the low range, then the high range
            if (lo_ne)      begin ti <= {2'b00, DIRTY_LO_MIN, 5'd0}; tile_hi <= 1'b0; st <= S_TILES_A; end
            else if (hi_ne) begin ti <= {2'b01, DIRTY_HI_MIN, 5'd0}; tile_hi <= 1'b1; st <= S_TILES_A; end
            else            begin tz <= 0; st <= S_TZERO; end   // nothing dirty -> skip tile emit
          end else sc <= sc + 1'b1;
        end

        // ---- tiles: stream the two dirty spans (vram -> rearranged 4bpp) ----
        S_TILES_A: begin st <= S_TILES_D; end
        S_TILES_D: begin
          OUT_SEL <= 2'd0; OUT_WE <= 1'b1; OUT_ADDR <= t_dst; OUT_DATA <= VRAM_DATA;
          if (ti == (tile_hi ? {2'b01, DIRTY_HI_MAX, 5'd31} : {2'b00, DIRTY_LO_MAX, 5'd31})) begin
            if (~tile_hi & hi_ne) begin ti <= {2'b01, DIRTY_HI_MIN, 5'd0}; tile_hi <= 1'b1; st <= S_TILES_A; end
            else begin tz <= 0; st <= S_TZERO; end
          end else begin ti <= ti + 1'b1; st <= S_TILES_A; end
        end
        // zero the reserved transparent tile 511
        S_TZERO: begin
          OUT_SEL <= 2'd0; OUT_WE <= 1'b1; OUT_ADDR <= {BLANK_TILE[8:0], 5'd0} + {11'd0, tz}; OUT_DATA <= 8'd0;
          if (tz == 5'd31) begin nci <= 0; nstep <= 0; st <= S_NT_A; end else tz <= tz + 1'b1;
        end

        // ---- name table: 32x32 tilemap; content rows 2..26 from SMS rows 0..24 ----
        // skip clean columns (DIRTY_COLS): their cells keep their last-written value in the
        // double-buffer set, so the per-frame tilemap write collapses to the few changed cols.
        S_NT_A: begin
          if (nci[4:0] == 5'd0 && ~DIRTY_COLS[nci[9:5]]) begin   // clean column -> skip all 32 rows
            if (nci[9:5] == 5'd31) begin oi <= 0; st <= S_OAM_INIT; end else nci <= nci + 11'd32;
          end else if (content) begin nbyte <= 0; nstep <= 0; st <= S_NT_D; end
          else begin
            // blank cell -> tile 511, pal0
            OUT_SEL <= 2'd1; OUT_WE <= 1'b1;
            OUT_ADDR <= {nci, 1'b0} + {15'd0, nstep[0]};
            OUT_DATA <= nstep[0] ? {6'd0, BLANK_TILE[9:8]} : BLANK_TILE[7:0];
            if (nstep[0]) begin
              if (nci == 11'd1023) begin oi <= 0; st <= S_OAM_INIT; end else nci <= nci + 1'b1;
            end
            nstep <= nstep + 1'b1;
          end
        end
        S_NT_D: begin
          // read 2 SMS bytes (lo,hi) then emit 2 SNES bytes
          case (nstep)
            2'd0: begin nt_lo <= VRAM_DATA; nstep <= 2'd1; end
            2'd1: begin
              // build SNES entry from SMS {hi=VRAM_DATA, lo=nt_lo}
              // SMS: tile= {hi[0],lo} ; h=hi[1] v=hi[2] pal=hi[3] prio=hi[4]
              // SNES: tile[9:0] | pal<<10 | prio<<13 | h<<14 | v<<15
              OUT_SEL <= 2'd1; OUT_WE <= 1'b1; OUT_ADDR <= {nci, 1'b0};
              OUT_DATA <= nt_lo;                       // low byte = tile[7:0]
              nstep <= 2'd2;
            end
            default: begin
              OUT_SEL <= 2'd1; OUT_WE <= 1'b1; OUT_ADDR <= {nci, 1'b0} + 16'd1;
              // SNES entry[15:8] = {v,h,prio, 0,0, pal, 0, tile8}; SMS hi: tile8=0,h=1,v=2,pal=3,prio=4
              OUT_DATA <= {VRAM_DATA[2], VRAM_DATA[1], VRAM_DATA[4], 2'b00, VRAM_DATA[3], 1'b0, VRAM_DATA[0]};
              if (nci == 11'd1023) begin oi <= 0; st <= S_OAM_INIT; end else begin nci <= nci + 1'b1; st <= S_NT_A; end
            end
          endcase
        end

        // ---- OAM init: 128 sprites off-screen (Y=$F0), low table only ----
        S_OAM_INIT: begin
          OUT_SEL <= 2'd3; OUT_WE <= 1'b1; OUT_ADDR <= {6'd0, oi};
          OUT_DATA <= (oi[1:0] == 2'd1) ? 8'hF0 : 8'd0;   // byte1 of each sprite = Y off-screen
          if (oi == 10'd511) begin si <= 0; so <= 0; sstep <= 0; st <= S_OAM_SCAN; end
          else oi <= oi + 1'b1;
        end

        // ---- SAT scan: read Y (terminator $D0), X, tile; compute; emit ----
        S_OAM_SCAN: begin
          case (sstep)
            3'd0: begin sstep <= 3'd1; end
            3'd1: begin
              if (VRAM_DATA == 8'hD0 || si == 7'd64 || so >= 8'd128) begin hidx <= 0; st <= S_OAM_HITAB; end
              else begin ey0_r <= VRAM_DATA + 9'd17; ey1_r <= VRAM_DATA + 9'd17 + 9'd8;
                         sstep <= 3'd2; end
            end
            3'd2: begin ex_r <= {1'b0, VRAM_DATA} - (r0[3] ? 9'd8 : 9'd0); sstep <= 3'd3; end
            default: begin
              // tile = (REG6[2]?256:0) + VRAM_DATA
              tall_r <= r1[1];
              if (r1[1]) begin
                et0_r <= ({1'b0, r6[2], VRAM_DATA}) & 10'h1FE;
                et1_r <= (({1'b0, r6[2], VRAM_DATA}) & 10'h1FE) | 10'd1;
              end else begin
                et0_r <= {1'b0, r6[2], VRAM_DATA};
              end
              ec <= 0; st <= S_OAM_EMIT;
            end
          endcase
        end

        // ---- emit OAM bytes: 4 per sprite (8 if 8x16 = two stacked) ----
        S_OAM_EMIT: begin
          // which half (tall bottom), target sprite, byte index
          // ec: 0..3 top sprite, 4..7 bottom (only if tall)
          if (so_cur < 9'd128) begin
            OUT_SEL <= 2'd3; OUT_WE <= 1'b1;
            OUT_ADDR <= {so_cur[7:0], 2'd0} + {14'd0, ec[1:0]};
            case (ec[1:0])
              2'd0: OUT_DATA <= ex_r[7:0];
              2'd1: OUT_DATA <= (ec[2] ? ey1_r[7:0] : ey0_r[7:0]);
              2'd2: OUT_DATA <= (ec[2] ? et1_r[7:0] : et0_r[7:0]);
              default: OUT_DATA <= 8'h20 | {7'd0, (ec[2] ? et1_r[8] : et0_r[8])};  // prio2,pal0,tile8
            endcase
            if (ec[1:0] == 2'd3 && ex_r[8])
              hi_tab[so_cur[6:2]] <= hi_tab[so_cur[6:2]] | (8'd1 << {so_cur[1:0], 1'b0});
          end
          // advance
          if (ec == (tall_r ? 4'd7 : 4'd3)) begin
            si <= si + 1'b1;
            so <= so + (tall_r ? 8'd2 : 8'd1);
            sstep <= 0; st <= S_OAM_SCAN;
          end else ec <= ec + 1'b1;
        end

        // ---- write the OAM high table (X bit8 / size) ----
        S_OAM_HITAB: begin
          OUT_SEL <= 2'd3; OUT_WE <= 1'b1; OUT_ADDR <= 16'd512 + {11'd0, hidx};
          OUT_DATA <= hi_tab[hidx];
          if (hidx == 5'd31) st <= S_DONE; else hidx <= hidx + 1'b1;
        end

        S_DONE: begin BUSY <= 0; DONE <= 1; st <= S_IDLE; end
        default: st <= S_IDLE;
      endcase
    end
  end
endmodule
