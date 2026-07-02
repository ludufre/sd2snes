#!/usr/bin/env python3
"""
bsx_download.py — host-side USB "satellite" that drives a BS-X (Satellaview)
over-the-air program DOWNLOAD to a real sd2snes device.

This is the host counterpart to the device's NEW FPGA receiver (bsx.v BS-X RECEIVER
FSM) + the MCU bridge (src/bsx_dl.{c,h}).  Where bsnes-plus emulates the satellite,
here WE are the satellite over USB:

  - We push a broadcast CATALOG (Directory / Town Status / Channel Map / data channel)
    into the page window at PSRAM 0x900000 so the Town BIOS LISTS a downloadable
    program and the Channel Map says "save it to FLASH-Free" (dest=3).
  - We split the program into 32KB SatellaWave data-group FRAGMENTS and stream them
    one at a time into the download RING at 0x980000.  The FPGA receiver serves the
    Town's $218A/$218B/$218C queue/prefix/data from the ring; the MCU bridge
    (bsx_dl_service) relays our descriptor mailbox to the FPGA (opcode 0xf7) and
    publishes the FPGA drain-notify back to us (opcode 0xf8).

Protocol (matches src/bsx_dl.h BYTE-FOR-BYTE):
  Descriptor mailbox @ 0x9A0000 (13 bytes, host -> MCU):
    off 0..3  'B' 'X' 'D' 'L'   magic
    off 4     seq                bump by 1 on EVERY write; MCU applies on change
    off 5     ctl                bit0=ARM(0x01) bit1=STAGE(0x02)
    off 6..7  chan_lo chan_hi    data-channel LCI (10-bit), little-endian
    off 8..10 base_lo mid hi     ring offset (0..0x1FFFF), little-endian 24-bit
    off 11..12 frames_lo hi      ceil(fragment_len/22), little-endian 16-bit
  Status mailbox @ 0x9A0010 (6 bytes, MCU -> host):
    off 0..3  'B' 'X' 'D' 'S'    magic
    off 4     dl_seq             FPGA drain notify; bumps (mod 4) when Town drained a frag
    off 5     ack                echo of the last applied descriptor seq

Usage:
  python3 utils/bsx_download.py <program.bs> [opts]
    --chan 0x125        data-channel LCI the Town tunes to (default LCI_DATA0 0x125)
    --dirid N           Directory/TownStatus ID (default 2; bump to force Town re-read)
    --folder NAME       building/Folder name (default "News")
    --name NAME         program name in the listing (default = file stem)
    --dest 3            0=WRAM 1=PSRAM 2=FLASHfull 3=FLASHfree(save, default)
    --autostart 0       0=No(list) 1=Optional 2=Yes(autoboot)
    --date YYYY-MM-DD   schedule date (default: today, host)
    --window HH:MM-HH:MM schedule window (default whole day)
    --no-catalog        skip the catalog push (assume it is already on the page window)
    --timeout SEC       per-fragment drain timeout warning (default 10)

Prereqs on the device side: a BS-X base cart (mapper_id == 3) must be RUNNING (the
Town) so bsx_dl_service ticks; the FPGA receiver core must be flashed.  The serial
port only exists with the SNES powered ON.
"""
import sys, os, math, time, argparse, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# Reuse the REAL USB layer (PUT/GET space=SNES, absolute PSRAM) — do NOT reimplement.
#   write_mem(addr, data, delay=0.01, force=False) -> (size, port)
#   read_mem(addr, size) -> (err, bytes)
#   find_port(default=...) -> port string
from usb_bsx import read_mem, find_port
from usb_bsx import write_mem as _raw_write_mem


def write_mem(addr, data, delay=0.01, force=False):
    """VERIFIED write: write + read-back + retry until byte-exact.  Raw USB writes are
    FLAKY under the Town's bus hammering -- a Town Status page once landed shifted +4
    bytes, the Town read an invalid wrapper and latched 'broadcast ended' (reboot-only
    recovery).  Every satellite write (catalog pages AND fragments) goes through this."""
    data = bytes(data)
    for attempt in range(1, 7):
        try:
            _raw_write_mem(addr, data, delay=delay, force=force)
            time.sleep(0.03)
            err, back = read_mem(addr, len(data))
        except SystemExit as e:
            # usb_bsx raises SystemExit on a garbled PUT-confirm / serial timeout --
            # exactly the transient this wrapper exists to absorb.  Retry.
            print(f"  [verify] {addr:#08x} transient error ({e}) attempt {attempt} -- retry", flush=True)
            time.sleep(0.2)
            continue
        if not err and bytes(back[:len(data)]) == data:
            if attempt > 1:
                print(f"  [verify] {addr:#08x} ({len(data)}B) OK on attempt {attempt}", flush=True)
            return
        print(f"  [verify] {addr:#08x} ({len(data)}B) mismatch/err={err} attempt {attempt} -- rewriting", flush=True)
        time.sleep(0.1)
    raise SystemExit(f"!! verified write FAILED 6x at {addr:#08x}")

# Reuse the REAL catalog builders (byte-exact SatellaWave serialization).
#   build_directory(dir_id, folder_name, folder_msg, files)  files=[dict(...)]
#   build_townstatus(ts_id, dir_id, file_ids, base_body=None)
#   build_channelmap(services)  services={svc:[(type,prog,timeout,autodest,lci)]}
#   build_data_channel(img, lci, title, body, template_lci=LCI_TOWN)  (in-place)
#   datagroup(body), pack_sched(month,day,hs,ms,he,me), patch_page(img,lci,payload)
from bsx_broadcast import (
    build_directory, build_townstatus, build_channelmap, build_data_channel,
    channelmap_append_service,
    datagroup, pack_sched, patch_page,
    PAGE, LCI_DIR, LCI_TOWN, LCI_MAP, LCI_DATA0, SVC_DOWNLOAD,
    LCI_WELCOME,
)

# Reuse the GOLDEN fragment format from the validated receiver model (THE spec).
#   make_fragments(program, chunk=0x8000) -> [10B DG header + chunk, ...]
from bsx_receiver_model import make_fragments, frag_frames, PKT  # PKT == 22 (utils/, local)

# ---- protocol constants (mirror src/bsx_dl.h) -------------------------------
PSRAM_PAGE_BASE = 0x900000             # broadcast page window base (0x900000 + LCI*0x200)
BS_DL_RING_ADDR = 0x980000             # download ring (bsx.v reads 0x980000 + offset) [128KB]
BS_DL_RING_SIZE = 0x020000             # 128KB
BS_DL_MBOX_ADDR = 0x9A0000             # descriptor mailbox (host -> MCU), 13 bytes
BS_DL_STATUS_ADDR = 0x9A0010           # status mailbox (MCU -> host), 6 bytes
BS_DL_STATUS_SEQ = BS_DL_STATUS_ADDR + 4   # 0x9A0014 = dl_seq (drain notify)
BS_DL_STATUS_ACK = BS_DL_STATUS_ADDR + 5   # 0x9A0015 = ack (last applied seq)

BS_DL_CTL_ARM = 0x01
BS_DL_CTL_STAGE = 0x02

CHUNK = 0x8000                         # 32KB SatellaWave fragment size (== make_fragments default)
RING_SLOTS = BS_DL_RING_SIZE // CHUNK  # 4 (the 128KB ring holds four 32KB fragments)


# ---- descriptor / status (de)serialization ----------------------------------

def build_descriptor(seq, ctl, chan, base, frames):
    """Serialize the 13-byte descriptor EXACTLY as bsx_dl.c reads it (hdr[0..12]).

    Verifies against bsx_dl.c:
      hdr[0..3]='BXDL'  hdr[4]=seq  hdr[5]=ctl
      chan   = hdr[6] | hdr[7]<<8                       (little-endian 16-bit, 10-bit value)
      base   = hdr[8] | hdr[9]<<8 | hdr[10]<<16         (little-endian 24-bit)
      frames = hdr[11] | hdr[12]<<8                     (little-endian 16-bit)
    """
    assert 0 <= seq <= 0xFF
    assert 0 <= ctl <= 0xFF
    assert 0 <= chan <= 0x3FF, f"chan {chan:#x} out of 10-bit range"
    assert 0 <= base < BS_DL_RING_SIZE, f"base {base:#x} out of ring"
    assert 0 <= frames <= 0xFFFF, f"frames {frames} out of 16-bit range"
    return bytes([
        ord('B'), ord('X'), ord('D'), ord('L'),   # 0..3  magic
        seq & 0xFF,                                # 4     seq (bump every write)
        ctl & 0xFF,                                # 5     ctl (ARM|STAGE)
        chan & 0xFF, (chan >> 8) & 0xFF,           # 6..7  chan_lo, chan_hi  (LE)
        base & 0xFF, (base >> 8) & 0xFF, (base >> 16) & 0xFF,  # 8..10 base_lo/mid/hi (LE)
        frames & 0xFF, (frames >> 8) & 0xFF,       # 11..12 frames_lo, frames_hi (LE)
    ])


def write_descriptor(seq, ctl, chan, base, frames):
    """PUT the 13-byte descriptor into the mailbox.  The MCU applies on seq change."""
    write_mem(BS_DL_MBOX_ADDR, build_descriptor(seq, ctl, chan, base, frames))


def read_status():
    """GET the 6-byte status mailbox -> (ok, dl_seq, ack).  ok = magic present."""
    err, st = read_mem(BS_DL_STATUS_ADDR, 6)
    if err or len(st) < 6 or st[:4] != b"BXDS":
        return False, None, None
    return True, st[4], st[5]


def read_dl_seq():
    """GET just the 1-byte drain-notify (0x9A0014).  Returns int or None on error."""
    err, b = read_mem(BS_DL_STATUS_SEQ, 1)
    if err or len(b) < 1:
        return None
    return b[0]


# ---- BS-X FLASH header adaptation -------------------------------------------
# SatellaWave Program.cs:1995-2014 (FLASH header adaptation):
# before SAVING a .bs to the Memory Pack, the BS-X header block is patched so the
# in-cart BIOS treats it as a freshly-flashed program (the BIOS rewrites the rest).
# LoROM: header at 0x7FB0; HiROM: at 0xFFB0.  The block we touch is at +0x20 (0x7FD0
# / 0xFFD0).  Offsets below are RELATIVE to the header base (add 0x7FB0 / 0xFFB0).
#   +0x20..0x23 = 0xFF   ; +0x24 = 0x01 if it was 0x00/0x80 ; +0x26/0x27 = 0xFF
#   +0x2A       = 0xFF

def detect_mapper(program):
    """Decide LoROM vs HiROM from the BS-X header checksum/complement region, falling
    back to size.  Returns the header base offset: 0x7FB0 (LoROM) or 0xFFB0 (HiROM).

    Heuristic mirrors the loader: a valid BS-X header has 'maker/title' ASCII-ish at
    +0x10 and the complement/checksum pair at +0x2C..0x2F.  We test LoROM first; if its
    header looks blank/invalid AND the file is large enough for a HiROM header, use HiROM.
    """
    def looks_like_header(base):
        if base + 0x30 > len(program):
            return False
        # +0x2C/0x2D = checksum complement, +0x2E/0x2F = checksum.  In a sane header
        # they are 16-bit complements of each other.  Treat all-0x00 / all-0xFF as blank.
        comp = program[base + 0x2C] | (program[base + 0x2D] << 8)
        csum = program[base + 0x2E] | (program[base + 0x2F] << 8)
        if (comp, csum) in ((0x0000, 0x0000), (0xFFFF, 0xFFFF)):
            return False
        return (comp ^ csum) == 0xFFFF

    if looks_like_header(0x7FB0):
        return 0x7FB0
    if looks_like_header(0xFFB0):
        return 0xFFB0
    # Fall back to size: >= 0x10000 (64KB) can hold a HiROM header; else LoROM.
    return 0x7FB0 if len(program) < 0x10000 else 0xFFB0


def adapt_flash_header(program, do_adapt=True):
    """Apply the BS-X FLASH-header adaptation in place (returns a NEW bytes).
    Returns (adapted_bytes, header_base, mapper_str).

    do_adapt=False (dest=1 PSRAM/run-once) leaves the program header UNTOUCHED.
    CRITICAL: the Town BIOS validates the program header IDENTICALLY for every dest
    (FUN_80cd20): magic byte at base+0x2A (0x7FDA/0xFFDA) MUST be 0x33, and the inverse
    checksum at base+0x2C ^ base+0x2E MUST be 0xFFFF -- there is NO dest=1 bypass.  The
    FLASH adaptation sets base+0x2A=0xFF (and 0x20..0x27), which FAILS the magic check ->
    Reception Error 21.  The adaptation is correct ONLY for dest=3 (FLASH-Free), where the
    BIOS rewrites the header after the flash write.  For dest=1 the program runs straight
    from PSRAM with its ORIGINAL (valid) header, so it must NOT be adapted."""
    buf = bytearray(program)
    base = detect_mapper(buf)
    mapper = "HiROM" if base == 0xFFB0 else "LoROM"
    if not do_adapt:
        return bytes(buf), base, mapper      # dest=1: original header (magic 0x33 intact)
    blk = base + 0x20            # 0x7FD0 (LoROM) / 0xFFD0 (HiROM)
    if blk + 0x0B <= len(buf):
        buf[blk + 0x0] = 0xFF    # +0x20  (0x7FD0/0xFFD0)
        buf[blk + 0x1] = 0xFF    # +0x21
        buf[blk + 0x2] = 0xFF    # +0x22
        buf[blk + 0x3] = 0xFF    # +0x23
        # +0x24 (0x7FD4): the block-size/start flag.  DO NOT touch it.  The GOLDEN over-air
        # program (bsnes bsxdat, which downloads+saves on hardware) keeps 0x7FD4 = ORIGINAL
        # (=0x00 for Arkanoid); SatellaWave's 0x00/0x80 -> 0x01 rewrite makes the Town's block
        # allocator request DOUBLE the size (8 MiB for a 4 MiB program) and FUN_80cd20's block
        # walk fail -> Error 21.  Verified byte-exact: golden vs host differed ONLY at 0x7FD4.
        buf[blk + 0x6] = 0xFF    # +0x26
        buf[blk + 0x7] = 0xFF    # +0x27
        buf[blk + 0xA] = 0xFF    # +0x2A  (THIS is the magic byte 0x33 -> 0xFF; dest=3 only)
    else:
        print(f"  WARN: program too short for header block at {blk:#x}; skipped adaptation")
    return bytes(buf), base, mapper


# ---- catalog push -----------------------------------------------------------

def push_catalog(chan, dirid, folder, fmsg, name, desc, filesize, dest, autostart,
                 sched, body_first_page, frag0):
    """Build & PUSH the broadcast catalog so the Town LISTS the program and the
    Channel Map says SAVE-to-FLASH (dest).  We write four pages directly to the page
    window: Directory (0x122), Town Status (0x123), Channel Map (0x124), and the
    program's data channel page (`chan`).  Mirrors bsx_stage_a.push()/streaming_image().

    The data-channel PAGE is the page the Town tunes to; the actual program BYTES come
    from the download RING (not this page).  Page payload here only needs the wrapper +
    a Data-Group header so the Town recognises the channel; we put the first <=440B of
    the program in it as a courtesy (the receiver streams the real fragments from ring).
    """
    # program number derived from the data channel (LCI_DATA0 + slot); slot 0 by default
    slot = (chan - LCI_DATA0) & 0xFF if chan >= LCI_DATA0 else 0
    prog = slot * 0x100
    fileid = slot + 1

    # We need a page-window image to patch (Directory/Town clone the Town wrapper, and
    # build_data_channel clones from a template page).  Prefer the LOCAL bsxpage.bin (the
    # exact image the Town booted, with valid 'BSX ' wrappers): the live-PSRAM read is flaky
    # (err=83 leaves BLANK wrappers, which the Town REJECTS -> the whole city goes
    # "broadcast ended").  Fall back to the live read, then to blank, only if it's missing.
    lo = min(LCI_WELCOME, LCI_DIR, LCI_TOWN, LCI_MAP, LCI_DATA0, chan)
    hi = max(LCI_DIR, LCI_TOWN, LCI_MAP, LCI_DATA0, chan)
    need = (hi + 1) * PAGE
    img = None
    for p in (os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bin", "bsxpage.bin"),
              os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bsxpage.bin"),
              "bsxpage.bin"):
        try:
            b = bytearray(open(p, "rb").read())
            if len(b) >= need:
                img = b
                print(f"  catalog base: {p} ({len(b)} bytes, valid wrappers)")
                break
        except Exception:
            continue
    if img is None:
        span_base = PSRAM_PAGE_BASE + lo * PAGE
        span_len = (hi - lo + 1) * PAGE
        err, live = read_mem(span_base, span_len)
        if err or len(live) < span_len:
            print(f"  NOTE: no local bsxpage.bin + live read failed (err={err}); blank wrappers")
            live = bytes(span_len)
        img = bytearray(need)
        img[lo * PAGE:need] = live

    # Town Status base body (preserve NPC/town setup if the live page had it).
    ts_off = LCI_TOWN * PAGE + 0x52
    ts_base = bytes(img[ts_off:ts_off + 19])

    # --- Directory (LCI 0x122): one Folder with one DownloadFile -------------
    files = [dict(
        fileid=fileid, name=name, desc=desc,
        svc=SVC_DOWNLOAD, prog=prog, filesize=filesize,
        dest=dest, autostart=autostart, sched=sched,
    )]
    patch_page(img, LCI_DIR, datagroup(build_directory(dirid, folder, fmsg, files)))

    # --- Town Status (LCI 0x123): the FileID active --------------------------
    patch_page(img, LCI_TOWN,
               datagroup(build_townstatus(dirid, dirid, [fileid], base_body=ts_base)))

    # --- Channel Map (LCI 0x124): service 0x0103, type 5 DownloadFile,
    #     dest=3 (FLASH-Free => Town SAVES), timeout=600 (per-fragment Fragment-Interval in
    #     VBlanks; SatellaWave default 10 ~= 170ms is too tight for the device's on-demand
    #     staging gap ~1.9s/frag -> Error 22).  The LIVE push lands in the Town's parsed
    #     Channel Map (verified in WRAM $7E9BEC) AND the booted bsxpage.bin must match -> both 600.
    autodest = (autostart & 3) | ((dest & 3) << 2)
    # MERGE the download service into the EXISTING Channel Map -- NEVER replace the map.
    # The Town's OWN services (0x0101 Town Status/Directory/Time, 0x0102 ...) live in the
    # same map; a download-only map ERASES them from the live parsed copy ($7E9BEC, which
    # the reception flow re-reads) and the post-download service re-resolution then FAILS
    # -> $13C9=3 -> St.GIGA "program ended" + the whole city goes OFFLINE (retrying the
    # lookup forever; restoring the full map revives it -- proven live on hardware).
    _cm = LCI_MAP * PAGE; _sf = _cm + 0x4d
    _has_dl = False
    if bytes(img[_sf:_sf+2]) == b"SF":
        _off = _sf + 8
        for _ in range(img[_sf+6]):
            _svc = (img[_off] << 8) | img[_off+1]; _n = img[_off+2]; _off += 3 + 13*_n
            if _svc == SVC_DOWNLOAD: _has_dl = True
    if _has_dl:
        # UPSERT the ONE record for our prog INSIDE the existing svc block, preserving
        # every other record (a streaming-ready boot image carries an 8-record channel
        # POOL in a single svc-0x0103 block -- splicing the whole block out would drop
        # the other 7 channels from the live map and any second program's service
        # lookup would fail -> $13C9=3 -> city offline).  In-place overwrite when the
        # prog exists (no size change); otherwise grow the block by one 13-byte record.
        _rec = bytes([5, (prog >> 8) & 0xFF, prog & 0xFF, 0, 0, 0, 0, 0,
                      (600 >> 8) & 0xFF, 600 & 0xFF, autodest, chan & 0xFF, (chan >> 8) & 0xFF])
        _szo = _cm + 0x4a
        _total = (img[_szo] << 16) | (img[_szo+1] << 8) | img[_szo+2]
        _off = _sf + 8
        _done = False
        for _ in range(img[_sf+6]):
            _svc = (img[_off] << 8) | img[_off+1]; _n = img[_off+2]
            if _svc == SVC_DOWNLOAD:
                _r = _off + 3
                for _k in range(_n):
                    _p = (img[_r+1] << 8) | img[_r+2]
                    if _p == prog:                       # overwrite in place, no size change
                        img[_r:_r+13] = _rec
                        print(f"  channel map: record prog={prog:#06x} UPDATED in place "
                              f"(pool of {_n} records preserved)")
                        _done = True
                        break
                    _r += 13
                if not _done:                            # grow the block by one record
                    _ins = _off + 3 + 13*_n
                    _end = _sf + _total
                    if (_end + 13) - _cm > PAGE:
                        raise SystemExit("!! Channel Map would overflow the page on upsert")
                    img[_ins+13:_end+13] = img[_ins:_end]
                    img[_ins:_ins+13] = _rec
                    img[_off+2] = _n + 1
                    _new_total = _total + 13
                    img[_szo] = (_new_total >> 16) & 0xFF
                    img[_szo+1] = (_new_total >> 8) & 0xFF
                    img[_szo+2] = _new_total & 0xFF
                    img[_sf+7] = sum(img[_sf:_sf+7]) & 0xFF
                    print(f"  channel map: record prog={prog:#06x} INSERTED into the block "
                          f"(pool of {_n} records preserved)")
                    _done = True
                break
            _off += 3 + 13*_n
        if not _done:
            channelmap_append_service(img, SVC_DOWNLOAD, [(5, prog, 600, autodest, chan)])
            print(f"  channel map: MERGED svc {SVC_DOWNLOAD:#06x} (town services preserved)")
    else:
        channelmap_append_service(img, SVC_DOWNLOAD, [(5, prog, 600, autodest, chan)])
        print(f"  channel map: MERGED svc {SVC_DOWNLOAD:#06x} (town services preserved)")

    # --- data channel page (`chan`): clone wrapper + title, then OVERRIDE the DG header
    #     + body with the REAL first data group (frag0).  The Town reads this page's DG
    #     header to learn the program's fragcount/size BEFORE it tunes the stream; the
    #     courtesy datagroup() always writes fragcount=1 / size24~430, which mismatches the
    #     N-fragment 32KB stream -> Reception Error 21.  frag0 already carries the correct
    #     header (continuity=0, fragcount=N, size24=chunk+5, offset24=0).
    build_data_channel(img, chan, name, body_first_page, template_lci=LCI_TOWN)
    _b = chan * PAGE
    img[_b + 0x48 : _b + PAGE] = frag0[:PAGE - 0x48]

    # --- write the touched pages back to the page window ---------------------
    for lci in (LCI_DIR, LCI_TOWN, LCI_MAP, chan):
        addr = PSRAM_PAGE_BASE + lci * PAGE
        write_mem(addr, bytes(img[lci * PAGE:(lci + 1) * PAGE]))
        print(f"  catalog: fed LCI 0x{lci:04X} -> {addr:#08x}")


def feed_boot_broadcast():
    """LIVE-FEED prototype: write a full streaming-ready broadcast (Channel Map WITH the
    download service) straight into the PSRAM page window 0x900000 -- so the Town, booted
    with NO bsxpage.bin on the SD, reads its Channel Map from what the satellite fed. Needs
    the firmware that makes bsxpage.bin optional (leaves 0x900000 for a live feed)."""
    from bsx_broadcast import make_streaming_image
    base = None
    for pth in (os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bin", "bsxpage.bin"),
                os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bsxpage.bin"),
                "bsxpage.bin"):
        if os.path.isfile(pth):
            base = pth; break
    if base is None:
        raise SystemExit("!! baseline bsxpage.bin not found (bin/bsxpage.bin) to build the boot broadcast")
    img = make_streaming_image(base)
    CH = 0x8000
    for off in range(0, len(img), CH):
        write_mem(PSRAM_PAGE_BASE + off, bytes(img[off:off+CH]))
    print(f"boot-broadcast: {len(img)} bytes -> 0x900000 (Channel Map with svc 0x0103). "
          f"NOW BOOT the .bs (with bsxpage.bin ABSENT on the SD).", flush=True)


def usb_set_device_time():
    """Set the device RTC (MCU + FPGA, live) to the host's current local time via the
    USB TIME opcode (0x0E).  The BS-X Town evaluates every program's broadcast schedule
    (month/day/window) against the FPGA RTC served on the Time channel: our catalog is
    stamped with TODAY, so the device clock MUST agree or the Town declares the program
    out of its broadcast window right after the reception ("program ended" + dead city).
    Requires the firmware with the TIME handler fix (set_fpga_time + break)."""
    import datetime as _dt
    import serial as _serial
    now = _dt.datetime.now()
    b = bytearray(512)
    b[0:4] = b"USBA"
    b[4] = 14          # USBINT_SERVER_OPCODE_TIME
    b[5] = 0           # space (don't care)
    b[6] = 0           # flags
    b[8]  = now.second
    b[9]  = now.minute
    b[10] = now.hour
    b[11] = now.day
    b[12] = now.month
    b[13] = (now.year >> 8) & 0xFF
    b[14] = now.year & 0xFF
    b[15] = (now.weekday() + 1) % 7      # tm_wday: 0=Sunday
    port = find_port()
    s = _serial.Serial(port, 9600, timeout=1); s.reset_input_buffer()
    s.write(bytes(b)); s.flush()
    time.sleep(0.3)
    resp = s.read(512)
    s.close()
    if resp[:4] == b"USBA" and len(resp) >= 6 and resp[5] == 0:
        print(f"device clock set: {now:%Y-%m-%d %H:%M:%S} (USB TIME)")
    else:
        # No/garbled response: OLD firmware (TIME handler fell through into OPCODE_MV --
        # possible USB-server wedge) or a busy server.  Do NOT claim success.
        print(f"!! TIME: no response from device ({resp[:8].hex() if resp else 'empty'}) --")
        print(f"!! OLD firmware? The clock was NOT (necessarily) set; if USB")
        print(f"!! wedges now, that's the old handler's fallthrough: reflash the firmware.")


WATCH_WRAM = False
_ACTIVE_LINK = None                     # (Seq, chan) once armed; disarmed in main()'s finally
_watch_last = {}
_watch_next = [0.0]
_watch_t0 = [None]

def wram_death_watch():
    """Town lifecycle telemetry (1 sample / 2s, logs only CHANGES): $13C9 state (3 =
    Channel-Map service lookup FAILED -> city offline; 0x18 = broadcast watchdog fired
    -> Error 22), $13C5 result, $0B74 town-restart reason (2 = healthy reload, 3 =
    offline), $1433/$1435 broadcast watchdog/preset.  Read via the ctx WRAM mirror at
    0xF5xxxx.  THE tool that cracked the city-death bug -- keep it wired."""
    if time.time() < _watch_next[0]:
        return
    _watch_next[0] = time.time() + 2.0
    if _watch_t0[0] is None:
        _watch_t0[0] = time.time()
    try:
        e1, a = read_mem(0xF513C0, 16)
        e2, b = read_mem(0xF51430, 8)
        e3, c = read_mem(0xF50B70, 8)
        if e1 or e2 or e3:
            return
        snap = dict(state=a[9], result=a[5], pause=a[2],
                    wdog=b[3] | (b[4] << 8), preset=b[5] | (b[6] << 8), restart=c[4])
        changed = {k: v for k, v in snap.items() if _watch_last.get(k) != v}
        if changed:
            _watch_last.update(snap)
            print(f"  [WRAM t+{time.time()-_watch_t0[0]:6.1f}s] " +
                  " ".join(f"{k}={v:#x}" for k, v in snap.items()) +
                  "   <<< " + ",".join(changed), flush=True)
    except Exception:
        pass


def push_townstatus_signoff(dirid):
    """Post-download QUIET sign-off: TSID bump with the SAME DirID and an EMPTY on-air
    FileID list.  The Town re-processes the Town Status quietly (no city restart), the
    program flips to 'cannot receive now', and the city STAYS ALIVE -- without this the
    channel silence after a completed download reads as 'broadcast died' and the Town
    shows St.GIGA "the program has ended" with the city never coming back."""
    img = None
    for pth in (os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bsxpage.bin"),
                "bsxpage.bin"):
        try:
            b = bytearray(open(pth, "rb").read())
            if len(b) >= (LCI_TOWN + 1) * PAGE:
                img = b
                break
        except Exception:
            continue
    if img is None:
        img = bytearray((LCI_TOWN + 1) * PAGE)
    ts_off = LCI_TOWN * PAGE + 0x52
    ts_base = bytes(img[ts_off:ts_off + 19])
    tsid = 2 + ((dirid - 2 + 1) % 249)          # any TSID != the one pushed (== dirid)
    patch_page(img, LCI_TOWN, datagroup(build_townstatus(tsid, dirid, [], base_body=ts_base)))
    addr = PSRAM_PAGE_BASE + LCI_TOWN * PAGE
    write_mem(addr, bytes(img[LCI_TOWN * PAGE:(LCI_TOWN + 1) * PAGE]))
    print(f"sign-off: Town Status TSID={tsid} (DirID={dirid} unchanged, no on-air files) "
          f"-> city stays alive, program goes off-air")


# ---- the download flow ------------------------------------------------------

class Seq:
    """Monotonic descriptor seq counter (wraps at 0xFF; MCU only compares != )."""
    def __init__(self, start=0):
        self.v = start

    def next(self):
        self.v = (self.v + 1) & 0xFF
        return self.v


def next_dirid():
    """Rotating Directory ID — the Town re-reads the Directory ONLY when the DirID
    changes (and a stale DirID leaves the city in 'broadcast ended'), so every push
    must use a fresh one.  Persisted in utils/.dirid; cycles 2..250."""
    f = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".dirid")
    try:
        n = int(open(f).read().strip())
    except Exception:
        n = 2
    n = n + 1 if n < 250 else 2
    try:
        open(f, "w").write(str(n))
    except Exception:
        pass
    return n


def download(rom_path, chan=LCI_DATA0, dirid=0, folder="News", fmsg="Broadcast",
             name=None, desc="Downloaded via USB satellite", dest=3, autostart=0,
             date=None, window="00:00-23:59", push=True, frag_timeout=10.0,
             skip_clock=False, feed_boot=False):
    program = open(rom_path, "rb").read()
    if not program:
        raise SystemExit(f"!! empty program: {rom_path}")
    # clock sync: the fixed firmware's TIME handler (break + immediate set_fpga_time)
    # makes this safe and LIVE -- the BS-X Time channel then serves the real date, so
    # the catalog's schedule (stamped with today) always matches the Town's clock.
    # (Requires firmware >= the TIME-handler fix; the stock handler fell through into
    # OPCODE_MV and could wedge the USB server.  --no-clock skips it.)
    if not skip_clock:
        usb_set_device_time()
    if feed_boot:
        feed_boot_broadcast()
    if name is None:
        name = os.path.splitext(os.path.basename(rom_path))[0][:20]
    if not dirid:                       # 0 = auto-rotate so the Town re-reads each push
        dirid = next_dirid()

    # 1) adapt the BS-X FLASH header ONLY for dest=3 (FLASH-Free, where the BIOS rewrites the
    #    header after the flash write).  For dest=1 (PSRAM/run-once, NO flash write -> NO BIOS
    #    rewrite) the program runs with whatever header it has, and the Town's FUN_80cd20 validates
    #    it: magic at base+0x2A MUST be 0x33, checksum base+0x2C^base+0x2E == 0xFFFF, block flags.
    #    The adaptation sets base+0x2A=0xFF -> magic 0xFF != 0x33 -> Error 21.  So dest=1 keeps the
    #    ORIGINAL header (magic 0x33).  (Verified on hardware: Arkanoid LoROM dest=1 + adaptation =
    #    Error 21 at FUN_80cd20; the header bytes themselves were intact, only the magic was wrong.)
    #    The program MUST also be LoROM (the Town reads the header at the LoROM base 0x7FB0).
    adapted, hdr_base, mapper = adapt_flash_header(program, do_adapt=(dest == 3))
    print(f"program: {len(program)} bytes  mapper={mapper} (header @ {hdr_base:#06x})  "
          f"flash-adapt={'YES (dest=3)' if dest == 3 else 'NO (dest=1, original header magic 0x33)'}")
    if mapper != "LoROM":
        print(f"  *** WARNING: program is {mapper}; the Town reads the header at the LoROM base "
              f"0x7FB0 -> a HiROM program FAILS with Error 21.  Use a LoROM .bs.")

    # 2) split into 32KB SatellaWave fragments (golden make_fragments)
    frags = make_fragments(adapted, chunk=CHUNK)
    nfrags = len(frags)
    frames = [frag_frames(len(f)) for f in frags]
    print(f"fragments: {nfrags} (32KB each; frames/frag={frames})")
    # Single ring slot at base 0: the stream is strictly drain-gated (the next fragment is
    # written only AFTER the FPGA signals the current one fully drained), so overwriting in
    # place is safe and sidesteps the slot-stride-vs-fragment-size (0x8000 vs 32778) overlap.

    # 3) build & push the catalog
    d = datetime.date.fromisoformat(date) if date else datetime.date.today()
    try:
        a, b = window.split("-")
        h1, m1 = map(int, a.split(":")); h2, m2 = map(int, b.split(":"))
    except Exception:
        raise SystemExit(f"!! bad --window {window!r} (use HH:MM-HH:MM)")
    sched = pack_sched(d.month, d.day, h1, m1, h2, m2)
    if push:
        print("catalog: pushing Directory / Town Status / Channel Map / data channel ...")
        push_catalog(chan, dirid, folder, fmsg, name, desc,
                     filesize=len(adapted), dest=dest, autostart=autostart,
                     sched=sched, body_first_page=adapted[:PAGE - 0x52], frag0=frags[0])
        print(f"catalog: pushed (DirID={dirid} dest={dest} autostart={autostart} chan=0x{chan:04X})")
        print(f"  NOTE: the Channel Map (LCI 0x{LCI_MAP:04X}) is cached by the Town at BOOT and never")
        print(f"  re-read, so for this program to be OFFERED its data channel (0x{chan:04X}, service")
        print(f"  0x{SVC_DOWNLOAD:04X}, dest={dest}) MUST already be in the bsxpage.bin the Town booted.")
        print(f"  Install a streaming-ready bsxpage.bin whose Channel Map pool record for this channel")
        print(f"  is dest=3 (FLASH-Free, so the download SAVES) + reboot the Town first.  The DEFAULT")
        print(f"  install pool (make_streaming_image) uses dest=1 (PSRAM, runs once, does NOT save) —")
        print(f"  it must be dest=3 in the BOOTED image; only Directory/Town Status are re-read live.")
    else:
        print("catalog: SKIPPED (--no-catalog)")

    seq = Seq(0)

    # 4) Reset any stale session: wipe the mailbox magic and let the MCU bridge SEE it
    #    absent (so its active/last_seq state resets) BEFORE we arm — otherwise a stale
    #    descriptor left by a crashed run could share our last_seq and swallow the ARM.
    write_mem(BS_DL_MBOX_ADDR, b"\x00\x00\x00\x00")
    time.sleep(0.08)                                  # > 2 bridge ticks (the bridge runs ~every 25ms)

    # 5) ARM only (no STAGE).  We stage fragments ON DEMAND: the FPGA bumps dl_seq only
    #    AFTER the Town has TUNED to chan and read $218A empty.  Staging strictly on the
    #    bump dodges BOTH the armed-empty spurious-bump race AND a $2189 tune discarding a
    #    pre-staged fragment.  A single ring slot (base 0) suffices because the flow is
    #    strictly drain-gated: fragment i is fully read before fragment i+1 is written.
    global _ACTIVE_LINK
    _ACTIVE_LINK = (seq, chan)          # main()'s finally disarms through this on ANY exit
    s = seq.next()
    write_descriptor(s, BS_DL_CTL_ARM, chan, 0, 0)
    print(f"armed: seq={s} chan=0x{chan:04X}.  Waiting for the Town to start the download ...")
    last_dl_seq = read_dl_seq()
    _tries = 0
    while last_dl_seq is None:          # a stale nonzero seq misread as 0 would fire a phantom bump
        _tries += 1
        if _tries > 30:
            raise SystemExit("!! could not read dl_seq after arming (30 attempts) -- USB/contention?")
        if _tries % 5 == 0:
            print(f"  ... reading dl_seq post-arm (attempt {_tries})", flush=True)
        time.sleep(0.2)
        last_dl_seq = read_dl_seq()

    # 6) STREAMING LOOP.  Each dl_seq bump = "the FPGA needs the next fragment": bump #1 is
    #    the Town's first empty $218A read (requesting fragment 0); bump #k requests
    #    fragment k-1 (fragment k-2 just drained); the bump after fragment N-1 drains ends it.
    staged = 0
    passes = 0            # completed full passes over the fragment set
    boundary_nudge = None # pass boundary: waiting (<=2s) for the Town's retune-bump
    last_request = time.time()  # quiet clock: reset on bumps AND after each completed stage

    def _stage(idx):
        """Write fragment idx (+ its prefix table: 0x10 first frame, 0x80 last) into the
        single ring slot and send the STAGE descriptor.  All writes verified."""
        write_mem(BS_DL_RING_ADDR, frags[idx])
        _pt = bytearray(frames[idx])
        _pt[0] = 0x10
        _pt[-1] |= 0x80
        write_mem(BS_DL_RING_ADDR + 0x8100, bytes(_pt))
        _s = seq.next()
        write_descriptor(_s, BS_DL_CTL_ARM | BS_DL_CTL_STAGE, chan, 0, frames[idx])

    def _fpga_serving_here():
        """True if the receiver is ARMED with the Town TUNED to our channel (fl bits) --
        i.e. the FPGA may be MID-SERVE and rewriting the single ring slot would TEAR the
        fragment under it.  None/unreadable -> unknown (treat as safe-to-wait)."""
        _e, _st = read_mem(BS_DL_STATUS_ADDR, 13)
        if _e or not _st or len(_st) < 13:
            return None
        _fl = _st[12]
        return bool(((_fl >> 6) & 1) and ((_fl >> 1) & 1))   # arm & chanok
    last_progress = time.time()
    idle_announced = False            # True once the completion line was printed
    stage_t0 = None; last_t = None    # timing instrumentation
    while True:
        if WATCH_WRAM:
            wram_death_watch()
        cur = read_dl_seq()
        if cur is None:
            time.sleep(0.2)
            continue
        if cur != last_dl_seq:
            last_dl_seq = cur
            last_progress = time.time()
            last_request = time.time()
            idle_announced = False   # a genuine request = the Town is active again
            if staged >= nfrags:
                # A bump AFTER the last fragment drained = end of a pass.  If the Town wants
                # the stream AGAIN (multi-pass: validate then program), it either RETUNES the
                # channel (discards staged state -> another bump follows and the normal path
                # below serves fragment 0), or keeps polling silently (no further edge -> we
                # nudge fragment 0 after 2s).  Never stage AT the boundary bump: a retune
                # right after would discard it and desync the fragment sequence.
                passes += 1
                staged = 0
                stage_t0 = None; last_t = None
                boundary_nudge = time.time()
                print(f"  === pass {passes} complete -> waiting for another request (multi-pass?) ===")
                continue
            boundary_nudge = None
            if staged < nfrags:
                _stage(staged)
                staged += 1
                last_request = time.time()   # quiet clock counts from AFTER the (slow,
                                             # verified) staging writes, not from the bump
                _now = time.time()
                if stage_t0 is None: stage_t0 = _now
                _dt = (_now - last_t) if last_t else 0.0
                last_t = _now
                lead = "Town started download" if staged == 1 else f"fragment {staged-1}/{nfrags} drained"
                print(f"  {lead} -> staged fragment {staged}/{nfrags} (frames={frames[staged-1]})  "
                      f"[+{_dt:.2f}s gap, total {_now-stage_t0:.1f}s]")
                # DBG: read the FPGA's STICKY 0x90 capture (firmware publishes it every poll)
                _e, _s = read_mem(BS_DL_STATUS_ADDR, 13)
                if not _e and _s and len(_s) >= 13:
                    _q90 = _s[8] | (_s[9] << 8)    # dl_queue at 1st 0x90 (0xFFFF = never seen)
                    _pf90 = _s[10]                 # pf_queue at 0x90
                    _fl = _s[12]
                    _af = _s[6] | (_s[7] << 8)
                    _saw = _fl & 1
                    tag = (f"*** SAW 0x90 @ dl_queue={_q90} pf_queue={_pf90} ***" if _saw
                           else f"no 0x90 (q={_q90:#x})")
                    print(f"  [DBG-FPGA stage{staged}] {tag}  applied={_af} "
                          f"fl={_fl:#04x}[first={(_fl>>7)&1} arm={(_fl>>6)&1} chanok={(_fl>>1)&1}]")
                else:
                    print(f"  [DBG-FPGA] status read err={_e}")
            continue
        if boundary_nudge and time.time() - boundary_nudge > 2.0:
            # no retune-bump: the Town may be polling the channel silently -> serve frag 0
            boundary_nudge = None
            _stage(0)
            staged = 1
            last_request = time.time()
            print(f"  (no retune-bump after 2s: nudged fragment 1/{nfrags} for pass {passes+1})")
        _quiet = time.time() - last_request
        if staged > 0 and boundary_nudge is None and _quiet > 12.0:
            # >=12s of silence AFTER the last completed stage (a fragment drains in ~6.3s)
            # = the current serving ended (reception done, aborted, or the Town retuned).
            # The REAL broadcast NEVER stops (bsnes = infinite carousel; the buildings go
            # sad-eyed the moment the tuned channel dies) -> restart the carousel from
            # fragment 1.  ANTI-TEAR GATE: the ring is a single slot, so NEVER rewrite it
            # while the FPGA may be MID-SERVE (armed + Town tuned here, e.g. the Town
            # paused mid-fragment on a city announcement) -- a torn fragment poisons the
            # BIOS block bitmap and dedup then rejects the clean re-serve (Error 21/22).
            # Hard fallback at 45s in case the fl bits are unreadable/stuck.
            _serving = _fpga_serving_here()
            if _serving and _quiet < 45.0:
                last_progress = time.time()   # keep the waiting message quiet
            else:
                # KEEP re-staging the download channel even after a full pass: the Town needs
                # the broadcast to keep flowing to FINALIZE the reception (受信中 -> saved).
                # Stopping here stalls the reception.  (The USB wedge under sustained writes is
                # a separate, later operational issue -- Ctrl-C the satellite once the game is
                # saved and running.)
                if staged >= nfrags:
                    if not idle_announced:
                        print(f"  === {nfrags}/{nfrags} served; keeping carousel alive so the "
                              f"reception can finalize (Ctrl-C once the game is saved) ===")
                        idle_announced = True
                else:
                    print(f"  === serving stalled at fragment {staged}/{nfrags} -> carousel restart ===")
                _stage(0)
                staged = 1
                last_request = time.time()
                stage_t0 = None; last_t = None
            last_request = time.time()
        if time.time() - last_progress > frag_timeout:
            if not idle_announced:   # stay quiet once the download completed
                tag = "the Town to start" if staged == 0 else f"fragment {staged}/{nfrags} to drain"
                print(f"  ... waiting on {tag} (>{frag_timeout:.0f}s; is the Town receiving? dl_seq={cur})")
            last_progress = time.time()
        time.sleep(0.05)
    # (no exit: continuous carousel; main()'s finally disarms via _ACTIVE_LINK)


def _disarm(seq, chan):
    """DISARM and clear the mailbox magic so a stale descriptor can't re-trigger.
    Wait for the bridge's applied-seq ACK before wiping the magic -- wiping first can
    destroy the descriptor before the MCU applies it (receiver stays armed).  Even on
    ACK timeout the wipe is safe now: the firmware auto-disarms the FPGA when the
    magic vanishes with a download active (bsx_dl.c)."""
    s = seq.next()
    write_descriptor(s, 0x00, chan, 0, 0)             # ctl=0 -> disarm
    acked = False
    for _ in range(20):                               # <=2s
        err, st = read_mem(BS_DL_STATUS_ADDR, 6)
        if not err and st and len(st) >= 6 and st[5] == s:
            acked = True
            break
        time.sleep(0.1)
    write_mem(BS_DL_MBOX_ADDR, b"\x00\x00\x00\x00")   # wipe 'BXDL' magic -> bridge goes inert
    print(f"disarmed: seq={s} ack={'yes' if acked else 'TIMEOUT (firmware auto-disarms on magic-loss)'}; magic cleared.")


# ---- main -------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="USB satellite: drive a BS-X over-the-air program download to sd2snes.")
    ap.add_argument("program", nargs="?", help="program image (.bs / Memory Pack image)")
    ap.add_argument("--chan", type=lambda x: int(x, 0), default=LCI_DATA0,
                    help=f"data-channel LCI (default {LCI_DATA0:#x})")
    ap.add_argument("--feed-boot", action="store_true",
                    help="LIVE broadcast: feed the boot broadcast into 0x900000 (no bsxpage.bin on SD)")
    ap.add_argument("--no-clock", action="store_true",
                    help="skip the USB TIME clock sync (old firmware without the TIME fix)")
    ap.add_argument("--watch", action="store_true",
                    help="log Town lifecycle WRAM telemetry (state/result/restart/watchdog)")
    ap.add_argument("--dirid", type=int, default=0,
                    help="Directory/TownStatus ID (default 0 = auto-rotate each run so the Town re-reads)")
    ap.add_argument("--folder", default="News", help="building/Folder name (default News)")
    ap.add_argument("--fmsg", default="Broadcast", help="building message")
    ap.add_argument("--name", default=None, help="program name in the listing (default = file stem)")
    ap.add_argument("--desc", default="Downloaded via USB satellite", help="program description")
    ap.add_argument("--dest", type=int, default=3,
                    help="0=WRAM 1=PSRAM 2=FLASHfull 3=FLASHfree(save, default)")
    ap.add_argument("--autostart", type=int, default=0,
                    help="0=No(list) 1=Optional 2=Yes(autoboot)")
    ap.add_argument("--date", help="YYYY-MM-DD schedule date (default: today)")
    ap.add_argument("--window", default="00:00-23:59", help="HH:MM-HH:MM schedule window")
    ap.add_argument("--no-catalog", action="store_true",
                    help="skip the catalog push (assume already on the page window)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="per-fragment drain timeout warning (s, default 10)")
    args = ap.parse_args()

    if not args.program:
        ap.print_help()
        raise SystemExit("\n!! missing program image (a .bs / Memory Pack image)")
    if not os.path.isfile(args.program):
        raise SystemExit(f"!! not found: {args.program}")

    global WATCH_WRAM
    WATCH_WRAM = args.watch
    print(f"port: {find_port()}")
    try:
        download(args.program, chan=args.chan, dirid=args.dirid, folder=args.folder,
                 fmsg=args.fmsg, name=args.name, desc=args.desc, dest=args.dest,
                 autostart=args.autostart, date=args.date, window=args.window,
                 push=not args.no_catalog, frag_timeout=args.timeout,
                 skip_clock=args.no_clock, feed_boot=args.feed_boot)
    except KeyboardInterrupt:
        print("\ncarousel interrupted -> station sign-off")
    finally:
        # ALWAYS disarm on the way out (Ctrl-C, SystemExit from a failed verified write,
        # normal return): with bs_dl_arm left set the channel stays HIJACKED (bsx.v
        # suppresses the stream-0 page while armed and the carousel-of-one keeps
        # re-serving the stale fragment) and the Town reads a phantom broadcast until a
        # power-cycle.  The ctl=0 descriptor is the real disarm; the magic wipe alone
        # (old code) left the FPGA armed because bsx_dl.c goes inert on magic loss
        # WITHOUT pushing arm=0.
        if _ACTIVE_LINK is not None:
            try:
                _disarm(*_ACTIVE_LINK)
            except BaseException as e:   # incl. SystemExit from a failed verified write
                print(f"(disarm failed: {e})")


if __name__ == "__main__":
    main()
