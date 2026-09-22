`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////
// st018_cpu.v -- compact multicycle ARMv3 (ARM6-class) core for the ST018
//
// Written for sd2snes (GPL-2.0, same terms as the rest of the project).
//
// Design goals, in order:
//   1. Architecturally correct for the 32-bit ARMv3 instruction set the ST018
//      firmware uses (verified in lockstep against an external reference
//      ARMv3 interpreter; see sim/lockstep/).
//   2. Small enough for the mk2's XC3S400 next to the sd2snes base logic.
//   3. Shallow logic per cycle so it runs directly from CLK2 (96 MHz).
//
// It is deliberately NOT cycle accurate and NOT pipelined. Every instruction
// walks through a short state sequence. A one-entry prefetch buffer fetches
// the next sequential instruction while the current one executes, which hides
// the memory latency for ordinary data-processing instructions.
//
// Implemented: all 16 data-processing ops (immediate / immediate-shift /
// register-shift operands, full barrel-shifter carry semantics), MUL/MLA,
// LDR/STR/LDRB/STRB (all addressing modes, rotated unaligned word loads),
// LDM/STM (all four modes, writeback, S bit / user bank, PC in list, empty
// list), SWP/SWPB, B/BL, SWI, MRS/MSR, undefined-instruction trap (coprocessor
// space and the ARMv4-only long multiplies), full register banking for
// USR/FIQ/IRQ/SVC/ABT/UND/SYS.
//
// Not implemented (the ST018 has no source for them): IRQ/FIQ inputs,
// abort inputs, 26-bit address modes, coprocessor interface.
//
// Edge-case policy follows the reference interpreter where the architecture
// leaves room
// (R15+12 for register-specified shifts and stores of PC, LDM/STM writeback
// timing, empty register list), because that is the implementation the game
// is known to run on.
//
// Memory interface: one outstanding request. m_req and the request fields are
// held until m_ack (a single-cycle pulse; m_rdata is valid in that cycle).
// The CPU never presents a new request in the cycle m_ack is high, and the
// memory side must not start a transaction in the cycle it acks. Word accesses
// ignore m_addr[1:0]; byte writes replicate the byte on all four lanes.
//////////////////////////////////////////////////////////////////////////////
module st018_cpu (
  input             clk,
  input             rst,          // synchronous, active high

  output reg        m_req,
  output reg        m_we,
  output reg        m_byte,
  output reg        m_code,       // 1 = instruction fetch
  output reg [31:0] m_addr,
  output reg [31:0] m_wdata,
  input      [31:0] m_rdata,
  input             m_ack,

  // debug / verification only (unconnected in the FPGA build -> pruned)
  output reg        dbg_retire,
  output reg [31:0] dbg_pc,
  output reg [31:0] dbg_ir,
  output            dbg_rf_we,
  output     [4:0]  dbg_rf_wa,
  output     [31:0] dbg_rf_wd,
  output     [31:0] dbg_cpsr
);

// ---------------------------------------------------------------------------
// state encoding
// ---------------------------------------------------------------------------
localparam [5:0]
  S_RESET = 6'd0,  S_FETCH = 6'd1,  S_DEC   = 6'd2,  S_OPS   = 6'd3,
  S_RS    = 6'd4,  S_SH    = 6'd5,  S_ALU   = 6'd6,  S_WB    = 6'd7,
  S_LADDR = 6'd8,  S_LREQ  = 6'd9,  S_LWAIT = 6'd10, S_LWB   = 6'd11,
  S_LWB2  = 6'd12, S_BR    = 6'd13, S_MRS   = 6'd14, S_MSR   = 6'd15,
  S_EXC   = 6'd16, S_BM1   = 6'd17, S_BM2   = 6'd18, S_BM3   = 6'd19,
  S_BMN   = 6'd20, S_BMS   = 6'd21, S_BMQ   = 6'd22, S_BMW   = 6'd23,
  S_BML   = 6'd24, S_BMD   = 6'd25, S_MUL1  = 6'd26, S_MUL2  = 6'd27,
  S_MUL3  = 6'd28, S_MUL4  = 6'd29, S_MULWB = 6'd30, S_SW1   = 6'd31,
  S_SW2   = 6'd32, S_SW3   = 6'd33, S_SW4   = 6'd34, S_SWWB  = 6'd35;

localparam [4:0] M_USR = 5'b10000, M_FIQ = 5'b10001, M_IRQ = 5'b10010,
                 M_SVC = 5'b10011, M_ABT = 5'b10111, M_UND = 5'b11011;

// shifter operations (registered control word)
localparam [2:0] SH_PASS = 3'd0, SH_ROT = 3'd1, SH_LSL = 3'd2, SH_LSR = 3'd3,
                 SH_ASR  = 3'd4, SH_RRX = 3'd5, SH_ZERO = 3'd6, SH_SIGN = 3'd7;

reg [5:0]  st;
reg [31:0] ir;
reg [31:0] pc;                      // address of the current instruction
reg [31:0] pc4_r, pc8_r, pc12_r;    // registered in S_DEC

// CPSR
reg f_n, f_z, f_c, f_v, f_i, f_f;
reg [4:0] mode;
// SPSRs {N,Z,C,V,I,F,M[4:0]}: 0 fiq, 1 irq, 2 svc, 3 abt, 4 und
reg [10:0] spsr0, spsr1, spsr2, spsr3, spsr4;   // not reset (undefined after reset on ARM)
initial begin spsr0 = 11'h10; spsr1 = 11'h10; spsr2 = 11'h10; spsr3 = 11'h10; spsr4 = 11'h10; end

// datapath registers
reg [31:0] opA, opB, shout, res, addr_calc, wd, mdata, base, wbv, bm_addr, acc;
reg        shc, alu_c, alu_v;
reg [31:0] p0;
reg [15:0] p1, p2, s12;
reg        pcw;                     // a PC write is pending for this instruction
reg [31:0] pcw_val;

// shifter control
reg [2:0]  sh_op;
reg [4:0]  sh_k;
reg [31:0] sh_mask;

// block transfer
reg [15:0] bm_mask;
reg [4:0]  bm_cnt;
reg [3:0]  bm_i;
reg [4:0]  xmode;
reg        bm_first;

// prefetch buffer
reg        pf_valid;
reg [29:0] pf_addr;
reg [31:0] pf_data;
reg        pf_want;
reg [29:0] pf_want_addr;

initial begin
  st = S_RESET; ir = 32'd0; pc = 32'd0; pc4_r = 32'd0; pc8_r = 32'd0; pc12_r = 32'd0;
  m_req = 1'b0; m_we = 1'b0; m_byte = 1'b0; m_code = 1'b0; m_addr = 32'd0; m_wdata = 32'd0;
  pf_valid = 1'b0; pf_addr = 30'd0; pf_data = 32'd0; pf_want = 1'b0; pf_want_addr = 30'd0;
  dbg_retire = 1'b0; dbg_pc = 32'd0; dbg_ir = 32'd0; pcw = 1'b0; pcw_val = 32'd0;
end

// ---------------------------------------------------------------------------
// instruction fields / decode (ir is stable from S_DEC to the end)
// ---------------------------------------------------------------------------
wire [3:0] rn = ir[19:16];
wire [3:0] rd = ir[15:12];
wire [3:0] rs = ir[11:8];
wire [3:0] rm = ir[3:0];
wire       b_I = ir[25], b_P = ir[24], b_U = ir[23], b_B = ir[22],
           b_W = ir[21], b_L = ir[20], b_S = ir[20];

wire c_mul  = (ir[27:23] == 5'b00000) && (ir[7:4] == 4'b1001);
wire c_mull = (ir[27:23] == 5'b00001) && (ir[7:4] == 4'b1001);   // ARMv4 only
wire c_swp  = (ir[27:24] == 4'b0001)  && (ir[7:4] == 4'b1001);
wire c_psr  = (ir[27:26] == 2'b00) && (ir[24:23] == 2'b10) && !ir[20] && !c_swp;
wire c_dp   = (ir[27:26] == 2'b00) && !c_mul && !c_mull && !c_swp && !c_psr;
wire c_sdt  = (ir[27:26] == 2'b01);
wire c_bdt  = (ir[27:25] == 3'b100);
wire c_b    = (ir[27:25] == 3'b101);
wire c_swi  = (ir[27:24] == 4'b1111);
wire c_und  = ((ir[27:26] == 2'b11) && !c_swi) || c_mull;

wire [3:0] dp_op   = ir[24:21];
wire dp_test  = (dp_op[3:2] == 2'b10);                  // TST TEQ CMP CMN
wire dp_arith = (dp_op[3:1] == 3'b001) || (dp_op[3:1] == 3'b010) ||
                (dp_op[3:1] == 3'b011) || (dp_op[3:1] == 3'b101);
wire dp_regsh = !b_I && ir[4];

// SDT: bit 25 CLEAR means immediate offset
wire sdt_imm = !b_I;
wire sdt_wb  = (b_W || !b_P) && (rd != rn || !b_L);

wire bm_empty = (ir[15:0] == 16'h0000);
wire bm_haspc = ir[15] | bm_empty;

// instructions that (may) write R15 -- no sequential prefetch for these
wire pcw_dp  = c_dp && !dp_test && (rd == 4'd15);
wire pcw_sdt = c_sdt && ((b_L && rd == 4'd15) || (sdt_wb && rn == 4'd15));
wire pcw_bdt = c_bdt && ((b_L && bm_haspc) || (b_W && rn == 4'd15));
wire pcw_misc = (c_swp && rd == 4'd15) || (c_psr && !ir[21] && rd == 4'd15);
wire dec_nopf = c_b | c_swi | c_und | pcw_dp | pcw_sdt | pcw_bdt | pcw_misc;

reg cond_pass;
always @* begin
  case (ir[31:28])
    4'h0: cond_pass = f_z;
    4'h1: cond_pass = !f_z;
    4'h2: cond_pass = f_c;
    4'h3: cond_pass = !f_c;
    4'h4: cond_pass = f_n;
    4'h5: cond_pass = !f_n;
    4'h6: cond_pass = f_v;
    4'h7: cond_pass = !f_v;
    4'h8: cond_pass = f_c && !f_z;
    4'h9: cond_pass = !f_c || f_z;
    4'ha: cond_pass = (f_n == f_v);
    4'hb: cond_pass = (f_n != f_v);
    4'hc: cond_pass = !f_z && (f_n == f_v);
    4'hd: cond_pass = f_z || (f_n != f_v);
    4'he: cond_pass = 1'b1;
    default: cond_pass = 1'b0;
  endcase
end

// ---------------------------------------------------------------------------
// PSR helpers
// ---------------------------------------------------------------------------
wire [31:0] cpsr_w = {f_n, f_z, f_c, f_v, 20'd0, f_i, f_f, 1'b0, mode};
assign dbg_cpsr = cpsr_w;

reg        has_spsr;
reg [10:0] spsr_cur;
always @* begin
  has_spsr = 1'b1;
  case (mode)
    M_FIQ: spsr_cur = spsr0;
    M_IRQ: spsr_cur = spsr1;
    M_SVC: spsr_cur = spsr2;
    M_ABT: spsr_cur = spsr3;
    M_UND: spsr_cur = spsr4;
    default: begin has_spsr = 1'b0;
                   spsr_cur = {f_n, f_z, f_c, f_v, f_i, f_f, mode}; end
  endcase
end
wire [31:0] spsr_w = {spsr_cur[10:7], 20'd0, spsr_cur[6:5], 1'b0, spsr_cur[4:0]};

// banked register mapping: (architectural reg, mode) -> physical slot 0..30
function [4:0] phys;
  input [3:0] r;
  input [4:0] m;
  begin
    if (!r[3])                            phys = {1'b0, r};
    else if (m == M_FIQ && r != 4'd15)    phys = {2'b10, r[2:0]};     // 16..22
    else if (r == 4'd13 || r == 4'd14) begin
      case (m)
        M_IRQ:   phys = r[0] ? 5'd23 : 5'd24;
        M_SVC:   phys = r[0] ? 5'd25 : 5'd26;
        M_ABT:   phys = r[0] ? 5'd27 : 5'd28;
        M_UND:   phys = r[0] ? 5'd29 : 5'd30;
        default: phys = {1'b0, r};
      endcase
    end else                              phys = {1'b0, r};
  end
endfunction

// ---------------------------------------------------------------------------
// register file
// ---------------------------------------------------------------------------
reg        rf_we;
reg [4:0]  rf_wa;
reg [31:0] rf_wd;
reg [3:0]  ra_r, rb_r;       // architectural read registers this cycle
wire [4:0] rmode = (st == S_BMN || st == S_BMS) ? xmode : mode;
wire [31:0] rfA, rfB;

st018_regfile u_rf (
  .clk(clk),
  .we(rf_we & ~rst), .wa(rf_wa), .wd(rf_wd),
  .ra(phys(ra_r, rmode)), .rda(rfA),
  .rb(phys(rb_r, mode)),  .rdb(rfB)
);
assign dbg_rf_we = rf_we & ~rst;
assign dbg_rf_wa = rf_wa;
assign dbg_rf_wd = rf_wd;

// ---------------------------------------------------------------------------
// barrel shifter (datapath in S_SH, controls computed a cycle earlier)
// ---------------------------------------------------------------------------
wire [31:0] r1 = sh_k[0] ? {opB[0],     opB[31:1]}  : opB;
wire [31:0] r2 = sh_k[1] ? {r1[1:0],   r1[31:2]}   : r1;
wire [31:0] r3 = sh_k[2] ? {r2[3:0],   r2[31:4]}   : r2;
wire [31:0] r4 = sh_k[3] ? {r3[7:0],   r3[31:8]}   : r3;
wire [31:0] rot = sh_k[4] ? {r4[15:0], r4[31:16]}  : r4;

reg [31:0] sh_y;
reg        sh_c;
always @* begin
  case (sh_op)
    SH_PASS: begin sh_y = opB;                  sh_c = f_c;     end
    SH_ROT:  begin sh_y = rot;                  sh_c = rot[31]; end
    SH_LSL:  begin sh_y = rot & sh_mask;        sh_c = rot[0];  end
    SH_LSR:  begin sh_y = rot & sh_mask;        sh_c = rot[31]; end
    SH_ASR:  begin sh_y = (rot & sh_mask) | (~sh_mask & {32{opB[31]}});
                                                sh_c = rot[31]; end
    SH_RRX:  begin sh_y = {f_c, opB[31:1]};     sh_c = opB[0];  end
    SH_ZERO: begin sh_y = 32'd0;                sh_c = 1'b0;    end
    default: begin sh_y = {32{opB[31]}};        sh_c = opB[31]; end
  endcase
end

// shifter control generation: amount source is the immediate field in S_DEC
// and the Rs register in S_RS.
reg  [1:0]  ctl_typ;
reg  [7:0]  ctl_amt;
reg         ctl_immf;       // immediate-shift semantics for #0
reg  [2:0]  ctl_op;
reg  [4:0]  ctl_k;
reg  [5:0]  ctl_n;
reg  [31:0] ctl_mask;
reg  [8:0]  ctl_ae;
integer ci;
always @* begin
  if (st == S_RS) begin                         // register-specified amount
    ctl_typ  = ir[6:5];
    ctl_amt  = (rs == 4'd15) ? pc12_r[7:0] : rfA[7:0];
    ctl_immf = 1'b0;
  end else if ((c_dp || c_psr) && b_I) begin    // rotated 8-bit immediate
    ctl_typ  = 2'd3;
    ctl_amt  = {3'd0, ir[11:8], 1'b0};
    ctl_immf = 1'b0;
  end else begin                                // immediate shift amount
    ctl_typ  = ir[6:5];
    ctl_amt  = {3'd0, ir[11:7]};
    ctl_immf = 1'b1;
  end
end
always @* begin
  ctl_ae = {1'b0, ctl_amt};
  if (ctl_immf && ctl_amt == 8'd0 && (ctl_typ == 2'd1 || ctl_typ == 2'd2))
    ctl_ae = 9'd32;
  ctl_k = 5'd0;
  ctl_n = 6'd0;
  if (ctl_immf && ctl_amt == 8'd0 && ctl_typ == 2'd3)
    ctl_op = SH_RRX;
  else if (ctl_ae == 9'd0)
    ctl_op = SH_PASS;
  else begin
    case (ctl_typ)
      2'd0: begin                                          // LSL
        if (ctl_ae <= 9'd32) begin ctl_op = SH_LSL; ctl_k = 5'd0 - ctl_ae[4:0]; ctl_n = ctl_ae[5:0]; end
        else ctl_op = SH_ZERO;
      end
      2'd1: begin                                          // LSR
        if (ctl_ae <= 9'd32) begin ctl_op = SH_LSR; ctl_k = ctl_ae[4:0]; ctl_n = ctl_ae[5:0]; end
        else ctl_op = SH_ZERO;
      end
      2'd2: begin                                          // ASR
        if (ctl_ae < 9'd32) begin ctl_op = SH_ASR; ctl_k = ctl_ae[4:0]; ctl_n = ctl_ae[5:0]; end
        else ctl_op = SH_SIGN;
      end
      default: begin ctl_op = SH_ROT; ctl_k = ctl_ae[4:0]; end  // ROR
    endcase
  end
  // LSL keeps bits i >= n; LSR/ASR keep bits i < 32-n (mirror image)
  for (ci = 0; ci < 32; ci = ci + 1) begin
    if (ctl_typ == 2'd0) ctl_mask[ci]    = (ci >= ctl_n);
    else                 ctl_mask[31-ci] = (ci >= ctl_n);
  end
end

// ---------------------------------------------------------------------------
// ALU (S_ALU)
// ---------------------------------------------------------------------------
reg  [31:0] ax, ay;
reg         acin;
always @* begin
  ax = opA; ay = shout; acin = 1'b0;
  case (dp_op)
    4'h2, 4'ha: begin ax = opA;   ay = ~shout; acin = 1'b1; end   // SUB CMP
    4'h3:       begin ax = shout; ay = ~opA;   acin = 1'b1; end   // RSB
    4'h5:       begin acin = f_c; end                              // ADC
    4'h6:       begin ax = opA;   ay = ~shout; acin = f_c;  end   // SBC
    4'h7:       begin ax = shout; ay = ~opA;   acin = f_c;  end   // RSC
    default:    ;                                                  // ADD CMN
  endcase
end
wire [32:0] asum = {1'b0, ax} + {1'b0, ay} + {32'd0, acin};
wire        av   = (ax[31] == ay[31]) && (asum[31] != ax[31]);

reg [31:0] alu_y;
always @* begin
  case (dp_op)
    4'h0, 4'h8: alu_y = opA & shout;
    4'h1, 4'h9: alu_y = opA ^ shout;
    4'hc:       alu_y = opA | shout;
    4'hd:       alu_y = shout;
    4'he:       alu_y = opA & ~shout;
    4'hf:       alu_y = ~shout;
    default:    alu_y = asum[31:0];
  endcase
end

// ---------------------------------------------------------------------------
// load data formatting (word loads rotate by the low address bits)
// ---------------------------------------------------------------------------
function [31:0] ldfmt;
  input [31:0] d;
  input [1:0]  a;
  input        byte_acc;
  begin
    if (byte_acc)
      case (a)
        2'd0: ldfmt = {24'd0, d[7:0]};
        2'd1: ldfmt = {24'd0, d[15:8]};
        2'd2: ldfmt = {24'd0, d[23:16]};
        default: ldfmt = {24'd0, d[31:24]};
      endcase
    else
      case (a)
        2'd0: ldfmt = d;
        2'd1: ldfmt = {d[7:0],   d[31:8]};
        2'd2: ldfmt = {d[15:0],  d[31:16]};
        default: ldfmt = {d[23:0], d[31:24]};
      endcase
  end
endfunction

reg [1:0] eaddr_lo;                         // low bits of the SDT/SWP address
wire [31:0] ld_sdt = ldfmt(mdata, eaddr_lo, b_B);

// misc combinational helpers
wire [31:0] pc_plus4 = pc + 32'd4;
wire [31:0] br_tgt   = pc8_r + {{6{ir[23]}}, ir[23:0], 2'b00};
reg  [4:0]  bm_pc_cnt;
integer pi;
always @* begin
  bm_pc_cnt = 5'd0;
  for (pi = 0; pi < 16; pi = pi + 1) bm_pc_cnt = bm_pc_cnt + {4'd0, ir[pi]};
end
reg [3:0] bm_low;
integer li;
always @* begin
  bm_low = 4'd0;
  for (li = 15; li >= 0; li = li - 1) if (bm_mask[li]) bm_low = li[3:0];
end

// ---------------------------------------------------------------------------
// combinational per-state control: register reads/writes, finishing, fetch
// ---------------------------------------------------------------------------
reg        fin;            // instruction retires this cycle
reg        fin_jump;       // ... and continues at fin_tgt instead of pc+4
reg [31:0] fin_tgt;
reg        fq_now;         // request an instruction fetch
reg [29:0] fq_addr;
reg        cpsr_restore;   // CPSR <= SPSR (current mode) this cycle

always @* begin
  ra_r = rn; rb_r = rm;
  rf_we = 1'b0; rf_wa = 5'd0; rf_wd = res;
  fin = 1'b0; fin_jump = pcw; fin_tgt = pcw_val;
  fq_now = 1'b0; fq_addr = pc_plus4[31:2];
  cpsr_restore = 1'b0;

  case (st)
    S_FETCH: begin
      fq_addr = pc[31:2];
      fq_now  = !(pf_valid && pf_addr == pc[31:2])
             && !(m_req && m_code && pf_addr == pc[31:2])
             && !(pf_want && pf_want_addr == pc[31:2]);
    end
    S_DEC: begin
      ra_r = c_mul ? rs : rn;
      rb_r = rm;
      fq_addr = pc_plus4[31:2];
      if (!cond_pass) begin
        fin = 1'b1; fin_jump = 1'b0;
        fq_now = !(pf_valid && pf_addr == pc_plus4[31:2]);
      end else
        fq_now = !dec_nopf && !(pf_valid && pf_addr == pc_plus4[31:2]);
    end
    S_OPS: begin
      ra_r = (c_sdt | c_mul) ? rd : rs;   // SDT: store data; MUL: accumulator (bits 15:12)
    end
    S_SH, S_LADDR: ra_r = rd;
    S_WB: begin
      if (!dp_test) begin
        if (rd == 4'd15) begin fin_jump = 1'b1; fin_tgt = res; end
        else begin rf_we = 1'b1; rf_wa = phys(rd, mode); rf_wd = res; end
      end
      if (b_S && rd == 4'd15 && has_spsr) cpsr_restore = 1'b1;
      fin = 1'b1;
    end
    S_MRS: begin
      if (rd == 4'd15) begin fin_jump = 1'b1; fin_tgt = b_B ? spsr_w : cpsr_w; end
      else begin rf_we = 1'b1; rf_wa = phys(rd, mode); rf_wd = b_B ? spsr_w : cpsr_w; end
      fin = 1'b1;
    end
    S_MSR: fin = 1'b1;
    S_LWAIT: begin
      if (m_ack && !m_code && !b_L && !sdt_wb) fin = 1'b1;
    end
    S_LWB: begin
      if (rd == 4'd15) begin fin_jump = 1'b1; fin_tgt = ld_sdt; end
      else begin rf_we = 1'b1; rf_wa = phys(rd, mode); rf_wd = ld_sdt; end
      if (!sdt_wb) fin = 1'b1;
    end
    S_LWB2: begin
      if (rn == 4'd15) begin fin_jump = 1'b1; fin_tgt = addr_calc; end
      else begin rf_we = 1'b1; rf_wa = phys(rn, mode); rf_wd = addr_calc; end
      fin = 1'b1;
    end
    S_BR: begin
      if (ir[24]) begin rf_we = 1'b1; rf_wa = phys(4'd14, mode); rf_wd = pc4_r; end
      fin = 1'b1; fin_jump = 1'b1; fin_tgt = br_tgt;
    end
    S_EXC: begin
      rf_we = 1'b1; rf_wa = phys(4'd14, c_swi ? M_SVC : M_UND); rf_wd = pc4_r;
      fin = 1'b1; fin_jump = 1'b1; fin_tgt = c_swi ? 32'h8 : 32'h4;
    end
    S_BM3: begin
      if (b_L && b_W && rn != 4'd15) begin rf_we = 1'b1; rf_wa = phys(rn, mode); rf_wd = wbv; end
    end
    S_BMN: ra_r = bm_low;
    S_BMS: ra_r = bm_i;
    S_BMW: begin
      if (m_ack && !m_code && !b_L && bm_first && b_W && rn != 4'd15) begin
        rf_we = 1'b1; rf_wa = phys(rn, xmode); rf_wd = wbv;
      end
    end
    S_BML: begin
      if (bm_first && b_W) begin
        if (rn != 4'd15) begin rf_we = 1'b1; rf_wa = phys(rn, xmode); rf_wd = wbv; end
      end else if (bm_i != 4'd15) begin
        rf_we = 1'b1; rf_wa = phys(bm_i, xmode); rf_wd = mdata;
      end
    end
    S_BMD: begin
      fin = 1'b1;
      if (b_L && ir[22] && bm_haspc) cpsr_restore = 1'b1;
    end
    S_MULWB: begin
      // MUL destination lives in bits 19:16 (the rn field)
      if (rn != 4'd15) begin rf_we = 1'b1; rf_wa = phys(rn, mode); rf_wd = res; end
      fin = 1'b1;
    end
    S_SWWB: begin
      if (rd == 4'd15) begin fin_jump = 1'b1; fin_tgt = ldfmt(mdata, eaddr_lo, b_B); end
      else begin rf_we = 1'b1; rf_wa = phys(rd, mode); rf_wd = ldfmt(mdata, eaddr_lo, b_B); end
      fin = 1'b1;
    end
    default: ;
  endcase

  if (fin && fin_jump) begin
    fq_now  = 1'b1;
    fq_addr = fin_tgt[31:2];
  end
end


// ---------------------------------------------------------------------------
// sequential part
// ---------------------------------------------------------------------------
reg data_issue;          // blocking temp: FSM issues a data access this cycle
reg [31:0] di_addr, di_wdata;
reg        di_we, di_byte;

always @(posedge clk) begin
  dbg_retire <= 1'b0;

  if (rst) begin
    st        <= S_RESET;
    m_req     <= 1'b0;
    m_we      <= 1'b0;
    m_byte    <= 1'b0;
    m_code    <= 1'b0;
    pf_valid  <= 1'b0;
    pf_want   <= 1'b0;
    pc        <= 32'd0;
    pcw       <= 1'b0;
    f_n <= 1'b0; f_z <= 1'b0; f_c <= 1'b0; f_v <= 1'b0;
    f_i <= 1'b1; f_f <= 1'b1; mode <= M_SVC;
  end else begin
    data_issue = 1'b0;
    di_addr = 32'd0; di_wdata = 32'd0; di_we = 1'b0; di_byte = 1'b0;

    // ---- memory port: acknowledge -------------------------------------
    if (m_ack) begin
      m_req <= 1'b0;
      if (m_code) begin
        pf_data  <= m_rdata;
        pf_valid <= 1'b1;
      end
    end

    // ---- main state machine --------------------------------------------
    case (st)
      S_RESET: st <= S_FETCH;

      S_FETCH: begin
        if (pf_valid && pf_addr == pc[31:2]) begin
          ir <= pf_data; st <= S_DEC;
        end else if (m_ack && m_code && pf_addr == pc[31:2]) begin
          ir <= m_rdata; st <= S_DEC;
        end
      end

      S_DEC: begin
        pc4_r  <= pc + 32'd4;
        pc8_r  <= pc + 32'd8;
        pc12_r <= pc + 32'd12;
        pcw    <= 1'b0;
        bm_mask <= bm_empty ? 16'h8000 : ir[15:0];
        bm_cnt  <= bm_empty ? 5'd16 : bm_pc_cnt;
        if (cond_pass) begin
          if (c_dp || c_sdt || c_psr || c_mul || c_swp) st <= S_OPS;
          else if (c_bdt)                              st <= S_BM1;
          else if (c_b)                                st <= S_BR;
          else                                         st <= S_EXC;  // SWI / UND
          if (c_psr && !ir[21]) st <= S_MRS;
        end
        // immediate-form shifter controls
        sh_op   <= ctl_op;
        sh_k    <= ctl_k;
        sh_mask <= ctl_mask;
      end

      S_OPS: begin
        if (c_mul) begin
          opB <= (rm == 4'd15) ? pc8_r : rfB;
          opA <= (rs == 4'd15) ? pc8_r : rfA;
          st  <= S_MUL1;
        end else if (c_swp) begin
          opA <= (rn == 4'd15) ? pc8_r : rfA;
          opB <= (rm == 4'd15) ? pc12_r : rfB;
          st  <= S_SW1;
        end else if (c_sdt) begin
          opA <= (rn == 4'd15) ? pc8_r : rfA;
          if (sdt_imm) begin
            shout <= {20'd0, ir[11:0]};
            st    <= S_LADDR;
          end else begin
            opB   <= (rm == 4'd15) ? pc8_r : rfB;
            shout <= (rm == 4'd15) ? pc8_r : rfB;
            st    <= (sh_op == SH_PASS) ? S_LADDR : S_SH;
          end
        end else if (c_psr) begin
          // MSR: the register operand is Rm itself -- bits 11:4 are SBZ and
          // never select a shift (R15 reads as PC+8, as in the reference)
          if (b_I) begin
            opB   <= {24'd0, ir[7:0]};
            shout <= {24'd0, ir[7:0]};
            st    <= (sh_op != SH_PASS) ? S_SH : S_MSR;
          end else begin
            shout <= (rm == 4'd15) ? pc8_r : rfB;
            st    <= S_MSR;
          end
        end else begin
          // data processing
          opA <= (rn == 4'd15) ? (dp_regsh ? pc12_r : pc8_r) : rfA;
          if (b_I) begin
            opB   <= {24'd0, ir[7:0]};
            shout <= {24'd0, ir[7:0]};
            shc   <= f_c;
          end else begin
            opB   <= (rm == 4'd15) ? (dp_regsh ? pc12_r : pc8_r) : rfB;
            shout <= (rm == 4'd15) ? (dp_regsh ? pc12_r : pc8_r) : rfB;
            shc   <= f_c;
          end
          if (dp_regsh)                 st <= S_RS;
          else if (sh_op != SH_PASS)    st <= S_SH;
          else                          st <= S_ALU;
        end
      end

      S_RS: begin
        sh_op   <= ctl_op;
        sh_k    <= ctl_k;
        sh_mask <= ctl_mask;
        st      <= S_SH;
      end

      S_SH: begin
        shout <= sh_y;
        shc   <= sh_c;
        st    <= c_sdt ? S_LADDR : (c_psr ? S_MSR : S_ALU);
      end

      S_ALU: begin
        res   <= alu_y;
        alu_c <= asum[32];
        alu_v <= av;
        st    <= S_WB;
      end

      S_WB: begin
        // S with Rd=15 restores the SPSR; in USR/SYS (no SPSR) the flags are
        // simply updated (reference behaviour; architecturally unpredictable)
        if (b_S && (rd != 4'd15 || !has_spsr)) begin
          f_n <= res[31];
          f_z <= (res == 32'd0);
          if (dp_arith) begin f_c <= alu_c; f_v <= alu_v; end
          else          f_c <= shc;
        end
      end

      S_MRS: ;

      S_MSR: begin
        if (ir[22]) begin
          if (has_spsr) begin
            case (mode)
              M_FIQ: begin if (ir[19]) spsr0[10:7] <= shout[31:28];
                           if (ir[16]) spsr0[6:0] <= {shout[7:6], shout[4:0]}; end
              M_IRQ: begin if (ir[19]) spsr1[10:7] <= shout[31:28];
                           if (ir[16]) spsr1[6:0] <= {shout[7:6], shout[4:0]}; end
              M_SVC: begin if (ir[19]) spsr2[10:7] <= shout[31:28];
                           if (ir[16]) spsr2[6:0] <= {shout[7:6], shout[4:0]}; end
              M_ABT: begin if (ir[19]) spsr3[10:7] <= shout[31:28];
                           if (ir[16]) spsr3[6:0] <= {shout[7:6], shout[4:0]}; end
              default: begin if (ir[19]) spsr4[10:7] <= shout[31:28];
                           if (ir[16]) spsr4[6:0] <= {shout[7:6], shout[4:0]}; end
            endcase
          end
        end else begin
          if (ir[19]) begin
            f_n <= shout[31]; f_z <= shout[30]; f_c <= shout[29]; f_v <= shout[28];
          end
          if (ir[16] && mode != M_USR) begin
            mode <= shout[4:0] | 5'h10;
            f_f  <= shout[6];
            f_i  <= shout[7];
          end
        end
      end

      // ---- single data transfer ----
      S_LADDR: begin
        addr_calc <= b_U ? (opA + shout) : (opA - shout);
        wd        <= (rd == 4'd15) ? pc12_r : rfA;
        st        <= S_LREQ;
      end
      S_LREQ: begin
        if (!m_req) begin
          data_issue = 1'b1;
          di_addr  = b_P ? addr_calc : opA;
          di_we    = !b_L;
          di_byte  = b_B;
          di_wdata = b_B ? {4{wd[7:0]}} : wd;
          eaddr_lo <= b_P ? addr_calc[1:0] : opA[1:0];
          st <= S_LWAIT;
        end
      end
      S_LWAIT: begin
        if (m_ack && !m_code) begin
          mdata <= m_rdata;
          if (b_L)         st <= S_LWB;
          else if (sdt_wb) st <= S_LWB2;
        end
      end
      S_LWB: begin
        if (rd == 4'd15) begin pcw <= 1'b1; pcw_val <= ld_sdt; end
        if (sdt_wb) st <= S_LWB2;
      end
      S_LWB2: ;

      // ---- branch / exception ----
      S_BR: ;
      S_EXC: begin
        case (c_swi ? M_SVC : M_UND)
          M_SVC:   spsr2 <= {f_n, f_z, f_c, f_v, f_i, f_f, mode};
          default: spsr4 <= {f_n, f_z, f_c, f_v, f_i, f_f, mode};
        endcase
        mode <= c_swi ? M_SVC : M_UND;
        f_i  <= 1'b1;
      end

      // ---- block data transfer ----
      S_BM1: begin
        base <= (rn == 4'd15) ? pc12_r : rfA;
        st   <= S_BM2;
      end
      S_BM2: begin
        wbv      <= b_U ? (base + {25'd0, bm_cnt, 2'b00}) : (base - {25'd0, bm_cnt, 2'b00});
        xmode    <= (ir[22] && (!b_L || !bm_haspc)) ? M_USR : mode;   // bit 22 = S (^) for LDM/STM
        bm_first <= 1'b1;
        st       <= S_BM3;
      end
      S_BM3: begin
        bm_addr <= (b_U ? base : wbv) + ((b_U == b_P) ? 32'd4 : 32'd0);
        if (b_L && b_W && rn == 4'd15) begin pcw <= 1'b1; pcw_val <= wbv; end
        st <= S_BMN;
      end
      S_BMN: begin
        if (bm_mask == 16'd0)
          st <= S_BMD;
        else begin
          bm_i    <= bm_low;
          bm_mask <= bm_mask & (bm_mask - 16'd1);
          st      <= b_L ? S_BMQ : S_BMS;
        end
      end
      S_BMS: begin
        if (!m_req) begin
          data_issue = 1'b1;
          di_addr  = bm_addr;
          di_we    = 1'b1;
          di_wdata = (bm_i == 4'd15) ? pc12_r : rfA;
          st <= S_BMW;
        end
      end
      S_BMQ: begin
        if (!m_req) begin
          data_issue = 1'b1;
          di_addr = bm_addr;
          st <= S_BMW;
        end
      end
      S_BMW: begin
        if (m_ack && !m_code) begin
          bm_addr <= bm_addr + 32'd4;
          mdata   <= m_rdata;
          if (b_L) st <= S_BML;
          else begin
            if (bm_first && b_W) begin
              bm_first <= 1'b0;
              if (rn == 4'd15) begin pcw <= 1'b1; pcw_val <= wbv; end
            end
            st <= S_BMN;
          end
        end
      end
      S_BML: begin
        if (bm_first && b_W) begin
          bm_first <= 1'b0;
          if (rn == 4'd15) begin pcw <= 1'b1; pcw_val <= wbv; end
          // stay: next cycle writes the loaded register
        end else begin
          if (bm_i == 4'd15) begin pcw <= 1'b1; pcw_val <= mdata; end
          st <= S_BMN;
        end
      end
      S_BMD: ;

      // ---- multiply ----
      S_MUL1: begin
        p0  <= opB[15:0] * opA[15:0];
        p1  <= opB[31:16] * opA[15:0];
        p2  <= opB[15:0] * opA[31:16];
        acc <= (rd == 4'd15) ? pc8_r : rfA;     // MUL accumulator lives in bits 15:12
        st  <= S_MUL2;
      end
      S_MUL2: begin s12 <= p1 + p2; st <= S_MUL3; end
      S_MUL3: begin
        res <= {p0[31:16] + s12, p0[15:0]};
        st  <= ir[21] ? S_MUL4 : S_MULWB;
      end
      S_MUL4: begin res <= res + acc; st <= S_MULWB; end
      S_MULWB: begin
        if (b_S) begin f_n <= res[31]; f_z <= (res == 32'd0); end
      end

      // ---- swap ----
      S_SW1: begin
        if (!m_req) begin
          data_issue = 1'b1;
          di_addr = opA;
          di_byte = b_B;
          eaddr_lo <= opA[1:0];
          st <= S_SW2;
        end
      end
      S_SW2: if (m_ack && !m_code) begin mdata <= m_rdata; st <= S_SW3; end
      S_SW3: begin
        if (!m_req) begin
          data_issue = 1'b1;
          di_addr  = opA;
          di_we    = 1'b1;
          di_byte  = b_B;
          di_wdata = b_B ? {4{opB[7:0]}} : opB;
          st <= S_SW4;
        end
      end
      S_SW4: if (m_ack && !m_code) st <= S_SWWB;
      S_SWWB: ;

      default: st <= S_FETCH;
    endcase

    // ---- CPSR restore (data processing S with Rd=15, LDM^ with PC) -------
    if (cpsr_restore) begin
      f_n <= spsr_cur[10]; f_z <= spsr_cur[9]; f_c <= spsr_cur[8]; f_v <= spsr_cur[7];
      f_i <= spsr_cur[6];  f_f <= spsr_cur[5]; mode <= spsr_cur[4:0] | 5'h10;
    end

    // ---- instruction completion ------------------------------------------
    if (fin) begin
      dbg_retire <= 1'b1;
      dbg_pc     <= pc;
      dbg_ir     <= ir;
      pcw        <= 1'b0;
      if (fin_jump) begin
        pc <= {fin_tgt[31:2], 2'b00};
        st <= S_FETCH;
      end else if (st == S_DEC) begin
        pc <= pc_plus4;
        st <= S_FETCH;
      end else begin
        pc <= pc4_r;
        if (pf_valid && pf_addr == pc4_r[31:2]) begin
          ir <= pf_data;
          st <= S_DEC;
        end else
          st <= S_FETCH;
      end
    end

    // ---- memory port: issue ----------------------------------------------
    if (data_issue) begin
      m_req   <= 1'b1;
      m_code  <= 1'b0;
      m_addr  <= di_addr;
      m_we    <= di_we;
      m_byte  <= di_byte;
      m_wdata <= di_wdata;
      if (di_we && di_addr[31:2] == pf_addr) pf_valid <= 1'b0;   // store over prefetch
    end else if (!m_req && (fq_now || pf_want)) begin
      m_req   <= 1'b1;
      m_code  <= 1'b1;
      m_we    <= 1'b0;
      m_byte  <= 1'b0;
      m_addr  <= {(fq_now ? fq_addr : pf_want_addr), 2'b00};
      pf_addr <= fq_now ? fq_addr : pf_want_addr;
      pf_valid <= 1'b0;
      pf_want <= 1'b0;
    end else if (fq_now) begin
      pf_want      <= 1'b1;
      pf_want_addr <= fq_addr;
    end
  end
end

endmodule

//////////////////////////////////////////////////////////////////////////////
// 31 x 32 register file, two synchronous read ports, one write port,
// write-first bypass. Two copies of a simple dual-port RAM (one per read
// port), inferred as block RAM: 2 RAMB16 on the mk2 (it saves ~350 LUTs
// there, where LUTs are the scarce resource), 2 M9K on the mk3. The explicit
// bypass makes the block RAM's own read-during-write behaviour irrelevant.
//////////////////////////////////////////////////////////////////////////////
module st018_regfile (
  input             clk,
  input             we,
  input      [4:0]  wa,
  input      [31:0] wd,
  input      [4:0]  ra,
  output     [31:0] rda,
  input      [4:0]  rb,
  output     [31:0] rdb
);
  (* ram_style = "block", ramstyle = "no_rw_check, M9K" *) reg [31:0] mema [0:31];
  (* ram_style = "block", ramstyle = "no_rw_check, M9K" *) reg [31:0] memb [0:31];
  reg [31:0] qa, qb, wdr;
  reg        bya, byb;
  integer i;
  initial begin
    for (i = 0; i < 32; i = i + 1) begin mema[i] = 32'd0; memb[i] = 32'd0; end
    qa = 32'd0; qb = 32'd0; wdr = 32'd0; bya = 1'b0; byb = 1'b0;
  end
  always @(posedge clk) begin
    if (we) begin
      mema[wa] <= wd;
      memb[wa] <= wd;
    end
    qa  <= mema[ra];
    qb  <= memb[rb];
    wdr <= wd;
    bya <= we && (wa == ra);
    byb <= we && (wa == rb);
  end
  assign rda = bya ? wdr : qa;
  assign rdb = byb ? wdr : qb;
endmodule
