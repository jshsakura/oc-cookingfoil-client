#include "ui/Application.hpp"
#include "core/Config.hpp"
#include "platform/Storage.hpp"
#include "platform/Installed.hpp"
#include "core/Log.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
namespace foil::ui {
// Server names are the user's own, except the stock ones config files ship
// with, which follow the interface language.
const char *screenLabel(Screen screen) {
  switch (screen) {
  case Screen::Library: return tr("모든 게임");
  case Screen::Installed:
  case Screen::Updates: return tr("내 게임");
  case Screen::Patches: return tr("패치|메뉴");
  case Screen::Homebrew: return tr("홈브류");
  case Screen::Retro: return tr("레트로 게임");
  case Screen::Saves: return tr("세이브");
  case Screen::Downloads: return tr("다운로드");
  case Screen::Settings: return tr("설정");
  default: return tr("홈");
  }
}
int menuIndex(Screen screen) {
  if (screen == Screen::Updates) screen = Screen::Installed; // a tab of 내 게임
  for (int i = 0; i < MenuItems; ++i)
    if (MenuScreens[i] == screen) return i;
  return 0;
}
Application::Application(SDL_Renderer *renderer, std::string configPath,
                         std::string statePath, std::string fixture)
    : paint_(renderer), configPath_(std::move(configPath)),
      statePath_(std::move(statePath)), fixture_(std::move(fixture)) {}
Application::~Application() {
  stop_ = true;
  retroCancel_ = true;
  updateCancel_ = true;
  installer_.cancel();
  if (network_.joinable())
    network_.join();
  if (deviceWorker_.joinable())
    deviceWorker_.join();
  patchCheckStop_ = true;
  if (patchCheckWorker_.joinable())
    patchCheckWorker_.join();
  if (patchWorker_.joinable())
    patchWorker_.join();
  if (cleanupWorker_.joinable())
    cleanupWorker_.join();
  if (saveWorker_.joinable())
    saveWorker_.join();
  if (retroWorker_.joinable())
    retroWorker_.join();
  if (updateWorker_.joinable())
    updateWorker_.join();
  if (logWorker_.joinable())
    logWorker_.join();
  if (homebrewWorker_.joinable())
    homebrewWorker_.join();
  if (homebrewCardWorker_.joinable())
    homebrewCardWorker_.join();
  if (shopSavesWorker_.joinable())
    shopSavesWorker_.join();
  if (dropboxWorker_.joinable())
    dropboxWorker_.join();
  if (awake_)
    platform::keepAwake(false);
}
bool Application::open() {
  // Startup runs on the main thread, so each step also lets the console
  // handle HOME, and logs how long the one before it took.
  auto stepAt = SDL_GetTicks();
  std::string stepName = "window";
  const auto step = [&](const std::string &caption, float progress, const char *name) {
    logLine("startup " + stepName + " " + std::to_string(SDL_GetTicks() - stepAt) + "ms");
    stepAt = SDL_GetTicks();
    stepName = name;
    paint_.splash(caption, progress);
    if (!platform::serviceApplet())
      exitRequested_ = true;
  };
  step("", 0.05f, "fonts");
  if (!paint_.fonts.open())
    return false;
  // The saved language is read now so the caption is already in it;
  // reload() sets it again with the config as the last fallback.
  if (!statePath_.empty())
    state_.language = loadState(statePath_).value.language;
  applyStartupLanguage();
  // Each step says what it is doing under the logo, so a slow SD card or
  // server shows where the time goes.
  step(tr("이미지 캐시를 준비하고 있습니다"), 0.2f, "images");
  images_ = std::make_unique<Images>(paint_.renderer);
  images_->source(
      {}, {},
      statePath_.empty()
          ? ""
          : std::filesystem::path(statePath_).parent_path().string() + "/cache",
      fixture_);
  step(tr("SD 카드의 패치와 다운로드 기록을 읽고 있습니다"), 0.4f, "sd");
#ifdef __SWITCH__
  usePatchFolder("sdmc:");
#else
  if (!statePath_.empty())
    usePatchFolder(std::filesystem::path(statePath_).parent_path().string() + "/sd");
#endif
  if (!configPath_.empty()) {
    step(tr("서버 설정을 읽고 있습니다"), 0.65f, "config");
    reload();
  }
#ifdef __SWITCH__
  step(tr("본체의 게임 목록을 읽고 있습니다"), 0.85f, "installed");
  refreshInstalled();
  // Album (applet mode) gets a few hundred MB and no title rights: say so
  // before something big fails, and how to open it properly.
  if (!platform::applicationMode()) {
    alertTitle_ = tr("애플릿 모드로 실행했습니다");
    alert_ = tr("앨범에서 열면 메모리가 적어 큰 게임 설치가 느리거나 실패할 수 있습니다. 게임을 R 버튼을 누른 채 실행하면 애플리케이션 모드로 열립니다.");
  }
#endif
  logLine("startup " + stepName + " " + std::to_string(SDL_GetTicks() - stepAt) + "ms");
  return true;
}
// Patches live on the SD card: a store under switch/cookingfoil and the
// atmosphere folders they switch on.
void Application::usePatchFolder(const std::string &sd) {
  if (patchWorker_.joinable())
    return;
  sdRoot_ = sd;
  homebrewLedger_ = homebrew::parseLedger(readFile(sd + "/switch/cookingfoil/homebrew.json"));
  // An app that closed mid-install left that one running: it reads as failed.
  history_ = history::parse(readFile(sd + "/switch/cookingfoil/downloads.json"));
  history::settle(history_);
  // A check of the old card stops first; it holds only a copy of the ledger.
  patchCheckStop_ = true;
  if (patchCheckWorker_.joinable())
    patchCheckWorker_.join();
  patchCheckDone_ = false;
  patchChecker_.reset();
  patchHealth_.clear();
  patchStore_ = std::make_unique<PatchStore>(sd, sd + "/switch/cookingfoil/patches");
  Error error;
  if (!patchStore_->load(error))
    logLine("patches: " + error.message);
  patchRecords_ = patchStore_->records();
  ++patchLedgerGen_;
  patchCheckWanted_ = true; // once after the app starts
}
void Application::applyStartupLanguage() {
  auto code = state_.language;
  if (code.empty())
    code = platform::systemLanguage();
  if (code.empty())
    code = config_.language;
  setLanguage(languageFromCode(code, Language::Korean));
  paint_.fonts.preferChinese(language() == Language::Chinese);
}
void Application::toggleInstallCheck(bool &skip) {
  skip = !skip;
  saveOwnState();
}
// What the app keeps for itself in state.json (settings, never config.json).
void Application::saveOwnState() {
  if (statePath_.empty())
    return;
  Error error;
  if (!saveState(statePath_, state_, error))
    logLine("state: " + error.message);
}
void Application::chooseLanguage(Language lang) {
  setLanguage(lang);
  paint_.fonts.preferChinese(lang == Language::Chinese);
  state_.language = languageCode(lang);
  shopSavesLoaded_ = false; // server saves come in the language picked
  if (!statePath_.empty()) {
    Error error;
    if (!saveState(statePath_, state_, error))
      logLine("language: " + error.message);
  }
  // Genre names, filter chips and home rows were built from text.
  if (session_) {
    genreNames_ = session_->genres();
    scoreLabels_ = session_->scoreLabels();
  } else if (!genreTable_.empty())
    genreNames_ = net::namesIn(genreTable_, languageCode());
  localizeNames();
  chips_ = libraryChips(groups_, genreNames_);
  rebuildHome();
  listKey_.clear();
  details_.clear(); // descriptions come back in the new language
}
void Application::setSession(std::unique_ptr<net::Session> session) {
  if (network_.joinable() || !session)
    return;
  clearCatalog();
  session_ = std::move(session);
  config_.servers = {session_->server()};
  state_ = session_->state();
  serverIndex_ = 0;
  pairingPending_ = session_->pairingPending();
  pairingInfo_ = session_->pairingInfo();
  applyStartupLanguage();
  // A session that already fetched the catalog brings its shelves and names.
  featured_ = session_->featured();
  features_ = session_->features();
  genreNames_ = session_->genres();
  scoreLabels_ = session_->scoreLabels();
  qr_.encode(pairingInfo_.url);
  pairPollAt_ = pairingPending_ ? SDL_GetTicks() : 0;
  if (!pairingPending_) pairSince_ = 0;
  else if (!pairSince_) pairSince_ = SDL_GetTicks();
  images_ = std::make_unique<Images>(paint_.renderer);
  images_->source(
      session_->server(), state_,
      statePath_.empty()
          ? ""
          : std::filesystem::path(statePath_).parent_path().string() + "/cache",
      fixture_);
}
void Application::localizeNames() {
  const auto slot = size_t(language());
  for (auto &g : groups_) {
    const auto *source = g.base ? &*g.base : &g.representative;
    // Cover art in the interface language when it differs, else the default.
    if (!source->icons.empty() || g.base)
      g.representative.icon = !source->icons.empty() && !source->icons[slot].empty() ? source->icons[slot]
                                                                                     : source->icon;
    if (source->names.empty())
      continue;
    const auto &names = source->names;
    // The chosen language first. A Korean interface then keeps the store's
    // own name (this is a Korean shop); the others fall back to English
    // before it.
    const bool korean = language() == Language::Korean;
    g.representative.name = !names[slot].empty()             ? names[slot]
                            : !korean && !names[1].empty()   ? names[1]
                                                             : source->name;
  }
}
void Application::setItems(std::vector<Item> items) {
  const auto started = SDL_GetTicks();
  groups_ = groupCatalog(items);
  groupIndex_.clear();
  groupIndex_.reserve(groups_.size());
  for (size_t i = 0; i < groups_.size(); ++i)
    groupIndex_.emplace(groups_[i].id, i);
  updatesStale_ = true;
  // Groups copy each item into files (and the base into representative);
  // only the base, or the representative without one, needs its names.
  for (auto &g : groups_) {
    for (auto *list : {&g.alternatives, &g.dlc})
      for (auto &f : *list)
        std::vector<std::string>().swap(f.names), std::vector<std::string>().swap(f.icons);
    if (g.update)
      std::vector<std::string>().swap(g.update->names), std::vector<std::string>().swap(g.update->icons);
    if (g.base)
      std::vector<std::string>().swap(g.representative.names), std::vector<std::string>().swap(g.representative.icons);
  }
  localizeNames();
  chips_ = libraryChips(groups_, genreNames_);
  rebuildHome();
  activeChips_.clear();
  ++catalogVersion_;
  connected_ = true;
  selected_ = 0;
  logLine("catalog grouped " + std::to_string(SDL_GetTicks() - started) + "ms, " + std::to_string(groups_.size()) + " games");
}
const Group *Application::findGroup(const std::string &id) const {
  const auto found = groupIndex_.find(id);
  return found == groupIndex_.end() ? nullptr : &groups_[found->second];
}
std::string Application::installedIcon(const InstalledGame &game) {
  if (!game.iconBytes.empty()) {
    auto key = "installed:" + game.titleId;
    images_->provide(key, game.iconBytes);
    return key;
  }
  const auto *group = findGroup(game.titleId);
  return group ? group->representative.icon : "";
}
void Application::clearCatalog() {
  serverPatches_.clear();
  patchesLoaded_ = false;
  patchError_ = {};
  groups_.clear();
  groupIndex_.clear();
  themes_.clear();
  chips_.clear();
  activeChips_.clear();
  ++catalogVersion_;
  hero_ = 0;
  incoming_.clear();
  selection_.reset();
  details_.clear();
  detail_ = {};
  detailLoading_ = detailRequest_ = false;
  requestedId_.clear();
  connected_ = false;
  pairingPending_ = false;
  pairingInfo_ = {};
  pairPollAt_ = 0;
  selected_ = 0;
  error_ = {};
  notice_.clear();
  if (screen_ == Screen::Detail)
    screen(Screen::Library);
}
std::string *Application::listQuery() {
  switch (screen_) {
  case Screen::Saves: return &savesQuery_;
  case Screen::Patches: return &patchQuery_;
  case Screen::Homebrew: return &homebrewQuery_;
  case Screen::Installed: return &installedQuery_;
  case Screen::Updates: return &updatesQuery_;
  default: return nullptr;
  }
}
void Application::openMenuScreen(Screen target) {
  // The menu opens the plain list; a theme only comes from Home's R.
  activeChips_.clear();
  librarySort_ = SortMode::Recent;
  screen(target);
}
void Application::screen(Screen screen) {
  // A toast belongs to the screen that showed it.
  if (screen != screen_) notice_.clear();
  menuPending_ = false;
  headerFocus_.clear();
  header_.clear();
  headerHome_.clear();
  // Leaving 패치 stops its check; the next visit goes on from there.
  if (screen_ == Screen::Patches && screen != Screen::Patches)
    patchCheckStop_ = true;
  screen_ = screen;
  nav_ = menuIndex(screen);
  selected_ = 0;
  row_ = 1;
  // Read again on the way in only when the last read is a while old;
  // passing through the menu does not reread the console every time.
  constexpr uint32_t InstalledFreshMs = 30000;
  if ((screen == Screen::Installed || screen == Screen::Updates || screen == Screen::Patches) &&
      (!installed_.installed.complete || SDL_GetTicks() - installedAt_ > InstalledFreshMs))
    refreshInstalled();
  if (screen == Screen::Patches) {
    fetchPatches();
    patchCheckWanted_ = true;
  }
  // 홈브류 asks the card which apps are still there; the list loads as it draws.
  if (screen == Screen::Homebrew)
    checkHomebrewCard();
  // 설정 says whether a newer build is on the server, asked once a launch.
  if (screen == Screen::Settings && appUpdate_ == AppUpdate::Unknown && connected_)
    checkAppUpdate();
  // Dropbox's popups belong to the visit that opened them.
  dropboxOpen_ = dropboxUnlinkAsk_ = dropboxChoosing_ = false;
  if (screen == Screen::Saves) {
    backupCounts_.clear();
    refreshInstalled();
  }
  // 레트로 게임 asks the card again which ROMs are there, and an empty list
  // asks the shop again (its folder may have been filled since).
  if (screen == Screen::Retro) {
    retroOnCard_.clear();
    retroView_ = retroHomeMissing_ ? RetroView::List : RetroView::Home;
    if (!retroWorker_.joinable()) retroLoaded_ = false;
  }
  // With nothing to select the cursor stays on the menu instead of vanishing.
  sidebar_ = autoSidebar_ = !hasFocusable();
}
bool Application::hasFocusable() const {
  switch (screen_) {
  case Screen::Home:
    return !groups_.empty();
  case Screen::Library:
    return true; // the search box is always there
  case Screen::Installed:
    return !deviceBusy_ && !gameRows(installedQuery_).empty();
  case Screen::Updates:
    return connected_ && !deviceBusy_ && !updateRows().empty();
  case Screen::Downloads:
    return downloadsTab_ != 0 && !downloadsList().empty();
  case Screen::Patches:
    return !patchRows().empty();
  case Screen::Homebrew:
    return !homebrewRows().empty();
  case Screen::Retro:
    return retroView_ == RetroView::List || !retroShelves_.empty();
  case Screen::Saves:
    return savesTab_ == 2 ? dropboxFocusable() : savesTab_ == 1 ? !shopSaveGroups().empty() : !deviceBusy_ && !saveGameRows().empty();
  case Screen::Settings:
  case Screen::Detail:
    return true;
  }
  return true;
}
void Application::message(const std::string &text) {
  notice_ = text;
  noticeAt_ = SDL_GetTicks();
}
void Application::reload(int prefer) {
  if (network_.joinable() && !done_) {
    message(tr("서버에 연결하고 있습니다."));
    return;
  }
  poll();
  auto parsed = loadConfig(configPath_);
  if (parsed.error) {
    error_ = parsed.error;
    screen(Screen::Settings);
    return;
  }
  const auto state = loadState(statePath_);
  if (state.error) {
    error_ = state.error;
    screen(Screen::Settings);
    return;
  }
  clearCatalog();
  session_.reset();
  serverIndex_ = 0;
  config_ = std::move(parsed.value);
  error_ = {};
  state_ = state.value;
  applyStartupLanguage();
  if (parsed.created)
    message(tr("설정 파일을 만들었습니다. 설정의 + 서버 추가에서 서버를 등록합니다."));
  if (prefer >= 0 && size_t(prefer) < config_.servers.size() && config_.servers[prefer].enabled) {
    connect(prefer);
    return;
  }
  auto found = std::find_if(config_.servers.begin(), config_.servers.end(),
                            [](const auto &s) { return s.enabled; });
  if (found == config_.servers.end()) {
    screen(Screen::Settings);
    return;
  }
  connect(found - config_.servers.begin());
}
void Application::connect(size_t index) {
  if (index >= config_.servers.size() || !config_.servers[index].enabled)
    return;
  if (network_.joinable() && !done_) {
    message(tr("서버에 연결하고 있습니다."));
    return;
  }
  poll();
  clearCatalog();
  serverIndex_ = index;
  session_ = std::make_unique<net::Session>(config_.servers[index], state_,
                                            statePath_, &stop_);
  connected_ = false;
  images_ = std::make_unique<Images>(paint_.renderer);
  images_->source(config_.servers[index], state_,
                  std::filesystem::path(statePath_).parent_path().string() +
                      "/cache",
                  fixture_);
  refresh();
}
void Application::refresh() {
  if (!session_ || network_.joinable())
    return;
  // The approval note may have changed since the session last asked.
  session_->setPairMessage(pairMessage());
  network_ = std::thread([this] {
    incomingError_ = {};
    const bool ok = guarded("catalog", [this] {
      if (session_->refresh(incomingError_) && !session_->unchanged())
        incoming_ = session_->takeItems();
    });
    if (!ok)
      incomingError_.message = tr("목록을 처리하다 오류가 났습니다. log.txt를 확인합니다.");
    done_ = true;
  });
}
void Application::poll() {
  pollDevice();
  pollSaveJob();
  pollRetro();
  pollShopSaves();
  pollDropbox();
  pollPatches();
  pollPatchCheck();
  pollCleanup();
  pollAppUpdate();
  pollLog();
  pollHomebrew();
  // No dimming or auto-sleep while something downloads.
  const bool downloading = installer_.running() || retroDownloading() || (patchWorker_.joinable() && !patchBusy_.empty()) ||
                           !patchQueue_.empty() ||
                           appUpdate_ == AppUpdate::Downloading;
  if (downloading != awake_) {
    awake_ = downloading;
    platform::keepAwake(awake_);
  }
  const auto installStatus = installer_.progress().status;
  if (observedInstall_ == install::Status::Running && installStatus == install::Status::Succeeded)
    refreshInstalled();
  // 다운로드 keeps how it ended.
  if (observedInstall_ == install::Status::Running && installStatus != install::Status::Running &&
      installStatus != install::Status::Idle) {
    const auto outcome = installStatus == install::Status::Succeeded ? history::Outcome::Done
                         : installStatus == install::Status::Cancelled ? history::Outcome::Cancelled
                                                                       : history::Outcome::Failed;
    if (history::finish(history_, outcome, installer_.progress().error, int64_t(std::time(nullptr)))) saveHistory();
  }
  // Patches picked with the game wait for its install: they start once it
  // succeeds and are dropped when it does not.
  if (patchQueueWaits_ && observedInstall_ == install::Status::Running && installStatus != install::Status::Running) {
    patchQueueWaits_ = false;
    if (installStatus != install::Status::Succeeded && !patchQueue_.empty()) {
      patchQueue_.clear();
      message(tr("게임을 설치하지 못해 함께 고른 패치를 받지 않았습니다."));
    }
  }
  pumpPatchQueue();
  // Done or cancelled on purpose: nothing left to resume. A failure keeps it.
  if (observedInstall_ == install::Status::Running &&
      (installStatus == install::Status::Succeeded || installStatus == install::Status::Cancelled))
    forgetInstall();
  observedInstall_ = installStatus;
  if (!done_.exchange(false)) {
    // Waiting for approval: asked every 5 seconds for 5 minutes, then - asks again.
    constexpr uint32_t PairWaitMs = 5 * 60 * 1000;
    if (!network_.joinable() && pairingPending_ && SDL_GetTicks() - pairSince_ < PairWaitMs &&
        SDL_GetTicks() - pairPollAt_ >= 5000) {
      pairPollAt_ = 0;
      refresh();
    }
    // Not connected: a few more tries, then it waits for - (새로고침).
    // Any error counts (a name that did not resolve is often the network
    // still joining); the console's own online check never stops a try,
    // since it can read offline on a network that works.
    constexpr uint32_t RetryMs = 5000;
    constexpr int MaxRetries = 3;
    if (!network_.joinable() && !connected_ && session_ && error_ && !pairingPending_ && retries_ < MaxRetries &&
        SDL_GetTicks() - offlineCheckAt_ >= RetryMs) {
      offlineCheckAt_ = SDL_GetTicks();
      ++retries_;
      refresh();
    }
    return;
  }
  network_.join();
  if (detailRequest_) {
    detailLoading_ = false;
    if (!incomingError_) {
      details_[session_->server().url + requestedId_] = {incomingDetail_,
                                                         SDL_GetTicks()};
      if (screen_ == Screen::Detail && detailGroup_ < groups_.size() &&
          groups_[detailGroup_].id == requestedId_)
      {
        detail_ = std::move(incomingDetail_);
        syncDetailPatches();
      }
    } else
      message(incomingError_.message);
    detailRequest_ = false;
    if (screen_ == Screen::Detail && detailGroup_ < groups_.size() &&
        groups_[detailGroup_].id != requestedId_)
      fetchDetail();
    return;
  }
  pairingPending_ = session_->pairingPending();
  pairingInfo_ = session_->pairingInfo();
  qr_.encode(pairingInfo_.url);
  pairPollAt_ = pairingPending_ ? SDL_GetTicks() : 0;
  if (!pairingPending_) pairSince_ = 0;
  else if (!pairSince_) pairSince_ = SDL_GetTicks();
  if (incomingError_) {
    error_ = incomingError_;
    refreshing_ = false;
    if (!pairingPending_)
      message(error_.message);
    return;
  }
  if (refreshing_) {
    refreshing_ = false;
    message(tr("목록을 다시 불러왔습니다."));
  }
  {
    // The session's copy is older for what the app owns (queue, language,
    // install checks); it only brings the keys.
    auto mine = state_;
    state_ = session_->state();
    state_.queueJson = mine.queueJson;
    state_.language = mine.language;
    state_.skipSignatureCheck = mine.skipSignatureCheck;
    state_.skipFirmwareCheck = mine.skipFirmwareCheck;
    state_.reportErrors = mine.reportErrors;
    state_.makeShortcuts = mine.makeShortcuts;
    state_.installTarget = mine.installTarget;
    state_.pairMessages = mine.pairMessages;
  }
  features_ = session_->features();
  // Same server: keep decoded covers; only the access keys may have changed.
  images_->updateState(state_);
  if (screen_ == Screen::Detail)
    screen_ = previous_;
  // A 304 keeps the catalog already on screen.
  retries_ = 0;
  if (session_->unchanged() && !groups_.empty()) {
    connected_ = true;
    error_ = {};
    return;
  }
  featured_ = session_->featured();
  genreNames_ = session_->genres();
  scoreLabels_ = session_->scoreLabels();
  setItems(std::move(incoming_));
  incoming_ = {};
  error_ = {};
  offerResume();
}
const std::vector<size_t> &Application::libraryList() const {
  std::string key = std::to_string(catalogVersion_) + "|" + std::to_string(int(librarySort_)) + "|";
  for (const auto i : activeChips_)
    key += std::to_string(i) + ",";
  key += "|" + query_;
  if (key == listKey_)
    return listCache_;
  std::vector<Theme> filters;
  for (const auto i : activeChips_)
    if (i > 0 && i < int(chips_.size()))
      filters.push_back(chips_[i]);
  listCache_ = browseFiltered(groups_, query_, filters, librarySort_, installedMask_);
  listKey_ = key;
  return listCache_;
}
// Home rows and the "not installed" filter both depend on what the console
// has, so they are rebuilt when either the catalog or the installed list moves.
void Application::rebuildHome() {
  installedMask_.assign(groups_.size(), false);
  if (installed_.installed.complete)
    for (const auto &t : installed_.installed.titles)
      if (const auto found = groupIndex_.find(t.titleId); found != groupIndex_.end())
        installedMask_[found->second] = true;
  // Admin picks (feature: featured) lead the home, mapped onto loaded groups.
  std::vector<HomeTheme> pinned;
  heroBanner_.clear();
  for (const auto &shelf : featured_) {
    HomeTheme row{{shelf.title, SortMode::Default, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}, {}};
    for (const auto &id : shelf.titleIds) {
      const auto found = groupIndex_.find(id);
      if (found == groupIndex_.end()) continue;
      const size_t i = found->second;
      if (groups_[i].base && !installedMask_[i] && row.items.size() < HomeRowLimit)
        row.items.push_back(i);
    }
    if (row.items.empty())
      continue;
    if (pinned.empty() && session_)
      heroBanner_ = shelf.bannerUrl.empty()
                        ? session_->server().url + "/api/shop/banner/" + groups_[row.items.front()].id +
                              (language() == Language::Korean ? "" : std::string("?lang=") + languageCode())
                        : shelf.bannerUrl;
    pinned.push_back(std::move(row));
  }
  heroes_ = homeHeroes(groups_, installedMask_, homeSeed_);
  heroFeatured_ = !pinned.empty();
  if (heroFeatured_) {
    const auto pick = pinned.front().items.front();
    heroes_.erase(std::remove_if(heroes_.begin(), heroes_.end(),
                                 [pick](const HeroSlide &s) { return s.index == pick; }),
                  heroes_.end());
    heroes_.insert(heroes_.begin(), {pick, pinned.front().theme.title});
    pinned.front().items.erase(pinned.front().items.begin());
  }
  if (heroes_.empty() && !groups_.empty())
    heroes_.push_back({homeHero(groups_, installedMask_), tr("추천 게임")});
  heroSlide_ = heroes_.empty() ? 0 : std::min(heroSlide_, int(heroes_.size()) - 1);
  hero_ = heroes_.empty() ? 0 : heroes_[heroSlide_].index;
  heroAt_ = SDL_GetTicks();
  themes_ = homeThemes(groups_, hero_, installedMask_, std::move(pinned), homeSeed_, genreNames_);
  ++catalogVersion_;
}
void Application::showHero(int slide) {
  if (heroes_.empty())
    return;
  const int count = int(heroes_.size());
  heroSlide_ = (slide % count + count) % count;
  hero_ = heroes_[heroSlide_].index;
  heroAt_ = SDL_GetTicks();
}
// The filter popup over 모든 게임 or a retro system's list; it edits a draft.
void Application::openViewPopup() {
  const bool retro = screen_ == Screen::Retro;
  viewPopup_ = true;
  popupColumn_ = 0;
  popupRow_ = 0;
  draftChips_ = retro ? retroFilters_ : activeChips_;
  draftSort_ = retro ? retroSort_ : librarySort_;
}
bool Application::draftOn(int index) const {
  return index == 0 ? draftChips_.empty()
                    : std::find(draftChips_.begin(), draftChips_.end(), index) != draftChips_.end();
}
// Row 0 is the hero; rows 1.. are themes_.
std::vector<size_t> Application::homeRow(int row) const {
  if (row <= 0)
    return groups_.empty() ? std::vector<size_t>{} : std::vector<size_t>{hero_};
  return size_t(row) <= themes_.size() ? themes_[row - 1].items : std::vector<size_t>{};
}
void Application::openTheme(size_t row) {
  if (themes_.empty())
    return;
  const auto &theme = themes_[std::min(row, themes_.size() - 1)].theme;
  screen(Screen::Library);
  query_.clear();
  // Land on the chip that shows the same games, sorted the theme's way.
  const auto same = [&](const Theme &c) {
    return c.filter == theme.filter && c.category == theme.category &&
           c.minPlayers == theme.minPlayers && c.maxPlayers == theme.maxPlayers &&
           c.maxSize == theme.maxSize && c.minSize == theme.minSize &&
           c.notInstalled == theme.notInstalled && c.onSale == theme.onSale &&
           c.minScore == theme.minScore && c.withoutCategory == theme.withoutCategory;
  };
  auto match = std::find_if(chips_.begin() + 1, chips_.end(), [&](const Theme &c) {
    return same(c) && (c.title == theme.title || !(theme.filter == Filter::All && theme.category.empty()));
  });
  // A row with no chip of its own (신작, 인기, 명작...): a chip with its name,
  // so 모든 게임 says which view it is.
  if (match == chips_.end()) {
    chips_.push_back(theme);
    match = chips_.end() - 1;
  }
  activeChips_.clear();
  activeChips_.push_back(int(match - chips_.begin()));
  librarySort_ = theme.sort;
  sidebar_ = !hasFocusable();
}
void Application::search(std::string query) {
  // 세이브, 유저 패치, 설치된 게임 and 업데이트 search their own lists.
  if (auto *own = listQuery()) {
    *own = std::move(query);
    selected_ = shopSaveRow_ = 0;
    sidebar_ = false;
    return;
  }
  // In 레트로 게임 the words search the system's list instead of the shop.
  if (screen_ == Screen::Retro) {
    retroQuery_ = std::move(query);
    if (retroView_ != RetroView::List) {
      retroView_ = RetroView::List;
      retroSystem_ = std::clamp(retroRow_, 0, std::max(0, int(retroSystems_.size()) - 1));
    }
    retroGames_.clear();
    retroListSystem_.clear();
    retroTotal_ = 0;
    selected_ = 0;
    sidebar_ = false;
    if (!retroWorker_.joinable()) loadRetroPage(false);
    return;
  }
  query_ = std::move(query);
  selected_ = 0;
  sidebar_ = false;
  screen_ = Screen::Library;
  nav_ = 1;
}
std::string Application::selectedName() const {
  if (screen_ == Screen::Library) {
    const auto &list = libraryList();
    if (!list.empty())
      return groups_[list[std::clamp(selected_, 0, int(list.size() - 1))]]
          .representative.name;
  }
  if (screen_ == Screen::Home && !groups_.empty()) {
    const auto list = homeRow(row_);
    return list.empty() ? ""
                        : groups_[list[std::clamp(selected_, 0, int(list.size() - 1))]]
                              .representative.name;
  }
  return "";
}
void Application::action(Action a) {
  poll();
  if (cleanupBusy_) // the progress modal is up until it finishes
    return;
  if (galleryOpen_ && screen_ == Screen::Detail) {
    if (a == Action::Back) galleryOpen_ = false;
    else if (!detail_.screenshots.empty()) {
      const auto count = detail_.screenshots.size();
      if (a == Action::Left || a == Action::Previous) screenshot_ = (screenshot_ + count - 1) % count;
      if (a == Action::Right || a == Action::Next) screenshot_ = (screenshot_ + 1) % count;
    }
    return;
  }
  if (alert_.empty() && (appUpdateAsk_ || appUpdate_ == AppUpdate::Downloading)) {
    appUpdateAction(a);
    return;
  }
  if (!alert_.empty()) {
    if (a == Action::Accept || a == Action::Back) {
      alert_.clear();
      alertTitle_.clear();
    }
    return;
  }
  if (selfShortcutAsk_) {
    selfShortcutAction(a);
    return;
  }
  if (serverForm_) {
    serverFormAction(a);
    return;
  }
  // Y on the approval screen writes a note for the admin.
  if (pairingPending_ && screen_ != Screen::Settings && a == Action::Queue) {
    askPairMessage();
    return;
  }
  if (viewPopup_) {
    // Filters sit in a two-column grid, sorts in one column to its right.
    constexpr int Cols = PopupFilterColumns;
    const auto orders = popupSorts();
    const int filters = std::max(1, int(popupFilterLabels().size())), sorts = int(orders.size());
    if (popupColumn_ == 0) {
      if (a == Action::Up && popupRow_ - Cols >= 0) popupRow_ -= Cols;
      if (a == Action::Down && popupRow_ + Cols < filters) popupRow_ += Cols;
      if (a == Action::Left && popupRow_ % Cols > 0) --popupRow_;
      if (a == Action::Right) {
        if (popupRow_ % Cols < Cols - 1 && popupRow_ + 1 < filters) {
          ++popupRow_;
        } else {
          popupColumn_ = 1;
          popupRow_ = std::min(sorts - 1, popupRow_ / Cols);
        }
      }
    } else {
      if (a == Action::Up) popupRow_ = std::max(0, popupRow_ - 1);
      if (a == Action::Down) popupRow_ = std::min(sorts - 1, popupRow_ + 1);
      if (a == Action::Left) {
        popupColumn_ = 0;
        popupRow_ = std::min(filters - 1, popupRow_ * Cols + Cols - 1);
      }
    }
    // Choices edit a draft so the list behind does not jump around; Y
    // applies the draft, B closes and drops it.
    if (a == Action::Accept) {
      if (popupColumn_ == 1) {
        draftSort_ = orders[popupRow_];
      } else if (popupRow_ == 0) {
        draftChips_.clear();
      } else {
        const auto found = std::find(draftChips_.begin(), draftChips_.end(), popupRow_);
        if (found == draftChips_.end()) draftChips_.push_back(popupRow_);
        else draftChips_.erase(found);
      }
    }
    if (a == Action::Queue) {
      viewPopup_ = false;
      selected_ = 0;
      if (screen_ == Screen::Retro) {
        retroFilters_ = draftChips_;
        retroSort_ = draftSort_;
        retroGames_.clear();
        retroListSystem_.clear();
        retroTotal_ = 0;
        if (!retroWorker_.joinable()) loadRetroPage(false);
      } else {
        activeChips_ = draftChips_;
        librarySort_ = draftSort_;
      }
    }
    if (a == Action::Back) viewPopup_ = false;
    return;
  }
  if (confirmInstall_) {
    if (a == Action::Accept && settled()) {
      confirmInstall_ = false;
      installSelected();
    }
    if (a == Action::Back)
      confirmInstall_ = false;
    return;
  }
  // The header's buttons: the d-pad moves along them, A presses one.
  if (!headerFocus_.empty() && !popupOpen() && headerAction(a)) return;
  if (a == Action::Up && headerFocus_.empty() && !sidebar_ && !popupOpen() && screen_ != Screen::Detail &&
      contentAtTop() && enterHeader())
    return;
  // 내 게임's tabs take L R wherever the cursor is, the menu included.
  if ((screen_ == Screen::Installed || screen_ == Screen::Updates) && !managedGame_ &&
      (a == Action::Previous || a == Action::Next)) {
    showMyGamesTab((myGamesTab() + (a == Action::Next ? 1 : 2)) % 3);
    return;
  }
  if (screen_ == Screen::Installed &&
      (managedGame_ || (!sidebar_ && a != Action::Back))) {
    installedAction(a);
    return;
  }
  if (screen_ == Screen::Saves && (managedGame_ || saveNeedsGame_ || savePicking_ || shopAppSaveAsk_ ||
                                   (savesTab_ == 2 && (dropboxModal() || dropboxSession_)) || (!sidebar_ && a != Action::Back) || a == Action::Queue ||
                                   a == Action::Previous || a == Action::Next)) {
    savesAction(a);
    return;
  }
  if (screen_ == Screen::Retro && (retroAsking_ || retroArchAsking_ || (!sidebar_ && (a != Action::Back || retroView_ == RetroView::List)) || a == Action::Queue ||
                                   a == Action::Previous || a == Action::Next)) {
    retroAction(a);
    return;
  }
  if (confirmResume_) {
    if (a == Action::Back)
      confirmResume_ = false; // asked again next launch
    else if (a == Action::Accept) {
      confirmResume_ = false;
      resumeInstall();
    } else if (a == Action::Queue && settled()) {
      confirmResume_ = false;
      forgetInstall();
    }
    return;
  }
  if (languagePopup_) {
    if (a == Action::Up) languageRow_ = std::max(0, languageRow_ - 1);
    if (a == Action::Down) languageRow_ = std::min(LanguageCount - 1, languageRow_ + 1);
    if (a == Action::Accept) {
      languagePopup_ = false;
      if (Language(languageRow_) != language()) chooseLanguage(Language(languageRow_));
    }
    if (a == Action::Back) languagePopup_ = false;
    return;
  }
  if (confirmCleanup_) {
    if (a == Action::Back)
      confirmCleanup_ = false;
    else if (a == Action::Accept && settled()) {
      confirmCleanup_ = false;
      startCleanup(true);
    }
    return;
  }
  if (homebrewRemoveAsk_ || homebrewAsk_ || shortcutRemoveAsk_ ||
      (screen_ == Screen::Homebrew && (a == Action::Previous || a == Action::Next || a == Action::Search ||
                                       (!sidebar_ && a != Action::Back)))) {
    homebrewAction(a);
    return;
  }
  if (confirmPatchRemove_ || managingPatch_ || pickingPatch_ || (screen_ == Screen::Patches && (a == Action::Queue || a == Action::Previous || a == Action::Next || a == Action::Search)) ||
      (screen_ == Screen::Patches && !sidebar_ && a != Action::Back)) {
    patchAction(a);
    return;
  }
  if (confirmCancel_) {
    // B keeps installing; A cancels, but not one pressed as the popup opened.
    if (a == Action::Accept && settled()) {
      if (cancelRetro_) retroCancel_ = true;
      else {
        installer_.cancel();
        exitAfterCancel_ = confirmExit_;
      }
      confirmCancel_ = false;
    }
    if (a == Action::Back)
      confirmCancel_ = false;
    return;
  }
  // B on 진행 중 stops what runs: the install first, else a retro download.
  if (screen_ == Screen::Downloads && a == Action::Back && downloadsTab_ == 0 &&
      (installer_.running() || retroDownloading())) {
    cancelRetro_ = !installer_.running();
    confirmCancel_ = true;
    confirmExit_ = false;
    dialogFocus_ = 0;
    return;
  }
  if (screen_ == Screen::Downloads &&
      (a == Action::Previous || a == Action::Next ||
       (!sidebar_ && downloadsTab_ != 0 &&
        (a == Action::Up || a == Action::Down || a == Action::Accept || a == Action::Search || a == Action::Queue)))) {
    downloadsAction(a);
    return;
  }
  if (a == Action::Reload) {
    retries_ = patchRetries_ = retroRetries_ = 0; // a refresh by hand earns the automatic tries again
    pairSince_ = 0;
    reload();
    return;
  }
  if (a == Action::Back) {
    // In a searched list, B clears the search before leaving.
    if (screen_ == Screen::Library && !query_.empty() && !sidebar_) {
      query_.clear();
      selected_ = 0;
      return;
    }
    if (auto *own = listQuery(); own && !own->empty() && !sidebar_) {
      own->clear();
      selected_ = shopSaveRow_ = 0;
      return;
    }
    if (screen_ == Screen::Detail) {
      screen_ = previous_;
      return;
    }
    if (sidebar_) {
      screen(Screen::Home);
      return;
    }
    sidebar_ = true;
    return;
  }
  if (sidebar_) {
    // Closed items are stepped over, so before approval Up and Down move
    // between 홈 and 설정 only.
    if (a == Action::Up || a == Action::Down) {
      int n = nav_;
      do n += a == Action::Up ? -1 : 1;
      while (n > 0 && n < MenuItems - 1 && !menuOpen(n));
      nav_ = std::clamp(n, 0, MenuItems - 1);
    }
    if (a == Action::Up || a == Action::Down) {
      autoSidebar_ = false; // the user is choosing; do not pull focus away
      // The screen under the cursor shows once it rests (render), so
      // holding the stick through the menu reads nothing on the way.
      menuPending_ = menuOpen(nav_) && MenuScreens[nav_] != screen_;
      menuAt_ = SDL_GetTicks();
    }
    if ((a == Action::Accept || a == Action::Right) && menuOpen(nav_)) {
      menuPending_ = false;
      const auto target = MenuScreens[nav_];
      // Into the screen already shown: the cursor moves in, nothing reloads
      // (설치된 게임 would read the console again).
      if (target != screen_) openMenuScreen(target);
      // Its content first; with nothing there yet, its header buttons.
      sidebar_ = autoSidebar_ = !hasFocusable();
      if (sidebar_ && enterHeader()) autoSidebar_ = false;
    }
    return;
  }
  if (screen_ == Screen::Home) {
    if (a == Action::Next) {
      openTheme(row_ > 0 ? size_t(row_ - 1) : 0);
      return;
    }
    // Vertical moves keep the column, like a store grid.
    if (a == Action::Up || a == Action::Down) {
      const int column = row_ == 0 ? 0 : selected_;
      row_ = std::clamp(row_ + (a == Action::Down ? 1 : -1), 0, int(themes_.size()));
      const auto target = homeRow(row_);
      // Shelves end in a 모두 보기 card, one past their last game.
      selected_ = row_ == 0 || target.empty()
                      ? 0
                      : std::min(column, int(target.size()));
    }
    const auto list = homeRow(row_);
    // On the banner, left and right flip through its slides.
    if (row_ == 0 && heroes_.size() > 1 && (a == Action::Right || (a == Action::Left && heroSlide_ > 0))) {
      showHero(heroSlide_ + (a == Action::Right ? 1 : -1));
      return;
    }
    if (a == Action::Left) {
      if (selected_ == 0)
        sidebar_ = true;
      else
        --selected_;
    }
    if (a == Action::Right && !list.empty())
      selected_ = std::min(int(list.size()) - (row_ == 0 ? 1 : 0), selected_ + 1);
    if (a == Action::Accept && row_ > 0 && selected_ >= int(list.size())) {
      openTheme(size_t(row_ - 1));
      return;
    }
    if (a == Action::Accept && !list.empty())
      openDetail(list[std::clamp(selected_, 0, int(list.size() - 1))]);
  } else if (screen_ == Screen::Library) {
    // L/R step through sort orders; filters are picked in the popup (Y).
    if (a == Action::Previous || a == Action::Next) {
      constexpr int Orders = int(std::size(SortOrders));
      int at = 0;
      for (int n = 0; n < Orders; ++n)
        if (SortOrders[n] == librarySort_) at = n;
      librarySort_ = SortOrders[(at + (a == Action::Next ? 1 : Orders - 1)) % Orders];
      selected_ = 0;
    }
    if (a == Action::Queue) {
      openViewPopup();
      return;
    }
    const auto list = libraryList();
    const int last = int(list.size()) - 1;
    // Up from the top row goes to the search box and filter (the header).
    if (a == Action::Up)
      selected_ = std::max(0, selected_ - 6);
    if (a == Action::Down)
      selected_ = std::max(0, std::min(last, selected_ + 6));
    if (a == Action::Left) {
      if (selected_ % 6 == 0)
        sidebar_ = true;
      else
        --selected_;
    }
    if (a == Action::Right)
      selected_ = std::max(0, std::min(last, selected_ + 1));
    if (a == Action::Accept && !list.empty() && selected_ >= 0)
      openDetail(list[selected_]);
  } else if (screen_ == Screen::Detail && selection_) {
    if (a == Action::Preview && !detail_.screenshots.empty()) {
      galleryOpen_ = true;
      screenshot_ = 0;
      return;
    }
    const int last = int(selection_->rows());
    // Left moves onto the description, where up and down scroll it; right
    // comes back to what gets installed. The right stick is the same
    // scroll, so it moves the cursor onto the description too.
    if ((a == Action::ScrollUp || a == Action::ScrollDown) && descriptionHeight_ > 0) descriptionFocus_ = true;
    const auto toLeftColumn = [&] { descriptionFocus_ = descriptionHeight_ > 0; };
    if (descriptionFocus_) {
      if (a == Action::Up || a == Action::ScrollUp)
        descriptionScroll_ = std::max(0, descriptionScroll_ - 1);
      if (a == Action::Down || a == Action::ScrollDown)
        descriptionScroll_ = std::min((descriptionHeight_ + ScrollStep - 1) / ScrollStep,
                                      descriptionScroll_ + 1);
      if (a == Action::Right || a == Action::Accept)
        descriptionFocus_ = false;
      return;
    }
    // Only a description that scrolls takes the cursor.
    // Rows: each item, the storage cards (index last), the install button.
    const int storage = last, button = last + 1;
    // On the storage row left and right only move the cursor between the
    // two cards; A picks the card under it. Left past the SD card goes on to
    // the description, like from any other row.
    if (part_ == storage && (a == Action::Left || a == Action::Right)) {
      if (a == Action::Right)
        storageCursor_ = 1;
      else if (storageCursor_ == 1)
        storageCursor_ = 0;
      else
        toLeftColumn();
      return;
    }
    if (a == Action::Left) {
      toLeftColumn();
      return;
    }
    const int before = part_;
    if (a == Action::Up)
      part_ = std::max(0, part_ - 1);
    if (a == Action::Down)
      part_ = std::min(button, part_ + 1);
    // Arriving on the row, the cursor starts on the card already chosen.
    if (part_ == storage && before != storage)
      storageCursor_ = selection_->target == "nand" ? 1 : 0;
    if (a == Action::Accept && part_ == storage) {
      selection_->target = storageCursor_ ? "nand" : "sd";
      return;
    }
    // A acts on what the cursor is on: an item toggles, the button installs.
    if ((a == Action::Search || a == Action::Accept) && part_ < last)
      selection_->toggle(part_);
    else if (a == Action::Accept && part_ == button)
      requestInstall();
    if (a == Action::ScrollUp)
      descriptionScroll_ = std::max(0, descriptionScroll_ - 1);
    if (a == Action::ScrollDown)
      descriptionScroll_ = std::min((descriptionHeight_ + ScrollStep - 1) / ScrollStep,
                                    descriptionScroll_ + 1);
  } else if (screen_ == Screen::Updates) {
    const auto rows = updateRows();
    if (a == Action::Search) requestSearch();
    if (a == Action::Up) selected_ = std::max(0, selected_ - 1);
    if (a == Action::Down) selected_ = std::min(int(rows.size()) - 1, selected_ + 1);
    selected_ = std::max(0, selected_);
    if (a == Action::Accept) installUpdates(false);
    if (a == Action::Queue && installed_.installed.complete) installUpdates(true);
    if (a == Action::Left) sidebar_ = true;
  } else if (screen_ == Screen::Settings) {
    if (a == Action::Up)
      selected_ = std::max(0, selected_ - 1);
    // Rows: each server, "+ 서버 추가", then the options card (Settings.cpp).
    if (a == Action::Down)
      selected_ = std::min(int(config_.servers.size()) + SettingsOptions, selected_ + 1);
    if (a == Action::Accept) {
      const int option = selected_ - int(config_.servers.size()) - 1;
      switch (option) {
      case 0:
        languagePopup_ = true;
        languageRow_ = int(language());
        break;
      case 1:
        state_.installTarget = installTarget() == "nand" ? "sd" : "nand";
        saveOwnState();
        break;
      case 2: appUpdateAction(a); break;
      case 3: startCleanup(false); break;
      // The switches, together.
      case 4: toggleInstallCheck(state_.skipSignatureCheck); break;
      case 5: toggleInstallCheck(state_.skipFirmwareCheck); break;
      case 6: toggleInstallCheck(state_.makeShortcuts); break;
      case 7: toggleInstallCheck(state_.reportErrors); break;
      default:
        // A server opens its form; "+ 서버 추가" an empty one.
        openServerForm(selected_ < int(config_.servers.size()) ? std::max(0, selected_) : -1);
      }
    }
    if (a == Action::Left)
      sidebar_ = true;
  } else if (screen_ != Screen::Detail && a == Action::Left)
    sidebar_ = true;
}
void Application::cover(const std::string &url, float x, float y, int size,
                        bool selected, const std::string &name, bool exact, bool dark) {
  if (selected)
    paint_.ring(x, y, size, size);
  // Covers never exceed 184pt, so the 256px server thumbnail is enough.
  const auto thumb = exact ? url : thumbnailUrl(url);
  if (!thumb.empty() && !images_->failed(thumb) && !images_->loaded(thumb)) {
    paint_.skeleton(x, y, size, size, 14, dark);
  } else if (thumb.empty() || images_->failed(thumb)) {
    // No art: a tinted tile with the title's first letter.
    static const SDL_Color tints[] = {{226, 221, 207, 255}, {214, 223, 216, 255},
                                      {228, 216, 208, 255}, {216, 220, 228, 255}};
    size_t seed = 0;
    for (const unsigned char c : name) seed = seed * 31 + c;
    paint_.rect(x, y, size, size, tints[seed % 4], 14);
    // The first real letter: "(K) 골든 엑스" and "[J] Game" start after the tag.
    const auto letters = utf8Characters(name);
    std::string initial = "?";
    int depth = 0;
    for (const auto &c : letters) {
      if (c == "(" || c == "[") { ++depth; continue; }
      if (c == ")" || c == "]") { depth = std::max(0, depth - 1); continue; }
      if (depth || (c.size() == 1 && !std::isalnum(static_cast<unsigned char>(c[0])))) continue;
      initial = c;
      break;
    }
    const int glyph = size * 2 / 5;
    paint_.fonts.middle(initial, x + (size - paint_.fonts.measure(initial, glyph, true)) / 2,
                        y + size / 2.f, glyph, Secondary, true);
    return;
  } else
    paint_.rect(x, y, size, size, Line, 14);
  images_->draw(thumb, x, y, size, size, 14, false, exact);
}
// A score in a small badge ending at `right`, tinted by band like the web
// shop: five bands so 98 and 78 do not look alike (deep green, green,
// olive, amber, red). Nothing for 0, a game without a score.
void Application::scoreBadge(int score, float right, float y) {
  if (score <= 0) return;
  const auto label = std::to_string(score);
  const float w = paint_.fonts.measure(label, 13, true) + 10, h = 19;
  const SDL_Color band = score >= 90   ? SDL_Color{13, 110, 64, 255}
                         : score >= 80 ? SDL_Color{34, 150, 90, 255}
                         : score >= 70 ? SDL_Color{118, 140, 28, 255}
                         : score >= 50 ? SDL_Color{196, 124, 0, 255}
                                       : SDL_Color{204, 62, 52, 255};
  paint_.rect(right - w, y, w, h, band, 5);
  paint_.fonts.middle(label, right - w + 5, y + h / 2, 13, Surface, true);
}
void Application::card(const Group &g, float x, float y, bool selected,
                       bool badges) {
  const auto index = size_t(&g - groups_.data());
  if (index < groups_.size())
    hit(x, y, 150, 220, [this, index] { openDetail(index); });
  cover(g.representative.icon, x, y, 150, selected, g.representative.name);
  // Download size on the top left, in the same small badge as the score.
  if (const auto bytes = g.base ? g.base->size : g.representative.size; bytes > 0) {
    const auto label = formatBytes(bytes);
    const float w = paint_.fonts.measure(label, 13, true) + 10, h = 19;
    paint_.rect(x + 6, y + 6, w, h, {22, 22, 26, 190}, 5);
    paint_.fonts.middle(label, x + 11, y + 6 + h / 2, 13, Surface, true);
  }
  // Review score on the poster's top right.
  scoreBadge(g.representative.score, x + 150 - 6, y + 6);
  if (badges) {
    if (selected) marquee(g.representative.name, x, y + 158, 18, 150, false);
    else paint_.fonts.text(g.representative.name, x, y + 158, 18, Ink, false, 150);
    // Badges in priority order; one that would spill past the card is left out.
    std::vector<std::string> labels;
    if (g.base && !g.base->priceDiscount.empty())
      labels.push_back(tr("eShop 할인"));
    if (g.update)
      labels.push_back(tr("업데이트|항목"));
    if (!g.dlc.empty())
      labels.push_back("DLC " + std::to_string(g.dlc.size()));
    float at = x;
    for (const auto &label : labels) {
      if (at + paint_.fonts.measure(label, 15, true) + 18 > x + 150)
        break;
      at += paint_.pill(label, at, y + 194) + 6;
    }
  } else if (selected)
    marquee(g.representative.name, x, y + 158, 18, 150, false);
  else
    paint_.fonts.text(g.representative.name, x, y + 158, 18, Ink, false, 150);
}
void Application::footer(
    const std::vector<std::pair<std::string, std::string>> &screenHints) {
  paint_.rect(0, 660, 1280, 60, Surface);
  paint_.rect(0, 660, 1280, 1, Line);
  const float left = statusBar();
  // - and + work on every screen; their hints show when there is room.
  auto hints = screenHints;
  hints.erase(std::remove_if(hints.begin(), hints.end(), [](const auto &h) { return h.first == "+" || h.first == "-"; }),
              hints.end());
  const auto width = [this](const std::vector<std::pair<std::string, std::string>> &list) {
    float w = 0;
    for (const auto &[key, label] : list)
      w += paint_.fonts.measure(label, 19) + paint_.keyWidth(key) + 40;
    return w;
  };
  auto all = hints;
  all.push_back({"-", tr("새로고침")});
  all.push_back({"+", tr("종료")});
  if (1248 - width(all) > left) hints = std::move(all);
  float x = 1248;
  for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
    const auto label = paint_.fonts.measure(it->second, 19), key = paint_.keyWidth(it->first);
    x -= label;
    paint_.fonts.middle(it->second, x, 691, 19, Ink);
    x -= key + 10;
    hit(x - 8, 660, key + label + 26, 60, [this, k = it->first] { hintAction(k); });
    paint_.key(it->first, x, 691);
    x -= 30;
  }
}
void Application::shell() {
  paint_.rect(0, 0, 232, 660, Surface);
  paint_.rect(231, 0, 1, 660, Line);
  paint_.logo(22, 24);
  // Search lives in "모든 게임"'s search bar; a separate menu item opened the
  // same list.
  // Translated on every draw, so a language change shows at once.
  const char *labels[MenuItems];
  for (int i = 0; i < MenuItems; ++i) labels[i] = screenLabel(MenuScreens[i]);
  // Looked up by screen, so reordering the menu keeps each icon with its item.
  const auto iconOf = [](Screen screen) {
    switch (screen) {
    case Screen::Home: return 0;
    case Screen::Library: return 2;
    case Screen::Retro: return 35;
    case Screen::Installed: return 6;
    case Screen::Saves: return 36;
    case Screen::Patches: return 32;
    case Screen::Homebrew: return 22;
    case Screen::Downloads: return 4;
    default: return 5;
    }
  };
  // A screen left open when approval began reads as 홈 in the menu.
  if (!menuOpen(nav_)) nav_ = 0;
  for (int i = 0; i < MenuItems; ++i) {
    if (!menuShown(i)) continue;
    const int y = menuRowTop(i);
    const int height = MenuRowHeight;
    const bool active = i == nav_, open = menuOpen(i);
    if (open)
      hit(16, y, 200, height, [this, i] {
        sidebar_ = true;
        nav_ = i;
        action(Action::Accept);
      });
    if (sidebar_ && active)
      paint_.ring(16, y, 200, height, 12);
    if (active)
      paint_.rect(16, y, 200, height, Ink, 12);
    const SDL_Color ink = active ? Surface : open ? Ink : SDL_Color{184, 182, 176, 255};
    paint_.icon(iconOf(MenuScreens[i]), 32, y + (height - 26) / 2, ink);
    // Longer languages step the size down to fit the menu instead of
    // running past it.
    constexpr float LabelRoom = 136;
    int labelSize = 21;
    while (labelSize > 16 && paint_.fonts.measure(labels[i], labelSize, active) > LabelRoom)
      --labelSize;
    paint_.fonts.middle(labels[i], 72, y + height / 2.f, labelSize, ink, active, LabelRoom);
    // 내 게임 counts its updates, so they are not missed without a menu row.
    const bool updates = MenuScreens[i] == Screen::Installed, downloads = MenuScreens[i] == Screen::Downloads;
    // A newer CookingFoil on the server: the same badge on 설정, where it is.
    if (open && MenuScreens[i] == Screen::Settings && updateNewer_) {
      paint_.rect(171, y + height / 2 - 14, 28, 28, Butter, 14);
      paint_.fonts.text("1", 185 - paint_.fonts.measure("1", 16, true) / 2, paint_.fonts.centerTop("1", 16, true, y + height / 2),
                        16, Ink, true);
    }
    if (open && (updates || downloads)) {
      const auto progress = installer_.progress();
      const auto count = updates ? pendingUpdates().size()
          : progress.waiting.size() + (installer_.running() ? 1 : 0) + (retroDownloading() ? 1 : 0);
      if (count) {
        paint_.rect(171, y + height / 2 - 14, 28, 28, Butter, 14);
        const auto text = count > 99 ? "99+" : std::to_string(count);
        paint_.fonts.text(text, 185 - paint_.fonts.measure(text, 16, true) / 2,
                          paint_.fonts.centerTop(text, 16, true, y + height / 2), 16, Ink, true);
      }
    }
  }
}
// Shop connection (its state only) and free space, compact on the
// footer's left.
// Returns where it ends so the key hints can stay clear of it.
float Application::statusBar() {
  const bool offline = error_.message == net::offlineMessage();
  // The Korean status is a key: statusColor() reads it, tr() draws it.
  const char *status = connected_                         ? "연결됨" // i18n: keep
                       : pairingPending_                  ? "승인 대기" // i18n: keep
                       : session_ && network_.joinable()  ? "연결 중" // i18n: keep
                       : offline                          ? "인터넷 없음" // i18n: keep
                       : session_ && error_               ? "연결 실패" // i18n: keep
                       : session_                         ? "연결 중" // i18n: keep
                                                          : "서버 없음"; // i18n: keep
  constexpr float Mid = 690;
  float x = 28;
  // First the console's own link, read once a second: a cut Wi-Fi shows
  // at a glance, before any server error does. Then the state in its color.
  if (SDL_GetTicks() - signalAt_ >= 1000 || !signalAt_) {
    signalAt_ = SDL_GetTicks();
    signal_ = net::signalStrength();
  }
  // A server that answers outranks the console's own check, which can read
  // offline on a working network.
  const bool cut = !connected_ && (signal_ < 0 || offline);
  paint_.icon(cut ? 20 : 39, x, Mid - 10, cut ? Bad : signal_ >= 0 && signal_ <= 1 ? Pending : Ink, 20);
  x += 30;
  x = paint_.fonts.middle(tr(status), x, Mid, 17, statusColor(status), true) + 30;
  const struct {
    int icon;
    std::optional<uint64_t> free, total;
  } disks[] = {{26, sdFree_, sdTotal_}, {27, nandFree_, nandTotal_}};
  for (const auto &d : disks) {
    paint_.icon(d.icon, x, Mid - 8, Secondary, 16);
    x += 24;
    constexpr float Bar = 56;
    paint_.rect(x, Mid - 3, Bar, 6, Line, 3);
    if (d.free && d.total && *d.total > 0) {
      const float used = 1.f - float(std::min(*d.free, *d.total)) / float(*d.total);
      paint_.rect(x, Mid - 3, std::max(6.f, Bar * used), 6, Butter, 3);
    }
    x += Bar + 8;
    const auto room = d.free ? trf("{0} 남음", {formatBytes(*d.free)}) : std::string(tr("확인 불가"));
    x = paint_.fonts.middle(room, x, Mid, 15, Secondary) + 26;
  }
#ifdef __SWITCH__
  if (!platform::applicationMode()) {
    const auto mode = tr("애플릿 모드");
    const float w = paint_.fonts.measure(mode, 15, true) + 18;
    paint_.rect(x - 8, Mid - 13, w, 26, {255, 236, 196, 255}, 8);
    paint_.fonts.middle(mode, x + 1, Mid, 15, Pending, true);
    x += w + 10;
  }
#endif
  return x;
}
void Application::render() {
  poll();
  hits_.clear();
  header_.clear();
  // Asked once a launch, as soon as a server answers; 설정's badge shows it.
  if (connected_ && appUpdate_ == AppUpdate::Unknown && !updateWorker_.joinable())
    checkAppUpdate();
  headerHome_.clear();
  // The menu's cursor has rested on another screen: show it, the cursor
  // staying on the menu.
  constexpr uint32_t MenuRestMs = 250;
  if (menuPending_ && SDL_GetTicks() - menuAt_ >= MenuRestMs) {
    menuPending_ = false;
    if (sidebar_ && menuOpen(nav_) && MenuScreens[nav_] != screen_) {
      openMenuScreen(MenuScreens[nav_]);
      sidebar_ = true;
      autoSidebar_ = false;
    }
  }
  // With nothing to select the cursor waits on the menu; once the screen
  // has something (a list finished loading) it moves in by itself.
  if (!sidebar_ && !hasFocusable() && !managedGame_ && headerFocus_.empty()) {
    // A search that found nothing keeps the cursor on its box, so B clears it.
    if (const auto *own = listQuery(); own && !own->empty()) headerFocus_ = "search";
    else sidebar_ = autoSidebar_ = true;
  }
  else if (sidebar_ && autoSidebar_ && screen_ != Screen::Detail && hasFocusable()) {
    sidebar_ = autoSidebar_ = false;
  }
#ifdef __SWITCH__
  if (SDL_GetTicks() - storageAt_ > 5000 || !storageAt_) {
    sdFree_ = platform::freeSpace("sd");
    nandFree_ = platform::freeSpace("nand");
    if (!sdTotal_)
      sdTotal_ = platform::totalSpace("sd");
    if (!nandTotal_)
      nandTotal_ = platform::totalSpace("nand");
    storageAt_ = SDL_GetTicks();
  }
#endif
  // After the first query (or on a PC, where there is nothing to ask) a
  // missing number means unknown, not "still loading".
  storageChecked_ = true;
  images_->decode();
  paint_.rect(0, 0, 1280, 720, Ground);
  if (screen_ != Screen::Detail)
    shell();
  SDL_Rect clip{screen_ == Screen::Detail ? 0 : 232, 0,
                screen_ == Screen::Detail ? 1280 : 1048, 660};
  SDL_RenderSetClipRect(paint_.renderer, &clip);
  if (pairingPending_ && screen_ != Screen::Settings)
    pairing();
  else if (screen_ == Screen::Home)
    home();
  else if (screen_ == Screen::Library)
    library();
  else if (screen_ == Screen::Detail)
    detail();
  else if (screen_ == Screen::Downloads)
    downloads();
  else if (screen_ == Screen::Settings)
    settings();
  else if (screen_ == Screen::Installed)
    installedScreen();
  else if (screen_ == Screen::Updates)
    updates();
  else if (screen_ == Screen::Patches)
    patchesScreen();
  else if (screen_ == Screen::Homebrew)
    homebrewScreen();
  else if (screen_ == Screen::Retro)
    retroScreen();
  else if (screen_ == Screen::Saves)
    savesScreen();
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
  if (pairingPending_ && screen_ != Screen::Settings)
    footer({{"Y", tr("메시지 입력")}, {"B", tr("뒤로")}});
  else if (!headerFocus_.empty())
    footer({{"A", headerFocus_.rfind("tab", 0) == 0 ? tr("다음 탭") : tr("선택")}, {"B", tr("뒤로")}}); // on a header button
  else if (screen_ == Screen::Library)
    footer({{"A", tr("선택")}, {"L R", tr("정렬")}, {"B", query_.empty() ? tr("뒤로") : tr("검색 지우기")}});
  else if (screen_ == Screen::Detail) {
    if (galleryOpen_) footer({{"L R", tr("이전·다음 사진")}, {"B", tr("닫기")}});
    else {
      const int items = selection_ ? int(selection_->rows()) : 0;
      const bool onButton = selection_ && part_ > items, onStorage = selection_ && part_ == items;
      using Hints = std::vector<std::pair<std::string, std::string>>;
      auto hints = descriptionFocus_ ? Hints{{"A", tr("설치 항목으로")}, {"B", tr("뒤로")}}
                                       : Hints{{"A", onButton ? tr("설치") : onStorage ? tr("이 위치로 고르기") : tr("켜고 끄기")},
                                               {"B", tr("뒤로")}};
      if (!detail_.screenshots.empty()) hints.insert(hints.end() - 1, {"ZR", tr("사진")});
      footer(hints);
    }
  }
  else if (screen_ == Screen::Downloads) {
    using Hints = std::vector<std::pair<std::string, std::string>>;
    if (downloadsTab_ == 0 || sidebar_ || downloadsList().empty())
      footer({{"B", (installer_.running() || retroDownloading()) && downloadsTab_ == 0 ? tr("취소") : tr("뒤로")}});
    else if (downloadsTab_ == 2)
      footer(Hints{{"A", tr("다시 시도")}, {"X", tr("목록에서 삭제")}, {"Y", tr("모두 삭제")}, {"B", tr("뒤로")}});
    else
      footer(Hints{{"X", tr("목록에서 삭제")}, {"Y", tr("모두 삭제")}, {"B", tr("뒤로")}});
  }
  else if (screen_ == Screen::Settings)
    footer({{"A", tr("선택")}, {"B", tr("뒤로")}}); // Y still rereads config.json; - shows as 새로고침
  else if (screen_ == Screen::Installed)
    // L R and Y are header buttons on this screen; the footer keeps the rest.
    footer(managedGame_ ? std::vector<std::pair<std::string, std::string>>{{"A", tr("선택")}, {"B", tr("뒤로")}}
           : installed_.games.empty() ? std::vector<std::pair<std::string, std::string>>{{"B", tr("뒤로")}}
                                      : std::vector<std::pair<std::string, std::string>>{{"A", tr("게임 관리")}, {"B", tr("뒤로")}});
  else if (screen_ == Screen::Patches)
    // L R and Y are header buttons, as on 설치된 게임.
    footer(patchRows().empty() || sidebar_
               ? std::vector<std::pair<std::string, std::string>>{{"B", tr("뒤로")}}
               : std::vector<std::pair<std::string, std::string>>{{"A", tr("패치 관리")}, {"B", tr("뒤로")}});
  else if (screen_ == Screen::Homebrew) {
    // A opens the app's popup: what it gets or updates, or for an installed
    // app its facts, patches and saves with 지우기.
    const auto rows = homebrewRows();
    if (rows.empty() || sidebar_)
      footer({{"B", tr("뒤로")}});
    else {
      const auto state = homebrewStatus(homebrewAt(rows[std::clamp(selected_, 0, int(rows.size()) - 1)]));
      footer({{"A", state == homebrew::Status::Installed ? tr("상세 정보")
                    : state == homebrew::Status::Update  ? tr("업데이트|동작")
                                                         : tr("받기")},
              {"B", tr("뒤로")}});
    }
  }
  else if (screen_ == Screen::Retro) {
    using Hints = std::vector<std::pair<std::string, std::string>>;
    if (sidebar_)
      footer({{"B", tr("뒤로")}});
    else if (retroView_ == RetroView::List)
      // X is on the search box; the footer keeps A and B.
      footer(retroGames_.empty() ? Hints{{"B", tr("뒤로")}} : Hints{{"A", tr("상세 정보")}, {"B", tr("뒤로")}});
    else
      footer(retroFocused() ? Hints{{"A", tr("상세 정보")}, {"B", tr("뒤로")}}
                            : Hints{{"A", tr("선택")}, {"B", tr("뒤로")}});
  }
  else if (screen_ == Screen::Saves && savesTab_ == 2 && !managedGame_) {
    using Hints = std::vector<std::pair<std::string, std::string>>;
    if (dropboxSession_) footer({{"B", tr("취소")}});
    else if (sidebar_ || !dropboxFocusable()) footer({{"B", tr("뒤로")}});
    else if (!dropboxLinked_) footer(Hints{{"A", tr("연결하기")}, {"B", tr("뒤로")}});
    else footer(Hints{{"A", tr("백업 관리")}, {"Y", tr("연결 해제")}, {"B", savesQuery_.empty() ? tr("뒤로") : tr("검색 지우기")}});
  }
  else if (screen_ == Screen::Saves)
    footer(managedGame_ ? std::vector<std::pair<std::string, std::string>>{{"A", tr("선택")}, {"B", tr("뒤로")}}
           : sidebar_ || savesTab_ == 2 || (savesTab_ == 1 ? shopSaves_.empty() : installed_.games.empty())
               ? std::vector<std::pair<std::string, std::string>>{{"B", tr("뒤로")}}
               : std::vector<std::pair<std::string, std::string>>{{"A", savesTab_ != 1 ? tr("세이브 관리") : tr("세이브 고르기")},
                                                                  {"B", tr("뒤로")}});
  else if (screen_ == Screen::Updates) {
    // Only offer what can happen now.
    // Y is the header button; the footer keeps A and B.
    if (!updateRows().empty())
      footer({{"A", updatesTab_ == 2 ? tr("이 DLC만 설치") : tr("이 게임만 업데이트")}, {"B", tr("뒤로")}});
    else
      footer({{"B", tr("뒤로")}});
  }
  else
    footer({{"A", tr("선택")}, {"B", tr("뒤로")}});
  if (!notice_.empty() && SDL_GetTicks() - noticeAt_ > 7000)
    notice_.clear();
  if (!notice_.empty()) {
    // A toast that hugs its text above the footer: info mark, then up to
    // two lines, centered on the content area.
    constexpr float MaxText = 860, Icon = 22;
    const float text = std::min(MaxText, paint_.fonts.measure(notice_, 18));
    const bool twoLines = paint_.fonts.measure(notice_, 18) > MaxText;
    const float w = 22 + Icon + 12 + text + 26, h = twoLines ? 72 : 50;
    // Centered on the page: the whole width when the menu is hidden.
    const float x = (screen_ == Screen::Detail ? 640 : 756) - w / 2, y = 646 - h;
    paint_.rect(x, y, w, h, {22, 22, 26, 235}, int(h / 2 < 25 ? h / 2 : 25));
    paint_.icon(33, x + 22, y + (h - Icon) / 2, Butter, int(Icon));
    if (twoLines)
      paint_.fonts.wrapped(notice_, x + 22 + Icon + 12, y + 12, 18, Surface, text + 4, 2);
    else
      paint_.fonts.middle(notice_, x + 22 + Icon + 12, y + h / 2, 18, Surface);
  }
  dialogs();
  SDL_RenderPresent(paint_.renderer);
}

void Application::requestInstall() {
  if (!selection_ || detailGroup_ >= groups_.size())
    return;
  if (!selection_->count()) {
    alert_ = tr("설치할 항목을 하나 이상 켭니다.");
    return;
  }
  confirmInstall_ = true;
  dialogFocus_ = 0;
}
} // namespace foil::ui

namespace foil::ui {
void Application::refreshAll() {
  // Reconnects the shop (and rereads config.json), then the console's
  // games and free space; a screen with its own list reloads with it.
  storageAt_ = 0;
  images_->forgetFailures();
#ifdef __SWITCH__
  if (!deviceBusy_ && !managedGame_) refreshInstalled();
#endif
  if (screen_ == Screen::Patches) patchError_ = {};
  homebrewLoaded_ = false; // 홈브류 asks the server again when it shows
  // 레트로 게임 starts over from its first screen and loads again.
  retroView_ = retroHomeMissing_ ? RetroView::List : RetroView::Home;
  retroShelves_.clear();
  retroGames_.clear();
  retroListSystem_.clear();
  retroQuery_.clear();
  retroFilters_.clear();
  retroSort_ = SortMode::Score;
  retroOnCard_.clear();
  retroRow_ = retroCol_ = 0;
  retroTotal_ = 0;
  retroLoaded_ = false;
  if (screen_ == Screen::Retro) selected_ = 0;
  if (network_.joinable() && !done_) {
    message(tr("서버에 연결하고 있습니다. 끝나면 목록이 바뀝니다."));
    return;
  }
  message(tr("새로고침합니다."));
  refreshing_ = !configPath_.empty();
  if (!configPath_.empty()) reload();
  else if (screen_ == Screen::Patches) fetchPatches();
}
bool Application::requestQuit() {
  // A console query in flight finishes first; the app then closes by itself.
  if (deviceBusy_) {
    exitAfterDevice_ = true;
    message(tr("본체 작업을 마치는 대로 종료합니다."));
    return false;
  }
  if (!installer_.running())
    return true;
  cancelRetro_ = false;
  confirmCancel_ = true;
  confirmExit_ = true;
  dialogFocus_ = 0;
  return false;
}
void Application::installSelected() {
  if (deviceBusy_) {
    alert_ = tr("본체 조회가 끝난 뒤 설치합니다.");
    return;
  }
  if (!selection_ || detailGroup_ >= groups_.size())
    return;
  const auto &g = groups_[detailGroup_];
  // Patches alone (every file off) start right away; with files they wait
  // for the install and never hold it up.
  const auto files = selection_->files();
  if (!files.empty()) {
    if (auto error = startInstall(files, g.representative.name, g.representative.icon, selection_->target)) {
      alert_ = error.message;
      return;
    }
  }
  queuePatches(selection_->patchesOn(), !files.empty());
  notice_.clear();
  downloadsTab_ = 0;
  screen_ = Screen::Downloads;
  nav_ = menuIndex(Screen::Downloads);
}
void Application::saveStateQuietly() {
  if (statePath_.empty())
    return;
  Error error;
  if (!saveState(statePath_, state_, error))
    logLine("state: " + error.message);
}
Error Application::startInstall(const std::vector<Item> &files, const std::string &name, const std::string &icon,
                                const std::string &target) {
  if (auto error = installer_.start(files, name, icon, target, session_ ? session_->server() : Server{}, state_))
    return error;
  rememberInstall(files, name, icon, target);
  history::Entry entry;
  entry.install = {name, icon, target, files};
  entry.at = int64_t(std::time(nullptr));
  history::add(history_, std::move(entry));
  saveHistory();
  observedInstall_ = install::Status::Running;
  return {};
}
void Application::saveHistory() {
  if (sdRoot_.empty()) return;
  std::error_code ec;
  makeDirectories(std::filesystem::path(sdRoot_ + "/switch/cookingfoil").string());
  std::string error;
  if (!atomicWrite(sdRoot_ + "/switch/cookingfoil/downloads.json", history::serialize(history_), error))
    logLine("downloads: " + error);
}
void Application::rememberInstall(const std::vector<Item> &files, const std::string &name,
                                  const std::string &icon, const std::string &target) {
  state_.queueJson = serializePending({name, icon, target, files});
  saveStateQuietly();
}
void Application::forgetInstall() {
  if (state_.queueJson == "[]")
    return;
  state_.queueJson = "[]";
  saveStateQuietly();
}
// Once per launch, when the shop is reachable: an install that was cut off
// (app closed, power, a failure) can pick up again.
void Application::offerResume() {
  if (resumeOffered_ || installer_.running() || !connected_)
    return;
  resumeOffered_ = true;
  if (parsePending(state_.queueJson)) {
    confirmResume_ = true;
    dialogFocus_ = 0;
  }
}
void Application::resumeInstall() {
  const auto pending = parsePending(state_.queueJson);
  if (!pending)
    return;
  if (auto error = startInstall(pending->files, pending->name, pending->icon, pending->target)) {
    alert_ = error.message;
    return;
  }
  downloadsTab_ = 0;
  screen_ = Screen::Downloads;
  nav_ = menuIndex(Screen::Downloads);
  sidebar_ = true;
}
void Application::resumeDialog() {
  const auto pending = parsePending(state_.queueJson);
  if (!pending) {
    confirmResume_ = false;
    return;
  }
  uint64_t bytes = 0;
  for (const auto &f : pending->files)
    bytes += f.size;
  // A picks it up, Y drops it, B asks again next launch.
  const auto body = modal(700, 24 + 90 + 16, tr("끊긴 설치가 있습니다"), tr("닫기"), tr("삭제"), tr("이어서 받기"), 4);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(pending->name, x, y, 22, Ink, true, body.w - 64);
  paint_.fonts.text(trf("{0}개 항목 · {1}", {std::to_string(pending->files.size()), formatBytes(bytes)}), x, y + 36, 18,
                    Secondary, false, body.w - 64);
  paint_.fonts.text(tr("이어서 받으면 이미 받은 부분은 건너뜁니다."), x, y + 64, 18, Secondary, false, body.w - 64);

}
} // namespace foil::ui
namespace foil::ui {
void Application::startCleanup(bool remove) {
  if (cleanupBusy_)
    return;
  if (installer_.running() || deviceBusy_) {
    message(tr("설치나 본체 작업이 끝난 뒤 정리합니다."));
    return;
  }
  cleanupBusy_ = true;
  cleanupRemoving_ = remove;
  cleanupProgress_.stage = cleanupProgress_.done = cleanupProgress_.total = 0;
  cleanupProgress_.listed = false;
  cleanupProgress_.bytes = 0;
  cleanupShown_ = 0;
  if (cleanupWorker_.joinable())
    cleanupWorker_.join();
  cleanupWorker_ = std::thread([this, remove] {
    if (!guarded("cleanup", [this, remove] { cleanupResult_ = platform::cleanOrphans(remove, &cleanupProgress_); }))
      cleanupResult_ = {{}, {tr("정리하다 오류가 났습니다. log.txt를 확인합니다.")}};
    cleanupDone_ = true;
  });
}
void Application::pollCleanup() {
  if (!cleanupDone_.exchange(false))
    return;
  cleanupWorker_.join();
  cleanupBusy_ = false;
  const auto &report = cleanupResult_.value;
  const auto count = std::to_string(report.files + report.placeholders);
  if (cleanupResult_.error) {
    alertTitle_ = tr("남은 파일을 정리하지 못했습니다");
    alert_ = cleanupResult_.error.message;
  } else if (cleanupRemoving_) {
    message(trf("남은 파일 {0}개 · {1}를 삭제했습니다.", {count, formatBytes(report.bytes)}));
    storageAt_ = 0; // free space changed
    refreshInstalled();
  } else if (report.files + report.placeholders == 0) {
    message(tr("정리할 남은 파일이 없습니다."));
  } else {
    confirmCleanup_ = true;
    dialogFocus_ = 1; // a stray A keeps the files
  }
}
void Application::cleanupProgressDialog() {
  const auto body = modal(620, 150, cleanupRemoving_ ? tr("남은 파일을 삭제하고 있습니다") : tr("남은 파일을 찾고 있습니다"), "", "", "", 23);
  const float x = body.x + 32, w = body.w - 64, y = body.y + 26;
  const int stage = cleanupProgress_.stage, done = cleanupProgress_.done, total = cleanupProgress_.total;
  const bool listed = cleanupProgress_.listed;
  const std::string where = stage == 2 ? tr("본체") : tr("SD 카드");
  const std::string step = stage >= 3      ? std::string(tr("마무리하고 있습니다"))
                           : listed && total ? trf("{0} · {1}/{2}개", {where, std::to_string(done), std::to_string(total)})
                                             : trf("{0} 확인 중", {where});
  paint_.fonts.text(step, x, y, 20, Ink, true, w);
  const auto bytes = formatBytes(cleanupProgress_.bytes);
  paint_.fonts.text(bytes, x + w - paint_.fonts.measure(bytes, 20, true), y, 20, Ink, true);
  // One bar that only fills: each storage is half, reading its records the
  // first fifth of that half and its leftovers the rest.
  float target = 1;
  if (stage < 3) {
    const float within = !listed ? 0.2f : total ? 0.2f + 0.8f * float(done) / float(total) : 1.f;
    target = (std::max(stage, 1) - 1 + within) / 2;
  }
  cleanupShown_ = std::max(cleanupShown_, cleanupShown_ + (target - cleanupShown_) * 0.2f);
  paint_.rect(x, y + 48, w, 10, Line, 5);
  paint_.rect(x, y + 48, std::max(10.f, w * cleanupShown_), 10, Butter, 5);
  paint_.fonts.text(cleanupRemoving_ ? tr("끝날 때까지 본체를 끄지 않습니다.") : tr("설치된 게임과 세이브는 건드리지 않습니다."),
                    x, y + 76, 17, Secondary, false, w);
}
void Application::languageDialog() {
  constexpr float Row = 64, Gap = 8;
  const auto body = modal(560, 16 + LanguageCount * (Row + Gap) + 8, "언어 · Language", tr("닫기"), "", "", 17); // i18n: keep (both scripts, readable in any language)
  const float x = body.x + 24, w = body.w - 48;
  for (int i = 0; i < LanguageCount; ++i) {
    const float y = body.y + 16 + i * (Row + Gap);
    hit(x, y, w, Row, [this, i] {
      languageRow_ = i;
      action(Action::Accept);
    });
    if (i == languageRow_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.flag(i, x + 18, y + (Row - 28) / 2, 42, 28);
    const auto name = languageName(Language(i));
    paint_.fonts.middle(name, x + 78, y + Row / 2, 21, Ink, true);
    if (Language(i) == language()) paint_.icon(8, x + w - 44, y + (Row - 26) / 2, Good, 26);
  }
}
void Application::cleanupDialog() {
  const auto &report = cleanupResult_.value;
  const auto body = modal(700, 24 + 90 + 16, tr("남은 파일을 삭제하시겠습니까?"), tr("취소"), "", tr("삭제"), 37);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(trf("{0}개 · {1}", {std::to_string(report.files + report.placeholders), formatBytes(report.bytes)}),
                    x, y, 22, Ink, true, body.w - 64);
  paint_.fonts.wrapped(tr("어떤 게임도 쓰지 않는 파일과 끊긴 설치의 조각을 삭제합니다. 설치된 게임과 세이브는 그대로입니다."),
                       x, y + 38, 18, Secondary, body.w - 64, 2);

}
} // namespace foil::ui
