"""verify_output.py - compare a rebuilt output folder with manifest.tsv (sha256 of the reference overlays).
usage: python verify_output.py <out folder> [--report <file.tsv>]
Prints per overlay/family: files in the manifest, matched (identical), differing, missing, and extra files not in the manifest.
"""
import collections
import os
import sys

import recipe_lib as R


def main():
    out = os.path.abspath(sys.argv[1])
    report = sys.argv[sys.argv.index('--report') + 1] if '--report' in sys.argv else os.path.join(out, '_recipe', 'verify.tsv')
    man = list(R.read_tsv(os.path.join(R.HERE, 'manifest.tsv')))
    prov = {(r['overlay'], r['path']): r['class'] for r in R.read_tsv(os.path.join(R.HERE, 'provenance.tsv'))} if os.path.exists(os.path.join(R.HERE, 'provenance.tsv')) else {}
    hidden = R.shadowed_files(man)
    stats = collections.defaultdict(lambda: collections.Counter())
    rows = []
    known = set()
    for m in man:
        fam = m['overlay'] + '/' + '/'.join(m['path'].split('/')[2:4])
        p = os.path.join(out, 'work', m['overlay'], m['path'])
        known.add(os.path.normcase(p))
        if not os.path.exists(p):
            st = 'hidden' if (m['overlay'], m['path']) in hidden else 'missing'
        else:
            st = 'match' if R.sha256_file(p) == m['sha256'] else 'differs'
        stats[fam][st] += 1
        cls = prov.get((m['overlay'], m['path']), '')
        stats['class:' + cls][st] += 1
        rows.append((m['overlay'], m['path'], st, cls))
    extra = []
    for ov in R.OVERLAYS:
        base = os.path.join(out, 'work', ov)
        for dp, dn, fn in os.walk(base):
            for f in fn:
                p = os.path.join(dp, f)
                if os.path.normcase(p) not in known:
                    extra.append(os.path.relpath(p, os.path.join(out, 'work')).replace('\\', '/'))
    os.makedirs(os.path.dirname(report), exist_ok=True)
    with open(report, 'w', encoding='utf-8') as f:
        f.write('overlay\tpath\tstatus\tprovenance_class\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
        for e in extra:
            f.write('%s\t%s\textra\t\n' % (e.split('/')[0], '/'.join(e.split('/')[1:])))
    tot = collections.Counter()
    print('%-50s %7s %7s %7s %7s %7s' % ('family', 'files', 'match', 'differ', 'missing', 'hidden'))
    for fam in sorted(stats):
        c = stats[fam]
        n = sum(c.values())
        print('%-50s %7d %7d %7d %7d %7d' % (fam, n, c['match'], c['differs'], c['missing'], c['hidden']))
        if not fam.startswith('class:'):
            tot.update(c)
    n = sum(tot.values())
    # match rate over the files the game loads (hidden files that exist from an older import are not counted either)
    loaded = [r for r in rows if (r[0], r[1]) not in hidden]
    lm = sum(1 for r in loaded if r[2] == 'match')
    print('%-50s %7d %7d %7d %7d %7d   match rate %.1f%% (%d of the %d files the game loads)' % (
        'TOTAL', n, tot['match'], tot['differs'], tot['missing'], tot['hidden'], 100.0 * lm / max(len(loaded), 1), lm, len(loaded)))
    print('hidden: not built, a higher overlay has the same file so the game never loads it (rebuild with --all-files to build them)')
    print('extra files not in the manifest:', len(extra))
    for e in extra[:15]:
        print('   ', e)
    print('report:', report)


if __name__ == '__main__':
    main()
