from lib import *
n=sys.argv[1];pat=sys.argv[2]
q=Pak(OV+n+'.pak');sb={s['name']:s for s in subs(q)};I=insts(q)
for ins in I:
    for x in isubs(q,ins):
        s=sb.get(x['name'])
        if s and re.search(pat,s['mat']):
            print(ins['i'],x['name'][-30:],s['mat'].split(',')[0].rsplit('/',1)[-1][:36],'fl',hex(x['flags']),hex(x['flags2']),'proto ns2',s['w'][11],{k:(v.min(0).round(2).tolist(),v.mean(0).round(2).tolist(),v.max(0).round(2).tolist()) for k,v in x['st'].items()})
