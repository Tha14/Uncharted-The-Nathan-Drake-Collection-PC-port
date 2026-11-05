#!/usr/bin/env python3
"""Re-check every out/*.pak (crashcheck 0, ps4check not worse than the pak the game loads now, ps4verify clean, same
joints and geometry) and write manifest.json with the sha256 of the ones that pass; the others are moved to rejected/."""
import sys, os, json, hashlib
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE); sys.dont_write_bytecode = True
from build_hat import gate
os.chdir(HERE)
man = {}; rej = []
for fn in sorted(os.listdir('out')):
    if not fn.endswith('.pak'):
        continue
    n = fn[:-4]; bad, info = gate(n, 'out/' + fn, verbose=False)
    gained = [k for k, v in info['tint_new'].items() if v[0] and v[1]]   # the gate checks tint against the PS3 masks
    if bad:
        rej.append((n, bad)); os.makedirs('rejected', exist_ok=True)
        for x in (fn, n + '.json'):
            if os.path.exists('out/' + x): os.replace('out/' + x, 'rejected/' + x)
        print('REJECT', n, bad); continue
    man[fn] = hashlib.sha256(open('out/' + fn, 'rb').read()).hexdigest()
    print('ok', n, info['layer'], 'check %d->%d crash %d->%d vram %#x->%#x' % (*info['check'], *info['crash'], *info['vram']), [g.split('/')[-1] for g in gained])
json.dump(man, open('manifest.json', 'w'), indent=0)
print('manifest', len(man), 'rejected', len(rej))
