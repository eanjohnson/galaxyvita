#!/usr/bin/env python3
"""Dump bytes/strings from a DOL at a guest virtual address.
Usage: dol_peek.py <main.dol> <hex-addr> [len]"""
import struct
import sys


def load(path):
    b = open(path, "rb").read()
    off = struct.unpack(">18I", b[0:72])
    addr = struct.unpack(">18I", b[72:144])
    size = struct.unpack(">18I", b[144:216])
    secs = [(addr[i], size[i], off[i]) for i in range(18) if size[i]]
    return b, secs


def read(b, secs, va, n):
    for a, s, o in secs:
        if a <= va < a + s:
            k = va - a
            return b[o + k:o + k + min(n, s - k)]
    return b""


def main():
    b, secs = load(sys.argv[1])
    va = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    d = read(b, secs, va, n)
    if not d:
        sys.exit("0x%08X is not in any DOL section" % va)
    for i in range(0, len(d), 16):
        row = d[i:i + 16]
        hexs = " ".join("%02x" % c for c in row)
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in row)
        print("%08X  %-47s  %s" % (va + i, hexs, txt))


main()
