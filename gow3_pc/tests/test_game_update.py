from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

import game_update
from mods import is_link


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)


def sfo(title_id):
    """A param.sfo holding only TITLE_ID."""
    key, raw = b'TITLE_ID\0\0\0\0', title_id.encode() + b'\0'
    size = (len(raw) + 3) // 4 * 4
    start = 20 + 16
    return (struct.pack('<4sIIII', b'\0PSF', 0x101, start, start + len(key), 1) +
            struct.pack('<HHIII', 0, 0x0204, len(raw), size, 0) + key + raw.ljust(size, b'\0'))


class GameUpdateTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, True)
        self.base, self.update, self.out = self.root / 'base', self.root / 'update', self.root / 'out'
        write(self.base / 'eboot.bin', 'base eboot')
        write(self.base / 'EXEC/WAD/A.WAD', 'wad')
        write(self.base / 'sce_sys/icon0.png', 'old icon')
        write(self.update / 'eboot.bin', 'update eboot')
        write(self.update / 'SCE_SYS/icon0.png', 'new icon')

    def view(self, ids=('CUSA01623', 'CUSA01623')):
        (self.base / 'sce_sys/param.sfo').write_bytes(sfo(ids[0]))
        self.new_sfo = sfo(ids[1]) + b'update'
        (self.update / 'SCE_SYS/param.sfo').write_bytes(self.new_sfo)
        return game_update.create_view(self.base, self.update, self.out)

    def test_update_files_replace_base_files_and_the_rest_stays(self):
        view = self.view()
        self.assertEqual((view / 'eboot.bin').read_text(), 'update eboot')
        self.assertEqual((view / 'EXEC/WAD/A.WAD').read_text(), 'wad')
        self.assertEqual((view / 'sce_sys/icon0.png').read_text(), 'new icon')
        self.assertEqual((view / 'sce_sys/param.sfo').read_bytes(), self.new_sfo)
        self.assertEqual([p.name for p in view.iterdir() if p.name.lower() == 'sce_sys'], ['sce_sys'])

    def test_folders_the_update_leaves_alone_are_linked_whole(self):
        view = self.view()
        self.assertTrue(is_link(view / 'EXEC'))
        self.assertFalse(is_link(view / 'sce_sys'))  # merged: base and update files together

    def test_removing_the_view_leaves_the_dumps_whole(self):
        view = self.view()
        game_update.remove_view(view)
        self.assertFalse(view.exists())
        self.assertEqual((self.base / 'EXEC/WAD/A.WAD').read_text(), 'wad')
        self.assertEqual((self.base / 'sce_sys/icon0.png').read_text(), 'old icon')
        self.assertEqual((self.update / 'SCE_SYS/param.sfo').read_bytes(), self.new_sfo)

    def test_dumps_are_not_modified(self):
        self.view()
        self.assertEqual((self.base / 'eboot.bin').read_text(), 'base eboot')
        self.assertEqual((self.base / 'sce_sys/icon0.png').read_text(), 'old icon')

    def test_different_titles_and_nested_folders_are_refused(self):
        with self.assertRaises(ValueError):
            self.view(('CUSA01623', 'CUSA01715'))
        (self.base / 'sce_sys/param.sfo').write_bytes(sfo('CUSA01623'))
        (self.base / 'EXEC/sce_sys').mkdir()
        (self.base / 'EXEC/sce_sys/param.sfo').write_bytes(sfo('CUSA01623'))
        write(self.base / 'EXEC/eboot.bin', 'nested eboot')
        with self.assertRaisesRegex(ValueError, 'separate folders'):
            game_update.create_view(self.base, self.base / 'EXEC', self.out)
        self.assertEqual(list(self.out.iterdir()) if self.out.exists() else [], [])


if __name__ == '__main__':
    unittest.main()
