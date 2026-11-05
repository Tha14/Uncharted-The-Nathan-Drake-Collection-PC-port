import pickle,re,bisect
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
i=bisect.bisect_left(A,0x10bc590)
for a in A[i:i+70]: print(hex(a),T[a][:70])
print('--- +0x2b0 in draw-ish code')
for a in A:
    if 0x1080000<=a<0x10d8000 and re.search(r'\+0x2b[04]\]',T[a]): print(hex(a),T[a][:70])
print('--- +0x2b0 callers of 10bc590')
for a in A:
    if re.search(r'call\s+0x10bc590',T[a]): print(hex(a),T[a][:50])
