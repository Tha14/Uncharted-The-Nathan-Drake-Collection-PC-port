"""disassemble VMA ranges of the PS4 ELF (u3_ps4.elf, vaddr 0 = file +0x10000), read-only: dis_elf.py START-END ..."""
import struct,sys,subprocess,os,re
E="D:/1. PC Games/Uncharted-windows/_claude_transfer/u3_ps4.elf"
d=open(E,'rb').read()
ph,sz,n=struct.unpack_from('<Q',d,32)[0],struct.unpack_from('<H',d,54)[0],struct.unpack_from('<H',d,56)[0]
segs=[struct.unpack_from('<IIQQQQQQ',d,ph+i*sz) for i in range(n)]
for spec in sys.argv[1:]:
    a,b=[int(x,16) for x in spec.split('-')]
    seg=[s for s in segs if s[0]==1 and s[3]<=a<s[3]+s[5]]
    if not seg: print('no segment for',spec); continue
    t,f,o,v,p,fs,ms,al=seg[0]
    open('_d.bin','wb').write(d[o+a-v:o+b-v]); open('_d.s','w').write('.text\n.incbin "_d.bin"\n')
    subprocess.run([r'C:\msys64\clang64\bin\clang.exe','--target=x86_64-pc-linux','-c','_d.s','-o','_d.o'],check=True)
    r=subprocess.run([r'C:\msys64\clang64\bin\llvm-objdump.exe','-d','--no-show-raw-insn','--x86-asm-syntax=intel','--adjust-vma=%#x'%a,'_d.o'],capture_output=True,text=True)
    print('==',spec); print('\n'.join(l.strip() for l in r.stdout.split('\n') if re.match(r'\s*[0-9a-f]+:',l)))
for x in ('_d.bin','_d.s','_d.o'):
    if os.path.exists(x): os.remove(x)
