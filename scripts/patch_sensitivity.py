#!/usr/bin/env python3
"""
Build firmware/sl6801_sensitive.bin from firmware/sl6801_backup.bin.

Patches:
  1. drv_tp_open (0x1bd560): replaces debug printf calls with tp_set_sensitivity()
     which writes CST726 register 0x80 (TH_GROUP) = 0x10 over I2C (0x38).
  2. drv_tp_ioctl wake (0x1bd5ec): calls tp_set_sensitivity() after every wake reset.
  3. lv_indev_drv_init (0x1adc94): increases LVGL scroll_limit from 10px to 20px.
  4. Partition table (0x00d01c): recomputes FIRM CCITT-FALSE CRC16.
"""
import os
import struct

REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BACKUP = os.path.join(REPO_DIR, "firmware", "sl6801_backup.bin")
OUTPUT = os.path.join(REPO_DIR, "firmware", "sl6801_sensitive.bin")

def encode_bl(pc: int, target: int) -> bytes:
    off = target - (pc + 4)
    assert -16777216 <= off <= 16777214 and (off & 1) == 0
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

def crc16_ccitt_false(buf: bytes) -> int:
    crc = 0xFFFF
    for b in buf:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def main(th_val: int = 0x10, scroll_limit: int = 20) -> None:
    patched = bytearray(open(BACKUP, "rb").read())

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
    patch1 += struct.pack("<H", 0xB51F)          # push {r0-r4, lr}
    patch1 += movw_r3                            # movw r3, #(th_val<<8 | 0x80)
    patch1 += struct.pack("<HH", 0xF8AD, 0x3008) # strh.w r3, [sp, #8]
    patch1 += struct.pack("<H", 0x2301)          # movs r3, #1
    patch1 += struct.pack("<H", 0x2000)          # movs r0, #0
    patch1 += struct.pack("<H", 0x9300)          # str r3, [sp]
    patch1 += struct.pack("<H", 0xAA02)          # add r2, sp, #8
    patch1 += struct.pack("<H", 0x2302)          # movs r3, #2
    patch1 += struct.pack("<H", 0x2138)          # movs r1, #0x38
    patch1 += encode_bl(0x00DBD57E, 0x008169DC)  # bl i2c_write
    patch1 += struct.pack("<H", 0xB005)          # add sp, #0x14
    patch1 += struct.pack("<HH", 0xF85D, 0xFB04) # ldr pc, [sp], #4
    patch1 += struct.pack("<H", 0xBF00)
    assert len(patch1) == 0x2A
    patched[0x1BD560:0x1BD58A] = patch1

    patch2 = bytearray()
    patch2 += struct.pack("<H", 0xBF00)
    patch2 += encode_bl(0x00DBD5EE, 0x00DBD47A)  # bl tp_drv_cst726_reset
    patch2 += encode_bl(0x00DBD5F2, 0x00DBD568)  # bl tp_set_sensitivity
    assert len(patch2) == 0x0A
    patched[0x1BD5EC:0x1BD5F6] = patch2

    assert patched[0x1ADC94] == 0x0A
    patched[0x1ADC94] = scroll_limit

    crc = crc16_ccitt_false(patched[0x00E000 : 0x00E000 + 0x1EA8F0])
    struct.pack_into("<H", patched, 0x00D01C, crc)

    with open(OUTPUT, "wb") as f:
        f.write(patched)
    print(f"Wrote {OUTPUT} (TH_GROUP=0x{th_val:02x}, scroll_limit={scroll_limit}, FIRM CRC16=0x{crc:04x})")

if __name__ == "__main__":
    main()
