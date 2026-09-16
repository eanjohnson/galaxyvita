#!/usr/bin/env python3
"""Force DolRecomp's generated chunks to leave HLE'd guest functions alone.

DolRecomp compiles a `bl` whose target lands inside the same C chunk as a plain
`goto`, so the call never returns to the chassis dispatcher and the HLE table in
runtime/chassis.c never sees it. For every address in runtime/hle_hooks.h this
rewrites those local jumps into a dispatcher escape, which is what a cross-chunk
call already emits.

Usage: patch-aot-hle.py <chunks-dir> [hle_hooks.h]
"""
import os
import re
import sys

HOOK_RE = re.compile(r"^\s*X\(0x([0-9A-Fa-f]{8})u,", re.M)


def hooked_addresses(header):
    return sorted({m.group(1).upper() for m in HOOK_RE.finditer(open(header).read())})


def main():
    chunks = sys.argv[1]
    header = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "runtime", "hle_hooks.h")
    addrs = hooked_addresses(header)
    if not addrs:
        sys.exit("no X(0x...) entries in %s" % header)
    # Only a `goto` that owns its whole line is a call or branch; the chunk's
    # entry dispatch keeps its `case 0x...: goto label_...;` on one line and must
    # stay, since the chassis intercepts before the chunk is ever entered.
    pats = [(a, re.compile(r"^([ \t]*)goto label_%s;$" % a, re.M)) for a in addrs]
    patched = 0
    for name in sorted(os.listdir(chunks)):
        if not name.endswith(".c"):
            continue
        path = os.path.join(chunks, name)
        src = open(path).read()
        out = src
        for addr, pat in pats:
            out = pat.sub(
                lambda m, a=addr: "%sctx->pc = 0x%su;\n%sreturn;" % (
                    m.group(1), a, m.group(1)), out)
        if out != src:
            open(path, "w").write(out)
            patched += 1
            print("patched %s" % name)
    print("hle escapes: %d address(es), %d chunk(s) rewritten" % (len(addrs), patched))


main()
