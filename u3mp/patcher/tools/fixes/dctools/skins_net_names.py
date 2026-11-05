"""Weapon skins: full material names in net.bin and weapon-art.bin + modules.bin size table.

Script form of the inline code of 2026-11-01 13:37 (sk7.py = recovered/sk7.py head + the part appended by
transcript-mining/inline-writes/013.py, then 014.py for modules.bin). Rule: every weapon-mod art path
'objects/weapons/<w>/<w>-mod/<name>' in net.bin (*weapon-mods* tables 0xe77d8fba / 0xa94f33c0, entry 0x28,
fields +0x18/+0x20) and weapon-art.bin (0x61d589d9 variants +0x58/+0x60) is repointed to the full
'path:name,guid,prime-shader.fx' string that the converted weapon actor paks in overlay-r3 actor33/ps4 carry
(appended strings, DCEdit). The 13:28 sk6 edit ('/' -> ':' in place) was superseded by this one (both start
from work/backup-skins). Needs the converted actor33 weapon paks.
Dry run by default; --apply writes (backup once to work/backup-skins). Idempotent: exits 0 when net.bin
already points at full names."""
import glob, os, re, shutil, struct, sys
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC
from dcedit import DCEdit
W = "{{PROJECT}}/work/overlay-r3/u3data/build/main/"
D = W + 'dc1/ps4/'; B = "{{PROJECT}}/work/backup-skins/"
full = defaultdict(set)
for f in glob.glob(glob.escape(W + 'actor33/ps4/') + '*.pak'):
    d = open(f, 'rb').read()
    if b'objects/weapons/' not in d: continue
    for x in re.findall(rb'objects/weapons/[a-zA-Z0-9_./-]+:[a-zA-Z0-9_.-]+(?:,[^\0]*)?\0', d):
        x = x[:-1].decode('latin-1'); full[x.split(',')[0]].add(x)
multi = {k: sorted(v) for k, v in full.items() if len(v) > 1}
if multi: print('warning: several full names for', list(multi)[:5], '(the first in sort order is used)')


def net(c):
    r = []
    for sy in (0xe77d8fba, 0xa94f33c0):
        t, o = c.bysid[sy]; a = c.ptr(o + 8)
        for i in range(c.u32(o)): r += [a + i * 0x28 + 0x18, a + i * 0x28 + 0x20]
    return r


def art(c):
    t, o = c.bysid[0x61d589d9]; vs = c.ptr(o + 16); r = []
    for i in range(c.u32(o)):
        v = c.ptr(vs + i * 8)
        if v: r += [v + 0x58, v + 0x60]
    return r


def fix(src, ptrs_of):
    c = DC(src); e = DCEdit(src); n = 0; miss = []; cache = {}; done = 0
    for q in ptrs_of(c):
        p = c.ptr(q)
        if not p: continue
        s = c.cstr(p)
        if not s or not s.startswith('objects/weapons/'): continue
        if ',' in s: done += 1; continue
        k = ':'.join(s.rsplit('/', 1)) if ':' not in s else s
        if k in full:
            f = sorted(full[k])[0]
            if f not in cache: cache[f] = e.add_string(f)
            e.putptr(q, cache[f]); n += 1
        else: miss.append(k)
    return e.build(), n, done, miss


cn = DC(D + 'net.bin')
if any(',' in (cn.cstr(cn.ptr(q)) or '') for q in net(cn) if cn.ptr(q)):
    print('already applied: net.bin weapon-mod art names are full names'); sys.exit(0)
if not full: raise SystemExit('no converted weapon actor paks with full material names in ' + W + 'actor33/ps4')
outs = {}
for fn, pf in (('net.bin', net), ('weapon-art.bin', art)):
    out, n, done, miss = fix(D + fn, pf); outs[fn] = out
    print(fn, 'repointed', n, 'already full', done, 'miss', miss[:8], os.path.getsize(D + fn), '->', len(out))
m = bytearray(open(D + 'modules.bin', 'rb').read())
for fn in outs:
    b = struct.pack('<Q', os.path.getsize(D + fn)); hits = [i for i in range(len(m) - 7) if m[i:i + 8] == b]
    assert len(hits) == 1, (fn, hits); struct.pack_into('<Q', m, hits[0], len(outs[fn]))
if '--apply' not in sys.argv:
    print('dry run, nothing written'); sys.exit(0)
os.makedirs(B, exist_ok=True)
for f in ('net.bin', 'weapon-art.bin', 'modules.bin'):
    if not os.path.exists(B + f): shutil.copy2(D + f, B + f)
for fn, out in outs.items(): open(D + fn, 'wb').write(out)
open(D + 'modules.bin', 'wb').write(m); print('written')
