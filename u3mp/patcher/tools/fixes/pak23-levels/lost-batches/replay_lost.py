"""replay_lost.py <pak name> <input pak> <output pak> [--unlocked]
Replays the lost cloud batches of 2026-10-29/30 on a pak in install order, md5-locked step by step
(SWAP.tsv / patches md5_before -> md5_after), from the earliest recorded state (= the converter's pre-batch state) to the
state the recipe's cloud-batches-r3 step starts from:
  batch98-water (in-place water layer; the 'copy' rows = full water rebuilds, not handled) | batch97-water-layer,
  batch99-lmuv (content rule = scanlm.py), batch100-blockers A+B+C (content rule = apply100.writes_for), revertB,
  batch101-detail / batch101m-museum (append101.build; data/*.bin = PS3 detail textures, game-derived),
  n012-museum-catchup (in-place + fade page), n014-ldn-tunnel-probes, n015-level-info, n015b-level-info.
n015r (London LEVEL_INFO revert) was NOT applied in the reference chain (the reference keeps 0x108/0x100/0x8): skipped.
A step whose md5_before does not match is reported and skipped (--unlocked: applied anyway by rule where one exists).
Prints the md5 after every step; exit 0 when every recorded step of this pak matched."""
import sys, os, json, csv, hashlib, struct, importlib.util
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import lostrules as LR
L = os.path.join(HERE, 'lost')
md5 = lambda b: hashlib.md5(b).hexdigest()
a = [x for x in sys.argv[1:] if not x.startswith('--')]; UNLOCKED = '--unlocked' in sys.argv
name, src, out = a[:3]; pak = name + '.pak'
d = bytearray(open(src, 'rb').read())
ok = True


def mod(path, nm):
    sp = importlib.util.spec_from_file_location(nm, path); m = importlib.util.module_from_spec(sp); sp.loader.exec_module(m); return m


def swaprow(batch):
    f = os.path.join(L, batch, 'SWAP.tsv')
    for r in csv.reader(open(f, encoding='utf-8'), delimiter='\t'):
        if r and os.path.basename(r[0] if r[0].endswith('.pak') else (r[1] if len(r) > 1 else '')) == pak: return r


def json_patch(batch, u32text=False):
    p = os.path.join(L, batch, 'patches', pak + '.json')
    if not os.path.exists(p): return None
    J = json.load(open(p))
    for v in J.get('variants', [J]):
        if md5(d) != v['md5_before']: continue
        for w in v['writes']:
            off, new = w[0], w[-1]
            b = struct.pack('<I', int(new, 16)) if u32text else bytes.fromhex(new)
            d[off:off + len(b)] = b
        return v['md5_after'], md5(d) == v['md5_after']
    return 'no variant for %s' % md5(d)[:8], False


def step(label, res):
    global ok
    if res is None: return
    if isinstance(res, tuple) and isinstance(res[0], str) and res[0].startswith('no variant'):
        print('  %-22s SKIP (%s)' % (label, res[0])); ok = False; return
    want, good = res
    print('  %-22s %s %s' % (label, md5(d)[:8], 'ok' if good else 'MISMATCH want %s' % want[:8])); ok &= good


def rule_step(batch, fn, before_col=2, after_col=3):
    r = swaprow(batch)
    if not r: return None
    if md5(d) != r[before_col] and not UNLOCKED: return ('no variant for %s (want %s)' % (md5(d)[:8], r[before_col][:8]), False)
    LR.apply(d, fn(d)); return r[after_col], md5(d) == r[after_col]


print(name, md5(d)[:8])
# water layer: batch98 supersedes batch97 (same in-place word where both exist)
r98 = json_patch('batch98-water', u32text=True)
step('batch98-water', r98) if r98 else step('batch97-water-layer', json_patch('batch97-water-layer', u32text=True))
step('batch99-lmuv (rule)', rule_step('batch99-lmuv', LR.rule99))
step('batch100 A+B+C (rule)', rule_step('batch100-blockers', lambda x: LR.rule100(x, 'ABC')))
RB = [r for r in json.load(open(os.path.join(L, 'batch100-blockers', 'revertB', 'revertB.json'))) if r[0] == pak]
if RB:
    r = RB[0]
    if md5(d) == r[2]:
        for off, old, new in r[4]: struct.pack_into('<I', d, off, int(new, 16))
        step('revertB', (r[3], md5(d) == r[3]))
    else: step('revertB', ('no variant for %s' % md5(d)[:8], False))
for b in ('batch101-detail', 'n012-museum-catchup', 'batch101m-museum'):
    if b == 'n012-museum-catchup':
        p = os.path.join(L, b, 'patches', pak + '.json')
        if not os.path.exists(p): continue
        J = json.load(open(p))
        if md5(d) != J['md5_before']: step(b, ('no variant for %s' % md5(d)[:8], False)); continue
        for o, old, new in J['writes_b92'] + J['writes_b96']: struct.pack_into('<I', d, o, int(new, 16))
        if 'fade' in J: d = bytearray(mod(os.path.join(L, b, 'apply_patches.py'), 'n012').build(bytes(d), J['fade']))
        step(b, (J['md5_after'], md5(d) == J['md5_after'])); continue
    r = swaprow(b)
    if not r: continue
    if md5(d) != r[2] and not UNLOCKED: step(b, ('no variant for %s' % md5(d)[:8], False)); continue
    m = mod(os.path.join(L, b, 'append101.py'), b.replace('-', '_'))
    D = json.load(open(os.path.join(L, b, 'data', 'targets.json')))
    data, rep = m.build(bytes(d), D['targets'][pak], D['textures'], os.path.join(L, b, 'data'))
    d = bytearray(data); step(b, (r[3], md5(d) == r[3]))
step('n014', json_patch('n014-ldn-tunnel-probes'))
for b in ('n015-level-info', 'n015b-level-info'):
    p = os.path.join(L, b, 'patches', pak + '.json')
    if os.path.exists(p):
        J = json.load(open(p))
        if md5(d) == J['md5_before']: step(b, json_patch(b))
        elif md5(d) != J['md5_after']: print('  %-22s not at its lock (%s), not applied' % (b, md5(d)[:8]))
open(out, 'wb').write(d)
print('final', md5(d), 'ALL STEPS OK' if ok else 'SOME STEPS FAILED')
sys.exit(0 if ok else 1)
