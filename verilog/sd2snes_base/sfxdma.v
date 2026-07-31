`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// sfxdma - autonomous PCM sound-effect fetcher (menu SFX played by the FPGA).
//
// Streams a one-shot PCM sound effect straight from PSRAM into the DAC's 2 KB
// dac_buf, with ZERO real-time MCU involvement, so a menu SFX keeps playing even
// while the MCU is stuck in a long blocking SD transaction (dir scan, cover load,
// f_open + FAT cluster walk).  Without this the MCU had to refill a 1 KB half of
// dac_buf every ~5.8 ms (menu_sfx_pump); when it couldn't, dac.v blindly re-wrapped
// the 2 KB ring -> the audible "frozen frames in a loop".
//
// The engine is a LOW-PRIORITY, MCU-ARMED PSRAM read master (modelled on the read
// side of dma.v).  The MCU supplies {base,len} + a kick; every counter here is
// bounded + monotonic and there is a watchdog, so a mis-sampled PSRAM word can
// never run away (cf. the dma.v list-mode wedge -- do NOT reintroduce that shape).
//
// Playback model (mirrors the old MCU pump, but in hardware):
//   * kick        -> prime the WHOLE 2 KB (both halves) from PSRAM, hold DAC play
//                    (prime_hold) until primed so the first frames are not garbage.
//   * DAC_STATUS  -> the dac.v read-pointer MSB (which 1 KB half is playing).  On
//                    each flip we refill the just-vacated half with the next 1 KB.
//   * one-shot    -> once the src pointer passes `len`, refills write SILENCE (not
//                    a loop).  After two consecutive fully-silent half-refills the
//                    entire 512-frame ring is silence; we drop `active` and the DAC
//                    plays the tail out into clean silence (never a buzzing loop).
//
// Byte order: the PCM body was DMA'd into PSRAM byte-for-byte by the MCU (same as
// the old SD->dac_buf offload), and main.v hands us the byte at ROM_ADDR using the
// SAME select convention as a SNES/MCU read, so we just stream bytes in order.
//////////////////////////////////////////////////////////////////////////////////
module sfxdma(
  input clkin,
  input reset,

  // MCU-armed control (mcu_cmd: 0xe8 = base/len + kick, 0xe9 = disable)
  input [23:0] sfx_base,    // PSRAM byte address of the PCM body (post-header)
  input [23:0] sfx_len,     // PCM body length in BYTES (frame_count * 4)
  input        kick,        // 1-cycle strobe: (re)start playback, newest wins
  input        disable_in,  // 1-cycle strobe: abort + release (game-load gating)

  // DAC read-pointer half flag (dac.v DAC_STATUS = dac_address_r[8])
  input DAC_STATUS,

  // High while an SD->PSRAM DMA owns the bus (main.v SD_DMA_TO_ROM).  During it
  // every STATE-machine master -- us included -- is locked out of PSRAM
  // (free_slot &= ~SD_DMA_TO_ROM), so our PCM reads cannot be serviced.  Instead
  // of stalling (which lets the DAC re-wrap the last buffer -> the "frozen loop"
  // buzz on game boot / game-info asset staging), we feed the DAC SILENCE while
  // this is high, WITHOUT consuming src_off, so the effect resumes seamlessly the
  // instant the DMA releases the bus.
  input sd_dma_active,

  // PSRAM read bus (arbitrated by main.v, mirrors the dma.v read handshake)
  input         BUS_RDY,
  output        BUS_RRQ,
  output [23:0] ROM_ADDR,
  input  [7:0]  ROM_DATA_IN, // = SFX_DINr, the byte main.v selected via ROM_ADDR0

  // dac_buf write port takeover (muxed in main.v when dac_active)
  output        dac_we,      // ACTIVE LOW (dac_buf wren = ~we)
  output [10:0] dac_addr,
  output [7:0]  dac_data,
  output        dac_active,  // 1 => this engine owns the dac_buf write port
  output        prime_hold,  // 1 => hold DAC play (initial prime in progress)

  // status (mcu_cmd 0xf7)
  output        active,
  output        done
);

localparam WE_ON  = 1'b0;
localparam WE_OFF = 1'b1;

localparam [11:0] DACBUF_BYTES = 12'd2048;
localparam [11:0] HALF_BYTES   = 12'd1024;
localparam [2:0]  SILENCE_HALVES_DONE = 3'd2; // 2 full silent halves = whole ring silent

// runaway backstop: longest SFX ~50 KB -> ~1.8M cycles; 2^25 ~ 33M >> that
localparam [24:0] WD_MAX = 25'h1FFFFFF;

localparam [2:0] ST_IDLE = 3'd0;
localparam [2:0] ST_READ = 3'd1; // real byte: issue PSRAM read
localparam [2:0] ST_WAIT = 3'd2; // real byte: wait for the read data
localparam [2:0] ST_PUT  = 3'd3; // present the dac_buf write (real or silence byte)
localparam [2:0] ST_RUN  = 3'd4; // primed & playing: watch DAC_STATUS for refills

reg [2:0]  state;         initial state = ST_IDLE;
reg [23:0] base_r;        initial base_r = 0;
reg [23:0] len_r;         initial len_r = 0;
reg [23:0] src_off;       initial src_off = 0;    // bytes of PCM body already consumed
reg [11:0] wr_addr;       initial wr_addr = 0;    // dac_buf write pointer (0..2047)
reg [11:0] fill_left;     initial fill_left = 0;  // bytes left in current fill
reg [7:0]  byte_r;        initial byte_r = 0;     // captured PCM byte to write
reg        prime_r;       initial prime_r = 0;    // this fill is the initial prime
reg        fill_silent;   initial fill_silent = 0;// this fill started at/after end-of-audio
reg [1:0]  silence_halves;initial silence_halves = 0;
reg        dac_prev;      initial dac_prev = 0;
reg        active_r;      initial active_r = 0;
reg        done_r;        initial done_r = 0;
reg [24:0] wd;            initial wd = 0;

reg        dac_we_r;      initial dac_we_r = WE_OFF;
reg [10:0] dac_addr_r;    initial dac_addr_r = 0;
reg [7:0]  dac_data_r;    initial dac_data_r = 0;
reg        prime_hold_r;  initial prime_hold_r = 0;

// Only request a real read; silence bytes (end-of-audio or SD-DMA lockout) and the
// fill-complete/idle states never touch the PSRAM bus.
assign BUS_RRQ    = BUS_RDY & (state == ST_READ) & (fill_left != 0) & (src_off < len_r)
                    & ~sd_dma_active;
assign ROM_ADDR   = base_r + src_off;
assign dac_we     = dac_we_r;
assign dac_addr   = dac_addr_r;
assign dac_data   = dac_data_r;
assign dac_active = active_r;
assign prime_hold = prime_hold_r;
assign active     = active_r;
assign done       = done_r;

wire [2:0] next_silence = {1'b0, silence_halves} + 3'd1;

always @(posedge clkin) begin
  dac_we_r <= WE_OFF; // default: no dac_buf write this cycle

  if (reset) begin
    state        <= ST_IDLE;
    active_r     <= 1'b0;
    prime_hold_r <= 1'b0;
    done_r       <= 1'b0;
    wd           <= 25'h0;
  end else if (disable_in) begin
    state        <= ST_IDLE;      // abort (game-load gating): release the DAC port
    active_r     <= 1'b0;
    prime_hold_r <= 1'b0;
    wd           <= 25'h0;
  end else if (kick) begin
    // (re)start playback -- newest wins.  Prime the WHOLE 2 KB from PSRAM.
    base_r         <= sfx_base;
    len_r          <= sfx_len;
    src_off        <= 24'h0;
    wr_addr        <= 12'h0;
    fill_left      <= DACBUF_BYTES;
    prime_r        <= 1'b1;
    fill_silent    <= 1'b0;        // SFX bodies are all > 2 KB, prime is real audio
    silence_halves <= 2'b0;
    dac_prev       <= 1'b0;        // read pointer parks at 0 during prime
    active_r       <= 1'b1;
    done_r         <= 1'b0;
    prime_hold_r   <= 1'b1;
    wd             <= 25'h0;
    state          <= ST_READ;
  end else begin
    if (active_r) wd <= wd + 1'b1;
    if (active_r & (wd > WD_MAX)) begin // runaway backstop (should be unreachable)
      state        <= ST_IDLE;
      active_r     <= 1'b0;
      prime_hold_r <= 1'b0;
      done_r       <= 1'b1;
    end else begin
      case (state)
        ST_READ: begin
          if (fill_left == 0) begin
            // current fill complete
            if (prime_r) prime_hold_r <= 1'b0; // initial prime done -> let the DAC play
            prime_r <= 1'b0;
            if (fill_silent) begin
              silence_halves <= next_silence[1:0];
              if (next_silence >= SILENCE_HALVES_DONE) begin
                active_r <= 1'b0;              // whole ring is silence now -> stop
                done_r   <= 1'b1;
                state    <= ST_IDLE;
              end else state <= ST_RUN;
            end else state <= ST_RUN;
          end else if (src_off >= len_r) begin
            // silence byte: no PSRAM read needed
            byte_r <= 8'h00;
            state  <= ST_PUT;
          end else if (sd_dma_active) begin
            // An SD->PSRAM DMA has the bus: our read cannot be serviced.  Emit a SILENCE
            // byte (keeps the DAC fed with zeros -> no re-wrap buzz) WITHOUT advancing
            // src_off, so once the DMA releases the bus we pick the real PCM stream back
            // up exactly where we left off.
            byte_r <= 8'h00;
            state  <= ST_PUT;
          end else begin
            // real byte: issue PSRAM read (BUS_RRQ asserted while RDY); advance once the
            // request is accepted (RDY drops as main latches it).
            if (BUS_RDY) state <= ST_WAIT;
          end
        end
        ST_WAIT: begin
          if (sd_dma_active) begin            // SD-DMA lockout landed mid-read: abandon the
            byte_r <= 8'h00;                  // in-flight read (it will never be serviced while
            state  <= ST_PUT;                 // the DMA owns the bus) and feed the DAC silence,
          end                                 // WITHOUT advancing src_off -> re-read on release.
          else if (BUS_RDY) begin             // read serviced -> ROM_DATA_IN valid
            byte_r  <= ROM_DATA_IN;
            src_off <= src_off + 1'b1;
            state   <= ST_PUT;
          end
        end
        ST_PUT: begin
          dac_addr_r <= wr_addr[10:0];
          dac_data_r <= byte_r;
          dac_we_r   <= WE_ON;                // one-cycle dac_buf write
          wr_addr    <= wr_addr + 1'b1;
          fill_left  <= fill_left - 1'b1;
          state      <= ST_READ;
        end
        ST_RUN: begin
          if (DAC_STATUS != dac_prev) begin
            // half flipped: refill the just-vacated half with the next 1 KB.
            // DAC now reading upper (STATUS=1) -> refill lower (0); else upper.
            dac_prev    <= DAC_STATUS;         // capture flip (don't re-trigger on return)
            wr_addr     <= DAC_STATUS ? 12'h0 : HALF_BYTES;
            fill_left   <= HALF_BYTES;
            fill_silent <= (src_off >= len_r);
            state       <= ST_READ;
          end
        end
        default: state <= ST_IDLE;
      endcase
    end
  end
end

endmodule
