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
// A guide card: its title, the lines of why, then a step a row.
constexpr float GuideBody = 60, GuideLine = 24, GuideStep = 40, GuideGap = 16, GuideWidth = 860;
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
      // Games matched by their file first: those are the ones that earn.
      // Matched by title (mostly translations) may not, so they follow, in
      // the server's order within each.
      std::stable_partition(raGames_.list.begin(), raGames_.list.end(), [](const auto &g) { return !g.byTitle; });
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
  paint_.icon(28, Left, titleMid - 18, Ink, 36);
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
    paint_.fonts.centered(std::to_string(i + 1), x + 14, top + 2, 16, OnButter, 28, 1, true);
    std::string step = tr(steps[i]);
    if (const auto dot = step.find(". "); dot != std::string::npos && dot <= 2) step = step.substr(dot + 2);
    paint_.fonts.wrapped(step, x + 40, top + 3, 17, Ink, StepWidth - 60, 2);
  }
  // Plain buttons the d-pad moves between, A on the one picked; each
  // answers its own tap.
  const float buttonY = y + 244;
  constexpr float ButtonH = 56;
  // Sized by their words, up to what keeps both on the row.
  const float phoneW = std::min(paint_.fonts.measure(tr("Connect with a phone"), 20, true), 360.f) + 60;
  buttonHit({Left, buttonY, phoneW, ButtonH}, 14, [this] {
    sidebar_ = false;
    raPageRow_ = raConnectButton_ = 0;
    action(Action::Accept);
  });
  if (focusable && raConnectButton_ == 0) paint_.ring(Left, buttonY, phoneW, ButtonH, 14);
  paint_.rect(Left, buttonY, phoneW, ButtonH, making ? Line : Butter, 14);
  paint_.fonts.middle(tr("Connect with a phone"), Left + 30, buttonY + ButtonH / 2, 20, making ? Secondary : OnButter, true,
                      phoneW - 60);
  const float handX = Left + phoneW + 16;
  const float handW = std::min(paint_.fonts.measure(tr("Enter here"), 20, true), 240.f) + 60;
  buttonHit({handX, buttonY, handW, ButtonH}, 14, [this] {
    sidebar_ = false;
    raPageRow_ = 0;
    raConnectButton_ = 1;
    action(Action::Accept);
  });
  if (focusable && raConnectButton_ == 1) paint_.ring(handX, buttonY, handW, ButtonH, 14);
  paint_.rect(handX, buttonY, handW, ButtonH, Ground, 14);
  paint_.fonts.middle(tr("Enter here"), handX + 30, buttonY + ButtonH / 2, 20, making ? Secondary : Ink, true, handW - 60);
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
  paint_.fonts.middle(trf("Expires in {0}", {clock}), 290 + cancelW + 20, cancelY + CancelH / 2, 18, Secondary, false,
                      870 - (290 + cancelW + 20));
  if (raQr_.encode(raLink_->url)) raQr_.draw(paint_.renderer, 908, int(Top + 75), 300);
}
// Signed in: the account row at `y`, the games' height; A on it signs out
// (after asking).
void Application::raAccountRow(float y, bool loading) {
  rowHit(260, y, 992, RowHeight, 16, [this] {
    if (inContent() && raPageRow_ == 0) action(Action::Accept);
    sidebar_ = false;
    raPageRow_ = 0;
  });
  const bool focused = inContent() && !raModal() && raPageRow_ == 0;
  if (focused) paint_.ring(260, y, 992, RowHeight, 16);
  paint_.rect(260, y, 992, RowHeight, Surface, 16);
  paint_.rect(278, y + 10, 64, 64, Ground, 32);
  paint_.icon(28, 278 + 18, y + 10 + 18, Ink, 28);
  paint_.fonts.text(raSignedIn_, 360, y + 12, 21, Ink, true, 560);
  // What the account has earned; what is missing is said by the card below.
  const std::string status = loading            ? std::string(tr("Loading the achievements."))
                             : raGames_.hasUser ? trf("{0} points · {1} games with achievements",
                                                      {std::to_string(raGames_.userPoints), std::to_string(raGames_.list.size())})
                                                : std::string(tr("Signed in on this console."));
  // retroarch.cfg holds one account for the console, the server its own
  // registered id: when they differ the player is told which one earns.
  const auto &registered = raAccount_.username;
  // RetroAchievements ids are not case sensitive.
  const bool differ = !registered.empty() && (registered.size() != raSignedIn_.size() ||
                                              !std::equal(registered.begin(), registered.end(), raSignedIn_.begin(),
                                                          [](unsigned char a, unsigned char b) {
                                                            return std::tolower(a) == std::tolower(b);
                                                          }));
  if (differ)
    paint_.fonts.text(trf("The server has {0} registered; achievements go to {1}, signed in on this console.",
                          {registered, raSignedIn_}),
                      360, y + 46, 16, Pending, true, 640);
  else
    paint_.fonts.text(status, 360, y + 46, 16, Secondary, false, 640);
  const std::string out = tr("Sign out|achievements");
  const float bw = paint_.fonts.measure(out, 17, true) + 36;
  paint_.rect(1230 - bw, y + 22, bw, 40, focused ? Butter : Ground, 12);
  paint_.fonts.middle(out, 1230 - bw + 18, y + 42, 17, focused ? OnButter : Ink, true);
}
// The account (or, not signed in, the connect card and a line naming the
// games), then the server's retro games with achievements, scrolling as
// one: the focused game is kept in the middle, the head at the top.
void Application::raGamesPage() {
  const bool signedIn = !raSignedIn_.empty();
  const auto &rows = raGames_.list;
  const bool loading = raAccount_.enabled && (raGamesJob_.busy() || !raGamesAsked_);
  const auto guide = raGuide();
  // Not signed in, the games are named under the card when there are any.
  const bool heading = !signedIn && !rows.empty();
  const float head = (signedIn ? RowStep : ConnectHeight + GuideGap) + (guide ? raGuideHeight(*guide) + GuideGap : 0) +
                     (heading ? HeadingRoom - GuideGap : 0);
  raPageRow_ = std::clamp(raPageRow_, 0, int(rows.size()));
  const float view = Bottom - Top;
  const float body = loading && rows.empty() ? 3 * RowStep : float(rows.size()) * RowStep;
  const float content = head + body, max = std::max(0.f, content - view);
  const float target = raPageRow_ == 0 ? 0 : std::clamp(head + (raPageRow_ - 1) * RowStep + RowHeight / 2 - view / 2, 0.f, max);
  const float offset = glide("raPage", 232, Top, 1048, view, target, max, 1);
  if (max > 0) paint_.scrollbar(1266, Top, view - 12, offset, view, content);
  clipTo(232, Top - 8, 1048, view + 8);
  const float y = Top - offset;
  if (signedIn) raAccountRow(y, loading);
  else raConnectCard(y);
  float under = y + (signedIn ? RowStep : ConnectHeight + GuideGap);
  if (guide) {
    raGuideCard(under, *guide);
    under += raGuideHeight(*guide) + GuideGap;
  }
  if (heading) {
    const float mid = under + HeadingRoom / 2 - GuideGap;
    const float end = paint_.fonts.middle(tr("Games with achievements on this server"), 270, mid, 20, Ink, true, 860);
    paint_.pill(std::to_string(rows.size()), end + 12, mid - 13);
  }
  const float listTop = y + head;
  if (rows.empty()) {
    if (loading)
      for (int n = 0; n < 3; ++n) paint_.skeleton(260, listTop + n * RowStep, 992, RowHeight, 16);
    unclip();
    return;
  }
  const bool focusable = inContent() && !raModal();
  // Game n is row n + 1, under the head; only those on screen are drawn.
  for (int n = 0; n < int(rows.size()); ++n) {
    if (listTop + n * RowStep + RowHeight < Top - 8) continue;
    if (listTop + n * RowStep > Bottom) break;
    rowHit(260, listTop + n * RowStep, 992, RowHeight, 16, [this, n] {
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
    if (g.earnedHardcore > 0) badge(trf("Hardcore {0}", {std::to_string(g.earnedHardcore)}), true);
    // With no id to count by, how many there are to earn.
    if (raGames_.hasUser) badge(trf("{0}/{1} earned", {std::to_string(g.earned), std::to_string(g.count)}), g.earned > 0);
    else badge(trf("{0} achievements", {std::to_string(g.count)}), false);
  }
  unclip();
}
// The one guide the page needs, if any: the server's key first (nothing
// lists without it), then a list that failed, then the id the server
// counts progress by, then a list that is empty.
std::optional<Application::RaGuide> Application::raGuide() const {
  const bool loading = raAccount_.enabled && (raGamesJob_.busy() || !raGamesAsked_);
  if (!raAccount_.enabled)
    return RaGuide{34, tr("The server needs a RetroAchievements key"),
                   tr("The server fetches each game's achievements and your progress from RetroAchievements, and it needs an API key to do so. Signing in on this console works without it."),
                   {tr("Ask the server's admin to open the admin dashboard."),
                    tr("Under Integrations, register a RetroAchievements API key."),
                    tr("Refresh this screen.")}};
  if (loading) return std::nullopt;
  if (raGamesError_)
    return RaGuide{25, tr("Could not load the achievements"), raGamesError_.message, {tr("Refresh this screen in a moment.")}, true};
  if (!raSignedIn_.empty() && !raGames_.hasUser)
    return RaGuide{13, tr("Register your RetroAchievements id on the server"),
                   tr("This console is signed in, but the server has no id for you, so it cannot show how far you are in each game."),
                   {tr("Open the server's web page on a phone or computer."),
                    tr("Register your RetroAchievements id there."),
                    tr("Refresh this screen.")}};
  if (raGames_.list.empty())
    return RaGuide{30, tr("No games are matched yet"),
                   tr("The server is fetching RetroAchievements' game lists by system and matching them with its ROMs. Only games with achievements appear here."),
                   {tr("Refresh this screen in a few minutes.")}};
  return std::nullopt;
}
float Application::raGuideHeight(const RaGuide &guide) {
  const int lines = paint_.fonts.lineCount(guide.body, 17, GuideWidth, 2);
  return GuideBody + lines * GuideLine + 22 + guide.steps.size() * GuideStep + 18;
}
void Application::raGuideCard(float y, const RaGuide &guide) {
  const float height = raGuideHeight(guide);
  const float stepsTop = y + GuideBody + paint_.fonts.lineCount(guide.body, 17, GuideWidth, 2) * GuideLine + 22;
  paint_.rect(260, y, 992, height, Surface, 20);
  paint_.rect(284, y + 24, 56, 56, guide.bad ? SDL_Color{Bad.r, Bad.g, Bad.b, 36} : Ground, 28);
  paint_.icon(guide.icon, 284 + 14, y + 24 + 14, guide.bad ? Bad : Ink, 28);
  constexpr float Text = 364, Width = GuideWidth;
  paint_.fonts.text(guide.title, Text, y + 24, 22, Ink, true, Width);
  paint_.fonts.wrapped(guide.body, Text, y + 60, 17, Secondary, Width, 2);
  for (size_t i = 0; i < guide.steps.size(); ++i) {
    const float top = stepsTop + i * GuideStep;
    paint_.rect(Text, top, 28, 28, Butter, 14);
    paint_.fonts.centered(std::to_string(i + 1), Text + 14, top + 2, 16, OnButter, 28, 1, true);
    paint_.fonts.middle(guide.steps[i], Text + 42, top + 14, 18, Ink, false, Width - 42);
  }
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
