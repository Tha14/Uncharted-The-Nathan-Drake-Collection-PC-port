from lib import *
q=Pak(OV+'air-multi.pak')
for m in mats(q):
    n=m['name']
    if 'asphalt-broken:default' in n or 'sand-rough' in n or 'manhole' in n:
        print(n,m['hash'],m['x3']);print('  P',m['params']);print('  B',[(a.replace('g_tNdFetch',''),b.rsplit('/',1)[-1]) for a,b in m['binds']])
