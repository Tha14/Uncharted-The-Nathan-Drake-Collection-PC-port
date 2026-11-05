import pickle,re,bisect,sys
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
for x in [0xe9498f,0x1225eb8,0xf0c4bd]:
    i=bisect.bisect_left(A,x)
    print('---',hex(x))
    for a in A[i-14:i+70]:
        t=T[a]
        if re.match(r'^[0-9a-f]{2}( [0-9a-f]{2})* *$',t): continue
        print(hex(a),t[:70])
