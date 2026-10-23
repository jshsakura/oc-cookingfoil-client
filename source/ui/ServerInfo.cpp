// The footer's link state, pressed: a popup naming every server in 설정,
// how each stands and how many games it brings; with the link failed or
// cut, the same press tries again (as 다시 시도 does).
#include "ui/Application.hpp"
#include "core/Config.hpp"
#include "net/Socket.hpp"
namespace foil::ui {
// How server `i` of 설정 stands now: a key statusColor() reads and tr() draws.
const char *Application::serverState(size_t i) const {
  const auto &server = config_.servers[i];
  const bool active = i == serverIndex_ && connected_;
  const bool current = i == serverIndex_ && session_;
  const auto other = serverStatus_.find(server.url);
  const bool read = other != serverStatus_.end() && !current;
  const bool waiting = read && other->second == "pairing", same = read && other->second == "same"; // i18n: as is
  return !server.enabled                         ? "Off|kept" // i18n: keep
         : read && other->second.empty()         ? "Connected" // i18n: keep
         : waiting                               ? "Awaiting approval" // i18n: keep
         : same                                  ? "Same server" // i18n: keep
         : read                                  ? "Failed|connection" // i18n: keep
         : active                                ? "Connected" // i18n: keep
         : current && pairingPending_            ? "Awaiting approval" // i18n: keep
         : current && network_.joinable() && !detailRequest_ ? "Connecting" // i18n: keep
         : current && error_.message == net::offlineMessage() ? "No internet" // i18n: keep
         : current && error_                     ? "Failed|connection" // i18n: keep
                                                 : "Not connected"; // i18n: keep
}
void Application::pressLinkState() {
  // A failed or cut link is tried again; any other state (connected, a
  // server with no games yet, connecting) shows where things stand.
  const bool failed = linkDropped() || (!connected_ && error_ && !pairingPending_);
  if (!failed && session_) {
    // Counted once as it opens: the lists do not change under the popup.
    serverGames_.clear();
    for (const auto &group : groups_)
      for (const auto &url : groupServers(group)) ++serverGames_[url];
    serverInfo_ = true;
    serverInfoRow_ = 0;
    return;
  }
  if (session_ && !network_.joinable()) refreshAll();
}
void Application::serverInfoAction(Action a) {
  const int last = std::max(0, int(config_.servers.size()) - 1);
  if (a == Action::Up) serverInfoRow_ = std::max(0, serverInfoRow_ - 1);
  if (a == Action::Down) serverInfoRow_ = std::min(last, serverInfoRow_ + 1);
  if (a == Action::Back || (a == Action::Accept && settled())) serverInfo_ = false;
}
void Application::serverInfoDialog() {
  constexpr int Shown = 4;
  constexpr float Row = 72, Gap = 8;
  const int count = int(config_.servers.size());
  const float listH = float(std::max(1, std::min(count, Shown))) * (Row + Gap) - Gap;
  const auto body = modal(720, 20 + listH + 20, tr("Servers"), tr("Close"), "", NoAccept, 39);
  const float x = body.x + 28, y = body.y + 20, textW = body.w - 56;
  serverInfoRow_ = std::clamp(serverInfoRow_, 0, std::max(0, count - 1));
  // Past four it scrolls, by the d-pad or a finger, as the bug report's list does.
  const float rowW = textW - (count > Shown ? 18 : 0);
  const float top = glide("serverInfo", x, y, rowW, listH, float(std::clamp(serverInfoRow_ - Shown / 2, 0, std::max(0, count - Shown))),
                          float(std::max(0, count - Shown)), Row + Gap);
  clipTo(x, y, rowW, listH);
  for (int n = std::max(0, int(top)); n < count && n <= int(top) + Shown; ++n) {
    const auto &server = config_.servers[size_t(n)];
    const float ry = y + (float(n) - top) * (Row + Gap), mid = ry + Row / 2;
    rowHit(x, ry, rowW, Row, 12, [this, n] { serverInfoRow_ = n; });
    if (n == serverInfoRow_) paint_.focusRow(x, ry, rowW, Row, Ground, 12);
    else paint_.rect(x, ry, rowW, Row, Ground, 12);
    paint_.rect(x + 20, mid - 6, 12, 12, serverDot(server.url), 6);
    // Its state on the right in its color, the games it brings under it.
    const char *status = serverState(size_t(n));
    const std::string state = tr(status);
    const auto games = serverGames_.find(server.url);
    const bool counted = std::string_view(status) == "Connected" && games != serverGames_.end(); // i18n: keep
    const std::string many = counted ? trf("{0} games", {std::to_string(games->second)}) : std::string();
    const float stateW = std::max(paint_.fonts.measure(state, 17, true), counted ? paint_.fonts.measure(many, 15) : 0.f);
    const float right = x + rowW - 20;
    paint_.fonts.middle(state, right - paint_.fonts.measure(state, 17, true), counted ? mid - 11 : mid, 17, statusColor(status), true);
    if (counted) paint_.fonts.middle(many, right - paint_.fonts.measure(many, 15), mid + 13, 15, Secondary);
    const auto host = hostOf(server.url);
    const float nameX = x + 46, nameW = rowW - 46 - stateW - 40;
    if (host.empty() || host == server.title) {
      paint_.fonts.middle(server.title, nameX, mid, 19, Ink, true, nameW);
    } else {
      paint_.fonts.middle(server.title, nameX, mid - 11, 19, Ink, true, nameW);
      paint_.fonts.middle(host, nameX, mid + 13, 15, Secondary, false, nameW);
    }
  }
  unclip();
  if (count > Shown) paint_.scrollbar(x + rowW + 8, y, listH, top, Shown, float(count));
}
} // namespace foil::ui
