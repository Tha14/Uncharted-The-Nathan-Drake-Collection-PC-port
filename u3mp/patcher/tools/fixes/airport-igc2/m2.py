import sys,re
exec(open('m1.py').read().split("for n in sys.argv")[0])
for n in sys.argv[1:]:
    f,l=find('actor33',n);print('=====',n,l)
    for nm,sh,ef,fe,par,bi,u in mats(f):
        print(' ',nm.split(',')[0].split('/')[-1][:36].ljust(36),'ef',ef[:10],'subs',len(u),'tint',[x.replace('COLOR_1_APPLY_','') for x in fe if 'TINT' in x and 'OCCL' not in x],'cloth' if 'DIFFUSE_LIGHT_CHEAP_CLOTH' in fe else '','color',[b[1][:10] for b in bi if 'Color1' in b[0]],[b[0][11:] for b in bi if 'Tint' in b[0]], {k:v for k,v in par.items() if 'olorTint' in k or 'Brightness1' in k})
