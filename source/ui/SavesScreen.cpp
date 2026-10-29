#include "core/Json.hpp"
#include "platform/Cores.hpp"
#include "ui/Application.hpp"
#include "core/Log.hpp"
#include "net/ServerSavesApi.hpp"
#include "platform/Installed.hpp"
#include "platform/Saves.hpp"
#include <algorithm>
#include <filesystem>
namespace foil::ui {
namespace {
constexpr float Top = 90, RowHeight = 84, RowStep = 96;
constexpr int Visible = 6; // rows down to the footer; 세이브 > 드롭박스 fits five under its account card
} // namespace
void Application::savesAction(Action a) {
  // A game's saves open in the same dialog as 설치된 게임's, saves only.
  if (managedGame_) {
    installedAction(a);
    return;
  }
  // Dropbox's popups and a sign-in waiting for the phone take every key.
  if (savesTab_ == 2 && (dropboxModal() || dropboxSession_)) {
    dropboxAction(a);
    return;
  }
  if (serverAppSaveAsk_) {
    if (a == Action::Back) serverAppSaveAsk_ = false;
    else if (a == Action::Accept && settled()) {
      serverAppSaveAsk_ = false;
      if (patchWorker_.joinable()) message(tr("Try again when the patch task is done."));
      else if (!serverSaves_.empty()) applyHomebrewSave(serverSaves_[std::clamp(serverSavePick_, 0, int(serverSaves_.size()) - 1)]);
    }
    return;
  }
  if (savePicking_) {
    const auto groups = serverSaveGroups();
    const int count = groups.empty() ? 0 : groups[std::clamp(serverSaveRow_, 0, int(groups.size()) - 1)].second;
    if (a == Action::Up) savePickRow_ = std::max(0, savePickRow_ - 1);
    if (a == Action::Down) savePickRow_ = std::min(count - 1, savePickRow_ + 1);
    if (a == Action::Back) savePicking_ = false;
    if (a == Action::Accept && count) {
      savePicking_ = false;
      serverSavePick_ = groups[std::clamp(serverSaveRow_, 0, int(groups.size()) - 1)].first + savePickRow_;
      chooseServerSave();
    }
    return;
  }
  if (saveNeedsGame_) {
    // The popup for a save whose game is not here: 게임 다운로드 or 닫기.
    if (a == Action::Back) saveNeedsGame_ = false;
    if (a == Action::Accept) {
      saveNeedsGame_ = false;
      if (saveGameGroup_ >= 0) openDetail(size_t(saveGameGroup_));
    }
    return;
  }
  if (a == Action::Previous || a == Action::Next) {
    savesTab_ = (savesTab_ + (a == Action::Next ? 1 : SavesTabs - 1)) % SavesTabs;
    if (savesTab_ == 1 && !serverSavesLoaded_) loadServerSaves();
    if (savesTab_ == 2) dropboxEnter();
    return;
  }
  // Y: the order of the tab (as 내 게임's), 드롭박스's once linked; - reads
  // the list again.
  if (a == Action::Queue && (savesTab_ != 2 || dropboxLinked_)) {
    cycleSavesSort();
    return;
  }
  if (a == Action::Queue || a == Action::Reload) {
    if (savesTab_ == 2) dropboxAction(a);
    else if (savesTab_ == 1) loadServerSaves();
    else if (savesTab_ == 0) {
      refreshInstalled();
      readBackupSummary();
    }
    return;
  }
  if (a == Action::Left) {
    sidebar_ = true;
    return;
  }
  // X searches every tab, 드롭박스 too.
  if (a == Action::Search) {
    requestSearch();
    return;
  }
  if (savesTab_ == 2) {
    dropboxAction(a);
    return;
  }
  if (savesTab_ == 1) {
    const int games = int(serverSaveGroups().size());
    if (!games) return;
    if (a == Action::Up) serverSaveRow_ = std::max(0, serverSaveRow_ - 1);
    if (a == Action::Down) serverSaveRow_ = std::min(games - 1, serverSaveRow_ + 1);
    if (a == Action::Accept) chooseLibraryGame();
    return;
  }
  const int count = int(saveGameRows().size());
  if (!count) return;
  if (a == Action::Up) selected_ = std::max(0, selected_ - 1);
  if (a == Action::Down) selected_ = std::min(count - 1, selected_ + 1);
  if (a == Action::Accept) openManagement();
}
void Application::savesScreen() {
  const bool onServer = savesTab_ == 1;
  // As on 홈브류: search at the right end, the tabs left of it, then the
  // order (Y). Dropbox has none until linked and the title takes its room;
  // the tabs stay put by the search.
  const float searchX = SearchX;
  searchBar(savesQuery_, searchX);
  const auto sort = savesSortLabel();
  const float sortW = myGamesButtonWidth(sort);
  const float tabsLeft = headerTabs({tr("This console|mine"), tr("Server"), tr("Dropbox")}, savesTab_, [this](int n) {
    savesTab_ = n;
    if (n == 1 && !serverSavesLoaded_) loadServerSaves();
    if (n == 2) dropboxEnter();
  }, searchX - 16);
  const float sortX = tabsLeft - 12 - sortW;
  const bool sorted = savesTab_ != 2 || dropboxLinked_;
  if (sorted) {
    headerItem("sort", sortX, 24, sortW, 48, [this] { cycleSavesSort(); });
    paint_.rect(sortX, 24, sortW, 48, Surface, 12);
    paint_.key("Y", sortX + 18, 48);
    paint_.icon(10, sortX + 60, 38, Ink, 20);
    paint_.fonts.middle(sort, sortX + 88, 48, 18, Ink, true, sortW - 106);
  }
  pageTitle(tr("Saves"),
            onServer ? (serverSavesLoaded_ ? countLabel(serverSaveGroups().size(), savesQuery_) : std::string())
            : savesTab_ == 0 && installed_.installed.complete ? countLabel(saveGameRows().size(), savesQuery_)
                                                             : "",
            sorted ? sortX : tabsLeft);
  if (savesTab_ == 2) {
    dropboxTab();
    return;
  }
  if (onServer) {
    serverSavesList();
    if (managedGame_) management();
    return;
  }
  if ((deviceBusy_ || savesUnread()) && !managedGame_) {
    for (int n = 0; n < Visible; ++n) paint_.skeleton(260, Top + n * RowStep, 992, RowHeight, 16);
    return;
  }
  if (deviceError_ && !managedGame_) {
    notice(tr("Couldn't read installed games."), deviceError_.message, 25);
    return;
  }
  const auto rows = saveGameRows();
  // Only games with a save or a backup are listed; the search narrows that.
  if (rows.empty() && (savesQuery_.empty() || saveGameRows("").empty())) {
    notice(tr("No games to manage saves for."), tr("Install a game and play it once to make a save."), 36);
    return;
  }
  if (rows.empty()) {
    notice(trf("No games match '{0}'.", {savesQuery_}), tr("Press B to clear the search."), 23);
    return;
  }
  const int count = int(rows.size());
  selected_ = std::clamp(selected_, 0, count - 1);
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("saves", 232, Top, 1048, 660 - (Top), float(std::clamp(selected_ - Visible / 2, 0, std::max(0, count - Visible))),
                          float((count) - (Visible)), RowStep);
  const int first = std::max(0, int(top));
  paint_.scrollbar(1266, Top, RowStep * Visible - 12, top, Visible, float(count));
  clipList(Top - 8, 660 - Top + 8);
  for (int i = first; i < count && i <= first + Visible + 1; ++i) {
    const auto &g = installed_.games[rows[i]];
    const float y = Top + (i - top) * RowStep;
    rowHit(260, y, 992, RowHeight, 16, [this, i] {
      if (inContent() && selected_ == i) action(Action::Accept);
      sidebar_ = false;
      selected_ = i;
    });
    if (inContent() && i == selected_ && !managedGame_) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    const auto icon = installedIcon(g);
    cover(icon, 278, y + 10, 64, false, g.name);
    if (inContent() && i == selected_ && !managedGame_) marquee(g.name, 360, y + 12, 21, 640);
    else paint_.fonts.text(g.name, 360, y + 12, 21, Ink, true, 640);
    paint_.fonts.text(g.titleId, 360, y + 46, 16, Secondary);
    // How many backups the SD card holds (save-keeper's included), read
    // on a worker when 세이브 opens.
    const int backups = backupsOf(g.applicationId);
    const std::string state = backups ? trf("{0} backups", {std::to_string(backups)}) : std::string(tr("No backups"));
    const float w = paint_.fonts.measure(state, 16, true) + 20;
    paint_.rect(1230 - w, y + 28, w, 28, backups ? Good : Line, 8);
    paint_.fonts.middle(state, 1230 - w + 10, y + 42, 16, backups ? Surface : Ink, true);
  }
  unclip();
  if (managedGame_) management();
}
void Application::loadServerSaves() {
  if (!session_ || !connected_ || serverSavesWorker_.joinable()) return;
  const auto servers = serversWith("");
  if (servers.empty()) return;
  const auto state = state_;
  serverSavesError_ = {};
  serverSavesWorker_ = std::thread([this, servers, state] {
    const platform::OnCore off(platform::NetworkCore);
    net::ServerSavesReply reply;
    if (!guarded("server saves", [&] {
          // The one in use says whether the list failed; another server that
          // does not answer is left out.
          std::vector<std::pair<std::string, std::vector<serversaves::ServerSave>>> lists;
          for (size_t i = 0; i < servers.size(); ++i) {
            net::ServerSavesReply theirs;
            auto saves = net::serverSaves(servers[i], state, theirs, &stop_).value;
            if (i == 0) reply = theirs;
            else if (theirs.error) logLine("server saves: " + servers[i].url + " not read: " + theirs.error.message);
            if (!theirs.error) lists.push_back({servers[i].url, std::move(saves)});
          }
          incomingServerSaves_ = serversaves::mergeServers(std::move(lists));
        }))
      reply.error.message = tr("Could not load the server saves.");
    incomingServerSavesError_ = reply.error;
    incomingServerSavesMissing_ = reply.missing;
    serverSavesDone_ = true;
  });
}
void Application::pollServerSaves() {
  if (!serverSavesDone_.exchange(false)) return;
  serverSavesWorker_.join();
  serverSavesLoaded_ = true;
  serverSavesError_ = incomingServerSavesError_;
  serverSavesMissing_ = incomingServerSavesMissing_;
  serverSaves_ = std::move(incomingServerSaves_);
  // An app's saves list here under the app, and in its popup in 홈브류.
  homebrewSaves_.clear();
  for (const auto &save : serverSaves_)
    if (!save.homebrewApp.empty()) homebrewSaves_.push_back(save);
  serverSaves_.erase(std::remove_if(serverSaves_.begin(), serverSaves_.end(),
                                  [](const auto &save) { return save.titleId.empty() && save.homebrewApp.empty(); }),
                   serverSaves_.end());
  sortServerSaves();
}
std::string Application::serverSaveTitle(const serversaves::ServerSave &save) const {
  return save.homebrewApp.empty() ? serversaves::gameName(save, languageCode())
                                  : homebrew::appTitle(homebrew_, save.homebrewApp);
}
std::string Application::serverSaveIcon(const serversaves::ServerSave &save) const {
  if (save.homebrewApp.empty()) return save.gameIcon;
  const auto *app = homebrew::findApp(homebrew_, save.homebrewApp);
  return app ? app->iconUrl : "";
}
bool Application::serverSaveTargetHere(const serversaves::ServerSave &save) const {
  if (!save.homebrewApp.empty()) {
    const auto *app = homebrew::findApp(homebrew_, save.homebrewApp);
    return !app || homebrewStatus(*app) != homebrew::Status::Available;
  }
  return std::any_of(installed_.games.begin(), installed_.games.end(),
                     [&](const auto &g) { return g.titleId == save.titleId; });
}
std::vector<int> Application::saveGameRows() const { return saveGameRows(savesQuery_); }
std::vector<int> Application::saveGameRows(const std::string &query) const {
  auto rows = gameRows(query);
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [this](int i) { return !hasSaveWork(installed_.games[size_t(i)].applicationId); }),
             rows.end());
  const int sort = savesSort_[0];
  const auto newest = [this](uint64_t id) -> std::string {
    const auto found = backupSummary_.find(id);
    return found == backupSummary_.end() ? std::string() : found->second.newest;
  };
  std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
    const auto &ga = installed_.games[size_t(a)], &gb = installed_.games[size_t(b)];
    if (sort == 0) { // latest backup first; games without one after, by name
      const auto na = newest(ga.applicationId), nb = newest(gb.applicationId);
      if (na != nb) return na > nb;
    } else if (sort == 2) { // installed or updated most recently first
      if (ga.lastUpdated != gb.lastUpdated) return ga.lastUpdated > gb.lastUpdated;
    } else if (sort == 3) { // most backups first
      const int ca = backupsOf(ga.applicationId), cb = backupsOf(gb.applicationId);
      if (ca != cb) return ca > cb;
    }
    return ga.name < gb.name;
  });
  return rows;
}
std::string Application::savesSortLabel() const {
  static const char *const Console[SavesSorts] = {"Latest backup|sort", "Name|sort", "Recently updated|sort",
                                                  "Most backups|sort"};
  static const char *const Server[SavesSorts] = {"Installed first|sort", "Installed only|sort", "Name|sort",
                                                 "Recently added|sort"};
  static const char *const Dropbox[DropboxSorts] = {"Latest backup|sort", "Name|sort", "With console|sort"};
  if (savesTab_ == 2) return tr(Dropbox[savesSort_[2]]);
  return tr(savesTab_ == 1 ? Server[savesSort_[1]] : Console[savesSort_[0]]);
}
void Application::cycleSavesSort() {
  // The Dropbox cursor is a game's id, so it stays on that game when listed.
  if (savesTab_ == 2) {
    if (dropboxLinked_) savesSort_[2] = (savesSort_[2] + 1) % DropboxSorts;
    return;
  }
  if (savesTab_ == 1) {
    // The cursor stays on the game it was on, when that one is still listed.
    const auto groups = serverSaveGroups();
    std::string on;
    if (!groups.empty()) {
      const auto &head = serverSaves_[size_t(groups[size_t(std::clamp(serverSaveRow_, 0, int(groups.size()) - 1))].first)];
      on = homebrew::ownerKey(head.titleId, head.homebrewApp);
    }
    savesSort_[1] = (savesSort_[1] + 1) % SavesSorts;
    sortServerSaves();
    const auto now = serverSaveGroups();
    serverSaveRow_ = 0;
    for (size_t i = 0; i < now.size(); ++i) {
      const auto &head = serverSaves_[size_t(now[i].first)];
      if (homebrew::ownerKey(head.titleId, head.homebrewApp) == on) serverSaveRow_ = int(i);
    }
    return;
  }
  const auto rows = saveGameRows();
  const uint64_t on = rows.empty() ? 0 : installed_.games[size_t(rows[size_t(std::clamp(selected_, 0, int(rows.size()) - 1))])].applicationId;
  savesSort_[0] = (savesSort_[0] + 1) % SavesSorts;
  const auto now = saveGameRows();
  selected_ = 0;
  for (size_t i = 0; i < now.size(); ++i)
    if (installed_.games[size_t(now[i])].applicationId == on) selected_ = int(i);
}
void Application::readBackupSummary() {
  if (backupSummaryWorker_.joinable()) {
    backupSummaryAgain_ = true; // one more read once this one is back
    return;
  }
  backupSummaryWorker_ = std::thread([this, root = saves::keeperRoot(sdRoot_)] {
    const platform::OnCore off(platform::WriterCore);
    guarded("backup summary", [&] {
      incomingSummary_ = saves::summarizeBackups(root);
      auto saved = platform::savedApplications();
      incomingSaved_ = saved.error ? std::nullopt : std::optional(std::move(saved.value));
    });
    backupSummaryDone_ = true;
  });
}
void Application::pollBackupSummary() {
  if (!backupSummaryDone_.exchange(false)) return;
  backupSummaryWorker_.join();
  backupSummary_ = std::move(incomingSummary_);
  // A failed read keeps what was known, else every game when nothing was.
  if (incomingSaved_) savedGames_ = std::move(incomingSaved_);
  if (std::exchange(backupSummaryAgain_, false)) readBackupSummary();
  else savesFresh_ = true;
}
void Application::sortServerSaves() {
  // A popup holds a save by its place in the list: the order waits for it.
  if (savePicking_ || saveNeedsGame_ || serverAppSaveAsk_) return;
  // By the order picked (Y): games and apps on this console first (and,
  // for "installed only", only those listed: serverSaveGroups), by name, or
  // newest first by the group's latest save. One owner's saves stay together.
  const int sort = savesSort_[1];
  std::map<std::string, int64_t> latest;
  for (const auto &save : serverSaves_) {
    auto &at = latest[homebrew::ownerKey(save.titleId, save.homebrewApp)];
    at = std::max(at, save.updatedAt);
  }
  std::stable_sort(serverSaves_.begin(), serverSaves_.end(), [this, sort, &latest](const auto &a, const auto &b) {
    const auto ka = homebrew::ownerKey(a.titleId, a.homebrewApp), kb = homebrew::ownerKey(b.titleId, b.homebrewApp);
    if (sort == 3 && latest[ka] != latest[kb]) return latest[ka] > latest[kb];
    if (sort <= 1) {
      const bool ha = serverSaveTargetHere(a), hb = serverSaveTargetHere(b);
      if (ha != hb) return ha;
    }
    const auto na = serverSaveTitle(a), nb = serverSaveTitle(b);
    if (na != nb) return na < nb;
    // One game's or app's saves side by side, for the groups.
    return homebrew::ownerKey(a.titleId, a.homebrewApp) < homebrew::ownerKey(b.titleId, b.homebrewApp);
  });
  serverSaveRow_ = std::clamp(serverSaveRow_, 0, std::max(0, int(serverSaveGroups().size()) - 1));
}
// Runs of one game's or app's saves in serverSaves_ (sorted so they sit
// together): the list shows one row per game or app.
std::vector<std::pair<int, int>> Application::serverSaveGroups() const {
  std::vector<std::pair<int, int>> out;
  std::string last; // the running group's owner
  for (int i = 0; i < int(serverSaves_.size()); ++i) {
    auto owner = homebrew::ownerKey(serverSaves_[i].titleId, serverSaves_[i].homebrewApp);
    if (!out.empty() && owner == last) ++out.back().second;
    else out.push_back({i, 1}), last = std::move(owner);
  }
  // "Installed only" keeps what can go on this console; the search keeps a
  // game or app when its name or any of its saves matches.
  out.erase(std::remove_if(out.begin(), out.end(),
                           [&](const std::pair<int, int> &group) {
                             if (savesSort_[1] == 1 && !serverSaveTargetHere(serverSaves_[group.first])) return true;
                             if (savesQuery_.empty()) return false;
                             const auto &head = serverSaves_[group.first];
                             if (textMatches({serverSaveTitle(head), head.homebrewApp}, savesQuery_)) return false;
                             for (int i = group.first; i < group.first + group.second; ++i)
                               if (textMatches({serverSaves_[i].name, serverSaves_[i].titleId}, savesQuery_)) return false;
                             return true;
                           }),
            out.end());
  return out;
}
std::vector<int> Application::gameRows(const std::string &query) const {
  std::vector<int> out;
  for (int i = 0; i < int(installed_.games.size()); ++i)
    if (textMatches({installed_.games[i].name, installed_.games[i].titleId}, query)) out.push_back(i);
  return out;
}
// A on a game's row opens its saves to pick from, even when there is one.
void Application::chooseLibraryGame() {
  const auto groups = serverSaveGroups();
  if (groups.empty() || deviceBusy_) return;
  const int start = groups[std::clamp(serverSaveRow_, 0, int(groups.size()) - 1)].first;
  serverSavePick_ = start;
  // A game not here asks for the game first.
  const bool app = !serverSaves_[start].homebrewApp.empty();
  if (!app && !serverSaveTargetHere(serverSaves_[start])) {
    chooseServerSave();
    return;
  }
  savePicking_ = true;
  savePickRow_ = 0;
}
void Application::chooseServerSave() {
  if (serverSaves_.empty() || deviceBusy_) return;
  const auto &save = serverSaves_[std::clamp(serverSavePick_, 0, int(serverSaves_.size()) - 1)];
  // An app's save is files for its folder, put there through the patch
  // worker as from 홈브류; it asks first.
  if (!save.homebrewApp.empty()) {
    if (patchWorker_.joinable()) message(tr("Try again when the patch task is done."));
    else serverAppSaveAsk_ = true;
    return;
  }
  const auto game = std::find_if(installed_.games.begin(), installed_.games.end(),
                                 [&](const auto &g) { return g.titleId == save.titleId; });
  if (game == installed_.games.end()) {
    // Asks first, as 유저 패치 does: the game comes from the server when it has it.
    const auto *group = findGroup(save.titleId);
    saveGameGroup_ = group ? int(group - groups_.data()) : -1;
    saveNeedsGame_ = true;
    return;
  }
  applying_ = save;
  selected_ = int(game - installed_.games.begin());
  openManagement(); // reads the game's saves; the rows then pick the account
}
void Application::applyServerSave() {
  if (!managedGame_ || !applying_ || applySave_ < 0 || applySave_ >= int(saves_.size()) || saveWorker_.joinable())
    return;
  if (installer_.running()) {
    message(tr("Apply the save after the install finishes."));
    return;
  }
  if (sdFree_ && *sdFree_ < applying_->size * 2 + (16u << 20)) {
    alertTitle_ = tr("Cannot apply the save");
    alert_ = trf("Not enough space on the SD card. {0} is needed.", {formatBytes(applying_->size * 2)});
    return;
  }
  platform::SaveJob job;
  job.save = saves_[applySave_];
  job.game = *managedGame_;
  job.sd = sdRoot_;
  job.copied = &saveCopied_;
  const auto save = *applying_;
  const auto server = session_ ? serverFor(applying_->server) : Server{};
  const auto state = state_;
  saveCopied_ = 0;
  saveRestoring_ = true;
  applyRunning_ = deviceBusy_ = true;
  saveWorker_ = std::thread([this, job, save, server, state] {
    const platform::OnCore off(platform::WriterCore);
    if (!guarded("server save", [&] {
          // Kept as a backup first (source "server"), so it can be restored
          // again later, then restored like any other backup.
          auto backup = platform::importBackup(
              job,
              [&](const std::string &dir) {
                Error error;
                std::error_code ec;
                makeDirectories(std::filesystem::path(dir).string());
                net::downloadServerSave(server, state, save, dir, saveCopied_, error, &stop_);
                return error;
              },
              "server");
          saveResult_ = backup.error ? backup.error : platform::restoreSave(job, backup.value);
        }))
      saveResult_.message = tr("Something went wrong while handling the save.");
    saveDone_ = true;
  });
}
void Application::applyConfirm() {
  if (!applyAsking_ || !applying_ || applySave_ < 0 || applySave_ >= int(saves_.size())) return;
  const auto &target = saves_[applySave_];
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  const auto body = modal(700, 20 + 30 + 84 + 16, tr("Apply this save?"), tr("Cancel"), "", tr("Apply|confirm"), 36);
  paint_.fonts.text(applying_->name, body.x + 32, body.y + 20, 21, Ink, true, body.w - 64);
  paint_.fonts.wrapped(target.missing ? trf("Creates a save for {0} and puts this save in it.", {target.userName})
                                      : trf("Replaces {0}'s current save with this one. The current save is backed up first and can be restored from Saves.",
                           {target.userName}),
                       body.x + 32, body.y + 56, 19, Secondary, body.w - 64, 3);
}
void Application::serverSavesList() {
  if (!serverSavesLoaded_ && !serverSavesWorker_.joinable() && connected_) loadServerSaves();
  needHomebrewList(); // apps' saves take the app's name and icon from it
  if (serverSaves_.empty()) {
    if (!connected_)
      offlineNotice(tr("Server saves load once the server is connected."));
    else if (!serverSavesLoaded_ || serverSavesWorker_.joinable())
      for (int n = 0; n < Visible; ++n) paint_.skeleton(260, Top + n * RowStep, 992, RowHeight, 16);
    else if (serverSavesMissing_)
      notice(tr("This server offers no saves."), tr("Saves put in the server's saves folder appear here."), 36);
    else if (serverSavesError_)
      notice(tr("Could not load the server saves."), serverSavesError_.message, 25);
    else
      notice(tr("No server saves to get."), tr("Saves put in the server's saves folder appear here."), 36);
    return;
  }
  // One row per game; its saves are picked after A.
  const auto groups = serverSaveGroups();
  const int count = int(groups.size());
  serverSaveRow_ = std::clamp(serverSaveRow_, 0, count - 1);
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("serverSaves", 232, Top, 1048, 660 - (Top), float(std::clamp(serverSaveRow_ - Visible / 2, 0, std::max(0, count - Visible))),
                          float((count) - (Visible)), RowStep);
  const int first = std::max(0, int(top));
  paint_.scrollbar(1266, Top, RowStep * Visible - 12, top, Visible, float(count));
  clipList(Top - 8, 660 - Top + 8);
  for (int i = first; i < count && i <= first + Visible + 1; ++i) {
    const auto [start, saves] = groups[i];
    const auto &save = serverSaves_[start];
    const float y = Top + (i - top) * RowStep;
    rowHit(260, y, 992, RowHeight, 16, [this, i] {
      if (inContent() && serverSaveRow_ == i) action(Action::Accept);
      sidebar_ = false;
      serverSaveRow_ = i;
    });
    if (inContent() && i == serverSaveRow_ && !managedGame_) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    const bool app = !save.homebrewApp.empty();
    const auto game = serverSaveTitle(save);
    cover(serverSaveIcon(save), 278, y + 10, 64, false, game, app); // app icons as 홈브류 draws them
    paint_.fonts.text(game, 360, y + 12, 21, Ink, true, 640);
    // An app's save goes through the patch worker; its row shows the work.
    bool busy = false;
    for (int n = start; n < start + saves; ++n) busy |= !patchBusy_.empty() && patchBusy_ == serverSaves_[n].id;
    const bool here = serverSaveTargetHere(save);
    const std::string state = busy ? tr(patchBusyLabel_.c_str())
                              : here ? tr("Ready to apply")
                              : app ? tr("App not installed") : tr("Game not installed");
    // Under the name: 홈브류 for an app, how many saves, then what they are:
    // the one save's name and description, or every save's name.
    float at = 360;
    if (app) at += paint_.pill(tr("Homebrew"), at, y + 46) + 6;
    if (saves > 1) at += paint_.pill(trf("{0} saves", {std::to_string(saves)}), at, y + 46) + 6;
    std::string what;
    if (saves > 1) {
      for (int n = start; n < start + saves; ++n) what += (what.empty() ? "" : " · ") + serverSaves_[n].name;
    } else {
      what = save.name;
      const auto line = save.description.substr(0, save.description.find('\n'));
      if (!line.empty() && line != save.name) what += " · " + line;
    }
    paint_.fonts.text(what, at, y + 48, 16, Secondary, false, 1180 - at - paint_.fonts.measure(state, 15, true));
    // The same grey label as every list's state, at the right end.
    paint_.pill(state, 1230 - paint_.fonts.measure(state, 15, true) - 18, y + 14);
  }
  unclip();
}
// Several saves for one game: pick one, as 유저 패치 picks a patch.
void Application::savePickDialog() {
  const auto groups = serverSaveGroups();
  if (!savePicking_ || groups.empty()) return;
  const auto [start, count] = groups[std::clamp(serverSaveRow_, 0, int(groups.size()) - 1)];
  constexpr int Shown = 5;
  constexpr float Row = 72, Gap = 8;
  const int rows = std::min(count, Shown);
  const auto &head = serverSaves_[start];
  const auto body = modal(860, 24 + rows * (Row + Gap) + 16, serverSaveTitle(head), tr("Close"), "",
                          tr("Apply|action"), 36);
  const float x = body.x + 32, w = body.w - 64;
  savePickRow_ = std::clamp(savePickRow_, 0, count - 1);
  // The row at the top follows the cursor, or the finger while it glides.
  const float topRow = glide("savePick", x, body.y + 24, w, rows * (Row + Gap), float(std::clamp(savePickRow_ - Shown / 2, 0, std::max(0, count - Shown))),
                          float((count) - (Shown)), (Row + Gap));
  const int top = std::max(0, int(topRow));
  clipTo(x, body.y + 24, w, rows * (Row + Gap));
  for (int n = top; n < count && n <= top + rows; ++n) {
    const auto &save = serverSaves_[start + n];
    const float y = body.y + 24 + (n - topRow) * (Row + Gap);
    rowHit(x, y, w, Row, 12, [this, n] {
      savePickRow_ = n;
      action(Action::Accept);
    });
    if (n == savePickRow_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.fonts.text(save.name, x + 20, y + 10, 20, Ink, true, w - 300);
    if (save.verified)
      paint_.verifiedPill(x + 20 + std::min(paint_.fonts.measure(save.name, 20, true), w - 300) + 10, y + 10);
    std::string meta;
    // When it was put on the server, then which game build it is for.
    if (save.updatedAt > 0) {
      const std::time_t at = save.updatedAt;
      std::tm local{};
      localtime_r(&at, &local);
      char day[16];
      std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
      meta = day;
    }
    if (!save.gameVersion.empty() && save.homebrewApp.empty()) meta += (meta.empty() ? "" : " · ") + trf("for game {0}", {save.gameVersion});
    if (const auto votes = voteSummary(save.votes); !votes.empty()) meta += (meta.empty() ? "" : " · ") + votes;
    if (!save.description.empty()) meta += (meta.empty() ? "" : " · ") + save.description;
    paint_.fonts.text(meta, x + 20, y + 40, 16, Secondary, false, w - 160);
    const auto size = formatBytes(save.size);
    paint_.fonts.middle(size, x + w - 20 - paint_.fonts.measure(size, 17, true), y + Row / 2, 17, Ink, true);
  }
  unclip();
}
void Application::saveNeedsGameDialog() {
  if (!saveNeedsGame_ || serverSaves_.empty()) return;
  const int pick = std::clamp(serverSavePick_, 0, int(serverSaves_.size()) - 1);
  const auto &save = serverSaves_[pick];
  const bool offered = saveGameGroup_ >= 0;
  // The game's saves on the server, so it is clear what waits for it.
  int start = pick, count = 0;
  for (const auto &[first, n] : serverSaveGroups())
    if (pick >= first && pick < first + n) start = first, count = n;
  constexpr int Shown = 4;
  constexpr float Cover = 96, Line = 30;
  const int listed = std::min(count, Shown);
  const auto body = modal(700, 24 + Cover + 20 + listed * Line + (count > Shown ? Line : 0) + 16,
                          tr("The game is not on this console"), tr("Close"), "",
                          offered ? tr("Download the game") : tr("OK"), 25);
  const float x = body.x + 32, y = body.y + 24, textX = x + Cover + 24, textW = body.w - 64 - Cover - 24;
  cover(save.gameIcon, x, y, int(Cover), false, serversaves::gameName(save, languageCode()));
  paint_.fonts.text(serversaves::gameName(save, languageCode()), textX, y + 4, 21, Ink, true, textW);
  paint_.fonts.wrapped(offered ? tr("Install the game and play it once, then apply the save.")
                               : tr("This server does not have the game. Install it and play once, then apply."),
                       textX, y + 40, 17, Secondary, textW, 3);
  float ly = y + Cover + 20;
  for (int n = 0; n < listed; ++n, ly += Line) {
    const auto &s = serverSaves_[start + n];
    const auto size = formatBytes(s.size);
    paint_.icon(36, x, ly + 2, Secondary, 20);
    paint_.fonts.text(s.name, x + 30, ly, 18, Ink, false, body.w - 64 - 30 - 100);
    paint_.fonts.text(size, x + body.w - 64 - paint_.fonts.measure(size, 17, true), ly + 1, 17, Secondary, true);
  }
  if (count > Shown) paint_.fonts.text(trf("and {0} more", {std::to_string(count - Shown)}), x + 30, ly, 17, Secondary, false, body.w - 64 - 30);
}
} // namespace foil::ui
