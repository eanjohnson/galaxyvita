Pinned checkouts of DolRecomp, ModernGekko, and RecompCore land here as git
submodules. SHAs live in `config/dependencies.lock.json`.

| Path | Upstream | Notes |
|---|---|---|
| `third_party/DolRecomp` | ExpansionPak/DolRecomp | C backend, Broadway |
| `third_party/ModernGekko` | ExpansionPak/ModernGekko | Vita host will consume the library, not `moderngekko-run` |
| `third_party/ModernGekko/vendor/dolphin` | ExpansionPak/RecompCore @ `moderngekko-vendor` | The only RecompCore graph. Do not add a second clone. |

RecompCore is a Dolphin fork with nested Externals (Windows Qt/FFmpeg, mGBA, …).
Those nested submodules are **not** initialized. Linux system packages cover the
host tools later; the Vita port does not want that tree on disk.
