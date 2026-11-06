"""Convert one PS3 level pak with one paktools version (run in its own process so versions never mix).
usage: convert_one.py <paktools> <method> <ps3 pak> <out pak> <res dir|-> <PS4 pak23/ps4 dir> <plan dir> [--mapbuild]
  stage     mkplan.plan -> fxextract (subprocess) -> ps4stage.convert [--mapbuild: + paramfix.fix + cubefix.fix,
            as mapbuild.cmd_build]; options come from the environment (U3_STAGE_*, U3_COLL_FILTER1, MKPLAN_*)
  ingame    ps4ingame.convert with IGNORE_OTHERS = True (mapbuild / mapb / stagepipe setting)
  vischeat  vischeat.convert (VIS_CHEAT-only killvis levels, mapb.conv_main_tree)"""
import json
import os
import subprocess
import sys

PT, M, SP, OP, RES, PS4, PD = sys.argv[1:8]
MB = '--mapbuild' in sys.argv
sys.path[:0] = [PT, os.path.join(PT, 'stage')]
os.makedirs(os.path.dirname(OP), exist_ok=True)
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import fastpaths   # noqa: E402  exact faster _enc_box / _bits_be for the converter versions that have the originals
fastpaths.install()
if M == 'ingame':
    import ps4ingame
    ps4ingame.IGNORE_OTHERS = True
    ps4ingame.convert(SP, OP)
elif M == 'vischeat':
    import vischeat
    vischeat.convert(SP, OP)
elif M == 'stage':
    import mkplan
    import ps4stage
    lv = os.path.basename(OP)[:-4]
    os.makedirs(PD, exist_ok=True)
    P = mkplan.plan(SP, (), RES, verbose=False) if 'verbose' in mkplan.plan.__code__.co_varnames else mkplan.plan(SP, (), RES)
    reqf, bf = os.path.join(PD, lv + '.req.json'), os.path.join(PD, lv + '.bundle.json')
    json.dump(P, open(os.path.join(PD, lv + '.plan.json'), 'w'), indent=1)
    json.dump(dict(mats=P['requests']['mats'], tex=P['requests']['tex'], techs=sorted({m[0] for m in P['requests']['mats']})),
              open(reqf, 'w'))
    subprocess.check_call([sys.executable, os.path.join(PT, 'stage', 'fxextract.py'), PS4, reqf, bf],
                          env=dict(os.environ, PYTHONPATH=PT + os.pathsep + os.path.join(PT, 'stage')), stdout=subprocess.DEVNULL)
    B = json.load(open(bf))
    bad = [m for m in B['mats'] if 'err' in m or not m.get('rec')]
    if bad:
        raise SystemExit('bundle errors %s' % bad[:2])
    ps4stage.convert(SP, P, B, OP, verbose=False)
    if MB:
        import paramfix
        import cubefix
        paramfix.fix(OP, P)
        cubefix.fix(OP, SP)
else:
    raise SystemExit('unknown method ' + M)
print(OP, os.path.getsize(OP))
