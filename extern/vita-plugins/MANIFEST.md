# Vita plugin downloads

Fetched 2026-09-14 from upstream GitHub releases. Binaries are gitignored;
re-run `scripts/download-vita-plugins.sh` to refresh.

`libshacccg.suprx` is Sony's shader compiler. It is **not** in this folder.
Install [ShaRKBR33D](sharkbr33d/ShaRKBR33D.vpk) on the Vita; it extracts the
module from the PSM runtime onto `ur0:data/libshacccg.suprx`.

Do **not** install both logging stacks, and do **not** install more than one
clock plugin (PSVshell, PSVshellPlus, oclockvita).

| Plugin | Version | File | SHA-256 | On-device path |
|---|---|---|---|---|
| vitacompanion user | 1.06 | `vitacompanion/vitacompanion.suprx` | `ba0c971464339eaf6cb4267057505d91e745e7cc6dbf5fd75f1023e1128f03b8` | `ur0:tai/vitacompanion.suprx` (`*main`) |
| vitacompanion kernel | 1.06 | `vitacompanion/vitacompanion_kernel.skprx` | `625361ace4b3f1e80ee69dca6f449c01566cc4664c179cd419dbcc11dd8bf130` | `ur0:tai/vitacompanion_kernel.skprx` (`*KERNEL`) |
| catlog kernel | v1.2 | `catlog/catlog.skprx` | `994ca4174daaf56383d9b8b1cfc5288a5209580c08969aa30098caa29ae96731` | `ur0:tai/catlog.skprx` (`*KERNEL`) |
| catlog user | v1.2 | `catlog/catlog.suprx` | `f78c9bb32635a39c1b656400ec33c58857688fcc9664e3cda27523298878e59d` | `ur0:tai/catlog.suprx` (`*NPXS10015`) |
| PrincessLog | v1.0 | `princesslog/net_logging_mgr.skprx` | `e2cc79d8a2d76f27534e603a03a690a630f16cac44906a9c722632308d0fea5a` | `ur0:tai/net_logging_mgr.skprx` (`*KERNEL`) |
| PrincessLog settings | v1.0 | `princesslog/NetLoggingMgrSettings.vpk` | `70f7bc6fadfa71f82a206d97c496a23b54f61193f3219d6b7dfb9c0c2a2abede` | install VPK once |
| CapUnlocker (was CoreUnlocker80000H) | v1.4 | `capunlocker/CapUnlocker.skprx` | `40df9f495275fd5780966827e69f6a83f6abb0bee5ea608d558a10c06d897da0` | `ur0:tai/CapUnlocker.skprx` (`*KERNEL`) |
| LowMemMode | v1.2 | `lowmemmode/LowMemMode.suprx` | `6d17d4bcbe835f5c138222d65b56d6e0329e8799db99820db61f692a9b5335eb` | `ur0:tai/LowMemMode.suprx` (`*main`) |
| PSVshell | v1.2beta | `psvshell/PSVshell.skprx` | `f6164ddfdc29428d25340431953c5f8e6381be2d21d935abbc67f688e973dea8` | `ur0:tai/PSVshell.skprx` (`*KERNEL`) |
| PSVshellPlus kernel | v1.4 | `psvshellplus/PSVshellPlus_Kernel.skprx` | `2c6f086e803ed5b9e8bcae39ca02684c473d8880c512aadb58a84ac1a20c638a` | `ur0:tai/PSVshellPlus_Kernel.skprx` (`*KERNEL`) |
| PSVshellPlus shell | v1.4 | `psvshellplus/PSVshellPlus_Shell.suprx` | `3a1e929532872524800744f37344fe6d4ddf25300120fe35451bedd338b748ce` | `ur0:tai/PSVshellPlus_Shell.suprx` (`*main`) |
| PSVshellPlus RCO | v1.4 | `psvshellplus/psvshell_plugin.rco` | `72285cc9354009fceaf91567431df490744a83f9f581e79c2e3ff940a48f02d6` | `ur0:data/PSVshell/psvshell_plugin.rco` |
| oclockvita | 1.2.1 | `oclockvita/oclockvita.suprx` | `4c5b4742dc9fdfbd11a6590aba45f9de87547dea10420e0c93c749ea063daf7d` | `ur0:tai/oclockvita.suprx` (`*main`) |
| ShaRKBR33D | v.1.2 | `sharkbr33d/ShaRKBR33D.vpk` | `283856eaad4f99c72ee3d6b0db16ce76dc4716f1d6ef2e9fccf9e65a4acba9af` | install VPK; run on device |

Recommended set for GalaxyVita: vitacompanion (both), **catlog** (not PrincessLog), CapUnlocker, LowMemMode, **PSVshellPlus** (not Electry PSVshell and not oclockvita), ShaRKBR33D.
