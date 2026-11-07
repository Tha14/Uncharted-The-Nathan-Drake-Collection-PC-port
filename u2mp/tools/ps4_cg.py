"""ps4_cg.py - call graph and reference index of the PS4 Uncharted 2 (Collection 1.02)
executable, for deciding whether a PS4 function is alive, dead or a stub (read-only).

Usage:
  python ps4_cg.py <boot-linked.bin> <eboot.elf> build              (once, ~3-6 min)
  python ps4_cg.py <boot-linked.bin> <eboot.elf> info  <hexaddr>... function start, size,
                                                     state, callers, refs, callees
  python ps4_cg.py <boot-linked.bin> <eboot.elf> callers <hexaddr>
  python ps4_cg.py <boot-linked.bin> <eboot.elf> callees <hexaddr>
  python ps4_cg.py <boot-linked.bin> <eboot.elf> refs  <hexaddr>    code sites (call/jmp/lea/mov)
                                                     and data slots (relocations) pointing at it
  python ps4_cg.py <boot-linked.bin> <eboot.elf> strs  <hexaddr>    strings the function references
  python ps4_cg.py <boot-linked.bin> <eboot.elf> imm32 <hex32>       functions using a 32-bit immediate
  --bias HEX   image address = boot-linked.bin offset - BIAS (default 183ce8)
  --cache DIR  default <repo>/work/ps4-102 (git-ignored)

How it works:
  * the whole text (0 .. PLT end) is disassembled once with MSYS2 clang64 llvm-objdump
    (raw bytes wrapped with .incbin into an object; the temp files are deleted). Each
    instruction line is reduced to: call/jmp/jcc targets, RIP-relative targets (lea/mov
    and memory operands), and 32-bit immediates. Nothing but addresses is cached.
  * the flat image has its relocations unapplied (pointer slots are zero), so data
    pointers come from the R_X86_64_RELATIVE entries of the original ELF (DT_SCE_RELA):
    slot -> target. A function whose address sits in a slot (vtable, callback table,
    script-native table) counts as referenced.
  * function starts = call targets + relocation targets in text + lea targets in text +
    16-byte aligned 'push rbp; mov rbp,rsp' prologues (the build keeps frame pointers).
  * state:  alive  = has a direct call/jmp, a lea/mov of its address, or a data slot,
                     transitively reachable is NOT computed (cheap reachability is
                     "referenced"); dead = no reference at all;
            stub   = body is a bare ret, or xor eax,eax/mov eax,imm then ret, or a
                     single jmp to another function.
Prints addresses and short strings only; no code bytes are written to the cache.
"""
import bisect, os, pickle, re, shutil, struct, subprocess, sys, tempfile

BIN = os.environ.get('PS3_LLVM_BIN', r'C:/msys64/clang64/bin')
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEF_CACHE = os.path.join(REPO, 'work', 'ps4-102')
TEXT_END = 0xdf0000
VERSION = 2
PROLOGUE = bytes.fromhex('554889e5')


def relocs(elf_path):
    d = open(elf_path, 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phnum, = struct.unpack_from('<H', d, 0x38)
    ph = [struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56) for i in range(phnum)]
    dyn = [p for p in ph if p[0] == 2][0]
    dl = [p for p in ph if p[0] == 0x61000000][0]
    tags = {}
    for i in range(dyn[2], dyn[2] + dyn[5], 16):
        t, v = struct.unpack_from('<qQ', d, i)
        tags.setdefault(t & 0xffffffffffffffff, v)
    ro, rs = tags[0x6100002f], tags[0x61000031]
    out = {}
    for i in range(rs // 24):
        off, info, add = struct.unpack_from('<QQq', d, dl[2] + ro + i * 24)
        if info & 0xffffffff == 8:
            out[off] = add
    return out


RX_ADDR = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
RX_HEX = re.compile(r'0x([0-9a-f]+)')


def build(img_path, elf_path, bias, cache):
    d = open(img_path, 'rb').read()
    tmp = tempfile.mkdtemp(prefix='ps4cg')
    calls, jmps, rips, imms, rets, insns = [], [], [], [], [], []
    try:
        open(os.path.join(tmp, 'p.bin'), 'wb').write(d[bias:bias + TEXT_END])
        open(os.path.join(tmp, 'p.s'), 'w').write('.text\n.incbin "p.bin"\n')
        subprocess.run([os.path.join(BIN, 'clang'), '--target=x86_64-linux', '-c', 'p.s', '-o', 'p.o'],
                       check=True, cwd=tmp)
        pr = subprocess.Popen([os.path.join(BIN, 'llvm-objdump'), '-d', '--no-show-raw-insn',
                               '--x86-asm-syntax=intel', 'p.o'], stdout=subprocess.PIPE, cwd=tmp,
                              text=True, bufsize=1 << 20)
        for line in pr.stdout:
            m = RX_ADDR.match(line)
            if not m:
                continue
            a = int(m.group(1), 16)
            mn = m.group(2)
            rest = m.group(3)
            insns.append(a)
            if mn == 'call':
                h = RX_HEX.match(rest)
                if h:
                    calls.append((a, int(h.group(1), 16)))
                elif '# 0x' in rest:
                    rips.append((a, int(rest.split('# 0x')[1].split()[0], 16), 1))
            elif mn == 'jmp' or (mn[0] == 'j' and mn != 'jmp'):
                h = RX_HEX.match(rest)
                if h:
                    jmps.append((a, int(h.group(1), 16), mn == 'jmp'))
                elif '# 0x' in rest:
                    rips.append((a, int(rest.split('# 0x')[1].split()[0], 16), 1))
                if mn == 'jmp':
                    rets.append(a)
            elif mn in ('ret', 'ud2', 'int3'):
                rets.append(a)
            else:
                if '# 0x' in rest:
                    rips.append((a, int(rest.split('# 0x')[1].split()[0], 16), 0))
                for h in RX_HEX.finditer(rest.split('#')[0]):
                    v = int(h.group(1), 16)
                    if 0x10000 <= v <= 0xffffffff and '[' not in rest.split('#')[0][max(0, h.start() - 2):h.start()]:
                        imms.append((a, v))
        pr.wait()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    rel = relocs(elf_path)
    an = {'calls': calls, 'jmps': jmps, 'rips': rips, 'imms': imms, 'rets': rets,
          'insns': insns, 'rel': rel, 'version': VERSION}
    os.makedirs(cache, exist_ok=True)
    pickle.dump(an, open(os.path.join(cache, 'ps4cg-v%d.pickle' % VERSION), 'wb'))
    return an


class CG:
    def __init__(self, img_path, elf_path, bias=0x183ce8, cache=DEF_CACHE):
        self.d = open(img_path, 'rb').read()
        self.bias = bias
        cp = os.path.join(cache, 'ps4cg-v%d.pickle' % VERSION)
        an = pickle.load(open(cp, 'rb')) if os.path.exists(cp) else build(img_path, elf_path, bias, cache)
        self.__dict__.update(an)
        self.insn_set = self.insns
        starts = set(t for s, t in self.calls if t < TEXT_END)
        starts.update(t for t in self.rel.values() if 0 < t < TEXT_END)
        starts.update(t for s, t, k in self.rips if 0 < t < TEXT_END and k == 0)
        # frame-pointer prologue (push rbp; mov rbp,rsp) on a 16-byte boundary
        iset = set()
        d, bias = self.d, self.bias
        for a in self.insns:
            if a % 16 == 0 and d[bias + a:bias + a + 4] == PROLOGUE:
                iset.add(a)
        starts.update(iset)
        self.starts = sorted(starts)
        self.callers = {}
        for s, t in self.calls:
            self.callers.setdefault(t, []).append(('call', s))
        for s, t, u in self.jmps:
            if u:
                self.callers.setdefault(t, []).append(('jmp', s))
        for s, t, k in self.rips:
            self.callers.setdefault(t, []).append(('rip', s))
        self.slots = {}
        for s, t in self.rel.items():
            self.slots.setdefault(t, []).append(s)
        self.byimm = {}
        for s, v in self.imms:
            self.byimm.setdefault(v, []).append(s)

    def a2o(self, a):
        return a + self.bias

    def func_of(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else None

    def func_end(self, f):
        i = bisect.bisect_right(self.starts, f)
        return self.starts[i] if i < len(self.starts) else TEXT_END

    def refs_to(self, f):
        code = [(k, s) for k, s in self.callers.get(f, []) if self.func_of(s) != f or k == 'rip']
        return code, self.slots.get(f, [])

    def callees(self, f):
        e = self.func_end(f)
        i = bisect.bisect_left(self.calls, (f, 0))
        out = []
        while i < len(self.calls) and self.calls[i][0] < e:
            out.append(self.calls[i][1])
            i += 1
        i = bisect.bisect_left(self.jmps, (f, 0, False))
        while i < len(self.jmps) and self.jmps[i][0] < e:
            s, t, u = self.jmps[i]
            if u and not (f <= t < e):
                out.append(t)
            i += 1
        return out

    def body_kind(self, f):
        b = self.d[self.a2o(f):self.a2o(f) + 16]
        if b[0] == 0xc3:
            return 'stub(ret)'
        if b[:3] == b'\x31\xc0\xc3' or b[:3] == b'\x33\xc0\xc3':
            return 'stub(ret0)'
        if b[0] == 0xb0 and b[2] == 0xc3:
            return 'stub(ret_al=%d)' % b[1]
        if b[0] == 0xb8 and b[5] == 0xc3:
            return 'stub(ret_eax=%d)' % struct.unpack_from('<I', b, 1)[0]
        if b[:4] == b'\x55\x48\x89\xe5' and b[4] == 0x5d and b[5] == 0xc3:
            return 'stub(ret)'
        if b[:4] == b'\x55\x48\x89\xe5' and b[4:7] == b'\x31\xc0\x5d' and b[7] == 0xc3:
            return 'stub(ret0)'
        if b[:4] == b'\x55\x48\x89\xe5' and b[4] == 0xb0 and b[6] == 0x5d and b[7] == 0xc3:
            return 'stub(ret_al=%d)' % b[5]
        if b[:4] == b'\x55\x48\x89\xe5' and b[4] == 0xb8 and b[9] == 0x5d and b[10] == 0xc3:
            return 'stub(ret_eax=%d)' % struct.unpack_from('<I', b, 5)[0]
        if b[0] == 0xe9:
            return 'thunk'
        return 'code'

    def state(self, f):
        code, slots = self.refs_to(f)
        k = self.body_kind(f)
        if k.startswith('stub'):
            return k
        return 'alive' if (code or slots) else 'dead'

    def strings(self, f, n=6):
        e = self.func_end(f)
        out = []
        i = bisect.bisect_left(self.rips, (f, 0, 0))
        while i < len(self.rips) and self.rips[i][0] < e and len(out) < n:
            t = self.rips[i][2 - 1]
            o = self.a2o(t)
            if 0 <= o < len(self.d) and t >= TEXT_END:
                z = self.d.find(b'\0', o, o + 120)
                s = self.d[o:z] if z > o else b''
                if len(s) >= 4 and all(0x20 <= c < 0x7f for c in s):
                    out.append(s.decode('latin1'))
            i += 1
        return out


def main(argv):
    argv = list(argv)
    bias, cache = 0x183ce8, DEF_CACHE
    if '--bias' in argv:
        i = argv.index('--bias'); bias = int(argv[i + 1], 16); del argv[i:i + 2]
    if '--cache' in argv:
        i = argv.index('--cache'); cache = argv[i + 1]; del argv[i:i + 2]
    if len(argv) < 4:
        print(__doc__)
        return 1
    if argv[3] == 'build':
        build(argv[1], argv[2], bias, cache)
        return 0
    cg = CG(argv[1], argv[2], bias, cache)
    cmd, args = argv[3], argv[4:]
    if cmd == 'info':
        for h in args:
            f = cg.func_of(int(h, 16))
            code, slots = cg.refs_to(f)
            print('%x size %x %s callers %s slots %s callees %s strs %s' % (
                f, cg.func_end(f) - f, cg.state(f),
                ' '.join('%s:%x' % (k, cg.func_of(s)) for k, s in code[:8]),
                ' '.join('%x' % s for s in slots[:4]),
                ' '.join('%x' % t for t in cg.callees(f)[:12]), cg.strings(f, 3)))
    elif cmd == 'callers':
        f = cg.func_of(int(args[0], 16))
        for k, s in cg.callers.get(f, []):
            print('%s %x in %x' % (k, s, cg.func_of(s)))
        for s in cg.slots.get(f, []):
            print('slot %x' % s)
    elif cmd == 'callees':
        f = cg.func_of(int(args[0], 16))
        for t in cg.callees(f):
            print('%x %s' % (t, cg.body_kind(t)))
    elif cmd == 'refs':
        v = int(args[0], 16)
        for k, s in cg.callers.get(v, []):
            print('%s %x in %x' % (k, s, cg.func_of(s)))
        for s in cg.slots.get(v, []):
            print('slot %x' % s)
    elif cmd == 'strs':
        f = cg.func_of(int(args[0], 16))
        for s in cg.strings(f, 50):
            print(s)
    elif cmd == 'imm32':
        for h in args:
            print(h, ' '.join('%x@%x' % (cg.func_of(s), s) for s in cg.byimm.get(int(h, 16), [])[:30]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
