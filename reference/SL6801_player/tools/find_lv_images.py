"""Scan the FIRM partition for LVGL v8 lv_img_dsc_t structs.

lv_img_dsc_t (v8): { u32 header; u32 data_size; const u8 *data; }
header bits: cf:5, always_zero:3, reserved:2, w:11, h:11
"""
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from slfw import Image  # noqa: E402

FLASH_BASE = 0x00C00000

CF_NAMES = {1: "RAW", 2: "RAW_ALPHA", 3: "RAW_CHROMA", 4: "TRUE_COLOR", 5: "TRUE_COLOR_ALPHA",
            6: "TRUE_COLOR_CHROMA", 7: "I1", 8: "I2", 9: "I4", 10: "I8",
            11: "A1", 12: "A2", 13: "A4", 14: "A8"}


def expected_size(cf, w, h):
    if cf in (4, 6):
        return w * h * 2
    if cf == 5:
        return w * h * 3
    if cf in (7, 8, 9, 10):
        bpp = {7: 1, 8: 2, 9: 4, 10: 8}[cf]
        return 4 * (1 << bpp) + ((w * bpp + 7) // 8) * h
    if cf in (11, 12, 13, 14):
        bpp = {11: 1, 12: 2, 13: 4, 14: 8}[cf]
        return ((w * bpp + 7) // 8) * h
    return None


def scan(img):
    data = img.data
    lo, hi = FLASH_BASE, FLASH_BASE + len(data)
    found = []
    for o in range(0xE000, len(data) - 12, 4):
        hdr, size, ptr = struct.unpack_from("<III", data, o)
        cf = hdr & 0x1F
        if cf not in CF_NAMES or (hdr >> 5) & 7:
            continue
        w, h = (hdr >> 10) & 0x7FF, (hdr >> 21) & 0x7FF
        if not (w and h) or not (lo <= ptr < hi) or not size or ptr - lo + size > len(data):
            continue
        exp = expected_size(cf, w, h)
        if exp is not None and exp != size:
            continue
        if exp is None and size > w * h * 4:
            continue
        found.append(dict(desc=o, cf=cf, w=w, h=h, size=size, off=ptr - lo))
    return found


if __name__ == "__main__":
    img = Image(open(sys.argv[1], "rb").read())
    res = scan(img)
    print(len(res), "image descriptors")
    print(Counter(CF_NAMES[r["cf"]] for r in res))
    print(Counter((r["w"], r["h"]) for r in res).most_common(15))
    for r in res[:40]:
        print(f"desc@{r['desc']:#08x} {CF_NAMES[r['cf']]:<18} {r['w']}x{r['h']} "
              f"size {r['size']:#x} data@{r['off']:#08x}")
