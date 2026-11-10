"""Builds the Havok class tables u2havok needs, on the player's PC, from the player's own files (nothing derived from the
games ships with the import, as with the U3 Havok tables step):

  DB3  Havok 6.1.0 layouts: the hkClass constructor calls and static hkClassMember arrays of the decrypted PS3
       Uncharted 2 EBOOT.elf (--ps3-elf, default <ps3 USRDIR>/EBOOT.elf)
  DB4  Havok 2011.3 layouts: the same in the PS4 Collection's big2-ps4_Shipping.elf (fake-signed SELF in the --ps4
       game folder): unwrapped, relocated (SCE RELA, R_X86_64_RELATIVE), the text disassembled with GNU objdump
  FILL The 2011 classes whose member tables the PS4 executable builds at run time (FILL_DECL): member declarations
       (order and types as in the Havok headers, no offsets); offsets and sizes are computed with the C layout rules
       and then checked on the player's retail PS4 actor packfiles (u2havok.check_tables)
  SIG4 PS4 __classnames__ signatures, read from the player's retail PS4 actor packfiles

  python u2hkgen.py --ps3-elf <EBOOT.elf> --ps4 <PS4 game folder> --out <tables.json> [--objdump <objdump>]
"""
import collections
import json
import os
import re
import shutil
import struct
import subprocess
import sys

TY = ('VOID BOOL CHAR INT8 UINT8 INT16 UINT16 INT32 UINT32 INT64 UINT64 REAL VECTOR4 QUATERNION MATRIX3 ROTATION '
      'QSTRANSFORM MATRIX4 TRANSFORM ZERO POINTER FUNCTIONPOINTER ARRAY INPLACEARRAY ENUM STRUCT SIMPLEARRAY '
      'HOMOGENEOUSARRAY VARIANT CSTRING ULONG FLAGS HALF STRINGPTR RELARRAY').split()
VERSION = 1


class TablesMissing(NotImplementedError):
    pass


# ------------------------------------------------------------------ executables
class Elf:
    def __init__(self, data):
        self.d = d = data
        if d[:4] != b'\x7fELF':
            raise TablesMissing('not an ELF file')
        self.e = '>' if d[5] == 2 else '<'
        self.ps = 4 if self.e == '>' else 8
        self.seg = []
        phoff = struct.unpack_from(self.e + 'Q', d, 0x20)[0]
        es, n = struct.unpack_from(self.e + 'HH', d, 0x36)
        self.ph = [struct.unpack_from(self.e + 'IIQQQQQQ', d, phoff + i * es) for i in range(n)]
        for t, fl, of, va, pa, fs, ms, al in self.ph:
            if t in (1, 0x61000010, 0x61000000) and fs:
                self.seg.append((va, of, fs, ms))
        self.entry = struct.unpack_from(self.e + 'Q', d, 0x18)[0]

    def off(self, va):
        for v, o, f, m in self.seg:
            if v <= va < v + f:
                return o + va - v
        return None

    def u32(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from(self.e + 'I', self.d, o)[0]

    def cstr(self, va):
        o = self.off(va) if va else None
        if o is None:
            return None
        e = self.d.find(b'\0', o, o + 200)
        if e <= o:
            return None
        try:
            x = self.d[o:e].decode('ascii')
        except UnicodeDecodeError:
            return None
        return x if x.isprintable() else None


def unwrap_ps4(path):
    """fake-signed PS4 SELF -> ELF with the R_X86_64_RELATIVE relocations applied (load address 0)"""
    d = open(path, 'rb').read()
    if d[:4] == b'\x7fELF':
        out = bytearray(d)
    else:
        if d[:4] != b'\x4f\x15\x3d\x1d':
            raise TablesMissing('%s is neither an ELF nor a PS4 SELF' % path)
        nseg = struct.unpack_from('<H', d, 0x18)[0]
        segs = [struct.unpack_from('<QQQQ', d, 0x20 + 32 * i) for i in range(nseg)]
        eo = 0x20 + 32 * nseg
        if d[eo:eo + 4] != b'\x7fELF':
            raise TablesMissing('%s: no ELF header in the SELF' % path)
        phoff = struct.unpack_from('<Q', d, eo + 0x20)[0]
        phnum = struct.unpack_from('<H', d, eo + 0x38)[0]
        ph = [struct.unpack_from('<IIQQQQQQ', d, eo + phoff + 56 * i) for i in range(phnum)]
        out = bytearray(max(p[2] + p[5] for p in ph))
        out[0:0x40 + 56 * phnum] = d[eo:eo + 0x40 + 56 * phnum]
        for flags, off, csize, usize in segs:
            if flags & 0x800:
                if flags & 0xa:
                    raise TablesMissing('%s is encrypted or compressed (a decrypted / fake-signed executable is '
                                        'needed)' % path)
                p = ph[flags >> 20]
                out[p[2]:p[2] + csize] = d[off:off + csize]
    e = Elf(bytes(out))
    dyn = [p for p in e.ph if p[0] == 2]
    dld = [p for p in e.ph if p[0] == 0x61000000]
    if dyn and dld:
        tags = {}
        for o in range(dyn[0][2], dyn[0][2] + dyn[0][5], 16):
            t, v = struct.unpack_from('<QQ', out, o)
            tags.setdefault(t, v)
        if 0x6100002f in tags:
            ro = dld[0][2] + tags[0x6100002f]
            loads = [p for p in e.ph if p[0] in (1, 0x61000010)]
            for o in range(ro, ro + tags.get(0x61000031, 0), 24):
                off, info, add = struct.unpack_from('<QQq', out, o)
                if info & 0xffffffff == 8:
                    for p in loads:
                        if p[3] <= off < p[3] + p[5]:
                            struct.pack_into('<Q', out, p[2] + off - p[3], add & 0xffffffffffffffff)
                            break
    return Elf(bytes(out))


def members(x, mem, n, names):
    ms = []
    sz = 0x18 if x.ps == 4 else 0x28
    mo = x.off(mem) if mem else None
    if mo is None or not n:
        return []
    for k in range(n):
        r = mo + k * sz
        if x.ps == 4:
            mn, c = struct.unpack_from('>II', x.d, r)
            t, st, ca, fl, of = struct.unpack_from('>BBhHH', x.d, r + 0xc)
        else:
            mn, c = struct.unpack_from('<QQ', x.d, r)
            t, st, ca, fl, of = struct.unpack_from('<BBhHH', x.d, r + 0x18)
        nm = x.cstr(mn)
        if nm is None or t >= len(TY) or st >= len(TY):
            raise ValueError('member %x %d' % (mem, k))
        ms.append(dict(name=nm, type=TY[t], sub=TY[st], carr=ca, flags=fl, off=of, cls=names.get(c) or x.cstr(c)))
    return ms


def finish(x, recs):
    names = {r['this']: r['name'] for r in recs}
    out = {}
    for r in recs:
        try:
            m = members(x, r['mem'], r['nmem'], names)
        except ValueError:
            m = []
        e = dict(parent=names.get(r['parent']), size=r['size'], members=m)
        if r['name'] not in out or (m and not out[r['name']]['members']):
            out[r['name']] = e
    return out


def scan_ps3(x):
    """PPC64: the most called function whose r4 is a 'hk...' class name = the hkClass constructor; its r3 / r5 / r6
    and the stack slots 0x70 / 0x78 (members, member count) give each class"""
    d = x.d
    opd = x.off(x.entry)
    toc = struct.unpack_from('>I', d, opd + 4)[0]
    v, o, f, _ = x.seg[0]
    end = o + f
    r = {2: toc}
    st = {}
    sites = collections.defaultdict(list)
    for j in range(o, end, 4):
        r[2] = toc
        w = struct.unpack_from('>I', d, j)[0]
        op = w >> 26
        rt = (w >> 21) & 31
        ra = (w >> 16) & 31
        si = w & 0xffff
        s = si - 0x10000 if si & 0x8000 else si
        if op == 14:
            if ra == 0:
                r[rt] = s & 0xffffffff
            elif ra in r:
                r[rt] = (r[ra] + s) & 0xffffffff
            else:
                r.pop(rt, None)
        elif op == 15:
            if ra == 0:
                r[rt] = (s << 16) & 0xffffffff
            elif ra in r:
                r[rt] = (r[ra] + (s << 16)) & 0xffffffff
            else:
                r.pop(rt, None)
        elif op == 24:
            if rt in r:
                r[ra] = r[rt] | si
        elif op == 32:
            if ra == 1 and rt >= 13:
                continue
            val = x.u32((r[ra] + s) & 0xffffffff) if ra in r else None
            if val is not None:
                r[rt] = val
            else:
                r.pop(rt, None)
        elif op == 31 and ((w >> 1) & 1023) == 444 and rt == ((w >> 11) & 31):
            if rt in r:
                r[ra] = r[rt]
            else:
                r.pop(ra, None)
        elif op in (36, 62) and ra == 1:
            st[s & ~3 if op == 62 else s] = r.get(rt)
        elif op in (58, 34, 40, 33, 42):
            if ra == 1 and rt >= 13:
                continue
            r.pop(rt, None)
        elif op == 30 or op == 21:
            if rt in r:
                r[ra] = r[rt] & 0xffffffff
            else:
                r.pop(ra, None)
        elif op == 31 and ((w >> 1) & 1023) == 986:
            if rt in r:
                r[ra] = r[rt]
            else:
                r.pop(ra, None)
        elif op == 18 and w & 3 == 1:
            li = w & 0x3fffffc
            li = li - 0x4000000 if li & 0x2000000 else li
            nm = x.cstr(r.get(4, 0))
            if nm and nm.startswith(('hk', 'Hk')):
                sites[v + j - o + li].append(dict(this=r.get(3), name=nm, parent=r.get(5), size=r.get(6),
                                                  mem=st.get(0x70) or 0, nmem=st.get(0x78) or 0))
            for q in range(3, 13):
                r.pop(q, None)
            st = {}
    if not sites:
        raise TablesMissing('no hkClass constructor calls found in the PS3 EBOOT')
    return max(sites.values(), key=len)


def scan_ps4(x, objdump, log=print):
    """x86-64: GNU objdump of the first PT_LOAD; the most called function whose rsi is a 'hk...' class name"""
    t = [p for p in x.ph if p[0] == 1 and p[3] == 0]
    if not t:
        raise TablesMissing('PS4 executable: no text segment at address 0')
    import tempfile
    tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.bin')
    tmp.write(x.d[t[0][2]:t[0][2] + t[0][5]])
    tmp.close()
    re_lea = re.compile(r'lea\s+(\w+),\[rip[^\]]*\]\s+# (0x[0-9a-f]+)')
    re_mi = re.compile(r'mov\s+(?:\w+ PTR )?\[rsp(?:\+(0x[0-9a-f]+))?\],(0x[0-9a-f]+)$')
    re_ms = re.compile(r'mov\s+(?:\w+ PTR )?\[rsp(?:\+(0x[0-9a-f]+))?\],(\w+)$')
    re_mr = re.compile(r'mov\s+(\w+),(\w+)$')
    re_mimm = re.compile(r'mov\s+(\w+),(0x[0-9a-f]+)$')
    re_xor = re.compile(r'xor\s+(\w+),(\w+)$')
    re_call = re.compile(r'call\s+(0x[0-9a-f]+)')
    low = {'esi': 'rsi', 'edi': 'rdi', 'edx': 'rdx', 'ecx': 'rcx', 'r8d': 'r8', 'r9d': 'r9', 'eax': 'rax', 'ebx': 'rbx',
           'ebp': 'rbp'}

    def n(a):
        return low.get(a, a)
    sites = collections.defaultdict(list)
    lines = collections.deque(maxlen=400)
    p = subprocess.Popen([objdump, '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', tmp.name],
                         stdout=subprocess.PIPE, text=True, errors='replace',
                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    try:
        for ln in p.stdout:
            m = re_call.search(ln)
            if not m:
                lines.append(ln)
                continue
            r = {}
            st = {}
            for l in lines:
                tx = l.split('\t')[-1].strip()
                mm = re_lea.search(tx)
                if mm:
                    r[n(mm.group(1))] = int(mm.group(2), 16)
                    continue
                mm = re_mi.search(tx)
                if mm:
                    st[int(mm.group(1) or '0', 16)] = int(mm.group(2), 16)
                    continue
                mm = re_ms.search(tx)
                if mm:
                    if n(mm.group(2)) in r:
                        st[int(mm.group(1) or '0', 16)] = r[n(mm.group(2))]
                    continue
                mm = re_mimm.search(tx)
                if mm:
                    r[n(mm.group(1))] = int(mm.group(2), 16)
                    continue
                mm = re_mr.search(tx)
                if mm:
                    if n(mm.group(2)) in r:
                        r[n(mm.group(1))] = r[n(mm.group(2))]
                    else:
                        r.pop(n(mm.group(1)), None)
                    continue
                mm = re_xor.search(tx)
                if mm and mm.group(1) == mm.group(2):
                    r[n(mm.group(1))] = 0
            nm = x.cstr(r.get('rsi', 0))
            if nm and nm.startswith(('hk', 'Hk')):
                sites[int(m.group(1), 16)].append(dict(this=r.get('rdi'), name=nm, parent=r.get('rdx'),
                                                       size=r.get('rcx'), mem=st.get(0x10, 0), nmem=st.get(0x18, 0)))
            lines = collections.deque([l for l in lines if re.search(r'(?:rbx|rbp|r12|r13|r14|r15|\[rsp)', l)],
                                      maxlen=400)
    finally:
        p.wait()
        os.unlink(tmp.name)
    if p.returncode or not sites:
        raise TablesMissing('objdump found no hkClass constructor calls in the PS4 executable')
    return max(sites.values(), key=len)


# ------------------------------------------------------------------ classes the PS4 executable builds at run time
# Havok 2011.3 member declarations (order, type, subtype, flags, class) of the classes whose member tables are not static
# data in the PS4 executable. No offsets: layout() places them with the C rules (alignment = size, vectors 16, pointers
# and arrays 8, structs = their largest alignment); u2havok.check_tables verifies the result on retail packfiles.
def _d(name, t, sub='VOID', flags=0, cls=None, carr=0):
    return dict(name=name, type=t, sub=sub, flags=flags, cls=cls, carr=carr)


FILL_DECL = {
    'hkpMoppCode': ('hkReferencedObject', [
        _d('info', 'STRUCT', cls='hkpMoppCodeCodeInfo'), _d('data', 'ARRAY', 'UINT8'), _d('buildType', 'ENUM', 'INT8')]),
    'hkpExtendedMeshShapeTrianglesSubpart': ('hkpExtendedMeshShapeSubpart', [
        _d('numTriangleShapes', 'INT32'), _d('vertexBase', 'POINTER', flags=0x400), _d('numVertices', 'INT32'),
        _d('indexBase', 'POINTER', flags=0x400), _d('vertexStriding', 'UINT16'), _d('triangleOffset', 'INT32'),
        _d('indexStriding', 'UINT16'), _d('stridingType', 'ENUM', 'INT8'), _d('flipAlternateTriangles', 'INT8'),
        _d('extrusion', 'VECTOR4'), _d('transform', 'QSTRANSFORM')]),
    'hkpExtendedMeshShape': ('hkpShapeCollection', [
        _d('embeddedTrianglesSubpart', 'STRUCT', cls='hkpExtendedMeshShapeTrianglesSubpart'),
        _d('aabbHalfExtents', 'VECTOR4'), _d('aabbCenter', 'VECTOR4'), _d('materialClass', 'POINTER', flags=0x400),
        _d('numBitsForSubpartIndex', 'INT32'),
        _d('trianglesSubparts', 'ARRAY', 'STRUCT', cls='hkpExtendedMeshShapeTrianglesSubpart'),
        _d('shapesSubparts', 'ARRAY', 'STRUCT', cls='hkpExtendedMeshShapeShapesSubpart'),
        _d('weldingInfo', 'ARRAY', 'UINT16'), _d('weldingType', 'ENUM', 'UINT8'),
        _d('defaultCollisionFilterInfo', 'UINT32'), _d('cachedNumChildShapes', 'INT32'), _d('triangleRadius', 'REAL')]),
    'hkaSkeletonMapperData': (None, [
        _d('skeletonA', 'POINTER', 'STRUCT', cls='hkaSkeleton'), _d('skeletonB', 'POINTER', 'STRUCT', cls='hkaSkeleton'),
        _d('simpleMappings', 'ARRAY', 'STRUCT', cls='hkaSkeletonMapperDataSimpleMapping'),
        _d('chainMappings', 'ARRAY', 'STRUCT', cls='hkaSkeletonMapperDataChainMapping'),
        _d('unmappedBones', 'ARRAY', 'INT16'), _d('extractedMotionMapping', 'QSTRANSFORM'),
        _d('keepUnmappedLocal', 'BOOL'), _d('mappingType', 'ENUM', 'INT32')]),
    'hkpBallSocketConstraintAtom': ('hkpConstraintAtom', [
        _d('solvingMethod', 'UINT8'), _d('bodiesToNotify', 'UINT8'), _d('velocityStabilizationFactor', 'UINT8'),
        _d('maxImpulse', 'REAL'), _d('inertiaStabilizationFactor', 'REAL')]),
}
_SCALAR = {'BOOL': 1, 'CHAR': 1, 'INT8': 1, 'UINT8': 1, 'INT16': 2, 'UINT16': 2, 'HALF': 2, 'INT32': 4, 'UINT32': 4,
           'REAL': 4, 'INT64': 8, 'UINT64': 8, 'ULONG': 8, 'POINTER': 8, 'CSTRING': 8, 'STRINGPTR': 8}
_VEC = {'VECTOR4': 16, 'QUATERNION': 16, 'MATRIX3': 48, 'ROTATION': 48, 'QSTRANSFORM': 48, 'MATRIX4': 64,
        'TRANSFORM': 64}


def _size_align(db, m):
    t = m['type']
    if t in _SCALAR:
        return _SCALAR[t], _SCALAR[t]
    if t in _VEC:
        return _VEC[t], 16
    if t in ('ENUM', 'FLAGS'):
        return _SCALAR[m['sub']], _SCALAR[m['sub']]
    if t == 'ARRAY':
        return 16, 8
    if t == 'STRUCT':
        c = db[m['cls']]
        return c['size'], _class_align(db, m['cls'])
    raise KeyError(t)


def _class_align(db, cls):
    a = 1
    c = cls
    while c:
        for m in db[c]['members'] or []:
            a = max(a, _size_align(db, m)[1])
        a = max(a, 8 if db[c]['parent'] or c in ('hkReferencedObject', 'hkBaseObject') else 1)
        c = db[c]['parent']
    return a


def layout(db):
    """add the FILL_DECL classes to the PS4 table with C-rule offsets and sizes (dependencies first)"""
    todo = dict(FILL_DECL)
    while todo:
        for cls, (parent, decl) in list(todo.items()):
            deps = [parent] + [m['cls'] for m in decl if m['type'] == 'STRUCT']
            if any(d in todo for d in deps if d):
                continue
            off = db[parent]['size'] if parent else 0
            align = _class_align(db, parent) if parent else 1
            ms = []
            for m in decl:
                s, a = _size_align(db, m)
                off = (off + a - 1) // a * a
                ms.append(dict(m, off=off))
                off += s
                align = max(align, a)
            db[cls] = dict(parent=parent, size=(off + align - 1) // align * align, members=ms)
            del todo[cls]


# ------------------------------------------------------------------ PS4 signatures from retail packfiles
def ps4_signatures(actor_dir, log=print):
    sig = {}
    for f in sorted(os.listdir(actor_dir)):
        if not f.endswith('.pak'):
            continue
        d = open(os.path.join(actor_dir, f), 'rb').read()
        for m in re.finditer(rb'(....)\x09(hk[A-Za-z0-9_]+)\x00', d, re.S):
            sig.setdefault(m.group(2).decode(), struct.unpack('<I', m.group(1))[0])
    return sig


def _packfiles(d):
    """little-endian packfiles in a PS4 pak: (data, loc fixup sources, glob fixup sources, [(offset, class)])"""
    mag = b'\x57\xe0\xe0\x57\x10\xc0\xc0\x10'
    i = 0
    while True:
        i = d.find(mag, i)
        if i < 0:
            return
        if d[i + 8:i + 16] == mag:
            i += 8
            continue
        b = d[i:]
        try:
            nsec = struct.unpack_from('<I', b, 0x14)[0]
            secs = {}
            for s in range(nsec):
                o = 0x40 + 0x30 * s
                secs[b[o:o + 19].split(b'\0')[0]] = struct.unpack_from('<7I', b, o + 20)
            st, lf, gf, vf, ex = secs[b'__data__'][:5]
            c0, cl = secs[b'__classnames__'][:2]
            cn = {}
            c = b[c0:c0 + cl]
            k = 0
            while k + 5 < len(c) and c[k:k + 4] != b'\xff\xff\xff\xff':
                j = c.find(b'\0', k + 5)
                cn[k + 5] = c[k + 5:j].decode()
                k = j + 1
            loc = {struct.unpack_from('<I', b, o)[0] for o in range(st + lf, st + gf, 8)}
            glob = {struct.unpack_from('<I', b, o)[0] for o in range(st + gf, st + vf - 11, 12)}
            virt = sorted((struct.unpack_from('<I', b, o)[0], cn.get(struct.unpack_from('<I', b, o + 8)[0]))
                          for o in range(st + vf, st + ex - 11, 12) if struct.unpack_from('<I', b, o)[0] != 0xffffffff)
            yield b[st:st + lf], loc, glob, virt
        except (KeyError, struct.error, UnicodeDecodeError):
            pass
        i += 8


def check_layout(db, actor_dir, log=print, max_objects=400):
    """the FILL_DECL layouts against the retail PS4 packfiles: array members where the arrays are (count, capacity
    flags, local fixup), object pointers where the global fixups are, objects not larger than their space"""
    want = set(FILL_DECL) | {'hkpStorageExtendedMeshShape', 'hkaSkeletonMapper'}

    def walk(cls, base, data, loc, glob, errs):
        c = cls
        while c:
            for m in db[c]['members'] or []:
                o = base + m['off']
                if m['type'] == 'ARRAY' and not m['flags'] & 0x400:
                    n, cap = struct.unpack_from('<iI', data, o + 8)
                    if cap & 0x3fffffff != n or (n and o not in loc):
                        errs.append('%s.%s' % (c, m['name']))
                elif m['type'] == 'POINTER' and m['sub'] == 'STRUCT' and not m['flags'] & 0x400:
                    if o not in glob and data[o:o + 8] != bytes(8):
                        errs.append('%s.%s' % (c, m['name']))
                elif m['type'] == 'STRUCT':
                    walk(m['cls'], o, data, loc, glob, errs)
            c = db[c]['parent']
    seen = collections.Counter()
    errs = []
    for f in sorted(os.listdir(actor_dir)):
        if not f.endswith('.pak') or sum(seen.values()) >= max_objects:
            continue
        d = open(os.path.join(actor_dir, f), 'rb').read()
        if b'hkpMoppCode' not in d and b'hkaSkeletonMapper' not in d:
            continue
        for data, loc, glob, virt in _packfiles(d):
            for k, (o, cls) in enumerate(virt):
                if cls not in want or cls not in db:
                    continue
                end = virt[k + 1][0] if k + 1 < len(virt) else len(data)
                if db[cls]['size'] > end - o:
                    errs.append('%s size' % cls)
                walk(cls, o, data, loc, glob, errs)
                seen[cls] += 1
    if errs or not seen:
        raise TablesMissing('Havok 2011 layouts do not fit the retail packfiles: %s' % (
            ', '.join(sorted(set(errs))[:6]) or 'no retail objects found'))
    log('Havok tables: layouts checked on %s retail objects' % dict(seen))


# ------------------------------------------------------------------ driver
def find_objdump(arg=''):
    here = os.path.dirname(os.path.abspath(__file__))
    cands = [arg, os.path.join(here, '..', '..', '..', 'u3mp', 'bin', 'objdump.exe'),
             os.path.join(here, '..', '..', '..', 'u3mp', 'bin', 'binutils', 'objdump.exe'),
             r'C:\msys64\ucrt64\bin\objdump.exe', r'C:\msys64\mingw64\bin\objdump.exe', r'C:\msys64\usr\bin\objdump.exe',
             shutil.which('objdump') or '']
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    return None


def ps4_executable(game):
    for c in ('big2-ps4_Shipping.elf', 'eboot.bin'):
        for base in (game, os.path.dirname(game)):
            p = os.path.join(base, c)
            if os.path.isfile(p):
                return p
    return None


def build(ps3_elf, ps4_game, actor_dir, out_path, objdump='', log=print):
    """write the tables (json) to out_path; raises TablesMissing with the reason"""
    if not ps3_elf or not os.path.isfile(ps3_elf):
        raise TablesMissing('no decrypted PS3 Uncharted 2 EBOOT.elf (--ps3-elf)')
    d3 = open(ps3_elf, 'rb').read()
    if d3[:4] != b'\x7fELF':
        raise TablesMissing('%s is not a decrypted ELF' % ps3_elf)
    ps4 = ps4_executable(ps4_game)
    if not ps4:
        raise TablesMissing('no big2-ps4_Shipping.elf / eboot.bin in the PS4 game folder')
    od = find_objdump(objdump)
    if not od:
        raise TablesMissing('GNU objdump not found (u3mp/bin, msys64 or PATH)')
    log('Havok tables: PS3 %s, PS4 %s, objdump %s' % (ps3_elf, ps4, od))
    x3 = Elf(d3)
    db3 = finish(x3, scan_ps3(x3))
    x4 = unwrap_ps4(ps4)
    db4 = finish(x4, scan_ps4(x4, od, log))
    layout(db4)
    check_layout(db4, actor_dir, log)
    sig = ps4_signatures(actor_dir, log)
    tables = dict(version=VERSION, db3=db3, db4=db4, sig4=sig)
    tmp = out_path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(tables, f)
    os.replace(tmp, out_path)
    log('Havok tables: %d PS3 / %d PS4 classes, %d PS4 signatures -> %s' % (len(db3), len(db4), len(sig), out_path))
    return tables


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ps3-elf', required=True)
    ap.add_argument('--ps4', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--objdump', default='')
    a = ap.parse_args()
    game = a.ps4
    u = os.path.join(game, 'u2data') if os.path.isdir(os.path.join(game, 'u2data')) else game
    build(a.ps3_elf, game, os.path.join(u, 'build', 'main', 'actor27', 'ps4'), a.out, a.objdump)


if __name__ == '__main__':
    sys.exit(main())
