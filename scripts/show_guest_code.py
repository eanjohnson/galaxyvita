#!/usr/bin/env python3
"""Print the generated C for a guest PC range. Usage: <chunks-dir> <hex-pc> [n]"""
import os
import re
import sys

CHUNK_RE = re.compile(r"^chunk_\d+_[A-Za-z0-9]+_([0-9A-Fa-f]{8})\.c$")
LABEL_RE = re.compile(r"^label_([0-9A-Fa-f]{8}):")


def pick_chunk(chunks, pc):
    best = None
    for name in os.listdir(chunks):
        m = CHUNK_RE.match(name)
        if not m:
            continue
        base = int(m.group(1), 16)
        if base <= pc and (best is None or base > best[0]):
            best = (base, name)
    return best


def main():
    chunks, pc_s = sys.argv[1], sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    pc = int(pc_s, 16 if pc_s.lower().startswith("0x") else 16)
    end = pc + count * 4
    best = pick_chunk(chunks, pc)
    if not best:
        sys.exit("no chunk covers %08X" % pc)
    path = os.path.join(chunks, best[1])
    print("// %s" % best[1])
    out, on, seen = [], False, False
    for line in open(path):
        m = LABEL_RE.match(line)
        if m:
            addr = int(m.group(1), 16)
            on = pc <= addr < end
            if on:
                seen = True
            elif seen and addr >= end:
                break
        if on:
            out.append(line.rstrip("\n"))
    if not out:
        sys.exit("no labels in %08X..%08X inside %s" % (pc, end, best[1]))
    print("\n".join(out))


main()
