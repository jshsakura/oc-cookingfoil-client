# Writing guide

How text on the screen, in the code and in git history is written.

Each rule carries its source: **(note)** a rule the maintainer stated, **(code: N sites)** a pattern
every site follows, **(history: sha)** a rule a fix established. "Enforced" says what catches a
break; "by convention" means nothing does, so review must.

## Never

1. **Never use Korean, or any non-English text, as a translation key.** The key passed to
   `tr()`/`trf()` is the English source string. Korean, Japanese and Chinese live only in the rows
   of `source/i18n/*.cpp`. Why: the code reads in English and English must not fall back to
   another language. (note) (history: the 0.2.154 conversion)
   Enforced: partly. See [What the i18n test enforces](#what-the-i18n-test-enforces); a key in
   Korean with a matching row would pass.
2. **Never put Korean text in a string literal outside the tables.** Why: it cannot be translated
   and shows Korean to every language. (note)
   Enforced: `tests/test_i18n.cpp:90-114`, unless the line carries a legitimate marker.
3. **Never ship a string without all four languages, or with different `{0}` placeholders.**
   Enforced: `tests/test_i18n.cpp:70-83`.
4. **Never use an em dash or an en dash, or join sentences with any dash**, in UI copy, messages,
   comments, commits or docs, in any language. Why: the maintainer's writing rule; it reads as two
   half-sentences glued together. (note) Enforced: by convention. The tables and sources hold none
   today.
5. **Never copy the Korean 합니다 statement form into the Japanese, Chinese or English row.**
   (note) Enforced: by convention.
6. **Never write a controller key as plain text** ("A 확인"); draw it with `Painter::key`.
   (note) Enforced: by convention.
7. **Never name the server in a message when only one server is set up.** (note)
   Where: gate on `manyServers()`, as in `Detail.cpp:146-151`. Enforced: by convention.
8. **Never put an emoji anywhere**: UI text, translation rows, comments, commit messages, pull
   request text or docs. (note) Enforced: by convention. The repository holds none.

## Where screen text lives

**Screen text goes through `tr("...")` or `trf("...", {args})`; the key is the English text.** When
two keys read the same in English, add `|context` (`"Resume|download"`); English shows the part
before the bar. (note) Where: `include/core/I18n.hpp:7-31`.

**Rows are `{key, ko, ja, zh}` in `source/i18n/Texts*.cpp`; English comes from the key.**
Where: `I18n.hpp:33-39`.

**Pass untranslated keys around and translate where you draw.** A label that code also compares
stays a key until the draw call. Why: logic must not depend on the chosen language.
(code: `Theme.hpp:28-35`, `Application.hpp:786`, `Application.cpp:2239`, `Settings.cpp:73`)

## What the i18n test enforces

`tests/test_i18n.cpp` runs in `make test` and in CI. It fails when:

- a row lacks a key, ko, ja or zh, or a row's `{n}` placeholders differ from its English key's
  (`:70-83`);
- two rows share a key (`:84-89`);
- a string literal in `source/`, `include/` or `engine-shim/` (outside `source/i18n/` and the
  `I18n.*` files) holds Hangul and is not the argument of `tr(` or `trf(` (`:90-114`);
- a literal passed to `tr(`/`trf(`, or a literal on an `i18n: keep` line, has no row (`:115-118`).

What it does not catch: a Korean key that has a matching row; Japanese or Chinese text in a literal
(it looks for Hangul lead bytes only, `:8-14`); text in `host/` or `tests/`; a translation that
reads badly.

The two line markers, and when they are legitimate:

| Marker | Effect | Legitimate when | Sites |
|---|---|---|---|
| `// i18n: as is` | The whole line is skipped | The text is the same in every language, or is data matched against what a server or file says, not shown as UI | 4, e.g. `Application.cpp:3420`, `ShopSaves.cpp:52` |
| `// i18n: keep` | The line's literals must have a row | The key is stored now and passed to `tr()` later | 37, e.g. `Theme.hpp:31-33`, `HomebrewExtras.cpp:14` |

Do not use either marker to silence a string that the user reads.

## Adding a string

1. Write the English key in `tr()`/`trf()` at the call site.
2. Add one row with Korean, Japanese and Chinese to the table for that area.
3. Read the row in all four languages. Each must read as a native speaker would write it. (note)
4. Run `make test`.
5. Look at it in the browser preview in Korean and in the longest language for that string.

## Korean

**Sentences use 합니다체**: descriptions, hints, confirmations, notices and toasts. (note)
Examples from the tables: "새로고침합니다.", "이 게임을 설치하시겠습니까?".

**Labels may stay noun forms**: menu items, buttons, tabs ("파일 정리", "이어서 받기"). (note)

**Natural Korean, not translated Korean**: "이 게임을 설치하시겠습니까?", not
"설치 대기열에 추가합니까?". (note)

## English, Japanese, Chinese

**Each language uses its own form for an instruction**: English the imperative, Japanese
「…してください」, Chinese 「请…」. (note)

**Quote button and setting names the same way every time**: English `"…"`, Japanese `「…」`,
Chinese `“…”`. (note)

## In every language

**Short means quick to read, not fewer words.** Keep the fact that prevents a mistake (what is
lost, what stays) and drop what the user can guess. Do not cut a sentence into a list of nouns.
(note)

**In labels, separate parts with a middle dot**: `"{0} files · {1}"`. (code: the tables)

**While the first answer is on its way, say it is loading, not offline.** (history: 3a57d9a)
Where: `Dialogs.cpp:118-126`.

**Long text shrinks to fit only where the code does it (tabs, menu labels); everywhere else it must
fit as written.** Where: `Dialogs.cpp:65-72`, `Application.cpp:2161-2167`.

## Code comments

Write in English. Name screens by their on-screen Korean names (`// 내 게임's tabs …`).
(code: 362 comment lines) What a comment may say is in
[CONTRIBUTING-GUIDE.md](CONTRIBUTING-GUIDE.md#comments).

## Commits and pull requests

**English, with a conventional prefix**: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`,
`chore`, `ci`, `build`, `revert`. (note) (code: all 473 non-merge commits)

**The subject is one sentence about behaviour as the user sees it**, lower case after the prefix,
no full stop. Name screens as they appear on screen, in Korean. (history: e77e751, 3fa696b;
94 of 508 subjects carry a Korean screen name)

```
fix: a pressed menu row darkens as itself, not as a card over its neighbours
fix: 파일 정리 names the second row 연결이 끊어진 다운로드
perf: a finished download no longer stalls the screen
```

Not "fix: update Touch.cpp" or "fix: pressed state bug".

**The body says what was wrong and why the change fixes it**, in a few plain sentences.
(history: 7e215d2, de8ac79, 17c14e6)

**A release bump is its own commit, `chore: release X.Y.Z`.**
(history: 433fba7)

**Release titles, notes and tag messages are in English.** (note)
