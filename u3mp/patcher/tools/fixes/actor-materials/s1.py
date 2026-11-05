from lib import *
s=S3()
for n,a,ix in s.idx:
    print(n,len(a.names),[x for x in a.names[:3]])
    print('  hair',[x for x in a.names if 'flynn' in x][:30])
rows=[ast.literal_eval(l) for l in open('../airport-igc/collapsed.txt')]
for r in rows:
    al=s.all(r[0]); hs=[(n,hashlib.sha1(d).hexdigest()[:8],len(d)) for n,d in al]
    print(r,hs)
