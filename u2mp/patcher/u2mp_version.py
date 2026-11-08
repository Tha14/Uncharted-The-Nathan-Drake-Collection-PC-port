"""u2mp_version.py - fingerprints of the shipped Uncharted 2 multiplayer import (like u3mp/patcher/u3mp_version.py).

shipped_id(): one sha256 over everything the import runs (u2mp_import.py, u2conv/, ../tools/, ../docs/import-list.tsv).
u2mp_import.py stores it in the import's state.json; the launcher compares it with the installed files, so after a
launcher update that changed the import it reruns the import by itself, and the import then redoes only the steps
whose own fingerprint changed (module_closure: a step's code and the modules it imports).
"""
import hashlib
import os
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOTS = (HERE / 'u2conv', HERE.parent / 'tools')
TEXT = ('.py', '.txt', '.json', '.tsv', '.md')
IMPORT_RE = re.compile(r'^\s*(?:from\s+(\w+)\s+import|import\s+([\w, ]+))', re.M)


def file_digest(p):
    """sha256 of a file; text with \n line ends, so a git checkout with CRLF gives the value of the release zip."""
    b = Path(p).read_bytes()
    if str(p).endswith(TEXT):
        b = b.replace(b'\r\n', b'\n')
    return hashlib.sha256(b).digest()


def module_path(name):
    for root in ROOTS:
        p = root / (name + '.py')
        if p.is_file():
            return p
    return None


def module_closure(names):
    """The u2conv / tools modules `names` import, directly or through each other (paths, sorted)."""
    seen, todo = {}, list(names)
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        p = module_path(name)
        seen[name] = p
        if p is None:
            continue
        for a, b in IMPORT_RE.findall(p.read_text(encoding='utf-8', errors='replace')):
            for n in ([a] if a else [x.strip().split(' ')[0] for x in b.split(',')]):
                if n and n not in seen:
                    todo.append(n)
    return sorted(p for p in seen.values() if p is not None)


def shipped_id(patcher=HERE):
    h = hashlib.sha256()
    patcher = Path(patcher)
    files = [patcher / 'u2mp_import.py', patcher / 'u2mp_version.py', patcher.parent / 'docs' / 'import-list.tsv']
    for root in (patcher / 'u2conv', patcher.parent / 'tools'):
        for dp, dn, fn in os.walk(root):
            dn[:] = sorted(d for d in dn if d != '__pycache__')
            files += [Path(dp) / f for f in sorted(fn) if not f.endswith(('.pyc', '.orig'))]
    for p in files:
        try:
            h.update(p.relative_to(patcher.parent).as_posix().encode() + b'\0' + file_digest(p))
        except (OSError, ValueError):
            continue
    return h.hexdigest()


if __name__ == '__main__':
    print(shipped_id())
