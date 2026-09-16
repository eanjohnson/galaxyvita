# Vita one-time setup

After this, iteration is host-driven. The device stays on the LAN with vitacompanion loaded.

## Assumptions

- HENkaku or Enso, taiHEN, VitaShell
- Unsafe homebrew enabled
- Several GiB free on `ux0:` (game extract is ~4 GiB; first-play needs more headroom)
- Vita and the build machine on the same IPv4 LAN

## Plugins

Host copies live in `extern/vita-plugins/` (binaries gitignored; hashes in
[`extern/vita-plugins/MANIFEST.md`](../extern/vita-plugins/MANIFEST.md)).
Refresh with `./scripts/download-vita-plugins.sh`.

Copy to `ur0:tai/` and edit `ur0:tai/config.txt`. Pick **one** logging stack
and **one** clock plugin. Recommended: catlog + PSVshellPlus.

```
*KERNEL
ur0:tai/vitacompanion_kernel.skprx
ur0:tai/catlog.skprx
ur0:tai/CapUnlocker.skprx
ur0:tai/PSVshellPlus_Kernel.skprx

*main
ur0:tai/vitacompanion.suprx
ur0:tai/PSVshellPlus_Shell.suprx
ur0:tai/LowMemMode.suprx

*NPXS10015
ur0:tai/catlog.suprx
```

Also copy `psvshell_plugin.rco` to `ur0:data/PSVshell/psvshell_plugin.rco`.
Install `ShaRKBR33D.vpk` once and run it to extract `libshacccg.suprx`
(Sony module, not redistributed here).

| Plugin | Local file | Notes |
|---|---|---|
| [vitacompanion](https://github.com/devnoname120/vitacompanion) 1.06 | `vitacompanion/*.suprx` + `*_kernel.skprx` | FTP **1337**, commands **1338**, synthetic input |
| [catlog](https://github.com/isage/catlog) v1.2 | `catlog/*` | Preferred logger. Settings in Vita Settings → Network |
| PrincessLog v1.0 | `princesslog/*` | Older alternative; do not load with catlog |
| `libshacccg.suprx` | via `sharkbr33d/ShaRKBR33D.vpk` | vitaGL shader compiler; extracted on-device |
| [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) v1.4 | `psvshellplus/*` | Preferred 500 MHz + HUD. Not with Electry PSVshell |
| Electry [PSVshell](https://github.com/Electry/PSVshell) v1.2beta | `psvshell/PSVshell.skprx` | Older clock plugin |
| [oclockvita](https://github.com/frangarcj/oclockvita) 1.2.1 | `oclockvita/oclockvita.suprx` | Oldest clock plugin |
| [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) v1.4 | `capunlocker/CapUnlocker.skprx` | 4th core (`0x80000`). Renamed from CoreUnlocker80000H |
| [LowMemMode](https://github.com/GrapheneCt/LowMemMode) v1.2 | `lowmemmode/LowMemMode.suprx` | Background FTP with enlarged-memory app |

Reboot after installing kernel plugins.

## Host environment

```bash
export GALAXYVITA_IP=192.168.x.x          # Settings → Network → Wi-Fi
export GALAXYVITA_TITLEID=GVITA0001
export VITASDK=$HOME/vitasdk
export PATH="$VITASDK/bin:$PATH"
```

Persist IP in `scripts/.vita-ip` (gitignored) or `.env.local`.

## First VPK install

1. Build with `./scripts/build-vita.sh`
2. `./scripts/deploy-vita.sh --vpk` copies the VPK to `ux0:/vpk/`
3. Install once with VitaShell (`ux0:vpk/galaxyvita.vpk`)
4. Later updates use `./scripts/deploy-vita.sh` which only pushes `eboot.bin` and relaunches

## Hands-off commands (vitacompanion)

```
echo 'screen on; nosleep on' | socat - TCP:$GALAXYVITA_IP:1338
echo 'destroy; launch GVITA0001' | socat - TCP:$GALAXYVITA_IP:1338
echo 'press cross; wait 100ms; release cross' | socat - TCP:$GALAXYVITA_IP:1338
```

FTP example:

```
curl -q -T eboot.bin ftp://$GALAXYVITA_IP:1337/ux0:/app/GVITA0001/eboot.bin
```
