"""Find LVGL v8 fonts (lv_font_t + lv_font_fmt_txt_dsc_t) in the flash dump.

lv_font_t (v8.3): get_glyph_dsc fn, get_glyph_bitmap fn, i16 line_height, i16 base_line,
    u8 subpx, i8 underline_position, i8 underline_thickness, pad, const void *dsc,
    const lv_font_t *fallback, void *user_data  (0x1C bytes)
lv_font_fmt_txt_dsc_t: glyph_bitmap*, glyph_dsc*, cmaps*, kern_dsc*, u16 kern_scale,
    u16 {cmap_num:9, bpp:4, kern_classes:1, bitmap_format:2}, cache*
lv_font_fmt_txt_glyph_dsc_t: u32 {bitmap_index:20, adv_w:12}, u8 box_w, box_h, i8 ofs_x, ofs_y
lv_font_fmt_txt_cmap_t: u32 range_start, u16 range_length, u16 glyph_id_start,
    const u16 *unicode_list, const void *glyph_id_ofs_list, u16 list_length, u8 type (16 bytes)
"""
import struct
import sys
from collections import Counter

FLASH = 0x00C00000
CMAP_TYPES = {0: "FORMAT0_FULL", 1: "SPARSE_FULL", 2: "FORMAT0_TINY", 3: "SPARSE_TINY"}


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def is_flash(d, p):
    return FLASH <= p < FLASH + len(d)


def parse_dsc(d, o):
    gb, gd, cm, kd = struct.unpack_from("<IIII", d, o)
    kern_scale, bits = struct.unpack_from("<HH", d, o + 16)
    return dict(glyph_bitmap=gb, glyph_dsc=gd, cmaps=cm, kern_dsc=kd, kern_scale=kern_scale,
                cmap_num=bits & 0x1FF, bpp=(bits >> 9) & 0xF, kern_classes=(bits >> 13) & 1,
                bitmap_format=(bits >> 14) & 3)


def parse_cmaps(d, dsc):
    out = []
    o = dsc["cmaps"] - FLASH
    for i in range(dsc["cmap_num"]):
        rs, rl, gs, ul, gl, ll, t = struct.unpack_from("<IHHIIHB", d, o + 16 * i)
        out.append(dict(range_start=rs, range_length=rl, glyph_id_start=gs, unicode_list=ul,
                        glyph_id_ofs_list=gl, list_length=ll, type=t))
    return out


def find_fonts(d):
    # Font structs share the same pair of function pointers; collect candidate pairs first.
    pairs = Counter()
    for o in range(0, len(d) - 0x1C, 4):
        a, b = u32(d, o), u32(d, o + 4)
        if a & 1 and b & 1 and is_flash(d, a) and is_flash(d, b) and 0 < abs(a - b) < 0x1000:
            dscp = u32(d, o + 16)
            if is_flash(d, dscp):
                pairs[(a, b)] += 1
    fonts = []
    for (a, b), n in pairs.items():
        for o in range(0, len(d) - 0x1C, 4):
            if u32(d, o) != a or u32(d, o + 4) != b:
                continue
            lh, bl = struct.unpack_from("<hh", d, o + 8)
            dscp = u32(d, o + 16)
            if not (0 < lh < 200 and is_flash(d, dscp)):
                continue
            dsc = parse_dsc(d, dscp - FLASH)
            if not (dsc["bpp"] in (1, 2, 4, 8) and 0 < dsc["cmap_num"] < 64
                    and is_flash(d, dsc["glyph_bitmap"]) and is_flash(d, dsc["glyph_dsc"])):
                continue
            fonts.append(dict(off=o, fn=(a, b), line_height=lh, base_line=bl,
                              fallback=u32(d, o + 20), dsc_off=dscp - FLASH, dsc=dsc,
                              cmaps=parse_cmaps(d, dsc)))
    return fonts


def glyph_count(d, f):
    n = 0
    for c in f["cmaps"]:
        n = max(n, c["glyph_id_start"] + (c["list_length"] if c["type"] in (1, 3) else c["range_length"]))
    return n


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    for f in find_fonts(d):
        ds = f["dsc"]
        print(f"font @{f['off']:#07x} line_h {f['line_height']} base {f['base_line']} bpp {ds['bpp']} "
              f"glyphs {glyph_count(d, f)} bitmaps@{ds['glyph_bitmap'] - FLASH:#07x} "
              f"glyph_dsc@{ds['glyph_dsc'] - FLASH:#07x} fallback {f['fallback']:#x} "
              f"kern {'yes' if ds['kern_dsc'] else 'no'} fmt {ds['bitmap_format']}")
        for c in f["cmaps"]:
            print(f"    cmap U+{c['range_start']:04X}+{c['range_length']} {CMAP_TYPES.get(c['type'], c['type'])} "
                  f"gid {c['glyph_id_start']} list_len {c['list_length']}")
