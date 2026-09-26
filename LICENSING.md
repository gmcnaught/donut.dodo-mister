# Licensing

Copyright (C) 2026 Grant McNaught.

Unless a section below says otherwise, the code in this repository is licensed under the
**GNU General Public License v3.0** (`LICENSE`), SPDX `GPL-3.0-only`. That includes `src/misterglue/`, `src/vendor/`
(mfgpu and the MiSTer fabric code, copied from the author's own gmloader-next work and relicensed here),
`tools/mister-wrapper/`, `dist/` and `scripts/`.

## Exceptions

| Path | Licence |
|---|---|
| `tools/mem_wc/` | GPL-2.0, from skmp/minicast (see its SPDX header and `tools/mem_wc/README.md`). |
| `patches/0001`, `0003`–`0005`, `0007` (SDL2) | Changes to SDL2 2.32.10, which is under the zlib licence. New files these patches add are GPL-3.0; a libSDL2 built with them is distributed under GPL-3.0 as a whole. |
| `patches/0002`, `0006` (FRT / Godot) | Changes to FRT 2.1.0 and Godot 3.5.2 (MIT, © Emanuele Fornara; © Juan Linietsky, Ariel Manzur and Godot Engine contributors). The engine binary in the release is built from those upstreams plus these patches. |
| `MiSTer_DonutDodo` | Built from MiSTer-devel/Main_MiSTer (GPL-3.0) plus `tools/mister-wrapper/overlay/`. The source is that upstream commit (pinned in `tools/mister-wrapper/build-hps.sh`) plus this repository. |

Donut Dodo and its assets are © Zapposh and are not part of this repository or its releases.
