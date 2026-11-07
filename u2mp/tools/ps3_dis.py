"""ps3_dis.py - disassemble address ranges of a PS3 PPU ELF with clang + llvm-objdump
(read-only; the extracted bytes go to a temporary directory and are deleted).

Usage:
  python ps3_dis.py <EBOOT.elf> <start-end|func> [...] [--annotate CACHE_DIR]
  start-end : hex vaddrs, e.g. 447a5c-447b40
  func      : a single hex vaddr; the range is that function (needs --annotate)
With --annotate the output gets comments: bl targets with import names, computed
addresses that hit strings/OPD entries, and 32-bit constants.

Needs clang/llvm-objdump (MSYS2 clang64: C:/msys64/clang64/bin) or set PS3_LLVM_BIN.
"""
import os, re, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps3_elf as P

BIN = os.environ.get('PS3_LLVM_BIN', r'C:/msys64/clang64/bin')


def disasm(elf, a, b):
    tmp = tempfile.mkdtemp(prefix='ps3dis')
    try:
        open(os.path.join(tmp, 'p.bin'), 'wb').write(elf.bytes(a, b - a))
        open(os.path.join(tmp, 'p.s'), 'w').write('.text\n.incbin "p.bin"\n')
        subprocess.run([os.path.join(BIN, 'clang'), '--target=powerpc64-linux', '-c', 'p.s', '-o', 'p.o'],
                       check=True, cwd=tmp)
        o = subprocess.run([os.path.join(BIN, 'llvm-objdump'), '-d', '--no-show-raw-insn',
                            '--adjust-vma=%#x' % a, 'p.o'], capture_output=True, text=True, cwd=tmp).stdout
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    out = []
    for l in o.split('\n'):
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
        if m and '\t' in l:
            out.append((int(m.group(1), 16), m.group(2).strip()))
    return out


def stub_names(an):
    names = {}
    for lib in getattr(an, 'libs', []):
        for f in lib['funcs']:
            if f['stub']:
                names[f['stub']] = '%s:%s' % (lib['lib'], f['name'])
    return names


def annotate(an, lines, stubs):
    e = an.elf
    out = []
    for a, txt in lines:
        notes = []
        m = re.search(r'\bbl\s+0x([0-9a-f]+)', txt)
        if m:
            t = int(m.group(1), 16)
            if t in stubs:
                notes.append(stubs[t])
        for v, k in an.refs_at.get(a, []):
            s = e.cstr(v, 120)
            if k == 'a' and v in an.opd:
                notes.append('OPD %x -> %x' % (v, an.opd[v]))
            elif k != 'm' and s and len(s) >= 3:
                notes.append('"%s"' % s.replace('\n', '\\n')[:70])
            elif k == 'k':
                notes.append('const %08x' % v)
            elif k == 'm':
                notes.append('[%x]' % v)
            elif k == 'a' and 'lwz' in txt:
                notes.append('=%x' % v)
        out.append('%08x  %-40s %s' % (a, txt, ('; ' + ' | '.join(notes)) if notes else ''))
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    elf_path = argv[1]
    cache = None
    if '--annotate' in argv:
        i = argv.index('--annotate')
        cache = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    an = None
    if cache:
        an = P.load(elf_path, cache)
        an.refs_at = {}
        for s, v, k in an.refs:
            an.refs_at.setdefault(s, []).append((v, k))
        import ps3_map
        an.libs = ps3_map.imports(an)
        stubs = stub_names(an)
        e = an.elf
    else:
        e = P.Elf(elf_path)
    for spec in argv[2:]:
        if '-' in spec:
            a, b = [int(x, 16) for x in spec.split('-')]
        else:
            a = int(spec, 16)
            a = an.func_of(a)
            b = an.func_end(a)
        lines = disasm(e, a, b)
        print('== %x-%x' % (a, b))
        if an:
            print('\n'.join(annotate(an, lines, stubs)))
        else:
            print('\n'.join('%08x  %s' % x for x in lines))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
