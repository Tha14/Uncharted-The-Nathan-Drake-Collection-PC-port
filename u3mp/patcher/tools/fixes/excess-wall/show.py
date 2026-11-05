import json, sys
H = json.load(open('det/%s.json' % sys.argv[1]))
for h in H:
    rows = h['rows']; print(h['pak'], len(rows), 'area', round(sum(r['area'] * r['fex'] for r in rows), 1))
    w = [r for r in rows if abs(r['c'][0] - 31.64) < .1 and 11 < r['c'][2] < 14]
    if w:
        print(' wall', [(r['sub'], r['t'], r['fex'], r['fhole']) for r in w])
    for r in sorted(rows, key=lambda r: -r['area'] * r['fex'])[:int(sys.argv[2]) if len(sys.argv) > 2 else 15]:
        print('  ', r['sub'], r['mat'][:26], r['c'], r['area'], r['fex'], r['fhole'], r['ny'], r['uses'])
