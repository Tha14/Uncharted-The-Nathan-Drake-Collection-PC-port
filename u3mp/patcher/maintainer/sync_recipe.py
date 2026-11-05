"""sync_recipe.py - (maintainers) copy the overlay recipe and its tools into u3mp/patcher/{recipe,tools}.

  python sync_recipe.py "<...>/PKG Converter"

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
ALLOWED = ('.py', '.pyi', '.txt', '.json', '.tsv', '.md', '.sh', '.cmd', '.gz', '.typed', '')
FORBIDDEN = ('.pak', '.psarc', '.bin', '.dds', '.png', '.jpg', '.wav', '.mp3', '.at9', '.elf', '.self', '.exe', '.dll',
             '.pyd', '.pkg', '.sfo', '.dc', '.dci', '.bnk', '.vtex')


def main():
    src = Path(sys.argv[1])
    if not (src / 'recipe' / 'rebuild_overlays.py').is_file():
        sys.exit('not a PKG Converter folder: ' + str(src))
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
                if ext in FORBIDDEN:
                    sys.exit('refusing %s: game data or a binary in the recipe' % (rel / f))
                if ext not in ALLOWED and f != 'VERSION':
                    sys.exit('unexpected file type %s (add it to ALLOWED after checking it)' % (rel / f))
                (dest / rel).mkdir(parents=True, exist_ok=True)
                shutil.copyfile(Path(dp) / f, dest / rel / f)
                n += 1
                size += (Path(dp) / f).stat().st_size
        print('%s: %d files, %.1f MB' % (part, n, size / 1e6))
    print('recipe', (DEST / 'recipe' / 'VERSION').read_text().strip())


if __name__ == '__main__':
    main()
