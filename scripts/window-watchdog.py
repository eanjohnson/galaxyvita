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

def window_by_vi(ts, lo, hi):
    """Window between two retrace counts rather than two wall-clock times.

    Two runs of the same boot do not reach the same place at the same second --
    that is the whole point of making one of them faster -- so comparing them
    at matched wall clock compares different parts of the game. It shows up as
    guest cost per frame differing between the runs, which cannot be caused by
    anything on the host side. Retraces are the guest's own clock, so matching
    on them compares the same stretch of game in both. It is the same
    correction as GV_RETRACE_CAP on the host harness."""
    a = min(ts, key=lambda x: abs(x['vi'] - lo))
    b = min(ts, key=lambda x: abs(x['vi'] - hi))
    if b['vi'] <= a['vi'] or b['t'] <= a['t']:
        return None
    # Refuse a window the log does not actually cover. min() over the nearest
    # tick silently clamps to the last one when the run has not got that far,
    # and then two runs get compared over different spans of game -- which is
    # the very error matching on retraces exists to prevent, reintroduced by
    # the matching itself. The watchdog ticks every two seconds, so a few
    # retraces of slack is the sampling grid and anything more is a miss.
    span = hi - lo
    slack = max(8, span // 20)
    if abs(a['vi'] - lo) > slack or abs(b['vi'] - hi) > slack:
        return None
    dt = b['t'] - a['t']
    d = {k: b.get(k, 0) - a.get(k, 0) for k in PH + ['samples', 'vi', 'hits', 'frames']}
    tot = sum(d[k] for k in PH) or 1
    return a['t'], b['t'], dt, d, tot


def show(path, lo, hi, by_vi=False):
    ts = ticks(path)
    if not ts:
        print(f"{path}: no complete ticks"); return
    w = (window_by_vi if by_vi else window)(ts, lo, hi)
    if not w:
        print(f"{path}: window {lo}-{hi} not covered "
              f"(log ends t={ts[-1]['t']}s vi={ts[-1]['vi']})"); return
    t0, t1, dt, d, tot = w
    vis = d['vi'] / dt
    print(f"{path.split('/')[-1]:14} t={t0}->{t1}s  VI/s={vis:5.2f}  "
          f"({vis/59.94*100:4.1f}% of real time)  frames={d['frames']:4d}  "
          f"blocks/s={d['hits']/dt/1000:7.1f}k")
    print("               " + "  ".join(f"{k}={d[k]/tot*100:4.1f}%" for k in PH))

if __name__ == '__main__':
    args = sys.argv[1:]
    by_vi = False
    if args and args[0] == '--vi':
        by_vi = True
        args = args[1:]
    lo, hi = int(args[0]), int(args[1])
    for p in args[2:]:
        show(p, lo, hi, by_vi)
