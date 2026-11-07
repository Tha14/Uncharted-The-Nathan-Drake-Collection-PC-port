"""ps4_dis.py - disassemble a range of the PS4 Uncharted 2 flat image (boot-linked.bin) with
MSYS2 objdump and print image addresses (file offset - BIAS) instead of file offsets
(read-only; prints instructions, writes nothing).

Usage:
  python ps4_dis.py <boot-linked.bin> <start-end|start+len> [...] [--bias HEX]
  python ps4_dis.py <boot-linked.bin> --vt <eboot.elf> <hexaddr> <count>
  addresses are image addresses in hex (e.g. 60c7f0-60c900 or 60c7f0+120)
  --vt prints the pointer slots of a table (vtable) from the ELF's RELATIVE relocations.
Call/jmp targets and RIP-relative operands are rewritten to image addresses as well.
"""
import re, subprocess, sys

OBJDUMP = r'C:/msys64/ucrt64/bin/objdump.exe'


def dis(binpath, a, b, bias):
    o = subprocess.run([OBJDUMP, '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel',
                        '--no-show-raw-insn', '--start-address=%#x' % (a + bias),
                        '--stop-address=%#x' % (b + bias), binpath],
                       capture_output=True, text=True).stdout
    out = []
    for l in o.split('\n'):
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
        if not m:
            continue
        addr = int(m.group(1), 16) - bias
        ins = m.group(2).strip()
        ins = re.sub(r'\b0x([0-9a-f]+) <\.data\+0x[0-9a-f]+>',
                     lambda mm: '%#x' % (int(mm.group(1), 16) - bias), ins)
        ins = re.sub(r'^((?:call|j[a-z]+)\s+)0x([0-9a-f]+)$',
                     lambda mm: '%s%#x' % (mm.group(1), int(mm.group(2), 16) - bias), ins)
        ins = re.sub(r'# 0x([0-9a-f]+)', lambda mm: '# %#x' % (int(mm.group(1), 16) - bias), ins)
        out.append('%08x  %s' % (addr, ins))
    return out


def main():
    args = sys.argv[1:]
    bias = 0x183ce8
    if '--bias' in args:
        i = args.index('--bias'); bias = int(args[i + 1], 16); del args[i:i + 2]
    binpath = args[0]
    if len(args) > 1 and args[1] == '--vt':
        sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
        import ps4_cg
        rel = ps4_cg.relocs(args[2])
        a, n = int(args[3], 16), int(args[4])
        for i in range(n):
            t = rel.get(a + 8 * i)
            print('%x slot %d -> %s' % (a + 8 * i, i, '%x' % t if t is not None else '-'))
        return
    for r in args[1:]:
        if '+' in r:
            s, n = r.split('+'); a = int(s, 16); b = a + int(n, 16)
        else:
            s, e = r.split('-'); a, b = int(s, 16), int(e, 16)
        print('== %x-%x' % (a, b))
        print('\n'.join(dis(binpath, a, b, bias)))


if __name__ == '__main__':
    main()
