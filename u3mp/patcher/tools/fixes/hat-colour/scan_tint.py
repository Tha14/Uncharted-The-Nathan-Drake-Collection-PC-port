"""Read-only survey: every converted (non-retail) actor pak the game loads from overlay-r3/overlay-port whose PS3 source
has a material with a tint mask. Reports whether the PS4 material that replaced it carries the tint features/bind, and
whether some other material got the tint features although PS3 had no tint mask for it. Writes scan_tint.tsv."""
import sys,os,io,contextlib,re
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
HERE=os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0,AM)
from lib import *
from ps3mat_am import read_materials
import ps4actor as A, ps3geom
s3=S3()
retail={f[:-4] for f in os.listdir(RET)}
seen=set(); rows=[]
for lay in (W+'/overlay-r3',W+'/overlay-r2',W+'/overlay-port',W+'/overlay-mp'):
    d=lay+'/u3data/build/main/actor33/ps4'
    if not os.path.isdir(d): continue
    for f in sorted(os.listdir(d)):
        n=f[:-4]
        if n in seen or n in retail or not f.endswith('.pak'): continue
        seen.add(n)
        try:
            q=Pak(d+'/'+f)
            if not q.items_of('GEOMETRY_1'): continue
            ms=A.tmpl_materials(q)
        except Exception as e:
            rows.append((n,lay.split('/')[-1],'ps4-parse-error',str(e)[:60])); continue
        src,data=s3.get(n)
        if not data: rows.append((n,lay.split('/')[-1],'no-ps3','')); continue
        try:
            p=Pak(data); m3,t3=read_materials(p)
        except Exception as e:
            rows.append((n,lay.split('/')[-1],'ps3-parse-error',str(e)[:60])); continue
        used=set()
        try:
            for v in ps3geom.prototypes(p).values(): used|=set(A.ps3_block_materials(p,v['proto']))
        except Exception: used=None
        ps3tint={m['name'] for m in m3 if any('tint-mask' in t3[i]['name'] for i in m['textures']) and (used is None or m['name'] in used)}
        ps3all={m['name'] for m in m3 if (used is None or m['name'] in used)}
        p4={}
        for m in ms:
            key=m['name'].split(',')[0]; nm=key.replace(':','/')
            tint='COLOR_1_APPLY_INSTANCE_TINT' in m['feats']; mask=any(b[0]=='g_tNdFetchColorTintMask1Map' for b in m['binds'])
            tmask=[b[1].split('/')[-1] for b in m['binds'] if b[0]=='g_tNdFetchColorTintMask1Map']
            p4[nm]=(tint,mask,tmask)
        for mn in sorted(ps3all):
            if 'shadowblocker' in mn: continue
            t=p4.get(mn)
            if t is None: rows.append((n,lay.split('/')[-1],'ps4-material-missing',mn)); continue
            if mn in ps3tint and not (t[0] and t[1]): rows.append((n,lay.split('/')[-1],'TINT-LOST',mn+' tint=%d mask=%d %s'%(t[0],t[1],t[2])))
            elif mn in ps3tint: rows.append((n,lay.split('/')[-1],'tint-ok',mn+' %s'%t[2]))
            elif t[0]: rows.append((n,lay.split('/')[-1],'TINT-EXTRA',mn+' mask=%d %s'%(t[1],t[2])))
        print(n,flush=True)
with open(HERE+'/scan_tint.tsv','w') as f:
    for r in rows: f.write('\t'.join(r)+'\n')
import collections; print(collections.Counter(r[2] for r in rows))
