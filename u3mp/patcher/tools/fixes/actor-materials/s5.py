from lib import *
import json
scan=json.load(open('scan.json'));done={f[:-4] for f in json.load(open('manifest.json'))}|{'mp-hero-flynn-coop','mp-hero-lazarevic-coop'}
u=json.load(open('usage.json'))
def used(q):
    g=q.items_of('GEOMETRY_1')[0];pg,b=g.page,g.data_off;ns=q.pu32(pg,b+8);r=q.resolve(pg,b+0x10);c=collections.Counter()
    for i in range(ns):
        x=q.resolve(r[0],r[1]+0x88*i+0x48);nv=q.pu32(r[0],r[1]+0x88*i+0x20);c[q.pstr(*x).split(',')[0] if x else None]+=nv
    return c
rows=[]
for r in scan:
    if r['n'] in done: continue
    c=used(Pak(find('actor33',r['n'])[0]));t=sum(c.values());sb=sum(v for k,v in c.items() if 'shadowblocker' in k)
    rows.append((r['n'],r['ps4'],r['ps3'],t,sb,sorted(u.get(r['n'],{}).keys())))
print(len(rows))
print('gap>=2',[x[:3] for x in rows if x[2]-x[1]>=2])
print('only shadowblocker visible',[x for x in rows if x[4]==x[3]])
print('unused',[x[0] for x in rows if not x[5]])
print('mostly sb',[x[:5] for x in rows if x[3] and x[4]/x[3]>0.5 and x[4]!=x[3]])
