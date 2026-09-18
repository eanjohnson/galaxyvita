# GalaxyVita progress

Living log. Newest entry first. Record evidence, not hopes.

## 2026-09-18 — -marm is not better, and the noise floor is about 3%

### The measurement that calibrates the others

`wd_split.log` and `wd_xchunk.log` came from builds that are behaviourally
identical -- the phase split adds byte stores and nothing else, and host
equivalence was bit-for-bit at 1200 retraces. They should have measured the
same. They measured **+1.2% to +2.8%**, in the direction that added work.

That is run-to-run noise on this rig, and nothing smaller than it can be read
from a single run. It should have been established before the first A/B rather
than fallen out of one by accident.

| result | against a ~3% floor |
|---|---|
| cross-chunk 1.32x | far above; safe |
| pooled decode buffer, 1.057x at 1100-1400 | above, and `gxvtx` moved as predicted; safe |
| pooled decode buffer, 1.011x at 400-800 | **inside the floor; do not quote it** |
| strict aliasing 0.99x | inside; correctly read as no effect |
| -marm 0.96-0.99x | inside |

The 1.011x above is a figure this log published yesterday without that caveat.

### -marm against -mthumb, AOT objects only

| retraces | -mthumb | -marm |
|---|---|---|
| 400-800 | 3.75 VI/s | 3.60 |
| 800-1100 | 1.79 | 1.77 |
| 1100-1400 | 1.48 | 1.46 |

Consistently negative and consistently inside the noise floor, so the honest
reading is "indistinguishable", not "4% slower". **Keep `-mthumb`**: it is the
toolchain default, it is denser, and ARM has not earned a change.

Sizes, which are not noisy and are the more interesting half:

| | object `.text` | ELF | `eboot.bin` |
|---|---|---|---|
| Thumb-2 | 73.63 MiB | 95,350,100 | 33,450,462 |
| ARM | **87.76 MiB** | 108,635,152 | **30,221,116** |

ARM carries 19% more `.text` and a 13.9% bigger ELF -- and packs 3.2 MB
*smaller*, because `eboot.bin` is compressed and ARM's fixed-width encoding
squeezes 3.60x where dense Thumb-2 manages 2.85x. Read across instruction sets,
eboot size reports the opposite of the truth.

The I-cache prediction did not resolve. 19% more `.text` against a 32 KiB L1I
should have cost something visible if instruction-cache pressure were the
binding constraint at these chunk sizes, and it did not move beyond noise. That
is weak evidence against the I-cache theory, not evidence for it -- and it
raises rather than lowers the value of the chunk-size sweep, which changes
function size by 8x rather than 19%.

No host equivalence gate was possible here: `-marm` means nothing on x86, so
the only check was the device boot reaching the same retraces. Weaker evidence
than every other change recorded above, which is why the flag stays put.

## 2026-09-18 — Pooling the decode buffer is 1.06x for three lines

`GxVertexLoader::Decode` was already written for reuse: it `clear()`s both
vectors and `reserve()`s before filling them, and `clear()` keeps capacity. The
caller threw that away by naming a fresh `GxDecodedDraw` per draw, so capacity
was freed at the end of every draw and reallocated at the start of the next --
roughly 20,000 allocate/free pairs a frame at 9,000-11,600 draws, before a
vertex is read. Hoisting it to a member is the whole change.

| retraces | phase split | pooled | speedup |
|---|---|---|---|
| 400-800 | 3.71 VI/s | 3.75 | 1.011x |
| 800-1100 | 1.72 | 1.79 | 1.041x |
| 1100-1400 | 1.40 | **1.48** | **1.057x** |

**The phase it was aimed at is the phase that moved**, which is the check that
it worked for the reason claimed and not by luck: `gxvtx` 23.1% -> 20.8%,
12.3% -> 10.3%, 0.7% -> 0.5%. In absolute seconds over the slow window, `guest`
went 54.6s -> 54.5s and `idle` 42.8s -> 42.8s, both flat; `gxvtx` gave up 7.0s
of the 10s the window lost. The remaining 3s came off `gx` parse and `ios`,
which is what less allocator traffic does to everything sharing the heap.

### The gate, and the gate that was nearly wrong

Host equivalence at 1200 retraces is exact, `draws=6315 vtx=25260` included --
the geometry reaching the sink is unchanged.

Pixel equivalence: 39 frames rendered by the software rasteriser at stride 30
across the whole boot, with and without the change, identical byte-for-byte.
36 of them carry real geometry (186-213 distinct colours), spanning frames 90
to 1140.

**The first attempt at that gate proved nothing and looked like it had passed.**
`GV_GFX_EVERY` defaults to a stride of 1, so a 40-frame dump is frames 0 to 39
-- the game's opening flash. Forty frames matched byte-for-byte and every one
of them was solid white or solid black. This is CLAUDE.md's own white-screen
trap arriving from a new direction: the frames were checked, and checking them
was still not evidence, because nothing had been drawn yet. A frame gate needs
a distinct-colour count next to it or it is not a gate.

## 2026-09-18 — Vertex loading is 23% of the console, and strict aliasing is worth nothing

### What was inside `gx`

The phase split, flown. VI/s matches the build it was taken against -- 3.71 vs
3.61, 1.72 vs 1.70, 1.40 vs 1.38 -- so the instrument costs nothing and its
shares can be read straight. The four new phases sum to within a point of the
single `gx` they replaced.

At retraces 1100-1400, the slow end:

| phase | share | what it is |
|---|---|---|
| **gxvtx** | **23.1%** | `GxVertexLoader::Decode` and the per-draw vectors |
| gx | 11.1% | command parse |
| gxsub | 7.6% | `sceGxmDraw` and the state re-issued per draw |
| gxcopy | 2.7% | EFB copy and present |

**This agrees with the nogfx arithmetic taken a different way.** That method put
decode at 31.6% and the sink at 11.8%; the direct measurement says 34.2% and
10.3%. Two unrelated methods within two points of each other is the strongest
evidence here, and it means the ranking can be trusted:

| | share at 1100-1400 |
|---|---|
| guest (recompiled code) | 25.5% |
| **vertex load** | **23.1%** |
| idle | 20.0% |
| gx command parse | 11.1% |
| gxsub | 7.6% |
| disp | 6.0% |
| ios | 4.0% |

Vertex loading is the largest single cost in the emulator after recompiled
guest code itself. The plan did not target it, and ranked `sceGxmDraw` batching
first -- which lives in the 7.6%.

The shares move with what is on screen, and the direction is the check on all
of it: at retraces 400-800, with almost no geometry, `gxcopy` is 11.3% and
`gxvtx` is 0.7%. Present costs the same every frame; vertex loading does not.

### `-fno-strict-aliasing` costs nothing, so it stays

Rebuilt the AOT objects with strict aliasing on, having already shown host
equivalence bit-for-bit at 1200 retraces. On the console:

| retraces | -fno-strict-aliasing | strict aliasing |
|---|---|---|
| 400-800 | 3.61 VI/s | 3.61 VI/s |
| 800-1100 | 1.70 VI/s | 1.68 VI/s |
| 1100-1400 | 1.38 VI/s | 1.36 VI/s |

Flat, fractionally negative, every phase share within a few tenths, and the run
reached t=722s so nothing was miscompiled. The reasoning for trying it was
sound -- with aliasing off, a guest store through the `u8*` RAM pointer forces
GCC to reload `ctx->gpr[]`, in code that stores constantly -- and 2.4% more code
bought exactly zero. **Keep the flag**: it is the safer of the two and costs
nothing. Recorded so nobody spends the hour again.

## 2026-09-18 — Graphics is worth 1.14x, and two thirds of it is decode

Two results from the same night, both taken on the cross-chunk build at full
clock, both matched on VI retraces rather than wall clock.

### All of graphics now buys 11-14%

`nogfx.txt` stubs out the GXM sink -- every `sceGxmDraw`, every state call,
every texture bind -- and leaves FIFO decode running. Against `wd_xchunk.log`,
the same eboot with graphics on:

| retraces | graphics on | graphics off | speedup |
|---|---|---|---|
| 400-800 | 3.61 VI/s | 4.10 VI/s | 1.136x |
| 800-1100 | 1.70 VI/s | 1.89 VI/s | 1.112x |
| 1100-1400 | 1.38 VI/s | 1.57 VI/s | 1.138x |

The same experiment on the pre-cross-chunk build said 1.10x/1.25x. Cross-chunk
removed dispatch overhead, and the graphics share came down with it -- which is
what a share does when something else gets cheaper and it does not.

**So every Phase 4 item that lives inside the sink shares a ceiling of ~12%.**
Batching draws, deduping per-draw state, U16 indices: none of them can return
more than a fraction of that, and the plan ranked them first.

### What survives is decode, and it is 2.7x the submission cost

A gx share is a share of its own run's wall clock, and the two runs cover the
same retraces in different times, so the shares do not subtract. Converted to
seconds over the matched window first:

| window | decode | GXM sink | decode/sink |
|---|---|---|---|
| 400-800 | 1.7% | 12.0% | 0.1x |
| 800-1100 | 22.2% | 9.8% | 2.3x |
| 1100-1400 | **31.6%** | 11.8% | **2.7x** |

The inversion at 400-800 is the reason to believe the rest of the table. With
almost nothing on screen, decode costs nothing and per-draw submission
dominates; as geometry arrives, decode scales with it and submission does not.
Noise does not produce that shape.

That makes ModernGekko's decode path -- ~32% of wall clock at the slow end --
the largest single cost in the emulator, and **no item in the optimisation plan
targets it.** `GxStateBackend::Draw` builds a fresh `GxDecodedDraw` per draw,
two `std::vector`s, at 9,000-11,600 draws a frame: roughly 20,000 heap
allocate/free pairs per frame before any vertex is touched. Pooling that buffer
was Phase 4 item 3 and was ranked third against a model of where the cost was
that this measurement has now replaced.

### Strict aliasing is not load-bearing for the generated C

`-fno-strict-aliasing` has been applied to the AOT objects unconditionally and
nobody had checked whether it was needed. Rebuilt without it and run to a fixed
1200 retraces against the recorded cross-chunk fingerprint:

```
         hits      fifo      mmio_w    retrace   gx        cycles
alias  6501763   2790620   2850229    1200    1109347   14597079739
ref    6501763   2790620   2850229    1200    1109347   14597079739
```

Identical on every counter, including the cycle count. Host equivalence at x86
`-O2` is necessary and not sufficient -- aliasing miscompiles are exactly the
class that appears on one target and not another -- so the device run decides
it, and a boot that stops progressing is the tell to watch for rather than a
wrong number.

## 2026-09-18 — The cross-chunk call is worth 1.32x on the console

The dead direct-call path, wired up and flown. Both eboots built from the same
host sources, same input script, compared at matched VI retraces.

| retraces | baseline | cross-chunk | speedup |
|---|---|---|---|
| 100-400 | 82 s | 74 s | 1.11x |
| 400-800 | 134 s | 110 s | 1.22x |
| 800-1100 | 232 s | 176 s | **1.32x** |

The baseline did not reach retrace 1400 inside its 700 s hold; the cross-chunk
build did, which is the same result stated without a ratio.

The phase split says it is the intended mechanism and not luck. At 800-1100:

| | baseline | cross-chunk |
|---|---|---|
| disp (the chassis's own per-block work) | 18.2% | **7.0%** |
| ios (serviced once per dispatch) | 11.1% | **4.2%** |
| guest | 26.5% | 30.9% |
| blocks/s | 211.0k | 68.8k |

`disp` and `ios` are exactly the costs paid per dispatch, and both fall by
roughly two thirds. `guest` holding steady is the check that both runs covered
the same stretch of game rather than one of them getting somewhere easier.

The gain grows with how heavy the phase is -- 1.11x at the title screen, 1.32x
at file select -- which follows, since heavier phases run more guest code and
therefore more subroutine calls.

**The 28% of extra code did not eat it.** The cross-chunk eboot is 33.45 MB
against 26.05 MB, because every direct call site emits a prototype, a call and
a continuation check where it used to emit two stores and a return. Chunk
functions were already about 47.6 KiB against a 32 KiB L1I, so this was the
real risk and the reason the plan said to measure rather than predict. It was
worth measuring and the answer was no.

It is now the default in `generate-aot.sh`. DolRecomp's own default stays off:
it is a general-purpose tool and direct calls are only safe for a runtime that
has told it which addresses it intercepts.

### What this moves to the front

Graphics. At the deepest window the cross-chunk build spends **43.4%** in `gx`,
because everything competing with it got cheaper. The 1.25x ceiling measured
for the GXM backend earlier today was measured on the *baseline* and is now an
underestimate of what graphics is worth.

And the idle loop. Its share rises from 50.0% to 60.2% at 400-800 and 19.4% to
25.8% at 800-1100 -- not because it got slower but because everything around it
got faster. The idle skip was reverted today for having a failure mode nobody
could explain; it is worth returning to, and still only with an explanation for
the cliff rather than a constant tuned until the numbers look nice.

## 2026-09-18 — The emulator is CPU-clock-bound, and graphics is worth 1.25x at most

Two device A/Bs, both matched on VI retraces rather than wall clock. That
matching is not a detail: the runs reach different parts of the game at the
same second, which is the whole point of making one faster, and comparing them
at matched wall clock gave answers that went in both directions and read as
noise. The tell was guest cost per frame differing between two runs that
differ only on the host side, which nothing on the host can cause.

### The clock is worth exactly its ratio

`gfx` ran at 444/222/222, `noclock` at the stock 333/222/111.

| retraces | 444 MHz | 333 MHz | speedup |
|---|---|---|---|
| 100-400 | 3.64 VI/s (84 s) | 2.66 VI/s (112 s) | **1.37x** |
| 400-800 | 3.01 (132 s) | 2.19 (182 s) | **1.37x** |
| 800-1100 | 1.29 (232 s) | 0.96 (312 s) | **1.34x** |

The CPU clock ratio is 444/333 = 1.333. The measured speedup is that ratio and
nothing more, in every window. Two things follow.

**No throttling.** The gain is as large in the last window, at t=682 s, as in
the first at t=56 s. PROGRESS.md has recorded an unexplained "plateau" before
now; whatever it was, it was not the SoC backing off, because an 11-minute run
holds its clock.

**Doubling the GPU clock bought nothing measurable.** 111 -> 222 MHz was in the
same change and the total is still just the CPU ratio. Every phase share is
also unchanged between the two clocks -- guest 26.6% vs 25.8%, idle 19.5% vs
19.4%, disp 17.4% vs 16.9% -- which is the signature of a workload where
everything scales with one clock. The console is not waiting on its GPU.

### Graphics is a third of the time and worth at most a quarter of it

`nogfx.txt` skips installing the GX sink, so the GXM submission path never
runs. ModernGekko's FIFO decoder still does, because that belongs to the
chassis.

| retraces | with GXM | without | speedup | gx share |
|---|---|---|---|---|
| 400-800 | 132 s | 120 s | **1.10x** | 10.8% -> 1.7% |
| 800-1100 | 232 s | 186 s | **1.25x** | 25.4% -> 19.3% |

So the entire GXM backend -- every `sceGxmDraw`, every state call, the whole
batching workstream that Phase 4 of the optimisation plan is built around --
is worth **1.25x at the slow phase, and that is the ceiling**, not an
estimate of what batching would achieve. Deleting the backend outright is the
upper bound and it is 25%.

And the run with graphics off *still* spends 19.3% of its time in `gx`. That
is FIFO decode, vertex loading and state tracking, which no item in Phase 4
touches. The single `gx` phase has been conflating two different costs of
roughly equal size, and needs splitting before any graphics work is aimed.

The plan's ranking therefore inverts. The recompiler work is the larger
target: `disp` is 17.4% and `ios` 11.0% at the same phase, and both fall with
the dispatch count.

## 2026-09-18 — The chassis was re-entered every 371 cycles of a 10,000-cycle slice

`emit_set_chunk_table()` has been in DolRecomp's emitter since the direct-call
path was written, with **no caller anywhere in the tree**. So `g_chunk_starts`
stayed NULL, `emit_cross_chunk_call()` returned false every time, and every
cross-chunk `bl` in Super Mario Galaxy returned to the chassis dispatcher
instead of calling the next chunk directly. Wiring it up is one call and an
env knob, `DOLRECOMP_C_CROSS_CHUNK_CALLS`.

The size of what that cost is best seen not in the dispatch count but in the
dispatch *budget*. The chassis hands each dispatch `SLICE_CYCLES = 10000`
emulated cycles and credits back whatever was used. Running to a fixed 1200 VI
retraces:

| | dispatches | cycles per dispatch | share of the 10,000 budget |
|---|---|---|---|
| before | 39,344,199 | 371.0 | **3.7%** |
| after | 6,501,763 | 2245.1 | 22.5% |

The chassis was being re-entered **27 times more often than its own slice
budget intended**, because a dispatch almost never ended on the budget -- it
ended on the next subroutine call. 150,742 call sites across 1321 of the 1322
chunks now call directly instead.

### The gate needed a new ruler

Every equivalence check in this project so far has compared two builds at a
fixed block count. That ruler is wrong for this change, and was wrong for the
idle skip before it: a block is a *host dispatch*, and both changes exist
precisely to make one dispatch cover more guest code. At a fixed block count
the two builds run the game to different places, and then every counter
downstream moves for reasons that have nothing to do with correctness. That is
most of why the idle-skip sweep was so hard to read.

`Chassis.retrace_cap` stops on the guest's own clock instead -- `GV_RETRACE_CAP`
in the host harness. At a fixed 1200 retraces:

```
cycles     14,597,081,505  vs  14,597,079,739   difference 1,766  (0.0000121%)
timebase    1,216,423,458  vs   1,216,423,311   difference   147
disc        reads=5894 bytes=22589152           identical
gx/fifo          0.397526  vs        0.397527
draws/gx (per mil) 5.6928  vs          5.6925
vtx/draw           4.0000  vs          4.0000
```

Twelve parts per billion on total emulated cycles. The residual is the cap
firing at the first dispatch boundary past the retrace, and the two builds have
different boundaries: between 400 and 1200 retraces the absolute gap stays flat
(2314 -> 2555 FIFO writes) while the relative gap falls threefold, 0.281% ->
0.091%. Divergence grows; a fixed stop offset does not.

### The hook that would have been stepped over

Direct calls skip whatever the host does between blocks, and here one of those
things is load-bearing. `runtime/hle_hooks.h` answers nine guest functions
natively -- the WUD/Bluetooth entry points, so that a stack this port has no
radio for stays dormant. A `bl` to one of them in another chunk would, with
direct calls on, enter that chunk, hit its entry `switch`, and run the
recompiled guest code the hook exists to replace.

`patch-aot-hle.py` cannot help. It rewrites *local* gotos and deliberately
leaves the entry switch alone, and says why in a comment: "the chassis
intercepts before the chunk is ever entered". That sentence was true when it
was written and is exactly the assumption direct calls break. So the emitter
now takes a list of addresses the host intercepts and keeps the old form for
them; `generate-aot.sh` points it at `hle_hooks.h` itself rather than a
generated copy, so the two cannot drift apart.

Twenty real `bl` sites are covered. Without the list the Bluetooth stack would
have woken up, thousands of blocks from anything that looked like a cause.

SMC sites turned out not to be a hazard: `chassis.c` only counts them.

### What this does not tell us

Dispatch count is not VI/s. The chassis round trip may be cheap; 6x less of
something free is still free. The profile puts `disp` at 15.4% of the slow
phase and `ios` at 9.6% -- and `ios_bt_service`, `flipper_advance` and
`maybe_irq` all run once per dispatch inside `advance_time`, so they thin out
with it. That is the wager. The device A/B in VI/s is the test, and it is the
only number that decides whether this lands.

## 2026-09-18 — The console was profiled, and it was never running at full clock

Two findings, one of which invalidates every performance number this file
records.

### The clock was never raised

`main.c` logged `clock: PSVshellPlus user-controlled` -- a sentence somebody
wrote instead of a measurement. `scePower*` appeared nowhere in the source and
`ScePower_stub` was not even in `target_link_libraries`. Asking the console
what it was actually running gave:

```
clock: on entry arm=333 bus=222 gpu=0 xbar=111
clock: now   arm=444 bus=222 gpu=222 xbar=166
```

**Every measurement ever taken in this project was taken at 75% CPU clock and
half GPU clock.** The app now asks for 444/222/222/166 at startup and logs both
what it asked for and what it was granted, so the number is observed rather
than assumed. `noclock.txt` opts out, for the A/B.

The general lesson is the one this file keeps relearning: a log line that
states a fact nobody measured is worse than no log line, because it stops the
next person looking. `PSVshellPlus user-controlled` read as "handled".

### The first on-device profile

The host profiler cannot port -- no `SIGPROF`, no `setitimer`, no
`/proc/self/maps`. So the emulation thread publishes instead of being
interrupted: a `volatile u8 phase` in `Chassis`, one store at each region
boundary, read at ~850 Hz by a sampler thread pinned to CPU2 alongside the
watchdog. Guest PCs go into a 4096-slot hash beside it. `prof.txt` turns it on;
it costs nothing measurable (20M-block host equivalence was identical, 4.607s
-> 4.555s).

The report emits **raw cumulative counts, not percentages**. That was not a
style choice: cumulative percentages kept reporting the disc-load burst long
after the card went quiet, and the only way to read a window out of two ticks
is to difference counts that subtract exactly.

Windowing a 630 s run at the slow end (t=606->630 s, vi/s=1.0):

| phase | cumulative @630 s | windowed t=606->630 s |
|---|---|---|
| gx (FIFO decode + GXM sink) | 20.9% | **33.6%** |
| guest (recompiled code) | 21.9% | 24.4% |
| idle (the SDK's wait loop) | 31.1% | 15.8% |
| disp (chassis per-block work) | 14.9% | 14.9% |
| ios | 8.8% | 8.8% |
| mmio | 2.1% | 2.1% |

This overturns the host profile. On the host, recompiled guest code was 75% of
time after the accessor inlining; on the console it is a quarter, and
**graphics is the single largest phase.** The share also moves steadily through
the run -- gx was 8.4% cumulative at t=52 s and 20.9% at t=630 s -- so any
figure quoted without a window and a phase attached is meaningless here. Report
windows.

One caveat on the guest-PC half: at 667k samples the hash had overflowed
170k of them (25%). The phase histogram is a plain array and is unaffected, but
the PC ranking from a long run is not trustworthy without more slots.

`CHASSIS_PH_DISC` exists because the first version of this had no disc phase
and `mmio` silently absorbed card I/O -- disc reads reach the chassis through
an IPC register write, so they were landing in the MMIO bucket and tracking
`io=13381ms` almost exactly. `disc_set_phase_slot()` hands `disc.c` a pointer
to the byte so it can set it without learning what a `Chassis` is.

`scripts/resolve-prof.py` turns the raw PC dump into something readable,
annotating known addresses and disassembling from the `// ADDR:` comments the
generator already leaves in the chunk C.

### The idle skip and the bound-256 cliff

`0x804AB358` was already named `OS_IDLE_WAIT` and already special-cased, but
only to log. Resolved, it is three instructions:

```
804ab354  bl      0x804A80FC
804ab358  lwz     r0, -8048(r13)   <- the hot PC
804ab35c  cmpwi   r0, 0
804ab360  bc    12, 2, 0x804AB358
```

A spin on the OS run-queue head. Because the timebase is cycle-derived, waiting
for an emulated retrace means burning real ARM cycles to make emulated time
pass, which is the classic idle-skip shape.

It was implemented, gated, and **it failed its gate.** Sweeping the bound on
how many slices may be credited at once, at 20M host blocks:

| bound | fifo writes | retraces | gx cmds | gx/retrace |
|---|---|---|---|---|
| off | 936,432 | 444 | 372,682 | 839.4 |
| 1 | 959,427 | 453 | 381,790 | 843.0 |
| 8 | 977,330 | 460 | 388,891 | 845.4 |
| 64 | 982,440 | 463 | 390,915 | 844.3 |
| 256 | **0** | 3,354 | 503 | — |
| 1024 | **0** | 3,360 | 503 | — |
| 4096 | 634 | 381,783 | 272 | — |

At bounds 1-64 work-per-retrace holds within 0.7% and the run gets 2-5% further
for the same blocks. Somewhere between 64 and 256 the game stops progressing
**entirely** -- `fifo=0`, no drawing at all, while retraces multiply by 7.5x.
The obvious theory, that `MSR[EE]` was left clear so interrupts stopped being
delivered, is wrong: interrupts went *up*, 381,696 with the skip on against
16,128 with it off.

So the failure mode is not understood, and the change was reverted. The gate
in the plan said the skip "must fail loudly rather than be tuned until the
numbers look nice", and picking 64 because 256 breaks is exactly that tuning.
2-5% does not buy a cliff nobody can explain -- particularly now that the
profile puts the idle loop at 16% of the slow phase rather than the 19-29% the
wall-clock `pc=` proxy suggested.

What survives is `advance_time()`, the bookkeeping lifted verbatim out of the
dispatch loop and proved equivalent at 20M blocks, which is worth keeping for
readability alone.

## 2026-09-17 — Profiled for the first time: 2.11x on the host, and the console is still unexplained

Nothing here had ever been profiled. Every performance statement in this
repository was inferred from a block counter, and the one optimisation in its
history -- inlining the cache-maintenance no-ops -- was found by reading a
hot-PC list, not by measuring where time went.

`perf` is not available here and `-pg` distorts exactly the thing being
measured, so the chassis samples itself: `SIGPROF` on a 1 kHz `ITIMER_PROF`
timer, the host PC out of the signal's `ucontext`, raw PCs resolved afterwards
against the symbol table. `GV_PROF` names the file. `ITIMER_PROF` and not
`ITIMER_REAL`, so a run blocked on I/O does not accumulate samples it never
spent executing.

The answer was not where anyone would have guessed:

| | before | after |
|---|---|---|
| `mem_read32` | **47.01%** | (inlined) |
| `resolve_addr` | **13.79%** | 0.80% |
| recompiled guest code | 24.91% | **75.37%** |
| GX decode | 0.33% | 0.49% |

Most of the emulator's time went on working out *where guest memory was*. The
cost was structural rather than algorithmic: recompiled chunks are separate
translation units and the accessors live in `cpu.c`, so each of roughly seven
hundred accesses per chunk was a real call into another object file, with a
four-way range check inside it and nothing foldable into the caller.

The fast path is now inline in DolRecomp's header and covers cached MEM1 --
one subtraction, one unsigned compare catching underflow and overrun together,
a byte-swapped load. Everything else falls through to the original code, now
named `*_slow`, so uncommon paths keep exactly the behaviour they had. Stores
take it only with no reservation outstanding and no write journal installed.

**2.11x on the host** -- 16.0s to 7.6s of CPU for the same 40M-block boot --
with output identical to the byte: hits, FIFO writes, MMIO counts, retraces and
GX command count all unchanged. Vita `.text` grows 51.88 -> 61.43 MiB against a
280 MiB budget.

**On the console it does not show, and that is the interesting part.** Three
runs, measured at the same wall-clock marks:

| | t=2s | t=4s | t=6s | t=8s | t=10s |
|---|---|---|---|---|---|
| before | 95,768 | 315,763 | 583,367 | 978,670 | 1,043,487 |
| after | 95,963 | 415,436 | 660,403 | 978,670 | 1,050,765 |
| after, no graphics | 96,285 | 490,992 | 843,775 | 1,041,729 | 1,094,243 |

The gain is real in the middle of the window -- 20-30%, and another 25% with
the GXM backend off -- but every run converges on a plateau and then collapses
to 35-50k blocks/s. The collapse happens with graphics disabled too, so it is
not only the renderer.

What was ruled out rather than assumed: file I/O is **408 ms out of ten
seconds**, 4%, now that the watchdog reports it. The emulated timebase is
derived from cycles, not wall clock, so nothing is pacing to real time.

What is still unknown is the console's steady-state speed, because the
14-second `launch` cap cannot reach the compute-bound phase, and the host
profiler does not port -- the Vita has no `SIGPROF`. An on-device profile needs
a sampling thread reading a phase written by the emulation thread, and the
measurement needs a LiveArea launch to run long enough to mean anything.

So: the host is 2.11x faster and the console is not yet explained. Real-time
is about 4M blocks/s (200M blocks is 50 s of Wii time, confirmed against both
the emulated timebase and the frame counter). The console is doing 35-100k.

## 2026-09-17 — Title screen and file select; the gate was the Nunchuk

The connect-a-remote screen was a deadlock of our own making. The status
report's extension bit was set from `ext_enabled`, which only becomes true
once the host writes `0x55` to `0xA400F0` to bring the port up -- and the host
only does that for an extension it has been told is present, which that bit is
the only way of learning. Nothing broke the cycle.

On hardware the bit means "something is plugged in", which is true before the
console is listening. The two facts are separate, and conflating them is what
deadlocks the handshake. `ext_attached` is now whether a Nunchuk is in the port
(it is, from reset -- SMG requires one, and the Vita has a left stick to drive
it with), `ext_enabled` stays "the port has been initialised", and the
identifier and calibration are installed on attach, because the identifier is
what the SDK reads to find out what it found.

The SDK finds it, initialises it, and switches the report mode from `0x33` to
`0x37` -- the mode carrying extension bytes. The boot then walks through the
connect screen to the **title screen**: the logo with its starfield, "Press
both A and B", (c) 2007 Nintendo.

Holding A and B (`GV_BTN=0x0C00`) clears that too, and the boot reaches the
**file select** -- "Please choose a file", the save-file planets rendered as
textured 3D geometry, the file numbers legible.

So the boot order is now: strap screen -> connect screen -> title screen ->
file select, each one gated by something we can name, and each one rendering.
At 120M blocks: 2.10M draws, 15.1M vertices, 571k display lists, `unk=0
pending=0`.

The first purely cosmetic defect is also the first visible one: a blown-out
white slab across the middle of the file select. The rasteriser has no per-draw
blend state, so a glow pass that should composite additively is drawn opaque.
That is workstream #3 (TEV and texturing) arriving on its own, and it is now
worth doing for what it fixes rather than for what it might.

The submodule pins are honest again. Both carried a local-only commit and both
pins were deliberately left stale, because a pin nobody can fetch is worse than
an old one. We have no push rights on the ExpansionPak repositories, so both
are forked to `eanjohnson` and pushed as a `galaxyvita` branch -- a branch and
not the fork's default, because our commits sit 13 and 151 behind their
upstreams and rebasing them forward would change the code under a tree whose
boot has just been validated frame by frame. Catching up to upstream is its own
job, to be measured, not a side effect of needing somewhere to push.

## 2026-09-17 — The boot renders the strap screen and stops asking for a Wii Remote

With display lists recorded rather than executed, a 90M-block boot dumps
frames that are simply readable. The wrist-strap health-and-safety screen
comes out in full colour with its textures, its text and the Nintendo logo,
and the screen after it is legible too: **"Connect the Wii Remote to Player
1."** That is where the boot now sits.

The boot order is now: strap screen (A dismisses it) -> the connect-a-remote
screen, which nothing dismisses. Behind it the game has already loaded the
title screen and the file select -- `LayoutData/TitleLogo.arc`,
`PressStart.arc`, `StageData/FileSelect.arc`, `LayoutData/FileInfo.arc`,
`ObjectData/FileSelectData{Mario,Luigi,Yoshi,Peach,Kinopio,Planet}.arc` and
`AudioRes/Stream/SMG_title_strm.ast`, the title theme.

At 200M blocks it runs to the cap without a stall: 3,037 frames, 2.89M draws,
22.8M vertices, 1.11M display lists, `unk=0 pending=0`, nothing unresolved.
Disc reads stop growing at 6,598 once file select is loaded, so it is idling
on the gate rather than making progress.

The gate is the one the frame names. The Bluetooth stack looks healthy from
below -- `conn=1 intr=1 reports=3203 mode=0x33 ir=1 leds=0x10`, and LED 1 lit
means the game did assign a player -- but WPAD is not accepting the remote as
player 1. That is the next thing to answer, and unlike every gate before it,
the game says out loud what it wants.

**The census was 26 files short** and had to be regenerated: it was written by
a host run that died 4.5 MB into the GX stream, so it never saw anything past
the strap screen. `scripts/check-staging.sh` passes at 30M blocks on the new
275-file list. Anything staged to the console before this was missing the title
screen and the file select -- exactly the failure mode the census check exists
to catch, arriving on schedule the first time the boot got further.

## 2026-09-17 — Display lists were being submitted as commands; the boot reaches file select

The boot froze at identical numbers across 55M- and 70M-block runs, and the
built-in report pinned it exactly: `gx first unknown opcode:
stream_byte=4456810 ... value=0x02 ... cmds=643902`. `0x02` is not a GX
opcode.

This read like a decode bug and was not one. An independent reference walker,
written from the GX command encoding and run over the whole 4.46 MB capture
with no help from our code, reached the same byte and computed the same
16-byte vertex stride. The decode was right about a stream that should never
have contained those bytes.

What the stream actually said was stranger than a bad opcode:

- **zero display lists** in 4.46 MB, in a game built out of them;
- array bases (CP `0xA0`-`0xBF`) written exactly once, at `GXInit`;
- only two vertex descriptors ever used, both direct-float;
- and then, at byte 4456550, three-byte vertices whose first two bytes are
  equal -- indexed geometry, against a descriptor set for direct floats.

Indexed display-list geometry was arriving in the command stream. The
write-gather pipe does not address the GP: it lands at the PI FIFO write
pointer, and `PI_FIFO_BASE` says where that is. `GXBeginDisplayList` retargets
it at a buffer in memory and `GXEndDisplayList` puts it back. We stored
`pi_fifo_base`, `pi_fifo_end` and `pi_fifo_wptr` and ignored all three, so
every write to `0xCC008000` went to the command processor -- no list was ever
recorded, and the list being recorded was executed instead. Logging the
register showed the base moving to `0x0116dd80` at `gxbytes=4456550`, the exact
byte the parse broke on.

Honouring the register costs about twenty lines:

| | before | after |
|---|---|---|
| draws | 3,784 | 265,166 |
| vertices | 15,192 | 2,101,170 |
| GX stream | 4.5 MB | 22.9 MB |
| display lists | 0 | 96,802, none unresolved |
| unknown opcodes | 204 | 0 |

The boot now runs past the wrist-strap screen and reaches the **file select**:
`LayoutData/FileSelect.arc`, `LayoutData/FileInfo.arc` and
`ObjectData/FileSelectData{Mario,Luigi,Yoshi,Planet}.arc`.

**The wrist-strap gate falls to A.** `GV_BTN=0x0800` with period 90 / hold 30
dismisses it.

Two hypotheses died on the way and are recorded so they are not retried. The
Mii database is not consulted for this: a zero-filled `RFL_DB.dat` of the right
size opens (`ret=6`) and changes nothing. And `WUDIsLinkedWBC` is load-bearing
rather than a leftover -- the state byte at `0x80692846` never leaves `0x00`
even with the full Bluetooth stack up (`conn=1 intr=1 reports=18206 mode=0x33
ir=1`), so the HLE return is the only thing that answers it.

Three instruments were repaired in the process, two of which had been quietly
broken:

- `GV_GX_TRACE` used `signal()`, whose SysV semantics reset the handler to
  `SIG_DFL`, so the second `SIGALRM` killed the run outright five seconds in.
  It has been unusable for every run long enough to be worth tracing.
- The trace was flushed only from the signal handler, so a run that reached its
  block cap wrote the whole capture into `free()`.
- The capture buffer was 1 MiB against a desync at 4.46 MB -- it ran out four
  megabytes before the interesting part.

And one was added: `gx_cp` now keeps a 512-entry ring of decoded commands and
prints it, with the vertex descriptor and all eight VATs, when the first
unknown opcode appears. An unknown opcode says where the parse stopped
agreeing with the game, never why. Why is a command a few back whose length was
wrong, and that ring is what named it.

## 2026-09-17 — 56% of the boot was cache maintenance, and the gate is the wrist strap

Two hot PCs were 56% of every dispatch block in a 5M-block boot: `0x804a2f14`
at 1.77M and `0x804a2f40` at 1.02M. They are `DCInvalidateRange` and
`DCFlushRange` -- three-instruction `bdnz` loops stepping a pointer 32 bytes at
a time.

DolRecomp emitted `dcbi`, `dcbf` and `dcbst` as `ppc_fallback_instruction`
followed by `return`. The chassis fallback matches none of them and falls out
of its tail with `pc = cia + 4`, so the behaviour was **already** "do nothing".
It just reached that answer by a call, a PC write, a return to the dispatcher
and a re-dispatch, once per cache line. Worse, the `bdnz` already had a `goto
label_` fast path and the `return` on the line above meant the loop could never
take it. Inlining the no-op is semantically identical and keeps the loop inside
the generated function.

At the same 5M cap: FIFO writes 9800 -> 14652, GX commands 3512 -> 5430,
retraces 32 -> 46, draws 28 -> 40. The hot-PC profile flattens completely --
top entry 1.77M -> 210k, no dominant sink left. `icbi` deliberately keeps the
fallback: instruction cache invalidation is how self-modifying code announces
itself, it is a signal worth keeping while unhandled, and it is not hot.

With that, 20M blocks reaches `fifo=1020765`, `gx=406095`, 2289 draws, 477
retraces, 438 XFB copies, and the disc read count stops growing at the full
22,589,152-byte census -- everything is loaded and a real frame loop is
running.

**And the screen is legible: it is the wrist-strap warning.** Rendered in
colour, with textures, correctly. So two earlier readings were wrong. The white
frames were not a missing TEV stand-in, they were an earlier boot stage the old
build never got past inside the block cap; texturing and colour work better
than the "everything is white" note suggested. And `persp=0 ortho=9156` is not
a misclassification -- the projection reads
`[0.0031 -1.0000 -0.0044 1.0000 -0.5000 -0.5000] ortho=1`, which decodes as
2/640 and 2/456 against a matching 640x456 viewport. A real 2D screen, drawn
over and over.

So the boot is not slow and not mis-rendering. It is **gated at Task 2**, the
Bluetooth wrist-strap screen (`hle_wud_is_ready` at `0x804EFB1C`): the game
draws the warning and waits for a Wii Remote our BT stack never reports ready.

## 2026-09-17 — Five launch paths, and the bubble is still the only one that works

A second SceShell plugin now exists to test this: `vita/plugin/`, built by
`build-gvlaunch.sh` from upstream vitacompanion plus `gvlaunch.patch`. It
coexists with the installed plugin rather than replacing it -- command port
1339, module name `gvlaunch`, and a `GV_MINIMAL` build that is the command
server and nothing else: no FTP, no input thread, no nosleep, no taipool. It is
listed last in `ur0:tai/config.txt`, so if it ever fails to load the stock
plugin is untouched and the console stays reachable. Both answer after a
reboot: 1338 says `vitacompanion 1.06`, 1339 says `vitacompanion 1.07`.

Every launch path tried, against the same eboot with the same `idle.txt` and
`nogfx.txt` flags the two surviving bubble runs used:

| path | result |
|---|---|
| `sceAppMgrLaunchAppByUri(0x20000)` (stock) | 5 ticks, killed |
| `sceAppMgrLaunchAppByUri(0xFFFFF)` | 6 ticks, killed |
| `sceAppMgrLoadExec("app0:eboot.bin")` | 14 ticks, killed, `proc=` restarts from zero |
| `sceAppMgrLaunchAppByName(0x60000)` | 6 ticks, killed |
| `sceAppMgrLaunchAppByName2` | rc=0, nothing started |
| `sceAppMgrLaunchAppByName2ForShell` | rc=0x00010004 (an app id), nothing started |
| **from the LiveArea bubble** | **30 ticks, ran to completion, clean exit** |

The flags word is not it. The SDK header says `sceAppMgrLaunchAppByUri`'s flags
"must be 0x20000" and that any other value opens the LiveArea instead, so
0xFFFFF was never the different launch path it looked like -- which is exactly
why it measured the same.

Two eliminations worth keeping. The stock plugin is not an app-killer: the
bubble launches survived while it was loaded, and nothing in its source
terminates an app except on the `quit` command. And the kill is not attached to
the process vitacompanion started -- `sceAppMgrLoadExec` produces a genuinely
new process, whose own `proc=` clock restarts at zero, and it is killed 14 s
into *its* life. A successor inherits it.

So the distinction is not which function, and not which process. It is that
every one of these is a programmatic launch, and the one that works is the
shell launching the app through its own UI. `sceAppMgrLaunchAppByName2ForShell`
is the closest thing to that and it returns an app id without starting
anything, which is the signature of a two-stage launch --
`sceAppMgrLaunchAppByName2ndStage` exists next to it in the stub library. Its
NID is published (0xADAC0E8D, and `_`-prefixed 0xDEDD2AED) but its arguments
are not documented anywhere, and calling an undocumented shell-internal
function with guessed arguments risks faulting SceShell, which is the one
failure mode that needs somebody at the device. Stopped there deliberately.

## 2026-09-17 — The cap was vitacompanion's `launch`, and nothing else

The user launched the app from its LiveArea bubble by hand. It ran to
completion, twice.

Same eboot, same slot, same flag files (`idle.txt` and `nogfx.txt`, so the app
emulates nothing and draws nothing -- the bare control), same everything. The
only variable is who started it. `boot.log` is **byte-identical for its first
seventeen lines** between the two paths and then diverges:

| | via vitacompanion `launch` | from the LiveArea bubble |
|---|---|---|
| last boot.log line | `idle: emulating nothing, holding for 60s` | `idle: held 60s without being stopped` + `GalaxyVita phase4 exit` |
| watchdog ticks | 5, to `t=10s` | 30-31, to `t=60s`/`t=62s` |
| ended by | stopped mid-hold | itself, cleanly |

The watchdog even matches rate-for-rate while both are alive (`pres=240, 381,
501, 622, 743` against `241, 362, 482, 603, 724`). One is killed at ten
seconds; the other runs its full sixty and prints its own exit line. Two bubble
launches, 05:59 and 06:03, both 60 s.

**So the ~14 s cap is vitacompanion's `launch` command.** Not the device, not
the plugin set, not `ATTRIBUTE2`, not the missing `livearea` directory, and
nothing GalaxyVita does. Every hardware result in this project up to here was
bounded by the test harness rather than by the port, and every entry below that
says "the app is stopped at ten seconds whatever it does" should be read with
the narrower true statement: *an app launched by vitacompanion* is.

It also retires the failed bisect honestly. The two survivals that would not
reproduce were never noise in the app -- they were noise in the launch path,
which is a thing that plausibly varies run to run in a way an app's own
structure cannot.

What this does not touch: the census fix, which was reproduced on the host by
mirroring the console's file set and never depended on a device run.

The cost of the mistake was most of two days, and the shape of it is worth
keeping: the harness was the one component never under suspicion, because it
was the instrument doing the measuring. Nine suspects were cleared one at a
time, each with real evidence, and every one of them was innocent. The control
app was built specifically to find what GalaxyVita was doing wrong, and its
13-tick death was read as proof the cause was device-wide -- when it was
actually proof the cause was in the only thing the control still shared with
the app, which was the launch.

**The open problem is now a harness problem**, and it collides with the
standing constraint that nobody babysits the console: the app must be startable
unattended, and `launch` is what breaks it. Next, in order: whether
vitacompanion's `launch` is itself killing the app (a held handle or a timer on
its side, in which case dropping the control socket after launching may be
enough); and failing that a second launch path -- `sceAppMgrLaunchAppByName`
from a small loader, or registering the title properly so the shell owns it the
way it owns VitaShell.

## 2026-09-17 — The bubble launch cannot be driven, and two more suspects fall

**vitacompanion's synthetic input does not work.** `press <button>` answers
"Input pressed." and nothing receives it. Pressing cross after a run, with the
shell sitting on the app's LiveArea, launched nothing; and holding cross across
an entire probe run -- pressed before launch, released after it died -- the app
read `buttons=0x00000000` on every one of its thirteen ticks. So the input is
reaching neither SceShell nor the application. Valid targets are cross, circle,
square, triangle, up, down, left, right, start, select, ps, l1, r1; `press`
holds until `release`, and a bare `press` with no release leaves the button
down.

That kills the bubble launch as an unattended test, and it matters beyond this
question: `vita_pad` reads through `sceCtrlPeekBufferPositive`, so synthetic
input can never drive the game either. Any plan that assumed it could is wrong.

**Extended memory is not the cause.** GVITA0001's `param.sfo` carries
`ATTRIBUTE2=0xc` -- the vita-mksfoex flag requesting the larger memory budget,
which GalaxyVita needs for 88 MB of guest RAM -- where VitaShell and VitaDB
Downloader both have `0x0`. It applies to the slot, so even the fifty-line
probe had been running as an extended-memory app all along. Patched to `0x0`,
uploaded and verified: 13 ticks. Rebooted in case SceAppMgr caches the app
database: 13 ticks again. Restored byte-identical.

**One real difference found and not yet tested.** GVITA0001's `sce_sys` holds
only `param.sfo` and `package`. VitaShell and VitaDB Downloader both also have
`icon0.png` and a `livearea` directory. This slot was never populated by a
normal VPK install, and an incomplete registration is a plausible reason for
the shell to reclaim the app -- but testing it needs either a real install
(vitacompanion has no `install`, and VitaShell cannot be driven without working
input) or the probe run from a different, properly registered title's slot.

Cleared so far: the emulator, the app's structure, the plugin set, the
extended-memory attribute. Untested: the registration, and the launch path.

## 2026-09-17 — The plugins are not the cause

Bisected `ur0:tai/config.txt` with nine plugins commented out: NoPowerLimits,
CapUnlocker, PSVshellPlus (kernel and shell), catlog, shellbat, pngshot,
LowMemMode and NoTrpDrm. Left loaded: the kernel infrastructure the rest sit
on (0syscall6, nonpdrm, repatch, kubridge, itls, yamt_helper), henkaku, and
vitacompanion -- which cannot come out, since it is the only way in.

Uploaded, read back and compared byte for byte before rebooting, and the
lifelines were checked as still uncommented on the device itself rather than
in the local copy. Device back in 31 s.

**Three probe runs, three deaths at 13 ticks** (proc=13499ms and alike). With
the plugins on, the probe survived twice in nine; with them off, zero in
three. That is no change, so the plugin set is cleared.

Config restored byte-identical to the backup, zero `GV-BISECT` lines left,
rebooted, and the current eboot put back in the slot. The console is exactly
as it was found -- worth stating, because PSVshellPlus is what the clock note
in the boot log depends on.

What is left to suspect, now that the emulator, the app's structure and the
plugins are all cleared: the launch path itself. Every run in this log was
started by vitacompanion's `launch`, and vitacompanion is the one thing that
could not be taken out of the experiment. Launching the same title from its
LiveArea bubble instead -- drivable unattended with vitacompanion's `press` --
is the test that has not been run, and it is the only remaining variable that
is ours rather than Sony's.

## 2026-09-17 — The cap is the console's, and it is intermittent

`vita/host/lifetime_probe.c` is the control: about fifty lines, ticking once a
second to a synced log, built as the same unsafe fself, put in the same
registered slot, launched the same way. No guest RAM, no AOT code, no chassis,
no graphics. **It dies after 13 ticks, at proc=13842ms.**

So the cap is not GalaxyVita's doing, and never was. The real app's watchdog
"t=10s" and the probe's ~14s of process time are the same wall -- GalaxyVita
spends several seconds allocating 88 MB and loading the DOL before its
watchdog starts counting. Every device number in this log has been bounded by
this and read as an emulator problem.

**A bisect I then got wrong, recorded because the mistake is the useful part.**
The probe takes flag files adding one GalaxyVita trait at a time. Run in
order:

| flags | result |
| --- | --- |
| (none) | 13 ticks |
| display | **60 ticks** |
| display + mem | **60 ticks** |
| display + mem + stack + threads | 13 ticks |
| display + stack | 13 ticks |
| display + smallstack | 13 ticks |
| display + poll | 13 ticks |
| display + threads | 13 ticks |
| display (repeat of row 2) | 13 ticks |

Read top-down that looks like a clean answer -- present a frame and live,
spawn a worker and die -- and it is wrong. The last row is the same
configuration as the second, and it died. The two survivals do not reproduce,
so nothing in the table controls anything: not presenting (GalaxyVita presents
at 60/s with `prc=0x00000000` and dies anyway, `pres=743` by t=10s), not the
88 MB, not the 8 MB stack, not the thread count, not whether main blocks in
sceKernelWaitThreadEnd. Two unreplicated survivals are not a signal, and
bisecting off them cost six device runs.

What is established: every application tested is stopped ~13.9 s after launch,
the timing is sharply repeatable, it is unrelated to load (idle and flat-out
die alike), and twice something suppressed it for a full 60 s and did not do so
again.

> **Superseded 2026-09-17.** Every run behind this paragraph was started by
> vitacompanion `launch`, which is the thing doing the stopping. Launched from
> the LiveArea the same binary runs to completion. The two unexplained 60 s
> survivals were the launch path being intermittent, not the app.


Not yet tried, and the obvious next move: the plugin set. `ur0:tai/config.txt`
loads NoPowerLimits, CapUnlocker, PSVshellPlus (kernel and shell), catlog,
yamt, LowMemMode and others. GVITA0001 is not listed there, so no user plugin
is injected into our app -- but kernel and SceShell plugins still apply, and an
intermittent timer that stops an application is exactly the shape of one of
them. Bisecting it means editing `ur0:tai/config.txt` and rebooting, which
vitacompanion can do unattended, but it changes the console's system
configuration and must keep vitacompanion itself loaded or the device stops
being reachable.

## 2026-09-16 — The boot was never stalling; the app was being stopped

With the census fixed the console ran materially further -- past the old
ceiling to 1,121,547 blocks, with the dispatch rate rising (+104k, +199k,
+332k, +486k per two seconds) instead of decaying as it used to. Then every
thread went quiet at once, as before.

**It is not a stall and it is not at a block count.** `idle.txt` (GV_IDLE=1)
runs the whole setup and emulates nothing: the watchdog stops at t=10s with
`hits=0`, no FIFO, no disc, no emulation. The app is stopped 10-12 seconds
after launch whatever it does. The "block count it stalls at" was only ever
how far the emulator got in ten seconds, which is why it moved from 810k to
1.2M when the media fix made the boot faster -- and why the earlier
"time is the constant" guess, retracted at the time as unfalsifiable, was
pointing the right way.

Ruled out, each with evidence rather than argument:

- *The emulator, entirely.* An idle run with no emulation dies identically.
- *The graphics backend.* `GV_NOGFX=1` reached 1,232,689 blocks, same end.
- *Memory.* Free memory is flat on every tick to the last line before death:
  `free=63488k cdram=93184k phy=26624k`, 62 MB spare, never moving.
- *File I/O.* The last line before death reports `wr=131 probe=0 lost=0`: the
  held-handle write succeeded, a fresh open succeeded, no tick was lost.
- *The watchdog exiting on an error.* It used to `return` when a reopen
  failed. It no longer can, and still stops at t=10s.
- *A crash.* Core dumps work on this device -- nine were written earlier the
  same day, 100-800 KB each -- and these runs produce none.
- *Sleep, screen-off, or a recoverable suspend.* `screen on`, `nosleep on` and
  a button press after it goes quiet do not wake it; it never resumes, over
  160 s of watching.
- *Never presenting a frame.* A display heartbeat holding a black frame, with
  graphics off so it genuinely runs, changes nothing.
- *System callbacks never being deliverable.* Nothing in this app was ever in
  a state to receive one; the CB forms of the thread waits change nothing.
- *A safe fself.* `vita_create_self(... UNSAFE)` already.

So: a Vita application here is terminated about ten to twelve seconds after
launch, for a reason not yet found, and no run of this emulator has ever
outlived it. That reframes the ceiling on every device result in this log.

One untested hypothesis worth recording, not yet evidence: the trap in
CLAUDE.md that "ShaccCg wedges after 5-9 compiles in one process" has the
same shape, and a warmer that has to batch-and-resume across launches is what
you would build if launches were silently capped at ten seconds. The selftest
does not settle it -- it only ever needed two compiles and finished in
seconds, passing 5/5 either way.

## 2026-09-16 — The stall past it was one file the census lost

The boot got past the audio-register wall and stopped again at 770k-810k
blocks: every thread silent at once, the process still resident, no core dump.
It read as a Vita-side hang because the host ran the same code to 20M blocks
without trouble. It was not Vita-side at all.

**The host and the console were not booting the same disc.** The console gets
only the files `generated/census.txt` names; the host gets the whole 3.2 GB
extraction. Mirroring the console's exact file set locally -- symlinks, 256
files -- reproduced the failure on the host immediately and exactly:
`fifo=8660 mmio_w=13284 last=0x804ab358` at 806,893 blocks, matching the
console's last watchdog line counter for counter. With the full extraction the
same block count gives `fifo=1310 mmio_w=5636`. One file was absent:
`files/AudioRes/SMR.szs`, which is the *first* thing the boot reads. It failed
49 times, the audio load bailed out, and the guest went off down an error path.

**The census lost it to uninitialised memory.** `DiscEntry` comes out of
`realloc` and `push()` never set `first_seen`; `disc_read` only assigns it when
it reads back as zero, so an entry that landed on dirty memory counted as
already-seen for the whole run and never entered the census. It came back as
`fs=4173317439`. Regenerating the census before the fix reproduced the same 248
files byte for byte -- same allocation pattern, same garbage -- which is what
made it look like the file genuinely was never read. With `first_seen = 0` the
census is 249 files and 22,589,117 bytes, up by exactly 196,608: `SMR.szs` to
the byte.

Two dead ends worth recording, both cheap and both wrong. Peak host stack
through the region is under 256 KB against the 8 MB the AOT thread gets, so
this was not the stack overflow whose signature it shares. And UBSan
(alignment, bounds, shift, object-size) is clean through 3M blocks across the
runtime and `gx_cp`, so it was not undefined behaviour that x86 tolerates and
ARM does not.

`scripts/check-staging.sh` is the check that found it, kept: it builds the tree
`stage-media-vita.sh` would send, boots it, boots the full extraction, and
requires the two to be indistinguishable. It passes at 20M blocks. The console
also gained a disc line in its log, naming any file a read wanted and could not
open -- it previously had no way to report the one failure only it can have.

**Not yet confirmed on hardware.** `10.42.0.206` is off-network (1337 and 1338
both time out) and this environment has no `.env.local`. The eboot is built and
the regenerated census is in place; the run is `./scripts/stage-media-vita.sh
generated/census.txt` then `./scripts/run-vita.sh`.

## 2026-09-16 — The console was running last week's code

The boot stopped at 659,568 blocks in a poll of an audio register, and the host
sailed past the same point. Two separate causes, one behind the other.

**The Vita links prebuilt AOT objects and nothing checks them.** CMake globs
`generated/build/aot-vita`; no dependency edge runs from an object back to the
chunk it was compiled from. The chunk holding `AIInit` was rewritten when
`patch-aot-hle.py` added the dispatcher escape for the `__AI_SRC_INIT` hook, and
its Vita object was never rebuilt -- `measure-aot-size.sh` skips the Vita half
when VITASDK is missing and says so on stderr and nowhere else. So on the
console that call compiled to a local `goto` and never reached the chassis: the
hook was present, correct, and unreachable. `scripts/check-aot-fresh.sh` fails
the configure now, asking ninja rather than comparing mtimes.

**The register it was polling was dead.** AISCNT at 0xCD006C08 free-runs at the
sample rate while PSTAT is set; it was one word of a four-word file that never
moved, so `AIInit`'s clock probe -- reset the counter, start it, spin until it
ticks -- could never finish. Modelling it turned out to be smaller than the stub
that was hiding it: both measurement loops now exit after two or three
dispatches, the probe accepts on its first pass, and the HLE entry came out. A
20M-block host run against the stubbed build is identical on every number
(`fifo=608906 retrace=306 gx=242178`, same IOS, Bluetooth and disc totals)
except the MMIO reads the probe itself does.

On the console the wall is gone. The old ceiling was 659,568 blocks with
`fifo` stuck at 1310; the same boot now runs through it, and the FIFO climbs
past 6,000 and keeps going.

Finding it took six device round-trips, and the tooling took most of the blame:

- `scripts/run-vita.sh` is the device loop in one command now -- upload, launch,
  stream the watchdog live, fetch the logs -- and it sets or clears `nogfx.txt`
  explicitly every run instead of inheriting it.
- The watchdog was calling `log_flush()` *before* writing its own line, which
  takes the log mutex. Wedge the emulator inside a flush and the watchdog blocks
  on the very thing it exists to report on, turning every diagnosis into "no
  output". Its line goes out first now, and the flush after it is a try-lock.
- A held file handle buffers, so the last ticks before a wedge never reached the
  card. Synced.
- `GV_TRACE` takes a starting block, without which a window trace on something
  that happens at block 683,844 is capped long before it gets there.

**The boot now stops somewhere else**, at 770k-810k blocks, and it is a
different shape of failure: every thread stops producing output at once, the
process stays resident (vitacompanion `kill` answers "Killed."), and no core
dump is written. Ruled out so far: the graphics backend (a `nogfx.txt` boot
stops identically), host FTP polling (a run with no polling at all reaches the
same place), a log-mutex deadlock and unflushed writes (both fixed above, no
change), a full memory card (a 64 MB upload succeeds), auto-suspend (`nosleep`
re-asserted every five seconds), and missing media (re-staged from a fresh
248-file, 22.4 MB census taken at 20M blocks). The host runs the same code to
20M blocks without trouble, so it is Vita-side.

## 2026-09-16 — The GX stream reaches the Vita's GPU

The graphics backend is real hardware now, not a host oracle.
`vita/gfx/gxm.c` + `shader_cache.c` + `gx_gxm.c` take the GX FIFO through
ModernGekko's command processor and out to sceGxm: context and ring buffers,
960x544 double-buffered display queue, on-device Cg compilation persisted to
`ux0:`, and a `GxSink` that turns GX transform/viewport/primitive state into
GXM draws.

Unattended self-test on the console (`scripts/gxm-selftest-vita.sh`):

```
programs: compiled=2 cached=0 failed=0
gxm frames=2 scenes=2 draws=8 vtx=30 drop_full=0 drop_nomtx=0 drop_novp=0
    drop_prim=0 scale=1.214 copy=640x448
probe clear 0d1a29  triangles ff3030  quads 30ff30  strip 4060ff  fan ffd040
```

Five pixel probes, all exact; the fetched frame shows all four topologies in
their quadrants. Then with the game attached, **the backend drew SMG's own
geometry on the console**: `gx cmds=486 draws=2` in, `gxm scenes=1 draws=2
vtx=8` out. The shader cache survives across builds -- a later, differently
linked binary launched `compiled=0 cached=2`.

vitaGL, mathneon, vitaShARK and SceShaccCgExt are off the link entirely. The
self is UNSAFE, because a safe self loads the ShaccCg module and then fails
every compile.

Two device-only defects had to go first, and neither was visible on the host:

- **The boot was I/O-bound on its own logging.** One open/append/close per log
  line on a memory card, and a line per PPC control-register write. The
  dispatch rate decayed 9614 -> 9367 -> 3266 -> 1163 blocks per two seconds as
  the log grew, which reads exactly like a hang. Batching to one write per
  12 KB took the same binary from ~196k blocks to 659,568.
- **Nothing could distinguish a hung boot from a slow one.** There is a
  watchdog thread now -- own file, no shared mutex, dispatch count every two
  seconds -- and a `nogfx.txt` flag that boots with graphics disabled. That
  flag is what exonerated this backend: with graphics off the boot stopped in
  exactly the same place.

Where it stops is now the interesting question, and it is a CPU/HLE question:
`stop=spin` at `0x804C82B0`, polling an OHCI register. At the same 659,568
blocks the host chassis shows the same IOS state, the same `retrace=5` and the
same `hangs=3`, but `mmio_r=4361` against the console's `5257974`.

Also new: `vita_create_self` aborts about half the time with "Cannot allocate N
bytes for SCE data at end of segment 0" depending purely on where the read-only
segment lands, so `build-vita.sh` pads and retries rather than leaving a coin
flip in the build.

See [docs/GRAPHICS-BACKEND.md](docs/GRAPHICS-BACKEND.md).

## 2026-09-15 — The Wii Remote reaches the game

`runtime/bt.c` models the Wii's Bluetooth dongle at HCI level with one remote
permanently synced to it, so every line of the game's own WPAD/KPAD code runs
recompiled (see [ARCHITECTURE.md](docs/ARCHITECTURE.md#bluetooth-and-the-wii-remote)).
Four defects kept it off the air. Each was found by reversing the SDK's own
stack in `sys/main.dol` and reading the state it left in RAM.

1. **Wrong HCI event code.** `EVT_RETURN_LINK_KEYS` was `0x0D` — QoS Setup
   Complete — instead of `0x15`. BTM's Read_Stored_Link_Key (`0x0C0D`, issued by
   WUD at `0x804F6690`) therefore never delivered a key, so the device record
   WUD had built from SYSCONF `BT.DINF` kept `+92 = 2` ("DINF-only") and the
   purge at `0x804E5ADC` deleted it. When the remote then connected,
   `BTA_DM_LINK_UP_EVT` (`0x804E8554`) could not find a record and disconnected
   it as unpaired: `0x804E85DC` → `0x804F48C4` → `btsnd_hcic_disconnect`, reason
   `0x13`. That `0x0406` was the blocker inherited from the previous session.
2. **BD_ADDR byte order in SYSCONF.** `scripts/make_nand.py` wrote the wire
   (little-endian) address into `BT.DINF`, but BTE's `STREAM_TO_BDADDR` reverses
   every address it parses out of an HCI packet, so WUD compared
   `01 00 00 1d 19 00` against `00 19 1d 00 00 01` and missed. DINF now stores
   the address back to front; `WIIMOTE_BDADDR[::-1]`.
3. **Connection direction.** The model waited for the console to open the HID
   channels. A real remote opens them itself — PSM `0x11` control, then `0x13`
   interrupt — and the SDK's HID host only ever answers. `connect_request()`
   plus handlers for L2CAP signalling `0x03`/`0x05` and a countdown in
   `bt_tick()` put the remote in the initiator's seat.
4. **Duplicate L2CAP configuration request.** This is what kept every HID output
   report off the air after the link was up. The remote sent a configuration
   request twice per channel: once on the connection response, once more when
   answering the console's own request. The second one puts the SDK's CCB back
   into `CST_CONFIG` and clears both halves of `config_done`; the SDK answers a
   repeated request (inbound half comes back) but never repeats its own request,
   so the outbound half is never re-confirmed and the channel sits in
   configuration for good. `L2CA_DataWrite` drops every packet on such a channel.
   `config_request()` is now one-shot per channel, tracked in `Bt.cfg_req_sent`.

   The evidence was the CCB itself. `l2cu_find_ccb_by_cid` (`0x8050AB74`) puts
   channel `cid` at `0x80695240 + 376 + 124*(cid - 0x40)`; `+4` is the state word
   and `+52` is `config_done` (`0x80506A7C` ORs in `0x01` for the inbound half,
   `0x80506930` ORs in `0x02` for the outbound). The interrupt channel read
   `state = 5` (`CST_CONFIG`) and `config_done = 0x01` — exactly the shape above.

The call chain from the game down to the air, all of it reversed this session
and useful next time something stops at the edge of the stack:

    0x804D5A60 WPAD command dispatcher   (per-channel struct table 0x80660170)
    0x804DC4D0 command executor          (bails if the BTA handle at +2243 < 0)
    0x804F2364 BTA_HH_SendData           (posts event 0x1708 to the BTA task)
    0x804F28B0 bta_hh_hdl_event          (cb_index[] at 0x8069215C)
    0x804F255C bta_hh_sm_execute         (state at cb+28, tables at 0x8054DA30)
    0x804F1D2C bta_hh_write_dev_act      (action table 0x8054DA00, index 8)
    0x80502E18 HID_HostWriteDev          (device records at 0x80694E38, 52 bytes)
    0x80504E9C hidh_conn_snd_data        (interrupt CID at +22, MTU at +24)
    0x80505B20 L2CA_DataWrite
    0x80505E2C l2c_csm_execute           (state jump table 0x805F2B30)

**Result**, `./scripts/run-host-chassis.sh generated/extracted/game/sys/main.dol 60000000`:

    bt cmds=30 unknown=0 events=86 acl_in=45 acl_out=3199 reports=3147
       conn=1 intr=1 mode=0x33 cont=1 ext=0 ir=1 leds=0x10 out_seen=0x06fe

`out_seen=0x06fe` is output reports `0x11` LEDs, `0x12` report mode, `0x13` and
`0x1A` IR enable, `0x14`/`0x19` speaker, `0x15` status, `0x16` write memory,
`0x17` read memory — the console reads the remote's EEPROM calibration, lights
player 1's LED, sets IR sensitivity and asks for mode `0x33` (buttons +
accelerometer + extended IR) continuously. The link survives the whole run;
`0x0411` Authentication_Requested and `0x0803` Sniff Mode are answered and no
Disconnect follows.

**The game acts on it.** `chassis_set_input_fn()` hands the runtime a pad
sampled at the remote's own 200 Hz; `tests/host/aot_chassis.c` drives a scripted
one from `GV_BTN` / `GV_IR` / `GV_BTN_PERIOD` / `GV_BTN_HOLD`, and
`vita/host/main.c` maps SceCtrl to it (right stick → pointer, left stick →
nunchuk, `L`/`△` → Z/C). At 60M blocks:

| | no input | `GV_BTN=0x0800 GV_IR=512,384` |
|---|---|---|
| GX bytes | 10,050,335 | 23,870,374 |
| draw commands | 9,202 | 113,074 |
| vertices | 41,008 | 833,196 |
| display lists | 2,056 | 103,011 |

Pressing A gets the game off the attract screens and into drawing real
geometry. Not yet verified on hardware: no VitaSDK on this host, so
`vita/host/main.c` is unbuilt, and the Vita host still does not call
`chassis_attach_media()`.

Open: `gx unk=2178` unknown FIFO commands appear once real geometry flows;
`fs misses=3` (`play_rec.dat`, `RFL_DB.dat`).

## 2026-09-14 — Phase 4: first GX FIFO write; game reaches `main`

Four boot gates fell in order. Evidence is `scripts/run-host-chassis.sh generated/extracted/game/sys/main.dol 4000000`.

1. **Error #001 "unauthorized device has been detected"** was the 38k-iteration "idle" loop: `OSFatal` at `0x804A6C1C` spinning after the SDK blacked the screen. RMGE01's anti-modchip check (`0x804C5764`) runs **two** probes and demands an exact drive error from each — reported by `DVDLowRequestError` into `0x806577E0`:
   - probe 1 `DVDLowUnencryptedRead` of `0x460A0000` (32 bytes, via `0x804C6A70`) must fail with `0x052100` (`BlockOOB`); compared at `0x804C5864` (`lis r4,5` + `addi r0,r4,8448`).
   - probe 2 `DVDLowReportKey` **`0xA4`** (via `0x804C7384`) must fail with `0x053100` **or** `0x052000` (`InvalidCommand`); compared at `0x804C590C`/`0x804C591C`.
   Both probes must return DIResult `2` (`DriveError`), not `0`/`1`. We had been reporting `0x053100` for probe 1 and answering `0xA4` from the `default: DI_SUCCESS` arm. Fixed in `runtime/ios.c`; `OSFatal` no longer fires.
2. **`OSPanic OSPlayTime.c:775 'Expired'`** followed. `ES_DIGETTICKETVIEW` (`0x1B`) and `ES_GETCONSUMPTION` (`0x16`) returned `IPC_SUCCESS` without writing their io vectors, so `ESP_GetTicketView` handed the SDK uninitialised stack that parsed as an expired play-time limit — and a garbage vector length made `DCFlushRange` walk ~56 MiB (1.77M dispatch blocks) between the two calls. `handle_es_v` now writes a zeroed `TicketView` (0xD8, title id `00010000-524D4745`, all content permissions set) and a zero consumption count.
3. **`OSPanic FileRipper.cpp:112 "File isn't exist."`** on `/AudioRes/SMR.szs`. `DVDConvertPathToEntrynum` (`0x804BEF5C`) was reading a **zeroed** FST. The apploader HLE had parked the FST at arena hi (`0x817EAEC0`) the way a real apploader does, but RMGE01's `OSInit` recomputes arena hi as the end of MEM1 (`OSReport "original arenaLo = 0x806cdfa0 arenaHi = 0x81800000"`) and ignores what we leave at `0x80000034`, so JKRHeap cleared the FST when it took the arena. Arena **lo** is honoured, so the FST now lands at arena lo (`0x806CDFA0`) and `0x80000030` is pushed past it. Guest FST reads back `n=2447` entries.
4. With the FST live the disc backend started serving: **`disc reads=152 bytes=588896 misses=0`**, `di reads=13`, `last_di=0x71` (`DVDLowRead`).

Result at 4M blocks: `enter main hits=199943` — the game reaches `main` (`0x803994D0`) for the first time — and **`GX FIFO write ea=0xcc008000 pc=0x804bd0ac`**, `fifo=634`, `gx=276 commands / 1513 bytes (cp=41 xf=7 bp=221 draw=1)`. The Phase 4 first-FIFO-write gate is **PASS**.

New blocker: hard spin at `0x804EB3B0`/`0x804F5500` — the WUD (Wii Bluetooth) state machine polling for state `5` at `0x80692846`. `/dev/usb/oh1/57e/305` opens and submits one `ioctlv 0x2` (HCI interrupt/event read); we reply `IPC_SUCCESS` immediately with no event, so the state machine never advances. A real HCI event read stays pending until an event exists. 40M blocks changes nothing but the spin count.

New host-side hooks in `runtime/chassis.c`: `OSPanic` (`0x804A3F24`, prints file/line/format — the only way to see an assertion, since it prints through `OSReport` and then parks in `PPCHalt`) and `DVDConvertPathToEntrynum` (`0x804BEF5C`, prints the path plus the live FST header). `runtime/ios.c` gained `handle_es_v`.

Vita eboot **not** rebuilt or deployed for this change; `10.42.0.206` was unreachable at last check.

## 2026-09-14 — Phase 4: empty OPEN paths were wiped .sdata

- Empty `IOS_Open` at `0x935E1000`/`0x935E1080` was not a bad IPC layout. DVDLowInit `strncpy`s `/dev/di` from SDA `0x8069E1E8`; that byte was 0 in guest RAM.
- DOL BSS `0x805F5A40` size `0xB8550` is a bounding box that overlaps initialized `.sdata`/`.sdata2` (`data6` `0x8069CCA0`, `data7` `0x806A3280`). Chassis zeroed BSS *after* mapping data, wiping `/dev/di`, `/dev/es`, `/dev/fs`.
- Fix: zero BSS first, then copy sections (real DOL load order). `to_ea` also maps uncached `0xC`/`0xD` mirrors; `IOS_Open` entry snapshots `r3` in case the IPC bump buffer is reused.
- Host: `IOS_Open("/dev/fs")`, `"/dev/es"` (ioctlv `0x20`/`0x1d`), `"/dev/di"`. 9 opens, last=`/dev/di`. No `pathname '/dev/di' does not exist`. Still `fifo=0` at 500k (DI reads are zero stubs).
- TITLEID `GVITA0001`, version `00.05`. Title-screen gate is **not** passed. Next: DI/FS content (disc FST / NAND) so boot can issue a GX FIFO write.

## 2026-09-14 — Phase 4: left PPCHalt; OSInit; scheduler idle

- `0x804A0970` was **PPCHalt**, not an IPC poll. OSInit panics if the IOS 33 IPC arena at `0x80003130` is missing.
- Lo-mem now matches Dolphin IOS 33: MEM2 arena `0x90000800–0x935E0000`, IPC buffer `0x935E0000–0x93600000`, Hollywood rev `0x11`.
- `runtime/ios.c` answers OPEN/CLOSE/IOCTL/IOCTLV (ES/FS/DI/STM/BT stubs). STM eventhook hangs. Physical MEM1/MEM2 (0x0… / 0x10000000) is mapped for Starlet pointers.
- External IRQ: the RVL stub at `0x80000500` `rfi`s into `0x804A1D3C` (AOT-covered). Decrementer SPR 22 is no longer a fake constant.
- Host and device match. Device `boot.log`: Revolution OS, firmware 33.14, 88 MiB, MEM1 `0x806cdfa0–0x81800000`, MEM2 `0x90000800–0x935e0000`, then `idle wait, no runnable thread hits=27462 fifo=0` at `0x804AB358`. `ios opens=0`. vitaGL still `960x544 shacc=1`. Clean `phase4 exit`.
- TITLEID `GVITA0001`, version `00.04`. Phase 4 title-screen gate is **not** passed. Next: wake `SelectThread` (empty run queue; no IOS_Open yet) so boot can reach a GX FIFO write.

## 2026-09-14 — Phase 4: IPC handshake; left SelectThread

- Empty run queue was `OSSleepThread` on the IOS 33 IPC queue `0x935E004C` (`lr=0x804D267C`). The IY-enable ACK IRQ was firing *before* the waiter slept (lost wakeup).
- Chassis now delays that ACK 64 slices, sets Hollywood `0xCD000030` bit `0x40000000` (RecompCore `WII_IPC`), level-triggers PI IPC from Y/IY, and remaps the `0x80000500` stub’s default `rfi` (`0x804A1D3C`) to the installed EXTERNAL handler `0x804A881C`.
- Host: `IOS_Open("/dev/stm/immediate")`, `"/dev/stm/eventhook"` (ioctl `0x1000` hangs, correct), `IPCCltInit`, then game/SDK PCs (`0x8046xxxx` / `0x804A6CC8`). Two OPEN path reads from `0x935E1000` are empty (`ENOENT`). Still `fifo=0` at 500k hits.

## 2026-09-14 — Phase 4 start: 960×544 present + Flipper HLE

- The old `0x800043E0` "spin" was BSS zeroing. AOT returns at `DOLRECOMP_C_LOOP_CYCLE_BUDGET`; the chassis never replenished `downcount`, so the same PC looked frozen.
- Chassis now slices `downcount`, HLEs Flipper PI/VI/CP/PE/DSP/EXI/AI/SI/DI and Hollywood IPC/AHB, skips null `bctrl` and `sc`, and retries FP-unavailable.
- Host and device both leave `__start` and run OS init. Device: `stop=spin hits=239026 fifo=0 last=0x804a0970 retrace=478` (IOS/IPC wait, not GX yet).
- GPU thread presents **960×544** on CPU1. ShaRKBR33D had already put `libshacccg.suprx` in `ur0:data/`; vitaGL loads `ur0:data/external/libshacccg.suprx` (copied on boot). `vglInitExtended` returns `GL_FALSE` on a successful 960×544 init — that is not a failure.
- Guest RAM is allocated before GPU init (24+64 MiB). eboot SELF **23 MiB**. TITLEID still `GVITA0001`, version `00.03`.
- Phase 4 gate (recognizable title screen) is **not** passed. Next: finish Hollywood/IOS enough to reach a GX FIFO write at `0xCC008000`, then vitaGL once `libshacccg` is on the device.

## 2026-09-14 — Phase 3 CoreHost on-device (dispatch hits > 0)

- Vita eboot links 1,323 prebuilt AOT objects + DolRecomp `cpu.c`/`dol.c` + `runtime/chassis.c`. ELF 59 MiB, compressed SELF **22 MiB**. TITLEID `GVITA0001`, ATTRIBUTE2=+109 MiB.
- User DOL pushed to `ux0:data/galaxyvita/sys/main.dol` (not in the VPK). Hash still the GalaxyPad RMGE01 pin.
- `ux0:data/galaxyvita/boot.log` on device:
  - `cpu-aot` CPU0 / `gpu-fifo` CPU1 / `audio-io` CPU2
  - `chassis MEM1=25165824 MEM2=67108864 broadway=1`
  - `dol mapped pc=0x8000403c`
  - `stop=spin hits=356 misses=0 fifo=0 smc=0 last=0x800043e0`
- Gate: Broadway boot, MEM1/MEM2 mapped, AOT dispatch hits > 0, no SMC storm. **PASS**. Black screen. First GX FIFO write not reached — boot spin at `0x800043E0` (needs PI/VI/IRQ HLE).
- Host chassis (`./scripts/run-host-chassis.sh`) matches the same stop.
- Null video/audio. RecompCore/Dolphin is not linked; chassis is DolRecomp CPUState + AOT.

## 2026-09-14 — Phase 2 AOT size gate PASS

- ModernGekko pinned `5417826` (`third_party/ModernGekko`). RecompCore is the vendored graph at `vendor/dolphin` `55c7b023` (`moderngekko-vendor`). Nested Dolphin Externals (Qt, FFmpeg-bin, …) are **not** checked out.
- Disc inventory (`wit FILES` paths only): 1 DOL, 0 REL, 1 RSO (`HomeButtonMenuWrapperRSO.rso`), 1 SEL (`product.sel`). HOME-menu RSO is not AOT'd.
- DolRecomp C backend, Broadway, 1024-instruction chunks → 1,322 C files, 320 MiB of generated source (gitignored). 20 possible SMC sites, unpatched.
- Host objects: **70.62 MiB** `.text`. Vita thumb+NEON objects: **51.88 MiB** `.text`. Gate 280 MiB: **PASS**.
- `tests/host/mem2_smoke.c`: MEM1=24 MiB, MEM2=64 MiB.
- Numbers: `docs/AOT-SIZE.md`. Scripts: `generate-aot.sh`, `inventory-disc.sh`, `measure-aot-size.sh`.

## 2026-09-14 — Hello host on-device; eboot hot-update works

- User installed `galaxyvita.vpk` once via VitaShell. TITLEID `GVITA0001` is in `ux0:app/` + `ux0:appmeta/`.
- First launch via vitacompanion (`launch GVITA0001`) wrote `ux0:data/galaxyvita/boot.log`:
  - `cpu-aot` affinity `0x10000` currentCpu=0
  - `gpu-fifo` affinity `0x20000` currentCpu=1
  - `audio-io` affinity `0x40000` currentCpu=2
  - then `GalaxyVita hello host exit` after the 30s vblank loop.
- `./scripts/deploy-vita.sh` now `destroy`s, FTP-pushes `eboot.bin`, and relaunches. No VitaShell after the first VPK. `kill TITLEID` on this vitacompanion 1.06 build fails; `destroy` works.
- Phase 1 gate is closed. Next is Phase 2 remaining work: pin ModernGekko + RecompCore, then AOT size.

## 2026-09-14 — Vita plugins downloaded

- All plugins from `docs/VITA-SETUP.md` fetched into `extern/vita-plugins/` (binaries gitignored).
- Includes vitacompanion 1.06 (user+kernel), catlog v1.2, PrincessLog v1.0, CapUnlocker v1.4 (CoreUnlocker80000H successor), LowMemMode v1.2, PSVshell v1.2beta, PSVshellPlus v1.4, oclockvita 1.2.1, ShaRKBR33D v.1.2.
- `libshacccg.suprx` is Sony's shader compiler and is **not** downloaded. ShaRKBR33D extracts it on the Vita.
- Hashes and install paths: `extern/vita-plugins/MANIFEST.md`. Re-fetch: `./scripts/download-vita-plugins.sh`.

## 2026-09-14 — DOL identity matches GalaxyPad RMGE01 rev 0

- `wit` v3.05a r8638 (host, gitignored under `extern/wit`) dumps the WBFS as `RMGE01` / NTSC-USA / IOS 33 / 1 signed DATA partition.
- Extracted **only** `sys/main.dol` (6,283,264 bytes). SHA-256 `2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09` **equals** GalaxyPad's pin. Container WBFS hash still differs; executable identity is the same.
- Host DolRecomp (`40637c4`, C backend, no LLVM) builds; 19/19 tests pass. Wii extract uses the wit bridge.

## 2026-09-14 — Phase 1 host toolchain + hello VPK

- VitaSDK **2026.08** (`vitasdk-core-2026.08.1-1`) installed at `$HOME/vitasdk` via vdpm. `arm-vita-eabi-gcc` is **15.2.0**, `--with-arch=armv7-a --with-tune=cortex-a9 --with-fpu=neon --with-float=hard --with-mode=thumb`.
- vdpm packages: `zlib`, `vitaGL` (plus taihen, vitaShaRK, SceShaccCgExt, libmathneon).
- `vitacompanion-cli` at `~/.local/bin/vita` (`vita payload`, `launch`, `copy`, …).
- Hello-world host `vita/host/main.c` compiles: three affinity-pinned threads (`0x10000/0x20000/0x40000`), log to `ux0:data/galaxyvita/boot.log` and `sceClibPrintf`.
- Artifacts (gitignored): `generated/build/vita/eboot.bin` (48 KiB), `generated/build/vita/galaxyvita.vpk` (45 KiB), TITLEID `GVITA0001`, ATTRIBUTE2=+109 MiB.
- **Not deployed.** `GALAXYVITA_IP` is unset; no device on this LAN from the host's point of view.

## 2026-09-14 — Disc container hashed (not extracted)

- File `Super Mario Galaxy [RMGE01].wbfs` left in place, gitignored, unmodified.
- Size **3,508,535,296** bytes — same as GalaxyPad's RMGE01 rev 0 container size.
- SHA-256 `0f5ba5a1c9da2a3319daa509db064a41e16a2a04c9114802a8f8addc14c1b2de` — **does not match** GalaxyPad `bd0d3d4bc1376a8614fd8f4fee5df86be6bc676e9d617944918fda79ed9e3589`.
- Do not treat this as the GalaxyPad pin until `sys/main.dol` is extracted and hashed. WBFS padding can share a size and still differ.

## 2026-09-14 — Phase 0 repo

- `git init`, `.gitignore` rejects WBFS/DOL/VPK, `scripts/check-repository.sh` passes.
- First commit `25c0d37` is docs/scripts/hello host only. No Nintendo data.

No AOT, no gameplay, no Vita launch claim.

## GX command stream (graphics milestone M0/M1 groundwork)

The FIFO parser was rewritten as a byte-at-a-time state machine. The old one
buffered whole commands in a 4 KB array; a single draw in this game reaches
933 KB of vertex payload, so 10,898 draws per boot overran the buffer and
desynced the stream.

Measured over 60M blocks (`GV_BTN=0x0800 GV_IR=512,384`), before -> after:

| | buffered parser | streaming parser |
|---|---|---|
| unknown bytes | 2178 | 206 |
| draws | 113,074 | 271,783 |
| vertices | 833,196 | 2,187,570 |
| display lists | 103,011 skipped | 101,707 executed (8.78 MB) |

Display lists are now followed through a guest-memory callback
(`gx_fifo_set_memory`), which is where most of this game's geometry lives.

Residual: 206 unknown bytes, a single self-recovering episode at stream offset
4,246,780. Cause is a vertex descriptor the guest never re-wrote: the SDK
caches CP state and only emits changed registers, and our copy diverges by one
descriptor across a display-list boundary. Related and unmodelled: SPR 921
(WPAR) retargets the write-gather pipe, which is how `GXBeginDisplayList`
builds a list in memory rather than in the FIFO; we treat every write-gather
store as a command-stream byte.

## Host software rasterizer (graphics milestones M0-M2)

`tests/host/gx_soft.c` is a dependency-free software rasterizer wired to the
FIFO through the `GxSink` callbacks in `runtime/gx_render.h`. It writes PNGs of
every EFB-to-XFB copy, so a render regression is visible in a file rather than
only on hardware. Enable with `GV_GFX_DUMP=<dir>`; `GV_GFX_EVERY=<n>` thins the
output, `GV_GFX_FRAMES=<n>` caps it, `GV_GFX_NOTEX=1` disables texturing and
`GV_GFX_SHADE=prim` hashes the primitive index into a colour (how the layout
was read before textures worked).

What the run at 60M blocks renders: the Wii health-and-safety screen and the
"Connect the Wii Remote to Player 1" prompt, both pixel-accurate -- full-colour
illustrations, anti-aliased text, the Nintendo logo. The game has loaded
`FileSelect.arc` by this point, so what stands between this and the main menu
is the Bluetooth/Wii Remote path, not graphics.

Five things had to be right for that image, in the order they were found:

1. **Indexed XF loads applied.** Opcodes 0x20/0x28/0x30/0x38 were counted but
   discarded. They load matrix and light memory out of the CP arrays; without
   them XF matrix memory never receives most of its contents. 4,219 loads per
   boot, none unmapped.
2. **Near-plane clipping.** GX clip space is `-w <= z <= 0`, so the near plane
   is `z + w = 0`, not `w > 0`. Clipping on `w` alone admits points between the
   eye and the near plane, which divide by a near-zero `w` and project to
   infinity -- they smeared across the frame. 795,781 of 1,440,082 triangles
   per boot are fully behind the near plane (the title-screen skybox surrounds
   the camera), and clipping them is what stopped the smearing.
3. **ZMODE, blending, colour update and scissor** (BP 0x40, 0x41, 0x20/0x21).
   Without them the last primitive drawn painted over everything; a frame was a
   single flat colour. BP 0x41 bit 3 also identifies SMG's depth-only prepass,
   which is 32,877 perspective triangles a boot that correctly write no colour.
4. **Texture decode.** I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR, de-tiled out of
   guest memory through the same callback display lists use, cached by
   (address, format, size). 21 textures, 0 unmapped, 0 unsupported. No TLUT
   loads (BP 0x64/0x65) occur in this phase, so paletted formats are untested.
5. **Alpha test before the depth write** (BP 0xF3). UI text is a cutout; with
   the test after the z-write, glyph bounding boxes occluded what was behind
   them. 68M pixels a boot are killed by it.

Texcoords interpolate in 1/w. TEV is not translated yet: the shader is
`texture * raster colour`, which is what stage 0 usually does and is enough for
these 2D passes.

Open: 332,873 colour-writing perspective triangles a boot land outside the
frustum and none land inside, while the depth prepass over the same geometry
rasterizes normally. The transform is confirmed correct for the ortho passes,
which share it, so this is a state or pass question rather than matrix maths.
It does not affect the screens above, which are 2D.

### Superseded (2026-09-15)

The hand-written GX path above is retired in favour of ModernGekko's Dolphin
VideoCommon — see [docs/GRAPHICS-BACKEND.md](docs/GRAPHICS-BACKEND.md) for the
decision and its evidence. `tests/host/gx_soft.c` keeps its value as a debug
oracle to diff Vita frames against; it is no longer the renderer.

Measured the same day: `scripts/probe-vita-gx.sh` builds all 11 Dolphin
VideoCommon sources in `moderngekko_dolphin_video` plus all 7 ModernGekko
`src/gpu/` sources for `arm-vita-eabi` — **18/18 objects, gcc 15.2.0**. One
source change was needed in the whole stack: `DriverDetails.cpp` had an
`#ifdef` chain over host OS with no `#else`, carried as
`patches/moderngekko-driverdetails-vita-os.patch`.

The open perspective-triangle question above is now a question for the Dolphin
pipeline, not for `gx_soft.c`, and is not worth chasing in the oracle.

### Shader target (2026-09-16)

Captured 32 distinct draw states from a real boot (`GV_GFX_STATEDUMP`), replayed
them through Dolphin's generators with the Vita's capability flags
(`scripts/dump-dolphin-shaders.sh`), and measured the gap to Cg.

| | |
|---|---|
| states captured | 32 (26 distinct pixel UIDs, up to 3 TEV stages / 3 texgens) |
| generated shader size | 6.6–9.1 KB, 64 shaders |
| converted to Cg with no residual GLSL | **64/64** (`scripts/glsl_to_cg.py`, ~120 lines) |
| validate as GLSL ES 3.00 | **64/64** under `glslangValidator` |
| `texture()` call sites per shader | **1** (all sampling funnels through `sampleTexture`) |
| `sampler2DArray` lines per shader | **2**, and `layer` is 0 in every state (stereo off) |
| **compile on-device via `SceShaccCg`** | **64/64 ok, 0 failed, 0 warnings, 0 errors** |
| GXP produced | 162,276 B total; largest 7,584 B, mean 2,535 B |

**The Cg question is settled: Dolphin's shader output runs through the Vita's
own compiler.** Integer types are first-class on GXM
(`SCE_GXM_PARAMETER_TYPE_S32`/`_U32`) and the integer ALU that TEV needs
compiles without complaint.

Getting there needed five mechanical fixes in `scripts/glsl_to_cg.py`, all
driven by on-device diagnostics rather than guesswork:

- **Cg has no unsigned integer type at all.** `uint`/`uint4` are not keywords,
  and an unknown type before an identifier parses as `uniform <block> {` -- one
  missing keyword produced 14 consecutive bogus block errors.
- Dolphin writes arrays type-side (`DolphinLightData[8] lights;`) -- legal
  GLSL, not legal Cg.
- `mat3`/`mat3x4` need Cg spellings, and `v * M` becomes `mul(M, v)`.
- Accessor `#define`s live *inside* the uniform block and must survive
  flattening unprefixed.
- No integer `clamp` overload exists; supplying one matters because TEV clamps
  values whose signed wrap-around is the hardware behaviour.

Two infrastructure lessons came out of it. **`-Wl,-q` is mandatory** for every
VitaSDK binary here -- `vita-elf-create` accepts an ELF without relocations
silently and the result jumps into padding before `main` (diagnosed from a core
dump). And **ShaccCg cannot compile unboundedly in one process**: it wedges
after 5-9 shaders no matter what they are. A segregated free-list allocator
kept its callback allocations flat, but something it allocates internally still
grows, so the self-test compiles a bounded batch per launch and resumes -- the
same shape the real on-device cache warmer will need.

See [docs/GRAPHICS-BACKEND.md](docs/GRAPHICS-BACKEND.md).
