import sys,re
sys.dont_write_bytecode=True
sys.path.insert(0,'gh2/u3mp/paktools')
import psarc_lib,inspect
print([n for n in dir(psarc_lib) if not n.startswith('_')])
