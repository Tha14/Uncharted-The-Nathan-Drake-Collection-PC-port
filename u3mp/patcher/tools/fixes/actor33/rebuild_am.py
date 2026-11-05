#!/usr/bin/env python3
"""Deterministic rebuild of the actor-materials and hat-colour paks from the recorded jobs lists
(no template search, no gates against the current overlay): each pak is rebuilt with the template and
texture cap the session's search picked, then checked against the recorded sha256.

  rebuild_am.py am   [names...]   jobs-am.tsv   -> ../actor-materials/out/<name>.pak  (build.py --one / mkretail rename)
  rebuild_am.py hats [names...]   jobs-hats.tsv -> ../hat-colour/out/<name>.pak      (build_hat.build)
  rebuild_am.py igc2 [names...]   jobs-igc2.tsv -> ../airport-igc2/out/<name>.pak    (its own build_hat.build)
jobs columns: name, PS3 source (informational: lib.S3 picks the newest), template, cap, sha256, [options]
option no-overlay-filter: build_hat without the OVERLAY/UV_3 template filter (added after that pak was built)
Run from the rendered job folder (<project>/scratch/actor-rebuild); actor-materials and hat-colour must be
rendered next to it. Exit 1 if any pak differs from its recorded hash. Idempotent: a pak already at its
recorded hash is not rebuilt."""
import hashlib, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SCR = os.path.dirname(HERE)
AM = os.path.join(SCR, 'actor-materials')
HC = os.path.join(SCR, 'hat-colour')
KINDS = {'hats': ('jobs-hats.tsv', 'hat-colour'), 'igc2': ('jobs-igc2.tsv', 'airport-igc2')}


def sha(f):
    return hashlib.sha256(open(f, 'rb').read()).hexdigest() if os.path.exists(f) else None


def jobs(fn):
    for l in open(os.path.join(HERE, fn)):
        if l.strip() and not l.startswith('#'):
            yield l.rstrip('\n').split('\t')


def retail_rename(n, t):
    """mkretail.py without its gates: ps4rename + VRAM gap removal + cb3fix"""
    sys.path.insert(0, AM)
    os.chdir(AM)
    from lib import Pak, RET, T
    import ps4rename
    sys.path.insert(0, T + '/stage')
    import cb3fix
    dst = os.path.join(AM, 'out', n + '.pak'); tmp = dst + '.tmp'
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    ps4rename.rename_pak(f'{RET}/{t}.pak', tmp, t, n, verbose=False)
    q_ = Pak(tmp); end = q_.pages[-1][0] + q_.pages[-1][1]
    if end != q_.vram_base:
        raw = open(tmp, 'rb').read(); open(tmp, 'wb').write(raw[:end] + raw[q_.vram_base:])
    cb3fix.fix(tmp, tmp)
    os.replace(tmp, dst)
    return dst


def main():
    kind = sys.argv[1]
    only = set(sys.argv[2:])
    bad = 0; t0 = time.time(); n_ok = n_built = 0
    if kind == 'am':
        rows = list(jobs('jobs-am.tsv')); outd = os.path.join(AM, 'out')
    else:
        jf, jd = KINDS[kind]
        HCK = os.path.join(SCR, jd)
        rows = list(jobs(jf)); outd = os.path.join(HCK, 'out')
    for r in rows:
        name, src, tmpl, cap, h = r[:5]
        if only and name not in only:
            continue
        dst = os.path.join(outd, name + '.pak')
        if sha(dst) == h:
            n_ok += 1; continue
        n_built += 1
        if kind == 'am' and src == 'retail':
            retail_rename(name, tmpl)
        elif kind == 'am':
            p = subprocess.run([sys.executable, '-B', 'build.py', '--one', name, tmpl, cap], cwd=AM, capture_output=True, text=True)
            if p.returncode:
                print('FAIL', name, (p.stdout + p.stderr).strip()[-300:]); bad += 1; continue
        else:
            # -I as batch_hats.py ran it: hat-colour/dis.py must not shadow the standard library, so the job
            # folder goes at the END of sys.path
            code = ('import sys; sys.path.append(%r); sys.argv = ["build_hat.py"]; import build_hat; '
                    'build_hat.build(%r, %r, %d, verbose=False)' % (HCK, name, tmpl, int(cap)))
            env = dict(os.environ)
            if len(r) > 5 and 'no-overlay-filter' in r[5].split(','):
                env['HAT_NO_OVERLAY_FILTER'] = '1'
            p = subprocess.run([sys.executable, '-I', '-B', '-c', code], cwd=HCK, capture_output=True, text=True, env=env)
            if p.returncode:
                print('FAIL', name, (p.stdout + p.stderr).strip()[-300:]); bad += 1; continue
        got = sha(dst)
        print(('OK  ' if got == h else 'DIFF') + ' %s <- %s cap %s %s' % (name, tmpl, cap, '' if got == h else got[:16] + ' != ' + h[:16]), flush=True)
        if got != h:
            bad += 1
    print('%s: %d already at the recorded hash, %d rebuilt, %d differ, %.0f s' % (kind, n_ok, n_built, bad, time.time() - t0))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
