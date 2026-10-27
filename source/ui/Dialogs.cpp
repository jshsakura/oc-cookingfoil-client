#include "ui/Application.hpp"
#include "install/Installer.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
namespace foil::ui {
// Key hints ending at `right`, like the screen footer; each one is tappable.
void Application::dialogHints(float right, float y, const std::string &close,
                              const std::string &apply, const std::string &accept, const std::string &remove) {
  // `accept` names what A does when the popup is one decision (다운로드);
  // NoAccept leaves A out, for a popup where only B does anything.
  std::vector<std::pair<std::string, std::string>> keys;
  if (accept != NoAccept)
    keys.push_back({"A", accept.empty() ? std::string(tr("Select")) : accept});
  if (!remove.empty())
    keys.push_back({"X", remove});
  if (!apply.empty())
    keys.push_back({"Y", apply});
  keys.push_back({"B", close});
  float width = 0;
  for (const auto &[key, label] : keys)
    width += paint_.keyLabelWidth(key, label, 18) + 22;
  float x = right - width + 22;
  for (const auto &[key, label] : keys) {
    const float w = paint_.keyLabelWidth(key, label, 18);
    // Pressed, a pill just round the key and its label, inside the footer band.
    hit(x - 6, y - 22, w + 12, 44, [this, key = key] {
      action(key == "A" ? Action::Accept : key == "Y" ? Action::Queue : key == "X" ? Action::Search : Action::Back);
    }, {x - 8, y - 20, w + 16, 40}, 20);
    const bool strong = key == "Y" || (key == "A" && !accept.empty());
    paint_.keyLabel(key, label, x, y, 18, strong ? Ink : Secondary, strong);
    x += w + 22;
  }
}
// Modal card with a header (title) and a footer (hints) set apart from the
// body by rules; the footer has its own tint. Returns the body rectangle.
SDL_FRect Application::modal(float width, float bodyHeight, const std::string &title,
                             const std::string &close, const std::string &apply, const std::string &accept,
                             int icon, const std::string &remove) {
  constexpr float Header = 60, Footer = 60; // the same band above and below
  const float height = Header + bodyHeight + Footer;
  const float left = (1280 - width) / 2, top = (720 - height) / 2;
  paint_.rect(left, top, width, height, Surface, 20);
  // An icon ahead of the title says what kind of popup this is.
  const float titleX = left + 32 + (icon >= 0 ? 34 : 0);
  if (icon >= 0) paint_.icon(icon, left + 32, top + Header / 2 - 12, Ink, 24);
  paint_.fonts.middle(title, titleX, top + Header / 2, 22, Ink, true, left + width - 32 - titleX);
  paint_.rect(left, top + Header - 1, width, 1, Line);
  const float footerTop = top + height - Footer;
  paint_.rect(left, footerTop, width, Footer, Ground, 20);
  paint_.rect(left, footerTop, width, 20, Ground);
  paint_.rect(left, footerTop, width, 1, Line);
  if (!close.empty()) // a progress modal has nothing to press
    dialogHints(left + width - 32, footerTop + Footer / 2, close, apply, accept, remove);
  return {left, top + Header, width, bodyHeight};
}
// Tabs at the top right, all in view: L and R badges at the ends step
// between them, a tap picks one. Returns the left edge.
float Application::headerTabs(const std::vector<std::string> &labels, int current,
                              const std::function<void(int)> &pick, float right, float leftLimit,
                              const std::vector<int> &counts) {
  const auto count = [&](int n) { return n < int(counts.size()) ? counts[n] : 0; };
  const auto badge = [&](int n) { return count(n) > 99 ? std::string("99+") : std::to_string(count(n)); };
  // A count above 0 rides after its label in a small grey label.
  const auto badgeWidth = [&](int n) {
    return count(n) > 0 ? std::max(20.f, paint_.fonts.measure(badge(n), 13, true) + 12) + 6 : 0.f;
  };
  // Longer languages step the labels down until the strip fits its room.
  int size = 18;
  float pad = 28, width = 0;
  for (;; --size, pad = 20) {
    width = paint_.keyWidth("L") + paint_.keyWidth("R") + 40;
    for (int n = 0; n < int(labels.size()); ++n) width += paint_.fonts.measure(labels[n], size, true) + pad + badgeWidth(n);
    if (size <= 14 || right - width >= leftLimit) break;
  }
  const float left = right - width;
  float x = left;
  // The tabs take the d-pad as one strip: the ring goes round all of them
  // and the one under the cursor is the one shown.
  if (headerFocus_.rfind("tab", 0) == 0) paint_.ring(left, 24, width, 48, 12);
  paint_.rect(x, 24, width, 48, Surface, 12);
  paint_.key("L", x + 12, 48);
  x += 12 + paint_.keyWidth("L") + 8;
  for (int n = 0; n < int(labels.size()); ++n) {
    const float textW = paint_.fonts.measure(labels[n], size, true), w = textW + pad + badgeWidth(n);
    if (n == current) paint_.rect(x, 30, w, 36, Ink, 9);
    paint_.fonts.middle(labels[n], x + pad / 2, 48, size, n == current ? Surface : Secondary, true);
    if (count(n) > 0) {
      // A grey label like the title's count; on the dark chosen tab, a light one.
      const float bw = badgeWidth(n) - 6, bx = x + pad / 2 + textW + 6;
      const bool chosen = n == current;
      paint_.rect(bx, 37, bw, 22, chosen ? SDL_Color{255, 255, 255, 51} : Line, 6);
      paint_.fonts.middle(badge(n), bx + (bw - paint_.fonts.measure(badge(n), 13, true)) / 2, 48, 13,
                          chosen ? Surface : Ink, true);
    }
    const auto run = [pick, n] { pick(n); };
    // Pressed, a tab darkens as the chosen one is drawn, not the whole strip's height.
    headerHit("tab" + std::to_string(n), x, 24, w, 48, run, {x, 30, w, 36}, 9);
    header_.push_back({"tab" + std::to_string(n), {x, 24, w, 48}, run, true});
    if (n == current) headerHome_ = "tab" + std::to_string(n);
    if (n == current && headerFocus_.rfind("tab", 0) == 0) headerFocus_ = "tab" + std::to_string(n);
    x += w;
  }
  paint_.key("R", x + 8, 48);
  return left;
}
float Application::pageTitle(const std::string &title, const std::string &meta, float metaRight) {
  constexpr float HeaderCenter = 48;
  float x = paint_.fonts.middle(title, 260, HeaderCenter, 30, Ink, true);
  // The count in a grey label beside the title, as 레트로 게임 shows its
  // system's; left out when the header has no room for it.
  const float room = metaRight > 0 ? metaRight - 16 - (x + 14) : 560.f;
  // Nothing to count draws no label: an empty list shows its notice instead.
  if (!meta.empty() && meta != "0" && paint_.fonts.measure(meta, 15, true) + 18 <= room)
    x += 14 + paint_.pill(meta, x + 14, HeaderCenter - 13);
  return x;
}
std::string Application::countLabel(size_t count, const std::string &query) const {
  return query.empty() ? std::to_string(count) : trf("Search results: {0}", {std::to_string(count)});
}
void Application::offlineNotice(const std::string &body) {
  if (session_ && !error_ && !pairingPending_)
    notice(tr("Loading from the server."), tr("This appears once the server answers."), 21);
  // No internet: what to fix is the console's link, as on 홈.
  else if (error_ && error_.message == net::offlineMessage())
    notice(tr("No internet connection."), error_.message + "\n" + tr("Try again in a moment."), 20, tr("Try again"), [this] { refreshAll(); }, "", "", {}, 46);
  else
    notice(tr("Not connected to a server."), body, 21);
}
void Application::notice(const std::string &title, const std::string &body, int icon, const std::string &button,
                         std::function<void()> press, const std::string &other, const std::string &otherKey,
                         std::function<void()> pressOther, int buttonIcon) {
  // Centered between the page header and the footer.
  constexpr float Width = 640, Left = 260 + (992 - Width) / 2, BodyTop = 110,
                  BodyBottom = 660, Center = Left + Width / 2, TextWidth = Width - 80;
  const int lines = body.empty() ? 0 : 4;
  // Measure first so the card hugs its text.
  const float bodyHeight = lines ? paint_.fonts.centered(body, -9999, -9999, 19,
                                                         Secondary, TextWidth, lines)
                                 : 0;
  constexpr float IconSize = 44;
  const float iconSpace = icon >= 0 ? IconSize + 16 : 0;
  constexpr float ButtonH = 56, ButtonGap = 28;
  const float buttonSpace = button.empty() ? 0 : ButtonGap + ButtonH;
  const float height = 48 + iconSpace + 30 + (lines ? 14 + bodyHeight : 0) + buttonSpace + 44;
  // An empty page still answers a finger as a list would: it stretches and
  // springs back (nothing to scroll, so its end is its start).
  const float pulled = glide("empty", 232, 84, 1048, BodyBottom - 84, 0, 0, 1);
  const float Top = (BodyTop + BodyBottom - height) / 2 - pulled;
  clipTo(232, 84, 1048, BodyBottom - 84);
  paint_.rect(Left, Top, Width, height, Surface, 20);
  if (icon >= 0)
    paint_.icon(icon, Center - IconSize / 2, Top + 40, Secondary, int(IconSize));
  paint_.fonts.centered(title, Center, Top + 44 + iconSpace, 24, Ink, TextWidth, 2, true);
  if (lines)
    paint_.fonts.centered(body, Center, Top + 92 + iconSpace, 19, Secondary, TextWidth, lines);
  if (!button.empty()) {
    // The main one in butter, with A while the cursor is on the page (as
    // 연결하기 is); the other beside it in the card's grey, with its own key.
    const bool focused = !sidebar_ && headerFocus_.empty();
    constexpr float Gap = 14;
    // An icon before the label when the button names one (다시 시도's arrows).
    constexpr float Mark = 22, MarkGap = 8;
    const float mark = buttonIcon >= 0 ? Mark + MarkGap : 0;
    const float width = (focused ? paint_.keyLabelWidth("A", button, 20, true) : paint_.fonts.measure(button, 20, true)) + mark + 64;
    const float otherWidth = other.empty() ? 0 : paint_.keyLabelWidth(otherKey, other, 20, true) + 64;
    const float total = width + (other.empty() ? 0 : Gap + otherWidth);
    const float x = Center - total / 2, y = Top + height - 44 - ButtonH;
    if (focused) paint_.ring(x, y, width, ButtonH, 14);
    paint_.rect(x, y, width, ButtonH, Butter, 14);
    // With A the key leads, then the icon and the label.
    float lx = x + 32;
    if (focused) {
      const float key = paint_.keyLabelWidth("A", button, 20, true) - paint_.fonts.measure(button, 20, true);
      paint_.key("A", lx, y + ButtonH / 2);
      lx += key;
    }
    if (buttonIcon >= 0) {
      paint_.icon(buttonIcon, lx, y + (ButtonH - Mark) / 2, Ink, int(Mark));
      lx += mark;
    }
    paint_.fonts.middle(button, lx, y + ButtonH / 2, 20, Ink, true);
    if (press) buttonHit({x, y, width, ButtonH}, 14, press);
    if (!other.empty()) {
      const float ox = x + width + Gap;
      paint_.rect(ox, y, otherWidth, ButtonH, Ground, 14);
      paint_.keyLabel(otherKey, other, ox + 32, y + ButtonH / 2, 20, Ink, true);
      if (pressOther) buttonHit({ox, y, otherWidth, ButtonH}, 14, pressOther);
    }
  }
  unclip();
}
void Application::viewPopup() {
  if (screen_ == Screen::Retro) {
    retroFilterPopup();
    return;
  }
  // Wide enough that every filter fits: a two-column checkbox grid on the left,
  // the sort radios on the right.
  constexpr float Width = 1080, RowHeight = 44, Gap = 10, SortWidth = 260;
  constexpr int Cols = PopupFilterColumns;
  const auto labels = popupFilterLabels();
  const auto orders = popupSorts();
  const int filterRows = (int(labels.size()) + Cols - 1) / Cols;
  const int rows = std::max(filterRows, int(orders.size()));
  const auto body = modal(Width, 44 + rows * RowHeight + 16, tr("Filter and sort"), tr("Cancel"), tr("Apply"), "", 10);
  const float gridLeft = body.x + 32, sortLeft = body.x + Width - 32 - SortWidth;
  const float cell = (sortLeft - 32 - gridLeft - Gap * (Cols - 1)) / Cols;
  paint_.fonts.middle(tr("Filter"), gridLeft, body.y + 26, 16, Secondary, true, sortLeft - 32 - gridLeft);
  paint_.fonts.middle(tr("Sort"), sortLeft, body.y + 26, 16, Secondary, true, SortWidth);
  paint_.rect(sortLeft - 18, body.y + 16, 1, rows * RowHeight + 24, Line);
  auto option = [&](int column, int index, float x, float y, float w, const std::string &label,
                    bool on, bool checkbox) {
    const bool focused = popupColumn_ == column && popupRow_ == index;
    const float h = RowHeight - 6, mid = y + h / 2;
    rowHit(x, y, w, h, 10, [this, column, index] {
      popupColumn_ = column;
      popupRow_ = index;
      action(Action::Accept);
    });
    // Applied: butter-tinted, bold, filled mark. Focus: border inside the row.
    const SDL_Color fill = on ? Cream : focused ? Ground : Surface;
    if (focused) paint_.focusRow(x, y, w, h, fill, 10);
    else paint_.rect(x, y, w, h, fill, 10);
    const int round = checkbox ? 6 : 10;
    paint_.rect(x + 12, mid - 10, 20, 20, on ? Ink : Muted, round);
    paint_.rect(x + 14, mid - 8, 16, 16, on ? Butter : Surface, round - 2);
    if (on && checkbox)
      paint_.icon(8, x + 14, mid - 8, OnButter, 16);
    else if (on)
      paint_.rect(x + 18, mid - 4, 8, 8, OnButter, 4);
    paint_.fonts.middle(label, x + 42, mid, 18, Ink, on, w - 52);
  };
  for (int n = 0; n < int(labels.size()); ++n)
    option(0, n, gridLeft + (n % Cols) * (cell + Gap), body.y + 44 + (n / Cols) * RowHeight, cell, labels[n],
           draftOn(n), true);
  for (int n = 0; n < int(orders.size()); ++n)
    option(1, n, sortLeft, body.y + 44 + n * RowHeight, SortWidth, sortLabel(orders[n]), orders[n] == draftSort_,
           false);
}
void Application::dialogs() {
  if (!confirmInstall_ && alert_.empty() && !serverForm_ && !confirmCancel_ && !viewPopup_ &&
      !confirmPatchRemove_ && !managingPatch_ && !pickingPatch_ && !tidyOpen_ && !homebrewSortAsk_ &&
      !languagePopup_ && !themePopup_ && languageGet_ == LanguageGet::None && !retroArchAsking_ && !raModal() && !saveNeedsGame_ && !savePicking_ && !serverAppSaveAsk_ &&
      !(screen_ == Screen::Saves && dropboxModal()) && !appUpdateAsk_ && appUpdate_ != AppUpdate::Downloading &&
      !homebrewRemoveAsk_ && !(homebrewAsk_ && homebrewExtras_) && !shortcutRemoveAsk_ && !selfShortcutAsk_ && !launchOffer_ && !adminEdit_.open && unqueueAsk_ < 0 && !bugAsk_ && !serverInfo_ && !clockModal() && !resumeAsked())
    return;
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  // When the popup on top changed, for the double-press guard (settled()).
  const int which = confirmCancel_ * 1 + (tidyOpen_ || homebrewSortAsk_) * 2 + confirmPatchRemove_ * 4 + confirmInstall_ * 8 +
                    dropboxUnlinkAsk_ * 32 + (languageGet_ == LanguageGet::Failed) * 16 + appUpdateAsk_ * 64 + homebrewRemoveAsk_ * 128 + (homebrewAsk_ && homebrewExtras_) * 256 +
                    (homebrewAsk_ && homebrewExtras_) * 512 + (homebrewAsk_ && homebrewSaveAsk_) * 1024 + serverAppSaveAsk_ * 2048 +
                    (serverForm_ && serverForm_->confirmDelete) * 4096 + shortcutRemoveAsk_ * 8192 +
                    selfShortcutAsk_ * 16384 + (unqueueAsk_ >= 0) * 32768 + bugAsk_ * 65536 +
                    clockModal() * 262144 + resumeAsked() * 524288 + raOpen_ * 1048576 + raListOpen_ * 2097152 + adminEdit_.open * 4194304 + serverInfo_ * 8388608 + bool(launchOffer_) * 16777216;
  if (which != popupKind_) {
    popupKind_ = which;
    popupAt_ = SDL_GetTicks();
  }
  if (alert_.empty() && (appUpdateAsk_ || appUpdate_ == AppUpdate::Downloading)) {
    appUpdateDialog();
  } else if (alert_.empty() && languageGet_ != LanguageGet::None) {
    languageGetDialog();
  } else if (alert_.empty() && clockModal()) {
    clockDialog();
  } else if (selfShortcutAsk_ && alert_.empty()) {
    selfShortcutDialog();
  } else if (launchOffer_ && alert_.empty()) {
    launchOfferDialog();
  } else if (adminEdit_.open && alert_.empty()) {
    adminEditDialog();
  } else if (resumeAsked() && alert_.empty()) {
    // The game's name in bold on as many lines as it takes, then in plain
    // text what A does and what Later does; the popup grows to fit them.
    constexpr float Width = 720, Text = Width - 64, Gap = 10;
    const auto &name = installQueue_.front().name;
    constexpr int NameLines = 6; // more than any name the server has
    const std::string ask = tr("The download resumes from where it stopped.");
    const std::string later = tr("To continue later, resume it from the Downloads menu.");
    const float nameH = paint_.fonts.lineCount(name, 22, Text, NameLines, true) * 22 * 1.3f;
    const float askH = paint_.fonts.lineCount(ask, 19, Text, 2) * 19 * 1.3f;
    const float laterH = paint_.fonts.lineCount(later, 18, Text, 2) * 18 * 1.3f;
    const auto body = modal(Width, 22 + nameH + Gap + askH + Gap + laterH + 18, tr("A download was in progress"), tr("Later"), "",
                            tr("Resume|download"), 25);
    float y = body.y + 22;
    paint_.fonts.wrapped(name, body.x + 32, y, 22, Ink, Text, NameLines, true);
    y += nameH + Gap;
    paint_.fonts.wrapped(ask, body.x + 32, y, 19, Ink, Text, 2);
    y += askH + Gap;
    paint_.fonts.wrapped(later, body.x + 32, y, 18, Secondary, Text, 2);
  } else if (viewPopup_) {
    viewPopup();
  } else if (languagePopup_) {
    languageDialog();
  } else if (themePopup_) {
    themeDialog();
  } else if (retroArchAsking_) {
    retroArchConfirm();
  } else if (raListOpen_) {
    raListDialog();
  } else if (raOpen_) {
    raDialog();
  } else if (saveNeedsGame_) {
    saveNeedsGameDialog();
  } else if (savePicking_) {
    savePickDialog();
  } else if (serverAppSaveAsk_ && alert_.empty()) {
    if (serverSaves_.empty()) serverAppSaveAsk_ = false;
    else appSaveConfirm(serverSaves_[std::clamp(serverSavePick_, 0, int(serverSaves_.size()) - 1)]);
  } else if (tidyOpen_ && alert_.empty()) {
    fileCleanupDialog();
  } else if (homebrewSortAsk_ && alert_.empty()) {
    homebrewSortDialog();
  } else if (confirmPatchRemove_ || managingPatch_ || pickingPatch_) {
    patchDialog();
  } else if (shortcutRemoveAsk_ && alert_.empty()) {
    shortcutRemoveDialog();
  } else if (homebrewRemoveAsk_ && alert_.empty()) {
    homebrewRemoveDialog();
  } else if (homebrewAsk_ && homebrewExtras_ && alert_.empty()) {
    // An alert from an app's patch or save job shows over these instead.
    homebrewExtrasDialog();
    if (homebrewAsk_ && homebrewExtras_ && homebrewSaveAsk_) {
      paint_.rect(0, 0, 1280, 720, {0, 0, 0, 120});
      blockTouch();
      homebrewSaveDialog();
    }
  } else if (serverForm_ && alert_.empty()) {
    serverFormDialog(); // a failed save's alert shows over it instead
  } else if (confirmInstall_ && selection_ && detailGroup_ < groups_.size()) {
    const auto &g = groups_[detailGroup_];
    // More than one server has every file: a row to pick which sends them.
    // A plugin's option comes from the server that offered it: no pick.
    const auto *option = optionInstall_ ? pickedOption() : nullptr;
    const bool pick = !option && selection_->servers().size() > 1;
    constexpr float PickRow = 64;
    // Its confirm's words on a warning card as 레트로 게임's (light red, the
    // warning icon), as tall as they are; the popup is wider so each keeps
    // to a line or two.
    constexpr float CardPad = 18, CardIcon = 56;
    // An unsigned file says so on the same card, in the server's words.
    const auto *unsignedFile = option ? nullptr : pickedUnsigned();
    const std::string note = option         ? option->confirmBody
                             : unsignedFile ? (!unsignedFile->body.empty() ? unsignedFile->body
                                                                           : std::string(tr("This file is not signed, so the console may refuse to install it.")))
                                            : std::string();
    const float cardW = CardIcon + OptionNoteWidth + CardPad;
    const float noteH = note.empty() ? 0 : paint_.fonts.lineCount(note, 18, OptionNoteWidth, OptionNoteLines) * 18 * 1.4f;
    const float noteBlock = note.empty() ? 0 : noteH + 2 * CardPad + 20;
    const std::string title = option ? (!option->confirmTitle.empty() ? option->confirmTitle : option->label)
                              : unsignedFile && !unsignedFile->title.empty() ? unsignedFile->title
                                                                             : std::string(tr("Install this game?"));
    const auto body = modal(note.empty() ? 700 : cardW + 64, 24 + 96 + 24 + (pick ? PickRow : 0) + noteBlock, title,
                            tr("Cancel"), "", tr("Install"), 4);
    const float x = body.x + 32, y = body.y + 24;
    if (!note.empty()) {
      const float top = y + 96 + 20;
      // The option's own colors and icon when it gives them.
      const auto own = [](const net::Rgb &rgb, SDL_Color otherwise) {
        return rgb.set ? SDL_Color{rgb.r, rgb.g, rgb.b, 255} : otherwise;
      };
      const net::Rgb unset;
      const SDL_Color fill = own(option ? option->cardColor : unset, RedTint),
                      text = own(option ? option->cardText : unset, Bad);
      paint_.rect(x, top, cardW, noteH + 2 * CardPad, fill, 14);
      paint_.icon(!option || option->cardIcon == "warning" ? 25 : 33, x + 18, top + CardPad, text, 24);
      paint_.fonts.wrapped(note, x + CardIcon, top + CardPad, 18, text, OptionNoteWidth, OptionNoteLines, false, 1.4f);
    }
    cover(g.representative.icon, x, y, 96, false, g.representative.name);
    paint_.fonts.text(g.representative.name, x + 116, y + 4, 22, Ink, true, body.w - 180);
    paint_.fonts.text(trf("{0} items · {1}", {std::to_string(selection_->count()),
                                              formatBytes(selection_->total())}),
                      x + 116, y + 42, 19, Secondary, false, body.w - 180);
    paint_.fonts.text(selection_->target == "nand" ? tr("Installs to the console.") : tr("Installs to the SD card."),
                      x + 116, y + 70, 19, Secondary, false, body.w - 180);
    if (pick) {
      const float top = y + 96 + 20, mid = top + 24, right = body.x + body.w - 32;
      // ‹ ● 서버 이름 › with the d-pad (or a tap on either side) stepping.
      constexpr float Width = 360;
      const float left = right - Width;
      paint_.fonts.middle(tr("Download from"), x, mid, 19, Ink, true, left - 16 - x);
      paint_.ring(left, top, Width, 48, 12);
      paint_.rect(left, top, Width, 48, Ground, 12);
      hit(left, top, Width / 2, 48, [this] { stepInstallServer(-1); });
      hit(left + Width / 2, top, Width / 2, 48, [this] { stepInstallServer(1); });
      paint_.icon(7, left + 10, mid - 12, Secondary, 24);
      paint_.icon(9, right - 34, mid - 12, Secondary, 24);
      const auto chosen = serverFor(selection_->server);
      paint_.rect(left + 48, mid - 6, 12, 12, serverDot(chosen.url), 6);
      paint_.fonts.middle(chosen.title, left + 70, mid, 19, Ink, true, Width - 70 - 44);
    }
  } else if (!alert_.empty()) {
    // As tall as the reason: a long one (a path, a server's words) is read
    // whole instead of cut at three lines.
    constexpr int MostLines = 8;
    const float lines = float(std::max(2, paint_.fonts.lineCount(alert_, 20, 700 - 64, MostLines)));
    const auto body = modal(700, 24 + lines * 20 * 1.3f + 20, alertTitle_.empty() ? std::string(tr("Cannot install")) : alertTitle_,
                            tr("Close"), "", tr("OK"), 25);
    paint_.fonts.wrapped(alert_, body.x + 32, body.y + 24, 20, Ink, body.w - 64, MostLines);
  } else if (bugAsk_) {
    bugReportDialog();
  } else if (serverInfo_) {
    serverInfoDialog();
  } else if (unqueueAsk_ >= 0 && unqueueAsk_ < int(installQueue_.size())) {
    // As the running download's: the same question, then which one it is
    // (its cover, name and what it is) and where it goes.
    const auto &q = installQueue_[size_t(unqueueAsk_)];
    const auto body = modal(720, 24 + 72 + 24, tr("Cancel the download task"), tr("Keep it"), "", tr("Cancel download"), 25);
    cover(q.icon, body.x + 32, body.y + 24, 72, false, q.name);
    paint_.fonts.text(q.name, body.x + 124, body.y + 30, 21, Ink, true, body.w - 156);
    paint_.fonts.text(q.file.size ? formatBytes(q.file.size) : std::string(), body.x + 124, body.y + 64, 18, Secondary,
                      false, body.w - 156);
  } else if (confirmCancel_ && cancelRetro_) {
    const bool pack = retroBusy_ == "pack";
    const auto body = modal(700, 24 + 30 + 16, pack ? tr("Cancel the RetroArch install?") : tr("Cancel the download?"),
                            tr("Keep downloading"), "", pack ? tr("Cancel install") : tr("Cancel download"), 25);
    paint_.fonts.text(pack ? tr("Files already here stay, and the next install picks up from them.") : tr("The partly downloaded game file is removed."),
                      body.x + 32, body.y + 24, 19, Secondary, false, body.w - 64);
  } else if (confirmCancel_) {
    // B keeps installing; A cancels, once the popup has settled.
    // Leaving stops the download where it is (it resumes next time), so it
    // is put as stopping, not cancelling.
    const auto body = confirmExit_ ? modal(700, 24 + 30 + 16, tr("Stop the download and exit?"), tr("Keep downloading"),
                                           "", tr("Stop and exit"), 25)
                                   : modal(700, 24 + 30 + 16, tr("Cancel the install?"), tr("Keep installing"), "",
                                           tr("Cancel install"), 25);
    // Stopped to leave, it goes on next time; cancelled, what came down goes.
    paint_.fonts.text(confirmExit_ ? tr("What came down so far can be picked up next time.")
                                   : tr("The partly downloaded game file is removed."),
                      body.x + 32, body.y + 24, 19, Secondary, false, body.w - 64);
  } else if (screen_ == Screen::Saves && dropboxModal()) {
    dropboxDialog(); // under an alert it raised, so the alert reads first
  }
}
} // namespace foil::ui
