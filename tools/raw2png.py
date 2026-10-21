#!/usr/bin/env python3
"""Convert a raw Samsung 7301 capture into a PNG for inspection.

Usage: raw2png.py <input.bin> <output.png> [width] [height]
"""
import struct
import sys
import zlib


def write_png(path, pixels, w, h):
    raw = b"".join(b"\x00" + bytes(pixels[y * w : (y + 1) * w]) for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def main():
    src = sys.argv[1]
    dst = sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 160
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 160

    data = open(src, "rb").read()
    # Skip the two byte header if present.
    off = 2 if len(data) >= w * h + 2 else 0
    px = data[off : off + w * h]
    if len(px) < w * h:
        px = px + bytes(w * h - len(px))
    print(f"{src}: {len(data)} bytes -> {w}x{h} image (offset {off})")
    write_png(dst, px, w, h)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
