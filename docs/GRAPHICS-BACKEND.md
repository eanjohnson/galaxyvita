# Graphics backend decision

Decided 2026-09-15. Supersedes the hand-written GX translator described in
[../PROGRESS.md](../PROGRESS.md) under "Host software rasterizer". See
[PLAN.md](PLAN.md) Phase B for the product target this serves.

## The decision

**Drive ModernGekko's Dolphin VideoCommon. Write only the Vita backend.**

The alternative — finish the hand-written GX translator in `runtime/gx_fifo.c`
plus `tests/host/gx_soft.c` — was rejected. It reimplements code the repo
already vendors, and it reimplements it worse: the FIFO parser, the vertex
loader and the texture decoder written for the host rasterizer are duplicates of
`gx_command_processor.cpp`, `gx_vertex_loader.cpp` and `gx_texture_decoder.cpp`.
Everything past those three — TEV, lighting, texgen, indirect textures, EFB
copy formats — is where the real work is, and Dolphin's version of it is the
reference implementation this project would otherwise be guessing at.

`tests/host/gx_soft.c` stays, scoped down to a **debug oracle**: a PNG dump we
can diff a Vita frame against. It is not on the product path and does not get
extended.

## What the porting surface actually is

`third_party/ModernGekko/include/moderngekko/gpu_backend.hpp` — seven virtuals,
all with empty defaults:

```
LoadCpRegister  LoadXfRegisters  LoadBpRegister  LoadIndexedXf
Draw            CallDisplayList  InvalidateVertexCache
```

That is the entire GX-facing contract. ModernGekko's `GxCommandProcessor` owns
the FIFO; the backend receives decoded register writes and vertex packets. The
mapping onto what was hand-built is one-to-one — `LoadIndexedXf` is literally
the indexed-XF fix landed this session.

Below that sits Dolphin's `AbstractGfx` (2 pure virtuals, 40 total) for the
resource and submission layer: pipelines, textures, framebuffers, scissor,
viewport, present. That is the part with real volume, and it is what Phase B
means by "implement `AbstractGfx` against `sceGxm`".

## ARMv7 viability — measured, not assumed

`scripts/probe-vita-gx.sh` compiles the 11 Dolphin VideoCommon files in the
`moderngekko_dolphin_video` target plus all 7 of ModernGekko's `src/gpu/`
sources for `arm-vita-eabi`, using the same flags as the CMake target (C++23,
`vendor/dolphin_legacy` + `vendor/picojson` + `src/dolphin_shims`).

**18/18 objects build** with gcc 15.2.0 arm-vita-eabi. One source change was
needed, and only one:

- `VideoCommon/DriverDetails.cpp` gated `m_os` behind an `#ifdef` chain with no
  `#else`, so it failed to compile on any platform Dolphin has not heard of.
  Fixed with `constexpr u32 m_os = OS_ALL;` — every bug in that table is keyed
  to a desktop or mobile driver, so `OS_ALL` selects exactly the applicable
  (OS-agnostic) subset. Kept as
  `patches/moderngekko-driverdetails-vita-os.patch` so it survives a submodule
  re-checkout.

Dolphin's own CMake refuses unknown architectures unless `ENABLE_GENERIC=ON`
(`vendor/dolphin/CMakeLists.txt:205-232`). That gate is about the JIT, which
DolRecomp replaces outright, so it costs us nothing here.

## The one open question: shader target

`APIType` is `{OpenGL, D3D, Vulkan, Metal, Nothing}` (`VideoCommon.h:44`). There
is no GXM entry, and the Vita is an SGX543MP4+ — GLES2-class hardware.

Two things make this smaller than it first looks:

1. **The binding syntax is ours.** `UBO_BINDING`, `SAMPLER_BINDING` and
   `VARYING_LOCATION` are preamble macros supplied by the *backend*, not baked
   into the generated body — OGL defines them three different ways depending on
   what the driver supports (`VideoBackends/OGL/ProgramShaderCache.cpp:759-790`),
   and D3DCommon defines them differently again. A Vita backend defines its own.
2. **Dolphin already degrades on capability flags.** `DolphinShaderCapabilities`
   carries ~24 of them (`bitfield`, `dual_source_blend`, `geometry_shaders`,
   `early_z`, `logic_op`, `framebuffer_fetch`, …) and
   `dolphin_shader_compiler.cpp` plumbs each one into `g_backend_info`. Setting
   them honestly for the Vita is the supported way to get conservative shaders.

What does **not** go away is integer math in the generated pixel shader bodies —
`int4`, `idot`, `>>`, `|`, `iround` — which is how Dolphin gets TEV bit-exact.
`bSupportsBitfield` only guards the UberShader path, not the specialised one.

The plan of record (`PLAN.md:205`) is a **precompiled GXP cache built on host
from a desktop ModernGekko UID dump**, and the compiler is shaped for exactly
that: `DolphinShaderCompiler::Compile()` returns shader source plus stable
FNV-1a-64 UIDs for each stage, so a host playthrough can enumerate SMG's UID set
offline and a `psp2cgc` pass can turn it into GXP before anything ships.

### Two findings that move PLAN.md:205

**There is no host GXP compiler.** `psp2cgc` ships with Sony's official SDK and
is not in the VitaSDK — `~/vitasdk/bin` has nothing that compiles shaders. So
"precompiled GXP cache built on host" cannot be built on the host as written.

**There is an on-device one.** The pinned SDK carries
`libSceShaccCg_stub.a` + `psp2/shacccg.h` (`sceShaccCgCompileProgram`, profiles
`SCE_SHACCCG_PROFILE_VP` / `_FP`) and vitaShARK's wrapper
(`shark_compile_shader(src, &size, type) -> SceGxmProgram *`). That is the
Vita's own Cg compiler, backed by `libshacccg.suprx`, which the SDK does not
distribute — `shark_init()` takes a path to it and the user supplies it from
their own console. Same posture as the ROM, and it fits the project's existing
"user provides their own files" scripting.

This keeps the *cache*, which is the part that matters for frame time, and moves
only *when* it is filled: enumerate SMG's UID set from a desktop ModernGekko
playthrough (`DolphinShaderCompiler::Compile` already returns stable FNV-1a-64
UIDs per stage), then warm those UIDs on-device once on first run and persist
the GXP blobs to `ux0:`. No runtime compilation on the hot path after run one.

## The Cg question, settled (2026-09-16)

Measured against 32 draw states captured from a real SMG boot
(`GV_GFX_STATEDUMP`, then `scripts/dump-dolphin-shaders.sh`), which generate 26
distinct pixel-shader UIDs and up to 3 TEV stages / 3 texgens. Shaders run
6.6-9.1 KB each.

**The translation is mechanical.** `scripts/glsl_to_cg.py` converts all 64
generated shaders (32 vertex, 32 pixel) to Cg with **zero residual GLSL
constructs**. It is ~120 lines, and most of it is undoing work the OpenGL
backend does: Dolphin's shader bodies are already written in an HLSL-flavoured
dialect (`float4`, `int4`, `iround`) and it is the *GLSL* backend that aliases
them down to `vec4`/`ivec4`. Cg wants the original spelling back.

What is genuinely structural is small and enumerable:

| Gap | Size | How |
|---|---|---|
| GLSL globals vs Cg entry-point semantics | 5-8 decls/shader | hoist to `static`, generate an entry point that copies in/out |
| `gl_FragCoord` / `gl_Position` / `gl_FragDepth` | 3 names | `WPOS` / `POSITION` / `DEPTH` semantics |
| `sampler2DArray` | **2 lines/shader** | collapses to `sampler2D`: the array layer is only used for stereo, which is off, so `layer` is 0 in every captured state |
| `texture()` | **1 call/shader** | all sampling funnels through one `sampleTexture` helper → `tex2Dlod` |
| std140 blocks | 3 blocks | flattened to top-level uniforms; GXM offers 14 uniform buffers and Dolphin wants 3 |
| `bitfieldExtract` | 1 fallback | Dolphin already emits its own when the backend says no bitfield support |

**The shaders are not desktop-only.** With a correct preamble, all 64 validate
as **GLSL ES 3.00** under `glslangValidator` — no SSBOs, no image load/store, no
dependence on `early_fragment_tests`. Every initial failure was a preamble
defect on our side (a bare `;` from an empty `FORCE_EARLY_Z`, a missing
`precision highp sampler2DArray`, `binding` layout used below ES 3.10), not a
shader that needed features the hardware lacks.

**Integer math has positive evidence on GXM.** `SceGxmParameterType` includes
`SCE_GXM_PARAMETER_TYPE_S32` and `_U32` as first-class shader parameter types
(`psp2/gxm.h:1316`), and vitaShARK's `shark_compile_shader_extended` carries a
`use_fastint` switch. So integers are not absent from the profile.

**There is no host GXP compiler.** `psp2cgc` ships only with Sony's official SDK.
The compiler that does exist is on-device: `libSceShaccCg_stub` +
`psp2/shacccg.h` (`sceShaccCgCompileProgram`, profiles `_VP`/`_FP`), wrapped by
vitaShARK as `shark_compile_shader(...) -> SceGxmProgram *`. It is backed by
`libshacccg.suprx`, which the SDK does not distribute — `shark_init()` takes a
path and the user supplies it from their own console. Same posture as the ROM,
and it fits the project's existing "user provides their own files" scripting.

So `PLAN.md`'s precompiled cache survives; only *when* it is filled moves.
Enumerate SMG's UID set from a desktop ModernGekko playthrough
(`DolphinShaderCompiler::Compile` returns stable FNV-1a-64 UIDs per stage), warm
those on-device once on first run, persist the GXP to `ux0:`. No runtime
compilation on the hot path after run one.

### The ALU question, answered on hardware (2026-09-16)

The open risk was whether ShaccCg's fragment profile accepts the **integer ALU**
the TEV code leans on. Only the console could answer it: the compiler is a
firmware module with no host equivalent, and there is no `libshacccg.suprx` or
Vita3K on this machine. `vita/gfx/shader_selftest.c` +
`scripts/shader-selftest-vita.sh` run that test unattended — translate, stage
the Cg, compile on-device, fetch a per-shader report.

Two false starts came first, both worth recording because each cost a day:

- **Every deploy crashed before `main`.** The link line was missing `-Wl,-q`.
  `vita-elf-create` needs retained relocations; without them it still accepts
  the ELF *silently* and the result jumps into padding. Confirmed from the
  user's core dump — PC `0x81002e10` sat in the zero padding between
  `_strdup_r` (ends `0x81002e08`) and `strlen` (starts `0x81002e40`) — and
  from `readelf -S`: **0 rel sections without the flag, 6 with**. This is a
  mandatory flag for every VitaSDK binary in this project, not a tuning knob.
- **vitaShARK is a dead end.** `shark_compile_shader` returns
  `error:-1 fatal internal error` for *everything*, including a two-line
  control shader, with GXM up, a 128 MB heap, `shark_init()` → 0 and an unsafe
  fself. Bypassed by driving `sceShaccCgCompileProgram` directly, with an
  `openFile` callback that hands the whole program over from memory.

With that out of the way the compiler answered immediately:

```
compiler libshacccg_version: SDK 3.0.0,
  graphics/release_branches/release_03.000/shacc r13225,
  sys/release_branches/release_03.000 r85995

control vertex: ok (244 B gxp)
```

A healthy compiler, and then one root cause under every Dolphin shader error:

> **The Vita's Cg profile has no unsigned integer type at all.** `uint` and
> `uint4` are not keywords. Every uniform block, function signature and
> bitfield helper Dolphin emits trips on them, and the parser cascades: an
> unknown type before an identifier reads as `uniform <block> {`, which is why
> a single missing keyword produced 14 consecutive "expected `{` after new
> uniform block declaration" errors.

A second, independent defect surfaced in the same report: Dolphin writes array
declarations in GLSL's type-side form, `DolphinLightData[8] lights_chan0_color;`
(six per shader, across all 64). That is legal GLSL and is not legal Cg.

Both are mechanical, and both are fixed in `scripts/glsl_to_cg.py`:

| Defect | Fix | Note |
| --- | --- | --- |
| no `uint`/`uvec`/`uint4` | map to `int`/`int4` | signed is the only width-correct option the profile has |
| duplicate `bitfieldExtract` | drop the `uint` overload | must run *before* the rename, or the two are textually identical |
| `int(val >> off) & mask` | `(val >> off) & mask` | `>>` is arithmetic once the type is signed; the mask discards the smeared sign |
| `Type[N] name;` | `Type name[N];` | declaration form only — Dolphin never emits the `Type[N](...)` constructor |

Local verification: 64/64 translate, **0 residual `uint`**, **0 residual
type-side arrays**, exactly one `gv_bfe` definition per shader.

**Confirmed on hardware 2026-09-16: 64 ok, 0 failed, 0 skipped, 0 warnings,
0 errors.** 162,276 bytes of GXP across 64 programs; largest 7,584 B, mean
2,535 B. Every shader Dolphin's generators emit for the captured SMG draw
states compiles on the console.

Two further defects surfaced during that run and are fixed:

| Defect | Fix | Why it matters |
| --- | --- | --- |
| `mat3`/`mat3x4` unknown | alias to `float3x3`/`float3x4`, rewrite `v * M` to `mul(M, v)` | GLSL `matCxR` is columns-by-rows and Cg `floatRxC` is rows-by-columns; the transpose cancels because Dolphin assigns whole 4-vectors by index and multiplies row-vector-first. Verified against both call sites. |
| `uniform #define ...` | pass preprocessor lines through the block flattener untouched | Dolphin puts accessor macros *inside* the uniform block; prefixing one with `uniform` stops the parser dead |
| no integer `clamp` | supply exact `int`/`intN` overloads | the TEV code clamps values whose signed wrap-around *is* the hardware behaviour, so rounding them through float would be a silent correctness bug |
| `0u` literals, file-scope `const` | strip the suffix; emit `static const` | in Cg a file-scope `const` is a *uniform with a default*, and the profile cannot honour defaults -- the value would read back as 0 at runtime |

### The compiler cannot run unbounded in one process

ShaccCg stops returning after a handful of compiles, regardless of what it is
compiling: the *identical* shader compiles five times and hangs on the sixth.
It hangs rather than failing, so there is no diagnostic.

Instrumenting the allocator that `sceShaccCgSetDefaultAllocator` installs
showed live memory returning to ~0 after every compile with a per-compile
high-water mark under 2 MB -- but ~28 MB of *churn* per compile across many
small allocations. That is newlib arena fragmentation, not a leak.

Two fixes were tried, in order:

- **A bump arena reset between compiles** broke the very next compile. The
  accounting had shown ~1 KB per compile never handed back, and reclaiming
  wholesale pulls that out from under the compiler. Reverted.
- **A segregated free list** (sizes rounded to powers of two, freed blocks
  returned to their bucket) keeps real `free` semantics while making
  fragmentation impossible. Arena use went flat -- 5,562 KB to 5,569 KB across
  six compiles while absorbing 305 MB of churn through 1.06 M block reuses --
  and every compile produced a byte-identical 3,380 B GXP, which is what rules
  out corruption.

That plus a 300 MB heap moved the wall from 5 compiles to 9, but did not remove
it: something the compiler allocates outside its callback allocator still
grows. Rather than keep chasing a closed-source firmware module, the self-test
now **compiles a bounded batch (6) per launch and resumes on the next one**,
recording progress after every shader so a wedge is skipped rather than
repeated. A fresh process reclaims everything unconditionally. The full 64
takes 11 launches.

This is not a workaround peculiar to the test -- it is the shape the real
cache warmer needs. Warming SMG's whole UID set in one process was never going
to work, and `PLAN.md`'s persist-to-`ux0:` design already assumed incremental
warming.

This vitacompanion build has no `install` command, so the runner borrows the
project's own registered `GVITA0001` slot, backing the real eboot up first
(23,124,998 B, sha256 `f3a70256ed3c2e35…`) and restoring it from a trap on exit.

If the ALU turns out to be the wall after all, the fallback is Dolphin's own
precedent — the Metal backend does not emit MSL from `PixelShaderGen` either;
it compiles the GLSL through glslang to SPIR-V and retargets with SPIRV-Cross
(`VideoBackends/Metal/MTLUtil.mm:493`). The same pivot is available here, and
`glslangValidator` is already on this machine.

## Consequences

- `runtime/gx_fifo.c` + `runtime/gx_render.h` are replaced by
  `GxCommandProcessor` + a `GpuBackend` implementation. The census and
  desync-diagnostic machinery in `gx_fifo.c` has done its job and does not
  carry over.
- `tests/host/gx_soft.c` freezes as a debug oracle.
- Dolphin's `VideoBackends/Software` (a complete CPU GX implementation —
  `Clipper`, `Rasterizer`, `Tev`, `TextureSampler`, `TransformUnit`, `EfbCopy`)
  becomes the reference for any behaviour the Vita backend gets wrong.
