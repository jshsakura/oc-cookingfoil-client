# Building and previewing

[English](BUILDING.md) · [한국어](BUILDING.ko.md)

[All guides](README.md) · [Working rules](CONTRIBUTING-GUIDE.md) · [Third-party code](../third_party/README.md)

CookingFoil uses C++17 and SDL2. The host build and console build share the interface and core
logic; console services have host stubs.

## Clone

```sh
git clone --recursive https://github.com/jshsakura/oc-cookingfoil-client
cd oc-cookingfoil-client
```

For an existing clone, run `git submodule update --init --recursive`.

## Host dependencies

On Ubuntu, install the development packages used by the host build, including minizip for
archive support:

```sh
sudo apt-get install build-essential pkg-config libsdl2-dev libsdl2-ttf-dev \
  libsdl2-image-dev libjson-c-dev libcurl4-openssl-dev libssl-dev zlib1g-dev \
  libjpeg-dev libminizip-dev fonts-noto-cjk ripgrep
make test host
```

The build includes the vendored SQLite amalgamation and zstd decoder. If minizip is missing,
the host build warns and excludes ZIP support; install it for the complete host build.

## Browser preview

```sh
./build-host/cookingfoil-host --fixture host/fixtures --lang en --serve 38520
```

Open `http://localhost:38520/`. The preview renders the same UI at docked 1920×1080 and accepts
keyboard, button, mouse and drag input. Use `--lang ko`, `ja` or `zh` to check another language.

For the app's normal multi-server configuration flow:

```sh
./build-host/cookingfoil-host --app-config path/to/config.json \
  --state out/preview/cookingfoil.db --serve 38520
```

`--config path/to/config.json` is a different host mode: it loads the first enabled server into
a preview session. Use `--app-config` when checking the app's own configuration and multiple servers.

The preview listens on all interfaces without authentication. Run it on a trusted network and
stop the process when finished. Installs, save access, deletion and cleanup need a real console.

## Screenshots

```sh
./build-host/cookingfoil-host --fixture host/fixtures --lang en --shot out/screens-en
```

This writes PNGs for the main screens and simulated states. General test fixtures include sample artwork.
Published screenshots use English in every documentation and website language. Omit artwork URLs
from sample metadata when showing missing icons, so the actual UI draws its native first-letter
tiles. Do not substitute the fixture illustrations for missing game icons.
Only copy reviewed screenshots into `docs/screenshots`; keep generated data and private server
configuration out of Git. [CI](../.github/workflows/ci.yml) also uploads host screenshots.

## Console build

With Docker available and `DEVKITPRO` unset:

```sh
make
```

The Makefile uses `devkitpro/devkita64`, applies `third_party/patches` to the pinned CyberFoil
engine and builds `cookingfoil.nro`. The first build also downloads and builds the pinned curl
and mbedTLS sources through `scripts/build-tls.sh`; allow time and network access for this step.

For a native [devkitPro](https://devkitpro.org/wiki/Getting_Started) environment, install the
Switch portlibs and run the same command with `DEVKITPRO` set:

```sh
dkp-pacman -S switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-curl switch-mbedtls \
  switch-json-c switch-freetype switch-harfbuzz switch-libwebp switch-libpng \
  switch-libjpeg-turbo switch-zlib switch-bzip2 switch-minizip
make
```

The version comes from `APP_VERSION` in `makefiles/switch.mk`; `make version` prints it.
Release tags must match it. Follow the [release rules](CONTRIBUTING-GUIDE.md#shipping) and land
changes through a pull request. Build outputs are release artifacts and stay out of Git.

## Before a pull request

Run `make test`, and check interface changes in the browser before the console. Add meaningful
tests for behavior changes, especially file placement and removal. Read the [design](DESIGN.md),
[writing](WRITING.md) and [working](CONTRIBUTING-GUIDE.md) rules before changing shared patterns.
