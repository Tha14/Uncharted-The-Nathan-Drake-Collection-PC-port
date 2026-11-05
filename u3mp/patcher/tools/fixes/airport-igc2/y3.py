import pickle,re,bisect
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
for pat in [r'\+0x2b0\]',r'\+0x2b4\]']:
    H=[a for a in A if re.search(pat,T[a])]
    print(pat,len(H))
    for a in H[:70]:
        if 'xmm' in T[a] or 'mov' in T[a]: print(hex(a),T[a][:60])
