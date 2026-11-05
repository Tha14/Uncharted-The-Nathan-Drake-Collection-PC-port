"""ASSETS agent PC job #3 (read-only on the PS4 game folder; writes only _claude_transfer/u3text/).
The earlier ps4-text extraction keyed the output by psarc basename, so u1data/u2data/u3data core.psarc collided and
only the Uncharted 1 text landed. This one extracts every text1/ entry of u3data/build/main/core.psarc only.
Run:  cd ~/mnt/Uncharted-windows/_claude_transfer && python3 fetch_u3text.py && tar cf u3text.tar u3text
Stage u3text.tar to /mnt/user-data/uploads/Uncharted-windows/_claude_transfer/u3text.tar"""
import os, sys
T = os.path.expanduser('~/mnt/Uncharted-windows/_claude_transfer')
sys.path.insert(0, T + '/maps/tools')
from psarc import Psarc
p = os.path.join(os.path.expanduser('~/mnt'), 'Uncharted - The Nathan Drake Collection [CUSA02320]', 'u3data', 'build', 'main', 'core.psarc')
O = T + '/u3text'; os.makedirs(O, exist_ok=True)
a = Psarc(p); k = 0
for i, n in enumerate(a.names):
    if n.lstrip('/').startswith('text1/'):
        dst = os.path.join(O, n.lstrip('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(dst):
            open(dst + '.tmp', 'wb').write(a.read(i + 1)); os.replace(dst + '.tmp', dst)
        k += 1
print('u3 text files', k, 'from', p)
