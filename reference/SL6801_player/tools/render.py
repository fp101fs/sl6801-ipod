"""Render raw RGB565 / LVGL image data from the flash dump to PNG for inspection."""
import struct
import sys
from PIL import Image as PImage

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from slfw import Image  # noqa: E402
from find_lv_images import scan  # noqa: E402


def rgb565(v):
    return ((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31


def render_lv(data, r, swap=True):
    w, h, off, cf = r["w"], r["h"], r["off"], r["cf"]
    im = PImage.new("RGB", (w, h))
    px = im.load()
    if cf in (4, 6):
        for y in range(h):
            for x in range(w):
                v = struct.unpack_from(">H" if swap else "<H", data, off + 2 * (y * w + x))[0]
                px[x, y] = rgb565(v)
    elif cf == 10:
        pal = [tuple(data[off + 4 * i + 2 - c] for c in range(3)) for i in range(256)]
        for y in range(h):
            for x in range(w):
                px[x, y] = pal[data[off + 1024 + y * w + x]]
    else:
        return None
    return im


if __name__ == "__main__":
    img = Image(open(sys.argv[1], "rb").read())
    outdir = sys.argv[2]
    tiles = []
    for r in scan(img):
        im = render_lv(img.data, r)
        if im:
            im.resize((im.width * 4, im.height * 4), PImage.NEAREST).save(
                f"{outdir}/lvimg_{r['desc']:06x}_{r['w']}x{r['h']}.png")
            tiles.append(im)
    sheet = PImage.new("RGB", (sum(t.width for t in tiles) + 4 * len(tiles), max(t.height for t in tiles)), "gray")
    x = 0
    for t in tiles:
        sheet.paste(t, (x, 0))
        x += t.width + 4
    sheet.resize((sheet.width * 3, sheet.height * 3), PImage.NEAREST).save(f"{outdir}/lvimg_sheet.png")
