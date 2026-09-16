#!/usr/bin/env python3
"""Tight RGBA dump -> PNG, so a frame pulled off the console can be looked at.

Stored (uncompressed) deflate blocks, the same trick tests/host/gx_soft.c uses,
so this needs nothing but the standard library."""
import struct
import sys
import zlib


def png(path, rgba, width, height):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgba[y * width * 4:(y + 1) * width * 4]
    out = bytearray(b"\x89PNG\r\n\x1a\n")

    def chunk(tag, data):
        out.extend(struct.pack(">I", len(data)))
        out.extend(tag)
        out.extend(data)
        out.extend(struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


def main():
    src, dst, width, height = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    data = open(src, "rb").read()
    want = width * height * 4
    if len(data) < want:
        raise SystemExit(f"{src}: {len(data)} bytes, expected {want}")
    png(dst, data[:want], width, height)


if __name__ == "__main__":
    main()
