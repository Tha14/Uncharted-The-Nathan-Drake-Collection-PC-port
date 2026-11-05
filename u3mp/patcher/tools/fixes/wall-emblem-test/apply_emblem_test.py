#!/usr/bin/env python3
"""Decisive split for the Airstrip wall-emblem problem. Dry run unless --apply is given. Writes only into overlay-r3 and
work/backup-emblem-test. Needs only the stock python (no numpy); uses ndpak from the Downloads paktools.

  --cloth    variant A: the air-multi-emblem-ground spawner (sid ecd46935, pos -8.98 0.14 -4.77, next to the garage-2 rolling
             door) spawns the RETAIL lobby art `loading-menu-cloth` instead of the converted patch art. In-place edit of the
             art-name string inside pak23/ps4/air-multi-ingame12.pak, one `actor loading-menu-cloth` line in the air-multi block
             of pak23_ps4.txt, and a byte-identical copy of the retail loading-menu-cloth.pak into the overlay's actor33/ps4.
             Spawner name / sid / type / position / tags are untouched, so the helper (emb_wall119) still writes this object's
             overlay table.
  --proto30  variant B: in the OTHER 7 converted patch paks (not ground) set prototype +0x30 from 8 (mkemblem's floor) to 2
             (the value retail loading-menu-cloth and every surveyed retail NdSimpleObject carry; retail is only ever 2/3/4).
  --apply    actually write (otherwise prints the plan and pre-checks only)
  --restore  put the backups back and delete the copied cloth pak (with --apply; dry run otherwise)

Both variants may be applied in the same run: they touch disjoint patches (A = ground only, B = the other seven)."""
import hashlib, json, os, shutil, struct, sys
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = '{{PROJECT}}/'
MAIN = os.environ.get('EMBTEST_MAIN', ROOT + 'work/overlay-r3/u3data/build/main/')     # env overrides: self-test on a copy only
BAK = os.environ.get('EMBTEST_BAK', ROOT + 'work/backup-emblem-test/')
RETAIL_CLOTH = "{{PS4}}/u3data/build/main/actor33/ps4/loading-menu-cloth.pak"
PAKTOOLS = '{{PAKTOOLS}}'
sys.path.insert(0, PAKTOOLS)
from ndpak import Pak

INGAME = 'pak23/ps4/air-multi-ingame12.pak'
TXT = 'pak23_ps4.txt'
CLOTH_REL = 'actor33/ps4/loading-menu-cloth.pak'
GROUND = 'air-multi-emblem-ground'
GROUND_SID = 0xecd46935
NEW_ART = b'loading-menu-cloth'
OTHERS = ['air-multi-emblem-diamond', 'air-multi-emblem-corr', 'air-multi-emblem-corr-blue', 'air-multi-emblem-tower',
          'air-multi-emblem-stucco', 'air-multi-emblem-board-0', 'air-multi-emblem-board-1']
STATE = os.environ.get('EMBTEST_STATE', HERE + '/applied-state.json')

args = set(sys.argv[1:])
apply_, restore, cloth, proto30 = '--apply' in args, '--restore' in args, '--cloth' in args, '--proto30' in args
bad = args - {'--apply', '--restore', '--cloth', '--proto30'}
if bad:
    sys.exit('unknown flag(s): ' + ' '.join(sorted(bad)))
md5 = lambda f: hashlib.md5(open(f, 'rb').read()).hexdigest()


def backup(rel):
    dst = BAK + rel
    if os.path.exists(dst):
        sys.exit('backup already exists (restore first): ' + dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(MAIN + rel, dst)


# ---------------------------------------------------------------- restore
if restore:
    st = json.load(open(STATE)) if os.path.exists(STATE) else {}
    rels = st.get('backed_up', [])
    if not rels and os.path.isdir(BAK):
        for dp, _, fs in os.walk(BAK):
            rels += [os.path.relpath(os.path.join(dp, f), BAK).replace('\\', '/') for f in fs]
    for rel in rels:
        if not os.path.exists(BAK + rel):
            sys.exit('no backup: ' + BAK + rel)
        print('restore', rel, '(unchanged)' if md5(BAK + rel) == md5(MAIN + rel) else '')
        if apply_:
            shutil.copy2(BAK + rel, MAIN + rel)
            os.remove(BAK + rel)
    added = st.get('added', [])
    if not st and os.path.exists(MAIN + CLOTH_REL):      # no state file (apply died early): the cloth copy is still ours
        added = [CLOTH_REL]
    for rel in added:
        print('delete', rel)
        if apply_ and os.path.exists(MAIN + rel):
            os.remove(MAIN + rel)
    if apply_:
        for dp, ds, fs in os.walk(BAK, topdown=False):
            if not os.listdir(dp):
                os.rmdir(dp)
        if os.path.exists(STATE):
            os.remove(STATE)
        print('restored')
    else:
        print('dry run: nothing written (add --apply)')
    sys.exit(0)

if not (cloth or proto30):
    sys.exit('nothing selected: give --cloth and/or --proto30 (and --apply to write)')
if os.path.exists(STATE):
    sys.exit('already applied (%s exists); run --restore --apply first' % STATE)
state = dict(backed_up=[], added=[])
writes = []     # (rel, bytes)

# ---------------------------------------------------------------- variant A: cloth art on the ground spawner
if cloth:
    p = Pak(MAIN + INGAME)
    raw = bytearray(p.d)
    grp = p.items_of('SPAWNER_GROUP')
    if len(grp) != 1:
        sys.exit('expected one SPAWNER_GROUP, found %d' % len(grp))
    it = grp[0]
    arr, n = p.resolve(it.page, it.data_off + 8), p.pu32(it.page, it.data_off + 0x10)
    hit = None
    for k in range(n):
        s = p.resolve(arr[0], arr[1] + 8 * k)
        if p.pu32(s[0], s[1] + 0x28) == GROUND_SID:
            hit = s
    if hit is None:
        sys.exit('ground spawner sid %08x not found in %s' % (GROUND_SID, INGAME))
    sb = p.pbase(hit[0]) + hit[1]
    name = p.pstr(*p.resolve(hit[0], hit[1] + 0x20))
    art = p.resolve(hit[0], hit[1] + 0x48)
    artn = p.pstr(*art)
    typ = p.u32(sb + 0x40)
    pos = struct.unpack_from('<3f', raw, sb)
    if artn != GROUND or typ != 0x2db62a42 or name != GROUND + '-spawner':
        sys.exit('ground spawner unexpected: name=%r art=%r type=%08x' % (name, artn, typ))
    aoff = p.pbase(art[0]) + art[1]
    slot = len(GROUND) + 1
    if raw[aoff:aoff + slot] != GROUND.encode() + b'\0':
        sys.exit('art string bytes at file+%#x are not %r' % (aoff, GROUND))
    refs = [k for k in p.ptrmap if p.resolve(*k) == art]
    if len(refs) != 1 or refs[0] != (hit[0], hit[1] + 0x48):
        sys.exit('art string is referenced %d times (%r); refusing to edit in place' % (len(refs), refs))
    # the string is also searched as text: make sure the name is not referenced by any other string in the pak
    if raw.count(GROUND.encode() + b'\0') != 1:
        sys.exit('more than one %r string in the ingame pak' % GROUND)
    raw[aoff:aoff + slot] = NEW_ART + b'\0' * (slot - len(NEW_ART))
    q = Pak(bytes(raw))      # must still parse, and the spawner must now name the cloth
    s2 = [p.resolve(arr[0], arr[1] + 8 * k) for k in range(n)]
    chk = [q.pstr(*q.resolve(s[0], s[1] + 0x48)) for s in s2 if q.pu32(s[0], s[1] + 0x28) == GROUND_SID]
    if chk != [NEW_ART.decode()]:
        sys.exit('post-patch check failed: %r' % chk)
    print('A: %s: spawner %r sid %08x type %08x pos (%.2f %.2f %.2f): art %r -> %r (in place at file+%#x, %d-byte slot)'
          % (INGAME, name, GROUND_SID, typ, *pos, artn, NEW_ART.decode(), aoff, slot))
    writes.append((INGAME, bytes(raw)))

    txt = open(MAIN + TXT, 'rb').read()
    nl = b'\r\n' if b'\r\n' in txt else b'\n'
    head = b'level-name air-multi' + nl
    i = txt.find(head)
    nxt = txt.find(b'level-name ', i + len(head))
    anchor = b'actor air-multi-emblem-board-1' + nl
    j = txt.find(anchor, i)
    if i < 0 or j < 0 or (0 <= nxt < j):
        sys.exit('air-multi block / anchor not found in ' + TXT)
    block = txt[i:nxt if nxt >= 0 else len(txt)]
    line = b'actor ' + NEW_ART + nl
    if line in block:
        sys.exit('air-multi block already lists loading-menu-cloth')
    j += len(anchor)
    new_txt = txt[:j] + line + txt[j:]
    print('A: %s: insert %r after %r in the air-multi block (line %d)'
          % (TXT, line.strip().decode(), anchor.strip().decode(), txt[:j].count(nl) + 1))
    writes.append((TXT, new_txt))

    if not os.path.exists(RETAIL_CLOTH):
        sys.exit('retail cloth pak missing: ' + RETAIL_CLOTH)
    if os.path.exists(MAIN + CLOTH_REL):
        sys.exit('overlay already has ' + CLOTH_REL)
    Pak(RETAIL_CLOTH)        # parses
    print('A: copy retail loading-menu-cloth.pak (%d bytes, md5 %s) -> overlay %s'
          % (os.path.getsize(RETAIL_CLOTH), md5(RETAIL_CLOTH), CLOTH_REL))

# ---------------------------------------------------------------- variant B: prototype +0x30 = 2 on the other seven
if proto30:
    for a in OTHERS:
        rel = 'actor33/ps4/%s.pak' % a
        p = Pak(MAIN + rel)
        raw = bytearray(p.d)
        g = p.items_of('GEOMETRY_1')
        if len(g) != 1:
            sys.exit('%s: expected one GEOMETRY_1' % rel)
        pg, b = g[0].page, g[0].data_off
        npr = p.u64(p.pbase(pg) + b + 0x18)
        pr = p.resolve(pg, b + 0x20)
        if npr != 1:
            sys.exit('%s: %d prototypes' % (rel, npr))
        r = p.resolve(pr[0], pr[1])
        mb = p.pbase(r[0]) + r[1]
        if p.u32(mb) != 3 or p.u32(mb + 0xc) != 0x5791a421:
            sys.exit('%s: prototype header unexpected' % rel)
        pname = p.pstr(*p.resolve(r[0], r[1] + 0x10))
        v30, v34 = struct.unpack_from('<II', raw, mb + 0x30)
        if (v30, v34) != (8, 1):
            sys.exit('%s: prototype +0x30/+0x34 = %d/%d, expected 8/1' % (rel, v30, v34))
        struct.pack_into('<I', raw, mb + 0x30, 2)
        print('B: %s: %s +0x30 8 -> 2 (file+%#x)' % (rel, pname, mb + 0x30))
        writes.append((rel, bytes(raw)))

if not apply_:
    print('dry run: nothing written (add --apply). Backups would go to', BAK)
    sys.exit(0)

os.makedirs(BAK, exist_ok=True)
for rel, _ in writes:
    backup(rel)
    state['backed_up'].append(rel)
for rel, data in writes:
    open(MAIN + rel, 'wb').write(data)
if cloth:
    shutil.copy2(RETAIL_CLOTH, MAIN + CLOTH_REL)
    state['added'].append(CLOTH_REL)
state['md5_after'] = {rel: md5(MAIN + rel) for rel, _ in writes}
state['flags'] = sorted(args)
json.dump(state, open(STATE, 'w'), indent=1)
print('done; backups in', BAK, '; state in', STATE)
