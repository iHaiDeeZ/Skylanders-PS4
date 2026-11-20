"""The README's banner (docs/images/banner.png, 1600 x 480): original art, no game art - a glowing
portal ring with a beam of light (like the app's icon, ps4/sce_sys/icon0.png) and the title in Nunito
(ps4/fonts, SIL Open Font License).

  python3 scripts/make-banner.py
"""
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
W, H = 1600, 480


def vertical_gradient(w, h, top, bottom):
    img = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)))
    return img


def glow_layer(draw_fn, blur, size=(W, H)):
    layer = Image.new('RGBA', size, (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    return layer.filter(ImageFilter.GaussianBlur(blur)) if blur else layer


def main():
    img = vertical_gradient(W, H, (22, 14, 56), (64, 34, 116)).convert('RGBA')

    # stars
    rnd = random.Random(1999)
    stars = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    sd = ImageDraw.Draw(stars)
    for _ in range(170):
        x, y = rnd.randrange(W), rnd.randrange(H)
        if 640 <= x <= 1560 and 90 <= y <= 400:  # (not behind the title)
            continue
        r = rnd.choice((1, 1, 1, 2))
        a = rnd.randrange(70, 200)
        sd.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, a))
    img = Image.alpha_composite(img, stars)

    cx, cy, rx, ry = 330, 330, 235, 72
    # the portal's floor: a blue pool of light
    pool = glow_layer(lambda d: d.ellipse([cx - rx + 30, cy - ry + 10, cx + rx - 30, cy + ry - 10], fill=(70, 170, 255, 170)), 22)
    img = Image.alpha_composite(img, pool)
    core = glow_layer(lambda d: d.ellipse([cx - 110, cy - 28, cx + 110, cy + 28], fill=(200, 240, 255, 200)), 18)
    img = Image.alpha_composite(img, core)

    # the beam
    beam = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    top_y, bot_y = 40, cy
    for y in range(top_y, bot_y):
        t = (y - top_y) / (bot_y - top_y)
        half = 26 + 70 * t
        a = int(150 * t ** 1.5)
        bd.line([(cx - half, y), (cx + half, y)], fill=(210, 235, 255, a))
    img = Image.alpha_composite(img, beam.filter(ImageFilter.GaussianBlur(10)))

    # the ring: a wide orange glow, then the bright ring
    ring_glow = glow_layer(lambda d: d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], outline=(255, 140, 30, 255), width=26), 18)
    img = Image.alpha_composite(img, ring_glow)
    ring = glow_layer(lambda d: d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], outline=(255, 205, 110, 255), width=9), 3)
    img = Image.alpha_composite(img, ring)

    # the title: a gold-to-orange gradient through the letters
    fonts = os.path.join(ROOT, 'ps4', 'fonts')
    bold = ImageFont.truetype(os.path.join(fonts, 'Nunito-ExtraBold.ttf'), 132)
    regular = ImageFont.truetype(os.path.join(fonts, 'Nunito-Regular.ttf'), 54)
    small = ImageFont.truetype(os.path.join(fonts, 'Nunito-Regular.ttf'), 36)
    tx, ty = 660, 92
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).text((tx, ty), 'Skylanders', font=bold, fill=255)
    grad = Image.new('RGBA', (W, H))
    gd = ImageDraw.Draw(grad)
    for x in range(W):
        t = min(1.0, max(0.0, (x - tx) / 820))
        gd.line([(x, 0), (x, H)], fill=(255, int(214 - 90 * t), int(90 - 60 * t), 255))
    shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    shadow.putalpha(mask.filter(ImageFilter.GaussianBlur(14)).point(lambda v: v * 0.8))
    glow = Image.new('RGBA', (W, H), (255, 150, 40, 0))
    glow.putalpha(mask.filter(ImageFilter.GaussianBlur(10)).point(lambda v: v * 0.55))
    img = Image.alpha_composite(img, shadow)
    img = Image.alpha_composite(img, glow)
    title = grad.copy()
    title.putalpha(ImageChops.multiply(mask, Image.new('L', (W, H), 255)))
    img = Image.alpha_composite(img, title)

    d = ImageDraw.Draw(img)
    d.text((tx + 6, ty + 168), "Spyro's Adventure on the PS4", font=regular, fill=(236, 226, 255, 255))
    d.text((tx + 8, ty + 246), 'Wii static recompilation  ·  jailbroken PS4 (GoldHEN)', font=small, fill=(176, 160, 222, 255))

    out = os.path.join(ROOT, 'docs', 'images', 'banner.png')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    img.convert('RGB').save(out, optimize=True)
    print(out)


if __name__ == '__main__':
    main()
