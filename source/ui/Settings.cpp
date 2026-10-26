#include "ui/Application.hpp"
#include "core/Config.hpp"
#include "platform/Shortcut.hpp"
#include <algorithm>
#include <cstring>
namespace foil::ui {
void Application::settings() {
  // Full-width cards that scroll as one page between the header and the
  // footer, so any number of servers and options fit.
  constexpr float Left = 260, Width = 992, Inner = Left + 22, Right = Left + Width - 22;
  constexpr float ViewTop = 86, ViewBottom = 650, Row = 46;
  // Each server is a card of its own, like "+ 서버 추가" below them, so the
  // cursor rings the card the same way on both.
  constexpr float ServerCard = 52, ServerStep = ServerCard + 12;
  pageTitle(tr("Settings"));
  const int count = int(config_.servers.size());
  // Where each part sits before scrolling.
  const float addTop = ViewTop + std::max(1, count) * ServerStep;
  const float optionsTop = addTop + 52 + 12, optionsHeight = SettingsOptions * Row + 16;
  const float contentBottom = optionsTop + optionsHeight + (error_ ? 40 : 0);
  // The cursor's row stays in view.
  const float focusTop = selected_ < count ? ViewTop + selected_ * ServerStep
                       : selected_ == count ? addTop
                                            : optionsTop + 8 + (selected_ - count - 1) * Row;
  const float focusHeight = selected_ < count ? 52 : selected_ == count ? 52 : Row;
  if (focusTop - settingsScroll_ < ViewTop) settingsScroll_ = focusTop - ViewTop + (selected_ == 0 ? 0 : 8);
  if (focusTop + focusHeight - settingsScroll_ > ViewBottom) settingsScroll_ = focusTop + focusHeight - ViewBottom + 8;
  settingsScroll_ = std::clamp(settingsScroll_, 0.f, std::max(0.f, contentBottom - ViewBottom));
  // The cursor keeps its card in view; a finger moves the page freely.
  const float pageTop = glide("settings", 232, ViewTop, 1048, ViewBottom - ViewTop, settingsScroll_,
                            contentBottom - ViewBottom, 1);
  const float oy = -pageTop;
  const SDL_Rect clip{int(Left - 8), int(ViewTop - 8), int(Width + 16), int(ViewBottom - ViewTop + 16)};
  SDL_RenderSetClipRect(paint_.renderer, &clip);
  // Taps only on what shows.
  const auto shown = [&](float y, float h) { return y >= ViewTop - 8 && y + h <= ViewBottom + 8; };
  if (contentBottom > ViewBottom)
    paint_.scrollbar(1266, ViewTop, ViewBottom - ViewTop, pageTop, ViewBottom - ViewTop, contentBottom - ViewTop);

  // Servers: a card each, or one with the empty line.
  if (!count) {
    paint_.rect(Left, ViewTop + oy, Width, ServerCard, Surface, 12);
    paint_.fonts.middle(tr("No servers added."), Inner, ViewTop + oy + ServerCard / 2, 20, Secondary, false, Width - 44);
  }
  for (int i = 0; i < count; ++i) {
    const float top = ViewTop + i * ServerStep + oy, mid = top + ServerCard / 2;
    if (shown(top, ServerCard))
      rowHit(Left, top, Width, ServerCard, 12, [this, i] {
        sidebar_ = false;
        selected_ = i;
        action(Action::Accept);
      });
    const auto &server = config_.servers[i];
    if (selected_ == i && !sidebar_) paint_.ring(Left, top, Width, ServerCard, 12);
    paint_.rect(Left, top, Width, ServerCard, Surface, 12);
    paint_.fonts.middle(server.title, Inner + 27, mid, 21, Ink, true, 640);
    const char *status = serverState(size_t(i));
    const char *statusText = tr(status);
    paint_.fonts.middle(statusText, Right - paint_.fonts.measure(statusText, 17), mid, 17, statusColor(status));
    paint_.rect(Inner, mid - 6, 12, 12, statusColor(status), 6);
  }
  // "+ 서버 추가" opens the server form, empty.
  if (selected_ == count && !sidebar_) paint_.ring(Left, addTop + oy, Width, 52, 12);
  paint_.rect(Left, addTop + oy, Width, 52, Surface, 12);
  paint_.fonts.middle(tr("+ Add server"), Inner, addTop + oy + 26, 21, Ink, true, Width - 44);
  if (shown(addTop + oy, 52))
    rowHit(Left, addTop + oy, Width, 52, 12, [this, count] {
      sidebar_ = false;
      selected_ = count;
      action(Action::Accept);
    });
  // The options share one card, one row each, focused like the server rows.
  paint_.rect(Left, optionsTop + oy, Width, optionsHeight, Surface, 18);
  // Text on a row's right keeps clear of its label: longer languages step
  // it down, then cut it, instead of drawing over the label.
  std::array<float, SettingsOptions> labelEnds{};
  const auto rightText = [&](int index, const std::string &text, float right, int size, SDL_Color color, bool bold) {
    const float room = right - labelEnds[index] - 24;
    while (size > 14 && paint_.fonts.measure(text, size, bold) > room) --size;
    const float width = std::min(paint_.fonts.measure(text, size, bold), room);
    paint_.fonts.middle(text, right - width, optionsTop + 8 + index * Row + (Row - 2) / 2 + oy,
                        size, color, bold, room);
  };
  // `toggle`: -1 draws `value`, 0 or 1 an off or on switch instead.
  const auto option = [&](int index, int icon, const std::string &label, const std::string &value,
                          SDL_Color valueColor, bool bold, int toggle = -1) {
    const float y = optionsTop + 8 + index * Row + oy;
    if (selected_ == count + 1 + index && !sidebar_)
      paint_.focusRow(Left + 14, y, Width - 28, Row - 2, Ground, 10);
    paint_.icon(icon, Inner, y + (Row - 2) / 2 - 12, Ink, 24);
    paint_.fonts.middle(label, Inner + 36, y + (Row - 2) / 2, 21, Ink, true);
    labelEnds[index] = Inner + 36 + paint_.fonts.measure(label, 21, true);
    if (toggle >= 0)
      paint_.toggle(Right - 52, y + (Row - 2) / 2 - 15, toggle == 1);
    else
      rightText(index, value, Right, bold ? 20 : 18, valueColor, bold);
    // Rules run edge to edge, between rows only.
    if (index < SettingsOptions - 1)
      paint_.rect(Left, y + Row - 1, Width, 1, Line);
    if (shown(y, Row - 2))
      rowHit(Left + 14, y, Width - 28, Row - 2, 10, [this, count, index] {
        sidebar_ = false;
        selected_ = count + 1 + index;
        action(Action::Accept);
      });
  };
  // A switch's note, left of it.
  const auto note = [&](int index, const std::string &text) { rightText(index, text, Right - 68, 17, Secondary, false); };
  // Interface language, kept in state.json (never in the user's
  // config.json). " · Language" stays in English so someone who cannot
  // read the current language still finds it.
  option(0, 17, std::string(tr("Language")) + (std::strcmp(languageCode(), "en") == 0 ? "" : " · Language"),
         languageName(), Ink, true);
  {
    const float y = optionsTop + 8 + oy, nameWidth = paint_.fonts.measure(languageName(), 20, true);
    if (const auto *extra = extraLanguage()) paint_.languageChip(extra->code, Right - nameWidth - 42, y + (Row - 2) / 2 - 11, 33, 22);
    else paint_.flag(int(language()), Right - nameWidth - 42, y + (Row - 2) / 2 - 11, 33, 22);
  }
  // Where installs go; A switches between the SD card and the console.
  option(1, 26, tr("Default location"), installTarget() == "nand" ? tr("Console") : tr("SD card"), Ink, true);
  // Ignoring signatures (the default) installs content that only runs with
  // sigpatches, which is what most homebrew installers expect.
  option(4, 34, tr("Ignore signature check"), "", Ink, true, state_.skipSignatureCheck ? 1 : 0);
  note(4, tr("Installs unsigned files too. Needs sigpatches"));
  // A HOME Menu shortcut with each retro game and homebrew app (and this
  // app's own), when the server builds one.
  option(5, 0, tr("Make HOME Menu shortcuts"), "", Ink, true, state_.makeShortcuts ? 1 : 0);
  // Said as it is: made here, or only from the server's shortcuts when this
  // console cannot make them.
  note(5, !state_.makeShortcuts            ? tr("Not made")
          : platform::canMakeShortcuts() ? tr("Creates a shortcut when a retro game or homebrew app downloads")
                                         : tr("Only for items the server has a shortcut for"));
  // The app's sounds: the start, cursor steps, the queue, done and errors.
  option(6, 44, tr("Sound effects"), "", Ink, true, state_.sounds ? 1 : 0);
  note(6, tr("Turns the startup sound and effects on or off"));
  // The log's tail to the server, only when pressed: what went wrong reaches
  // whoever runs it.
  option(7, 33, tr("Send a bug report"),
         bugWorker_.joinable() ? std::string(tr("Sending")) : std::string(tr("Sends the end of the app's log to the connected server")),
         Secondary, false);
  // What the app leaves on the card: A shows how much each kind takes in a
  // popup and deletes the ones picked.
  option(3, 14, tr("File cleanup"), tidyBusy_ && tidyDeleting_ ? tr("Deleting") : tr("Clears leftover files and caches"),
         Secondary, false);
  // This build against the server's newest; A checks or updates.
  option(2, 3, tr("App update"), appUpdateStatus(), appUpdate_ == AppUpdate::Available ? Ink : Secondary,
         appUpdate_ == AppUpdate::Available);
  if (error_)
    paint_.fonts.wrapped(error_.message + (error_.line ? " " + trf("(line {0})", {std::to_string(error_.line)}) : ""),
                         Left, optionsTop + optionsHeight + 8 + oy, 17, Secondary, Width, 1);
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
}
} // namespace foil::ui
