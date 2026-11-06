"""Build an overlay pak23_ps4.txt that adds PS3 level definitions (PS4 format) for ported levels.

usage: python mkregistry.py <ps4 build/main> <ps3 pak23.txt> <overlay build/main> level [level...]

Only entries the PS4 data (or the overlay) can satisfy are written: packages, actors, sound banks
and particle modules must exist as files; symbols must be defined in a PS4 DC module. Anything
dropped is reported, because a level that waits on a missing dependency never finishes loading.
"""
import os, re, struct, sys


def sid(s):
    c = 0
    for ch in s.encode():
        c ^= ch << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
    return c


def parse_defs(text):
    defs, order, cur = {}, [], None
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 'level-name':
            cur = parts[1]
            defs[cur] = []
            order.append(cur)
        if cur is not None:
            defs[cur].append(line.rstrip())
    return defs, order


def main():
    ps4, ps3txt, ovl = sys.argv[1:4]
    levels = sys.argv[4:]
    src = os.path.join(ovl, 'pak23_ps4.txt')
    if not os.path.exists(src):
        src = os.path.join(ps4, 'pak23_ps4.txt')
    base = open(src, encoding='latin1', newline='').read().replace('\r\n', '\n')
    print('base', src)
    d4, order4 = parse_defs(base)
    d3, _ = parse_defs(open(ps3txt, encoding='latin1').read())

    roots = [ovl] + [r for r in os.environ.get('MKREG_EXTRA', '').split(os.pathsep) if r] + [ps4]

    def exists(*rel):
        return any(os.path.exists(os.path.join(root, *rel)) for root in roots)

    dc_blobs = []
    for root in roots:
        dcdir = os.path.join(root, 'dc1', 'ps4')
        for dp, _dn, fn in os.walk(dcdir):
            for f in fn:
                if f.endswith('.bin'):
                    dc_blobs.append(open(os.path.join(dp, f), 'rb').read())

    def symbol_ok(name):
        k = struct.pack('<I', sid(name))
        return any(k in b for b in dc_blobs)

    out_defs = {}
    for lv in levels:
        if lv not in d3:
            print('!! %s: not in the PS3 level list' % lv)
            continue
        keep, drop = [], []
        for line in d3[lv]:
            p = line.split()
            kind, val = p[0], (p[1] if len(p) > 1 else '')
            ok = True
            if kind == 'package':
                f = val[:-len('-ingame')] + '-ingame12.pak' if val.endswith('-ingame') else val + '.pak'
                ok = exists('pak23', 'ps4', f)
            elif kind == 'actor':
                ok = exists('actor33', 'ps4', val + '.pak')
            elif kind == 'sound-bank':
                ok = exists('sound1', 'banks', 'ps4', val + '.bnk')
            elif kind == 'particle-module':
                ok = exists('dc2', 'part-module', val + '.bin')
            elif kind == 'symbol':
                ok = symbol_ok(val)
            (keep if ok else drop).append(line)
        out_defs[lv] = keep
        print('%s: %d lines kept, dropped: %s' % (lv, len(keep), ', '.join(' '.join(l.split()[:2]) for l in drop) or '-'))

    lines = []
    for lv in order4:
        if lv in out_defs:
            continue
        lines += d4[lv]
    for lv in levels:
        if lv in out_defs:
            lines += out_defs[lv]
    dst = os.path.join(ovl, 'pak23_ps4.txt')
    os.makedirs(ovl, exist_ok=True)
    head = base[:base.find('level-name')] if 'level-name' in base else ''
    open(dst, 'w', encoding='latin1', newline='').write((head + '\n'.join(lines) + '\n').replace('\n', '\r\n'))
    print('wrote', dst)


if __name__ == '__main__':
    main()
