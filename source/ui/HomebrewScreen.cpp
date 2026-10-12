#include "ui/Application.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "net/HomebrewApi.hpp"
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
      error.message = tr("홈브류 목록을 처리하다 오류가 났습니다.");
    homebrewFinish_ = [this, apps = std::move(apps), unchanged, error, etag]() mutable {
      homebrewLoaded_ = true;
      homebrewError_ = error;
      if (error) return;
      homebrewEtag_ = etag;
      if (!unchanged) homebrew_ = std::move(apps);
      if (screen_ == Screen::Homebrew && sidebar_ && autoSidebar_ && hasFocusable()) sidebar_ = autoSidebar_ = false;
    };
    homebrewDone_ = true;
  });
}
void Application::pollHomebrew() {
  if (!homebrewDone_.exchange(false)) return;
  homebrewWorker_.join();
  if (homebrewFinish_) std::exchange(homebrewFinish_, nullptr)();
}
void Application::checkHomebrewCard() {
  homebrewOnCard_.clear();
  for (const auto &[id, record] : homebrewLedger_) homebrewOnCard_[record.file] = fileExists(sdRoot_ + "/" + record.file);
}
homebrew::Status Application::homebrewStatus(const homebrew::App &app) const {
  return homebrew::status(app, homebrewLedger_, [this](const std::string &file) {
    const auto found = homebrewOnCard_.find(file);
    return found != homebrewOnCard_.end() && found->second;
  });
}
std::vector<int> Application::homebrewRows() const {
  std::vector<int> rows;
  for (int i = 0; i < int(homebrew_.size()); ++i) {
    const auto &app = homebrew_[i];
    const auto state = homebrewStatus(app);
    if (homebrewTab_ == 0 && state == homebrew::Status::Available) continue;
    if (homebrewTab_ == 2 && state != homebrew::Status::Update) continue;
    if (!textMatches({app.name, app.author, app.fileName}, homebrewQuery_)) continue;
    rows.push_back(i);
  }
  return rows;
}
void Application::saveHomebrewLedger() {
  std::string error;
  if (!atomicWrite(sdRoot_ + LedgerPath, homebrew::serializeLedger(homebrewLedger_), error))
    logLine("homebrew ledger: " + error);
}
// Through the retro worker, so 다운로드 shows it running and keeps how it ended.
bool Application::getHomebrew(const homebrew::App &app) {
  if (retroWorker_.joinable()) {
    message(tr("지금 받는 것이 끝난 뒤 다시 시도합니다."));
    return false;
  }
  uint64_t bytes = app.files.empty() ? app.size : 0;
  for (const auto &file : app.files) bytes += file.size;
  if (bytes && sdFree_ && *sdFree_ < bytes + (8u << 20)) {
    alertTitle_ = tr("홈브류를 받을 수 없습니다");
    alert_ = trf("SD 카드 공간이 부족합니다. {0}가 필요합니다.", {formatBytes(bytes)});
    return false;
  }
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
            message(trf("{0} 다운로드를 취소했습니다.", {app.name}));
            return;
          }
          if (!ok) {
            alertTitle_ = tr("홈브류를 받지 못했습니다");
            alert_ = error.message;
            return;
          }
          settleHomebrew(app);
          if (installShortcut(app.name, app.iconUrl, app.forwarderUrl, app.forwarderTitleId, app.forwarderSize)) return;
          message(trf("{0}을(를) 받았습니다. hbmenu에서 실행합니다.", {app.name}));
        };
      })) {
    message(tr("지금 받는 것이 끝난 뒤 다시 시도합니다."));
    return false;
  }
  startRetroEntry("homebrew", app.name, app.iconUrl, app.downloadUrl, app.size, homebrew::serialize(app));
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
  for (const auto &old : stale)
    if (old != app.installPath && fileExists(sdRoot_ + "/" + old)) {
      std::filesystem::remove(sdRoot_ + "/" + old, ec);
      removeEmptyFolders(old);
      homebrewOnCard_.erase(old);
    }
  homebrew::Record record{app.hash, app.version, app.installPath, {}};
  for (const auto &file : app.files)
    if (file.path != app.installPath) record.files.push_back(file.path);
  homebrewLedger_[app.id] = record;
  homebrewOnCard_[app.installPath] = true;
  saveHomebrewLedger();
}
// Empty folders left behind, up to (not including) switch/.
void Application::removeEmptyFolders(const std::string &path) {
  std::error_code ec;
  for (auto folder = std::filesystem::path(path).parent_path(); folder.has_parent_path() && folder != "switch";
       folder = folder.parent_path()) {
    const auto full = sdRoot_ + "/" + folder.string();
    if (!std::filesystem::is_empty(full, ec) || ec) break;
    std::filesystem::remove(full, ec);
  }
}
// The app's files (a folder install's too), the folders they leave empty,
// and its ledger entry; files the ledger does not list stay.
void Application::removeHomebrew(const homebrew::App &app) {
  const auto found = homebrewLedger_.find(app.id);
  if (found == homebrewLedger_.end()) return;
  const auto path = sdRoot_ + "/" + found->second.file;
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec && fileExists(path)) {
    alert_ = trf("파일을 지우지 못했습니다: {0}", {found->second.file});
    return;
  }
  for (const auto &file : found->second.files) {
    std::filesystem::remove(sdRoot_ + "/" + file, ec);
    removeEmptyFolders(file);
  }
  removeEmptyFolders(found->second.file);
  homebrewOnCard_.erase(found->second.file);
  homebrewLedger_.erase(found);
  saveHomebrewLedger();
  message(trf("{0}을(를) 지웠습니다.", {app.name}));
}
void Application::homebrewAction(Action a) {
  const auto rows = homebrewRows();
  const auto *picked = rows.empty() ? nullptr : &homebrew_[rows[std::clamp(selected_, 0, int(rows.size()) - 1)]];
  if (homebrewRemoveAsk_) {
    if (a == Action::Back) homebrewRemoveAsk_ = false;
    else if (a == Action::Accept && settled()) {
      homebrewRemoveAsk_ = false;
      if (picked) removeHomebrew(*picked);
    }
    return;
  }
  if (homebrewAsk_) {
    if (a == Action::Back) homebrewAsk_ = false;
    else if (a == Action::Accept && settled()) {
      homebrewAsk_ = false;
      if (picked) getHomebrew(*picked);
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
  if (a == Action::Accept && picked) {
    if (homebrewStatus(*picked) == homebrew::Status::Installed) homebrewRemoveAsk_ = true;
    else homebrewAsk_ = true;
  }
}
void Application::homebrewScreen() {
  if (!homebrewLoaded_ && !homebrewWorker_.joinable() && connected_) loadHomebrew();
  const auto rows = homebrewRows();
  int updates = 0;
  for (const auto &app : homebrew_) updates += homebrewStatus(app) == homebrew::Status::Update;
  // As on 유저 패치: search at the right end, the tabs left of it.
  searchBar(homebrewQuery_);
  // Only 업데이트 carries a count (a badge, when there is one).
  const float tabsLeft = headerTabs({tr("설치됨|탭"), tr("전체"), tr("업데이트|탭")}, homebrewTab_, [this](int n) {
    homebrewTab_ = n;
    selected_ = 0;
  }, SearchX - 16, 0, {0, 0, updates});
  pageTitle(tr("홈브류"), homebrewLoaded_ ? std::to_string(rows.size()) : std::string(), tabsLeft);
  if (!connected_) {
    notice(tr("서버에 연결되어 있지 않습니다."), tr("서버에 연결한 뒤 홈브류를 확인합니다."), 21);
    return;
  }
  if (!homebrewLoaded_) {
    paint_.skeletonCards(260, 104, 6);
    return;
  }
  if (homebrewError_) {
    notice(tr("홈브류 목록을 불러오지 못했습니다."), homebrewError_.message, 25);
    return;
  }
  if (rows.empty()) {
    if (!homebrewQuery_.empty())
      notice(trf("'{0}'에 맞는 홈브류가 없습니다.", {homebrewQuery_}), tr("B로 검색어를 지웁니다."), 23);
    else if (homebrewTab_ == 0)
      notice(tr("받은 홈브류가 없습니다."), tr("R로 전체 목록을 열어 받을 앱을 고릅니다."), 22);
    else if (homebrewTab_ == 2)
      notice(tr("모두 최신입니다."), tr("받은 홈브류에 새 버전이 없습니다."), 24);
    else
      notice(tr("서버에 홈브류가 없습니다."), tr("서버에 앱을 올리면 여기에 보입니다."), 22);
    return;
  }
  // A grid of icons like 내 게임: the state on the icon, the size under the name.
  selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
  const int first = std::max(0, selected_ / 6 - 1) * 6;
  paint_.scrollbar(1266, 104, 500, float(first / 6), 2.1f, float((rows.size() + 5) / 6));
  for (int n = 0; n < 18 && first + n < int(rows.size()); ++n) {
    const int i = first + n;
    const auto &app = homebrew_[rows[i]];
    const float x = 260 + n % 6 * 170, y = 104 + n / 6 * 240;
    hit(x, y, 150, 220, [this, i] {
      if (inContent() && selected_ == i) action(Action::Accept);
      sidebar_ = false;
      selected_ = i;
    });
    cover(app.iconUrl, x, y, 150, inContent() && selected_ == i, app.name, true);
    const auto state = homebrewStatus(app);
    const bool busy = retroBusy_ == "homebrew:" + app.id;
    if (busy || state != homebrew::Status::Available)
      paint_.pill(busy ? tr("받는 중") : state == homebrew::Status::Update ? tr("새 버전 있음") : tr("설치됨"), x + 8, y + 8);
    paint_.fonts.text(app.name, x, y + 158, 18, Ink, false, 150);
    // Size first; the version beside it only when both fit under the icon.
    float px = x;
    if (app.size) px += paint_.pill(formatBytes(app.size), px, y + 194) + 6;
    const auto version = "v" + app.version;
    if (!app.version.empty() && px + paint_.fonts.measure(version, 15, true) + 18 <= x + 150)
      paint_.pill(version, px, y + 194);
  }
}
// Before it comes down: the app's icon, name and facts, and its description
// when the server has one (without, the card stays short).
void Application::homebrewAskDialog() {
  const auto rows = homebrewRows();
  if (rows.empty()) {
    homebrewAsk_ = false;
    return;
  }
  const auto &app = homebrew_[rows[std::clamp(selected_, 0, int(rows.size()) - 1)]];
  const bool update = homebrewStatus(app) == homebrew::Status::Update;
  constexpr float Icon = 160, Width = 820;
  const float textW = Width - 64 - Icon - 28;
  // Measured off screen first, so the card hugs the description.
  constexpr int Lines = 6;
  const float described = app.description.empty() ? 0
                        : paint_.fonts.wrapped(app.description, -9999, -9999, 18, Ink, textW, Lines);
  const float bodyH = 24 + std::max(Icon, 106 + described) + 24;
  const auto body = modal(Width, bodyH, update ? tr("새 버전을 받을까요?") : tr("이 앱을 받을까요?"), tr("취소"), "",
                          update ? tr("업데이트|동작") : tr("받기"), 22);
  const float x = body.x + 32, y = body.y + 24, textX = x + Icon + 28;
  cover(app.iconUrl, x, y, int(Icon), false, app.name, true);
  paint_.fonts.text(app.name, textX, y, 24, Ink, true, textW);
  std::string facts = app.version.empty() ? "" : "v" + app.version;
  if (!app.author.empty()) facts += (facts.empty() ? "" : " · ") + app.author;
  if (app.size) facts += (facts.empty() ? "" : " · ") + formatBytes(app.size);
  paint_.fonts.text(facts, textX, y + 38, 18, Ink, true, textW);
  std::string where = app.installPath;
  if (app.releasedAt.size() >= 10) where = app.releasedAt.substr(0, 10) + " · " + where;
  paint_.fonts.text(where, textX, y + 68, 16, Secondary, false, textW);
  if (described) paint_.fonts.wrapped(app.description, textX, y + 106, 18, Ink, textW, Lines);
}
void Application::homebrewRemoveDialog() {
  const auto rows = homebrewRows();
  if (rows.empty()) {
    homebrewRemoveAsk_ = false;
    return;
  }
  const auto &app = homebrew_[rows[std::clamp(selected_, 0, int(rows.size()) - 1)]];
  const auto body = modal(700, 24 + 70 + 16, tr("홈브류를 지우시겠습니까?"), tr("취소"), "", tr("지우기"), 37);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(app.name, x, y, 21, Ink, true, body.w - 64);
  paint_.fonts.text(trf("SD 카드에서 {0}을(를) 지웁니다.", {app.installPath}), x, y + 38, 19, Secondary,
                    false, body.w - 64);
}
} // namespace foil::ui
