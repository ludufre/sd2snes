#!/usr/bin/env python3
"""
Golden model of the BS-X satellite RECEIVER FSM to be implemented in bsx.v.

Faithful port of bsnes-plus BSXBase ($218A/$218B/$218C queue/prefix/data) with the
device difference: instead of bsnes loading the next BSX{ch}-{count}.bin file on the
$218A empty-read, the MCU STAGES the next fragment into PSRAM (0xA00000 ring) and the
FPGA serves it. This model is the spec the RTL must match register-for-register; it
also frames a program exactly like SatellaWave SaveChannelFile so a hardware bus
capture can be diffed against it.

Validation: drain the receiver exactly as the Town BIOS does (read queue size, then
per frame read 1 prefix + 22 data) and confirm the reassembled program == the original.
"""
import math

PKT = 22  # bytes per stream frame ("unit")


def frag_frames(nbytes):
    return math.ceil(nbytes / PKT)


def make_fragments(program, chunk=0x8000):
    """Split a program into SatellaWave-style fragments: each = 10-byte Data-Group
    header (DGID, continuity, size24=chunk+5, fixed=1, fragcount, offset24) + chunk."""
    n = math.ceil(len(program) / chunk)
    frags = []
    for i in range(n):
        co = i * chunk
        body = program[co:co + (len(program) - co if i == n - 1 else chunk)]
        size = len(body) + 5
        hdr = bytes([0, i, (size >> 16) & 0xFF, (size >> 8) & 0xFF, size & 0xFF,
                     1, n, (co >> 16) & 0xFF, (co >> 8) & 0xFF, co & 0xFF])
        frags.append(hdr + body)
    return frags


class Receiver:
    """One stream. PSRAM holds only the CURRENT fragment (the MCU ring); the FPGA
    serves it byte-by-byte. Mirrors bsnes BSXStream + enter() pacing."""
    def __init__(self, pace_period=8):
        self.pace_period = pace_period   # pacer ticks between buffering one frame
        self._pace = 0
        # bsnes BSXStream fields:
        self.channel = 0
        self.frag = None        # current fragment bytes resident in PSRAM (device: 0xA00000)
        self.queue = 0          # frames left to BUFFER from the current fragment
        self.pf_queue = 0       # buffered status frames (<=0x80)
        self.dt_queue = 0       # buffered data frames (<=0x80)
        self.offset = 0         # byte index within the current fragment
        self.first = True
        self.pf_latch = False
        self.dt_latch = False
        self.status = 0
        # device streaming:
        self.dl_seq = 0         # bumps ONCE per fragment-needed (edge), like bs_erase_seq
        self.need_next = False  # waiting for the MCU to stage; gates the one-shot notify
        self.staged = None      # MCU-staged next fragment (None = not ready yet)
        self.overrun = False

    # --- MCU side -----------------------------------------------------------
    def mcu_stage(self, frag):
        """MCU writes the descriptor + DMAs the fragment into the ring."""
        self.staged = frag

    # --- pacer (FPGA timer ~ bsnes enter()) ---------------------------------
    def tick(self):
        self._pace += 1
        if self._pace < self.pace_period:
            return
        self._pace = 0
        if self.queue > 0:
            if self.pf_latch and self.dt_latch:
                self.queue -= 1
                if self.pf_queue < 0x80:
                    self.pf_queue += 1
                else:
                    self.overrun = True
                if self.dt_queue < 0x80:
                    self.dt_queue += 1

    # --- $2188/$2189 channel select -----------------------------------------
    def write_channel_lo(self, data):
        if (self.channel & 0xFF) != data:
            self._reset_fragment_seq()
        self.channel = (self.channel & 0x3F00) | data

    def write_channel_hi(self, data):
        if (self.channel >> 8) != (data & 0x3F):
            self._reset_fragment_seq()
        self.channel = (self.channel & 0xFF) | ((data & 0x3F) << 8)

    def _reset_fragment_seq(self):
        self.frag = None
        self.staged = None
        self.need_next = False
        self.queue = self.pf_queue = self.dt_queue = self.offset = 0
        self.first = True

    # --- $218B / $218C latch enables (writing 0 clears) ---------------------
    def write_pf_latch(self, data):
        self.pf_latch = (data != 0)
        self.pf_queue = 0

    def write_dt_latch(self, data):
        self.dt_latch = (data != 0)
        self.dt_queue = 0

    # --- $218A read: Queue Size (+ fragment-advance trigger) ----------------
    def read_queue(self):
        if not self.pf_latch or not self.dt_latch:
            return 0
        # "fully consumed" = nothing left to buffer (queue==0) AND nothing buffered
        # (pf/dt==0). bsnes omits queue==0 and relies on its always-running enter()
        # pacer; on the device the pacer is a clock divider, so a $218A read can land
        # right after a load before any frame is buffered -> require queue==0 too, or
        # it re-triggers and skips the just-loaded fragment.
        if self.queue == 0 and self.pf_queue == 0 and self.dt_queue == 0:
            # current fragment fully drained -> need the next one
            self.offset = 0
            if self.staged is not None:
                self.frag = self.staged
                self.staged = None
                self.queue = frag_frames(len(self.frag))
                self.first = True
                self.need_next = False
            elif not self.need_next:
                self.need_next = True                 # edge: notify the MCU exactly once
                self.dl_seq = (self.dl_seq + 1) & 3
            # else: already notified, still waiting -> just report empty (Town retries)
        return self.pf_queue | (0x80 if self.overrun else 0)

    # --- $218B read: prefix/status byte -------------------------------------
    def read_prefix(self):
        if not self.pf_latch:
            return 0
        prefix = 0
        if self.pf_queue:
            if self.first:
                prefix |= 0x10          # Packet-Start (first frame carries the DG header)
                self.first = False
            self.pf_queue -= 1
            if self.queue == 0 and self.pf_queue == 0:
                prefix |= 0x80          # Packet-End (last frame of this data group)
        self.status |= prefix
        return prefix

    # --- $218C read: one data byte ------------------------------------------
    def read_data(self):
        if not self.dt_latch:
            return 0
        data = 0
        if self.dt_queue:
            data = self.frag[self.offset] if self.frag and self.offset < len(self.frag) else 0
            self.offset += 1
            if self.offset % PKT == 0:
                self.dt_queue -= 1
        return data

    # --- $218D read: status summary (clear gated by $2194.0, here always) ---
    def read_status(self, clear_enable=True):
        s = self.status
        if clear_enable:
            self.status = 0
        return s


def simulate_download(program, chunk=0x8000, pace_period=8, max_steps=50_000_000):
    """Drain the receiver like the Town BIOS and reassemble the program.
    BIOS per data-group: read prefix+22*data per frame until Packet-End (0x80),
    strip the 10-byte DG header from the first frame, append the body. The MCU
    stages the next fragment when the FPGA bumps dl_seq."""
    frags = make_fragments(program, chunk)
    rx = Receiver(pace_period=pace_period)
    # BIOS reset/tune sequence:
    rx.write_channel_lo(0x25); rx.write_channel_hi(0x01)   # tune the program channel
    rx.write_pf_latch(1); rx.write_dt_latch(1)             # enable both latches
    mcu_next = 0
    last_seq = rx.dl_seq
    rx.mcu_stage(frags[mcu_next]); mcu_next += 1           # MCU primes the first fragment

    out = bytearray()
    groups_done = 0
    steps = 0
    while groups_done < len(frags):
        # MCU services the notify (stage next fragment) — bounded, never blocks
        if rx.dl_seq != last_seq:
            last_seq = rx.dl_seq
            if mcu_next < len(frags):
                rx.mcu_stage(frags[mcu_next]); mcu_next += 1
        qs = rx.read_queue()
        if qs == 0:
            rx.tick(); steps += 1
            if steps > max_steps:
                raise RuntimeError("stall")
            continue
        # read this data group frame-by-frame until Packet-End
        group = bytearray()
        end = False
        while not end:
            # let the pacer buffer frames if the queue ran dry mid-group
            guard = 0
            while rx.pf_queue == 0:
                rx.tick(); steps += 1; guard += 1
                if guard > max_steps:
                    raise RuntimeError("frame stall")
            pfx = rx.read_prefix()
            frame = bytes(rx.read_data() for _ in range(PKT))
            group += frame
            if pfx & 0x80:
                end = True
        # strip 10-byte DG header, take size24-5 body bytes
        size24 = (group[2] << 16) | (group[3] << 8) | group[4]
        body = group[10:10 + (size24 - 5)]
        out += body
        groups_done += 1
    return bytes(out), rx


def _selftest():
    import os
    # deterministic pseudo-program (multi-fragment, non-aligned tail)
    prog = bytes(((i * 73 + (i >> 5) * 31) & 0xFF) for i in range(0x14000 + 123))  # ~80KB+123
    got, rx = simulate_download(prog, chunk=0x8000, pace_period=4)
    assert got == prog, f"MISMATCH len got={len(got)} want={len(prog)} firstdiff={next((i for i in range(min(len(got),len(prog))) if got[i]!=prog[i]), 'tail')}"
    # also a tiny program (single sub-fragment, < 22 bytes tail handling)
    for sz in (1, 21, 22, 23, 0x7FFF, 0x8000, 0x8001, 0x20000):
        p = bytes((i & 0xFF) for i in range(sz))
        g, _ = simulate_download(p, chunk=0x8000, pace_period=2)
        assert g == p, f"MISMATCH at size {sz}: got {len(g)}"
    print("OK: receiver model round-trips programs byte-exact across sizes")
    print(f"  (validated 1..0x20000 bytes; multi-fragment 0x14000+123; overrun seen={rx.overrun})")


if __name__ == "__main__":
    _selftest()
