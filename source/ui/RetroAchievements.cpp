// RetroAchievements: 레트로 게임's X opens the sign-in. The console signs in
// with RetroAchievements itself and keeps the token in retroarch.cfg (the
// password is used once and kept nowhere); the server keeps only the id, so
// it can list a game's achievements with the player's progress.
#include "ui/Application.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "net/AchievementsApi.hpp"
#include "net/DropboxUtil.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
namespace foil::ui {
namespace {
constexpr int Fields = 2; // id, password; then sign in and the phone, side by side
constexpr int Phone = Fields + 1;
constexpr uint32_t LinkLife = 5 * 60 * 1000; // the server drops a link after five minutes
// The console's sign-in: retroarch.cfg with the given lines set. A file that
// is there but did not read is left alone rather than replaced by them.
Error writeConfig(const std::string &sd, const std::map<std::string, std::string> &values) {
  const auto path = sd + "/" + achievements::ConfigPath;
  const auto text = readFile(path);
  std::error_code ec;
  if (text.empty() && std::filesystem::file_size(path, ec) > 0 && !ec)
    return {tr("Could not read RetroArch's settings.")};
  makeDirectories(std::filesystem::path(path).parent_path().string());
  std::string error;
  if (!atomicWrite(path, achievements::setConfig(text, values), error))
    return {trf("Could not write RetroArch's settings: {0}", {error})};
  return {};
}
// The sign-in itself, on the worker: RetroAchievements' token into
// retroarch.cfg. `refused` when RetroAchievements said no to the password.
struct ConsoleSignIn {
  std::string user;
  Error error;
  bool refused = false;
};
ConsoleSignIn signInOnConsole(const std::string &sd, const std::string &user, std::string password,
                              const std::atomic<bool> *cancel) {
  auto login = net::signInRetroAchievements(user, password, cancel);
  std::fill(password.begin(), password.end(), '\0');
  if (login.error) return {"", login.error, !login.value.error.empty()};
  if (auto error = writeConfig(sd, achievements::signInValues(login.value.user, login.value.token)))
    return {"", error, false};
  logLine("achievements: signed in on the console");
  return {login.value.user, {}, false};
}
Job::Finish crashed(Error &error) {
  return [&error] { error.message = tr("Something went wrong with RetroAchievements. Check log.txt."); };
}
std::string day(int64_t at) {
  const std::time_t t = at;
  std::tm local{};
  localtime_r(&t, &local);
  char text[16];
  std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
  return text;
}
} // namespace

// Once a run (새로고침 asks again): the server's id and the card's sign-in.
void Application::readRaAccount() {
  if (raRead_ || raJob_.busy()) return; // asked again next frame once the job is done
  raRead_ = true;
  const auto sd = sdRoot_;
  const bool online = session_ && connected_;
  const auto server = online ? session_->server() : Server{};
  const auto state = state_;
  raJob_.run("read", [this, sd, online, server, state]() -> Job::Finish {
    const auto signedIn = achievements::signedInUser(readFile(sd + "/" + achievements::ConfigPath));
    auto account = online ? net::achievementsAccount(server, state) : Parsed<achievements::Account>{};
    return [this, signedIn, account] {
      raSignedIn_ = signedIn;
      if (!account.error) raAccount_ = account.value;
    };
  }, crashed(raError_));
}
void Application::openRaPopup() {
  if (!retroArchPresent()) {
    message(tr("Install RetroArch to sign in to RetroAchievements."));
    return;
  }
  raOpen_ = true;
  raRow_ = 0;
  raError_ = {};
  raPassword_.clear();
  // The id the server keeps (from the web, or another console) is filled in.
  if (raUser_.empty()) raUser_ = !raSignedIn_.empty() ? raSignedIn_ : raAccount_.username;
}
void Application::raAction(Action a) {
  if (raLink_) {
    if (a == Action::Back) raLinkClose(); // back to the fields
    return;
  }
  if (raJob_.is("signin") || raJob_.is("signout") || raJob_.is("link")) return; // it finishes before anything else
  const bool signedIn = !raSignedIn_.empty();
  const int last = signedIn ? 0 : Fields;
  if (a == Action::Back) {
    raOpen_ = false;
    std::fill(raPassword_.begin(), raPassword_.end(), '\0');
    raPassword_.clear();
    return;
  }
  if (a == Action::Up) raRow_ = raRow_ == Phone ? Fields - 1 : std::max(0, raRow_ - 1);
  if (a == Action::Down && raRow_ < last) ++raRow_; // the phone's button sits beside sign in: already the last row
  if (!signedIn && a == Action::Right && raRow_ == Fields) raRow_ = Phone;
  if (!signedIn && a == Action::Left && raRow_ == Phone) raRow_ = Fields;
  if (a != Action::Accept || !settled()) return;
  if (!signedIn && raRow_ < Fields) {
    raField_ = raRow_;
    textRequest_ = TextRequest{raRow_ == 0 ? tr("RetroAchievements id") : tr("RetroAchievements password"),
                               raRow_ == 0 ? raUser_ : "", raRow_ == 1, 64};
    return;
  }
  if (raJob_.busy()) return; // the account is still being read: the note says so, and A works once it is done
  if (signedIn) raSignOut();
  else if (raRow_ == Phone) raPhone();
  else raSignIn();
}
void Application::raSignIn() {
  if (raUser_.empty() || raPassword_.empty()) {
    raError_.message = tr("Enter the id and the password.");
    return;
  }
  const auto user = raUser_, password = std::exchange(raPassword_, std::string());
  const auto sd = sdRoot_;
  const bool toServer = session_ && connected_ && raAccount_.enabled;
  const auto server = toServer ? session_->server() : Server{};
  const auto state = state_;
  raError_ = {};
  raJob_.run("signin", [this, user, password, sd, toServer, server, state]() -> Job::Finish {
    const auto signedIn = signInOnConsole(sd, user, password, &stop_);
    if (signedIn.error) return [this, error = signedIn.error] { raError_ = error; };
    // The server keeps the id too, for the lists; the sign-in stands without it.
    const auto account = toServer ? net::setAchievementsAccount(server, state, signedIn.user, &stop_)
                                  : Parsed<achievements::Account>{};
    return [this, user = signedIn.user, error = account.error] { raSignedIn(user, error); };
  }, crashed(raError_));
}
void Application::raSignOut() {
  const auto sd = sdRoot_;
  raError_ = {};
  raJob_.run("signout", [this, sd]() -> Job::Finish {
    if (auto error = writeConfig(sd, achievements::signOutValues())) return [this, error] { raError_ = error; };
    return [this] {
      raSignedIn_.clear();
      raOpen_ = false;
      raRow_ = 0;
      message(tr("Signed out of RetroAchievements on this console."));
    };
  }, crashed(raError_));
}
void Application::raSignedIn(const std::string &user, const Error &serverError) {
  raLinkClose();
  raSignedIn_ = raUser_ = user;
  if (!serverError && raAccount_.enabled) raAccount_.username = user;
  raOpen_ = false;
  message(serverError ? trf("Signed in to RetroAchievements. The server did not keep the id: {0}", {serverError.message})
                      : std::string(tr("Signed in to RetroAchievements.")));
}
// The phone sign-in: the console makes a key for this QR alone, the phone
// seals the id and password to it, and the server only passes the envelope on.
void Application::raPhone() {
  if (!session_ || !connected_) {
    raError_.message = tr("Connect to the server to enter it on a phone.");
    return;
  }
  const auto server = session_->server();
  const auto state = state_;
  raError_ = {};
  raLinkStop_ = false;
  raJob_.run("link", [this, server, state]() -> Job::Finish {
    seal::Key key;
    if (!seal::generate(key)) return [this] { raError_.message = tr("Could not make the key for the QR code."); };
    const auto publicKey = net::dropbox::base64UrlEncode({key.point.begin(), key.point.end()});
    bool old = false;
    auto link = net::startAchievementsLink(server, state, publicKey, old);
    if (link.error) return [this, error = link.error] { raError_ = error; };
    return [this, key, link = link.value, url = achievements::linkUrl(server.url, link.value.path, publicKey)] {
      if (!raOpen_) return; // closed while it was asked
      raLink_ = RaLink{key, link, url, "", SDL_GetTicks() + LinkLife};
      raLinkWait();
    };
  }, crashed(raError_));
}
void Application::raLinkWait() {
  if (!raLink_ || !session_) return;
  if (SDL_GetTicks() >= raLink_->until) {
    raLinkClose();
    raError_.message = tr("The QR code expired. Open a new one.");
    return;
  }
  const auto server = session_->server();
  const auto state = state_;
  const auto code = raLink_->start.code, secret = raLink_->key.secret, sd = sdRoot_;
  // Whatever comes back is for this link only: one closed or replaced since ignores it.
  const auto stop = [this, code](const Error &error) {
    if (!raLink_ || raLink_->start.code != code) return;
    raLinkClose();
    raError_ = error;
  };
  raJob_.run("linkwait", [this, server, state, code, secret, sd, stop]() -> Job::Finish {
    const auto wait = net::waitSealed(server, state, code, &raLinkStop_);
    if (raLinkStop_) return nullptr;
    if (wait.error) {
      // Cut off, even while the envelope came: the same link hands it over
      // again until the console reports, so wait a moment and ask again.
      for (int i = 0; i < 20 && !raLinkStop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      return [this, code] { raLinkAgain(code, ""); };
    }
    if (wait.value.gone) return [stop] { stop({tr("The QR code expired. Open a new one.")}); };
    if (wait.value.waiting) return [this, code] { raLinkAgain(code, ""); };
    const auto credentials = achievements::openEnvelope(secret, wait.value.sealed, code);
    if (credentials.error) {
      // The phone asks again without using up a try.
      const auto result = net::sendLinkResult(server, state, code, false, "", "unreadable", &stop_);
      if (result.error) return [stop, error = result.error] { stop(error); };
      if (result.value == "failed")
        return [stop] { stop({tr("Could not open what the phone sent. Open a new QR code.")}); };
      return [this, code] { raLinkAgain(code, tr("Could not open what the phone sent. Enter it again on the phone.")); };
    }
    const auto signedIn = signInOnConsole(sd, credentials.value.user, credentials.value.password, &stop_);
    if (signedIn.refused) {
      // A wrong password: the phone asks again, three tries in all.
      const auto result = net::sendLinkResult(server, state, code, false, "", "", &stop_);
      if (result.error || result.value != "wrong")
        return [stop] { stop({tr("RetroAchievements refused the password too many times. Open a new QR code.")}); };
      return [this, code] { raLinkAgain(code, tr("RetroAchievements refused the password. Enter it again on the phone.")); };
    }
    if (signedIn.error) return [stop, error = signedIn.error] { stop(error); };
    const auto result = net::sendLinkResult(server, state, code, true, signedIn.user, "", &stop_);
    return [this, user = signedIn.user, error = result.error] { raSignedIn(user, error); };
  }, [this] {
    raLinkClose();
    raError_.message = tr("Something went wrong with RetroAchievements. Check log.txt.");
  });
}
void Application::raLinkAgain(const std::string &code, const std::string &note) {
  if (!raLink_ || raLink_->start.code != code) return;
  if (!note.empty()) raLink_->note = note;
  raLinkWait();
}
void Application::raLinkClose() {
  raLinkStop_ = true;
  if (raLink_) std::fill(raLink_->key.secret.begin(), raLink_->key.secret.end(), '\0');
  raLink_.reset();
}
// X 업적 at the header's right end, one width whatever it says, so nothing
// moves while the page loads or when the sign-in changes.
float Application::raHeaderButton(float right) {
  const auto label = tr("Achievements");
  constexpr float NameRoom = 120;
  const float w = paint_.keyLabelWidth("X", label, 19, true) + 40 + NameRoom, x = right - w;
  const bool ready = retroArchPresent();
  headerItem("achievements", x, 24, w, 48, [this] { openRaPopup(); });
  paint_.rect(x, 24, w, 48, Surface, 12);
  const float end = paint_.keyLabel("X", label, x + 20, 48, 19, ready ? Ink : Secondary, true, ready);
  const std::string state = !ready                 ? tr("After RetroArch")
                            : !raSignedIn_.empty() ? raSignedIn_
                                                   : tr("Not signed in");
  paint_.fonts.middle(state, end + 12, 48, 16, !raSignedIn_.empty() && ready ? Good : Secondary, true, x + w - 16 - end - 12);
  return x;
}
void Application::raDialog() {
  const bool signedIn = !raSignedIn_.empty(), busy = raJob_.is("signin") || raJob_.is("signout") || raJob_.is("link");
  constexpr float Text = 636, Row = 60, Gap = 10;
  const int rows = signedIn ? 1 : Fields + 1;
  const float bodyH = 24 + 128 + rows * (Row + Gap) + 44;
  const std::string accept = busy || raLink_ ? NoAccept
                      : signedIn         ? tr("Sign out|achievements")
                      : raRow_ < Fields  ? tr("Enter")
                      : raRow_ == Phone  ? tr("Show QR code")
                                         : tr("Sign in");
  const auto body = modal(700, bodyH, "RetroAchievements", busy ? "" : raLink_ ? tr("Cancel") : tr("Close"), "", accept, 31);
  const float x = body.x + 32;
  float y = body.y + 24;
  if (raLink_) {
    raLinkView(x, y, Text);
    return;
  }
  // What it is, where else the id goes, and what it needs.
  y += paint_.fonts.wrapped(tr("Earn achievements while you play retro games. Once signed in, RetroArch records them on RetroAchievements."),
                            x, y, 18, Ink, Text, 2) + 8;
  y += paint_.fonts.wrapped(tr("The id can be registered on the server's web page too. A ROM changed from the original, such as a translation, may not earn achievements."),
                            x, y, 16, Secondary, Text, 2) + 6;
  paint_.fonts.wrapped(tr("The password signs in once and is kept neither on the console nor on the server."), x, y, 16,
                       Secondary, Text, 2);
  y = body.y + 24 + 128;
  const auto button = [&](int n, const std::string &label, float bx, float bw, float ry) {
    hit(bx, ry, bw, Row, [this, n] {
      raRow_ = n;
      action(Action::Accept);
    });
    if (raRow_ == n) paint_.ring(bx, ry, bw, Row, 14); // under the button
    paint_.rect(bx, ry, bw, Row, raRow_ == n ? Butter : Ground, 14);
    paint_.fonts.centered(label, bx + bw / 2, ry + Row / 2 - 14, 20, Ink, bw, 1, true);
  };
  const auto field = [&](int n, const std::string &label, const std::string &value) {
    const float ry = y + n * (Row + Gap);
    hit(x, ry, Text, Row, [this, n] {
      raRow_ = n;
      action(Action::Accept);
    });
    if (raRow_ == n) paint_.focusRow(x, ry, Text, Row, Ground, 12);
    else paint_.rect(x, ry, Text, Row, Ground, 12);
    paint_.fonts.middle(label, x + 20, ry + Row / 2, 18, Secondary, true);
    paint_.fonts.middle(value, x + 180, ry + Row / 2, 19, value.empty() ? Secondary : Ink, true, Text - 200);
  };
  if (signedIn) {
    paint_.fonts.middle(trf("This console is signed in as {0}.", {raSignedIn_}), x, y - 20, 18, Good, true, Text);
    button(0, busy ? tr("Signing out") : tr("Sign out|achievements"), x, Text, y);
  } else {
    field(0, tr("Id|account"), raUser_.empty() ? std::string(tr("Press A to enter")) : raUser_);
    field(1, tr("Password"), raPassword_.empty() ? std::string(tr("Press A to enter")) : std::string(raPassword_.size(), '*'));
    // Sign in here, or type the password on a phone instead of the console.
    const float ry = y + Fields * (Row + Gap), half = (Text - Gap) / 2;
    button(Fields, raJob_.is("signin") ? tr("Signing in") : tr("Sign in"), x, half, ry);
    button(Phone, raJob_.is("link") ? tr("Making the QR code") : tr("Enter on a phone"), x + half + Gap, half, ry);
  }
  const float noteY = y + rows * (Row + Gap) + 6;
  if (raError_) paint_.fonts.wrapped(raError_.message, x, noteY, 16, Bad, Text, 2);
  else if (raJob_.is("read")) paint_.fonts.wrapped(tr("Checking the account."), x, noteY, 16, Secondary, Text, 2);
  else if (!raAccount_.enabled && raRead_ && !raJob_.busy())
    paint_.fonts.wrapped(tr("This server has no RetroAchievements key, so games' achievement lists do not show."), x,
                         noteY, 16, Secondary, Text, 2);
}
// The QR for the phone, what it does, and how long it lasts.
void Application::raLinkView(float x, float y, float w) {
  constexpr float Size = 260;
  if (raQr_.encode(raLink_->url)) raQr_.draw(paint_.renderer, int(x), int(y + 8), int(Size));
  const float tx = x + Size + 32, tw = w - Size - 32;
  float ty = y + 8;
  ty += paint_.fonts.wrapped(tr("Scan the QR code with your phone's camera and enter the RetroAchievements password there."),
                             tx, ty, 19, Ink, tw, 4) + 12;
  ty += paint_.fonts.wrapped(tr("The phone locks the password so only this console can open it. The phone has to reach this server's address."),
                             tx, ty, 16, Secondary, tw, 5) + 16;
  if (!raLink_->note.empty()) ty += paint_.fonts.wrapped(raLink_->note, tx, ty, 17, Bad, tw, 3) + 8;
  else ty += paint_.fonts.wrapped(tr("Waiting for the phone"), tx, ty, 17, Pending, tw, 2) + 8;
  const uint32_t now = SDL_GetTicks();
  const int left = raLink_->until > now ? int((raLink_->until - now + 999) / 1000) : 0;
  char clock[16];
  std::snprintf(clock, sizeof(clock), "%d:%02d", left / 60, left % 60);
  paint_.fonts.text(trf("Expires in {0}", {clock}), tx, ty, 16, Secondary, false, tw);
}
// The game page's card, under its facts: how many achievements and points,
// X for the list. A game matched by its title says the patch may not count.
int Application::raCard(int y) {
  const auto &summary = retroMeta_.achievements;
  if (!summary.raGameId) return y;
  constexpr float X = 40, W = 750;
  const float height = summary.byTitle ? 92 : 64;
  hit(X, y, W, height, [this] { openRaList(); });
  paint_.rect(X, y, W, height, Surface, 16);
  paint_.icon(31, X + 20, y + 20, Ink, 24);
  paint_.fonts.middle(trf("{0} achievements · {1} points", {std::to_string(summary.count), std::to_string(summary.points)}),
                      X + 56, y + 32, 19, Ink, true, W - 260);
  paint_.keyChip("X", tr("See achievements"), X + W - 20, y + 32);
  if (summary.byTitle)
    paint_.fonts.middle(tr("Found by title, so a ROM changed from the original, such as a translation, may not earn them."), X + 56, y + 66, 16, Pending,
                        false, W - 80);
  return int(y + height + 16);
}
void Application::openRaList() {
  if (!retroMeta_.achievements.raGameId || !session_) return;
  raListOpen_ = true;
  raListRow_ = 0;
  const auto id = retroPick_.id;
  if (raListGame_ == id && (!raList_.list.empty() || raListJob_.busy())) return; // shown or coming already
  raListGame_ = id;
  raList_ = {};
  raListError_ = {};
  raListAsked_.clear();
  raListFetch();
}
void Application::raListFetch() {
  if (raListAsked_ == raListGame_ || raListJob_.busy() || !session_) return;
  const auto id = raListAsked_ = raListGame_;
  const auto server = session_->server();
  const auto state = state_;
  raListJob_.run("game", [this, server, state, id]() -> Job::Finish {
    bool missing = false;
    auto game = net::gameAchievements(server, state, id, missing, &stop_);
    return [this, game, id] {
      if (raListGame_ != id) return; // another game's page asked since
      if (game.error) raListError_ = game.error;
      else raList_ = game.value;
    };
  }, crashed(raListError_));
}
void Application::raListAction(Action a) {
  const int last = std::max(0, int(raList_.list.size()) - 1);
  if (a == Action::Back || a == Action::Search) raListOpen_ = false;
  if (a == Action::Up) raListRow_ = std::max(0, raListRow_ - 1);
  if (a == Action::Down) raListRow_ = std::min(last, raListRow_ + 1);
}
void Application::raListDialog() {
  constexpr int Shown = 5;
  constexpr float Row = 76, Gap = 8, Badge = 56;
  raListFetch(); // a fetch for the game before finishes first
  const bool loading = raListJob_.busy() || raListAsked_ != raListGame_;
  const auto &list = raList_.list;
  const auto body = modal(900, 24 + 34 + Shown * (Row + Gap) + 8, retroPick_.name, tr("Close"), "", NoAccept, 31);
  const float x = body.x + 32, w = body.w - 64;
  float y = body.y + 24;
  // Whose progress, or how to get it shown.
  const std::string head = raList_.hasUser
                               ? trf("{0}: {1} of {2} earned · {3} points",
                                     {raList_.username, std::to_string(raList_.earned), std::to_string(list.size()),
                                      std::to_string(raList_.userPoints)})
                           : loading ? std::string()
                                               : std::string(tr("Sign in or register the id to see your progress."));
  paint_.fonts.middle(head, x, y + 12, 18, raList_.hasUser ? Ink : Secondary, true, w);
  y += 34;
  if (list.empty()) {
    if (loading)
      for (int n = 0; n < 3; ++n) paint_.skeleton(x, y + n * (Row + Gap), w, Row, 12);
    else
      paint_.fonts.wrapped(raListError_ ? raListError_.message : std::string(tr("No achievements to show.")), x, y + 10, 18,
                           raListError_ ? Bad : Secondary, w, 3);
    return;
  }
  const int count = int(list.size());
  raListRow_ = std::clamp(raListRow_, 0, count - 1);
  const float top = glide("raList", x, y, w, Shown * (Row + Gap), float(std::clamp(raListRow_ - Shown / 2, 0, std::max(0, count - Shown))),
                          float(count - Shown), Row + Gap);
  const int first = int(top);
  clipTo(x, y, w, Shown * (Row + Gap));
  for (int n = first; n < count && n <= first + Shown; ++n) {
    const auto &a = list[size_t(n)];
    const float ry = y + (n - top) * (Row + Gap);
    hit(x, ry, w, Row, [this, n] { raListRow_ = n; });
    if (n == raListRow_) paint_.focusRow(x, ry, w, Row, Ground, 12);
    else paint_.rect(x, ry, w, Row, Ground, 12);
    // The coloured badge once earned, the grey one until then.
    const auto badge = a.earned ? a.badge : !a.badgeLocked.empty() ? a.badgeLocked : a.badge;
    if (!badge.empty() && !images_->loaded(badge) && !images_->failed(badge))
      paint_.skeleton(x + 10, ry + (Row - Badge) / 2, Badge, Badge, 10);
    if (!badge.empty()) images_->draw(badge, x + 10, ry + (Row - Badge) / 2, Badge, Badge, 10);
    else paint_.rect(x + 10, ry + (Row - Badge) / 2, Badge, Badge, Line, 10);
    const float tx = x + 10 + Badge + 16;
    const auto points = trf("{0} pts", {std::to_string(a.points)});
    const float right = x + w - 16;
    const float pw = paint_.pill(points, right - paint_.fonts.measure(points, 15, true) - 18, ry + 12);
    std::string state;
    if (a.earned) state = a.earnedHardcore ? tr("Earned · hardcore") : tr("Earned");
    if (a.earned && a.earnedAt) state += " · " + day(a.earnedAt);
    if (!state.empty()) paint_.fonts.middle(state, right - paint_.fonts.measure(state, 14, true), ry + Row - 18, 14, Good, true);
    const float textW = right - pw - 12 - tx;
    paint_.fonts.text(a.title, tx, ry + 10, 19, a.earned ? Ink : Secondary, true, textW);
    paint_.fonts.text(a.description, tx, ry + 40, 15, Secondary, false,
                      right - tx - (state.empty() ? 0 : paint_.fonts.measure(state, 14, true) + 12));
  }
  unclip();
  paint_.scrollbar(x + w + 10, y, Shown * (Row + Gap) - Gap, top, Shown, float(count));
}
} // namespace foil::ui
