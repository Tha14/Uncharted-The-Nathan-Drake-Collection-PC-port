"""The 248-byte empty PS4 level pak (paktools data/empty-level.pak; overlay -pak stubs, placeholders, lab34-spawners):
a login page and an empty VRAM table, nothing else.  usage: empty_level.py <out.pak>   (PYTHONPATH: paktools)"""
import sys
from ps4writer import PakWriter, make_login_and_vram_page

w = PakWriter()
make_login_and_vram_page(w, w.page(), [], [])
d = w.build()
assert len(d) == 248, len(d)
open(sys.argv[1], 'wb').write(d)
