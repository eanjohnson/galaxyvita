# Disc identity

Recorded 2026-09-14. The original WBFS was not modified. Extracted `main.dol` lives only under gitignored `generated/extracted/`.

## Container

| Field | Value |
|---|---|
| Filename | `Super Mario Galaxy [RMGE01].wbfs` |
| Bytes | 3508535296 |
| SHA-256 | `0f5ba5a1c9da2a3319daa509db064a41e16a2a04c9114802a8f8addc14c1b2de` |
| SHA-1 | `4a8f22e7a22b1af640ddb571b2ae78a7f0788703` |

Container SHA-256 **differs** from GalaxyPad's WBFS pin `bd0d3d4bc1376a8614fd8f4fee5df86be6bc676e9d617944918fda79ed9e3589` even though the byte length matches. Treat the dumps as different *containers*.

## Disc (wit v3.05a r8638 DUMP)

| Field | Value |
|---|---|
| Title ID | `RMGE01` |
| Name | Super Mario Galaxy |
| Region | NTSC/USA |
| Region setting | 1 [USA] |
| IOS | 33 (`00000001-00000021`) |
| Type | WBFS/Wii, 1 encrypted/scrubbed/signed DATA partition |
| Virtual size | 4699979776 bytes |
| Files (wit) | 2383 files, 79 directories |

## Primary executable

`sys/main.dol` extracted with `wit EXTRACT --files '+/sys/main.dol'`.

| Field | Value |
|---|---|
| Bytes | 6283264 |
| SHA-256 | `2c680585a8f58e1cc9c5521b579057f12b124ff0ef409e470a57606c50a93c09` |
| SHA-1 | `9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd` |

This **matches** GalaxyPad's RMGE01 revision 0 DOL pin. AOT work can use the GalaxyPad DOL identity. Do not assume the rest of the filesystem matches their manifest (they counted 2386 files).

HOME-menu RSO files are not yet inventoried.

Do not paste game bytes here.
