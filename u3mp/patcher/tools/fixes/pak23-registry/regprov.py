#!/usr/bin/env python3
"""Line provenance of an overlay pak23_ps4.txt: express every line as a copy of a line range of a game text
(retail PS4 pak23_ps4.txt, PS3 1.19 pak23.txt, DLC / Lab level .txt files) or as a literal (our own lines).
Writes a json recipe that regbuild.py replays byte for byte from the user's own game files.

usage: regprov.py <reference pak23_ps4.txt> <out recipe.json> (SRC_ID=PATH | @DIR) ...
  SRC_ID names a source text (retail, ps3-p18, ps3-p13, ps3-disc, <archive>__<entry>); @DIR = every file of DIR
  (as written by regsources.py; retail, ps3-p18, ps3-p13, ps3-disc are preferred in that order).
Line model: the file is split on LF; a trailing CR is recorded per op (CRLF or LF) so mixed endings replay exactly.
A copy op takes source lines verbatim (strip=0) or rstripped (strip=1, how mkregistry / build_map_levels write
PS3 lines). Lines found in no source are literals: our own additions (emblem actors, lab34 levels, ...).
"""
import json
import os
import sys

CR = chr(13)
PRI = ['retail.txt', 'ps3-p18.txt', 'ps3-p13.txt', 'ps3-disc.txt']


def load_lines(path):
    d = open(path, 'rb').read().decode('latin1')
    return d.split('\n')


def src_lines(path):
    return [x[:-1] if x.endswith(CR) else x for x in load_lines(path)]


def load_sources(args):
    srcs = []
    for a in args:
        if a.startswith('@'):
            fs = sorted(os.listdir(a[1:]))
            for f in [x for x in PRI if x in fs] + [x for x in fs if x not in PRI]:
                srcs.append((f[:-4], src_lines(os.path.join(a[1:], f))))
            continue
        k, p = a.split('=', 1)
        srcs.append((k, src_lines(p)))
    return srcs


def main():
    ref_path, out = sys.argv[1], sys.argv[2]
    srcs = load_sources(sys.argv[3:])
    ref = load_lines(ref_path)
    body = [(l[:-1], True) if l.endswith(CR) else (l, False) for l in ref]
    idx = [{}, {}, {}]                       # trigram / bigram / unigram (rstripped) -> [(source, line)]
    for si, (k, L) in enumerate(srcs):
        S = [x.rstrip() for x in L]
        for i in range(len(L)):
            for n in (3, 2, 1):
                if i + n <= len(L):
                    lst = idx[3 - n].setdefault(tuple(S[i:i + n]), [])
                    if len(lst) < 64:
                        lst.append((si, i))
    ops = []
    k = 0
    n = len(body)
    while k < n:
        best = None
        for g, gram in enumerate((3, 2, 1)):
            if k + gram > n:
                continue
            key = tuple(x.rstrip() for x, _ in body[k:k + gram])
            for si, i in idx[g].get(key, ()):
                L = srcs[si][1]
                for strip in (0, 1):
                    r = 0
                    while (k + r < n and i + r < len(L) and body[k + r][1] == body[k][1] and
                           (L[i + r].rstrip() if strip else L[i + r]) == body[k + r][0]):
                        r += 1
                    if r and (best is None or r > best[2] or (r == best[2] and si < best[0])):
                        best = (si, i, r, strip)
            if best and best[2] >= gram:
                break
        if best is None:
            if ops and ops[-1][0] == 'L' and ops[-1][2] == body[k][1]:
                ops[-1][1].append(body[k][0])
            else:
                ops.append(['L', [body[k][0]], body[k][1]])
            k += 1
            continue
        si, i, r, strip = best
        ops.append(['C', srcs[si][0], i, r, body[k][1], strip])
        k += r
    lit = sum(len(o[1]) for o in ops if o[0] == 'L')
    used = sorted({o[1] for o in ops if o[0] == 'C'})
    json.dump(dict(reference=os.path.basename(ref_path), lines=n, sources=used, ops=ops), open(out, 'w'), indent=0)
    print('%s: %d lines, %d ops, %d literal lines, sources %s' % (out, n, len(ops), lit, used))


if __name__ == '__main__':
    main()
