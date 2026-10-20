#include "ui/Application.hpp"
#include "platform/Cores.hpp"
#include "core/Log.hpp"
#include "net/RetroApi.hpp"
#include "core/Json.hpp"
#include "core/Sha256.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
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
    {"All", 0, false, 0, 0},             // i18n: keep
    {"Rated 80+", 80, false, 0, 0},     // i18n: keep
    {"Rated 90+", 90, false, 0, 0},     // i18n: keep
    {"Own bezel", 0, true, 0, 0},          // i18n: keep
    {"Under 1 MB", 0, false, 0, MB},         // i18n: keep
    {"Under 10 MB", 0, false, 0, 10 * MB},   // i18n: keep
    {"100 MB and up", 0, false, 100 * MB, 0}, // i18n: keep
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
bool Application::retroArchPresent() const { return cardHas(retro::RetroArchApp); }
bool Application::retroInstalled(const retro::Game &game) const {
  if (game.romPath.empty()) return false;
  const auto found = retroOnCard_.find(game.romPath);
  return found != retroOnCard_.end() && found->second;
}
// The game's core is not on the card while RetroArch is.
bool Application::retroCoreMissing(const retro::Game &game) const {
  const auto core = retro::corePath(game.core);
  return !core.empty() && retroArchPresent() && !cardHas(core);
}
// RetroArch and the game's core both on the card: it can start.
bool Application::retroReady(const retro::Game &game) const {
  const auto core = retro::corePath(game.core);
  return retroArchPresent() && (core.empty() || cardHas(core));
}
bool Application::retroArchPending() const {
  return std::any_of(history_.begin(), history_.end(), [](const history::Entry &e) {
    return e.kind == "retroarch" && (e.outcome == history::Outcome::Queued || e.outcome == history::Outcome::Running);
  });
}
bool Application::retroBezelMissing(const retro::Game &game) const {
  if (game.bezel == retro::Game::Bezel::None || game.bezelPath.empty() || !retroInstalled(game)) return false;
  const auto found = retroOnCard_.find(game.bezelPath);
  return found != retroOnCard_.end() && !found->second;
}
bool Application::runRetroJob(const std::string &busy, std::function<std::function<void()>()> job) {
  if (!session_ && busy != "shortcut") return false; // a shortcut is made from the card, with the server's picture if any
  if (busy == "list") {
    if (retroListWorker_.joinable()) return false;
    retroListWorker_ = std::thread([this, job = std::move(job)] {
      std::function<void()> finish;
      if (!guarded("retro list", [&] { finish = job(); }))
        finish = [this] { message(tr("Something went wrong with the retro task. Check log.txt.")); };
      retroListFinish_ = std::move(finish);
      retroListDone_ = true;
    });
    return true;
  }
  if (retroWorker_.joinable()) return false;
  retroBusy_ = busy;
  retroReceived_ = retroKept_ = 0;
  retroWorker_ = std::thread([this, job = std::move(job)] {
    // Off the drawing core: checking the files already on the card (their
    // hashes) and writing them took frames from the screen.
    const platform::OnCore off(platform::WriterCore);
    std::function<void()> finish;
    if (!guarded("retro", [&] { finish = job(); }))
      finish = [this] { message(tr("Something went wrong with the retro task. Check log.txt.")); };
    retroFinish_ = std::move(finish);
    retroDone_ = true;
  });
  return true;
}
void Application::pollRetro() {
  if (retroDone_.exchange(false)) {
    retroWorker_.join();
    retroBusy_.clear();
    if (retroFinish_) std::exchange(retroFinish_, nullptr)();
  }
  if (!retroListDone_.exchange(false)) {
    if (!retroListWorker_.joinable()) startRetroDetail(); // one wanted while a list was loading
    if (!retroListWorker_.joinable() && !shelfWant_.empty()) {
      const auto want = shelfWant_;
      const auto row = std::find_if(retroShelves_.begin(), retroShelves_.end(),
                                    [&](const retro::Shelf &s) { return s.system.id == want; });
      if (row != retroShelves_.end()) loadShelfMore(int(row - retroShelves_.begin()));
      else shelfWant_.clear();
    }
    return;
  }
  retroListWorker_.join();
  if (retroListFinish_) std::exchange(retroListFinish_, nullptr)();
  // L R pressed while a list was loading: that result was for the system
  // left behind, so the one now shown loads here.
  if (retroView_ == RetroView::List && !retroListWorker_.joinable() && !retroSystems_.empty() && !retroError_ &&
      retroListSystem_ != retroSystems_[retroSystem_].id)
    loadRetroPage(false);
  if (screen_ == Screen::Retro && !hasFocusable() && !retroListWorker_.joinable()) sidebar_ = true;
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
      shelfPage_.clear();
      shelfDone_.clear();
      shelfWant_.clear();
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
  raLinkClose(); // from 업적: its QR goes
  retroView_ = RetroView::List;
  retroSystem_ = system;
  retroQuery_.clear();
  retroGames_.clear();
  retroListSystem_.clear();
  retroTotal_ = 0;
  selected_ = 0;
  sidebar_ = false;
  if (!retroListWorker_.joinable()) loadRetroPage(false);
}
void Application::loadShelfMore(int row) {
  if (!session_ || row < 0 || row >= int(retroShelves_.size())) return;
  const auto system = retroShelves_[size_t(row)].system.id;
  if (system.empty() || shelfDone_.count(system)) return;
  if (retroListWorker_.joinable()) {
    shelfWant_ = system; // asked again once the worker is free (pollRetro)
    return;
  }
  shelfWant_.clear();
  const auto server = session_->server();
  const auto state = state_;
  const auto sd = sdRoot_;
  const int page = shelfPage_[system] + 1;
  runRetroJob("list", [this, server, state, system, page, sd]() -> std::function<void()> {
    net::RetroReply reply;
    auto result = net::retroGames(server, state, system, page, retro::Filter{}, reply, &stop_);
    std::map<std::string, bool> onCard;
    for (const auto &g : result.value.games)
      for (const auto *path : {&g.romPath, &g.bezelPath})
        if (!path->empty()) onCard[*path] = fileExists(sd + "/" + *path);
    return [this, result = std::move(result.value), reply, system, page, onCard = std::move(onCard)]() mutable {
      if (reply.error) return; // asked again when the shelf comes near its end
      const auto shelf = std::find_if(retroShelves_.begin(), retroShelves_.end(),
                                      [&](const retro::Shelf &s) { return s.system.id == system; });
      if (shelf == retroShelves_.end()) return;
      // Versions of one game take one card, as on the home: what the shelf
      // shows already (by id or by its original title) is left out.
      std::set<std::string> ids, originals;
      for (const auto &g : shelf->games) {
        ids.insert(g.id);
        if (!g.original.empty()) originals.insert(g.original);
      }
      for (auto &g : result.games) {
        if (ids.count(g.id) || (!g.original.empty() && originals.count(g.original))) continue;
        ids.insert(g.id);
        if (!g.original.empty()) originals.insert(g.original);
        shelf->games.push_back(std::move(g));
      }
      shelfPage_[system] = page;
      if (result.games.empty() || page * std::max(1, result.pageSize) >= result.total) shelfDone_.insert(system);
      for (auto &[path, there] : onCard) retroOnCard_[path] = there;
    };
  });
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
// The version under the cursor, in full (its cover, size, where it goes),
// fetched off the drawing thread; the popup shows it once it is here.
void Application::requestRetroDetail(const std::string &id) {
  retroDetailWant_ = id;
  startRetroDetail();
}
void Application::startRetroDetail() {
  if (retroDetailWant_.empty() || screen_ != Screen::RetroDetail || !session_) return;
  const auto server = session_->server();
  const auto state = state_;
  const auto sd = sdRoot_;
  const auto id = retroDetailWant_;
  runRetroJob("list", [this, server, state, sd, id]() -> std::function<void()> {
    net::RetroReply reply;
    auto detail = net::retroDetail(server, state, id, reply, &stop_).value;
    std::map<std::string, bool> onCard;
    for (const auto *path : {&detail.game.romPath, &detail.game.bezelPath})
      if (!path->empty()) onCard[*path] = fileExists(sd + "/" + *path);
    return [this, detail = std::move(detail), reply, id, onCard = std::move(onCard)]() mutable {
      if (screen_ != Screen::RetroDetail || retroDetailWant_ != id) return; // closed, or another one wanted now
      retroDetailWant_.clear();
      retroDetailLoading_ = false;
      if (reply.error || detail.game.id.empty()) return;
      // What is known of the game; one version may know more than another.
      retroMeta_ = std::move(detail.meta);
      detail_.screenshots = retroMeta_.screenshots;
      for (auto &[path, there] : onCard) retroOnCard_[path] = there;
      retroPick_ = std::move(detail.game);
      retroLoaded_games_[retroPick_.id] = retroPick_;
      if (detail.variants.size() >= 2 && retroVariants_.empty()) {
        // First look at its versions: the server's pick starts checked.
        const auto best = std::find_if(detail.variants.begin(), detail.variants.end(),
                                       [](const retro::Variant &v) { return v.representative && !v.missing; });
        if (best != detail.variants.end()) retroChecked_ = {best->id};
      }
      if (detail.variants.size() >= 2) retroVariants_ = std::move(detail.variants);
      const auto at = std::find_if(retroVariants_.begin(), retroVariants_.end(),
                                   [&](const retro::Variant &v) { return v.id == retroPick_.id; });
      if (at != retroVariants_.end()) retroVariant_ = int(at - retroVariants_.begin());
    };
  });
}
void Application::moveRetroVariant(int step) {
  if (retroVariants_.size() < 2) return;
  const int next = std::clamp(retroVariant_ + step, 0, int(retroVariants_.size()) - 1);
  if (next == retroVariant_) return;
  retroVariant_ = next;
  requestRetroDetail(retroVariants_[size_t(next)].id);
}
bool Application::getRetroGame(const retro::Game &game, bool again) {
  if (game.bundleUrl.empty()) return false;
  if (!again && retroInstalled(game) && !retroBezelMissing(game)) {
    message(retroArchPresent() ? tr("Already on the card. Start it from RetroArch.")
                               : tr("You already have this game. Press Y to get RetroArch and play it."));
    return false;
  }
  if (sdFree_ && *sdFree_ < game.size + game.size / 10 + (8u << 20)) {
    alertTitle_ = tr("Cannot get the game");
    alert_ = trf("Not enough space on the SD card. {0} is needed.", {formatBytes(game.size)});
    return false;
  }
  // RetroArch or the game's core missing: the base pack goes in the queue
  // first, so the game (and its shortcut) only come once it can start.
  const bool ready = retroReady(game);
  if (!ready && !retroArchPending() && syncRetroArch(false))
    message(tr("RetroArch comes first, then the game: it needs RetroArch's files to start."));
  // Its turn comes in 다운로드's queue, behind whatever downloads now.
  return queueJob("retro", game.name, game.iconUrl, game.bundleUrl, game.size, retro::serializeGame(game));
}
bool Application::runRetroGame(const retro::Game &game, const Server &server) {
  if (game.bundleUrl.empty()) return false;
  const auto state = state_;
  const auto sd = sdRoot_;
  retroBytes_ = game.size;
  if (!retroWorker_.joinable()) retroCancel_ = false;
  // Its turn came but RetroArch's base files did not (the server busy, a
  // failure): the game is not fetched, since neither it nor its shortcut
  // could start; it says so and why.
  if (!retroReady(game) && !retroArchPending()) {
    finishRetroEntry("retro", history::Outcome::Failed,
                     tr("RetroArch's base files are not on the card, so the game was not downloaded. Get RetroArch (Y on the retro screen), then try again."),
                     0);
    alertTitle_ = tr("Could not get the game");
    alert_ = tr("RetroArch's base files are not on the card, so the game was not downloaded. Get RetroArch (Y on the retro screen), then try again.");
    return true;
  }
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
          cardChanged();
          if (finishRetroEntry("retro", ok ? history::Outcome::Done : cancelled ? history::Outcome::Cancelled : history::Outcome::Failed,
                           ok || cancelled ? "" : error.message, ok ? game.size : received))
            return;
          if (cancelled && !ok) {
            message(trf("Cancelled the download of {0}.", {game.name}));
            return;
          }
          if (!ok) {
            alertTitle_ = tr("Could not get the game");
            alert_ = error.message;
            return;
          }
          // A shortcut only once it can start (RetroArch and its core there).
          if (retroReady(game) && installRetroShortcut(game)) return; // 다운로드 shows the shortcut going in
          message(retroArchPresent() ? trf("Got {0}. Start it from RetroArch.", {game.name})
                                     : trf("Got {0}. Press Y to get RetroArch too.", {game.name}));
        };
      }))
    return false;
  return true;
}
// True when it stopped for a pause: it waits at the front of the queue.
bool Application::finishRetroEntry(const std::string &kind, history::Outcome outcome, const std::string &error,
                                   uint64_t bytes, uint64_t kept) {
  const bool pausing = std::exchange(retroPausing_, false);
  const auto running = std::find_if(history_.begin(), history_.end(), [&](const history::Entry &e) {
    return e.kind == kind && e.outcome == history::Outcome::Running;
  });
  if (running == history_.end()) return false;
  logLine(kind + " " + (outcome == history::Outcome::Done ? "done" : outcome == history::Outcome::Cancelled ? "cancelled" : "failed") + ": " +
          running->install.name + (error.empty() ? "" : " · " + error));
  if (outcome == history::Outcome::Done) sounds_.play(kind == "shortcut" ? Cue::Shortcut : Cue::Done);
  else if (outcome == history::Outcome::Failed) sounds_.play(Cue::Error);
  if (pausing && outcome == history::Outcome::Cancelled) {
    running->outcome = history::Outcome::Queued;
    history::takeFirstTurn(history_, running->id);
    queueHeld_ = true;
    saveHistory();
    rebuildQueue();
    return true;
  }
  // RetroArch already up to date fetched nothing: no entry for it.
  if (outcome == history::Outcome::Done && bytes == 0 && kind == "retroarch") history_.erase(running);
  else {
    if (bytes && !running->install.files.empty()) running->install.files.front().size = bytes;
    // What it took, for 완료 (nothing to install after: no install time).
    if (outcome == history::Outcome::Done && bytes) {
      // What came over the network, for its average speed: files already
      // on the card were not fetched.
      running->bytes = bytes - std::min(kept, bytes);
      running->startedAt = jobStartedAt_;
      running->downloadSeconds = (SDL_GetTicks() - jobStartMs_) / 1000.0;
    }
    history::finish(history_, outcome, outcome == history::Outcome::Failed ? failureText(error) : error,
                    int64_t(std::time(nullptr)), kind);
  }
  saveHistory();
  return false;
}
// The small downloads' pause: stop now and keep the part, or go on.
void Application::pauseSmallDownload(bool pause) {
  if (pause) {
    if (!retroDownloading()) return;
    retroPausing_ = true;
    retroCancel_ = true;
    return;
  }
  queueHeld_ = false;
  if (auto error = launchQueued())
    alert_ = error.message;
}
// A HOME Menu shortcut goes in through the installer like any other title;
// false when there is none to make, 설정 turned them off, or the server has
// none for it.
bool Application::installShortcut(const std::string &name, const std::string &icon, const std::string &url,
                                  const std::string &titleId, uint64_t size, bool justDownloaded) {
  if (url.empty() || !session_ || !state_.makeShortcuts) return false;
  Item shortcut;
  shortcut.url = url;
  shortcut.name = name;
  shortcut.titleId = shortcut.baseId = titleId;
  shortcut.size = size;
  shortcut.icon = icon;
  shortcut.type = "shortcut"; // an extra of what came down: no row of its own in 다운로드
  shortcut.format = "nsp";     // its address ends in the server's route, not .nsp
  // Next in the queue, ahead of what waits: it belongs to what just came down.
  const bool waits = downloadRunning();
  if (auto error = startInstall({shortcut}, trf("{0} shortcut", {name}), icon, "sd", {}, true)) {
    // Refused before it began: made here instead, when it can be.
    logLine("shortcut: the server's for " + name + " refused: " + error.message);
    if (shortcutFailed(titleId)) return true;
    alertTitle_ = tr("Could not make the shortcut");
    alert_ = error.message;
    return true;
  }
  if (!waits)
    message(justDownloaded ? trf("Got {0}. Installing its HOME Menu shortcut.", {name})
                           : trf("Installing the {0} HOME Menu shortcut.", {name}));
  return true;
}
bool Application::syncRetroArch(bool full, bool first) {
  return queueJob("retroarch", "RetroArch", "", "retroarch", 0, full ? "full" : "", 0, first);
}
bool Application::runRetroArch(bool full, const Server &server) {
  const auto state = state_;
  const auto sd = sdRoot_;
  retroBytes_ = 0;
  if (!retroWorker_.joinable()) retroCancel_ = false;
  if (!runRetroJob("pack", [this, server, state, sd, full]() -> std::function<void()> {
        net::RetroReply reply;
        auto pack = net::retroPack(server, state, reply, &stop_);
        // The server still making its lists (503 with Retry-After): wait as
        // long as it says, a few times, before giving up.
        for (int tries = 0; reply.error && reply.retryAfter > 0 && tries < 4 && !retroCancel_ && !stop_; ++tries) {
          logLine("retroarch manifest busy, again in " + std::to_string(reply.retryAfter) + " s");
          for (int waited = 0; waited < reply.retryAfter * 10 && !retroCancel_ && !stop_; ++waited) SDL_Delay(100);
          reply = {};
          pack = net::retroPack(server, state, reply, &stop_);
        }
        if (reply.error) {
          const auto why = reply.error.message;
          return [this, why] {
            finishRetroEntry("retroarch", history::Outcome::Failed, why, 0);
            alertTitle_ = tr("Could not get RetroArch");
            alert_ = why;
          };
        }
        auto record = full ? retro::PackRecord{} : retro::parsePackRecord(readFile(sd + PackRecordPath));
        // Files on the card with no record of them (the record lost, or put
        // there by hand) but the right size: their hash says whether they
        // are the server's, which spares fetching the whole pack again.
        if (!full) {
          int adopted = 0;
          // Counted in bytes for the card: no bytes come in while it runs,
          // and on a slow card a big core takes a while.
          uint64_t toCheck = 0;
          for (const auto &file : pack.value.files)
            if (!record.files.count(file.path) && !file.sha256.empty()) toCheck += file.size;
          cardCheck_.done = 0;
          cardCheck_.total = toCheck;
          for (const auto &file : pack.value.files) {
            if (retroCancel_ || stop_) break;
            if (record.files.count(file.path) || file.sha256.empty()) continue;
            const uint64_t before = cardCheck_.done;
            const auto local = sd + "/" + file.path;
            if (fileExists(local) && fileSize(local) == file.size &&
                sha256File(local, &retroCancel_, &cardCheck_.done) == file.sha256) {
              record.files[file.path] = file.sha256;
              ++adopted;
            }
            cardCheck_.done = before + file.size;
          }
          if (adopted) {
            logLine("retroarch: " + std::to_string(adopted) + " files already on the card kept");
            std::string kept;
            atomicWrite(sd + PackRecordPath, retro::serializePackRecord(record), kept);
          }
        }
        cardCheck_.total = 0;
        // Stopped while checking: nothing is planned or asked for.
        const auto plan = retroCancel_ || stop_ ? retro::PackPlan{} : retro::planPack(pack.value, record, fileExists, fileSize, sd);
        retroBytes_ = plan.bytes;
        Error error;
        int done = 0;
        std::string ignored;
        // Most of the pack: the whole pack as one tar, writing only what the
        // plan wants (changed, missing; a settings file already there stays).
        if (!pack.value.bundleUrl.empty() && retro::planByBundle(plan, pack.value.bundleSize)) {
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
          cardChanged();
          if (finishRetroEntry("retroarch",
                               cancelled ? history::Outcome::Cancelled : error ? history::Outcome::Failed : history::Outcome::Done,
                               cancelled ? "" : error.message, received))
            return;
          if (cancelled)
            message(trf("RetroArch install cancelled after {0} of {1} files. Installing again picks up from there.",
                        {std::to_string(done), std::to_string(total)}));
          else if (error) {
            alertTitle_ = tr("Could not get RetroArch");
            alert_ = trf("Got {0} of {1}. Trying again picks up where it stopped. {2}",
                         {std::to_string(done), std::to_string(total), error.message});
          } else
            message(total ? trf("Got {0} RetroArch files.", {std::to_string(total)})
                          : std::string(tr("RetroArch is up to date.")));
        };
      }))
    return false;
  return true;
}
void Application::retroAction(Action a) {
  if (raModal()) {
    raAction(a);
    return;
  }
  if (retroArchAsking_) {
    if (a == Action::Back) retroArchAsking_ = false;
    if (a == Action::Accept || a == Action::Queue) {
      retroArchAsking_ = false;
      syncRetroArch(a == Action::Queue);
    }
    return;
  }
  // On 업적 the header keeps its keys: X goes back, L R open the systems
  // and Y is RetroArch's, as on the shelves.
  if (retroView_ == RetroView::Achievements && a == Action::Search) {
    leaveRaPage();
    return;
  }
  if (retroView_ == RetroView::Achievements && (a == Action::Previous || a == Action::Next)) {
    if (!retroShelves_.empty()) openRetroList(retroRow_);
    return;
  }
  if (retroView_ == RetroView::Achievements && a != Action::Queue) {
    raPageAction(a);
    return;
  }
  // X on the shelves: RetroAchievements (search is the system list's).
  if (a == Action::Search && retroView_ == RetroView::Home) {
    openRaPage();
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
      if (!retroListWorker_.joinable()) loadRetroPage(false);
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
    const int count = int(retroSystems_.size());
    retroSystem_ = (retroSystem_ + (a == Action::Next ? 1 : count - 1)) % count;
    retroGames_.clear();
    retroListSystem_.clear();
    retroTotal_ = 0;
    selected_ = 0;
    if (!retroListWorker_.joinable()) loadRetroPage(false);
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
  if (selected_ >= int(retroGames_.size()) - 8 && !retroListWorker_.joinable()) loadRetroPage(true);
  if (a == Action::Accept)
    if (const auto *game = retroFocused()) openRetroDetail(*game);
}
// What a game is: its file format, a bezel, its score; for the list's
// labels and the info popup.
float Application::achievementLabel(const achievements::Summary &a, float x, float top, float height, int size) {
  if (a.count <= 0) return 0;
  const auto text = trf("Achievements {0}", {std::to_string(a.count)});
  const float icon = float(size), w = 8 + icon + 4 + paint_.fonts.measure(text, size, true) + 10;
  paint_.rect(x, top, w, height, a.byTitle ? Line : Butter, int(height / 3));
  paint_.icon(28, x + 8, top + (height - icon) / 2, Ink, size);
  paint_.fonts.middle(text, x + 8 + icon + 4, top + height / 2, size, Ink, true);
  return w;
}
std::vector<std::string> Application::retroLabels(const retro::Game &game, bool detail) const {
  std::vector<std::string> out;
  if (const auto dot = game.file.rfind('.'); dot != std::string::npos) {
    auto format = game.file.substr(dot + 1);
    std::transform(format.begin(), format.end(), format.begin(), ::toupper);
    out.push_back(format);
  }
  if (game.bezel == retro::Game::Bezel::Game) out.push_back(tr("Game bezel"));
  else if (game.bezel == retro::Game::Bezel::System) out.push_back(tr("Default bezel"));
  // What kind of release it is, in one word; the popup adds where it came
  // from and its version.
  const auto &edition = game.edition;
  if (edition.source == "official") out.push_back(tr("Official"));
  else if (edition.source == "patch") out.push_back(tr("Korean|script"));
  if (detail && !edition.from.empty()) {
    const auto from = edition.from == "jp" ? tr("Japan") : edition.from == "us" ? tr("US")
                    : edition.from == "eu" ? tr("Europe") : "";
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
  if (game.rating > 0) out.push_back(trf("Score {0}", {std::to_string(game.rating)}));
  return out;
}
void Application::retroArchConfirm() {
  const auto body = modal(700, 24 + 84 + 24, tr("Get RetroArch again?"), tr("Cancel"), tr("Get all again"), tr("Get again"), 3);
  paint_.fonts.wrapped(tr("It is already on the SD card. Only changed files and missing cores come down again. Get all again fetches every file anew; either way your settings stay."), body.x + 32,
                       body.y + 24, 19, Secondary, body.w - 64, 3);
}
void Application::retroScreen() {
  if (retroRetryAt_ && SDL_GetTicks() >= retroRetryAt_) {
    retroRetryAt_ = 0;
    retroLoaded_ = false;
  }
  if (!retroLoaded_ && connected_ && !retroListWorker_.joinable()) loadRetroHome();
  if (retroView_ == RetroView::Home) retroHome();
  else if (retroView_ == RetroView::Achievements) raPage();
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
    paint_.fonts.middle(trf("Installing RetroArch · {0} / {1}", {formatBytes(retroReceived_.load()), formatBytes(retroBytes_)}),
                        322, y + Height / 2, 19, Ink, true);
    paint_.rect(910, y + 28, 316, 8, Line, 4);
    paint_.rect(910, y + 28, std::max(8.f, 316 * ratio), 8, Butter, 4);
    return;
  }
  hit(260, y, 992, Height, [this] { action(Action::Queue); });
  const bool ready = retroArchPresent();
  paint_.icon(ready ? 24 : 25, 282, y + 19, ready ? Good : Pending, 26);
  // What the player needs to know: whether games will run, and where they show up.
  paint_.fonts.middle(!ready                 ? tr("RetroArch is not installed. Install it with Y to play games.")
                      : state_.makeShortcuts ? tr("RetroArch is installed. Downloading a game adds a shortcut to the HOME Menu.")
                                             : tr("RetroArch is installed."),
                      322, y + Height / 2, 19, Ink, true, 900);
}
void Application::retroList() {
  const auto *system = retroSystems_.empty() ? nullptr : &retroSystems_[retroSystem_];
  // Right: the same filter button and search box as 모든 게임.
  searchBar(retroQuery_);
  std::string summary = retroFilters_.empty() ? std::string(tr("All")) : popupFilterLabels()[retroFilters_.front()];
  if (retroFilters_.size() > 1) summary = trf("{0} +{1}", {summary, std::to_string(retroFilters_.size() - 1)});
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
    // L and R are bare key badges: pressed, the badge darkens, not a box round it.
    headerItem("previous", 256, 26, paint_.keyWidth("L") + 8, 44, [this] { action(Action::Previous); }, false,
               {x, Mid - 16, paint_.keyWidth("L"), 32}, 16);
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
    headerItem("next", x + 8, 26, paint_.keyWidth("R") + 8, 44, [this] { action(Action::Next); }, false,
               {x + 12, Mid - 16, paint_.keyWidth("R"), 32}, 16);
    paint_.key("R", x + 12, Mid);
  } else
    pageTitle(tr("Retro games"), "", 0);
  // Here Y opens the filters, so the strip shows only a RetroArch install
  // that is running (the home is where it starts).
  const bool banner = retroBusy_ == "pack";
  if (banner) retroBanner(90);
  const float Top = banner ? 170 : 90;
  const int Visible = banner ? 4 : 5;
  if (retroGames_.empty()) {
    if (!connected_)
      offlineNotice(tr("Retro games load once the server is connected."));
    else if (retroMissing_)
      notice(tr("This server has no retro games."), tr("They appear here once the server has a retro folder."), 35);
    else if (retroError_ && retroRetryAt_)
      notice(tr("The server is reading its retro files."), tr("It loads again by itself shortly."), 35);
    else if (retroError_)
      notice(tr("Could not load the retro games."), retroError_.message, 25);
    else if (retroListWorker_.joinable() || !retroLoaded_ || retroListSystem_.empty())
      for (int n = 0; n <= Visible; ++n) paint_.skeleton(260, Top + n * RowStep, 992, RowHeight, 16);
    else if (!retroQuery_.empty())
      notice(tr("No results."), tr("Press X to try another name, or B to clear the search."), 1);
    else
      notice(tr("No retro games yet."), tr("ROMs put in the server's retro folder appear here."), 35);
    return;
  }
  const int count = int(retroGames_.size());
  selected_ = std::clamp(selected_, 0, count - 1);
  // The focus rides the middle row once past it, the list scrolling under it.
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("retro", 232, Top, 1048, 660 - (Top), float(std::clamp(selected_ - Visible / 2, 0, std::max(0, count - Visible))),
                          float((count) - (Visible)), RowStep);
  const int first = int(top);
  paint_.scrollbar(1266, Top, RowStep * Visible - 12, top, Visible, float(std::max(retroTotal_, count)));
  // One row past the footer so a long list looks like it goes on.
  clipTo(232, Top - 8, 1048, 660 - Top + 8);
  for (int i = first; i < count && i <= first + Visible + 1; ++i) {
    const auto &game = retroGames_[i];
    const float y = Top + (i - top) * RowStep;
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
    // Under the name, labels: its achievements first, then its format, a
    // bezel, its score.
    float at = 360;
    if (const float w = achievementLabel(game.achievements, at, y + 46, 26, 15)) at += w + 6;
    for (const auto &label : retroLabels(game)) at += paint_.pill(label, at, y + 46) + 6;
    at += versionsBadge(game.variants, at, y + 46, false); // with the labels: the cover is too small for it
    const float right = 1230;
    const bool busy = retroBusy_ == game.id;
    const bool bezel = retroBezelMissing(game), there = retroInstalled(game) && !bezel;
    const std::string state = busy ? tr("Downloading") : bezel ? tr("Get bezel") : there ? tr("Installed") : tr("Not installed");
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
  unclip();
}
// A focused name too long for its row scrolls: a pause, then left at a
// steady pace, a pause at the end, and back to the start.
void Application::marquee(const std::string &text, float x, float y, int size, float width, bool bold) {
  const float full = paint_.fonts.measure(text, size, bold);
  if (full <= width) {
    paint_.fonts.text(text, x, y, size, Ink, bold);
    return;
  }
  // A ticker: rest at the start, then slide left with the name coming
  // round again from the right, and rest once it is whole again.
  constexpr float Speed = 40, Pause = 1800, Gap = 56; // px per second, ms, px
  // Starts from the beginning each time the focus lands on a name; several
  // names (a popup title and a row) run on their own clocks.
  const uint32_t now = SDL_GetTicks();
  if (marquees_.size() > 16)
    for (auto it = marquees_.begin(); it != marquees_.end();)
      it = now - it->second.second > 250 ? marquees_.erase(it) : std::next(it);
  auto &[startedAt, drawnAt] = marquees_[text];
  if (!drawnAt || now - drawnAt > 250) startedAt = now;
  drawnAt = now;
  // It eases into its speed and eases out as the name comes whole again,
  // so it never jumps off or stops dead.
  constexpr float Ramp = .8f; // s
  const float loop = full + Gap, run = loop / Speed + Ramp;
  const float t = float((now - startedAt) % uint32_t(Pause + run * 1000)) / 1000;
  const float m = std::max(0.f, t - Pause / 1000);
  const float shift = m < Ramp         ? Speed * m * m / (2 * Ramp)
                      : m < run - Ramp ? Speed * (m - Ramp / 2)
                                       : loop - Speed * (run - m) * (run - m) / (2 * Ramp);
  SDL_Rect outer;
  SDL_RenderGetClipRect(paint_.renderer, &outer);
  const bool clipped = SDL_RenderIsClipEnabled(paint_.renderer);
  SDL_Rect inner{int(x), int(y - 4), int(width), size * 2};
  // Stay inside a scrolling area's clip as well.
  if (clipped && !SDL_IntersectRect(&inner, &outer, &inner))
    return;
  SDL_RenderSetClipRect(paint_.renderer, &inner);
  paint_.fonts.text(text, x - shift, y, size, Ink, bold);
  if (shift > 0)
    paint_.fonts.text(text, x - shift + loop, y, size, Ink, bold);
  SDL_RenderSetClipRect(paint_.renderer, clipped ? &outer : nullptr);
}
} // namespace foil::ui
