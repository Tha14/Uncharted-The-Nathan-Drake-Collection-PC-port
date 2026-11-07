"""u2_session_scan.py - find every PS4 instruction that touches a session/net global of the
Uncharted 2 (Collection 1.02) image and say what it does (read, write, which value, which
field), grouped by function. Read-only; prints addresses, mnemonics and immediates only.

Input is an llvm-objdump listing of the text (0 .. 0xdf0000) made the same way ps4_cg.py
does it (addresses there are image addresses):
  clang --target=x86_64-linux -c p.s -o p.o   (p.s = .incbin of the text bytes)
  llvm-objdump -d --no-show-raw-insn --x86-asm-syntax=intel p.o > full.dis
Keep full.dis outside the repo (it is derived from game code).

Usage:
  python u2_session_scan.py <full.dis> refs <hexaddr>[:<len>] ...   direct RIP refs to a global
        (len default 1; a range catches field accesses like [rip+X+0x2c4] folded by the
        compiler) and 'lea reg,[X]' followed by [reg+off] uses in the next 40 instructions
  python u2_session_scan.py <full.dis> writes <hexaddr>               only stores to the global,
        with the stored value
  python u2_session_scan.py <full.dis> calls <hexaddr>                call sites of a function,
        with the 'mov edi/esi/edx/ecx, imm' set up in the 8 instructions before
Function starts come from the ps4_cg pickle in work/ps4-102 (run ps4_cg.py build first).
"""
import bisect, os, pickle, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RX = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')


def load(path):
    out = []
    for line in open(path, encoding='latin1'):
        m = RX.match(line)
        if m:
            rest = re.sub(r'\s*<[^>]*>', '', m.group(3)).strip()
            out.append((int(m.group(1), 16), m.group(2), rest))
    return out


def starts(dis=None):
    cp = os.path.join(REPO, 'work', 'ps4-102', 'ps4cg-v2.pickle')
    an = pickle.load(open(cp, 'rb'))
    s = set(t for _, t in an['calls'] if t < 0xdf0000)
    s.update(t for t in an['rel'].values() if 0 < t < 0xdf0000)
    s.update(t for _, t, k in an['rips'] if 0 < t < 0xdf0000 and k == 0)
    if dis:  # 16-byte aligned 'push rbp; mov rbp, rsp' prologues
        for i in range(len(dis) - 1):
            if dis[i][0] % 16 == 0 and dis[i][1] == 'push' and dis[i][2] == 'rbp' and                     dis[i + 1][1] == 'mov' and dis[i + 1][2] == 'rbp, rsp':
                s.add(dis[i][0])
    return sorted(s)


def func_of(st, a):
    i = bisect.bisect_right(st, a) - 1
    return st[i] if i >= 0 else 0


def rip_target(rest):
    m = re.search(r'# 0x([0-9a-f]+)', rest)
    return int(m.group(1), 16) if m else None


def main(argv):
    dis = load(argv[0])
    st = starts(dis)
    cmd = argv[1]
    if cmd in ('refs', 'writes'):
        for spec in argv[2:]:
            a, n = (spec.split(':') + ['1'])[:2]
            a, n = int(a, 16), int(n, 16)
            print('== %x (+%x)' % (a, n))
            hits = []
            for i, (pc, mn, rest) in enumerate(dis):
                t = rip_target(rest)
                if t is None or not (a <= t < a + n):
                    continue
                if mn == 'lea' and cmd == 'refs':
                    reg = rest.split(',')[0].strip()
                    uses = []
                    for pc2, mn2, r2 in dis[i + 1:i + 41]:
                        if re.search(r'\[%s \+ 0x[0-9a-f]+\]' % reg, r2) or re.search(r'\[%s\]' % reg, r2):
                            uses.append('%x %s %s' % (pc2, mn2, r2.split('#')[0].strip()))
                        if re.match(r'%s\b' % reg, r2) and mn2 in ('mov', 'lea', 'xor', 'pop') and not r2.startswith(reg + ' +'):
                            break
                    hits.append((pc, 'lea', '%s; uses: %s' % (rest.split('#')[0].strip(), ' | '.join(uses[:6]))))
                    continue
                is_store = rest.startswith(('dword ptr [rip', 'qword ptr [rip', 'byte ptr [rip', 'word ptr [rip')) and \
                    mn in ('mov', 'inc', 'dec', 'add', 'sub', 'or', 'and')
                if cmd == 'writes' and not is_store:
                    continue
                hits.append((pc, mn, rest.split('#')[0].strip() + '  ; [%x]' % t))
            for pc, mn, txt in hits:
                print('%x  fn %x  %s %s' % (pc, func_of(st, pc), mn, txt))
            print('count %d in %d functions' % (len(hits), len(set(func_of(st, h[0]) for h in hits))))
    elif cmd == 'calls':
        a = int(argv[2], 16)
        tgt = '%x' % a
        for i, (pc, mn, rest) in enumerate(dis):
            if mn in ('call', 'jmp') and rest == '0x' + tgt:
                args = []
                for pc2, mn2, r2 in dis[max(0, i - 8):i]:
                    if mn2 in ('mov', 'xor', 'lea') and re.match(r'(e?[sd]i|r[sd]i|e?[cd]x|r[cd]x|r8d?|r9d?)\b', r2):
                        args.append(r2.split('#')[0].strip() + (' ; [%x]' % rip_target(r2) if rip_target(r2) else ''))
                print('%x  fn %x  %s  %s' % (pc, func_of(st, pc), mn, ' | '.join(args)))


if __name__ == '__main__':
    main(sys.argv[1:])
