#!/usr/bin/env python3
"""fidelity.py <map> [...]  |  fidelity.py --all
1:1 audit of a converted map against its PS3 source (MAPS-A: geometry + object completeness).
Per level of the map (mapsets.plan rows):
  * presence: the PS4 pak exists (release-maps-3 / retail PS4);
  * BG levels: prototypes, instances, triangles (LOD0 groups, as the converter keeps), VIS_INFO_1 and
    COLLISION_DATA_HAVOK_BG presence: PS3 vs PS4;
  * ingame12: spawner count and names, spawner art groups (tag e07cb817) that resolve to no actor pak,
    item types present on PS3 but missing on PS4;
  * pak23_ps4.txt: actor / symbol lines of the level dropped vs PS3 1.19.
FAIL = any PS3 object without a PS4 counterpart.  Output: rows + JSON (fidelity/<map>.json)."""
import sys, os, json, glob, struct, re, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, 'stage'))
import mapsets
from ndpak import Pak
from dc import sid
REL = '/mnt/user-data/outputs/release-maps-3/u3data/build/main/pak23/ps4'
RETAIL = '/mnt/user-data/uploads/Uncharted - The Nathan Drake Collection [CUSA02320]/u3data/build/main/pak23/ps4'
STAGED = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/maps/ps3'
OUT = '/mnt/user-data/outputs/fidelity'
P23 = '/mnt/user-data/outputs/dc3/u3data/build/main/pak23_ps4.txt'
P3TXT = '/home/claude/u3/ps3pkg/tree/dlc-patch18/build/main/pak23.txt'
INV = '/mnt/user-data/uploads/Uncharted The Nathan Drake Collection - U3 Multiplayer/work/inventory/ps4-files.tsv'
ART = 0xe07cb817
SHARED = {'multi-all', 'coop-all', 'coop-shade-all'}
_actors = None


def actor_sids():
    global _actors
    if _actors is None:
        names = set()
        for l in open(INV):
            p = l.split('\t')[0]
            if '/actor33/' in p and p.endswith('.pak'):
                names.add(os.path.basename(p)[:-4])
        for f in glob.glob('/mnt/user-data/outputs/**/actor33/**/*.pak', recursive=True):
            names.add(os.path.basename(f)[:-4])
        _actors = {sid(n): n for n in names}
    return _actors


def ps3_src(key, name, s):
    if s and s[0] == 'tree':
        return s[1]
    for d in (os.path.join(STAGED, key), os.path.join(STAGED, '*')):
        g = glob.glob(os.path.join(d, name + '.pak'))
        if g:
            return g[0]
    g = glob.glob('/home/claude/u3/ps3pkg/tree/*/build/main/pak23/' + name + '.pak')
    return sorted(g)[-1] if g else None


_retail = None


def retail_names():
    global _retail
    if _retail is None:
        _retail = set()
        for l in open(INV):
            p = l.split('\t')[0]
            if '/pak23/ps4/' in p and p.endswith('.pak'):
                _retail.add(os.path.basename(p)[:-4])
    return _retail


def ps4_path(name):
    """PS4 pak of a level: release-maps-3, else retail (path when the retail file is in the container, the
    string 'retail' when only the PS4 inventory lists it), else None"""
    for d in (REL, RETAIL):
        p = os.path.join(d, name + '.pak')
        if os.path.exists(p):
            return p
    return 'retail' if name in retail_names() else None


def bg_counts_ps3(p):
    import bggeom
    from ps4stage import lod0_groups
    C = bggeom.collection(p)
    tris = 0; insts = 0
    for c in C:
        pg, po = c['hdr']; hw0 = struct.unpack_from('>I', p.d, p.pbase(pg) + po)[0]
        keep = set()
        for mn, first, cnt in lod0_groups(c['groups'], hw0 >> 24):
            keep.update(range(first, first + cnt))
        for bi, (bpg, blk) in enumerate(c['blocks']):
            if bi in keep:
                ii = p.resolve(bpg, blk + 4); tris += struct.unpack_from('>H', p.d, p.pbase(ii[0]) + ii[1])[0]
        insts += len(c['insts'])
    return dict(protos=len(C), insts=insts, tris=tris)


def bg_counts_ps4(q):
    from stagecmp import protos_insts
    from ps4bg import geometry
    P, I = protos_insts(q); G = geometry(q)
    return dict(protos=len(P), insts=len(I), tris=sum(s['ni'] // 3 for s in G['subs']))


def spawners_of(p):
    """[(spawner name, art group name or None)]; art = string pointer at +0x34 (PS3) / +0x48 (PS4)"""
    from sg import spawners, spawner_info
    out = []
    ao = 0x34 if p.ptrsize == 4 else 0x48
    for it, (pg, o) in spawners(p):
        try:
            i = spawner_info(p, (pg, o))
        except Exception:
            continue
        art = None
        r = p.resolve(pg, o + ao)
        if r:
            try:
                s = p.pstr(*r)
                if s and all(32 < ord(c) < 127 for c in s):
                    art = s
            except Exception:
                pass
        out.append((i['name'], art))
    return out


def pak23_blocks(path):
    t = open(path, 'rb').read().decode('latin1').replace('\r', '')
    out = {}
    for b in re.split(r'\n(?=level-name )', t):
        m = re.search(r'^level-id (\S+)', b, re.M)
        if m:
            out[m.group(1)] = [x.strip() for x in b.split('\n') if x.strip()]
    return out


_p23 = None


def audit(key):
    global _p23
    if _p23 is None:
        _p23 = (pak23_blocks(P23), pak23_blocks(P3TXT))
    b4, b3 = _p23
    acts = actor_sids()
    pl = mapsets.plan(key)
    rows = []; fails = []
    for r in pl['rows']:
        lv = r['level']
        if lv in SHARED:
            continue
        row = dict(level=lv)
        # ---- main level
        q4 = ps4_path(lv); s3 = ps3_src(key, lv, r['src'])
        row['ps4'] = bool(q4); row['ps3'] = bool(s3)
        if not q4:
            fails.append('%s: no PS4 pak' % lv)
        if q4 and q4 != 'retail' and s3:
            try:
                p3 = Pak(s3); p4 = Pak(q4)
                t3 = {i.type for i in p3.items}; t4 = {i.type for i in p4.items}
                if 'PM_COLLECTION_2' in t3:
                    c3 = bg_counts_ps3(p3); c4 = bg_counts_ps4(p4)
                    row['bg'] = dict(ps3=c3, ps4=c4)
                    for k in c3:
                        if c3[k] != c4[k]:
                            fails.append('%s: BG %s %d -> %d' % (lv, k, c3[k], c4[k]))
                for ty in ('VIS_INFO_1', 'COLLISION_DATA_HAVOK_BG', 'LIGHT_TABLE_1', 'WATER_DATA', 'CAMERA_TABLE_1'):
                    if ty in t3 and ty not in t4:
                        fails.append('%s: %s missing on PS4' % (lv, ty))
            except Exception as ex:
                row['error'] = repr(ex)[:200]; fails.append('%s: audit error %r' % (lv, ex))
        # ---- ingame12
        if r['ingame']:
            i4 = ps4_path(lv + '-ingame12'); i3 = ps3_src(key, lv + '-ingame12', r['src_ig'])
            if not i4:
                fails.append('%s-ingame12: no PS4 pak' % lv)
            if i4 and i4 != 'retail' and i3:
                try:
                    p3 = Pak(i3); p4 = Pak(i4)
                    s3 = spawners_of(p3); s4 = spawners_of(p4)
                    n3 = collections.Counter(n for n, a in s3); n4 = collections.Counter(n for n, a in s4)
                    lost = list((n3 - n4).elements())
                    unres = sorted({a for n, a in s4 if a and sid(a) not in acts})
                    row['spawners'] = dict(ps3=len(s3), ps4=len(s4), lost=len(lost), art_unresolved=len(unres))
                    if lost:
                        fails.append('%s-ingame12: %d spawners lost (%s)' % (lv, len(lost), ', '.join(lost[:5])))
                    if unres:
                        fails.append('%s-ingame12: %d art groups without actor (%s)' % (lv, len(unres), ' '.join(unres[:5])))
                    t3 = collections.Counter(i.type for i in p3.items); t4 = collections.Counter(i.type for i in p4.items)
                    miss = sorted(set(t3) - set(t4) - {'VRAM_DESC_TABLE', 'PAK_LOGIN_TABLE'})
                    if miss:
                        row['ig_types_missing'] = miss; fails.append('%s-ingame12: item types dropped %s' % (lv, miss))
                except Exception as ex:
                    row['ig_error'] = repr(ex)[:200]; fails.append('%s-ingame12: audit error %r' % (lv, ex))
        # ---- pak23 block
        if lv in b3 and lv in b4 and not r['on_ps4']:
            l3 = {x for x in b3[lv] if x.split()[0] in ('actor', 'symbol')}
            l4 = {x for x in b4[lv] if x.split()[0] in ('actor', 'symbol')}
            dropped = sorted(l3 - l4)
            if dropped:
                row['pak23_dropped'] = dropped
                acts_d = [x for x in dropped if x.startswith('actor')]
                if acts_d:
                    fails.append('%s: pak23 actors dropped %s' % (lv, ' '.join(x.split()[1] for x in acts_d[:6])))
        rows.append(row)
    os.makedirs(OUT, exist_ok=True)
    res = dict(map=key, rows=rows, fails=fails)
    json.dump(res, open(os.path.join(OUT, key + '.json'), 'w'), indent=1)
    return res


ALL = ['ice-cave', 'gompa', 'shambhala', 'warzone', 'temple', 'train', 'village', 'airport', 'cha', 'desert', 'facility',
       'highrise', 'ldn', 'museum', 'ruins', 'san', 'syria', 'yemen', 'graveyard', 'oasis', 'oldquarter', 'streets', 'fort',
       'drydocks1', 'coop-fort', 'coop-air', 'coop-dig', 'coop-ldn', 'coop-mon', 'coop-syria']

if __name__ == '__main__':
    keys = ALL if sys.argv[1] == '--all' else sys.argv[1:]
    for k in keys:
        r = audit(k)
        print('== %s: %d fails' % (k, len(r['fails'])), flush=True)
        for f in r['fails'][:25]:
            print('   ', f)
