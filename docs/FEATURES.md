# Feature guide

[English](FEATURES.md) · [한국어](FEATURES.ko.md)

[All guides](README.md) · [Getting started](GETTING-STARTED.md)

CookingFoil connects your console to files you keep on your own server. The server supplies
metadata and available capabilities. The client handles selection, local state and installation.

## Browse and install

Home shelves, search, filters and sort orders help you find files. A game detail page groups
base, update and DLC entries so you can choose what to install and whether it goes to the SD
card or console storage. The CyberFoil engine installs NSP, NSZ, XCI and XCZ over HTTP.

With multiple servers enabled, catalogs are brought together. Each item retains its source
server, and a source picker is available when several servers offer it. Installed version
checks help the Updates screen show what is newer on the server.

## Downloads and recovery

The queue and completed, failed and canceled records persist in `cookingfoil.db`. Supported
installs retain resume data, including range positions, rather than starting every file over.
After an interruption, the app can offer to resume on launch. Network recovery and retry run
off the drawing thread, and active downloads keep the console awake.

Resume depends on the download type and the server still offering the matching content. A
server change or an unavailable file can require another attempt. The client never silently
falls back from a server's designated direct download address to its public route.

## Homebrew

When a server advertises homebrew, the app lists its NROs and folder installs. It compares the
server's content hash with its local ledger to show installed and updated builds. Folder
installs compare the supplied file hashes and fetch the files that differ. A completed single
file download replaces the existing file only after it has been received and checked.

Supported entries can create HOME Menu shortcuts. Removal follows the recorded file list so
unrelated files on the card are left alone. The app installs what the server offers; it does
not independently subscribe to GitHub releases. Repository tracking belongs to the server.

## Saves

The Saves screen separates console saves, server saves and Dropbox backups. You can back up
per-user saves, restore a local backup, import a server save, or upload and download Dropbox
revisions. Operations that access console saves need the appropriate homebrew permissions.

Local backups use `sdmc:/switch/oc-save-keeper/` and the save-keeper layout and metadata.
The normal retention limit is five backups per game. Rollback copies made before a restore
are counted separately so they do not push out user backups. Removed backups go to the shared
trash layout. Restore checks distinguish account saves from shared device saves.

Dropbox sign-in uses a QR flow through a compatible server bridge. The console exchanges the
authorization result with Dropbox and transfers backups directly. Connecting Dropbox does
not automatically apply a remote save. You choose the backup and confirm the operation.
Sharing Dropbox backups with save-keeper requires both bridges to use the same Dropbox app.

## Patches

Download, enable, disable or remove server-provided game patches. Supported paths cover
LayeredFS `romfs` and `exefs`, cheats, IPS `exefs_patches`, and FPSLocker patches. FPSLocker
patches still need FPSLocker installed.

The placement ledger records the files the app owns. Patches that would place conflicting
files are identified before enabling them. Cheats are merged per build ID with the user's
original cheats kept first; disabling all of them restores the original cheat file. Planned
file moves are recorded before execution so a later launch can finish an interrupted move.
Restart the game for a patch change to take effect.

## Retro library

A compatible server can offer system lists, searchable game catalogs, game variants and
download bundles. The client can receive the server's RetroArch pack, game files and HOME
Menu shortcuts, and show RetroAchievements information when supplied and configured.

This repository includes no ROMs, BIOS files or RetroArch. Prepare your own permitted content
and the server-side packs needed by the features you enable.

## Connect to PC (MTP)

Opens the SD card on a PC over a USB cable so you can browse and move files. Select
**Connect to PC via MTP** in Settings, then plug in the cable. Press B or unplug the cable to
disconnect. Sensitive folders such as Nintendo and emuMMC cannot be changed. Applet mode (started
from the Album) does not have enough memory to connect, so start the app from a HOME Menu shortcut.

When the connection closes, the app deletes unfinished copies and the `._*` and `.DS_Store`
files a Mac leaves behind.

- **Windows**: the console appears as a device in File Explorer. Nothing else to install.
- **Mac**: macOS has no MTP file manager, so use [OpenMTP](https://openmtp.ganeshrvel.com/)
  with Kalam mode turned on in its settings.

**If a Mac does not connect**: while Preview, Photos or Image Capture is open, macOS claims the
console as a camera and does not let go, so OpenMTP cannot reach it. Quit all three apps and plug
the cable in again. Quitting the `ptpcamerad` process does not help because it restarts at once.

## Maintenance

The installed game manager handles games and their saves. Leftover cleanup asks for confirmation
before removing content that no installed title references and app-created placeholders left
by unfinished installs. Destructive confirmations default to Cancel.

Settings also supports updating the client from a build offered by a compatible server.
The downloaded update is checked and staged before the app replaces its executable on launch.
An available update does not mean it has already been installed.

## Input and languages

Touch and controller input share the same interface. The app uses Korean, English, Japanese
and Chinese and keeps the selected language across launches. The host preview uses this same
UI with fixture data or a real server; hardware-only operations are stubbed on the host.
