#!/usr/bin/env python3
"""
bsx_soundlink.py — OPTION A: pushes a BS-X program "on the air now" and lets the Town AUTO-RECEIVE
(the classic "The program is about to begin. One moment please..." with the St.GIGA logo -> reception ->
saves to the Memory Pack). PUSH / scheduled model (SoundLink style), opposite of the on-demand catalog
in bsx_server.py (option B).

When to use: you want the program to START on its own at the scheduled time (the Town pulls the player into reception).
Each push triggers the reception screen. In a real service, it would be 1x per scheduled time (few/day).

Pre: 'bsx_stage_a.py install' done (streaming-ready image on the SD + /tmp/bsxpage_stream.bin) and the Town
booted 1x. BS-X RTC matching --rtc-date.

Usage:
  bsx_soundlink.py <title> [--data prog.bin] [--slot 0] [--rtc-date 1997-03-01]   # single push, now
  bsx_soundlink.py --schedule grid.csv [--poll 60] [--rtc-date ...]               # follows the grid
"""
import sys, os, time, datetime, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsx_broadcast as bx
import bsx_server as srv     # reuses transport, grid, next_dirid, compose_and_push

STREAM_IMG = "/tmp/bsxpage_stream.bin"


def push_one(stream, title, body, slot, sched, tr):
    active = [dict(slot=slot, name=title, desc=title, body=body, sched=sched)]
    img, changed = bx.compose_live_state(stream, active, srv.next_dirid())
    for lci in changed:
        tr.write_page(lci, img[lci * 0x200:(lci + 1) * 0x200])
    print(f"  push '{title}' (slot {slot}) -> Town should show 'about to begin' and receive. "
          f"pages {[hex(c) for c in changed]}")


def main():
    ap = argparse.ArgumentParser(description="OPTION A: SoundLink push (auto-reception 'about to begin').")
    ap.add_argument("title", nargs="?", help="program title (single push)")
    ap.add_argument("--data", help="program content file (optional)")
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--schedule", help="CSV grid (continuous mode)")
    ap.add_argument("--rtc-date", default="1997-03-01")
    ap.add_argument("--poll", type=float, default=60.0)
    ap.add_argument("--transport", default="usb", choices=["usb", "esp"])
    args = ap.parse_args()

    if not os.path.isfile(STREAM_IMG):
        raise SystemExit("!! run 'bsx_stage_a.py install' first.")
    d = datetime.date.fromisoformat(args.rtc_date)
    sched = bx.pack_sched(d.month, d.day, 0, 0, 23, 59)
    tr = srv.make_transport(args.transport)
    stream = open(STREAM_IMG, "rb").read()

    if args.schedule:
        rows = srv.load_schedule(args.schedule)
        last = None
        while True:
            act = srv.active_now(rows, datetime.datetime.now())
            key = tuple(sorted(r["title"] for r in act))
            if key != last and act:
                r = act[0]
                body = open(r["datafile"], "rb").read() if r["datafile"] and os.path.isfile(r["datafile"]) else b""
                push_one(stream, r["title"], body, 0, sched, tr)
                last = key
            time.sleep(args.poll)
    else:
        if not args.title:
            raise SystemExit("usage: bsx_soundlink.py <title> [--data prog.bin]")
        body = open(args.data, "rb").read() if args.data and os.path.isfile(args.data) else b""
        push_one(stream, args.title, body, args.slot, sched, tr)


if __name__ == "__main__":
    main()
