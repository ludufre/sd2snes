// SPDX-License-Identifier: GPL-2.0-or-later
//
// m6507.v -- MOS 6507 (6502 core, 13 address pins) written from scratch
//            for the sd2snes Mk.II (Spartan-3 XC3S400).  Milestone 1.
//
// BUS MODEL (read this first)
//   ab / we / dout describe the bus cycle that is CURRENTLY presented.
//   That cycle completes on the next clock edge where ce = 1:
//     - if we = 0, din (valid at that edge) is the byte read from ab
//     - if we = 1, dout is the byte written to ab
//   At that same edge the core computes the bus cycle for the NEXT cycle.
//   So one ce pulse == one 6502 cycle.  Withholding ce stalls the CPU
//   (that is how WSYNC / RDY is done outside), and the outside world has
//   the whole interval between two ce pulses to answer a read.
//   Memory-mapped strobes in the shell should be:   rd = ce & ~we,
//   wr = ce & we  (dummy reads and double writes occur exactly as on a
//   real 6502).
//
// SCOPE
//   All 151 documented opcodes, binary and decimal (NMOS) ADC/SBC.
//   No IRQ/NMI (the 6507 has no such pins).  BRK works.
//   Undocumented opcodes (Passo 29, after the collection test): SLO RLA SRE RRA SAX LAX DCP ISB,
//   ANC ALR ARR SBX XAA LXA, SBC #$EB, and the 2/3-byte NOPs with their documented lengths and timing.
//   RRA/ISB use the binary adder (decimal mode not modelled for them).  JAM opcodes run as 1-byte NOPs.
//   SHA ($93 $9F), SHX ($9E), SHY ($9C), TAS ($9B) store reg & (H+1) (H = high byte of the base address; the
//   page-crossing address corruption is not modelled); LAS ($BB): A = X = S = M & S.
//   (checked against the ALE with a test ROM: values and cycle counts).
//   Cycle counts follow the documented table (page-cross penalty on
//   indexed reads, branch +1 / +2, RMW dummy write, etc.).
//
// STATUS: untested when written.  Verify with tb_m6507_cycles.v and
//         tb_m6507_func.v before trusting anything.
//
`timescale 1ns / 1ps

module m6507 (
  input             clk,
  input             rst,     // synchronous, active high
  input             ce,      // one pulse per CPU cycle
  input      [7:0]  din,
  output reg [15:0] ab,
  output reg        we,
  output reg [7:0]  dout,
  output            sync     // 1 while the presented cycle is an opcode fetch
);

  // ------------------------------------------------------------------
  // constants
  // ------------------------------------------------------------------
  localparam [5:0]
    ST_RES0   = 6'd0,  ST_RES1   = 6'd1,  ST_FETCH  = 6'd2,  ST_OP1    = 6'd3,
    ST_ZPDUM  = 6'd4,  ST_IND_LO = 6'd5,  ST_IND_HI = 6'd6,  ST_ABS_HI = 6'd7,
    ST_FIX    = 6'd8,  ST_EA     = 6'd9,  ST_RMW_D  = 6'd10, ST_RMW_W  = 6'd11,
    ST_BR1    = 6'd12, ST_BR2    = 6'd13, ST_PUSH   = 6'd14, ST_PL_D   = 6'd15,
    ST_PL_R   = 6'd16, ST_JSR_D  = 6'd17, ST_JSR_PH = 6'd18, ST_JSR_PL = 6'd19,
    ST_JSR_HI = 6'd20, ST_RTS_D  = 6'd21, ST_RTS_L  = 6'd22, ST_RTS_H  = 6'd23,
    ST_RTS_I  = 6'd24, ST_RTI_D  = 6'd25, ST_RTI_P  = 6'd26, ST_RTI_L  = 6'd27,
    ST_RTI_H  = 6'd28, ST_BRK1   = 6'd29, ST_BRK2   = 6'd30, ST_BRK3   = 6'd31,
    ST_BRK4   = 6'd32, ST_BRK5   = 6'd33, ST_JI_LO  = 6'd34, ST_JI_HI  = 6'd35;

  localparam [3:0]
    M_IMP = 4'd0, M_IMM = 4'd1, M_ZP  = 4'd2, M_ZPX  = 4'd3, M_ZPY  = 4'd4,
    M_ABS = 4'd5, M_ABSX= 4'd6, M_ABSY= 4'd7, M_INDX = 4'd8, M_INDY = 4'd9;

  localparam [1:0] K_RD = 2'd0, K_WR = 2'd1, K_RMW = 2'd2;

  // ------------------------------------------------------------------
  // state
  // ------------------------------------------------------------------
  reg [7:0]  A, X, Y, S, IR, ADL, ADH, PLO, tmp, bh;
  reg [15:0] PC;
  reg        fC, fZ, fI, fD, fV, fN;
  reg        cx, br_cross;
  reg [5:0]  st;

  assign sync = (st == ST_FETCH);

  wire [15:0] pc1 = PC + 16'd1;

  // ------------------------------------------------------------------
  // opcode decode  (aaa bbb cc)
  // ------------------------------------------------------------------
  wire [2:0] aaa = IR[7:5];
  wire [2:0] bbb = IR[4:2];
  wire [1:0] cc  = IR[1:0];

  wire is_br = (IR[4:0] == 5'b10000);
  wire is_special = (IR == 8'h00) | (IR == 8'h20) | (IR == 8'h40) | (IR == 8'h60) |
                    (IR == 8'h08) | (IR == 8'h28) | (IR == 8'h48) | (IR == 8'h68) |
                    (IR == 8'h4C) | (IR == 8'h6C);

  reg [3:0] mode;
  reg [1:0] kind;
  always @* begin
    mode = M_IMP;
    kind = K_RD;
    case (cc)
      2'b01: begin
        kind = (aaa == 3'd4 && bbb != 3'd2) ? K_WR : K_RD;
        case (bbb)
          3'd0: mode = M_INDX;
          3'd1: mode = M_ZP;
          3'd2: mode = M_IMM;                                          // $89: NOP #imm
          3'd3: mode = M_ABS;
          3'd4: mode = M_INDY;
          3'd5: mode = M_ZPX;
          3'd6: mode = M_ABSY;
          default: mode = M_ABSX;
        endcase
      end
      2'b10: begin
        kind = (aaa == 3'd4) ? K_WR : (aaa == 3'd5) ? K_RD : K_RMW;
        case (bbb)
          3'd0: mode = (aaa >= 3'd4) ? M_IMM : M_IMP;                  // $82 $C2 $E2: NOP #imm
          3'd1: mode = M_ZP;
          3'd3: mode = M_ABS;
          3'd5: mode = (aaa == 3'd4 || aaa == 3'd5) ? M_ZPY : M_ZPX;
          3'd7: mode = (aaa == 3'd5 || aaa == 3'd4) ? M_ABSY : M_ABSX;   // $9E SHX: abs,Y
          default: mode = M_IMP;      // 2: accumulator/implied, 4 and 6: TXS/TSX/JAM
        endcase
      end
      2'b00: begin
        kind = (aaa == 3'd4) ? K_WR : K_RD;
        case (bbb)
          3'd0: mode = (aaa >= 3'd4) ? M_IMM : M_IMP;                  // $80: NOP #imm
          3'd1: mode = M_ZP;
          3'd3: mode = M_ABS;
          3'd5: mode = M_ZPX;                                          // $14 $34 ..: NOP zp,X
          3'd7: mode = M_ABSX;                                         // $1C $3C ..: NOP abs,X
          default: mode = M_IMP;
        endcase
      end
      default: begin                  // cc=11: undocumented, addressing of the cc=01 column
        kind = (aaa == 3'd4) ? K_WR : (aaa == 3'd5 || bbb == 3'd2) ? K_RD : K_RMW;
        case (bbb)
          3'd0: mode = M_INDX;
          3'd1: mode = M_ZP;
          3'd2: mode = M_IMM;
          3'd3: mode = M_ABS;
          3'd4: mode = M_INDY;
          3'd5: mode = (aaa == 3'd4 || aaa == 3'd5) ? M_ZPY : M_ZPX;
          3'd6: mode = M_ABSY;
          default: mode = (aaa == 3'd4 || aaa == 3'd5) ? M_ABSY : M_ABSX;
        endcase
      end
    endcase
  end

  wire [7:0] idx = (mode == M_ZPX || mode == M_ABSX || mode == M_INDX) ? X : Y;
  wire [7:0] stv = (cc == 2'b01) ? A : (cc == 2'b10) ? X : (cc == 2'b11) ? (A & X) : Y;

  wire [7:0] zpsum  = ADL + idx;
  wire [8:0] absum  = {1'b0, ADL} + {1'b0, idx};
  wire [8:0] indsum = {1'b0, PLO} + {1'b0, Y};

  // branch condition: IR[7:6] picks the flag, IR[5] the value branched on
  reg cond;
  always @* begin
    case (IR[7:6])
      2'b00:   cond = (fN == IR[5]);
      2'b01:   cond = (fV == IR[5]);
      2'b10:   cond = (fC == IR[5]);
      default: cond = (fZ == IR[5]);
    endcase
  end
  wire [15:0] btgt = pc1 + {{8{din[7]}}, din};

  // ------------------------------------------------------------------
  // ALU
  // ------------------------------------------------------------------
  // binary ADC
  wire [8:0] adc9  = {1'b0, A} + {1'b0, din} + {8'b0, fC};
  wire       adc_v = ~(A[7] ^ din[7]) & (A[7] ^ adc9[7]);
  // decimal ADC (NMOS: Z from binary sum, N/V from intermediate, C decimal)
  wire [4:0] al0   = {1'b0, A[3:0]} + {1'b0, din[3:0]} + {4'b0, fC};
  wire [4:0] al    = (al0 > 5'd9) ? {1'b1, (al0[3:0] + 4'd6)} : al0;
  wire [9:0] dt0   = {2'b0, A[7:4], 4'b0} + {2'b0, din[7:4], 4'b0} + {5'b0, al};
  wire [9:0] dt1   = (dt0 >= 10'h0A0) ? (dt0 + 10'h060) : dt0;
  wire       adc_dv = ~(A[7] ^ din[7]) & (A[7] ^ dt0[7]);

  // binary SBC (all flags come from the binary result, also in decimal mode)
  wire [8:0] sbc9  = {1'b0, A} + {1'b0, ~din} + {8'b0, fC};
  wire       sbc_v = (A[7] ^ din[7]) & (A[7] ^ sbc9[7]);
  // decimal SBC accumulator result
  wire [9:0] sl0   = {6'b0, A[3:0]} - {6'b0, din[3:0]} + {9'b0, fC} - 10'd1;
  wire [9:0] sl1   = sl0[9] ? ({6'b0, (sl0[3:0] - 4'd6)} - 10'h010) : sl0;
  wire [9:0] sd0   = {2'b0, A[7:4], 4'b0} - {2'b0, din[7:4], 4'b0} + sl1;
  wire [9:0] sd1   = sd0[9] ? (sd0 - 10'h060) : sd0;

  // compares
  wire [8:0] cmpa9 = {1'b0, A} - {1'b0, din};
  wire [8:0] cmpx9 = {1'b0, X} - {1'b0, din};
  wire [8:0] cmpy9 = {1'b0, Y} - {1'b0, din};

  // shifter / inc / dec (accumulator form at OP1, memory form at RMW_D)
  wire [7:0] shin = (st == ST_RMW_D) ? tmp : A;
  reg  [7:0] sh_res;
  reg        sh_c;
  always @* begin
    case (aaa)
      3'd0:    begin sh_res = {shin[6:0], 1'b0}; sh_c = shin[7]; end   // ASL
      3'd1:    begin sh_res = {shin[6:0], fC};   sh_c = shin[7]; end   // ROL
      3'd2:    begin sh_res = {1'b0, shin[7:1]}; sh_c = shin[0]; end   // LSR
      3'd3:    begin sh_res = {fC, shin[7:1]};   sh_c = shin[0]; end   // ROR
      3'd6:    begin sh_res = shin - 8'd1;       sh_c = fC;      end   // DEC
      default: begin sh_res = shin + 8'd1;       sh_c = fC;      end   // INC
    endcase
  end

  // undocumented RMW combos: the second operation takes the new memory value r = sh_res
  wire [8:0] rra9  = {1'b0, A} + {1'b0, sh_res} + {8'b0, sh_c};
  wire       rra_v = ~(A[7] ^ sh_res[7]) & (A[7] ^ rra9[7]);
  wire [8:0] isb9  = {1'b0, A} + {1'b0, ~sh_res} + {8'b0, fC};
  wire       isb_v = (A[7] ^ sh_res[7]) & (A[7] ^ isb9[7]);
  wire [8:0] dcp9  = {1'b0, A} - {1'b0, sh_res};
  // immediate combos
  wire [7:0] anda  = A & din;
  wire [7:0] arr_r = {fC, anda[7:1]};
  wire [8:0] sbx9  = {1'b0, A & X} - {1'b0, din};

  // ------------------------------------------------------------------
  // tasks (all called from the single clocked block below)
  // ------------------------------------------------------------------
  task nz;
    input [7:0] r;
    begin
      fN <= r[7];
      fZ <= (r == 8'h00);
    end
  endtask

  task loadp;                       // PLP / RTI: bits 5 and 4 do not exist
    input [7:0] p;
    begin
      fN <= p[7]; fV <= p[6]; fD <= p[3]; fI <= p[2]; fZ <= p[1]; fC <= p[0];
    end
  endtask

  // memory-read operations (immediate and all read addressing modes)
  task exec_rd;
    begin
      case (cc)
        2'b01: begin
          case (aaa)
            3'd0: begin A <= A | din; nz(A | din); end                 // ORA
            3'd1: begin A <= A & din; nz(A & din); end                 // AND
            3'd2: begin A <= A ^ din; nz(A ^ din); end                 // EOR
            3'd3: begin                                                // ADC
              if (fD) begin
                A <= dt1[7:0]; fC <= dt1[9] | dt1[8];
                fN <= dt0[7];  fV <= adc_dv;
              end else begin
                A <= adc9[7:0]; fC <= adc9[8];
                fN <= adc9[7];  fV <= adc_v;
              end
              fZ <= (adc9[7:0] == 8'h00);
            end
            3'd5: begin A <= din; nz(din); end                         // LDA
            3'd6: begin fC <= ~cmpa9[8]; nz(cmpa9[7:0]); end           // CMP
            3'd7: begin                                                // SBC
              A  <= fD ? sd1[7:0] : sbc9[7:0];
              fC <= sbc9[8]; fV <= sbc_v;
              fN <= sbc9[7]; fZ <= (sbc9[7:0] == 8'h00);
            end
            default: ;
          endcase
        end
        2'b10: begin
          if (aaa == 3'd5) begin X <= din; nz(din); end                // LDX
        end
        2'b00: begin
          case (aaa)
            3'd1: if (bbb == 3'd1 || bbb == 3'd3) begin fZ <= ((A & din) == 8'h00); fN <= din[7]; fV <= din[6]; end  // BIT
            3'd5: begin Y <= din; nz(din); end                         // LDY
            3'd6: if (bbb == 3'd0 || bbb == 3'd1 || bbb == 3'd3) begin fC <= ~cmpy9[8]; nz(cmpy9[7:0]); end   // CPY
            3'd7: if (bbb == 3'd0 || bbb == 3'd1 || bbb == 3'd3) begin fC <= ~cmpx9[8]; nz(cmpx9[7:0]); end   // CPX
            default: ;
          endcase
        end
        default: begin                                                 // cc=11
          if (aaa == 3'd5) begin                                       // LAX / LXA / LAS
            if (bbb == 3'd2) begin A <= (A | 8'hEE) & din; X <= (A | 8'hEE) & din; nz((A | 8'hEE) & din); end
            else if (bbb == 3'd6) begin A <= din & S; X <= din & S; S <= din & S; nz(din & S); end   // $BB LAS
            else begin A <= din; X <= din; nz(din); end
          end else if (bbb == 3'd2) begin
            case (aaa)
              3'd0, 3'd1: begin A <= anda; nz(anda); fC <= anda[7]; end                    // ANC
              3'd2: begin A <= {1'b0, anda[7:1]}; nz({1'b0, anda[7:1]}); fC <= anda[0]; end // ALR
              3'd3: begin A <= arr_r; nz(arr_r); fC <= arr_r[6]; fV <= arr_r[6] ^ arr_r[5]; end // ARR (binary)
              3'd4: begin A <= (A | 8'hEE) & X & din; nz((A | 8'hEE) & X & din); end       // XAA (unstable)
              3'd6: begin X <= sbx9[7:0]; fC <= ~sbx9[8]; nz(sbx9[7:0]); end               // SBX
              default: begin                                                              // $EB = SBC #
                A  <= fD ? sd1[7:0] : sbc9[7:0];
                fC <= sbc9[8]; fV <= sbc_v; fN <= sbc9[7]; fZ <= (sbc9[7:0] == 8'h00);
              end
            endcase
          end
        end
      endcase
    end
  endtask

  // implied / accumulator operations
  task exec_imp;
    begin
      case (IR)
        8'h0A, 8'h2A, 8'h4A, 8'h6A: begin A <= sh_res; fC <= sh_c; nz(sh_res); end
        8'hAA: begin X <= A; nz(A); end                                // TAX
        8'h8A: begin A <= X; nz(X); end                                // TXA
        8'hA8: begin Y <= A; nz(A); end                                // TAY
        8'h98: begin A <= Y; nz(Y); end                                // TYA
        8'hBA: begin X <= S; nz(S); end                                // TSX
        8'h9A: S <= X;                                                 // TXS
        8'hCA: begin X <= X - 8'd1; nz(X - 8'd1); end                  // DEX
        8'hE8: begin X <= X + 8'd1; nz(X + 8'd1); end                  // INX
        8'h88: begin Y <= Y - 8'd1; nz(Y - 8'd1); end                  // DEY
        8'hC8: begin Y <= Y + 8'd1; nz(Y + 8'd1); end                  // INY
        8'h18: fC <= 1'b0;                                             // CLC
        8'h38: fC <= 1'b1;                                             // SEC
        8'h58: fI <= 1'b0;                                             // CLI
        8'h78: fI <= 1'b1;                                             // SEI
        8'hB8: fV <= 1'b0;                                             // CLV
        8'hD8: fD <= 1'b0;                                             // CLD
        8'hF8: fD <= 1'b1;                                             // SED
        default: ;                                                     // NOP / undocumented
      endcase
    end
  endtask

  // exec strobes (evaluated only inside the ce branch)
  wire do_imm = (st == ST_OP1) & ~is_special & ~is_br & (mode == M_IMM);
  wire do_imp = (st == ST_OP1) & ~is_special & ~is_br & (mode == M_IMP);
  wire do_rdx = ((st == ST_FIX) & (kind == K_RD) & ~cx) |
                ((st == ST_EA)  & (kind == K_RD));

  // ------------------------------------------------------------------
  // main sequencer
  // ------------------------------------------------------------------
  always @(posedge clk) begin
    if (rst) begin
      st <= ST_RES0; ab <= 16'hFFFC; we <= 1'b0; dout <= 8'h00;
      A <= 8'h00; X <= 8'h00; Y <= 8'h00; S <= 8'hFD; PC <= 16'h0000;
      IR <= 8'hEA; ADL <= 8'h00; ADH <= 8'h00; PLO <= 8'h00; tmp <= 8'h00; bh <= 8'h00;
      fC <= 1'b0; fZ <= 1'b0; fI <= 1'b1; fD <= 1'b0; fV <= 1'b0; fN <= 1'b0;
      cx <= 1'b0; br_cross <= 1'b0;
    end else if (ce) begin
      case (st)
        // ---- reset vector -------------------------------------------
        ST_RES0: begin ADL <= din; ab <= 16'hFFFD; st <= ST_RES1; end
        ST_RES1: begin PC <= {din, ADL}; ab <= {din, ADL}; st <= ST_FETCH; end

        // ---- opcode fetch / first operand ---------------------------
        ST_FETCH: begin IR <= din; PC <= pc1; ab <= pc1; st <= ST_OP1; end

        ST_OP1: begin
          if (is_special) begin
            case (IR)
              8'h00: begin                                   // BRK
                PC <= pc1; ab <= {8'h01, S}; we <= 1'b1; dout <= pc1[15:8];
                S <= S - 8'd1; st <= ST_BRK1;
              end
              8'h20: begin                                   // JSR
                ADL <= din; PC <= pc1; ab <= {8'h01, S}; st <= ST_JSR_D;
              end
              8'h40: begin ab <= {8'h01, S}; st <= ST_RTI_D; end   // RTI
              8'h60: begin ab <= {8'h01, S}; st <= ST_RTS_D; end   // RTS
              8'h08: begin                                   // PHP
                ab <= {8'h01, S}; we <= 1'b1; dout <= {fN, fV, 2'b11, fD, fI, fZ, fC};
                S <= S - 8'd1; st <= ST_PUSH;
              end
              8'h48: begin                                   // PHA
                ab <= {8'h01, S}; we <= 1'b1; dout <= A;
                S <= S - 8'd1; st <= ST_PUSH;
              end
              8'h28, 8'h68: begin ab <= {8'h01, S}; st <= ST_PL_D; end  // PLP, PLA
              8'h4C, 8'h6C: begin                            // JMP abs / (ind)
                ADL <= din; PC <= pc1; ab <= pc1; st <= ST_ABS_HI;
              end
              default: begin ab <= PC; st <= ST_FETCH; end
            endcase
          end else if (is_br) begin
            if (cond) begin
              PC <= btgt; bh <= pc1[15:8];
              br_cross <= (btgt[15:8] != pc1[15:8]);
              ab <= pc1; st <= ST_BR1;
            end else begin
              PC <= pc1; ab <= pc1; st <= ST_FETCH;
            end
          end else begin
            case (mode)
              M_IMM: begin PC <= pc1; ab <= pc1; st <= ST_FETCH; end
              M_ZP: begin
                PC <= pc1; ab <= {8'h00, din};
                we <= (kind == K_WR); dout <= stv; st <= ST_EA;
              end
              M_ZPX, M_ZPY, M_INDX: begin
                ADL <= din; PC <= pc1; ab <= {8'h00, din}; st <= ST_ZPDUM;
              end
              M_ABS, M_ABSX, M_ABSY: begin
                ADL <= din; PC <= pc1; ab <= pc1; st <= ST_ABS_HI;
              end
              M_INDY: begin
                ADL <= din; PC <= pc1; ab <= {8'h00, din}; st <= ST_IND_LO;
              end
              default: begin ab <= PC; st <= ST_FETCH; end     // M_IMP
            endcase
          end
        end

        // ---- addressing-mode cycles ---------------------------------
        ST_ZPDUM: begin
          if (mode == M_INDX) begin
            ADL <= zpsum; ab <= {8'h00, zpsum}; st <= ST_IND_LO;
          end else begin
            ab <= {8'h00, zpsum};
            we <= (kind == K_WR); dout <= stv; st <= ST_EA;
          end
        end

        ST_IND_LO: begin PLO <= din; ab <= {8'h00, ADL + 8'd1}; st <= ST_IND_HI; end

        ST_IND_HI: begin
          if (mode == M_INDX) begin
            ab <= {din, PLO};
            we <= (kind == K_WR); dout <= stv; st <= ST_EA;
          end else begin                                       // (zp),Y
            cx <= indsum[8]; ADL <= indsum[7:0]; ADH <= din;
            ab <= {din, indsum[7:0]}; st <= ST_FIX;
          end
        end

        ST_ABS_HI: begin
          if (IR == 8'h4C) begin                               // JMP abs
            PC <= {din, ADL}; ab <= {din, ADL}; st <= ST_FETCH;
          end else if (IR == 8'h6C) begin                      // JMP (ind)
            ADH <= din; ab <= {din, ADL}; st <= ST_JI_LO;
          end else if (mode == M_ABSX || mode == M_ABSY) begin
            cx <= absum[8]; ADL <= absum[7:0]; ADH <= din; PC <= pc1;
            ab <= {din, absum[7:0]}; st <= ST_FIX;
          end else begin                                       // absolute
            PC <= pc1; ab <= {din, ADL};
            we <= (kind == K_WR); dout <= stv; st <= ST_EA;
          end
        end

        ST_JI_LO: begin PLO <= din; ab <= {ADH, ADL + 8'd1}; st <= ST_JI_HI; end
        ST_JI_HI: begin PC <= {din, PLO}; ab <= {din, PLO}; st <= ST_FETCH; end

        // read at a possibly-uncorrected indexed address
        ST_FIX: begin
          if (kind == K_RD && !cx) begin
            ab <= PC; st <= ST_FETCH;                          // data already read
          end else begin
            ab <= {ADH + {7'b0, cx}, ADL};
            we <= (kind == K_WR); st <= ST_EA;
            case (IR)
              8'h93, 8'h9F: dout <= A & X & (ADH + 8'd1);      // SHA
              8'h9E:        dout <= X & (ADH + 8'd1);          // SHX
              8'h9C:        dout <= Y & (ADH + 8'd1);          // SHY
              8'h9B: begin  dout <= A & X & (ADH + 8'd1); S <= A & X; end   // TAS
              default:      dout <= stv;
            endcase
          end
        end

        // ---- the final memory access --------------------------------
        ST_EA: begin
          case (kind)
            K_RD:  begin ab <= PC; st <= ST_FETCH; end
            K_WR:  begin we <= 1'b0; ab <= PC; st <= ST_FETCH; end
            default: begin tmp <= din; dout <= din; we <= 1'b1; st <= ST_RMW_D; end
          endcase
        end

        ST_RMW_D: begin                      // old value is being written back
          dout <= sh_res; fC <= sh_c; nz(sh_res); st <= ST_RMW_W;
          if (cc == 2'b11) begin
            case (aaa)
              3'd0: begin A <= A | sh_res; nz(A | sh_res); end                 // SLO
              3'd1: begin A <= A & sh_res; nz(A & sh_res); end                 // RLA
              3'd2: begin A <= A ^ sh_res; nz(A ^ sh_res); end                 // SRE
              3'd3: begin A <= rra9[7:0]; fC <= rra9[8]; fV <= rra_v; nz(rra9[7:0]); end   // RRA
              3'd6: begin fC <= ~dcp9[8]; nz(dcp9[7:0]); end                   // DCP
              default: begin A <= isb9[7:0]; fC <= isb9[8]; fV <= isb_v; nz(isb9[7:0]); end // ISB
            endcase
          end
        end
        ST_RMW_W: begin we <= 1'b0; ab <= PC; st <= ST_FETCH; end

        // ---- branches -----------------------------------------------
        ST_BR1: begin
          if (br_cross) begin ab <= {bh, PC[7:0]}; st <= ST_BR2; end
          else          begin ab <= PC; st <= ST_FETCH; end
        end
        ST_BR2: begin ab <= PC; st <= ST_FETCH; end

        // ---- stack: PHA/PHP/PLA/PLP ---------------------------------
        ST_PUSH: begin we <= 1'b0; ab <= PC; st <= ST_FETCH; end
        ST_PL_D: begin S <= S + 8'd1; ab <= {8'h01, S + 8'd1}; st <= ST_PL_R; end
        ST_PL_R: begin
          if (IR == 8'h68) begin A <= din; nz(din); end
          else loadp(din);
          ab <= PC; st <= ST_FETCH;
        end

        // ---- JSR ------------------------------------------------------
        ST_JSR_D:  begin ab <= {8'h01, S}; we <= 1'b1; dout <= PC[15:8];
                         S <= S - 8'd1; st <= ST_JSR_PH; end
        ST_JSR_PH: begin ab <= {8'h01, S}; dout <= PC[7:0];
                         S <= S - 8'd1; st <= ST_JSR_PL; end
        ST_JSR_PL: begin we <= 1'b0; ab <= PC; st <= ST_JSR_HI; end
        ST_JSR_HI: begin PC <= {din, ADL}; ab <= {din, ADL}; st <= ST_FETCH; end

        // ---- RTS ------------------------------------------------------
        ST_RTS_D: begin S <= S + 8'd1; ab <= {8'h01, S + 8'd1}; st <= ST_RTS_L; end
        ST_RTS_L: begin ADL <= din; S <= S + 8'd1; ab <= {8'h01, S + 8'd1}; st <= ST_RTS_H; end
        ST_RTS_H: begin PC <= {din, ADL}; ab <= {din, ADL}; st <= ST_RTS_I; end
        ST_RTS_I: begin PC <= pc1; ab <= pc1; st <= ST_FETCH; end

        // ---- RTI ------------------------------------------------------
        ST_RTI_D: begin S <= S + 8'd1; ab <= {8'h01, S + 8'd1}; st <= ST_RTI_P; end
        ST_RTI_P: begin loadp(din); S <= S + 8'd1; ab <= {8'h01, S + 8'd1}; st <= ST_RTI_L; end
        ST_RTI_L: begin ADL <= din; S <= S + 8'd1; ab <= {8'h01, S + 8'd1}; st <= ST_RTI_H; end
        ST_RTI_H: begin PC <= {din, ADL}; ab <= {din, ADL}; st <= ST_FETCH; end

        // ---- BRK ------------------------------------------------------
        ST_BRK1: begin ab <= {8'h01, S}; dout <= PC[7:0]; S <= S - 8'd1; st <= ST_BRK2; end
        ST_BRK2: begin ab <= {8'h01, S}; dout <= {fN, fV, 2'b11, fD, fI, fZ, fC};
                       S <= S - 8'd1; st <= ST_BRK3; end
        ST_BRK3: begin we <= 1'b0; ab <= 16'hFFFE; fI <= 1'b1; st <= ST_BRK4; end
        ST_BRK4: begin ADL <= din; ab <= 16'hFFFF; st <= ST_BRK5; end
        ST_BRK5: begin PC <= {din, ADL}; ab <= {din, ADL}; st <= ST_FETCH; end

        default: begin ab <= PC; we <= 1'b0; st <= ST_FETCH; end
      endcase

      // register updates for read/implied instructions (one copy of the ALU
      // muxing instead of one per call site)
      if (do_imm | do_rdx) exec_rd;
      if (do_imp)          exec_imp;
    end
  end

endmodule
