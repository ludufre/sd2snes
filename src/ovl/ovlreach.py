#!/usr/bin/env python3
"""ovlreach.py <firmware.elf> <firmware.dis> <overlay undef list> <veneer names...>

Link-time check of the card overlay rules (ovl.h), on the final code (after LTO):

 1. nothing reachable from the in-game command server (game_cmd_serve), from the USB
    server (usbint_handler, which the game loop serves too) short of load_rom, from an
    exception/interrupt handler, or from a function a card module reaches through its API
    table (mod_api) may enter an overlay -- in a game the block can hold a session module,
    and a module calling into an overlay would have its own code overwritten.  load_rom is
    where a USB boot goes: it drops the session module before anything else.  (The game
    loop itself is inside main, with msu1_loop inlined, so it cannot be a root of its own;
    ovl_ensure refuses at run time while a module is live.);
 2. nothing an overlay calls into may reuse ptrcache (READDIR sort, module loader, the
    ZX0 window, the overlay loader itself): the running overlay would be overwritten.

"Enter an overlay" = reach a veneer (a resident function of the given names) or the
loader.  "Reuse ptrcache" = a function whose literal pool holds an address inside
ptrcache.  Direct calls and tail calls (bl / b) are followed; calls through a function
pointer are not (listed as a warning when a root's closure makes any).  Exit 1 on a hit,
with the call path."""
import re, struct, sys

elf_path, dis_path, undef_path = sys.argv[1:4]
veneers = set(sys.argv[4:])
# never return: whatever runs under them never comes back to the caller's code
NORETURN = {'led_panic'}
# leaving the game: load_rom drops the session module (module_drop) before anything else
GAME_EXIT = {'load_rom'}

# ---- symbols and section bytes from the ELF (minimal ELF32 LE reader) ---------------
data = open(elf_path, 'rb').read()
shoff, = struct.unpack_from('<I', data, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
secs = []
for i in range(shnum):
    name, typ, flags, addr, off, size, link, info, align, entsize = \
        struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize)
    secs.append((name, typ, flags, addr, off, size, link))
syms = {}          # name -> (addr, size, type)
for name, typ, flags, addr, off, size, link in secs:
    if typ != 2:   # SHT_SYMTAB
        continue
    stroff = secs[link][4]
    for j in range(size // 16):
        st_name, st_value, st_size, st_info, st_other, st_shndx = \
            struct.unpack_from('<IIIBBH', data, off + j * 16)
        n = data[stroff + st_name:data.index(b'\0', stroff + st_name)].decode()
        if n and not n.startswith('$'):
            syms.setdefault(n, (st_value, st_size, st_info & 15))

def read(addr, size):
    for name, typ, flags, saddr, off, ssize, link in secs:
        if typ == 1 and saddr <= addr and addr + size <= saddr + ssize:
            return data[off + addr - saddr: off + addr - saddr + size]
    return None

def base(n):
    return n.split('.')[0]

ptr = syms.get('ptrcache')
if not ptr:
    sys.exit('ovlreach: no ptrcache in %s' % elf_path)
p_lo, p_hi = ptr[0], ptr[0] + max(ptr[1], 8192)

# ---- call graph from the disassembly ------------------------------------------------
fn_re = re.compile(r'^[0-9a-f]+ <([^>]+)>:$')
ref_re = re.compile(r'<([^>+]+)(\+0x[0-9a-f]+)?>')
calls, uses_ptr, indirect = {}, set(), set()
cur = None
for line in open(dis_path):
    line = line.rstrip('\n')
    m = fn_re.match(line)
    if m:
        cur = m.group(1)
        calls.setdefault(cur, set())
        continue
    if cur is None or '\t' not in line:
        continue
    parts = line.split('\t')
    if len(parts) < 3:
        continue
    mnem = parts[2].strip().split()[0] if parts[2].strip() else ''
    if mnem == '.word':
        try:
            v = int(parts[3].strip().split()[0], 16)
        except (IndexError, ValueError):
            continue
        if p_lo <= v < p_hi:
            uses_ptr.add(cur)
        continue
    if mnem.startswith(('b', 'cb')):
        if mnem.startswith(('blx', 'bx')) and '<' not in line:
            indirect.add(cur)
            continue
        m = ref_re.search(line)
        if m and m.group(1) != cur:
            calls[cur].add(m.group(1))

enter = {f for f in calls if base(f) in veneers or base(f) == 'ovl_ensure'
         or any(base(g) == 'ovl_ensure' for g in calls[f])}
if not enter:
    sys.exit('ovlreach: no veneer found in the firmware (wrong names?)')

def closure(roots, stop=()):
    """BFS; returns {fn: parent} for every function reached"""
    seen = {r: None for r in roots if r in calls}
    todo = list(seen)
    while todo:
        f = todo.pop()
        if f in stop or base(f) in NORETURN:
            continue
        for g in calls.get(f, ()):
            if g not in seen:
                seen[g] = f
                todo.append(g)
    return seen

def path(seen, f):
    out = []
    while f is not None:
        out.append(f)
        f = seen[f]
    return ' <- '.join(out)

bad = 0
# ---- rule 1 --------------------------------------------------------------------------
roots = {'game_cmd_serve': sorted(f for f in calls if base(f) == 'game_cmd_serve')}
roots['usbint_handler'] = sorted(f for f in calls if base(f) == 'usbint_handler')
roots['exception/interrupt handlers'] = sorted(f for f in calls if base(f).endswith('Handler'))
api = [n for n in syms if base(n) == 'mod_api']
mod_fns = []
for n in api:
    a, s, t = syms[n]
    blob = read(a, s) or b''
    for k in range(0, len(blob) - 3, 4):
        v = struct.unpack_from('<I', blob, k)[0] & ~1
        for fn, (fa, fs, ft) in syms.items():
            if ft == 2 and fa & ~1 == v and fn in calls:
                mod_fns.append(fn)
                break
roots['the module API table'] = sorted(set(mod_fns))
# a root that vanished (inlined, renamed) would pass the check unchecked: only the
# interrupt handlers may be absent
REQUIRED = {'game_cmd_serve', 'usbint_handler', 'the module API table'}
for what, r in roots.items():
    # the overlay entry points themselves are where a hit stops being interesting
    seen = closure(r, stop=enter | ({f for f in calls if base(f) in GAME_EXIT}
                                    if what == 'usbint_handler' else set()))
    if not seen:
        if what in REQUIRED:
            print('ovlreach: ERROR %s not found in the image (inlined or renamed?), not checked'
                  % what)
            bad += 1
            continue
        print('ovlreach: warning: %s not found in the image, not checked' % what)
    hits = sorted(f for f in seen if f in enter)
    ind = sorted(f for f in seen if f in indirect)
    print('ovlreach: %-30s %4d functions reached, %d through pointers not followed'
          % (what + ':', len(seen), len(ind)))
    for h in hits:
        print('ovlreach: ERROR %s enters an overlay: %s' % (what, path(seen, h)))
        bad += 1

# ---- rule 2 --------------------------------------------------------------------------
undef = [l.strip() for l in open(undef_path) if l.strip()]
targets = [f for f in calls if base(f) in set(undef)]
seen = closure(targets)
forbid = uses_ptr | enter
hits = sorted(f for f in seen if f in forbid)
print('ovlreach: %-30s %4d functions reached from %d overlay imports, %d through pointers not followed'
      % ('overlay imports:', len(seen), len(targets), len(sorted(f for f in seen if f in indirect))))
for h in hits:
    print('ovlreach: ERROR the overlay reaches a ptrcache user: %s' % path(seen, h))
    bad += 1
print('ovlreach: ptrcache users: %s' % ', '.join(sorted(uses_ptr)))
sys.exit(1 if bad else 0)
