#!/usr/bin/env python3
"""End-to-end PS3 -> PS4 conversion of one background stage level (+ its -ingame12 package):
  1. mkplan  - which PS4 materials / effects / textures to reuse (ps4mats.jsonl.gz, ps4tex.tsv)
  2. fxextract (run as a subprocess against the PS4 pak23/ps4 directory) - the bundle
  3. ps4stage - the stage pak;  ps4ingame - the -ingame12 pak (NAV_MESH_1 / FEATURE_DB_2 converted, LEVEL_OFFSET_1 dropped)
  4. stagecheck - integrity check
usage: stagepipe.py <stage> --ps3 DIR --ps4 DIR --res DIR --work DIR --out DIR [--texdict CSV]
Writes <out>/<stage>.pak, <out>/<stage>-ingame12.pak and <work>/<stage>.{plan,req,bundle,report}.json"""
import argparse, json, os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))


def run(stage, ps3, ps4, res, work, out, texdict=None, force=False):
    if texdict:
        os.environ['U3_TEXDICT'] = texdict
    import mkplan, ps4stage, ps4ingame, stagecheck
    os.makedirs(work, exist_ok=True); os.makedirs(out, exist_ok=True)
    t0 = time.time()
    src = os.path.join(ps3, stage + '.pak')
    planf = os.path.join(work, stage + '.plan.json')
    reqf = os.path.join(work, stage + '.req.json')
    bundf = os.path.join(work, stage + '.bundle.json')
    if force or not os.path.exists(planf):
        pl = mkplan.plan(src, (), res)
        json.dump(pl, open(planf, 'w'), indent=1)
        req = dict(mats=pl['requests']['mats'], tex=pl['requests']['tex'],
                   techs=sorted({m[0] for m in pl['requests']['mats']}))
        json.dump(req, open(reqf, 'w'))
    pl = json.load(open(planf))
    if force or not os.path.exists(bundf):
        fx = os.path.join(HERE, 'fxextract.py')
        if not os.path.exists(fx):
            fx = os.path.join(os.path.dirname(HERE), 'fxextract.py')
        subprocess.check_call([sys.executable, fx, ps4, reqf, bundf + '.tmp'])
        os.replace(bundf + '.tmp', bundf)
    bundle = json.load(open(bundf))
    errs = [m for m in bundle['mats'] if 'err' in m or not m.get('rec')]
    if errs:
        raise SystemExit('bundle errors: %s' % errs[:3])
    outp = os.path.join(out, stage + '.pak')
    data, rep = ps4stage.convert(src, pl, bundle, outp, verbose=False)
    ig = os.path.join(ps3, stage + '-ingame12.pak')
    rep['ingame12'] = None
    if os.path.exists(ig):
        ps4ingame.IGNORE_OTHERS = True
        from ndpak import Pak
        dropped = [i.type for i in Pak(ig).items if i.type in (('LEVEL_OFFSET_1',) if ps4ingame.CONVERT_NAV else ('FEATURE_DB_2', 'NAV_MESH_1', 'LEVEL_OFFSET_1'))]
        ps4ingame.convert(ig, os.path.join(out, stage + '-ingame12.pak'))
        rep['ingame12'] = dict(dropped=dropped)
    e, info = stagecheck.check(outp, quiet=True)
    rep['check'] = e
    rep['seconds'] = round(time.time() - t0, 1)
    json.dump(rep, open(os.path.join(work, stage + '.report.json'), 'w'), indent=1)
    print('%s: %d bytes, check %s, %d approximations, %.0fs' % (stage, len(data), 'OK' if not e else e[:3],
                                                                len(rep['approx']), rep['seconds']))
    return rep


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('stage', nargs='+')
    for k in ('ps3', 'ps4', 'res', 'work', 'out'):
        ap.add_argument('--' + k, required=True)
    ap.add_argument('--texdict')
    ap.add_argument('--force', action='store_true')
    a = ap.parse_args()
    for s in a.stage:
        try:
            run(s, a.ps3, a.ps4, a.res, a.work, a.out, a.texdict, a.force)
        except BaseException as ex:
            print('%s: FAILED %r' % (s, ex))
