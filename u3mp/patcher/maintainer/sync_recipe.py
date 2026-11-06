"""sync_recipe.py - (maintainers) copy the overlay recipe and its tools into u3mp/patcher/{recipe,tools}.

  python sync_recipe.py "<...>/PKG Converter" --private "<extra files checkout>"

The extra files checkout lists, in private-files.txt, the files that are packaged from it and never committed here
("file <path under u3mp/patcher>") and the steps left out of this steps/ORDER.txt ("order <id> after <id>"): they are
copied into its launcher-files/ instead.  The release build adds them back before packaging.
The launcher runs whatever recipe is copied here (steps/ORDER.txt).  Left out: the maintainers' research files
(transcript mining, test results, the hash tables only provenance.py reads), caches, and anything that is not text or
a known table.  The copy refuses files with game-data extensions (paks, archives, textures, sound, executables).
"""
import fnmatch
import os
import shutil
import sys
from pathlib import Path

DEST = Path(__file__).resolve().parent.parent
SKIP_DIRS = {'__pycache__', 'transcript-mining', 'test-results', 'ps3-hashes', 'pylib'}
SKIP_FILES = ('*.pyc', '*.orig', '*.log', '*.tmp', 'backup-hashes-*.tsv', 'retail-ps4-*.tsv', 'transfer-hashes.tsv',
              'stage-assets-hashes.tsv')
ALLOWED = ('.py', '.pyi', '.txt', '.json', '.tsv', '.md', '.sh', '.cmd', '.gz', '.typed', '.c', '')
# our own compiled helpers are built from their .c source by tools/package_windows.sh, never committed
BUILT_HERE = ('vagenc.dll',)
FORBIDDEN = ('.pak', '.psarc', '.bin', '.dds', '.png', '.jpg', '.wav', '.mp3', '.at9', '.elf', '.self', '.exe', '.dll',
             '.pyd', '.pkg', '.sfo', '.dc', '.dci', '.bnk', '.vtex')


def read_private(extra):
    files, steps = set(), set()
    for line in (extra / 'private-files.txt').read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        kind, _, rest = line.partition(' ')
        if kind == 'file':
            files.add(rest.strip())
        elif kind == 'order':
            steps.add(rest.partition(' after ')[0].strip())
    return files, steps


def main():
    args = sys.argv[1:]
    if '--private' not in args or args.index('--private') + 1 >= len(args) or len(args) != 3:
        sys.exit(__doc__)
    i = args.index('--private')
    extra = Path(args[i + 1])
    src = Path(next(a for j, a in enumerate(args) if j not in (i, i + 1)))
    if not (extra / 'private-files.txt').is_file():
        sys.exit('not the extra files checkout (no private-files.txt): ' + str(extra))
    private, private_steps = read_private(extra)
    if not (src / 'recipe' / 'rebuild_overlays.py').is_file():
        sys.exit('not a PKG Converter folder: ' + str(src))
    n_private = 0
    for part in ('recipe', 'tools'):
        dest = DEST / part
        if dest.exists():
            shutil.rmtree(dest)
        n = size = 0
        for dp, dn, fn in os.walk(src / part):
            dn[:] = sorted(d for d in dn if d not in SKIP_DIRS)
            rel = Path(dp).relative_to(src / part)
            for f in sorted(fn):
                if any(fnmatch.fnmatch(f, pat) for pat in SKIP_FILES):
                    continue
                ext = os.path.splitext(f)[1].lower()
                if f in BUILT_HERE:
                    continue
                # a fix job folder is on the steps' PYTHONPATH: a script named like a standard module (dis.py,
                # stat.py) replaces it for every import (hat-colour failed so with Python 3.14's argparse)
                if part == 'tools' and f.endswith('.py') and f[:-3] in sys.stdlib_module_names and \
                        '__init__.py' not in fn:
                    sys.exit('refusing %s: named like the standard module %s (rename it)' % (rel / f, f[:-3]))
                if ext in FORBIDDEN:
                    sys.exit('refusing %s: game data or a binary in the recipe' % (rel / f))
                if ext not in ALLOWED and f != 'VERSION':
                    sys.exit('unexpected file type %s (add it to ALLOWED after checking it)' % (rel / f))
                key = (Path(part) / rel / f).as_posix()
                if key in private:
                    out = extra / 'launcher-files' / 'u3mp' / 'patcher' / key
                    out.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(Path(dp) / f, out)
                    n_private += 1
                    continue
                (dest / rel).mkdir(parents=True, exist_ok=True)
                shutil.copyfile(Path(dp) / f, dest / rel / f)
                n += 1
                size += (Path(dp) / f).stat().st_size
        print('%s: %d files, %.1f MB' % (part, n, size / 1e6))
    order = DEST / 'recipe' / 'steps' / 'ORDER.txt'
    kept = [l for l in order.read_text(encoding='utf-8').splitlines() if l.split('#', 1)[0].strip() not in private_steps]
    order.write_text('\n'.join(kept) + '\n', encoding='utf-8', newline='\n')
    print('extra files checkout: %d files, %d steps left out of ORDER.txt' % (n_private, len(private_steps)))
    print('recipe', (DEST / 'recipe' / 'VERSION').read_text().strip())
    sys.path.insert(0, str(DEST))
    import u3mp_version
    cid = u3mp_version.recipe_fingerprint(DEST)
    (DEST / 'recipe' / 'CONTENT_ID').write_text(cid + '\n', encoding='ascii')
    print('CONTENT_ID', cid[:16], '(the launcher compares it with state.json to update an import by itself)')


if __name__ == '__main__':
    main()
