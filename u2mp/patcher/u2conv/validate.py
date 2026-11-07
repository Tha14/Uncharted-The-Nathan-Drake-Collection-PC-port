"""Check the converters on files both platforms have (PS3 original -> converted, compared with the retail PS4 file).

  python validate.py --ps4 <PS4 game> --ps3 <PS3 base> [--ps3-dlc <DLC>] --work <dir> [--only xvag,light,...]

Checks (a JSON report; nothing is installed):
  xvag    the 4 PS-ADPCM music tracks retail PS4 kept: byte-identical (with the retail interleave)
  light   LIGHT_TABLE_1 of the shared MP levels: byte-identical (pointers compared as offsets)
  vis     VIS_INFO_1 of the shared MP levels: byte-identical
  ingame  shared -ingame10 / -npc-ingame10 paks: every item compared block by block (paktools cmpitems)
  skins   the 15 shared mp-hero skins, converted with a template other than themselves: ps4check, prototypes,
          submeshes (index counts and materials per submesh) against retail
  stage   shared sky / wide background paks: stagecheck, item types, submesh / texture counts against retail
  banks   banks both have: structure (sound / grain / name counts) against retail
"""
import argparse
import collections
import contextlib
import io
import json
import os
import struct
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import u2paktools  # noqa: E402

u2paktools.setup()
from ndpak import Pak  # noqa: E402

XVAG_PAIRS = ['146_JungleExplore_LOOP1', '148_JungleExplore_ENDTAG1', '149_JungleExplore_LOOP2',
              '150_JungleExplore_ENDTAG2']
LEVEL_PAIRS = ['village-multi-wide', 'gompa-multi-wide', 'sham-multi-wide', 'dig-multi', 'stage-multi']
INGAME_PAIRS = ['sky-village-multi-ingame10', 'gompa-multi-npc-ingame10', 'village-multi-npc-ingame10',
                'stage-multi-ingame10', 'treasure-multi-ingame10', 'dig-multi-ingame10']
STAGE_PAIRS = ['sky-village-multi', 'village-multi-wide', 'gompa-multi-wide', 'sham-multi-wide']
SKINS = ['chloe-variation', 'divesuit', 'eddie', 'elena', 'elena-divesuit', 'merc', 'navarro', 'pirate', 'red', 'roman',
         'skeleton-1-glow', 'skeleton-1-platinum', 'sullivan-blue', 'sullivan-red', 'sullivan-winter', 'udf']


def ps3_file(ps3, key, work):
    c = ps3.candidates(key)
    if not c:
        return None
    p = os.path.join(work, 'ps3', key.replace('/', '_'))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if not os.path.exists(p):
        with open(p, 'wb') as f:
            f.write(c[0].read())
    return p


def item_bytes(p, it):
    """item bytes with every pointer slot replaced by its target offset relative to the item"""
    b = bytearray(p.d[p.pbase(it.page) + it.data_off:p.pbase(it.page) + it.data_off + it.size])
    for o in range(0, it.size - 7, 8):
        r = p.resolve(it.page, it.data_off + o)
        if r is not None:
            b[o:o + 8] = (r[1] - it.data_off if r[0] == it.page else 0xfeed0000 + r[0]).to_bytes(8, 'little')
    return bytes(b)


def blob_bytes(blob):
    b = bytearray(blob.b)
    for pos, t, off in blob.relocs:
        r, o = t.root()
        b[pos:pos + 8] = (o + off).to_bytes(8, 'little')
    return bytes(b)


def check_xvag(ps3, ps4, work):
    import xvag
    out = {}
    for n in XVAG_PAIRS:
        src = ps3.candidates('sound1/music/%s.xvag' % n)
        ref = os.path.join(ps4.main, 'sound1', 'music', n + '.xvag')
        if not src or not os.path.exists(ref):
            out[n] = 'pair missing'
            continue
        r = open(ref, 'rb').read()
        il = struct.unpack_from('<I', r, 0x38)[0]
        out[n] = 'identical' if xvag.convert(src[0].read(), il) == r else 'DIFFERENT'
    return out


def check_tables(ps3, ps4, work, what):
    import u2light
    import u2vis
    out = {}
    for n in LEVEL_PAIRS:
        a = ps3_file(ps3, 'pak22/%s.pak' % n, work)
        ref = os.path.join(ps4.pak_dir, n + '.pak')
        if not a or not os.path.exists(ref):
            out[n] = 'pair missing'
            continue
        p, q = Pak(a), Pak(ref)
        t = 'LIGHT_TABLE_1' if what == 'light' else 'VIS_INFO_1'
        if not p.items_of(t):
            continue
        ib = q.items_of(t)[0]
        if what == 'light':
            blob = u2light.convert_light_table(p, None, True, {})
        else:
            bl = u2vis.vis_blobs(p)
            if len(bl) != 1:
                out[n] = 'split VIS item (not compared)'
                continue
            blob = bl[0]
        mine, theirs = blob_bytes(blob), item_bytes(q, ib)
        out[n] = 'identical (%d bytes)' % len(mine) if mine == theirs else 'DIFFERENT (%d vs %d bytes)' % (
            len(mine), len(theirs))
    return out


def check_ingame(ps3, ps4, work):
    import u2ingame
    import cmpitems
    out = {}
    for n in INGAME_PAIRS:
        a = ps3_file(ps3, 'pak22/%s.pak' % n, work)
        ref = os.path.join(ps4.pak_dir, n + '.pak')
        if not a or not os.path.exists(ref):
            out[n] = 'pair missing'
            continue
        dst = os.path.join(work, 'out', n + '.pak')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            notes = u2ingame.convert(a, dst, drop_navmesh=True)
        except Exception as e:
            out[n] = 'FAILED %s: %s' % (type(e).__name__, e)
            continue
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            cmpitems.cmp(dst, ref)
        lines = [l for l in buf.getvalue().splitlines() if not l.startswith('   ')]
        out[n] = dict(notes=notes, items=lines)
    return out


def submeshes(p):
    g = p.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    nsub = p.u64(p.pbase(pg) + b + 8)
    sr = p.resolve(pg, b + 0x10)
    nproto = p.u64(p.pbase(pg) + b + 0x18)
    subs = []
    for i in range(nsub):
        o = sr[1] + 0x88 * i
        nv, ni, ns = struct.unpack_from('<III', p.d, p.pbase(sr[0]) + o + 0x20)
        mat = p.pstr(*p.resolve(sr[0], o + 0x48)).split(',')[0]
        subs.append((ni, mat, nv))
    return nproto, subs


def check_skins(ps3, ps4, work):
    import u2actor
    out = {}
    tot = collections.Counter()
    for s in SKINS:
        name = 'mp-hero-%s' % s
        a = ps3_file(ps3, 'actor27/%s.pak' % name, work)
        ref = os.path.join(ps4.actor_dir, name + '.pak')
        if not a or not os.path.exists(ref):
            out[name] = 'pair missing'
            continue
        dst = os.path.join(work, 'out', name + '.pak')
        try:
            tmpl, shared, total = u2actor.choose_template(a, ps4.actor_dir, exclude=(name,))
            u2actor.convert(a, tmpl, dst)
            errs = u2actor.check(dst)
            np_a, sa = submeshes(Pak(dst))
            np_b, sb = submeshes(Pak(ref))
            ca = collections.Counter((ni, m) for ni, m, _ in sa)
            cb = collections.Counter((ni, m) for ni, m, _ in sb)
            same = sum((ca & cb).values())
            va = sum(nv for _, _, nv in sa)
            vb = sum(nv for _, _, nv in sb)
            out[name] = dict(template=os.path.basename(tmpl), check=errs or 'ok', prototypes='%d/%d' % (np_a, np_b),
                             submeshes_matching='%d of %d' % (same, len(sb)), submeshes='%d/%d' % (len(sa), len(sb)),
                             vertices='%d/%d' % (va, vb), size='%d/%d' % (os.path.getsize(dst), os.path.getsize(ref)))
            tot['skins'] += 1
            tot['sub_match'] += same
            tot['sub_retail'] += len(sb)
            tot['check_ok'] += not errs
        except Exception as e:
            out[name] = 'FAILED %s: %s' % (type(e).__name__, str(e)[:200])
    out['_total'] = dict(tot)
    return out


def check_stage(ps3, ps4, work):
    import u2level
    res = u2level.build_res(ps4.pak_dir, os.path.join(work, 'res'))
    out = {}
    for n in STAGE_PAIRS:
        a = ps3_file(ps3, 'pak22/%s.pak' % n, work)
        ref = os.path.join(ps4.pak_dir, n + '.pak')
        if not a or not os.path.exists(ref):
            out[n] = 'pair missing'
            continue
        dst = os.path.join(work, 'out', n + '.pak')
        try:
            k, notes = u2level.convert(a, dst, ps4.pak_dir, res, os.path.join(work, 'levels'))
            p, q = Pak(dst), Pak(ref)
            ta = collections.Counter(i.type for i in p.items if i.type != 'RAW_DATA')
            tb = collections.Counter(i.type for i in q.items if i.type != 'RAW_DATA')
            na, sa = submeshes(p)
            nb, sb = submeshes(q)
            out[n] = dict(notes=notes, item_types='same' if set(ta) == set(tb) else '%s vs %s' % (dict(ta), dict(tb)),
                          submeshes='%d/%d' % (len(sa), len(sb)),
                          indices='%d/%d' % (sum(x[0] for x in sa), sum(x[0] for x in sb)),
                          textures='%d/%d' % (ta['VRAM_DESC'], tb['VRAM_DESC']),
                          size='%d/%d' % (os.path.getsize(dst), os.path.getsize(ref)))
        except Exception as e:
            out[n] = 'FAILED %s: %s' % (type(e).__name__, str(e)[:300])
    return out


def check_banks(ps3, ps4, work, limit=40):
    import u2bank
    import bnkconv
    out = collections.Counter()
    ex = []
    ff = u2bank.find_ffmpeg()
    names = sorted(f for f in os.listdir(ps4.bank_dir) if f.endswith('.bnk'))
    for f in names[:limit]:
        c = ps3.candidates('sound1/banks/' + f)
        if not c:
            continue
        try:
            o, _ = u2bank.convert(c[0].read(), ffmpeg=ff)
        except NotImplementedError:
            out['needs ffmpeg'] += 1
            continue
        except Exception as e:
            out['failed'] += 1
            ex.append('%s: %s' % (f, e))
            continue
        r = open(os.path.join(ps4.bank_dir, f), 'rb').read()
        if o == r:
            out['identical'] += 1
            continue
        ho = struct.unpack_from('<16I', o, struct.unpack_from('<I', o, 8)[0])
        hr = struct.unpack_from('<16I', r, struct.unpack_from('<I', r, 8)[0])
        if ho[:2] == hr[:2] and ho[5:7] == hr[5:7]:
            out['same structure (sound/grain/name counts), different samples'] += 1
        else:
            out['different structure'] += 1
            ex.append('%s: %s vs %s' % (f, [hex(x) for x in ho[:8]], [hex(x) for x in hr[:8]]))
    return dict(counts=dict(out), examples=ex[:5])


CHECKS = dict(xvag=check_xvag, light=lambda a, b, w: check_tables(a, b, w, 'light'),
              vis=lambda a, b, w: check_tables(a, b, w, 'vis'), ingame=check_ingame, skins=check_skins,
              stage=check_stage, banks=check_banks)


def run(ps3, ps4, work, only=None, log=print):
    os.makedirs(work, exist_ok=True)
    rep = {}
    for k, fn in CHECKS.items():
        if only and k not in only:
            continue
        log('validate: %s ...' % k)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                rep[k] = fn(ps3, ps4, work)
        except Exception as e:
            rep[k] = 'FAILED %s: %s' % (type(e).__name__, traceback.format_exc()[-600:])
        log('validate: %s: %s' % (k, json.dumps(rep[k])[:600]))
    with open(os.path.join(work, 'validation.json'), 'w', encoding='utf-8') as f:
        json.dump(rep, f, indent=1)
    return rep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ps4', required=True)
    ap.add_argument('--ps3', required=True)
    ap.add_argument('--ps3-dlc')
    ap.add_argument('--work', required=True)
    ap.add_argument('--only')
    a = ap.parse_args()
    import sources
    rep = run(sources.PS3(a.ps3, a.ps3_dlc), sources.PS4(a.ps4), a.work,
              only=set(a.only.split(',')) if a.only else None)
    print(json.dumps(rep, indent=1)[:20000])


if __name__ == '__main__':
    main()
