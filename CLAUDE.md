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
- The Vita does not need the whole 3.2 GB extraction. The boot touches 25 MB of
  it; `GV_DISC_CENSUS` records exactly which files, and
  `scripts/stage-media-vita.sh` sends only those.
