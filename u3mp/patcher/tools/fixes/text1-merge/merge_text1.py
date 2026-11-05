#!/usr/bin/env python3
"""overlay-r3 text1: PS4 core.psarc text banks 2..23 merged with the PS3 multiplayer strings.

Rebuilt 2026-11-05 from the 66 reference files (the cloud merge script of 2026-10-27 was lost); output is
byte-identical to the reference overlay for all 66 files.

Text bank layout (big-endian on PS3 and PS4 alike): u32 count; count x (u32 hash, u32 offset) sorted by hash;
then the string blob, offsets relative to the blob start, strings NUL-terminated.

Merge rule (found from the reference files):
  * start from the PS4 bank <lang>.<cat> of u3data/build/main/core.psarc (text1/ps4/), blob kept as is;
  * for every key of KEYS[cat] in hash order whose string in the PS3 GOTY patch (goty-patch.psarc
    build/main/text1/<lang>.<cat>) is missing in the PS4 bank or differs from it: append the PS3 string to the
    blob and point the record (new or existing) at it;
  * then append the helper's own message CUSTOM (English in every language) to <lang>.common.
Languages 2..23 (PS4 numbering), categories common, networking, subtitles (66 files; .web is not merged).

usage: merge_text1.py [--ps4 DIR] [--ps3-disc DIR] [--out DIR] [--apply]
       dry run by default (reports what would change); --apply writes, backing up replaced files to
       work/backup-text1-merge/ once. Idempotent: files already equal to the merge result are left alone."""
import argparse, os, shutil, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
for p in (os.environ.get('RECIPE_TRANSFER_TOOLS', ''), r"{{TRANSFER_TOOLS}}", os.path.join(HERE, '..', '..', 'transfer-tools')):
    if p and os.path.isdir(p) and p not in sys.path:
        sys.path.insert(0, p)
from psarc_lib import Psarc

KEYS = {
    'common': [0x23a8bc26, 0x5a32b147, 0xa13fea91, 0xeaff5517, 0xec7dd02d],
    'networking': [0x06b674d2, 0x0bf5520b, 0x0d751ea9, 0x2337fc6d, 0x292e3933, 0x3142b4b9, 0x34a82676, 0x3c019260,
                   0x40b4fcf5, 0x4936c79b, 0x4b9fc741, 0x4df7da2c, 0x4fb13f09, 0x5089f550, 0x53b08a29, 0x5448e8e7,
                   0x5771979e, 0x5a32b147, 0x5b81d425, 0x5dcad389, 0x5ef3acf0, 0x627d0cfa, 0x66091197, 0x6632d07d,
                   0x66bc114d, 0x68b998bd, 0x6d39fb20, 0x71fa6cb2, 0x72ce8aea, 0x80312981, 0x80c4bacf, 0x86dcad2f,
                   0x8d720f58, 0xa56b5768, 0xb5864132, 0xb5a7aba3, 0xbb8baf42, 0xe012fa1d, 0xeba721fe, 0xecb2dcd1,
                   0xfebc1573],
    'subtitles': [0x9291ae5e],
}
CUSTOM = {'common': [(0x4a93e6b9, b'Your party does not have enough players for the selected game.')]}
LANGS = range(2, 24)
CATS = ('common', 'networking', 'subtitles')


def parse(d):
    n = struct.unpack_from('>I', d, 0)[0]
    recs = dict(struct.unpack_from('>II', d, 4 + 8 * i) for i in range(n))
    blob = d[4 + 8 * n:]
    return recs, blob


def string_at(blob, o):
    return blob[o:blob.index(b'\0', o)]


def merge(ps4, ps3, cat):
    recs, blob = parse(ps4)
    p3recs, p3blob = parse(ps3)
    blob = bytearray(blob)
    changed = []
    for h in KEYS[cat]:
        if h not in p3recs:
            continue
        s = string_at(p3blob, p3recs[h])
        if h in recs and string_at(blob, recs[h]) == s:
            continue
        recs[h] = len(blob); blob += s + b'\0'; changed.append(h)
    for h, s in CUSTOM.get(cat, []):
        recs[h] = len(blob); blob += s + b'\0'; changed.append(h)
    hs = sorted(recs)
    out = struct.pack('>I', len(hs)) + b''.join(struct.pack('>II', h, recs[h]) for h in hs) + bytes(blob)
    return out, changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ps4', default=r"{{PS4}}")
    ap.add_argument('--ps3-disc', default=r"{{PS3_DISC}}")
    ap.add_argument('--out', default=r"{{PROJECT}}/work/overlay-r3/u3data/build/main/text1/ps4")
    ap.add_argument('--backup', default=r"{{PROJECT}}/work/backup-text1-merge")
    ap.add_argument('--apply', action='store_true')
    A = ap.parse_args()
    core = Psarc(os.path.join(A.ps4, 'u3data', 'build', 'main', 'core.psarc'))
    gp = Psarc(os.path.join(A.ps3_disc, 'goty-patch.psarc'))
    idx4 = {n: i + 1 for i, n in enumerate(core.names)}
    idx3 = {n: i + 1 for i, n in enumerate(gp.names)}
    nw = ns = 0
    for L in LANGS:
        for cat in CATS:
            f = '%d.%s' % (L, cat)
            k4 = 'text1/ps4/' + f; k3 = 'build/main/text1/' + f
            if k4 not in idx4 or k3 not in idx3:
                sys.exit('missing input %s (PS4 core.psarc) or %s (PS3 goty-patch.psarc)' % (k4, k3))
            out, changed = merge(core.read(idx4[k4]), gp.read(idx3[k3]), cat)
            dst = os.path.join(A.out, f)
            if os.path.isfile(dst) and open(dst, 'rb').read() == out:
                ns += 1; continue
            print('%-16s %d strings merged%s' % (f, len(changed), '' if A.apply else ' (dry run)'))
            if A.apply:
                os.makedirs(A.out, exist_ok=True)
                if os.path.isfile(dst):
                    os.makedirs(A.backup, exist_ok=True)
                    bk = os.path.join(A.backup, f)
                    if not os.path.exists(bk):
                        shutil.copyfile(dst, bk)
                tmp = dst + '.tmp'; open(tmp, 'wb').write(out); os.replace(tmp, dst)
            nw += 1
    if nw == 0:
        print('already applied: %d text banks up to date' % ns)
    else:
        print('%s %d text banks (%d already up to date)' % ('wrote' if A.apply else 'would write', nw, ns))


if __name__ == '__main__':
    main()
