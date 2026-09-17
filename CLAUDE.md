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
- **vitacompanion's `launch` kills the app after ~14 seconds.** Started from
  the LiveArea bubble the same binary runs as long as it likes; started with
  `launch` it is stopped at ~13.9 s, reliably, whatever it does. This was
  mistaken for a device-wide application cap and cost most of two days, because
  the harness was the one component never suspected -- it was the instrument
  doing the measuring, and a fifty-line control app built to exonerate
  GalaxyVita died the same way, which looked like proof and was the opposite.
  Any hardware run shorter than ~14 s is suspect by default. Before concluding
  anything from a device result, check the app was not simply stopped.
- **`press` and `release` do not work.** vitacompanion answers "Input pressed."
  and the input reaches neither SceShell nor the application (verified: an app
  read `buttons=0x00000000` with cross held across its whole life). The
  LiveArea cannot be driven from the host, and since `vita_pad` reads through
  the same `sceCtrlPeekBufferPositive`, synthetic input can never drive the
  game either.
- **The write-gather pipe is not the GP.** Writes to `0xCC008000` land at the
  PI FIFO write pointer, and `PI_FIFO_BASE` says where that is.
  `GXBeginDisplayList` points it at a buffer in memory. Feeding every WGP write
  to the command processor recorded no display lists and executed their
  contents as commands; the boot then froze on a vertex-stride desync 4.5 MB
  into the stream, which read convincingly like a decoder bug and was not one.
  When a GX parse goes wrong, check what the stream should have contained
  before suspecting the decode -- an independent walker over the capture is
  cheap and settles it either way.
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
- The Vita does not need the whole 3.2 GB extraction. The boot touches 25 MB of
  it; `GV_DISC_CENSUS` records exactly which files, and
  `scripts/stage-media-vita.sh` sends only those.
