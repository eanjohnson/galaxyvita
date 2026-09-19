# GalaxyVita

Port Super Mario Galaxy to the PS Vita by static recompilation (DolRecomp:
PowerPC/Broadway -> C -> ARMv7).

## Standing permissions

**Committing is pre-authorised. Commit whenever you want, without asking.**
Granted by the user on 2026-09-16, explicitly so that a context compaction
cannot make me forget it. Commit early and often: `runtime/`, `tests/host/`
and `vita/gfx/` spent a long time untracked, and an `rm -f` on untracked
`runtime/gx_fifo.c` + `gx_fifo.h` destroyed them unrecoverably. Untracked work
is the standing hazard here; committing is the mitigation.

**Pushing is not covered — ask first.** Same for anything else outward-facing.

## Hard constraints

- **No copyrighted game bytes in git or in the published VPK.** The user's ROM
  is their own legal backup. Ship scripting that takes a user-supplied ROM and
  produces the VPK. The Wii common key must not be embedded. `.gitignore` and
  `scripts/check-repository.sh` reject WBFS/DOL/VPK -- keep it that way.
- **`libshacccg.suprx` is not distributable and must not ship in the VPK.** The
  user extracts it from their own console's firmware.
- **All Vita iteration is automated over vitacompanion.** The user is not going
  to babysit the console. Never leave a step that needs someone at the device.
  Hold `flock -n` on the device: two runners at once wedge it and cost a power
  cycle.

## Attribution

End commit messages with:

```
Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01M7SFsAsZLFosK55HNuG34y
```

## Traps that have already cost time

- `pkill -f <pattern>` matches **my own shell's command line**. Anchor it, and
  never put the literal binary path in the same command as the `pkill`.
- `-Wl,-q` is mandatory for hand-linked VitaSDK binaries. `vita-elf-create`
  accepts an ELF with no relocations silently and the result jumps into padding.
- ShaccCg wedges after 5-9 compiles in one process regardless of content. The
  shader cache warmer must batch-and-resume across launches. It also needs an
  **unsafe** fself: as a safe self it loads and then fails every compile.
- `vita-elf-create` appends ~1.8 KB of SCE data at the end of the read-only
  segment and aborts when that segment ends within 1.8 KB of a page boundary.
  Where it ends is arbitrary, so ~half of all builds fail for a reason
  unrelated to the change. `scripts/build-vita.sh` pads and retries; do not
  "fix" a failing build by reverting the change that happened to trip it.
- **The Vita links prebuilt AOT objects that nothing checks.** CMake globs
  `generated/build/aot-vita`; there is no dependency edge from an object back
  to the chunk it came from, and `measure-aot-size.sh` skips the Vita half
  entirely when VITASDK is missing, on stderr and nowhere else. One stale
  object made an HLE hook unreachable on the console while every host run had
  it, and cost two days of chasing a "host/Vita divergence" that was really
  last week's code. `scripts/check-aot-fresh.sh` now fails the configure; if a
  device result disagrees with a host result, suspect this first.
- **A census that loses a file wedges the console and only the console.** The
  boot is staged from `generated/census.txt`, and the census is written by a
  host run that can see every file, so a lost entry is a file only the device
  misses -- and the symptom lands thousands of blocks away, as an error path,
  not as a missing-file error. One uninitialised `first_seen` cost days that
  way. `scripts/check-staging.sh` boots the staged subset against the full
  extraction and requires them to be indistinguishable; run it after
  regenerating a census, before staging.
- **A run launched over vitacompanion dies at ~14 s unless something keeps the
  console awake.** For two days this read as "`launch` kills the app after
  ~13.9 s, reliably, whatever it does", and the harness was the one component
  never suspected -- it was the instrument doing the measuring, and a
  fifty-line control app built to exonerate GalaxyVita died the same way, which
  looked like proof and was the opposite. The cap is the OS suspending an app
  it thinks nobody is using. Holding `nosleep on` over vitacompanion and
  calling `sceKernelPowerTick` from the display thread defeats it: runs started
  with plain `launch` now reach **1162 s** unattended, which is how the file
  select was ever measured. So `launch` is fine and a LiveArea tap is not
  needed. Any hardware run that stops near 14 s means the wakeup lapsed, not
  that the app failed -- check that before concluding anything from it.
- **`press` and `release` do not work.** vitacompanion answers "Input pressed."
  and the input reaches neither SceShell nor the application (verified: an app
  read `buttons=0x00000000` with cross held across its whole life). The
  LiveArea cannot be driven from the host, and since `vita_pad` reads through
  the same `sceCtrlPeekBufferPositive`, no synthetic *button* ever reaches the
  game that way. The game is driven instead by data it reads for itself:
  `scripts/send-input-vita.sh` writes a script of (mask, frames) steps to
  `input.txt`, the watchdog thread polls it every two seconds, and
  `scripts/snap-vita.sh` pulls the resulting frame back as a PNG. The
  LiveArea still cannot be driven, so the app is still started with `launch`.
- **The write-gather pipe is not the GP.** Writes to `0xCC008000` land at the
  PI FIFO write pointer, and `PI_FIFO_BASE` says where that is.
  `GXBeginDisplayList` points it at a buffer in memory. Feeding every WGP write
  to the command processor recorded no display lists and executed their
  contents as commands; the boot then froze on a vertex-stride desync 4.5 MB
  into the stream, which read convincingly like a decoder bug and was not one.
  When a GX parse goes wrong, check what the stream should have contained
  before suspecting the decode -- an independent walker over the capture is
  cheap and settles it either way.
- **`destroy` kills the FTP server.** vitacompanion's `destroy` quits *every*
  application, and VitaShell -- which is what serves FTP on 1337 -- is an
  application. So the convenient hammer takes the file transport down with it:
  1337 stops answering while vitacompanion keeps replying normally on 1338,
  which reads as a wedged console and is a self-inflicted one. It cost two
  reboots before the connection was made, and the second time the scripts'
  own `destroy` did it between a successful FTP probe and the very next
  command. `kill TITLEID` is less reliable, which is why the scripts used to
  reach for `destroy`; they now try the targeted kill, check whether FTP still
  answers, and escalate only when it did not take. Restarting FTP afterwards
  needs someone to press SELECT in VitaShell, so this is worth avoiding.
- **Do not abort an FTP transfer mid-stream, and a wedged FTP is not a power
  cycle.** `curl --max-time` killing a RETR part-way, or reading a frame dump
  the app is still writing, leaves ftpvita unable to serve anything: port 1337
  stops answering while vitacompanion on 1338 keeps replying normally, which
  reads exactly like a hung application and is not one. `destroy` does not
  clear it. vitacompanion's own `reboot` does, and the console comes back in
  about a minute with nothing lost -- so this never needs anyone at the device.
  Let transfers finish; if a dump may still be being written, wait for the size
  to stop changing rather than capping the transfer.
- **The frame dump is card I/O in the render thread.** Each frame is 2 MB and
  the write is synchronous, so a small `dump.txt` stride queues tens of
  megabytes in a couple of seconds of guest time and starves everything else
  sharing the card, ftpvita included. Stride 8 was enough to do it. Keep the
  product of count and frequency low; the useful frames are rarely so close
  together that it matters.
- **The timebase advances at dispatch boundaries, so chunk size changes what
  the emulator does.** Shrinking chunks from 1024 to 128 instructions makes the
  timing quantum 8x finer, and guest MMIO polling loops then spin a different
  number of times before the condition they wait on flips -- so FIFO writes, GX
  commands and MMIO counts all shift by ~0.2% at a matched retrace count. That
  is not a miscompile: it is bounded, it does not accumulate over a 60x range
  of run lengths, and its direction varies by phase, where real divergence
  would grow and never heal. Host equivalence for a chunk-size change must
  therefore gate on retraces, disc reads and the final PC -- and on the other
  counters staying *bounded*, not identical. Gating on FIFO or GX fails every
  variant by construction, exactly as gating on `hits` would.
- **Before gating on a counter, ask whether the change under test can move it
  legitimately.** Three separate measurements here were taken against a ruler
  that moves with the thing being measured: a fixed block cap when the change
  removed dispatches, matched wall clock when the change altered speed, and
  host equivalence on FIFO writes when the change altered the timing quantum.
  Each one produced a confident number that was wrong, and each was caught only
  because something else refused to reconcile.
- **Run-to-run noise on the device is about 3%, so nothing smaller is a
  result.** Two builds that were behaviourally identical -- bit-for-bit host
  equivalent, differing only by profiler byte stores -- measured 1.2% to 2.8%
  apart on hardware, in the direction that added work. Every A/B here is a
  single run of each side, so a 1-3% difference is not distinguishable from
  taking the same build twice. Quote a sub-3% delta as "no measurable effect",
  and if a change genuinely needs to be resolved at that size, run each side
  more than once before believing either. This floor was established by
  accident after five A/Bs had already been run and published.
- **A matched retrace is not a matched workload; check the draw count at the
  window boundary.** Two passes of the same eboot with the same `input.txt`
  came out 2.2x apart at retraces 800-1100, which no graphics change could do.
  The reason is in the gxm report: at frame ~1100 one run had issued 530,006
  draws and the other 1,109,774, and at frame 1400 it was 2.86 M against 3.34 M.
  The input replay had landed somewhere slightly different and the game was
  showing less. VI/s is the right yardstick for *speed* and stays so -- but it
  only compares two runs that are drawing the same thing, and this input script
  does not guarantee that from launch to launch. Before believing any device
  A/B, print `draws=` from the watchdog tick nearest each window edge on both
  sides; within a percent or two is the same workload, and a factor of two is
  not a result. The good pass here was recognised by matching a third run of
  the same binary to within 1% on exactly that counter.

- **Diff the assembly before claiming a micro-optimisation; the compiler has
  usually done it already.** Two changes here were written up as removing work
  from the hottest store in the game -- a duplicated address predicate run
  twice per MMIO write, and an eight-byte scratch zeroed before four of it was
  overwritten -- and both compile to the same code they replaced. GCC had
  common-subexpression-eliminated the predicate (both were static and pure) and
  elided the dead stores. The tell was an `eboot.bin` identical in size to the
  build before them; `arm-vita-eabi-gcc -S` on the one file, before and after,
  settles it in seconds and costs no device time. Neither change was wrong to
  make -- one predicate is the better way to write it -- but crediting either
  with a device result would have been inventing a cause for someone else's
  number. The rule is the project's own: do not assert what was not measured.

- **The emitter's obvious waste is mostly the compiler's to remove, and it
  already has.** Every emitted guest instruction begins `ctx->pc = 0x...`, one
  store each, 1.36 M of them across the tree -- and an analysis of which ones
  anything could actually observe said 51.5% were removable, which read as a
  10% cut to `.text` in the phase that owns 47% of the window. It is already
  done: one chunk carries 1,023 pc stores in its C and 482 in its assembly.
  GCC sinks the dead ones for exactly the reason the analysis found them,
  and the two numbers agree to within a point. What is *not* removable by the
  compiler is aliasing it cannot see through: a guest store goes through a
  `u8*`, character pointers may alias anything even under strict aliasing --
  which is why turning that flag on measured flat and always would have -- so
  every `mem_write32` forces a reload of every cached `ctx->gpr[]`. A probe of
  the emitted shape shows three reloads across two stores, and holding the
  registers in locals removes all three. The rule: before writing an emitter
  pass, compile one chunk with `-S` and count what survives. The passes worth
  writing are the ones about aliasing and invariants, not the ones about dead
  code.

  That last sentence was then measured and is the opposite of true, which is
  the more useful half of this entry. Handing GCC the aliasing guarantee it was
  missing -- guest RAM and CPUState are disjoint objects, stated with `restrict`
  on inlined accessors taking both -- removes every `cpu->ram`, `cpu->ram_size`
  and `g_mem_write_journal` reload, exactly as predicted: 6,256 of 30,523 loads
  across twelve chunks, a fifth of all loads gone. The emitted code gets 4.3%
  **bigger**. Those loads were relieving register pressure, and without them
  GCC spills instead. A saving in loads is not a saving in instructions, let
  alone in time, and an invariant the compiler reloads is not automatically an
  invariant worth hoisting.

  Sample the chunks at random when pricing this kind of change. On the four
  *largest* chunks the same hoist measured -1.4%, -2.5%, -1.9% and +0.5%, which
  reads like a small win worth bundling. The largest chunks are the ones already
  under the most register pressure and they are not representative; twelve
  chunks picked at random say +4.3% and settle it the other way.

- **Census the emitted assembly by CPUState field before theorising about
  which C construct is expensive.** The plan's static emitter census counted C
  text and priced two targets -- guest-register caching at 5.80% of emitted
  code, the memory fast-path invariants at 4.78%. Both were measured before
  being built and between them they were worth 0.85%. What the plan does not
  mention anywhere, because nobody thought to look at it, is `downcount`: an
  `s64` slice counter read-modify-written at the head of every basic block,
  holding two registers on a 32-bit machine for a value that never leaves
  [-1280, 10000]. It was the single hottest field by a factor of three, and
  narrowing it to `s32` removed 7.4% to 10.8% of emitted instructions -- most
  of it not the arithmetic but the freed register letting GCC keep guest
  registers live, which is the win the 5.80% target was aiming at and could not
  reach directly. `scripts/census-emitted.py` buckets every load in a compiled
  chunk by the field it touches and takes seconds to run. Run it first. A
  census of the source ranks what you thought to name; a census of the
  assembly ranks what is there.

- **A change worth a percent cannot be measured on its own here, so bundle
  them.** The noise floor is 3%, and by this point every remaining CPU-side
  saving is one to two: the PE drain, the prototype vertex, the dispatch PC
  filter, the census interleave. Run each as its own A/B and the honest report
  is four times "no measurable effect", which is indistinguishable from four
  times "no effect" and would have thrown all four away. Gate them separately
  -- host equivalence is exact and does not care how small the change is -- and
  then measure the sum in one device run, saying in the write-up which changes
  the number covers. The corollary is that a bundle only works if every part of
  it passed its own gate first: a bundle is a measurement shortcut, never a
  correctness one.
- **A new phase needs adding to the parser's alternation, not just its
  display list.** `window-watchdog.py` names the phases twice: `PH` decides
  what is printed and in what order, and a regex alternation decides what is
  read off the `prof` line. `gxdl` went into `PH` and not into the
  alternation, so every window printed `gxdl= 0.0s` -- for a phase whose
  cumulative counter in the same log was climbing by 38 samples a tick. A
  phase that reads exactly zero in every window while the run as a whole has
  thousands of samples in it is the instrument, not the result; the two lists
  disagreeing is the first thing to check. Worse, zero is a plausible answer
  here -- it reads as "this phase costs nothing", which is the conclusion the
  measurement existed to test.

- **`eboot.bin` is compressed, so its size is not code size.** The -marm
  build's objects carry 19% more `.text` than the Thumb-2 build and its ELF is
  13.9% bigger -- and its `eboot.bin` is 3.2 MB *smaller*, because ARM's
  fixed-width encoding compresses 3.60x where dense Thumb-2 manages 2.85x. Read
  across instruction sets, eboot size reports the opposite of the truth. It had
  already been quoted in this project as a code-size proxy for the cross-chunk
  change; that comparison happened to be Thumb against Thumb so the ratio was
  roughly fair, but it was luck. Compare `arm-vita-eabi-size` on the objects,
  or the ELF -- and for an I-cache argument only the uncompressed figure means
  anything, since that is what the cache holds.
- **`GV_AOT_TARGETS=vita` halves an AOT experiment and removes its gate.** The
  host objects are 42 of the 80 minutes and 250 MB, and a flag or arch sweep
  only ever asks "is this faster on the console" -- so skipping them is right
  for `-marm` or an `-O` sweep, which cannot change behaviour. It is wrong for
  an emitter change, because with no host objects there is no host equivalence
  run, and the only remaining check is the device boot reaching the same
  retraces. That is much weaker evidence, and the difference is easy to forget
  three experiments later. Say which gate a result passed when quoting it.
- **A frame dump at the default stride is all opening flash.** `GV_GFX_EVERY`
  defaults to 1, so `GV_GFX_FRAMES=40` dumps frames 0 to 39 -- every one of
  them inside the game's white flash. Forty such frames compared byte-for-byte
  across a graphics change match perfectly and prove nothing, and that reads
  exactly like a passing pixel gate. Stride the dump out (`GV_GFX_EVERY=30`
  reaches frame 1140 in 39 shots) and count distinct colours per frame before
  believing the comparison: real content here is 186-213, the flash is 1.
- **"The screen is white" is not evidence until the host frame is checked.**
  The host renders frames 0 to 99 of this boot as solid white -- the game's
  own opening flash -- and the strap screen only appears around frame 100. A
  device dump window that ends before then shows white because white is
  correct, and it has now twice been read as a rendering bug and chased into
  the backend. Before diagnosing a frame, open the same frame number from a
  host run; `fx_ref` in the job scratch is one. The same goes for the visible
  symptom the user reports: the console showing a white screen for the first
  several seconds is the game, not the port.
- **GXM's default uniform buffer is not cleared.** What
  `sceGxmReserveFragmentDefaultUniformBuffer` hands out holds whatever was
  there before, so a draw that does not upload a uniform reads the previous
  scene's value, not zero. A fragment shader that can `discard` and a draw
  that skips the upload is a frame that vanishes for no visible reason -- the
  screen clear is itself such a draw, and it was being discarded against
  leftover alpha-test state, leaving the display showing the buffer's previous
  contents. Upload before the first use, and where there is a choice, pick the
  encoding whose all-zero value is the harmless one.

- **A device script called from inside another device script always fails.**
  Every runner takes `flock -n` on `galaxyvita-device.lock`, and so does
  `send-input-vita.sh` -- and a child that reopens the lock file gets its own
  file description, so the parent's lock excludes it. It exits 1 without
  writing anything, and a harness that redirects it to `/dev/null 2>&1` -- as
  every ad-hoc runner in this project has -- turns that into silence. The runs
  then use whatever `input.txt` was left on the card by the last script that
  ran *outside* a lock, which is comparable between runs and is not what the
  runner asked for. Send the input before taking the lock, or check the exit
  status; do not discover this by wondering why a new input script had no
  effect.
- **The host and the console do not run the same workload, so a host profile
  cannot aim graphics work.** There is no input replay on the host, so it never
  leaves the strap screen: at retrace 1400 a host run has 7,491 draws and 29,964
  vertices where the console, replaying `input.txt` to the file select, has 3.02M
  draws and 21.2M vertices by the same frame count. A 1 kHz sample of the host at
  that point puts the entire ModernGekko GPU stack at 1.8% and does not rank
  `GxVertexLoader::Decode` at all, while the device profiler says graphics is 49%
  of the 1100-1400 window. Both are right about the run they measured. Use the
  host for equivalence gates, which only need the two sides to do the same thing;
  aim performance work with the device phase counters.
- **The host profile does not describe the console, and a cumulative
  percentage does not describe a phase.** On the host, recompiled guest code is
  75% of time; on the device it is a quarter, and the largest single phase at
  the slow end of a run is graphics. The device profiler therefore reports raw
  cumulative counts, not percentages, so that differencing two watchdog ticks
  gives an exact window -- cumulative percentages went on reporting the
  disc-load burst for minutes after the card went quiet, which reads as a
  steady state and is a decaying average. Quote a phase share only with the
  window and the phase it was taken in; the gx share moved from 8% to 21%
  across one 630 s run. Note also that the guest-PC hash overflows on long runs
  (25% of samples dropped by 667k), so the PC ranking needs more slots before a
  long-run ranking means anything; the phase histogram is a plain array and is
  fine.
- **A truncated counter reads as a small counter.** The watchdog wrote the gxm
  report into a 352-byte buffer and `snprintf` cut the line at `dblend=`, so
  every field after it -- including four counters added specifically to measure
  a change -- was absent from every tick but the first dozen, with nothing to
  mark that anything had been dropped. Read off the last line of a long run,
  those counters looked like zero, which is precisely the answer that says the
  change under test did nothing; the last tick that still fit said 27 of 40. If
  a counter you just added reads zero, check that the line it lives on is
  complete before believing it.
- **A log line asserting something nobody measured is worse than no log line.**
  `main.c` printed `clock: PSVshellPlus user-controlled` for weeks. Nothing set
  a clock, `ScePower_stub` was not even linked, and the console was running at
  333 MHz ARM with the GPU at 0/111 -- so every performance number in the
  project's history was taken at 75% CPU clock. The line read as "handled" and
  stopped anyone looking. Log what was asked for *and* what was granted.
- **A census only covers as far as its run got.** `scripts/check-staging.sh`
  compares the staged subset against the full extraction and will say "ok"
  while the census is still missing every file the game has not asked for
  yet -- because at that block count neither tree asks for them either. The
  census that boots to the strap screen names 275 files; the one driven to the
  file-select screen names 276, and the extra one is a 7 MB AST music stream.
  Without it the device ran to frame 1057 and stopped, with the disc doing
  ~330 reads a second that all missed while megabytes transferred stayed flat.
  That reads like a scan for something that is not there and is nothing of the
  kind: it is ordinary streaming playback walking forward through a file the
  card does not have, ~210 KB per watchdog tick. Map the offset back through
  the FST before theorising -- `w1 << 2` is the disc offset, the entry that
  covers it is the answer, and it took one short script to turn "the game is
  scanning the disc" into a filename. Re-take the census from a run that goes
  at least as far as the device will, and re-check staging at that same depth.
- **The host harness resolves its media paths against the current directory,
  and missing media is a warning on stderr rather than an error.**
  `aot_chassis.c` defaults `game` to `generated/extracted/game` and `nand` to
  `generated/nand`, both relative, and `chassis_attach_media` failing only
  prints `warning: media attach incomplete`. Run it from a tree without the
  NAND and the boot still reaches the same retrace with the same FIFO writes,
  GX commands and disc reads -- but 8% fewer dispatches and 43% fewer MMIO
  reads, because the guest takes different paths through IOS. That reads
  exactly like a real behavioural difference in whatever change is under test,
  and it cost an hour of hunting a phantom bug in an idle-skip patch that
  turned out to be provably inert. If two trees that should agree do not,
  check `warning:` on stderr before reading anything into the counters -- and
  when comparing trees, prove the unpatched copy reproduces the original first.
- **Whether a chunk function fits the instruction cache is irrelevant; total
  footprint is not.** The sweep ran both ways. 128-instruction chunks put every
  function at 8.4 KiB, a quarter of the 32 KiB L1I, and `guest` time did not
  improve while the console got 6-7% slower overall, because eight times more
  chunk boundaries means eight times more fall-through dispatches. 4096 put them
  at 238 KiB, seven times the cache, and bought 1.111x in the dispatch-bound
  early window while buying nothing where the game is actually slow. `-marm`
  carried 19% more `.text` and came back flat. So do not argue for a change on
  the grounds that it makes hot functions cache-resident -- three experiments
  say that is not the variable. What did show up, only at 4096, is that a 77 MiB
  `.text` slows down code whose machine code did not change at all: vertex load
  and submit are C++ compiled identically in both builds and both got slower.
  Total footprint presses on everything sharing the memory system. 1024 stays;
  the sweep is finished and does not need repeating.
- The Vita does not need the whole 3.2 GB extraction. The boot touches 25 MB of
  it; `GV_DISC_CENSUS` records exactly which files, and
  `scripts/stage-media-vita.sh` sends only those.
