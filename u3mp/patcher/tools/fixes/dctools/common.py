"""dctools/common.py: shared DC helpers of the dctools scripts.

Restored 2026-11-05 from the session transcripts: the original (the project's own
u3mp/dctools/common.py, 1445 bytes) was printed in full on 2026-11-01 15:09 (sub-agent
af07dd9685e27624a, line 45) and at 00:21 (main transcript line 5817). It was a thin layer over
paktools/dc.py: `from dc import DC, sid, dump` plus the cloud-sandbox paths below and three helpers.
The only local change then was the sys.path line (pointed at scratchpad/gh2/u3mp/paktools).

Here dc.py is loaded by file path (no name lookup, so the unrelated tools/fixes/dc.py script that the
recipe renders into scratch/ can never shadow it), in this order: a dc.py next to this file (the
maps-batch lib/ convention), $PAKTOOLS, then ../../paktools and ../paktools relative to this file.
If none exists, an embedded copy of the same reader is used (identical code to paktools/dc.py)."""
import importlib.util
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load_dc():
    cands = [os.path.join(_HERE, 'dc.py')]
    if os.environ.get('PAKTOOLS'):
        cands.append(os.path.join(os.environ['PAKTOOLS'], 'dc.py'))
    cands += [os.path.join(_HERE, '..', '..', 'paktools', 'dc.py'), os.path.join(_HERE, '..', 'paktools', 'dc.py')]
    for p in cands:
        if os.path.isfile(p):
            src = open(p, encoding='utf-8', errors='replace').read()
            if 'class DC' not in src or 'def sid' not in src or 'def dump' not in src:
                continue                                   # not the reader (e.g. an unrelated dc.py script)
            spec = importlib.util.spec_from_file_location('dc', p)
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
            sys.modules.setdefault('dc', m)
            return m
    return None


_dc = _load_dc()
if _dc is not None:
    DC, sid, dump = _dc.DC, _dc.sid, _dc.dump
else:                                                      # embedded copy of paktools/dc.py (GPL v2, same code)
    def sid(s):
        """CRC32 poly 0x04C11DB7 MSB-first, init 0 (Naughty Dog string id)."""
        c = 0
        for ch in s.encode():
            c ^= ch << 24
            for _ in range(8):
                c = ((c << 1) ^ 0x04C11DB7) & 0xffffffff if c & 0x80000000 else (c << 1) & 0xffffffff
        return c

    class DC:
        def __init__(self, path):
            self.d = d = open(path, 'rb').read()
            self.path = path
            if d[:4] == b'DC00':
                self.be, self.ps = True, 4
            elif d[:4] == b'00CD':
                self.be, self.ps = False, 8
            else:
                raise ValueError('not DC: %r' % d[:4])
            e = '>' if self.be else '<'
            self.e = e
            if self.ps == 4:
                self.reloc = struct.unpack_from(e + 'I', d, 8)[0]
            else:
                self.reloc = struct.unpack_from(e + 'Q', d, 8)[0]
            n = struct.unpack_from(e + 'I', d, 0x14)[0]
            self.syms = []
            o = 0x18
            for i in range(n):
                if self.ps == 4:
                    s, t, off = struct.unpack_from(e + 'III', d, o); o += 12
                else:
                    s, t = struct.unpack_from(e + 'II', d, o); off = struct.unpack_from(e + 'Q', d, o + 8)[0]; o += 16
                self.syms.append((s, t, off))
            self.bysid = {s: (t, off) for s, t, off in self.syms}
            # relocation bitmap
            self.ptrs = set()
            if self.reloc and self.reloc < len(d):
                cnt = struct.unpack_from(e + 'I', d, self.reloc)[0]
                bm = d[self.reloc + 4:self.reloc + 4 + cnt]
                for bi, byte in enumerate(bm):
                    if byte:
                        for k in range(8):
                            if byte & (1 << k):
                                self.ptrs.add((bi * 8 + (7 - k if self.be else k)) * self.ps)

        def u32(self, o): return struct.unpack_from(self.e + 'I', self.d, o)[0]
        def i32(self, o): return struct.unpack_from(self.e + 'i', self.d, o)[0]
        def f32(self, o): return struct.unpack_from(self.e + 'f', self.d, o)[0]
        def ptr(self, o):
            return struct.unpack_from(self.e + ('I' if self.ps == 4 else 'Q'), self.d, o)[0]
        def isptr(self, o): return o in self.ptrs
        def cstr(self, o):
            if not o or o >= len(self.d): return None
            z = self.d.find(b'\0', o)
            return self.d[o:z].decode('latin1')

        def sym(self, s):
            if isinstance(s, str): s = sid(s)
            return self.bysid.get(s)

    def dump(c, off, n, indent=''):
        """dump n pointer-size words with pointer/string annotation"""
        out = []
        for k in range(0, n * c.ps, c.ps):
            o = off + k
            if c.isptr(o):
                v = c.ptr(o); s = c.cstr(v)
                if s and len(s) > 1 and all(32 <= ord(ch) < 127 for ch in s):
                    out.append('%s%05x: -> %05x "%s"' % (indent, o, v, s[:60]))
                else:
                    out.append('%s%05x: -> %05x' % (indent, o, v))
            else:
                if c.ps == 8:
                    a, b = struct.unpack_from(c.e + 'II', c.d, o)
                    out.append('%s%05x: %08x %08x' % (indent, o, a, b))
                else:
                    out.append('%s%05x: %08x' % (indent, o, c.u32(o)))
        return '\n'.join(out)

# Paths of the original (cloud sandbox, 2026-10-25..30). Kept for reference only; no script here uses them.
P4R = "/mnt/user-data/uploads/Uncharted - The Nathan Drake Collection [CUSA02320]/u3data/build/main/dc1/ps4/"
P4 = "/mnt/user-data/outputs/dc/u3data/build/main/dc1/ps4/net.bin"
P3 = "/home/claude/u3/ps3pkg/tree/dlc-patch18/build/main/bin/dc1/net.bin"
P3D = "/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/ps3dc/net.bin/build/main/bin/dc1/net.bin"
P3_13 = "/home/claude/u3/ps3pkg/tree/dlc-patch13/build/main/bin/dc1/net.bin"
P3DIR = "/home/claude/u3/ps3pkg/tree/dlc-patch18/build/main/bin/dc1/"
PAL = 0x290349e3


def objtype(c, o):
    return c.u32(o - 4)   # type id precedes object on both (PS4: [0][type])


def part_entries(c):
    """find part entries: pointer to an 'mp-' artgroup string at entry+ps, palette sid at +0x2c/+0x20"""
    pal = {s for s, t, o in c.syms if t == PAL}
    es, po = (0x38, 0x2c) if c.ps == 8 else (0x2c, 0x20)
    r = {}
    for q in sorted(c.ptrs):
        e = q - c.ps
        if e < 0: continue
        s = c.cstr(c.ptr(q))
        if not s or not s.startswith('mp-'): continue
        if c.u32(e + po) not in pal and c.u32(e + po) != 0: continue
        r.setdefault(e, s)
    return r


def fields(c, e):
    if c.ps == 8:
        return [c.u32(e + k) for k in (0x10, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34)]
    return [c.u32(e + k) for k in (0x8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28)]
