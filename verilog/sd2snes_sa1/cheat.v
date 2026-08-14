`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company:
// Engineer:
//
// Create Date:    16:53:07 07/01/2014
// Design Name:
// Module Name:    cheat
// Project Name:
// Target Devices:
// Tool versions:
// Description:
//
// Dependencies:
//
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
//
//////////////////////////////////////////////////////////////////////////////////

// SA-1 savestate machinery gate.  Always on for mk3 (as before); on mk2 it is
// opt-in via SA1_SS_MK2 so an experimental Spartan-3 build can carry it.  Same
// derived-macro pattern as REGSHADOW_ACTIVE in main.v; the repo uses no include
// files, so every file that needs the gate derives it locally.
`ifdef MK3
`define SA1_SS_ACTIVE
`elsif SA1_SS_MK2
`define SA1_SS_ACTIVE
`endif

module cheat(
  input clk,
  input [7:0] SNES_PA,
  input [23:0] SNES_ADDR,
  input [7:0] SNES_DATA,
  input SNES_wr_strobe,
  input SNES_rd_strobe,
  input SNES_reset_strobe,
  input snescmd_enable,
  input nmicmd_enable,
  input return_vector_enable,
  input branch1_enable,
  input branch2_enable,
  input branch3_enable,
  input pad_latch,
  input snes_ajr,
  input overlay_combo,   // FPGA-detected in-game menu combo (from ctx JOY1 capture; mask is
                         // MCU-programmable via CMD 0xd6, default $4230): gate the IRQ hook
  input SNES_cycle_start,
  input [2:0] pgm_idx,
  input pgm_we,
  input [31:0] pgm_in,
  output [7:0] data_out,
  output cheat_hit,
  output snescmd_unlock,
  output scene_fresh      // 1 = the game's frame loop ran recently (see below); served at $F90720
);

// IRQ hook: required for games that run their vblank logic on IRQ with NMI
// disabled (e.g. Super Mario RPG gameplay) -- without it the in-game hook (and
// the cheat overlay probe inside it) never fires there. The auto_nmi/auto_irq
// usage heuristic picks the active vector per scene, same as the base core
// (where this code is unconditional). Now enabled on mk2 as well: dropping the
// ROM-cheat comparators (-131 LUTs, ROM cheats are patched into PSRAM by the MCU)
// and BS-pack support (-69) freed enough of the Spartan-3 for the overlay.
`define IRQ_HOOK_ENABLE

wire snescmd_wr_strobe = snescmd_enable & SNES_wr_strobe;

reg cheat_enable = 0;
reg nmi_enable = 0;
reg irq_enable = 0;
reg holdoff_enable = 0; // temp disable hooks after reset
reg buttons_enable = 0;
reg wram_present = 0;
wire branch_wram = cheat_enable & wram_present;

// Savestate/cheat-overlay handler enable (bit 6 of pgm reg 7, set by the MCU's
// savestate_enable_handler).  When set, the NMI hook's branch offsets route into
// the nmi_savestate handler (which hosts the in-game cheat-overlay probe); when
// clear they route to nmi_exit.  The base core has this; it was stripped from the
// SA-1 core, which is why in-game savestates/overlay never fired here.  SA-1 does
// not support loadstate, so savestate_force_entry is always 0 (overlay entry is
// driven purely by |pad_data).  Real reg on both configs now (mk2 runs the
// overlay too, gated by pgm reg 7 bit 6 from the MCU).
reg savestate_enable = 0;
// Force savestate-handler entry until the handler returns on its own (the
// in-game hook must keep jumping into the savestate handler until its logic
// has finished).  Required by the full-savestate resume protocol: after a
// save/load completes, the handler parks waiting for the NEXT hook entry to
// bump CS_STATE past the wait loop -- with the trigger buttons long released,
// so |pad_data alone would never re-enter and the game would hang blanked
// (seen in hardware).  Ported verbatim from the base core.  On mk2 savestates
// stay disabled (MCU gate), so the wire folds to 0 there.
`ifdef SA1_SS_ACTIVE
reg savestate_force_entry_enable_strobe = 0;
reg savestate_force_entry_disable_strobe = 0;
reg savestate_force_entry = 0;

always @(posedge clk) begin
  if(savestate_force_entry_enable_strobe) begin
    savestate_force_entry <= 1'b1;
  end else if(savestate_force_entry_disable_strobe) begin
    savestate_force_entry <= 1'b0;
  end
end
`else
wire savestate_force_entry = 1'b0;
`endif

reg auto_nmi_enable = 1;
reg auto_nmi_enable_sync = 0;
`ifdef IRQ_HOOK_ENABLE
reg auto_irq_enable = 0;
reg auto_irq_enable_sync = 0;
`endif
reg hook_enable_sync = 0;

reg [1:0] sync_delay = 2'b10;

reg [4:0] nmi_usage = 5'h00;
`ifdef IRQ_HOOK_ENABLE
reg [4:0] irq_usage = 5'h00;
`endif
reg [20:0] usage_count = 21'h1fffff;

reg [29:0] hook_enable_count = 0;
reg hook_disable = 0;

reg [1:0] vector_unlock_r = 0;
wire vector_unlock = |vector_unlock_r;

reg [1:0] reset_unlock_r = 2'b10;
wire reset_unlock = |reset_unlock_r;

// ROM-cheat comparators: mk3 only.  On mk2 ROM cheats are patched into PSRAM by
// the MCU, so these 6 comparators (-131 LUTs) are dropped to fit the overlay.
`ifndef MK2
reg [23:0] cheat_addr[5:0];
reg [7:0] cheat_data[5:0];
`endif
reg [5:0] cheat_enable_mask;

reg snescmd_unlock_r = 0;
assign snescmd_unlock = snescmd_unlock_r;

reg [7:0] nmicmd = 0;
reg [7:0] return_vector = 8'hea;

reg [7:0] branch1_offset = 8'h00;
reg [7:0] branch2_offset = 8'h00;
reg [7:0] branch3_offset = 8'h04;

reg [15:0] pad_data = 0;

`ifndef MK2
wire [5:0] cheat_match_bits ={(cheat_enable_mask[5] & (SNES_ADDR == cheat_addr[5])),
                              (cheat_enable_mask[4] & (SNES_ADDR == cheat_addr[4])),
                              (cheat_enable_mask[3] & (SNES_ADDR == cheat_addr[3])),
                              (cheat_enable_mask[2] & (SNES_ADDR == cheat_addr[2])),
                              (cheat_enable_mask[1] & (SNES_ADDR == cheat_addr[1])),
                              (cheat_enable_mask[0] & (SNES_ADDR == cheat_addr[0]))};
`else
// mk2: no ROM-cheat comparators (MCU patches ROM cheats into PSRAM) -> fold away.
wire [5:0] cheat_match_bits = 6'h00;
`endif
wire cheat_addr_match = |cheat_match_bits;

wire [1:0] nmi_match_bits = {SNES_ADDR == 24'h00FFEA, SNES_ADDR == 24'h00FFEB};
`ifdef IRQ_HOOK_ENABLE
wire [1:0] irq_match_bits = {SNES_ADDR == 24'h00FFEE, SNES_ADDR == 24'h00FFEF};
`endif
wire [1:0] rst_match_bits = {SNES_ADDR == 24'h00FFFC, SNES_ADDR == 24'h00FFFD};

wire nmi_addr_match = |nmi_match_bits;
`ifdef IRQ_HOOK_ENABLE
wire irq_addr_match = |irq_match_bits;
`endif
wire rst_addr_match = |rst_match_bits;

wire hook_enable = ~|hook_enable_count;

assign data_out =
`ifndef MK2
                  cheat_match_bits[0] ? cheat_data[0]
                : cheat_match_bits[1] ? cheat_data[1]
                : cheat_match_bits[2] ? cheat_data[2]
                : cheat_match_bits[3] ? cheat_data[3]
                : cheat_match_bits[4] ? cheat_data[4]
                : cheat_match_bits[5] ? cheat_data[5]
                :
`endif
                  nmi_match_bits[1] ? 8'h10
`ifdef IRQ_HOOK_ENABLE
                : irq_match_bits[1] ? 8'h10
`endif
                : rst_match_bits[1] ? 8'h7D
                : nmicmd_enable ? nmicmd
                : return_vector_enable ? return_vector
                : branch1_enable ? branch1_offset
                : branch2_enable ? branch2_offset
                : branch3_enable ? branch3_offset
                : 8'h2a;

assign cheat_hit = (snescmd_unlock & hook_enable_sync & (nmicmd_enable | return_vector_enable | branch1_enable | branch2_enable | branch3_enable))
                   | (reset_unlock & rst_addr_match)
                   | (cheat_enable & cheat_addr_match)
                   | (hook_enable_sync & (((auto_nmi_enable_sync & nmi_enable) & nmi_addr_match & vector_unlock)
`ifdef IRQ_HOOK_ENABLE                   
                                           |((auto_irq_enable_sync & irq_enable) & irq_addr_match & vector_unlock)
`endif
                                           ));

// irq/nmi detect based on CPU access pattern
// 4 writes (mirrored to B bus) signify that the CPU pushes PB, PC and
// SR to the stack and is going to read the vector address in the next
// two cycles.
// B bus mirror is used (combined with A BUS /WR!) so the write pattern
// cannot be confused with backwards DMA transfers.

reg [7:0] next_pa_addr = 0;
reg [2:0] cpu_push_cnt = 0;

always @(posedge clk) begin
  if(SNES_reset_strobe) begin
    cpu_push_cnt <= 0;
  end else if(SNES_wr_strobe) begin
    cpu_push_cnt <= cpu_push_cnt + 1;
    if(cpu_push_cnt == 3'b0) begin
      next_pa_addr <= SNES_PA - 1;
    end else begin
      if(SNES_PA == next_pa_addr) begin
         next_pa_addr <= next_pa_addr - 1;
      end else begin
        cpu_push_cnt <= 3'b0;
      end
    end
  end else if(SNES_rd_strobe) begin
    cpu_push_cnt <= 3'b0;
  end
end

// IRQ hook rate limit: cap IRQ hooking to ~once per frame.  A raster effect
// (e.g. Super Mario RPG's dialog box) fires an H-IRQ every scanline; the
// savestate/overlay handler is far longer than the ~63us scanline gap, so
// hooking every raster IRQ makes the handlers pile up and the game hangs
// waiting for its own delayed ISR (deterministic freeze with $4200=$01, seen
// in hardware).  After hooking one IRQ, suppress further IRQ hooks for ~12ms
// (2^20 CLK2): the raster burst then runs unhooked (the game's own IRQ still
// vectors at full speed -- we only skip OUR redirect) while the overlay probe
// still runs about once per frame.  NMI needs no limit (naturally once/frame).
`ifdef IRQ_HOOK_ENABLE
reg [19:0] irq_hold = 0;
wire irq_hold_ok = ~|irq_hold;
// overlay_combo covers physical combos (incl. the savestate default inputs, see
// main.v); savestate_force_entry keeps the redirect armed while a savestate is
// in flight (buttons long released), so the resume-wait IRQ can re-enter the
// handler in IRQ-driven scenes.  It clears at the unlock-drop on handler exit.
// force_entry must also BYPASS the auto_nmi/auto_irq usage heuristic: the
// ~1.7s frozen save has zero vector fetches, so the usage window rolls over
// and flips auto_irq off -- which would unarm the very IRQ the resume waits
// for (seen in hardware as CS_STATE parked at 1 after a field save).
wire irq_arm = irq_enable & irq_match_bits[1] & irq_hold_ok
             & ((auto_irq_enable_sync & overlay_combo) | savestate_force_entry);
always @(posedge clk) begin
  if(SNES_reset_strobe) irq_hold <= 0;
  else if(SNES_rd_strobe & hook_enable_sync & irq_arm & (cpu_push_cnt == 4))
    irq_hold <= 20'hfffff;
  else if(|irq_hold) irq_hold <= irq_hold - 1'b1;
end
`else
wire irq_arm = 1'b0;
`endif

// make patched vectors visible for last cycles of NMI/IRQ handling only
always @(posedge clk) begin
  if(SNES_reset_strobe) begin
    vector_unlock_r <= 2'b00;
  end else if(SNES_rd_strobe) begin
    if(hook_enable_sync
      & ((auto_nmi_enable_sync & nmi_enable & nmi_match_bits[1])
`ifdef IRQ_HOOK_ENABLE
        | irq_arm
`endif
        )
      & cpu_push_cnt == 4) begin
      vector_unlock_r <= 2'b11;
    end else if(|vector_unlock_r) begin
      vector_unlock_r <= vector_unlock_r - 1;
    end
  end
end

// make patched reset vector visible for first fetch only
// (including masked read by Ultra16)
always @(posedge clk) begin
  if(SNES_reset_strobe) begin
    reset_unlock_r <= 2'b11;
  end else if(SNES_cycle_start) begin
    if(rst_addr_match & |reset_unlock_r) begin
      reset_unlock_r <= reset_unlock_r - 1;
    end
  end
end

reg snescmd_unlock_disable_strobe = 1'b0;
reg [6:0] snescmd_unlock_disable_countdown = 0;
reg snescmd_unlock_disable = 0;

always @(posedge clk) begin
`ifdef SA1_SS_ACTIVE
  savestate_force_entry_disable_strobe <= 0;
  // Pulse (unlike the base core, where the enable strobe latches forever): on
  // the SA-1 core force_entry must actually CLEAR at the unlock-drop, because
  // it also arms the IRQ redirect below -- a permanently-latched force_entry
  // would re-introduce the per-frame IRQ redirect the combo gate exists to
  // prevent (the SMRPG raster-IRQ freeze class).
  savestate_force_entry_enable_strobe <= 0;
`endif
  if(SNES_reset_strobe) begin
    snescmd_unlock_r <= 0;
    snescmd_unlock_disable <= 0;
  end else begin
    if(SNES_rd_strobe) begin
      // *** GAME -> INGAME HOOK ***
      if(hook_enable_sync
        & ((auto_nmi_enable_sync & nmi_enable & nmi_match_bits[1])
`ifdef IRQ_HOOK_ENABLE
          | irq_arm
`endif
          )
        & cpu_push_cnt == 4) begin
        // remember where we came from (IRQ/NMI) for hook exit
        return_vector <= SNES_ADDR[7:0];
        snescmd_unlock_r <= 1;
      end
      if(rst_match_bits[1] & |reset_unlock_r) begin
        snescmd_unlock_r <= 1;
      end
`ifdef SA1_SS_ACTIVE
      if(branch1_enable & savestate_enable & |pad_data) begin
        savestate_force_entry_enable_strobe <= 1;
      end
`endif
    end
    // give some time to exit snescmd memory and jump to original vector
    // sta @NMI_VECT_DISABLE    1-2 (after effective write)
    // jmp ($ffxx)              3 (excluding address fetch)
    // *** (INGAME HOOK -> GAME) ***
    if(SNES_cycle_start) begin
      if(snescmd_unlock_disable) begin
        if(|snescmd_unlock_disable_countdown) begin
          snescmd_unlock_disable_countdown <= snescmd_unlock_disable_countdown - 1;
        end else if(snescmd_unlock_disable_countdown == 0) begin
          snescmd_unlock_r <= 0;
          snescmd_unlock_disable <= 0;
`ifdef SA1_SS_ACTIVE
          savestate_force_entry_disable_strobe <= 1;
`endif
        end
      end
    end
    if(snescmd_unlock_disable_strobe) begin
      snescmd_unlock_disable_countdown <= 7'd6;
      snescmd_unlock_disable <= 1;
    end
  end
end


always @(posedge clk) usage_count <= usage_count - 1;

// Scene liveness for the in-game overlay probe (served to the SNES at $F90720 by
// main.v).  The NMI/IRQ hook can fire while the game's frame loop is NOT running --
// e.g. a Super Mario RPG scene transition, where the S-CPU is parked mid-RPC waiting
// on the SA-1 and the vblank handler is a stub.  Opening the overlay there hangs the
// game on resume (proven in hardware on mk2).  Unlike overlay_combo (which only gates
// the IRQ redirect) this is a pure liveness observation, available on BOTH platforms:
// the frame loop is alive if the S-CPU either forwarded the pad word to SA-1 IRAM
// $3010/$3011 (the SMRPG world/field convention, see the ss_iram_pad snoop in sa1.v)
// or polled the auto-joypad registers $4218/$4219 (every normal menu/battle loop)
// recently.  Both are plain SNES-bus accesses, so no ctx/SA-1 internals are needed.
//
// Compare values are SNES_ADDR[15:1] (bit 0 dropped so each pair matches with one
// comparator): $3010>>1 = $1808, $4218>>1 = $210C.  ~SNES_ADDR[22] restricts the
// match to banks $00-$3F/$80-$BF, where both windows live (same idiom as
// snoop_4200_enable/r4016_enable in main.v, and the same bank half address.v uses
// for its own IRAM decode).
//
// ~snescmd_unlock_r IS LOAD-BEARING, NOT HYGIENE: the hook stub itself reads $4218
// on EVERY entry (snes/nmihook.a65 "lda @$004218", before branch1, on the only path
// that reaches the probe), and it runs under the unlock.  Without this mask the
// probe's own hook would re-arm the bit microseconds before the probe samples it and
// the gate could never close -- it would read fresh=1 in a dead scene, which is the
// exact failure this whole feature exists to prevent.  The overlay and igmenu read
// $4218 under the unlock too, and the SA-1 savestate restore writes IRAM $3000-$37FF
// over the bus, so BOTH terms are masked.  ctx.v:431-435 already ignores $421x reads
// "during NMI hook" with the same !snescmd_unlock test.  (snescmd_unlock_r is the reg
// behind the snescmd_unlock output; using it directly keeps this self-contained.)
//
// Expiry rides the free-running usage_count rollover (2^21 CLK2 ~ 24.4ms @85.87MHz):
// an event reloads all three bits, each rollover shifts one out, so the bit survives
// 3 rollovers = a 48.8-73.2ms window.  THREE bits, not two: with two the worst case
// is 24.4ms, SHORTER than one frame of an engine that only touches the pad every
// other frame (~33ms), so a legitimate press could land in a dead window.  At three
// bits a missed frame genuinely cannot close the gate; a parked frame loop still does.
wire scene_pad_wr = SNES_wr_strobe & ~snescmd_unlock_r & ~SNES_ADDR[22] & (SNES_ADDR[15:1] == 15'h1808); // IRAM $3010/$3011
wire scene_joy_rd = SNES_rd_strobe & ~snescmd_unlock_r & ~SNES_ADDR[22] & (SNES_ADDR[15:1] == 15'h210c); // $4218/$4219
reg [2:0] scene_fresh_r = 3'b000;
always @(posedge clk) begin
  if(scene_pad_wr | scene_joy_rd) scene_fresh_r <= 3'b111;
  else if(usage_count == 21'd0)   scene_fresh_r <= {1'b0, scene_fresh_r[2:1]};
end
assign scene_fresh = |scene_fresh_r;

// Try and autoselect NMI or IRQ hook
always @(posedge clk) begin
  if(usage_count == 21'b0) begin
    nmi_usage <= SNES_cycle_start & nmi_match_bits[1];
`ifdef IRQ_HOOK_ENABLE    
    irq_usage <= SNES_cycle_start & irq_match_bits[1];
`endif

`ifdef IRQ_HOOK_ENABLE    
    if(|nmi_usage & |irq_usage) begin
      auto_nmi_enable <= 1'b1;
      auto_irq_enable <= 1'b0;
    end else if(irq_usage == 5'b0) begin
      auto_nmi_enable <= 1'b1;
      auto_irq_enable <= 1'b0;
    end else if(nmi_usage == 5'b0) begin
      auto_nmi_enable <= 1'b0;
      auto_irq_enable <= 1'b1;
    end
`else
    auto_nmi_enable <= |nmi_usage;
`endif
  end else begin
    if(SNES_cycle_start & nmi_match_bits[0]) nmi_usage <= nmi_usage + 1;
`ifdef IRQ_HOOK_ENABLE    
    if(SNES_cycle_start & irq_match_bits[0]) irq_usage <= irq_usage + 1;
`endif
  end
end

// Do not change vectors while they are being read
always @(posedge clk) begin
  if(SNES_cycle_start) begin
    if(nmi_addr_match
`ifdef IRQ_HOOK_ENABLE    
      | irq_addr_match
`endif
      ) sync_delay <= 2'b10;
    else begin
      if (|sync_delay) sync_delay <= sync_delay - 1;
      if (sync_delay == 2'b00) begin
        auto_nmi_enable_sync <= auto_nmi_enable;
`ifdef IRQ_HOOK_ENABLE
        auto_irq_enable_sync <= auto_irq_enable;
`endif
        hook_enable_sync <= hook_enable;
      end
    end
  end
end

// CMD 0x85: disable hooks for 10 seconds
always @(posedge clk) begin
  if((snescmd_unlock & snescmd_wr_strobe & ~|SNES_ADDR[8:0] & (SNES_DATA == 8'h85))
     | (holdoff_enable & SNES_reset_strobe)) begin
    hook_enable_count <= 30'd960000000;
  end else if (|hook_enable_count) begin
    hook_enable_count <= hook_enable_count - 1;
  end
end

always @(posedge clk) begin
  if(SNES_reset_strobe) begin
    snescmd_unlock_disable_strobe <= 1'b0;
  end else begin
    snescmd_unlock_disable_strobe <= 1'b0;
    if(snescmd_unlock & snescmd_wr_strobe) begin
      if(~|SNES_ADDR[8:0]) begin
        case(SNES_DATA)
          8'h82: cheat_enable <= 1;
          8'h83: cheat_enable <= 0;
          8'h84: {nmi_enable, irq_enable} <= 2'b00;
        endcase
      end else if(SNES_ADDR[8:0] == 9'h1fd) begin
        snescmd_unlock_disable_strobe <= 1'b1;
      end
    end else if(pgm_we) begin
`ifndef MK2
      if(pgm_idx < 6) begin // mk3 ROM-cheat comparators (dropped on mk2)
        cheat_addr[pgm_idx] <= pgm_in[31:8];
        cheat_data[pgm_idx] <= pgm_in[7:0];
      end else
`endif
      if(pgm_idx == 6) begin // set rom patch enable
        cheat_enable_mask <= pgm_in[5:0];
      end else if(pgm_idx == 7) begin // set/reset global enable / hooks
      // pgm_in[14:8] are reset bit flags
      // pgm_in[6:0] are set bit flags
        {savestate_enable, wram_present, buttons_enable, holdoff_enable, irq_enable, nmi_enable, cheat_enable}
         <= ({savestate_enable, wram_present, buttons_enable, holdoff_enable, irq_enable, nmi_enable, cheat_enable}
          & ~pgm_in[14:8])
          | pgm_in[6:0];
      end
    end
  end
end

// map controller input to cmd output
// check button combinations
// L+R+Start+Select : $3030
// L+R+Select+X     : $2070
// L+R+Start+A      : $10b0
// L+R+Start+B      : $9030
// L+R+Start+Y      : $5030
// L+R+Start+X      : $1070
always @(posedge clk) begin
  if(snescmd_wr_strobe) begin
    if(SNES_ADDR[8:0] == 9'h1f0) begin
      pad_data[7:0] <= SNES_DATA;
    end else if(SNES_ADDR[8:0] == 9'h1f1) begin
      pad_data[15:8] <= SNES_DATA;
    end
  end
end

always @* begin
  case(pad_data)
    16'h3030: nmicmd = 8'h80;
    16'h2070: nmicmd = 8'h81;
    16'h10b0: nmicmd = 8'h82;
    16'h9030: nmicmd = 8'h83;
    16'h5030: nmicmd = 8'h84;
    16'h1070: nmicmd = 8'h85;
    default: nmicmd = 8'h00;
  endcase
end

always @* begin
  if(buttons_enable) begin
    if(snes_ajr) begin
      if(nmicmd) begin
        branch1_offset = 8'h30;   // nmi_echocmd
      end else begin
        if(branch_wram) begin
          branch1_offset = 8'h3a; // nmi_patches
        end else begin
          if(savestate_enable & (savestate_force_entry | |pad_data)) begin
            branch1_offset = 8'h3f; // nmi_savestate
          end else begin
            branch1_offset = 8'h43; // nmi_exit
          end
        end
      end
    end else begin
      if(pad_latch) begin
        if(branch_wram) begin
          branch1_offset = 8'h3a; // nmi_patches
        end else begin
          branch1_offset = 8'h43; // nmi_exit
        end
      end else begin
        branch1_offset = 8'h00;   // continue with MJR
      end
    end
  end else begin
    if(branch_wram) begin
      branch1_offset = 8'h3a;     // nmi_patches
    end else begin
      if(savestate_enable & |pad_data) begin
        branch1_offset = 8'h3f;   // nmi_savestate
      end else begin
        branch1_offset = 8'h43;   // nmi_exit
      end
    end
  end
end

always @* begin
  if(nmicmd == 8'h81) begin
    branch2_offset = 8'h14;       // nmi_stop
  end else if(branch_wram) begin
    branch2_offset = 8'h00;       // nmi_patches
  end else begin
    if(savestate_enable) begin
      branch2_offset = 8'h05;     // nmi_savestate
    end else begin
      branch2_offset = 8'h09;     // nmi_exit
    end
  end
end

always @* begin
  if(savestate_enable) begin
    branch3_offset = 8'h00;       // nmi_savestate
  end else begin
    branch3_offset = 8'h04;       // nmi_exit
  end
end

endmodule
