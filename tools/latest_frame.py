from pathlib import Path
from PIL import Image
import re
root=Path(__file__).resolve().parent.parent
folder=max((root/'analysis/uncharted/captures').iterdir(),key=lambda p:p.stat().st_mtime)
frame=max(folder.glob('output*.raw'),key=lambda p:p.stat().st_mtime)
w,h=map(int,re.search(r'_(\d+)x(\d+)\.raw',frame.name).groups())
Image.frombytes('RGBA',(w,h),frame.read_bytes()).convert('RGB').resize((960,540)).save(root/'analysis/current.png')
print(frame)
