# Draws shakedown.ico: a 3 x 3 grid of cores, one of them failing.
import sys
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64]


def draw(n):
    s = 8
    img = Image.new("RGBA", (n * s, n * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cell, r = n * s / 3, n * s * 0.125
    for i in range(9):
        cx, cy = cell * (i % 3 + 0.5), cell * (i // 3 + 0.5)
        color = (229, 72, 77, 255) if i == 5 else (255, 255, 255, 255)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=color)
    return img.resize((n, n), Image.LANCZOS)


imgs = [draw(n) for n in SIZES]
out = sys.argv[1] if len(sys.argv) > 1 else "shakedown.ico"
imgs[-1].save(out, sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])
