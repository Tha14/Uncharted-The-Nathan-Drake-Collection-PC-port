import sys,struct,re
sys.argv=['x','0']
sys.path.insert(0,'../coop-voice')
import importlib.util
src=open('../coop-voice/ppc.py').read().split('\nif ')[0]
g={};exec(src.split("a=int(sys.argv")[0],g)
d=g['d'];dis=g['dis'];S=g['S'];o=g['o']
print(S[:2])
for pat,name in [(struct.pack('>4f',1,0,.5,.5),'1,0,.5,.5'),(struct.pack('>4f',1,1,1,0),'1,1,1,0')]:
    L=[m.start() for m in re.finditer(re.escape(pat),d)]
    print(name,[hex(x) for x in L][:8])
    for f in L[:8]:
        for va,off,fs,fl in S:
            if off<=f<off+fs:
                v=va+f-off
                hi=(v+0x8000)>>16;lo=v&0xffff
                # scan text for addi rX,rY,lo after lis hi
                t0=S[0][1];t1=t0+S[0][2]
                for i in range(t0,t1-4,4):
                    w=struct.unpack_from('>I',d,i)[0]
                    if w>>26==14 and (w&0xffff)==lo:
                        for j in range(1,8):
                            w2=struct.unpack_from('>I',d,i-4*j)[0]
                            if w2>>26==15 and (w2&0xffff)==hi and ((w2>>16)&31)==0 and ((w2>>21)&31)==((w>>16)&31):
                                a=S[0][0]+i-t0
                                print(' ref',hex(a),'data',hex(v))
                                for k in range(-12,22): print('   ',hex(a+4*k),dis(a+4*k))
