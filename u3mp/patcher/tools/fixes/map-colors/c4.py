from lib import *
q=Pak(OV+'air-multi.pak')
for t in q.textures():
    if t['format'] in(0x53,) and 'ground' in t['path']:
        print(t['path'].rsplit('/',2)[1][:44],t['width'],t['mips'],t['unk34'],{k:np.round(v,1) for k,v in bc5(q,t).items()})
    elif 'normal' in t['path'] and 'ground' in t['path']:print('NOTBC5',t['path'].rsplit('/',2)[1][:44],hex(t['format']),t['width'])
