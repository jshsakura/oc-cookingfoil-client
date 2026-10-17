#include "ui/Application.hpp"
#include "core/Forwarder.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/Sha256.hpp"
#include "net/HomebrewApi.hpp"
#include "platform/Cores.hpp"
#include "platform/Shortcut.hpp"
#include <algorithm>
#include <filesystem>
#include <utility>
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
  const bool titles = features_.count("homebrew-titles") > 0;
  homebrewWorker_ = std::thread([this, server, state, etag, titles]() mutable {
    const platform::OnCore off(platform::NetworkCore); // the drawing core keeps the screen moving
    std::vector<homebrew::App> apps;
    bool unchanged = false;
    Error error;
    if (!guarded("homebrew", [&] { net::homebrewList(server, state, etag, apps, unchanged, error, &stop_, titles); }))
      error.message = tr("Something went wrong reading the homebrew list.");
    homebrewFinish_ = [this, apps = std::move(apps), unchanged, error, etag, shown = homebrewLoaded_ && !homebrew_.empty()]() mutable {
      homebrewLoaded_ = true;
      // A look again that failed keeps the list it had: said in the log only.
      if (error && shown) {
        logLine("homebrew: list kept, " + error.message);
        error = {};
        unchanged = true;
        etag = homebrewEtag_;
      }
      homebrewError_ = error;
      // A row in 다운로드 asked for an app's page before the list was here.
      const auto wanted = std::exchange(homebrewOpenId_, std::string());
      if (error) return;
      homebrewEtag_ = etag;
      if (!unchanged) {
        // The cursor (and an open page) stays on the same app when the new
        // list puts it somewhere else.
        const auto *was = screen_ == Screen::Homebrew ? homebrewPicked() : nullptr;
        const auto keep = was ? was->id : std::string();
        homebrew_ = std::move(apps);
        sortShopSaves(); // apps' saves in 공유 now sort by the app's name
        matchHomebrewCard();
        // Its server shortcuts, asked about once the card was looked at.
        if (homebrewScanned_ && shortcutsUnasked()) scanHomebrewCard();
        if (!keep.empty()) {
          const auto rows = homebrewRows();
          for (int n = 0; n < int(rows.size()); ++n)
            if (homebrewAt(rows[n]).id == keep) selected_ = n;
        }
      }
      if (screen_ == Screen::Homebrew && sidebar_ && autoSidebar_ && hasFocusable()) sidebar_ = autoSidebar_ = false;
      if (!wanted.empty() && screen_ == Screen::Homebrew && !openHomebrewPage(wanted)) {
        alertTitle_.clear();
        alert_ = tr("The file no longer exists on the server.");
      }
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
    takeHomebrewLedgerCheck();
    // The shortcuts asked about: whichever the console no longer has goes.
    // Only those: a list that came while it looked was not asked about.
    for (const auto id : homebrewShortcutsAsked_) shortcuts_.erase(id);
    shortcuts_.insert(homebrewShortcutsFound_.begin(), homebrewShortcutsFound_.end());
    signaturePatches_ = signaturePatchesFound_;
    matchHomebrewCard();
    if (shortcutsUnasked()) scanHomebrewCard(); // the list came while it looked
    if (screen_ == Screen::Homebrew && sidebar_ && autoSidebar_ && hasFocusable()) sidebar_ = autoSidebar_ = false;
  }
  if (!homebrewDone_.exchange(false)) return;
  homebrewWorker_.join();
  if (homebrewFinish_) std::exchange(homebrewFinish_, nullptr)();
}
// A listed app's server shortcut the last look did not ask the console about.
bool Application::shortcutsUnasked() const {
  const std::set<uint64_t> asked(homebrewShortcutsAsked_.begin(), homebrewShortcutsAsked_.end());
  return std::any_of(homebrew_.begin(), homebrew_.end(), [&](const homebrew::App &app) {
    const auto id = homebrewServerShortcut(app);
    return id && !asked.count(id);
  });
}
void Application::checkHomebrewCard() {
  // Before the first scan only: after it, what it found stands until the
  // next one answers, with no stat per app on the drawing thread.
  if (!homebrewScanned_) {
    homebrewOnCard_.clear();
    for (const auto &[id, record] : homebrewLedger_) homebrewOnCard_[record.file] = fileExists(sdRoot_ + "/" + record.file);
  }
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
  homebrewLedgerLooked_ = homebrewLedger_;
  homebrewCardWorker_ = std::thread([this, sd, languages, ids, ledger = homebrewLedgerLooked_, known = homebrewCard_]() mutable {
    const platform::OnCore off(platform::WriterCore); // every .nro read off the drawing core
    std::vector<homebrew::CardNro> found;
    std::set<uint64_t> shortcuts;
    homebrew::LedgerCheck check;
    if (!guarded("homebrew card", [&] {
          found = homebrew::scanCard(sd, languages, known);
          // The ledger against the card: a stat each, a hash only where it
          // differs from what was seen.
          check = homebrew::checkLedger(
              ledger, [&](const std::string &file) { return homebrew::cardFile(sd + "/" + file); },
              [&](const std::string &file) { return sha256File(sd + "/" + file); },
              [&](const std::string &file) {
                const auto nro = std::find_if(found.begin(), found.end(),
                                              [&](const homebrew::CardNro &n) { return n.path == file; });
                return nro == found.end() ? std::string() : nro->version;
              });
          for (const auto &nro : found) ids.push_back(forwarder::titleId(forwarder::loaderPath(nro.path)));
          shortcuts = platform::presentShortcuts(ids);
          signaturePatchesFound_ = forwarder::signaturePatchesFound(sd);
        }))
      found.clear();
    homebrewCardFound_ = std::move(found);
    homebrewLedgerFound_ = std::move(check);
    homebrewShortcutsFound_ = std::move(shortcuts);
    homebrewShortcutsAsked_ = ids;
    homebrewCardDone_ = true;
  });
}
// What the check found, for the records not changed here since it began
// (a download settled meanwhile is newer than the look).
void Application::takeHomebrewLedgerCheck() {
  const auto check = std::exchange(homebrewLedgerFound_, {});
  const auto unchanged = [&](const std::string &id) {
    const auto now = homebrewLedger_.find(id);
    const auto then = homebrewLedgerLooked_.find(id);
    return now != homebrewLedger_.end() && then != homebrewLedgerLooked_.end() && now->second == then->second;
  };
  for (const auto &[id, record] : homebrewLedgerLooked_)
    if (unchanged(id))
      if (const auto on = check.onCard.find(record.file); on != check.onCard.end()) homebrewOnCard_[record.file] = on->second;
  bool changed = false;
  for (const auto &[id, record] : check.fixed)
    if (unchanged(id)) {
      homebrewLedger_[id] = record;
      changed = true;
    }
  for (const auto &id : check.followed)
    logLine("homebrew " + id + ": the card's file is not the one installed; the ledger follows the card");
  if (check.hashed) logLine("homebrew ledger: hashed " + std::to_string(check.hashed) + " file(s) whose size or time changed");
  homebrewLedgerLooked_.clear();
  if (changed) saveHomebrewLedger();
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
// An app on the card shows the icon in its file there, as the HOME Menu
// and other launchers do; the server's only for one not on the card.
std::string Application::homebrewIcon(const homebrew::App &app) {
  const auto file = homebrewFile(app);
  const auto nro = std::find_if(homebrewCard_.begin(), homebrewCard_.end(),
                                [&](const homebrew::CardNro &entry) { return entry.path == file; });
  const auto key = nro == homebrewCard_.end() || nro->icon.empty() ? std::string() : "card:" + nro->path;
  const auto icon = homebrew::tileIcon(app, nro != homebrewCard_.end(), key);
  if (icon == key && !key.empty()) images_->provide(key, nro->icon);
  return icon;
}
homebrew::Status Application::homebrewStatus(const homebrew::App &app) const {
  if (app.cardOnly) return homebrew::Status::Installed;
  // A title: what the console has installed, by its version.
  if (app.title) {
    const auto *game = homebrewInstalledTitle(app);
    return game ? homebrew::cardStatus(app.version, game->displayVersion) : homebrew::Status::Available;
  }
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
  homebrew::sortRows(homebrew_, rows, homebrewSort_); // the server's order is the newest first
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
  bool closing;
  {
    std::lock_guard<std::mutex> held(historyMutex_);
    closing = historyClosing_;
    ledgerPending_ = homebrew::serializeLedger(homebrewLedger_);
  }
  if (closing)
    writePendingLedger(); // the writer is gone, so straight to the card
  else
    wakeHistoryWriter();
}
void Application::writePendingLedger() {
  std::optional<std::string> text;
  {
    std::lock_guard<std::mutex> held(historyMutex_);
    text = std::exchange(ledgerPending_, std::nullopt);
  }
  std::string error;
  if (text && !atomicWrite(sdRoot_ + LedgerPath, *text, error))
    logLine("homebrew ledger: " + error);
}
// Into 다운로드's queue; its turn runs it on the retro worker, so 다운로드
// shows it running and keeps how it ended.
const InstalledGame *Application::homebrewInstalledTitle(const homebrew::App &app) const {
  if (!app.title) return nullptr;
  const auto found = std::find_if(installed_.games.begin(), installed_.games.end(),
                                  [&](const InstalledGame &g) { return g.titleId == app.titleId; });
  return found == installed_.games.end() ? nullptr : &*found;
}
bool Application::getHomebrew(const homebrew::App &app) {
  if (app.cardOnly) return false; // nothing on the server to get
  // A title goes in as a game's NSP does, where games go.
  if (app.title) {
    Item file;
    file.url = app.downloadUrl;
    file.name = app.name;
    file.icon = app.iconUrl;
    file.titleId = file.baseId = app.titleId;
    file.size = app.size;
    file.versionName = app.version;
    file.format = "nsp"; // its address ends in /download
    file.requiredFirmware = app.requiredFirmware; // checked as a game's is
    if (auto error = startInstall({file}, app.name, app.iconUrl, installTarget())) {
      alertTitle_ = tr("Cannot download this app");
      alert_ = error.message;
      return false;
    }
    return true;
  }
  uint64_t bytes = app.files.empty() ? app.size : 0;
  for (const auto &file : app.files) bytes += file.size;
  // A bundle's bytes are the tar's (headers and padding too), a zip's its own.
  if (!app.bundleUrl.empty()) bytes = homebrew::bundleSize(app);
  if (app.archive.usable()) bytes = app.archive.size;
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
  if (!app.bundleUrl.empty()) bytes = homebrew::bundleSize(app);
  if (app.archive.usable()) bytes = app.archive.size;
  const auto server = session_ ? session_->server() : Server{};
  const auto state = state_;
  const auto sd = sdRoot_;
  retroBytes_ = bytes;
  retroCancel_ = false;
  if (!runRetroJob("homebrew:" + app.id, [this, server, state, sd, app, stale = homebrewStale(app)]() -> std::function<void()> {
        Error error;
        const bool ok = net::homebrewDownload(server, state, app, sd, retroReceived_, error, &retroCancel_, &retroKept_, &homebrewCheck_);
        // The older copies go here, off the drawing thread.
        std::vector<std::string> removed;
        for (const auto &old : ok ? stale : std::vector<std::string>{})
          if (fileExists(sd + "/" + old)) {
            std::error_code ec;
            std::filesystem::remove(sd + "/" + old, ec);
            homebrew::removeEmptyFolders(sd, old);
            removed.push_back(old);
          }
        const bool cancelled = retroCancel_;
        const uint64_t received = retroReceived_, kept = retroKept_;
        // The new file as written, so the next card check needs no hash.
        const auto seen = ok ? homebrew::cardFile(sd + "/" + app.installPath) : std::nullopt;
        const auto fileSha = seen ? sha256File(sd + "/" + app.installPath) : std::string();
        return [this, ok, error, app, cancelled, received, kept, seen, fileSha, removed] {
          cardChanged();
          if (finishRetroEntry("homebrew", ok ? history::Outcome::Done : cancelled ? history::Outcome::Cancelled : history::Outcome::Failed,
                           ok || cancelled ? "" : error.message, received, kept))
            return;
          if (cancelled && !ok) {
            message(trf("Cancelled the download of {0}.", {app.name}));
            return;
          }
          if (!ok) {
            alertTitle_ = tr("Could not download the app");
            alert_ = error.message;
            return;
          }
          settleHomebrew(app, seen, fileSha, removed);
          if (autoHomebrewShortcut(app)) return;
          // Why it ended so soon: only the changed files came.
          message(trf("Downloaded {0}. Open it from hbmenu.", {app.name}) +
                  (kept ? " " + trf("{0} already on the card was kept.", {formatBytes(kept)}) : std::string()));
        };
      }))
    return false;
  return true;
}
// After a download: the ledger points at the new place, and an older copy
// elsewhere goes (the one a previous build kept, or one straight under
// switch/), so no duplicate stays. Files a folder install no longer lists
// stay put: they may be the app's settings or saves.
std::vector<std::string> Application::homebrewStale(const homebrew::App &app) const {
  std::vector<std::string> stale{"switch/" + app.fileName};
  if (const auto previous = homebrewLedger_.find(app.id); previous != homebrewLedger_.end()) stale.push_back(previous->second.file);
  // A copy the card scan found goes too when it has the same file name; a
  // differently named file in the app's folder may be something else.
  const auto card = homebrewCardOf_.find(app.id);
  const auto cardPath = card == homebrewCardOf_.end() ? std::string() : homebrewCard_[card->second].path;
  if (!cardPath.empty() && cardPath.substr(cardPath.rfind('/') + 1) == app.fileName) stale.push_back(cardPath);
  stale.erase(std::remove(stale.begin(), stale.end(), app.installPath), stale.end());
  return stale;
}
// The worker deleted the older copies already; here only what the screens
// know follows.
void Application::settleHomebrew(const homebrew::App &app, const std::optional<homebrew::CardFile> &seen,
                                 const std::string &fileSha, const std::vector<std::string> &removed) {
  for (const auto &old : removed) {
    homebrewOnCard_.erase(old);
    homebrewCard_.erase(std::remove_if(homebrewCard_.begin(), homebrewCard_.end(),
                                       [&](const homebrew::CardNro &nro) { return nro.path == old; }),
                        homebrewCard_.end());
  }
  homebrew::Record record{app.hash, app.version, app.installPath, {}, seen.value_or(homebrew::CardFile{}), fileSha, {}};
  // The app came again; its HOME Menu shortcut did not change with it.
  if (const auto before = homebrewLedger_.find(app.id); before != homebrewLedger_.end())
    record.shortcutKey = before->second.shortcutKey;
  for (const auto &file : app.files)
    if (file.path != app.installPath) record.files.push_back(file.path);
  for (const auto &entry : app.archive.entries)
    if (entry.path != app.installPath) record.files.push_back(entry.path);
  homebrewLedger_[app.id] = record;
  homebrewOnCard_[app.installPath] = true;
  saveHomebrewLedger();
  matchHomebrewCard();
}
// The app's files (a folder install's too), the folders they leave empty,
// and its ledger entry; files the ledger does not list stay. An app with no
// ledger entry loses the one file the card scan found, nothing else.
void Application::removeHomebrew(const homebrew::App &picked) {
  // A title is taken off the console as a game is; 다운로드's list of
  // installed titles follows.
  if (picked.title) {
    const auto *game = homebrewInstalledTitle(picked);
    if (!game) return;
    if (deviceBusy_) {
      message(tr("Try again in a moment."));
      return;
    }
    RemovalRequest request;
    request.applicationId = game->applicationId;
    removal_ = request;
    removalAt_ = SDL_GetTicks();
    performRemoval();
    if (!deviceBusy_) removal_.reset(); // refused (an install is running)
    return;
  }
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
    // The page's panel: its rows (패치, 세이브, the HOME Menu shortcut,
    // 삭제), then the button below them (-1).
    const int extras = picked ? int(homebrewExtraKinds(*picked).size()) : 0;
    homebrewAskRow_ = std::clamp(homebrewAskRow_, -1, extras - 1);
    if (a == Action::Back) {
      homebrewAsk_ = false;
      if (homebrewReturn_ != Screen::Homebrew) screen(std::exchange(homebrewReturn_, Screen::Homebrew));
    } else if (a == Action::Up) homebrewAskRow_ = homebrewAskRow_ < 0 ? extras - 1 : std::max(0, homebrewAskRow_ - 1);
    else if (a == Action::Down) homebrewAskRow_ = homebrewAskRow_ < 0 || homebrewAskRow_ >= extras - 1 ? -1 : homebrewAskRow_ + 1;
    else if (a == Action::Preview && picked && !picked->screenshots.empty()) {
      galleryOpen_ = true;
      screenshot_ = 0;
    } else if (a == Action::Accept && settled() && picked && homebrewAskRow_ < 0) homebrewButton(*picked);
    else if (a == Action::Accept && settled() && picked) {
      const auto kinds = homebrewExtraKinds(*picked);
      if (homebrewAskRow_ >= 0 && kinds[homebrewAskRow_] == 3) { // 지우기 asks first
        homebrewAsk_ = false;
        homebrewRemoveAsk_ = true;
      } else if (homebrewAskRow_ >= 0 && kinds[homebrewAskRow_] == 4) {
        // The shortcut row makes one, installs it again when the server
        // rebuilt it, or asks before taking one away.
        if (homebrewShortcutStale(*picked))
          requestHomebrewShortcut(*picked);
        else if (homebrewHasShortcut(*picked)) {
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
  if (a == Action::Queue) {
    openHomebrewSort();
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
  // The app's page, whatever its state: its patches and saves are there.
  if (a == Action::Accept && picked) {
    homebrewReturn_ = Screen::Homebrew;
    descriptionScroll_ = 0;
    descriptionFocus_ = false;
    unglide();
    homebrewAsk_ = true;
    homebrewAskRow_ = homebrewStatus(*picked) == homebrew::Status::Installed ? 0 : -1;
    homebrewExtras_ = 0;
    homebrewSaveAsk_ = false;
  }
}
void Application::openHomebrewSort() {
  homebrewSortAsk_ = true;
  const auto at = std::find(std::begin(homebrew::Orders), std::end(homebrew::Orders), homebrewSort_);
  homebrewSortRow_ = at == std::end(homebrew::Orders) ? 0 : int(at - std::begin(homebrew::Orders));
}
// A picks the order at once and closes, as the language popup does.
void Application::homebrewSortAction(Action a) {
  constexpr int Count = int(std::size(homebrew::Orders));
  if (a == Action::Back || a == Action::Queue) homebrewSortAsk_ = false;
  else if (a == Action::Up) homebrewSortRow_ = std::max(0, homebrewSortRow_ - 1);
  else if (a == Action::Down) homebrewSortRow_ = std::min(Count - 1, homebrewSortRow_ + 1);
  else if (a == Action::Accept) {
    homebrewSort_ = homebrew::Orders[homebrewSortRow_];
    homebrewSortAsk_ = false;
    selected_ = 0;
  }
}
void Application::homebrewSortDialog() {
  constexpr float Width = 460, Row = 56, Gap = 6;
  constexpr int Count = int(std::size(homebrew::Orders));
  const auto body = modal(Width, 16 + Count * (Row + Gap) + 10, tr("Sort"), tr("Close"), "", tr("Select"), 10);
  const float x = body.x + 24, w = body.w - 48;
  for (int n = 0; n < Count; ++n) {
    const float y = body.y + 16 + n * (Row + Gap), mid = y + Row / 2;
    const bool on = homebrew::Orders[n] == homebrewSort_;
    hit(x, y, w, Row, [this, n] {
      homebrewSortRow_ = n;
      action(Action::Accept);
    });
    if (n == homebrewSortRow_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Surface, 12);
    // A radio like the filter popup's sorts.
    paint_.rect(x + 16, mid - 10, 20, 20, on ? Ink : SDL_Color{176, 174, 167, 255}, 10);
    paint_.rect(x + 18, mid - 8, 16, 16, on ? Butter : Surface, 8);
    if (on) paint_.rect(x + 22, mid - 4, 8, 8, Ink, 4);
    paint_.fonts.middle(sortLabel(homebrew::Orders[n]), x + 50, mid, 20, Ink, on, w - 66);
  }
}
void Application::homebrewScreen() {
  if (!homebrewLoaded_ && !homebrewWorker_.joinable() && connected_) loadHomebrew();
  if (homebrewPageOpen()) {
    homebrewDetail();
    return;
  }
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
  // Left of the tabs, the order: Y or a tap picks another.
  const std::string order = sortLabel(homebrewSort_);
  const float sortW = paint_.keyLabelWidth("Y", order, 18, true) + 66, sortX = tabsLeft - 12 - sortW;
  headerItem("sort", sortX, 24, sortW, 48, [this] { openHomebrewSort(); });
  paint_.rect(sortX, 24, sortW, 48, Surface, 12);
  paint_.key("Y", sortX + 18, 48);
  paint_.icon(10, sortX + 60 - 4, 38, Ink, 20);
  paint_.fonts.middle(order, sortX + 84, 48, 18, Ink, true, sortW - 100);
  pageTitle(tr("Homebrew"), homebrewLoaded_ ? countLabel(rows.size(), homebrewQuery_) : std::string(), sortX);
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
  paint_.fonts.text(app.title ? std::string(tr("Removes it from the console and the HOME Menu. User saves are kept."))
                              : trf("Removes {0} from the SD card.", {homebrewFile(app)}),
                    x, y + 38, 19, Secondary, false, body.w - 64);
  if (shortcut)
    paint_.fonts.text(tr("Its HOME Menu shortcut is deleted too."), x, y + 68, 19, Secondary, false, body.w - 64);
}
} // namespace foil::ui
