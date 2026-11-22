"""Quick survey of a SL68xx flash dump: header, partitions, entropy map, strings."""
import struct
import sys
import math
from collections import Counter

data = open(sys.argv[1], "rb").read()


def u32(o):
    return struct.unpack_from("<I", data, o)[0]


print("magic", data[:4])
for o in range(4, 0x60, 4):
    print(f"  hdr+{o:#04x}: {u32(o):#010x}")

print("\nentropy / fill map (64 KiB blocks):")
for b in range(0, len(data), 0x10000):
    blk = data[b:b + 0x10000]
    c = Counter(blk)
    ent = -sum(v / len(blk) * math.log2(v / len(blk)) for v in c.values())
    top, cnt = c.most_common(1)[0]
    print(f"  {b:#08x}: entropy {ent:4.2f}  most common {top:#04x} x{cnt}")


def strings(minlen=6):
    cur = bytearray()
    start = 0
    for i, ch in enumerate(data):
        if 0x20 <= ch < 0x7f:
            if not cur:
                start = i
            cur.append(ch)
        else:
            if len(cur) >= minlen:
                yield start, cur.decode()
            cur = bytearray()


print("\nstrings:")
for off, s in strings():
    print(f"  {off:#08x} {s}")
