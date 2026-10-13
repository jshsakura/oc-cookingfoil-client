#include "ui/Application.hpp"
#include "core/Forwarder.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "net/HomebrewApi.hpp"
#include "platform/Shortcut.hpp"
#include <algorithm>
#include <filesystem>
namespace foil::ui {
namespace {
constexpr const char *LedgerPath = "/switch/cookingfoil/homebrew.json";
bool fileExists(const std::string &path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}
} // namespace
// The list, asked again with its ETag each time 홈브류 shows after a refresh.
void Application::loadHomebrew() {
  if (!session_ || !connected_ || homebrewWorker_.joinable()) return;
  const auto server = session_->server();
  const auto state = state_;
  auto etag = homebrewEtag_;
  homebrewWorker_ = std::thread([this, server, state, etag]() mutable {
    std::vector<homebrew::App> apps;
    bool unchanged = false;
    Error error;
    if (!guarded("homebrew", [&] { net::homebrewList(server, state, etag, apps, unchanged, error, &stop_); }))
      error.message = tr("Something went wrong reading the homebrew list.");
    homebrewFinish_ = [this, apps = std::move(apps), unchanged, error, etag]() mutable {
      homebrewLoaded_ = true;
      homebrewError_ = error;
      if (error) return;
      homebrewEtag_ = etag;
      if (!unchanged) {
        homebrew_ = std::move(apps);
        sortShopSaves(); // apps' saves in 공유 now sort by the app's name
        matchHomebrewCard();
      }
      if (screen_ == Screen::Homebrew && sidebar_ && autoSidebar_ && hasFocusable()) sidebar_ = autoSidebar_ = false;
    };
    homebrewDone_ = true;
  });
}
void Application::pollHomebrew() {
  if (homebrewCardDone_.exchange(false)) {
    homebrewCardWorker_.join();
    homebrewCard_ = std::move(homebrewCardFound_);
    homebrewCardFound_.clear();
    homebrewScanned_ = true;
    // The shortcuts asked about: whichever the console no longer has goes.
    for (const auto &nro : homebrewCard_) shortcuts_.erase(forwarder::titleId(forwarder::loaderPath(nro.path)));
    for (const auto &app : homebrew_) shortcuts_.erase(homebrewServerShortcut(app));
    shortcuts_.insert(homebrewShortcutsFound_.begin(), homebrewShortcutsFound_.end());
    signaturePatches_ = signaturePatchesFound_;
    matchHomebrewCard();
    if (screen_ == Screen::Homebrew && sidebar_ && autoSidebar_ && hasFocusable()) sidebar_ = autoSidebar_ = false;
  }
  if (!homebrewDone_.exchange(false)) return;
  homebrewWorker_.join();
  if (homebrewFinish_) std::exchange(homebrewFinish_, nullptr)();
}
void Application::checkHomebrewCard() {
  homebrewOnCard_.clear();
  for (const auto &[id, record] : homebrewLedger_) homebrewOnCard_[record.file] = fileExists(sdRoot_ + "/" + record.file);
  scanHomebrewCard();
}
// Every .nro under switch/, read off the main thread: apps other tools or
// the player put there count as installed too.
void Application::scanHomebrewCard() {
  if (homebrewCardWorker_.joinable() || sdRoot_.empty()) return;
  const auto sd = sdRoot_;
  const auto languages = homebrew::nacpLanguages(languageCode());
  // Which apps have a HOME Menu shortcut: the one made here for each file,
  // and the server's for each listed app.
  std::vector<uint64_t> ids;
  for (const auto &app : homebrew_)
    if (const auto id = homebrewServerShortcut(app)) ids.push_back(id);
  homebrewCardWorker_ = std::thread([this, sd, languages, ids]() mutable {
    std::vector<homebrew::CardNro> found;
    std::set<uint64_t> shortcuts;
    if (!guarded("homebrew card", [&] {
          found = homebrew::scanCard(sd, languages);
          for (const auto &nro : found) ids.push_back(forwarder::titleId(forwarder::loaderPath(nro.path)));
          shortcuts = platform::presentShortcuts(ids);
          signaturePatchesFound_ = forwarder::signaturePatchesFound(sd);
        }))
      found.clear();
    homebrewCardFound_ = std::move(found);
    homebrewShortcutsFound_ = std::move(shortcuts);
    homebrewCardDone_ = true;
  });
}
// Which card file each listed app owns (the one at its install path
// first), and the files no listed app owns, as rows of their own.
void Application::matchHomebrewCard() {
  homebrewCardOf_.clear();
  homebrewLocal_.clear();
  for (size_t i = 0; i < homebrewCard_.size(); ++i) {
    const auto &nro = homebrewCard_[i];
    if (const auto *app = homebrew::matchCard(homebrew_, nro.path)) {
      const auto [at, fresh] = homebrewCardOf_.emplace(app->id, i);
      if (!fresh && nro.path == app->installPath) at->second = i;
      continue;
    }
    homebrew::App local;
    local.cardOnly = true;
    local.id = "card:" + nro.path;
    local.fileName = nro.path.substr(nro.path.rfind('/') + 1);
    local.name = nro.name.empty() ? local.fileName.substr(0, local.fileName.size() - 4) : nro.name;
    local.author = nro.author;
    local.version = nro.version;
    local.installPath = nro.path;
    local.size = nro.size;
    homebrewLocal_.push_back(std::move(local));
  }
}
const homebrew::App &Application::homebrewAt(int index) const {
  return index < int(homebrew_.size()) ? homebrew_[index] : homebrewLocal_[index - homebrew_.size()];
}
// The server's icon, else the one in the app's file on the card: the server
// answers 404 for an app whose release has no icon or that it has not
// fetched yet, and a timeout leaves the tile blank until the next refresh.
std::string Application::homebrewIcon(const homebrew::App &app) {
  const bool serverFailed = !app.iconUrl.empty() && images_->failed(app.iconUrl);
  const auto file = homebrewFile(app);
  const auto nro = std::find_if(homebrewCard_.begin(), homebrewCard_.end(),
                                [&](const homebrew::CardNro &entry) { return entry.path == file; });
  const auto key = nro == homebrewCard_.end() || nro->icon.empty() ? std::string() : "card:" + nro->path;
  const auto icon = homebrew::tileIcon(app, serverFailed, key);
  if (icon == key && !key.empty()) images_->provide(key, nro->icon);
  return icon;
}
homebrew::Status Application::homebrewStatus(const homebrew::App &app) const {
  if (app.cardOnly) return homebrew::Status::Installed;
  const auto card = homebrewCardOf_.find(app.id);
  return homebrew::status(app, homebrewLedger_, [this](const std::string &file) {
    const auto found = homebrewOnCard_.find(file);
    return found != homebrewOnCard_.end() && found->second;
  }, card == homebrewCardOf_.end() ? nullptr : &homebrewCard_[card->second].version);
}
// The app's file on the card: the ledger's while it is there, else the one
// the card scan matched, else where it would go.
std::string Application::homebrewFile(const homebrew::App &app) const {
  const auto found = homebrewLedger_.find(app.id);
  if (found != homebrewLedger_.end()) {
    const auto there = homebrewOnCard_.find(found->second.file);
    if (there != homebrewOnCard_.end() && there->second) return found->second.file;
  }
  const auto card = homebrewCardOf_.find(app.id);
  return card == homebrewCardOf_.end() ? app.installPath : homebrewCard_[card->second].path;
}
std::vector<int> Application::homebrewRows() const {
  std::vector<int> rows;
  // 설치됨: every app on the card, tools and ports alike, then the card's
  // files no listed app owns; the other tabs, each kind of the list.
  const auto kind = homebrewTab_ == 2 ? homebrew::Kind::Port : homebrew::Kind::Tool;
  for (int i = 0; i < int(homebrew_.size()); ++i) {
    const auto &app = homebrew_[i];
    if (homebrewTab_ == 0 ? homebrewStatus(app) == homebrew::Status::Available : app.kind != kind) continue;
    if (!textMatches({app.name, app.author, app.fileName}, homebrewQuery_)) continue;
    rows.push_back(i);
  }
  for (int i = 0; homebrewTab_ == 0 && i < int(homebrewLocal_.size()); ++i) {
    const auto &app = homebrewLocal_[i];
    if (textMatches({app.name, app.author, app.fileName}, homebrewQuery_)) rows.push_back(int(homebrew_.size()) + i);
  }
  return rows;
}
// The app under the cursor, the one the popups act on.
const homebrew::App *Application::homebrewPicked() const {
  const auto rows = homebrewRows();
  return rows.empty() ? nullptr : &homebrewAt(rows[std::clamp(selected_, 0, int(rows.size()) - 1)]);
}
void Application::saveHomebrewLedger() {
  std::string error;
  if (!atomicWrite(sdRoot_ + LedgerPath, homebrew::serializeLedger(homebrewLedger_), error))
    logLine("homebrew ledger: " + error);
}
// Into 다운로드's queue; its turn runs it on the retro worker, so 다운로드
// shows it running and keeps how it ended.
bool Application::getHomebrew(const homebrew::App &app) {
  if (app.cardOnly) return false; // nothing on the server to get
  uint64_t bytes = app.files.empty() ? app.size : 0;
  for (const auto &file : app.files) bytes += file.size;
  if (bytes && sdFree_ && *sdFree_ < bytes + (8u << 20)) {
    alertTitle_ = tr("Cannot download this app");
    alert_ = trf("Not enough space on the SD card. {0} is needed.", {formatBytes(bytes)});
    return false;
  }
  return queueJob("homebrew", app.name, app.iconUrl, app.downloadUrl, bytes ? bytes : app.size, homebrew::serialize(app));
}
bool Application::runHomebrew(const homebrew::App &app) {
  if (app.cardOnly) return false;
  uint64_t bytes = app.files.empty() ? app.size : 0;
  for (const auto &file : app.files) bytes += file.size;
  const auto server = session_ ? session_->server() : Server{};
  const auto state = state_;
  const auto sd = sdRoot_;
  retroBytes_ = bytes;
  retroCancel_ = false;
  if (!runRetroJob("homebrew:" + app.id, [this, server, state, sd, app]() -> std::function<void()> {
        Error error;
        const bool ok = net::homebrewDownload(server, state, app, sd, retroReceived_, error, &retroCancel_);
        const bool cancelled = retroCancel_;
        const uint64_t received = retroReceived_;
        return [this, ok, error, app, cancelled, received] {
          storageAt_ = 0;
          finishRetroEntry("homebrew", ok ? history::Outcome::Done : cancelled ? history::Outcome::Cancelled : history::Outcome::Failed,
                           ok || cancelled ? "" : error.message, received);
          if (cancelled && !ok) {
            message(trf("Cancelled the download of {0}.", {app.name}));
            return;
          }
          if (!ok) {
            alertTitle_ = tr("Could not download the app");
            alert_ = error.message;
            return;
          }
          settleHomebrew(app);
          if (autoHomebrewShortcut(app)) return;
          message(trf("Downloaded {0}. Open it from hbmenu.", {app.name}));
        };
      }))
    return false;
  return true;
}
// After a download: the ledger points at the new place, and an older copy
// elsewhere goes (the one a previous build kept, or one straight under
// switch/), so no duplicate stays. Files a folder install no longer lists
// stay put: they may be the app's settings or saves.
void Application::settleHomebrew(const homebrew::App &app) {
  std::error_code ec;
  const auto previous = homebrewLedger_.find(app.id);
  std::vector<std::string> stale{"switch/" + app.fileName};
  if (previous != homebrewLedger_.end()) stale.push_back(previous->second.file);
  // A copy the card scan found goes too when it has the same file name; a
  // differently named file in the app's folder may be something else.
  const auto card = homebrewCardOf_.find(app.id);
  const auto cardPath = card == homebrewCardOf_.end() ? std::string() : homebrewCard_[card->second].path;
  if (!cardPath.empty() && cardPath.substr(cardPath.rfind('/') + 1) == app.fileName) stale.push_back(cardPath);
  for (const auto &old : stale)
    if (old != app.installPath && fileExists(sdRoot_ + "/" + old)) {
      std::filesystem::remove(sdRoot_ + "/" + old, ec);
      homebrew::removeEmptyFolders(sdRoot_, old);
      homebrewOnCard_.erase(old);
      homebrewCard_.erase(std::remove_if(homebrewCard_.begin(), homebrewCard_.end(),
                                         [&](const homebrew::CardNro &nro) { return nro.path == old; }),
                          homebrewCard_.end());
    }
  homebrew::Record record{app.hash, app.version, app.installPath, {}};
  for (const auto &file : app.files)
    if (file.path != app.installPath) record.files.push_back(file.path);
  homebrewLedger_[app.id] = record;
  homebrewOnCard_[app.installPath] = true;
  saveHomebrewLedger();
  matchHomebrewCard();
}
// The app's files (a folder install's too), the folders they leave empty,
// and its ledger entry; files the ledger does not list stay. An app with no
// ledger entry loses the one file the card scan found, nothing else.
void Application::removeHomebrew(const homebrew::App &picked) {
  const auto app = picked; // a card row goes away with its file
  const auto file = homebrewFile(app);
  const auto found = homebrewLedger_.find(app.id);
  const bool ledgered = found != homebrewLedger_.end() && found->second.file == file;
  const bool onCard = std::any_of(homebrewCard_.begin(), homebrewCard_.end(),
                                  [&](const homebrew::CardNro &nro) { return nro.path == file; });
  if (!ledgered && !onCard) return;
  // Its shortcut would only open an error once the file is gone.
  const std::vector<uint64_t> shortcutIds{homebrewShortcutId(app), homebrewServerShortcut(app)};
  std::error_code ec;
  bool removed = true;
  if (ledgered) {
    const auto path = sdRoot_ + "/" + file;
    std::filesystem::remove(path, ec);
    removed = !ec || !fileExists(path);
  } else {
    std::string error;
    removed = homebrew::removeCardFile(sdRoot_, file, error);
    if (!removed) logLine("homebrew remove: " + error);
  }
  if (!removed) {
    alert_ = trf("Could not delete the file: {0}", {file});
    return;
  }
  if (ledgered) {
    for (const auto &extra : found->second.files) {
      std::filesystem::remove(sdRoot_ + "/" + extra, ec);
      homebrew::removeEmptyFolders(sdRoot_, extra);
    }
    homebrew::removeEmptyFolders(sdRoot_, file);
  }
  if (found != homebrewLedger_.end()) {
    homebrewOnCard_.erase(found->second.file);
    homebrewLedger_.erase(found);
    saveHomebrewLedger();
  }
  removeShortcuts(shortcutIds, app.name);
  homebrewCard_.erase(std::remove_if(homebrewCard_.begin(), homebrewCard_.end(),
                                     [&](const homebrew::CardNro &nro) { return nro.path == file; }),
                      homebrewCard_.end());
  matchHomebrewCard();
  message(trf("Removed {0}.", {app.name}));
}
void Application::homebrewAction(Action a) {
  const auto rows = homebrewRows();
  const auto *picked = homebrewPicked();
  // Back to the app's card either way, its shortcut row showing the result.
  if (shortcutRemoveAsk_) {
    if (a == Action::Back) {
      shortcutRemoveAsk_ = false;
      homebrewAsk_ = true;
    } else if (a == Action::Accept && settled()) {
      shortcutRemoveAsk_ = false;
      homebrewAsk_ = true;
      if (picked) removeHomebrewShortcut(*picked);
    }
    return;
  }
  if (homebrewRemoveAsk_) {
    if (a == Action::Back) homebrewRemoveAsk_ = false;
    else if (a == Action::Accept && settled()) {
      homebrewRemoveAsk_ = false;
      if (picked) removeHomebrew(*picked);
    }
    return;
  }
  if (homebrewAsk_ && (homebrewExtras_ || homebrewSaveAsk_)) {
    homebrewExtrasAction(a);
    return;
  }
  if (homebrewAsk_) {
    // Up and down move between the app and its rows: 패치, 세이브, the
    // HOME Menu shortcut and 지우기.
    const int extras = picked ? int(homebrewExtraKinds(*picked).size()) : 0;
    homebrewAskRow_ = std::clamp(homebrewAskRow_, -1, extras - 1);
    // An installed app has nothing to get: its rows start at the first one.
    const int first = picked && homebrewStatus(*picked) == homebrew::Status::Installed && extras ? 0 : -1;
    if (a == Action::Back) homebrewAsk_ = false;
    else if (a == Action::Up) homebrewAskRow_ = std::max(first, homebrewAskRow_ - 1);
    else if (a == Action::Down) homebrewAskRow_ = std::min(extras - 1, homebrewAskRow_ + 1);
    else if (a == Action::Accept && settled() && picked) {
      const auto kinds = homebrewExtraKinds(*picked);
      if (homebrewAskRow_ >= 0 && kinds[homebrewAskRow_] == 3) { // 지우기 asks first
        homebrewAsk_ = false;
        homebrewRemoveAsk_ = true;
      } else if (homebrewAskRow_ >= 0 && kinds[homebrewAskRow_] == 4) {
        // The shortcut row makes one, or asks before taking one away.
        if (homebrewHasShortcut(*picked)) {
          homebrewAsk_ = false;
          shortcutRemoveAsk_ = true;
        } else
          requestHomebrewShortcut(*picked); // the card stays, its row says 있음 or 대기 중
      } else if (homebrewAskRow_ >= 0) {
        homebrewExtras_ = kinds[homebrewAskRow_];
        homebrewExtraRow_ = 0;
      } else if (homebrewStatus(*picked) != homebrew::Status::Installed) {
        homebrewAsk_ = false;
        getHomebrew(*picked);
      }
    }
    return;
  }
  if (a == Action::Previous || a == Action::Next) {
    homebrewTab_ = (homebrewTab_ + (a == Action::Next ? 1 : 2)) % 3;
    selected_ = 0;
    return;
  }
  if (a == Action::Search) {
    requestSearch();
    return;
  }
  // Rows of six, as on 내 게임.
  const int count = int(rows.size());
  if (a == Action::Up) selected_ = std::max(0, selected_ - 6);
  if (a == Action::Down) selected_ = std::min(count - 1, selected_ + 6);
  if (a == Action::Left) {
    if (selected_ % 6 == 0) sidebar_ = true;
    else --selected_;
  }
  if (a == Action::Right) selected_ = std::min(count - 1, selected_ + 1);
  selected_ = std::max(0, selected_);
  // The app's popup first, whatever its state: its patches and saves are there.
  if (a == Action::Accept && picked) {
    homebrewAsk_ = true;
    homebrewAskRow_ = homebrewStatus(*picked) == homebrew::Status::Installed ? 0 : -1;
    homebrewExtras_ = 0;
    homebrewSaveAsk_ = false;
  }
}
void Application::homebrewScreen() {
  if (!homebrewLoaded_ && !homebrewWorker_.joinable() && connected_) loadHomebrew();
  // An app's patches and saves come from 유저 패치's and 세이브's lists.
  if (homebrewLoaded_ && connected_) {
    if (!patchesLoaded_ && !patchFetching_ && !patchError_) fetchPatches();
    if (!shopSavesLoaded_ && !shopSavesWorker_.joinable()) loadShopSaves();
  }
  const auto rows = homebrewRows();
  int updates[3] = {0, 0, 0}; // 설치됨, 도구, 포트
  for (const auto &app : homebrew_) {
    const bool update = homebrewStatus(app) == homebrew::Status::Update;
    updates[0] += update;
    updates[app.kind == homebrew::Kind::Port ? 2 : 1] += update;
  }
  // As on 유저 패치: search at the right end, the tabs left of it.
  searchBar(homebrewQuery_);
  // Each tab counts the new versions in it (a badge, when there is one).
  const float tabsLeft = headerTabs({tr("Installed|tab"), tr("Tools"), tr("Ports")}, homebrewTab_, [this](int n) {
    homebrewTab_ = n;
    selected_ = 0;
  }, SearchX - 16, 0, {updates[0], updates[1], updates[2]});
  pageTitle(tr("Homebrew"), homebrewLoaded_ ? countLabel(rows.size(), homebrewQuery_) : std::string(), tabsLeft);
  if (!connected_) {
    notice(tr("Not connected to a server."), tr("Connect to the server to see its homebrew."), 21);
    return;
  }
  if (!homebrewLoaded_) {
    paint_.skeletonCards(260, 104, 6);
    return;
  }
  if (homebrewError_) {
    notice(tr("Could not load the homebrew list."), homebrewError_.message, 25);
    return;
  }
  if (rows.empty()) {
    if (!homebrewQuery_.empty())
      notice(trf("No homebrew matches '{0}'.", {homebrewQuery_}), tr("Press B to clear the search."), 23);
    else if (homebrewTab_ == 0 && !homebrewScanned_)
      paint_.skeletonCards(260, 104, 6);
    else if (homebrewTab_ == 0)
      notice(tr("No homebrew installed."), tr("Get one from the Tools or Ports tab and it shows here."), 22);
    else if (homebrewTab_ == 2)
      notice(tr("The server has no ports."), tr("Ports added on the server show up here."), 22);
    else
      notice(tr("The server has no tools."), tr("Apps added on the server show up here."), 22);
    return;
  }
  // A grid of icons like 내 게임: the state on the icon, the size under the name.
  selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
  // The row at the top follows the cursor, or the finger while it glides.
  const int rowCount = int((rows.size() + 5) / 6);
  const float top = glide("homebrew", 232, 104 - 8, 1048, 660 - 104 + 8, float(std::max(0, selected_ / 6 - 1)),
                          float(rowCount - 2), 240);
  const int first = int(top) * 6;
  paint_.scrollbar(1266, 104, 500, top, 2.1f, float(rowCount));
  clipTo(232, 104 - 8, 1048, 660 - 104 + 8);
  for (int n = 0; n < 24 && first + n < int(rows.size()); ++n) {
    const int i = first + n;
    const auto &app = homebrewAt(rows[i]);
    const float x = 260 + n % 6 * 170, y = 104 + (n / 6 + int(top) - top) * 240;
    hit(x, y, 150, 220, [this, i] {
      if (inContent() && selected_ == i) action(Action::Accept);
      sidebar_ = false;
      selected_ = i;
    });
    cover(homebrewIcon(app), x, y, 150, inContent() && selected_ == i, app.name, true);
    const auto state = homebrewStatus(app);
    const bool busy = retroBusy_ == "homebrew:" + app.id;
    if (busy || state != homebrew::Status::Available)
      paint_.pill(busy ? tr("Downloading") : state == homebrew::Status::Update ? tr("Update|item") : tr("Installed"), x + 8, y + 8);
    if (inContent() && selected_ == i) marquee(app.name, x, y + 158, 18, 150, false);
    else paint_.fonts.text(app.name, x, y + 158, 18, Ink, false, 150);
    // Size first; the version beside it only when both fit under the icon.
    float px = x;
    if (app.size) px += paint_.pill(formatBytes(app.size), px, y + 194) + 6;
    const auto version = "v" + app.version;
    if (!app.version.empty() && px + paint_.fonts.measure(version, 15, true) + 18 <= x + 150)
      paint_.pill(version, px, y + 194);
  }
  unclip();
}
// Before it comes down: the app's icon, name and facts, and its description
// when the server has one (without, the card stays short).
void Application::homebrewAskDialog() {
  const auto *picked = homebrewPicked();
  if (!picked) {
    homebrewAsk_ = false;
    homebrewExtras_ = 0;
    homebrewSaveAsk_ = false;
    return;
  }
  const auto &app = *picked;
  const auto status = homebrewStatus(app);
  const bool update = status == homebrew::Status::Update, installed = status == homebrew::Status::Installed;
  const auto extras = homebrewExtraKinds(app);
  const bool hasShortcut = homebrewHasShortcut(app);
  constexpr float Icon = 160, Width = 820, Row = 56, Gap = 10;
  const float textW = Width - 64 - Icon - 28;
  // Measured off screen first, so the card hugs the description.
  constexpr int Lines = 6;
  // A card file no listed app owns says so where a description would be.
  const std::string description = app.cardOnly ? tr("This app isn't on the server's list, so it can't be downloaded or updated here.") : app.description;
  const float described = description.empty() ? 0
                        : paint_.fonts.wrapped(description, -9999, -9999, 18, Ink, textW, Lines);
  const float top = 24 + std::max(Icon, 106 + described);
  const float bodyH = top + (extras.empty() ? 0 : 16 + extras.size() * (Row + Gap) - Gap) + 24;
  // A focused 패치 or 세이브 row opens its list; otherwise A acts on the app.
  const auto accept = homebrewAskRow_ >= 0 ? tr("Select") : update ? tr("Update|action") : tr("Get");
  const auto body = modal(Width, bodyH, installed ? tr("Installed app") : update ? tr("Update it?") : tr("Get this app?"),
                          installed ? tr("Close") : tr("Cancel"), "", accept, 22);
  const float x = body.x + 32, y = body.y + 24, textX = x + Icon + 28;
  cover(homebrewIcon(app), x, y, int(Icon), false, app.name, true);
  marquee(app.name, textX, y, 24, textW);
  std::string facts = app.version.empty() ? "" : "v" + app.version;
  if (!app.author.empty()) facts += (facts.empty() ? "" : " · ") + app.author;
  if (app.size) facts += (facts.empty() ? "" : " · ") + formatBytes(app.size);
  paint_.fonts.text(facts, textX, y + 38, 18, Ink, true, textW);
  std::string where = homebrewFile(app);
  if (app.releasedAt.size() >= 10) where = app.releasedAt.substr(0, 10) + " · " + where;
  paint_.fonts.text(where, textX, y + 68, 16, Secondary, false, textW);
  if (described) paint_.fonts.wrapped(description, textX, y + 106, 18, app.cardOnly ? Secondary : Ink, textW, Lines);
  // One row each for the app's patches and saves, with how many there are.
  float rowY = body.y + top + 16;
  const float rowW = body.w - 64;
  for (int i = 0; i < int(extras.size()); ++i, rowY += Row + Gap) {
    hit(x, rowY, rowW, Row, [this, i] {
      homebrewAskRow_ = i;
      action(Action::Accept);
    });
    if (i == homebrewAskRow_ && !homebrewExtras_) paint_.focusRow(x, rowY, rowW, Row, Ground, 12);
    else paint_.rect(x, rowY, rowW, Row, Ground, 12);
    if (extras[i] == 4) { // the HOME Menu shortcut, and whether there is one
      // Dimmed when A cannot do anything with it.
      const auto ink = !hasShortcut && !homebrewShortcutPossible(app) ? Secondary : Ink;
      paint_.icon(0, x + 20, rowY + 15, ink, 26);
      paint_.fonts.middle(tr("HOME Menu shortcut"), x + 62, rowY + Row / 2, 20, ink, true);
      // Judged ahead: whether one can be made here, and whether it would open.
      // Unavailable only when neither this console nor the server can make
      // one; one waiting in 다운로드 says so.
      const bool blocked = !hasShortcut && !homebrewShortcutPossible(app);
      const bool local = platform::canMakeShortcuts() && homebrew::cardNroPath(homebrewFile(app));
      const bool unsigned_ = !hasShortcut && !blocked && local && !signaturePatches_;
      const bool queued = std::any_of(installQueue_.begin(), installQueue_.end(), [&](const QueuedInstall &q) {
        return (q.kind == "shortcut" && q.name == trf("{0} shortcut", {app.name})) ||
               (!app.forwarderUrl.empty() && q.file.url == app.forwarderUrl);
      });
      const std::string state = hasShortcut ? tr("Made") : queued ? tr("Queued") : blocked ? tr("Unavailable")
                                : unsigned_ ? tr("Needs sig patches") : tr("None");
      paint_.fonts.middle(state, x + rowW - 20 - paint_.fonts.measure(state, 17, true), rowY + Row / 2, 17,
                          hasShortcut ? Good : unsigned_ ? Pending : Secondary, true);
      continue;
    }
    if (extras[i] == 3) { // 지우기, in the colour removal uses everywhere
      paint_.icon(37, x + 20, rowY + 15, Bad, 26);
      paint_.fonts.middle(tr("Delete"), x + 62, rowY + Row / 2, 20, Bad, true);
      continue;
    }
    const bool patches = extras[i] == 1;
    paint_.icon(patches ? 32 : 36, x + 20, rowY + 15, Ink, 26);
    const float labelEnd = paint_.fonts.middle(patches ? tr("Patches") : tr("Saves"), x + 62, rowY + Row / 2, 20, Ink, true);
    int count = 0, on = 0;
    if (patches)
      for (const auto &row : homebrewPatchRows(app)) ++count, on += row.record && row.record->on;
    else
      count = int(homebrewSaveRows(app).size());
    paint_.pill(std::to_string(count), labelEnd + 10, rowY + Row / 2 - 13);
    if (on) {
      const auto state = trf("{0} on", {std::to_string(on)});
      paint_.fonts.middle(state, x + rowW - 20 - paint_.fonts.measure(state, 17, true), rowY + Row / 2, 17, Good, true);
    }
  }
}
void Application::homebrewRemoveDialog() {
  const auto rows = homebrewRows();
  if (rows.empty()) {
    homebrewRemoveAsk_ = false;
    return;
  }
  const auto &app = homebrewAt(rows[std::clamp(selected_, 0, int(rows.size()) - 1)]);
  const bool shortcut = homebrewHasShortcut(app);
  const auto body = modal(700, 24 + 70 + (shortcut ? 30 : 0) + 16, tr("Remove this app?"), tr("Cancel"), "",
                          tr("Delete"), 37);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(app.name, x, y, 21, Ink, true, body.w - 64);
  paint_.fonts.text(trf("Removes {0} from the SD card.", {homebrewFile(app)}), x, y + 38, 19, Secondary,
                    false, body.w - 64);
  if (shortcut)
    paint_.fonts.text(tr("Its HOME Menu shortcut is deleted too."), x, y + 68, 19, Secondary, false, body.w - 64);
}
} // namespace foil::ui
