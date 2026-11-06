"""u3mp_version.py - a fingerprint of the shipped multiplayer recipe (u3mp/patcher/recipe + tools).

u3mp_import.py stores it in the data folder's state.json after a recipe run; the launcher compares it with the
installed files, so after a launcher update that changed the recipe or a converter it reruns the import by itself
(rebuild_overlays.py then reruns only the steps whose own fingerprint changed, see recipe_lib.step_fingerprint).
"""
import hashlib
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEXT = ('.py', '.sh', '.txt', '.json', '.tsv', '.md', '.cmd', '.c')


def recipe_fingerprint(patcher=HERE):
    """sha256 over recipe/ and tools/ (relative path + content; text with \n line ends, so a git checkout with
    CRLF gives the same value as the release zip)."""
    h = hashlib.sha256()
    for part in ('recipe', 'tools'):
        base = Path(patcher) / part
        for dp, dn, fn in os.walk(base):
            dn[:] = sorted(d for d in dn if d != '__pycache__')
            for f in sorted(fn):
                if f.endswith(('.pyc', '.orig')) or f == 'CONTENT_ID':
                    continue
                p = Path(dp) / f
                try:
                    b = p.read_bytes()
                except OSError:
                    continue
                if f.endswith(TEXT):
                    b = b.replace(b'\r\n', b'\n')
                h.update(p.relative_to(patcher).as_posix().encode() + b'\0' + hashlib.sha256(b).digest())
    return h.hexdigest()


def shipped_id(patcher=HERE):
    """The fingerprint of the installed recipe: recipe/CONTENT_ID (written by maintainer/sync_recipe.py, so the
    launcher does not hash 1 500 files at every start), else computed."""
    try:
        v = (Path(patcher) / 'recipe' / 'CONTENT_ID').read_text(encoding='ascii').strip()
        if len(v) == 64:
            return v
    except OSError:
        pass
    return recipe_fingerprint(patcher)


if __name__ == '__main__':
    print(recipe_fingerprint())
