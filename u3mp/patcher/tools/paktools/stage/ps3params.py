"""PS3 material constants (name -> floats) from the RSX program-instance nodes of a PS3 level pak.

material {name*, 0, 0, 8, passes*} ; the pass tree reaches program-instance nodes:
  material +0x10 -> u32 passes*[8]; pass = run of 0x20-byte entries {0, 0, u32 id, 0, node*, 0, 0, 0}
  node +0x10 ptr ?, +0x14 ptr param table, +0x18 ptr ?, +0x1c ptr constant values
  param entry 0x10 bytes {u16 index, u16 0, u16 size, u8 class(1 = uniform), u8 kind(2 = fragment const /
  1 = sampler), u32 offset, char* name}; kind 2 = material constant (class 0 vertex, 1 fragment): BE floats at values+offset.
Every distinct name found under the material is returned (first value wins; the lighting passes repeat them)."""
import struct


def _is_param_table(p, a):
    try:
        for k in range(2):
            r = p.resolve(a[0], a[1] + 0x10 * k + 0xc)
            if not r:
                return False
            s = p.pstr(*r)
            if not s or not s.isprintable() or len(s) < 3:
                return False
        return True
    except Exception:
        return False


def _node_params(p, node, out):
    tbl = p.resolve(node[0], node[1] + 0x14)
    vals = p.resolve(node[0], node[1] + 0x1c)
    if not tbl or not vals or not _is_param_table(p, tbl):
        return False
    vb = p.pbase(vals[0]) + vals[1]
    tb = p.pbase(tbl[0]) + tbl[1]
    k = 0
    while k < 256:
        r = p.resolve(tbl[0], tbl[1] + 0x10 * k + 0xc)
        if not r:
            break
        try:
            name = p.pstr(*r)
        except Exception:
            break
        if not name or not name.isprintable():
            break
        idx, z, size, cls, kind, off = struct.unpack_from('>HHHBBI', p.d, tb + 0x10 * k)
        if kind == 2 and 0 < size <= 64 and size % 4 == 0 and off < 0x1000 and name not in out:
            out[name] = list(struct.unpack_from('>%df' % (size // 4), p.d, vb + off))
        k += 1
    return True


def material_params(p, mat_ptr):
    out = {}
    pl = p.resolve(mat_ptr[0], mat_ptr[1] + 0x10)
    if not pl:
        return out
    for k in range(8):
        ps = p.resolve(pl[0], pl[1] + 4 * k)
        if not ps:
            continue
        for e in range(32):
            if p.pu32(ps[0], ps[1] + 0x20 * e) != 0:
                break
            node = p.resolve(ps[0], ps[1] + 0x20 * e + 0x10)
            if not node:
                break
            _node_params(p, node, out)
    return out


def _walk(p, mat_ptr):
    """[(pass k, entry e, node)] of a material's 8 technique passes (runs of 0x20-byte entries)"""
    out = []
    pl = p.resolve(mat_ptr[0], mat_ptr[1] + 0x10)
    if not pl:
        return out
    for k in range(8):
        ps = p.resolve(pl[0], pl[1] + 4 * k)
        if not ps:
            continue
        for e in range(32):
            if p.pu32(ps[0], ps[1] + 0x20 * e) != 0:
                break
            node = p.resolve(ps[0], ps[1] + 0x20 * e + 0x10)
            if not node:
                break
            out.append((k, e, node))
    return out


OWNED = __import__('os').environ.get('U3_PS3PARAMS_LEAKY') != '1'


def all_material_params(p):
    """{ps3 material name: {param: [floats]}}
    agent Q (2026-10-29): a pass run is not terminated where the material's entries end, so the walk also reads the
    entries of neighbouring materials' passes and took their constants ('first value wins'): e.g. the collision debug
    material's magenta g_colorTint1 (0.276,0.04,0.276) landed on Museum floors, cardboard boxes and Oasis stucco.
    Ownership rule: a node reached at entry > 0 that is another material's entry-0 (own) node is skipped.  Checked
    against the retail exact twins (MP + SP paks): kept params 9893 present in the retail feature set / 122 not;
    dropped 7056 absent / 26 present.  U3_PS3PARAMS_LEAKY=1 gives the old reader."""
    mt = p.items_of('MATERIAL_TABLE_1')
    if not mt:
        return {}
    mt = mt[0]
    pg, b = mt.page, mt.data_off
    nmat = p.pu32(pg, b + 0xc)
    ma = p.resolve(pg, b + 0x20)
    mats = []
    for i in range(nmat):
        m = p.resolve(ma[0], ma[1] + 4 * i)
        mats.append((p.pstr(*p.resolve(*m)), m))
    if not OWNED:
        return {n: material_params(p, m) for n, m in mats}
    W = {}
    own = {}
    for n, m in mats:
        W[n] = _walk(p, m)
        for k, e, node in W[n]:
            if e == 0:
                own.setdefault(node, set()).add(n)
    res = {}
    for n, m in mats:
        out = {}
        for k, e, node in W[n]:
            if e > 0 and node in own and n not in own[node]:
                continue
            _node_params(p, node, out)
        res[n] = out
    return res
