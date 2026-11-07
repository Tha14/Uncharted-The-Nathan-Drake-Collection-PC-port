"""ps4_imports.py - name the imports of the PS4 Uncharted 2 executable (Collection 1.02) and
find their call sites in the prepared flat image (boot-linked.bin). Read-only.

Usage:
  python ps4_imports.py <decrypted eboot.elf> <boot-linked.bin> list   [regex]
  python ps4_imports.py <decrypted eboot.elf> <boot-linked.bin> callers <regex>
  --bias HEX      image address = flat-file offset - BIAS (default 183ce8)
  --names FILE    NID table "{"NID#lib","name"}," (default gow3_pc/src/import_names.inc)

How: the PT_DYNAMIC tags DT_SCE_JMPREL/SYMTAB/STRTAB (offsets into the PT_SCE_DYNLIBDATA
segment) give each JUMP_SLOT relocation: GOT slot address and symbol "NID#lib#mod".
The library id is mapped to its name through DT_SCE_IMPORT_LIB. In the flat image every
`jmp qword [rip+rel32]` (ff 25) whose target is such a GOT slot is a PLT stub; every
`call rel32` (e8) to a stub, and every `call/jmp qword [rip+rel32]` (ff 15 / ff 25 outside
the PLT) to a GOT slot, is a call site. The containing function is the heuristic start
from ps4_xref.py (16-byte aligned after int3/nop padding).

Prints addresses and names only. Use: `callers "sceNpMatching2|sceNetSend|sceVoice"`.
"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEF_NAMES = os.path.join(REPO, 'gow3_pc', 'src', 'import_names.inc')

B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'


def b64id(s):
    v = 0
    for c in s:
        v = v * 64 + B64.index(c)
    return v


def load_names(path):
    names = {}
    if os.path.exists(path):
        for m in re.finditer(r'\{"([^"#]+)#([^"]+)","([^"]+)"\}', open(path, encoding='utf-8', errors='replace').read()):
            names[(m.group(1), m.group(2))] = m.group(3)
            names.setdefault((m.group(1), None), m.group(3))
    return names


def elf_imports(path, names):
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phnum, = struct.unpack_from('<H', d, 0x38)
    dyn = dlib = None
    for i in range(phnum):
        t, f, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', d, phoff + 56 * i)
        if t == 2:
            dyn = (off, fs)
        elif t == 0x61000000:
            dlib = off
    tags = {}
    for i in range(dyn[1] // 16):
        t, v = struct.unpack_from('<QQ', d, dyn[0] + 16 * i)
        tags.setdefault(t, []).append(v)
    stro = dlib + tags[0x61000035][0]
    symo = dlib + tags[0x61000039][0]
    jro, jrs = dlib + tags[0x61000029][0], tags[0x6100002d][0]

    def cs(o):
        e = d.index(b'\0', stro + o)
        return d[stro + o:e].decode('ascii', 'replace')
    libs = {}
    for v in tags.get(0x61000015, []):
        libs[(v >> 48) & 0xffff] = cs(v & 0xffffffff)
    out = []
    for i in range(jrs // 24):
        off, info, add = struct.unpack_from('<QQq', d, jro + 24 * i)
        si = info >> 32
        nm = cs(struct.unpack_from('<I', d, symo + 24 * si)[0])
        parts = nm.split('#')
        lib = libs.get(b64id(parts[1]), parts[1]) if len(parts) > 1 else '?'
        name = names.get((parts[0], lib)) or names.get((parts[0], None)) or parts[0]
        out.append((off, name, lib, parts[0]))
    return out


def scan(img, bias, imps):
    got = {o: n for o, n, l, nid in imps}
    d = img
    stubs = {}
    sites = {}
    for m in re.finditer(rb'\xff[\x15\x25]', d):
        o = m.start()
        rel, = struct.unpack_from('<i', d, o + 2)
        t = o + 6 + rel - bias
        if t in got:
            a = o - bias
            if d[o + 1] == 0x25 and d[o + 6:o + 7] in (b'\x68', b'\xcc', b'\x90', b'\xff'):
                stubs[a] = got[t]
            sites.setdefault(got[t], []).append(a)
    for m in re.finditer(rb'[\xe8\xe9]', d):
        o = m.start()
        rel, = struct.unpack_from('<i', d, o + 1)
        t = o + 5 + rel - bias
        if t in stubs:
            sites.setdefault(stubs[t], []).append(o - bias)
    for n in sites:
        sites[n] = sorted(set(a for a in sites[n] if a not in stubs))
    return stubs, sites


def func_start(d, bias, a):
    o = a + bias
    lim = max(0, o - 0x20000)
    while o > lim:
        o -= 1
        if o % 16 == 0 and d[o - 1] in (0xcc, 0x90, 0xc3) and d[o] != 0xcc:
            # previous byte is padding or a ret; accept when the run before is padding
            if d[o - 1] in (0xcc, 0x90) or d[o - 2:o] == b'\xc3\xcc':
                return o - bias
            if d[o - 1] == 0xc3 and d[o] in (0x55, 0x41, 0x48, 0x53, 0x56, 0x57, 0x50):
                return o - bias
    return None


def main(argv):
    argv = list(argv)
    bias = 0x183ce8
    names_path = DEF_NAMES
    if '--bias' in argv:
        i = argv.index('--bias'); bias = int(argv[i + 1], 16); del argv[i:i + 2]
    if '--names' in argv:
        i = argv.index('--names'); names_path = argv[i + 1]; del argv[i:i + 2]
    if len(argv) < 4:
        print(__doc__)
        return 1
    imps = elf_imports(argv[1], load_names(names_path))
    rx = re.compile(argv[4] if len(argv) > 4 else '.')
    if argv[3] == 'list':
        for o, n, l, nid in imps:
            if rx.search(n) or rx.search(l):
                print('%08x  %-45s %s' % (o, n, l))
        return 0
    img = open(argv[2], 'rb').read()
    stubs, sites = scan(img, bias, imps)
    for o, n, l, nid in imps:
        if not (rx.search(n) or rx.search(l)):
            continue
        st = [a for a, nn in stubs.items() if nn == n]
        fs = sorted(set(func_start(img, bias, a) or 0 for a in sites.get(n, [])))
        print('%-45s %-22s stub %s  sites %d  funcs %s' % (
            n, l, ' '.join('%x' % a for a in st) or '-', len(sites.get(n, [])),
            ' '.join('%x' % f for f in fs) or '-'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
