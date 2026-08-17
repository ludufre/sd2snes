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

## DATE    "Fri Jul 27 00:34:51 2018"

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
create_clock -name {SPI_SCK} -period 20.833 -waveform { 0.000 10.417 } [get_ports { SPI_SCK }]


#**************************************************************
# Create Generated Clock
#**************************************************************

create_generated_clock -name {snes_pll|altpll_component|auto_generated|pll1|clk[0]} -source [get_pins {snes_pll|altpll_component|auto_generated|pll1|inclk[0]}] -duty_cycle 50/1 -multiply_by 12 -master_clock {CLKIN} [get_pins {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] 


#**************************************************************
# Set Clock Latency
#**************************************************************



#**************************************************************
# Set Clock Uncertainty
#**************************************************************

set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {CLKIN}] -setup 0.100  
set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {CLKIN}] -hold 0.070  
set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {CLKIN}] -setup 0.100  
set_clock_uncertainty -rise_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {CLKIN}] -hold 0.070  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {CLKIN}] -setup 0.100  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -rise_to [get_clocks {CLKIN}] -hold 0.070  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {CLKIN}] -setup 0.100  
set_clock_uncertainty -fall_from [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -fall_to [get_clocks {CLKIN}] -hold 0.070  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.080  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.110  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.080  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.110  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {SPI_SCK}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {SPI_SCK}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.080  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.110  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.080  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.110  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -rise_to [get_clocks {SPI_SCK}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {SPI_SCK}] -fall_to [get_clocks {SPI_SCK}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {CLKIN}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -rise_from [get_clocks {CLKIN}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -rise_from [get_clocks {CLKIN}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -rise_from [get_clocks {CLKIN}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -rise_from [get_clocks {CLKIN}] -rise_to [get_clocks {CLKIN}]  0.020  
set_clock_uncertainty -rise_from [get_clocks {CLKIN}] -fall_to [get_clocks {CLKIN}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {CLKIN}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -fall_from [get_clocks {CLKIN}] -rise_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -fall_from [get_clocks {CLKIN}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -setup 0.070  
set_clock_uncertainty -fall_from [get_clocks {CLKIN}] -fall_to [get_clocks {snes_pll|altpll_component|auto_generated|pll1|clk[0]}] -hold 0.100  
set_clock_uncertainty -fall_from [get_clocks {CLKIN}] -rise_to [get_clocks {CLKIN}]  0.020  
set_clock_uncertainty -fall_from [get_clocks {CLKIN}] -fall_to [get_clocks {CLKIN}]  0.020  


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

# --- CHIPFEAT (a26_feat_out) is quasi-static configuration, not data ---
#        The MCU writes the 16-bit CHIPFEAT word (SPI opcode 0xef) during game
#        load, with the a26 core held in or before its reset, and never again
#        while the core runs; the word is therefore stable for the core's whole
#        runtime and has no synchronous timing relationship to the core's
#        ce-gated captures. OPERATIONAL CONTRACT this false path encodes:
#        firmware must never re-program CHIPFEAT while the a26 core is running.
#        Modelled on sd2snes_nes/main.sdc (nes_feat_out). When the core lands,
#        scope this -to the core instance (as the NES one does) so any other
#        consumer of a26_feat_out stays fully timed.
#        NOTE pos-STA: after synthesis, check main.sta.rpt for Warning
#        332049/332174 pointing at this line (empty collection = the false path
#        was silently ignored). In the skeleton the collection IS empty on
#        purpose -- nothing consumes a26_feat_out yet, so Quartus trims the
#        register; the warning disappears once the core consumes the word.
set_false_path -from [get_registers {mcu_cmd:snes_mcu_cmd|a26_feat_out[*]}] -to [get_registers {a26_core:a26_inst|*}]


#**************************************************************
# Set Multicycle Path
#**************************************************************

# --- multicycles (tia_ce 26/25, cpu_ce 78/77) ---
#        DELIBERATELY not applied blanket-style yet: paths ending at the block
#        RAM ports inside a26 (cart_rom / riot_ram / sc_ram / the video M9Ks)
#        are clocked every CLK2 and must stay single-cycle; the CE relaxations
#        belong only to the RDY-enabled register-to-register paths (arlet/ and
#        the ce-gated regs). First STA decides whether they are needed at all
#        (the cones were written registered/shallow); if violations appear
#        INSIDE ce-covered paths, add here:
#          set_multicycle_path -setup 26 -hold 25 -from <tia_ce regs> -to <tia_ce regs>
#          set_multicycle_path -setup 78 -hold 77 -from <cpu_ce regs> -to <cpu_ce regs>
#        and re-check 332049/332174 in main.sta.rpt after every edit.
#
# CPU-sourced paths (incl. the microcode RAM's output register): every consumer
# inside a26 is CE-gated (ALU/state on RDY = cpu_ce, TIA registers on the
# cpu_ce-qualified write strobe), and the sources only change on the same CE
# grid, spaced >= 26 CLK2 (tia_ce floor; cpu_ce is 3x that).  The a26_video
# serializer is deliberately NOT covered (it runs at CLK2 rate against the
# main.v BUF handshake) -- do not widen these filters.
set_multicycle_path -setup 26 -from [get_registers {*|cpu:arlet_cpu|*}] -to [get_registers {*|a26:a26_inst|*}]
set_multicycle_path -hold  25 -from [get_registers {*|cpu:arlet_cpu|*}] -to [get_registers {*|a26:a26_inst|*}]

# TIA-internal: every register in a26_tia advances on TIA_CE (spacing floor 26).
set_multicycle_path -setup 26 -from [get_registers {*|a26_tia:tia|*}] -to [get_registers {*|a26_tia:tia|*}]
set_multicycle_path -hold  25 -from [get_registers {*|a26_tia:tia|*}] -to [get_registers {*|a26_tia:tia|*}]

# Span-RAM read -> dirty_pub capture: the FSM interposes ST_SPANRD/ST_SPANRD2
# between the RAM address settling and the ST_SPANB0 capture (>= 2 CLK by
# construction, longer under slot_free backpressure).
set_multicycle_path -setup 2 -from [get_registers {*|a26_video:vid|altsyncram:span*}] -to [get_registers {*|a26_video:vid|dirty_pub[*]}]
set_multicycle_path -hold  1 -from [get_registers {*|a26_video:vid|altsyncram:span*}] -to [get_registers {*|a26_video:vid|dirty_pub[*]}]



#**************************************************************
# Set Maximum Delay
#**************************************************************



#**************************************************************
# Set Minimum Delay
#**************************************************************



#**************************************************************
# Set Input Transition
#**************************************************************

