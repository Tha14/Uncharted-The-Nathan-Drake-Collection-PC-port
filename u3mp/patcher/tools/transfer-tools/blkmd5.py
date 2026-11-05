import hashlib,sys,os,json
# blkmd5.py ROOT LIST OUT : per-64KB-block md5 of each listed file under ROOT
root,lst,out=sys.argv[1:4]; B=65536; res={}
for p in open(lst).read().split():
    f=os.path.join(root,p)
    if not os.path.exists(f): res[p]=None; continue
    d=open(f,'rb').read(); res[p]=[len(d)]+[hashlib.md5(d[i:i+B]).hexdigest()[:12] for i in range(0,len(d),B)]
json.dump(res,open(out,'w'))
