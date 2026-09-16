# AOT size gate (Phase 2)

Measured 2026-09-14. Generated C is gitignored under `generated/aot/`. No game bytes here.

## Input

- DOL: `sys/main.dol` 6,283,264 bytes, SHA-256 matches GalaxyPad RMGE01 rev 0
- DolRecomp `40637c4`, C backend, Broadway, `DOLRECOMP_C_CHUNK_INSTRUCTIONS=1024`
- 1,322 chunk files + dispatcher stub (`generated.c` is comments only)
- 20 possible SMC sites listed in `generated_smc.txt` (not patched yet)

## Object size (`-O2`)

| Target | `.text` | `.data` | `.bss` |
|---|---|---|---|
| Host x86_64 gcc 16.1.1 | 70.62 MiB | 0 | 0 |
| Vita `arm-vita-eabi-gcc` 15.2.0 thumb+NEON | **51.88 MiB** | 0 | 0 |

Vita `.text` vs 280 MiB gate: **PASS** (51.88 MiB).

## Budget against enlarged USER_RW (~474 MiB)

| Item | Size |
|---|---|
| AOT `.text` (measured) | 52 MiB |
| Guest MEM1+MEM2 | 88 MiB |
| Remaining for runtime, caches, host | ~334 MiB |

The 280 MiB gate is AOT + estimated runtime. Runtime is not linked yet. Even a 100 MiB stripped RecompCore/host would still fit with ~130 MiB of cache headroom.

## Broadway / MEM2

`tests/host/mem2_smoke.c` linked against DolRecomp `cpu.c`:

```
broadway MEM1=25165824 MEM2=67108864 DOLRECOMP_CPU_BROADWAY=1
```

## Reproduce

```
./scripts/extract-disc.sh            # identity DOL if missing
./scripts/inventory-disc.sh
./scripts/generate-aot.sh
./scripts/measure-aot-size.sh
```
