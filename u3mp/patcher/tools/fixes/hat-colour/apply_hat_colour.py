#!/usr/bin/env python3
"""Install the rebuilt head-wear paks from out/ (listed in manifest.json) into overlay-r3. Standard library only.
  (no flags)  dry run: prints what would change, writes nothing
  --apply     backs up the current overlay-r3 copies to work/backup-hat-colour, then installs
  --restore   puts the backups back; a pak overlay-r3 did not have before is removed again
  --only A,B  limit to these actor names (dry run / apply / restore)
--work DIR overrides the work folder (to test this script away from the real overlay)."""
import argparse, hashlib, json, os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_WORK = os.path.abspath(os.path.join(HERE, '..', '..', 'work'))
REL = 'overlay-r3/u3data/build/main/actor33/ps4'
SRC = os.path.join(HERE, 'out')


def sha(f):
    with open(f, 'rb') as fh:
        return hashlib.sha256(fh.read()).hexdigest()


a = argparse.ArgumentParser()
a.add_argument('--apply', action='store_true'); a.add_argument('--restore', action='store_true'); a.add_argument('--work', default=DEFAULT_WORK); a.add_argument('--only', default='')
o = a.parse_args()
if o.apply and o.restore:
    sys.exit('use --apply or --restore, not both')
DST = os.path.join(o.work, REL); BAK = os.path.join(o.work, 'backup-hat-colour'); STATE = os.path.join(BAK, 'state.json')
if not os.path.isdir(DST):
    sys.exit('missing ' + DST)
MAN = json.load(open(os.path.join(HERE, 'manifest.json')))
only = {x.strip() + '.pak' for x in o.only.split(',') if x.strip()}
if only:
    MAN = {n: h for n, h in MAN.items() if n in only}
    if not MAN:
        sys.exit('--only names nothing in manifest.json')

if o.restore:
    if not os.path.exists(STATE):
        sys.exit('nothing to restore: no ' + STATE)
    st = json.load(open(STATE))
    for n, had in st.items():
        if n not in MAN:
            continue
        d, b = os.path.join(DST, n), os.path.join(BAK, n)
        if had:
            if not os.path.exists(b):
                sys.exit('missing backup ' + b)
            shutil.copy2(b, d)
            assert sha(d) == sha(b)
            print(n, 'restored')
        elif os.path.exists(d):
            if sha(d) != MAN[n]:
                print(n, 'NOT removed: overlay-r3 copy is not the one this script installed'); continue
            os.remove(d); print(n, 'removed from overlay-r3 (it was not there before)')
    print('restore done; backups kept in', BAK)
    sys.exit(0)

st = json.load(open(STATE)) if os.path.exists(STATE) else {}
for n, h in MAN.items():
    s, d, b = os.path.join(SRC, n), os.path.join(DST, n), os.path.join(BAK, n)
    if not os.path.exists(s) or sha(s) != h:
        sys.exit('out/ pak missing or changed since manifest: ' + n)
    cur = sha(d)[:16] if os.path.exists(d) else 'not in overlay-r3 (overlay-port copy is used now)'
    print(f'{n}: {cur} -> {h[:16]} ({os.path.getsize(s)} bytes)')
    if not o.apply:
        continue
    os.makedirs(BAK, exist_ok=True)
    if n not in st:                     # the first apply records the original; a later one must not overwrite it
        st[n] = os.path.exists(d)
        if st[n]:
            shutil.copy2(d, b)
        with open(STATE, 'w') as f:
            json.dump(st, f, indent=0)
    shutil.copy2(s, d)
    assert sha(d) == h
print('installed' if o.apply else 'dry run: no change')
