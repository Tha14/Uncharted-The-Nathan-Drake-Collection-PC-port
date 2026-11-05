"""device-side extractor: PS4 material records + EFFECT closures + VRAM_DESC/texels from PS4 paks.
usage: fxextract.py <pak dir> <requests.json> <out.json>
requests: {"mats": [[pak, "dir/file:variant"], ...], "tex": [[pak, fullpath], ...], "techs": [pak, ...]}"""
import sys, os, json, struct, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak

D, req, outp = sys.argv[1], json.load(open(sys.argv[2])), sys.argv[3]
cache = {}
def P(fn):
    if fn not in cache:
        if len(cache) > 6: cache.clear()
        q = Pak(os.path.join(D, fn))
        idx = {}
        for it in q.items:
            idx.setdefault(it.page, []).append(it)
        for k in idx: idx[k].sort(key=lambda i: i.off)
        q._idx = idx
        cache[fn] = q
    return cache[fn]

def container(q, pg, off):
    L = q._idx.get(pg, [])
    offs = [i.off for i in L]
    k = bisect.bisect_right(offs, off) - 1
    if k >= 0 and L[k].off <= off < L[k].data_off + L[k].size:
        return L[k]
    return None

def S(q, pg, off):
    r = q.resolve(pg, off)
    return q.pstr(*r) if r else None

def techs(q):
    et = q.items_of('EFFECT_TABLE')[0]
    t = q.resolve(et.page, et.data_off + 0x18)
    n = q.pu32(*t)
    return [q.pstr(*q.resolve(t[0], t[1] + 8 + 8 * i)) for i in range(n)]

def closure(q, eit):
    seq = [eit]; ids = {(eit.page, eit.off): 0}; k = 0; out = []
    while k < len(seq):
        it = seq[k]; k += 1
        ptrs = []
        for oo in range(it.data_off, it.data_off + it.size, 8):
            r = q.resolve(it.page, oo)
            if r:
                c = container(q, *r)
                if c is None:
                    raise ValueError('loose pointer')
                if (c.page, c.off) not in ids:
                    ids[(c.page, c.off)] = len(seq); seq.append(c)
                ptrs.append((oo - it.data_off, ids[(c.page, c.off)], r[1] - c.data_off))
        b = q.pbase(it.page)
        out.append(dict(type=it.type, name=it.name, off=it.data_off, page=it.page,
                        data=q.d[b + it.data_off:b + it.data_off + it.size].hex(), ptrs=ptrs))
    return out

def material(q, key):
    g = q.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    n = q.u64(q.pbase(pg) + b + 0x48)
    mp, mo = q.resolve(pg, b + 0x50)
    for i in range(n):
        o = mo + i * 0x48
        name = S(q, mp, o)
        if name.split(',')[0] != key:
            continue
        nf, npar, nb, x3 = struct.unpack_from('<4I', q.d, q.pbase(mp) + o + 0x30)
        tail = struct.unpack_from('<2I', q.d, q.pbase(mp) + o + 0x40)
        fr = q.resolve(mp, o + 0x10)
        feats = [q.pstr(*q.resolve(fr[0], fr[1] + 8 * k)) for k in range(nf if fr else 0)]
        params = []
        pr = q.resolve(mp, o + 0x20)
        for k in range(npar if pr else 0):
            po = pr[1] + 0x18 * k
            vr = q.resolve(pr[0], po + 8)
            cnt, x = struct.unpack_from('<II', q.d, q.pbase(pr[0]) + po + 0x10)
            vb = q.pbase(vr[0]) + vr[1]
            params.append([S(q, pr[0], po), cnt, x, q.d[vb:vb + 4 * max(1, cnt)].hex()])
        binds = []
        br = q.resolve(mp, o + 0x28)
        for k in range(nb if br else 0):
            bo = br[1] + 0x28 * k
            binds.append([S(q, br[0], bo), S(q, br[0], bo + 8), S(q, br[0], bo + 0x10),
                          q.d[q.pbase(br[0]) + bo + 0x18:q.pbase(br[0]) + bo + 0x28].hex()])
        rec = dict(name=name, shader=S(q, mp, o + 8), hash=S(q, mp, o + 0x18), feats=feats,
                   params=params, binds=binds, x3=x3, tail=list(tail),
                   raw=q.d[q.pbase(mp) + o:q.pbase(mp) + o + 0x48].hex())
        # effect
        et = q.items_of('EFFECT_TABLE')[0]
        ne = q.pu32(et.page, et.data_off)
        arr = q.resolve(et.page, et.data_off + 8)
        h = int(rec['hash'], 16)
        for k in range(ne):
            eo = arr[1] + 0x18 * k
            if q.u64(q.pbase(arr[0]) + eo + 0x10) == h:
                ep = q.resolve(arr[0], eo)
                eit = container(q, *ep)
                rec['fx_entry'] = [q.pu32(arr[0], eo + 8), q.pu32(arr[0], eo + 12), ep[1] - eit.data_off]
                rec['fx'] = closure(q, eit)
                break
        # first submesh using it: stream signature
        nsub = q.u64(q.pbase(pg) + b + 8); sr = q.resolve(pg, b + 0x10)
        for si in range(nsub):
            so = sr[1] + 0x88 * si
            if S(q, sr[0], so + 0x48) != name:
                continue
            sig = []
            for which, cnt_off in ((0x30, 0x28), (0x38, 0x2c)):
                ns = q.pu32(sr[0], so + cnt_off); st = q.resolve(sr[0], so + which)
                lst = []
                for kk in range(ns if st else 0):
                    e = st[1] + 0x18 * kk
                    f = q.resolve(st[0], e + 8); fb = q.pbase(f[0]) + f[1]
                    lst.append([q.d[fb], q.pu32(st[0], e) >> 16, q.d[fb + 3]])
                sig.append(lst)
            rec['streams'] = sig
            rec['subraw'] = q.d[q.pbase(sr[0]) + so:q.pbase(sr[0]) + so + 0x88].hex()
            break
        # textures bound by the material: VRAM_DESC (+ resident texels) and GEOMETRY_1 texture entry
        if not hasattr(q, '_tex'):
            q._tex = {t['path']: t for t in q.textures()}
            gt = {}
            nt = q.u64(q.pbase(pg) + b + 0x38); tr = q.resolve(pg, b + 0x40)
            for k in range(nt if tr else 0):
                to = tr[1] + 0x28 * k
                gt[S(q, tr[0], to)] = q.d[q.pbase(tr[0]) + to + 8:q.pbase(tr[0]) + to + 0x28].hex()
            q._gtex = gt
        texs = {}
        for bnd in binds:
            full = bnd[2]
            t = q._tex.get(full + '.ndb') or q._tex.get(full)
            e = dict(gtex=q._gtex.get(full))
            if t is not None and 'LightMap' not in bnd[0] and t['vramSize'] <= 0x80000:
                tb = q.pbase(t['page']) + t['off'] + 0x20
                e['path'] = t['path']
                e['desc'] = q.d[tb:tb + 0x40].hex()
                e['texels'] = q.vram_bytes(t['vramOffset'], t['vramSize']).hex()
            texs[full] = e
        rec['textures'] = texs
        return rec
    return None

out = dict(mats=[], tex=[], techs={})
for fn in req.get('techs', []):
    out['techs'][fn] = techs(P(fn))
for fn, key in req.get('mats', []):
    try:
        q = P(fn)
        r = material(q, key)
        out['mats'].append(dict(pak=fn, key=key, rec=r, techs=None))
        if fn not in out['techs']:
            out['techs'][fn] = techs(q)
    except Exception as ex:
        out['mats'].append(dict(pak=fn, key=key, err=repr(ex)))
for fn, path in req.get('tex', []):
    try:
        q = P(fn)
        if not hasattr(q, '_tex2'):
            q._tex2 = {t['path']: t for t in q.textures()}
        t = q._tex2[path]
        b = q.pbase(t['page']) + t['off'] + 0x20
        out['tex'].append(dict(pak=fn, path=path, desc=q.d[b:b + 0x40].hex(),
                               texels=q.vram_bytes(t['vramOffset'], t['vramSize']).hex()))
    except Exception as ex:
        out['tex'].append(dict(pak=fn, path=path, err=repr(ex)))
json.dump(out, open(outp, 'w'))
print('ok', len(out['mats']), len(out['tex']))
