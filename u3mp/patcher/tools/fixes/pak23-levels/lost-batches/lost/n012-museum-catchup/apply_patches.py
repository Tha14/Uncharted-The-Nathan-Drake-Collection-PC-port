#!/usr/bin/env python3
"""apply_patches.py (fixer L, n012-museum-catchup): bring the installed Museum paks up to the converter fixes every other map
already has (batch92 shadow-only flag, batch96 blocker hide, batch94 prototype fade distances).
usage:  python3 apply_patches.py <overlay-r3 pak23/ps4 dir> <backup dir> [--dry] [--revert]
Per pak (patches/<pak>.json): md5 must be md5_before (the PC overlay-r3 file as of 2026-10-29 23:30 UTC); then the in-place
instance-submesh writes (+0x20 |= 0x10000 / 0x1010000, +0x24 |= 0x101) -> md5_inplace; then, museum-multi.pak only, the fade page
is appended (prototype +0x48 -> {start, end, 0, 0}) -> md5_after. Any other md5 = SKIP. The original is copied to the backup
dir before the first write; --revert copies it back. Standalone (python 3 only)."""
import sys, os, json, struct, hashlib, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
PAD = 0xaa
def align(n, a):
    return (n + a - 1) // a * a


def parse(d):
    h = struct.unpack_from('<HHIIIIIIIIII', d, 0)
    magic, flags, hdr_size, lp, lo, pagect, peo, nfix, fixdir, vram_size, vp, vo = h
    assert magic == 0x0a79 and peo == 0x2c and nfix == 8
    pages = [struct.unpack_from('<III', d, 0x2c + 12 * i) for i in range(pagect)]
    fix = []
    for i in range(8):
        pg, off, cnt = struct.unpack_from('<iii', d, fixdir + 12 * i)
        base = pg * 0x80000
        for j in range(max(cnt, 0)):
            fix.append(struct.unpack_from('<HHI', d, base + off + 8 * j))
    return dict(flags=flags, lp=lp, lo=lo, vp=vp, vo=vo, pages=pages, fix=fix, vram=d[len(d) - vram_size:] if vram_size else b'')


def build(d, rec, revert=False):
    P = parse(d)
    pages = [bytearray(d[po:po + ps]) for po, ps, _u in P['pages']]
    fix = [tuple(x) for x in P['fix']]
    if revert:   # exact inverse: drop the appended page and its fixups, zero the +0x48 fields
        assert bytes(pages[-1]) == bytes.fromhex(rec['newpage'])
        pages.pop()
        nf = [tuple(x) for x in rec['newfix']]
        assert fix[-len(nf):] == nf
        fix = fix[:-len(nf)]
        for pg, off, val in rec['writes']:
            assert struct.unpack_from('<Q', pages[pg], off)[0] == val
            struct.pack_into('<Q', pages[pg], off, 0)
    else:
        for pg, off, val in rec['writes']:
            assert struct.unpack_from('<Q', pages[pg], off)[0] == 0
            struct.pack_into('<Q', pages[pg], off, val)
        pages.append(bytearray.fromhex(rec['newpage']))
        fix += [tuple(x) for x in rec['newfix']]
    pagect = len(pages)
    fixdir_off = 0x2c + 12 * pagect
    fix_off = align(fixdir_off + 8 * 12, 16)
    hdr_size = fix_off + 8 * len(fix)
    offs = []
    o = align(hdr_size, 16)
    for p in pages:
        offs.append(o)
        o = align(o + len(p), 16)
    vram_base = offs[-1] + len(pages[-1])
    hdr = bytearray(struct.pack('<HHIIIIIIIIII', 0x0a79, P['flags'], hdr_size, P['lp'], P['lo'], pagect, 0x2c, 8,
                                fixdir_off, len(P['vram']), P['vp'], P['vo']))
    for p, po in zip(pages, offs):
        hdr += struct.pack('<III', po, len(p), 0)
    hdr += struct.pack('<III', 0, fix_off, len(fix))
    hdr += struct.pack('<III', 0, 0, 0) * 7
    while len(hdr) < fix_off:
        hdr.append(PAD)
    for s, dp, off in fix:
        hdr += struct.pack('<HHI', s, dp, off)
    out = bytearray(hdr)
    for p, po in zip(pages, offs):
        while len(out) < po:
            out.append(PAD)
        out += p
    while len(out) < vram_base:
        out.append(PAD)
    out += P['vram']
    return bytes(out)


def main(argv):
    d = argv[0]; dry = '--dry' in argv; rev = '--revert' in argv
    only = set(argv[argv.index('--only') + 1:]) if '--only' in argv else None
    pdir = os.path.join(HERE, 'patches')
    for jf in sorted(os.listdir(pdir)):
        rec = json.load(open(os.path.join(pdir, jf)))
        f = rec['pak']
        if only and f not in only:
            continue


def md5(b): return hashlib.md5(b).hexdigest()


def main(argv):
    a = [x for x in argv if not x.startswith('--')]; dry = '--dry' in argv; rev = '--revert' in argv
    pak_dir, bak = a[0], a[1]; ok = skip = 0
    for jf in sorted(os.listdir(os.path.join(HERE, 'patches'))):
        R = json.load(open(os.path.join(HERE, 'patches', jf))); f = R['pak']
        dst = os.path.join(pak_dir, f); b = os.path.join(bak, f)
        if rev:
            if os.path.exists(b) and md5(open(b, 'rb').read()) == R['md5_before']:
                if not dry: shutil.copyfile(b, dst)
                print('reverted', f); ok += 1
            else: print('no backup for', f); skip += 1
            continue
        if not os.path.exists(dst): print('MISSING', f); skip += 1; continue
        d = bytearray(open(dst, 'rb').read()); m = md5(d)
        if m == R['md5_after']: print('already', f); ok += 1; continue
        if m != R['md5_before']: print('SKIP (md5 %s, expected %s)' % (m, R['md5_before']), f); skip += 1; continue
        for o, old, new in R['writes_b92'] + R['writes_b96']:
            assert struct.unpack_from('<I', d, o)[0] == int(old, 16), (f, o)
            struct.pack_into('<I', d, o, int(new, 16))
        if md5(d) != R['md5_inplace']: print('FAILED in-place md5', f); skip += 1; continue
        out = bytes(d)
        if 'fade' in R:
            out = build(out, R['fade'])
        if md5(out) != R['md5_after']: print('FAILED final md5', f); skip += 1; continue
        if dry: print('would patch', f, len(R['writes_b92']), '+', len(R['writes_b96']), 'words', '+ fade' if 'fade' in R else ''); ok += 1; continue
        os.makedirs(bak, exist_ok=True)
        if not os.path.exists(b): shutil.copyfile(dst, b)
        open(dst, 'wb').write(out)
        print('patched', f, R['md5_after']); ok += 1
    print('ok', ok, 'skipped', skip)


if __name__ == '__main__':
    main(sys.argv[1:])
