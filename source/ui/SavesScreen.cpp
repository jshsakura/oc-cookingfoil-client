#include "core/Json.hpp"
#include "ui/Application.hpp"
#include "core/Log.hpp"
#include "net/ShopSavesApi.hpp"
#include "platform/Saves.hpp"
#include <algorithm>
#include <filesystem>
namespace foil::ui {
namespace {
constexpr float Top = 90, RowHeight = 84, RowStep = 96;
constexpr int Visible = 5;
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
  if (shopAppSaveAsk_) {
    if (a == Action::Back) shopAppSaveAsk_ = false;
    else if (a == Action::Accept && settled()) {
      shopAppSaveAsk_ = false;
      if (patchWorker_.joinable()) message(tr("패치 작업이 끝난 뒤 다시 시도합니다."));
      else if (!shopSaves_.empty()) applyHomebrewSave(shopSaves_[std::clamp(shopSavePick_, 0, int(shopSaves_.size()) - 1)]);
    }
    return;
  }
  if (savePicking_) {
    const auto groups = shopSaveGroups();
    const int count = groups.empty() ? 0 : groups[std::clamp(shopSaveRow_, 0, int(groups.size()) - 1)].second;
    if (a == Action::Up) savePickRow_ = std::max(0, savePickRow_ - 1);
    if (a == Action::Down) savePickRow_ = std::min(count - 1, savePickRow_ + 1);
    if (a == Action::Back) savePicking_ = false;
    if (a == Action::Accept && count) {
      savePicking_ = false;
      shopSavePick_ = groups[std::clamp(shopSaveRow_, 0, int(groups.size()) - 1)].first + savePickRow_;
      chooseShopSave();
    }
    return;
  }
  if (saveNeedsGame_) {
    // The popup for a save whose game is not here: 게임 받으러 가기 or 닫기.
    if (a == Action::Back) saveNeedsGame_ = false;
    if (a == Action::Accept) {
      saveNeedsGame_ = false;
      if (saveGameGroup_ >= 0) openDetail(size_t(saveGameGroup_));
    }
    return;
  }
  if (a == Action::Previous || a == Action::Next) {
    savesTab_ = (savesTab_ + (a == Action::Next ? 1 : SavesTabs - 1)) % SavesTabs;
    if (savesTab_ == 1 && !shopSavesLoaded_) loadShopSaves();
    if (savesTab_ == 2) dropboxEnter();
    return;
  }
  if (a == Action::Queue || a == Action::Reload) {
    if (savesTab_ == 2) dropboxAction(a);
    else if (savesTab_ == 1) loadShopSaves();
    else if (savesTab_ == 0) refreshInstalled();
    return;
  }
  if (a == Action::Left) {
    sidebar_ = true;
    return;
  }
  // X searches every tab, 드롭박스 too (Y unlinks there).
  if (a == Action::Search) {
    requestSearch();
    return;
  }
  if (savesTab_ == 2) {
    dropboxAction(a);
    return;
  }
  if (savesTab_ == 1) {
    const int games = int(shopSaveGroups().size());
    if (!games) return;
    if (a == Action::Up) shopSaveRow_ = std::max(0, shopSaveRow_ - 1);
    if (a == Action::Down) shopSaveRow_ = std::min(games - 1, shopSaveRow_ + 1);
    if (a == Action::Accept) chooseShopGame();
    return;
  }
  const int count = int(saveGameRows().size());
  if (!count) return;
  if (a == Action::Up) selected_ = std::max(0, selected_ - 1);
  if (a == Action::Down) selected_ = std::min(count - 1, selected_ + 1);
  if (a == Action::Accept) openManagement();
}
void Application::savesScreen() {
  const bool shop = savesTab_ == 1;
  // Search at the right end, as on every list; the tabs left of it.
  const float searchX = SearchX;
  searchBar(savesQuery_, searchX);
  const float tabsLeft = headerTabs({tr("내 기기"), tr("공유"), tr("드롭박스")}, savesTab_, [this](int n) {
    savesTab_ = n;
    if (n == 1 && !shopSavesLoaded_) loadShopSaves();
    if (n == 2) dropboxEnter();
  }, searchX - 16);
  pageTitle(tr("세이브"),
            shop ? (shopSavesLoaded_ ? std::to_string(shopSaveGroups().size()) : std::string())
            : savesTab_ == 0 && installed_.installed.complete ? std::to_string(saveGameRows().size())
                                                             : "",
            tabsLeft);
  if (savesTab_ == 2) {
    dropboxTab();
    return;
  }
  if (shop) {
    shopSavesList();
    if (managedGame_) management();
    return;
  }
  if (deviceBusy_ && !managedGame_) {
    for (int n = 0; n <= Visible; ++n) paint_.skeleton(260, Top + n * RowStep, 992, RowHeight, 16);
    return;
  }
  if (deviceError_ && !managedGame_) {
    notice(tr("설치 목록을 읽지 못했습니다."), deviceError_.message, 25);
    return;
  }
  if (installed_.games.empty()) {
    notice(tr("세이브를 관리할 게임이 없습니다."), tr("게임을 설치하고 한 번 실행하면 세이브가 생깁니다."), 36);
    return;
  }
  const auto rows = saveGameRows();
  if (rows.empty()) {
    notice(trf("'{0}'에 맞는 게임이 없습니다.", {savesQuery_}), tr("B로 검색어를 지웁니다."), 23);
    return;
  }
  const int count = int(rows.size());
  selected_ = std::clamp(selected_, 0, count - 1);
  const int first = std::clamp(selected_ - Visible / 2, 0, std::max(0, count - Visible));
  paint_.scrollbar(1266, Top, RowStep * Visible - 12, float(first), Visible, float(count));
  const auto backupsRoot = saves::keeperRoot(sdRoot_);
  for (int i = first; i < count && i <= first + Visible; ++i) {
    const auto &g = installed_.games[rows[i]];
    const float y = Top + (i - first) * RowStep;
    hit(260, y, 992, RowHeight, [this, i] {
      if (inContent() && selected_ == i) action(Action::Accept);
      sidebar_ = false;
      selected_ = i;
    });
    if (inContent() && i == selected_ && !managedGame_) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    std::string icon = "installed:" + g.titleId;
    if (!g.iconBytes.empty()) images_->provide(icon, g.iconBytes);
    else {
      const auto *match = findGroup(g.titleId);
      icon = match ? match->representative.icon : "";
    }
    cover(icon, 278, y + 10, 64, false, g.name);
    paint_.fonts.text(g.name, 360, y + 12, 21, Ink, true, 640);
    paint_.fonts.text(g.titleId, 360, y + 46, 16, Secondary);
    // How many backups the SD card already holds (save-keeper's included),
    // counted once per visit rather than every frame.
    auto counted = backupCounts_.find(g.applicationId);
    if (counted == backupCounts_.end())
      counted = backupCounts_.emplace(g.applicationId, saves::countBackups(backupsRoot, g.applicationId)).first;
    const int backups = counted->second;
    const std::string state = backups ? trf("백업 {0}개", {std::to_string(backups)}) : std::string(tr("백업 없음"));
    const float w = paint_.fonts.measure(state, 16, true) + 20;
    paint_.rect(1230 - w, y + 28, w, 28, backups ? Good : Line, 8);
    paint_.fonts.middle(state, 1230 - w + 10, y + 42, 16, backups ? Surface : Ink, true);
  }
  if (managedGame_) management();
}
void Application::loadShopSaves() {
  if (!session_ || !connected_ || shopSavesWorker_.joinable()) return;
  const auto server = session_->server();
  const auto state = state_;
  shopSavesError_ = {};
  shopSavesWorker_ = std::thread([this, server, state] {
    net::ShopSavesReply reply;
    if (!guarded("shop saves", [&] { incomingShopSaves_ = net::shopSaves(server, state, reply, &stop_).value; }))
      reply.error.message = tr("서버 세이브 목록을 불러오지 못했습니다.");
    incomingShopSavesError_ = reply.error;
    incomingShopSavesMissing_ = reply.missing;
    shopSavesDone_ = true;
  });
}
void Application::pollShopSaves() {
  if (!shopSavesDone_.exchange(false)) return;
  shopSavesWorker_.join();
  shopSavesLoaded_ = true;
  shopSavesError_ = incomingShopSavesError_;
  shopSavesMissing_ = incomingShopSavesMissing_;
  shopSaves_ = std::move(incomingShopSaves_);
  // An app's saves list here under the app, and in its popup in 홈브류.
  homebrewSaves_.clear();
  for (const auto &save : shopSaves_)
    if (!save.homebrewApp.empty()) homebrewSaves_.push_back(save);
  shopSaves_.erase(std::remove_if(shopSaves_.begin(), shopSaves_.end(),
                                  [](const auto &save) { return save.titleId.empty() && save.homebrewApp.empty(); }),
                   shopSaves_.end());
  sortShopSaves();
}
std::string Application::shopSaveTitle(const shopsaves::ShopSave &save) const {
  return save.homebrewApp.empty() ? shopsaves::gameName(save, languageCode())
                                  : homebrew::appTitle(homebrew_, save.homebrewApp);
}
std::string Application::shopSaveIcon(const shopsaves::ShopSave &save) const {
  if (save.homebrewApp.empty()) return save.gameIcon;
  const auto *app = homebrew::findApp(homebrew_, save.homebrewApp);
  return app ? app->iconUrl : "";
}
bool Application::shopSaveTargetHere(const shopsaves::ShopSave &save) const {
  if (!save.homebrewApp.empty()) {
    const auto *app = homebrew::findApp(homebrew_, save.homebrewApp);
    return !app || homebrewStatus(*app) != homebrew::Status::Available;
  }
  return std::any_of(installed_.games.begin(), installed_.games.end(),
                     [&](const auto &g) { return g.titleId == save.titleId; });
}
void Application::sortShopSaves() {
  // A popup holds a save by its place in the list: the order waits for it.
  if (savePicking_ || saveNeedsGame_ || shopAppSaveAsk_) return;
  // Games and apps on this console first, then by name.
  std::stable_sort(shopSaves_.begin(), shopSaves_.end(), [this](const auto &a, const auto &b) {
    const bool ha = shopSaveTargetHere(a), hb = shopSaveTargetHere(b);
    if (ha != hb) return ha;
    const auto na = shopSaveTitle(a), nb = shopSaveTitle(b);
    if (na != nb) return na < nb;
    // One game's or app's saves side by side, for the groups.
    return homebrew::ownerKey(a.titleId, a.homebrewApp) < homebrew::ownerKey(b.titleId, b.homebrewApp);
  });
  shopSaveRow_ = std::clamp(shopSaveRow_, 0, std::max(0, int(shopSaveGroups().size()) - 1));
}
// Runs of one game's or app's saves in shopSaves_ (sorted so they sit
// together): the list shows one row per game or app.
std::vector<std::pair<int, int>> Application::shopSaveGroups() const {
  std::vector<std::pair<int, int>> out;
  const auto owner = [this](int i) { return homebrew::ownerKey(shopSaves_[i].titleId, shopSaves_[i].homebrewApp); };
  for (int i = 0; i < int(shopSaves_.size()); ++i)
    if (!out.empty() && owner(out.back().first) == owner(i)) ++out.back().second;
    else out.push_back({i, 1});
  // The search keeps a game or app when its name or any of its saves matches.
  out.erase(std::remove_if(out.begin(), out.end(),
                           [&](const std::pair<int, int> &group) {
                             if (savesQuery_.empty()) return false;
                             const auto &head = shopSaves_[group.first];
                             if (textMatches({shopSaveTitle(head), head.homebrewApp}, savesQuery_)) return false;
                             for (int i = group.first; i < group.first + group.second; ++i)
                               if (textMatches({shopSaves_[i].name, shopSaves_[i].titleId}, savesQuery_)) return false;
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
// How many server saves the game under the cursor has (the footer says
// 세이브 고르기 when there is more than one).
int Application::shopSaveChoices() const {
  const auto groups = shopSaveGroups();
  return groups.empty() ? 0 : groups[std::clamp(shopSaveRow_, 0, int(groups.size()) - 1)].second;
}
// A on a game's row: one save goes straight on, several open a picker.
void Application::chooseShopGame() {
  const auto groups = shopSaveGroups();
  if (groups.empty() || deviceBusy_) return;
  const auto [start, count] = groups[std::clamp(shopSaveRow_, 0, int(groups.size()) - 1)];
  shopSavePick_ = start;
  // A game not here asks for the game first; an app's saves always pick.
  const bool app = !shopSaves_[start].homebrewApp.empty();
  if (count == 1 || (!app && !shopSaveTargetHere(shopSaves_[start]))) {
    chooseShopSave();
    return;
  }
  savePicking_ = true;
  savePickRow_ = 0;
}
void Application::chooseShopSave() {
  if (shopSaves_.empty() || deviceBusy_) return;
  const auto &save = shopSaves_[std::clamp(shopSavePick_, 0, int(shopSaves_.size()) - 1)];
  // An app's save is files for its folder, put there through the patch
  // worker as from 홈브류; it asks first.
  if (!save.homebrewApp.empty()) {
    if (patchWorker_.joinable()) message(tr("패치 작업이 끝난 뒤 다시 시도합니다."));
    else shopAppSaveAsk_ = true;
    return;
  }
  const auto game = std::find_if(installed_.games.begin(), installed_.games.end(),
                                 [&](const auto &g) { return g.titleId == save.titleId; });
  if (game == installed_.games.end()) {
    // Asks first, as 유저 패치 does: the game comes from the shop when it has it.
    const auto *group = findGroup(save.titleId);
    saveGameGroup_ = group ? int(group - groups_.data()) : -1;
    saveNeedsGame_ = true;
    return;
  }
  applying_ = save;
  selected_ = int(game - installed_.games.begin());
  openManagement(); // reads the game's saves; the rows then pick the account
}
void Application::applyShopSave() {
  if (!managedGame_ || !applying_ || applySave_ < 0 || applySave_ >= int(saves_.size()) || saveWorker_.joinable())
    return;
  if (installer_.running()) {
    message(tr("설치가 끝난 뒤 세이브를 적용합니다."));
    return;
  }
  if (sdFree_ && *sdFree_ < applying_->size * 2 + (16u << 20)) {
    alertTitle_ = tr("세이브를 적용할 수 없습니다");
    alert_ = trf("SD 카드 공간이 부족합니다. {0}가 필요합니다.", {formatBytes(applying_->size * 2)});
    return;
  }
  platform::SaveJob job;
  job.save = saves_[applySave_];
  job.game = *managedGame_;
  job.sd = sdRoot_;
  job.copied = &saveCopied_;
  const auto save = *applying_;
  const auto server = session_ ? session_->server() : Server{};
  const auto state = state_;
  saveCopied_ = 0;
  saveRestoring_ = true;
  applyRunning_ = deviceBusy_ = true;
  saveWorker_ = std::thread([this, job, save, server, state] {
    if (!guarded("shop save", [&] {
          // Kept as a backup first (source "shop"), so it can be restored
          // again later, then restored like any other backup.
          auto backup = platform::importBackup(
              job,
              [&](const std::string &dir) {
                Error error;
                std::error_code ec;
                makeDirectories(std::filesystem::path(dir).string());
                net::downloadShopSave(server, state, save, dir, saveCopied_, error, &stop_);
                return error;
              },
              "shop");
          saveResult_ = backup.error ? backup.error : platform::restoreSave(job, backup.value);
        }))
      saveResult_.message = tr("세이브를 다루다 오류가 났습니다.");
    saveDone_ = true;
  });
}
void Application::applyConfirm() {
  if (!applyAsking_ || !applying_ || applySave_ < 0 || applySave_ >= int(saves_.size())) return;
  const auto &target = saves_[applySave_];
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  const auto body = modal(700, 20 + 30 + 84 + 16, tr("이 세이브를 적용합니까?"), tr("취소"), "", tr("적용합니다"), 36);
  paint_.fonts.text(applying_->name, body.x + 32, body.y + 20, 21, Ink, true, body.w - 64);
  paint_.fonts.wrapped(target.missing ? trf("{0}에 세이브를 새로 만들고 이 세이브를 넣습니다.", {target.userName})
                                      : trf("{0}의 지금 세이브를 이 세이브로 바꿉니다. 지금 세이브는 먼저 자동으로 백업해 세이브 화면에서 되돌릴 수 있습니다.",
                           {target.userName}),
                       body.x + 32, body.y + 56, 19, Secondary, body.w - 64, 3);
}
void Application::shopSavesList() {
  if (!shopSavesLoaded_ && !shopSavesWorker_.joinable() && connected_) loadShopSaves();
  needHomebrewList(); // apps' saves take the app's name and icon from it
  if (shopSaves_.empty()) {
    if (!connected_)
      notice(tr("서버에 연결되어 있지 않습니다."), tr("서버에 연결한 뒤 서버 세이브를 불러옵니다."), 21);
    else if (!shopSavesLoaded_ || shopSavesWorker_.joinable())
      for (int n = 0; n <= Visible; ++n) paint_.skeleton(260, Top + n * RowStep, 992, RowHeight, 16);
    else if (shopSavesMissing_)
      notice(tr("이 서버는 세이브를 제공하지 않습니다."), tr("서버의 saves 폴더에 세이브를 넣으면 여기에 나타납니다."), 36);
    else if (shopSavesError_)
      notice(tr("서버 세이브 목록을 불러오지 못했습니다."), shopSavesError_.message, 25);
    else
      notice(tr("받을 수 있는 서버 세이브가 없습니다."), tr("서버의 saves 폴더에 세이브를 넣으면 여기에 나타납니다."), 36);
    return;
  }
  // One row per game; its saves are picked after A.
  const auto groups = shopSaveGroups();
  const int count = int(groups.size());
  shopSaveRow_ = std::clamp(shopSaveRow_, 0, count - 1);
  const int first = std::clamp(shopSaveRow_ - Visible / 2, 0, std::max(0, count - Visible));
  paint_.scrollbar(1266, Top, RowStep * Visible - 12, float(first), Visible, float(count));
  for (int i = first; i < count && i <= first + Visible; ++i) {
    const auto [start, saves] = groups[i];
    const auto &save = shopSaves_[start];
    const float y = Top + (i - first) * RowStep;
    hit(260, y, 992, RowHeight, [this, i] {
      if (inContent() && shopSaveRow_ == i) action(Action::Accept);
      sidebar_ = false;
      shopSaveRow_ = i;
    });
    if (inContent() && i == shopSaveRow_ && !managedGame_) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    const bool app = !save.homebrewApp.empty();
    const auto game = shopSaveTitle(save);
    cover(shopSaveIcon(save), 278, y + 10, 64, false, game, app); // app icons as 홈브류 draws them
    paint_.fonts.text(game, 360, y + 12, 21, Ink, true, 640);
    // Under the name: 홈브류 for an app, how many saves, or the one save's name.
    float at = 360;
    if (app) at += paint_.pill(tr("홈브류"), at, y + 46) + 6;
    if (saves > 1) at += paint_.pill(trf("세이브 {0}개", {std::to_string(saves)}), at, y + 46) + 6;
    else paint_.fonts.text(save.name, at, y + 48, 16, Secondary, false, 640);
    // An app's save goes through the patch worker; its row shows the work.
    bool busy = false;
    for (int n = start; n < start + saves; ++n) busy |= !patchBusy_.empty() && patchBusy_ == shopSaves_[n].id;
    const bool here = shopSaveTargetHere(save);
    const std::string state = busy ? tr(patchBusyLabel_.c_str())
                              : here ? tr("적용할 수 있음")
                              : app ? tr("앱 미설치") : tr("게임 미설치");
    // The same grey label as every list's state, at the right end.
    paint_.pill(state, 1230 - paint_.fonts.measure(state, 15, true) - 18, y + 14);
  }
}
// Several saves for one game: pick one, as 유저 패치 picks a patch.
void Application::savePickDialog() {
  const auto groups = shopSaveGroups();
  if (!savePicking_ || groups.empty()) return;
  const auto [start, count] = groups[std::clamp(shopSaveRow_, 0, int(groups.size()) - 1)];
  constexpr int Shown = 5;
  constexpr float Row = 72, Gap = 8;
  const int rows = std::min(count, Shown);
  const auto &head = shopSaves_[start];
  const auto body = modal(860, 24 + rows * (Row + Gap) + 16, shopSaveTitle(head), tr("닫기"), "",
                          tr("적용하기"), 36);
  const float x = body.x + 32, w = body.w - 64;
  savePickRow_ = std::clamp(savePickRow_, 0, count - 1);
  const int top = std::clamp(savePickRow_ - Shown / 2, 0, std::max(0, count - Shown));
  for (int n = top; n < top + rows; ++n) {
    const auto &save = shopSaves_[start + n];
    const float y = body.y + 24 + (n - top) * (Row + Gap);
    hit(x, y, w, Row, [this, n] {
      savePickRow_ = n;
      action(Action::Accept);
    });
    if (n == savePickRow_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.fonts.text(save.name, x + 20, y + 10, 20, Ink, true, w - 160);
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
    if (!save.gameVersion.empty() && save.homebrewApp.empty()) meta += (meta.empty() ? "" : " · ") + trf("게임 {0}용", {save.gameVersion});
    if (!save.description.empty()) meta += (meta.empty() ? "" : " · ") + save.description;
    paint_.fonts.text(meta, x + 20, y + 40, 16, Secondary, false, w - 160);
    const auto size = formatBytes(save.size);
    paint_.fonts.middle(size, x + w - 20 - paint_.fonts.measure(size, 17, true), y + Row / 2, 17, Ink, true);
  }
}
void Application::saveNeedsGameDialog() {
  if (!saveNeedsGame_ || shopSaves_.empty()) return;
  const int pick = std::clamp(shopSavePick_, 0, int(shopSaves_.size()) - 1);
  const auto &save = shopSaves_[pick];
  const bool offered = saveGameGroup_ >= 0;
  // The game's saves on the server, so it is clear what waits for it.
  int start = pick, count = 0;
  for (const auto &[first, n] : shopSaveGroups())
    if (pick >= first && pick < first + n) start = first, count = n;
  constexpr int Shown = 4;
  constexpr float Cover = 96, Line = 30;
  const int listed = std::min(count, Shown);
  const auto body = modal(700, 24 + Cover + 20 + listed * Line + (count > Shown ? Line : 0) + 16,
                          tr("게임이 본체에 없습니다"), tr("닫기"), "",
                          offered ? tr("게임 받으러 가기") : tr("확인"), 25);
  const float x = body.x + 32, y = body.y + 24, textX = x + Cover + 24, textW = body.w - 64 - Cover - 24;
  cover(save.gameIcon, x, y, int(Cover), false, shopsaves::gameName(save, languageCode()));
  paint_.fonts.text(shopsaves::gameName(save, languageCode()), textX, y + 4, 21, Ink, true, textW);
  paint_.fonts.wrapped(offered ? tr("게임을 설치하고 한 번 실행한 뒤 세이브를 적용합니다.")
                               : tr("이 서버에는 이 게임이 없습니다. 게임을 설치하고 한 번 실행한 뒤 적용합니다."),
                       textX, y + 40, 17, Secondary, textW, 3);
  float ly = y + Cover + 20;
  for (int n = 0; n < listed; ++n, ly += Line) {
    const auto &s = shopSaves_[start + n];
    const auto size = formatBytes(s.size);
    paint_.icon(36, x, ly + 2, Secondary, 20);
    paint_.fonts.text(s.name, x + 30, ly, 18, Ink, false, body.w - 64 - 30 - 100);
    paint_.fonts.text(size, x + body.w - 64 - paint_.fonts.measure(size, 17, true), ly + 1, 17, Secondary, true);
  }
  if (count > Shown) paint_.fonts.text(trf("외 {0}개", {std::to_string(count - Shown)}), x + 30, ly, 17, Secondary);
}
} // namespace foil::ui
