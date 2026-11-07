"""ps4_xref.py - read-only cross-reference helper for the prepared PS4 flat image of
Uncharted 2 (boot-linked.bin, image address = file offset - 0x183ce8 by default).

Usage:
  python ps4_xref.py <boot-linked.bin> str   <regex>      strings + RIP-relative code refs
  python ps4_xref.py <boot-linked.bin> imm   <hex32> ...  mov r32, imm32 / cmp / push sites of a 32-bit id
  python ps4_xref.py <boot-linked.bin> dis   <hexaddr> [n]   disassemble n bytes (default 0x100)
                                                             from the function start containing addr
  --bias HEX  image address = file offset - BIAS (default 183ce8)

Function starts are a heuristic: the nearest 16-byte aligned address after int3/nop
padding (clang). Prints addresses, names and short strings only.
"""
import os, re, shutil, struct, subprocess, sys, tempfile

BIN = os.environ.get('PS3_LLVM_BIN', r'C:/msys64/clang64/bin')


class Img:
    def __init__(self, path, bias):
        self.d = open(path, 'rb').read()
        self.bias = bias
        self._lea = None

    def a2o(self, a):
        return a + self.bias

    def o2a(self, o):
        return o - self.bias

    def lea_index(self):
        """map target offset -> [site offsets] for REX.W lea/mov reg, [rip+rel32]"""
        if self._lea is None:
            idx = {}
            d = self.d
            for m in re.finditer(rb'[\x48\x4c][\x8d\x8b][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', d):
                o = m.start()
                rel, = struct.unpack_from('<i', d, o + 3)
                t = o + 7 + rel
                if 0 <= t < len(d):
                    idx.setdefault(t, []).append(o)
            self._lea = idx
        return self._lea

    def func_start(self, o):
        d = self.d
        p = o & ~0xf
        while p > 0:
            if p % 16 == 0 and d[p - 1] in (0xcc, 0x90, 0xc3) and d[p] not in (0xcc,):
                # require padding or a ret just before
                if d[p - 1] in (0xcc, 0x90) or d[p - 1] == 0xc3:
                    return p
            p -= 16 if p % 16 == 0 else p % 16
        return 0

    def dis(self, o, n):
        tmp = tempfile.mkdtemp(prefix='ps4dis')
        try:
            open(os.path.join(tmp, 'p.bin'), 'wb').write(self.d[o:o + n])
            open(os.path.join(tmp, 'p.s'), 'w').write('.text\n.incbin "p.bin"\n')
            subprocess.run([os.path.join(BIN, 'clang'), '--target=x86_64-linux', '-c', 'p.s', '-o', 'p.o'],
                           check=True, cwd=tmp)
            out = subprocess.run([os.path.join(BIN, 'llvm-objdump'), '-d', '--no-show-raw-insn',
                                  '--x86-asm-syntax=intel', '--adjust-vma=%#x' % self.o2a(o), 'p.o'],
                                 capture_output=True, text=True, cwd=tmp).stdout
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        return [l for l in out.split('\n') if re.match(r'\s*[0-9a-f]+:', l)]


def main(argv):
    argv = list(argv)
    bias = 0x183ce8
    if '--bias' in argv:
        i = argv.index('--bias')
        bias = int(argv[i + 1], 16)
        del argv[i:i + 2]
    if len(argv) < 4:
        print(__doc__)
        return 1
    im = Img(argv[1], bias)
    cmd = argv[2]
    if cmd == 'str':
        r = re.compile(argv[3].encode())
        lea = im.lea_index()
        seen = set()
        for m in r.finditer(im.d):
            s = m.start()
            while s > 0 and 0x20 <= im.d[s - 1] < 0x7f:
                s -= 1
            if s in seen:
                continue
            seen.add(s)
            e = im.d.find(b'\0', s)
            sites = lea.get(s, [])
            print('%08x %-60s %s' % (im.o2a(s), im.d[s:e][:60].decode('latin1'),
                  ' '.join('%x@%x' % (im.o2a(im.func_start(x)), im.o2a(x)) for x in sites[:8])))
    elif cmd == 'imm':
        for h in argv[3:]:
            v = struct.pack('<I', int(h, 16))
            o = im.d.find(v)
            out = []
            while o >= 0:
                op = im.d[o - 1]
                if 0xb8 <= op <= 0xbf or op in (0x3d, 0x68) or (im.d[o - 2] in (0x81, 0xc7)):
                    out.append('%x@%x' % (im.o2a(im.func_start(o)), im.o2a(o - 1)))
                o = im.d.find(v, o + 1)
            print(h, ' '.join(out[:20]))
    elif cmd == 'dis':
        a = int(argv[3], 16)
        n = int(argv[4], 16) if len(argv) > 4 else 0x100
        f = im.func_start(im.a2o(a))
        print('== function start (heuristic) %x' % im.o2a(f))
        print('\n'.join(im.dis(f, n)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
