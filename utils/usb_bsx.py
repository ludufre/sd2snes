#!/usr/bin/env python3
"""
usb_bsx.py — feeds the BS-X broadcast/pages into PSRAM via USB (FxPakPro).

WRITE counterpart of usb_read.py: uses PUT (opcode 1) with space=SNES (1), whose
address goes RAW into cmd_buffer[256..259] (big-endian) and is written by the firmware via
sram_writeblock() — the SAME path cheats/savestates use. The firmware confirms the
PUT with a 512-byte 'USBA' header BEFORE accepting the data (usbinterface.c:749-765),
so the I/O flow mirrors usb_put.py (file PUT).

The target region is the Memory Pack / BS-X page of the base cart (mapper 3), in PSRAM:
  BS_PACK_ADDR = 0x900000 .. +BS_PACK_SIZE (1 MB)   (src/memory.h)
BS-X page/channel N starts at 0x900000 + N*0x200 (address.v: 0x900000+{bs_page,off}).

For safety, feed/page/poke REFUSE to write outside [0x900000, 0xA00000) unless
you pass --force (avoids stomping other live PSRAM). The serial port only exists with the SNES
ON; we open with a short write_timeout -> a wedged USB server FAILS fast, does not hang.

Usage:
  usb_bsx.py feed <img.bin> [--addr 0x900000] [--delay 0.01]   # PUT of the whole image
  usb_bsx.py page <N> <data.bin> [--delay ...]                  # PUT at 0x900000 + N*0x200
  usb_bsx.py poke <addr> <hexbytes>                             # e.g. poke 0x900048 DEADBEEF
  usb_bsx.py verify <img.bin> [--addr 0x900000]                # read-back (GET) + diff
  usb_bsx.py fill <addr> <size> <byte> [--force] [--no-verify]  # fills region (read-back)
  usb_bsx.py read <addr> <size> [-o out.bin]                   # raw GET (like usb_read)

Spike examples (Stage 0.3):
  usb_bsx.py feed bsxpage_B.bin                 # swap the broadcast live
  usb_bsx.py verify bsxpage_B.bin               # confirm the write landed
  usb_bsx.py poke 0x900048 AA55AA55             # marker bytes at the data offset (0x48)
"""
import sys, os, glob, time, struct, serial

BLK = 512
BS_PACK_ADDR = 0x900000
BS_PACK_SIZE = 0x100000           # 1 MB  (src/memory.h: BS_PACK_SIZE)
BS_PAGE_SIZE = 0x200              # 512 B per page/channel (bsx.v: 0x900000 + N*0x200)
BS_END = BS_PACK_ADDR + BS_PACK_SIZE

# Port detection by VID:PID, inline so this utility is standalone. Without
# this, with an
# ESP32 (SNEStooth) plugged in alongside, the alphabetical glob would talk to the wrong chip.
SD2SNES_VID = 0x1209             # src/usbdesc.c
SD2SNES_PID = 0x5A22
_LAST_RESORT = "/dev/cu.usbmodemDEMO000000001"


def find_port(default=_LAST_RESORT):
    if os.environ.get("SD2SNES_PORT"):
        return os.environ["SD2SNES_PORT"]
    try:
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        for p in ports:
            if (p.vid, p.pid) == (SD2SNES_VID, SD2SNES_PID):
                return p.device
        for p in ports:
            hay = " ".join(filter(None, (p.product, p.description, p.manufacturer))).lower()
            if "sd2snes" in hay or "fxpak" in hay or "ikari" in hay:
                return p.device
    except Exception:
        pass
    cands = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    return cands[0] if cands else default


def mkcmd_mem(op, addr, size):
    """512B header for a memory op (GET/PUT) with space=SNES. addr/size BE."""
    b = bytearray(BLK); b[0:4] = b"USBA"
    b[4] = op    # 0=GET, 1=PUT
    b[5] = 1     # SPACE_SNES
    b[6] = 0     # flags
    b[252] = (size >> 24) & 0xFF; b[253] = (size >> 16) & 0xFF
    b[254] = (size >> 8) & 0xFF;  b[255] = size & 0xFF
    b[256] = (addr >> 24) & 0xFF; b[257] = (addr >> 16) & 0xFF
    b[258] = (addr >> 8) & 0xFF;  b[259] = addr & 0xFF
    return bytes(b)


def _rd(s, n, to=2.0):
    buf = b""; t = time.time()
    while len(buf) < n and time.time() - t < to:
        c = s.read(n - len(buf))
        if c: buf += c; t = time.time()
    return buf


def _check_region(addr, size, force):
    if force:
        return
    if addr < BS_PACK_ADDR or (addr + size) > BS_END:
        raise SystemExit(
            f"!! [{addr:#08x}, {addr + size:#08x}) outside the BS-X region "
            f"[{BS_PACK_ADDR:#08x}, {BS_END:#08x}).\n"
            f"   Use --force to write outside it (careful: stomps live PSRAM).")


def write_mem(addr, data, delay=0.01, force=False):
    """PUT space=SNES: writes `data` at `addr`. Mirrors the usb_put.py flow."""
    size = len(data)
    _check_region(addr, size, force)
    pad = (BLK - (size % BLK)) % BLK
    payload = data + b"\x00" * pad
    port = find_port()
    # write_timeout: a wedged USB server makes the write FAIL fast instead of hanging.
    s = serial.Serial(port, 9600, timeout=0.5, write_timeout=5); s.reset_input_buffer()
    s.write(mkcmd_mem(1, addr, size)); s.flush()
    resp = _rd(s, BLK, 2.0)   # device confirms with a 'USBA' header BEFORE the data
    if resp[:4] != b"USBA":
        s.close()
        raise SystemExit(
            f"!! device did not confirm the PUT (received {len(resp)}B, header={resp[:4]!r}).\n"
            f"   USB server probably wedged — POWER-CYCLE the SNES. (port {port})")
    try:
        for i in range(0, len(payload), BLK):
            s.write(payload[i:i + BLK]); s.flush()
            if delay:
                time.sleep(delay)
    except serial.SerialTimeoutException:
        s.close()
        raise SystemExit(
            "!! timeout writing to the device (USB server wedged mid-PUT).\n"
            "   POWER-CYCLE the SNES and try again.")
    _rd(s, BLK, 0.5)          # drain final response, if any (defensive)
    s.close()
    return size, port


def read_mem(addr, size):
    """GET space=SNES (like usb_read.py). Returns (err, bytes)."""
    port = find_port()
    s = serial.Serial(port, 9600, timeout=1.5, write_timeout=5); s.reset_input_buffer()
    s.write(mkcmd_mem(0, addr, size)); s.flush()
    hdr = _rd(s, BLK, 2.0)
    err = hdr[5] if len(hdr) >= 6 else 0xFF
    need = ((size + BLK - 1) // BLK) * BLK
    data = b""
    while len(data) < need:
        c = s.read(need - len(data))
        if not c:
            break
        data += c
    s.close()
    return err, data[:size]


def mkcmd_file(op, path, size=0):
    """512B header for a FILE op (space=FILE=0). path at offset 256, NUL-term."""
    b = bytearray(BLK); b[0:4] = b"USBA"; b[4] = op; b[5] = 0; b[6] = 0
    b[252] = (size >> 24) & 0xFF; b[253] = (size >> 16) & 0xFF
    b[254] = (size >> 8) & 0xFF;  b[255] = size & 0xFF
    pb = path.encode() + b"\x00"
    b[256:256 + len(pb)] = pb
    return bytes(b)


def put_file(local, remote):
    """PUT of a local FILE to the device's SD (space=FILE). `remote` = ABSOLUTE file
    path (e.g. /sd2snes/bsxpage.bin) -- sending a directory wedges the USB server."""
    if not os.path.isfile(local):
        raise SystemExit(f"!! local file not found: {local}")
    if not remote or not remote.startswith("/") or remote.endswith("/"):
        raise SystemExit(f"!! invalid destination: {remote!r} -- use an ABSOLUTE file path")
    data = open(local, "rb").read(); size = len(data)
    data += b"\x00" * ((BLK - (size % BLK)) % BLK)
    port = find_port()
    s = serial.Serial(port, 9600, timeout=0.5, write_timeout=5); s.reset_input_buffer()
    s.write(mkcmd_file(1, remote, size)); s.flush()
    resp = _rd(s, BLK, 2.0)
    if resp[:4] != b"USBA":
        s.close()
        raise SystemExit(f"!! device did not confirm the PUT (header={resp[:4]!r}) -- POWER-CYCLE the SNES")
    try:
        for i in range(0, len(data), BLK):
            s.write(data[i:i + BLK]); s.flush(); time.sleep(0.01)
    except serial.SerialTimeoutException:
        s.close()
        raise SystemExit("!! timeout on PUT (USB server wedged) -- POWER-CYCLE the SNES")
    _rd(s, BLK, 1.0); s.close()
    return size, True



# ---- subcomandos -----------------------------------------------------------

def _pop_opt(args, name, conv=str, default=None):
    if name in args:
        i = args.index(name); v = conv(args[i + 1]); del args[i:i + 2]; return v
    return default


def _pop_flag(args, name):
    if name in args:
        args.remove(name); return True
    return False


def cmd_feed(args):
    addr = _pop_opt(args, "--addr", lambda x: int(x, 0), BS_PACK_ADDR)
    delay = _pop_opt(args, "--delay", float, 0.01)
    force = _pop_flag(args, "--force")
    if not args:
        raise SystemExit("usage: usb_bsx.py feed <img.bin> [--addr 0x900000] [--delay 0.01] [--force]")
    path = args[0]
    if not os.path.isfile(path):
        raise SystemExit(f"!! file not found: {path}")
    data = open(path, "rb").read()
    size, port = write_mem(addr, data, delay=delay, force=force)
    print(f"feed: {size}B -> {addr:#08x} (port {port})")


def cmd_page(args):
    delay = _pop_opt(args, "--delay", float, 0.01)
    force = _pop_flag(args, "--force")
    if len(args) < 2:
        raise SystemExit("usage: usb_bsx.py page <N> <data.bin> [--delay ...] [--force]")
    n = int(args[0], 0); path = args[1]
    if not os.path.isfile(path):
        raise SystemExit(f"!! file not found: {path}")
    data = open(path, "rb").read()
    addr = BS_PACK_ADDR + n * BS_PAGE_SIZE
    size, port = write_mem(addr, data, delay=delay, force=force)
    print(f"page {n}: {size}B -> {addr:#08x} (port {port})")


def cmd_poke(args):
    force = _pop_flag(args, "--force")
    if len(args) < 2:
        raise SystemExit("usage: usb_bsx.py poke <addr> <hexbytes> [--force]")
    addr = int(args[0], 0)
    hexs = args[1].replace(" ", "").replace("0x", "")
    try:
        data = bytes.fromhex(hexs)
    except ValueError:
        raise SystemExit(f"!! invalid hex: {args[1]!r}")
    size, port = write_mem(addr, data, delay=0, force=force)
    print(f"poke: {data.hex()} -> {addr:#08x} ({size}B, port {port})")


def cmd_verify(args):
    addr = _pop_opt(args, "--addr", lambda x: int(x, 0), BS_PACK_ADDR)
    if not args:
        raise SystemExit("usage: usb_bsx.py verify <img.bin> [--addr 0x900000]")
    path = args[0]
    if not os.path.isfile(path):
        raise SystemExit(f"!! file not found: {path}")
    want = open(path, "rb").read()
    err, got = read_mem(addr, len(want))
    diffs = [i for i in range(min(len(want), len(got))) if want[i] != got[i]]
    print(f"verify: addr={addr:#08x} size={len(want)} err={err} got={len(got)}B "
          f"diffs={len(diffs)}")
    if diffs:
        for i in diffs[:8]:
            print(f"  @{addr + i:#08x}: want {want[i]:02X} got {got[i]:02X}")
        if len(diffs) > 8:
            print(f"  ... +{len(diffs) - 8} differing bytes")
        sys.exit(1)
    print("  OK (read-back identical)")


def cmd_fill(args):
    """Fills a REGION with a repeated byte (read-back verified per block).
    Main use: zero the download pack to 0xFF before transmitting (0x400000,
    outside the guard [0x900000,0xA00000) -> requires --force).  32KB chunk."""
    verify = not _pop_flag(args, "--no-verify")
    force = _pop_flag(args, "--force")
    if len(args) < 3:
        raise SystemExit("usage: usb_bsx.py fill <addr> <size> <byte> [--force] [--no-verify]\n"
                         "  e.g.: usb_bsx.py fill 0x400000 0x100000 0xFF --force")
    addr = int(args[0], 0); size = int(args[1], 0); val = int(args[2], 0) & 0xFF
    CHUNK = 0x8000
    fill = bytes([val]) * CHUNK
    done = 0
    while done < size:
        n = min(CHUNK, size - done)
        block = fill if n == CHUNK else bytes([val]) * n
        ok = False
        for _ in range(5):
            write_mem(addr + done, block, delay=0, force=force)
            if not verify:
                ok = True; break
            time.sleep(0.02)
            err, back = read_mem(addr + done, min(0x100, n))
            if not err and bytes(back[:min(0x100, n)]) == bytes([val]) * min(0x100, n):
                ok = True; break
        if not ok:
            raise SystemExit(f"!! fill FAILED at {addr + done:#08x} (5 attempts)")
        done += n
    print(f"fill: {size} bytes = {val:#04x} in [{addr:#08x}, {addr + size:#08x})"
          f"{' (verified)' if verify else ''}")


def cmd_read(args):
    out = _pop_opt(args, "-o")
    if len(args) < 2:
        raise SystemExit("usage: usb_bsx.py read <addr> <size> [-o out.bin]")
    addr = int(args[0], 0); size = int(args[1], 0)
    err, data = read_mem(addr, size)
    print(f"read: addr={addr:#08x} size={size} err={err} got={len(data)}B")
    if out:
        open(out, "wb").write(data); print(f"-> {out}")
    else:
        for i in range(0, min(len(data), 256), 16):
            row = data[i:i + 16]
            print(f"  {addr + i:06X}: " + " ".join(f"{x:02X}" for x in row))


CMDS = {"feed": cmd_feed, "page": cmd_page, "poke": cmd_poke, "fill": cmd_fill,
        "verify": cmd_verify, "read": cmd_read}


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__); return
    op = args[0]
    if op not in CMDS:
        raise SystemExit(f"unknown subcommand: {op!r}\n{__doc__}")
    CMDS[op](args[1:])


if __name__ == "__main__":
    main()
