# Design guide

How the app looks, moves and answers input. When you add a screen, copy the nearest existing one
instead of inventing a pattern, and check it in the browser preview before a console.

Each rule carries its source: **(note)** a rule the maintainer stated, **(code: N sites)** a pattern
every site follows, **(history: sha)** a rule a fix established.

## Never

1. **Never add a handheld layout.** Lay out for docked 1920x1080 only. (note)
2. **Never write a controller key as plain text.** Draw it with `Painter::key`. (note)
3. **Never let a pressed look spill past what was touched.** A button darkens as itself.
   (history: 4125819, 179c56e, f5637fa, e77e751)
4. **Never let anything shift when data arrives.** Placeholders and header buttons take their final
   size from the first frame. (note) (history: 94402e1, 15bd96b)
5. **Never answer a touch under a popup.** (history: f14a31c) (code: 9 `blockTouch()` sites)
6. **Never show a server's name or dot with only one server set up.** (note)
7. **Never use a character as an icon.** No emoji and no symbol glyphs (check marks, stars, arrows
   standing in for a picture). Draw it with `Painter::icon` from the stroke set in `IconPaths`.
   Why: the bundled fonts lack these glyphs, and a glyph never matches the stroke icons. (note)
   (code: `Painter.cpp:341-342`, `Downloads.cpp:320`) Enforced: by convention.

## Canvas

**Lay out in 1280x720 units; the renderer scales to the docked output.** (code: `Theme.hpp:6`,
`outputScale_` in `Application.hpp:1370`)

**Draw rounded shapes with `Painter::rect(…, radius)` and icons with `Painter::icon`.** Both
rasterize at physical pixels, so corners never step and icons never blur. (history: 1c5b51c) Where: `Painter.cpp:10-90`, `Painter.cpp:418-451`.

| Fixed region | Value | Where |
|---|---|---|
| Menu column | x 0 to 232; rows x 16 to 216, 52 high, 58 pitch from y 92 | `Theme.hpp:6`, `Application.hpp:62` |
| Content column | x 260 to 1252 | `Dialogs.cpp:106`, `Application.cpp:2282-2292` |
| Header band | items y 24, 48 high, centre y 48 | `Dialogs.cpp:78`, `Library.cpp:7-8` |
| Footer | y 660, 60 high; hints end at x 1248, centre y 691 | `Application.cpp:2061-2105` |
| Detail top line | y 30, chips end at x 1248 | `Theme.hpp:7-9` |
| Card grid | 150 square, 170 x 240 pitch, six per row | `Painter.cpp:129-136` |

## Colour

**Semantic colours come from `Theme.hpp`**: Ground the page, Surface every card and bar, Ink text
and strong elements, Secondary quiet text, Line rules, Butter brand and focus. A one-off shade is a
named `constexpr` beside its use. (code: `Theme.hpp:10-17`, `Painter.cpp:122`, `Painter.cpp:462`)

**State colours (Good, Pending, Bad) are picked from the untranslated key.** (code:
`Theme.hpp:28-35`)

**Server dots use `ServerColors`, kept apart from the state colours.** (code: `Theme.hpp:18-27`)

**Show server names and dots only behind `manyServers()`.** (note) (code: `Detail.cpp:146-151`,
`Application.cpp:2024-2025`)

## Focus

**Butter marks the cursor, one mark at a time.** Pick the helper by space around the target:

| Helper | When | Where |
|---|---|---|
| `ring` | Room around the target | `Painter.cpp:171-174` |
| `outline` | Packed rows a ring would overlap | `Painter.cpp:163-165` |
| `focusRow` | A row inside a panel | `Painter.cpp:166-170` |
| one ring round the strip | Tabs | `Dialogs.cpp:75-77` |
| `scrollbar(…, active)` | A text area with the cursor | `Painter.hpp:24-29` |

(code: the helpers above)

**A button's A badge shows only while the cursor is on its page.** (code: `Dialogs.cpp:155-174`)

## Touch and pressed look

**Give `hit` the drawn shape (`look`) and radius; the overlay is that shape at black alpha 36.**
Only a target with no shape of its own (a cover) gets the default card 8px larger.
(history: 7991527 then 4125819, 179c56e, f5637fa, e77e751) Where: `Application.hpp:1267-1272`,
`Application.cpp:2556-2572`.

**Small buttons use `buttonHit`: 4px more touch above and below, pressed look on the drawing.**
(code: 13 sites) Where: `Touch.cpp:18-21`.

**The pressed look lingers 120 ms after lift and drops when the screen or popup state changes; a
drag presses nothing.** (history: f14a31c) Where: `Touch.cpp:147-149`, `Application.cpp:2557-2563`,
`Glide.cpp:65`.

**What was touched takes the cursor.** (history: a5098aa) Where: `Touch.cpp:22-32`,
`Touch.cpp:154-155`, `Application.cpp:2146-2155`.

**A tap goes to what the last frame drew.** (history: 7f9f5d5) Where: `Touch.cpp:156-166`.

**What touch reaches, the pad reaches.** Register header items with `headerItem`; every footer and
dialog hint is tappable and runs its key's action. (code: 12 `headerItem` sites) Where:
`Touch.cpp:33-107`, `Touch.cpp:185-198`, `Dialogs.cpp:21-30`.

**Inside a scrolling area only the visible part answers; the touch that stops a coasting list
opens nothing.** (code: `Touch.cpp:6-15`, `Glide.cpp:37-51`)

**A one-decision popup takes A only after 250 ms.** Longer swallowed deliberate presses.
(code: `Application.hpp:1568-1573`)

## Key badges and hints

**Every key on screen is `key`, `keyLabel` or `keyChip`: an ink circle, the key centred.** (note)
(code: 32 sites) Where: `Painter.hpp:53-68`, `Painter.cpp:452-503`.

**A disabled action greys its badge and label instead of hiding.** (code: `Painter.cpp:462`,
`Painter.cpp:477`)

**Offer only what can happen now.** (code: `Application.cpp:2525`) "-" and "+" join the footer only
when they fit. (code: `Application.cpp:2078-2091`)

**Dialog hints read A, X, Y, B; A names the decision when there is one.** (code: `Dialogs.cpp:5-31`)

## Header

**Three items read left to right: the filter or Y button, the tabs, the search.** Search sits at
`SearchX`; anchor the tabs at `SearchX - 16` and the button at `tabsLeft - 12 - width`. When the
button hides, no gap opens. (note) (history: 3a57d9a) Where: `Application.hpp:658-660`,
`HomebrewScreen.cpp:797-805`, `InstalledScreen.cpp:190-191`.

**Header buttons are as wide as their widest label and are drawn from the first frame.** A button
that exists only with some data sits left of the fixed ones. (note) (code:
`UpdatesScreen.cpp:49-56`, `RetroHome.cpp:26-29`, `RetroAchievements.cpp:242-243`)

**Tab and menu labels shrink to 14pt and 16pt before they overflow.** (code: `Dialogs.cpp:65-72`,
`Application.cpp:2161-2167`)

## Loading

**While data loads, draw `skeleton` blocks of the final size in the final place.** Menu, footer
hints and header buttons included. (note) (history: 94402e1, b141d07, 72dfd14, 15bd96b)
(code: 40 sites) Where: `Painter.cpp:118-136`, `Application.cpp:2277-2292`.

**Skeletons breathe in the shades of their backdrop.** (history: 94094ec) Where:
`Painter.cpp:118-128`.

## Notices, toasts, modals

**Empty, error and waiting states use `notice()`; short confirmations use `message()`; a decision
or a failure uses `modal()`.** (code) Where: `Dialogs.cpp:127-189`, `Application.cpp:775-779`,
`Dialogs.cpp:32-53`.

**A toast rises in, never moves when its text changes, and goes after 7 s.** (code:
`Application.cpp:2534-2554`, `Application.hpp:1470`)

## Sounds

**Cues are made in code, never loaded from files.** (code: `include/core/Chime.hpp`)

**Each cue has one meaning.** Move: the cursor or a picker steps. Add: something is queued. Done:
a job ends well. Shortcut: a quieter Done. Error: a job fails or an alert appears, once.
(code: `Application.cpp:1109`, `:1119`, `:1445`, `:2297`, `:2732`, `:2851`, `:2990`,
`RetroScreen.cpp:482-483`)

**The drawing thread only leaves a request for a cue.** (history: de8ac79) Where:
`Sounds.hpp:50-56`.

**Repeats are rate-limited and voices capped, so mashed keys never stack.** (history: 9308ed0)
Where: `Sounds.cpp:8-12`.

**The start cue rings on every landing shown, is scheduled before any SD read, and keeps its events
on whole seconds.** (note) Where: `Painter.cpp:292-297`, `Application.cpp:253-256`,
`Sounds.cpp:88-94`.

## Motion

**Scrolling areas register with `glide()` and move like a phone's list; buttons bring the view
back to the cursor.** (code: `Glide.cpp:5-36`, `Application.hpp:1316-1365`)

**Things ease in; nothing pops.** (code: toast `Application.cpp:2544-2547`, scroll hint
`Painter.cpp:142-161`) (history: f775c23)
