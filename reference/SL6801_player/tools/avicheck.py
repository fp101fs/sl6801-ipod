"""Check whether an AVI file can be played by the firmware's video player.

Usage: avicheck.py file.avi [...]

Applies the same checks as the firmware (yp3_2.0.43, functions around 0xCF2DBC-0xCF3500):
- RIFF 'AVI ' starting with LIST hdrl / avih, then one or two stream lists right after avih:
  a video stream ('vids', handler 'MJPG') and optionally an audio stream ('auds'), in either order.
  If a second LIST follows the first stream, it must be the other stream.
- 'movi' must appear in the first 0x7800 bytes, and the first audio chunk too if there is audio.
- Video exactly 128x160; audio, if present, has 2 channels (played as 16-bit PCM).
- Inside movi only ..dc / ..wb chunks (at most 0x7800 bytes each), ending at idx1 (or ix00).
"""
import struct
import sys

HEAD = 0x7800


def fourcc(d, o):
    return d[o:o + 4].decode("latin1")


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def check(path):
    data = open(path, "rb").read()
    d = data[:HEAD]
    errs, info = [], {}
    if fourcc(d, 0) != "RIFF" or fourcc(d, 8) != "AVI ":
        return ["not a RIFF AVI file"], info
    if fourcc(d, 12) != "LIST" or fourcc(d, 20) != "hdrl" or fourcc(d, 24) != "avih":
        return ["file must start with LIST hdrl / avih"], info
    usec = u32(d, 32)
    info["fps"] = round(1e6 / usec, 2) if usec else 0
    p = 0x20 + u32(d, 28)

    def stream(o):
        if fourcc(d, o) != "LIST" or fourcc(d, o + 8) != "strl" or fourcc(d, o + 12) != "strh":
            return None
        strh_size = u32(d, o + 16)
        strf = o + 0x14 + strh_size
        if fourcc(d, strf) != "strf":
            return dict(type="?", error="strf does not follow strh")
        return dict(type=fourcc(d, o + 0x14), handler=fourcc(d, o + 0x18), strf=strf, next=o + u32(d, o + 4) + 8)

    s1 = stream(p)
    if not s1:
        return [f"expected LIST strl / strh right after avih (at {p:#x}), found {fourcc(d, p)!r}"], info
    streams = [s1]
    if fourcc(d, s1["next"]) == "LIST":
        s2 = stream(s1["next"])
        if not s2 or s2["type"] not in ("vids", "auds") or s2["type"] == s1["type"]:
            what = fourcc(d, s1["next"] + 8) if not s2 else s2["type"]
            errs.append(f"a LIST right after the first stream must be the other stream, found {what!r} "
                        "(a file without audio needs the next LIST to be something else, which ffmpeg's "
                        "'odml' list is not; add an audio track)")
        else:
            streams.append(s2)
    video = next((s for s in streams if s["type"] == "vids"), None)
    audio = next((s for s in streams if s["type"] == "auds"), None)
    if not video:
        errs.append("no video stream")
    else:
        f = video["strf"]
        w, h, comp = u32(d, f + 12), u32(d, f + 16), fourcc(d, f + 24)
        info.update(width=w, height=h)
        handler = video["handler"] if video is streams[0] else comp
        if handler != "MJPG":
            errs.append(f"video codec must be MJPG, found {handler!r}")
        if (w, h) != (128, 160):
            errs.append(f"video must be exactly 128x160, found {w}x{h}")
    if audio:
        f = audio["strf"]
        info.update(audio_format=u16(d, f + 8), channels=u16(d, f + 10), rate=u32(d, f + 12),
                    bits=u16(d, f + 22))
        if info["channels"] != 2:
            errs.append(f"audio must have 2 channels, found {info['channels']}")
        if info["audio_format"] != 1 or info["bits"] != 16:
            errs.append(f"audio is played as 16-bit PCM, found format {info['audio_format']} "
                        f"with {info['bits']} bits")

    movi = d.find(b"movi")
    if movi < 0:
        errs.append("'movi' not within the first 0x7800 bytes")
        return errs, info
    if audio:
        tag = b"01wb" if video is streams[0] else b"00wb"
        if d.find(tag, movi) < 0:
            errs.append(f"no {tag.decode()} audio chunk within the first 0x7800 bytes")

    o, frames, biggest = movi + 4, 0, 0
    while o + 8 <= len(data):
        cid, size = fourcc(data, o), u32(data, o + 4)
        if cid == "idx1" or cid == "ix00":
            break
        if cid[2:] not in ("dc", "wb"):
            errs.append(f"unexpected chunk {cid!r} at {o:#x} inside movi")
            break
        if size > HEAD:
            errs.append(f"chunk {cid} at {o:#x} is {size} bytes, more than {HEAD}")
            break
        frames += cid[2:] == "dc"
        biggest = max(biggest, size) if cid[2:] == "dc" else biggest
        o += 8 + size + (size & 1)
    info.update(frames=frames, largest_frame=biggest)
    return errs, info


def main():
    ok = True
    for path in sys.argv[1:]:
        errs, info = check(path)
        print(f"{path}: {'OK' if not errs else 'NOT PLAYABLE'}  {info}")
        for e in errs:
            print("  -", e)
        ok &= not errs
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
