import sys,struct,re
sys.path.insert(0,'gh2/u3mp/paktools')
from dc import DC,sid,dump
c=DC('_ps3menu.bin')
print(len(c.syms),len(c.ptrs),hex(c.reloc))
T={}
for n in ['PAGE_EULA','PAGE_EULA_CONFIRM','PAGE_OPTIONS','BUTTON_OPTIONS_EULA','EULA_PICTURES','MENU_TITLE_EULA']:
  s=c.sym(n);print(n,hex(sid(n)),s and (hex(s[0]),hex(s[1])))
# occurrences of each id
for n in ['PAGE_EULA','PAGE_EULA_CONFIRM','BUTTON_OPTIONS_EULA','EULA_PICTURES','MENU_TITLE_EULA']:
  b=struct.pack('>I',sid(n));print(n,[hex(m.start()) for m in re.finditer(re.escape(b),c.d)])
