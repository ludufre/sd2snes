// a26 -- the Atari 2600 console: 6507 + address decode + RIOT (128B RAM, timer,
// I/O ports) + cartridge (bank switching, optional Superchip RAM) + the TIA and
// its audio.  Everything the games see; nothing about the SNES side, which is
// the wrapper's job (a26_core.v).
//
// CLOCKING / BUS CONVENTION -- read this before touching anything here.
//
//   CLK is 96MHz.  TIA_CE marks a color clock (3.579545MHz, spacing dithers
//   26/27 CLK) and CPU_CE is every third TIA_CE, so a CPU cycle is at least 78
//   CLK long and always starts ON a color clock.
//
//   The Arlet core has no clock-enable port: RDY IS the enable, one CPU cycle
//   per enabled edge.  With that arrangement the address bus is driven by
//   registers for the whole (long) cycle and every peripheral has ~26 color
//   clocks to answer, so all read data is REGISTERED IN CLK and muxed after
//   (the block RAM rule, and what the core samples at the enabled edge).
//   The core's own DIHOLD is the memory data register that upstream expects
//   the outside world to provide -- see arlet/README.txt, local change 2.
//
//   Bus strobes are the LEVEL of WE/~WE qualified by the enable, never an edge:
//   WE stays high for two or three consecutive cycles in JSR/BRK, and an edge
//   detector would drop the second and third push and corrupt the stack.  RES
//   masks the writes the core performs while running its reset through the BRK
//   microcode (three pushes with an address that is whatever S powers up as).
//
//   WSYNC and the boot copy hold the CPU by clearing RDY.  The RIOT timer keeps
//   counting through both -- on the real machine phi2 never stops, only the
//   processor's RDY line moves.
`timescale 1ns / 1ps

module a26 (
  input             CLK,
  input             RST,
  input             TIA_CE,           // color clock enable
  input             CPU_CE,           // CPU cycle enable (TIA_CE / 3)

  // CHIPFEAT 0xef (contract 7): [3:0] scheme, [4] superchip, [5] width,
  // [6] tv, [11:8] size_class.  Latched by the MCU before reset, never while
  // the core is running, so it is read as a constant here.
  input      [15:0] FEAT,

  // .a26 image fetch: PSRAM read client, serviced by the main.v arbiter.
  // Used once, during the boot copy into the cartridge block RAM.  ROM_ADDR is
  // an OFFSET into the staged image; main.v adds the region base (0x300000).
  output reg        ROM_RRQ,
  output reg [23:0] ROM_ADDR,
  input      [7:0]  ROM_DATA,
  input             ROM_RDY,
  output            BOOT_ACTIVE,      // 1 while copying: CPU held in reset

  // controls, already mapped by main.v from the $EF player window (contract 6)
  input      [7:0]  SWCHA_IN,         // joystick directions, active low
  input      [7:0]  SWCHB_IN,         // console switch shadow
  input      [1:0]  INPT45,           // {INPT5, INPT4} fire buttons, active low

  // video, re-exported from the TIA
  output            VID_VISIBLE,
  output     [1:0]  VID_CODE,
  output            LINE_START,
  output            LINE_VIS_START,
  output     [6:0]  COLUBK,
  output     [6:0]  COLUPF,
  output     [6:0]  COLUP0,
  output     [6:0]  COLUP1,
  output     [7:0]  CTRLPF,
  output            VSYNC_EDGE,
  output            VBLANK_ON,

  // audio, re-exported from the mixer (unipolar 0..510, ~31.4kHz strobe)
  output     [8:0]  AU_MIX,
  output            AU_TICK,

  // debug / instrumentation
  output     [12:0] DBG_AB,
  output     [2:0]  DBG_BANK,
  output            DBG_WSYNC
);

  // ---------------------------------------------------------------------
  // CHIPFEAT
  // ---------------------------------------------------------------------
  localparam [3:0] SCH_2K = 4'd0,
                   SCH_4K = 4'd1,
                   SCH_F8 = 4'd2,
                   SCH_F6 = 4'd3,
                   SCH_F4 = 4'd4;

  // Power-on bank, per scheme.  The real cartridges come up on whichever bank
  // the flip-flop happens to latch, and emulators disagree; the vectors live in
  // the last bank, so that is the safe pick until a title says otherwise
  // (per-title overrides are a CRC32 database job, not RTL).
  localparam [2:0] BANK_INIT_F8 = 3'd1,   // 2 banks
                   BANK_INIT_F6 = 3'd3,   // 4 banks
                   BANK_INIT_F4 = 3'd7;   // 8 banks

  wire [3:0] scheme     = FEAT[3:0];
  wire       superchip  = FEAT[4];
  wire [3:0] size_class = FEAT[11:8];

  // ---------------------------------------------------------------------
  // CPU
  // ---------------------------------------------------------------------
  wire [15:0] cpu_ab;
  wire [7:0]  cpu_do;
  wire        cpu_we, cpu_res;
  reg  [7:0]  cpu_di;

  wire        wsync_halt;
  wire        boot_active;
  wire        cpu_rdy   = CPU_CE & ~wsync_halt & ~boot_active;
  wire        cpu_reset = RST | boot_active;

  // the 6507 only bonds out 13 address pins; A13-A15 do not exist
  wire [12:0] A = cpu_ab[12:0];

  cpu arlet_cpu (
    .clk(CLK), .reset(cpu_reset),
    .AB(cpu_ab), .DI(cpu_di), .DO(cpu_do), .WE(cpu_we),
    .IRQ(1'b0), .NMI(1'b0),                 // the 6507 has neither pin
    .RDY(cpu_rdy), .RES(cpu_res)
  );

  wire wr_strobe = cpu_rdy & cpu_we & ~cpu_res;
  wire rd_strobe = cpu_rdy & ~cpu_we;

  // ---------------------------------------------------------------------
  // Address decode (6507, A12..A0)
  // ---------------------------------------------------------------------
  wire tia_cs  = ~A[12] & ~A[7];              // $00-$7F and mirrors
  wire ram_cs  = ~A[12] & ~A[9] &  A[7];      // RIOT RAM $80-$FF and mirrors
  wire riot_cs = ~A[12] &  A[9] &  A[7];      // RIOT registers $280-$29F "
  wire rom_cs  =  A[12];                      // cartridge $1000-$1FFF

  // ---------------------------------------------------------------------
  // RIOT RAM, 128 bytes.  Dedicated synchronous read, mux afterwards.
  // ---------------------------------------------------------------------
  reg [7:0] riot_ram [0:127];
  reg [7:0] riot_ram_q;
  always @(posedge CLK) begin
    if (ram_cs & wr_strobe) riot_ram[A[6:0]] <= cpu_do;
    riot_ram_q <= riot_ram[A[6:0]];
  end

  // ---------------------------------------------------------------------
  // RIOT registers.
  //   read  $280 SWCHA  $281 SWACNT  $282 SWCHB  $283 SWBCNT
  //         $284 INTIM  $285 INSTAT
  //   write $280-$283 as above, $294-$297 TIM1T/TIM8T/TIM64T/T1024T
  // A3 (the interrupt-enable half of the write decode) is ignored: the 6507
  // has no interrupt pin, so both halves mean the same thing here.
  // ---------------------------------------------------------------------
  wire riot_wr    = riot_cs & wr_strobe;
  wire riot_rd    = riot_cs & rd_strobe;
  wire timer_wr   = riot_wr & A[4] & A[2];    // $294-$297
  wire io_wr      = riot_wr & ~A[4];          // $280-$283
  wire rd_intim   = riot_rd & A[2] & ~A[0];   // $284: clears the timer flag
  // reading $285 (INSTAT) clears the PA7 edge flag; v0 has no edge detect, so
  // that bit is hardwired 0 and the read has nothing to clear yet

  reg [7:0] swcha_out, swchb_out;
  reg [7:0] swacnt, swbcnt;
  reg [7:0] intim;
  reg [9:0] tim_presc;
  reg [1:0] tim_div;
  reg       tim_expired;                      // past underflow: 1 count/cycle
  reg       tim_flag;                         // INSTAT bit 7

  // interval per divider setting; after an underflow the timer free-runs at
  // one count per cycle until the next write, which is what the flag is for
  wire [9:0] tim_max = tim_expired ? 10'd0
                     : (tim_div == 2'd0) ? 10'd0
                     : (tim_div == 2'd1) ? 10'd7
                     : (tim_div == 2'd2) ? 10'd63 : 10'd1023;

  always @(posedge CLK) begin
    if (RST) begin
      swcha_out <= 8'hFF; swchb_out <= 8'hFF;
      swacnt <= 8'h00; swbcnt <= 8'h00;
      intim <= 8'h00; tim_presc <= 10'd0; tim_div <= 2'd3;
      tim_expired <= 1'b0; tim_flag <= 1'b0;
    end else begin
      if (io_wr) begin
        case (A[1:0])
          2'd0: swcha_out <= cpu_do;
          2'd1: swacnt    <= cpu_do;
          2'd2: swchb_out <= cpu_do;
          2'd3: swbcnt    <= cpu_do;
        endcase
      end
      if (timer_wr) begin
        intim       <= cpu_do;
        tim_div     <= A[1:0];
        tim_presc   <= 10'd0;
        tim_expired <= 1'b0;
        tim_flag    <= 1'b0;
      end else if (CPU_CE) begin
        // phi2 keeps running while the CPU is held by WSYNC or by the boot
        // copy, so this counts CPU_CE and not the CPU's advance
        if (tim_presc == tim_max) begin
          tim_presc <= 10'd0;
          intim     <= intim - 8'd1;
          if (intim == 8'd0) begin
            tim_expired <= 1'b1;
            tim_flag    <= 1'b1;
          end
        end else begin
          tim_presc <= tim_presc + 10'd1;
        end
      end
      if (rd_intim) tim_flag <= 1'b0;
    end
  end

  // input pins win on the bits the DDR marks as inputs
  wire [7:0] swcha_rd = (swcha_out & swacnt) | (SWCHA_IN & ~swacnt);
  wire [7:0] swchb_rd = (swchb_out & swbcnt) | (SWCHB_IN & ~swbcnt);
  // INSTAT: bit 7 is the timer underflow flag, bit 6 the PA7 edge flag (not
  // implemented in v0), the rest reads as 0
  wire [7:0] instat_rd = {tim_flag, 1'b0, 6'b000000};

  wire [7:0] riot_rd_mux = A[2] ? (A[0] ? instat_rd : intim)
                                : (A[1] ? (A[0] ? swbcnt : swchb_rd)
                                        : (A[0] ? swacnt : swcha_rd));
  reg [7:0] riot_q;
  always @(posedge CLK) riot_q <= riot_rd_mux;

  // ---------------------------------------------------------------------
  // Cartridge: bank register + hotspots.
  //
  // A hotspot is decoded from the ADDRESS alone, on reads as well as writes --
  // the cartridge logic only sees the address bus, never R/W or the data.  It
  // is sampled once per CPU access (cpu_rdy), so one access is one switch, and
  // it takes effect at the end of the access: the byte that access returns
  // still comes from the old bank.
  // ---------------------------------------------------------------------
  reg [2:0] bank;
  wire      hs_win = (A[12:4] == 9'h1FF);     // $1FF0-$1FFF
  wire [3:0] hs_lo = A[3:0];
  wire hs_f8 = hs_win & (hs_lo[3:1] == 3'b100);                   // $1FF8-$1FF9
  wire hs_f6 = hs_win & (hs_lo >= 4'd6) & (hs_lo <= 4'd9);        // $1FF6-$1FF9
  wire hs_f4 = hs_win & (hs_lo >= 4'd4) & (hs_lo <= 4'd11);       // $1FF4-$1FFB

  always @(posedge CLK) begin
    if (RST) begin
      case (scheme)
        SCH_F8:  bank <= BANK_INIT_F8;
        SCH_F6:  bank <= BANK_INIT_F6;
        SCH_F4:  bank <= BANK_INIT_F4;
        default: bank <= 3'd0;
      endcase
    end else if (cpu_rdy) begin
      case (scheme)
        SCH_F8: if (hs_f8) bank <= {2'b00, hs_lo[0]};
        SCH_F6: if (hs_f6) bank <= {1'b0, hs_lo[1:0] + 2'd2};     // $1FF6 -> 0
        SCH_F4: if (hs_f4) bank <= hs_lo[2:0] + 3'd4;             // $1FF4 -> 0
        default: ;
      endcase
    end
  end

  // 2K images ignore A11 (the classic half-size mirror); everything else maps
  // the 4K window straight into the selected bank
  wire [2:0]  bank_eff = (scheme == SCH_F8) ? {2'b00, bank[0]}
                       : (scheme == SCH_F6) ? {1'b0, bank[1:0]}
                       : (scheme == SCH_F4) ? bank : 3'd0;
  wire [11:0] rom_off  = (scheme == SCH_2K) ? {1'b0, A[10:0]} : A[11:0];

  // ---------------------------------------------------------------------
  // Superchip: 128 bytes of RAM in front of the cartridge window,
  // write port $1000-$107F, read port $1080-$10FF, both ahead of ROM.
  // ---------------------------------------------------------------------
  wire sc_wr_win = superchip & rom_cs & (A[11:7] == 5'b00000);
  wire sc_rd_win = superchip & rom_cs & (A[11:7] == 5'b00001);

  reg [7:0] sc_ram [0:127];
  reg [7:0] sc_q;
  always @(posedge CLK) begin
    if (sc_wr_win & wr_strobe) sc_ram[A[6:0]] <= cpu_do;
    sc_q <= sc_ram[A[6:0]];
  end

  // ---------------------------------------------------------------------
  // Cartridge ROM, 32KB of block RAM, filled once by the boot copy.
  // Write port = the copy, read port = the CPU: a plain simple-dual-port.
  // ---------------------------------------------------------------------
  reg [7:0] cart_rom [0:32767];
  reg [7:0] rom_q;
  reg [14:0] boot_addr;
  wire       boot_we;
  always @(posedge CLK) begin
    if (boot_we) cart_rom[boot_addr] <= ROM_DATA;
    rom_q <= cart_rom[{bank_eff, rom_off}];
  end

  // ---------------------------------------------------------------------
  // Boot copy: PSRAM 0x300000.. -> cartridge block RAM, one byte per request.
  // The CPU is held in reset for the duration, so it comes out of reset with
  // the image already in place and reads its vectors from it.
  // ---------------------------------------------------------------------
  localparam BOOT_REQ  = 2'd0,
             BOOT_WAIT = 2'd1,
             BOOT_DONE = 2'd2;

  reg [1:0]  boot_state;
  reg [15:0] boot_last;                       // last byte index, from size_class

  always @* begin
    case (size_class)
      4'd0:    boot_last = 16'd2047;          // 2K
      4'd1:    boot_last = 16'd4095;          // 4K
      4'd2:    boot_last = 16'd8191;          // 8K
      4'd3:    boot_last = 16'd16383;         // 16K
      default: boot_last = 16'd32767;         // 32K (and any out-of-range value)
    endcase
  end

  // ROM_DATA is valid on the ROM_RDY pulse, so the block RAM write happens on
  // that same edge, with boot_addr still pointing at the byte just fetched
  assign boot_we     = (boot_state == BOOT_WAIT) & ROM_RDY;
  assign boot_active = (boot_state != BOOT_DONE);
  assign BOOT_ACTIVE = boot_active;

  always @(posedge CLK) begin
    if (RST) begin
      boot_state <= BOOT_REQ;
      boot_addr  <= 15'd0;
      ROM_RRQ    <= 1'b0;
      ROM_ADDR   <= 24'd0;
    end else begin
      ROM_RRQ <= 1'b0;
      case (boot_state)
        BOOT_REQ: begin
          ROM_RRQ    <= 1'b1;
          ROM_ADDR   <= {9'd0, boot_addr};
          boot_state <= BOOT_WAIT;
        end
        BOOT_WAIT: if (ROM_RDY) begin
          if ({1'b0, boot_addr} == boot_last[15:0]) begin
            boot_state <= BOOT_DONE;
          end else begin
            boot_addr  <= boot_addr + 15'd1;
            boot_state <= BOOT_REQ;
          end
        end
        default: ;                            // BOOT_DONE: stay put until reset
      endcase
    end
  end

  // ---------------------------------------------------------------------
  // TIA + audio
  // ---------------------------------------------------------------------
  wire [7:0] tia_dout;
  wire [3:0] audc0, audc1, audv0, audv1;
  wire [4:0] audf0, audf1;

  a26_tia tia (
    .CLK(CLK), .RST(RST), .TIA_CE(TIA_CE), .CPU_CE(CPU_CE),
    .CS(tia_cs), .A(A[5:0]), .DIN(cpu_do), .DOUT(tia_dout),
    .WR_STROBE(tia_cs & wr_strobe), .RD_STROBE(tia_cs & rd_strobe),
    .WSYNC_HALT(wsync_halt),
    .INPT45(INPT45),
    .VID_VISIBLE(VID_VISIBLE), .VID_CODE(VID_CODE),
    .LINE_START(LINE_START), .LINE_VIS_START(LINE_VIS_START),
    .COLUBK(COLUBK), .COLUPF(COLUPF), .COLUP0(COLUP0), .COLUP1(COLUP1),
    .CTRLPF(CTRLPF), .VSYNC_EDGE(VSYNC_EDGE), .VBLANK_ON(VBLANK_ON),
    .AUDC0(audc0), .AUDC1(audc1), .AUDF0(audf0), .AUDF1(audf1),
    .AUDV0(audv0), .AUDV1(audv1)
  );

  a26_audio aud (
    .CLK(CLK), .RST(RST), .TIA_CE(TIA_CE),
    .AUDC0(audc0), .AUDC1(audc1), .AUDF0(audf0), .AUDF1(audf1),
    .AUDV0(audv0), .AUDV1(audv1),
    .MIX(AU_MIX), .TICK(AU_TICK)
  );

  // ---------------------------------------------------------------------
  // Read mux.  Every source is registered in CLK and stable long before the
  // enabled edge samples it; the select comes from the address, which is held
  // for the whole CPU cycle.
  // ---------------------------------------------------------------------
  always @* begin
    if      (tia_cs)    cpu_di = tia_dout;
    else if (ram_cs)    cpu_di = riot_ram_q;
    else if (riot_cs)   cpu_di = riot_q;
    else if (sc_rd_win) cpu_di = sc_q;
    else                cpu_di = rom_q;
  end

  assign DBG_AB    = A;
  assign DBG_BANK  = bank;
  assign DBG_WSYNC = wsync_halt;

endmodule
