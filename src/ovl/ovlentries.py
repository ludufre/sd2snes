#!/usr/bin/env python3
"""ovlentries.py <ovlN.raw> <ovlN.nm> <N> <ovl_stub.c> <names...>: the three lists of one
card overlay agree (ovl.h).

names is OVL<N>_RENAME (the Makefile).  The header the overlay was linked with
(ovl/ovl_hdr<N>.c) must hold exactly len(names) entries, entry i being OVL_<names[i]>; and
the resident veneers (ovl_stub.c) must call entry i of overlay N from the function named
names[i], every index once.  A swap or an index out of range links clean and jumps into
the wrong function at run time.  <ovlN.raw> is the .text of the overlay ELF (the header
first), <ovlN.nm> its `nm -S`.  Exit 1 on any mismatch."""
import re, struct, sys

raw_path, nm_path, n, stub_path = sys.argv[1:5]
names = sys.argv[5:]
err = []

# ---- the header the overlay was linked with ----------------------------------------
syms = {}
for l in open(nm_path):
    f = l.split()
    if len(f) == 4:
        syms[f[3]] = (int(f[0], 16), int(f[1], 16))
    elif len(f) == 3:
        syms[f[2]] = (int(f[0], 16), None)
hdr, start = syms.get('ovl_hdr' + n), syms.get('__ovl_start')
if not hdr or hdr[1] is None or not start:
    sys.exit('ovlentries: no ovl_hdr%s / __ovl_start in %s' % (n, nm_path))
if hdr[0] != start[0]:
    sys.exit('ovlentries: ovl_hdr%s is not at the start of the image' % n)
raw = open(raw_path, 'rb').read()
if raw[0:3] != b'SOV' or raw[3] != 0x30 + int(n):
    sys.exit('ovlentries: no overlay %s header at the start of %s' % (n, raw_path))
count = (hdr[1] - 16) // 4
if count != len(names):
    err.append('the header has %d entries, OVL%s_RENAME %d' % (count, n, len(names)))
for i, name in enumerate(names[:count]):
    v, = struct.unpack_from('<I', raw, 16 + 4 * i)
    s = syms.get('OVL_' + name)
    if not s:
        err.append('entry %d: no OVL_%s in the overlay' % (i, name))
    elif v & ~1 != s[0] & ~1:
        err.append('entry %d is 0x%x, OVL_%s is at 0x%x' % (i, v, name, s[0]))

# ---- the veneers -------------------------------------------------------------------
src = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), open(stub_path).read(),
             flags=re.S)
src = re.sub(r'//[^\n]*', '', src)
fn_re = re.compile(r'^[A-Za-z_][\w \t*]*[\s*]([A-Za-z_]\w*)\s*$')
tok_re = re.compile(r'\b[RV]\(\s*(M[12])\s*,\s*(\d+)|\bE\((\d+)\)|\b(M[12])\b')
fn, ovl, used = None, None, {}
for ln, line in enumerate(src.split('\n'), 1):
    if line.startswith('#'):
        continue
    if line[:1].isalpha() or line[:1] == '_':
        m = fn_re.match(line.split('(')[0]) if '(' in line else None
        if m:
            fn = m.group(1)
    for t in tok_re.finditer(line):
        if t.group(4):
            ovl = t.group(4)
            continue
        if t.group(1):
            ovl, idx = t.group(1), int(t.group(2))
        else:
            idx = int(t.group(3))
        if ovl != 'M' + n:
            continue
        where = '%s:%d (%s)' % (stub_path, ln, fn)
        if idx in used:
            err.append('%s calls entry %d, already called from %s' % (where, idx, used[idx]))
            continue
        used[idx] = where
        if idx >= len(names):
            err.append('%s calls entry %d, overlay %s has %d' % (where, idx, n, len(names)))
        elif fn != names[idx]:
            err.append('%s calls entry %d, which is %s' % (where, idx, names[idx]))
for i in range(len(names)):
    if i not in used:
        err.append('no veneer calls entry %d (%s)' % (i, names[i]))

for e in err:
    print('ovlentries: overlay %s: ERROR %s' % (n, e))
if err:
    sys.exit(1)
print('  OVLENTRIES overlay %s: %d entries, header and veneers agree' % (n, len(names)))
