#include "ui/Application.hpp"
#include "core/Config.hpp"
#include <algorithm>
namespace foil::ui {
void Application::settings() {
  // Full-width cards that scroll as one page between the header and the
  // footer, so any number of servers and options fit.
  constexpr float Left = 260, Width = 992, Inner = Left + 22, Right = Left + Width - 22;
  constexpr float ViewTop = 86, ViewBottom = 650, ServerRow = 61, Row = 46;
  pageTitle(tr("설정"));
  const int count = int(config_.servers.size());
  // Where each part sits before scrolling.
  const float cardHeight = std::max(1, count) * ServerRow + 16;
  const float addTop = ViewTop + cardHeight + 16;
  const float optionsTop = addTop + 52 + 12, optionsHeight = SettingsOptions * Row + 16;
  const float contentBottom = optionsTop + optionsHeight + (error_ ? 40 : 0);
  // The cursor's row stays in view.
  const float focusTop = selected_ < count ? 102 + selected_ * ServerRow - 3
                       : selected_ == count ? addTop
                                            : optionsTop + 8 + (selected_ - count - 1) * Row;
  const float focusHeight = selected_ < count ? 52 : selected_ == count ? 52 : Row;
  if (focusTop - settingsScroll_ < ViewTop) settingsScroll_ = focusTop - ViewTop + (selected_ == 0 ? 0 : 8);
  if (focusTop + focusHeight - settingsScroll_ > ViewBottom) settingsScroll_ = focusTop + focusHeight - ViewBottom + 8;
  settingsScroll_ = std::clamp(settingsScroll_, 0.f, std::max(0.f, contentBottom - ViewBottom));
  const float oy = -settingsScroll_;
  const SDL_Rect clip{int(Left - 8), int(ViewTop - 8), int(Width + 16), int(ViewBottom - ViewTop + 16)};
  SDL_RenderSetClipRect(paint_.renderer, &clip);
  // Taps only on what shows.
  const auto shown = [&](float y, float h) { return y >= ViewTop - 8 && y + h <= ViewBottom + 8; };
  if (contentBottom > ViewBottom)
    paint_.scrollbar(1266, ViewTop, ViewBottom - ViewTop, settingsScroll_, ViewBottom - ViewTop, contentBottom - ViewTop);

  // Servers: one row each, or the empty line.
  paint_.rect(Left, ViewTop + oy, Width, cardHeight, Surface, 18);
  if (!count) paint_.fonts.text(tr("등록된 서버가 없습니다."), Inner, 108 + oy, 20, Secondary);
  for (int i = 0; i < count; ++i) {
    const float y = 102 + i * ServerRow + oy;
    if (shown(y - 3, 52))
      hit(Left + 14, y - 3, Width - 28, 52, [this, i] {
        sidebar_ = false;
        selected_ = i;
        action(Action::Accept);
      });
    const auto &server = config_.servers[i];
    if (selected_ == i && !sidebar_) paint_.focusRow(Left + 14, y - 3, Width - 28, 52, Ground, 10);
    const bool active = size_t(i) == serverIndex_ && connected_;
    paint_.fonts.middle(server.title, Inner + 27, y + 23, 21, Ink, true, 640);
    const bool current = size_t(i) == serverIndex_ && session_;
    // The Korean status is a key: statusColor() reads it, tr() draws it.
    const char *status = !server.enabled                         ? "꺼 둠" // i18n: keep
                        : active                                ? "연결됨" // i18n: keep
                        : current && pairingPending_            ? "승인 대기" // i18n: keep
                        : current && network_.joinable() && !detailRequest_ ? "연결 중" // i18n: keep
                        : current && error_.message == net::offlineMessage() ? "인터넷 없음" // i18n: keep
                        : current && error_                     ? "연결 실패" // i18n: keep
                                                                : "연결 안 함"; // i18n: keep
    const char *statusText = tr(status);
    paint_.fonts.middle(statusText, Right - paint_.fonts.measure(statusText, 17), y + 23, 17,
                        statusColor(status));
    paint_.rect(Inner, y + 17, 12, 12, statusColor(status), 6);
    if (i + 1 < count) paint_.rect(Left, y + 54, Width, 1, Line);
  }
  // "+ 서버 추가" opens the server form, empty.
  if (selected_ == count && !sidebar_) paint_.ring(Left, addTop + oy, Width, 52, 12);
  paint_.rect(Left, addTop + oy, Width, 52, Surface, 12);
  paint_.fonts.middle(tr("+ 서버 추가"), Inner, addTop + oy + 26, 21, Ink, true);
  if (shown(addTop + oy, 52))
    hit(Left, addTop + oy, Width, 52, [this, count] {
      sidebar_ = false;
      selected_ = count;
      action(Action::Accept);
    });
  // The options share one card, one row each, focused like the server rows.
  paint_.rect(Left, optionsTop + oy, Width, optionsHeight, Surface, 18);
  // `toggle`: -1 draws `value`, 0 or 1 an off or on switch instead.
  const auto option = [&](int index, int icon, const std::string &label, const std::string &value,
                          SDL_Color valueColor, bool bold, int toggle = -1) {
    const float y = optionsTop + 8 + index * Row + oy;
    if (selected_ == count + 1 + index && !sidebar_)
      paint_.focusRow(Left + 14, y, Width - 28, Row - 2, Ground, 10);
    paint_.icon(icon, Inner, y + (Row - 2) / 2 - 12, Ink, 24);
    paint_.fonts.middle(label, Inner + 36, y + (Row - 2) / 2, 21, Ink, true);
    if (toggle >= 0)
      paint_.toggle(Right - 52, y + (Row - 2) / 2 - 15, toggle == 1);
    else
      paint_.fonts.middle(value, Right - paint_.fonts.measure(value, bold ? 20 : 18, bold), y + (Row - 2) / 2,
                          bold ? 20 : 18, valueColor, bold);
    // Rules run edge to edge, between rows only.
    if (index < SettingsOptions - 1)
      paint_.rect(Left, y + Row - 1, Width, 1, Line);
    if (shown(y, Row - 2))
      hit(Left + 14, y, Width - 28, Row - 2, [this, count, index] {
        sidebar_ = false;
        selected_ = count + 1 + index;
        action(Action::Accept);
      });
  };
  // A switch's note, left of it.
  const auto note = [&](int index, const std::string &text) {
    const float y = optionsTop + 8 + index * Row + oy;
    paint_.fonts.middle(text, Right - 68 - paint_.fonts.measure(text, 17), y + (Row - 2) / 2, 17, Secondary);
  };
  // Interface language, kept in state.json (never in the user's
  // config.json). " · Language" stays in English so someone who cannot
  // read the current language still finds it.
  option(0, 17, std::string(tr("언어")) + (language() == Language::English ? "" : " · Language"),
         languageName(language()), Ink, true);
  {
    const float y = optionsTop + 8 + oy, nameWidth = paint_.fonts.measure(languageName(language()), 20, true);
    paint_.flag(int(language()), Right - nameWidth - 42, y + (Row - 2) / 2 - 11, 33, 22);
  }
  // Where installs go; A switches between the SD card and the console.
  option(1, 26, tr("기본 저장 위치"), installTarget() == "nand" ? tr("본체") : tr("SD 카드"), Ink, true);
  // Ignoring signatures (the default) installs content that only runs with
  // sigpatches, which is what most homebrew installers expect.
  option(4, 34, tr("서명 검사 무시"), "", Ink, true, state_.skipSignatureCheck ? 1 : 0);
  note(4, tr("서명이 없는 파일도 설치합니다. 시그니처 패치가 필요합니다"));
  // Ignoring firmware (the default) clears the version a game asks for, so
  // it installs and launches on older firmware when it can run there.
  option(5, 27, tr("펌웨어 검사 무시"), "", Ink, true, state_.skipFirmwareCheck ? 1 : 0);
  note(5, tr("요구 버전보다 낮은 펌웨어에서도 설치합니다"));
  // A HOME Menu shortcut with each retro game and homebrew app (and this
  // app's own), when the server builds one.
  option(6, 0, tr("홈 메뉴 바로가기 만들기"), "", Ink, true, state_.makeShortcuts ? 1 : 0);
  note(6, state_.makeShortcuts ? tr("레트로 게임 및 홈브류 다운로드 시 바로가기를 자동 생성합니다") : tr("만들지 않습니다"));
  // Leftover cleanup: content no installed game uses, found first and only
  // deleted after a confirmation.
  option(3, 14, tr("남은 파일 정리"),
         !cleanupBusy_ ? tr("중단된 다운로드와 쓰지 않는 설치 파일을 찾아 지웁니다") : cleanupRemoving_ ? tr("지우는 중") : tr("찾는 중"), Secondary, false);
  // This build against the server's newest; A checks or updates.
  option(2, 3, tr("앱 업데이트"), appUpdateStatus(), appUpdate_ == AppUpdate::Available ? Ink : Secondary,
         appUpdate_ == AppUpdate::Available);
  // When on, an error popup sends the log's tail to the server, where the
  // admin page shows it; the last send's result reads left of the switch.
  option(7, 40, tr("오류 로그 전송"), "", Ink, true, state_.reportErrors ? 1 : 0);
  note(7, !state_.reportErrors ? std::string(tr("보내지 않습니다"))
          : logStatus_.empty() ? std::string(tr("오류가 발생하면 연결한 서버로 로그를 보냅니다"))
                               : logStatus_);
  if (error_)
    paint_.fonts.wrapped(error_.message + (error_.line ? " " + trf("({0}번째 줄)", {std::to_string(error_.line)}) : ""),
                         Left, optionsTop + optionsHeight + 8 + oy, 17, Secondary, Width, 1);
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
}
} // namespace foil::ui
