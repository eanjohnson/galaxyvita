# GalaxyVita progress

Living log. Newest entry first. Record evidence, not hopes.

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
