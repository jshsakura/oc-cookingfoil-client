# Third-party notices

CookingFoil is licensed under the GNU General Public License v3.0 or later (see [LICENSE](LICENSE)).
It includes or links the components below. Each remains under its own license.

## Compiled into the app

### CyberFoil install engine

- Source: https://github.com/luketanti/CyberFoil (submodule `third_party/CyberFoil`, pinned commit
  `eaf0353100e03f2772a037eccdec9f680f8a8de0`, modified by the patches in `third_party/patches`)
- Only the install, `nx`, `data` and a few `util` sources are compiled (see `makefiles/switch.mk`).
  The original copyright and license headers in those files are kept.
- License files shipped with CyberFoil, in `third_party/CyberFoil/`:

| File | Covers | License | Copyright |
|---|---|---|---|
| `LICENSE` | CyberFoil | GPL-3.0 | CyberFoil authors |
| `awoo.LICENSE` | Awoo Installer, which CyberFoil derives from | GPL-3.0 | Awoo Installer authors |
| `Tinfoil.LICENSE` | Tinfoil, which Awoo Installer derives from | MIT | Copyright (c) 2017-2018 Adubbz |
| `lib.LICENSE` | CyberFoil library code | MIT | Copyright (c) 2026 luketanti |
| `libusbhsfs.LICENSE` | libusbhsfs (USB code, not compiled into CookingFoil) | ISC | Copyright (c) 2020, DarkMatterCore; Copyright (c) 2020, XorTroll |

### SQLite

- Source: https://www.sqlite.org/ amalgamation 3.46.1 (`third_party/sqlite`, unmodified),
  SHA3-256 of `sqlite-amalgamation-3460100.zip`
  af6aae8d3eccc608857c63cf56efbadc70da48b5c719446b353ed88dded1e288
- License: public domain (https://www.sqlite.org/copyright.html)
- Holds the download queue, history and resume points.

### QR Code generator library

- Source: https://github.com/nayuki/QR-Code-generator (submodule `third_party/QR-Code-generator`,
  C++ encoder only, unmodified)
- License: MIT
- Copyright (c) Project Nayuki
- Full text: header of `third_party/QR-Code-generator/cpp/qrcodegen.cpp`

### oc-save-keeper

- Source: https://github.com/jshsakura/oc-save-keeper (commit
  `9aaa871e1ba7562cf08fc6562ef7ca3522c20649`, ported and modified, not a submodule)
- Ported into `include/core/SaveMeta.hpp`, `source/core/SaveMeta.cpp` (backup metadata and sync
  rules), `include/core/Zip.hpp`, `source/core/Zip.cpp` (zip archives) and
  `include/net/DropboxUtil.hpp`, `source/net/DropboxUtil.cpp` (Dropbox helpers), with their tests.
  The devkitPro build setup in `makefiles/switch.mk` is also adapted from it.
- License: MIT
- Copyright (c) 2026 jshsakura

### sphaira (HOME Menu shortcuts)

- Source: https://github.com/ITotalJustice/sphaira (commit
  `338348e74b6d278bc570a9be5373d25197bf6c8d`, ported and modified, not a submodule)
- How sphaira builds and installs a forwarder (`owo.cpp`: RomFS, PFS0, NCA, CNMT, NPDM and
  NACP patching, NCM and NS install; `keys.cpp`: the NCA header key sources) is ported into
  `include/core/Forwarder.hpp`, `source/core/Forwarder.cpp` and `source/platform/Shortcut.cpp`.
- Its homebrew loader `hbl` (`main.c`, `trampoline.s`, `hbl.json`) is in `forwarder/hbl/`,
  unchanged apart from a header comment, and built into `romfs/forwarder/` by
  `makefiles/switch.mk`.
- License: GPL-3.0, by ITotalJustice
- hbl derives from nx-hbloader: ISC, Copyright 2017-2018 nx-hbloader Authors
  (`forwarder/hbl/nx-hbloader.LICENSE.md`)

### Icons

- Some interface icons are redrawn from [Lucide](https://lucide.dev) (see comments in
  `source/ui/Painter.cpp`).
- License: ISC (portions that come from Feather are MIT)
- Copyright (c) Lucide Contributors; portions copyright (c) Cole Bemis as part of Feather
- Full text: https://lucide.dev/license

## Bundled data

### Fonts

All fonts are licensed under the SIL Open Font License 1.1. The license texts are in
`romfs/fonts/` (embedded in the app) and `host/fonts/` (host build).

| Font in the app | Based on | Copyright | License text |
|---|---|---|---|
| CookingFoil Sans | Pretendard 1.3.9, subset | Copyright (c) 2021, Kil Hyung-jin, with Reserved Font Name Pretendard | `OFL.txt` |
| CookingFoil Sans JP | Pretendard JP 1.3.9, subset | Copyright (c) 2021, Kil Hyung-jin, with Reserved Font Name Pretendard JP | `OFL-JP.txt` |
| CookingFoil Sans SC | Noto Sans SC, subset (instanced at 400 and 700) | Copyright 2014-2021 Adobe, with Reserved Font Name 'Source' | `OFL-SC.txt` |
| Baloo 2 ExtraBold (wordmark) | Baloo 2, unmodified | Copyright 2019 The Baloo 2 Project Authors | `OFL-Baloo2.txt` |

The subsets are renamed as the OFL requires for modified versions. They are produced by
`scripts/subset-fonts.py`.

### CA certificate bundle

- File: `romfs/cacert.pem`
- The Mozilla CA certificate bundle as extracted and distributed by curl
  (https://curl.se/docs/caextract.html)
- License: Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/)
- Source and date: see the header of the file and `romfs/README.txt`

## Linked libraries

The console build links these libraries from devkitPro's portlibs. The host build links the
system's SDL2, SDL2_ttf, SDL2_image, json-c, libcurl and, when installed, minizip. Full license
texts are in each project's source distribution.

| Library | License | Copyright | Upstream |
|---|---|---|---|
| libnx | ISC | switchbrew contributors | https://github.com/switchbrew/libnx |
| SDL2 | zlib | Sam Lantinga and contributors | https://www.libsdl.org |
| SDL2_ttf | zlib | Sam Lantinga and contributors | https://github.com/libsdl-org/SDL_ttf |
| SDL2_image | zlib | Sam Lantinga and contributors | https://github.com/libsdl-org/SDL_image |
| libcurl | curl license (MIT/X derivative) | Daniel Stenberg and contributors | https://curl.se |
| Mbed TLS | Apache-2.0 | The Mbed TLS Contributors | https://github.com/Mbed-TLS/mbedtls |
| json-c | MIT | json-c contributors | https://github.com/json-c/json-c |
| FreeType | FreeType License (FTL) | The FreeType Project | https://freetype.org |
| HarfBuzz | MIT ("Old MIT") | HarfBuzz contributors | https://github.com/harfbuzz/harfbuzz |
| libwebp | BSD-3-Clause | Google Inc. | https://chromium.googlesource.com/webm/libwebp |
| libpng | libpng license | Cosmin Truta and the libpng contributors | http://www.libpng.org |
| libjpeg-turbo | IJG License and BSD-3-Clause | The libjpeg-turbo Project and the Independent JPEG Group | https://libjpeg-turbo.org |
| zstd | BSD-3-Clause | Meta Platforms, Inc. and affiliates | https://github.com/facebook/zstd |
| zlib | zlib | Jean-loup Gailly and Mark Adler | https://zlib.net |
| minizip | zlib | Gilles Vollant, Mathias Svensson and contributors | https://github.com/madler/zlib/tree/develop/contrib/minizip |
| bzip2 | bzip2 license (BSD-style) | Julian Seward | https://sourceware.org/bzip2/ |

FreeType asks that binary distributions credit it: portions of this software are copyright
The FreeType Project (www.freetype.org). All rights reserved.

libjpeg-turbo asks that binary distributions credit it: this software is based in part on the
work of the Independent JPEG Group.

## Server

CookingFoil talks to the CookingFoil server (https://github.com/jshsakura/oc-cookingfoil, MIT).
The server is a separate project and is not included in this repository.
