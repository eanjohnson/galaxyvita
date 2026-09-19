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

Each phase is reported twice: as a share of the window, and as seconds of it.
The seconds line is the one to compare across runs -- shares are taken against
different wall clocks and do not subtract.

Usage: window-watchdog.py [--vi] <from> <to> <watchdog.log>...
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
            for k, v in re.findall(r'(guest|idle|disp|gxvtx|gxsub|gxcopy|gx|ios|mmio|disc)=(\d+)', line):
                cur[k] = int(v)
        m = re.match(r'gxm frames=(\d+) scenes=\d+ draws=(\d+)', line)
        if m:
            cur['frames'] = int(m.group(1))
            cur['draws'] = int(m.group(2))
    return [t for t in out if 'samples' in t and 'vi' in t]

# gxvtx/gxsub/gxcopy are absent from logs written before the split and read
# as zero there, which is what a run that did not measure them should look
# like, and keeps those logs comparable on every other phase.
PH = ['guest', 'idle', 'disp', 'gx', 'gxvtx', 'gxsub', 'gxcopy',
      'ios', 'mmio', 'disc']

def window(ts, lo, hi):
    a = min(ts, key=lambda x: abs(x['t'] - lo))
    b = min(ts, key=lambda x: abs(x['t'] - hi))
    if b['t'] <= a['t']:
        return None
    dt = b['t'] - a['t']
    d = {k: b.get(k, 0) - a.get(k, 0) for k in PH + ['samples', 'vi', 'hits', 'frames']}
    tot = sum(d[k] for k in PH) or 1
    return a['t'], b['t'], dt, d, tot, a.get('draws'), b.get('draws')

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
    return a['t'], b['t'], dt, d, tot, a.get('draws'), b.get('draws')


def show(path, lo, hi, by_vi=False):
    ts = ticks(path)
    if not ts:
        print(f"{path}: no complete ticks"); return
    w = (window_by_vi if by_vi else window)(ts, lo, hi)
    if not w:
        print(f"{path}: window {lo}-{hi} not covered "
              f"(log ends t={ts[-1]['t']}s vi={ts[-1]['vi']})"); return
    t0, t1, dt, d, tot, d0, d1 = w
    vis = d['vi'] / dt
    print(f"{path.split('/')[-1]:14} t={t0}->{t1}s  VI/s={vis:5.2f}  "
          f"({vis/59.94*100:4.1f}% of real time)  frames={d['frames']:4d}  "
          f"blocks/s={d['hits']/dt/1000:7.1f}k")
    print("               " + "  ".join(f"{k}={d[k]/tot*100:4.1f}%" for k in PH))
    # Shares belong to different wall clocks between two runs, so they do not
    # subtract: a phase can hold a larger share of a shorter run while costing
    # fewer seconds. Seconds do subtract, and they are what "where did the time
    # go" actually means. Every A/B in this project has had to do this
    # conversion by hand, and getting it wrong is how a graphics phase once
    # looked like it had grown when it had halved.
    print("               " + "  ".join(f"{k}={d[k]/tot*dt:5.1f}s" for k in PH))
    # Cumulative draws at each edge, because matching on retraces is not the
    # same as matching on workload. Two passes of one eboot with one input
    # script came out 2.2x apart here, and the reason was that at frame ~1100
    # one had issued 530,006 draws and the other 1,109,774 -- the replay had
    # landed somewhere different and the game was showing less. Speed is only
    # comparable between runs that were drawing the same thing, so these two
    # numbers have to agree between the sides before the line above means
    # anything. Within a percent or two is the same workload.
    if d0 is not None and d1 is not None:
        per = (d1 - d0) / max(d['frames'], 1)
        print(f"               workload draws {d0:,} -> {d1:,}"
              f"  ({per:,.0f} per frame)")

if __name__ == '__main__':
    args = sys.argv[1:]
    by_vi = False
    if args and args[0] == '--vi':
        by_vi = True
        args = args[1:]
    lo, hi = int(args[0]), int(args[1])
    for p in args[2:]:
        show(p, lo, hi, by_vi)
