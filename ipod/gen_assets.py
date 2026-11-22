#!/usr/bin/env python3
"""
Generate anti-aliased iPod Mini/Nano Click Wheel (156x156) and default Album Art (64x64)
as LVGL 8.3 LV_IMG_CF_INDEXED_8BIT C headers.
"""
import math
import os
import struct
import subprocess
import tempfile
import wave
from PIL import Image, ImageDraw, ImageFont

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen")
os.makedirs(OUT_DIR, exist_ok=True)


def write_indexed8_header(img: Image.Image, path: str, sym: str, transparent_bg: bool = False) -> None:
    w, h = img.size
    if transparent_bg:
        rgba = img.convert("RGBA")
        # Reserve palette index 0 for fully transparent; quantize opaque pixels to 255 colors
        rgb_img = Image.new("RGB", (w, h), (40, 100, 164))
        rgb_img.paste(rgba, mask=rgba.split()[3])
        pal_img = rgb_img.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
        raw_pal = pal_img.getpalette()
        alpha_data = list(rgba.getchannel("A").getdata())
        idx_data = list(pal_img.getdata())

        buf = bytearray()
        buf += bytes((0, 0, 0, 0))  # index 0 = transparent
        for i in range(255):
            r, g, b = raw_pal[i * 3 : i * 3 + 3]
            buf += bytes((b, g, r, 0xFF))
        for a, idx in zip(alpha_data, idx_data):
            buf.append(0 if a < 128 else (idx + 1))
    else:
        pal_img = img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT)
        raw_pal = pal_img.getpalette()
        pixels = bytes(pal_img.getdata())
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
    print(f"Generated {path} ({w}x{h}, {len(buf)} bytes, transparent={transparent_bg})")


def make_clickwheel() -> Image.Image:
    # Render at 4x (624x624) with transparent background outside wheel, then downsample to 156x156
    S = 4
    W, H = 156 * S, 156 * S
    cx, cy = W // 2, H // 2
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = img.load()
    draw = ImageDraw.Draw(img)

    # Subtle outer bevel shadow around wheel
    r_out = 73 * S
    r_in = 25 * S
    draw.ellipse(
        (cx - r_out - 2 * S, cy - r_out - 1 * S, cx + r_out + 2 * S, cy + r_out + 3 * S),
        fill=(24, 72, 126, 255),
    )

    # Pure crisp white Click Wheel ring
    draw.ellipse((cx - r_out, cy - r_out, cx + r_out, cy + r_out), fill=(248, 250, 252, 255))
    draw.ellipse((cx - r_out + S, cy - r_out + S, cx + r_out - S, cy + r_out - S), fill=(255, 255, 255, 255))

    # Recessed anodized blue center button (matches photo!)
    draw.ellipse((cx - r_in - S, cy - r_in - S, cx + r_in + S, cy + r_in + S), fill=(28, 76, 128, 255))
    for y in range(cy - r_in, cy + r_in + 1):
        for x in range(cx - r_in, cx + r_in + 1):
            dx, dy = x - cx, y - cy
            d2 = dx * dx + dy * dy
            if d2 <= r_in * r_in:
                nx = (dx + r_in * 0.3) / (r_in * 1.4)
                ny = (dy + r_in * 0.3) / (r_in * 1.4)
                rad = min(1.0, math.hypot(nx, ny))
                r = int(max(24, min(110, 86 - 52 * rad)))
                g = int(max(82, min(176, 158 - 64 * rad)))
                b = int(max(142, min(226, 218 - 62 * rad)))
                px[x, y] = (r, g, b, 255)

    # Crisp iPod light-gray markings
    lbl = (176, 182, 190, 255)

    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 12 * S)
    except Exception:
        font = ImageFont.load_default()
    tb = draw.textbbox((0, 0), "MENU", font=font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    draw.text((cx - tw // 2, cy - 58 * S - th // 2), "MENU", fill=lbl, font=font)

    lx, ly = cx - 52 * S, cy
    draw.rectangle((lx - 9 * S, ly - 5 * S, lx - 7 * S, ly + 5 * S), fill=lbl)
    draw.polygon([(lx - 7 * S, ly), (lx + 1 * S, ly - 5 * S), (lx + 1 * S, ly + 5 * S)], fill=lbl)
    draw.polygon([(lx + 1 * S, ly), (lx + 9 * S, ly - 5 * S), (lx + 9 * S, ly + 5 * S)], fill=lbl)

    rx, ry = cx + 52 * S, cy
    draw.polygon([(rx - 9 * S, ry - 5 * S), (rx - 9 * S, ry + 5 * S), (rx - 1 * S, ry)], fill=lbl)
    draw.polygon([(rx - 1 * S, ry - 5 * S), (rx - 1 * S, ry + 5 * S), (rx + 7 * S, ry)], fill=lbl)
    draw.rectangle((rx + 7 * S, ry - 5 * S, rx + 9 * S, ry + 5 * S), fill=lbl)

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


def write_click_pcm_header(mp3_path: str, out_path: str) -> None:
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        wav_path = tf.name
    try:
        subprocess.check_call(["afconvert", "-f", "WAVE", "-d", "LEI16@22050", "-c", "1", mp3_path, wav_path])
        with wave.open(wav_path, "rb") as w:
            raw = w.readframes(w.getnframes())
    finally:
        if os.path.exists(wav_path):
            os.remove(wav_path)

    samples = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    peak = max(abs(s) for s in samples) or 1
    # Trim leading silence right up to 2% of peak, take 768 samples (3 x 256-frame periods = 34.8ms)
    start = next((i for i, s in enumerate(samples) if abs(s) > peak * 0.02), 0)
    start = max(0, start - 8)
    clip = samples[start : start + 768]
    while len(clip) < 768:
        clip.append(0)
    # Normalize to crisp, comfortable iPod click level (~24000 peak)
    scale = 24000.0 / peak
    norm = [max(-32767, min(32767, int(round(s * scale)))) for s in clip]

    with open(out_path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define CLICK_PCM_LEN 768\n")
        f.write("static const int16_t click_pcm[CLICK_PCM_LEN] = {\n")
        for off in range(0, len(norm), 12):
            f.write("  " + ",".join(str(v) for v in norm[off : off + 12]) + ",\n")
        f.write("};\n")
    print(f"Generated {out_path} ({len(norm)} samples @ 22050 Hz, {len(norm) * 2} bytes)")


if __name__ == "__main__":
    write_indexed8_header(make_clickwheel(), os.path.join(OUT_DIR, "clickwheel.h"), "img_clickwheel", transparent_bg=True)
    write_indexed8_header(make_album_art(), os.path.join(OUT_DIR, "album_default.h"), "img_album_default", transparent_bg=False)
    mp3 = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets", "clickwheel.mp3")
    if os.path.exists(mp3):
        write_click_pcm_header(mp3, os.path.join(OUT_DIR, "click_pcm.h"))
