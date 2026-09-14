# Architecture (working)

See [PLAN.md](PLAN.md) for the full design. This file tracks the *implemented* map.

## Process split

| Where | What |
|---|---|
| Host PC | Disc verify, extract, DolRecomp C generation, `arm-vita-eabi` compile, VPK pack, vitacompanion deploy |
| Vita | eboot (host + runtime + AOT), game tree on `ux0:data/galaxyvita/game/` |

## Threads (target)

Vita affinity masks are **not** `1 << core`:

| Core | Affinity | Name | Work |
|---|---|---|---|
| CPU0 | `0x10000` | cpu-aot | AOT dispatch, CoreTiming |
| CPU1 | `0x20000` | gpu-fifo | GX FIFO, GXM/vitaGL (all GPU API) |
| CPU2 | `0x40000` | audio-io | DSP HLE, sceAudioOut, ux0 reads |
| CPU3 | `0x80000` | assist | optional; needs CoreUnlocker |

Current code: hello-world host that pins three threads and logs `currentCpuId`. Runtime not linked yet.

## Graphics

Target: VideoCommon + vitaGL (bring-up) then native GXM. EFB depth access stays on.

## Memory

`param.sfo` ATTRIBUTE2 enlarged memory (+109 MiB) is mandatory for the game binary. Hello-world may run without it.
