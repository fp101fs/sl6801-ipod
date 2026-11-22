"""Unpack / repack the user-editable assets of the yp3_2.0.43 SL6801 firmware.

  unpack <firmware.bin> <workdir>
      Writes images/*.png, strings.json, fonts/*.png + fonts/*.json, tone/*.mp3 and manifest.json.
  pack <firmware.bin> <workdir> <out.bin>
      Applies whatever was edited in <workdir> to a copy of <firmware.bin>, fixes all checksums.

Changed or grown data is written to the free flash area after the TONE partition, which is
memory-mapped at 0xC00000 + offset like the rest of the flash.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import unicodedata

from PIL import Image as PImage, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slfw import Image, crc16  # noqa: E402
from find_lv_images import scan as scan_images, CF_NAMES  # noqa: E402

FLASH = 0x00C00000
# .data load image in flash and its run address in RAM (from the startup copy table).
DATA_FLASH, DATA_RAM, DATA_RAM_END = 0x39470, 0x80CC40, 0x81AAEC
STR_TABLE, STR_COUNT, LANGS = 0x53154, 219, 10
LANG_CODES = {"Deutsch": "de", "English": "en", "Français": "fr", "Italiano": "it", "日本語": "ja",
              "nederlands": "nl", "Polski": "pl", "Português": "pt", "Română": "ro", "Español": "es"}
STD_GET_GLYPH_DSC = 0xD374ED  # lv_font_get_glyph_dsc_fmt_txt in this build
CHROMA = 0x07E0  # LV_COLOR_CHROMA_KEY (pure green)
UI_FONT = "font_464b0"  # 1bpp 12px font used for all translated UI text


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def ptr_to_off(p):
    """Map a runtime address to a flash offset (flash XIP or .data load image)."""
    if FLASH <= p < FLASH + 0x200000:
        return p - FLASH
    if DATA_RAM <= p < DATA_RAM_END:
        return p - DATA_RAM + DATA_FLASH
    return None


def off_to_ram(o):
    return o - DATA_FLASH + DATA_RAM


def cstr(d, o):
    return bytes(d[o:d.index(0, o)])


def pixels_hash(im):
    im = im.convert("RGBA")
    return hashlib.sha256(im.tobytes() + repr(im.size).encode()).hexdigest()


# ---------------------------------------------------------------- colors

def c565_to_rgb(v):
    r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)


def rgb_to_565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


# ---------------------------------------------------------------- images

def decode_image(d, r):
    w, h, off, cf = r["w"], r["h"], r["off"], r["cf"]
    im = PImage.new("RGBA", (w, h))
    px = im.load()
    if cf in (4, 6):
        for i in range(w * h):
            v = struct.unpack_from(">H", d, off + 2 * i)[0]
            a = 0 if cf == 6 and v == CHROMA else 255
            px[i % w, i // w] = c565_to_rgb(v) + (a,)
    elif cf == 10:
        pal = [(d[off + 4 * i + 2], d[off + 4 * i + 1], d[off + 4 * i], d[off + 4 * i + 3]) for i in range(256)]
        for i in range(w * h):
            px[i % w, i // w] = pal[d[off + 1024 + i]]
    else:
        return None
    return im


def encode_image(im, cf):
    im = im.convert("RGBA")
    w, h = im.size
    out = bytearray()
    if cf in (4, 6):
        raw = im.tobytes()
        for i in range(0, len(raw), 4):
            r, g, b, a = raw[i:i + 4]
            if cf == 6 and a < 128:
                v = CHROMA
            else:
                v = rgb_to_565(r, g, b)
                if cf == 6 and v == CHROMA:
                    v = 0x07C0  # keep opaque pure green from turning transparent
            out += struct.pack(">H", v)
    elif cf == 10:
        raw = im.tobytes()
        px = [raw[i:i + 4] for i in range(0, len(raw), 4)]
        colors = sorted(set(px))
        if len(colors) <= 256:  # exact palette when possible
            index = {c: i for i, c in enumerate(colors)}
            pal = [c for col in colors for c in col]
            idx = bytes(index[p] for p in px)
        else:
            q = im.quantize(256, method=PImage.Quantize.FASTOCTREE)
            pal = q.getpalette("RGBA") or []
            idx = q.tobytes()
        pal += [0] * (1024 - len(pal))
        for i in range(256):
            r, g, b, a = pal[4 * i:4 * i + 4]
            out += bytes((b, g, r, a))
        out += idx
    else:
        raise ValueError(f"unsupported color format {cf}")
    return bytes(out)


def find_images(d):
    return [r for r in scan_images(Image(d)) if r["cf"] in (4, 6, 10)]


# ---------------------------------------------------------------- strings

def read_strings(d):
    table = []
    for lang in range(LANGS):
        row = []
        for i in range(STR_COUNT):
            p = u32(d, STR_TABLE + 4 * (lang * STR_COUNT + i))
            row.append(None if p == 0 else cstr(d, ptr_to_off(p)).decode("utf-8"))
        table.append(row)
    codes = [LANG_CODES[row[0]] for row in table]
    return codes, table


# ---------------------------------------------------------------- fonts

def find_fonts(d):
    """Find font descriptors (lv_font_fmt_txt_dsc_t) and the lv_font_t that use them."""
    fonts = []
    lo, hi = FLASH + 0xE000, FLASH + 0x1FC000

    def is_flash(p):
        return lo <= p < hi

    for o in range(0xE000, 0x182270 - 28, 4):
        gb, gd, cm, kd, _ks, bits, cache = struct.unpack_from("<IIIIHHI", d, o)
        if not (is_flash(gb) and is_flash(gd) and is_flash(cm) and (kd == 0 or is_flash(kd))):
            continue
        cmap_num, bpp = bits & 0x1FF, (bits >> 9) & 0xF
        if cmap_num != 1 or bpp not in (1, 2, 4, 8) or not (0x800000 <= cache < 0x870000):
            continue
        in_data = DATA_FLASH <= o < DATA_FLASH + (DATA_RAM_END - DATA_RAM)
        dsc_addr = off_to_ram(o) if in_data else FLASH + o
        users = [f for f in range(0xE000, 0x182270 - 0x18, 4) if u32(d, f + 16) == dsc_addr
                 and u32(d, f) & 1 and is_flash(u32(d, f))]
        if not users:
            continue
        fo = users[0]
        kind = "lvgl" if u32(d, fo) == STD_GET_GLYPH_DSC else "sl"
        rs, rl, gs, ul, gl, ll, ctype = struct.unpack_from("<IHHIIHB", d, cm - FLASH)
        line_h, base = struct.unpack_from("<hh", d, fo + 8)
        fonts.append(dict(name=f"font_{o:05x}", dsc=o, font=fo, kind=kind, bpp=bpp,
                          line_height=line_h, base_line=base, cmap=cm - FLASH, cache=ptr_to_off(cache),
                          range_start=rs, range_length=rl, glyph_id_start=gs, unicode_list=ul,
                          list_length=ll, cmap_type=ctype, bitmaps=gb - FLASH, glyph_dsc=gd - FLASH))
    return [f for f in fonts if f["kind"] == "sl"]


def font_glyphs(d, f):
    """Return {codepoint: (adv, box_w, box_h, ofs_x, ofs_y, bitmap_bytes)} for an "sl" font."""
    codes = struct.unpack_from(f"<{f['list_length']}H", d, f["unicode_list"] - FLASH)
    out = {}
    for i, c in enumerate(codes):
        bi, adv, bw, bh, ox, oy = struct.unpack_from("<IIHHhh", d, f["glyph_dsc"] + 16 * i)
        n = (bw * bh * f["bpp"] + 7) // 8
        out[c] = (adv, bw, bh, ox, oy, bytes(d[f["bitmaps"] + bi:f["bitmaps"] + bi + n]))
    return out


def glyph_to_image(g, bpp):
    adv, bw, bh, ox, oy, bm = g
    im = PImage.new("L", (max(bw, 1), max(bh, 1)), 0)
    px = im.load()
    for i in range(bw * bh):
        bit = i * bpp
        v = (bm[bit // 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1)
        px[i % bw, i // bw] = v * 255 // ((1 << bpp) - 1)
    return im


def glyph_from_image(im, bpp):
    bw, bh = im.size
    px = im.load()
    bits = bytearray((bw * bh * bpp + 7) // 8)
    mx = (1 << bpp) - 1
    for i in range(bw * bh):
        v = (px[i % bw, i // bw] * mx + 127) // 255
        bit = i * bpp
        bits[bit // 8] |= v << (8 - bpp - bit % 8)
    return bytes(bits)


def font_sheet(d, f, path):
    glyphs = font_glyphs(d, f)
    cols, cell = 32, max(f["line_height"], 8) + 4
    rows = (len(glyphs) + cols - 1) // cols
    sheet = PImage.new("L", (cols * cell, rows * cell), 40)
    for n, (c, g) in enumerate(sorted(glyphs.items())):
        x0, y0 = (n % cols) * cell + 2, (n // cols) * cell + 2
        gi = glyph_to_image(g, f["bpp"])
        top = f["line_height"] - f["base_line"] - g[2] - g[4]
        sheet.paste(gi, (x0 + max(g[3], 0), y0 + max(top, 0)))
    scale = 2 if sheet.width < 2000 else 1
    sheet.resize((sheet.width * scale, sheet.height * scale), PImage.NEAREST).save(path)


def render_ttf_glyphs(ttf, size, codes, bpp, baseline_ofs, y_shift=0):
    """Render codes from a TTF/OTF into "sl" glyph tuples aligned to the given baseline offset."""
    from fontTools.ttLib import TTFont
    tt = TTFont(ttf, fontNumber=0)
    cmap = tt.getBestCmap()
    font = ImageFont.truetype(ttf, size)
    out = {}
    pad_w = 8 if bpp == 1 else 2
    for c in codes:
        if c not in cmap:
            continue
        ch = chr(c)
        canvas = PImage.new("L", (size * 4, size * 4), 0)
        dr = ImageDraw.Draw(canvas)
        dr.fontmode = "1" if bpp == 1 else "L"
        bx, by = size, size * 3  # pen position (left, baseline)
        dr.text((bx, by), ch, fill=255, font=font, anchor="ls")
        adv = round(font.getlength(ch))
        bbox = canvas.getbbox()
        if bbox is None:
            out[c] = (adv, 0, 0, 0, 0, b"")
            continue
        x0, y0, x1, y1 = bbox
        bw = (x1 - x0 + pad_w - 1) // pad_w * pad_w
        crop = PImage.new("L", (bw, y1 - y0), 0)
        crop.paste(canvas.crop((x0, y0, x1, y1)), (0, 0))
        if bpp == 1:
            crop = crop.point(lambda v: 255 if v >= 128 else 0)
        oy = baseline_ofs + (by - y1) + y_shift
        out[c] = (adv, bw, y1 - y0, x0 - bx, oy, glyph_from_image(crop, bpp))
    return out


def font_baseline(glyphs):
    """Estimate the font's baseline as ofs_y of flat-bottomed reference glyphs."""
    for ch in "HIE0":
        if ord(ch) in glyphs:
            return glyphs[ord(ch)][4]
    return 0


# ---------------------------------------------------------------- unpack

def cmd_unpack(fw, work):
    d = bytearray(open(fw, "rb").read())
    img = Image(d)
    for sub in ("images", "fonts", "tone"):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    manifest = dict(firmware=os.path.abspath(fw), firmware_sha256=hashlib.sha256(d).hexdigest(),
                    images={}, fonts={}, tone={})

    for r in find_images(d):
        name = f"img_{r['desc']:05x}_{r['w']}x{r['h']}"
        im = decode_image(d, r)
        im.save(os.path.join(work, "images", name + ".png"))
        manifest["images"][name] = dict(r, cf_name=CF_NAMES[r["cf"]], hash=pixels_hash(im))

    codes, table = read_strings(d)
    entries = [dict(id=i, **{codes[l]: table[l][i] for l in range(LANGS)}) for i in range(STR_COUNT)]
    with open(os.path.join(work, "strings.json"), "w", encoding="utf-8") as f:
        json.dump(dict(languages=codes, strings=entries), f, ensure_ascii=False, indent=1)

    for fnt in find_fonts(d):
        name = fnt["name"]
        glyphs = font_glyphs(d, fnt)
        font_sheet(d, fnt, os.path.join(work, "fonts", name + ".png"))
        cfg = dict(_info=dict(bpp=fnt["bpp"], glyphs=len(glyphs), line_height=fnt["line_height"],
                              base_line=fnt["base_line"], baseline_ofs=font_baseline(glyphs),
                              ui_font=name == UI_FONT),
                   chars="".join(chr(c) for c in sorted(glyphs) if c >= 0x20),
                   rebuild=None)
        with open(os.path.join(work, "fonts", name + ".json"), "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=1)
        manifest["fonts"][name] = fnt

    for name, off, size in img.tone_files():
        data = bytes(d[off:off + size])
        open(os.path.join(work, "tone", name), "wb").write(data)
        manifest["tone"][name] = dict(off=off, size=size, sha256=hashlib.sha256(data).hexdigest())

    with open(os.path.join(work, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print(f"unpacked {len(manifest['images'])} images, {STR_COUNT}x{LANGS} strings, "
          f"{len(manifest['fonts'])} fonts, {len(manifest['tone'])} tone(s) to {work}")


# ---------------------------------------------------------------- pack

class Allocator:
    def __init__(self, d, start, end):
        self.d, self.pos, self.end, self.start = d, start, end, start

    def put(self, data, align=4):
        self.pos = (self.pos + align - 1) // align * align
        if self.pos + len(data) > self.end:
            raise SystemExit(f"out of free flash space ({len(data)} bytes needed)")
        o = self.pos
        self.d[o:o + len(data)] = data
        self.pos += len(data)
        return o


def build_font(d, fnt, cfg, alloc, extra_chars, log):
    orig = font_glyphs(d, fnt)
    rb = cfg["rebuild"]
    want = set(orig) | {ord(c) for c in cfg.get("chars", "")} | {ord(c) for c in rb.get("add_chars", "")}
    want |= extra_chars
    want = {c for c in want if c < 0x10000}
    replace = rb.get("mode", "add_missing") == "replace"
    targets = sorted(want if replace else want - set(orig))
    base = font_baseline(orig)
    new = render_ttf_glyphs(rb["ttf"], rb["size"], targets, fnt["bpp"], base, rb.get("y_shift", 0))
    glyphs = dict(orig)
    glyphs.update(new)
    missing = sorted(c for c in want if c not in glyphs)
    log(f"  {fnt['name']}: {len(new)} glyphs rendered from {os.path.basename(rb['ttf'])}, "
        f"{len(glyphs)} total" + (f", {len(missing)} still missing: "
                                  f"{''.join(chr(c) for c in missing[:40])}" if missing else ""))

    # Bitmap indices stay relative to the original bitmap block: untouched glyphs keep their
    # bitmaps where they are, new ones are appended in the free area (which lies after it).
    orig_codes = struct.unpack_from(f"<{fnt['list_length']}H", d, fnt["unicode_list"] - FLASH)
    orig_index = {c: u32(d, fnt["glyph_dsc"] + 16 * i) for i, c in enumerate(orig_codes)}
    codes = sorted(glyphs)
    gdsc = bytearray()
    for c in codes:
        adv, bw, bh, ox, oy, bm = glyphs[c]
        if c in new:
            index = alloc.put(bm, 1) - fnt["bitmaps"] if bm else 0
        else:
            index = orig_index[c]
        gdsc += struct.pack("<IIHHhh", index, adv, bw, bh, ox, oy)
    ul = alloc.put(struct.pack(f"<{len(codes)}H", *codes))
    gdo = alloc.put(bytes(gdsc))
    cmap = alloc.put(struct.pack("<IHHIIHBx", codes[0], codes[-1], 0, FLASH + ul, 0, len(codes), fnt["cmap_type"]))
    struct.pack_into("<II", d, fnt["dsc"] + 4, FLASH + gdo, FLASH + cmap)
    if fnt["cache"] is not None:  # cache in .bss is zeroed at boot; in .data it has a stale init value
        struct.pack_into("<II", d, fnt["cache"], 0, 0)
    if "line_height" in rb or "base_line" in rb:
        struct.pack_into("<hh", d, fnt["font"] + 8, rb.get("line_height", fnt["line_height"]),
                         rb.get("base_line", fnt["base_line"]))
    return set(glyphs)


def encode_bl(pc, target):
    """Thumb-2 BL (T1) from pc to target, as the two little-endian halfwords."""
    off = target - (pc + 4)
    if not -(1 << 24) <= off < (1 << 24) or off & 1:
        raise ValueError(f"BL target {target:#x} out of range from {pc:#x}")
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    hw1 = 0xF000 | (s << 10) | ((off >> 12) & 0x3FF)
    hw2 = 0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack("<HH", hw1, hw2)


def apply_app(d, orig, app_json, free_start, free_end, log):
    """Place an sdk/build.py app in flash and redirect its BL call sites. Returns the new free_end."""
    from xref import decode_bl
    with open(app_json) as f:
        app = json.load(f)
    code = open(app["bin"], "rb").read()
    off = int(app["flash_offset"], 16)
    if off < free_start or off + len(code) > free_end:
        raise SystemExit(f"app at {off:#x}+{len(code):#x} is outside the free area {free_start:#x}-{free_end:#x}")
    if any(b != 0xFF for b in orig[off:off + len(code)]):
        raise SystemExit(f"flash at {off:#x} is not empty in the original image")
    d[off:off + len(code)] = code
    log(f"app: {len(code)} bytes at flash {off:#x} ({FLASH + off:#x})")
    for p in app["patches"]:
        at, target = int(p["bl_at"], 16), int(p["target"], 16)
        o = at - FLASH
        hw1, hw2 = struct.unpack_from("<HH", orig, o)
        cur = decode_bl(hw1, hw2, at)
        if cur is None or cur[0] != "bl" or (p.get("expect") and cur[1] != int(p["expect"], 16)):
            raise SystemExit(f"unexpected instruction at {at:#x}: {orig[o:o + 4].hex()}")
        d[o:o + 4] = encode_bl(at, target)
        log(f"  bl at {at:#x}: {cur[1]:#x} -> {p['symbol']} ({target:#x})")
    return off


def cmd_pack(fw, work, out, app_json=None):
    img = Image(open(fw, "rb").read())
    d = img.data  # edit the image's own buffer so fix_checksums sees the changes
    with open(os.path.join(work, "manifest.json"), encoding="utf-8") as f:
        man = json.load(f)
    if hashlib.sha256(d).hexdigest() != man["firmware_sha256"]:
        raise SystemExit("firmware does not match the one this workdir was unpacked from")
    orig = bytes(d)
    log = print
    changes = 0

    # TONE first: it may grow and push the start of the free area.
    tone_part = img.part("TONE")
    for name, info in man["tone"].items():
        data = open(os.path.join(work, "tone", name), "rb").read()
        if hashlib.sha256(data).hexdigest() == info["sha256"]:
            continue
        p = tone_part["off"]
        free_end = img.part("PSMP")["off"]
        if p + 0x400 + len(data) > free_end:
            raise SystemExit("tone file too large")
        d[p + 0x400:free_end] = b"\xff" * (free_end - p - 0x400)
        d[p + 0x400:p + 0x400 + len(data)] = data
        struct.pack_into("<I", d, p, len(data))
        struct.pack_into("<I", d, tone_part["crc_off"] - 4, 0x400 + len(data))
        log(f"tone {name}: {info['size']} -> {len(data)} bytes")
        changes += 1
    tone_part = img.part("TONE")
    start = (tone_part["off"] + tone_part["size"] + 0xFFF) // 0x1000 * 0x1000
    end = img.part("PSMP")["off"]
    if app_json:
        end = apply_app(d, orig, app_json, start, end, log)
        changes += 1
    alloc = Allocator(d, start, end)

    # Strings
    codes, table = read_strings(orig)
    with open(os.path.join(work, "strings.json"), encoding="utf-8") as f:
        sj = json.load(f)
    new_strs = {}
    used_chars = set()
    for e in sj["strings"]:
        for l, code in enumerate(codes):
            s = e.get(code)
            if s is not None:
                used_chars |= {ord(c) for c in s}
            if s == table[l][e["id"]]:
                continue
            slot = STR_TABLE + 4 * (l * STR_COUNT + e["id"])
            if s is None:
                struct.pack_into("<I", d, slot, 0)
            else:
                if s not in new_strs:
                    new_strs[s] = alloc.put(s.encode("utf-8") + b"\0", 1)
                struct.pack_into("<I", d, slot, FLASH + new_strs[s])
            log(f"string {e['id']} [{code}]: {table[l][e['id']]!r} -> {s!r}")
            changes += 1

    # Images
    for name, r in man["images"].items():
        im = PImage.open(os.path.join(work, "images", name + ".png"))
        if pixels_hash(im) == r["hash"]:
            continue
        data = encode_image(im, r["cf"])
        w, h = im.size
        if (w, h) == (r["w"], r["h"]):
            d[r["off"]:r["off"] + len(data)] = data
            log(f"image {name}: replaced in place")
        else:
            o = alloc.put(data)
            hdr = r["cf"] | (w << 10) | (h << 21)
            struct.pack_into("<III", d, r["desc"], hdr, len(data), FLASH + o)
            log(f"image {name}: resized to {w}x{h} (layout code may still assume the old size)")
        changes += 1

    # Fonts
    font_chars = {}
    for name, fnt in man["fonts"].items():
        with open(os.path.join(work, "fonts", name + ".json"), encoding="utf-8") as f:
            cfg = json.load(f)
        if cfg.get("rebuild"):
            extra = used_chars if name == UI_FONT and cfg["rebuild"].get("include_strings", True) else set()
            font_chars[name] = build_font(d, fnt, cfg, alloc, extra, log)
            changes += 1
        else:
            font_chars[name] = set(font_glyphs(orig, fnt))

    ui = font_chars.get(UI_FONT, set())
    missing = sorted(c for c in used_chars if c not in ui and c >= 0x20)
    if missing:
        log(f"note: {len(missing)} characters used in strings have no glyph in the UI font: "
            + "".join(chr(c) for c in missing[:60]))

    img.fix_checksums()
    open(out, "wb").write(d)
    sectors = sorted({i // 0x1000 for i in range(len(d)) if d[i] != orig[i]})
    log(f"{changes} change(s); free area used {alloc.pos - alloc.start} of {alloc.end - alloc.start} bytes")
    runs = []
    for s in sectors:
        if runs and runs[-1][1] == s:
            runs[-1][1] = s + 1
        else:
            runs.append([s, s + 1])
    log(f"wrote {out}")
    if runs:
        # Write the whole image: smtlink_dump skips sectors whose contents already match. Upstream
        # smtlink_dump ignores the file offset of write_flash, so per-range writes would be wrong.
        log("changed ranges: " + ", ".join(f"{a * 0x1000:#x}-{b * 0x1000:#x}" for a, b in runs))
        log(f"flash (bootloader mode):\n  smtlink_dump init write_flash 0 0 0 {os.path.basename(out)}")


def cmd_preview(fw, font_name, texts, out, scale=3):
    """Draw text lines with a font from a (packed) image, placing glyphs the way lv_draw_letter does."""
    d = open(fw, "rb").read()
    fnt = next(f for f in find_fonts(d) if f["name"] == font_name)
    glyphs = font_glyphs(d, fnt)
    lh, bl = fnt["line_height"], fnt["base_line"]
    width = max(sum(glyphs.get(ord(c), (lh // 2,))[0] for c in t) for t in texts) + 8
    canvas = PImage.new("L", (width, lh * len(texts) + 8), 0)
    for row, text in enumerate(texts):
        x, y = 4, 4 + row * lh
        for ch in text:
            g = glyphs.get(ord(ch))
            if g is None:  # mark missing glyphs with a hollow box
                ImageDraw.Draw(canvas).rectangle((x, y + 2, x + lh // 2 - 2, y + lh - 3), outline=128)
                x += lh // 2
                continue
            if g[1] and g[2]:
                gi = glyph_to_image(g, fnt["bpp"])
                canvas.paste(gi, (x + g[3], y + (lh - bl) - g[2] - g[4]), gi)
            x += g[0]
    canvas.resize((canvas.width * scale, canvas.height * scale), PImage.NEAREST).save(out)
    print("wrote", out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    u = sub.add_parser("unpack")
    u.add_argument("firmware")
    u.add_argument("workdir")
    p = sub.add_parser("pack")
    p.add_argument("firmware")
    p.add_argument("workdir")
    p.add_argument("output")
    p.add_argument("--app", help="app.json from sdk/build.py to inject")
    v = sub.add_parser("preview", help="render text with a firmware font to PNG")
    v.add_argument("firmware")
    v.add_argument("font", help=f"font name from the workdir, e.g. {UI_FONT}")
    v.add_argument("output")
    v.add_argument("text", nargs="+", help="one argument per line")
    a = ap.parse_args()
    if a.cmd == "unpack":
        cmd_unpack(a.firmware, a.workdir)
    elif a.cmd == "pack":
        cmd_pack(a.firmware, a.workdir, a.output, a.app)
    else:
        cmd_preview(a.firmware, a.font, a.text, a.output)


if __name__ == "__main__":
    main()
