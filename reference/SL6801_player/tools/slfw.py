"""Parser for SL6801/SL6806 ("Jointbees MP3", yp3_) SPI flash images.

Layout (offsets are into the raw SPI flash dump):
  0x000000  HLKJ bootloader header (0x60 bytes) + bootloader code, loaded to RAM
  hdr+0x20  offset of partition table
  hdr+0x5C  CRC16 of the header's first 0x5C bytes
  partition table: u32 count, u32 version?, then 16-byte entries starting at +16:
      char name[4]; u32 offset; u32 length; u16 crc16 (at +12, or +14 if ver>=0x30)
"""
import struct
import sys


def crc16(buf, c=0xFFFF):
    for b in buf:
        t = (c >> 8) ^ b
        t ^= t >> 4
        c = ((c << 8) ^ t ^ (t << 5) ^ (t << 12)) & 0xFFFF
    return c


class Image:
    def __init__(self, data):
        self.data = bytearray(data)
        if self.data[:4] != b"HLKJ":
            raise ValueError("bad HLKJ magic")

    def u32(self, o):
        return struct.unpack_from("<I", self.data, o)[0]

    def u16(self, o):
        return struct.unpack_from("<H", self.data, o)[0]

    @property
    def boot(self):
        d = self.data
        return dict(
            load=self.u32(4), entry=self.u32(8), off=self.u32(12),
            size=self.u32(0x10), crc=self.u32(0x14), ptab=self.u32(0x20),
        )

    def partitions(self):
        p = self.boot["ptab"]
        n = self.u32(p)
        ver = self.u32(p + 8)
        out = []
        for i in range(n):
            e = p + 16 + 16 * i
            name = bytes(self.data[e:e + 4]).decode("latin1")
            off, ln = self.u32(e + 4), self.u32(e + 8)
            crc_off = e + (14 if ver >= 0x30 else 12)
            out.append(dict(name=name, off=off, size=ln, crc=self.u16(crc_off), crc_off=crc_off))
        return out

    def part(self, name):
        return next(p for p in self.partitions() if p["name"] == name)

    def firm_header(self):
        """FIRM starts with a 0x30-byte header; a block of it is copied to RAM before running."""
        o = self.part("FIRM")["off"]
        return dict(hdr_size=self.u32(o), timestamp=self.u32(o + 4), ram_load=self.u32(o + 0x10),
                    entry=self.u32(o + 0x14), ram_size=self.u32(o + 0x18), ram_crc=self.u32(o + 0x1C),
                    ram_src=o + self.u32(o))

    def tone_files(self):
        """TONE: u32 size + name per entry (0x24 bytes each?), data starts at +0x400."""
        p = self.part("TONE")
        size = self.u32(p["off"])
        name = bytes(self.data[p["off"] + 4:p["off"] + 0x24]).split(b"\0")[0].decode()
        return [(name, p["off"] + 0x400, size)]

    def fix_checksums(self):
        f = self.firm_header()
        struct.pack_into("<I", self.data, self.part("FIRM")["off"] + 0x1C,
                         crc16(self.data[f["ram_src"]:f["ram_src"] + f["ram_size"]]))
        b = self.boot
        struct.pack_into("<I", self.data, 0x14, crc16(self.data[b["off"]:b["off"] + b["size"]]))
        # The boot ROM checks this CRC of the header's first 0x5C bytes.
        struct.pack_into("<I", self.data, 0x5C, crc16(self.data[:0x5C]))
        for p in self.partitions():
            if p["name"] == "PSMP":
                continue
            struct.pack_into("<H", self.data, p["crc_off"],
                             crc16(self.data[p["off"]:p["off"] + p["size"]]))


def cmd_info(img):
    b = img.boot
    calc = crc16(img.data[b["off"]:b["off"] + b["size"]])
    print(f"bootloader: load {b['load']:#x} entry {b['entry']:#x} file {b['off']:#x}+{b['size']:#x} "
          f"crc {b['crc']:#06x} calc {calc:#06x}")
    for p in img.partitions():
        line = f"{p['name']}: off {p['off']:#08x} size {p['size']:#08x} crc {p['crc']:#06x}"
        if p["name"] != "PSMP":
            line += f" calc {crc16(img.data[p['off']:p['off'] + p['size']]):#06x}"
        print(line)
    f = img.firm_header()
    calc = crc16(img.data[f["ram_src"]:f["ram_src"] + f["ram_size"]])
    print(f"FIRM: RAM block {f['ram_size']:#x} bytes -> {f['ram_load']:#x}, entry {f['entry']:#x}, "
          f"crc {f['ram_crc']:#06x} calc {calc:#06x}; rest runs in place at 0xC00000 + flash offset")
    for name, off, size in img.tone_files():
        print(f"TONE file {name!r} at {off:#x} size {size:#x}")


def cmd_extract(img, outdir):
    import os
    os.makedirs(outdir, exist_ok=True)
    b = img.boot
    open(f"{outdir}/boot.bin", "wb").write(img.data[b["off"]:b["off"] + b["size"]])
    for p in img.partitions():
        open(f"{outdir}/{p['name']}.bin", "wb").write(img.data[p["off"]:p["off"] + p["size"]])
    for name, off, size in img.tone_files():
        open(f"{outdir}/tone_{name}", "wb").write(img.data[off:off + size])
    print("extracted to", outdir)


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    e = sub.add_parser("extract")
    e.add_argument("outdir")
    f = sub.add_parser("fixcrc", help="recompute all checksums and write a new image")
    f.add_argument("output")
    a = ap.parse_args()
    img = Image(open(a.image, "rb").read())
    if a.cmd == "info":
        cmd_info(img)
    elif a.cmd == "extract":
        cmd_extract(img, a.outdir)
    elif a.cmd == "fixcrc":
        img.fix_checksums()
        open(a.output, "wb").write(img.data)
        print("wrote", a.output)


if __name__ == "__main__":
    main()
