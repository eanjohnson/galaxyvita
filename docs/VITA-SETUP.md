# Vita one-time setup

After this, iteration is host-driven. The device stays on the LAN with vitacompanion loaded.

## Assumptions

- HENkaku or Enso, taiHEN, VitaShell
- Unsafe homebrew enabled
- Several GiB free on `ux0:` (game extract is ~4 GiB; first-play needs more headroom)
- Vita and the build machine on the same IPv4 LAN

## Plugins

Copy to `ur0:tai/` and edit `ur0:tai/config.txt`.

```
*KERNEL
ur0:tai/vitacompanion_kernel.skprx
ur0:tai/net_logging_mgr.skprx          # PrincessLog / catlog — pick one stack
# ur0:tai/CoreUnlocker80000H.skprx     # optional 4th core

*main
ur0:tai/vitacompanion.suprx
```

| Plugin | Ports / notes |
|---|---|
| [vitacompanion](https://github.com/devnoname120/vitacompanion) user + kernel | FTP **1337**, commands **1338**, synthetic buttons/sticks/touch |
| PrincessLog / [catlog](https://github.com/isage/catlog) | `printf` / `sceClibPrintf` to the host. Point it at this PC. `socat TCP-LISTEN:9999,fork,reuseaddr -` |
| `libshacccg.suprx` | Required for vitaGL runtime shader compile (graphics bring-up) |
| PSVshell / oclockvita | 500 MHz clock; on-screen core load |
| CoreUnlocker80000H | Optional. Threads use affinity `0x80000` for CPU3 |
| LowMemMode | Optional. Helps background FTP while this enlarged-memory app runs |

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
2. `./scripts/deploy-vita.sh --vpk` copies the VPK to `ux0:/download/`
3. Install once with VitaShell
4. Later updates use `./scripts/deploy-vita.sh` which only pushes `eboot.bin` and relaunches

## Hands-off commands (vitacompanion)

```
echo 'screen on; nosleep on' | socat - TCP:$GALAXYVITA_IP:1338
echo 'quit all; launch GVITA0001' | socat - TCP:$GALAXYVITA_IP:1338
echo 'press cross; wait 100ms; release cross' | socat - TCP:$GALAXYVITA_IP:1338
```

FTP example:

```
curl -q -T eboot.bin ftp://$GALAXYVITA_IP:1337/ux0:/app/GVITA0001/eboot.bin
```
