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
  shader cache warmer must batch-and-resume across launches.
