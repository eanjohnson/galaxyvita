# Architecture (working)

See [PLAN.md](PLAN.md) for the full design. This file tracks the *implemented*
map. [PROGRESS.md](../PROGRESS.md) carries the dated evidence for each claim
here. [GRAPHICS-BACKEND.md](GRAPHICS-BACKEND.md) records why the renderer is
ModernGekko's Dolphin VideoCommon rather than a hand-written GX translator.

## Process split

| Where | What |
|---|---|
| Host PC | Disc verify, extract, DolRecomp C generation, `arm-vita-eabi` compile, VPK pack, vitacompanion deploy |
| Vita | eboot (host + runtime + AOT), game tree on `ux0:data/galaxyvita/game/` |

No Nintendo bytes live in this repository or in the VPK. The user supplies their
own disc; `scripts/extract-disc.sh` turns it into the game tree,
`scripts/make_nand.py` synthesises the NAND tree, and `.gitignore` plus
`scripts/check-repository.sh` reject WBFS, DOL and VPK files outright.

## Threads (target)

Vita affinity masks are **not** `1 << core`:

| Core | Affinity | Name | Work |
|---|---|---|---|
| CPU0 | `0x10000` | cpu-aot | AOT dispatch, CoreTiming |
| CPU1 | `0x20000` | gpu-fifo | GX FIFO, GXM/vitaGL (all GPU API) |
| CPU2 | `0x40000` | audio-io | DSP HLE, sceAudioOut, ux0 reads |
| CPU3 | `0x80000` | assist | optional; needs CoreUnlocker |

## The chassis

`runtime/chassis.c` is the machine: CPU, Flipper/Hollywood MMIO, GX FIFO, the
IOS IPC HLE, the disc and NAND backends. Both hosts are thin —
`tests/host/aot_chassis.c` for headless PC runs, `vita/host/main.c` for the
device — and everything they share lives in `runtime/`, which uses no `getenv`
and no host headers.

`scripts/run-host-chassis.sh <dol> <max-blocks>` is the iteration loop. Its
knobs, all read by the host harness and passed into the chassis through setters:

| Variable | Effect |
|---|---|
| `GV_GAME_ROOT`, `GV_NAND_ROOT` | where the extracted game tree and generated NAND live |
| `GV_TRACE=lo:hi[:max]` | log every AOT block entry inside an address window, with `lr` |
| `GV_PC_CENSUS=<file>`, `GV_CENSUS_FROM=<blocks>` | dump the block-entry PC census, optionally restarted partway |
| `GV_VEC_DUMP=<hexaddr>` | print 48 guest words from that address at the end of a run |
| `GV_FIFO_DUMP=<file>` | write the captured GX FIFO bytes |
| `GV_BT_LOG=<n>` | log the next `n` HCI commands and ACL packets in both directions |
| `GV_BTN`, `GV_IR`, `GV_BTN_PERIOD`, `GV_BTN_HOLD` | the scripted player (see *Input*) |

**Tracing gotcha.** DolRecomp compiles a `bl` whose target lands in the same
1024-instruction chunk as a plain `goto`, so intra-chunk calls are invisible to
`GV_TRACE`; only function entries reached from another chunk show up. When a
trace comes back empty, check whether caller and callee share a chunk
(`generated/aot/generated/chunks/chunk_*_<hex>.c`) before concluding the code
never ran. `scripts/gdis.py <HEXADDR> [count]` disassembles guest code and
`scripts/gxref.py <dol> <HEXADDR>` finds references to an address.

## HLE hook table

`runtime/hle_hooks.h` is the single list of guest entry points the chassis
answers natively. Two consumers read it: `runtime/chassis.c` builds its dispatch
table from it, and `scripts/patch-aot-hle.py` rewrites the generated chunks so
calls to those addresses always escape to the dispatcher — otherwise the
intra-chunk `goto` above would run straight past the hook. The addresses are
RMGE01-specific by design.

What is hooked, and why:

- `WUDIsLinkedWBC` — the balance board query; there is no board, and the SDK's
  own answer needs BTA state we do not model.
- `__AI_SRC_INIT` — the audio sample-rate converter init spins on AI hardware
  that does not exist yet.
- The Home Button Menu's RSO entry points (`HBMCreateRSO`, `HBMInitRSO`,
  `HBMCalcRSO`, `HBMDrawRSO`, `HBMGetSelectBtnNumRSO`, `HBMSetAdjustFlagRSO`,
  `HBMStartBlackOutRSO`, and `loadModule`). The menu is a relocatable module
  loaded off the disc and linked at runtime; running it would mean implementing
  the RSO loader for a UI that is not on the port's path.

## Graphics

GPU thread (CPU1) owns present via vitaGL. `libshacccg.suprx` is the ShaRKBR33D
extract; vitaGL wants it at `ur0:data/external/`.

The guest now writes the FIFO: `runtime/gx_fifo.c` decodes the command stream at
`0xCC008000` (CP, XF, BP, indexed loads, display lists, draw commands and vertex
counts). A 60M-block run with a scripted A press produces ~23.8 MB of FIFO,
113k draw commands, 833k vertices and 103k display lists — real geometry, not
just clears. The decoder still reports some unknown opcodes (`gx unk`) once that
volume starts flowing; that is the next graphics thread to pull.

## IOS / Hollywood

`runtime/ios.c` is a Broadway-side IPC HLE (not Starlet LLE), following
RecompCore `WII_IPC` / `IOS.cpp`. Command buffers may be physical
(`0x13xxxxxx` → MEM2) or uncached (`0xDxxxxxxx`).

- **Deferred reply queue.** An IOS request that cannot be answered now (a parked
  USB read, most of all) is left pending and its reply is queued, not dropped.
  `Ios.replyq` drains as results arrive; `ios replyq head/tail/dropped` in the
  run report is the health check. Replying immediately with "nothing" is what
  originally hung the Bluetooth stack.
- **`/dev/di`.** Backed by `runtime/disc.c` over an extracted `wit` tree
  (`sys/boot.bin`, `sys/bi2.bin`, `sys/apploader.img`, `sys/main.dol`,
  `sys/fst.bin`, `files/`). Offsets are partition-relative bytes, i.e. what
  `DVDLowRead`'s command word carries after its shift. DIResult semantics
  matter: RMGE01's anti-modchip check demands a specific *drive error* from two
  probes, so `DVDLowRequestError` has to report `0x052100` for the unencrypted
  read of `0x460A0000` and `0x053100`/`0x052000` for report key `0xA4`, both
  with DIResult `2` (`DriveError`).
- **`/dev/es`.** `ES_DIGETTICKETVIEW` and `ES_GETCONSUMPTION` write real io
  vectors — a zeroed `TicketView` (0xD8 bytes, title `00010000-524D4745`, all
  content permissions set) and a zero consumption count. Returning success
  without writing them handed the SDK uninitialised stack and an "Expired"
  `OSPanic`.
- **`/dev/fs` (ISFS).** `runtime/nand.c` maps guest paths onto a host directory
  generated by `scripts/make_nand.py`: SYSCONF and `setting.txt` only, both
  synthesised here. FS result codes follow Dolphin's `ConvertResult`
  (`-(ResultCode + 100)`). WUD writes SYSCONF back, so the tree is read/write.
- **Apploader HLE + the FST.** A real apploader parks the FST at arena *hi*, but
  RMGE01's `OSInit` recomputes arena hi as the end of MEM1 and ignores
  `0x80000034`, so JKRHeap cleared it. The FST goes at arena **lo** instead, and
  `0x80000030` is pushed past it.
- **Interrupts.** Hollywood IY enable ACKs IOS with Y2, delayed 64 slices so
  `OSSleepThread` on the IPC queue is not a lost wakeup. PI IPC is
  level-triggered from Y/IY; `0xCD000030` bit `0x40000000` is the Broadway IPC
  flag. The `0x80000500` stub's default `rfi` (`0x804A1D3C`) is remapped to the
  installed EXTERNAL handler (`0x804A881C`). PE finish/token interrupts are
  raised from the FIFO decoder as the guest's own tokens go by.
- DOL BSS is zeroed **before** initialized data sections: the RMGE01 BSS box
  overlaps `.sdata`.

## Timing

The timebase is cycle-accurate rather than wall-clock: the chassis credits
executed cycles into `tb_frac` and advances the timebase one tick per 12 CPU
cycles, which is also what drives the decrementer and its interrupt. VI retrace
comes out of `flipper_advance()` on the same clock, so frame count and timebase
stay consistent with each other regardless of how fast the host runs. The Wii
Remote's 200 Hz report clock is a third divider (`CPU_CYCLES_PER_WIIMOTE_SAMPLE`)
off the same cycle count.

## DSP

No DSP code runs. `runtime/flipper.c` models the mailbox, the ARAM DMA engine and
the ucode boot handshake well enough that the SDK's task scheduler is satisfied:
boot-select words are collected until `DSP_BOOT_START_PC`, at which point the
model answers `DSP_INIT` and the ucode's greeting. After that SMG drives its
ucode with a word count followed by that many words, tagging each command in the
first word; the dispatcher matches replies by the tag's high half in submission
order, so the model answers every command exactly once and in order with
`DSP_SYNC` plus the tagged greeting. Audio output itself is still a null backend.

## Bluetooth and the Wii Remote

`runtime/bt.c` and `runtime/bt.h` model the Wii's Bluetooth dongle at the **HCI**
level, with one Wii Remote permanently synced to it. The alternative — HLE'ing
WPAD — would leave the game's own input smoothing, calibration and pointer maths
unrun, so instead the Vita's sticks and buttons become HID reports and every line
of the game's input code stays on the recompiled path. Only the corner of
Bluetooth the SDK actually uses is implemented: no inquiry, no SCO, no
encryption above L2CAP.

The SDK stack above it is WUD (the SDK's own Bluetooth host, driving
`/dev/usb/oh1/57e/305`), BTE/BTA (Broadcom's stack), BTA-HH and HID host, then
WPAD and KPAD. Four things about it are easy to get wrong and were:

1. **BTE reverses BD_ADDRs.** `STREAM_TO_BDADDR` flips byte order on parse, so
   BTE's internal addresses are big-endian while the HCI wire is little-endian.
   SYSCONF's `BT.DINF` — which is what the console pages at boot — must hold the
   address in BTE's order, not the wire's.
2. **The remote leads.** The remote pages the console, and the remote opens both
   HID L2CAP channels (PSM `0x11` control first, then `0x13` interrupt). The
   SDK's HID host only ever answers.
3. **One configuration request per channel.** L2CAP configures each direction
   separately, so the remote sends a request of its own as well as answering the
   console's — but exactly one. A second request puts the SDK's channel back into
   `CST_CONFIG` and clears both halves of its `config_done`; the SDK answers the
   repeat, so the inbound half returns, but it never repeats its own request, so
   the outbound half never does. The channel then sits in configuration for good
   and every `L2CA_DataWrite` on it is dropped — which looks exactly like a
   healthy link that simply never carries data.
4. **Pairing is checked twice.** A device is real to WUD only if it is in
   `BT.DINF` *and* the dongle confirms a stored link key through Return Link Keys
   (HCI event `0x15`). Miss the confirmation and WUD purges the record, then
   disconnects the next link-up as unpaired.

With those right the console does what it does to a real remote: reads the
accelerometer calibration out of EEPROM, lights player 1's LED, sets IR
sensitivity, and asks for report mode `0x33` continuously.

### Input

`chassis_set_input_fn()` installs a callback the chassis calls at the remote's
own 200 Hz, just before each report is built; `chassis_set_input()` is the
fire-and-forget alternative. `vita/host/main.c` maps SceCtrl onto it — right
stick to the IR pointer, left stick to the nunchuk, `L`/`△` to Z and C, `✕`/`□`
to A and B, `START`/`SELECT` to `+`/`−`. The headless harness drives a scripted
player from `GV_BTN` (a button mask in `BT_BTN_*` bit order), `GV_IR="x,y"`, and
`GV_BTN_PERIOD`/`GV_BTN_HOLD`, which hold the mask for part of every cycle so the
game sees presses *and* releases.

## CPU

DolRecomp's output is PowerPC/Broadway → C → ARMv7. Three corrections live on
the chassis side rather than in the generated code:

- **SPR field decode.** The 10-bit SPR field carries the number's halves the
  other way round: bits 11-15 are the low five, bits 16-20 the high five.
  Reading it straight turns `mtdbat2u` (540) into `mtgqr0` (912), which quietly
  repoints every paired-single store at a quantised format. HID4 (SPR 1011) has
  no home in the recompiler's access table either — `__OSPSInit` writes it while
  bringing paired singles up — so the chassis keeps it itself. That part belongs
  upstream in DolRecomp's `ppc_spr_access` table.
- **`MSR[RI]`.** An SPR the helpers do not know raises a program exception, and
  taking one costs MSR's `EE`, `IR`, `DR` and — the expensive bit — `RI`.
  Clearing `cpu->exception` afterwards hides the trap but not the damage: with
  `RI` gone the SDK's exception stub reads every later interrupt as
  unrecoverable and sends it to `__OSUnhandledException` instead of the handler
  table, killing the decrementer and with it OSAlarm. The fallback rolls back
  the whole machine state, not just the exception flag.
- **Dot-form CR0** in the chassis's fallback interpreter: `andi.`, `rlwinm.` and
  friends must set CR0 from the *result*. Leave CR0 stale and a later `bc` takes
  whichever arm it happens to name — which makes branch polarity in reversed SDK
  code look impossible.

## Memory

`param.sfo` ATTRIBUTE2 enlarged memory (+109 MiB) is mandatory. Guest RAM is
`sceKernelAllocMemBlock` USER_RW, not the newlib heap. AOT `.text` is ~52 MiB
(compressed SELF ~22 MiB); see [AOT-SIZE.md](AOT-SIZE.md).
