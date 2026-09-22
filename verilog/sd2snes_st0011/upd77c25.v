`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Engineer: ikari
//
// Create Date:    17:09:03 01/16/2011
// Design Name:
// Module Name:    upd77c25
// Project Name: sd2snes
// Target Devices: xc3s400
// Tool versions: ISE 13.1
// Description: NEC uPD96050 core for ST011 (derived from the uPD77C25 DSP1-4 core)
//
// Dependencies:
//
// Revision:
// Revision 0.2 - core fully operational, firmware download
//
//////////////////////////////////////////////////////////////////////////////////
`include "config.vh"

module upd77c25(
  input [7:0] DI,
  output [7:0] DO,
  input A0,
  input enable,
  input reg_oe_falling,
  input reg_oe_rising,
  input reg_we_rising,
  input RST,
  input CLK,

  input PGM_WR,
  input [23:0] PGM_DI,
  input [13:0] PGM_WR_ADDR,

  // external-program-SRAM readback verification (passed to extpgm)
  output psram_rrq,
  output [23:0] psram_addr,
  input [15:0] psram_din,
  input psram_rdy,

  input vsum_start,
  output vsum_busy,
  output [31:0] vsum,

  input DAT_WR,
  input [15:0] DAT_DI,
  input [10:0] DAT_WR_ADDR,

  input DP_enable,
  input [11:0] DP_ADDR,

  input [15:0] dsp_feat,

  // external program memory -- the only program source (upd77c25_extpgm.v)
  input ext_pgm_en,
  output [18:0] RAM_ADDR,
  inout [7:0] RAM_DATA,
  output RAM_OE,
  output RAM_WE,

  // savestate scan port -- inert here: main.v ties ss_halt/ss_window_en to 0
  input ss_halt,        // MCU debug halt request
  input ss_window_en,   // 1 = DSP1-4 (scan overlay active); 0 = uPD96050 (plain RAM)
  output ss_halted,     // 1 = freeze in effect, safe to snapshot/restore

  // debug
  output [15:0] updDR,
  output [15:0] updSR,
  output [13:0] updPC,
  output [15:0] updA,
  output [15:0] updB,
  output [5:0] updFL_A,
  output [5:0] updFL_B
);

parameter STATE_FETCH = 8'b00000001;
parameter STATE_LOAD  = 8'b00000010;
parameter STATE_ALU1  = 8'b00000100;
parameter STATE_ALU2  = 8'b00001000;
parameter STATE_STORE = 8'b00010000;
parameter STATE_NEXT  = 8'b00100000;
parameter STATE_IDLE1 = 8'b01000000;
parameter STATE_IDLE2 = 8'b10000000;

// STATE_ALU2 exists only as a dead cycle: its entire body is
// "insn_state <= STATE_STORE". Everything STATE_STORE consumes (alu_p,
// alu_q, cond_true) is registered at the STATE_ALU1 edge and is therefore
// already valid without it, and the one signal that used the ALU1|ALU2
// window -- the KLM second data-RAM read address in ram_addra -- still
// presents its address for a full cycle before STATE_STORE samples
// ram_douta. Set to 0 to restore the extra cycle; that is the only
// difference, so it is a one-line revert if this is ever suspect.
parameter SKIP_ALU2 = 1;

// Publish pc_next to the fetch unit during STATE_STORE, a cycle before
// `pc` takes it, so a cache hit costs no stall cycles in STATE_NEXT.
// Set to 0 to fall back to looking up only once pc has changed; both are
// functionally correct, they differ only in cycles per instruction.
// Kept as parameters so st011_rate_tb can A/B them without editing.
parameter PC_LOOKAHEAD = 1;

// Passed through to upd77c25_extpgm. 0 removes the one-shot cache
// prewarm; see that module's PREWARM block for what it is for.
parameter PREWARM_ENABLE = 1;

// Entries in the fetch unit's fully-associative loop buffer (0 disables).
// See the LOOP BUFFER note in upd77c25_extpgm.v.
parameter LOOPBUF_ENTRIES = 0;

// Passed through to upd77c25_extpgm. 1 reads every missed word twice and
// commits only on agreement. Keep 0: it doubles the cost of a cache miss
// (32 -> 62 cycles at 96MHz) for no benefit.
parameter READ_VERIFY = 0;

parameter I_OP = 2'b00;
parameter I_RT = 2'b01;
parameter I_JP = 2'b10;
parameter I_LD = 2'b11;

parameter SR_RQM = 15;
parameter SR_DRS = 12;
parameter SR_DRC = 10;

reg [1:0] flags_ov0;
reg [1:0] flags_ov1;
reg [1:0] flags_z;
reg [1:0] flags_c;
reg [1:0] flags_s0;
reg [1:0] flags_s1;

reg [13:0] pc;        // program counter (14 bits, for ST011's 16K-word space;
                       // the uPD77C25 only needed 11)

reg [7:0] insn_state; // execute state

reg [2:0] regs_dpb;  // widened 2->3 bits: the uPD77C25's RAM is 1K words
                     // (dpb+dph+dpl=10b); the uPD96050's is 2K words (11b)
reg [3:0] regs_dph;
reg [3:0] regs_dpl;

reg [10:0] regs_rp;

wire [15:0] ram_dina;
reg [15:0] ram_dina_r;
assign ram_dina = ram_dina_r;



// External program fetch -- the only program source in this core: 16384
// 24-bit words in the Bus 2 SRAM, 3 bytes per word. See upd77c25_extpgm.v.
wire [23:0] ext_pgm_dout;
wire ext_pgm_ready;
wire ext_pgm_busy_wr;

// ---- THROUGHPUT (ST011) ------------------------------------------------
//
// ST011 moves bulk data through DR by DMA, with no handshake: a byte lands
// every ~370 ns (8-9 cycles of Mesen's 22 MHz DSP clock) whether or not the
// previous one was consumed. The transfer loops (words 197-200 in, 243-247
// out) are 4 instructions per byte. If a byte is not consumed in time it is
// overwritten, the loop counter never reaches zero and the DSP waits in
// JRQM forever.
//
// At 96MHz a cached instruction takes 6 cycles (62.5 ns), about 6 per byte
// slot. SKIP_ALU2 and PC_LOOKAHEAD are what get it there. An external fetch
// costs ~32 cycles, i.e. a whole slot, so the transfer code must never miss:
// see PINNED TABLE and PREWARM in upd77c25_extpgm.v.
wire [13:0] pc_next;        // combinational next-pc, valid in STATE_STORE
wire pc_early_valid;

upd77c25_extpgm #(.PREWARM_ENABLE(PREWARM_ENABLE),
                  .READ_VERIFY(READ_VERIFY)) extpgm (
  .CLK(CLK),
  .psram_rrq(psram_rrq),
  .psram_addr(psram_addr),
  .psram_din(psram_din),
  .psram_rdy(psram_rdy),
  .vsum_start(vsum_start),
  .vsum_busy(vsum_busy),
  .vsum(vsum),
  .enable(ext_pgm_en),

  .pc(pc),
  .pc_early(pc_next),
  .pc_early_valid(pc_early_valid),
  .dout(ext_pgm_dout),
  .ready(ext_pgm_ready),

  .PGM_WR(PGM_WR),
  .PGM_DI(PGM_DI),
  .PGM_WR_ADDR(PGM_WR_ADDR),
  .wr_busy(ext_pgm_busy_wr),

  .RAM_ADDR(RAM_ADDR),
  .RAM_DATA(RAM_DATA),
  .RAM_OE(RAM_OE),
  .RAM_WE(RAM_WE)
);

// Program source. The 2048-word on-chip program ROM is not instantiated:
// the ST011 program is 16384 words and always comes from the external fetch
// path (upd77c25_extpgm.v).
//
// There is therefore no opcode source while ext_pgm_en is low. That is safe
// because mcu_cmd.v powers up with dspx_reset_out = 1 and main.v wires
// .RST(~dspx_reset), so the core stays in reset until the MCU has written
// the feature bits and loaded the firmware. The cold-start gates below also
// require ext_pgm_en & ext_pgm_ready, so if that ordering ever changes the
// core stalls at pc=0 instead of executing garbage.
wire [23:0] opcode_w = ext_pgm_dout;
reg [1:0] op;
reg [1:0] op_pselect;
reg [3:0] op_alu;
reg op_asl;
reg [1:0] op_dpl;
reg [3:0] op_dphm;
reg op_rpdcr;
reg [3:0] op_src;
reg [3:0] op_dst;

wire [15:0] dat_doutb;
// The data ROM image arrives byte-swapped relative to the true words
// (confirmed against the ST011 dump and the MesenCE trace). Data ROM storage
// does not transform bytes, so the swap is applied here at the point of use.
wire [15:0] dat_doutb_fixed = {dat_doutb[7:0], dat_doutb[15:8]};

`ifdef MK2
`ifndef DEBUG
upd77c25_datrom datrom (
  .clka(CLK), // input clka
  .wea(DAT_WR), // input [0 : 0] wea
  .addra(DAT_WR_ADDR), // input [10 : 0] addra
  .dina(DAT_DI), // input [15 : 0] dina
  .clkb(CLK), // input clkb
  .addrb(regs_rp), // input [10 : 0] addrb
  .doutb(dat_doutb) // output [15 : 0] doutb
);
`endif
`endif
`ifdef MK3
upd77c25_datrom datrom (
  .clock(CLK), // input clka
  .wren(DAT_WR), // input [0 : 0] wea
  .wraddress(DAT_WR_ADDR), // input [10 : 0] addra
  .data(DAT_DI), // input [15 : 0] dina
  .rdaddress(regs_rp), // input [10 : 0] addrb
  .q(dat_doutb) // output [15 : 0] doutb
);
`endif

wire [15:0] ram_douta;
wire [10:0] ram_addra;
reg [7:0] DP_DOr;
wire [7:0] DP_DO;
wire [7:0] UPD_DO;

// suppress RAM write when the access targets the scan register-file / control
// ss_ctrl / ss_regwin / ss_frozen are USED here (and at ram_wea below) but assigned
// further down.  Declare them before first use -- XST (mk2) rejects use-before-decl;
// Quartus (mk3) tolerates it.  Split decl/assign keeps the savestate functional on both.
wire ss_ctrl;
wire ss_regwin;
reg  ss_frozen; initial ss_frozen = 1'b0;
wire ram_web = reg_we_rising & DP_enable & ~ss_regwin & ~ss_ctrl;

`ifdef MK2
`ifndef DEBUG
upd77c25_datram datram (
  .clka(CLK), // input clka
  .wea(ram_wea), // input [0 : 0] wea
  .addra(ram_addra), // input [10 : 0] addra
  .dina(ram_dina), // input [15 : 0] dina
  .douta(ram_douta), // output [15 : 0] douta
  .clkb(CLK), // input clkb
  .web(ram_web), // input [0 : 0] web
  .addrb(DP_ADDR), // input [11 : 0] addrb
  .dinb(DI), // input [7 : 0] dinb
  .doutb(DP_DO) // output [7 : 0] doutb
);
`endif
`endif
`ifdef MK3
upd77c25_datram datram (
  .clock(CLK), // input clka
  .wren_a(ram_wea), // input [0 : 0] wea
  .address_a(ram_addra), // input [10 : 0] addra
  .data_a(ram_dina), // input [15 : 0] dina
  .q_a(ram_douta), // output [15 : 0] douta
  .wren_b(ram_web), // input [0 : 0] web
  .address_b(DP_ADDR), // input [11 : 0] addrb
  .data_b(DI), // input [7 : 0] dinb
  .q_b(DP_DO) // output [7 : 0] doutb
);
`endif
// gate off the core's port-A RAM write while halted, so it can't race the
// port-B restore (or repeatedly rewrite during the frozen snapshot). The
// pending write (if any) replays on unhalt since insn_state is preserved.
assign ram_wea = ((op != I_JP) && op_dst == 4'b1111 && insn_state == STATE_NEXT) & ~ss_frozen;
assign ram_addra = {regs_dpb,
                    regs_dph | ((|(insn_state & (STATE_ALU1 | STATE_ALU2)) && op_dst == 4'b1100)
                                ? 4'b0100
                                : 4'b0000),
                    regs_dpl};

reg signed [15:0] regs_k;
reg signed [15:0] regs_l;
reg [15:0] regs_trb;
reg [15:0] regs_tr;
reg [15:0] regs_dr;
reg [15:0] regs_sr;
reg [15:0] regs_so;   // serial output. Serial I/O is not implemented (as in
                       // ares), but SO is load-bearing: SOL/SOM write it and
                       // JMPSO jumps to it -- ST011's command dispatch is a
                       // JMPSO through a data-ROM table (word 19).
reg [15:0] regs_si;   // serial input; never written by any instruction.
reg [3:0] regs_sp;

reg cond_true;

reg [8:0] jp_brch;
reg [10:0] jp_na;
reg [1:0] jp_bank;    // opcode[1:0]: program-page bank bits, only non-zero on
                       // ST011 (16K program space); always 0 for DSP1-4

// jp_brch top 3 bits: 100=JP, 101=CALL (matches the existing call-push check
// below). Bit 0 of the full 9-bit field distinguishes the page-clear ($x00)
// from the page-set ($x01) opcode variant for those two unconditional forms.
wire jp_page_explicit = (jp_brch[8:6] == 3'b100) | (jp_brch[8:6] == 3'b101);
wire jp_page_bit = jp_page_explicit ? jp_brch[0] : pc[13];
wire [13:0] jp_target = {jp_page_bit, jp_bank, jp_na};

// Declared here rather than with the other register file entries: the
// pc_next expression below reads it, and ISE requires declaration
// before use even though Icarus does not.
// 16 entries, as in ares. Mesen-S uses 8; a deeper stack than the chip is
// harmless, a shallower one silently corrupts return addresses.
reg [13:0] stack [15:0];

// stack[regs_sp-1], registered. pc_next reads the stack top for RT, and
// PC_LOOKAHEAD publishes pc_next straight into the fetch unit's cache
// address -- so the subtract and the 16-entry mux ended up combinationally
// in front of a block RAM address pin, and became the worst CLK21 path on
// mk2 once the cache arrays were free to move (-1.341 ns).
//
// Safe to register: regs_sp and stack only change at the STATE_STORE edge,
// and the next STATE_STORE is six cycles later, so this is always settled
// by the time pc_next is used.
reg [13:0] stack_top;
always @(posedge CLK) stack_top <= stack[regs_sp-1];

// The next pc, computed combinationally so it can be published to the
// fetch unit during STATE_STORE -- one cycle before `pc` itself takes it.
// STATE_STORE assigns `pc <= pc_next` for every instruction form, so this
// wire and the register can never disagree.
//
// Everything it depends on is stable by STATE_STORE: cond_true and the
// jp_* fields are registered in STATE_ALU1 and STATE_FETCH respectively,
// and regs_sp/stack are only modified by this same STATE_STORE assignment.
assign pc_next =
    (op == I_RT) ? stack_top
  : (op == I_JP) ? (cond_true ? ((jp_brch == 9'b0) ? regs_so[13:0] : jp_target)
                              : (pc + 14'd1))
  :                (pc + 14'd1);
assign pc_early_valid = (PC_LOOKAHEAD != 0) && (insn_state == STATE_STORE);

reg [15:0] ld_id;
reg [3:0] ld_dst;

wire [31:0] mul_result = regs_k * regs_l;
reg [15:0] regs_m;
reg [15:0] regs_n;

reg [15:0] alu_p;
reg [15:0] alu_q;
reg [15:0] alu_r;
// Fresh (this-instruction) overflow, for the arithmetic ALU ops
// (SUB/ADD/SBB/ADC/DEC/INC). Needed as a combinational value separate
// from flags_ov0 itself, since flags_ov1's correct update (see the
// STATE_FETCH block below) depends on comparing this instruction's own
// overflow against the OLD flags_ov1 within the same clock edge --
// something a non-blocking-assigned register can't supply to itself.
wire alu_ov0_arith = op_alu[0]
  ? (alu_q[15] ^ alu_r[15]) & ~(alu_q[15] ^ alu_p[15])
  : (alu_q[15] ^ alu_r[15]) & (alu_q[15] ^ alu_p[15]);

reg [1:0] alu_store;


reg [15:0] idb;

reg [15:0] regs_ab [1:0];

reg [3:0] cpu_wait = 0;

assign updDR = regs_dr;
assign updSR = regs_sr;
assign updPC = pc;
assign updA = regs_ab[0];
assign updB = regs_ab[1];
assign updFL_A = {flags_s1[0],flags_s0[0],flags_c[0],flags_z[0],flags_ov1[0],flags_ov0[0]};
assign updFL_B = {flags_s1[1],flags_s0[1],flags_c[1],flags_z[1],flags_ov1[1],flags_ov0[1]};

// ---- savestate scan port -------------------------------------------------
// INERT IN THIS CORE: main.v ties ss_window_en and ss_halt to 0, so
// ss_ctrl, ss_regwin, ss_halt_eff and ss_frozen fold to 0 and synthesis
// removes the overlay. It is kept, not deleted, because ram_wea/ram_web and
// the DO mux reference these signals; with them at 0 the SNES data RAM window
// at DP_ADDR is plain RAM.
//
// (DSP1-4 background: while halted the state is exposed as a byte window at
// DP_ADDR $600-$6FF, $7FF is a halt control byte; ss_halt_snes is the
// savestate handler's halt source.)
reg ss_halt_snes;
initial ss_halt_snes = 1'b0;
wire ss_halt_eff = ss_halt | ss_halt_snes;
assign ss_ctrl = ss_window_en & DP_enable & (DP_ADDR == 12'h7ff);
always @(posedge CLK) begin
  if(~RST) ss_halt_snes <= 1'b0;
  else if(ss_ctrl & reg_we_rising) ss_halt_snes <= DI[0];
end

// Boundary-gated freeze: when a halt is requested (ss_halt_eff), let the DSP
// finish the current transaction and stop at an IDLE instruction boundary
// (STATE_FETCH with RQM=1, i.e. between CPU<->DSP transactions) rather than
// mid-transaction. This keeps the captured cut consistent with the SNES CPU so
// the handshake re-syncs cleanly on resume. An internal counter forces the
// freeze after 256 cycles if no boundary is reached (so it can never hang).
// ss_frozen declared above (before its first use); assigned in the always block below.
reg [7:0] ss_wait; initial ss_wait = 8'h00;
wire ss_boundary = (insn_state == STATE_FETCH) & regs_sr[SR_RQM];
always @(posedge CLK) begin
  if(~RST | ~ss_halt_eff) begin
    ss_frozen <= 1'b0;
    ss_wait   <= 8'h00;
  end else if(~ss_frozen) begin
    ss_wait <= ss_wait + 1'b1;
    if(ss_boundary | (&ss_wait)) ss_frozen <= 1'b1;
  end
end
assign ss_halted = ss_frozen;

// $600-$6FF register-file window (live only once actually frozen)
assign ss_regwin = ss_window_en & DP_enable & ss_frozen & (DP_ADDR[10:8] == 3'b110);
wire [3:0] ss_stk_idx = (DP_ADDR[7:0] - 8'h34) >> 1;
reg [7:0] ss_reg_do;
// Read the arrays through scalar wires so the combinational readback mux below reads
// scalars, not 2D arrays -- XST (mk2) rejects a memory array in an @(*) sensitivity list
// (Xst:902 "Unexpected ... event"); Quartus (mk3) tolerates it.  Behavior-neutral.
wire [15:0] ss_rab0 = regs_ab[0];
wire [15:0] ss_rab1 = regs_ab[1];
wire [13:0] ss_stk  = stack[ss_stk_idx];
always @(*) begin
  // Window is 0x34-0x53: 16 entries x 2 bytes, matching the stack depth.
  if (DP_ADDR[7:0] >= 8'h34 && DP_ADDR[7:0] <= 8'h53)
    ss_reg_do = DP_ADDR[0] ? {2'b0, ss_stk[13:8]}
                           : ss_stk[7:0];
  else case (DP_ADDR[7:0])
    8'h00: ss_reg_do = pc[7:0];
    8'h01: ss_reg_do = {2'b0, pc[13:8]};
    8'h02: ss_reg_do = ss_rab0[7:0];
    8'h03: ss_reg_do = ss_rab0[15:8];
    8'h04: ss_reg_do = ss_rab1[7:0];
    8'h05: ss_reg_do = ss_rab1[15:8];
    8'h06: ss_reg_do = regs_tr[7:0];
    8'h07: ss_reg_do = regs_tr[15:8];
    8'h08: ss_reg_do = regs_trb[7:0];
    8'h09: ss_reg_do = regs_trb[15:8];
    8'h0a: ss_reg_do = regs_dr[7:0];
    8'h0b: ss_reg_do = regs_dr[15:8];
    8'h0c: ss_reg_do = regs_sr[7:0];
    8'h0d: ss_reg_do = regs_sr[15:8];
    8'h0e: ss_reg_do = regs_rp[7:0];
    8'h0f: ss_reg_do = {5'b0, regs_rp[10:8]};
    8'h10: ss_reg_do = regs_k[7:0];
    8'h11: ss_reg_do = regs_k[15:8];
    8'h12: ss_reg_do = regs_l[7:0];
    8'h13: ss_reg_do = regs_l[15:8];
    8'h14: ss_reg_do = regs_m[7:0];
    8'h15: ss_reg_do = regs_m[15:8];
    8'h16: ss_reg_do = regs_n[7:0];
    8'h17: ss_reg_do = regs_n[15:8];
    8'h18: ss_reg_do = {regs_dph, regs_dpl};
    8'h19: ss_reg_do = {5'b0, regs_dpb};
    8'h1a: ss_reg_do = {4'b0, regs_sp};
    8'h1b: ss_reg_do = insn_state;
    8'h1c: ss_reg_do = {2'b0, updFL_A};
    8'h1d: ss_reg_do = {2'b0, updFL_B};
    8'h1e: ss_reg_do = idb[7:0];
    8'h1f: ss_reg_do = idb[15:8];
    8'h20: ss_reg_do = alu_p[7:0];
    8'h21: ss_reg_do = alu_p[15:8];
    8'h22: ss_reg_do = alu_q[7:0];
    8'h23: ss_reg_do = alu_q[15:8];
    8'h24: ss_reg_do = alu_r[7:0];
    8'h25: ss_reg_do = alu_r[15:8];
    8'h26: ss_reg_do = ram_dina_r[7:0];
    8'h27: ss_reg_do = ram_dina_r[15:8];
    8'h28: ss_reg_do = ld_id[7:0];
    8'h29: ss_reg_do = ld_id[15:8];
    8'h2a: ss_reg_do = {op, op_pselect, op_alu};
    8'h2b: ss_reg_do = {op_asl, op_dpl, op_dphm, op_rpdcr};
    8'h2c: ss_reg_do = {op_src, op_dst};
    8'h2d: ss_reg_do = {ld_dst, 1'b0, alu_store, cond_true};
    8'h2e: ss_reg_do = jp_brch[7:0];
    8'h2f: ss_reg_do = {5'b0, jp_bank, jp_brch[8]};
    8'h30: ss_reg_do = jp_na[7:0];
    8'h31: ss_reg_do = {5'b0, jp_na[10:8]};
    8'h32: ss_reg_do = {4'b0, cpu_wait};
    8'h33: ss_reg_do = 8'hd1; // magic, sanity-check on restore
    8'h54: ss_reg_do = regs_so[7:0];
    8'h55: ss_reg_do = regs_so[15:8];
    8'h56: ss_reg_do = regs_si[7:0];
    8'h57: ss_reg_do = regs_si[15:8];
    default: ss_reg_do = 8'h00;
  endcase
end
// --------------------------------------------------------------------------

initial begin
  alu_store = 2'b11;
  insn_state = STATE_IDLE1;
  regs_sp = 4'b0000;
  pc = 14'b0;
  regs_sr = 16'b0;
  regs_rp = 16'h0000;
  regs_dpb = 3'b0;
  regs_dph = 4'b0;
  regs_dpl = 4'b0;
  regs_k = 16'b0;
  regs_l = 16'b0;
  regs_ab[0] = 16'b0;
  regs_ab[1] = 16'b0;
  flags_ov0 = 2'b0;
  flags_ov1 = 2'b0;
  flags_z = 2'b0;
  flags_c = 2'b0;
  flags_s0 = 2'b0;
  flags_s1 = 2'b0;
  regs_tr = 16'b0;
  regs_trb = 16'b0;
  regs_dr = 16'b0;
  regs_so = 16'b0;
  regs_si = 16'b0;
end

always @(posedge CLK) begin
  if(RST & ~ss_frozen) begin
    if(enable & reg_we_rising & (A0 == 1'b0)) begin
      if(!regs_sr[SR_DRC]) begin
        if(regs_sr[SR_DRS] == 1'b1) begin
          regs_sr[SR_RQM] <= 1'b0;
        end
      end else begin
        regs_sr[SR_RQM] <= 1'b0;
      end
    end
    else if(enable & reg_oe_rising & (A0 == 1'b0)) begin
      if(!regs_sr[SR_DRC]) begin
        if(regs_sr[SR_DRS] == 1'b1) begin
          regs_sr[SR_RQM] <= 1'b0;
        end
      end else begin
        regs_sr[SR_RQM] <= 1'b0;
      end
    end else if((op_src == 4'b1000 && op[1] == 1'b0 && insn_state == STATE_STORE)
             || (op_dst == 4'b0110 && op != 2'b10 && insn_state == STATE_STORE)) begin
      regs_sr[SR_RQM] <= 1'b1;
    end
  end else if(RST & ss_frozen) begin
    if(ss_regwin & reg_we_rising & (DP_ADDR[7:0] == 8'h0d))
      regs_sr[SR_RQM] <= DI[7]; // restore bit 15 (offset $0d high byte)
  end else begin
    regs_sr[SR_RQM] <= 1'b0;
  end
end

always @(posedge CLK) begin
  if(RST & ~ss_frozen) begin
    if(enable & reg_we_rising & (A0 == 1'b0)) begin
      if(!regs_sr[SR_DRC]) begin
        if(regs_sr[SR_DRS] == 1'b0) begin
          regs_sr[SR_DRS] <= 1'b1;
        end else begin
          regs_sr[SR_DRS] <= 1'b0;
        end
      end
    end else if(enable & reg_oe_rising) begin
      case(A0)
        1'b0: begin
          if(!regs_sr[SR_DRC]) begin
            if(regs_sr[SR_DRS] == 1'b0) begin
              regs_sr[SR_DRS] <= 1'b1;
            end else begin
              regs_sr[SR_DRS] <= 1'b0;
            end
          end
        end
      endcase
    end
  end else if(RST & ss_frozen) begin
    if(ss_regwin & reg_we_rising & (DP_ADDR[7:0] == 8'h0d))
      regs_sr[SR_DRS] <= DI[4]; // restore bit 12 (offset $0d high byte)
  end else begin
    regs_sr[SR_DRS] <= 1'b0;
  end
end

always @(posedge CLK) begin
  if(RST & ~ss_frozen) begin
    if(enable & reg_we_rising & (A0 == 1'b0)) begin
      if(!regs_sr[SR_DRC]) begin
        if(regs_sr[SR_DRS] == 1'b0) begin
          regs_dr[7:0] <= DI;
        end else begin
          regs_dr[15:8] <= DI;
        end
      end else begin
        // 8-bit mode: low byte only, high byte preserved. ST011 mixes
        // 8-bit and 16-bit (words 233-239) transfers on live data.
        regs_dr[7:0] <= DI;
      end
    end else if(ld_dst == 4'b0110 && insn_state == STATE_STORE) begin
      if (op == I_OP || op == I_RT) regs_dr <= idb;
      else if (op == I_LD) regs_dr <= ld_id;
    end
  end else if(RST & ss_frozen) begin
    if(ss_regwin & reg_we_rising) begin
      if(DP_ADDR[7:0] == 8'h0a) regs_dr[7:0]  <= DI; // restore low byte
      if(DP_ADDR[7:0] == 8'h0b) regs_dr[15:8] <= DI; // restore high byte
    end
  end else begin
    regs_dr <= 16'h0000;
  end
end

assign UPD_DO = (A0 ? regs_sr[15:8] : (regs_sr[SR_DRC] ? regs_dr[7:0] : (regs_sr[SR_DRS] ? regs_dr[15:8] : regs_dr[7:0])));
assign DO = ss_ctrl ? {7'b0, ss_halted}
          : ss_regwin ? ss_reg_do
          : (DP_enable ? DP_DO : UPD_DO);

always @(posedge CLK) begin
  if(RST & ~ss_frozen) begin
    case(insn_state)
      STATE_FETCH: begin
        insn_state <= STATE_LOAD;
        if(op == I_OP || op == I_RT) begin
          if(|op_alu) begin
            flags_z[op_asl] <= (alu_r == 0);
            flags_s0[op_asl] <= alu_r[15];
          end
          case(op_alu)
            // Logical/shift ops: OV0=0, OV1=0, S1 = new sign. Cy=0 except
            // SAR1/RCL1, which carry the shifted-out bit.
            //
            // ---- OV1 / S1 semantics -------------------------------
            //     if(!ov1) s1 = s0;       // before the per-ALU switch
            //     ov1 = (ov0 & ov1) ? (s0 == s1) : (ov0 | ov1);
            // Same rule in ares, Mesen-S and MesenCE, and verified against the full
            // MesenCE ST011 trace (tools/iss.c, zero mismatches). Two overflows in the
            // same direction leave OV1 set. The S1 guard also applies to the
            // logical/shift ops.
            4'b0001, 4'b0010, 4'b0011, 4'b1010, 4'b1101, 4'b1110, 4'b1111: begin
              flags_c[op_asl] <= 0;
              flags_ov0[op_asl] <= 0;
              flags_ov1[op_asl] <= 0;
              // guarded on the OLD OV1, exactly as in the arithmetic case:
              // S1 is "direction of last overflow" and must survive an op
              // that cannot overflow.
              if(!flags_ov1[op_asl]) flags_s1[op_asl] <= alu_r[15];
            end
            4'b1011: begin  // SAR1
              flags_c[op_asl] <= alu_q[0];
              flags_ov0[op_asl] <= 0;
              flags_ov1[op_asl] <= 0;
              // guarded on the OLD OV1, exactly as in the arithmetic case:
              // S1 is "direction of last overflow" and must survive an op
              // that cannot overflow.
              if(!flags_ov1[op_asl]) flags_s1[op_asl] <= alu_r[15];
            end
            4'b1100: begin  // RCL1
              flags_c[op_asl] <= alu_q[15];
              flags_ov0[op_asl] <= 0;
              flags_ov1[op_asl] <= 0;
              // guarded on the OLD OV1, exactly as in the arithmetic case:
              // S1 is "direction of last overflow" and must survive an op
              // that cannot overflow.
              if(!flags_ov1[op_asl]) flags_s1[op_asl] <= alu_r[15];
            end
            // SUB, ADD, SBB, ADC, DEC, INC: the rule documented above.
            4'b0100, 4'b0101, 4'b0110, 4'b0111, 4'b1000, 4'b1001: begin
              if(op_alu[0]) begin
                flags_c[op_asl] <= (alu_r < alu_q);
              end else begin
                flags_c[op_asl] <= (alu_r > alu_q);
              end
              flags_ov0[op_asl] <= alu_ov0_arith;
              // S1 first (using the OLD OV1), then OV1 (using the OLD S1).
              // They never conflict: S1 only changes when OV1 was clear, and
              // OV1 only depends on S1 when OV1 was set.
              if(!flags_ov1[op_asl]) flags_s1[op_asl] <= alu_r[15];
              flags_ov1[op_asl] <= (alu_ov0_arith & flags_ov1[op_asl])
                                 ? (alu_r[15] == flags_s1[op_asl])
                                 : (alu_ov0_arith | flags_ov1[op_asl]);
            end
          endcase
        end

        op <= opcode_w[23:22];
        op_pselect <= opcode_w[21:20];
        op_alu <= opcode_w[19:16];
        op_asl <= opcode_w[15];
        op_dpl <= opcode_w[14:13];
        op_dphm <= opcode_w[12:9];
        op_rpdcr <= opcode_w[8];
        op_src <= opcode_w[7:4];
        op_dst <= opcode_w[3:0];
        jp_brch <= opcode_w[21:13];
        jp_na <= opcode_w[12:2];
        jp_bank <= opcode_w[1:0];

        ld_id <= opcode_w[21:6];
        ld_dst <= opcode_w[3:0];

        regs_m <= {mul_result[31], mul_result[29:15]};
        regs_n <= {mul_result[14:0], 1'b0};
      end
      STATE_LOAD: begin
        insn_state <= STATE_ALU1;
        case(op)
          I_OP, I_RT: begin
            case(op_src)
              4'b0000: idb <= regs_trb;
              4'b0001: idb <= regs_ab[0];
              4'b0010: idb <= regs_ab[1];
              4'b0011: idb <= regs_tr;
              4'b0100: idb <= {regs_dpb,regs_dph,regs_dpl};
              4'b0101: idb <= regs_rp;
              4'b0110: idb <= dat_doutb_fixed; // Address: [regs_rp]
              4'b0111: idb <= flags_s1[0] ? 16'h7fff : 16'h8000;
              4'b1000: idb <= regs_dr;
              4'b1001: idb <= regs_dr;
              4'b1010: idb <= regs_sr;
              4'b1011: idb <= regs_si; // SI, MSB opcode variant
              4'b1100: idb <= regs_si; // SI, LSB opcode variant
              4'b1101: idb <= regs_k;
              4'b1110: idb <= regs_l;
              4'b1111: idb <= ram_douta; // Address: [regs_dp]
            endcase
          end
        endcase
      end
      STATE_ALU1: begin
        insn_state <= SKIP_ALU2 ? STATE_STORE : STATE_ALU2;
        case(op)
          I_OP, I_RT: begin
            alu_q <= regs_ab[op_asl];
            if(op_alu[3:1] == 3'b100) begin
              alu_p <= 16'h0001;
            end else begin
              case(op_pselect)
                2'b00:
                  alu_p <= ram_douta;
                2'b01:
                  alu_p <= idb;
                2'b10:
                  alu_p <= regs_m;
                2'b11:
                  alu_p <= regs_n;
              endcase
            end
          end
          I_JP: begin
            case(jp_brch)
              9'b000_000_000: cond_true <= 1; // JMPSO, jump to SO register (opcode $000)
              9'b100_000_000: cond_true <= 1; // JP,  page bit cleared (opcode $100)
              9'b100_000_001: cond_true <= 1; // JP,  page bit set     (opcode $101)
              9'b101_000_000: cond_true <= 1; // CALL, page bit cleared (opcode $140)
              9'b101_000_001: cond_true <= 1; // CALL, page bit set     (opcode $141)
              9'b010_000_000: cond_true <= (flags_c[0] == 0);
              9'b010_000_010: cond_true <= (flags_c[0] == 1);
              9'b010_000_100: cond_true <= (flags_c[1] == 0);
              9'b010_000_110: cond_true <= (flags_c[1] == 1);
              9'b010_001_000: cond_true <= (flags_z[0] == 0);
              9'b010_001_010: cond_true <= (flags_z[0] == 1);
              9'b010_001_100: cond_true <= (flags_z[1] == 0);
              9'b010_001_110: cond_true <= (flags_z[1] == 1);
              9'b010_010_000: cond_true <= (flags_ov0[0] == 0);
              9'b010_010_010: cond_true <= (flags_ov0[0] == 1);
              9'b010_010_100: cond_true <= (flags_ov0[1] == 0);
              9'b010_010_110: cond_true <= (flags_ov0[1] == 1);
              9'b010_011_000: cond_true <= (flags_ov1[0] == 0);
              9'b010_011_010: cond_true <= (flags_ov1[0] == 1);
              9'b010_011_100: cond_true <= (flags_ov1[1] == 0);
              9'b010_011_110: cond_true <= (flags_ov1[1] == 1);
              9'b010_100_000: cond_true <= (flags_s0[0] == 0);
              9'b010_100_010: cond_true <= (flags_s0[0] == 1);
              9'b010_100_100: cond_true <= (flags_s0[1] == 0);
              9'b010_100_110: cond_true <= (flags_s0[1] == 1);
              9'b010_101_000: cond_true <= (flags_s1[0] == 0);
              9'b010_101_010: cond_true <= (flags_s1[0] == 1);
              9'b010_101_100: cond_true <= (flags_s1[1] == 0);
              9'b010_101_110: cond_true <= (flags_s1[1] == 1);
              9'b010_110_000: cond_true <= (regs_dpl == 0);
              9'b010_110_001: cond_true <= (regs_dpl != 0);
              9'b010_110_010: cond_true <= (regs_dpl == 4'b1111);
              9'b010_110_011: cond_true <= (regs_dpl != 4'b1111);
              9'b010_111_100: cond_true <= (regs_sr[SR_RQM] == 0);
              9'b010_111_110: cond_true <= (regs_sr[SR_RQM] == 1);
              default: cond_true <= 0;
            endcase
          end
        endcase
      end
      STATE_ALU2: begin
        insn_state <= STATE_STORE;
      end
      STATE_STORE: begin
        insn_state <= STATE_NEXT;
        if(op[1] == 1'b0) begin
          case(op_alu)
            4'b0001: alu_r <= alu_q | alu_p;
            4'b0010: alu_r <= alu_q & alu_p;
            4'b0011: alu_r <= alu_q ^ alu_p;
            4'b0100: alu_r <= alu_q - alu_p;
            4'b0101: alu_r <= alu_q + alu_p;
            // SBB/ADC/SHL1 take carry from the OTHER accumulator's flags
            // (verified against the MesenCE trace; same-accumulator carry
            // mismatches ~17k times).
            4'b0110: alu_r <= alu_q - alu_p - flags_c[~op_asl];
            4'b0111: alu_r <= alu_q + alu_p + flags_c[~op_asl];
            4'b1000: alu_r <= alu_q - alu_p;
            4'b1001: alu_r <= alu_q + alu_p;
            4'b1010: alu_r <= ~alu_q;
            4'b1011: alu_r <= {alu_q[15], alu_q[15:1]};
            4'b1100: alu_r <= {alu_q[14:0], flags_c[~op_asl]};
            4'b1101: alu_r <= {alu_q[13:0], 2'b11};
            4'b1110: alu_r <= {alu_q[11:0], 4'b1111};
            4'b1111: alu_r <= {alu_q[7:0], alu_q[15:8]};
          endcase
        end
        case(op)
          I_OP, I_RT: begin
            case(op_dst)
              4'b0001: begin
                regs_ab[0] <= idb;
                alu_store <= 2'b10;
              end
              4'b0010: begin
                regs_ab[1] <= idb;
                alu_store <= 2'b01;
              end
              4'b0011: regs_tr <= idb;
              4'b0100: {regs_dpb,regs_dph,regs_dpl} <= idb[10:0];
              4'b0101: regs_rp <= idb;
//              4'b0110: regs_dr <= idb;
              4'b0111: begin
                regs_sr[14] <= idb[14];
                regs_sr[13] <= idb[13];
                regs_sr[11] <= idb[11];
                regs_sr[SR_DRC] <= idb[10];
                regs_sr[9] <= idb[9];
                regs_sr[8] <= idb[8];
                regs_sr[7] <= idb[7];
                regs_sr[1] <= idb[1];
                regs_sr[0] <= idb[0];
              end
              4'b1000, 4'b1001: regs_so <= idb; // SO (LSB/MSB opcode variants
                                                 // both write the full value)
              4'b1010: regs_k <= idb;
              4'b1011: begin
                regs_k <= idb;
                regs_l <= dat_doutb_fixed;
              end
              4'b1100: begin
                regs_k <= ram_douta;
                regs_l <= idb;
              end
              4'b1101: regs_l <= idb;
              4'b1110: regs_trb <= idb;
              4'b1111: ram_dina_r <= idb;
            endcase
          end
          I_LD: begin
            case(ld_dst)
              4'b0001: regs_ab[0] <= ld_id;
              4'b0010: regs_ab[1] <= ld_id;
              4'b0011: regs_tr <= ld_id;
              4'b0100: {regs_dpb,regs_dph,regs_dpl} <= ld_id[10:0];
              4'b0101: regs_rp <= ld_id;
//              4'b0110: regs_dr <= ld_id;
              4'b0111: begin
                regs_sr[14] <= ld_id[14];
                regs_sr[13] <= ld_id[13];
                regs_sr[11] <= ld_id[11];
                regs_sr[SR_DRC] <= ld_id[10];
                regs_sr[9] <= ld_id[9];
                regs_sr[8] <= ld_id[8];
                regs_sr[7] <= ld_id[7];
                regs_sr[1] <= ld_id[1];
                regs_sr[0] <= ld_id[0];
              end
              4'b1000, 4'b1001: regs_so <= ld_id;
              4'b1010: regs_k <= ld_id;
              4'b1011: begin
                regs_k <= ld_id;
                regs_l <= dat_doutb_fixed;
              end
              4'b1100: begin
                regs_k <= ram_douta;
                regs_l <= ld_id;
              end
              4'b1101: regs_l <= ld_id;
              4'b1110: regs_trb <= ld_id;
              4'b1111: ram_dina_r <= ld_id;
            endcase
          end
        endcase
        // pc itself now comes from the shared pc_next wire above (which
        // the fetch unit has already seen this cycle); only the side
        // effects stay here.
        pc <= pc_next;
        case(op)
          I_OP, I_RT: begin
            if(op_rpdcr) regs_rp <= regs_rp - 1;
            if(op == I_RT) regs_sp <= regs_sp - 1;
          end
          I_JP: begin
            // CALL/LCALL (jp_brch $14x) push the return address.
            if(cond_true && (jp_brch[8:6] == 3'b101)) begin
              stack[regs_sp] <= pc + 1;
              regs_sp <= regs_sp + 1;
            end
          end
        endcase
        cpu_wait <= dsp_feat[3:0];
      end

      STATE_NEXT: begin
        if(~|cpu_wait) begin
          // Hold until the external fetch for the new pc is ready (a cache or
          // loop-buffer hit makes this true on the first cycle).
          if(ext_pgm_en & ext_pgm_ready) insn_state <= STATE_IDLE1;
        end else begin
          insn_state <= STATE_NEXT;
          cpu_wait <= cpu_wait - 1;
        end
      end

      STATE_IDLE1: begin
        // Cold-start gate. Every instruction after the first reaches
        // this state through STATE_NEXT, which already waited for the
        // fetch -- but the entry into IDLE1 from reset does not. Without
        // this the core runs one instruction's worth of whatever `dout`
        // happens to hold before the first external fetch completes: all
        // zeros on a configured FPGA, which decodes as an OP that
        // advances pc, silently skipping word 0 of the firmware.
        //
        // Costs nothing in steady state (the condition is already true
        // on arrival), and the whole body is inside the guard so the
        // DP/accumulator writeback still happens exactly once.
        if(ext_pgm_en & ext_pgm_ready) begin
          insn_state <= STATE_FETCH;
          case(op)
            I_OP, I_RT: begin
              case(op_dpl)
                2'b01: regs_dpl <= regs_dpl + 1;
                2'b10: regs_dpl <= regs_dpl - 1;
                2'b11: regs_dpl <= 4'b0000;
              endcase
              regs_dph <= regs_dph ^ op_dphm;
              if(|op_alu && alu_store[op_asl]) regs_ab[op_asl] <= alu_r;
              alu_store <= 2'b11;
            end
          endcase
        end
      end
    endcase
  end else if(RST & ss_frozen) begin
    // savestate RESTORE (Phase 2): load FSM-owned state from the scan window.
    // regs_dr and regs_sr bits 15/12 are restored in their own blocks below;
    // everything else is here. Mirrors the read mux offset map exactly.
    if(ss_regwin & reg_we_rising) begin
      if(DP_ADDR[7:0] >= 8'h34 && DP_ADDR[7:0] <= 8'h53) begin
        if(DP_ADDR[0]) stack[ss_stk_idx][13:8] <= DI[5:0];
        else           stack[ss_stk_idx][7:0]  <= DI;
      end else case(DP_ADDR[7:0])
        8'h00: pc[7:0]          <= DI;
        8'h01: pc[13:8]         <= DI[5:0];
        8'h02: regs_ab[0][7:0]  <= DI;
        8'h03: regs_ab[0][15:8] <= DI;
        8'h04: regs_ab[1][7:0]  <= DI;
        8'h05: regs_ab[1][15:8] <= DI;
        8'h06: regs_tr[7:0]     <= DI;
        8'h07: regs_tr[15:8]    <= DI;
        8'h08: regs_trb[7:0]    <= DI;
        8'h09: regs_trb[15:8]   <= DI;
        // $0a/$0b regs_dr restored in its own block
        8'h0c: begin regs_sr[7] <= DI[7]; regs_sr[1] <= DI[1]; regs_sr[0] <= DI[0]; end
        8'h0d: begin regs_sr[14] <= DI[6]; regs_sr[13] <= DI[5]; regs_sr[11] <= DI[3];
                     regs_sr[SR_DRC] <= DI[2]; regs_sr[9] <= DI[1]; regs_sr[8] <= DI[0]; end
        8'h0e: regs_rp[7:0]     <= DI;
        8'h0f: regs_rp[10:8]    <= DI[2:0];
        8'h10: regs_k[7:0]      <= DI;
        8'h11: regs_k[15:8]     <= DI;
        8'h12: regs_l[7:0]      <= DI;
        8'h13: regs_l[15:8]     <= DI;
        8'h14: regs_m[7:0]      <= DI;
        8'h15: regs_m[15:8]     <= DI;
        8'h16: regs_n[7:0]      <= DI;
        8'h17: regs_n[15:8]     <= DI;
        8'h18: begin regs_dph <= DI[7:4]; regs_dpl <= DI[3:0]; end
        8'h19: regs_dpb <= DI[2:0];
        8'h1a: regs_sp  <= DI[3:0];
        8'h1b: insn_state <= DI;
        8'h1c: begin flags_s1[0] <= DI[5]; flags_s0[0] <= DI[4]; flags_c[0] <= DI[3];
                     flags_z[0] <= DI[2]; flags_ov1[0] <= DI[1]; flags_ov0[0] <= DI[0]; end
        8'h1d: begin flags_s1[1] <= DI[5]; flags_s0[1] <= DI[4]; flags_c[1] <= DI[3];
                     flags_z[1] <= DI[2]; flags_ov1[1] <= DI[1]; flags_ov0[1] <= DI[0]; end
        8'h1e: idb[7:0]    <= DI;
        8'h1f: idb[15:8]   <= DI;
        8'h20: alu_p[7:0]  <= DI;
        8'h21: alu_p[15:8] <= DI;
        8'h22: alu_q[7:0]  <= DI;
        8'h23: alu_q[15:8] <= DI;
        8'h24: alu_r[7:0]  <= DI;
        8'h25: alu_r[15:8] <= DI;
        8'h26: ram_dina_r[7:0]  <= DI;
        8'h27: ram_dina_r[15:8] <= DI;
        8'h28: ld_id[7:0]  <= DI;
        8'h29: ld_id[15:8] <= DI;
        8'h2a: begin op <= DI[7:6]; op_pselect <= DI[5:4]; op_alu <= DI[3:0]; end
        8'h2b: begin op_asl <= DI[7]; op_dpl <= DI[6:5]; op_dphm <= DI[4:1]; op_rpdcr <= DI[0]; end
        8'h2c: begin op_src <= DI[7:4]; op_dst <= DI[3:0]; end
        8'h2d: begin ld_dst <= DI[7:4]; alu_store <= DI[2:1]; cond_true <= DI[0]; end
        8'h2e: jp_brch[7:0] <= DI;
        8'h2f: begin jp_brch[8] <= DI[0]; jp_bank <= DI[6:5]; end
        8'h30: jp_na[7:0]   <= DI;
        8'h31: jp_na[10:8]  <= DI[2:0];
        8'h32: cpu_wait     <= DI[3:0];
        8'h54: regs_so[7:0]  <= DI;
        8'h55: regs_so[15:8] <= DI;
        8'h56: regs_si[7:0]  <= DI;
        8'h57: regs_si[15:8] <= DI;
        default: ; // $33 magic (read-only)
      endcase
    end
  end else begin
    insn_state <= STATE_IDLE1;
    pc <= 14'b0;
    regs_sp <= 4'b0000;
    cond_true <= 0;
    regs_sr[14] <= 0;
    regs_sr[13] <= 0;
    regs_sr[11] <= 0;
    regs_sr[SR_DRC] <= 0;
    regs_sr[9] <= 0;
    regs_sr[8] <= 0;
    regs_sr[7] <= 0;
    regs_rp <= 11'h000; // as ares power(); firmware loads RP before use
    regs_dpb <= 3'b0;
    regs_dph <= 4'b0;
    regs_dpl <= 4'b0;
    regs_k <= 16'b0;
    regs_l <= 16'b0;
    regs_ab[0] <= 16'b0;
    regs_ab[1] <= 16'b0;
    flags_ov0 <= 2'b0;
    flags_ov1 <= 2'b0;
    flags_z <= 2'b0;
    flags_c <= 2'b0;
    flags_s0 <= 2'b0;
    flags_s1 <= 2'b0;
    regs_tr <= 16'b0;
    regs_trb <= 16'b0;
    regs_so <= 16'b0;
    regs_si <= 16'b0;
    op_pselect <= 2'b0;
    op_alu <= 4'b0;
    op_asl <= 1'b0;
    op_dpl <= 2'b0;
    op_dphm <= 4'b0;
    op_rpdcr <= 1'b0;
    op_src <= 4'b0;
    op_dst <= 4'b0;
    jp_brch <= 9'b0;
    jp_na <= 11'b0;
    jp_bank <= 2'b0;
    ld_id <= 16'b0;
    ld_dst <= 4'b0;
    regs_m <= 16'b0;
    regs_n <= 16'b0;
  end
end

endmodule
