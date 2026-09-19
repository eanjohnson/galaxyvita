#!/usr/bin/env python3
"""Count emitted ARM instructions by the CPUState field they touch.

The recompiler's output is 61 MiB of ARM and the only honest way to aim work at
it is to know what it spends itself on. This compiles one or more generated
chunks to assembly with the real build flags, then buckets every load whose
base offset matches a CPUState field.

It is a static census: it counts *emitted* instructions, not executed ones, and
weights every chunk equally regardless of how hot it is. Use it to find
structural waste -- a field touched at every basic block, an accessor the
compiler reloads because it cannot prove no aliasing -- and then measure the
result on the device, because instruction count is not time. This project has
twice written up a static saving that the compiler had already taken; run this
before and after and compare, rather than reading the "before" alone.

  scripts/census-emitted.py generated/aot/generated/chunks/chunk_0767*.c
  scripts/census-emitted.py --baseline before.json chunks/*.c
"""
import argparse, json, os, re, subprocess, sys, tempfile, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "third_party", "DolRecomp", "src")
GEN = os.path.join(ROOT, "generated", "aot", "generated")
CC = os.environ.get("GV_AOT_CC",
                    os.path.expanduser("~/vitasdk/bin/arm-vita-eabi-gcc"))
FLAGS = os.environ.get(
    "GV_AOT_CFLAGS",
    "-O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb "
    "-fno-strict-aliasing -std=gnu11").split()

FIELDS = ["gpr", "fpr", "pc", "lr", "ctr", "cr", "xer", "msr",
          "reserve_valid", "ram", "ram_size", "downcount"]

INSN = re.compile(r"^\s+[a-z][a-z0-9.]*\s")
LOAD = re.compile(r"^\s+(?:ldr|ldrd|ldrb|ldrh|ldrsb|ldrsh)[a-z.]*\s+"
                  r"[^,]+,\s*(?:[^,]+,\s*)?\[r[0-9]+(?:,\s*#(-?[0-9]+))?\]")


def field_offsets():
    """Ask the target compiler, because host offsets differ: CPUState holds
    pointers, so every field after `ram` sits somewhere else in a 64-bit
    build. Reading them off the host is how you mislabel the hottest field."""
    body = "#include <stddef.h>\n#include \"cpu/cpu.h\"\nconst int o[] = {\n"
    body += "".join("  (int)offsetof(CPUState, %s),\n" % f for f in FIELDS)
    body += "  (int)sizeof(CPUState)};\n"
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "o.c")
        open(c, "w").write(body)
        out = subprocess.run([CC, "-O2", "-I" + SRC, "-S", "-o", "-", c],
                             capture_output=True, text=True)
        if out.returncode:
            sys.exit("offset probe failed:\n" + out.stderr)
        words = re.findall(r"\.word\s+(\d+)", out.stdout.split("o:", 1)[-1])
    return dict(zip(FIELDS, (int(w) for w in words)))


def namer(off):
    """Only distinctive offsets are trustworthy. Small ones (gpr[0], gpr[1])
    are also the shape of an ordinary load through any other pointer, so they
    are reported but should not be read as guest-register traffic on their
    own."""
    names = {}
    for f, o in OFF.items():
        if f == "gpr":
            for i in range(32):
                names.setdefault(o + 4 * i, "gpr[%d]" % i)
        elif f != "fpr":
            names[o] = f
    def f(o):
        if o in names:
            return names[o]
        if OFF["fpr"] <= o < OFF["pc"]:
            return "fpr"
        return None
    return f


def census(paths):
    counts = collections.Counter()
    total = 0
    for p in paths:
        for line in open(p):
            line = line.replace("\t", "    ")
            if line.lstrip().startswith((".", "@")):
                continue
            if not INSN.match(line):
                continue
            total += 1
            m = LOAD.match(line)
            if m:
                name = NAME(int(m.group(1) or 0))
                if name:
                    counts[name] += 1
    return total, counts


def compile_all(sources, outdir):
    asm = []
    for s in sources:
        o = os.path.join(outdir, os.path.basename(s) + ".s")
        r = subprocess.run([CC] + FLAGS + ["-I" + SRC, "-I" + GEN,
                                           "-S", "-o", o, s],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("compile failed for %s:\n%s" % (s, r.stderr[:2000]))
        asm.append(o)
    return asm


ap = argparse.ArgumentParser()
ap.add_argument("sources", nargs="+", help="generated chunk .c files")
ap.add_argument("--baseline", help="JSON from an earlier run, to diff against")
ap.add_argument("--save", help="write this run's totals as JSON")
ap.add_argument("--top", type=int, default=15)
a = ap.parse_args()

OFF = field_offsets()
NAME = namer(OFF)
with tempfile.TemporaryDirectory() as d:
    total, counts = census(compile_all(a.sources, d))

base = json.load(open(a.baseline)) if a.baseline else None
print("%d chunks, %d ARM instructions%s" % (
    len(a.sources), total,
    "" if not base else "  (%+.2f%% vs baseline %d)" % (
        (total - base["total"]) * 100.0 / base["total"], base["total"])))
gpr = sum(v for k, v in counts.items() if k.startswith("gpr"))
print("  %-14s %7d  %5.2f%%" % ("gpr (all)", gpr, gpr * 100.0 / total))
for k, v in counts.most_common(a.top):
    line = "  %-14s %7d  %5.2f%%" % (k, v, v * 100.0 / total)
    if base:
        was = base["counts"].get(k, 0)
        line += "   was %7d" % was
    print(line)
if a.save:
    json.dump({"total": total, "counts": counts}, open(a.save, "w"), indent=1)
    print("saved %s" % a.save)
