import pickle,re,bisect,struct
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
d=open('../live/instance-mat/_text.bin','rb').read()
i=bisect.bisect_left(A,0x10c6387)
# find xmm0 def
for a in reversed(A[i-260:i]):
    if re.match(r'v\w+ xmm0,',T[a]) or 'xmm0,XMMWORD' in T[a]:
        print('xmm0 def',hex(a),T[a]);
        m=re.search(r'# (0x[0-9a-f]+)',T[a])
        if m: v=int(m.group(1),16);print(struct.unpack('<4f',d[v:v+16]))
        break
for a in reversed(A[i-260:i]):
    if re.match(r'v\w+ xmm1,',T[a]):
        print('xmm1 def',hex(a),T[a]);break
for b,n in ((0x10ca730,60),(0x10bbfaf,36)):
    j=bisect.bisect_left(A,b);print('--',hex(b))
    for a in A[j:j+n]: print(hex(a),T[a][:70])
