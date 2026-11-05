import sys,os,json,struct,hashlib
# blkapply.py ROOT PATCH : apply block patch (json header + blobs) in place, verify md5
root,patch=sys.argv[1:3]; B=65536
raw=open(patch,'rb').read(); hl=struct.unpack('<I',raw[:4])[0]; H=json.loads(raw[4:4+hl]); pos=4+hl
for e in H:
    f=os.path.join(root,e['path']); d=bytearray(open(f,'rb').read()) if os.path.exists(f) else bytearray()
    if hashlib.md5(d).hexdigest()==e['md5']:
        pos+=sum(n for i,n in e['blocks']); print(e['path'],'OK already'); continue
    if len(d)<e['size']: d+=bytes(e['size']-len(d))
    del d[e['size']:]
    for i,n in e['blocks']:
        d[i*B:i*B+n]=raw[pos:pos+n]; pos+=n
    ok=hashlib.md5(d).hexdigest()==e['md5']
    if ok:
        open(f+'.tmp','wb').write(d); os.replace(f+'.tmp',f)
    print(e['path'],'OK' if ok else 'MD5 MISMATCH (not written)')
