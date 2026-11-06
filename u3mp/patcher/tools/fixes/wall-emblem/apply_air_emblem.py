#!/usr/bin/env python3
"""Install the air-multi wall emblem prototype into overlay-r3. Dry run unless --apply is given. NOT run by its author.
Backs up every overwritten file to work/backup-wall-emblem/<relative path> (never overwrites an existing backup), and refuses
if air-multi.pak or air-multi-ingame12.pak changed since the prototype was built (it was built from those exact files).
The helper C (helper_paste.inc) is pasted and built separately; without it the patches show no emblem.
Undo: --restore copies the backups back and deletes the 8 actor paks."""
import hashlib, json, os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = '{{PROJECT}}/'
MAIN = ROOT + 'work/overlay-r3/u3data/build/main/'
BAK = ROOT + 'work/backup-wall-emblem/'
OUT = HERE + '/out/'
plan = json.load(open(OUT + 'spawn-plan.json'))
base = json.load(open(HERE + '/base-md5.json'))
apply_, restore = '--apply' in sys.argv, '--restore' in sys.argv
md5 = lambda f: hashlib.md5(open(f, 'rb').read()).hexdigest()
arts = [a['art'] for a in plan['actors']]
files = ['pak23/ps4/air-multi.pak', 'pak23/ps4/air-multi-ingame12.pak', 'pak23_ps4.txt']


def backup(rel):
    dst = BAK + rel
    if os.path.exists(dst):
        sys.exit('backup already exists: ' + dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(MAIN + rel, dst)


if restore:
    for rel in files:
        print('restore', rel)
        if not os.path.exists(BAK + rel):
            sys.exit('no backup: ' + BAK + rel)
        if md5(BAK + rel) != md5(MAIN + rel) and rel.endswith('air-multi.pak'):
            print('  note: installed air-multi.pak differs from the backup; later edits to it would be lost')
        if apply_:
            shutil.copy2(BAK + rel, MAIN + rel)
    for a in arts:
        print('delete actor', a)
        if apply_ and os.path.exists(MAIN + 'actor33/ps4/%s.pak' % a):
            os.remove(MAIN + 'actor33/ps4/%s.pak' % a)
    print('restored' if apply_ else 'dry run: nothing written (use --apply)')
    sys.exit(0)
txt = open(MAIN + 'pak23_ps4.txt', 'rb').read()
# A clean recipe run (2026-11-06): registry-pak23 already wrote the emblem actor lines, but no emblem pak is installed
# yet; that is not "installed": install the paks and keep the registry as it is.
lines_only = b'air-multi-emblem' in txt and not any(os.path.exists(MAIN + 'actor33/ps4/%s.pak' % a) for a in arts)
if b'air-multi-emblem' in txt and not lines_only:
    # The spawners and actors need no correction; the fix is the helper bucket (helper_paste.inc).
    ok = all(os.path.exists(MAIN + 'actor33/ps4/%s.pak' % a) and md5(MAIN + 'actor33/ps4/%s.pak' % a) == md5(OUT + 'actor33/ps4/%s.pak' % a) for a in arts)
    ig = md5(MAIN + files[1]) == md5(OUT + files[1])
    mp = open(MAIN + files[0], 'rb').read()
    hid = all(not any(mp[h['off']:h['off'] + 2 * h['ni']]) for h in plan['hide'])
    print('already installed: actors match %s, ingame12 matches %s, background patches hidden %s' % (ok, ig, hid))
    print('nothing to write; air-multi.pak is left alone (it carries a later edit). Paste helper_paste.inc and rebuild.')
    sys.exit(0 if ok and ig and hid else 1)
for rel in files[:2]:
    if md5(MAIN + rel) != base[rel]:
        sys.exit('changed since build, rebuild the prototype first: ' + rel)
nl = b'\r\n' if b'\r\n' in txt else b'\n'
head = b'level-name air-multi' + nl
i = txt.find(head)
anchor = b'actor air-multi-wind-socks' + nl
j = txt.find(anchor, i)
nxt = txt.find(b'level-name ', i + len(head))
if i < 0 or j < 0 or (0 <= nxt < j):
    sys.exit('air-multi level block not found')
j += len(anchor)
new = txt if lines_only else txt[:j] + b''.join(b'actor ' + a.encode() + nl for a in arts) + txt[j:]
for a in arts:
    src, dst = OUT + 'actor33/ps4/%s.pak' % a, MAIN + 'actor33/ps4/%s.pak' % a
    print('install', os.path.basename(dst), os.path.getsize(src))
    if os.path.exists(dst):
        sys.exit('actor exists: ' + dst)
print('install 2 level paks, ' + ('the %d actor lines are already in pak23_ps4.txt' if lines_only else
                                  'add %d actor lines to the air-multi block') % len(arts))
if not apply_:
    print('dry run: nothing written (use --apply)')
    sys.exit(0)
for rel in files:
    backup(rel)
for a in arts:
    shutil.copy2(OUT + 'actor33/ps4/%s.pak' % a, MAIN + 'actor33/ps4/%s.pak' % a)
for rel in files[:2]:
    shutil.copy2(OUT + rel, MAIN + rel)
open(MAIN + 'pak23_ps4.txt', 'wb').write(new)
print('done; backups in', BAK)
