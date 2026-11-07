#!/usr/bin/env python3
"""apply_london.py - London (and every other map) look fixes for overlay-r3.

 1. fade records (bug fix, default): every background prototype flagged "fade with distance" gets its PS3 fade
    distances back; without them the PS4 engine never draws it (fadefix.py).  Applies to every stage pak in
    overlay-r3 whose PS3 source is found (London: ldn-train-multi, -lit, -tunnel, -tunnel-2 and the coop-ldn-*).
 2. sun (bug fix, default): ldn-train-multi.pak / -lit.pak sun intensity back to the PS3 / retail value 886.71
    (an earlier workaround set it to 5; retail ldn-train-stage ships 886.71 with the same render settings).
 3. --instflags (judgement call, off by default): PS3 instance flag bits 16..19 -> PS4 instance flag bytes = 0
    (the retail ldn-train-stage rule, not yet seen in game).

usage: apply_london.py [--maps ldn|all] [--instflags] [--out DIR] [--apply] [--restore] [--check]
  default      dry run: reports per pak, writes nothing
  --out DIR    dry run that also writes the rebuilt paks to DIR (for crashcheck / ps4check)
  --apply      back up to work/backup-london (never overwritten), then write overlay-r3
  --restore    copy the backups back (with --apply); refuses when the game runs
  --check      run crashcheck + ps4check on the written paks (--out or --apply)
  --maps ldn   London paks only (default: all)"""
import hashlib, json, os, shutil, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from llib import *
import fadefix
BK = W + 'backup-london/'
SUN = {'ldn-train-multi.pak': 0x2a848a8, 'ldn-train-multi-lit.pak': 0x53e208}
SUN_PS3 = 886.71
PY = sys.executable
md5 = lambda b: hashlib.md5(b).hexdigest()


def game_running():
    return any(l.split()[:1] == ['uncharted.exe'] for l in subprocess.run(['tasklist'], capture_output=True).stdout.decode('ascii', 'replace').lower().splitlines())


# paks whose overlay-r3 copy already fails crashcheck before any change here (check-all.log); left alone unless --include-failing
SKIP = {'dig-coop-approach.pak'}


def targets(maps, include_failing=False):
    out = []
    for f in sorted(os.listdir(OV)):
        if not f.endswith('.pak') or f.endswith('-ingame12.pak') or f.endswith('-killvis.pak'): continue
        if maps == 'ldn' and not (f.startswith('ldn-') or f.startswith('coop-ldn')): continue
        if f in SKIP and not include_failing: print('%-45s skipped: fails crashcheck already (use --include-failing)' % f); continue
        out.append(f)
    return out


def ps3_source(f):
    h = ps3_find(f)
    return h[0] if h else None


def sun_fix(raw, f):
    """-> (new bytes, old value) or (raw, None)"""
    if f not in SUN: return raw, None
    o = SUN[f]
    q = Pak(raw); lt = q.items_of('LIGHT_TABLE_1')[0]
    sun = q.resolve(lt.page, lt.data_off + 0x48)
    at = q.pbase(sun[0]) + sun[1] + 0x38
    v = struct.unpack_from('<f', raw, at)[0]
    if abs(v - SUN_PS3) < 0.01: return raw, None
    b = bytearray(raw); struct.pack_into('<f', b, at, SUN_PS3)
    return bytes(b), v


def main():
    a = sys.argv[1:]
    maps = a[a.index('--maps') + 1] if '--maps' in a else 'all'
    out = a[a.index('--out') + 1] if '--out' in a else None
    apply_ = '--apply' in a; inst = '--instflags' in a; check = '--check' in a
    if '--restore' in a:
        if apply_ and game_running(): sys.exit('game running: stop it first')
        if not os.path.exists(BK + 'manifest.json'): sys.exit('no backups in ' + BK)
        man = json.load(open(BK + 'manifest.json'))
        for f, m in man.items():
            if not os.path.exists(BK + f): print('no backup', f); continue
            if md5(open(BK + f, 'rb').read()) != m['md5_before']: sys.exit('backup md5 differs: ' + f)
            print('restore', f)
            if apply_: shutil.copy2(BK + f, OV + f)
        print('restored' if apply_ else 'dry run: nothing written (use --restore --apply)'); return
    if apply_ and game_running(): sys.exit('game running: stop it first')
    if out: os.makedirs(out, exist_ok=True)
    written = []; manifest = json.load(open(BK + 'manifest.json')) if os.path.exists(BK + 'manifest.json') else {}
    for f in targets(maps, '--include-failing' in a):
        raw = open(OV + f, 'rb').read()
        try: q = Pak(raw)
        except Exception as e: print('%-45s unreadable %r' % (f, e)); continue
        need, pr = fadefix.scan(q)
        new = raw; notes = []
        if need or inst:
            src = ps3_source(f)
            if src is None:
                print('%-45s %d fade-flagged prototypes without record, NO PS3 SOURCE - skipped' % (f, len(need))); continue
            p3 = Pak(ps3_read(f))
            try:
                data, rep = fadefix.fix(OV + f, None, inst_flags=inst, ps3_pak=p3)
            except Exception as e:
                print('%-45s FAILED %r' % (f, e)); continue
            if rep['mism']:
                print('%-45s prototype mismatch against PS3 %s (%s) - skipped' % (f, src[0], rep['mism'][:5])); continue
            if data is not None:
                new = data
                notes.append('fade records added %d/%d (PS3 %s)' % (rep['fixed'], rep['flagged'], src[0]))
                if inst and rep.get('inst_flags'): notes.append('instance flag words changed %d' % rep['inst_flags'])
        new, oldsun = sun_fix(new, f)
        if oldsun is not None: notes.append('sun %.4g -> %.2f' % (oldsun, SUN_PS3))
        if new is raw or new == raw:
            print('%-45s ok (nothing to do)' % f); continue
        print('%-45s %s  (%d -> %d bytes)' % (f, '; '.join(notes), len(raw), len(new)))
        if out:
            open(os.path.join(out, f), 'wb').write(new); written.append(os.path.join(out, f))
        if apply_:
            os.makedirs(BK, exist_ok=True)
            if not os.path.exists(BK + f):
                shutil.copy2(OV + f, BK + f)
                manifest[f] = dict(md5_before=md5(raw))
            elif md5(open(BK + f, 'rb').read()) != manifest.get(f, {}).get('md5_before'):
                sys.exit('existing backup of %s does not match the manifest' % f)
            open(OV + f, 'wb').write(new); written.append(OV + f)
            manifest[f]['md5_after'] = md5(new); manifest[f]['notes'] = notes
            json.dump(manifest, open(BK + 'manifest.json', 'w'), indent=1)
    if not out and not apply_:
        print('dry run: nothing written (use --out DIR or --apply)')
    if check and written:
        print('== crashcheck'); subprocess.run([PY, PT + '/crashcheck.py'] + written)
        print('== ps4check'); subprocess.run([PY, PT + '/ps4check.py'] + written)


if __name__ == '__main__':
    main()
