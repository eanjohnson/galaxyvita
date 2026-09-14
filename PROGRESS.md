# GalaxyVita progress

Living log. Newest entry first. Record evidence, not hopes.

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
