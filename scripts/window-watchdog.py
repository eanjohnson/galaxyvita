#!/usr/bin/env python3
"""Window a watchdog log, and report VI/s rather than blocks/s.

Two things this exists to stop.

The profiler reports cumulative counts, so reading a tick directly gives an
average over the whole run so far -- which for minutes after the disc goes
quiet is still dominated by the load burst. Differencing two ticks gives the
window, and because the report is counts and not percentages the subtraction
is exact.

And blocks/s is not a figure of merit. Between the title screen and file
select it rose 2.3x while real speed fell 3.3x, because file select runs about
seven times more guest code per frame. VI retraces are the emulated machine's
own clock and are the only number here worth comparing across phases; blocks/s
is printed alongside only so the disagreement stays visible.

Usage: window-watchdog.py <t_from> <t_to> <watchdog.log>...
"""
import re, sys

def ticks(path):
    out, cur = [], None
    for line in open(path, errors='replace'):
        m = re.match(r'watchdog t=(\d+)s hits=(\d+)', line)
        if m:
            cur = {'t': int(m.group(1)), 'hits': int(m.group(2))}
            out.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r'irq .*\bvi=(\d+)', line)
        if m:
            cur['vi'] = int(m.group(1))
        m = re.match(r'prof samples=(\d+) over=(\d+)ms', line)
        if m:
            cur['samples'], cur['ms'] = int(m.group(1)), int(m.group(2))
            for k, v in re.findall(r'(guest|idle|disp|gx|ios|mmio|disc)=(\d+)', line):
                cur[k] = int(v)
        m = re.match(r'gxm frames=(\d+)', line)
        if m:
            cur['frames'] = int(m.group(1))
    return [t for t in out if 'samples' in t and 'vi' in t]

PH = ['guest', 'idle', 'disp', 'gx', 'ios', 'mmio', 'disc']

def window(ts, lo, hi):
    a = min(ts, key=lambda x: abs(x['t'] - lo))
    b = min(ts, key=lambda x: abs(x['t'] - hi))
    if b['t'] <= a['t']:
        return None
    dt = b['t'] - a['t']
    d = {k: b.get(k, 0) - a.get(k, 0) for k in PH + ['samples', 'vi', 'hits', 'frames']}
    tot = sum(d[k] for k in PH) or 1
    return a['t'], b['t'], dt, d, tot

def show(path, lo, hi):
    ts = ticks(path)
    if not ts:
        print(f"{path}: no complete ticks"); return
    w = window(ts, lo, hi)
    if not w:
        print(f"{path}: window {lo}-{hi}s not covered (log ends t={ts[-1]['t']}s)"); return
    t0, t1, dt, d, tot = w
    vis = d['vi'] / dt
    print(f"{path.split('/')[-1]:14} t={t0}->{t1}s  VI/s={vis:5.2f}  "
          f"({vis/59.94*100:4.1f}% of real time)  frames={d['frames']:4d}  "
          f"blocks/s={d['hits']/dt/1000:7.1f}k")
    print("               " + "  ".join(f"{k}={d[k]/tot*100:4.1f}%" for k in PH))

if __name__ == '__main__':
    lo, hi = int(sys.argv[1]), int(sys.argv[2])
    for p in sys.argv[3:]:
        show(p, lo, hi)
