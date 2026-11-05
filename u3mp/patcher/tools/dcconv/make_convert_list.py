#!/usr/bin/env python3
"""Write tools/fixes/dcconv/dc1-convert.tsv (the file list of the convert-dc1 recipe step) and test the converter.

  python make_convert_list.py <pairs.tsv> <pairs_reference.tsv> <schema.json,...> <out.tsv>

pairs.tsv: ps3 file <TAB> overlay file (the current overlay path); pairs_reference.tsv: the same rows with the
file the cloud conversion produced (the work/backup-* copy where a later fix changed the overlay file).
The PS3 path must contain '/<layer>/dc1/ps4/...' (layer = data2, patch18, patch13, goty-patch, disc), as
written by the 2026-11-05 extraction. Each row gets the reference sha256 and 'identical' / 'different'."""
import collections, hashlib, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dcconv

cur = [l.rstrip('\n').split('\t') for l in open(sys.argv[1], encoding='utf-8') if l.strip()]
ref = [l.rstrip('\n').split('\t') for l in open(sys.argv[2], encoding='utf-8') if l.strip()]
C = dcconv.Converter([json.load(open(x)) for x in sys.argv[3].split(',')])
out = open(sys.argv[4], 'w', encoding='utf-8', newline='\n')
out.write('# overlay\tPS4 path under build/main\tPS3 layer\tsha256 of the cloud conversion\tconverter state\n')
tot = collections.Counter()
for (p3, p4), (_, r4) in zip(cur, ref):
    ov = p4.split('/work/')[1].split('/')[0]
    rel4 = p4.split('/u3data/build/main/')[1]
    layer = p3.replace('\\', '/').split('/ps3/')[1].split('/')[0]
    want = open(r4, 'rb').read()
    got, _ = C.convert(open(p3, 'rb').read(), collections.Counter())
    st = 'identical' if got == want else 'different'
    tot[st] += 1
    out.write('%s\t%s\t%s\t%s\t%s\n' % (ov, rel4, layer, hashlib.sha256(want).hexdigest(), st))
print(dict(tot))
