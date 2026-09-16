# GalaxyVita — Super Mario Galaxy on PlayStation Vita via AOT recompilation

## Goal

Build **GalaxyVita**: a Vita homebrew that runs Super Mario Galaxy by ahead-of-time recompiling the Wii PowerPC `main.dol` into native ARMv7 code, then executing it against a Dolphin-derived Wii compatibility runtime with a Vita-native host (GXM graphics, sceAudio, sceCtrl/touch, 3-core threading).

This is **not** a general Wii emulator frontend and **not** a from-scratch C++ port of Petari. It is a game-specific Vita integration of the same stack GalaxyPad uses on Apple:

```
User-owned RMGE01 Wii image
        │
        ├── identity + partition + main.dol verification (host)
        ▼
DolRecomp (Broadway / 64 MiB MEM2) → portable C
        ▼
vitaSDK (arm-vita-eabi) compiles C → ARMv7-A + NEON module
        ▼
ModernGekko + RecompCore (Dolphin-derived Wii services)
        ├── MEM1 / MEM2 / IOS / NAND / DSP HLE / GX
        ├── Vita GXM (later) / vitaGL (bring-up) renderer
        ├── 3-core thread map + optional 4th core
        ▼
GalaxyVita.vpk  (no Nintendo data inside the published tree)
```

The CPU translation is the solved part of this stack. The hard part is **fitting a Wii runtime onto 3 Cortex-A9 cores, ~365–474 MiB USER_RW, 112 MiB CDRAM, and a PowerVR SGX543MP4+ TBDR**, plus a **hands-off deploy/test loop on a real Vita**.

---

## What this is, legally

- The workspace currently contains only `Super Mario Galaxy [RMGE01].wbfs`. That file is the owner's legal backup. It must never be committed, uploaded, or packaged into a redistributable VPK that we publish.
- End-state user flow: **user supplies their own image → host scripts extract, recompile, compile, pack → local `.vpk`**. We distribute source, scripts, and a ROM-free host. Generated AOT objects exist only on the builder's machine.
- Licenses: ModernGekko / RecompCore / Dolphin / DolRecomp / GalaxyPad-adjacent code are GPL-3.0. GalaxyVita will be **GPL-3.0-or-later**. Petari is a read-only semantic map (RMGK01, not our DOL).
- Public binary/source release is a later rights gate. Private technical work is authorized now.

---

## Reference inputs (already researched)

### GalaxyPad (the Apple proof)

Repo: https://github.com/chrissotraidis/galaxypad

Proven facts we will copy as process, not as code:

- Exact USA `RMGE01` revision 0 is the supported input. `config/galaxypad-disc.json` pins `sys/main.dol` SHA-256 `2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09` (6,283,264 bytes). Petari's Korean map does **not** apply to this DOL.
- DolRecomp **C backend**, 1024-instruction chunks, indexed dispatch. LLVM is not required.
- No runtime PowerPC JIT on mobile. Interpreter fallback only as a measured, bounded bring-up path.
- HOME-menu RSO (`HomeButtonMenuWrapperRSO.rso` + `product.sel`) is **not executed**; native menu replaces it.
- EFB/Z readback is mandatory. Disabling it breaks Pull Stars / galaxy selection. GalaxyPad proved this on 2026-09-13.
- Performance reality check: iPad Pro generally holds ~60 with dips; **iPhone 14 is ~32 FPS in reported scenes** with AOT + Metal. Vita is far weaker than an A15. 60 FPS on Vita is not the first acceptance bar. First-play loop at a stable cadence (even 20–30 FPS) is the first product bar.

Maintained forks GalaxyPad uses (we pin ExpansionPak upstream first, then cherry-pick Apple-only changes we actually need):

| Role | Upstream | GalaxyPad fork |
|---|---|---|
| Runtime | ExpansionPak/ModernGekko | chrissotraidis/ModernGekko |
| Dolphin core | ExpansionPak/RecompCore | chrissotraidis/RecompCore |
| Recompiler | ExpansionPak/DolRecomp | chrissotraidis/DolRecomp |

Pipeline reference: ExpansionPak/ModernGekko-Template (`make recompile ISO=...`).

### The two Grok shares

The share pages (`Nintendo Wii to PS Vita Port Feasibility`, `Automated PS Vita Game Testing Tools`) are JS-rendered and did not yield transcript text. The plan below independently covers both topics from primary sources (vitaSDK, vitacompanion, Copetti/psdevwiki, GalaxyPad docs, Dolphin VideoCommon, vitaGL/PVR_PSP2/GXM).

### Local ROM

Path: `/home/apollo/galaxyvita/Super Mario Galaxy [RMGE01].wbfs`

Treat as **unverified until hashed**. GalaxyPad's pinned container is 3,508,535,296 bytes, SHA-256 `bd0d3d4bc1376a8614fd8f4fee5df86be6bc676e9d617944918fda79ed9e3589`. Phase 1 records our file's actual identity and either matches that pin or documents a different revision and stops auto-adopting GalaxyPad hashes.

---

## Hardware budget (the real constraint)

### CPU — 4× Cortex-A9 MPCore, 444 MHz (500 MHz with CFW clock)

- ARMv7-A, VFPv3-D32, NEON, 32+32 KiB L1/core, 2 MiB shared L2, out-of-order, 2-issue.
- **Games/homebrew may use CPU0, CPU1, CPU2.** CPU3 is reserved for system (~10–15% idle). Unlocking it requires `sceKernelCreateThread` / `sceKernelChangeThreadCpuAffinityMask` with mask **`0x80000`** and typically GrapheneCt's CoreUnlocker80000H.
- Default plan: **hard-pin three threads to the three user cores**. Optional 4th-core work (shader compile, log drain, DVD prefetch) is a later opt-in, not a correctness dependency.

### Memory

| Pool | Size | Use |
|---|---|---|
| USER_RW (MAIN) | ~365 MiB default; **+109 MiB enlarged ≈ 474 MiB** | Guest MEM1+MEM2 (88 MiB), AOT code, CPU-side caches, audio |
| CDRAM | 112 MiB usable VRAM | EFB, textures, GXM buffers |
| PHYCONT | ~26 MiB | Contiguous GPU/audio if needed |
| OS reserved | ~112 MiB | Untouchable |

**Must ship with `param.sfo` ATTRIBUTE2 enlarged-memory (+109 MiB).** Without it the AOT module plus 88 MiB guest RAM will not fit.

**Make-or-break gate:** `main.dol` is 6.28 MiB of Broadway code. DolRecomp C compiled for ARMv7 can easily land in the **40–150 MiB** range. If the linked module plus runtime plus 88 MiB guest plus a bounded texture cache exceeds ~450 MiB, the port is dead until we split/compress/HLE. This is Phase 2, measured, not assumed.

### GPU — PowerVR SGX543MP4+ (TBDR)

- 4 USSE clusters, ~200 MHz, ~25.6 GFLOPS, GLES2-class, max texture 4096, no compute, no geometry shaders as a product path.
- Tile-based deferred: **EFB peek = force a resolve**. Same class of cost GalaxyPad fights on Apple TBDR, on a GPU an order of magnitude weaker.
- Native API: `sceGxm`. Compatibility layers: **vitaGL** (GL→GXM, needs `libshacccg.suprx` on device for runtime GLSL/Cg), **PVR_PSP2** (Imagination GLES2, closer to real GLSL).

### Display / input / audio

- 960×544 OLED (1000) / LCD (2000). Native 16:9. Present Wii 640×456 EFB scaled; do **not** render internal 2×/3× on Vita.
- Dual analog, face/shoulder buttons, front + rear touch, gyro/accel, mic — enough to replace Wiimote+Nunchuk without motion as a requirement.
- `sceAudioOut` (or SDL2/vita). DSP **HLE only**. LLE DSP is not viable.

### Storage

- Game extract is ~3.5–4.7 GiB. Lives on `ux0:` (SD2Vita / memory card), **not** inside the VPK. VPK is host + AOT module + LiveArea. Disc tree is imported to `ux0:data/galaxyvita/game/` (or similar) by the user/build script.

---

## Architecture

### Process split: host PC vs Vita

Everything that touches the DOL happens **on the Linux host**. The Vita never sees a recompiler, `wit`, or a JIT.

```
HOST (this machine)                         VITA (real hardware)
-------------------                         --------------------
verify-disc.sh                              eboot.bin (host + runtime)
extract (wit / dolrecomp extract)           AOT module (compiled C)
dolrecomp → generated C                     ux0:data/galaxyvita/game/  (user data)
arm-vita-eabi-gcc -O2 -mfpu=neon            vitaGL/GXM + sceAudio + sceCtrl
link runtime + module → eboot.bin           vitacompanion FTP 1337 / cmd 1338
vita-make-fself + vita-pack-vpk             PrincessLog / debugnet stdout
vitacompanion-cli payload / launch          synthetic input (kernel module)
```

### Runtime layers (Vita process)

```
vita/host/          SCE app, threads, input, present, logging
vita/gfx/           Video backend (vitaGL bring-up → GXM product)
runtime/            ModernGekko + RecompCore, Vita OS shims
aot/                Generated C (gitignored), compiled into the SELF
```

Do not try to run ModernGekko's desktop `moderngekko-run` on the Vita. We take the **library/runtime** (RecompCore + module ABI) and write a Vita `CoreHost` analogous to GalaxyPad's `apple/ios/` host.

### Threading model (the biggest CPU-side effort)

Dolphin's dual-core map, pinned to Vita cores. Names are the Dolphin threads; the right column is the pin.

Vita affinity masks are not `1 << core`. User cores are `0x10000` / `0x20000` / `0x40000`; system core CPU3 is `0x80000`.

| Vita core | Affinity | Thread | Work | Priority |
|---|---|---|---|---|
| CPU0 | `0x10000` | `cpu-aot` | AOT dispatch (`StaticRecompCore::Run` / chassis), CoreTiming, Broadway-facing MMIO | Highest (e.g. 64) |
| CPU1 | `0x20000` | `gpu-fifo` | `Fifo::RunGpuLoop`, opcode decode, vertex load, shader bind, GXM `sceGxmBeginScene`/`Draw`/`EndScene` | High (e.g. 80) |
| CPU2 | `0x40000` | `audio-io` | DSP HLE, `sceAudioOut` pump, DVD/async file reads from `ux0` | Medium (e.g. 96) |
| CPU3 (opt.) | `0x80000` | `assist` | Shader compile, log drain, disc prefetch, texture decode overflow | Low |

Rules:

1. **Never let the AOT thread migrate.** Cortex-A9 L1 is per-core; migration of the hot loop is a cache disaster.
2. GPU thread owns **all** GXM/vitaGL calls. CPU thread only writes the GX FIFO / AsyncRequests. Same discipline as Dolphin dual-core and GalaxyPad's renderer thread.
3. Audio callback must not take the GPU lock. Underruns are measured separately from FPS.
4. File I/O shares CPU2 with audio via a small work queue so DVD reads cannot stall the FIFO thread (Galaxy's scene transitions are DVD-heavy).
5. Start in dual-core (CPU0+CPU1) with audio on CPU2 as soon as the host exists. Single-core is a debug fallback only (`GALAXYVITA_SINGLE_CORE=1`).
6. Use `sceKernelChangeThreadCpuAffinityMask` at thread create. Log `sceKernelGetThreadInfo` currentCpuId on boot so we can prove the pins from PrincessLog without touching the device.

Synchronization (keep Dolphin's, do not invent a new FIFO):

- Existing `Fifo` circular buffer + CP watermarks.
- `AsyncRequests` for EFB peeks (CPU waits on GPU). This **will** stall CPU0; that is correct, not a bug. Optimize peek batching after we have traces, not before.
- Audio DMA queue with a ~100–200 ms reserve (GalaxyPad's StaticRecomp audio servo is the reference).

Idle skip: GalaxyPad's RMGE01 scheduler wait at `0x804AB358` is a candidate once identity matches. Port as an exact-DOL-hash-gated policy, not a global idle hack.

### Graphics pipeline (the biggest GPU-side effort)

Wii GX is a fixed-function TEV pipeline. Dolphin already lowers it in **VideoCommon** (API-agnostic) then a backend (OGL / Vulkan / D3D / Metal). We add a Vita backend. We do **not** rewrite opcode decode, BP/CP/XF, TEV→shader UID, or texture cache policy.

```
Guest GX FIFO
    → CommandProcessor / Fifo          (CPU0 writes, CPU1 reads)
    → OpcodeDecoding
    → VertexLoader (software on Vita; no HW vertex loader, same as GalaxyPad iOS)
    → VertexManager
    → ShaderGen (UID → GLSL or Cg or GXP)
    → TextureCache + EFBInterface
    → Vita backend: AbstractGfx / AbstractTexture / AbstractFramebuffer
    → sceGxm present to 960×544
```

**Phase A — pixels on screen (vitaGL).** Fastest path to a title screen.

- Port Dolphin's OGL backend down to a GLES2 subset vitaGL actually implements.
- Disable geometry shaders, SSBOs, compute, GL4 paths.
- Force software vertex loading (GalaxyPad already does this on iOS).
- Internal resolution **1×** (640×456 EFB). Scale to 960×544 at present.
- Runtime shader compile via `libshacccg.suprx` (device requirement, documented).
- Accept that vitaGL overhead is temporary.

**Phase B — playable (native GXM backend).** This is the product renderer.

- Implement Dolphin's `AbstractGfx` surface against `sceGxm`:
  - linear / swizzled textures in CDRAM first, USER_RW uncached overflow
  - 16-bit depth + 32-bit color EFB at 1×
  - staging textures for EFB peek (color rarely, depth always for Star Pointer)
  - precompiled GXP shader cache, UID set enumerated on host from a desktop
    ModernGekko playthrough, blobs warmed on-device once via SceShaccCg (there is
    no host GXP compiler outside Sony's SDK — see [GRAPHICS-BACKEND.md](GRAPHICS-BACKEND.md))
- No runtime compiler on the hot path after first run.
- Present: double-buffer, vsync on, 960×544, letterbox 4:3 or native 16:9 (game already has both; Petari `RenderMode.cpp`).

**EFB / Pull Stars (non-negotiable):**

Dolphin RMG settings, also our baseline:

```
[Video_Hacks]
EFBAccessEnable = True
EFBAccessDeferInvalidation = True
[Video_Enhancements]
ArbitraryMipmapDetection = True
```

Star Pointer does `GXPeekZ` from a draw-sync callback (Petari `StarPointerDirector.cpp`). Fabricated or disabled depth is a product bug, not an optimization. Instrument every peek (coord, value, wait_ms, core) over PrincessLog. Optimize only with matched traces: one resolve per tile per frame, cache same-coord repeats (GalaxyPad measured repeats as 0.26% of peek time — do not "optimize" those).

**What we cut on Vita (documented, default, honest):**

- No 2×/3× IR.
- No hires textures, no widescreen hack (native 16:9 exists).
- Optional: disable bloom/EFB copies that are purely post-process if a scene is GPU-bound **and** a side-by-side screenshot gate says the scene is still readable. Default-off experiment, never a silent default.
- THP movies: software decode is expensive; first milestone may stub or downsample. Not a first-play blocker if the opening can skip/survive.

### CPU recompilation

1. Build DolRecomp from the pinned ExpansionPak (or GalaxyPad fork if Wii/C-backend patches are required).
2. Guard **Broadway + 64 MiB MEM2**. DolRecomp can silently fall back to Gekko/0 MEM2 without a titles DB. A script fails the build if the generated metadata is not Wii.
3. `dolrecomp extract` / `wit extract` the WBFS to `generated/extracted/` (gitignored).
4. `dolrecomp <main.dol> RMGE01 generated/aot/` with `--map` only after we validate a map against **this** DOL's bytes. Do not feed Petari's RMGK01 map into RMGE01.
5. Compile generated C with:

```
arm-vita-eabi-gcc -O2 -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard \
  -ffast-math   # only if a correctness gate allows; start WITHOUT -ffast-math
```

6. Chunk size 1024 + indexed dispatch, matching GalaxyPad, unless a Vita I-cache experiment says otherwise (smaller chunks = more dispatch overhead; larger = I-cache thrash on 32 KiB L1I). **Measure both.**
7. Link as a relocatable blob or as objects into `eboot.bin`. Vita has no `dlopen` of unsigned `.so` in the way Linux does; the AOT module is **statically linked** into the SELF for v1. A later split (module as a data file + custom loader) is only if eboot size / LiveArea limits force it.
8. Coverage telemetry: AOT hit / interpreter fallback / invalid PC / SMC store. Same discipline as GalaxyPad. SMC is unhandled in DolRecomp; treat every executable write as an incident. HOME RSO is replaced, not recompiled.

NEON: RecompCore already has host-side paired-single / PS helpers. On Vita, implement those helpers with NEON/VFP rather than scalar C. This is a Phase 5 win, not Phase 2.

### Audio, input, NAND

**Audio:** DSP HLE → float mix → `sceAudioOutOpenPort` 48 kHz stereo. Wii Remote speaker stream mixed into the same port with a separate volume. Measure underruns; do not claim audio OK because music plays.

**Input (Classic Pointer first, Direct Touch later):**

| Wii | Vita default |
|---|---|
| Nunchuk stick | Left analog |
| IR pointer | Right analog **and/or** front touch |
| A / B | Cross / Circle (documented, remappable) |
| C / Z | L / R |
| Spin (shake) | Square (dedicated; never reuse A/B) |
| Plus (pause) | Start |
| Tilt stages | Right analog as tilt-stick (non-motion path is mandatory) |
| HOME | Select → native overlay, not the RSO |

vitacompanion kernel input (`press cross`, `press left-stick x y`, `press front-touch slot x y`) is how we drive this **without touching the device**.

**Saves:** clean virtual NAND under `ux0:data/galaxyvita/nand/`. Never commit saves. Test create / Grand Star write / quit / relaunch / load.

---

## Tooling (installed on this machine + once on the Vita)

### Host packages we install after plan approval

| Tool | Why | Source |
|---|---|---|
| VitaSDK + vdpm | `arm-vita-eabi-gcc`, `vita-elf-create`, `vita-make-fself`, `vita-mksfoex`, `vita-pack-vpk`, headers/stubs | https://github.com/vitasdk/vdpm (`./bootstrap-vitasdk.sh`) |
| vdpm packages | zlib, vitaGL, SDL2, taihen, pthread-embedded as needed | `vdpm install ...` |
| CMake ≥ 3.16, Ninja, Git, Python 3, pkg-config | all builds | distro |
| Node.js | vitacompanion-cli | distro / nvm |
| vitacompanion-cli | `vita payload`, `vita launch`, `vita cp`, screen/nosleep | `npm i -g vitacompanion-cli` (https://github.com/imcquee/vitacompanion-cli) |
| Wiimms ISO Tools (`wit`) | Wii WBFS extract | wit.wiimm.de / DolRecomp `--setup` |
| DolRecomp, ModernGekko, RecompCore | AOT + runtime | git submodules, pinned SHAs in `config/dependencies.lock.json` |
| netcat / socat | PrincessLog / debugnet listeners | distro |

`VITASDK=/usr/local/vitasdk` on `PATH`. Docker (`vitasdk/vitasdk:latest`) is a CI fallback, not the interactive loop.

### Vita, one-time (human installs plugins; after that we never touch it)

Documented in `docs/VITA-SETUP.md`. Assumed: HENkaku/Enso, taiHEN, VitaShell, unsafe homebrew, enough `ux0` free space (~8 GiB).

| Plugin | Role |
|---|---|
| `vitacompanion.suprx` + `vitacompanion_kernel.skprx` | FTP 1337, command 1338, **synthetic buttons/sticks/touch** |
| PrincessLog / catlog (`net_logging_mgr` or `catlog`) | `printf` / `sceClibPrintf` → TCP on the host |
| `libshacccg.suprx` | vitaGL runtime shader compiler (bring-up) |
| PSVshell / oclockvita | 500 MHz clock, frame/CPU overlay |
| CoreUnlocker80000H | optional CPU3; not required for v1 |
| LowMemMode | optional; lets background FTP survive enlarged-memory app |

Env on the host: `GALAXYVITA_IP`, `GALAXYVITA_TITLEID=GVITA0001`.

### Hands-off iteration loop (this is how we work)

`scripts/vita-dev.sh` is the only command we run day to day:

```
# 1. build eboot/vpk
cmake --build generated/build/vita --target galaxyvita

# 2. wake + prevent sleep
echo 'screen on; nosleep on' | nc $GALAXYVITA_IP 1338

# 3. kill previous, push eboot.bin over FTP (seconds, not a full VPK reinstall)
#    first install uses VPK via vita cp + VitaShell is NOT required after first time:
#    vitacompanion-cli `vita payload` copies eboot.bin + param.sfo into ux0:app/GVITA0001/
vita payload     # or our python equivalent using curl FTP + cmd 1338

# 4. launch
echo 'launch GVITA0001' | nc $GALAXYVITA_IP 1338

# 5. attach log
nc -kl 9999 >> generated/logs/vita-$(date +%s).log

# 6. inject a scripted scene (title A+B, file select, etc.)
cat tests/input/title-to-file-select.txt | nc $GALAXYVITA_IP 1338
# example file:
#   wait 5s
#   press cross; wait 100ms; release cross
#   press left-stick 0 128; wait 2s; release left-stick
```

First install of a new TITLEID still needs a VPK once (`vita cp galaxyvita.vpk ux0:/download/` plus a one-time VitaShell install **or** we document using VitaShell once). After that, **eboot hot-update** is the loop. Game data on `ux0:data/galaxyvita/` is not rewritten unless the disc hash changes.

Crash dumps: `ux0:data/galaxyvita/crash.log` pulled over FTP; optional `vita-parse-core` later. GDB (`vdbtcp` port 31337) is a fallback, not the default loop.

---

## ROM → VPK pipeline (user-facing, no copyrighted bytes in git)

```
scripts/verify-disc.sh <image>     # title ID, rev, hashes vs config/galaxyvita-disc.json
scripts/extract-disc.sh            # wit/dolrecomp → generated/extracted/
scripts/generate-aot.sh            # DolRecomp C + Broadway/MEM2 guard
scripts/build-vita.sh              # compile host + aot + pack VPK
scripts/deploy-vita.sh             # vitacompanion payload + launch + log
scripts/package-user.sh            # the public command: IMAGE=... ./scripts/package-user.sh
```

`package-user.sh` is the promised end-state:

1. Refuse to run if the image is not `RMGE01` rev 0 with the pinned DOL hash (or a documented later allowlist).
2. Extract to a unique staging dir.
3. Generate + compile AOT.
4. Pack `GalaxyVita.vpk` **without** the disc tree.
5. Print: "Copy the VPK to the Vita; copy the extracted game folder to `ux0:data/galaxyvita/game/`" **or** optionally stream the extract over FTP as a separate `scripts/push-gamedata.sh`.

`.gitignore` must include:

```
*.wbfs *.iso *.gcm *.dol *.rel
ref/rom/ generated/ extracted/ aot/
*.vpk eboot.bin
ux0-dumps/ nand/ saves/ logs/*.bin
```

The existing WBFS stays in the workspace root but is gitignored immediately in Phase 0. We do not move or rename the user's original file.

---

## Repository layout (created in Phase 0)

```
galaxyvita/
  README.md
  PROGRESS.md                 # living status; every milestone appends a dated entry
  LICENSE                     # GPL-3.0-or-later
  docs/
    PLAN.md                   # this document, copied into the repo
    VITA-SETUP.md             # one-time plugin/IP/clock instructions
    DISC-IDENTITY.md          # filled after verify-disc
    ARCHITECTURE.md           # threading + gfx + memory budget
    EFB-READBACK.md
    INPUT.md
    PERF.md
    RIGHTS-STATUS.md          # private-only until a later gate
  config/
    dependencies.lock.json    # pinned SHAs
    galaxyvita-disc.json      # identity allowlist, no game bytes
    runtime.ini
  scripts/                    # bootstrap, build, deploy, package-user
  tests/input/                # vitacompanion command scripts
  vita/
    host/                     # sceApp, threads, logging, CoreHost
    gfx/                      # vitaGL backend then GXM
    input/
    audio/
    sce_sys/                  # LiveArea, original (non-Nintendo) icons
  runtime/                    # shims over ModernGekko/RecompCore
  third_party/                # submodules: DolRecomp, ModernGekko, RecompCore
  generated/                  # gitignored
  CMakeLists.txt              # Vita toolchain + host tools as separate presets
```

Git: local `git init` on `/home/apollo/galaxyvita`. No GitHub remote unless you provide one. First commit is skeleton + docs + gitignore; **never** the WBFS.

TITLEID: `GVITA0001`. App name: `GalaxyVita`. Version starts `00.01`.

---

## Phased execution

Each phase has an **exit gate**. We do not start the next phase on a green compile.

### Phase 0 — Repo and progress (hours)

- `git init`, `.gitignore`, `LICENSE`, `README.md`, `PROGRESS.md`, copy this plan to `docs/PLAN.md`.
- `docs/RIGHTS-STATUS.md`: private technical work; no public redistribution.
- Do not move the WBFS.

### Phase 1 — Tooling + hello-Vita loop (day)

- Install VitaSDK via vdpm; prove `arm-vita-eabi-gcc -v`.
- Install vitacompanion-cli; write `scripts/vita-env.sh` reading `GALAXYVITA_IP`.
- Build the official hello_world sample, pack VPK, deploy with vitacompanion, launch, capture a PrincessLog/`debugScreen` line on the host.
- **Gate:** a rebuild → `vita payload` → relaunch cycle with **no hands on the Vita**, log line visible on the host. If the Vita is not on the LAN yet, this gate is blocked and we still finish host-side SDK install.

### Phase 2 — Disc identity + AOT size gate (1–2 days)

- Hash the local WBFS; write `docs/DISC-IDENTITY.md` and `config/galaxyvita-disc.json`.
- Pin and build DolRecomp + `wit`; extract; inventory `.dol/.rel/.rso`.
- Generate C; compile a **host** module first (Linux x86_64 or aarch64) via ModernGekko-Template to prove Broadway/MEM2 and get a code-size number.
- Cross-compile the same C with vitaSDK; record `.text/.data` size.
- **Gate:** Vita `.text` of the AOT objects + estimated runtime < ~280 MiB, leaving room for 88 MiB guest + caches in the 474 MiB enlarged pool. If over, stop and design splitting/HLE before any graphics work.

### Phase 3 — Vita host skeleton + runtime shims (several days)

- CMake Vita app: enlarged memory SFO, 500 MHz request (user clock plugin still needed), three pinned threads that only `printf` their core id.
- Port a **Null** video/audio backend of RecompCore (headless). Goal: load AOT, run until first GX FIFO write, log PC/coverage.
- Replace POSIX pieces RecompCore assumes: threads → `sceKernel*`, files → `sceIo`/`newlib` on `ux0`, time → `sceKernelGetProcessTimeWide`, mutex/event.
- **Gate:** PrincessLog shows Broadway boot, MEM1/MEM2 mapped, AOT dispatch hits > 0, no SMC storm. Black screen is OK.

### Phase 4 — Graphics bring-up (the long pole)

- vitaGL init, clear, present 960×544 at 60 Hz empty.
- Wire VideoCommon → vitaGL GLES2 subset. Software vertex loader.
- 1× EFB. Enable EFB access. Log peek latency.
- **Gate:** recognizable title screen (wrist-strap / logo) on the real Vita, photographed or framebuffer-dumped over FTP. Cadence is recorded, not gated yet.

### Phase 5 — Input, audio, first-play

- sceCtrl + front touch → Classic Pointer map. Scripted `tests/input/title-to-file-select.txt`.
- DSP HLE → sceAudioOut.
- NAND path; new file; Gateway Galaxy movement/jump/spin; first Grand Star; Observatory; save; kill; relaunch; load.
- **Gate:** first-play loop on device, driven by vitacompanion scripts as far as pointer-heavy scenes allow; remaining pointer scenes may need one recorded stick-script. "Boots" is not "playable".

### Phase 6 — GXM backend + 3-core tuning

- Replace vitaGL draws with GXM. Host-built shader cache.
- Pin and prove core map with PSVshell overlay + our logs.
- NEON helpers, texture-cache cap, peek batching with evidence.
- **Gate:** matched scene vs Phase 4, better frame time, depth still correct (Pull Star still works).

### Phase 7 — User packaging

- `scripts/package-user.sh` from a clean clone + user image.
- README: "bring your own RMGE01 rev 0".
- Strip any remaining Nintendo bytes from the tree; `scripts/check-repository.sh` rejects WBFS/DOL/VPK-with-gamedata.

---

## Key decisions

1. **AOT C backend, compiled on the host for ARMv7, statically linked.** No on-device recompiler, no JIT, no LLVM-to-Vita until C is proven.
2. **Exact RMGE01 rev 0 only**, identity-gated like GalaxyPad. Other regions are a new pin, not a flag.
3. **Three hard-pinned cores** (AOT / GPU / audio+IO). CPU3 is optional later.
4. **Enlarged-memory +109 MiB is mandatory.** Refuse to ship a default-memory build.
5. **vitaGL first, GXM second.** Pixels before a custom backend. PVR_PSP2 is a spare GLES2 option if vitaGL's subset is too small; it is not the first bet (more moving parts, less homebrew battle-testing than vitaGL).
6. **EFB depth stays on.** Performance work that breaks Pull Stars is rejected.
7. **Internal resolution 1×, present 960×544.** Quality options come after playable.
8. **DSP HLE, not LLE.**
9. **HOME RSO replaced by a native overlay.**
10. **Game data never in git or in the published VPK.** User image in, VPK+data-folder out.
11. **Real Vita is the only performance oracle.** No Vita3K acceptance. Host ModernGekko is for AOT generation and shader UID harvest only.
12. **GPL-3.0-or-later** to stay honest with Dolphin/ModernGekko.

---

## Risks (ordered)

| Risk | Why it can kill the port | Mitigation |
|---|---|---|
| AOT `.text` too big | 6.28 MiB DOL → tens/hundreds of MiB ARM; 474 MiB is the hard ceiling | Phase 2 size gate before gfx; chunk/HLE/split module |
| CPU too slow | iPhone 14 AOT+Metal ~32 FPS in heavy scenes; Vita A9 @ 500 MHz is much slower | 1× IR, idle skip, NEON helpers, honest FPS target, scene-by-scene |
| EFB peek on TBDR | Pull Stars need it; resolve is expensive | Keep it; batch tiles; never fake depth |
| RecompCore POSIX/GL assumptions | Dolphin is a desktop app | Vita shims; strip Qt/imgui/ffmpeg; Null backends first |
| vitaGL subset | Dolphin OGL wants desktop GL | Subset + software vertex; then GXM |
| SMC / uncovered code | DolRecomp does not handle SMC | Inventory; HOME RSO replaced; fallback telemetry |
| Memory fragmentation | Many small GXM allocations in 112 MiB CDRAM | Arena allocators; texture budget; eviction |
| DVD I/O on ux0 | Scene loads stutter if on GPU or AOT core | Dedicated CPU2 queue; read-ahead |
| Legal | Shipping a VPK that embeds derived game code is still derived from a DOL we must not distribute | User-local generation only; CI never sees a ROM |

---

## Open questions (do not block Phase 0–1)

1. Vita firmware, Enso/HENkaku state, and LAN IP (`GALAXYVITA_IP`). If the device is not yet plugin-ready, Phase 1 host SDK still proceeds; deploy gate waits.
2. GitHub/remote URL, if any. Default is local git only.
3. Whether CoreUnlocker80000H is acceptable on your taiHEN config (optional CPU3).
4. Acceptance FPS for "playable" (proposal: first-play completable, no audio-destroying underrun, depth-correct Pull Stars; 60 FPS is a later stretch).

---

## First implementation steps after approval

1. `git init` in `/home/apollo/galaxyvita`; write `.gitignore` so the WBFS cannot be added; `PROGRESS.md` + `docs/PLAN.md`.
2. Bootstrap VitaSDK (`vdpm`) and prove a hello-world VPK builds.
3. Install vitacompanion-cli; add `scripts/vita-dev.sh` skeleton.
4. Hash the local WBFS into `docs/DISC-IDENTITY.md`.
5. Add pinned submodules (DolRecomp, ModernGekko, RecompCore) without fetching game data.

No Nintendo assets, no generated AOT, and no VPK with game data ever land on a branch that could be pushed.
