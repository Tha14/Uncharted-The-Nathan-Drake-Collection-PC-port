"""Havok packfiles of Uncharted 2 actors: PS3 (Havok-6.1.0-r1, 32-bit BE) -> PS4 (hk_2011.3.0-r1, 64-bit LE).

Built on the U3 reflection-driven converter (u3mp paktools/hkconvert.py, PS3 2010.2 -> PS4 2011.3), loaded from
its source with other class tables:
  DB3  Havok 6.1 class layouts of the U2 PS3 executable (hkClass constructor calls + static hkClassMember arrays)
  DB4  Havok 2011.3 class layouts of the PS4 executable (as U3 PS4)
Both are built on the player's PC by u2hkgen (from the player's PS3 EBOOT.elf, PS4 executable and retail packfiles)
and loaded from the json it writes (configure()); nothing derived from the games ships with the import.
U2 PS3 and PS4 actor packfiles hold the same classes (2000 pairs of the actor27 paks), except:
  hkaBone                 6.1 object (skeleton keeps pointers) -> 2011 inline struct of hkaSkeleton.bones
  hkaSkeletonMapperData   6.1 top-level object -> 2011 hkaSkeletonMapper {mapping: hkaSkeletonMapperData}
The model changes are in transform_61() (run before the U3 2010 -> 2011 changes).
"""
import os
import struct
import sys
import types

import u2paktools

import u2hkgen

_mod = None
_TABLES = None          # path of the u2hkgen json
_BUILD = None           # callable that writes it when missing (the importer's u2hkgen.build call)
TablesMissing = u2hkgen.TablesMissing


def configure(path, build=None):
    """use the Havok tables at path; build() writes them when the file is missing or of another version"""
    global _TABLES, _BUILD, _mod
    _TABLES, _BUILD, _mod = path, build, None


def _load_tables():
    import json
    if not _TABLES:
        raise TablesMissing('no Havok tables configured')
    t = None
    if os.path.isfile(_TABLES):
        try:
            t = json.load(open(_TABLES, encoding='utf-8'))
        except ValueError:
            t = None
    if not t or t.get('version') != u2hkgen.VERSION:
        if _BUILD is None:
            raise TablesMissing('Havok tables %s not found' % _TABLES)
        t = _BUILD()
    return t


def module():
    """hkconvert loaded with the U2 class tables"""
    global _mod
    if _mod is not None:
        return _mod
    path = os.path.join(u2paktools.PAKTOOLS, 'hkconvert.py')
    src = open(path, encoding='utf-8').read()
    old = ("DB3 = json.load(open(os.path.join(HERE, 'data', 'hk2010_ps3.json')))\n"
           "DB4 = json.load(open(os.path.join(HERE, 'data', 'hk2011_ps4.json')))\n")
    if src.count(old) != 1:
        raise RuntimeError('hkconvert.py changed: the class table load was not found once')
    src = src.replace(old, 'DB3 = _U2_DB3\nDB4 = _U2_DB4\n')
    # retail U2 packfile data layout: an object is padded to 16, array data is not; a string follows the previous
    # data unaligned and is padded to 16 after (U3 aligns before every string and after every array)
    for old_, new_, what in (("        if d[0] == 'str':\n            w.align(16)\n", "        if d[0] == 'str':\n",
                              'string writer'),
                             ("            w.glob = saved\n            w.align(16)\n", "            w.glob = saved\n",
                              'array writer'),
                             ("        write_struct(w, o, cls, v, objmap, deferred)\n        flush_deferred(",
                              "        write_struct(w, o, cls, v, objmap, deferred)\n        w.align(16)\n"
                              "        flush_deferred(", 'object writer'),
                             # local fixups in the order their targets were written
                             ("for s, d in sorted(w.loc))", "for s, d in sorted(w.loc, key=lambda x: x[1]))",
                              'local fixup table')):
        if src.count(old_) != 1:
            raise RuntimeError('hkconvert.py changed: the %s was not found once' % what)
        src = src.replace(old_, new_)
    mod = types.ModuleType('u2_hkconvert')
    mod.__file__ = path
    t = _load_tables()
    mod.__dict__['_U2_DB3'] = t['db3']
    mod.__dict__['_U2_DB4'] = t['db4']
    exec(compile(src, path, 'exec'), mod.__dict__)
    mod.SIG4.update(t['sig4'])
    u3_transform = mod.transform_objects

    def transform(objs):
        return u3_transform(transform_61(objs))
    mod.transform_objects = transform
    u3_write_struct = mod.write_struct

    def write_struct(w, o, cls, v, objmap, deferred):
        # members the 6.1 object lacks: arrays are written empty with DONT_DEALLOCATE as retail has them
        for c in mod.chain(mod.DB4, cls):
            for m in mod.DB4[c]['members'] or []:
                if m['type'] == 'ARRAY' and m['name'] not in v and not m['carr'] and not (m['flags'] & 0x400):
                    struct.pack_into('<I', w.D, o + m['off'] + 12, 0x80000000)
        u3_write_struct(w, o, cls, v, objmap, deferred)
    mod.write_struct = write_struct
    # 6.1 hkVariant {ptr object, ptr class} (hkRootLevelContainer) -> 2011 a plain object pointer
    u3_read_value, u3_msize = mod.read_value, mod.msize

    def read_value(pf, db, m, o):
        if m['type'] == 'VARIANT':
            return u3_read_value(pf, db, dict(m, type='POINTER'), o)
        return u3_read_value(pf, db, m, o)

    def msize(db, m, ps):
        if m['type'] == 'VARIANT':
            return 2 * ps * max(1, m['carr'])
        return u3_msize(db, m, ps)
    mod.read_value, mod.msize = read_value, msize
    _mod = mod
    return mod


# hkpMotion::MotionType: 6.1 has STABILIZED_SPHERE (3) / STABILIZED_BOX (5), gone in 2011
MOTION_TYPE = {0: 0, 1: 1, 2: 2, 3: 2, 4: 3, 5: 3, 6: 4, 7: 5, 8: 6, 9: 7}
HALF_ONE = 0x3f80
# hkpCollidableQualityType 6.1 -> 2011 as retail has it (keyframed props: 2 -> 1, ragdoll bodies: 4 -> 4)
QUALITY = {0: 0, 1: 1, 2: 1, 3: 4, 4: 4}
STAB_MAX_ANGLE = struct.unpack("<f", struct.pack("<I", 0x5f7ffff0))[0]   # retail 2011 default
MOTIONS = ('hkpMotion', 'hkpKeyframedRigidMotion', 'hkpMaxSizeMotion', 'hkpBoxMotion', 'hkpSphereMotion',
           'hkpThinBoxMotion', 'hkpFixedRigidMotion', 'hkpCharacterMotion')


def half(f):
    """hkHalf: the upper 16 bits of the float"""
    return struct.unpack('>I', struct.pack('>f', float(f)))[0] >> 16


def _motion(m):
    if not isinstance(m, dict):
        return
    if 'type' in m:
        m['type'] = MOTION_TYPE.get(m['type'], m['type'])
    ms = m.get('motionState')
    if isinstance(ms, dict):
        for k in ('linearDamping', 'angularDamping'):
            if isinstance(ms.get(k), float):
                ms[k] = half(ms[k])
        ms['timeFactor'] = HALF_ONE
    m['gravityFactor'] = HALF_ONE


def _subpart(s):
    """6.1 TrianglesSubpart -> 2011 (typeAndFlags, index striding enum, transform)"""
    s['typeAndFlags'] = (s.get('type', 0) & 1) | ((s.get('materialIndexStridingType', 0) & 3) << 1) | \
        (s.get('numMaterials', 0) << 3)
    if 'stridingType' in s and s['stridingType']:
        s['stridingType'] += 1                  # 2011 inserted INDICES_INT8
    s['transform'] = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0]


def transform_61(objs):
    """Havok 6.1 -> 2010/2011 object model changes. objs: [id, class, value dict]"""
    byid = {o[0]: o for o in objs}
    drop = set()
    out = []
    for o in objs:
        oid, cls, v = o
        if cls in MOTIONS:
            _motion(v)
        if cls == 'hkpRigidBody':
            _motion(v.get('motion'))
            v['contactPointCallbackDelay'] = v.pop('processContactCallbackDelay', 0xffff)
            v['numShapeKeysInContactPointProperties'] = v.pop('numUserDatasInContactPointProperties', 0)
            bp = (v.get('collidable') or {}).get('broadPhaseHandle')
            if isinstance(bp, dict) and 'objectQualityType' in bp:
                q = bp['objectQualityType']
                bp['objectQualityType'] = QUALITY.get(q, q + 1 if 0 < q < 0x8000 else q)
        if cls == 'hkpStorageExtendedMeshShape':
            sc = v.get('scaling') or [1.0, 1.0, 1.0, 1.0]
            for s in [v.get('embeddedTrianglesSubpart')] + list(v.get('trianglesSubparts') or []):
                if isinstance(s, dict):
                    _subpart(s)
                    s['transform'][8:12] = list(sc)
            v['cachedNumChildShapes'] = -0x80000000
        if cls in ('hkpRagdollConstraintData', 'hkpLimitedHingeConstraintData') and isinstance(v.get('atoms'), dict):
            v['atoms']['setupStabilization'] = dict(type=23, enabled=0, maxAngle=STAB_MAX_ANGLE)
        if isinstance(v.get('atoms'), dict) and isinstance(v['atoms'].get('ballSocket'), dict):
            v['atoms']['ballSocket'].update(solvingMethod=1, velocityStabilizationFactor=0x30,
                                            inertiaStabilizationFactor=0.0)
        if cls == 'hkpStorageExtendedMeshShapeMeshSubpartStorage':
            w = v.get('materials') or []
            v['materials'] = [dict(filterInfo=w[i], restitution=0, friction=HALF_ONE, userData=w[i + 1])
                              for i in range(0, len(w) - 1, 2)]
        if cls == 'hkaSkeleton':
            bones = []
            for b in v.get('bones') or []:
                if isinstance(b, tuple) and b[0] == 'obj' and b[1] in byid:
                    bones.append(dict(byid[b[1]][2]))
                    drop.add(b[1])
                elif isinstance(b, dict):
                    bones.append(b)
            v['bones'] = bones
        if cls == 'hkaSkeletonMapperData':
            o = [oid, 'hkaSkeletonMapper', dict(mapping=v)]
        out.append(o)
    return [o for o in out if o[0] not in drop]


def convert_packfile(b3):
    return module().convert_packfile(b3)
