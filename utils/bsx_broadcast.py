#!/usr/bin/env python3
"""
bsx_broadcast.py — authors a BS-X (Satellaview) broadcast with 1 downloadable
PROGRAM, in the sd2snes page format (PSRAM 0x900000), by COPY-AND-PATCH of a
baseline bsxpage.bin.

Why it exists: the Town's "Receive Program" reads the Directory channel (LCI 0x122)
and lists the DownloadFiles whose schedule (month/day/window) contains the live
clock (Time Channel, channel 0, served by the FPGA). The Yakumono test pack only
has ITEMS (shop, no schedule) -> no program shows up. This generator injects 1
Folder "Files" with 1 DownloadFile scheduled for TODAY (all day), and adds the
FileID to the Town Status (LCI 0x123). RE verdict: CONTENT gap, not FPGA.

The serialization is BYTE-EXACT to SatellaWave (LuigiBlood) Program.cs ExportBSX:
  Directory header  -> Program.cs:1816-1820
  Folder            -> Program.cs:1827-1867
  DownloadFile      -> Program.cs:1872-1884 (common) + 2021-2086 (body+SCHEDULE)
  SCHEDULE (5 bytes)-> Program.cs:2056-2063
  TownStatus        -> Program.cs:2450-2488
  Channel Map       -> Program.cs:2562-2669 (only with --update-map)
  Data-Group header -> Program.cs:2687-2704 (SaveChannelFile, 1 fragment)

sd2snes page layout (each channel LCI N at 0x900000 + N*0x200), decoded from the
baseline: 0x00 'BSX ' + maker[8] + title[16] ; 0x32 sta ; 0x34 stb ; 0x48 = the
channel payload (10-byte Data-Group header + body). The wrapper (0x00-0x47) is
PRESERVED from the baseline (validated by the loader). Payload ceiling = 0x200-0x48 = 440 B.

Usage:
  bsx_broadcast.py -o out.bin [--baseline bsxpage.bin]
      [--name NAME] [--desc DESC] [--date YYYY-MM-DD] [--window HH:MM-HH:MM]
      [--folder BUILDING_NAME] [--dest 1|2|3] [--autostart 0|1|2] [--update-map]
  bsx_broadcast.py --decode bsxpage.bin        # only inspects (Directory/TownStatus/Map)

Then: push via USB with  usb_bsx.py feed out.bin   and test "Receive Program".
"""
import sys, os, argparse, datetime

PAGE = 0x200
DATA_OFF = 0x48                  # start of the channel payload within the page
PAY_MAX = PAGE - DATA_OFF        # 440 bytes per channel (1 page) — bsx.v ceiling
LCI_WELCOME, LCI_DIR, LCI_TOWN, LCI_MAP = 0x121, 0x122, 0x123, 0x124
LCI_DATA0 = 0x125                # 1st LCI of the program data-channel pool
SVC_DOWNLOAD = 0x0103            # service_broadcast of downloadable program (SatellaWave Program.cs:390)
IMG_SIZE = 0x80000               # 512KB (the loader expects the full image)


def bsx_str(s, newline_to_cr=True):
    """Encodes like ConvertToBSXStringBytes (Program.cs:2758): Shift-JIS, \\n->0x0D."""
    if newline_to_cr:
        s = s.replace("\r\n", "\n").replace("\n", "\r")
    try:
        return s.encode("cp932")
    except UnicodeEncodeError:
        return s.encode("cp932", "replace")


def pack_sched(month, day, hs, ms, he, me):
    """5-byte SCHEDULE (Program.cs:2056-2063). all-day default=10 08 00 17 ec."""
    return bytes([
        (month & 0x0F) << 4,
        (day & 0x1F) << 3,
        ((hs & 0x1F) << 3) | ((ms >> 3) & 7),
        ((ms & 7) << 5) | (he & 0x1F),
        (me & 0x3F) << 2,
    ])


def unpack_sched(b):
    m = b[0] >> 4; d = b[1] >> 3
    hs = b[2] >> 3; ms = ((b[2] & 7) << 3) | (b[3] >> 5)
    he = b[3] & 0x1F; me = b[4] >> 2
    return m, d, hs, ms, he, me


def datagroup(body):
    """Wraps the body in the 10-byte Data-Group header (1 fragment, Program.cs:2687)."""
    n = len(body) + 5
    hdr = bytes([0, 0, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF, 0x01, 0x01, 0, 0, 0])
    return hdr + body


# ---- channel-body builders (byte-exact to SatellaWave) -----------------------

def build_directory(dir_id, folder_name, folder_msg, files):
    """1 Folder (building, purpose=Files type=Building) with N DownloadFiles."""
    out = bytearray()
    out += bytes([dir_id, 1, 0, 0, 0])                 # DirID, FolderCount=1, 3x unused
    # --- Folder (Program.cs:1827-1867) ---
    out += bytes([0, len(files)])                      # Flags=0, FileCount
    nm = bsx_str(folder_name)[:20]; out += nm + b"\x00" * (20 - len(nm)); out += b"\x00"
    msg = bsx_str(folder_msg); out += bytes([len(msg) + 1]) + msg + b"\x00"
    out += bytes([0x00])                               # FolderType = purpose0|type0 (Files/Building)
    out += bytes([0x01, 0, 0, 0x00, 0, 0, 0])          # FolderID=1, 2x unk, Mugshot=0, 3x unk
    # --- DownloadFiles (Program.cs:1872-2086) ---
    for f in files:
        out += bytes([f["fileid"], 0])                 # FileID, Check
        nm = bsx_str(f["name"])[:20]; out += nm + b"\x00" * (20 - len(nm)); out += b"\x00"
        desc = bsx_str(f["desc"]); out += bytes([len(desc) + 1]) + desc + b"\x00"
        svc, prog, sz = f["svc"], f["prog"], f["filesize"]
        out += bytes([(svc >> 8) & 0xFF, svc & 0xFF, (prog >> 8) & 0xFF, prog & 0xFF])
        out += bytes([(sz >> 16) & 0xFF, (sz >> 8) & 0xFF, sz & 0xFF, 0, 0, 0])
        flags = (0 if f.get("also_at_home", False) else 1) << 2 | (1 if f.get("streamed") else 0) << 3
        out += bytes([flags, 0])
        out += bytes([(f.get("autostart", 0) & 3) | ((f["dest"] & 3) << 2), 0, 0])
        out += f["sched"]                              # 5 bytes
        out += bytes([0, 0, 0, 0, 0, 0])               # include(4)=none + 2x unk
    return bytes(out)


def build_townstatus(ts_id, dir_id, file_ids, base_body=None):
    """TownStatus (Program.cs:2450-2488). Preserves the baseline's NPC/town setup if given."""
    out = bytearray(24)
    out[0] = 0                                         # Flag
    out[1] = ts_id                                     # Town Status ID
    out[2] = dir_id                                    # Directory ID (== Directory's DirID)
    # 3..6 = 0 ; 7 = radio<<6|apu<<4 ; 8 = 0 ; 9..16 = NPC ; 17..18 = townsetup ; 19..22 = 0
    if base_body and len(base_body) >= 19:
        out[7] = base_body[7]                          # apu/radio (keeps sound/effects)
        out[9:17] = base_body[9:17]                    # NPC flags
        out[17:19] = base_body[17:19]                  # fountain/season
    else:
        out[7] = 0x30                                  # apu=Effects/MusicB (like baseline)
    out[23] = len(file_ids)                            # Number of file IDs
    out += bytes(file_ids)
    return bytes(out)


def build_channelmap(services):
    """Channel Map BSX0124 (Program.cs:2562-2669). services={svc:[(type,prog,timeout,autodest,lci)]}."""
    body = bytearray(b"SF\x00\x00\x00\x00")
    body.append(len(services))                         # ServiceCount
    body.append(sum(body) & 0xFF)                      # checksum of the header's 7 bytes
    for svc, chans in services.items():
        body += bytes([(svc >> 8) & 0xFF, svc & 0xFF, len(chans)])
        for (typ, prog, timeout, autodest, lci) in chans:
            body += bytes([typ, (prog >> 8) & 0xFF, prog & 0xFF, 0, 0, 0, 0, 0])
            body += bytes([(timeout >> 8) & 0xFF, timeout & 0xFF, autodest])
            body += bytes([lci & 0xFF, (lci >> 8) & 0xFF])   # LCI little-endian here
    size = len(body)
    return bytes([0, 0, (size >> 16) & 0xFF, (size >> 8) & 0xFF, size & 0xFF]) + bytes(body)


def channelmap_append_service(img, service, records):
    """Appends ONE service block to the Channel Map (page 0x124) IN-PLACE (proven in the spike).
    records = [(type, prog, timeout, auto_dest, lci), ...]. Keeps the existing blocks,
    increments ServiceCount, recomputes checksum (over SF[0..6]) and the size24. LCI little-endian."""
    cm = LCI_MAP * PAGE; SF = cm + 0x4d; szo = cm + 0x4a
    if bytes(img[SF:SF + 2]) != b"SF":
        raise SystemExit("!! Channel Map missing 'SF' signature at 0x124+0x4d")
    old = (img[szo] << 16) | (img[szo + 1] << 8) | img[szo + 2]
    block = bytearray([(service >> 8) & 0xFF, service & 0xFF, len(records)])
    for (typ, prog, timeout, auto_dest, lci) in records:
        block += bytes([typ, (prog >> 8) & 0xFF, prog & 0xFF, 0, 0, 0, 0, 0,
                        (timeout >> 8) & 0xFF, timeout & 0xFF, auto_dest, lci & 0xFF, (lci >> 8) & 0xFF])
    if (SF + old + len(block)) - cm > PAGE:
        raise SystemExit(f"!! Channel Map overflowed the page ({len(records)} records too many)")
    img[SF + old:SF + old + len(block)] = block
    img[SF + 6] += 1                                 # ServiceCount++
    img[SF + 7] = sum(img[SF:SF + 7]) & 0xFF         # checksum = sum of SF[0..6]
    new = old + len(block)
    img[szo] = (new >> 16) & 0xFF; img[szo + 1] = (new >> 8) & 0xFF; img[szo + 2] = new & 0xFF


def build_data_channel(img, lci, title, body, template_lci=LCI_TOWN):
    """Writes a program's DATA channel into the `lci` page: clones the 'BSX ' wrapper from a
    page with data, sets the title, the Data-Group header and the body (1st fragment). Body > 0x1AE
    (440-10-... = 1 page) is truncated in this page; multi-page streaming is done by the server."""
    import math
    dc = lci * PAGE; tpl = template_lci * PAGE
    img[dc:dc + 0x48] = img[tpl:tpl + 0x48]          # clones wrapper (BSX/maker/sta/stb)
    t = bsx_str(title)[:16]; img[dc + 0x10:dc + 0x20] = t + b"\x00" * (16 - len(t))
    first = min(len(body), PAGE - 0x52)
    fc = max(1, math.ceil(len(body) / 32768)) if body else 1
    img[dc + 0x48] = 0; img[dc + 0x49] = 0           # DGID, continuity
    img[dc + 0x4a] = (first >> 16) & 0xFF; img[dc + 0x4b] = (first >> 8) & 0xFF; img[dc + 0x4c] = first & 0xFF
    img[dc + 0x4d] = 1; img[dc + 0x4e] = fc & 0xFF    # fixed, fragment count
    img[dc + 0x4f] = 0; img[dc + 0x50] = 0; img[dc + 0x51] = 0  # offset24 = 0
    img[dc + 0x52:dc + 0x52 + first] = body[:first]


def make_streaming_image(baseline_path, nslots=8):
    """'streaming-ready' image: base channels + Channel Map with a POOL of `nslots` program slots
    (service 0x0103, prog 0x0000..0x0(N-1)00 -> LCI 0x0125..0x0125+N-1). Initial Directory EMPTY
    (0 folders); the programs enter live via the Directory without touching the Channel Map (which caches)."""
    img = bytearray(open(baseline_path, "rb").read())
    need = (LCI_DATA0 + nslots + 1) * PAGE
    if len(img) < need:
        img += bytearray(need - len(img))
    records = [(5, i * 0x100, 10, 0x04, LCI_DATA0 + i) for i in range(nslots)]  # type5, timeout=10 (SatellaWave default; 1 fails reception of a real program), autostart=No dest=PSRAM
    channelmap_append_service(img, SVC_DOWNLOAD, records)
    for i in range(nslots):
        build_data_channel(img, LCI_DATA0 + i, f"Slot {i:02d}", b"")            # empty data channels
    # Empty Directory (new DirID, 0 folders) + Town Status without FileIDs
    patch_page(img, LCI_DIR, datagroup(bytes([1, 0, 0, 0, 0])))
    ts_base = bytes(img[LCI_TOWN * PAGE + 0x52:LCI_TOWN * PAGE + 0x52 + 19])
    patch_page(img, LCI_TOWN, datagroup(build_townstatus(1, 1, [], base_body=ts_base)))
    return bytes(img)


def compose_live_state(stream_img, active, dirid, folder="News", fmsg="Broadcast"):
    """Composes a LIVE STATE over the streaming-ready image, WITHOUT touching the Channel Map.
    active = [{slot, name, desc, body, sched}] (slot 0..nslots-1; prog=slot*0x100; LCI=0x125+slot;
    FileID=slot+1). Rewrites Directory (0x122, new DirID -> Town re-reads) + Town Status (0x123,
    active FileIDs) + the body of the active data channels. Returns (new_img, changed_lcis)."""
    img = bytearray(stream_img)
    files = []
    for p in active:
        s = p["slot"]
        files.append(dict(fileid=s + 1, name=p["name"], desc=p.get("desc", ""),
                          svc=SVC_DOWNLOAD, prog=s * 0x100, filesize=len(p.get("body", b"")) or 0x8000,
                          dest=1, autostart=0, sched=p["sched"]))
        build_data_channel(img, LCI_DATA0 + s, p["name"], p.get("body", b""))   # data-channel body
    patch_page(img, LCI_DIR, datagroup(build_directory(dirid, folder, fmsg, files)))
    ts_base = bytes(stream_img[LCI_TOWN * PAGE + 0x52:LCI_TOWN * PAGE + 0x52 + 19])
    patch_page(img, LCI_TOWN, datagroup(build_townstatus(dirid, dirid, [f["fileid"] for f in files], base_body=ts_base)))
    changed = [LCI_DIR, LCI_TOWN] + [LCI_DATA0 + p["slot"] for p in active]
    return bytes(img), changed


# ---- page patch --------------------------------------------------------------

def page_payload(img, lci):
    base = lci * PAGE
    return bytes(img[base + DATA_OFF: base + PAGE])


def patch_page(img, lci, payload):
    """Keeps the page's 0x00-0x47 wrapper; rewrites 0x48.. with `payload`; zeroes the rest."""
    if len(payload) > PAY_MAX:
        raise SystemExit(f"!! channel 0x{lci:X}: payload {len(payload)}B > ceiling {PAY_MAX}B "
                         f"(bsx.v serves 1 page/channel; shorten name/desc).")
    base = lci * PAGE
    img[base + DATA_OFF: base + PAGE] = payload + b"\x00" * (PAY_MAX - len(payload))


# ---- decode (inspection) -----------------------------------------------------

def decode(img):
    print(f"image: {len(img)} bytes ({len(img)//PAGE} pages)")
    for lci, label in ((LCI_WELCOME, "Welcome"), (LCI_DIR, "Directory"),
                       (LCI_TOWN, "TownStatus"), (LCI_MAP, "ChannelMap")):
        base = lci * PAGE
        title = bytes(img[base + 0x10: base + 0x20]).split(b"\x00")[0].decode("latin1", "replace")
        pay = page_payload(img, lci)
        print(f"\n[LCI 0x{lci:X}] {label}  title={title!r}  payload[:24]={pay[:24].hex(' ')}")
        if lci == LCI_DIR:
            body = pay[10:]  # skip DG header
            print(f"  DirID={body[0]} Folders={body[1]}")
        if lci == LCI_TOWN:
            body = pay[10:]
            if len(body) >= 24:
                print(f"  Flag={body[0]} TSID={body[1]} DirID={body[2]} nFileIDs={body[23]} "
                      f"FileIDs={list(body[24:24+body[23]])}")
            else:
                print(f"  (short TownStatus, {len(body)}B — no FileID table)")


# ---- main --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Authors a BS-X broadcast with 1 program (sd2snes page format).")
    ap.add_argument("--baseline", default="/tmp/bsxpage_baseline.bin")
    ap.add_argument("-o", "--out")
    ap.add_argument("--decode", metavar="IMG", help="only inspects Directory/TownStatus/Map")
    ap.add_argument("--name", default="Test Program", help="program name (<=20)")
    ap.add_argument("--desc", default="Fed via USB", help="program description")
    ap.add_argument("--folder", default="sd2snes", help="building/Folder name (<=20)")
    ap.add_argument("--fmsg", default="Receive me!", help="building message")
    ap.add_argument("--date", help="YYYY-MM-DD (default: today, host)")
    ap.add_argument("--wildcard", action="store_true", help="month=0/day=0 (any date; INFERRED, not confirmed)")
    ap.add_argument("--window", default="00:00-23:59", help="HH:MM-HH:MM (default all day)")
    ap.add_argument("--dest", type=int, default=3, help="0=WRAM 1=PSRAM 2=FLASHfull 3=FLASHfree(default)")
    ap.add_argument("--autostart", type=int, default=0, help="0=No(list) 1=Optional 2=Yes(autoboot)")
    ap.add_argument("--svc", type=lambda x: int(x, 0), default=0x0103, help="service_broadcast (default 0x0103)")
    ap.add_argument("--prog", type=lambda x: int(x, 0), default=0x0020, help="program_number")
    ap.add_argument("--lci", type=lambda x: int(x, 0), default=0x0125, help="LCI of the program's data channel")
    ap.add_argument("--dirid", type=int, default=2, help="Directory/TownStatus ID (change to force a re-read)")
    ap.add_argument("--update-map", action="store_true", help="adds the program to Channel Map 0x124")
    args = ap.parse_args()

    if args.decode:
        decode(bytearray(open(args.decode, "rb").read())); return
    if not args.out:
        raise SystemExit("!! missing -o OUT (or use --decode)")
    if not os.path.isfile(args.baseline):
        raise SystemExit(f"!! baseline not found: {args.baseline} (download with usb_get /sd2snes/bsxpage.bin)")

    img = bytearray(open(args.baseline, "rb").read())
    if len(img) < IMG_SIZE:
        img += b"\x00" * (IMG_SIZE - len(img))

    # window / date
    d = datetime.date.fromisoformat(args.date) if args.date else datetime.date.today()
    try:
        a, b = args.window.split("-"); h1, m1 = map(int, a.split(":")); h2, m2 = map(int, b.split(":"))
    except Exception:
        raise SystemExit(f"!! invalid --window: {args.window!r} (use HH:MM-HH:MM)")
    mo, dy = (0, 0) if args.wildcard else (d.month, d.day)
    sched = pack_sched(mo, dy, h1, m1, h2, m2)

    prog = {
        "fileid": 1, "name": args.name, "desc": args.desc,
        "svc": args.svc, "prog": args.prog, "filesize": 0,
        "dest": args.dest, "autostart": args.autostart, "sched": sched,
    }
    # Directory (0x122)
    dir_body = build_directory(args.dirid, args.folder, args.fmsg, [prog])
    patch_page(img, LCI_DIR, datagroup(dir_body))
    # Town Status (0x123) — keeps the baseline's NPC/town setup, adds the FileID
    ts_base = page_payload(img, LCI_TOWN)[10:]
    ts_body = build_townstatus(args.dirid, args.dirid, [1], base_body=ts_base)
    patch_page(img, LCI_TOWN, datagroup(ts_body))
    # Channel Map (0x124) — optional
    if args.update_map:
        autodest = (args.autostart & 3) | ((args.dest & 3) << 2)
        services = {args.svc: [(5, args.prog, 10, autodest, args.lci)]}  # type 5 = DownloadFile
        patch_page(img, LCI_MAP, build_channelmap(services))

    if len(img) != IMG_SIZE:
        img = img[:IMG_SIZE] + b"\x00" * max(0, IMG_SIZE - len(img))
    open(args.out, "wb").write(img[:IMG_SIZE])

    m, dy, hs, ms, he, me = unpack_sched(sched)
    print(f"OK -> {args.out}  ({IMG_SIZE} bytes)")
    print(f"  program: name={args.name!r} fileid=1 svc=0x{args.svc:04X} prog=0x{args.prog:04X} "
          f"dest={args.dest} autostart={args.autostart}")
    print(f"  schedule: {sched.hex(' ')}  => month={m} day={dy} window {hs:02d}:{ms:02d}-{he:02d}:{me:02d}")
    print(f"  Directory body={len(dir_body)}B  TownStatus body={len(ts_body)}B  "
          f"(ceiling/channel {PAY_MAX}B)  update_map={args.update_map}")
    print(f"  feed:  python3 utils/usb_bsx.py feed {args.out}")


if __name__ == "__main__":
    main()
