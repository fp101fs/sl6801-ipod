#!/usr/bin/env python3
"""
Generate anti-aliased iPod Mini/Nano Click Wheel (156x156) and default Album Art (64x64)
as LVGL 8.3 LV_IMG_CF_INDEXED_8BIT C headers.
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen")
os.makedirs(OUT_DIR, exist_ok=True)


def write_indexed8_header(img: Image.Image, path: str, sym: str) -> None:
    pal_img = img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT)
    w, h = pal_img.size
    raw_pal = pal_img.getpalette()  # 768 bytes R,G,B
    pixels = bytes(pal_img.getdata())

    # LVGL LV_IMG_CF_INDEXED_8BIT: 256 * 4 bytes (B, G, R, A=0xFF) + w*h pixel indices
    buf = bytearray()
    for i in range(256):
        r, g, b = raw_pal[i * 3 : i * 3 + 3]
        buf += bytes((b, g, r, 0xFF))
    buf += pixels

    header_u32 = 10 | (w << 10) | (h << 21)  # LV_IMG_CF_INDEXED_8BIT = 10
    with open(path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"static const uint8_t {sym}_map[{len(buf)}] = {{\n")
        for off in range(0, len(buf), 16):
            chunk = buf[off : off + 16]
            f.write("  " + ",".join(f"0x{b:02x}" for b in chunk) + ",\n")
        f.write("};\n\n")
        f.write(f"static const lv_img_dsc_t {sym} = {{\n")
        f.write(f"  .header = 0x{header_u32:08x}u,\n")
        f.write(f"  .data_size = {len(buf)}u,\n")
        f.write(f"  .data = {sym}_map,\n")
        f.write("};\n")
    print(f"Generated {path} ({w}x{h}, {len(buf)} bytes)")


def make_clickwheel() -> Image.Image:
    # Render at 4x (624x624) for buttery-smooth anti-aliasing, then downsample to 156x156
    S = 4
    W, H = 156 * S, 156 * S
    cx, cy = W // 2, H // 2
    img = Image.new("RGB", (W, H))
    px = img.load()

    # Background anodized blue gradient matching bottom housing (y=157..313 of 320)
    for y in range(H):
        t = y / (H - 1)
        r = int(68 - 22 * t)
        g = int(142 - 36 * t)
        b = int(208 - 34 * t)
        for x in range(W):
            px[x, y] = (r, g, b)

    draw = ImageDraw.Draw(img)

    # Subtle outer bevel shadow around wheel
    r_out = 73 * S
    r_in = 25 * S
    draw.ellipse((cx - r_out - 2 * S, cy - r_out - 1 * S, cx + r_out + 2 * S, cy + r_out + 3 * S), fill=(30, 82, 138))

    # Pure crisp white Click Wheel ring with tiny off-white inner shading
    draw.ellipse((cx - r_out, cy - r_out, cx + r_out, cy + r_out), fill=(250, 251, 253))
    draw.ellipse((cx - r_out + S, cy - r_out + S, cx + r_out - S, cy + r_out - S), fill=(255, 255, 255))

    # Recessed anodized blue center button (matches photo!)
    draw.ellipse((cx - r_in - S, cy - r_in - S, cx + r_in + S, cy + r_in + S), fill=(28, 76, 128))
    for y in range(cy - r_in, cy + r_in + 1):
        for x in range(cx - r_in, cx + r_in + 1):
            dx, dy = x - cx, y - cy
            d2 = dx * dx + dy * dy
            if d2 <= r_in * r_in:
                # Subtle radial metallic sheen
                nx = (dx + r_in * 0.3) / (r_in * 1.4)
                ny = (dy + r_in * 0.3) / (r_in * 1.4)
                rad = min(1.0, math.hypot(nx, ny))
                r = int(max(24, min(110, 86 - 52 * rad)))
                g = int(max(82, min(176, 158 - 64 * rad)))
                b = int(max(142, min(226, 218 - 62 * rad)))
                px[x, y] = (r, g, b)

    # Crisp iPod light-gray markings (#B6BCC4)
    lbl = (176, 182, 190)

    # Top: "MENU"
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 12 * S)
    except Exception:
        font = ImageFont.load_default()
    tb = draw.textbbox((0, 0), "MENU", font=font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    draw.text((cx - tw // 2, cy - 58 * S - th // 2), "MENU", fill=lbl, font=font)

    # Left: |<<  (bar + two left triangles)
    lx, ly = cx - 52 * S, cy
    draw.rectangle((lx - 9 * S, ly - 5 * S, lx - 7 * S, ly + 5 * S), fill=lbl)
    draw.polygon([(lx - 7 * S, ly), (lx + 1 * S, ly - 5 * S), (lx + 1 * S, ly + 5 * S)], fill=lbl)
    draw.polygon([(lx + 1 * S, ly), (lx + 9 * S, ly - 5 * S), (lx + 9 * S, ly + 5 * S)], fill=lbl)

    # Right: >>|  (two right triangles + bar)
    rx, ry = cx + 52 * S, cy
    draw.polygon([(rx - 9 * S, ry - 5 * S), (rx - 9 * S, ry + 5 * S), (rx - 1 * S, ry)], fill=lbl)
    draw.polygon([(rx - 1 * S, ry - 5 * S), (rx - 1 * S, ry + 5 * S), (rx + 7 * S, ry)], fill=lbl)
    draw.rectangle((rx + 7 * S, ry - 5 * S, rx + 9 * S, ry + 5 * S), fill=lbl)

    # Bottom: > || (play triangle + two pause bars)
    bx, by = cx, cy + 53 * S
    draw.polygon([(bx - 10 * S, by - 5 * S), (bx - 10 * S, by + 5 * S), (bx - 2 * S, by)], fill=lbl)
    draw.rectangle((bx + 2 * S, by - 5 * S, bx + 4 * S, by + 5 * S), fill=lbl)
    draw.rectangle((bx + 7 * S, by - 5 * S, bx + 9 * S, by + 5 * S), fill=lbl)

    return img.resize((156, 156), Image.Resampling.LANCZOS)


def make_album_art() -> Image.Image:
    S = 4
    W, H = 64 * S, 64 * S
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        t = y / (H - 1)
        r = int(232 - 36 * t)
        g = int(238 - 30 * t)
        b = int(246 - 22 * t)
        for x in range(W):
            px[x, y] = (r, g, b)

    draw = ImageDraw.Draw(img)
    # Glossy blue circle badge + white beamed eighth notes
    cx, cy = W // 2, H // 2
    draw.ellipse((10 * S, 10 * S, 54 * S, 54 * S), fill=(48, 126, 208))
    draw.ellipse((12 * S, 12 * S, 52 * S, 52 * S), fill=(68, 150, 232))
    # Musical note icon (white)
    w = (255, 255, 255)
    draw.rectangle((26 * S, 20 * S, 44 * S, 24 * S), fill=w)
    draw.rectangle((26 * S, 20 * S, 29 * S, 41 * S), fill=w)
    draw.rectangle((41 * S, 20 * S, 44 * S, 38 * S), fill=w)
    draw.ellipse((20 * S, 37 * S, 29 * S, 44 * S), fill=w)
    draw.ellipse((35 * S, 34 * S, 44 * S, 41 * S), fill=w)
    # Crisp dark outer border
    draw.rectangle((0, 0, W - 1, H - 1), outline=(60, 66, 74), width=S)
    return img.resize((64, 64), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    write_indexed8_header(make_clickwheel(), os.path.join(OUT_DIR, "clickwheel.h"), "img_clickwheel")
    write_indexed8_header(make_album_art(), os.path.join(OUT_DIR, "album_default.h"), "img_album_default")
