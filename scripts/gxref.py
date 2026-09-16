#!/usr/bin/env python3
"""Find references to a guest address in a DOL.

Catches direct b/bl branches, 32-bit words holding the address (function
pointer tables), and lis/addi (or ori) pairs -- including the scheduled case
where the compiler drops unrelated instructions between the two halves.

  ./scripts/gxref.py main.dol 804E7508
"""
import struct
import sys

WINDOW = 16  # instructions the addi may drift past its lis


def load(path):
    b = open(path, "rb").read()
    off = struct.unpack(">18I", b[0:72])
    addr = struct.unpack(">18I", b[72:144])
    size = struct.unpack(">18I", b[144:216])
    return b, [(addr[i], size[i], off[i]) for i in range(18) if size[i]]


def main():
    b, secs = load(sys.argv[1])
    target = int(sys.argv[2], 16)
    lo = target & 0xFFFF
    hi = ((target >> 16) + (1 if lo & 0x8000 else 0)) & 0xFFFF
    for a, s, o in secs:
        words = struct.unpack(">%dI" % (s // 4), b[o:o + (s // 4) * 4])
        for k, w in enumerate(words):
            va = a + 4 * k
            op = w >> 26
            if op == 18:
                d = w & 0x03FFFFFC
                if d & 0x02000000:
                    d -= 0x04000000
                t = (d if w & 2 else va + d) & 0xFFFFFFFF
                if t == target:
                    print("%08X  %s 0x%08X" % (va, "bl" if w & 1 else "b", t))
            elif w == target:
                print("%08X  .word 0x%08X" % (va, w))
            elif op == 15 and (w & 0xFFFF) == hi and ((w >> 16) & 31) == 0:
                rd = (w >> 21) & 31
                for j in range(k + 1, min(k + 1 + WINDOW, len(words))):
                    n = words[j]
                    nop = n >> 26
                    if nop == 15 and ((n >> 21) & 31) == rd:
                        break  # rD reloaded; the pair cannot span this
                    if nop in (14, 24) and ((n >> 16) & 31) == rd and (n & 0xFFFF) == lo:
                        print("%08X  lis/%s(+%d) -> 0x%08X"
                              % (va, "addi" if nop == 14 else "ori", j - k, target))
                        break


main()
