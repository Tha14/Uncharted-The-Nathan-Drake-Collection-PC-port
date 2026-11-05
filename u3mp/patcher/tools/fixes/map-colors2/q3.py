from lib import *
import gzip,ps3params
# PS4 formats
q=Pak(OV+'air-multi.pak')
T={t['path'].split('texture2/')[-1].split('/')[0:]and t['path']:t for t in q.textures()}
for k,t in T.items():
    if re.search('ground-concrete-(normal|hazard|color)|tarmac-normal|concrete-dirty-blend',k): print(k.split('/')[-2],hex(t['format']),t['width'],t['height'],t['mips'])
R=[json.loads(l) for l in gzip.open('rt.jsonl.gz','rt')]
Rm={m['n'].split(',')[0]:(x['pak'],m) for x in R for m in x.get('mats',[])}
for k,(pk,m) in Rm.items():
    if k.endswith('air-yem-metal-sheet-wall:default'):
        qq=Pak(RT+pk);TT={t['path']:t for t in qq.textures()}
        for t in TT.values():
            if 'sheet-metal-wall-normal' in t['path']: print('retail shared',t['path'].split('/')[-2],hex(t['format']))
# PS3 block
p=Pak('p3/air-multi.pak')
for t in p.textures():
    if 'ground-concrete-normal' in t['path'] or 'hazard-color' in t['path']: print('ps3',t['path'].split('/')[-2],t.get('format'),t['width'],t['height'])
mt=p.items_of('MATERIAL_TABLE_1')[0];pg,b=mt.page,mt.data_off
nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20);th=p.resolve(pg,b+0x18);nm=p.resolve(pg,b+0x30);ntex=p.pu32(pg,b+4)
for i in range(nmat):
    m=p.resolve(ma[0],ma[1]+4*i);name=p.pstr(*p.resolve(*m))
    if name.endswith('concrete-multi-hazard/polished-dust'):
        k,e,node=ps3params._walk(p,m)[0];bb=p.resolve(node[0],node[1]+0x10)
        for o in range(0,0x90,0x10):
            rr=p.resolve(bb[0],bb[1]+o); x=None
            if rr and rr[0]==th[0] and th[1]<=rr[1]<th[1]+4*ntex: x=p.pstr(*p.resolve(nm[0],nm[1]+4*((rr[1]-th[1])//4))).split('/')[-2]
            print(hex(o),x,struct.unpack_from('>4I',p.d,p.pbase(bb[0])+bb[1]+o))
# overlay paks: effects with overlay
O=[json.loads(l) for l in gzip.open('ov.jsonl.gz','rt')]
print('overlay mats with COLOR_n overlay:',collections.Counter((x['pak']) for x in O for m in x.get('mats',[]) if any('APPLY_OVERLAY' in f for f in m['f'])).most_common(10))
print('with SHARED1_B:',collections.Counter(x['pak'] for x in O for m in x.get('mats',[]) if 'BLEND_FACTOR_USE_SHARED_TEXTURE_1_B' in m['f']).most_common(8))
for x in O:
    if x['pak']=='air-multi.pak':
        for m in x['mats']:
            if re.search('hazard|striped|-red:|concrete-multi',m['n']): print(m['n'].split(',')[0].split('/')[-1],m['h'],[b[1].split('/')[-1][:30] for b in m['b'] if 'LightMap' not in b[0]])
