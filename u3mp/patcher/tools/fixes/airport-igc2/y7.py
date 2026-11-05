import pickle,re,bisect,struct
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
d=open('../live/instance-mat/_text.bin','rb').read()
i=bisect.bisect_left(A,0xf0c4ba)
for a in A[i:i+70]:
    t=T[a]
    if re.match(r'^[0-9a-f]{2}( [0-9a-f]{2})* *$',t): continue
    x=''
    m=re.search(r'# (0x[0-9a-f]+)',t)
    if m and 'xmm' in t and int(m.group(1),16)<len(d): x=str(struct.unpack_from('<4f',d,int(m.group(1),16)))
    print(hex(a),t[:62],x)
