"""Turns the harness's raw 4-bit image dumps into a scaled PNG mock of the screen."""
import sys

from PIL import Image


def decode(buf, w, h):
    pal = [(buf[i * 4 + 2], buf[i * 4 + 1], buf[i * 4]) for i in range(16)]
    px = buf[64:]
    im = Image.new("RGB", (w, h))
    for y in range(h):
        for x in range(w):
            b = px[y * (w // 2) + x // 2]
            im.putpixel((x, y), pal[(b >> 4) if x % 2 == 0 else (b & 15)])
    return im


raw = open(sys.argv[1], "rb").read()
board = decode(raw[:64 + 35 * 140], 70, 140)
nxt = decode(raw[64 + 35 * 140:], 28, 14)
screen = Image.new("RGB", (128, 160), (0x0A, 0x0C, 0x16))
screen.paste((0x4A, 0x5A, 0x8A), (2, 8, 76, 152))
screen.paste((0x14, 0x18, 0x28), (3, 9, 75, 151))
screen.paste(board, (4, 10))
screen.paste((0x4A, 0x5A, 0x8A), (84, 26, 120, 48))
screen.paste((0x14, 0x18, 0x28), (85, 27, 119, 47))
screen.paste(nxt, (88, 30))
screen.resize((128 * 4, 160 * 4), Image.NEAREST).save(sys.argv[2])
