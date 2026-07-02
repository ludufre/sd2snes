#!/usr/bin/env python3
"""
bsx_server.py — 24h REAL-TIME BS-X (Satellaview) broadcast service for the sd2snes.

Model (proven on hardware + bsnes-plus): the broadcast lives in PSRAM 0x900000; the Channel Map is read
once at boot and CACHED for the whole session (hence a FIXED POOL of slots, seeded via `bsx_stage_a.py
install`). The Directory (catalog of the buildings) is read once per DirID CHANGE, and each change TRIGGERS
the NORMAL Satellaview RECEPTION: St.GIGA "about to begin" logo + the town reloads (the character returns to
start). This is AUTHENTIC (identical on hardware, on bsnes-plus and in real videos), it is NOT a bug and has
NO flag to suppress. The cart does NOT restart (no power-cycle); it is the town GAME that does this soft-restart.
=> the schedule windows should be in HOURS (few receptions/day, like St.GIGA), never in seconds.

QUIET PATH (no restart, proven on-screen + WRAM 7EA21D): the building LISTS every cached Directory entry
(the list = the fixed pool, only changes on restart), BUT the Town Status File-IDs gate the
RECEPTIBILITY -- who is "on air" (receivable) now. Trying to receive an entry outside the File-ID gives
"cannot receive now"; inside, it receives (proven both ways). Bumping only the Town Status ID with the
DirID UNCHANGED swaps the "on air" QUIETLY (no restart, no re-reading the Directory). => alternative without
constant restarts: FIXED pool of programs in the Directory (1 restart to cache) + schedule = toggle of the
Town Status File-IDs. Restart only for a new entry outside the pool. TODO: this server still
uses a DirID bump on every swap (= 1 reception/restart per swap) -- migrate to the File-IDs toggle to
zero out restarts (see the FPGA receiver / bsx.v).

Driven by a SCHEDULE (CSV, SatellaView+ ScheduleData.csv style): each row = a program on air
in a time window. The loop evaluates the host clock, and when the "on air now" set changes,
it recomposes and pushes the state. Abstract transport: USB today (PUT space=SNES), ESP32 serial later.

CSV schedule (`utils/bsx_schedule.csv`):
    start,end,title,folder,datafile
    08:00,11:00,Demo Manha,1,/caminho/programa.bin
    11:00,14:00,Demo Tarde,1,
  - start/end = HH:MM (host clock; window may cross midnight).
  - folder = building FolderID (1=News). datafile = program content (optional; empty=stub).

Prerequisites:
  - `bsx_stage_a.py install` already run (streaming-ready image at /tmp/bsxpage_stream.bin + on the SD) and the
    Town booted once (Channel Map seeded).
  - The device's BS-X RTC matching --rtc-date (the Directory window uses that date, whole day).

Usage:
    bsx_server.py [schedule.csv] [--rtc-date 1997-03-01] [--poll 5] [--once] [--transport usb|esp]
"""
import sys, os, csv, time, datetime, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsx_broadcast as bx
import usb_bsx as u

PAGE = 0x200
STREAM_IMG = "/tmp/bsxpage_stream.bin"
DIRID_FILE = "/tmp/bsx_dirid"        # persistent DirID counter (shared with bsx_stage_a.py)
DEFAULT_SCHEDULE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bsx_schedule.csv")


def next_dirid():
    """Ever-increasing DirID (persists across runs; wrap 1..250). Changing the DirID on each feed makes
    the Town reprocess the Directory; persisting avoids colliding with the value the Town already cached."""
    n = 1
    if os.path.exists(DIRID_FILE):
        try:
            n = int(open(DIRID_FILE).read().strip())
        except ValueError:
            n = 1
    n = (n % 250) + 1
    open(DIRID_FILE, "w").write(str(n))
    return n


# ---- transport (abstracts USB -> ESP32) --------------------------------------

class Transport:
    def write_page(self, lci, data):
        raise NotImplementedError


class UsbTransport(Transport):
    """PUT space=SNES via usb_bsx (proven in the spike)."""
    def write_page(self, lci, data):
        u.write_mem(0x900000 + lci * PAGE, data)


class EspSerialTransport(Transport):
    """Stage C (design): new opcode UP_OP_PUT_SRAM in uart_proto -> sram_writeblock. Not implemented."""
    def write_page(self, lci, data):
        raise NotImplementedError("ESP32 serial = Stage C (design only). Use --transport usb for now.")


def make_transport(name):
    return {"usb": UsbTransport, "esp": EspSerialTransport}[name]()


# ---- schedule ---------------------------------------------------------------

def load_schedule(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if not r.get("title"):
                continue
            rows.append(dict(start=r["start"].strip(), end=r["end"].strip(),
                             title=r["title"].strip(), folder=int(r.get("folder", 1) or 1),
                             datafile=(r.get("datafile") or "").strip()))
    return rows


def _hm(s):
    h, m = s.split(":"); return int(h) * 60 + int(m)


def active_now(rows, now):
    """Programs whose window [start,end) contains the current time (crosses midnight if start>end)."""
    cur = now.hour * 60 + now.minute
    out = []
    for r in rows:
        s, e = _hm(r["start"]), _hm(r["end"])
        on = (s <= cur < e) if s <= e else (cur >= s or cur < e)
        if on:
            out.append(r)
    return out


# ---- composer + push --------------------------------------------------------

def compose_and_push(stream, rows_active, sched, dirid, tr, nslots=8):
    """Maps the active ones onto slots (by index), composes and pushes the changed pages."""
    active = []
    for i, r in enumerate(rows_active[:nslots]):
        body = b""
        if r["datafile"] and os.path.isfile(r["datafile"]):
            body = open(r["datafile"], "rb").read()
        active.append(dict(slot=i, name=r["title"], desc=r["title"], body=body, sched=sched))
    img, changed = bx.compose_live_state(stream, active, dirid)
    for lci in changed:
        tr.write_page(lci, img[lci * PAGE:(lci + 1) * PAGE])
    return changed


def main():
    ap = argparse.ArgumentParser(description="24h real-time BS-X broadcast service (sd2snes).")
    ap.add_argument("schedule", nargs="?", default=DEFAULT_SCHEDULE, help="CSV schedule (default utils/bsx_schedule.csv)")
    ap.add_argument("--rtc-date", default="1997-03-01", help="BS-X RTC date (matches the Directory window)")
    ap.add_argument("--poll", type=float, default=5.0, help="clock evaluation interval (s)")
    ap.add_argument("--once", action="store_true", help="evaluate/push once and exit (test)")
    ap.add_argument("--demo", action="store_true",
                    help="DEMO ONLY: cycles 1 schedule program per poll; each swap = DirID bump = "
                         "1 St.GIGA reception + town restart. Do NOT use with a short poll (annoying); the "
                         "real frequency should be in hours. See the QUIET PATH in the module docstring.")
    ap.add_argument("--transport", default="usb", choices=["usb", "esp"])
    ap.add_argument("--stream-img", default=STREAM_IMG, help="streaming-ready image (from bsx_stage_a install)")
    args = ap.parse_args()

    if not os.path.isfile(args.stream_img):
        raise SystemExit(f"!! {args.stream_img} missing — run 'bsx_stage_a.py install' first.")
    if not os.path.isfile(args.schedule):
        raise SystemExit(f"!! schedule not found: {args.schedule}")

    d = datetime.date.fromisoformat(args.rtc_date)
    sched = bx.pack_sched(d.month, d.day, 0, 0, 23, 59)   # window = RTC date, whole day
    tr = make_transport(args.transport)
    rows = load_schedule(args.schedule)
    stream = open(args.stream_img, "rb").read()
    print(f"bsx_server: {len(rows)} schedule entries; RTC={args.rtc_date}; transport={args.transport}; "
          f"poll={args.poll}s")

    last_key, tick = None, 0
    while True:
        now = datetime.datetime.now()
        if args.demo:
            act = [rows[tick % len(rows)]] if rows else []   # cycles 1 per poll
            tick += 1
        else:
            act = active_now(rows, now)
        key = tuple(sorted(r["title"] for r in act))
        if key != last_key:
            dirid = next_dirid()
            changed = compose_and_push(stream, act, sched, dirid, tr)
            print(f"[{now:%H:%M:%S}] on air: {list(key) or '(nothing)'}  (DirID={dirid}, "
                  f"pages {[hex(c) for c in changed]})")
            last_key = key
        if args.once:
            break
        time.sleep(args.poll)


if __name__ == "__main__":
    main()
