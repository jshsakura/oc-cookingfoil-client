#include "ui/Application.hpp"
#include "core/Log.hpp"
#include "net/RetroApi.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <set>
#include <sys/stat.h>
namespace foil::ui {
namespace {
constexpr float RowHeight = 84, RowStep = 96;
const std::string PackRecordPath = "/switch/cookingfoil/retro-pack.json";
// The retro list's filters; several combine (the strictest of each kind wins).
struct RetroCondition {
  const char *title;
  int minRating;
  bool bezel;
  uint64_t minSize, maxSize;
};
constexpr uint64_t MB = 1024 * 1024;
// Titles go through tr() where the popup lists them.
const RetroCondition RetroConditions[] = {
    {"전체", 0, false, 0, 0},             // i18n: keep
    {"평점 80 이상", 80, false, 0, 0},     // i18n: keep
    {"평점 90 이상", 90, false, 0, 0},     // i18n: keep
    {"전용 베젤", 0, true, 0, 0},          // i18n: keep
    {"1MB 이하", 0, false, 0, MB},         // i18n: keep
    {"10MB 이하", 0, false, 0, 10 * MB},   // i18n: keep
    {"100MB 이상", 0, false, 100 * MB, 0}, // i18n: keep
};
constexpr SortMode RetroSorts[] = {SortMode::Score, SortMode::Name, SortMode::Largest, SortMode::Recent};
bool fileExists(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}
uint64_t fileSize(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
}
} // namespace
bool Application::retroArchPresent() const { return fileExists(sdRoot_ + "/" + retro::RetroArchApp); }
bool Application::retroInstalled(const retro::Game &game) const {
  if (game.romPath.empty()) return false;
  const auto found = retroOnCard_.find(game.romPath);
  return found != retroOnCard_.end() && found->second;
}
// The game's core is not on the card while RetroArch is.
bool Application::retroCoreMissing(const retro::Game &game) const {
  const auto core = retro::corePath(game.core);
  return !core.empty() && retroArchPresent() && !fileExists(sdRoot_ + "/" + core);
}
bool Application::retroBezelMissing(const retro::Game &game) const {
  if (!game.bezel || game.bezelPath.empty() || !retroInstalled(game)) return false;
  const auto found = retroOnCard_.find(game.bezelPath);
  return found != retroOnCard_.end() && !found->second;
}
bool Application::runRetroJob(const std::string &busy, std::function<std::function<void()>()> job) {
  if (retroWorker_.joinable() || !session_) return false;
  retroBusy_ = busy;
  retroReceived_ = 0;
  retroWorker_ = std::thread([this, job = std::move(job)] {
    std::function<void()> finish;
    if (!guarded("retro", [&] { finish = job(); }))
      finish = [this] { message(tr("레트로 작업 중 오류가 났습니다. log.txt를 확인합니다.")); };
    retroFinish_ = std::move(finish);
    retroDone_ = true;
  });
  return true;
}
void Application::pollRetro() {
  if (!retroDone_.exchange(false)) return;
  retroWorker_.join();
  retroBusy_.clear();
  if (retroFinish_) std::exchange(retroFinish_, nullptr)();
  // L R pressed while a list was loading: that result was for the system
  // left behind, so the one now shown loads here.
  if (retroView_ == RetroView::List && !retroWorker_.joinable() && !retroSystems_.empty() && !retroError_ &&
      retroListSystem_ != retroSystems_[retroSystem_].id)
    loadRetroPage(false);
  if (screen_ == Screen::Retro && !hasFocusable() && !retroWorker_.joinable()) sidebar_ = true;
}
void Application::loadRetroHome() {
  if (!session_ || !connected_) return;
  const auto server = session_->server();
  const auto state = state_;
  const auto sd = sdRoot_;
  runRetroJob("list", [this, server, state, sd]() -> std::function<void()> {
    net::RetroReply reply;
    auto shelves = net::retroHome(server, state, reply, &stop_).value;
    // An older server has no home: its systems, and the list straight away.
    std::vector<retro::System> systems;
    const bool noHome = reply.missing;
    if (noHome) {
      reply = {};
      systems = net::retroSystems(server, state, reply, &stop_).value;
      systems.erase(std::remove_if(systems.begin(), systems.end(), [](const auto &s) { return s.romCount <= 0; }),
                    systems.end());
    } else
      for (const auto &shelf : shelves) systems.push_back(shelf.system);
    std::map<std::string, bool> onCard;
    for (const auto &shelf : shelves)
      for (const auto &g : shelf.games)
        for (const auto *path : {&g.romPath, &g.bezelPath})
          if (!path->empty()) onCard[*path] = fileExists(sd + "/" + *path);
    return [this, shelves = std::move(shelves), systems = std::move(systems), reply, noHome,
            onCard = std::move(onCard)]() mutable {
      retroLoaded_ = true;
      retroMissing_ = reply.missing;
      retroError_ = reply.error;
      // A busy server's "ask again later", three times at most.
      retroRetries_ = reply.retryAfter ? retroRetries_ + 1 : 0;
      retroRetryAt_ = reply.retryAfter && retroRetries_ <= 3 ? SDL_GetTicks() + reply.retryAfter * 1000u : 0;
      retroHomeMissing_ = noHome;
      retroShelves_ = std::move(shelves);
      for (auto &[path, there] : onCard) retroOnCard_[path] = there;
      // The list keeps its system across a reload when that system is still there.
      const auto current = retroSystem_ < int(retroSystems_.size()) ? retroSystems_[retroSystem_].id : "";
      retroSystems_ = std::move(systems);
      const auto kept = std::find_if(retroSystems_.begin(), retroSystems_.end(),
                                     [&](const auto &s) { return s.id == current; });
      retroSystem_ = kept != retroSystems_.end() ? int(kept - retroSystems_.begin()) : 0;
      retroRow_ = std::clamp(retroRow_, -1, std::max(0, int(retroShelves_.size()) - 1));
      if (noHome) retroView_ = RetroView::List;
      if (retroView_ == RetroView::List) {
        retroGames_.clear();
        retroListSystem_.clear();
        retroPage_ = retroTotal_ = 0;
        if (!retroSystems_.empty()) loadRetroPage(false);
      }
    };
  });
}
void Application::openRetroList(int system) {
  if (system < 0 || system >= int(retroSystems_.size())) return;
  retroView_ = RetroView::List;
  retroSystem_ = system;
  retroQuery_.clear();
  retroGames_.clear();
  retroListSystem_.clear();
  retroTotal_ = 0;
  selected_ = 0;
  sidebar_ = false;
  if (!retroWorker_.joinable()) loadRetroPage(false);
}
void Application::loadRetroPage(bool next) {
  if (!session_ || retroSystems_.empty()) return;
  if (next && int(retroGames_.size()) >= retroTotal_) return;
  const auto server = session_->server();
  const auto state = state_;
  const auto system = retroSystems_[retroSystem_].id;
  const int page = next ? retroPage_ + 1 : 1;
  const auto sd = sdRoot_;
  const auto filter = retroFilter();
  runRetroJob("list", [this, server, state, system, page, sd, filter]() -> std::function<void()> {
    net::RetroReply reply;
    auto result = net::retroGames(server, state, system, page, filter, reply, &stop_);
    // Which of these are already on the card, looked up here, off the UI thread.
    std::map<std::string, bool> onCard;
    for (const auto &g : result.value.games)
      for (const auto *path : {&g.romPath, &g.bezelPath})
        if (!path->empty()) onCard[*path] = fileExists(sd + "/" + *path);
    return [this, result = std::move(result.value), reply, system, page, filter, onCard = std::move(onCard)]() mutable {
      // Switched system, search or filter meanwhile: pollRetro asks for the right one.
      if (retroSystems_.empty() || retroSystems_[retroSystem_].id != system || retroFilter() != filter) {
        retroListSystem_.clear();
        return;
      }
      retroError_ = reply.error;
      if (reply.error) return;
      if (page == 1) retroGames_.clear();
      retroListSystem_ = system;
      retroPage_ = page;
      retroTotal_ = result.total;
      for (auto &g : result.games) retroGames_.push_back(std::move(g));
      for (auto &[path, there] : onCard) retroOnCard_[path] = there;
    };
  });
}
retro::Filter Application::retroFilter() const {
  retro::Filter f;
  f.query = retroQuery_;
  f.sort = retroSort_ == SortMode::Name      ? "name"
           : retroSort_ == SortMode::Largest ? "size"
           : retroSort_ == SortMode::Recent  ? "added"
                                             : "rating";
  for (const int n : retroFilters_) {
    if (n <= 0 || n >= int(std::size(RetroConditions))) continue;
    const auto &c = RetroConditions[n];
    f.minRating = std::max(f.minRating, c.minRating);
    f.bezel = f.bezel || c.bezel;
    f.minSize = std::max(f.minSize, c.minSize);
    if (c.maxSize) f.maxSize = f.maxSize ? std::min(f.maxSize, c.maxSize) : c.maxSize;
  }
  return f;
}
std::vector<std::string> Application::popupFilterLabels() const {
  std::vector<std::string> out;
  if (screen_ == Screen::Retro)
    for (const auto &c : RetroConditions) out.push_back(tr(c.title));
  else
    for (const auto &chip : chips_) out.push_back(chip.title);
  return out;
}
std::vector<SortMode> Application::popupSorts() const {
  if (screen_ == Screen::Retro) return {std::begin(RetroSorts), std::end(RetroSorts)};
  return {std::begin(SortOrders), std::end(SortOrders)};
}
const retro::Game *Application::retroFocused() const {
  if (!inContent()) return nullptr;
  if (retroView_ == RetroView::Home) {
    if (retroRow_ < 0 || retroRow_ >= int(retroShelves_.size())) return nullptr;
    const auto &games = retroShelves_[retroRow_].games;
    return retroCol_ < int(games.size()) ? &games[retroCol_] : nullptr;
  }
  return retroGames_.empty() ? nullptr : &retroGames_[std::clamp(selected_, 0, int(retroGames_.size()) - 1)];
}
// A on a game: says so when it is already there, otherwise asks first.
void Application::askRetroGame(const retro::Game &game) {
  retroPick_ = game;
  retroAsking_ = true;
}
bool Application::getRetroGame(const retro::Game &game, bool again) {
  if (game.bundleUrl.empty()) return false;
  if (retroWorker_.joinable() && retroBusy_ != "list") {
    message(tr("레트로 작업이 끝난 뒤 다시 시도합니다."));
    return false;
  }
  if (!again && retroInstalled(game) && !retroBezelMissing(game)) {
    message(retroArchPresent() ? tr("이미 받은 게임입니다. RetroArch에서 실행합니다.")
                               : tr("이미 받은 게임입니다. Y로 RetroArch를 받으면 실행할 수 있습니다."));
    return false;
  }
  if (sdFree_ && *sdFree_ < game.size + game.size / 10 + (8u << 20)) {
    alertTitle_ = tr("게임을 받을 수 없습니다");
    alert_ = trf("SD 카드 공간이 부족합니다. {0}가 필요합니다.", {formatBytes(game.size)});
    return false;
  }
  const auto server = session_ ? session_->server() : Server{};
  const auto state = state_;
  const auto sd = sdRoot_;
  retroBytes_ = game.size;
  if (!retroWorker_.joinable()) retroCancel_ = false;
  if (!runRetroJob(game.id, [this, server, state, sd, game]() -> std::function<void()> {
        Error error;
        const bool ok = net::retroBundle(server, state, game, sd, retroReceived_, error, &retroCancel_);
        // A ROM cut off part way would read as there; it goes, and so does
        // its bezel, so the card never lists half a game.
        if (!ok) {
          std::error_code ec;
          for (const auto *path : {&game.romPath, &game.bezelPath})
            if (!path->empty()) std::filesystem::remove(sd + "/" + *path, ec);
        }
        const bool there = !game.romPath.empty() && fileExists(sd + "/" + game.romPath);
        const bool bezel = !game.bezelPath.empty() && fileExists(sd + "/" + game.bezelPath);
        const bool cancelled = retroCancel_;
        const uint64_t received = retroReceived_;
        return [this, ok, error, game, there, bezel, cancelled, received] {
          if (!game.romPath.empty()) retroOnCard_[game.romPath] = there;
          if (!game.bezelPath.empty()) retroOnCard_[game.bezelPath] = bezel;
          storageAt_ = 0;
          finishRetroEntry("retro", ok ? history::Outcome::Done : cancelled ? history::Outcome::Cancelled : history::Outcome::Failed,
                           ok || cancelled ? "" : error.message, ok ? game.size : received);
          if (cancelled && !ok) {
            message(trf("{0} 다운로드를 취소했습니다.", {game.name}));
            return;
          }
          if (!ok) {
            alertTitle_ = tr("게임을 받지 못했습니다");
            alert_ = error.message;
            return;
          }
          // Its core missing: the RetroArch files come next, only what is
          // missing or changed.
          if (retroCoreMissing(game)) syncRetroArch();
          if (installRetroShortcut(game)) return; // 다운로드 shows the shortcut going in
          message(retroArchPresent() ? trf("{0}을(를) 받았습니다. RetroArch에서 실행합니다.", {game.name})
                                     : trf("{0}을(를) 받았습니다. Y로 RetroArch도 받습니다.", {game.name}));
        };
      })) {
    message(tr("레트로 작업이 끝난 뒤 다시 시도합니다."));
    return false;
  }
  startRetroEntry("retro", game.name, game.iconUrl, game.bundleUrl, game.size, retro::serializeGame(game));
  return true;
}
// 다운로드 lists retro downloads beside installs: one entry while it runs,
// ended by finishRetroEntry.
void Application::startRetroEntry(const std::string &kind, const std::string &name, const std::string &icon,
                                  const std::string &url, uint64_t size, const std::string &payload) {
  Item file;
  file.url = url;
  file.name = name;
  file.icon = icon;
  file.size = size;
  history::Entry entry;
  entry.install = {name, icon, "sd", {file}};
  entry.kind = kind;
  entry.payload = payload;
  entry.at = int64_t(std::time(nullptr));
  history::add(history_, std::move(entry));
  saveHistory();
}
void Application::finishRetroEntry(const std::string &kind, history::Outcome outcome, const std::string &error,
                                   uint64_t bytes) {
  const auto running = std::find_if(history_.begin(), history_.end(), [&](const history::Entry &e) {
    return e.kind == kind && e.outcome == history::Outcome::Running;
  });
  if (running == history_.end()) return;
  // RetroArch already up to date fetched nothing: no entry for it.
  if (outcome == history::Outcome::Done && bytes == 0 && kind == "retroarch") history_.erase(running);
  else {
    if (bytes && !running->install.files.empty()) running->install.files.front().size = bytes;
    history::finish(history_, outcome, error, int64_t(std::time(nullptr)), kind);
  }
  saveHistory();
}
// The game's HOME Menu shortcut goes in through the installer like any
// other title; false when the server has none or another install runs.
bool Application::installRetroShortcut(const retro::Game &game) {
  return installShortcut(game.name, game.iconUrl, game.forwarderUrl, game.forwarderTitleId, game.forwarderSize);
}
// A HOME Menu shortcut goes in through the installer like any other title;
// false when there is none to make, 설정 turned them off, or the server has
// none for it.
bool Application::installShortcut(const std::string &name, const std::string &icon, const std::string &url,
                                  const std::string &titleId, uint64_t size, bool justDownloaded) {
  if (url.empty() || !session_ || !state_.makeShortcuts) return false;
  if (installer_.running()) {
    if (!justDownloaded) return false;
    message(trf("{0}을(를) 받았습니다. 지금 설치가 끝난 뒤 다시 받으면 홈 메뉴 바로가기를 만듭니다.", {name}));
    return true;
  }
  Item shortcut;
  shortcut.url = url;
  shortcut.name = name;
  shortcut.titleId = shortcut.baseId = titleId;
  shortcut.size = size;
  shortcut.icon = icon;
  if (auto error = startInstall({shortcut}, trf("{0} 바로가기", {name}), icon, "sd")) {
    alertTitle_ = tr("바로가기를 만들지 못했습니다");
    alert_ = error.message;
    return true;
  }
  message(justDownloaded ? trf("{0}을(를) 받았습니다. 홈 메뉴 바로가기를 설치합니다.", {name})
                         : trf("홈 메뉴에 {0} 바로가기를 설치합니다.", {name}));
  return true;
}
bool Application::syncRetroArch(bool full) {
  const auto server = session_ ? session_->server() : Server{};
  const auto state = state_;
  const auto sd = sdRoot_;
  retroBytes_ = 0;
  if (!retroWorker_.joinable()) retroCancel_ = false;
  if (!runRetroJob("pack", [this, server, state, sd, full]() -> std::function<void()> {
        net::RetroReply reply;
        auto pack = net::retroPack(server, state, reply, &stop_);
        if (reply.error) return [this, reply] { message(reply.error.message); };
        auto record = full ? retro::PackRecord{} : retro::parsePackRecord(readFile(sd + PackRecordPath));
        const auto plan = retro::planPack(pack.value, record, fileExists, fileSize, sd);
        retroBytes_ = plan.bytes;
        Error error;
        int done = 0;
        std::string ignored;
        // Many files: the whole pack as one tar, writing only what the plan
        // wants (changed, missing; a settings file already there stays).
        constexpr size_t BundleFrom = 50;
        if (!pack.value.bundleUrl.empty() && plan.fetch.size() >= BundleFrom) {
          std::set<std::string> wanted;
          for (const auto &file : plan.fetch) wanted.insert(file.path);
          retroBytes_ = pack.value.bundleSize;
          if (net::retroPackBundle(server, state, pack.value, sd,
                                   [&](const std::string &path) { return !wanted.count(path); }, retroReceived_, error,
                                   &retroCancel_)) {
            for (const auto &file : plan.fetch) record.files[file.path] = file.sha256;
            done = int(plan.fetch.size());
          }
        }
        for (const auto &file : plan.fetch) {
          if (retroCancel_ || done == int(plan.fetch.size()) || error) break;
          // A busy server answers 429: wait a little longer each time and
          // try the same file again before giving up.
          bool ok = false;
          for (int attempt = 0; attempt < 5 && !retroCancel_; ++attempt) {
            long status = 0;
            error = {};
            if ((ok = net::retroFile(server, state, file, sd, retroReceived_, error, &retroCancel_, &status))) break;
            if (status != 429) break;
            for (int waited = 0; waited < (2 << attempt) * 10 && !retroCancel_; ++waited) SDL_Delay(100);
          }
          if (!ok) break;
          record.files[file.path] = file.sha256;
          // Written as it goes, so a sync cut off (power, a crash) resumes
          // from here instead of from the start.
          if (++done % 100 == 0) atomicWrite(sd + PackRecordPath, retro::serializePackRecord(record), ignored);
        }
        // Whatever arrived is recorded, so a retry fetches only the rest.
        record.version = error ? record.version : pack.value.version;
        atomicWrite(sd + PackRecordPath, retro::serializePackRecord(record), ignored);
        const int total = int(plan.fetch.size());
        const bool cancelled = retroCancel_;
        const uint64_t received = retroReceived_;
        return [this, error, done, total, cancelled, received] {
          storageAt_ = 0;
          finishRetroEntry("retroarch",
                           cancelled ? history::Outcome::Cancelled : error ? history::Outcome::Failed : history::Outcome::Done,
                           cancelled ? "" : error.message, received);
          if (cancelled)
            message(trf("RetroArch 설치를 취소했습니다. {0}/{1}개를 받았고, 다시 설치하면 이어서 받습니다.",
                        {std::to_string(done), std::to_string(total)}));
          else if (error) {
            alertTitle_ = tr("RetroArch를 받지 못했습니다");
            alert_ = trf("{0}/{1}개를 받았습니다. 다시 받으면 이어서 받습니다. {2}",
                         {std::to_string(done), std::to_string(total), error.message});
          } else
            message(total ? trf("RetroArch 파일 {0}개를 받았습니다.", {std::to_string(total)})
                          : std::string(tr("RetroArch가 최신입니다.")));
        };
      })) {
    message(tr("레트로 작업이 끝난 뒤 다시 시도합니다."));
    return false;
  }
  startRetroEntry("retroarch", "RetroArch", "", "retroarch", 0, "");
  return true;
}
void Application::retroAction(Action a) {
  if (retroArchAsking_) {
    if (a == Action::Back) retroArchAsking_ = false;
    if (a == Action::Accept || a == Action::Queue) {
      retroArchAsking_ = false;
      syncRetroArch(a == Action::Queue);
    }
    return;
  }
  if (retroAsking_) {
    if (a == Action::Back) retroAsking_ = false;
    if (a == Action::Accept) {
      retroAsking_ = false;
      getRetroGame(retroPick_, true);
    }
    return;
  }
  if (a == Action::Queue && retroView_ == RetroView::List) {
    openViewPopup(); // Y in a system's list: filters and order, as in 모든 게임
    return;
  }
  if (a == Action::Queue) {
    // Y while it downloads stops it (what came so far is kept); already
    // there, Y asks before fetching it again.
    if (retroBusy_ == "pack") retroCancel_ = true;
    else if (retroArchPresent()) retroArchAsking_ = true;
    else syncRetroArch();
    return;
  }
  if (retroView_ == RetroView::Home) {
    retroHomeAction(a);
    return;
  }
  if (a == Action::Back) {
    // B drops a search first, then goes back to the shelves.
    if (!retroQuery_.empty()) {
      retroQuery_.clear();
      retroGames_.clear();
      retroListSystem_.clear();
      selected_ = 0;
      if (!retroWorker_.joinable()) loadRetroPage(false);
    } else if (retroHomeMissing_)
      sidebar_ = true;
    else
      retroView_ = RetroView::Home;
    return;
  }
  if (a == Action::Search) {
    requestSearch();
    return;
  }
  if ((a == Action::Previous || a == Action::Next) && !retroSystems_.empty()) {
    if (retroWorker_.joinable() && retroBusy_ != "list") {
      message(tr("레트로 작업이 끝난 뒤 다시 시도합니다."));
      return;
    }
    const int count = int(retroSystems_.size());
    retroSystem_ = (retroSystem_ + (a == Action::Next ? 1 : count - 1)) % count;
    retroGames_.clear();
    retroListSystem_.clear();
    retroTotal_ = 0;
    selected_ = 0;
    if (!retroWorker_.joinable()) loadRetroPage(false);
    return;
  }
  if (a == Action::Left) {
    sidebar_ = true;
    return;
  }
  if (retroGames_.empty()) return;
  if (a == Action::Up) selected_ = std::max(0, selected_ - 1);
  if (a == Action::Down) selected_ = std::min(int(retroGames_.size()) - 1, selected_ + 1);
  // The next page arrives before the cursor reaches the end.
  if (selected_ >= int(retroGames_.size()) - 8 && !retroWorker_.joinable()) loadRetroPage(true);
  if (a == Action::Accept)
    if (const auto *game = retroFocused()) askRetroGame(*game);
}
// What a game is: its file format, a bezel, its score; for the list's
// labels and the info popup.
std::vector<std::string> Application::retroLabels(const retro::Game &game, bool detail) const {
  std::vector<std::string> out;
  if (const auto dot = game.file.rfind('.'); dot != std::string::npos) {
    auto format = game.file.substr(dot + 1);
    std::transform(format.begin(), format.end(), format.begin(), ::toupper);
    out.push_back(format);
  }
  if (game.bezel) out.push_back(tr("베젤 포함"));
  // What kind of release it is, in one word; the popup adds where it came
  // from and its version.
  const auto &edition = game.edition;
  if (edition.source == "official") out.push_back(tr("정발"));
  else if (edition.source == "patch") out.push_back(tr("한글"));
  if (detail && !edition.from.empty()) {
    const auto from = edition.from == "jp" ? tr("일본판") : edition.from == "us" ? tr("북미판")
                    : edition.from == "eu" ? tr("유럽판") : "";
    if (*from) out.push_back(from);
  }
  if (detail && !edition.version.empty()) out.push_back(edition.version);
  // Versions of one game share a name; the tag tells them apart. With a
  // region or an edition, the flag and the badge do that instead.
  if (!game.variant.empty() && game.region.empty() && edition.source.empty()) {
    auto tag = game.variant;
    if (tag.size() > 28) {
      size_t cut = 26;
      while (cut > 0 && (static_cast<unsigned char>(tag[cut]) & 0xC0) == 0x80) --cut; // not mid-character
      tag = tag.substr(0, cut) + "…";
    }
    out.push_back(tag);
  }
  if (game.rating > 0) out.push_back(trf("평점 {0}점", {std::to_string(game.rating)}));
  return out;
}
// A on a game: what it is, and one button that does what it needs (get
// it, add its bezel), or says it is already there. B closes.
void Application::retroConfirm() {
  if (!retroAsking_) return;
  const auto &game = retroPick_;
  const bool bezel = retroBezelMissing(game), there = retroInstalled(game) && !bezel;
  const auto system = std::find_if(retroSystems_.begin(), retroSystems_.end(),
                                   [&](const auto &s) { return s.id == game.system; });
  const auto title = system != retroSystems_.end() ? retro::systemName(*system, languageCode()) : std::string(tr("레트로 게임"));
  // One decision, so the footer says it: A gets it, B backs out.
  const auto get = there ? tr("다시 받기") : bezel ? tr("베젤 받기") : tr("다운로드|받기");
  constexpr float Cover = 200;
  const auto body = modal(780, 24 + Cover + 24, title, tr("취소"), "", get, 35);
  const float x = body.x + 32, y = body.y + 24, textX = x + Cover + 28, textW = body.w - 64 - Cover - 28;
  cover(game.iconUrl, x, y, int(Cover), false, game.name, true);
  paint_.region(game.region, x + Cover - 44, y + Cover - 32, 36, 24);
  float at = y + paint_.fonts.wrapped(game.name, textX, y, 22, Ink, textW, 3, true) + 6;
  if (!game.original.empty() && game.original != game.name)
    at += paint_.fonts.wrapped(game.original, textX, at, 17, Secondary, textW, 2) + 4;
  float px = textX;
  for (const auto &label : retroLabels(game, true)) px += paint_.pill(label, px, at + 6) + 6;
  at += 44;
  std::string facts = formatBytes(game.size);
  if (game.ratingCount > 0) facts += " · " + trf("{0}명 평가", {std::to_string(game.ratingCount)});
  paint_.fonts.text(facts, textX, at, 18, Ink, true, textW);
  if (!game.romPath.empty())
    paint_.fonts.text(game.romPath, textX, at + 30, 16, Secondary, false, textW);
  if (retroCoreMissing(game))
    paint_.fonts.wrapped(trf("{0} 코어가 없어 게임을 받은 뒤 함께 받습니다.", {game.core}), textX, at + 58, 16, Secondary, textW, 2);
}
void Application::retroArchConfirm() {
  const auto body = modal(700, 24 + 84 + 24, tr("RetroArch를 다시 받을까요?"), tr("취소"), tr("전체 다시 받기"), tr("다시 받기"), 3);
  paint_.fonts.wrapped(tr("이미 SD 카드에 있습니다. 바뀐 파일과 빠진 코어만 다시 받습니다. 전체 다시 받기는 모든 파일을 새로 받으며, 두 경우 모두 설정은 그대로 둡니다."), body.x + 32,
                       body.y + 24, 19, Secondary, body.w - 64, 3);
}
void Application::retroScreen() {
  if (retroRetryAt_ && SDL_GetTicks() >= retroRetryAt_) {
    retroRetryAt_ = 0;
    retroLoaded_ = false;
  }
  if (!retroLoaded_ && connected_ && !retroWorker_.joinable()) loadRetroHome();
  if (retroView_ == RetroView::Home) retroHome();
  else retroList();
}
// RetroArch's state in a strip: there or not (Y gets it), and that each
// game also gets a HOME Menu shortcut; or how far its download has come.
void Application::retroBanner(float y) {
  constexpr float Height = 64;
  paint_.rect(260, y, 992, Height, Surface, 16);
  if (retroBusy_ == "pack") {
    const float ratio = retroBytes_ ? std::min(1.f, float(retroReceived_.load()) / float(retroBytes_)) : 0.f;
    paint_.icon(4, 282, y + 19, Ink, 26);
    paint_.fonts.middle(trf("RetroArch를 설치하고 있습니다 · {0} / {1}", {formatBytes(retroReceived_.load()), formatBytes(retroBytes_)}),
                        322, y + Height / 2, 19, Ink, true);
    paint_.rect(910, y + 28, 316, 8, Line, 4);
    paint_.rect(910, y + 28, std::max(8.f, 316 * ratio), 8, Butter, 4);
    return;
  }
  hit(260, y, 992, Height, [this] { action(Action::Queue); });
  const bool ready = retroArchPresent();
  paint_.icon(ready ? 24 : 25, 282, y + 19, ready ? Good : Pending, 26);
  paint_.fonts.middle(ready ? tr("RetroArch가 설치되어 있습니다. 게임을 받으면 홈 메뉴 바로가기도 함께 만듭니다.")
                            : tr("RetroArch가 설치되어 있지 않습니다. Y로 먼저 설치하면 게임마다 홈 메뉴 바로가기를 함께 만듭니다."),
                      322, y + Height / 2, 19, Ink, true, 900);
}
void Application::retroList() {
  const auto *system = retroSystems_.empty() ? nullptr : &retroSystems_[retroSystem_];
  // Right: the same filter button and search box as 모든 게임.
  searchBar(retroQuery_);
  std::string summary = retroFilters_.empty() ? std::string(tr("전체")) : popupFilterLabels()[retroFilters_.front()];
  if (retroFilters_.size() > 1) summary = trf("{0} 외 {1}", {summary, std::to_string(retroFilters_.size() - 1)});
  summary += " · " + std::string(sortLabel(retroSort_));
  const float viewW = std::min(300.f, paint_.keyLabelWidth("Y", summary, 18, true) + 66), viewX = SearchX - 16 - viewW;
  headerItem("filter", viewX, 24, viewW, 48, [this] { openViewPopup(); });
  paint_.rect(viewX, 24, viewW, 48, Surface, 12);
  paint_.key("Y", viewX + 18, 48);
  paint_.icon(10, viewX + 60, 38, Ink, 20);
  paint_.fonts.middle(summary, viewX + 88, 48, 18, Ink, true, viewW - 106);
  // Left: the system between L and R, which step to the one before or after:
  // its icon on a white tile (some are dark line art), name and game count.
  constexpr float Mid = 48;
  float x = 260;
  if (system) {
    headerItem("previous", 256, 26, paint_.keyWidth("L") + 8, 44, [this] { action(Action::Previous); });
    paint_.key("L", x, Mid);
    x += paint_.keyWidth("L") + 12;
    if (!system->iconUrl.empty()) {
      paint_.rect(x, Mid - 20, 40, 40, {255, 255, 255, 255}, 10);
      images_->draw(system->iconUrl, x + 5, Mid - 15, 30, 30, 4);
      x += 50;
    }
    const float room = viewX - 16 - x - paint_.keyWidth("R") - 70;
    x = paint_.fonts.middle(retro::systemName(*system, languageCode()), x, Mid, 26, Ink, true, room);
    x += 10 + paint_.pill(std::to_string(retroTotal_), x + 10, Mid - 13);
    headerItem("next", x + 8, 26, paint_.keyWidth("R") + 8, 44, [this] { action(Action::Next); });
    paint_.key("R", x + 12, Mid);
  } else
    pageTitle(tr("레트로 게임"), "", 0);
  // Here Y opens the filters, so the strip shows only a RetroArch install
  // that is running (the home is where it starts).
  const bool banner = retroBusy_ == "pack";
  if (banner) retroBanner(90);
  const float Top = banner ? 170 : 90;
  const int Visible = banner ? 4 : 5;
  if (retroGames_.empty()) {
    if (!connected_)
      notice(tr("서버에 연결되어 있지 않습니다."), tr("서버에 연결한 뒤 레트로 게임을 불러옵니다."), 21);
    else if (retroMissing_)
      notice(tr("이 서버는 레트로 게임을 제공하지 않습니다."), tr("서버에 레트로 폴더를 만들면 여기에 나타납니다."), 35);
    else if (retroError_ && retroRetryAt_)
      notice(tr("서버가 레트로 파일을 읽고 있습니다."), tr("잠시 뒤 자동으로 다시 불러옵니다."), 35);
    else if (retroError_)
      notice(tr("레트로 게임 목록을 불러오지 못했습니다."), retroError_.message, 25);
    else if (!retroBusy_.empty() || !retroLoaded_ || retroListSystem_.empty())
      for (int n = 0; n <= Visible; ++n) paint_.skeleton(260, Top + n * RowStep, 992, RowHeight, 16);
    else if (!retroQuery_.empty())
      notice(tr("검색 결과가 없습니다."), tr("X로 다른 이름을 찾거나 B로 검색을 지웁니다."), 1);
    else
      notice(tr("레트로 게임이 없습니다."), tr("서버의 레트로 폴더에 롬을 넣으면 여기에 나타납니다."), 35);
    return;
  }
  const int count = int(retroGames_.size());
  selected_ = std::clamp(selected_, 0, count - 1);
  // The focus rides the middle row once past it, the list scrolling under it.
  const int first = std::clamp(selected_ - Visible / 2, 0, std::max(0, count - Visible));
  paint_.scrollbar(1266, Top, RowStep * Visible - 12, float(first), Visible, float(std::max(retroTotal_, count)));
  // One row past the footer so a long list looks like it goes on.
  for (int i = first; i < count && i <= first + Visible; ++i) {
    const auto &game = retroGames_[i];
    const float y = Top + (i - first) * RowStep;
    hit(260, y, 992, RowHeight, [this, i] {
      if (inContent() && selected_ == i) action(Action::Accept);
      sidebar_ = false;
      selected_ = i;
    });
    if (inContent() && i == selected_) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    cover(thumbnailUrl(game.iconUrl), 278, y + 10, 64, false, game.name, true);
    paint_.region(game.region, 278 + 64 - 23, y + 10 + 64 - 16, 20, 13);
    if (inContent() && i == selected_) marquee(game.name, 360, y + 12, 21, 620);
    else paint_.fonts.text(game.name, 360, y + 12, 21, Ink, true, 620);
    // Under the name, labels: its format, a bezel, its score.
    float at = 360;
    for (const auto &label : retroLabels(game)) at += paint_.pill(label, at, y + 46) + 6;
    const float right = 1230;
    const bool busy = retroBusy_ == game.id;
    const bool bezel = retroBezelMissing(game), there = retroInstalled(game) && !bezel;
    const std::string state = busy ? tr("받는 중") : bezel ? tr("베젤 받기") : there ? tr("설치됨") : tr("설치 안 됨");
    const float w = paint_.fonts.measure(state, 16, true) + 20;
    paint_.rect(right - w, y + 12, w, 28, busy || bezel ? Butter : there ? Good : Line, 8);
    paint_.fonts.middle(state, right - w + 10, y + 26, 16, there && !busy ? Surface : Ink, true);
    if (busy) {
      const float ratio = retroBytes_ ? std::min(1.f, float(retroReceived_.load()) / float(retroBytes_)) : 0.f;
      paint_.rect(right - 160, y + 58, 160, 8, Line, 4);
      paint_.rect(right - 160, y + 58, std::max(8.f, 160 * ratio), 8, Butter, 4);
    } else {
      const auto size = formatBytes(game.size);
      paint_.fonts.text(size, right - paint_.fonts.measure(size, 17, true), y + 50, 17, Ink, true);
    }
  }
}
// A focused name too long for its row scrolls: a pause, then left at a
// steady pace, a pause at the end, and back to the start.
void Application::marquee(const std::string &text, float x, float y, int size, float width) {
  const float full = paint_.fonts.measure(text, size, true);
  if (full <= width) {
    paint_.fonts.text(text, x, y, size, Ink, true);
    return;
  }
  constexpr float Speed = 60, Pause = 2000; // px per second, ms: rest first, then move
  // Starts from the beginning each time the focus lands on a new name.
  if (text != marqueeText_) {
    marqueeText_ = text;
    marqueeAt_ = SDL_GetTicks();
  }
  const float travel = full - width, run = travel / Speed * 1000;
  const float t = float((SDL_GetTicks() - marqueeAt_) % uint32_t(Pause * 2 + run));
  const float shift = t < Pause ? 0 : t < Pause + run ? (t - Pause) / 1000 * Speed : travel;
  SDL_Rect outer;
  SDL_RenderGetClipRect(paint_.renderer, &outer);
  const bool clipped = SDL_RenderIsClipEnabled(paint_.renderer);
  const SDL_Rect inner{int(x), int(y - 4), int(width), size * 2};
  SDL_RenderSetClipRect(paint_.renderer, &inner);
  paint_.fonts.text(text, x - shift, y, size, Ink, true);
  SDL_RenderSetClipRect(paint_.renderer, clipped ? &outer : nullptr);
}
} // namespace foil::ui
