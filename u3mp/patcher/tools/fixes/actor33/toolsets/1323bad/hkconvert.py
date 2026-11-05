#!/usr/bin/env python3
"""Havok packfile converter: PS3 (hk_2010.2.0-r1, 32-bit BE) -> PS4 (hk_2011.3.0-r1, 64-bit LE).

Reflection-driven: class layouts come from the hkClass/hkClassMember tables of both executables
(data/hk2010_ps3.json from the PS3 eboot, data/hk2011_ps4.json from the PS4 image; a few PS4
classes whose member tables are built at run time are given in OVERRIDE4 below).  Members are
matched by name; members missing on PS3 are zero, NOT_SERIALIZABLE (0x400) members are zero.
Class signatures for the PS4 __classnames__ section come from retail PS4 packfiles (SIG4).

Used for actor collision (COLLISION_DATA_FOREGROUND: hkpRigidBody + shapes + motion).
"""
import json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
DB3 = json.load(open(os.path.join(HERE, 'data', 'hk2010_ps3.json')))
DB4 = json.load(open(os.path.join(HERE, 'data', 'hk2011_ps4.json')))


def _m(name, t, sub='VOID', off=0, carr=0, flags=0, cls=None):
    return dict(name=name, type=t, sub=sub, carr=carr, flags=flags, off=off, cls=cls)


# PS4 classes whose member arrays are run-time initialised (.bss) - layouts checked against retail
OVERRIDE4 = {
    'hkpMaterial': [_m('responseType', 'ENUM', 'INT8', 0), _m('rollingFrictionMultiplier', 'HALF', off=2),
                    _m('friction', 'REAL', off=4), _m('restitution', 'REAL', off=8)],
    'hkcdShape': [_m('type', 'ENUM', 'UINT8', 0x0c), _m('dispatchType', 'ENUM', 'UINT8', 0x0d),
                  _m('bitsPerKey', 'UINT8', off=0x0e), _m('shapeInfoCodecType', 'ENUM', 'UINT8', 0x0f)],
    'hkpShapeCollection': [_m('disableWelding', 'BOOL', off=0x20), _m('collectionType', 'ENUM', 'UINT8', 0x21)],
    'hkpMotion': [_m('type', 'ENUM', 'UINT8', 0x0c), _m('deactivationIntegrateCounter', 'UINT8', off=0x0d),
                  _m('deactivationNumInactiveFrames', 'UINT16', off=0x0e, carr=2),
                  _m('motionState', 'STRUCT', off=0x20, cls='hkMotionState'),
                  _m('inertiaAndMassInv', 'VECTOR4', off=0xd0), _m('linearVelocity', 'VECTOR4', off=0xe0),
                  _m('angularVelocity', 'VECTOR4', off=0xf0),
                  _m('deactivationRefPosition', 'VECTOR4', off=0x100, carr=2),
                  _m('deactivationRefOrientation', 'UINT32', off=0x120, carr=2),
                  _m('savedMotion', 'POINTER', 'STRUCT', 0x128, cls='hkpMaxSizeMotion'),
                  _m('savedQualityTypeIndex', 'UINT16', off=0x130), _m('gravityFactor', 'HALF', off=0x132)],
    # 2011.3 64-bit layouts recovered from retail PS4 packfiles (member tables are run-time built)
    'hkpConstraintInstance': [_m('owner', 'POINTER', off=0x10, flags=0x400), _m('data', 'POINTER', 'STRUCT', 0x18, cls='hkpConstraintData'),
                              _m('constraintModifiers', 'POINTER', 'STRUCT', 0x20, cls='hkpModifierConstraintAtom'),
                              _m('entities', 'POINTER', 'STRUCT', 0x28, carr=2, cls='hkpEntity'),
                              _m('priority', 'ENUM', 'UINT8', 0x38), _m('wantRuntime', 'BOOL', off=0x39),
                              _m('destructionRemapInfo', 'ENUM', 'UINT8', 0x3a),
                              _m('name', 'STRINGPTR', off=0x50), _m('userData', 'ULONG', off=0x58)],
    'hkdBreakableShape': [_m('typeAndFlags', 'UINT8', off=0x20), _m('connectivityType', 'ENUM', 'UINT8', 0x21),
                          _m('flexibleJointType', 'ENUM', 'UINT8', 0x22), _m('qualityType', 'ENUM', 'UINT8', 0x23),
                          _m('parent', 'POINTER', 'STRUCT', 0x28, flags=0x400, cls='hkdBreakableShape'),
                          _m('majorAxisSpace', 'UINT32', off=0x30), _m('comAndMass', 'VECTOR4', off=0x40),
                          _m('inertiaAndValues', 'STRUCT', off=0x50, cls='hkHalf8'),
                          _m('children', 'ARRAY', 'STRUCT', 0x60, cls='hkdShapeInstanceInfo'),
                          _m('graphicsShape', 'POINTER', 'STRUCT', 0x70, cls='hkdGraphicsShape'),
                          _m('objectProperties', 'STRUCT', off=0x78, cls='hkdObjectProperties'),
                          _m('userData', 'ULONG', off=0x88)],
    'hkdGraphicsShape': [_m('type', 'ENUM', 'UINT8', 0x0c), _m('numChildTransforms', 'INT32', off=0x10)],
}
# classes whose PS4 member table is built at run time but whose layout cannot have changed (same size,
# no pointer-sized members): take the PS3 table.  Constraint atoms are the important case.
for _c, _d in DB3.items():          # PS4 parents the table scan missed (atoms: hkpConstraintAtom)
    if _c in DB4 and DB4[_c]['parent'] is None and _d['parent'] in DB4:
        DB4[_c]['parent'] = _d['parent']
_PTRT = ('POINTER', 'CSTRING', 'STRINGPTR', 'ULONG', 'FUNCTIONPOINTER', 'ARRAY', 'SIMPLEARRAY', 'STRUCT')
for _c, _d in DB3.items():
    if _c in DB4 and not DB4[_c]['members'] and _d['members'] and DB4[_c]['size'] == _d['size'] \
            and DB4[_c]['parent'] == _d['parent'] and not any(m['type'] in _PTRT for m in _d['members']):
        DB4[_c]['members'] = [dict(m) for m in _d['members']]
OVERRIDE4_EXTRA = {
    'hkpConstraintMotor': [_m('type', 'ENUM', 'INT8', 0x0c)],
    'hkpBvTreeShape': [_m('bvTreeType', 'ENUM', 'UINT8', 0x18)],
}
for _c, _v in OVERRIDE4_EXTRA.items():
    if _c in DB4 and not DB4[_c]['members']:
        DB4[_c]['members'] = _v
if 'hkHalf8' not in DB4 and 'hkHalf8' in DB3:
    DB4['hkHalf8'] = DB3['hkHalf8']           # plain 8 x hkHalf, identical layout
for k, v in OVERRIDE4.items():
    if k in DB4:
        DB4[k]['members'] = v

# PS4 class signatures (from retail PS4 packfiles)
SIG4 = {'hkClass': 0x75585ef6, 'hkClassMember': 0x5c7ea4c2, 'hkClassEnum': 0x8a3609cf, 'hkClassEnumItem': 0xce6f8a6c,
        'hkpRigidBody': 0xa34b3e93, 'hkpConvexVerticesShape': 0xffafc3aa, 'hkpConvexVerticesConnectivity': 0x63d38e9c,
        'hkpBoxMotion': 0x78a6b020, 'hkpSphereMotion': 0x78a6b020, 'hkpCylinderShape': 0x2f66d4c0,
        'hkpListShape': 0x946b2f20, 'hkpConvexTranslateShape': 0xd697cfe7, 'hkpBvCompressedMeshShape': 0xb63b3f27,
        'hkdBreakableBody': 0x5a0cc6fd, 'hkdBreakableShape': 0xae4ec162, 'hkdBreakableShapeConnectionList': 0xe1026cd5,
        'hkdStringObject': 0x4a6c78b8, 'hkdPropertyFlagSet': 0xef7ca988, 'havokGraphicsShape': 0xcb4b5fa9,
        'hkpSimpleBreakableMaterial': 0xd3e86674, 'hkdCompoundBreakableShape': 0xa693fb56,
        'hkdWoodController': 0x7e07d575, 'hkpConstraintInstance': 0x34eba5f, 'hkpRagdollConstraintData': 0x4cc52cb3,
        'hkpSpringDamperConstraintMotor': 0x7ead26f6, 'hkpLimitedHingeConstraintData': 0xf4eeac0a,
        'hkpConvexTransformShape': 0xe9d81ca, 'hkpBoxShape': 0xc1112ea, 'hkpBreakableConstraintData': 0x7d6310c8,
        'hkpSphereShape': 0x89b169a3}

SCALAR = {'BOOL': 1, 'CHAR': 1, 'INT8': 1, 'UINT8': 1, 'INT16': 2, 'UINT16': 2, 'HALF': 2, 'INT32': 4, 'UINT32': 4,
          'REAL': 4, 'INT64': 8, 'UINT64': 8}
FMT = {'BOOL': 'B', 'CHAR': 'b', 'INT8': 'b', 'UINT8': 'B', 'INT16': 'h', 'UINT16': 'H', 'HALF': 'H', 'INT32': 'i',
       'UINT32': 'I', 'REAL': 'f', 'INT64': 'q', 'UINT64': 'Q'}
VECN = {'VECTOR4': 4, 'QUATERNION': 4, 'MATRIX3': 12, 'ROTATION': 12, 'QSTRANSFORM': 12, 'MATRIX4': 16, 'TRANSFORM': 16}


def chain(db, cls):
    out = []
    while cls:
        out.append(cls)
        cls = db[cls]['parent']
    return out[::-1]


def all_members(db, cls):
    ms = []
    for c in chain(db, cls):
        ms += db[c]['members'] or []
    return ms


def msize(db, m, ps):
    t = m['type']
    if t in SCALAR:
        s = SCALAR[t]
    elif t in VECN:
        s = 4 * VECN[t]
    elif t in ('POINTER', 'CSTRING', 'STRINGPTR', 'ULONG', 'FUNCTIONPOINTER'):
        s = ps
    elif t in ('ENUM', 'FLAGS'):
        s = SCALAR[m['sub']]
    elif t == 'STRUCT':
        s = db[m['cls']]['size']
    elif t == 'ARRAY':
        s = ps + 8
    elif t == 'SIMPLEARRAY':
        s = ps + 4 if ps == 4 else 16
    elif t == 'ZERO' or t == 'VOID':
        s = 0
    else:
        raise KeyError('member type %s' % t)
    return s * max(1, m['carr'])


def elem_member(m):
    """the element description of an ARRAY/SIMPLEARRAY member"""
    return dict(name=m['name'], type=m['sub'], sub='STRUCT' if m['sub'] == 'POINTER' else 'VOID', carr=0, flags=0,
                off=0, cls=m['cls'])


# ------------------------------------------------------------------ reading (any platform)
class Packfile:
    def __init__(self, b):
        self.b = b
        self.e = '<' if b[0x11] == 1 else '>'
        self.ps = b[0x10]
        e = self.e
        nsec = struct.unpack_from(e + 'I', b, 0x14)[0]
        hsize = 0x40
        self.secs = []
        for i in range(nsec):
            o = hsize + 0x30 * i
            self.secs.append((b[o:o + 19].split(b'\0')[0].decode(), struct.unpack_from(e + '7I', b, o + 20)))
        st, lf = self.secs[0][1][:2]
        c = b[st:st + lf]
        self.cn, self.sig = {}, {}
        i = 0
        while i + 5 < len(c) and c[i:i + 4] != b'\xff\xff\xff\xff':
            j = c.find(b'\0', i + 5)
            self.cn[i + 5] = c[i + 5:j].decode()
            self.sig[self.cn[i + 5]] = struct.unpack_from(e + 'I', c, i)[0]
            i = j + 1
        st, lf, gf, vf, ex, im, end = [s for s in self.secs if s[0] == '__data__'][0][1]
        self.D = b[st:st + lf]
        self.loc, self.glob, self.virt = {}, {}, []
        for o in range(st + lf, st + gf, 8):
            a, t = struct.unpack_from(e + 'II', b, o)
            if a != 0xffffffff:
                self.loc[a] = t
        for o in range(st + gf, st + vf - 11, 12):
            a, s, t = struct.unpack_from(e + 'III', b, o)
            if a != 0xffffffff:
                self.glob[a] = t
        for o in range(st + vf, st + ex - 11, 12):
            a, s, t = struct.unpack_from(e + 'III', b, o)
            if a != 0xffffffff:
                self.virt.append((a, self.cn[t]))
        self.virt.sort()
        self.objclass = dict(self.virt)
        cs, co = struct.unpack_from(e + 'II', b, 0x18)
        self.contents = struct.unpack_from(e + 'I', b, 0x1c)[0]

    def u(self, fmt, o):
        return struct.unpack_from(self.e + fmt, self.D, o)[0]


def read_value(pf, db, m, o):
    """-> python value of member m stored at data offset o"""
    if m['carr']:
        m1 = dict(m, carr=0)
        s = msize(db, m1, pf.ps)
        return [read_value(pf, db, m1, o + s * i) for i in range(m['carr'])]
    t = m['type']
    if t in SCALAR:
        return pf.u(FMT[t], o)
    if t in VECN:
        return list(struct.unpack_from(pf.e + '%df' % VECN[t], pf.D, o))
    if t in ('ENUM', 'FLAGS'):
        return pf.u(FMT[m['sub']], o)
    if t == 'ULONG':
        return pf.u('I' if pf.ps == 4 else 'Q', o)
    if t in ('POINTER', 'FUNCTIONPOINTER'):
        if o in pf.glob:
            return ('obj', pf.glob[o])
        if o in pf.loc:
            return ('data', pf.loc[o])
        return None
    if t in ('CSTRING', 'STRINGPTR'):
        if o in pf.loc:
            a = pf.loc[o]
            return pf.D[a:pf.D.find(b'\0', a)].decode('latin-1')
        return None
    if t == 'STRUCT':
        return read_struct(pf, db, m['cls'], o)
    if t in ('ARRAY', 'SIMPLEARRAY'):
        n = pf.u('i', o + pf.ps)
        if n <= 0 or o not in pf.loc:
            return []
        a = pf.loc[o]
        em = elem_member(m)
        s = msize(db, em, pf.ps)
        return [read_value(pf, db, em, a + s * i) for i in range(n)]
    if t in ('ZERO', 'VOID'):
        return None
    raise KeyError(t)


def read_struct(pf, db, cls, o):
    v = {}
    for m in all_members(db, cls):
        v[m['name']] = read_value(pf, db, m, o + m['off'])
    return v


# ------------------------------------------------------------------ writing PS4
class Writer:
    def __init__(self):
        self.D = bytearray()
        self.loc = []      # (src, dst)
        self.glob = []     # (src, dst object offset, as source pf object id -> resolved later)
        self.virt = []     # (offset, class)
        self.pending = []  # (deferred data writers)

    def align(self, a=16):
        while len(self.D) % a:
            self.D.append(0)


def write_value(w, base, m, v, objmap, deferred):
    """write member m value v at w.D[base+off]; arrays/strings are deferred (written after the object)."""
    o = base + m['off']
    if m['carr']:
        m1 = dict(m, carr=0, off=0)
        s = msize(DB4, m1, 8)
        for i in range(m['carr']):
            write_value(w, o + s * i, m1, (v[i] if isinstance(v, list) and i < len(v) else None), objmap, deferred)
        return
    if m['flags'] & 0x400:                       # NOT_SERIALIZABLE: zero (empty arrays keep DONT_DEALLOCATE,
        if m['type'] == 'ARRAY':                 # as the retail exporter wrote them)
            struct.pack_into('<I', w.D, o + 12, 0x80000000)
        return
    t = m['type']
    if v is None:
        return
    if t in SCALAR:
        if t == 'REAL':
            struct.pack_into('<f', w.D, o, float(v))
        else:
            n = SCALAR[t]
            struct.pack_into('<' + {1: 'B', 2: 'H', 4: 'I', 8: 'Q'}[n], w.D, o, int(v) & ((1 << (8 * n)) - 1))
    elif t in VECN:
        struct.pack_into('<%df' % VECN[t], w.D, o, *v)
    elif t in ('ENUM', 'FLAGS'):
        n = SCALAR[m['sub']]
        struct.pack_into('<' + {1: 'B', 2: 'H', 4: 'I', 8: 'Q'}[n], w.D, o, int(v) & ((1 << (8 * n)) - 1))
    elif t == 'ULONG':
        struct.pack_into('<Q', w.D, o, int(v))
    elif t in ('POINTER', 'FUNCTIONPOINTER'):
        if isinstance(v, tuple) and v[0] == 'obj':
            w.glob.append((o, v[1]))
    elif t in ('CSTRING', 'STRINGPTR'):
        deferred.append(('str', o, v))
    elif t == 'STRUCT':
        write_struct(w, o, m['cls'], v, objmap, deferred)
    elif t in ('ARRAY', 'SIMPLEARRAY'):
        n = len(v)
        if t == 'ARRAY':
            struct.pack_into('<QII', w.D, o, 0, n, 0x80000000 | n)
        else:
            struct.pack_into('<Qi', w.D, o, 0, n)
        if n:
            mark = []                     # global fixups of the elements go here (writer order)
            w.glob.append(mark)
            deferred.append(('arr', o, m, v, mark))


# PS4 member <- PS3 value(s) when the 2010 -> 2011 class change renamed things
ALIAS = {('hkFourTransposedPoints', 'vertices'): lambda v: [v['x'], v['y'], v['z']] if 'x' in v else None}


def write_struct(w, o, cls, v, objmap, deferred):
    for c in chain(DB4, cls):
        for m in DB4[c]['members'] or []:
            if m['name'] in v:
                write_value(w, o, m, v[m['name']], objmap, deferred)
            elif (c, m['name']) in ALIAS:
                write_value(w, o, m, ALIAS[(c, m['name'])](v), objmap, deferred)


def flush_deferred(w, deferred, objmap):
    while deferred:
        d = deferred.pop(0)
        if d[0] == 'str':
            w.align(16)
            w.loc.append((d[1], len(w.D)))
            w.D += d[2].encode('latin-1') + b'\0'
            w.align(16)
        else:
            _, o, m, v, mark = d
            em = dict(elem_member(m), off=0)
            s = msize(DB4, em, 8)
            w.align(16)
            a = len(w.D)
            w.loc.append((o, a))
            w.D += b'\0' * (s * len(v))
            sub = []
            saved, w.glob = w.glob, mark
            for i, x in enumerate(v):
                write_value(w, a + s * i, em, x, objmap, sub)
            w.glob = saved
            w.align(16)
            deferred[0:0] = sub


def _ptr_targets(cls, v, out):
    """pointer targets of an object value, in PS4 member order (the order the Havok writer follows)"""
    for c in chain(DB4, cls):
        for m in DB4[c]['members'] or []:
            if m['flags'] & 0x400 or m['name'] not in v:
                continue
            _val_targets(m, v[m['name']], out)


def _val_targets(m, x, out):
    if x is None:
        return
    if m['carr'] and isinstance(x, list):
        for y in x:
            _val_targets(dict(m, carr=0), y, out)
        return
    t = m['type']
    if t in ('POINTER',) and isinstance(x, tuple) and x[0] == 'obj':
        out.append(x[1])
    elif t == 'STRUCT' and isinstance(x, dict):
        _ptr_targets(m['cls'], x, out)
    elif t in ('ARRAY', 'SIMPLEARRAY') and isinstance(x, list):
        em = elem_member(m)
        for y in x:
            _val_targets(em, y, out)


def _quat_from_rows(r):
    """3x3 rotation (rows) -> quaternion x,y,z,w"""
    import math
    (a, b, c), (d, e, f), (g, h, i) = r
    tr = a + e + i
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        return [(h - f) / s, (c - g) / s, (d - b) / s, 0.25 * s]
    if a > e and a > i:
        s = math.sqrt(1.0 + a - e - i) * 2
        return [0.25 * s, (b + d) / s, (c + g) / s, (h - f) / s]
    if e > i:
        s = math.sqrt(1.0 + e - a - i) * 2
        return [(b + d) / s, 0.25 * s, (f + h) / s, (c - g) / s]
    s = math.sqrt(1.0 + i - a - e) * 2
    return [(c + g) / s, (f + h) / s, 0.25 * s, (d - b) / s]


def transform_objects(objs):
    """2010.2 -> 2011.3 model changes.  objs: list of [id, cls, value] (PS3 member names); may append."""
    nid = [0]
    out = list(objs)
    for o in objs:
        oid, cls, v = o
        ch = chain(DB4, cls)
        if 'hkdBreakableShape' in ch:
            # 2011: strength moved to a per-shape hkpSimpleBreakableMaterial (typeAndFlags 0x11 in all retail data)
            nid[0] += 1
            mid = ('new', nid[0])
            out.append([mid, 'hkpSimpleBreakableMaterial', dict(strength=v.get('strength', 0.0), typeAndFlags=0x11)])
            v['material'] = ('obj', mid)
        if 'hkdController' in ch and 'breakingImpulse' not in v:
            v['breakingImpulse'] = -1.0
        if cls == 'hkpConvexTransformShape' and isinstance(v.get('transform'), list) and len(v['transform']) == 16:
            import math
            t = v['transform']                       # hkTransform: 3 rotation columns (vec4) + translation
            cols = [t[0:3], t[4:7], t[8:11]]
            sc = [math.sqrt(sum(x * x for x in c)) or 1.0 for c in cols]
            sc = [1.0 if abs(x - 1.0) < 1e-5 else x for x in sc]    # retail stores an exact unit scale
            rows = [[cols[j][i] / sc[j] for j in range(3)] for i in range(3)]
            q = _quat_from_rows(rows)
            v['transform'] = list(t[12:15]) + [0.0] + q + sc + [0.0]
            v.setdefault('extraScale', [0.0, 0.0, 0.0, 0.0])
    return out


def convert_packfile(b3, fixups=None):
    """PS3 packfile bytes -> PS4 packfile bytes.  fixups(cls, value_dict) may patch values.
    Objects are written in the Havok packfile-writer order: depth first from the contents object,
    following pointers in PS4 member order (arrays in place); each object is followed by its arrays."""
    pf = Packfile(b3)
    assert pf.ps == 4 and pf.e == '>', 'expected a PS3 packfile'
    w = Writer()
    objmap = {}
    objs = []
    for a, cls in pf.virt:
        if cls not in DB3 or cls not in DB4:
            raise KeyError('class %s not in the reflection tables' % cls)
        v = read_struct(pf, DB3, cls, a)
        if fixups:
            fixups(cls, v)
        objs.append([a, cls, v])
    objs = transform_objects(objs)
    byid = {o[0]: o for o in objs}
    order = []
    seen = set()

    def visit(oid):
        if oid in seen or oid not in byid:
            return
        seen.add(oid)
        order.append(oid)
        tg = []
        _ptr_targets(byid[oid][1], byid[oid][2], tg)
        for t in tg:
            visit(t)
    visit(pf.contents)
    for o in objs:
        visit(o[0])
    for oid in order:
        _, cls, v = byid[oid]
        w.align(16)
        o = len(w.D)
        objmap[oid] = o
        w.virt.append((o, cls))
        w.D += b'\0' * DB4[cls]['size']
        deferred = []
        write_struct(w, o, cls, v, objmap, deferred)
        flush_deferred(w, deferred, objmap)
    w.align(16)
    def _flat(l):
        for e in l:
            if isinstance(e, list):
                yield from _flat(e)
            else:
                yield e
    glob = [(s, objmap[t]) for s, t in _flat(w.glob) if t in objmap]
    # classnames
    classes = ['hkClass', 'hkClassMember', 'hkClassEnum', 'hkClassEnumItem']
    for _, c in w.virt:
        if c not in classes:
            classes.append(c)
    cn = bytearray()
    cnoff = {}
    for c in classes:
        if c not in SIG4:
            raise KeyError('no PS4 signature for %s' % c)
        cn += struct.pack('<I', SIG4[c]) + b'\x09'
        cnoff[c] = len(cn)
        cn += c.encode() + b'\0'
    while len(cn) % 16:
        cn += b'\xff'
    contents_cls = pf.objclass.get(pf.contents, w.virt[0][1])
    lf = b''.join(struct.pack('<II', s, d) for s, d in sorted(w.loc))
    lf += b'\xff' * ((-len(lf)) % 16)
    gf = b''.join(struct.pack('<III', s, 2, d) for s, d in glob)     # writer order, not sorted
    gf += b'\xff' * ((-len(gf)) % 16)
    vf = b''.join(struct.pack('<III', o, 0, cnoff[c]) for o, c in w.virt)
    vf += b'\xff' * ((-len(vf)) % 16)
    data = bytes(w.D)
    hdr = bytearray(bytes.fromhex('57e0e05710c0c010000000000900000008010101030000000200000000000000'))
    struct.pack_into('<I', hdr, 0x1c, objmap.get(pf.contents, 0))
    hdr += struct.pack('<II', 0, cnoff[contents_cls]) + b'hk_2011.3.0-r1\x00\xff' + bytes.fromhex('00000000ffffffff')
    out = bytearray(hdr)
    st_cn = 0xd0
    st_d = st_cn + len(cn)
    dl = len(data)
    secs = [(b'__classnames__', st_cn, [len(cn)] * 6), (b'__types__', st_d, [0] * 6),
            (b'__data__', st_d, [dl, dl + len(lf), dl + len(lf) + len(gf), dl + len(lf) + len(gf) + len(vf)] +
             [dl + len(lf) + len(gf) + len(vf)] * 2)]
    for tag, st, offs in secs:
        out += tag.ljust(19, b'\0') + b'\xff' + struct.pack('<7I', st, *offs)
    out += cn + data + lf + gf + vf
    return bytes(out)


if __name__ == '__main__':
    src = open(sys.argv[1], 'rb').read()
    out = convert_packfile(src)
    open(sys.argv[2], 'wb').write(out)
    print('wrote', sys.argv[2], len(out))
