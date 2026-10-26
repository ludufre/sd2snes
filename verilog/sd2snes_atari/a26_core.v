// SPDX-License-Identifier: GPL-2.0-or-later
//
// a26_core.v -- Atari 2600 core: m6507 + riot6532 + tia + tia_audio.
//               Milestone 5 (simulation level; not yet in the sd2snes shell).
//
// MEMORY MAP (13-bit 6507 bus)
//   A12 = 1              cartridge ROM (read through the external ROM port)
//   A12 = 0, A7 = 0      TIA   (A5..A0 for writes, A3..A0 for reads)
//   A12 = 0, A7 = 1      RIOT  (A9 = 0: 128 bytes of RAM, A9 = 1: I/O + timer)
//   The stack ($0100-$01FF) lands in the same 128 bytes of RAM.
//
// CLOCKING
//   cc_ce (colour clock, 3.579545 MHz) comes from a phase accumulator:
//       CC_INC = round(3579545 * 2^25 / f_clk)
//   1251142 is correct for a 96 MHz clk.  RECOMPUTE for the real Mk.II clock.
//   `pause` suppresses cc_ce: CPU, TIA, RIOT timer and audio all advance on
//   it, so they stop where they are and resume on the next colour clock.
//   The TIA derives the CPU slot: cpu_ce = phi2_ce & rdy (WSYNC stalls the
//   CPU only), riot tick = phi2_ce (the timer never stalls).
//   One CPU cycle spans at least ~12 clk in any real configuration; the RIOT
//   RAM read is registered and needs >= 2.
//
// ROM PORT (the ROM is NOT held in block RAM)
//   After every CPU cycle that starts a cartridge read, the core pulses
//   rom_rd one clock after the address changed, with rom_addr = {bank, A11..A0}.
//   The memory side answers with rom_ack + rom_data.  The answer has to arrive
//   before the next CPU cycle ends (~3 colour clocks).  If it does not,
//   rom_miss latches (sticky, cleared by diag_clr) and the stale byte is used;
//   rom_miss_ev pulses for one clk on every such CPU cycle.
//
// MAPPERS (mapper input)
//   0 = 2K (mirrored)   1 = 4K   2 = F8 (8K)   3 = F6 (16K)   4 = F4 (32K)
//   Hotspots switch the bank on the access itself.  Power-on bank = last bank.
//   Superchip (sc_en = 1, with F8 / F6 / F4): 128 bytes of RAM at the start of every 4K bank.  Write
//   $1000-$107F, read $1080-$10FF (A11..A8 = 0, A7 selects the port); the ROM bytes of that range are
//   never seen.  The RAM is distributed RAM (LUTs): the 16 block RAMs of the shell are all in use.  A
//   read of the write port gives the ROM byte and writes nothing (the console would write the bus
//   value; no program relies on it).  The Mk.II core ROM is 16K, so the firmware sets sc_en for F8 and F6 only.
//   5 = FE (Activision, 8K; step 24): no hotspot in the cartridge window.  The cycle after
//   any access to $01FE (the stack byte of a JSR / RTS made with SP = $FF) carries the high
//   byte of the target / return address on the data bus: D5 = 1 ($Fxxx) selects the first
//   4K of the image, D5 = 0 ($Dxxx) the second.  Power-on bank = the first 4K (reset vector
//   $F000 at image offset $0FFC).  Same rule as the MiSTer core (FE_latch, not cpu_d(5)).
//
// CONTROLS: no logic here.  The shell owns the register window that the SNES
// writes (pads, console switches) and simply drives these inputs:
//   swcha_in : joystick port, active low   (P0 = bits 7:4, P1 = bits 3:0)
//   swchb_in : console switches
//   fire0/1  : INPT4/INPT5 pin levels, 1 = released
//   inpt     : INPT0..3 pin levels (paddles); tie to 0 for joystick-only use
//   All of them are sampled on cc_ce before use (one colour clock of delay).
//
// STATUS: untested when written.  Verify with tb_a26_core.v.
//
`timescale 1ns / 1ps

module a26_core #(
  parameter [24:0] CC_INC = 25'd1251142
) (
  input             clk,
  input             rst,
  input      [2:0]  mapper,
  input             sc_en,        // Superchip RAM (see MAPPERS)

  input      [7:0]  swcha_in,
  input      [7:0]  swchb_in,
  input             fire0,
  input             fire1,
  input      [3:0]  inpt,
  input             pause,        // 1 = no colour clocks: everything below freezes, nothing is reset

  output     [14:0] rom_addr,
  output            rom_rd,
  input      [7:0]  rom_data,
  input             rom_ack,

  output            pix_stb,
  output     [7:0]  pix_x,
  output            pix_vis,
  output     [1:0]  pix_src,
  output     [6:0]  pix_col,
  output     [7:0]  hcount,
  output            vsync,
  output            vblank,
  output     [6:0]  colubk,
  output     [6:0]  colupf,
  output     [6:0]  colup0,
  output     [6:0]  colup1,

  output     [4:0]  aud_mix,

  output            cpu_rdy,
  output     [15:0] cpu_ab,
  output            cpu_sync,
  output reg        rom_miss,
  input             diag_clr,     // clears rom_miss (a new miss on the same clk wins)
  output            rom_miss_ev   // one pulse per CPU cycle that used a stale ROM byte
);

  // ---- colour clock ----------------------------------------------------
  // The carry is registered: cc_ce drives hundreds of clock enables and must
  // come straight from a flip-flop to meet 96 MHz.  Everything downstream
  // sees the same one-clock shift, so the relative timing is unchanged.
  reg  [24:0] acc;
  reg         cc_ce_r;
  wire [25:0] acc_n = {1'b0, acc} + {1'b0, CC_INC};
  wire        cc_ce = cc_ce_r;
  always @(posedge clk) begin
    if (rst) begin acc <= 25'd0; cc_ce_r <= 1'b0; end
    else     begin acc <= acc_n[24:0]; cc_ce_r <= acc_n[25] & ~pause; end
  end

  // ---- controls: resynchronised to the colour clock -------------------------
  // The shell registers change on any clk (a SNES write).  Sampling them on
  // cc_ce makes every path from them into the CPU/RIOT/TIA start at a
  // colour-clock edge, like the rest of the core (see ucf.extra).
  reg [7:0] swcha_s, swchb_s;
  reg       fire0_s, fire1_s;
  reg [3:0] inpt_s;
  always @(posedge clk) begin
    if (rst) begin
      swcha_s <= 8'hFF; swchb_s <= 8'hFF;
      fire0_s <= 1'b1;  fire1_s <= 1'b1; inpt_s <= 4'h0;
    end else if (cc_ce) begin
      swcha_s <= swcha_in; swchb_s <= swchb_in;
      fire0_s <= fire0;    fire1_s <= fire1;   inpt_s <= inpt;
    end
  end

  // ---- CPU ---------------------------------------------------------------
  wire        phi2_ce;
  wire        tia_rdy;
  wire        cpu_ce = phi2_ce & tia_rdy;
  wire        cpu_we;
  wire [7:0]  cpu_dout;
  wire [7:0]  cpu_din;

  m6507 u_cpu (.clk(clk), .rst(rst), .ce(cpu_ce), .din(cpu_din),
               .ab(cpu_ab), .we(cpu_we), .dout(cpu_dout), .sync(cpu_sync));

  assign cpu_rdy = tia_rdy;

  // ---- address decode ----------------------------------------------------
  wire rom_cs  =  cpu_ab[12];
  wire tia_cs  = ~cpu_ab[12] & ~cpu_ab[7];
  wire riot_cs = ~cpu_ab[12] &  cpu_ab[7];

  wire tia_wr  = cpu_ce &  cpu_we & tia_cs;
  wire riot_wr = cpu_ce &  cpu_we & riot_cs;
  wire riot_rd = cpu_ce & ~cpu_we & riot_cs;

  // ---- TIA ---------------------------------------------------------------
  wire [7:0] tia_dout;
  wire [3:0] audc0, audc1, audv0, audv1;
  wire [4:0] audf0, audf1;

  tia u_tia (.clk(clk), .rst(rst), .cc_ce(cc_ce),
             .a(cpu_ab[5:0]), .wr(tia_wr), .din(cpu_dout), .dout(tia_dout),
             .inpt(inpt_s), .inpt4(fire0_s), .inpt5(fire1_s),
             .rdy(tia_rdy), .phi2_ce(phi2_ce), .hcount_o(hcount),
             .vsync(vsync), .vblank(vblank),
             .pix_stb(pix_stb), .pix_x(pix_x), .pix_vis(pix_vis),
             .pix_src(pix_src), .pix_col(pix_col),
             .colubk(colubk), .colupf(colupf), .colup0(colup0), .colup1(colup1),
             .audc0(audc0), .audc1(audc1), .audf0(audf0), .audf1(audf1),
             .audv0(audv0), .audv1(audv1));

  // ---- audio ---------------------------------------------------------------
  wire aud_tick = cc_ce & ((hcount == 8'd9) | (hcount == 8'd81));
  wire [3:0] vol0, vol1;

  tia_audio u_aud (.clk(clk), .rst(rst), .tick(aud_tick),
                   .audc0(audc0), .audc1(audc1), .audf0(audf0), .audf1(audf1),
                   .audv0(audv0), .audv1(audv1),
                   .vol0(vol0), .vol1(vol1), .mix(aud_mix));

  // ---- RIOT ----------------------------------------------------------------
  wire [7:0] riot_dout;
  wire [7:0] pa_out_unused, pa_oe_unused, pb_out_unused, pb_oe_unused;

  riot6532 u_riot (.clk(clk), .rst(rst), .tick(phi2_ce),
                   .rd(riot_rd), .wr(riot_wr),
                   .rs(cpu_ab[9]), .a(cpu_ab[6:0]), .din(cpu_dout),
                   .dout(riot_dout),
                   .pa_in(swcha_s), .pa_out(pa_out_unused), .pa_oe(pa_oe_unused),
                   .pb_in(swchb_s), .pb_out(pb_out_unused), .pb_oe(pb_oe_unused));

  // ---- cartridge: bankswitching and ROM port -------------------------------
  wire [12:0] ab13 = cpu_ab[12:0];

  reg       hs_hit;
  reg [2:0] hs_bank;
  always @* begin
    hs_hit  = 1'b0;
    hs_bank = 3'd0;
    case (mapper)
      3'd2: begin                                   // F8
        if (ab13 == 13'h1FF8) begin hs_hit = 1'b1; hs_bank = 3'd0; end
        if (ab13 == 13'h1FF9) begin hs_hit = 1'b1; hs_bank = 3'd1; end
      end
      3'd3: begin                                   // F6
        if (ab13 >= 13'h1FF6 && ab13 <= 13'h1FF9) begin
          hs_hit = 1'b1; hs_bank = {1'b0, ab13[3:0] - 4'd6};
        end
      end
      3'd4: begin                                   // F4
        if (ab13 >= 13'h1FF4 && ab13 <= 13'h1FFB) begin
          hs_hit = 1'b1; hs_bank = ab13[3:0] - 4'd4;
        end
      end
      default: ;
    endcase
  end

  reg [2:0] last_bank;
  always @* begin
    case (mapper)
      3'd2:    last_bank = 3'd1;
      3'd3:    last_bank = 3'd3;
      3'd4:    last_bank = 3'd7;
      3'd5:    last_bank = 3'd0;      // FE powers up in the first 4K
      default: last_bank = 3'd0;
    endcase
  end

  // FE: fe_arm = the cycle now completing follows an access to $01FE; fe_bus = its data bus
  reg        fe_arm;
  wire [7:0] fe_bus = cpu_we ? cpu_dout : cpu_din;
  always @(posedge clk) begin
    if (rst)         fe_arm <= 1'b0;
    else if (cpu_ce) fe_arm <= (cpu_ab[12:0] == 13'h01FE);
  end

  reg [2:0] bank;
  always @(posedge clk) begin
    if (rst)                                bank <= last_bank;
    else if (cpu_ce && rom_cs && hs_hit)    bank <= hs_bank;
    else if (cpu_ce && mapper == 3'd5 && fe_arm) bank <= {2'b00, ~fe_bus[5]};
  end

  wire [2:0] bank_eff = (rom_cs && hs_hit) ? hs_bank : bank;
  assign rom_addr = (mapper == 3'd0) ? {4'b0000, 1'b0, cpu_ab[10:0]}
                                     : {bank_eff, cpu_ab[11:0]};

  reg ce_d;
  always @(posedge clk) ce_d <= cpu_ce | rst;
  assign rom_rd = ce_d & rom_cs & ~cpu_we;

  reg [7:0] rom_q;
  reg       rom_valid;
  always @(posedge clk) begin
    if (rom_ack) rom_q <= rom_data;
    if (rst) begin
      rom_valid <= 1'b0;
      rom_miss  <= 1'b0;
    end else begin
      if (rom_ack)      rom_valid <= 1'b1;
      else if (cpu_ce)  rom_valid <= 1'b0;
      if (diag_clr)     rom_miss <= 1'b0;
      if (rom_miss_ev)  rom_miss <= 1'b1;
    end
  end
  assign rom_miss_ev = cpu_ce & rom_cs & ~cpu_we & ~rom_valid;

  // ---- Superchip RAM ---------------------------------------------------------
  // Same shape as the RIOT RAM: registered read, so the output is stable long before the next cpu_ce.
  // (ucf.extra: the RAM and everything it feeds stay in the slow group.)
  wire sc_win = rom_cs & sc_en & (cpu_ab[11:8] == 4'b0000);
  wire sc_wr  = cpu_ce & cpu_we & sc_win & ~cpu_ab[7];
  wire sc_rd  = sc_win & cpu_ab[7] & ~cpu_we;
  (* ram_style = "distributed" *) reg [7:0] sc_ram [0:127];
  reg [7:0] sc_q;
  always @(posedge clk) begin
    if (sc_wr) sc_ram[cpu_ab[6:0]] <= cpu_dout;
    sc_q <= sc_ram[cpu_ab[6:0]];
  end

  // ---- CPU read mux --------------------------------------------------------
  // Passo 29: the TIA drives only D7/D6 on a read; D5..D0 keep the last byte the bus carried (the operand
  // or address byte of the previous cycle, or the byte of a dummy read), as on the console.  Some programs
  // depend on it (e.g. AND $0F written for AND #$0F).
  reg [7:0] last_bus = 8'h00;
  always @(posedge clk) if (cpu_ce) last_bus <= cpu_we ? cpu_dout : cpu_din;
  assign cpu_din = rom_cs ? (sc_rd ? sc_q : rom_q) : (riot_cs ? riot_dout : {tia_dout[7:6], last_bus[5:0]});

endmodule
