## Generated SDC file "main.sdc"

## Copyright (C) 2018  Intel Corporation. All rights reserved.
## Your use of Intel Corporation's design tools, logic functions 
## and other software and tools, and its AMPP partner logic 
## functions, and any output files from any of the foregoing 
## (including device programming or simulation files), and any 
## associated documentation or information are expressly subject 
## to the terms and conditions of the Intel Program License 
## Subscription Agreement, the Intel Quartus Prime License Agreement,
## the Intel FPGA IP License Agreement, or other applicable license
## agreement, including, without limitation, that your use is for
## the sole purpose of programming logic devices manufactured by
## Intel and sold by Intel or its authorized distributors.  Please
## refer to the applicable agreement for further details.


## VENDOR  "Altera"
## PROGRAM "Quartus Prime"
## VERSION "Version 18.0.0 Build 614 04/24/2018 SJ Lite Edition"

## DATE    "Tue Oct 30 12:19:07 2018"

##
## DEVICE  "EP4CE15F17C8"
##


#**************************************************************
# Time Information
#**************************************************************

set_time_format -unit ns -decimal_places 3



#**************************************************************
# Create Clock
#**************************************************************

create_clock -name {CLKIN} -period 125 -waveform { 0.000 62.5 } [get_ports {CLKIN}]
create_clock -name {SPI_SCK} -period 20.833 -waveform { 10.416 20.833 } [get_ports {SPI_SCK}]


#**************************************************************
# Create Generated Clock
#**************************************************************

create_generated_clock -name {snes_pll|altpll_component|auto_generated|pll1|clk[0]} -source [get_pins {snes_pll|altpll_component|auto_generated|pll1|inclk[0]}] -duty_cycle 50/1 -multiply_by 21 -divide_by 2 -master_clock {CLKIN} [get_pins {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] 


#**************************************************************
# Set Clock Latency
#**************************************************************



#**************************************************************
# Set Clock Uncertainty
#**************************************************************

set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {SPI_SCK}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {SPI_SCK}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {SPI_SCK}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {SPI_SCK}]  0.020  


#**************************************************************
# Set Input Delay
#**************************************************************



#**************************************************************
# Set Output Delay
#**************************************************************



#**************************************************************
# Set Clock Groups
#**************************************************************



#**************************************************************
# Set False Path
#**************************************************************

set_false_path  -from  [get_clocks {SPI_SCK}]  -to  [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]


#**************************************************************
# Set Multicycle Path
#**************************************************************

# ============================================================================
# NES core multicycle constraints (sd2snes_nes only -- see nes_wrap.v header)
#
# FOUNDATION (do not touch these numbers without re-deriving BOTH sides):
# every sequential element inside the `NES` hierarchy (nes_wrap:nes_core |
# NES:core) is gated by the single clock-enable `ce` generated by nes_wrap's
# SYSCLK pacer. The pacer guarantees a MINIMUM of 13 CLK2 cycles between
# consecutive ce pulses (derivation in nes_wrap.v's header; measured in
# simulation: min 13 under PSRAM stress, min 15 nominal, avg 15.65). The one
# upstream exception (ApuLookupTable's free-running tmp_a/tmp_b) was ce-gated
# in apu.v specifically so the blanket constraint below is sound.
#
# Chosen values (deliberately far below the 13-cycle floor, leaving >3x
# margin so a future pacer tweak does not silently invalidate timing):
#   - internal NES reg->reg:              setup 4  (budget 47.6ns; the
#     unconstrained critical path measured ~24ns, so 4 both closes timing
#     comfortably and keeps a 2x path-growth allowance), hold 3 (restores the
#     hold check to the launch edge, standard N/N-1 pairing).
#   - boundary paths (see per-rule notes): setup 2 / hold 1, each one only
#     where the FSM structure PROVES a full dead cycle between launch and the
#     first capture-of-consequence.
#
# Register name patterns use entity:instance-tolerant wildcards
# ("*|NES:core|*" = everything inside the NES instance).
# ============================================================================

# --- 1. Internal NES core paths (launch ce edge -> capture ce edge >= 13
#        cycles later; claim only 4) ---
set_multicycle_path -setup 4 -from [get_registers {*|NES:core|*}] -to [get_registers {*|NES:core|*}]
set_multicycle_path -hold  3 -from [get_registers {*|NES:core|*}] -to [get_registers {*|NES:core|*}]

# --- 2. NES -> nes_wrap FSM boundary (need_read/need_write, combinational
#        functions of NES regs, sampled by the wrap FSM only in S_IDLE, which
#        is >= 2 cycles after the ce edge that launched them -- S_SETTLE's
#        transition is unconditional/data-independent, so the capture at the
#        intermediate edge is of no consequence) ---
# NOTE on the patterns: nes_core is a TOP-LEVEL instance, so its own registers
# have no "|"-prefixed parent in the netlist name ("nes_wrap:nes_core|state..."),
# and Quartus may re-encode the FSM (one-hot regs named "state.S_IDLE" etc.) --
# hence "state*", not "state[*]", and no leading "*|". A previous revision used
# "*|nes_wrap:..." and was silently IGNORED (STA Warning 332049 "empty
# collection") -- always check main.sta.rpt for 332049/332174 after touching
# these.
set_multicycle_path -setup 2 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|state*}]
set_multicycle_path -hold  1 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|state*}]
set_multicycle_path -setup 2 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|ROM_BUS_RRQ}]
set_multicycle_path -hold  1 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|ROM_BUS_RRQ}]
set_multicycle_path -setup 2 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|ROM_BUS_WRQ}]
set_multicycle_path -hold  1 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|ROM_BUS_WRQ}]
set_multicycle_path -setup 2 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|req_is_write}]
set_multicycle_path -hold  1 -from [get_registers {*|NES:core|*}] -to [get_registers {nes_wrap:nes_core|req_is_write}]

# --- 3. NES -> main.v arbiter address/data capture (mem_addr/mem_dout change
#        only on a ce edge; NES_ROM_ADDRr/DATAr capture-of-consequence happens
#        when NES_ROM_RRQ/WRQ is observed high, which is >= 3 cycles after
#        that ce edge -- claim 2) ---
set_multicycle_path -setup 2 -from [get_registers {*|NES:core|*}] -to [get_registers {NES_ROM_ADDRr[*]}]
set_multicycle_path -hold  1 -from [get_registers {*|NES:core|*}] -to [get_registers {NES_ROM_ADDRr[*]}]
set_multicycle_path -setup 2 -from [get_registers {*|NES:core|*}] -to [get_registers {NES_ROM_DATAr[*]}]
set_multicycle_path -hold  1 -from [get_registers {*|NES:core|*}] -to [get_registers {NES_ROM_DATAr[*]}]

# --- 4. nes_wrap -> NES read-data return (mem_rdata_cpu_r/_ppu_r latched on the
#        S_WAIT->S_PACE edge; the earliest ce edge that lets the NES capture
#        it is >= 2 cycles later, because S_PACE always spends >= 1 full cycle
#        before ce_pulse_r can rise) ---
set_multicycle_path -setup 2 -from [get_registers {nes_wrap:nes_core|mem_rdata_cpu_r[*]}] -to [get_registers {*|NES:core|*}]
set_multicycle_path -hold  1 -from [get_registers {nes_wrap:nes_core|mem_rdata_cpu_r[*]}] -to [get_registers {*|NES:core|*}]
set_multicycle_path -setup 2 -from [get_registers {nes_wrap:nes_core|mem_rdata_ppu_r[*]}] -to [get_registers {*|NES:core|*}]
set_multicycle_path -hold  1 -from [get_registers {nes_wrap:nes_core|mem_rdata_ppu_r[*]}] -to [get_registers {*|NES:core|*}]

# --- 4b. bridge joypad shift-registers (sr1/sr2) -> NES core: mesma relacao
#     derivada do pacer que os mem_rdata_* acima. Os sr* so mudam no ciclo de
#     CLK2 imediatamente apos um pulso de ce (tapJ_strobe/tapJ_clock sao
#     niveis ce-registrados vindos do core; o edge-detect da bridge dispara
#     1 CLK2 depois), e o core consome joypad_data apenas sob o PROXIMO ce
#     (>=13 CLK2 depois -- piso do pacer, ver header do nes_wrap.v). Mesmo
#     par setup-2/hold-1 dos mem_rdata. (STA pos-janela-CHR: pior caminho
#     sr2[0] -> APU DmcChan|IrqEnable, -0.058ns single-cycle.)
#     Conferir 332049/332174 apos sintese (colecao vazia = ignorado). ---
set_multicycle_path -setup 2 -from [get_registers {nes_wrap:nes_core|nes_bridge:bridge|sr1[*] nes_wrap:nes_core|nes_bridge:bridge|sr2[*]}] -to [get_registers {*|NES:core|*}]
set_multicycle_path -hold  1 -from [get_registers {nes_wrap:nes_core|nes_bridge:bridge|sr1[*] nes_wrap:nes_core|nes_bridge:bridge|sr2[*]}] -to [get_registers {*|NES:core|*}]

# --- 5. mcu_cmd nes_feat_out (CHIPFEAT 0xef) -> NES core: quasi-static
#        configuration. This register now carries the FULL mapper_flags[15:0]
#        word (Fase 0 replaced the old MAPPER_BUF placeholder wiring -- see
#        main.v's nes_wrap instantiation). Same rationale as the original
#        MAPPER_BUF false path it supersedes: STA showed the config->
#        MultiMapper decode -> prg_allow -> from_data_bus -> APU/CPU register
#        D-input cone as the sole failing group (~-1.2ns at 85C), and the word
#        is written only by the MCU during game load (src/memory.c writes
#        CHIPFEAT before assert/deassert_reset), while the NES core is held in
#        or before its reset (SNES_reset_strobe) -- stable for the core's
#        entire runtime, so no synchronous timing relationship to the core's
#        ce-gated captures exists or is needed. OPERATIONAL CONTRACT this
#        false path encodes: firmware must never re-program CHIPFEAT while the
#        NES core is running. Scoped -to the core only, so any other consumer
#        of nes_feat_out stays fully timed.
#        NOTE pos-STA (invariante do projeto): apos re-sintese, conferir
#        no main.sta.rpt que NAO ha Warning 332049/332174 apontando esta linha
#        (colecao vazia = false path silenciosamente ignorado). ---
set_false_path -from [get_registers {mcu_cmd:snes_mcu_cmd|nes_feat_out[*]}] -to [get_registers {*|NES:core|*}]
#        Extensao (Fase 1, janela CHR): a re-estruturacao do arbiter expos o
#        MESMO cone quase-estatico terminando em NES_ROM_DATAr[*] (registrador
#        do main.v que so captura dados PARA o core em execucao -- fora do
#        pattern {*|NES:core|*} acima). Mesma justificativa/contrato
#        operacional: nes_feat_out e estavel sempre que NES_ROM_DATAr importa.
#        (STA pos-janela-CHR: 5 paths -0.296ns, todos nes_feat_out ->
#        NES_ROM_DATAr[7].) Conferir 332049/332174 apos sintese, como acima.
set_false_path -from [get_registers {mcu_cmd:snes_mcu_cmd|nes_feat_out[*]}] -to [get_registers {NES_ROM_DATAr[*]}]

# NOT constrained (kept full-speed on purpose):
#   - ce_pulse_r -> NES (the clock-enable fan-out itself: consumed at the very
#     next edge, genuinely single-cycle);
#   - the wrap FSM and pacer registers among themselves;
#   - the main.v arbiter FSM (full-speed by design, CONTRACT SS3.2);
#   - breadcrumb bc_r captures (shallow mux paths, meet timing at full speed).


#**************************************************************
# Set Maximum Delay
#**************************************************************



#**************************************************************
# Set Minimum Delay
#**************************************************************



#**************************************************************
# Set Input Transition
#**************************************************************

