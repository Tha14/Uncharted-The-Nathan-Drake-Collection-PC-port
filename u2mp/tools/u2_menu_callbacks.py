"""u2_menu_callbacks.py - pair the menu control callbacks registered by Uncharted 2 PS3
(decrypted EBOOT.elf) with those of the PS4 Collection (prepared flat image), keyed by the
control's string id (CRC-32 MSB-first). Read-only; prints addresses and ids only.

Usage:
  python u2_menu_callbacks.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> [--bias 183ce8] [--names FILE]

PS3: every `bl <register>` whose r4 is a 32-bit constant (lis/oris+ori) and r5 an OPD
loaded from the TOC pool, inside the functions that call the registration routines found
from BUTTON_MULTIPLAYER (0xed9d8ae1).
PS4: every `mov dword [rbx+8*rax+disp32], imm32 ; lea rcx, [rip+rel32]` (record id + callback).
For each PS4 callback the first bytes tell whether it is a bare `ret` (stripped).
--names: optional text file of control/page names; their CRC ids label the rows.
"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps3_elf as P
import ps3_map as M
import ps4_xref as X


def ps3_pairs(an):
    refs_at = {}
    for s, v, k in an.refs:
        refs_at.setdefault(s, []).append((v, k))
    # registration routines = targets of bl right after the ed9d8ae1 constant
    regs = set()
    for s, k in an.byval.get(0xed9d8ae1, []):
        for cs, t in an.calls:
            if s < cs <= s + 16:
                regs.add(t)
                break
    callers = set()
    for t in regs:
        for cs in an.callers.get(t, []):
            callers.add(an.func_of(cs))
    # second registration family (page/state callbacks): routines called >= 4 times from
    # the same registering functions with an OPD in r5
    import collections
    cnt = collections.Counter()
    for f in callers:
        end = an.func_end(f)
        for cs, t in an.calls:
            if f <= cs < end and any(v in an.opd for a in range(cs - 24, cs, 4)
                                     for v, k in refs_at.get(a, []) if k == 'a'):
                cnt[t] += 1
    regs |= {t for t, n in cnt.items() if n >= 4}
    out = []
    for f in sorted(callers):
        end = an.func_end(f)
        last_k, last_opd = None, None
        calls = {cs: t for cs, t in an.calls if f <= cs < end}
        for a in range(f, end, 4):
            for v, k in refs_at.get(a, []):
                if k == 'k':
                    last_k = v
                elif k == 'a' and v in an.opd:
                    last_opd = v
            if a in calls and calls[a] in regs:
                out.append((last_k, an.opd.get(last_opd), a, calls[a]))
                last_k = last_opd = None
    return out


def ps4_pairs(im):
    d = im.d
    out = {}
    for m in re.finditer(rb'\xc7\x84\xc3(....)(....)\x48\x8d\x0d(....)', d, re.S):
        disp, = struct.unpack('<I', m.group(1))
        sid, = struct.unpack('<I', m.group(2))
        rel, = struct.unpack('<i', m.group(3))
        if disp > 0x10000:
            continue
        lea = m.start() + 11
        t = lea + 7 + rel
        if not (0 <= t < len(d)):
            continue
        if any(x[0] == im.o2a(m.start()) for x in out.get(sid, [])):
            continue
        out.setdefault(sid, []).append((im.o2a(m.start()), im.o2a(t), d[t] == 0xc3, disp))
    return out


def main(argv):
    argv = list(argv)
    bias = 0x183ce8
    names = {}
    if '--bias' in argv:
        i = argv.index('--bias'); bias = int(argv[i + 1], 16); del argv[i:i + 2]
    if '--names' in argv:
        i = argv.index('--names')
        for n in open(argv[i + 1]).read().split():
            names[M.sid(n)] = n
        del argv[i:i + 2]
    if len(argv) < 3:
        print(__doc__)
        return 1
    an = P.load(argv[1], M.DEF_CACHE)
    im = X.Img(argv[2], bias)
    p4 = ps4_pairs(im)
    print('# sid      name                          PS3 site  PS3 callback  reg      | PS4 site  PS4 callback  stripped')
    seen = set()
    for sid, cb, site, reg in ps3_pairs(an):
        r = p4.get(sid, [])
        seen.add(sid)
        p = '; '.join('%x %x %s' % (s, c, 'RET' if ret else 'code') for s, c, ret, _ in r) or '-'
        print('%-8s %-30s %08x  %-12s  %x | %s' % ('%08x' % sid if sid is not None else '?', names.get(sid, ''),
                                                   site, '%x' % cb if cb else '?', reg, p))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
