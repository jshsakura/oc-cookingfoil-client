// 업적: X on the retro home opens it under the same header, laid out as
// Dropbox is. Not signed in, a card with the steps and two buttons the
// d-pad picks between (the phone, or the keyboard); while the
// phone is awaited, its QR on that card; signed in, the account and then
// the server's retro games with achievements and the player's progress.
#include "ui/Application.hpp"
#include "net/AchievementsApi.hpp"
#include <algorithm>
#include <cstdio>
namespace foil::ui {
namespace {
constexpr float Top = 90, Bottom = 660, CardHeight = 466, RowHeight = 84, RowStep = 96;
// The connect card above the games, and the line naming them under it.
constexpr float ConnectHeight = 324, HeadingRoom = 56;
} // namespace

void Application::openRaPage() {
  if (!retroArchPresent()) {
    message(tr("Install RetroArch to sign in to RetroAchievements."));
    return;
  }
  retroView_ = RetroView::Achievements;
  raPageRow_ = raConnectButton_ = 0;
  raError_ = {};
  sidebar_ = false;
  headerFocus_.clear();
  unglide();
}
void Application::leaveRaPage() {
  raLinkClose();
  retroView_ = RetroView::Home;
  raError_ = {};
}
// Once a run, when the server keeps a RetroAchievements key (새로고침 and a
// sign-in ask again).
void Application::raGamesFetch() {
  if (raGamesAsked_ || raGamesJob_.busy() || !session_ || !connected_ || !raAccount_.enabled) return;
  raGamesAsked_ = true;
  const auto server = session_->server();
  const auto state = state_;
  raGamesJob_.run("games", [this, server, state]() -> Job::Finish {
    bool old = false;
    auto games = net::achievementGames(server, state, old, &stop_);
    return [this, games] {
      raGamesError_ = games.error;
      if (!games.error) raGames_ = games.value;
      raPageRow_ = std::clamp(raPageRow_, 0, int(raGames_.list.size()));
    };
  }, [this] { raGamesError_.message = tr("Something went wrong with RetroAchievements. Check log.txt."); });
}
void Application::raPage() {
  readRaAccount();
  retroHeader();
  if (!retroArchPresent()) {
    notice(tr("Install RetroArch to sign in to RetroAchievements."), "", 25);
    return;
  }
  // The card's sign-in is still being read: neither the steps nor the
  // account for a moment before the other.
  if (raJob_.is("read") && raSignedIn_.empty()) {
    paint_.skeleton(260, Top, 992, RowHeight, 16);
    return;
  }
  if (raLink_) {
    raLinkCard();
    return;
  }
  raGamesFetch();
  raGamesPage();
}
// Not signed in, at `y` (it scrolls with the games under it): what it is
// for, the three steps side by side, and the two buttons.
void Application::raConnectCard(float y) {
  const bool making = raJob_.is("link") || raJob_.is("signin");
  const bool focusable = inContent() && !raModal() && raPageRow_ == 0;
  raConnectButton_ = std::clamp(raConnectButton_, 0, 1);
  paint_.rect(260, y, 992, ConnectHeight, Surface, 20);
  constexpr float Left = 300;
  const float titleMid = y + 48;
  paint_.icon(31, Left, titleMid - 18, Ink, 36);
  paint_.fonts.middle(tr("Collect achievements with RetroAchievements"), Left + 52, titleMid, 26, Ink, true, 860);
  paint_.fonts.wrapped(tr("Earn achievements while you play retro games. Once signed in, RetroArch records them on RetroAchievements."),
                       Left, y + 80, 19, Secondary, 920, 1);
  paint_.fonts.wrapped(tr("The id can be registered on the server's web page too. A ROM changed from the original, such as a translation, may not earn achievements."),
                       Left, y + 110, 16, Secondary, 920, 1);
  const char *steps[] = {"1. Press Connect with a phone below.",
                         "2. Scan the QR code with your phone and enter the password.",
                         "3. This console finishes signing in on its own."};
  constexpr float StepWidth = 304;
  for (int i = 0; i < 3; ++i) {
    const float x = Left + i * StepWidth, top = y + 152;
    paint_.rect(x, top, 28, 28, Butter, 14);
    paint_.fonts.centered(std::to_string(i + 1), x + 14, top + 2, 16, Ink, 28, 1, true);
    std::string step = tr(steps[i]);
    if (const auto dot = step.find(". "); dot != std::string::npos && dot <= 2) step = step.substr(dot + 2);
    paint_.fonts.wrapped(step, x + 40, top + 3, 17, Ink, StepWidth - 60, 2);
  }
  // Plain buttons the d-pad moves between, A on the one picked; each
  // answers its own tap.
  const float buttonY = y + 244;
  constexpr float ButtonH = 56;
  const float phoneW = paint_.fonts.measure(tr("Connect with a phone"), 20, true) + 60;
  buttonHit({Left, buttonY, phoneW, ButtonH}, 14, [this] {
    sidebar_ = false;
    raPageRow_ = raConnectButton_ = 0;
    action(Action::Accept);
  });
  if (focusable && raConnectButton_ == 0) paint_.ring(Left, buttonY, phoneW, ButtonH, 14);
  paint_.rect(Left, buttonY, phoneW, ButtonH, making ? Line : Butter, 14);
  paint_.fonts.middle(tr("Connect with a phone"), Left + 30, buttonY + ButtonH / 2, 20, making ? Secondary : Ink, true);
  const float handX = Left + phoneW + 16;
  const float handW = paint_.fonts.measure(tr("Enter here"), 20, true) + 60;
  buttonHit({handX, buttonY, handW, ButtonH}, 14, [this] {
    sidebar_ = false;
    raPageRow_ = 0;
    raConnectButton_ = 1;
    action(Action::Accept);
  });
  if (focusable && raConnectButton_ == 1) paint_.ring(handX, buttonY, handW, ButtonH, 14);
  paint_.rect(handX, buttonY, handW, ButtonH, Ground, 14);
  paint_.fonts.middle(tr("Enter here"), handX + 30, buttonY + ButtonH / 2, 20, making ? Secondary : Ink, true);
  const float noteX = handX + handW + 28;
  if (raJob_.is("link"))
    paint_.fonts.middle(tr("Making the QR code"), noteX, buttonY + ButtonH / 2, 19, Secondary, true, 1220 - noteX);
  else if (raError_)
    paint_.fonts.wrapped(raError_.message, noteX, buttonY + 4, 17, Bad, 1220 - noteX, 2);
}
// The phone is awaited: the QR on the card, what to do with it, and how
// long it lasts. B drops it with its key.
void Application::raLinkCard() {
  paint_.rect(260, Top, 992, CardHeight, Surface, 20);
  paint_.pill(tr("Waiting for the phone"), 290, Top + 30, false, 17);
  paint_.fonts.text(tr("Enter the password on your phone"), 290, Top + 86, 28, Ink, true, 580);
  float y = Top + 140;
  y += paint_.fonts.wrapped(tr("Scan the QR code with your phone's camera and enter the RetroAchievements password there."),
                            290, y, 21, Secondary, 570, 3) + 12;
  y += paint_.fonts.wrapped(tr("The phone locks the password so only this console can open it. The phone has to reach this server's address."),
                            290, y, 18, Secondary, 570, 3) + 10;
  // Over http the page itself could be swapped on the way; the phone warns too.
  if (raLink_->url.rfind("http://", 0) == 0)
    y += paint_.fonts.wrapped(tr("The address is http, so someone on the same network could swap the page and see the password."),
                              290, y, 18, Pending, 570, 2) + 10;
  if (!raLink_->note.empty()) paint_.fonts.wrapped(raLink_->note, 290, y, 18, Bad, 570, 2);
  const uint32_t now = SDL_GetTicks();
  const int left = raLink_->until > now ? int((raLink_->until - now + 999) / 1000) : 0;
  char clock[16];
  std::snprintf(clock, sizeof(clock), "%d:%02d", left / 60, left % 60);
  // B, or a tap here, drops the QR.
  constexpr float CancelH = 52;
  const float cancelY = Top + CardHeight - 30 - CancelH;
  const float cancelW = paint_.keyLabelWidth("B", tr("Cancel"), 19, true) + 48;
  buttonHit({290, cancelY, cancelW, CancelH}, 12, [this] {
    sidebar_ = false;
    action(Action::Back);
  });
  paint_.rect(290, cancelY, cancelW, CancelH, Ground, 12);
  paint_.keyLabel("B", tr("Cancel"), 290 + 24, cancelY + CancelH / 2, 19, Ink, true);
  paint_.fonts.middle(trf("Expires in {0}", {clock}), 290 + cancelW + 20, cancelY + CancelH / 2, 18, Secondary);
  if (raQr_.encode(raLink_->url)) raQr_.draw(paint_.renderer, 908, int(Top + 75), 300);
}
// Signed in: the account row at `y`, the games' height; A on it signs out
// (after asking).
void Application::raAccountRow(float y, bool loading) {
  hit(260, y, 992, RowHeight, [this] {
    if (inContent() && raPageRow_ == 0) action(Action::Accept);
    sidebar_ = false;
    raPageRow_ = 0;
  });
  const bool focused = inContent() && !raModal() && raPageRow_ == 0;
  if (focused) paint_.ring(260, y, 992, RowHeight, 16);
  paint_.rect(260, y, 992, RowHeight, Surface, 16);
  paint_.rect(278, y + 10, 64, 64, Ground, 32);
  paint_.icon(31, 278 + 18, y + 10 + 18, Ink, 28);
  paint_.fonts.text(raSignedIn_, 360, y + 12, 21, Ink, true, 560);
  const std::string status = !raAccount_.enabled ? std::string(tr("This server has no RetroAchievements key, so games' achievement lists do not show."))
                             : loading           ? std::string(tr("Loading the achievements."))
                             : raGamesError_     ? raGamesError_.message
                             : raGames_.hasUser  ? trf("{0} points · {1} games with achievements",
                                                       {std::to_string(raGames_.userPoints), std::to_string(raGames_.list.size())})
                                                 : std::string(tr("The server has no id registered, so progress does not show."));
  paint_.fonts.text(status, 360, y + 46, 16, raGamesError_ && !loading ? Bad : Secondary, false, 640);
  const std::string out = tr("Sign out|achievements");
  const float bw = paint_.fonts.measure(out, 17, true) + 36;
  paint_.rect(1230 - bw, y + 22, bw, 40, focused ? Butter : Ground, 12);
  paint_.fonts.middle(out, 1230 - bw + 18, y + 42, 17, Ink, true);
}
// The account (or, not signed in, the connect card and a line naming the
// games), then the server's retro games with achievements, scrolling as
// one: the focused game is kept in the middle, the head at the top.
void Application::raGamesPage() {
  const bool signedIn = !raSignedIn_.empty();
  const auto &rows = raGames_.list;
  const bool loading = raAccount_.enabled && (raGamesJob_.busy() || !raGamesAsked_);
  // Not signed in with no key on the server, there is nothing to list.
  const bool listed = signedIn || raAccount_.enabled;
  const float head = signedIn ? RowStep : ConnectHeight + (listed ? HeadingRoom : 0);
  raPageRow_ = std::clamp(raPageRow_, 0, int(rows.size()));
  const float view = Bottom - Top;
  const float body = !listed ? 0 : rows.empty() ? (loading ? 3 * RowStep : RowStep) : float(rows.size()) * RowStep;
  const float content = head + body, max = std::max(0.f, content - view);
  const float target = raPageRow_ == 0 ? 0 : std::clamp(head + (raPageRow_ - 1) * RowStep + RowHeight / 2 - view / 2, 0.f, max);
  const float offset = glide("raPage", 232, Top, 1048, view, target, max, 1);
  if (max > 0) paint_.scrollbar(1266, Top, view - 12, offset, view, content);
  clipTo(232, Top - 8, 1048, view + 8);
  const float y = Top - offset;
  if (signedIn) raAccountRow(y, loading);
  else raConnectCard(y);
  if (!listed) {
    unclip();
    return;
  }
  if (!signedIn) {
    const float mid = y + ConnectHeight + HeadingRoom / 2 + 2;
    const float end = paint_.fonts.middle(tr("Games with achievements on this server"), 270, mid, 20, Ink, true);
    if (!rows.empty()) paint_.pill(std::to_string(rows.size()), end + 12, mid - 13);
  }
  const float listTop = y + head;
  if (rows.empty()) {
    if (loading)
      for (int n = 0; n < 3; ++n) paint_.skeleton(260, listTop + n * RowStep, 992, RowHeight, 16);
    else if (raGamesError_ && !signedIn)
      paint_.fonts.text(raGamesError_.message, 270, listTop + 16, 18, Bad, false, 960);
    else if (!raGamesError_)
      paint_.fonts.text(tr("No retro games on this server have achievements yet."), 270, listTop + 16, 18, Secondary);
    unclip();
    return;
  }
  const bool focusable = inContent() && !raModal();
  // Game n is row n + 1, under the head; only those on screen are drawn.
  for (int n = 0; n < int(rows.size()); ++n) {
    if (listTop + n * RowStep + RowHeight < Top - 8) continue;
    if (listTop + n * RowStep > Bottom) break;
    hit(260, listTop + n * RowStep, 992, RowHeight, [this, n] {
      if (inContent() && raPageRow_ == n + 1) action(Action::Accept);
      sidebar_ = false;
      raPageRow_ = n + 1;
    });
    const auto &g = rows[size_t(n)];
    const float y = listTop + n * RowStep;
    const bool focused = focusable && raPageRow_ == n + 1;
    if (focused) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    cover(g.icon, 278, y + 10, 64, false, g.title);
    if (focused) marquee(g.title, 360, y + 12, 21, 500);
    else paint_.fonts.text(g.title, 360, y + 12, 21, Ink, true, 500);
    // Under the name: a game found by its title says it may not count;
    // otherwise its system and when something was last earned.
    std::string line = g.system;
    if (g.lastEarnedAt) line += (line.empty() ? "" : " · ") + trf("Last earned {0}", {achievements::localDay(g.lastEarnedAt)});
    if (g.byTitle) paint_.fonts.text(tr("Found by title, so it may not earn them."), 360, y + 46, 16, Pending, false, 520);
    else paint_.fonts.text(line, 360, y + 46, 16, Secondary, false, 520);
    // Right to left: the points, then how many are earned.
    float right = 1230;
    const auto badge = [&](const std::string &text, bool on) {
      const float w = paint_.fonts.measure(text, 16, true) + 20;
      right -= w;
      paint_.rect(right, y + 28, w, 28, on ? Good : Line, 8);
      paint_.fonts.middle(text, right + 10, y + 42, 16, on ? Surface : Ink, true);
      right -= 8;
    };
    badge(trf("{0} pts", {std::to_string(g.points)}), false);
    // With no id to count by, how many there are to earn.
    if (raGames_.hasUser) badge(trf("{0}/{1} earned", {std::to_string(g.earned), std::to_string(g.count)}), g.earned > 0);
    else badge(trf("{0} achievements", {std::to_string(g.count)}), false);
  }
  unclip();
}
void Application::raPageAction(Action a) {
  if (a == Action::Back) {
    if (raLink_) {
      raLinkClose(); // the QR first, then the page
      raError_ = {};
    } else
      leaveRaPage();
    return;
  }
  if (raLink_ || !retroArchPresent()) {
    if (a == Action::Left) sidebar_ = true;
    return;
  }
  const bool signedIn = !raSignedIn_.empty();
  // The connect card's two buttons sit side by side: left past the first
  // is the menu, as anywhere else.
  if (a == Action::Left) {
    if (!signedIn && raPageRow_ == 0 && raConnectButton_ > 0) raConnectButton_ = 0;
    else sidebar_ = true;
    return;
  }
  if (a == Action::Right) {
    if (!signedIn && raPageRow_ == 0) raConnectButton_ = 1;
    return;
  }
  const int last = int(raGames_.list.size());
  if (a == Action::Up) raPageRow_ = std::max(0, raPageRow_ - 1);
  if (a == Action::Down) raPageRow_ = std::min(last, raPageRow_ + 1);
  if (a != Action::Accept || !settled()) return;
  if (raPageRow_ == 0) {
    if (signedIn) openRaPopup(); // asks before signing out
    else if (!raJob_.busy()) { // not while the account is read or the QR is on its way
      if (raConnectButton_ == 0) raPhone();
      else openRaPopup();
    }
    return;
  }
  const auto &row = raGames_.list[size_t(raPageRow_ - 1)];
  retro::Game game;
  game.id = row.gameId;
  game.name = row.title;
  game.system = row.system;
  game.iconUrl = row.icon;
  openRetroDetail(game); // its page fills in the rest
}
std::vector<std::pair<std::string, std::string>> Application::raPageHints() const {
  // B says where it goes: back to the shelves.
  const std::pair<std::string, std::string> back{"B", tr("Retro games")};
  if (raLink_) return {{"B", tr("Cancel")}};
  if (raPageRow_ > 0) return {{"A", tr("Details")}, back};
  if (!raSignedIn_.empty()) return {{"A", tr("Sign out|achievements")}, back};
  return {{"A", raConnectButton_ == 0 ? tr("Connect with a phone") : tr("Enter here")}, back};
}
} // namespace foil::ui
