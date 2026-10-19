// 업적: X on the retro home opens it, laid out as Dropbox is. Not signed in,
// a card with the steps, A for the phone and Y for the keyboard; while the
// phone is awaited, its QR on that card; signed in, the account and then
// the server's retro games with achievements and the player's progress.
#include "ui/Application.hpp"
#include "net/AchievementsApi.hpp"
#include <algorithm>
#include <cstdio>
namespace foil::ui {
namespace {
constexpr float Top = 90, CardHeight = 466, RowHeight = 84, RowStep = 96;
constexpr int Visible = 6; // rows in view, down to the footer
} // namespace

void Application::openRaPage() {
  if (!retroArchPresent()) {
    message(tr("Install RetroArch to sign in to RetroAchievements."));
    return;
  }
  retroView_ = RetroView::Achievements;
  raPageRow_ = 0;
  raError_ = {};
  sidebar_ = false;
  headerFocus_.clear();
  unglide();
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
  pageTitle(tr("Achievements"));
  readRaAccount();
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
  if (raSignedIn_.empty()) {
    if (raLink_) raLinkCard();
    else raConnectCard();
    return;
  }
  raGamesFetch();
  raAccountList();
}
void Application::raConnectCard() {
  const bool making = raJob_.is("link") || raJob_.is("signin");
  if (inContent() && !raModal()) paint_.ring(260, Top, 992, CardHeight, 20);
  paint_.rect(260, Top, 992, CardHeight, Surface, 20);
  // The icon beside the title, what it is for, the three steps with
  // numbered dots, and the buttons the first step names.
  constexpr float Left = 300, TitleMid = Top + 62;
  paint_.icon(31, Left, TitleMid - 20, Ink, 40);
  paint_.fonts.middle(tr("Collect achievements with RetroAchievements"), Left + 58, TitleMid, 28, Ink, true, 860);
  paint_.fonts.wrapped(tr("Earn achievements while you play retro games. Once signed in, RetroArch records them on RetroAchievements."),
                       Left, TitleMid + 40, 20, Secondary, 920, 2);
  const char *steps[] = {"1. Press Connect with a phone below.",
                         "2. Scan the QR code with your phone and enter the RetroAchievements password.",
                         "3. This console finishes signing in on its own."};
  for (int i = 0; i < 3; ++i) {
    const float mid = Top + 188 + i * 50;
    paint_.rect(Left, mid - 15, 30, 30, Butter, 15);
    paint_.fonts.centered(std::to_string(i + 1), Left + 15, mid - 13, 17, Ink, 30, 1, true);
    std::string step = tr(steps[i]);
    if (const auto dot = step.find(". "); dot != std::string::npos && dot <= 2) step = step.substr(dot + 2);
    paint_.fonts.middle(step, Left + 46, mid, 20, Ink, false, 880);
  }
  constexpr float ButtonY = Top + 346, ButtonH = 60;
  const float phoneW = paint_.keyLabelWidth("A", tr("Connect with a phone"), 21, true) + 64;
  paint_.rect(Left, ButtonY, phoneW, ButtonH, making ? Line : Butter, 14);
  paint_.keyLabel("A", tr("Connect with a phone"), Left + 32, ButtonY + ButtonH / 2, 21, Ink, true, !making);
  const float handX = Left + phoneW + 16;
  const float handW = paint_.keyLabelWidth("Y", tr("Enter here"), 21, true) + 64;
  hit(handX, ButtonY, handW, ButtonH, [this] {
    sidebar_ = false;
    action(Action::Queue);
  });
  paint_.rect(handX, ButtonY, handW, ButtonH, Ground, 14);
  paint_.keyLabel("Y", tr("Enter here"), handX + 32, ButtonY + ButtonH / 2, 21, Ink, true, !making);
  // A or a tap on the card connects with the phone.
  hit(260, Top, 992, CardHeight, [this] {
    sidebar_ = false;
    action(Action::Accept);
  });
  const float noteX = handX + handW + 28;
  if (raJob_.is("link"))
    paint_.fonts.middle(tr("Making the QR code"), noteX, ButtonY + ButtonH / 2, 19, Secondary, true, 1220 - noteX);
  else if (raError_)
    paint_.fonts.wrapped(raError_.message, noteX, ButtonY + 6, 18, Bad, 1220 - noteX, 2);
  paint_.fonts.wrapped(tr("The id can be registered on the server's web page too. A ROM changed from the original, such as a translation, may not earn achievements."),
                       270, Top + CardHeight + 18, 17, Secondary, 972, 2);
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
  paint_.fonts.text(trf("Expires in {0}", {clock}), 290, Top + CardHeight - 50, 18, Secondary);
  if (raQr_.encode(raLink_->url)) raQr_.draw(paint_.renderer, 908, int(Top + 75), 300);
}
// Signed in: the account first, then the games. The account row is the
// games' height and scrolls with them; A on it signs out (after asking).
void Application::raAccountList() {
  const auto &rows = raGames_.list;
  const int count = int(rows.size()) + 1;
  raPageRow_ = std::clamp(raPageRow_, 0, count - 1);
  const bool focusable = inContent() && !raModal();
  const float top = glide("raPage", 232, Top, 1048, 660 - Top, float(std::clamp(raPageRow_ - Visible / 2, 0, std::max(0, count - Visible))),
                          float(count - Visible), RowStep);
  const int first = int(top);
  paint_.scrollbar(1266, Top, RowStep * Visible - 12, top, Visible, float(count));
  clipTo(232, Top - 8, 1048, 660 - Top + 8);
  const bool loading = raAccount_.enabled && (raGamesJob_.busy() || !raGamesAsked_);
  {
    const float y = Top - top * RowStep;
    hit(260, y, 992, RowHeight, [this] {
      if (inContent() && raPageRow_ == 0) action(Action::Accept);
      sidebar_ = false;
      raPageRow_ = 0;
    });
    const bool focused = focusable && raPageRow_ == 0;
    if (focused) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    paint_.rect(278, y + 10, 64, 64, Ground, 32);
    paint_.icon(31, 278 + 18, y + 10 + 18, Ink, 28);
    paint_.fonts.text(raSignedIn_, 360, y + 12, 21, Ink, true, 560);
    const std::string status = !raAccount_.enabled ? std::string(tr("This server has no RetroAchievements key, so games' achievement lists do not show."))
                               : loading           ? std::string(tr("Loading the achievements."))
                               : raGamesError_     ? raGamesError_.message
                               : raGames_.hasUser  ? trf("{0} points · {1} games with achievements",
                                                         {std::to_string(raGames_.userPoints), std::to_string(rows.size())})
                                                   : std::string(tr("The server has no id registered, so progress does not show."));
    paint_.fonts.text(status, 360, y + 46, 16, raGamesError_ && !loading ? Bad : Secondary, false, 640);
    const std::string out = tr("Sign out|achievements");
    const float bw = paint_.fonts.measure(out, 17, true) + 36;
    paint_.rect(1230 - bw, y + 22, bw, 40, focused ? Butter : Ground, 12);
    paint_.fonts.middle(out, 1230 - bw + 18, y + 42, 17, Ink, true);
  }
  if (rows.empty()) {
    const float y = Top + RowStep - top * RowStep;
    if (loading)
      for (int n = 0; n + 1 < Visible; ++n) paint_.skeleton(260, y + n * RowStep, 992, RowHeight, 16);
    else if (raAccount_.enabled && !raGamesError_)
      paint_.fonts.text(tr("No retro games on this server have achievements yet."), 290, y + 20, 19, Secondary);
    unclip();
    return;
  }
  // Game n is row n + 1, under the account.
  for (int n = std::max(0, first - 1); n < int(rows.size()) && n + 1 <= first + Visible + 1; ++n) {
    const auto &g = rows[size_t(n)];
    const float y = Top + (n + 1 - top) * RowStep;
    hit(260, y, 992, RowHeight, [this, n] {
      if (inContent() && raPageRow_ == n + 1) action(Action::Accept);
      sidebar_ = false;
      raPageRow_ = n + 1;
    });
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
    badge(trf("{0}/{1} earned", {std::to_string(g.earned), std::to_string(g.count)}), g.earned > 0);
  }
  unclip();
}
void Application::raPageAction(Action a) {
  if (a == Action::Back) {
    if (raLink_) raLinkClose(); // the QR first, then the page
    else retroView_ = RetroView::Home;
    raError_ = {};
    return;
  }
  if (a == Action::Left) {
    sidebar_ = true;
    return;
  }
  if (raLink_ || !retroArchPresent()) return;
  if (raSignedIn_.empty()) {
    if (raJob_.busy() || !settled()) return; // reading the account, or the QR on its way
    if (a == Action::Accept) raPhone();
    if (a == Action::Queue) openRaPopup();
    return;
  }
  const int last = int(raGames_.list.size());
  if (a == Action::Up) raPageRow_ = std::max(0, raPageRow_ - 1);
  if (a == Action::Down) raPageRow_ = std::min(last, raPageRow_ + 1);
  if (a != Action::Accept || !settled()) return;
  if (raPageRow_ == 0) {
    openRaPopup(); // asks before signing out
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
  if (raLink_) return {{"B", tr("Cancel")}};
  if (raSignedIn_.empty())
    return {{"A", tr("Connect with a phone")}, {"Y", tr("Enter here")}, {"B", tr("Back")}};
  if (raPageRow_ == 0) return {{"A", tr("Sign out|achievements")}, {"B", tr("Back")}};
  return {{"A", tr("Details")}, {"B", tr("Back")}};
}
} // namespace foil::ui
