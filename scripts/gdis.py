#!/usr/bin/env python3
"""Print the guest disassembly DolRecomp left in its generated C, by address.

The chunks carry every instruction as a `// ADDR: mnemonic` comment, so the
generated tree doubles as a disassembly listing; this pulls a range out of it.
"""
import os, re, sys

CHUNKS = os.path.join(os.path.dirname(__file__), "..",
                      "generated/aot/generated/chunks")


def chunk_for(addr):
    for name in os.listdir(CHUNKS):
        if not name.endswith(".c"):
            continue
        try:
            start = int(name.rsplit("_", 1)[1][:-2], 16)
        except ValueError:
            continue
        if start <= addr < start + 0x1000:
            return os.path.join(CHUNKS, name)
    return None


def main():
    start = int(sys.argv[1], 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    seen = set()
    addr = start
    end = start + count * 4
    while addr < end:
        path = chunk_for(addr)
        if not path:
            print("%08X: <no chunk>" % addr)
            addr += 4
            continue
        if path not in seen:
            seen.add(path)
            with open(path) as f:
                text = f.read()
            for m in re.finditer(r"// ([0-9A-F]{8}): (.*)", text):
                lines[int(m.group(1), 16)] = m.group(2).rstrip()
        addr = (addr & ~0xFFF) + 0x1000 if False else addr + 4
    for a in range(start, end, 4):
        print("%08X: %s" % (a, lines.get(a, "<none>")))


lines = {}
main()
