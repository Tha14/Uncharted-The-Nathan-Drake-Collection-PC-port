"""Compile and link only the new freestanding helper, without console binaries.

v4.93: split link. Sections are placed in up to three regions:
  A  .text.entry first, then every other read-only section      -> flat blob at rx+0x1000 (limit 0xe000)
  B  sections named .text.ext* / .rodata.ext*                    -> second code segment rx2 (EXT_CODE_DELTA
     bytes after the start of A, limit EXT_CODE_SIZE)
  C  writable sections (.bss*, .data*, .u3ext*)                  -> ext data segment rw2 (EXT_DATA_DELTA after A)
PC-relative relocations between regions are resolved with those fixed deltas (they must match the layout in
build_u3_profile_probe.py, which checks them). With nothing in B or C the output is the legacy flat blob
(the pool helper keeps its format); otherwise it is a 'U3HX' container:
  struct {char magic[4]="U3HX"; u32 version=1, a_size, b_delta, b_size, c_delta, c_size, c_init;}  (32 bytes)
  then A (a_size bytes), B (b_size bytes), C initial contents (c_init bytes; the rest of C is zero).
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

EXT_CODE_DELTA=0x13000   # rx2 - helper = (rw+0x4000) - (rx+0x1000), rw = rx+0x10000
EXT_CODE_SIZE=0xb0000   # v7.55 (part C): BearSSL in region B, was 0x90000 (ext code 0x7a770 at v7.54); rw2 = image 0x2118000 now. pl01: pl01: was 0x80000 (ext code 0x77e70 of 0x78000 at v7.52); rw2 = image 0x20f8000 now. v5.80 (main, after c011: ext code 0x4bfd4 of 0x4c000): rw2 = image 0x20e8000. v5.67: was 0x30000 (ext code 0x268aa of 0x2c000 after b003); rw2 = image 0x20b8000 now
EXT_DATA_DELTA=EXT_CODE_DELTA+EXT_CODE_SIZE   # rw2 = rx2 + EXT_CODE_SIZE
EXT_DATA_SIZE=0x60000   # v5.74: was 0x40000 (ext data 0x37bc0 after c009); u3_lan_profiles.c CIN_LIST_OFF must equal this
HELPER_LIMIT=0xe000

def link_object(raw,code_delta=EXT_CODE_DELTA,data_delta=EXT_DATA_DELTA):
    if raw[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64 object')
    shoff=struct.unpack_from('<Q',raw,40)[0]
    shsize,count,namesindex=struct.unpack_from('<HHH',raw,58)
    if shsize!=64: raise ValueError('unsupported section headers')
    sections=[struct.unpack_from('<IIQQQQIIQQ',raw,shoff+i*64) for i in range(count)]
    def data(i):
        s=sections[i];return raw[s[4]:s[4]+s[5]]
    names=data(namesindex)
    def name(i):
        n=sections[i][0];return names[n:names.index(0,n)].decode()
    allocated=[i for i,s in enumerate(sections) if s[2]&2 and s[5]]
    allocated.sort(key=lambda i:(name(i)!='.text.entry',name(i)!='.bss.u3ext',i))   # v7.56: X (first in .bss.u3ext) at rw2+0, where the probe builder writes ExtState fields
    if not allocated or name(allocated[0])!='.text.entry':raise ValueError('missing entry section')
    def region(i):
        s=sections[i];n=name(i)
        if s[2]&1: return 2                                   # SHF_WRITE -> C
        if s[1]!=1: raise ValueError('unexpected NOBITS read-only section '+n)
        return 1 if n.startswith('.text.ext') or n.startswith('.rodata.ext') else 0
    blobs=[bytearray(),bytearray(),bytearray()];base=[0,code_delta,data_delta];place={}
    cinit=0
    for i in allocated:
        s=sections[i];r=region(i);b=blobs[r]
        align=max(1,s[8]);b.extend(b'\0'*((-len(b))%align))
        place[i]=(r,len(b))
        if s[1]==8: b.extend(b'\0'*s[5])                       # NOBITS: zero fill
        else:
            b.extend(data(i))
            if r==2: cinit=len(b)
    off={i:base[r]+o for i,(r,o) in place.items()}
    records=[];fixups=[]
    fix_sec=[i for i in allocated if name(i)=='.data.u3fix']
    for i,s in enumerate(sections):
        if s[1]!=4:continue
        if s[7] not in place:
            if not sections[s[7]][2]&2:continue               # relocations of a non-allocated section
            raise ValueError('relocation to unplaced section')
        symbols=data(s[6]);strings=data(sections[s[6]][6])
        for p in range(0,s[5],24):
            o,info,addend=struct.unpack_from('<QQq',data(i),p)
            index,kind=info>>32,info&0xffffffff
            symname,_,_,section,value,_=struct.unpack_from('<IBBHQQ',symbols,index*24)
            if section not in off or kind not in (1,2,4):raise ValueError('unsupported or external relocation: kind %d, symbol %s, in %s'%(kind,strings[symname:strings.index(0,symname)].decode(),name(s[7])))
            target=off[section]+value;at=off[s[7]]+o
            r,local=place[s[7]];local+=o
            if kind==1:   # R_X86_64_64: helper-relative now, run-time address after u3_fix_apply() (plat.h); writable data only
                if r!=2:raise ValueError('absolute relocation in read-only section %s -> %s'%(name(s[7]),strings[symname:strings.index(0,symname)].decode()))
                struct.pack_into('<Q',blobs[r],local,(target+addend)&0xffffffffffffffff);fixups.append(at)
            else:struct.pack_into('<i',blobs[r],local,target+addend-at)
            records.append(dict(offset=at,target=target,kind=kind,name=strings[symname:strings.index(0,symname)].decode()))
    if fixups:
        if not fix_sec:raise ValueError('%d absolute relocations but no .data.u3fix table (plat.h u3_fix)'%len(fixups))
        fr,fo=place[fix_sec[0]];cap=sections[fix_sec[0]][5]//8-1
        if len(fixups)>cap:raise ValueError('%d absolute relocations, u3_fix holds %d'%(len(fixups),cap))
        struct.pack_into('<Q',blobs[fr],fo,len(fixups))
        for k,at in enumerate(sorted(fixups)):struct.pack_into('<Q',blobs[fr],fo+8+k*8,at)
    elif fix_sec:
        fr,fo=place[fix_sec[0]];struct.pack_into('<Q',blobs[fr],fo,0)
    a,b,c=(bytes(x) for x in blobs)
    regions=dict(a_size=len(a),b_size=len(b),c_size=len(c),b_delta=code_delta,c_delta=data_delta,abs_fixups=len(fixups))
    if len(a)>HELPER_LIMIT:raise ValueError('helper region A is 0x%x bytes, limit 0x%x'%(len(a),HELPER_LIMIT))
    if len(b)>EXT_CODE_SIZE:raise ValueError('helper region B too large')
    if len(c)>EXT_DATA_SIZE:raise ValueError('helper region C too large')
    if not b and not c:return a,records,regions
    header=b'U3HX'+struct.pack('<7I',1,len(a),code_delta,len(b),data_delta,len(c),cinit)
    return header+a+b+c[:cinit],records,regions

def split_blob(blob):
    """(A, B, B delta, C size, C initial bytes, C delta) of a U3HX container or a legacy flat blob."""
    if blob[:4]!=b'U3HX':return blob,b'',EXT_CODE_DELTA,0,b'',EXT_DATA_DELTA
    version,na,bd,nb,cd,nc,ci=struct.unpack_from('<7I',blob,4)
    if version!=1 or ci>nc or len(blob)!=32+na+nb+ci:raise ValueError('bad U3HX helper container')
    a=blob[32:32+na];b=blob[32+na:32+na+nb];c=blob[32+na+nb:]
    return a,b,bd,nc,c,cd

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--clang',type=Path,default=Path('C:/msys64/clang64/bin/clang.exe'))
    p.add_argument('--source',type=Path,help='Alternative freestanding helper with .text.entry')
    a=p.parse_args();source=a.source or Path(__file__).with_name('u3_pool_init.c')
    a.out.parent.mkdir(parents=True,exist_ok=True);obj=a.out.with_suffix('.o')
    command=[str(a.clang),'--target=x86_64-unknown-elf','-std=c11','-O2','-ffreestanding',
             '-fno-builtin','-fPIC','-mno-red-zone','-mno-sse','-fno-stack-protector',
             '-fno-asynchronous-unwind-tables','-fno-jump-tables','-c',str(source),'-o',str(obj)]
    subprocess.run(command,check=True)
    blob,relocations,regions=link_object(obj.read_bytes());a.out.write_bytes(blob)
    manifest=dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  blob_sha256=hashlib.sha256(blob).hexdigest(),bytes=len(blob),regions=regions,
                  entry_offset=0,relocations=relocations,command=command)
    a.out.with_suffix('.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps({k:v for k,v in manifest.items() if k!='relocations'},indent=2))
if __name__=='__main__':main()
