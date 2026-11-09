# miniaudio — vendored third-party notice (P8 M-S0)

- **Upstream**: <https://github.com/mackron/miniaudio> / <https://miniaud.io>
- **Version lock**: **v0.11.26 (2026-10-31)**. The version string is on line 3 of `miniaudio.h`;
  `MA_VERSION_MAJOR/MINOR/REVISION` = 0/11/26. Fetched from tag `0.11.26` via the
  gh-proxy mirror of `raw.githubusercontent.com` (direct GitHub HTTPS is blocked on this machine).
- **Files kept** (only these):
  | file | role |
  |---|---|
  | `miniaudio.h` (~4 MB) | single-header library, declarations + full implementation behind `MINIAUDIO_IMPLEMENTATION` |
  | `miniaudio.c` (56 B) | the ONE implementation TU: `#define MINIAUDIO_IMPLEMENTATION` + `#include "miniaudio.h"` |
  | `VENDOR.md` | this notice |
- **NOT vendored**: `examples/`, `extras/`, `tests/`, `resources/`, `research/`, `tools/`,
  `.github/`, `README.md`, upstream `LICENSE`. M-S0 only needs the `ma_engine` high-level API;
  the full dual-license text is already embedded at the end of `miniaudio.h`.
- **License**: upstream offers a **choice of Public Domain (unlicense.org) or MIT-0**
  (stated on line 2 of `miniaudio.h`, full text of both alternatives at the end of that file).
  This project chooses **Public Domain / Unlicense**. No LGPL-style relinking obligation,
  no source-offer requirement, no C++20 requirement (pure C99, project stays C++17).
- **Build discipline**: `MINIAUDIO_IMPLEMENTATION` is defined in **exactly one** TU
  (`thirdparty/miniaudio/miniaudio.c`, compiled into the `nl_miniaudio` static target).
  Never define it in `core/` or in any tool TU. Consumer code includes `<miniaudio.h>` for
  declarations only.
- **Upgrade policy**: a version bump is its own acceptance round — CMake configure,
  both graphics-backend build trees, `audio-tools selftest`, offline dump determinism,
  evidence PNG reshoot, `VENDOR.md` version lock update. Do not bump in passing.
- **Scope note**: miniaudio's official manual states HRTF and environmental occlusion are
  not supported. They are not required for this track's goal (scene 3D SFX + in-game
  audio debugging); the upgrade path is the official `engine_steamaudio` example.
