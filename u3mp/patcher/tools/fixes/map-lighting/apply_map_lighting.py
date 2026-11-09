#!/usr/bin/env python3
"""apply_map_lighting.py (job map-lighting): multiplayer map lighting fixes, in place, no size change.

1. Sun shadow blockers (every overlay-r3 pak23 level).
   A blocker instance-submesh (material name contains shadowblocker, ik-only or canopy-shadow) must be
   shadow-only: +0x20 |= 0x10000 (ik-only: 0x1010000) and +0x24 |= 0x101 (the retail hidden-blocker word,
   38/38 lib-shadowblocker in retail). Converted paks had +0x20 = 0 on some of them, so the blocker went to
   the colour pass (invisible) and never into the sun-shadow pass: the sun floods interiors. London Underground
   library (ldn-train-multi-lit inst 51): bright cream floor, dark walls, hard light shafts; confirmed in game
   (2026-11-09) that this restores the PS3 look. Same words as batch100 class B, which had been reverted
   without a London test. 19 paks: air-multi(-shade), cave, fort, highrise, museum, shambhala, village(-stage),
   warzone(-stage), yemen, ldn-train-multi-lit and the coop syria / dig / fort-coop-tower paks.

2. Fort (fort-multi, fort-multi-stage), from the PS3 NV40 fragment programs:
   * ground/stone-steps: PS3 draws the stone layer (the blend factor saturates), the PS4 template shows layer 2,
     which held the wet grass colour + crumple-wet normal (chrome-like stairs). Colour1/Colour2 and
     Normal1/Normal2 binds swapped; env strength 0.05 and sun specular 0 (PS3: no sun specular term,
     env * 0.6 * vertex light), template metalness 0.85 -> 0 (PS3 has no metalness).
   * fort-mlti-wall-plaster-cream(-emblem): PS3 multiplies the colour by g_diffuseColorBorderTint
     (0.349, 0.335, 0.311); the PS4 template has no border tint and shows layer 2 (streak x 0.708).
     Colour2 bound to the cream texture, g_colorTint2 = the PS3 border tint.
   * fort-mlti-symbols, dig-ship-barrel, fort-mlti-wall-plaster-cream-emblem (stage): metalness 0.85 -> 0.

usage: apply_map_lighting.py [--work <work dir>] [--apply]
Dry run by default. Idempotent: values already set are left alone. Backup of every pak it changes:
<work>/backup-map-lighting/<pak> (an existing backup is never overwritten)."""
import os, sys, glob, struct, shutil, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
for p in (os.environ.get('PAKTOOLS', ''), os.environ.get('RECIPE_TRANSFER_TOOLS', '')):
    if p and p not in sys.path: sys.path.insert(0, p)
from ndpak import Pak

PAK23 = os.path.join('overlay-r3', 'u3data', 'build', 'main', 'pak23', 'ps4')
BORDER_TINT = (0.34935078, 0.33464769, 0.31090671)


def geometry(q):
    its = q.items_of('GEOMETRY_1')
    return its[0] if its else None


# ---------------------------------------------------------------- 1. shadow blockers
def blocker_writes(q):
    g = geometry(q)
    if not g: return []
    pg, b = g.page, g.data_off
    il = q.resolve(pg, b + 0x30); n = q.u64(q.pbase(pg) + b + 0x28)
    if not il: return []
    nsub = q.u64(q.pbase(pg) + b + 8); sref = q.resolve(pg, b + 0x10); mat = {}
    for k in range(nsub):
        o = sref[1] + 0x88 * k; nm = q.pstr(*q.resolve(sref[0], o + 8)); mr = q.resolve(sref[0], o + 0x48)
        mat[nm] = (q.pstr(*mr) if mr else '').split(',')[0]
    W = []
    for i in range(n):
        r = q.resolve(il[0], il[1] + 8 * i); ns = q.pu32(r[0], r[1] + 0x20); sr = q.resolve(r[0], r[1] + 0x30)
        if not sr: continue
        for k in range(ns):
            o = sr[1] + 0x50 * k; ab = q.pbase(sr[0]) + o
            m = mat.get(q.pstr(*q.resolve(sr[0], o)), '')
            if not ('shadowblocker' in m or 'ik-only' in m or 'canopy-shadow' in m): continue
            f, f2 = struct.unpack_from('<II', q.d, ab + 0x20)
            n20 = f | (0x1010000 if 'ik-only' in m else 0x10000) if not f & 0x10000 else f
            if n20 != f: W.append((ab + 0x20, f, n20, m))
            if f2 | 0x101 != f2: W.append((ab + 0x24, f2, f2 | 0x101, m))
    return W


# ---------------------------------------------------------------- 2. Fort materials
def materials(q):
    """name -> {'params': {pname: file offset of the value}, 'binds': {bname: file offset of the bind record},
    'bindpath': {bname: path}}"""
    g = geometry(q); out = {}
    if not g: return out
    pg, b = g.page, g.data_off; n = q.pu32(pg, b + 0x48); r = q.resolve(pg, b + 0x50)
    if not r: return out
    mp, mo = r
    for i in range(n):
        o = mo + i * 0x48; nm = q.pstr(*q.resolve(mp, o)).split(',')[0]
        nf, npar, nb, _x3 = struct.unpack_from('<4I', q.d, q.pbase(mp) + o + 0x30)
        P, B, BP = {}, {}, {}
        pr = q.resolve(mp, o + 0x20)
        for k in range(npar if pr else 0):
            po = pr[1] + 0x18 * k; vr = q.resolve(pr[0], po + 8)
            if vr: P[q.pstr(*q.resolve(pr[0], po))] = q.pbase(vr[0]) + vr[1]
        br = q.resolve(mp, o + 0x28)
        for k in range(nb if br else 0):
            bo = br[1] + 0x28 * k; bn = q.pstr(*q.resolve(br[0], bo))
            B[bn] = q.pbase(br[0]) + bo; fr = q.resolve(br[0], bo + 0x10); BP[bn] = q.pstr(*fr) if fr else ''
        out[nm] = dict(params=P, binds=B, bindpath=BP)
    return out


def value_users(M):
    u = {}
    for nm, m in M.items():
        for pn, a in m['params'].items(): u.setdefault(a, []).append((nm, pn))
    return u


def fort_edits(q, pak):
    """list of (description, offset, old bytes, new bytes)"""
    M = materials(q); users = value_users(M); E = []

    def find(suffix):
        hits = [nm for nm in M if nm.endswith(suffix)]
        return hits[0] if len(hits) == 1 else None

    def setf(nm, pn, vals):
        if nm is None or pn not in M[nm]['params']: return
        a = M[nm]['params'][pn]
        if len(users[a]) != 1: return                       # shared value block: leave it
        old = bytes(q.d[a:a + 4 * len(vals)]); new = struct.pack('<%df' % len(vals), *vals)
        if old != new: E.append(('%s %s' % (nm.split('/')[-1], pn), a, old, new))

    def bind_copy(nm, dst, src):                            # dst bind -> src bind's texture (short + full path)
        B = M[nm]['binds']; d0, s0 = B[dst], B[src]
        old = bytes(q.d[d0 + 8:d0 + 0x18]); new = bytes(q.d[s0 + 8:s0 + 0x18])
        if old != new: E.append(('%s %s -> %s texture' % (nm.split('/')[-1], dst, src), d0 + 8, old, new))

    steps = find('ground/stone-steps:default')
    if steps:
        BP = M[steps]['bindpath']
        if 'stone-step-color' not in BP.get('g_tNdFetchColor2Map', ''):   # not yet swapped
            B = M[steps]['binds']
            for a, c in (('g_tNdFetchColor1Map', 'g_tNdFetchColor2Map'), ('g_tNdFetchNormal1Map', 'g_tNdFetchNormal2Map')):
                x, y = B[a], B[c]; ox, oy = bytes(q.d[x + 8:x + 0x18]), bytes(q.d[y + 8:y + 0x18])
                E.append(('stone-steps %s <-> %s' % (a, c), x + 8, ox, oy)); E.append(('', y + 8, oy, ox))
        setf(steps, 'g_specularEnvironmentStrength', [0.05])
        setf(steps, 'g_specularBrightness', [0.0])
        setf(steps, 'g_specularMetalness', [0.0])
    for suffix in ('fort/fort-mlti-wall-plaster-cream:default', 'fort/fort-mlti-wall-plaster-cream-emblem:default'):
        nm = find(suffix)
        if nm and 'g_diffuseColorBorderTint' not in M[nm]['params'] \
                and 'g_colorTint2' in M[nm]['params']:
            bind_copy(nm, 'g_tNdFetchColor2Map', 'g_tNdFetchColor1Map')
            setf(nm, 'g_colorTint2', list(BORDER_TINT))
    if pak == 'fort-multi-stage.pak':
        for suffix in ('fort/fort-mlti-wall-plaster-cream-emblem:default', 'fort/fort-mlti-symbols:default', 'wood/dig-ship-barrel:default'):
            setf(find(suffix), 'g_specularMetalness', [0.0])
    return E


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', default=os.environ.get('RECIPE_WORK', "{{WORK}}"))
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    root = os.path.join(a.work, PAK23); bak = os.path.join(a.work, 'backup-map-lighting')
    if not os.path.isdir(root): sys.exit('no %s' % root)
    changed = 0
    for path in sorted(glob.glob(os.path.join(root, '*.pak'))):
        name = os.path.basename(path)
        try:
            q = Pak(open(path, 'rb').read())
            W = [(o, struct.pack('<I', old), struct.pack('<I', new), 'blocker ' + m) for o, old, new, m in blocker_writes(q)]
            if name in ('fort-multi.pak', 'fort-multi-stage.pak'):
                W += [(o, old, new, desc) for desc, o, old, new in fort_edits(q, name)]
        except Exception as e:
            print('skip (unreadable)', name, e); continue
        if not W: continue
        changed += 1
        print('%s %s: %d writes' % ('patch' if a.apply else 'would patch', name, len(W)))
        for o, old, new, desc in W:
            if desc: print('   %#x %s' % (o, desc))
        if not a.apply: continue
        os.makedirs(bak, exist_ok=True)
        if not os.path.exists(os.path.join(bak, name)): shutil.copyfile(path, os.path.join(bak, name))
        d = bytearray(q.d)
        for o, old, new, _ in W:
            if bytes(d[o:o + len(old)]) != old: sys.exit('unexpected bytes in %s at %#x' % (name, o))
            d[o:o + len(new)] = new
        open(path, 'wb').write(d)
    print('%d paks %s' % (changed, 'patched' if a.apply else 'to patch') if changed else 'already applied (nothing to change)')


if __name__ == '__main__':
    main()
