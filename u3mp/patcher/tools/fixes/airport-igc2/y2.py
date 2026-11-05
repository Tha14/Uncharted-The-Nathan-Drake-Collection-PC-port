import pickle,re,sys,bisect,struct
A,T=pickle.load(open('../live/instance-mat/_dis.pkl','rb'))
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
G='{{PS4}}/u3data/build/main/dc1/ps4/'
c=DC(G+'player-health-settings.bin')
for s,t,o in c.syms:
    print(hex(s),hex(t),hex(o),D.get(t))
print(c.reloc, len(c.d))
for o in range(0x20,c.reloc,4):
    a=c.u32(o);f=c.f32(o);print(hex(o),hex(a),'%.3f'%f,D.get(a,''),'PTR' if o in c.ptrs else '')
for n in ['*player-damage-fx-settings*','*player-blood-fx-settings*','*player-combat-fx-settings*','*player-damage-fx-glows*']:
    v=sid(n)
    d=open('../live/instance-mat/_text.bin','rb').read()
    print(n,hex(v),[hex(m.start()) for m in re.finditer(re.escape(struct.pack('<I',v)),d)][:8])
    # menu offsets
hits=[a for a in A if re.search(r'\+0x7fc\]',T[a])][:30]
print([(hex(a),T[a][:40]) for a in hits])
