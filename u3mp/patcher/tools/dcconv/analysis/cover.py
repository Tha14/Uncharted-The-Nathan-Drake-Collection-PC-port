"""How much of a merged PS4 DC module is explained by retail PS4 bytes, byte-swapped PS3 words or PS3 strings?
usage: cover.py <dcconv dir> <target ps4> <retail ps4> <ps3 1.19> [more ps3 ...]"""
import sys, collections
sys.path.insert(0, sys.argv[1])
import dcconv as C
T = C.Mod(open(sys.argv[2], 'rb').read()); R = C.Mod(open(sys.argv[3], 'rb').read())
P3 = [C.Mod(open(p, 'rb').read()) for p in sys.argv[4:]]
d = bytearray(T.d)
for s in T.slots: d[s:s + 8] = b'\0' * 8          # pointers come from the graph, not from a source
cov = bytearray(len(d))
for s in T.slots: cov[s:s + 8] = b'\x01' * 8
cov[:T.data_start] = b'\x01' * T.data_start
K = 16


def index(buf, step):
    ix = collections.defaultdict(list)
    for i in range(0, len(buf) - K, step): ix[bytes(buf[i:i + K])].append(i)
    return ix


r = bytearray(R.d)
for s in R.slots: r[s:s + 8] = b'\0' * 8
src = [('retail', r)]
for m in P3:
    w = bytearray(m.d)
    sw = bytearray(len(w))
    for i in range(0, len(w) - 3, 4): sw[i:i + 4] = w[i:i + 4][::-1]
    src.append(('ps3raw', w)); src.append(('ps3sw32', sw))
stat = collections.Counter()
for name, buf in src:
    ix = index(buf, 4 if name != 'ps3raw' else 1)
    i = T.data_start
    while i < T.reloc - K:
        if cov[i] and cov[i + K - 1]: i += 4; continue
        k = bytes(d[i:i + K])
        if any(k) and k in ix:
            j = ix[k][0]; n = K
            while i + n < T.reloc and j + n < len(buf) and d[i + n] == buf[j + n]: n += 1
            for x in range(i, i + n):
                if not cov[x]: cov[x] = 1; stat[name] += 1
            i += n
        else: i += 4
zero = sum(1 for x in range(T.data_start, T.reloc) if not cov[x] and d[x] == 0)
left = sum(1 for x in range(T.data_start, T.reloc) if not cov[x] and d[x] != 0)
print('target', len(T.d), 'data', T.reloc - T.data_start, 'slots', len(T.slots), dict(stat), 'zero-unexplained', zero, 'NONZERO-unexplained', left)
