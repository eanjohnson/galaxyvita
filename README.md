# GalaxyVita

Super Mario Galaxy on PlayStation Vita through ahead-of-time recompilation.

The game's PowerPC code is translated on a PC into native ARMv7, then linked
against a Dolphin-derived Wii runtime with a Vita host (GXM/vitaGL, sceAudio,
sceCtrl/touch, three pinned Cortex-A9 cores). You supply a legally obtained
USA `RMGE01` revision 0 image. This repository does not include, download, or
redistribute game data.

Built on [DolRecomp](https://github.com/ExpansionPak/DolRecomp),
[ModernGekko](https://github.com/ExpansionPak/ModernGekko), and
[RecompCore](https://github.com/ExpansionPak/RecompCore). Process and
identity gates follow [GalaxyPad](https://github.com/chrissotraidis/galaxypad).

**Status:** private technical bring-up. Not playable. See [PROGRESS.md](PROGRESS.md).

## What you need

- A hacked PS Vita (HENkaku/Enso, taiHEN, VitaShell) on the same LAN
- A USA Super Mario Galaxy Wii dump, title ID `RMGE01`, revision 0
- Linux host with CMake, Ninja, Git, Python 3, Node.js (for vitacompanion-cli)

Game data is **not** packed into the published VPK. The build scripts extract
your image on the host, generate the AOT module, and produce a VPK plus a
separate game-data folder you copy to `ux0:data/galaxyvita/game/`.

## Quick start (host)

```bash
git clone --recurse-submodules <this-repo>
cd galaxyvita
./scripts/bootstrap-host.sh
# After VitaSDK is on PATH:
./scripts/build-vita.sh          # hello-world / current target
# Optional, once GALAXYVITA_IP is set and vitacompanion is on the device:
./scripts/deploy-vita.sh
```

Full architecture, threading, and graphics plan: [docs/PLAN.md](docs/PLAN.md).
One-time device plugins: [docs/VITA-SETUP.md](docs/VITA-SETUP.md).

## Legal

Nintendo's game and trademarks belong to their owners. This project is not
affiliated with Nintendo or Sony. Source is GPL-3.0-or-later because it
adapts Dolphin-derived code. Do not commit disc images, extracted Nintendo
files, generated AOT, saves, or VPKs that embed game data.

## Title

- TITLEID: `GVITA0001`
- App name: GalaxyVita
