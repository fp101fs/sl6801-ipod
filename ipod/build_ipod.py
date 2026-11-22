#!/usr/bin/env python3
"""
Build the complete Blue iPod Classic + Click Wheel + Touch Sensitivity firmware:
  1. Generates 4x supersampled anti-aliased Click Wheel & Album Art assets.
  2. Compiles ipod/ipod_ui.c for ARM Cortex-M4F (Thumb-2, hard-float) via ziglang.
  3. Injects the binary into FIRM's 128 KB free cave at flash offset 0x019000.
  4. Patches the 5 music page BL calls in view_page_create.
  5. Applies the CST726 touch sensitivity patch (reg 0x80 = 0x10, scroll_limit = 20).
  6. Recomputes FIRM CRC16 (CCITT-FALSE) and writes firmware/sl6801_ipod.bin.
  7. Generates scripts/flash_ipod.sh to flash ONLY the modified 4 KB sectors.
"""
import os
import struct
import subprocess
import sys

IPOD_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(IPOD_DIR)
BUILD_DIR = os.path.join(IPOD_DIR, "build")
BACKUP = os.path.join(REPO_DIR, "firmware", "sl6801_backup.bin")
OUTPUT = os.path.join(REPO_DIR, "firmware", "sl6801_ipod.bin")
FLASH_SH = os.path.join(REPO_DIR, "scripts", "flash_ipod.sh")

FLASH_BASE = 0x00C00000
INJECT_OFF = 0x019000
INJECT_VMA = FLASH_BASE + INJECT_OFF
INJECT_MAX = 0x20000  # 128 KB free cave

HOOKS = [
    ("hook_music_menu_create",   0x00D7D56E, 0x00D8B9D8),
    ("hook_music_song_create",   0x00D7D574, 0x00D8CC94),
    ("hook_music_play_create",   0x00D7D57A, 0x00D8BEB0),
    ("hook_music_artist_create", 0x00D7D586, 0x00D8A630),
    ("hook_music_album_create",  0x00D7D58C, 0x00D8A188),
]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"Command failed: {' '.join(cmd[:4])}")
    if r.stdout.strip():
        print(r.stdout.strip())
    if r.stderr.strip():
        print(r.stderr.strip())


def encode_bl(pc: int, target: int) -> bytes:
    off = target - (pc + 4)
    assert -16777216 <= off <= 16777214 and (off & 1) == 0, f"BL out of range: {hex(pc)} -> {hex(target)}"
    s = (off >> 24) & 1
    i1 = (off >> 23) & 1
    i2 = (off >> 22) & 1
    j1 = (~i1 ^ s) & 1
    j2 = (~i2 ^ s) & 1
    imm10 = (off >> 12) & 0x3FF
    imm11 = (off >> 1) & 0x7FF
    return struct.pack("<HH", 0xF000 | (s << 10) | imm10, 0xD000 | (j1 << 13) | (j2 << 11) | imm11)


def encode_b16(pc: int, target: int) -> bytes:
    off = target - (pc + 4)
    assert -2048 <= off <= 2046 and (off & 1) == 0
    return struct.pack("<H", 0xE000 | ((off >> 1) & 0x7FF))


def decode_bl(pc: int, raw4: bytes) -> int:
    hw1, hw2 = struct.unpack("<HH", raw4)
    s = (hw1 >> 10) & 1
    imm10 = hw1 & 0x3FF
    j1 = (hw2 >> 13) & 1
    j2 = (hw2 >> 11) & 1
    imm11 = hw2 & 0x7FF
    i1 = (~(j1 ^ s)) & 1
    i2 = (~(j2 ^ s)) & 1
    off = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1)
    if s:
        off -= (1 << 25)
    return pc + 4 + off


def elf_symbols(path: str) -> dict:
    d = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for s in secs:
        if s[1] != 2:
            continue
        strtab = secs[s[6]]
        for o in range(s[4], s[4] + s[5], 16):
            name, value, _sz, _info, _oth, shndx = struct.unpack_from("<IIIBBH", d, o)
            if name and shndx:
                n = d[strtab[4] + name : d.index(0, strtab[4] + name)].decode()
                out[n] = value
    return out


def crc16_ccitt_false(buf: bytes) -> int:
    crc = 0xFFFF
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def apply_sensitivity_patch(patched: bytearray, th_val: int = 0x10, scroll_limit: int = 20) -> None:
    imm16 = (th_val << 8) | 0x80
    movw_r3 = struct.pack(
        "<HH",
        0xF240 | (((imm16 >> 11) & 1) << 10) | ((imm16 >> 12) & 0xF),
        (((imm16 >> 8) & 0x7) << 12) | (3 << 8) | (imm16 & 0xFF),
    )
    patch1 = bytearray()
    patch1 += encode_bl(0x00DBD560, 0x00DBD568)
    patch1 += encode_b16(0x00DBD564, 0x00DBD58A)
    patch1 += struct.pack("<H", 0xBF00)
    patch1 += struct.pack("<H", 0xB51F)
    patch1 += movw_r3
    patch1 += struct.pack("<HH", 0xF8AD, 0x3008)
    patch1 += struct.pack("<H", 0x2301)
    patch1 += struct.pack("<H", 0x2000)
    patch1 += struct.pack("<H", 0x9300)
    patch1 += struct.pack("<H", 0xAA02)
    patch1 += struct.pack("<H", 0x2302)
    patch1 += struct.pack("<H", 0x2138)
    patch1 += encode_bl(0x00DBD57E, 0x008169DC)
    patch1 += struct.pack("<H", 0xB005)
    patch1 += struct.pack("<HH", 0xF85D, 0xFB04)
    patch1 += struct.pack("<H", 0xBF00)
    assert len(patch1) == 0x2A
    patched[0x1BD560:0x1BD58A] = patch1

    patch2 = bytearray()
    patch2 += struct.pack("<H", 0xBF00)
    patch2 += encode_bl(0x00DBD5EE, 0x00DBD47A)
    patch2 += encode_bl(0x00DBD5F2, 0x00DBD568)
    assert len(patch2) == 0x0A
    patched[0x1BD5EC:0x1BD5F6] = patch2

    assert patched[0x1ADC94] == 0x0A
    patched[0x1ADC94] = scroll_limit


def main() -> None:
    os.makedirs(BUILD_DIR, exist_ok=True)
    # 1. Generate assets
    run([sys.executable, os.path.join(IPOD_DIR, "gen_assets.py")])

    # 2. Compile & link with ziglang
    zig = [sys.executable, "-m", "ziglang"]
    cflags = [
        "-target", "thumb-freestanding-eabihf",
        "-mcpu=cortex_m4+vfp4d16sp",
        "-mthumb", "-Os", "-g", "-std=c11",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic",
        "-ffunction-sections", "-fdata-sections",
        "-Wall", "-Wextra", "-Werror=implicit-function-declaration",
        f"-I{IPOD_DIR}",
    ]
    obj = os.path.join(BUILD_DIR, "ipod_ui.o")
    elf = os.path.join(BUILD_DIR, "ipod_ui.elf")
    binf = os.path.join(BUILD_DIR, "ipod_ui.bin")

    run([*zig, "cc", *cflags, "-c", os.path.join(IPOD_DIR, "ipod_ui.c"), "-o", obj])

    keep = [f"-Wl,-e,{HOOKS[0][0]}"] + [f"-Wl,-u,{name}" for name, _, _ in HOOKS[1:]]
    run([
        *zig, "cc", *cflags, obj,
        "-nostdlib", "-static", "-Wl,--gc-sections",
        "-Wl,--no-dynamic-linker", "-Wl,-z,norelro",
        *keep, "-T", os.path.join(IPOD_DIR, "ipod.ld"), "-o", elf,
    ])
    run([*zig, "objcopy", "-O", "binary", "--only-section=.text", elf, binf])

    code = open(binf, "rb").read()
    assert len(code) <= INJECT_MAX, f"Injected code too large: {len(code)} > {INJECT_MAX}"
    print(f"Compiled iPod UI binary: {len(code)} bytes ({len(code)/1024:.1f} KB / {INJECT_MAX//1024} KB cave)")

    syms = elf_symbols(elf)
    orig = open(BACKUP, "rb").read()
    patched = bytearray(orig)

    # Verify target free cave is all zeros!
    cave = patched[INJECT_OFF : INJECT_OFF + len(code)]
    assert all(b == 0 for b in cave), "Target cave is NOT zero-filled!"
    patched[INJECT_OFF : INJECT_OFF + len(code)] = code

    # Patch the 5 BL calls in view_page_create
    for sym, bl_vma, expect_vma in HOOKS:
        assert sym in syms, f"Missing symbol {sym}"
        target = syms[sym] & ~1
        file_off = bl_vma - FLASH_BASE
        actual = decode_bl(bl_vma, patched[file_off : file_off + 4])
        assert actual == expect_vma, f"Unexpected BL at {hex(bl_vma)}: got {hex(actual)}, expected {hex(expect_vma)}"
        patched[file_off : file_off + 4] = encode_bl(bl_vma, target)
        print(f"  Hooked {sym}: BL at 0x{bl_vma:08x} (0x{expect_vma:08x} -> 0x{target:08x})")

    # Apply touch sensitivity patch
    apply_sensitivity_patch(patched, th_val=0x10, scroll_limit=20)

    # Recompute FIRM CRC16
    crc = crc16_ccitt_false(patched[0x00E000 : 0x00E000 + 0x1EA8F0])
    struct.pack_into("<H", patched, 0x00D01C, crc)
    print(f"Updated FIRM CRC16: 0x{crc:04x}")

    with open(OUTPUT, "wb") as f:
        f.write(patched)

    # Find contiguous changed 4KB sector ranges for ultra-fast flashing
    sectors = [s for s in range(0, len(orig), 4096) if orig[s : s + 4096] != patched[s : s + 4096]]
    ranges = []
    for s in sectors:
        if ranges and ranges[-1][0] + ranges[-1][1] == s:
            ranges[-1] = (ranges[-1][0], ranges[-1][1] + 4096)
        else:
            ranges.append((s, 4096))

    lines = [
        "#!/bin/bash",
        "set -e",
        'REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"',
        '"$REPO_DIR/smartlink_flash/smtlink_dump" init \\',
    ]
    for i, (addr, sz) in enumerate(ranges):
        cont = " \\" if i < len(ranges) - 1 else ""
        lines.append(f'  erase_flash 0x{addr:x} 0x{sz:x} write_flash 0x{addr:x} 0x{addr:x} 0x{sz:x} "$REPO_DIR/firmware/sl6801_ipod.bin"{cont}')
    lines.append('echo "Flashed iPod Classic firmware! Unplug USB and long-press Power to boot."')
    with open(FLASH_SH, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.chmod(FLASH_SH, 0o755)

    total_kb = sum(sz for _, sz in ranges) // 1024
    print(f"Wrote {OUTPUT} and {FLASH_SH} ({total_kb} KB across {len(ranges)} ranges: {[(hex(a), hex(s)) for a, s in ranges]})")


if __name__ == "__main__":
    main()
