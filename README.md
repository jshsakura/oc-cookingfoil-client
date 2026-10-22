<div align="center">
  <h1><img src="brand/cookingfoil-readme-256.png" alt="CookingFoil" width="192" height="192" /></h1>
  <p><strong>Your files on your server. Your library on your console.</strong></p>
  <p>A personal homebrew client for games, homebrew apps, saves and patches.</p>
  <p>
  <a href="https://jshsakura.github.io/oc-cookingfoil/?lang=en"><img src="brand/buttons/website-en.svg" alt="Website" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.md#connecting-a-client"><img src="brand/buttons/connect-en.svg" alt="Connect console" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.md#quick-start"><img src="brand/buttons/setup-en.svg" alt="Setup guide" width="160" height="44" /></a>
  <a href="docs/README.md"><img src="brand/buttons/docs-en.svg" alt="Documentation" width="160" height="44" /></a>
  </p>
  <p><a href="README.md">English</a> · <a href="README.ko.md">한국어</a></p>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml"><img src="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/releases"><img src="https://img.shields.io/github/v/release/jshsakura/oc-cookingfoil-client?color=FFC23D" alt="GitHub release" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-16161A" alt="GPL-3.0-or-later" /></a>
</div>

![CookingFoil home screen](docs/screenshots/en/01-home.png)

Connect to your own [CookingFoil server](https://github.com/jshsakura/oc-cookingfoil), browse your files
and choose what to install. Browse several servers together, compare installed versions with updates,
and pick up downloads that were interrupted.

## Take a look

| Game detail | Downloads |
|---|---|
| ![Game detail](docs/screenshots/en/02-detail.png) | ![Downloads](docs/screenshots/en/04-downloads.png) |
| Installed games | Saves and Dropbox |
| ![Installed games](docs/screenshots/en/07-installed.png) | ![Saves and Dropbox](docs/screenshots/en/08-dropbox-linked.png) |

Captured from the real English UI with sample data. Entries without icons use the app’s native first-letter tiles.

## Choose what you want. Keep the rest in order.

| What you want to do | What CookingFoil handles |
|---|---|
| **Browse and install** | Search, filters, sorting and game details. Choose base, update and DLC files, and SD card or console storage. NSP, NSZ, XCI and XCZ installs use the CyberFoil engine. |
| **Use several servers** | Browse their catalogs together and choose a download server when several offer the same item. |
| **Continue a download** | Keep the queue and completed, failed and canceled records. Supported downloads resume from their saved position, wait for connectivity, and keep the console awake. |
| **Find updates** | Compare installed games with server versions. Homebrew updates use content hashes supplied by the server. |
| **Manage homebrew** | Install and update a single NRO or an app folder. Create HOME Menu shortcuts for supported apps. |
| **Keep saves** | Back up and restore per-user saves locally, import server saves, and upload or download Dropbox backups. A rollback backup is made before a restore. |
| **Apply patches** | Enable and disable game file, cheat, IPS and FPSLocker patches. Check file conflicts and merge cheats per build ID. |
| **Browse retro games** | Pick games by system and download their bundles. Use server-provided RetroArch packs, shortcuts and RetroAchievements information. |
| **Make room** | Manage installed games and confirm leftover cleanup. Cleanup targets content no installed title references. |

Korean, English, Japanese and Chinese; touch and controller input. Homebrew, patches, retro and some
integrations appear according to the server's capabilities. You choose when to apply saves and patches.

## Get started

1. **Prepare a server.** Follow the [CookingFoil server guide](https://github.com/jshsakura/oc-cookingfoil#readme), or use a server you have permission to access.
2. **Copy the client.** Download [`cookingfoil.nro`](https://github.com/jshsakura/oc-cookingfoil-client/releases/latest/download/cookingfoil.nro) from the latest release, or from your server’s web page, and copy it to `switch/cookingfoil/` on your SD card. No config file is needed.
3. **Connect.** Launch the app from the Homebrew Menu and enter the address in **Settings → Add server**. A `config.json` written on a PC works too.
4. **Approve the device.** If the server requires approval, scan the QR code on the console, sign in on your phone and approve it. The console connects when approved.

You need a console that can run homebrew. Use application mode for operations that need console
permissions, including save management. If applet mode blocks access, the app explains how to launch it.

See [Getting started](docs/GETTING-STARTED.md) for configuration, authentication and multiple servers.

## Develop and preview

The C++17 and SDL2 host build runs the real UI on a PC. Explore fixture data or connect a server,
and control the app from your browser.

```sh
git clone --recursive https://github.com/jshsakura/oc-cookingfoil-client
cd oc-cookingfoil-client
# Install the host dependencies from the build guide first.
make test host
./build-host/cookingfoil-host --fixture host/fixtures --lang en --serve 38520
```

Open `http://localhost:38520/`. Console installs, save access and deletion need hardware checks.
See [Building and previewing](docs/BUILDING.md) for dependencies, console builds and screenshots.

## Read more

| Guide | Covers |
|---|---|
| [Getting started](docs/GETTING-STARTED.md) | Installation, config, QR approval, multiple servers and SD card files |
| [Features](docs/FEATURES.md) | Downloads, homebrew, saves, patches, retro and their requirements |
| [Building and previewing](docs/BUILDING.md) | Host and console builds, tests and browser preview |
| [Design](docs/DESIGN.md) | Layout, focus, input and feedback |
| [Writing and translations](docs/WRITING.md) | English translation keys, four languages and writing rules |
| [Working rules](docs/CONTRIBUTING-GUIDE.md) | Threads, safe file writes, tests and pull requests |
| [Third-party code](third_party/README.md) | Install engine and patch maintenance |

## License and content

The client is [GPL-3.0-or-later](LICENSE). The separate server project is MIT-licensed.
The client uses the install engine from [CyberFoil](https://github.com/luketanti/CyberFoil),
which derives from Awoo Installer and Tinfoil. See [Third-party notices](THIRD_PARTY_NOTICES.md).

No games, keys, BIOS files or RetroArch are included. Use content you legally own or have permission
to use, and patches you are allowed to distribute.
