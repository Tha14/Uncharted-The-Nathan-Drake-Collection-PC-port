"""psarc_hash.py <out.tsv> <psarc...>: sha256 of every entry of each PSARC (streamed, nothing extracted to disk).
Columns: psarc\tentry\tsize\tsha256"""
import sys, os, hashlib, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "paktools"))
from psarc_lib import Psarc
out = sys.argv[1]; t0 = time.time()
with open(out, "a", encoding="utf-8") as fh:
    for pth in sys.argv[2:]:
        try:
            p = Psarc(pth)
        except Exception as e:
            print("skip", pth, e, flush=True); continue
        for i, n in enumerate(p.names):
            try:
                d = p.read(i + 1)
            except Exception as e:
                fh.write(f"{pth}\t{n}\t-1\tERR {e}\n"); continue
            fh.write(f"{pth}\t{n}\t{len(d)}\t{hashlib.sha256(d).hexdigest()}\n")
        fh.flush()
        print("done", os.path.basename(pth), len(p.names), "entries", int(time.time() - t0), "s", flush=True)
