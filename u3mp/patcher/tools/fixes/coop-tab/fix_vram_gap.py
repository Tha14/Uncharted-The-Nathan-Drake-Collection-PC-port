#!/usr/bin/env python3
"""fix_vram_gap.py - repair the paks the v7.38 fade-record fix (scratch/london/apply_london.py) rebuilt with a gap
between the last page and the VRAM data.

Cause (runtime-tests baseline-1793668096216144900, net-trace level slots): coop-airport-1, coop-airport-2,
coop-airport-wide, coop-syria-turret1, coop-syria-walls go 2 -> 12 (level error 0xc) right after their pak is read;
the same for ldn-train-multi-tunnel-2 in the v7.38 London run.  Every one of them was rebuilt by ps4writer.py, which
aligns the VRAM start to 16 (vram_base = align(last page end, 16)), while every pak the game loads has the VRAM data
starting exactly at the end of the last page: the PS4 package loader streams the file and takes the VRAM bytes from
where the last page ends (there is no VRAM file-offset field in the header, only vramSize).  Rebuilt paks whose last
page happens to end on a 16-byte boundary (coop-airport-3a, -3b, -stage, coop-syria-stage, ldn-train-multi, ...)
got no padding and load fine; the ones whose last page ends 8 mod 16 got 8 pad bytes and fail.  The fade records
themselves are fine (this keeps them).

Repair: delete the pad bytes between the last page end and (file length - vramSize).  Nothing in the header points at
the VRAM start, so no other byte changes.

usage: fix_vram_gap.py [--apply] [--restore] [--dir DIR]
  default    dry run over overlay-r3 pak23/ps4 (and actor33/ps4): lists every pak with a gap, writes nothing
  --apply    back up to work/backup-vramgap/ (never overwritten), then write the repaired paks into overlay-r3
  --restore  copy the backups back (dry run unless --apply is also given); refuses while the game runs
  --dir DIR  scan another folder of paks instead (dry run only)
"""
import hashlib, json, os, shutil, struct, subprocess, sys

P = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # project root
OV = os.path.join(P, 'work', 'overlay-r3', 'u3data', 'build', 'main')
SCAN = [os.path.join(OV, 'pak23', 'ps4'), os.path.join(OV, 'actor33', 'ps4')]
BK = os.path.join(P, 'work', 'backup-vramgap')
MAN = os.path.join(BK, 'manifest.json')
md5 = lambda b: hashlib.md5(b).hexdigest()


def game_running():
    try:
        return 'uncharted' in subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower()
    except Exception:
        return False


def analyse(d):
    """-> (last page end, vram_base, gap) or None when not a PS4 pak we understand"""
    if len(d) < 0x2c or struct.unpack_from('<H', d, 0)[0] != 0x0a79:
        return None
    (magic, flags, hdr_size, login_pg, login_off, page_ct, page_ent_off, n_fix, fix_off,
     vram_size, vram_pg, vram_off) = struct.unpack_from('<HHIIIIIIIIII', d, 0)
    if page_ct == 0 or page_ct > 4096 or page_ent_off + 12 * page_ct > len(d):
        return None
    end = 0
    for i in range(page_ct):
        off, size, _ = struct.unpack_from('<III', d, page_ent_off + 12 * i)
        if off + size > len(d):
            return None
        end = max(end, off + size)
    vram_base = len(d) - vram_size
    return end, vram_base, vram_base - end


def repair(d, end, vram_base):
    out = d[:end] + d[vram_base:]
    a = analyse(out)
    assert a and a[2] == 0, 'repair check failed'
    return out


def main():
    a = sys.argv[1:]
    apply_ = '--apply' in a
    if '--restore' in a:
        if apply_ and game_running():
            sys.exit('game running: stop it first')
        if not os.path.exists(MAN):
            sys.exit('no backups in ' + BK)
        man = json.load(open(MAN))
        for rel, m in man.items():
            src = os.path.join(BK, rel)
            if not os.path.exists(src):
                print('no backup', rel); continue
            if md5(open(src, 'rb').read()) != m['md5_before']:
                sys.exit('backup md5 differs: ' + rel)
            print('restore', rel)
            if apply_:
                shutil.copy2(src, os.path.join(OV, rel))
        print('restored' if apply_ else 'dry run: nothing written (use --restore --apply)')
        return
    dirs = [a[a.index('--dir') + 1]] if '--dir' in a else SCAN
    if '--dir' in a:
        apply_ = False
    if apply_ and game_running():
        sys.exit('game running: stop it first')
    man = json.load(open(MAN)) if os.path.exists(MAN) else {}
    found = 0; bad = 0; scanned = 0
    for dd in dirs:
        if not os.path.isdir(dd):
            continue
        for f in sorted(os.listdir(dd)):
            if not f.endswith('.pak'):
                continue
            path = os.path.join(dd, f)
            d = open(path, 'rb').read()
            r = analyse(d)
            scanned += 1
            if r is None:
                continue
            end, vb, gap = r
            if gap == 0:
                continue
            if gap < 0 or gap > 15:
                bad += 1
                print('%-45s gap %d: not a padding gap, left alone' % (f, gap)); continue
            found += 1
            rel = os.path.relpath(path, OV)
            print('%-45s last page ends %#x, VRAM at %#x: %d pad bytes -> removed (%d -> %d bytes)' % (f, end, vb, gap, len(d), len(d) - gap))
            if apply_:
                new = repair(d, end, vb)
                dst = os.path.join(BK, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not os.path.exists(dst):
                    shutil.copy2(path, dst)
                    man[rel] = dict(md5_before=md5(d))
                elif md5(open(dst, 'rb').read()) != man.get(rel, {}).get('md5_before'):
                    sys.exit('existing backup of %s does not match the manifest' % rel)
                open(path, 'wb').write(new)
                man[rel]['md5_after'] = md5(new); man[rel]['gap'] = gap
                json.dump(man, open(MAN, 'w'), indent=1)
    print('scanned %d paks, %d with a VRAM gap%s' % (scanned, found, ', %d odd (left alone)' % bad if bad else ''))
    if not apply_:
        print('dry run: nothing written (use --apply)')


if __name__ == '__main__':
    main()
