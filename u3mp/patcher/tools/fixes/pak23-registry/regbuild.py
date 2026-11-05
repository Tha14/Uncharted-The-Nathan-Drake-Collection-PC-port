#!/usr/bin/env python3
"""Replay a regprov.py recipe: write an overlay pak23_ps4.txt from the user's own game texts (regsources.py output).
usage: regbuild.py <recipe.json> <sources dir> <out pak23_ps4.txt> [--sha256 HEX]
Idempotent: if <out> already has the expected content nothing is written. Exit 1 when the result does not match
--sha256 (the reference hash), so a region or version with different game texts is reported, not installed."""
import hashlib
import json
import os
import sys

CR = chr(13)


def build(recipe, srcdir):
    cache = {}

    def lines(sid):
        if sid not in cache:
            d = open(os.path.join(srcdir, sid + '.txt'), 'rb').read().decode('latin1').split('\n')
            cache[sid] = [x[:-1] if x.endswith(CR) else x for x in d]
        return cache[sid]

    out = []
    for op in recipe['ops']:
        if op[0] == 'L':
            out += [x + (CR if op[2] else '') for x in op[1]]
        else:
            _, sid, i, n, crlf, strip = op
            seg = lines(sid)[i:i + n]
            if len(seg) != n:
                raise SystemExit('source %s too short (%d lines from %d)' % (sid, n, i))
            out += [(x.rstrip() if strip else x) + (CR if crlf else '') for x in seg]
    return '\n'.join(out).encode('latin1')


def main():
    rec, srcdir, dst = sys.argv[1:4]
    want = sys.argv[sys.argv.index('--sha256') + 1] if '--sha256' in sys.argv else None
    data = build(json.load(open(rec)), srcdir)
    h = hashlib.sha256(data).hexdigest()
    if want and h != want:
        print('MISMATCH %s: built %s, reference %s (not written)' % (dst, h[:16], want[:16]))
        return 1
    if os.path.exists(dst) and open(dst, 'rb').read() == data:
        print('already applied', dst)
        return 0
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst + '.tmp', 'wb').write(data)
    os.replace(dst + '.tmp', dst)
    print('wrote %s (%d bytes, sha256 %s)' % (dst, len(data), h[:16]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
