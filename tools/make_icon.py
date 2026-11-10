"""Draws the project's own icon (no game artwork): a gold compass rose on a dark rounded square, in the launcher's
colours. Writes launcher/assets/icon.png, uncharted.ico (16..256 px), window_icon.bmp, and the copies the Qt window and
the game runtime use. Needs Pillow.  usage: python tools/make_icon.py"""
import math
import shutil
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
BG, RIM, GOLD, GOLD_DARK, INK = (20, 20, 23, 255), (58, 48, 30, 255), (232, 178, 62, 255), (168, 120, 34, 255), (20, 20, 23, 255)


def draw(size=1024):
    s = size
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad, r = s * 0.04, s * 0.22
    d.rounded_rectangle([pad, pad, s - pad, s - pad], radius=r, fill=BG, outline=RIM, width=int(s * 0.018))
    c = s / 2
    # outer ring and tick marks
    ring = s * 0.36
    d.ellipse([c - ring, c - ring, c + ring, c + ring], outline=GOLD, width=int(s * 0.022))
    for i in range(36):
        a = math.radians(i * 10)
        inner = ring * (0.86 if i % 9 == 0 else 0.92)
        d.line([c + inner * math.sin(a), c - inner * math.cos(a), c + ring * math.sin(a), c - ring * math.cos(a)],
               fill=GOLD, width=int(s * (0.012 if i % 9 == 0 else 0.006)))
    # compass rose: four long points (north highlighted) and four short ones
    def point(angle, length, half_width, light, dark):
        a = math.radians(angle)
        tip = (c + length * math.sin(a), c - length * math.cos(a))
        left = (c + half_width * math.sin(a - math.pi / 2), c - half_width * math.cos(a - math.pi / 2))
        right = (c + half_width * math.sin(a + math.pi / 2), c - half_width * math.cos(a + math.pi / 2))
        d.polygon([tip, left, (c, c)], fill=light)
        d.polygon([tip, right, (c, c)], fill=dark)
    for ang in (45, 135, 225, 315):
        point(ang, ring * 0.55, s * 0.045, GOLD_DARK, (120, 86, 26, 255))
    for ang in (90, 180, 270):
        point(ang, ring * 0.82, s * 0.07, GOLD, GOLD_DARK)
    point(0, ring * 0.95, s * 0.075, (255, 214, 120, 255), GOLD)
    hub = s * 0.035
    d.ellipse([c - hub, c - hub, c + hub, c + hub], fill=INK, outline=GOLD, width=int(s * 0.01))
    return im


def main():
    big = draw(1024)
    icon = big.resize((256, 256), Image.LANCZOS)
    assets = ROOT / 'launcher' / 'assets'
    icon.save(assets / 'icon.png')
    icon.save(assets / 'uncharted.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    small = Image.new('RGB', (64, 64), BG[:3])
    small.paste(big.resize((64, 64), Image.LANCZOS), (0, 0), big.resize((64, 64), Image.LANCZOS))
    small.save(assets / 'window_icon.bmp')
    for dst in (ROOT / 'launcher-qt' / 'assets' / 'uncharted.ico', ROOT / 'gow3_pc' / 'packaging' / 'windows' / 'uncharted.ico'):
        shutil.copyfile(assets / 'uncharted.ico', dst)
    print('icons written')


if __name__ == '__main__':
    main()
