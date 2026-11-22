"""Build injectable apps into one image.

Usage: build.py [--zig] [-o OUT] <app dir>...   e.g. build.py sdk/apps/flutter sdk/apps/mp3tris
Uses arm-none-eabi-gcc (ARM_GCC_BIN, default C:/Tools/arm-gnu-toolchain/bin), or zig cc from the
ziglang pip package with --zig. Compiles each <app dir>/*.c plus sdk/src/*.c and links them into one
image; the APP menu lists the apps' FW_MENU_ITEM rows in the order the apps are given. Each
<app dir>/assets/X.png becomes a header X.h (visible to that app only) with an lv_img_dsc_t named img_X.

Outputs OUT (default sdk/build): app.elf, app.bin, app.json (and app.map, app.lst with gcc). app.json
is what `assets.py pack --app` reads: load address, flash offset and the BL patches pointing firmware
call sites at app symbols. Patches come from sdk/patches.json plus each app's optional patches.json,
as [{"bl_at": "0x...", "target": "symbol", "expect": "0x..."}].
"""
import json
import os
import struct
import subprocess
import sys

SDK = os.path.dirname(os.path.abspath(__file__))
FLASH = 0xC00000
BASE = 0x00DD0000
GCC_BIN = os.environ.get("ARM_GCC_BIN", "C:/Tools/arm-gnu-toolchain/bin")

COMMON = ["-mthumb", "-Os", "-std=c11", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
          "-g", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
          "-Werror=implicit-function-declaration"]
# The FPU is single precision (FPv4-SP); the firmware and ROM use the hard-float ABI too.
GCC_FLAGS = ["-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard", "-fno-pic",
             "-fno-tree-loop-distribute-patterns", *COMMON]
ZIG_FLAGS = ["-target", "thumb-freestanding-eabihf", "-mcpu=cortex_m4+vfp4d16sp", "-fno-pic", *COMMON]
ZIG = [sys.executable, "-m", "ziglang"]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"failed: {os.path.basename(cmd[0])} {' '.join(cmd[1:3])}")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def compile_obj(zig, src, incs, obj):
    if zig:
        run([*ZIG, "cc", *ZIG_FLAGS, *incs, "-c", src, "-o", obj])
    else:
        run([f"{GCC_BIN}/arm-none-eabi-gcc", *GCC_FLAGS, *incs, "-c", src, "-o", obj])


def link(zig, objs, ld, out, roots):
    """Links objs; roots are the patch targets, kept by --gc-sections (the first is the entry)."""
    elf = os.path.join(out, "app.elf")
    binf = os.path.join(out, "app.bin")
    keep = [f"-Wl,-e,{roots[0]}"] + [f"-Wl,-u,{r}" for r in roots[1:]]
    if zig:
        run([*ZIG, "cc", *ZIG_FLAGS, *objs, "-nostdlib", "-static", "-Wl,--gc-sections", *keep,
             "-Wl,--no-dynamic-linker", "-Wl,-z,norelro", "-T", ld, "-o", elf])
        run([*ZIG, "objcopy", "-O", "binary", "--only-section=.text", elf, binf])
        return elf
    gcc = f"{GCC_BIN}/arm-none-eabi-"
    run([gcc + "gcc", *GCC_FLAGS, *objs, "-nostdlib", "-T", ld, *keep, "-Wl,--gc-sections",
         f"-Wl,-Map={os.path.join(out, 'app.map')}", "-lgcc", "-o", elf])
    run([gcc + "objcopy", "-O", "binary", "--only-section=.text", elf, binf])
    with open(os.path.join(out, "app.lst"), "w") as f:
        f.write(run([gcc + "objdump", "-d", "-S", elf]))
    return elf


def elf_symbols(path):
    """Return {name: value} for defined symbols in an ELF32 little-endian file."""
    d = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    out = {}
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for o in range(s[4], s[4] + s[5], 16):
            name, value, _size, _info, _other, shndx = struct.unpack_from("<IIIBBH", d, o)
            if name and shndx:
                n = d[strtab[4] + name:d.index(0, strtab[4] + name)].decode()
                out[n] = value
    return out


def build_app(zig, app, out, sdk_inc):
    """Compiles one app dir into OUT/obj/<app name>/ and returns its object files."""
    name = os.path.basename(app)
    obj_dir = os.path.join(out, "obj", name)
    gen = os.path.join(obj_dir, "gen")
    os.makedirs(gen, exist_ok=True)
    assets = os.path.join(app, "assets")
    for f in sorted(os.listdir(assets)) if os.path.isdir(assets) else []:
        if f.endswith(".png"):
            run([sys.executable, "-I", os.path.join(SDK, "..", "tools", "img2c.py"), os.path.join(assets, f),
                 os.path.join(gen, f[:-4] + ".h"), "img_" + f[:-4]])
    objs = []
    for f in sorted(os.listdir(app)):
        if f.endswith(".c"):
            obj = os.path.join(obj_dir, f[:-2] + ".o")
            compile_obj(zig, os.path.join(app, f), [*sdk_inc, "-I", app, "-I", gen], obj)
            objs.append(obj)
    if not objs:
        raise SystemExit(f"no .c files in {app}")
    return objs


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--zig", action="store_true", help="build with zig cc instead of arm-none-eabi-gcc")
    ap.add_argument("-o", "--out", default=os.path.join(SDK, "build"), help="output directory")
    ap.add_argument("apps", nargs="+")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    sdk_inc = ["-I", os.path.join(SDK, "include")]

    objs = []
    patches = json.load(open(os.path.join(SDK, "patches.json")))
    for app in map(os.path.abspath, a.apps):
        objs += build_app(a.zig, app, out, sdk_inc)
        extra = os.path.join(app, "patches.json")
        if os.path.exists(extra):
            patches += json.load(open(extra))
    sdk_obj = os.path.join(out, "obj", "sdk")
    os.makedirs(sdk_obj, exist_ok=True)
    for f in sorted(os.listdir(os.path.join(SDK, "src"))):
        if f.endswith(".c"):
            obj = os.path.join(sdk_obj, f[:-2] + ".o")
            compile_obj(a.zig, os.path.join(SDK, "src", f), sdk_inc, obj)
            objs.append(obj)

    elf = link(a.zig, objs, os.path.join(SDK, "app.ld"), out, [p["target"] for p in patches])
    binf = os.path.join(out, "app.bin")
    syms = elf_symbols(elf)
    resolved = []
    for p in patches:
        if p["target"] not in syms:
            raise SystemExit(f"symbol {p['target']} not found")
        resolved.append(dict(bl_at=p["bl_at"], target=f"{syms[p['target']] & ~1:#x}", symbol=p["target"],
                             expect=p.get("expect")))
    info = dict(bin=binf, load=f"{BASE:#x}", flash_offset=f"{BASE - FLASH:#x}",
                size=os.path.getsize(binf), patches=resolved)
    with open(os.path.join(out, "app.json"), "w") as f:
        json.dump(info, f, indent=1)
    n_items = (syms["__fw_menu_end"] - syms["__fw_menu_start"]) // 12
    print(f"built {binf}: {info['size']} bytes at {BASE:#x}, {n_items} menu item(s)")
    for p in resolved:
        print(f"  patch bl at {p['bl_at']} -> {p['symbol']} ({p['target']})")


if __name__ == "__main__":
    main()
