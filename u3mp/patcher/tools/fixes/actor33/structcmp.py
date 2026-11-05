"""Structural comparison of a rebuilt actor pak with its reference (for the 'equivalent' class):
skeleton (JOINT_HIERARCHY count/joints), FG sections, submesh count + vertex/index totals, material names,
texture names, item-type counts, crashcheck errors, ps4check error count.
usage: structcmp.py <paktools(f525859)> <built.pak> <reference.pak>  -> one json line"""
import sys, os, json, io, contextlib, collections, hashlib, struct
T = sys.argv[1]; sys.path[:0] = [T, T + '/stage']
from ndpak import Pak
import ps4actor, crashcheck, ps4check


def sig(path):
    q = Pak(path)
    d = {}
    d['types'] = dict(collections.Counter(i.type for i in q.items if i.type not in ('RAW_DATA', 'VRAM_DESC')))
    d['fg'] = sorted(i.name for i in q.items_of('FOREGROUND_SECTION_2'))
    try:
        _, _, names, nj = ps4actor.tmpl_fg_info(q); d['joints'] = (nj, hashlib.sha1(names).hexdigest()[:8])
    except Exception:
        d['joints'] = None
    jh = q.items_of('JOINT_HIERARCHY'); d['jh'] = [i.size for i in jh]
    g = q.items_of('GEOMETRY_1')
    if g:
        g = g[0]; pg, b = g.page, g.data_off; r = q.resolve(pg, b + 0x10); n = q.pu32(pg, b + 8); nv = ni = 0; subs = []
        for k in range(n):
            o = r[1] + 0x88 * k; nv += q.pu32(r[0], o + 0x20); ni += q.pu32(r[0], o + 0x24)
            subs.append(q.pstr(*q.resolve(r[0], o + 8)))
        d['geo'] = (n, nv, ni); d['subs'] = sorted(subs)
        d['mats'] = sorted(m['name'].split(',')[0] for m in ps4actor.tmpl_materials(q))
    d['tex'] = sorted(t['path'].split('texture2/')[-1] for t in q.textures())
    with contextlib.redirect_stdout(io.StringIO()):
        cc = crashcheck.check(path); cc = cc[0] if isinstance(cc, tuple) else cc
        e, _, _ = ps4check.check(path, quiet=True)
    d['crash'] = len(cc or [])
    d['check'] = len([x for x in e if not x.startswith('unknown item type') and not (x.startswith('tiled size') and 'env-reflection' in x)])
    return d


a, b = sig(sys.argv[2]), sig(sys.argv[3])
same = {k: a.get(k) == b.get(k) for k in ('types', 'fg', 'joints', 'jh', 'geo', 'subs', 'mats', 'tex')}
print(json.dumps(dict(same=same, crash=(a['crash'], b['crash']), check=(a['check'], b['check']),
                      equivalent=all(same[k] for k in ('fg', 'joints', 'jh', 'geo', 'subs', 'mats')) and a['crash'] == 0 and a['check'] <= b['check'],
                      tex_diff=sorted(set(a['tex']) ^ set(b['tex']))[:6])))
