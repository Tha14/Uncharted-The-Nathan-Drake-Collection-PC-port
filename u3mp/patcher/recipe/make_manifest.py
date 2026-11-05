"""Inventory the overlay folders: path, size, sha256, mtime -> manifest.tsv + manifest.json.
Usage: python make_manifest.py <work-dir> <out-dir> [overlay names...]"""
import sys, os, hashlib, json, time
work, out = sys.argv[1], sys.argv[2]
names = sys.argv[3:] or ["overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"]
rows = []
t0 = time.time()
for ov in names:
    base = os.path.join(work, ov)
    for dp, dn, fn in os.walk(base):
        for f in fn:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, base).replace("\\", "/")
            h = hashlib.sha256()
            with open(p, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            st = os.stat(p)
            rows.append({"overlay": ov, "path": rel, "size": st.st_size, "sha256": h.hexdigest(), "mtime": int(st.st_mtime)})
    print(ov, len(rows), "files so far", int(time.time() - t0), "s", flush=True)
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "manifest.tsv"), "w", encoding="utf-8") as fh:
    fh.write("overlay\tpath\tsize\tsha256\tmtime\n")
    for r in rows:
        fh.write(f"{r['overlay']}\t{r['path']}\t{r['size']}\t{r['sha256']}\t{r['mtime']}\n")
with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as fh:
    json.dump(rows, fh, indent=0)
print("done", len(rows), "files", int(time.time() - t0), "s")
