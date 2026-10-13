# Third-party code

## Install engine (CyberFoil)

[CyberFoil](https://github.com/luketanti/CyberFoil) 1.4.5 is pinned at commit
`eaf0353100e03f2772a037eccdec9f680f8a8de0`. `makefiles/switch.mk` compiles only what HTTP
NSP/NSZ/XCI/XCZ installs need: the install sources, `nx`, `data` and a few utilities. The CyberFoil
screens and Plutonium are not used, and the USB and local-file installers are left out. The
original copyright and license headers are kept.

`engine-shim/` replaces the engine's UI and config headers so its calls reach CookingFoil's
install state instead. SDL rendering stays on the app's main thread. The engine's own config
module, which writes files, is not linked.

### Dropped connections and verification

- **A dropped connection is waited out.** When the engine's own quick retries fail, it asks
  whether to keep going. `engine-shim/ui/MainApplication.hpp` answers by waiting in rounds of
  10 seconds that grow to 60 seconds, about ten minutes in all, then gives up. The engine
  resumes at the byte where it stopped. Cancelling ends the wait right away.
- **The install item is retried.** If the item still fails, `source/install/Installer.cpp` tries
  it again from the start, up to 3 attempts in total. The engine skips content it already
  finished, so a retry picks up where it broke.
- **NCA verification is never auto-approved.** Signature checks stay on, and a verification
  failure is always answered with cancel.

### Patches

`third_party/apply-patches.sh` runs automatically as part of `make`. It fingerprints the whole
patch series (`patches/*.patch`). When the fingerprint differs from the one stored in the
submodule, it resets the submodule to the pinned commit and applies the series in order from
the start. When it matches, nothing happens. Patches may touch the same lines, so they are
applied as a series rather than checked one by one.

`patches/0001-cookingfoil-network-and-cleanup.patch`:

- Sends Basic, device approval and Cloudflare headers only to the same origin as the server, and
  does not follow redirects, so auth headers never go elsewhere.
- Verifies the HTTPS certificate and host name using the app's CA bundle in RomFS.
- Reports HTTP progress to the install manager and honors cancellation.
- Tracks new CNMT and NCA content before writing it, so a failed or cancelled install can clean
  up what this session wrote. Content that was already on the console is not tracked.
- Commits the content meta and application records after the content is installed. A failed
  ticket import is not treated as success.
- If install services fail to start, closes only the services that were opened.

`patches/0002-cookingfoil-resume-after-failure.patch`:

- After a failed install, keeps the content that finished downloading (registered content) and
  removes only unfinished placeholders, so a retry resumes instead of starting over.
- A cancel still removes everything the session wrote.

`patches/0003-cookingfoil-faster-install-streaming.patch`:

- The install fetch reads 512 KB at a time instead of curl's 16 KB default, keeps the socket
  alive, and resolves IPv4 only, as the app's other downloads do.
- NSP/NSZ: the download and writer threads sleep a millisecond while they wait instead of
  spinning, and so do the progress loops. The download runs on core 2 and the writer (zstd,
  AES, ncm) on core 1 when the process may use them. The ring's byte counters are atomic.
- XCI/XCZ: one connection streams each NCA on a fetch thread into at most three 4 MB chunks
  while the install thread writes them. Before, it fetched 4 MB, wrote it, and opened a new
  connection for the next 4 MB.
- Every NCA body writer hands each full 8 MB buffer to a card thread of its own and fills the
  next one meanwhile, so zstd and AES run while the card is written instead of between writes.
  At most one buffer waits; a write error comes back on the next flush or at close, and close
  waits for the card.
- Large ranges are fetched like aria2: consecutive chunks on several connections at once
  (`SetParallelDownload`; the app sets 8 x 4 MB as an application, 4 x 2 MB from the album),
  handed on strictly in order. A failed chunk is asked for again from where it stopped, up to
  three times; after that, or on 401/403/404/416, the rest goes on the single stream with its
  own retries. Every range request shares one handle setup.
- Pause: when the install manager is paused (`instPage::isInstallPaused`), every connection is
  dropped and the fetch waits; on resume each piece asks for the rest from the byte it stopped
  at. A pause never spends a retry. Busy (503), rate-limited (429/1015) and tunnel timeouts (524)
  wait 5 seconds before asking again, other failures 1 to 2 seconds.

### Limits

The patches were checked by applying them to the pinned commit in a clean checkout and comparing
the result with the working sources, by re-applying them, and by host and Switch builds. Install
and cleanup results need to be confirmed on a console. A full rollback is not guaranteed after a
power loss, or when a system service fails while the metadata is being committed.

## QR code generation

Project Nayuki's [QR-Code-generator](https://github.com/nayuki/QR-Code-generator) is pinned at
`3c6d0b3cefb4e049dc337e82237c9644399716a8`. It keeps its MIT license and original headers, and
only the C++ encoder is compiled, unmodified. The app draws the modules itself in
`source/ui/Qr.cpp`, with a quiet zone of at least 4 modules, black on white, and an integer
scale. No logo is drawn over the code. The pairing link is never sent to an image service or an
external QR API. This dependency file is not split up to meet the app's 800-line limit for its
own sources.
