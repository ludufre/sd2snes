#!/usr/bin/env python3
"""
bsx_stage_a.py — DE-RISK of the "fixed Channel Map + live Directory" model: proves that programs
go on/off the air WITHOUT rebooting the Town (the Channel Map caches at boot; the Directory re-reads live).

Flow:
  1)  bsx_stage_a.py install     # builds the streaming-ready image + writes it to the SD; then BOOT a .bs once
  2)  bsx_stage_a.py a           # LIVE feed: program A in slot 0  -> should appear in the News building
  3)  bsx_stage_a.py b           # LIVE feed: program B in slot 1 (A removed) -> A disappears, B appears
  4)  bsx_stage_a.py empty       # LIVE feed: no program  -> everything disappears

The Channel Map (pool of 8 slots, service 0x0103) is only laid down at `install`+boot. The a/b/empty states
touch only the Directory (0x122, new DirID on each feed -> Town reprocesses) + Town Status (0x123) + the data
channel of the active slot (0x125+slot). They NEVER rewrite the Channel Map -> no reboot.

Pre: the device's BS-X RTC must match the schedule window (--date, default 1997-03-01 which the
user fixed in the firmware menu). /tmp/bsxpage_baseline.bin = the original bsxpage.bin (backup).
"""
import sys, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bsx_broadcast as bx
import usb_bsx as u

BASELINE = "/tmp/bsxpage_baseline.bin"
STREAM_IMG = "/tmp/bsxpage_stream.bin"
SD_PATH = "/sd2snes/bsxpage.bin"
DIRID_FILE = "/tmp/bsx_dirid"
NSLOTS = 8

# RTC the user fixed in the menu (1997-03-01). Schedule = that date, whole day.
SCHED_MONTH, SCHED_DAY = 3, 1


def sched():
    return bx.pack_sched(SCHED_MONTH, SCHED_DAY, 0, 0, 23, 59)


def states():
    return {
        "empty": [],
        "a": [dict(slot=0, name="Programa A", desc="Slot 0 ao vivo via USB", body=b"AAAA" * 64, sched=sched())],
        "b": [dict(slot=1, name="Programa B", desc="Slot 1 ao vivo via USB", body=b"BBBB" * 64, sched=sched())],
    }


def next_dirid():
    n = 2
    if os.path.exists(DIRID_FILE):
        try:
            n = int(open(DIRID_FILE).read().strip()) + 1
        except ValueError:
            n = 2
    if n > 250:
        n = 2
    open(DIRID_FILE, "w").write(str(n))
    return n


def feed_pages(img, changed):
    for lci in changed:
        addr = 0x900000 + lci * 0x200
        page = img[lci * 0x200:(lci + 1) * 0x200]
        u.write_mem(addr, page)
        print(f"  fed LCI 0x{lci:04X} -> {addr:#08x} ({len(page)}B)")


def cmd_install():
    img = bx.make_streaming_image(BASELINE, nslots=NSLOTS)
    open(STREAM_IMG, "wb").write(img)
    print(f"streaming image: {len(img)}B (Channel Map pool of {NSLOTS} slots) -> {STREAM_IMG}")
    print(f"writing to SD {SD_PATH} (original backup at {BASELINE}) ...")
    size, _ok = u.put_file(STREAM_IMG, SD_PATH)
    print(f"PUT: {size}B -> {SD_PATH}")
    open(DIRID_FILE, "w").write("1")
    print(">>> NOW BOOT a .bs (lays down the Channel Map). Then: bsx_stage_a.py a")


def cmd_state(name):
    if not os.path.exists(STREAM_IMG):
        raise SystemExit("!! run 'install' first")
    active = states()[name]
    dirid = next_dirid()
    img, changed = bx.compose_live_state(open(STREAM_IMG, "rb").read(), active, dirid)
    print(f"state '{name}': {len(active)} program(s), DirID={dirid}, schedule {SCHED_MONTH:02d}/{SCHED_DAY:02d}")
    feed_pages(img, changed)
    print(">>> check the NEWS building (no reboot).")


def main():
    a = sys.argv[1:]
    if not a or a[0] in ("-h", "--help"):
        print(__doc__); return
    if a[0] == "install":
        cmd_install()
    elif a[0] in states():
        cmd_state(a[0])
    else:
        raise SystemExit(f"unknown command: {a[0]}\n{__doc__}")


if __name__ == "__main__":
    main()
