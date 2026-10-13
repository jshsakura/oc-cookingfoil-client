#include "ui/Application.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <filesystem>
namespace foil::ui {
namespace {
constexpr int VisibleRows = 4;
constexpr float RowTop = 90, RowHeight = 108, RowStep = 124;
// "any" or nothing means the patch does not care which version is installed.
bool versionMismatch(const std::string &wanted, const std::string &installed) {
  return !wanted.empty() && wanted != "any" && !installed.empty() && wanted != installed;
}
bool missingFiles(const std::map<std::string, PatchHealth> &health, const std::string &id) {
  const auto found = health.find(id);
  return found != health.end() && found->second.missing > 0;
}
bool changedFiles(const std::map<std::string, PatchHealth> &health, const std::string &id) {
  const auto found = health.find(id);
  return found != health.end() && found->second.changed > 0;
}
// What the check found, for a patch that is on: missing files first.
const char *healthNote(const std::map<std::string, PatchHealth> &health, const Application::PatchRow &row) {
  if (!row.record || !row.record->on) return nullptr;
  if (missingFiles(health, row.patch.id)) return tr("Some files missing");
  if (changedFiles(health, row.patch.id)) return tr("Some files changed");
  return nullptr;
}
bool updateAvailable(const Application::PatchRow &row) {
  return row.record && row.onServer && !row.patch.hash.empty() && row.record->hash != row.patch.hash;
}
const char *filterLabel(Application::PatchFilter filter) {
  switch (filter) {
  case Application::PatchFilter::Stored: return tr("Installed");
  case Application::PatchFilter::Updates: return tr("Updates");
  default: return tr("All");
  }
}
// Busy labels stay Korean keys in patchBusyLabel_ and are translated only
// when drawn, so the progress check compares keys, never screen text.
constexpr const char *Removing = "Deleting";     // i18n: keep
constexpr const char *Updating = "Updating";   // i18n: keep
constexpr const char *Getting = "Downloading";        // i18n: keep
constexpr const char *Disabling = "Turning off";      // i18n: keep
constexpr const char *Enabling = "Turning on";       // i18n: keep
constexpr const char *Reapplying = "Reapplying"; // i18n: keep
// One run of the light check: the lookups and the time it may take before
// it stops and goes on at the next visit.
constexpr size_t CheckLooks = 4000;
constexpr std::chrono::milliseconds CheckBudget{1500};
} // namespace
bool Application::patchFilterMatches(PatchFilter filter, const PatchRow &row) {
  switch (filter) {
  case PatchFilter::Stored: return row.record.has_value();
  case PatchFilter::Updates: return updateAvailable(row);
  default: return true;
  }
  return true;
}
std::vector<Application::PatchRow> Application::patchRows(std::vector<PatchRow> rows) const {
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [this](const PatchRow &row) {
                              return !patchFilterMatches(patchFilter_, row) ||
                                     !textMatches({row.game, row.patch.name, row.patch.titleId, row.patch.homebrewApp},
                                                  patchQuery_);
                            }),
             rows.end());
  return rows;
}
std::vector<Application::PatchRow> Application::allPatchRows() const {
  std::vector<PatchRow> rows;
  auto describe = [&](PatchRow &row) {
    const auto record = patchRecords_.find(row.patch.id);
    if (record != patchRecords_.end())
      row.record = record->second;
    // An app's patch goes under the app as 홈브류 lists it: its name and
    // icon, or its folder when the list does not have it.
    if (!row.patch.homebrewApp.empty()) {
      const auto *app = homebrew::findApp(homebrew_, row.patch.homebrewApp);
      row.game = homebrew::appTitle(homebrew_, row.patch.homebrewApp);
      row.icon = app ? app->iconUrl : "";
      // An app the list does not know cannot be told missing, so no warning.
      row.installed = !app || homebrewStatus(*app) != homebrew::Status::Available;
      return;
    }
    if (const auto *group = findGroup(row.patch.titleId)) {
      row.game = group->representative.name;
      row.icon = group->representative.icon;
      row.shopGroup = int(group - groups_.data());
    }
    const auto local = std::find_if(installed_.games.begin(), installed_.games.end(),
                                    [&](const InstalledGame &g) { return g.titleId == row.patch.titleId; });
    if (local != installed_.games.end()) {
      row.installed = true;
      row.installedVersion = local->displayVersion;
      if (row.game.empty())
        row.game = local->name;
    }
    // Games the shop does not list: the name and icon the server sent.
    if (row.game.empty()) {
      static const char *codes[] = {"ko", "en", "ja", "zh"};
      const std::string code = languageCode();
      for (size_t k = 0; k < row.patch.gameNames.size() && row.game.empty(); ++k)
        if (code == codes[k]) row.game = row.patch.gameNames[k];
      if (row.game.empty()) row.game = row.patch.gameName;
      for (size_t k = 0; k < row.patch.gameNames.size() && row.game.empty(); ++k)
        row.game = row.patch.gameNames[k];
    }
    if (row.icon.empty() && !row.patch.gameIcon.empty())
      row.icon = row.patch.gameIcon[0] == '/' && session_ ? session_->server().url + row.patch.gameIcon : row.patch.gameIcon;
    if (row.game.empty())
      row.game = row.patch.titleId;
  };
  // Apps' patches list here too, each under its app, as well as in 홈브류.
  for (const auto &patch : serverPatches_) {
    PatchRow row{patch, std::nullopt, true, "", "", "", false};
    describe(row);
    rows.push_back(std::move(row));
  }
  // Patches kept on the card but gone from the server can still be removed.
  for (const auto &[id, record] : patchRecords_) {
    if (std::any_of(serverPatches_.begin(), serverPatches_.end(), [&](const UserPatch &p) { return p.id == id; }))
      continue;
    PatchRow row;
    row.patch.id = id;
    row.patch.titleId = record.titleId;
    row.patch.homebrewApp = record.homebrewApp;
    row.patch.name = record.name;
    row.patch.version = record.version;
    row.patch.size = record.bytes;
    describe(row);
    rows.push_back(std::move(row));
  }
  // Newest on the server first, then by game and patch name.
  std::stable_sort(rows.begin(), rows.end(), [](const PatchRow &a, const PatchRow &b) {
    if (a.patch.updatedAt != b.patch.updatedAt)
      return a.patch.updatedAt > b.patch.updatedAt;
    // Patches for a game nobody could name (shown by Title ID) go last.
    const bool knownA = a.game != a.patch.titleId, knownB = b.game != b.patch.titleId;
    if (knownA != knownB)
      return knownA;
    if (a.game != b.game)
      return a.game < b.game;
    return a.patch.name < b.patch.name;
  });
  return rows;
}
std::vector<Application::PatchGame> Application::patchGames(const std::vector<PatchRow> &rows) {
  std::vector<PatchGame> games;
  std::map<std::string, size_t> at;
  for (int i = 0; i < int(rows.size()); ++i) {
    const auto &row = rows[i];
    // A game by its Title ID, an app by its folder.
    const auto key = homebrew::ownerKey(row.patch.titleId, row.patch.homebrewApp);
    auto found = at.find(key);
    if (found == at.end()) {
      found = at.emplace(key, games.size()).first;
      games.push_back({key, row.game, row.icon, row.installed, {}, !row.patch.homebrewApp.empty()});
    }
    games[found->second].rows.push_back(i);
  }
  return games;
}
const Application::PatchRow *Application::pickedPatch(const std::vector<PatchRow> &rows) const {
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const PatchRow &r) { return r.patch.id == patchPickId_; });
  return found == rows.end() ? nullptr : &*found;
}
void Application::fetchPatches() {
  if (!session_ || patchWorker_.joinable() || !connected_)
    return;
  patchFetching_ = runPatchJob("", "", [this](net::Session &session, PatchStore &, Error &error, std::string &) {
    auto list = session.patches();
    error = list.error;
    incomingPatchRetry_ = session.patchesRetryAfter();
    incomingPatches_ = std::move(list.value);
  });
}
// One job at a time: the worker owns the store until it reports back.
bool Application::runPatchJob(
    const std::string &id, const std::string &label,
    std::function<void(net::Session &, PatchStore &, Error &, std::string &)> job, const std::string &failTitle) {
  if (patchWorker_.joinable() || !patchStore_ || !session_)
    return false;
  // A job that moves files comes first: the light check stops where it is.
  if (!id.empty())
    patchCheckStop_ = true;
  patchBusy_ = id;
  patchBusyLabel_ = label;
  patchFailTitle_ = failTitle;
  patchReceived_ = 0;
  patchMoving_ = 0;
  incomingPatchError_ = {};
  patchMessage_.clear();
  auto session = std::make_shared<net::Session>(session_->server(), state_, "", &stop_);
  patchWorker_ = std::thread([this, session, job = std::move(job)] {
    const bool ok = guarded("patches", [&] { job(*session, *patchStore_, incomingPatchError_, patchMessage_); });
    if (!ok)
      incomingPatchError_.message = tr("Patch error. Check log.txt.");
    patchDone_ = true;
  });
  return true;
}
void Application::pollPatches() {
  if (!patchDone_.exchange(false))
    return;
  patchWorker_.join();
  patchRecords_ = patchStore_->records();
  if (!patchBusy_.empty()) {
    // The job moved files: what the check knew of this patch is old now.
    patchHealth_.erase(patchBusy_);
    ++patchLedgerGen_;
    patchCheckWanted_ = screen_ == Screen::Patches;
  }
  if (patchFetching_) {
    patchFetching_ = false;
    patchError_ = incomingPatchError_;
    // A busy server (503) or one still scanning (a partial list) says when
    // to ask again.
    constexpr int MaxRetries = 3;
    patchRetries_ = incomingPatchRetry_ ? patchRetries_ + 1 : 0;
    patchRetryAt_ = incomingPatchRetry_ && patchRetries_ <= MaxRetries
                        ? std::max<uint32_t>(1, SDL_GetTicks() + incomingPatchRetry_ * 1000u) : 0;
    if (!patchError_) {
      serverPatches_ = std::move(incomingPatches_);
      patchesLoaded_ = true;
    } else if (screen_ == Screen::Patches && !patchRows().empty()) {
      // Patches already on the SD card still list; the toast says the
      // shop's list is missing from 전체.
      message(trf("{0} Press Y to try again.", {patchError_.message}));
    }
  } else if (incomingPatchError_) {
    alertTitle_ = patchFailTitle_.empty() ? std::string(tr("Patch failed")) : patchFailTitle_;
    alert_ = incomingPatchError_.message;
    settlePatchJob(true, incomingPatchError_.message);
  } else if (!patchMessage_.empty())
    message(patchMessage_);
  patchBusy_.clear();
  if (screen_ == Screen::Patches && !hasFocusable())
    sidebar_ = true;
}
void Application::startPatchCheck() {
  // A copy of an older ledger is dropped; one cut short goes on.
  if (!patchChecker_ || patchCheckGen_ != patchLedgerGen_) {
    patchChecker_ = std::make_unique<PatchChecker>(patchStore_->sdRoot(), patchStore_->storeRoot(), patchRecords_);
    patchCheckGen_ = patchLedgerGen_;
  }
  patchCheckStop_ = false;
  auto *checker = patchChecker_.get();
  patchCheckWorker_ = std::thread([this, checker] {
    guarded("patch check", [&] { checker->run(CheckLooks, CheckBudget, &patchCheckStop_); });
    patchCheckDone_ = true;
  });
}
// Polled every frame; never waits on the check's thread.
void Application::pollPatchCheck() {
  if (patchCheckDone_.exchange(false)) {
    patchCheckWorker_.join();
    if (patchCheckGen_ == patchLedgerGen_) {
      for (const auto &[id, health] : patchChecker_->health())
        patchHealth_[id] = health;
      if (patchChecker_->finished())
        patchChecker_.reset(); // the next visit looks again from the start
    } else
      patchChecker_.reset();
  }
  // Not while a patch job moves files; a list fetch does not touch the card.
  const bool jobRunning = patchWorker_.joinable() && !patchBusy_.empty();
  if (patchCheckWanted_ && patchStore_ && !patchCheckWorker_.joinable() && !jobRunning) {
    patchCheckWanted_ = false;
    startPatchCheck();
  }
}
namespace {
enum class PatchOp { Get, Update, Enable, Disable, Remove, Game, Reapply };
struct PatchChoice {
  PatchOp op;
  std::string title, body;
};
int patchOpIcon(PatchOp op) {
  switch (op) {
  case PatchOp::Game: return 27;   // gamepad
  case PatchOp::Get: return 4;     // download
  case PatchOp::Update:
  case PatchOp::Reapply: return 3; // refresh
  case PatchOp::Remove: return 37; // trash
  default: return 38;              // power, for on and off
  }
}
// What the management popup offers for a row, most likely first. `missing`:
// files the check found gone from the card while the patch is on.
std::vector<PatchChoice> patchChoices(const Application::PatchRow &row, bool missing) {
  std::vector<PatchChoice> out;
  const bool app = !row.patch.homebrewApp.empty();
  // A patch is no use without its game: when the shop has it, getting the
  // game comes first. An app comes from 홈브류 instead.
  if (!app && !row.installed && row.shopGroup >= 0)
    out.push_back({PatchOp::Game, tr("Download the game"), tr("The game must be installed before the patch can be applied.")});
  if (!row.record) {
    if (row.onServer)
      out.push_back({PatchOp::Get, tr("Download and apply the patch"), tr("Downloads the patch to the SD card and applies it right away.")});
    return out;
  }
  if (missing && row.record->on)
    out.push_back({PatchOp::Reapply, tr("Reapply the patch"), tr("Restores patch files that went missing or were changed.")});
  if (updateAvailable(row))
    out.push_back({PatchOp::Update, tr("Update the patch"), tr("Replaces it with the new version. A patch that was on stays on.")});
  if (row.record->on)
    out.push_back({PatchOp::Disable, tr("Turn off the patch"),
                   app ? tr("Returns the app to its original state. The files stay, so you can turn the patch back on anytime.")
                       : tr("Returns the game to its original state. The files stay, so you can turn the patch back on anytime.")});
  else
    out.push_back({PatchOp::Enable, tr("Turn on the patch"),
                   app ? tr("Applies the downloaded patch. It takes effect the next time you start the app.")
                       : tr("Applies the downloaded patch. It takes effect the next time you start the game.")});
  out.push_back({PatchOp::Remove, tr("Delete the patch"), row.record->on ? tr("Turns the patch off and deletes its files from the SD card.")
                                                                 : tr("Deletes the downloaded patch files from the SD card.")});
  return out;
}
// The names of the patches that are on and share files with this one,
// quoted for the message: A', 'B. Empty when it shares none.
std::string overlapNames(const PatchStore &store, const std::string &id) {
  std::vector<std::string> owners;
  for (const auto &clash : store.conflicts(id))
    if (!clash.owner.empty() && std::find(owners.begin(), owners.end(), clash.owner) == owners.end())
      owners.push_back(clash.owner);
  std::string names;
  for (const auto &owner : owners) {
    const auto *record = store.find(owner);
    names += (names.empty() ? "" : "', '") + (record && !record->name.empty() ? record->name : owner);
  }
  return names;
}
} // namespace
void Application::patchAction(Action a) {
  const auto rows = patchRows();
  const auto games = patchGames(rows);
  const auto *picked = pickedPatch(rows);
  if (confirmPatchRemove_) {
    if (a == Action::Back)
      confirmPatchRemove_ = false;
    else if (a == Action::Accept && picked && settled()) {
      confirmPatchRemove_ = managingPatch_ = false;
      const auto &row = *picked;
      runPatchJob(row.patch.id, Removing, [id = row.patch.id](net::Session &, PatchStore &store, Error &error, std::string &done) {
        if (store.remove(id, error))
          done = tr("Patch deleted.");
      });
    }
    return;
  }
  if (managingPatch_) {
    // B goes back to the game's patch list when it was open.
    if (!picked || a == Action::Back) {
      managingPatch_ = false;
      return;
    }
    const auto choices = patchChoices(*picked, missingFiles(patchHealth_, picked->patch.id));
    if (a == Action::Up)
      patchMenuRow_ = std::max(0, patchMenuRow_ - 1);
    if (a == Action::Down)
      patchMenuRow_ = std::min(int(choices.size()) - 1, patchMenuRow_ + 1);
    if (a == Action::Accept && !choices.empty())
      runPatchOp(int(choices[std::clamp(patchMenuRow_, 0, int(choices.size()) - 1)].op));
    return;
  }
  const auto open = [&](const PatchRow &row) {
    if (patchWorker_.joinable()) {
      message(tr("Try again when the patch task is done."));
      return;
    }
    if (patchChoices(row, missingFiles(patchHealth_, row.patch.id)).empty())
      return;
    patchPickId_ = row.patch.id;
    managingPatch_ = true;
    patchMenuRow_ = 0;
  };
  if (pickingPatch_) {
    const int index = std::clamp(selected_, 0, std::max(0, int(games.size()) - 1));
    if (games.empty() || a == Action::Back) {
      pickingPatch_ = false;
      return;
    }
    const auto &game = games[index];
    patchPickRow_ = std::clamp(patchPickRow_, 0, int(game.rows.size()) - 1);
    if (a == Action::Up)
      patchPickRow_ = std::max(0, patchPickRow_ - 1);
    if (a == Action::Down)
      patchPickRow_ = std::min(int(game.rows.size()) - 1, patchPickRow_ + 1);
    if (a == Action::Accept)
      open(rows[game.rows[patchPickRow_]]);
    return;
  }
  if (a == Action::Previous || a == Action::Next) {
    patchFilter_ = PatchFilter((int(patchFilter_) + (a == Action::Next ? 1 : PatchFilters - 1)) % PatchFilters);
    selected_ = 0;
    if (!hasFocusable())
      sidebar_ = true;
    return;
  }
  if (a == Action::Search) {
    requestSearch();
    return;
  }
  if (rows.empty()) {
    if (a == Action::Left)
      sidebar_ = true;
    return;
  }
  if (a == Action::Up)
    selected_ = std::max(0, selected_ - 1);
  if (a == Action::Down)
    selected_ = std::min(int(games.size()) - 1, selected_ + 1);
  if (a == Action::Left)
    sidebar_ = true;
  selected_ = std::clamp(selected_, 0, int(games.size()) - 1);
  if (a != Action::Accept)
    return;
  // One patch: straight to what to do with it. Several: pick one first.
  const auto &game = games[selected_];
  if (game.rows.size() == 1) {
    open(rows[game.rows.front()]);
    return;
  }
  pickingPatch_ = true;
  patchPickRow_ = 0;
}
void Application::runPatchOp(int opIndex) {
  const auto op = PatchOp(opIndex);
  const auto rows = patchRows();
  const auto *picked = pickedPatch(rows);
  if (!picked)
    return;
  const auto &row = *picked;
  if (op == PatchOp::Game) {
    managingPatch_ = pickingPatch_ = false;
    openDetail(size_t(row.shopGroup));
    return;
  }
  if (op == PatchOp::Remove) {
    confirmPatchRemove_ = true;
    dialogFocus_ = 1; // a stray A keeps the patch
    return;
  }
  managingPatch_ = false;
  // A download waits its turn in 다운로드's queue, as everything does.
  if (op == PatchOp::Get || op == PatchOp::Update) {
    queuePatchJobs({row.patch}, 0);
    return;
  }
  if (op == PatchOp::Reapply) {
    startPatchReapply(row.patch, row.onServer);
    return;
  }
  if (op == PatchOp::Disable)
    startPatchDisable(row.patch);
  else
    startPatchEnable(row.patch);
}
// Fetches, stores and (when fresh and nothing clashes) turns on a patch.
// An app's patch says so in its messages: the app, not a game, restarts.
void Application::startPatchGet(const UserPatch &patch, bool fresh) {
  // Room for the unpacked files plus a little for folders.
  if (sdFree_ && *sdFree_ < patch.size + patch.size / 10 + (1 << 20)) {
    alertTitle_ = tr("Cannot download patch");
    alert_ = trf("Not enough space on the SD card. {0} is needed.", {formatBytes(patch.size)});
    settlePatchJob(true, alert_);
    return;
  }
  runPatchJob(patch.id, fresh ? Getting : Updating,
              [patch, fresh, received = &patchReceived_](net::Session &session, PatchStore &store, Error &error,
                                                         std::string &done) {
                // What an earlier try left in staging is resumed (or cleared)
                // by the download itself, and kept when it drops again.
                std::error_code ec;
                if (!session.downloadPatch(patch, store.staging(patch.id), *received, error))
                  return;
                if (!store.commit(patch, error)) {
                  std::filesystem::remove_all(store.staging(patch.id), ec);
                  return;
                }
                if (!fresh) {
                  done = tr("Patch updated.");
                  return;
                }
                // A fresh patch turns on right away, over any patch already
                // on that shares its files.
                const auto over = overlapNames(store, patch.id);
                if (!store.enable(patch.id, error))
                  return;
                done = !over.empty() ? trf("Patch downloaded and on. Files it shares with '{0}' now come from this patch.", {over})
                       : patch.homebrewApp.empty() ? tr("Patch downloaded and on. Applies when the game restarts.")
                                                   : tr("Patch downloaded and on. Applies when the app restarts.");
              });
}
// The next queued patch goes to the worker the 패치 screen uses, once it is
// free and no alert (a failed patch's among them) waits to be read.
void Application::pumpPatchQueue() {
  if (!patchStore_ || !session_) {
    patchQueue_.clear();
    return;
  }
  while (!patchQueue_.empty() && !patchWorker_.joinable() && !installer_.running() && !retroDownloading() &&
         alert_.empty()) {
    auto patch = patchQueue_.front();
    patchQueue_.pop_front();
    const auto found = patchRecords_.find(patch.id);
    const auto step = patchStep(patch, found == patchRecords_.end() ? nullptr : &found->second);
    if (step == PatchStep::Skip) continue; // already on the card and on
    patchQueueNow_ = patch;
    if (step == PatchStep::Enable) startPatchEnable(patch);
    else startPatchGet(patch, step == PatchStep::Get);
  }
}
// The patch the worker fetches or switches on, and the ones queued after it.
float Application::patchDownloadCard(float y) {
  constexpr float Height = 120;
  const bool running = patchWorker_.joinable() && !patchBusy_.empty();
  UserPatch patch = !patchQueue_.empty() ? patchQueue_.front() : UserPatch{};
  if (running) {
    const auto known = std::find_if(serverPatches_.begin(), serverPatches_.end(),
                                    [&](const UserPatch &p) { return p.id == patchBusy_; });
    patch = patchQueueNow_.id == patchBusy_ ? patchQueueNow_ : known != serverPatches_.end() ? *known : UserPatch{};
    if (patch.name.empty()) patch.name = patchBusy_;
  }
  std::string icon;
  if (const auto *group = findGroup(baseTitleId(patch.titleId))) icon = group->representative.icon;
  else if (!patch.gameIcon.empty())
    icon = patch.gameIcon[0] == '/' && session_ ? session_->server().url + patch.gameIcon : patch.gameIcon;
  if (!installer_.running() && !retroDownloading()) paint_.ring(260, y, 992, Height, 18);
  paint_.rect(260, y, 992, Height, Surface, 18);
  if (icon.empty()) {
    paint_.rect(284, y + 20, 80, 80, Ground, 14);
    paint_.icon(32, 308, y + 44, Ink, 32);
  } else
    cover(icon, 284, y + 20, 80, false, patch.name);
  marquee(patch.name, 386, y + 20, 22, 700);
  const bool fetching = running && (patchBusyLabel_ == Getting || patchBusyLabel_ == Updating);
  const uint64_t got = fetching ? patchReceived_.load() : 0, total = patch.size, moving = patchMoving_.load();
  const double ratio = !running ? 0.0
                       : !fetching ? (moving ? std::min(1.0, double(patchReceived_.load()) / double(moving)) : 1.0)
                       : total     ? std::min(1.0, double(got) / double(total))
                                   : 0.0;
  const auto percent = std::to_string(int(ratio * 100 + 0.5)) + "%";
  paint_.fonts.text(percent, 1228 - paint_.fonts.measure(percent, 22, true), y + 20, 22, Ink, true);
  paint_.rect(386, y + 60, 842, 10, Line, 5);
  if (running) paint_.rect(386, y + 60, std::max(10.0, 842 * ratio), 10, Butter, 5);
  std::string line = !running   ? std::string(tr("Downloads once the game is installed."))
                     : fetching ? trf("{0} · SD card · {1} / {2}", {tr("Patch"), formatBytes(got), formatBytes(total)})
                                : std::string(tr(patchBusyLabel_.c_str()));
  const size_t waiting = patchQueue_.size() - (running ? 0 : 1);
  if (waiting) line += " · " + trf("{0} patches waiting", {std::to_string(waiting)});
  paint_.fonts.text(line, 386, y + 82, 17, Secondary, false, 842);
  return Height;
}
// Files moved so far, for the busy row's bar.
static void trackMoves(PatchStore &store, std::atomic<uint64_t> &done, std::atomic<uint64_t> &total) {
  store.progress([&done, &total](size_t moved, size_t all) {
    done = moved;
    total = all;
  });
}
void Application::startPatchDisable(const UserPatch &patch) {
  runPatchJob(patch.id, Disabling, [this, id = patch.id, app = !patch.homebrewApp.empty()](
                                       net::Session &, PatchStore &store, Error &error, std::string &done) {
    trackMoves(store, patchReceived_, patchMoving_);
    const bool ok = store.disable(id, error);
    store.progress(nullptr);
    if (ok)
      done = app ? tr("Patch off. The app is back to normal after a restart.")
                 : tr("Patch off. The game is back to normal after a restart.");
  });
}
// The store's copies go back first; only when it lacks some does the patch
// come down again, through the same download and commit as an update, which
// turns it back on in its place in the order.
void Application::startPatchReapply(const UserPatch &patch, bool onServer) {
  runPatchJob(patch.id, Reapplying, [this, patch, onServer](net::Session &session, PatchStore &store, Error &error,
                                                            std::string &done) {
    size_t left = 0;
    trackMoves(store, patchReceived_, patchMoving_);
    const bool ok = store.reapply(patch.id, left, error);
    store.progress(nullptr);
    if (!ok)
      return;
    if (left && onServer) {
      patchReceived_ = 0;
      patchMoving_ = 0;
      if (!session.downloadPatch(patch, store.staging(patch.id), patchReceived_, error))
        return;
      if (!store.commit(patch, error)) {
        std::error_code ec;
        std::filesystem::remove_all(store.staging(patch.id), ec);
        return;
      }
      left = 0;
    }
    const bool app = !patch.homebrewApp.empty();
    done = left ? trf("This patch is gone from the server, so {0} files could not be put back.", {std::to_string(left)})
           : app ? tr("Patch reapplied. Restart the app to apply it.")
                 : tr("Patch reapplied. Restart the game to apply it.");
  });
}
void Application::startPatchEnable(const UserPatch &patch) {
  // Files it shares with patches already on come from this one from now on.
  runPatchJob(patch.id, Enabling, [this, id = patch.id, app = !patch.homebrewApp.empty()](
                                      net::Session &, PatchStore &store, Error &error, std::string &done) {
    const auto over = overlapNames(store, id);
    trackMoves(store, patchReceived_, patchMoving_);
    const bool ok = store.enable(id, error);
    store.progress(nullptr);
    if (!ok)
      return;
    done = !over.empty() ? trf("Patch on. Files it shares with '{0}' now come from this patch.", {over})
           : app         ? tr("Patch on. Applies when the app restarts.")
                         : tr("Patch on. Applies when the game restarts.");
  });
}
void Application::patchesScreen() {
  // The list loads once per connection; Y asks again, and so does the
  // time a busy server asked for.
  if (patchRetryAt_ && SDL_GetTicks() >= patchRetryAt_) {
    patchRetryAt_ = 0;
    patchError_ = {};
  }
  if (!patchesLoaded_ && !patchFetching_ && !patchError_ && connected_)
    fetchPatches();
  needHomebrewList(); // apps' patches take the app's name and icon from it
  const auto all = allPatchRows();
  const auto rows = patchRows(all);
  // The title counts what the tab shows, as on 내 게임 and 홈브류.
  int on = 0;
  for (const auto &row : rows)
    on += row.record && row.record->on;
  // As on 세이브: search at the right end, the tabs (L R or a tap) left of
  // it; - reloads the list.
  const float searchX = SearchX;
  searchBar(patchQuery_, searchX);
  const float tabsLeft =
      headerTabs({filterLabel(PatchFilter::Stored), filterLabel(PatchFilter::All), filterLabel(PatchFilter::Updates)},
                 int(patchFilter_), [this](int n) {
                   patchFilter_ = PatchFilter(n);
                   selected_ = 0;
                 }, searchX - 16);
  const float titleEnd =
      pageTitle(tr("Mods"), !patchQuery_.empty() ? countLabel(rows.size(), patchQuery_) : on ? trf("{0} · {1} on", {std::to_string(rows.size()), std::to_string(on)}) : std::to_string(rows.size()), tabsLeft);
  // The background file check says it is running, quietly: a pulsing dot
  // and a word beside the title, while the list stays usable.
  if (patchChecking()) {
    const std::string label = tr("Checking files");
    const float x = titleEnd + 16, w = paint_.fonts.measure(label, 15) + 22;
    if (x + w < tabsLeft - 12) {
      const float t = float(SDL_GetTicks() % 1000) / 1000.f;
      const auto alpha = Uint8(110 + 145 * (t < 0.5f ? t * 2 : (1 - t) * 2));
      paint_.rect(x, 48 - 4, 8, 8, SDL_Color{Pending.r, Pending.g, Pending.b, alpha}, 4);
      paint_.fonts.middle(label, x + 14, 48, 15, Secondary);
    }
  }
  if (rows.empty() && !patchQuery_.empty() && !all.empty()) {
    notice(trf("No patches match '{0}'.", {patchQuery_}), tr("Press B to clear the search."), 23);
    return;
  }
  if (rows.empty()) {
    // Only an actual request shows the skeleton; a failed one says so below.
    if (patchFetching_ || (!patchesLoaded_ && connected_ && !patchError_)) {
      for (int n = 0; n <= VisibleRows; ++n)
        paint_.skeleton(260, RowTop + n * RowStep, 992, RowHeight, 18);
    } else if (!connected_)
      notice(tr("Not connected to a server."), tr("Connect to a server to load the patch list."), 21);
    else if (patchError_ && patchRetryAt_)
      notice(tr("The server is reading its patches."), tr("It loads again by itself shortly."), 25);
    else if (patchError_)
      notice(tr("Could not load the patch list."), patchError_.message, 25);
    else if (patchFilter_ != PatchFilter::All && !all.empty())
      notice(patchFilter_ == PatchFilter::Stored ? tr("No patches installed.") : tr("No patches to update."),
             tr("Patches go into atmosphere/contents/<title ID> on the SD card and take effect the next time the game starts."), 32);
    else
      notice(tr("No patches available."), tr("Patches in the server's patches folder appear here."), 32);
    return;
  }
  const auto games = patchGames(rows);
  selected_ = std::clamp(selected_, 0, int(games.size()) - 1);
  // The focus rides the middle row once past it, the list scrolling under it.
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("patches", 232, RowTop, 1048, 660 - (RowTop), float(std::clamp(selected_ - VisibleRows / 2, 0, std::max(0, int(games.size()) - VisibleRows))),
                          float((int(games.size())) - (VisibleRows)), RowStep);
  const int first = int(top);
  paint_.scrollbar(1266, RowTop, RowStep * VisibleRows - 16, top, VisibleRows, float(games.size()));
  // One more row than fits, cut off by the footer, so a longer list looks
  // like it goes on instead of looking like four games in all.
  clipTo(232, RowTop - 8, 1048, 660 - RowTop + 8);
  for (int i = first; i < int(games.size()) && i <= first + VisibleRows + 1; ++i) {
    const auto &game = games[i];
    const float y = RowTop + (i - top) * RowStep;
    hit(260, y, 992, RowHeight, [this, i] {
      if (inContent() && selected_ == i)
        action(Action::Accept);
      sidebar_ = false;
      selected_ = i;
    });
    if (inContent() && i == selected_ && !pickingPatch_)
      paint_.ring(260, y, 992, RowHeight, 18);
    paint_.rect(260, y, 992, RowHeight, Surface, 18);
    cover(game.icon, 282, y + 14, 80, false, game.game, game.app); // app icons as 홈브류 draws them
    // The name runs up to the state label on the right (at most ~110px) and
    // ends in an ellipsis past it; 상세 정보 shows it whole.
    constexpr float NameWidth = 1230 - 110 - 16 - 382;
    paint_.fonts.text(game.game, 382, y + 14, 22, Ink, true, NameWidth);
    // A single patch is named; several are counted.
    const auto &lead = rows[game.rows.front()];
    std::string meta;
    if (game.rows.size() == 1) {
      meta = lead.patch.name;
      if (!lead.patch.version.empty()) meta += " · v" + lead.patch.version;
      if (!lead.patch.author.empty()) meta += " · " + lead.patch.author;
    } else {
      int on = 0;
      for (const int r : game.rows) on += rows[r].record && rows[r].record->on;
      meta = trf("{0} patches", {std::to_string(game.rows.size())});
      if (on) meta += " · " + trf("{0} on", {std::to_string(on)});
    }
    paint_.fonts.text(meta, 382, y + 46, 18, Secondary, false, 1230 - 16 - 382);
    // What the patches touch, then any warning about the game itself.
    std::vector<std::string> kinds;
    bool mismatch = false, gone = true;
    uint64_t bytes = 0;
    for (const int r : game.rows) {
      const auto &row = rows[r];
      for (const auto &kind : row.patch.kinds)
        if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) kinds.push_back(kind);
      mismatch |= versionMismatch(row.patch.gameVersion, row.installedVersion);
      gone &= !row.onServer;
      bytes += row.record ? row.record->bytes : row.patch.size;
    }
    float at = 382;
    for (const auto &kind : kinds)
      at += paint_.pill(patchKindLabel(kind), at, y + 74) + 6;
    std::string warning;
    const char *note = nullptr;
    for (const int r : game.rows)
      if (const char *n = healthNote(patchHealth_, rows[r]); n && (!note || missingFiles(patchHealth_, rows[r].patch.id)))
        note = n;
    if (note)
      warning = note;
    else if (!game.installed && game.app)
      warning = tr("App not installed · in Homebrew");
    else if (!game.installed)
      warning = lead.shopGroup >= 0 ? tr("Not installed · on the server") : tr("Game not installed");
    else if (mismatch && game.rows.size() == 1 && !game.app)
      warning = trf("Game {0} only", {lead.patch.gameVersion});
    if (gone)
      warning = tr("Gone from server");
    if (!warning.empty()) {
      paint_.icon(25, at + 4, y + 77, game.installed ? Pending : Secondary, 20);
      paint_.fonts.middle(warning, at + 30, y + 87, 16, game.installed ? Pending : Secondary, true);
    }
    // Right column: the state of the game's patches, size or progress below.
    const float right = 1230;
    const PatchRow *busy = nullptr;
    bool update = false, on = false, stored = false;
    for (const int r : game.rows) {
      const auto &row = rows[r];
      if (patchBusy_ == row.patch.id && !row.patch.id.empty()) busy = &row;
      update |= updateAvailable(row);
      on |= row.record && row.record->on;
      stored |= row.record.has_value();
    }
    const std::string state = busy ? tr(patchBusyLabel_.c_str())
                              : update ? tr("Update|item")
                              : on ? tr("On")
                              : stored ? tr("Off") : tr("Not installed");
    const SDL_Color fill = busy || update ? Butter : on ? Good : Line;
    const SDL_Color ink = on && !busy && !update ? Surface : Ink;
    const float w = paint_.fonts.measure(state, 16, true) + 20;
    paint_.rect(right - w, y + 16, w, 30, fill, 8);
    paint_.fonts.middle(state, right - w + 10, y + 31, 16, ink, true);
    const bool switching = patchBusyLabel_ == Enabling || patchBusyLabel_ == Disabling;
    const uint64_t of = switching ? patchMoving_.load() : busy ? busy->patch.size : 0;
    if (busy && of > 0) {
      const float ratio = std::min(1.f, float(patchReceived_.load()) / float(of));
      paint_.rect(right - 180, y + 76, 180, 8, Line, 4);
      paint_.rect(right - 180, y + 76, std::max(8.f, 180 * ratio), 8, Butter, 4);
    } else {
      const auto size = formatBytes(bytes);
      paint_.fonts.text(size, right - paint_.fonts.measure(size, 18, true), y + 68, 18, Ink, true);
    }
  }
  unclip();
}
// A game's patches, one row each; A opens the chosen one's menu.
void Application::patchPicker(const std::vector<PatchRow> &rows, const PatchGame &game) {
  // Laid out like a single patch's 상세 정보: the game beside its icon, then
  // one row per patch with its name, its kinds as pills, its state and size.
  constexpr float Width = 760, Row = 76, Gap = 8, Icon = 64;
  constexpr int Visible = 4;
  const int count = int(game.rows.size()), shown = std::min(count, Visible);
  const float textW = Width - 64 - Icon - 20;
  const float gameH = paint_.fonts.wrapped(game.game, -10000, -10000, 18, Ink, textW, 2, true);
  const float head = std::max(Icon, gameH + 4 + 24);
  const auto body = modal(Width, 24 + head + 16 + shown * (Row + Gap) - Gap + 24, tr("Details"), tr("Close"), "", "", 32);
  const float x = body.x + 32, w = body.w - 64;
  cover(game.icon, x, body.y + 24, int(Icon), false, game.game, game.app);
  paint_.fonts.wrapped(game.game, x + Icon + 20, body.y + 24, 18, Ink, textW, 2, true);
  paint_.fonts.text(trf("{0} patches", {std::to_string(count)}), x + Icon + 20, body.y + 24 + gameH + 4, 16, Secondary);
  const float listTop = body.y + 24 + head + 16;
  // The row at the top follows the cursor, or the finger while it glides.
  const float pickTop = glide("patchPick", x, listTop, w, shown * (Row + Gap), float(std::clamp(patchPickRow_ - Visible / 2, 0, std::max(0, count - Visible))),
                          float((count) - (Visible)), (Row + Gap));
  const int first = int(pickTop);
  clipTo(x, listTop, w, shown * (Row + Gap));
  for (int n = first; n < count && n <= first + Visible; ++n) {
    const auto &row = rows[game.rows[n]];
    const float y = listTop + (n - pickTop) * (Row + Gap);
    hit(x, y, w, Row, [this, n] {
      patchPickRow_ = n;
      action(Action::Accept);
    });
    const bool focused = n == patchPickRow_ && !managingPatch_ && !confirmPatchRemove_;
    if (focused) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    const float nameW = w - 200;
    if (focused) marquee(row.patch.name, x + 20, y + 12, 18, nameW);
    else paint_.fonts.text(row.patch.name, x + 20, y + 12, 18, Ink, true, nameW);
    float at = x + 20;
    for (const auto &kind : row.patch.kinds) {
      const std::string label = patchKindLabel(kind);
      if (at + paint_.fonts.measure(label, 15, true) + 18 > x + 20 + nameW) break;
      at += paint_.pill(label, at, y + 40) + 6;
    }
    std::string meta;
    if (!row.patch.version.empty()) meta = "v" + row.patch.version;
    if (!row.onServer) meta += (meta.empty() ? "" : " · ") + std::string(tr("Gone from server"));
    if (row.record && !row.record->shadowed.empty())
      meta += (meta.empty() ? "" : " · ") + std::string(tr("Partly covered by another patch"));
    if (const char *note = healthNote(patchHealth_, row))
      meta += (meta.empty() ? "" : " · ") + std::string(note);
    if (!meta.empty() && at < x + 20 + nameW)
      paint_.fonts.middle(meta, at + (at > x + 20 ? 6 : 0), y + 53, 15, Secondary, false, x + 20 + nameW - at);
    const bool busy = patchBusy_ == row.patch.id && !row.patch.id.empty();
    const bool update = updateAvailable(row);
    const std::string state = busy ? tr(patchBusyLabel_.c_str())
                              : !row.record ? tr("Not installed")
                              : update ? tr("Update|item")
                              : row.record->on ? tr("On") : tr("Off");
    const bool lit = row.record && row.record->on && !busy && !update;
    const float sw = paint_.fonts.measure(state, 15, true) + 20;
    paint_.rect(x + w - 16 - sw, y + 10, sw, 26, busy || update ? Butter : lit ? Good : Surface, 8);
    paint_.fonts.middle(state, x + w - 16 - sw + 10, y + 23, 15, lit ? Surface : Ink, true);
    const auto size = formatBytes(row.record ? row.record->bytes : row.patch.size);
    paint_.fonts.text(size, x + w - 16 - paint_.fonts.measure(size, 17, true), y + 44, 17, Ink, true);
  }
  unclip();
}
void Application::patchDialog() {
  const auto rows = patchRows();
  const auto games = patchGames(rows);
  if (games.empty()) {
    confirmPatchRemove_ = managingPatch_ = pickingPatch_ = false;
    return;
  }
  if (pickingPatch_)
    patchPicker(rows, games[std::clamp(selected_, 0, int(games.size()) - 1)]);
  const auto *picked = pickedPatch(rows);
  if (!picked) {
    confirmPatchRemove_ = managingPatch_ = false;
    return;
  }
  const auto &row = *picked;
  if (managingPatch_) {
    if (pickingPatch_) paint_.rect(0, 0, 1280, 720, {0, 0, 0, 120});
    patchMenu(row);
  }
  if (!confirmPatchRemove_)
    return;
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  const auto body = modal(700, 24 + 70 + 16, tr("Delete this patch?"), tr("Cancel"), "", tr("Delete"), 37);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(row.game + " · " + row.patch.name, x, y, 21, Ink, true, body.w - 64);
  paint_.fonts.text(row.record && row.record->on ? tr("If it is on, it is turned off and deleted from the SD card.")
                                                 : tr("Deletes the downloaded files from the SD card."),
                    x, y + 38, 19, Secondary, false, body.w - 64);

}
// Like 게임 관리: what the patch is, then one row per thing to do with it.
void Application::patchMenu(const PatchRow &row) {
  const auto choices = patchChoices(row, missingFiles(patchHealth_, row.patch.id));
  constexpr float Width = 760, ChoiceHeight = 64, ChoiceGap = 10, Icon = 80;
  const bool described = !row.patch.description.empty();
  // The whole description, as many lines as the screen leaves room for
  // (header, footer and the rows below); measured off screen first.
  // Beside the icon: the game in bold, the patch under it in light type, then its kinds.
  // Both names show whole (up to two lines) instead of running.
  const float textW = Width - 64 - Icon - 20;
  const auto size = formatBytes(row.record ? row.record->bytes : row.patch.size);
  const float sizeW = paint_.fonts.measure(size, 17, true), nameW = textW - sizeW - 16;
  const float gameH = paint_.fonts.wrapped(row.game, -10000, -10000, 18, Ink, nameW, 2, true);
  const float nameH = paint_.fonts.wrapped(row.patch.name, -10000, -10000, 16, Secondary, nameW, 2);
  const float head = std::max(Icon, gameH + 4 + nameH + 10 + 26);
  const float fixed = 24 + head + 16 + 8 + choices.size() * (ChoiceHeight + ChoiceGap) + 14;
  const int room = std::max(2, int((720 - 48 - 60 - 60 - fixed) / (17 * 1.3f)));
  const float describedHeight =
      described ? paint_.fonts.wrapped(row.patch.description, -10000, -10000, 17, Secondary, Width - 64, room) + 10 : 0;
  const float bodyHeight = fixed + describedHeight;
  const auto body = modal(Width, bodyHeight, tr("Details"), tr("Close"), "", "", 32);
  const float x = body.x + 32, w = body.w - 64, textX = x + Icon + 20;
  float y = body.y + 24;
  cover(row.icon, x, y, int(Icon), false, row.game, !row.patch.homebrewApp.empty());
  paint_.fonts.wrapped(row.game, textX, y, 18, Ink, nameW, 2, true);
  const float nameY = y + gameH + 4;
  paint_.fonts.wrapped(row.patch.name, textX, nameY, 16, Secondary, nameW, 2);
  paint_.fonts.text(size, x + w - sizeW, y + 1, 17, Ink, true);
  const float pillY = nameY + nameH + 10;
  float at = textX;
  for (const auto &kind : row.patch.kinds)
    at += paint_.pill(patchKindLabel(kind), at, pillY) + 6;
  std::string meta;
  if (!row.patch.version.empty()) meta = "v" + row.patch.version;
  if (!row.patch.author.empty()) meta += (meta.empty() ? "" : " · ") + row.patch.author;
  if (row.record && !row.record->shadowed.empty())
    meta += (meta.empty() ? "" : " · ") + std::string(tr("Partly covered by another patch"));
  if (const char *note = healthNote(patchHealth_, row))
    meta += (meta.empty() ? "" : " · ") + std::string(note);
  if (!meta.empty())
    paint_.fonts.middle(meta, at + (at > textX ? 6 : 0), pillY + 13, 17, Secondary, false, x + w - at);
  y += head + 16;
  if (described) {
    paint_.fonts.wrapped(row.patch.description, x, y, 17, Secondary, w, room);
    y += describedHeight;
  }
  y += 8;
  for (int i = 0; i < int(choices.size()); ++i) {
    hit(x, y, w, ChoiceHeight, [this, i] {
      patchMenuRow_ = i;
      action(Action::Accept);
    });
    if (i == patchMenuRow_ && !confirmPatchRemove_)
      paint_.focusRow(x, y, w, ChoiceHeight, Ground, 12);
    else
      paint_.rect(x, y, w, ChoiceHeight, Ground, 12);
    const SDL_Color ink = choices[i].op == PatchOp::Remove ? Bad : Ink;
    paint_.icon(patchOpIcon(choices[i].op), x + 20, y + 19, ink, 26);
    paint_.fonts.text(choices[i].title, x + 62, y + 10, 19, ink, true);
    paint_.fonts.text(choices[i].body, x + 62, y + 37, 16, Secondary, false, w - 82);
    y += ChoiceHeight + ChoiceGap;
  }
}
} // namespace foil::ui
