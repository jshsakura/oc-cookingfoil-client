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
bool updateAvailable(const Application::PatchRow &row) {
  return row.record && row.onServer && !row.patch.hash.empty() && row.record->hash != row.patch.hash;
}
const char *filterLabel(Application::PatchFilter filter) {
  switch (filter) {
  case Application::PatchFilter::Stored: return tr("설치됨");
  case Application::PatchFilter::Updates: return tr("업데이트|탭");
  default: return tr("전체");
  }
}
// Busy labels stay Korean keys in patchBusyLabel_ and are translated only
// when drawn, so the progress check compares keys, never screen text.
constexpr const char *Removing = "지우는 중";     // i18n: keep
constexpr const char *Updating = "업데이트 중";   // i18n: keep
constexpr const char *Getting = "받는 중";        // i18n: keep
constexpr const char *Disabling = "끄는 중";      // i18n: keep
constexpr const char *Enabling = "켜는 중";       // i18n: keep
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
  patchBusy_ = id;
  patchBusyLabel_ = label;
  patchFailTitle_ = failTitle;
  patchReceived_ = 0;
  incomingPatchError_ = {};
  patchMessage_.clear();
  auto session = std::make_shared<net::Session>(session_->server(), state_, "", &stop_);
  patchWorker_ = std::thread([this, session, job = std::move(job)] {
    const bool ok = guarded("patches", [&] { job(*session, *patchStore_, incomingPatchError_, patchMessage_); });
    if (!ok)
      incomingPatchError_.message = tr("패치를 처리하다 오류가 났습니다. log.txt를 확인합니다.");
    patchDone_ = true;
  });
  return true;
}
void Application::pollPatches() {
  if (!patchDone_.exchange(false))
    return;
  patchWorker_.join();
  patchRecords_ = patchStore_->records();
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
      message(trf("{0} Y로 다시 시도합니다.", {patchError_.message}));
    }
  } else if (incomingPatchError_) {
    alertTitle_ = patchFailTitle_.empty() ? std::string(tr("패치를 처리하지 못했습니다")) : patchFailTitle_;
    alert_ = incomingPatchError_.message;
  } else if (!patchMessage_.empty())
    message(patchMessage_);
  patchBusy_.clear();
  if (screen_ == Screen::Patches && !hasFocusable())
    sidebar_ = true;
}
namespace {
enum class PatchOp { Get, Update, Enable, Disable, Remove, Game };
struct PatchChoice {
  PatchOp op;
  std::string title, body;
};
int patchOpIcon(PatchOp op) {
  switch (op) {
  case PatchOp::Game: return 27;   // gamepad
  case PatchOp::Get: return 4;     // download
  case PatchOp::Update: return 3;  // refresh
  case PatchOp::Remove: return 37; // trash
  default: return 38;              // power, for on and off
  }
}
// What the management popup offers for a row, most likely first.
std::vector<PatchChoice> patchChoices(const Application::PatchRow &row) {
  std::vector<PatchChoice> out;
  const bool app = !row.patch.homebrewApp.empty();
  // A patch is no use without its game: when the shop has it, getting the
  // game comes first. An app comes from 홈브류 instead.
  if (!app && !row.installed && row.shopGroup >= 0)
    out.push_back({PatchOp::Game, tr("게임 받으러 가기"), tr("서버에서 이 게임을 설치합니다. 패치는 게임이 있어야 적용됩니다.")});
  if (!row.record) {
    if (row.onServer)
      out.push_back({PatchOp::Get, tr("다운로드 및 적용"), tr("SD 카드에 받아 바로 적용합니다. 겹치는 다른 패치는 꺼 둡니다.")});
    return out;
  }
  if (updateAvailable(row))
    out.push_back({PatchOp::Update, tr("업데이트|동작"), tr("켜져 있던 패치는 바꾼 뒤 다시 켭니다.")});
  if (row.record->on)
    out.push_back({PatchOp::Disable, tr("끄기"), app ? tr("앱을 다시 켜면 원래대로 돌아옵니다. 받은 파일은 남겨 둡니다.")
                                                     : tr("게임을 다시 켜면 원래대로 돌아옵니다. 받은 파일은 남겨 둡니다.")});
  else
    out.push_back({PatchOp::Enable, tr("켜기"), app ? tr("앱을 다시 켜면 적용됩니다.") : tr("게임을 다시 켜면 적용됩니다.")});
  out.push_back({PatchOp::Remove, tr("삭제"), row.record->on ? tr("패치를 끄고 SD 카드에서 지웁니다.")
                                                            : tr("SD 카드에 받아 둔 파일을 지웁니다.")});
  return out;
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
          done = tr("패치를 지웠습니다.");
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
    const auto choices = patchChoices(*picked);
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
      message(tr("패치 작업이 끝난 뒤 다시 시도합니다."));
      return;
    }
    if (patchChoices(row).empty())
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
  if (op == PatchOp::Get || op == PatchOp::Update) {
    startPatchGet(row.patch, op == PatchOp::Get);
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
    alertTitle_ = tr("패치를 받을 수 없습니다");
    alert_ = trf("SD 카드 공간이 부족합니다. {0}가 필요합니다.", {formatBytes(patch.size)});
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
                  done = tr("패치를 업데이트했습니다.");
                  return;
                }
                // A fresh patch turns on right away unless it would replace
                // someone else's files; then it waits, switched off.
                if (!store.conflicts(patch.id).empty()) {
                  done = tr("패치를 받았습니다. 다른 패치와 겹쳐서 꺼 두었습니다.");
                  return;
                }
                if (store.enable(patch.id, error))
                  done = patch.homebrewApp.empty() ? tr("패치를 받아 켰습니다. 게임을 다시 켜면 적용됩니다.")
                                                   : tr("패치를 받아 켰습니다. 앱을 다시 켜면 적용됩니다.");
              });
}
void Application::queuePatches(std::vector<UserPatch> patches, bool afterInstall) {
  for (auto &patch : patches) {
    // Picked again before it ran: once in the queue is enough.
    const bool queued = std::any_of(patchQueue_.begin(), patchQueue_.end(),
                                    [&](const UserPatch &p) { return p.id == patch.id; });
    if (!queued && patchBusy_ != patch.id) patchQueue_.push_back(std::move(patch));
  }
  if (afterInstall && !patchQueue_.empty()) patchQueueWaits_ = true;
  pumpPatchQueue();
}
// The next queued patch goes to the worker the 패치 screen uses, once it is
// free and no alert (a failed patch's among them) waits to be read.
void Application::pumpPatchQueue() {
  if (!patchStore_ || !session_) {
    patchQueue_.clear();
    return;
  }
  while (!patchQueue_.empty() && !patchQueueWaits_ && !patchWorker_.joinable() && alert_.empty()) {
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
  paint_.fonts.text(patch.name, 386, y + 20, 22, Ink, true, 700);
  const bool fetching = running && (patchBusyLabel_ == Getting || patchBusyLabel_ == Updating);
  const uint64_t got = fetching ? patchReceived_.load() : 0, total = patch.size;
  const double ratio = !running ? 0.0 : !fetching ? 1.0 : total ? std::min(1.0, double(got) / double(total)) : 0.0;
  const auto percent = std::to_string(int(ratio * 100 + 0.5)) + "%";
  paint_.fonts.text(percent, 1228 - paint_.fonts.measure(percent, 22, true), y + 20, 22, Ink, true);
  paint_.rect(386, y + 60, 842, 10, Line, 5);
  if (running) paint_.rect(386, y + 60, std::max(10.0, 842 * ratio), 10, Butter, 5);
  std::string line = !running   ? std::string(tr("게임 설치가 끝나면 받습니다."))
                     : fetching ? trf("{0} · SD 카드 · {1} / {2}", {tr("패치|항목"), formatBytes(got), formatBytes(total)})
                                : std::string(tr(patchBusyLabel_.c_str()));
  const size_t waiting = patchQueue_.size() - (running ? 0 : 1);
  if (waiting) line += " · " + trf("패치 {0}개 대기", {std::to_string(waiting)});
  paint_.fonts.text(line, 386, y + 82, 17, Secondary, false, 842);
  return Height;
}
void Application::startPatchDisable(const UserPatch &patch) {
  runPatchJob(patch.id, Disabling, [id = patch.id, app = !patch.homebrewApp.empty()](
                                       net::Session &, PatchStore &store, Error &error, std::string &done) {
    if (store.disable(id, error))
      done = app ? tr("패치를 껐습니다. 앱을 다시 켜면 원래대로 돌아옵니다.")
                 : tr("패치를 껐습니다. 게임을 다시 켜면 원래대로 돌아옵니다.");
  });
}
void Application::startPatchEnable(const UserPatch &patch) {
  if (patchStore_) {
    const auto clash = patchStore_->conflicts(patch.id);
    if (!clash.empty()) {
      const auto owner = clash.front().owner;
      const auto found = patchRecords_.find(owner);
      alertTitle_ = tr("패치를 켤 수 없습니다");
      alert_ = owner.empty()
                   ? trf("직접 넣어 둔 파일과 겹칩니다. {0} 를 확인합니다.", {clash.front().path})
                   : trf("'{0}' 패치와 같은 파일을 바꿉니다. 그 패치를 끈 뒤 켭니다.",
                         {found == patchRecords_.end() ? owner : found->second.name});
      return;
    }
  }
  runPatchJob(patch.id, Enabling, [id = patch.id, app = !patch.homebrewApp.empty()](
                                      net::Session &, PatchStore &store, Error &error, std::string &done) {
    if (store.enable(id, error))
      done = app ? tr("패치를 켰습니다. 앱을 다시 켜면 적용됩니다.") : tr("패치를 켰습니다. 게임을 다시 켜면 적용됩니다.");
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
  pageTitle(tr("패치|메뉴"), on ? trf("{0} · 켜짐 {1}", {std::to_string(rows.size()), std::to_string(on)}) : std::to_string(rows.size()), tabsLeft);
  if (rows.empty() && !patchQuery_.empty() && !all.empty()) {
    notice(trf("'{0}'에 맞는 패치가 없습니다.", {patchQuery_}), tr("B로 검색어를 지웁니다."), 23);
    return;
  }
  if (rows.empty()) {
    // Only an actual request shows the skeleton; a failed one says so below.
    if (patchFetching_ || (!patchesLoaded_ && connected_ && !patchError_)) {
      for (int n = 0; n <= VisibleRows; ++n)
        paint_.skeleton(260, RowTop + n * RowStep, 992, RowHeight, 18);
    } else if (!connected_)
      notice(tr("서버에 연결되어 있지 않습니다."), tr("서버에 연결한 뒤 패치 목록을 불러옵니다."), 21);
    else if (patchError_ && patchRetryAt_)
      notice(tr("서버가 패치를 읽고 있습니다."), tr("잠시 뒤 자동으로 다시 불러옵니다."), 25);
    else if (patchError_)
      notice(tr("패치 목록을 불러오지 못했습니다."), patchError_.message, 25);
    else if (patchFilter_ != PatchFilter::All && !all.empty())
      notice(patchFilter_ == PatchFilter::Stored ? tr("설치한 패치가 없습니다.") : tr("업데이트할 패치가 없습니다."),
             tr("패치는 SD 카드의 atmosphere/contents/타이틀 ID 폴더에 설치되고, 게임을 다시 켜면 적용됩니다."), 32);
    else
      notice(tr("받을 수 있는 패치가 없습니다."), tr("서버의 patches 폴더에 패치를 넣으면 여기에 나타납니다."), 32);
    return;
  }
  const auto games = patchGames(rows);
  selected_ = std::clamp(selected_, 0, int(games.size()) - 1);
  // The focus rides the middle row once past it, the list scrolling under it.
  const int first = std::clamp(selected_ - VisibleRows / 2, 0, std::max(0, int(games.size()) - VisibleRows));
  paint_.scrollbar(1266, RowTop, RowStep * VisibleRows - 16, float(first), VisibleRows, float(games.size()));
  // One more row than fits, cut off by the footer, so a longer list looks
  // like it goes on instead of looking like four games in all.
  for (int i = first; i < int(games.size()) && i <= first + VisibleRows; ++i) {
    const auto &game = games[i];
    const float y = RowTop + (i - first) * RowStep;
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
    paint_.fonts.text(game.game, 382, y + 14, 22, Ink, true, 560);
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
      meta = trf("패치 {0}개", {std::to_string(game.rows.size())});
      if (on) meta += " · " + trf("켜짐 {0}개", {std::to_string(on)});
    }
    paint_.fonts.text(meta, 382, y + 46, 18, Secondary, false, 560);
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
    if (!game.installed && game.app)
      warning = tr("앱 미설치 · 홈브류에 있음");
    else if (!game.installed)
      warning = lead.shopGroup >= 0 ? tr("게임 미설치 · 서버에 있음") : tr("게임 미설치");
    else if (mismatch && game.rows.size() == 1 && !game.app)
      warning = trf("게임 {0} 전용", {lead.patch.gameVersion});
    if (gone)
      warning = tr("서버에서 사라짐");
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
                              : update ? tr("업데이트|항목")
                              : on ? tr("켜짐")
                              : stored ? tr("꺼짐") : tr("설치 안 됨");
    const SDL_Color fill = busy || update ? Butter : on ? Good : Line;
    const SDL_Color ink = on && !busy && !update ? Surface : Ink;
    const float w = paint_.fonts.measure(state, 16, true) + 20;
    paint_.rect(right - w, y + 16, w, 30, fill, 8);
    paint_.fonts.middle(state, right - w + 10, y + 31, 16, ink, true);
    if (busy && busy->patch.size > 0 && patchBusyLabel_ != Enabling && patchBusyLabel_ != Disabling) {
      const float ratio = std::min(1.f, float(patchReceived_.load()) / float(busy->patch.size));
      paint_.rect(right - 180, y + 76, 180, 8, Line, 4);
      paint_.rect(right - 180, y + 76, std::max(8.f, 180 * ratio), 8, Butter, 4);
    } else {
      const auto size = formatBytes(bytes);
      paint_.fonts.text(size, right - paint_.fonts.measure(size, 18, true), y + 68, 18, Ink, true);
    }
  }
}
// A game's patches, one row each; A opens the chosen one's menu.
void Application::patchPicker(const std::vector<PatchRow> &rows, const PatchGame &game) {
  constexpr float Width = 760, Row = 76, Gap = 8;
  constexpr int Visible = 5;
  const int count = int(game.rows.size()), shown = std::min(count, Visible);
  const auto body = modal(Width, 16 + shown * (Row + Gap) + 8, game.game, tr("닫기"), "", "", 32);
  const float x = body.x + 24, w = body.w - 48;
  const int first = std::clamp(patchPickRow_ - Visible / 2, 0, std::max(0, count - Visible));
  for (int n = first; n < count && n < first + Visible; ++n) {
    const auto &row = rows[game.rows[n]];
    const float y = body.y + 16 + (n - first) * (Row + Gap);
    hit(x, y, w, Row, [this, n] {
      patchPickRow_ = n;
      action(Action::Accept);
    });
    if (n == patchPickRow_ && !managingPatch_ && !confirmPatchRemove_)
      paint_.focusRow(x, y, w, Row, Ground, 12);
    else
      paint_.rect(x, y, w, Row, Ground, 12);
    paint_.fonts.text(row.patch.name, x + 20, y + 10, 21, Ink, true, w - 220);
    std::string meta;
    if (!row.patch.version.empty()) meta = "v" + row.patch.version;
    for (const auto &kind : row.patch.kinds) meta += (meta.empty() ? "" : " · ") + std::string(patchKindLabel(kind));
    if (!row.onServer) meta += (meta.empty() ? "" : " · ") + std::string(tr("서버에서 사라짐"));
    paint_.fonts.text(meta, x + 20, y + 42, 17, Secondary, false, w - 220);
    const bool busy = patchBusy_ == row.patch.id && !row.patch.id.empty();
    const bool update = updateAvailable(row);
    const std::string state = busy ? tr(patchBusyLabel_.c_str())
                              : !row.record ? tr("설치 안 됨")
                              : update ? tr("업데이트|항목")
                              : row.record->on ? tr("켜짐") : tr("꺼짐");
    const bool lit = row.record && row.record->on && !busy && !update;
    const float sw = paint_.fonts.measure(state, 16, true) + 20;
    paint_.rect(x + w - 16 - sw, y + 10, sw, 28, busy || update ? Butter : lit ? Good : Surface, 8);
    paint_.fonts.middle(state, x + w - 16 - sw + 10, y + 24, 16, lit ? Surface : Ink, true);
    const auto size = formatBytes(row.record ? row.record->bytes : row.patch.size);
    paint_.fonts.text(size, x + w - 16 - paint_.fonts.measure(size, 17, true), y + 44, 17, Ink, true);
  }
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
  const auto body = modal(700, 24 + 70 + 16, tr("패치를 지우시겠습니까?"), tr("취소"), "", tr("지우기"), 37);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(row.game + " · " + row.patch.name, x, y, 21, Ink, true, body.w - 64);
  paint_.fonts.text(row.record && row.record->on ? tr("켜져 있으면 먼저 끄고 SD 카드에서 지웁니다.")
                                                 : tr("SD 카드에 받아 둔 파일을 지웁니다."),
                    x, y + 38, 19, Secondary, false, body.w - 64);

}
// Like 게임 관리: what the patch is, then one row per thing to do with it.
void Application::patchMenu(const PatchRow &row) {
  const auto choices = patchChoices(row);
  constexpr float Width = 760, ChoiceHeight = 64, ChoiceGap = 10, Icon = 80;
  const bool described = !row.patch.description.empty();
  const float bodyHeight = 24 + Icon + 16 + (described ? 56 : 0) + 8 +
                           choices.size() * (ChoiceHeight + ChoiceGap) + 14;
  const auto body = modal(Width, bodyHeight, row.game, tr("닫기"), "", "", 32);
  const float x = body.x + 32, w = body.w - 64, textX = x + Icon + 20, textW = w - Icon - 20;
  float y = body.y + 24;
  // The game's icon beside the patch: its name, then what kind and how big.
  cover(row.icon, x, y, int(Icon), false, row.game, !row.patch.homebrewApp.empty());
  const auto size = formatBytes(row.record ? row.record->bytes : row.patch.size);
  const float sizeW = paint_.fonts.measure(size, 18, true);
  paint_.fonts.text(row.patch.name, textX, y + 4, 22, Ink, true, textW - sizeW - 16);
  paint_.fonts.text(size, x + w - sizeW, y + 8, 18, Ink, true);
  float at = textX;
  for (const auto &kind : row.patch.kinds)
    at += paint_.pill(patchKindLabel(kind), at, y + 44) + 6;
  std::string meta;
  if (!row.patch.version.empty()) meta = "v" + row.patch.version;
  if (!row.patch.author.empty()) meta += (meta.empty() ? "" : " · ") + row.patch.author;
  if (!meta.empty())
    paint_.fonts.middle(meta, at + (at > textX ? 6 : 0), y + 57, 17, Secondary, false, x + w - at);
  y += Icon + 16;
  if (described) {
    paint_.fonts.wrapped(row.patch.description, x, y, 18, Secondary, w, 2);
    y += 56;
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
    paint_.fonts.text(choices[i].title, x + 62, y + 8, 21, ink, true);
    paint_.fonts.text(choices[i].body, x + 62, y + 37, 17, Secondary, false, w - 82);
    y += ChoiceHeight + ChoiceGap;
  }
}
} // namespace foil::ui
