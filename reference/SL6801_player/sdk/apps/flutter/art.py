"""Draws the sprites for Flutter into assets/*.png (run with the project venv).

The hero is a round purple puffball with one big eye, a small beak and a stubby wing.
Frames: wing up, wing down, and a dazed frame for game over.
"""
import os

from PIL import Image, ImageDraw

W, H = 16, 12
S = 8  # supersampling factor
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")

BODY = (156, 92, 220, 255)
BELLY = (214, 180, 250, 255)
OUTLINE = (52, 24, 88, 255)
WING = (118, 58, 180, 255)
BEAK = (255, 170, 40, 255)
WHITE = (255, 255, 255, 255)
BLACK = (20, 10, 30, 255)


def box(x0, y0, x1, y1):
    return (x0 * S, y0 * S, x1 * S - 1, y1 * S - 1)


def frame(wing_up, dazed=False):
    im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse(box(1, 1, 14, 12), fill=OUTLINE)
    d.ellipse(box(1.8, 1.8, 13.2, 11.2), fill=BODY)
    d.ellipse(box(4, 6.5, 11, 11), fill=BELLY)
    d.polygon([(13 * S, 5.5 * S), (16 * S - 1, 7 * S), (13 * S, 8.5 * S)], fill=BEAK)
    if wing_up:
        d.ellipse(box(0, 2.5, 7, 7), fill=OUTLINE)
        d.ellipse(box(0.8, 3.2, 6.2, 6.3), fill=WING)
    else:
        d.ellipse(box(0, 6, 7, 10.5), fill=OUTLINE)
        d.ellipse(box(0.8, 6.7, 6.2, 9.8), fill=WING)
    d.ellipse(box(7.5, 2, 12.5, 7), fill=WHITE)
    if dazed:
        d.line([(8.5 * S, 3 * S), (11.5 * S, 6 * S)], fill=BLACK, width=S)
        d.line([(11.5 * S, 3 * S), (8.5 * S, 6 * S)], fill=BLACK, width=S)
    else:
        d.ellipse(box(9.6, 3, 11.8, 5.6), fill=BLACK)
    small = im.resize((W, H), Image.LANCZOS)
    # Hard alpha edges: the firmware uses chroma keying, not alpha blending.
    a = small.getchannel("A").point(lambda v: 255 if v >= 110 else 0)
    small.putalpha(a)
    return small


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, im in (("bird_up", frame(True)), ("bird_down", frame(False)), ("bird_dazed", frame(False, True))):
        im.save(os.path.join(OUT, name + ".png"))
    sheet = Image.new("RGBA", (W * 3 * 8 + 16, H * 8), (40, 60, 110, 255))
    for i, n in enumerate(("bird_up", "bird_down", "bird_dazed")):
        f = Image.open(os.path.join(OUT, n + ".png")).resize((W * 8, H * 8), Image.NEAREST)
        sheet.alpha_composite(f, (i * (W * 8 + 8), 0))
    sheet.save(os.path.join(os.path.dirname(OUT), "art_preview.png"))
    print("wrote", OUT)
