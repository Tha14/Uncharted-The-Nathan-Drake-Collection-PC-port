"""Uncharted 2 JOINT_HIERARCHY, PS3 -> PS4, on the U3 skeleton converter (u3mp paktools/skel.py).

The skeleton blob, joint names, B / S tables and the D block are the U3 formats (877 of 949 PS3/PS4 actor27 pairs
come out byte-identical apart from uninitialised pad bytes, IMPORT.md).  U2 difference: props with a ragdoll or a
skeleton mapper keep a Havok packfile behind their D block (PS3 D +0x4c, PS4 D +0x90; u2actorcoll.jh_d_packfile).
The PS4 item then holds the converted packfile (wrapper + packfile) right after the D block.
skel.py is loaded from its own source with two asserted edits; the U3 file stays untouched.
"""
import os
import types

import u2paktools

u2paktools.setup()
import u2actorcoll  # noqa: E402

_EDITS = [
    # read: a D block with a packfile is read by u2actorcoll (other D contents still stop the conversion)
    ("""        if struct.unpack_from('>I', dd)[0] != 3 or any(dd[4:]):
            raise NotImplementedError('JOINT_HIERARCHY D block differs from {3, 0...}')
""",
     """        if struct.unpack_from('>I', dd)[0] != 3 or any(dd[4:]):
            _pf = _u2_d_packfile(p, it)
            if _pf is None:
                raise NotImplementedError('JOINT_HIERARCHY D block differs from {3, 0...}')
            _u2_pf[0] = _pf
"""),
    ("""    return dict(ver=ver, nj=nj, blob=blob, names=names, d=d is not None, b=B is not None, s=S,
                sid=p.pu32(pg, o + 0x1c))""",
     """    _pf, _u2_pf[0] = _u2_pf[0], None
    return dict(ver=ver, nj=nj, blob=blob, names=names, d=d is not None, b=B is not None, s=S,
                sid=p.pu32(pg, o + 0x1c), pf=_pf)"""),
    # build: D template, then (when there is one) the converted packfile; D +0x90 points at its wrapper
    ("""        b.raw(D_TEMPLATE)
""",
     """        _do = b.pos
        b.raw(D_TEMPLATE)
        if m.get('pf') is not None:
            b.pad(16, 0xaa)
            rel.append((_do + 0x90, b.pos))
            b.raw(_u2_d_wrapper(_u2_convert(m['pf'])))
"""),
]

_mod = None


def module():
    global _mod
    if _mod is None:
        path = os.path.join(u2paktools.PAKTOOLS, 'skel.py')
        src = open(path, encoding='utf-8').read()
        for old, new in _EDITS:
            if src.count(old) != 1:
                raise RuntimeError('skel.py changed: an edit point was not found once')
            src = src.replace(old, new)
        mod = types.ModuleType('u2_skel')
        mod.__file__ = path
        import u2havok
        mod.__dict__.update(_u2_d_packfile=u2actorcoll.jh_d_packfile, _u2_d_wrapper=u2actorcoll.d_wrapper,
                            _u2_convert=u2havok.convert_packfile, _u2_pf=[None])
        exec(compile(src, path, 'exec'), mod.__dict__)
        _mod = mod
    return _mod


def read_jh(p, it):
    return module().read_jh(p, it)


def build_jh(m):
    return module().build_jh(m)


def jh_blob(p, it):
    return build_jh(read_jh(p, it))
