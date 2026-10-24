# Working rules

How code is written and shipped in this repository. Read [DESIGN.md](DESIGN.md) for how things look
and answer input, and [WRITING.md](WRITING.md) for text, translations and commit messages.

Each rule carries its source: **(note)** a rule the maintainer stated, **(code: N sites)** a pattern
every site follows, **(history: sha)** a rule a fix established. "Enforced" says what catches a
break; "by convention" means nothing does, so review must.

## Never

1. **Never do SD card I/O or wait on a lock in a draw or press handler.** No `stat`, file read or
   write, sqlite call, free-space query, or `lock_guard` on a mutex a worker holds while it writes.
   Why: the console's SD card answers slowly while a download writes, and a frame that waits for it
   freezes the screen. (note) (history: 0ab848d, d05222b, 7e215d2, de8ac79, 17c14e6)
   Enforced: by convention.
2. **Never put personal data in code, tests, fixtures, comments, commits or the build.** No real
   IPs, host names, domains, emails, account names, keys, tokens or machine paths. Use
   `192.0.2.0/24`, `198.51.100.0/24`, `203.0.113.0/24`, `example.com`, and keys that are plainly
   fake. (note) (history: 6db0cf2)
   Enforced: by convention in the repository. The maintainer's machine scans pushes and builds
   outside it.
3. **Never ship a server, or import another app's server settings.** A new `config.json` starts
   with no servers. Why: an imported shop showed up on the maintainer's console as if the app
   handed out their settings. (note) Where: `Config.cpp:195-214`. Enforced: by convention.
4. **Never commit binaries** (`.nro`, `.nacp`, `.elf`, signatures). (note)
   Enforced: partly, `.gitignore` lists the build outputs.
5. **Never push to `main`.** Every change lands through a pull request from its own branch.
   (note) (history: every first-parent commit on `main` since 179c56e, #7, is a pull request)
   Enforced: by convention.
6. **Never add download speed measurement or timing logs.** Improve speed by reading the code.
   Why: logging became the bottleneck, and measuring on the device was ended by the maintainer.
   (note) (history: 0bbec4b) Enforced: by convention.
7. **Never add a speculative network workaround.** Change connection code only with evidence from
   a console, one change at a time, and first check the server and another client (CyberFoil).
   Why: a night of client changes chased a fault in a home router's DNS. (note)
   (history: d35f9b8) Enforced: by convention.
8. **Never fall back from the server's named direct download address to its public address.** A
   failure there is the download's failure. Why: the public route may be metered; the server's
   admin switch alone decides. (note) Where: `include/net/Direct.hpp:10-11`.
   Enforced: `tests/test_direct.cpp:21`, `tests/test_direct.cpp:185`.
9. **Never hand-roll a substitute for the standard tool.** Records live in sqlite, downloads resume
   with real ranges, not JSON files and restarts. (note) (history: b6d3264)
   Enforced: by convention.
10. **Never compute or patch server metadata on the client.** Sizes, order, hashes and names come
    from the server; show them and ask again (ETag). A wrong value is a server fix. (note)
    Enforced: by convention.
11. **Never write a file in place where a crash would leave half of it.** (code: 36 + 3 sites)
    (history: 8b453bd) Enforced: by convention. See [Files on the card](#files-on-the-card).
12. **Never touch the card with a server-given path or id before its allow check.** (code: 26
    sites) (history: 08cf85b) Enforced: the placement tests. See
    [Paths from the server](#paths-from-the-server).
13. **Never write a log line except through `logLine`.** (code: 1 writer) Where: `Log.cpp:231-254`.
    Enforced: by convention.

## The drawing thread

**Only the main thread draws and calls SDL.** Workers write their result into a member and raise an
atomic flag; `poll()` joins them and applies the result. (code: 34 worker threads, 20 `Done_`
flags) Where: `Application.cpp:3276-3318` is the shape to copy.

**Run a worker's body in `guarded("name", ...)`.** An exception becomes a logged error, not a
crash. (code: 31 sites) Where: `include/core/Log.hpp:32`.

**Hand the UI a copy, a queue or an atomic, never a lock it must wait for.**
(history: 7e215d2, de8ac79, 17c14e6) Where: the log queue `Log.cpp:57-77`, cue requests
`Sounds.hpp:50-56`, the records writer `Application.cpp:3330-3360`.

**Pin heavy work off the drawing core with `platform::OnCore`.** Card writes, installs, zstd and AES
go on `WriterCore`; curl and TLS on `NetworkCore`. Why: libnx starts every thread on the drawing
core, so a download shared core 0 with every frame. (note) (history: 3b7c238)
(code: 12 sites `WriterCore`, 5 sites `NetworkCore`, every request in `Api.cpp:387`)
Where: `include/platform/Cores.hpp:1-9`.

**After anything writes to or removes from the card, call `cardChanged()`.** Free space and file
checks are read again on the worker. (note) Where: `Application.cpp:3319-3323`.

**Do not make startup slower.** Put new work after startup or on the loading thread, and never
put an SD read in front of the start cue. Why: startup was brought from about 5 s to under 2 s and
the cue slipped a beat once SD reads came first. (note) Where: `Application.cpp:224-256`.

## Files on the card

**Recursive deletion goes through `platform::removePath`.** Switch's directory-open errors do
not match the POSIX assumptions of `std::filesystem::remove_all`; a file can survive a reported
successful removal, and client-side directory traversal can retain handles during deletion.
The platform adapter dispatches by native entry type, deletes the tree through FS, and checks
that the name is gone. Preparation errors must stop the operation; cleanup errors must be
reported. Enforced by `tests/check_filesystem_policy.py` in `make test`.

**Replacement failures retain a fully written temporary copy.** Removing a destination and
renaming its replacement are separate operations on Switch. If the latter fails, deleting the
temporary copy loses both versions. Native service fault tests cover that boundary, existing
targets, missing sources, and a successful delete response with a surviving name.

**Resume compatibility needs an end-to-end commit check.** Old `.resume.old` records may be
read by the legacy fallback. On successful download, remove recognized resume metadata before
patch payload validation; test both old-only records and old/current records together.

**Whole small files go through `atomicWrite`.** It writes a temp file, syncs, renames, works
around the card refusing to rename over a file, and skips the write when nothing changed.
(code: 36 sites) (history: cc617d6) Where: `source/core/Json.cpp:117-146`.

**Downloads go to a `.part` file and are renamed when complete.** (code: 3 sites) Where:
`HomebrewApi.cpp:86`, `RetroApi.cpp:140`, `ClientUpdateApi.cpp:77`.

**Persistent records go in the sqlite store, not a new JSON file.** (note) (history: b6d3264)
Where: `include/core/DownloadStore.hpp`.

**Every cache key changes when the content changes.** Size and mtime alone are not enough: use the
full URL with its `?v=`, a sha256, or ctime. Never build a shortcut from cached bytes. (note)
(history: 25e4d2c) Where: `Images.cpp:432-433`, `Shortcuts.cpp:242-245`.

## Paths from the server

**Check every server-given path or id against its allow function before it reaches the card.**
(code: 26 call sites of `pathAllowed`, `homebrewPathAllowed`, `appFolderAllowed`,
`packPathAllowed`, `bundlePathAllowed`, `patchPathAllowed`; plus `isSafeArchivePath` and
`TitleGuard`) Where: `include/core/Patches.hpp:47-49`, `include/core/Retro.hpp:129-132`,
`include/core/TitleGuard.hpp:17-23`, `include/core/Zip.hpp:15`.

**Any change to where files land ships with tests that pin it:** the right path, nothing outside
the allowed folder, old files removed only after success. Why: a wrong placement is only seen on
the console, too late. (note) Where: `tests/test_homebrew.cpp`, `tests/test_retro.cpp`,
`tests/test_patches.cpp`, `tests/test_forwarder.cpp`, `tests/test_client_update.cpp`.

## Servers

**Assume several servers.** Never reach for `session_` as "the" server; a per-item record keeps the
server it came from. (note) (code: a `server` field in `Catalog.hpp:33`, `Homebrew.hpp:91`,
`Patches.hpp:17`, `ShopSaves.hpp:22`, `Selection.hpp:44`, `State.hpp:92`, `Types.hpp:43`)

## Secrets and logs

**Log through `logLine`.** Every line loses URL hosts and queries and anything after a credential
name; a bug report also loses the known secret values and host names. (code: `Log.cpp:126-182`,
`Log.cpp:205-221`) (history: 4fd984c, cf68129, 03c0693)

**Auth headers go only to the configured server's own origin; the download host gets the download
token alone.** Where: `include/net/Direct.hpp:21-23`.

## Comments

The rule is pstack's no-comments standard, not the density the code has today. A comment stays only
when it states a why the code cannot show: a console, SD card or library quirk, a decision with its
reason, or a trap. Delete narration of what the next line does, section labels, restated names and
filler. When a comment explains a surprise in our own code, change the code (a name, a type, an
extracted function) so the surprise goes away, then delete the comment. Do not shorten a long
justification into a smaller one.

Delete. `source/ui/Painter.cpp:395-399` says the same thing twice and hides magic numbers:

```cpp
// Glyph-like marks read better a touch heavier than the menu icons.
// Marks read a touch heavier than outline icons.
const char *iconStroke(int kind) {
  return kind == 8 ? "3.2" : kind == 7 || kind == 9 ? "2.6" : "2.2";
}
```

After, once the kinds are named (the names below do not exist yet; kinds 7, 8 and 9 are the back
chevron, the check mark and the forward chevron), no comment is needed:

```cpp
const char *iconStroke(int kind) {
  return kind == IconCheck ? "3.2" : kind == IconBack || kind == IconForward ? "2.6" : "2.2";
}
```

Keep. `source/core/Json.cpp:136-138` explains a platform quirk no name can carry:

```cpp
// The console's SD card refuses to rename over an existing file
// (EEXIST). Move the old file aside, rename, then drop the old one,
// so a power cut leaves either file readable rather than neither.
```

## Build, test, preview

```sh
make test     # unit tests on the host; must pass before a pull request
make host     # the host binary for the preview
make          # cookingfoil.nro (Docker devkitpro/devkita64 when DEVKITPRO is unset)
```

**Add a new test file's `run*Tests()` to the sum in `tests/test_main.cpp`.** The runner calls each
one by name. (code: every test file)

**Check every UI change in the browser preview before it reaches a console.** Why: installing on
the console for each look is slow; the preview runs the same UI code docked. (note)

```sh
./build-host/cookingfoil-host --fixture host/fixtures --serve <port>
```

Drive it with `/key?k=`, `/tap?x=&y=`, `/drag*` and read `/frame.png` (`host/WebView.cpp:237-270`).
Start it only for the check and stop it after, by PID. It listens on all interfaces without auth.
Install, delete and cleanup need a console.

**Debug a console-only bug from the log the app posts to the server**, not from photos of the
screen. Ask for a retry, then read the log. (note)

## Shipping

**A release is a tag.** Raise `APP_VERSION` in `makefiles/switch.mk` in its own
`chore: release X.Y.Z` commit on a `chore/release-X.Y.Z` branch, merge it through a pull request,
then push the tag `vX.Y.Z` on that merge. The release workflow builds the `.nro` from the tag and
refuses a tag that does not match `APP_VERSION`. (history: 433fba7, the 0.4.3 release)
Where: `.github/workflows/release.yml`.

**Servers take a release from GitHub; nobody else builds to ship.** A server that follows the
repository picks up the new release by itself; one set to manual takes a `.nro` uploaded on its
admin page.

## Commits and pull requests

**One behaviour per commit, one topic per pull request, each on its own branch** (`fix/…`,
`feat/…`, `perf/…`, `chore/release-X.Y.Z`). (history: PRs #7 to #44)

**CI must be green**: `make test`, the host screenshots and the console build
(`.github/workflows/ci.yml`).

Message style is in [WRITING.md](WRITING.md#commits-and-pull-requests).
