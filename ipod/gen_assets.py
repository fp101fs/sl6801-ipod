#!/usr/bin/env python3
"""
Generate 4x supersampled anti-aliased assets for the 60 FPS Blue iPod Classic UI:
  - gen/clickwheel.h     : 156x156 transparent Click Wheel (LV_IMG_CF_INDEXED_8BIT)
  - gen/album_default.h  : 62x72 3D-tilted Album Art + Glossy Floor Reflection with
                           dual-bank specular sheen palette (LV_IMG_CF_INDEXED_8BIT)
  - gen/sprites.h        : Compact LV_IMG_CF_INDEXED_4BIT baked sprites for
                           Play, Pause, Battery, and 5 Click Wheel quadrant press glows
  - gen/click_pcm.h      : 44.1 kHz soft-limited Click Wheel audio PCM
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


def write_album_sheen_header(img: Image.Image, path: str, sym: str) -> None:
    """Write 62x72 3D-tilted Album Art + Glossy Floor Reflection as LV_IMG_CF_INDEXED_8BIT
    with dual palette banks:
      - indices 0..127   : base colors (index 0 = pure white #FFFFFF background)
      - indices 128..255 : +34% glossy specular sheen versions of indices 0..127
    At runtime (60 FPS), OR'ing 0x80 onto pixels inside the moving diagonal sheen band
    produces a zero-artifact animated glass reflection with zero per-pixel RGB math."""
    w, h = img.size
    rgb = img.convert("RGB")
    # Quantize to 127 colors; index 0 is reserved for pure white background
    pal_img = rgb.quantize(colors=127, method=Image.Quantize.MEDIANCUT)
    raw_pal = list(pal_img.getpalette()[: 127 * 3])
    while len(raw_pal) < 127 * 3:
        raw_pal.append(255)
    rgb_pixels = list(rgb.getdata())
    idx_pixels = list(pal_img.getdata())

    pal_base = [(255, 255, 255)]
    for i in range(127):
        pal_base.append((raw_pal[i * 3], raw_pal[i * 3 + 1], raw_pal[i * 3 + 2]))

    buf = bytearray()
    # Bank 0: normal colors (0..127)
    for r, g, b in pal_base:
        buf += bytes((b, g, r, 0xFF))
    # Bank 1: glossy specular highlight colors (128..255)
    for idx_i, (r, g, b) in enumerate(pal_base):
        if idx_i == 0:
            buf += bytes((255, 255, 255, 0xFF))
        else:
            sr = min(255, int(r + (255 - r) * 0.36))
            sg = min(255, int(g + (255 - g) * 0.36))
            sb = min(255, int(b + (255 - b) * 0.36))
            buf += bytes((sb, sg, sr, 0xFF))

    for (r, g, b), idx in zip(rgb_pixels, idx_pixels):
        if r >= 253 and g >= 253 and b >= 253:
            buf.append(0)
        else:
            buf.append(idx + 1)

    header_u32 = 10 | (w << 10) | (h << 21)
    with open(path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"#define ALBUM_ART_W {w}\n")
        f.write(f"#define ALBUM_ART_H {h}\n")
        f.write(f"#define ALBUM_ART_BYTES {len(buf)}\n\n")
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
    print(f"Generated {path} ({w}x{h}, {len(buf)} bytes, 3D-tilted + sheen dual palette)")


def encode_indexed4(img: Image.Image, matte_rgb=(255, 255, 255)) -> tuple[int, int, bytes]:
    """Encode an RGBA Pillow image to LV_IMG_CF_INDEXED_4BIT (16-color palette, index 0 = transparent)."""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    rgb_img = Image.new("RGB", (w, h), matte_rgb)
    rgb_img.paste(rgba, mask=rgba.split()[3])
    pal_img = rgb_img.quantize(colors=15, method=Image.Quantize.MEDIANCUT)
    raw_pal = list(pal_img.getpalette()[: 15 * 3])
    while len(raw_pal) < 15 * 3:
        raw_pal.append(0)

    alpha = list(rgba.getchannel("A").getdata())
    idxs = list(pal_img.getdata())

    buf = bytearray()
    buf += bytes((0, 0, 0, 0))  # index 0 = transparent
    for i in range(15):
        r, g, b = raw_pal[i * 3 : i * 3 + 3]
        buf += bytes((b, g, r, 0xFF))

    for y in range(h):
        row_off = y * w
        for x in range(0, w, 2):
            i0 = row_off + x
            p0 = 0 if alpha[i0] < 96 else (idxs[i0] + 1)
            if x + 1 < w:
                i1 = i0 + 1
                p1 = 0 if alpha[i1] < 96 else (idxs[i1] + 1)
            else:
                p1 = 0
            buf.append(((p0 & 0xF) << 4) | (p1 & 0xF))

    return w, h, bytes(buf)


def write_sprites_header(sprites: list[tuple[str, Image.Image, tuple[int, int, int]]], path: str) -> None:
    total = 0
    with open(path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        for sym, img, matte in sprites:
            w, h, buf = encode_indexed4(img, matte_rgb=matte)
            total += len(buf)
            hdr = 9 | (w << 10) | (h << 21)  # LV_IMG_CF_INDEXED_4BIT = 9
            f.write(f"static const uint8_t {sym}_map[{len(buf)}] = {{\n")
            for off in range(0, len(buf), 16):
                chunk = buf[off : off + 16]
                f.write("  " + ",".join(f"0x{b:02x}" for b in chunk) + ",\n")
            f.write("};\n")
            f.write(f"static const lv_img_dsc_t {sym} = {{\n")
            f.write(f"  .header = 0x{hdr:08x}u, .data_size = {len(buf)}u, .data = {sym}_map,\n")
            f.write("};\n\n")
    print(f"Generated {path} ({len(sprites)} INDEXED_4BIT sprites, {total} bytes total)")


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

    # Recessed anodized blue center button
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


def make_album_art_3d() -> Image.Image:
    """Render authentic Apple iPod Classic 3D-perspective-tilted Album Art + Glossy Floor Reflection (62x72)."""
    S = 4
    AW, AH = 58 * S, 58 * S
    # 1. Build crisp flat 4x album artwork
    art = Image.new("RGB", (AW, AH))
    apx = art.load()
    for y in range(AH):
        t = y / (AH - 1)
        r = int(236 - 44 * t)
        g = int(242 - 34 * t)
        b = int(252 - 20 * t)
        for x in range(AW):
            apx[x, y] = (r, g, b)

    draw = ImageDraw.Draw(art)
    # Glossy Aqua blue medallion + highlight + crisp white beamed eighth notes
    draw.ellipse((8 * S, 8 * S, 50 * S, 50 * S), fill=(36, 108, 196))
    draw.ellipse((10 * S, 10 * S, 48 * S, 48 * S), fill=(62, 146, 236))
    # Upper glossy dome highlight inside medallion
    draw.pieslice((12 * S, 11 * S, 46 * S, 34 * S), 180, 360, fill=(116, 188, 250))

    w = (255, 255, 255)
    draw.rectangle((23 * S, 18 * S, 41 * S, 22 * S), fill=w)
    draw.rectangle((23 * S, 18 * S, 26 * S, 38 * S), fill=w)
    draw.rectangle((38 * S, 18 * S, 41 * S, 35 * S), fill=w)
    draw.ellipse((17 * S, 34 * S, 26 * S, 41 * S), fill=w)
    draw.ellipse((32 * S, 31 * S, 41 * S, 38 * S), fill=w)
    # Crisp dark bezel border
    draw.rectangle((0, 0, AW - 1, AH - 1), outline=(48, 56, 66), width=S)

    # 2. Project into 3D iPod Classic tilted perspective + glossy floor reflection on pure white
    W, H = 62 * S, 72 * S
    out = Image.new("RGB", (W, H), (255, 255, 255))
    opx = out.load()

    x0, x1 = 2 * S, 58 * S
    span_x = x1 - x0
    # Left edge taller (y=1..57), right edge perspective-foreshortened (y=6..52)
    top_l, bot_l = 1 * S, 57 * S
    top_r, bot_r = 6 * S, 52 * S
    refl_h = 13 * S

    for x in range(x0, x1):
        u = (x - x0) / float(span_x - 1)
        # Subtle perspective foreshortening along u
        u_persp = (u * 1.12) / (1.0 + 0.12 * u)
        sx = min(AW - 1, max(0, int(u_persp * (AW - 1))))

        yt = top_l + (top_r - top_l) * u
        yb = bot_l + (bot_r - bot_l) * u
        col_h = yb - yt

        # Main tilted album cover
        for y in range(int(yt), int(yb) + 1):
            if 0 <= y < H:
                v = (y - yt) / max(1.0, col_h)
                sy = min(AH - 1, max(0, int(v * (AH - 1))))
                r, g, b = apx[sx, sy]
                # Subtle right-side depth shading (authentic 3D tilt lighting)
                shade = 1.0 - 0.11 * u
                opx[x, y] = (int(r * shade), int(g * shade), int(b * shade))

        # Glossy floor reflection fading smoothly into white LCD
        for ry in range(1, refl_h):
            y = int(yb) + ry
            if 0 <= y < H:
                frac = ry / float(refl_h)
                sy = min(AH - 1, max(0, int((1.0 - frac * 0.28) * (AH - 1))))
                r, g, b = apx[sx, sy]
                alpha = 0.42 * ((1.0 - frac) ** 1.6)
                rr = int(255 * (1.0 - alpha) + r * alpha)
                gg = int(255 * (1.0 - alpha) + g * alpha)
                bb = int(255 * (1.0 - alpha) + b * alpha)
                opx[x, y] = (rr, gg, bb)

    return out.resize((62, 72), Image.Resampling.LANCZOS)


def make_play_icon() -> Image.Image:
    S = 4
    img = Image.new("RGBA", (12 * S, 12 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Shadow + glossy blue Apple play triangle
    d.polygon([(3 * S, 2 * S), (3 * S, 11 * S), (10 * S + 1, 6 * S + 2)], fill=(18, 62, 120, 220))
    d.polygon([(2 * S, 1 * S), (2 * S, 10 * S), (10 * S, 5 * S + 2)], fill=(42, 124, 218, 255))
    d.polygon([(3 * S, 2 * S), (3 * S, 6 * S), (8 * S, 5 * S)], fill=(116, 188, 252, 255))
    return img.resize((12, 12), Image.Resampling.LANCZOS)


def make_pause_icon() -> Image.Image:
    S = 4
    img = Image.new("RGBA", (12 * S, 12 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Two glossy blue pause bars
    for bx in (2 * S, 7 * S):
        d.rectangle((bx, 2 * S, bx + 3 * S, 10 * S + 2), fill=(26, 86, 168, 255))
        d.rectangle((bx, 1 * S, bx + 3 * S, 6 * S), fill=(76, 162, 246, 255))
    return img.resize((12, 12), Image.Resampling.LANCZOS)


def make_battery_icon() -> Image.Image:
    S = 4
    img = Image.new("RGBA", (24 * S, 12 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # Metallic outer body
    d.rounded_rectangle((1 * S, 1 * S, 20 * S, 10 * S), radius=2 * S, fill=(232, 238, 244, 255), outline=(66, 76, 88, 255), width=S)
    # Right nub
    d.rectangle((20 * S, 3 * S, 22 * S + 2, 8 * S), fill=(66, 76, 88, 255))
    # Glossy Apple green gradient fill
    px = img.load()
    for y in range(2 * S + 1, 9 * S):
        t = (y - 2 * S) / float(7 * S)
        if t < 0.45:
            r, g, b = int(134 - 40 * t), int(238 - 18 * t), int(146 - 42 * t)
        else:
            r, g, b = int(52 - 18 * t), int(186 - 50 * t), int(68 - 22 * t)
        for x in range(2 * S + 1, 18 * S):
            px[x, y] = (r, g, b, 255)
    return img.resize((24, 12), Image.Resampling.LANCZOS)


def make_wheel_glow(kind: str) -> Image.Image:
    """Create crisp anti-aliased Aqua tactile press glow sprite matching the Click Wheel geometry."""
    S = 4
    if kind == "center":
        W, H = 50 * S, 50 * S
        img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        cx, cy = W // 2, H // 2
        d.ellipse((2 * S, 2 * S, W - 2 * S, H - 2 * S), outline=(146, 212, 255, 255), width=3 * S)
        d.ellipse((5 * S, 5 * S, W - 5 * S, H - 5 * S), outline=(220, 242, 255, 220), width=2 * S)
        return img.resize((50, 50), Image.Resampling.LANCZOS)

    # Quadrant arc pill glow (top/bot: 52x28, left/right: 28x52)
    if kind in ("top", "bot"):
        W, H = 52 * S, 28 * S
        img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((3 * S, 3 * S, W - 3 * S, H - 3 * S), radius=11 * S, fill=(76, 162, 248, 255), outline=(168, 218, 255, 255), width=2 * S)
        d.rounded_rectangle((6 * S, 5 * S, W - 6 * S, H // 2), radius=6 * S, fill=(144, 206, 255, 255))
        return img.resize((52, 28), Image.Resampling.LANCZOS)
    else:
        W, H = 28 * S, 52 * S
        img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((3 * S, 3 * S, W - 3 * S, H - 3 * S), radius=11 * S, fill=(76, 162, 248, 255), outline=(168, 218, 255, 255), width=2 * S)
        d.rounded_rectangle((5 * S, 6 * S, W // 2, H - 6 * S), radius=6 * S, fill=(144, 206, 255, 255))
        return img.resize((28, 52), Image.Resampling.LANCZOS)


def write_click_pcm_header(mp3_path: str, out_path: str) -> None:
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        wav_path = tf.name
    try:
        subprocess.check_call(["afconvert", "-f", "WAVE", "-d", "LEI16@44100", "-c", "1", mp3_path, wav_path])
        with wave.open(wav_path, "rb") as w:
            raw = w.readframes(w.getnframes())
    finally:
        if os.path.exists(wav_path):
            os.remove(wav_path)

    samples = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    peak = max(abs(s) for s in samples) or 1
    # Trim leading silence right up to 2% of peak, take 1024 samples (4 x 256-frame periods @ 44.1kHz = 23.2ms)
    start = next((i for i, s in enumerate(samples) if abs(s) > peak * 0.02), 0)
    start = max(0, start - 16)
    clip = samples[start : start + 1024]
    while len(clip) < 1024:
        clip.append(0)
    # Soft-limited click at 50% volume (peak ~7750) for subtle, crisp tactile feedback
    norm = []
    for s in clip:
        x = (s / peak) * 3.2
        y = math.tanh(x)
        norm.append(max(-32767, min(32767, int(round(y * 7750.0)))))

    with open(out_path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define CLICK_PCM_LEN 1024\n")
        f.write("static const int16_t click_pcm[CLICK_PCM_LEN] = {\n")
        for off in range(0, len(norm), 12):
            f.write("  " + ",".join(str(v) for v in norm[off : off + 12]) + ",\n")
        f.write("};\n")
    rms = math.sqrt(sum(v * v for v in norm) / len(norm))
    print(f"Generated {out_path} ({len(norm)} samples @ 44100 Hz, peak={max(abs(v) for v in norm)}, rms={rms:.0f})")


if __name__ == "__main__":
    write_indexed8_header(make_clickwheel(), os.path.join(OUT_DIR, "clickwheel.h"), "img_clickwheel", transparent_bg=True)
    write_album_sheen_header(make_album_art_3d(), os.path.join(OUT_DIR, "album_default.h"), "img_album_default")
    write_sprites_header(
        [
            ("img_ico_play", make_play_icon(), (220, 224, 232)),
            ("img_ico_pause", make_pause_icon(), (220, 224, 232)),
            ("img_ico_battery", make_battery_icon(), (220, 224, 232)),
            ("img_glow_top", make_wheel_glow("top"), (255, 255, 255)),
            ("img_glow_bot", make_wheel_glow("bot"), (255, 255, 255)),
            ("img_glow_left", make_wheel_glow("left"), (255, 255, 255)),
            ("img_glow_right", make_wheel_glow("right"), (255, 255, 255)),
            ("img_glow_center", make_wheel_glow("center"), (56, 128, 198)),
        ],
        os.path.join(OUT_DIR, "sprites.h"),
    )
    mp3 = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets", "clickwheel.mp3")
    if os.path.exists(mp3):
        write_click_pcm_header(mp3, os.path.join(OUT_DIR, "click_pcm.h"))
