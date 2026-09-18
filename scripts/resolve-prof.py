#!/usr/bin/env python3
"""Turn a profiler PC histogram into something you can act on.

    ./scripts/resolve-prof.py prof-pc.txt [-n 30]

The profilers emit raw guest addresses and nothing else. There is no symbol
map for RMGE01 -- PLAN.md is explicit that feeding another revision's map to
this DOL is not allowed -- so names come from two places that cannot go stale,
because they are read out of the source rather than copied from it:

  * the addresses runtime/chassis.c already names (#define NAME 0x80...), which
    is where OS_IDLE_WAIT and friends live, and
  * runtime/hle_hooks.h, which carries the SDK name of everything hooked.

Anything else is resolved the way scripts/gdis.py does it: DolRecomp leaves
every guest instruction in the generated C as a `// ADDR: mnemonic` comment, so
the chunk tree is a disassembly listing and the instruction at a hot PC can be
printed straight out of it. Knowing an address is 19% of the run is not useful;
knowing it is a `bdnz` two instructions into a wait loop is.
"""
import argparse, os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CHUNKS = os.path.join(ROOT, "generated/aot/generated/chunks")


def known_names():
    """Named guest addresses, read out of the runtime rather than duplicated."""
    names = {}
    chassis = os.path.join(ROOT, "runtime/chassis.c")
    if os.path.exists(chassis):
        with open(chassis) as f:
            for line in f:
                m = re.match(r"#define\s+([A-Z_0-9]+)\s+0x([0-9A-Fa-f]{8})u?\s*$", line)
                if m:
                    names.setdefault(int(m.group(2), 16), m.group(1))
    hooks = os.path.join(ROOT, "runtime/hle_hooks.h")
    if os.path.exists(hooks):
        with open(hooks) as f:
            for line in f:
                m = re.search(r"X\(0x([0-9A-Fa-f]{8})u?,\s*\w+,\s*\"([^\"]+)\"", line)
                if m:
                    names[int(m.group(1), 16)] = m.group(2) + " (HLE)"
    return names


def chunk_for(addr):
    if not os.path.isdir(CHUNKS):
        return None
    best = None
    for name in os.listdir(CHUNKS):
        if not name.endswith(".c"):
            continue
        try:
            start = int(name.rsplit("_", 1)[1][:-2], 16)
        except ValueError:
            continue
        if start <= addr and (best is None or start > best[0]):
            best = (start, os.path.join(CHUNKS, name))
    return best[1] if best else None


def disasm(addr, before=1, after=2):
    """The guest instructions around addr, from the generated C's comments."""
    path = chunk_for(addr)
    if not path:
        return []
    want = {addr + 4 * d for d in range(-before, after + 1)}
    out = {}
    pat = re.compile(r"^\s*//\s*([0-9A-Fa-f]{8}):\s*(.*?)\s*$")
    with open(path, errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if m:
                a = int(m.group(1), 16)
                if a in want:
                    out[a] = m.group(2)
                    if len(out) == len(want):
                        break
    return [(a, out[a]) for a in sorted(out)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("histogram", help="prof-pc.txt from the device, or - for stdin")
    ap.add_argument("-n", type=int, default=25, help="how many entries to resolve")
    ap.add_argument("--no-disasm", action="store_true")
    args = ap.parse_args()

    src = sys.stdin if args.histogram == "-" else open(args.histogram)
    rows, header = [], ""
    for line in src:
        line = line.strip()
        if line.startswith("#"):
            header = line
            continue
        m = re.match(r"0x([0-9A-Fa-f]{8})\s+(\d+)\s+([\d.]+)%", line)
        if m:
            rows.append((int(m.group(1), 16), int(m.group(2)), m.group(3)))
    if not rows:
        print("no histogram rows found -- is this a prof-pc.txt?", file=sys.stderr)
        return 1

    names = known_names()
    total = sum(r[1] for r in rows)
    if header:
        print(header)
    print(f"# {len(rows)} distinct PCs, {total} samples in this file\n")
    for addr, hits, pct in rows[: args.n]:
        label = names.get(addr, "")
        print(f"0x{addr:08x}  {hits:8d}  {pct:>5}%  {label}")
        if not args.no_disasm:
            for a, text in disasm(addr):
                mark = "->" if a == addr else "  "
                print(f"               {mark} {a:08x}  {text}")
            print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
