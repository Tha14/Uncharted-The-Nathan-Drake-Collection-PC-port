import sys, struct
sys.path.insert(0, sys.argv[1])
import dcconv as C
a = C.Mod(open(sys.argv[2], 'rb').read()); b = C.Mod(open(sys.argv[3], 'rb').read())
M = C.anchors(a, b)
def hdr_ok(t, h):
    t4 = M.get(t)
    return t4 is not None and t4 >= 8 and b.d[t4 - 8:t4] == b'\0\0\0\0' + struct.pack('<I', h)
objs = C.objects(a, hdr_ok); keys, _ = C.obj_keys(a, objs)
st4 = [M.get(o[0]) - (8 if o[2] is not None else 0) if M.get(o[0]) is not None else None for o in objs]
n = 0; lim = int(sys.argv[4]) if len(sys.argv) > 4 else 10
for i, ob in enumerate(objs):
    nx = st4[i + 1] if i + 1 < len(objs) else b.reloc
    ev = C.align_object(a, b, M, ob, nx, [b'u3final/big3/'])
    if ev is None:
        n += 1
        if n <= lim:
            s3, e3 = C.region(ob)
            print('FAIL obj %x-%x hdr=%s key=%s s4=%s e4=%s' % (s3, e3, ob[2] and hex(ob[2]), keys[i], st4[i] and hex(st4[i]), nx and hex(nx)))
            print('   ps3', a.d[s3:e3][:80].hex(), 'slots', [hex(x - s3) for x in a.dslots if s3 <= x < e3][:10])
            if st4[i] is not None and nx: print('   ps4', b.d[st4[i]:nx][:120].hex())
print('failed', n, 'of', len(objs))
