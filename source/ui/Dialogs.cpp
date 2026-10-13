#include "ui/Application.hpp"
#include <algorithm>
namespace foil::ui {
// Key hints ending at `right`, like the screen footer; each one is tappable.
void Application::dialogHints(float right, float y, const std::string &close,
                              const std::string &apply, const std::string &accept, const std::string &remove) {
  // `accept` names what A does when the popup is one decision (다운로드).
  std::vector<std::pair<std::string, std::string>> keys{{"A", accept.empty() ? std::string(tr("선택")) : accept}};
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
    hit(x - 6, y - 22, w + 12, 44, [this, key = key] {
      action(key == "A" ? Action::Accept : key == "Y" ? Action::Queue : key == "X" ? Action::Search : Action::Back);
    });
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
  constexpr float Header = 72, Footer = 60;
  const float height = Header + bodyHeight + Footer;
  const float left = (1280 - width) / 2, top = (720 - height) / 2;
  paint_.rect(left, top, width, height, Surface, 20);
  // An icon ahead of the title says what kind of popup this is.
  const float titleX = left + 32 + (icon >= 0 ? 36 : 0);
  if (icon >= 0) paint_.icon(icon, left + 32, top + Header / 2 - 13, Ink, 26);
  // A title too long for the header runs, so the whole name can be read.
  marquee(title, titleX, paint_.fonts.centerTop(title, 24, true, top + Header / 2), 24, left + width - 32 - titleX);
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
    hit(x, 24, w, 48, run);
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
void Application::notice(const std::string &title, const std::string &body, int icon) {
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
  const float height = 48 + iconSpace + 30 + (lines ? 14 + bodyHeight : 0) + 44;
  const float Top = (BodyTop + BodyBottom - height) / 2;
  paint_.rect(Left, Top, Width, height, Surface, 20);
  if (icon >= 0)
    paint_.icon(icon, Center - IconSize / 2, Top + 40, Secondary, int(IconSize));
  paint_.fonts.centered(title, Center, Top + 44 + iconSpace, 24, Ink, TextWidth, 2, true);
  if (lines)
    paint_.fonts.centered(body, Center, Top + 92 + iconSpace, 19, Secondary, TextWidth, lines);
}
void Application::viewPopup() {
  // Wide enough that every filter fits: a two-column checkbox grid on the left,
  // the sort radios on the right.
  constexpr float Width = 1080, RowHeight = 44, Gap = 10, SortWidth = 260;
  constexpr int Cols = PopupFilterColumns;
  const auto labels = popupFilterLabels();
  const auto orders = popupSorts();
  const int filterRows = (int(labels.size()) + Cols - 1) / Cols;
  const int rows = std::max(filterRows, int(orders.size()));
  const auto body = modal(Width, 44 + rows * RowHeight + 16, tr("필터와 정렬"), tr("취소"), tr("적용"), "", 10);
  const float gridLeft = body.x + 32, sortLeft = body.x + Width - 32 - SortWidth;
  const float cell = (sortLeft - 32 - gridLeft - Gap * (Cols - 1)) / Cols;
  paint_.fonts.middle(tr("필터"), gridLeft, body.y + 26, 16, Secondary, true);
  paint_.fonts.middle(tr("정렬"), sortLeft, body.y + 26, 16, Secondary, true);
  paint_.rect(sortLeft - 18, body.y + 16, 1, rows * RowHeight + 24, Line);
  auto option = [&](int column, int index, float x, float y, float w, const std::string &label,
                    bool on, bool checkbox) {
    const bool focused = popupColumn_ == column && popupRow_ == index;
    const float h = RowHeight - 6, mid = y + h / 2;
    hit(x, y, w, h, [this, column, index] {
      popupColumn_ = column;
      popupRow_ = index;
      action(Action::Accept);
    });
    // Applied: butter-tinted, bold, filled mark. Focus: border inside the row.
    const SDL_Color fill = on ? SDL_Color{255, 241, 204, 255} : focused ? Ground : Surface;
    if (focused) paint_.focusRow(x, y, w, h, fill, 10);
    else paint_.rect(x, y, w, h, fill, 10);
    const int round = checkbox ? 6 : 10;
    paint_.rect(x + 12, mid - 10, 20, 20, on ? Ink : SDL_Color{176, 174, 167, 255}, round);
    paint_.rect(x + 14, mid - 8, 16, 16, on ? Butter : Surface, round - 2);
    if (on && checkbox)
      paint_.icon(8, x + 14, mid - 8, Ink, 16);
    else if (on)
      paint_.rect(x + 18, mid - 4, 8, 8, Ink, 4);
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
      !confirmPatchRemove_ && !managingPatch_ && !pickingPatch_ && !confirmCleanup_ && !confirmResume_ &&
      !languagePopup_ && !cleanupBusy_ && !retroAsking_ && !retroArchAsking_ && !saveNeedsGame_ && !savePicking_ && !shopAppSaveAsk_ &&
      !(screen_ == Screen::Saves && dropboxModal()) && !appUpdateAsk_ && appUpdate_ != AppUpdate::Downloading &&
      !homebrewRemoveAsk_ && !homebrewAsk_ && !shortcutRemoveAsk_ && !selfShortcutAsk_)
    return;
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  // When the popup on top changed, for the double-press guard (settled()).
  const int which = confirmCancel_ * 1 + confirmCleanup_ * 2 + confirmPatchRemove_ * 4 + confirmInstall_ * 8 +
                    confirmResume_ * 16 + dropboxUnlinkAsk_ * 32 + appUpdateAsk_ * 64 + homebrewRemoveAsk_ * 128 + homebrewAsk_ * 256 +
                    (homebrewAsk_ && homebrewExtras_) * 512 + (homebrewAsk_ && homebrewSaveAsk_) * 1024 + shopAppSaveAsk_ * 2048 +
                    (serverForm_ && serverForm_->confirmDelete) * 4096 + shortcutRemoveAsk_ * 8192 +
                    selfShortcutAsk_ * 16384;
  if (which != popupKind_) {
    popupKind_ = which;
    popupAt_ = SDL_GetTicks();
  }
  if (alert_.empty() && (appUpdateAsk_ || appUpdate_ == AppUpdate::Downloading)) {
    appUpdateDialog();
  } else if (selfShortcutAsk_ && alert_.empty()) {
    selfShortcutDialog();
  } else if (viewPopup_) {
    viewPopup();
  } else if (languagePopup_) {
    languageDialog();
  } else if (retroArchAsking_) {
    retroArchConfirm();
  } else if (retroAsking_) {
    retroConfirm();
  } else if (saveNeedsGame_) {
    saveNeedsGameDialog();
  } else if (savePicking_) {
    savePickDialog();
  } else if (shopAppSaveAsk_ && alert_.empty()) {
    if (shopSaves_.empty()) shopAppSaveAsk_ = false;
    else appSaveConfirm(shopSaves_[std::clamp(shopSavePick_, 0, int(shopSaves_.size()) - 1)]);
  } else if (cleanupBusy_) {
    cleanupProgressDialog();
  } else if (confirmResume_) {
    resumeDialog();
  } else if (confirmCleanup_) {
    cleanupDialog();
  } else if (confirmPatchRemove_ || managingPatch_ || pickingPatch_) {
    patchDialog();
  } else if (shortcutRemoveAsk_ && alert_.empty()) {
    shortcutRemoveDialog();
  } else if (homebrewRemoveAsk_ && alert_.empty()) {
    homebrewRemoveDialog();
  } else if (homebrewAsk_ && alert_.empty()) {
    // An alert from an app's patch or save job shows over these instead.
    homebrewAskDialog();
    if (homebrewAsk_ && homebrewExtras_) {
      paint_.rect(0, 0, 1280, 720, {0, 0, 0, 120});
      blockTouch();
      homebrewExtrasDialog();
    }
    if (homebrewAsk_ && homebrewExtras_ && homebrewSaveAsk_) {
      paint_.rect(0, 0, 1280, 720, {0, 0, 0, 120});
      blockTouch();
      homebrewSaveDialog();
    }
  } else if (serverForm_ && alert_.empty()) {
    serverFormDialog(); // a failed save's alert shows over it instead
  } else if (confirmInstall_ && selection_ && detailGroup_ < groups_.size()) {
    const auto &g = groups_[detailGroup_];
    const auto body = modal(700, 24 + 96 + 24, tr("이 게임을 설치하시겠습니까?"), tr("취소"), "", tr("설치"), 4);
    const float x = body.x + 32, y = body.y + 24;
    cover(g.representative.icon, x, y, 96, false, g.representative.name);
    paint_.fonts.text(g.representative.name, x + 116, y + 4, 22, Ink, true, body.w - 180);
    paint_.fonts.text(trf("{0}개 항목 · {1}", {std::to_string(selection_->count()),
                                              formatBytes(selection_->total())}),
                      x + 116, y + 42, 19, Secondary);
    paint_.fonts.text(selection_->target == "nand" ? tr("본체에 설치합니다.") : tr("SD 카드에 설치합니다."),
                      x + 116, y + 70, 19, Secondary);
  } else if (!alert_.empty()) {
    const auto body = modal(700, 24 + 84 + 16, alertTitle_.empty() ? std::string(tr("설치할 수 없습니다")) : alertTitle_,
                            tr("닫기"), "", tr("확인"), 25);
    paint_.fonts.wrapped(alert_, body.x + 32, body.y + 24, 20, Ink, body.w - 64, 3);
  } else if (confirmCancel_ && cancelRetro_) {
    const bool pack = retroBusy_ == "pack";
    const auto body = modal(700, 24 + 30 + 16, pack ? tr("RetroArch 설치를 취소하시겠습니까?") : tr("다운로드를 취소하시겠습니까?"),
                            tr("계속 받기"), "", pack ? tr("설치 취소") : tr("다운로드 취소"), 25);
    paint_.fonts.text(pack ? tr("받은 파일은 남겨 두고 다음에 이어서 받습니다.") : tr("받던 게임 파일은 삭제합니다."),
                      body.x + 32, body.y + 24, 19, Secondary, false, body.w - 64);
  } else if (confirmCancel_) {
    // B keeps installing; A cancels, once the popup has settled.
    const auto body = modal(700, 24 + 30 + 16,
                            confirmExit_ ? tr("설치를 취소하고 종료하시겠습니까?") : tr("설치를 취소하시겠습니까?"),
                            tr("계속 설치"), "", tr("설치 취소"), 25);
    paint_.fonts.text(tr("받던 부분은 다음에 이어서 받을 수 있습니다."), body.x + 32, body.y + 24, 19, Secondary, false,
                      body.w - 64);
  } else if (screen_ == Screen::Saves && dropboxModal()) {
    dropboxDialog(); // under an alert it raised, so the alert reads first
  }
}
} // namespace foil::ui
