"""hash_tree.py <root> <out.tsv> [subdirs...]: path\tsize\tsha256 for every file under root (or given subdirs)."""
import sys, os, hashlib, time
root, out = sys.argv[1], sys.argv[2]
subs = sys.argv[3:] or ["."]
t0 = time.time(); n = 0
with open(out, "w", encoding="utf-8") as fh:
    fh.write("path\tsize\tsha256\n")
    for s in subs:
        for dp, dn, fn in os.walk(os.path.join(root, s)):
            for f in fn:
                p = os.path.join(dp, f)
                h = hashlib.sha256()
                with open(p, "rb") as g:
                    for c in iter(lambda: g.read(1 << 20), b""): h.update(c)
                fh.write(f"{os.path.relpath(p, root).replace(chr(92), '/')}\t{os.path.getsize(p)}\t{h.hexdigest()}\n")
                n += 1
                if n % 500 == 0: print(n, int(time.time() - t0), "s", flush=True)
print("done", n, int(time.time() - t0), "s")
