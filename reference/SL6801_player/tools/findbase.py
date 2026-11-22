"""Guess the memory address the FIRM partition is mapped at by matching pointers to string starts."""
import struct
import sys
from collections import Counter

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from slfw import Image, crc16  # noqa: E402

img = Image(open(sys.argv[1], "rb").read())
firm = [p for p in img.partitions() if p["name"] == "FIRM"][0]
fo, fs = firm["off"], firm["size"]
blob = bytes(img.data[fo:fo + fs])

hdr = struct.unpack_from("<12I", blob, 0)
print("FIRM header words:", [hex(x) for x in hdr])
for n in (0x1000, hdr[6]):
    print(f"crc16(FIRM[0x30:0x30+{n:#x}]) = {crc16(blob[0x30:0x30 + n]):#06x}",
          f"crc16(FIRM[0:{n:#x}]) = {crc16(blob[0:n]):#06x}")

# string starts in FIRM (offset relative to start of FIRM partition)
starts = set()
for i in range(1, len(blob) - 4):
    if blob[i - 1] == 0 and all(0x20 <= c < 0x7F for c in blob[i:i + 4]):
        starts.add(i)

words = [struct.unpack_from("<I", blob, i)[0] for i in range(0, len(blob) - 3, 4)]
votes = Counter()
for w in words:
    if 0x00A00000 <= w < 0x02000000 or 0x10000000 <= w < 0x20000000 or 0x60000000 <= w < 0x70000000:
        for s in starts:
            pass
cands = Counter()
ptrs = [w for w in words if 0x00900000 <= w < 0x01000000]
start_list = sorted(starts)
# Bases aligned to 4 KiB; test each base by counting pointer hits on string starts.
for base in range(0x00900000, 0x00F00000, 0x1000):
    hits = sum(1 for w in ptrs if (w - base) in starts)
    if hits > 20:
        cands[base] = hits
for base, hits in cands.most_common(8):
    print(f"base {base:#010x}: {hits} pointer->string hits "
          f"(flash offset of FIRM start => flash mapped at {base - fo:#010x})")
