#include "ui/Application.hpp"
#include "platform/Shortcut.hpp"
#include "net/Socket.hpp"
#include "net/Direct.hpp"
#include "net/Segmented.hpp"
#include "net/Tuner.hpp"
#include "platform/Focus.hpp"
#include "platform/Cores.hpp"
#include "core/Config.hpp"
#include "platform/Storage.hpp"
#include "platform/Installed.hpp"
#include "core/Log.hpp"
#include "core/Json.hpp"
#include "core/Forwarder.hpp"
#include "core/Homebrew.hpp"
#include "core/NczPoint.hpp"
#include "core/TitleGuard.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
namespace foil::ui {
// Server names are the user's own, except the stock ones config files ship
// with, which follow the interface language.
const char *screenLabel(Screen screen) {
  switch (screen) {
  case Screen::Library: return tr("All games");
  case Screen::Installed:
  case Screen::Updates: return tr("My games");
  case Screen::Patches: return tr("Mods");
  case Screen::Homebrew: return tr("Homebrew");
  case Screen::Retro: return tr("Retro games");
  case Screen::Saves: return tr("Saves");
  case Screen::Downloads: return tr("Downloads");
  case Screen::Settings: return tr("Settings");
  default: return tr("Home");
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
      statePath_(std::move(statePath)), fixture_(std::move(fixture)) {
  platform::onLeavingScreen([this] { keepDownloadPlace(); });
}
Application::~Application() {
  platform::stopWatchingScreen();
  stop_ = true;
  retroCancel_ = true;
  updateCancel_ = true;
  // Closing (HOME, power, an update): a download stops where it is and its
  // place is written down, so the next start goes on from it.
  installer_.stop();
  installer_.wait();
  // Finished in its last step as the app closed, or in the frame it closed
  // in, before the screen heard: kept as done, or the next start would take
  // it from the queue and fetch it all again.
  if (observedInstall_ == install::Status::Running && installer_.progress().status == install::Status::Succeeded) {
    logLine("install done as the app closed: " + installer_.progress().name);
    history::finish(history_, history::Outcome::Done, "", int64_t(std::time(nullptr)));
    saveHistory();
    forgetInstall();
  }
  keepResumeChanges();
  // How the downloads stand goes to the card now, before the workers below
  // are waited on: a close the system cuts short must not lose it.
  flushHistory();
  if (network_.joinable())
    network_.join();
  if (deviceWorker_.joinable())
    deviceWorker_.join();
  patchCheckStop_ = true;
  if (patchCheckWorker_.joinable())
    patchCheckWorker_.join();
  if (patchWorker_.joinable())
    patchWorker_.join();
  if (tidyWorker_.joinable())
    tidyWorker_.join();
  if (saveWorker_.joinable())
    saveWorker_.join();
  if (retroWorker_.joinable())
    retroWorker_.join();
  if (retroListWorker_.joinable())
    retroListWorker_.join();
  if (bugWorker_.joinable())
    bugWorker_.join();
  if (updateWorker_.joinable())
    updateWorker_.join();
  if (clockWorker_.joinable())
    clockWorker_.join();
  if (resumeCheckWorker_.joinable())
    resumeCheckWorker_.join();
  if (homebrewWorker_.joinable())
    homebrewWorker_.join();
  if (homebrewCardWorker_.joinable())
    homebrewCardWorker_.join();
  if (homebrewFolderWorker_.joinable())
    homebrewFolderWorker_.join();
  if (shopSavesWorker_.joinable())
    shopSavesWorker_.join();
  if (stateWorker_.joinable())
    stateWorker_.join();
  if (reloadWorker_.joinable())
    reloadWorker_.join();
  if (backupSummaryWorker_.joinable())
    backupSummaryWorker_.join();
  if (dropboxWorker_.joinable())
    dropboxWorker_.join();
  raLinkStop_ = true; // a phone sign-in waiting on the server lets go at once
  raJob_.join();
  raListJob_.join();
  raGamesJob_.join();
  keysJob_.join();
  voteJob_.join();
  historyPageJob_.join(); // reads and clears the store, which closes after
  flushHistory();
  if (storageWorker_.joinable())
    storageWorker_.join();
  if (consoleWorker_.joinable())
    consoleWorker_.join();
  for (const auto &task : std::exchange(consoleTasks_, {})) guarded("console task", task);
  if (awakeWorker_.joinable())
    awakeWorker_.join();
  if (awakeApplied_)
    platform::keepAwake(false);
}
void Application::runOffScreen(std::function<void()> task) {
  consoleTasks_.push_back(std::move(task));
  pumpConsoleTasks();
}
void Application::pumpConsoleTasks() {
  if (consoleWorker_.joinable()) {
    if (!consoleDone_) return; // the next poll starts what came meanwhile
    consoleWorker_.join();
  }
  if (consoleTasks_.empty()) return;
  consoleDone_ = false;
  consoleWorker_ = std::thread([this, tasks = std::exchange(consoleTasks_, {})] {
    const platform::OnCore off(platform::WriterCore);
    for (const auto &task : tasks) guarded("console task", task);
    consoleDone_ = true;
  });
}
void Application::applyAwake() {
  if (awakeWorker_.joinable()) {
    if (!awakeDone_) return; // the next poll sends what is wanted by then
    awakeWorker_.join();
  }
  if (awakeApplied_ == awake_) return;
  awakeApplied_ = awake_;
  awakeDone_ = false;
  awakeWorker_ = std::thread([this, on = awake_] {
    platform::keepAwake(on);
    awakeDone_ = true;
  });
}
// The server restarting: said once per restart, small, and kept up while it
// lasts; taken down as soon as the server answers again.
void Application::serverRestartNotice() {
  const auto heard = net::restartSerial();
  if (heard != restartSeen_) {
    restartSeen_ = heard;
    restartShowing_ = true;
    message(tr("The server is restarting. Downloads go on shortly."));
  }
  // While it restarts the list is asked every few seconds (a 304 when
  // nothing changed): its first plain answer is what says the server is back.
  constexpr uint32_t ProbeMs = 5000;
  const bool restarting = net::serverRestarting();
  if ((refreshAfterRestart_ || (restartShowing_ && restarting)) && session_ && !network_.joinable() &&
      (!restarting || SDL_GetTicks() - restartProbeAt_ >= ProbeMs)) {
    restartProbeAt_ = SDL_GetTicks();
    refreshAfterRestart_ = false;
    refresh();
  }
  if (!restartShowing_) return;
  if (net::serverRestarting()) {
    noticeAt_ = SDL_GetTicks();
    return;
  }
  restartShowing_ = false;
  if (notice_ == tr("The server is restarting. Downloads go on shortly.")) notice_.clear();
}
// The server took this console back (its account off, its password
// changed): a refresh, where the session drops the old key and asks for
// approval again, so the QR code shows instead of each screen failing.
void Application::followTakeBack() {
  if (session_ && net::takeConsoleRevoked(session_->server().url)) takenBack_ = true;
  if (!takenBack_ || !session_ || network_.joinable()) return;
  takenBack_ = false;
  logLine("the server took this console back; refreshing to ask for approval again");
  refresh();
}
// Where a cut-off file stands, from the install thread to the card.
bool Application::keepResumeChanges() {
  auto changes = installer_.takeResumeChanges();
  if (changes.empty() || !store_.isOpen())
    return false;
  {
    std::lock_guard<std::mutex> held(historyMutex_);
    if (historyClosing_) {
      // Closing: the writer is gone, so straight to the card.
      for (const auto &[key, point] : changes)
        point ? store_.saveResumePoint(key, *point) : store_.forgetResumePoint(key);
      return true;
    }
    for (auto &[key, point] : changes)
      resumePending_[key] = std::move(point);
  }
  wakeHistoryWriter();
  return true;
}
void Application::keepDownloadPlace() {
  if (!installer_.running())
    return;
  using namespace std::chrono_literals;
  constexpr int Steps = 40; // 10 ms each: the system waits on this before it suspends the app
  // Each download that answers is waited on, and only those (one between
  // files, or in a stream with no place to keep, would hold HOME for nothing).
  const auto served = net::checkpointsServed();
  net::checkpointSoon();
  for (int i = 0; i < Steps && net::checkpointsServed() - served < uint64_t(net::segmentedRunning()); ++i)
    std::this_thread::sleep_for(10ms);
  const auto written = resumeWritten_.load();
  if (!keepResumeChanges())
    return;
  for (int i = 0; i < Steps && resumeWritten_.load() == written; ++i)
    std::this_thread::sleep_for(10ms);
}
bool Application::open() {
  // The first steps run on the main thread; the slow ones (the SD card, the
  // server settings, the console's games) on a thread of their own while
  // this one keeps the butter bouncing and lets the console handle HOME.
  {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(paint_.renderer, &w, &h);
    outputScale_ = w / 1280.f;
  }
  const uint32_t shownAt = SDL_GetTicks();
  const auto seconds = [&] { return (SDL_GetTicks() - shownAt) / 1000.f; };
  float progressShown = 0.03f;
  // What the loading thread is on, for the frames drawn meanwhile.
  std::mutex shownLock;
  std::string caption;
  // Each step says what it is doing under the logo and owns a stretch of
  // the bar sized to how long it takes on a console (the server settings
  // about half): `from` where it starts, `to` where the next one does.
  float stepFrom = 0.03f, stepTo = 0.06f;
  const auto background = [&](const std::string &text, float from, float to) {
    std::lock_guard<std::mutex> lock(shownLock);
    caption = text;
    stepFrom = from;
    stepTo = to;
  };
  // The first frame makes the mark and wordmark (the loading thread never
  // touches the renderer); the cues are made on their own thread meanwhile.
  paint_.splash("", progressShown, seconds());
  sounds_.open();
  // Set to play now, not after the records are read: a card that takes
  // longer than the first landing would push the whole cue a beat late.
  // The records turn it off again if sounds are off.
  sounds_.playStart(shownAt);
  // Everything after this is read on the loading thread. It never draws:
  // the fonts are the screen's once open, and the language it sets is
  // applied to them when it is done.
  background("", 0.03f, 0.14f);
  loading_ = true;
  std::atomic<bool> loaded{false}, fontsOpen{false}, fontsFailed{false};
  std::thread loader([&] {
    const platform::OnCore off(platform::WriterCore); // the drawing core keeps the splash moving
    // The records file is read by several steps below: from the card once,
    // from memory after, until the start is done.
    const DownloadStore::KeepReads keep;
    guarded("startup", [&] {
      if (!paint_.fonts.open()) {
        fontsFailed = true;
        return;
      }
      fontsOpen = true;
      // The saved language is read now so the captions are already in it;
      // reload() sets it again with the config as the last fallback.
      background(tr("Reading the app's records on the SD card."), 0.14f, 0.38f);
      if (!statePath_.empty()) {
        const auto saved = loadState(statePath_).value;
        state_.language = saved.language;
        state_.sounds = saved.sounds;
      }
      applyStartupLanguage();
      sounds_.setOn(state_.sounds);
      // Each step says what it is doing under the logo, so a slow SD card or
      // server shows where the time goes.
      background(tr("Preparing the image cache"), 0.38f, 0.40f);
      images_ = std::make_unique<Images>(paint_.renderer, outputScale_);
      images_->source({}, {}, imageCacheFolder(), fixture_);
      background(tr("Reading patches and download history on the SD card"), 0.40f, 0.62f);
#ifdef __SWITCH__
      const std::string sd = "sdmc:";
#else
      const std::string sd = statePath_.empty() ? std::string() : std::filesystem::path(statePath_).parent_path().string() + "/sd";
#endif
      if (!sd.empty() && !patchWorker_.joinable()) {
        openCardRecords(sd);
        background(tr("Reading patches and download history on the SD card"), 0.62f, 0.66f);
        openPatchStore(sd);
      }
      if (!configPath_.empty()) {
        background(tr("Reading the server settings"), 0.66f, 0.86f);
        reload();
      }
#ifdef __SWITCH__
      background(tr("Reading the games on this console"), 0.86f, 0.92f);
      refreshInstalled();
      // Album (applet mode) gets a few hundred MB and no title rights: say so
      // before something big fails, and how to open it properly.
      if (!platform::applicationMode()) {
        alertTitle_ = tr("Running in applet mode");
        alert_ = tr("Opened from the Album there is little memory, so big installs can be slow or fail. Launch a game while holding R to open in application mode.");
      }
#endif
    });
    loaded = true;
  });
  // The steps' shares follow what they take on the console (records and
  // downloads a third of a second each, the fonts and the settings less).
  // Within a step the bar's goal creeps toward the next step's start (most
  // of the way in under a second, never reaching it); the bar follows at a
  // limited speed, so a new step never makes it jump.
  // A fast card still waits for the start cue (user, 2026-10-18): the bar
  // fills up on the fanfare's chord, so the landings, the sound and the
  // last word come as one. Without the sound it ends where the chord would
  // be (the cue starts on the first landing it is ready for). A card slower
  // than that ends when its reading does. The catalog that comes meanwhile
  // is laid out, so the home opens filled.
  constexpr uint32_t SyncFillMs = 450;
  const auto chordAt = [&] {
    const uint32_t heard = sounds_.startHeardAt();
    return (heard ? heard : shownAt + 1000) + Sounds::FanfareMs;
  };
  while (!loaded || int32_t(chordAt() - SyncFillMs - SDL_GetTicks()) > 0) {
    if (loaded && done_.exchange(false)) {
      takeNetwork();
      warmHome();
    }
    if (loaded && images_) images_->decode();
    std::string text;
    float goal = 0;
    {
      std::lock_guard<std::mutex> lock(shownLock);
      text = caption;
      // The bar runs on time alone: steadily to 80% over three seconds,
      // easing in at the end, and waits there; when the loading is done the
      // fill below takes it the rest of the way. The steps only name what
      // runs, so none makes it stop or leap.
      const float t = std::min(1.f, seconds() / 3.f);
      goal = 0.8f * (1 - (1 - t) * (1 - t) * 0.5f - 0.5f * (1 - t));
    }
    progressShown = std::max(progressShown, goal);
    const uint32_t frameAt = SDL_GetTicks();
    paint_.splash(fontsOpen ? text : std::string(), progressShown, seconds());
    if (!platform::serviceApplet())
      exitRequested_ = true; // asked to close: it does once loading ends
    const uint32_t spent = SDL_GetTicks() - frameAt;
    if (spent < 16) SDL_Delay(16 - spent); // no vsync (the host preview): about 60 a second
  }
  loader.join();
  loading_ = false;
  if (fontsFailed)
    return false;
  paint_.fonts.preferLanguage(language());
  // The bar runs on to its end before the app shows, rather than the app
  // popping up from part way: a fill, then a moment full to read the last
  // word.
  constexpr uint32_t HoldMs = 250;
  const float from = progressShown;
  // A card slower than the start cue keeps its beat too: the bar fills up on
  // the butter's next landing at least MinFillMs away.
  constexpr uint32_t MinFillMs = 400;
  const int32_t toChord = int32_t(chordAt() - SDL_GetTicks());
  const uint32_t toLanding = 1000 - (SDL_GetTicks() - shownAt) % 1000;
  const uint32_t FillMs = toChord > 0 ? uint32_t(toChord) : toLanding + (toLanding < MinFillMs ? 1000 : 0);
  // The last word changes each start: a kitchen line or a nod to a game.
  static const char *const Done[] = {"Well done!", "Golden brown, just right!", "A wild homebrew appeared!",
                                     "It's dangerous to go alone! Take this.", "You cooked something tasty!",
                                     "3, 2, 1, GO!", "Let's-a go!",
                                     "Your library is in this castle!",
                                     "All your base are belong to us.",
                                     "Do a barrel roll!",
                                     "Hey! Listen!",
                                     "Finish him!",
                                     "Hadouken!",
                                     "Shoryuken!",
                                     "It's super effective!",
                                     "A winner is you!",
                                     "The cake is not a lie.",
                                     "Would you kindly press A?",
                                     "Snake? Snake?! SNAAAKE!",
                                     "Up, Up, Down, Down, Left, Right, Left, Right, B, A",
                                     "War never changes. Your library does.",
                                     "Stay a while and listen.",
                                     "Nothing is true, everything is downloadable.",
                                     "Praise the sun!",
                                     "YOU DIED... just kidding, it loaded.",
                                     "Continue? 9... 8... 7...",
                                     "Hey, you. You're finally awake.",
                                     "Get over here!",
                                     "Fus Ro Dah!",
                                     "Waka waka waka!",
                                     "Gotta go fast!",
                                     "Achievement unlocked: opened the app",
                                     "Press any button. Except HOME.",
                                     "PLAYER 1 READY",
                                     "It's-a me!",
                                     "Your quest begins now.",
                                     "Loading... the boss is waiting."};
  const std::string done = tr(Done[std::time(nullptr) % std::size(Done)]);
  const uint32_t fillFrom = SDL_GetTicks();
  for (;;) {
    const uint32_t gone = SDL_GetTicks() - fillFrom;
    const float t = std::min(1.f, gone / float(FillMs)), eased = 1 - (1 - t) * (1 - t);
    paint_.splash(done, from + (1 - from) * eased, seconds());
    // The catalog that arrives while the last word shows is laid out now, so
    // the home opens filled rather than with its placeholders.
    if (done_.exchange(false)) {
      takeNetwork();
      warmHome();
    }
    if (images_) images_->decode();
    if (!platform::serviceApplet()) {
      exitRequested_ = true;
      break;
    }
    if (gone >= FillMs + HoldMs)
      break;
    const uint32_t spent = SDL_GetTicks() - fillFrom - gone;
    if (spent < 16) SDL_Delay(16 - spent);
  }
  paint_.splashDone();
  return true;
}
// Patches live on the SD card: a store under switch/cookingfoil and the
// atmosphere folders they switch on.
void Application::usePatchFolder(const std::string &sd) {
  if (patchWorker_.joinable())
    return;
  openCardRecords(sd);
  openPatchStore(sd);
}
// The app's records on the card: the homebrew ledger and 다운로드's list.
void Application::openCardRecords(const std::string &sd) {
  sdRoot_ = sd;
  net::rememberTuningIn(sd + "/switch/cookingfoil/tuning.txt");
  const auto ledgerFile = sd + "/switch/cookingfoil/homebrew.json";
  homebrewLedger_ = homebrew::parseLedger(readFile(ledgerFile));
  // An older ledger holds folder installs' file lists: they move to their
  // own files once, and the ledger is written without them.
  std::vector<std::string> moved;
  bool lists = true;
  for (const auto &[id, record] : homebrewLedger_) {
    if (record.files.empty()) continue;
    const auto path = sd + "/" + homebrew::fileListPath(id);
    std::string error;
    makeDirectories(std::filesystem::path(path).parent_path().string());
    if (atomicWrite(path, homebrew::serializeFileList(record.files), error)) moved.push_back(id);
    else {
      logLine("homebrew files: " + error);
      lists = false;
    }
  }
  if (!moved.empty() && lists) {
    std::string error;
    if (atomicWrite(ledgerFile, homebrew::serializeLedger(homebrewLedger_), error))
      logLine("homebrew ledger: file lists of " + std::to_string(moved.size()) + " app(s) moved to their own files");
    else
      logLine("homebrew ledger: " + error);
  }
  // An app that closed mid-install left that one running: it goes back in
  // the queue, which picks up again once the shop is reachable.
  openDownloads(sd);
  rebuildQueue();
  // Left waiting or cut off last time: asked about once the shop is
  // reachable (offerResume), so nothing is said here.
}
// 패치's store: its ledger, and what a cut off change left half done.
void Application::openPatchStore(const std::string &sd) {
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
  if (!loading_) // open() applies it to the fonts on its own thread
    paint_.fonts.preferLanguage(language());
}
void Application::toggleInstallCheck(bool &skip) {
  skip = !skip;
  saveOwnState();
}
// What the app keeps for itself in cookingfoil.db (settings, never
// config.json), written by the history writer.
void Application::saveOwnState() {
  if (statePath_.empty())
    return;
  bool closing;
  {
    std::lock_guard<std::mutex> held(historyMutex_);
    statePending_ = state_;
    closing = historyClosing_;
  }
  if (closing)
    writePendingState(); // the writer is gone, so straight to the card
  else
    wakeHistoryWriter();
}
void Application::writePendingState() {
  std::lock_guard<std::mutex> writing(stateWriteLock_);
  std::optional<State> state;
  {
    std::lock_guard<std::mutex> held(historyMutex_);
    state = std::exchange(statePending_, std::nullopt);
  }
  Error error;
  if (state && !saveSettings(statePath_, *state, error))
    logLine("state: " + error.message);
}
void Application::chooseLanguage(Language lang) {
  setLanguage(lang);
  paint_.fonts.preferLanguage(lang);
  state_.language = languageCode(lang);
  shopSavesLoaded_ = false; // server saves come in the language picked
  saveOwnState();
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
  labelBook_ = serverLabels();
  qr_.encode(pairingInfo_.url);
  pairPollAt_ = pairingPending_ ? SDL_GetTicks() : 0;
  if (!pairingPending_) pairSince_ = 0;
  else if (!pairSince_) pairSince_ = SDL_GetTicks();
  images_ = std::make_unique<Images>(paint_.renderer, outputScale_);
  images_->source(
      session_->server(), state_,
      imageCacheFolder(),
      fixture_);
  renewTokensWith(session_->server());
}
void Application::localizeNames() {
  // The chosen language first. A Korean interface then keeps the store's own
  // name and art (this is a Korean shop); the others go through
  // languageChain (traditional Chinese to simplified, then English) before it.
  const std::string code = languageCode();
  const auto pick = [&](const LanguageTexts &texts) { return displayText(texts, code); };
  for (auto &g : groups_) {
    const auto *source = g.base ? &*g.base : &g.representative;
    if (!source->icons.empty() || g.base) {
      const auto *icon = pick(source->icons);
      g.representative.icon = icon ? *icon : source->icon;
    }
    if (source->names.empty())
      continue;
    const auto *name = pick(source->names);
    g.representative.name = name ? *name : source->name;
  }
}
namespace {
// Groups copy each item into files (and the base into representative);
// only the base, or the representative without one, needs its names.
std::vector<Group> groupedCatalog(std::vector<Item> items) {
  auto groups = groupCatalog(std::move(items));
  for (auto &g : groups) {
    for (auto *list : {&g.alternatives, &g.dlc})
      for (auto &f : *list)
        LanguageTexts().swap(f.names), LanguageTexts().swap(f.icons);
    if (g.update)
      LanguageTexts().swap(g.update->names), LanguageTexts().swap(g.update->icons);
    if (g.base)
      LanguageTexts().swap(g.representative.names), LanguageTexts().swap(g.representative.icons);
  }
  return groups;
}
} // namespace
void Application::setItems(std::vector<Item> items) { setGroups(groupedCatalog(std::move(items))); }
void Application::setGroups(std::vector<Group> groups) {
  groups_ = std::move(groups);
  groupIndex_.clear();
  groupIndex_.reserve(groups_.size());
  for (size_t i = 0; i < groups_.size(); ++i)
    groupIndex_.emplace(groups_[i].id, i);
  updatesStale_ = true;
  localizeNames();
  chips_ = libraryChips(groups_, genreNames_);
  rebuildHome();
  activeChips_.clear();
  ++catalogVersion_;
  connected_ = true;
  serverAnsweredAt_ = SDL_GetTicks();
  selected_ = 0;
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
  if (!patchesFixed_) {
    serverPatches_.clear();
    patchesLoaded_ = false;
  }
  patchError_ = {};
  groups_.clear();
  groupIndex_.clear();
  themes_.clear();
  chips_.clear();
  activeChips_.clear();
  ++catalogVersion_;
  hero_ = 0;
  incoming_.clear();
  incomingGroups_.clear();
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
  unglide();
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
  // The list as the server has it now, asked with its ETag: unchanged costs
  // a short reply, and the list on screen stays until a new one comes.
  if (screen == Screen::Homebrew) {
    checkHomebrewCard();
    if (homebrewLoaded_ && !homebrewError_) loadHomebrew();
  }
  // 설정 says whether a newer build is on the server, asked once a launch.
  if (screen == Screen::Settings && appUpdate_ == AppUpdate::Unknown && connected_)
    checkAppUpdate();
  // Dropbox's popups belong to the visit that opened them.
  dropboxOpen_ = dropboxUnlinkAsk_ = dropboxChoosing_ = false;
  raOpen_ = raListOpen_ = false; // RetroAchievements' too
  raLinkClose();
  if (screen == Screen::Saves) {
    savesFresh_ = false;
    readBackupSummary();
    refreshInstalled();
  }
  // 레트로 게임 asks the card again which ROMs are there, and an empty list
  // asks the shop again (its folder may have been filled since).
  if (screen == Screen::Retro) {
    retroOnCard_.clear();
    retroView_ = retroHomeMissing_ ? RetroView::List : RetroView::Home;
    if (!retroListWorker_.joinable()) retroLoaded_ = false;
  }
  // With nothing to select the cursor stays on the menu instead of vanishing.
  sidebar_ = autoSidebar_ = !hasFocusable();
}
bool Application::hasFocusable() const {
  switch (screen_) {
  case Screen::Home:
    return !groups_.empty() || homeOffersSettings() || homeOffersRetry(); // its notice's button
  case Screen::Library:
    return true; // the search box is always there
  case Screen::Installed:
    return !deviceBusy_ && !gameRows(installedQuery_).empty();
  case Screen::Updates:
    return connected_ && !deviceBusy_ && !updateRows().empty();
  case Screen::Downloads:
    return downloadsTab_ == 0 ? installer_.running() || !installQueue_.empty() || retroDownloading() || patchDownloading()
                              : !downloadsList().empty();
  case Screen::Patches:
    return !patchRows().empty();
  case Screen::Homebrew:
    return !homebrewRows().empty();
  case Screen::Retro:
    return retroView_ != RetroView::Home || !retroShelves_.empty();
  case Screen::Saves:
    return savesTab_ == 2 ? dropboxFocusable() : savesTab_ == 1 ? !shopSaveGroups().empty() : !deviceBusy_ && !saveGameRows().empty();
  case Screen::Settings:
  case Screen::Detail:
  case Screen::RetroDetail:
  case Screen::RetroArch:
    return true;
  }
  return true;
}
void Application::message(const std::string &text) {
  if (notice_.empty()) noticeShownAt_ = SDL_GetTicks();
  notice_ = text;
  noticeAt_ = SDL_GetTicks();
}
void Application::reload(int prefer) {
  if (network_.joinable() && !done_) {
    message(tr("Connecting to the server."));
    return;
  }
  if (loading_) {
    applyReload(prefer, readForReload(true));
    return;
  }
  reloadPrefer_ = prefer;
  if (reloadWorker_.joinable()) {
    reloadAgain_ = true;
    return;
  }
  reloadWorker_ = std::thread([this] {
    guarded("reload", [&] { reloadRead_ = readForReload(false); });
    reloadDone_ = true;
  });
}
Application::ReloadRead Application::readForReload(bool waitOut) {
  ReloadRead read;
  read.config = loadConfig(configPath_);
  // Read with no servers at start: read again a few times before 설정 takes
  // over (a console was seen reading none at start and its servers a moment
  // later). Only the first time: a refresh with none answers at once. A file made just now (a first start) has none to wait for.
  const bool first = std::exchange(configReadOnce_, true) == false;
  for (int again = 0; first && !read.config.created && again < 3 && !read.config.error && read.config.value.servers.empty(); ++again) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    read.config = loadConfig(configPath_);
  }
  if (read.config.error) return read;
  {
    // The stored device key gives way to the console's own once all of these
    // took it (core/State), as the state is read below.
    std::vector<std::string> urls;
    for (const auto &s : read.config.value.servers) urls.push_back(s.url);
    setKnownServers(std::move(urls));
  }
  writePendingState(); // what was just set, before it is read back
  read.state = loadState(statePath_, &read.busy);
  // At start the loading screen waits for a write still going, so the app
  // opens once, read, rather than showing its menu and then reading again.
  for (const auto until = SDL_GetTicks() + 15000; waitOut && read.state.error && read.busy && SDL_GetTicks() < until;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    read.state = loadState(statePath_, &read.busy);
  }
  return read;
}
void Application::applyReload(int prefer, ReloadRead read) {
  if (network_.joinable() && !done_) {
    message(tr("Connecting to the server."));
    return;
  }
  // Work that finished while the files were read is taken with the session
  // it ran on, before that session is replaced.
  poll();
  auto &parsed = read.config;
  if (parsed.error) {
    error_ = parsed.error;
    screen(Screen::Settings);
    return;
  }
  auto &state = read.state;
  const bool busy = read.busy;
  if (state.error && busy && !session_) {
    // Held by a write still going, not broken: the servers show now (not an
    // empty 설정) and the records are read again until they can be.
    config_ = std::move(parsed.value);
    waitForState(prefer);
    return;
  }
  if (state.error) {
    error_ = state.error;
    screen(Screen::Settings);
    return;
  }
  // - on the server already shown keeps its list until the new one is in;
  // anything else (another server, none left) starts from empty.
  const std::string shown = refreshing_ && connected_ && session_ ? session_->server().url : "";
  size_t target = config_.servers.size();
  {
    const auto &servers = parsed.value.servers;
    if (prefer >= 0 && size_t(prefer) < servers.size() && servers[prefer].enabled) target = prefer;
    else
      target = std::find_if(servers.begin(), servers.end(), [](const auto &s) { return s.enabled; }) - servers.begin();
    keepCatalog_ = !shown.empty() && target < servers.size() && servers[target].url == shown;
  }
  if (!keepCatalog_) {
    clearCatalog();
    session_.reset();
  }
  serverIndex_ = 0;
  config_ = std::move(parsed.value);
  error_ = {};
  state_ = state.value;
  applyStartupLanguage();
  if (parsed.created)
    message(tr("Created the config file. Add a server with \"+ Add server\" in Settings."));
  if (prefer >= 0 && size_t(prefer) < config_.servers.size() && config_.servers[prefer].enabled) {
    connect(prefer);
    return;
  }
  auto found = std::find_if(config_.servers.begin(), config_.servers.end(),
                            [](const auto &s) { return s.enabled; });
  // No server to connect to: the screen stays where it is; 홈 says how to
  // add one (설정 is a tap away). Only an error in the files sends it there.
  if (found == config_.servers.end())
    return;
  connect(found - config_.servers.begin());
}
void Application::connect(size_t index) {
  if (index >= config_.servers.size() || !config_.servers[index].enabled)
    return;
  if (network_.joinable() && !done_) {
    message(tr("Connecting to the server."));
    return;
  }
  poll();
  const bool keep = std::exchange(keepCatalog_, false) && session_ && connected_ &&
                    session_->server().url == config_.servers[index].url;
  if (!keep) clearCatalog();
  serverIndex_ = index;
  session_ = std::make_unique<net::Session>(config_.servers[index], state_,
                                            statePath_, &stop_);
  session_->setConfigPath(configPath_);
  others_.clear();
  serverItems_.clear();
  serverStatus_.clear();
  std::vector<Server> others;
  for (size_t i = 0; i < config_.servers.size(); ++i) {
    const auto &server = config_.servers[i];
    if (i == index || !server.enabled) continue;
    others_.push_back(std::make_unique<net::Session>(server, state_, statePath_, &stop_));
    others_.back()->setConfigPath(configPath_);
    renewTokensWith(server);
    others.push_back(server);
  }
  if (keep) {
    // The covers already drawn stay; only the keys may be new.
    images_->updateState(state_);
  } else {
    connected_ = false;
    images_ = std::make_unique<Images>(paint_.renderer, outputScale_);
    images_->source(config_.servers[index], state_, imageCacheFolder(), fixture_);
  }
  images_->alsoFrom(others);
  renewTokensWith(config_.servers[index]);
  refresh();
}
void Application::renewTokensWith(const Server &server) {
  net::setTokenRenewer(server.url, [server, state = state_] { return net::renewDownloadToken(server, state); });
}
labels::Book Application::serverLabels() const {
  labels::Book book;
  if (session_) labels::merge(book, session_->labels());
  for (const auto &other : others_) labels::merge(book, other->labels());
  return book;
}
void Application::refresh() {
  if (!session_ || network_.joinable())
    return;
  // The approval note may have changed since the session last asked.
  session_->setPairMessage(pairMessage());
  // A menu drawn before the server named its features (a failed first try)
  // was a guess, 레트로 게임 and 홈브류 left out: the next try shows the
  // placeholders again until it answers. A known menu stays as it is.
  if (features_.empty()) menuKnown_ = false;
  network_ = std::thread([this] {
    // Reading the catalog (seconds of JSON) stays off the drawing core too.
    const platform::OnCore off(platform::NetworkCore);
    incomingError_ = {};
    incomingCatalog_ = false;
    const bool ok = guarded("catalog", [this] {
      const bool read = session_->refresh(incomingError_);
      const bool changed = read && !session_->unchanged();
      // Grouped here, off the drawing thread: it takes a few hundred
      // milliseconds for a large catalog, which stopped the screen.
      if (read && readOthers(changed)) {
        incomingCatalog_ = true;
        incomingGroups_ = groupedCatalog(mergeServers([&] {
          std::vector<ServerItems> lists{{session_->server().url, serverItems_[session_->server().url]}};
          for (const auto &other : others_)
            if (const auto kept = serverItems_.find(other->server().url); kept != serverItems_.end())
              lists.push_back({other->server().url, kept->second});
          return lists;
        }()));
      } else if (changed && others_.empty()) {
        incomingCatalog_ = true;
        incomingGroups_ = groupedCatalog(mergeServers({{session_->server().url, session_->takeItems()}}));
      }
    });
    if (!ok)
      incomingError_.message = tr("Failed to process the list. Check log.txt.");
    done_ = true;
  });
}
void Application::waitForState(int prefer) {
  statePrefer_ = prefer;
  if (stateWaiting_) return; // already reading again
  stateWaiting_ = true;
  stateWaitAt_ = SDL_GetTicks();
  error_ = {};
  if (stateWorker_.joinable()) stateWorker_.join();
  stateWorker_ = std::thread([this, path = statePath_] {
    while (!stop_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      bool busy = false;
      guarded("state", [&] {
        const auto read = loadState(path, &busy);
        if (!read.error) busy = false;
      });
      if (!busy) break; // readable now, or broken: reload says which
    }
    stateReady_ = true;
  });
}
void Application::poll() {
  // Applied once a connection still running is done, as reload() itself
  // would wait for it.
  if (reloadDone_ && !(network_.joinable() && !done_) && reloadDone_.exchange(false)) {
    reloadWorker_.join();
    const bool dropped = std::exchange(reloadDrop_, false);
    if (std::exchange(reloadAgain_, false))
      reload(reloadPrefer_);
    else if (!dropped)
      applyReload(reloadPrefer_, std::move(reloadRead_));
  }
  if (stateReady_.exchange(false)) {
    stateWorker_.join();
    stateWaiting_ = false;
    reload(statePrefer_);
  }
  pollDevice();
  pollSaveJob();
  pollRetro();
  pollShopSaves();
  pollBackupSummary();
  pollDropbox();
  raJob_.poll();
  historyPageJob_.poll();
  keysJob_.poll();
  voteJob_.poll();
  raListJob_.poll();
  raGamesJob_.poll();
  adminJob_.poll();
  pollPatches();
  pollPatchCheck();
  pollFileCleanup();
  pollAppUpdate();
  pollClock();
  // The keys were made again at a load (core/State): said once.
  if (const auto why = statePath_.empty() ? Rebound::None : takeRebound(); why != Rebound::None) {
    alertTitle_ = tr("Approve this console again");
    alert_ = why == Rebound::Moved     ? tr("This card came from another console, so this console is approved again on the server.")
             : why == Rebound::Changed ? tr("The saved sign-in details were changed, so this console is approved again on the server.")
                                       : tr("The app's records could not be read and were made again, so this console is approved again on the server.");
  }
  pollLog();
  pollHomebrew();
  pollResumeCheck();
  // No dimming or auto-sleep while something downloads.
  const bool downloading = installer_.running() || retroDownloading() || (patchWorker_.joinable() && !patchBusy_.empty()) ||
                           !patchQueue_.empty() ||
                           appUpdate_ == AppUpdate::Downloading;
  awake_ = downloading;
  applyAwake();
  pumpConsoleTasks();
  keepResumeChanges();
  serverRestartNotice();
  followTakeBack();
  const auto installStatus = installer_.progress().status;
  if (installStatus == install::Status::Running) {
    const auto bytes = net::receivedBytes();
    if (bytes != lastBytes_) {
      lastBytes_ = bytes;
      lastByteMs_ = SDL_GetTicks();
    }
  }
  if (observedInstall_ == install::Status::Running && installStatus == install::Status::Succeeded) {
    // The server's shortcut over an older one: the HOME Menu reads its new
    // icon. The console calls run before the installed list is read, off the
    // drawing thread.
    std::vector<uint64_t> fresh, replaced;
    for (const auto &part : installer_.progress().parts) {
      // A shortcut (its id in the shortcut range), with a row of its own or not.
      const uint64_t id = forwarder::parseHexId(part.titleId);
      if (!forwarder::shortcutId(id)) continue;
      fresh.push_back(id);
      shortcuts_.insert(id);
      // One made here before for the same app goes, so it shows once: asked
      // whether or not the last look saw it (a missing one is no error).
      for (const auto &app : homebrew_)
        if (app.forwarderTitleId == part.titleId) rememberShortcutKey(app.id, app.forwarderKey);
      for (const auto &app : homebrew_)
        if (app.forwarderTitleId == part.titleId)
          if (const auto local = homebrewShortcutId(app); local && local != id) {
            replaced.push_back(local);
            shortcuts_.erase(local);
          }
    }
    refreshInstalled(fresh.empty() ? std::function<void()>() : [fresh, replaced] {
      for (const auto id : fresh) platform::refreshShortcutIcon(id);
      for (const auto local : replaced)
        if (const auto error = platform::removeShortcut(local)) logLine("shortcut: the one made here stays: " + error.message);
    });
  }
  // Stopped as the app closes: not an end. Its row, its place in the queue
  // and where it stands all stay, and the next start goes on from there.
  const bool stopped = installStatus == install::Status::Stopped;
  // 다운로드 keeps how it ended.
  if (observedInstall_ == install::Status::Running && installStatus != install::Status::Running &&
      installStatus != install::Status::Idle && !stopped) {
    // A finished install keeps what it took: bytes, the download (to the
    // last byte), the install after it.
    if (installStatus == install::Status::Succeeded)
      for (auto &e : history_)
        if (e.outcome == history::Outcome::Running && e.kind.empty()) {
          e.bytes = lastBytes_ - jobStartBytes_;
          e.startedAt = jobStartedAt_;
          e.downloadSeconds = (lastByteMs_ - jobStartMs_) / 1000.0;
          e.installSeconds = (SDL_GetTicks() - lastByteMs_) / 1000.0;
          break;
        }
    const auto outcome = installStatus == install::Status::Succeeded ? history::Outcome::Done
                         : installStatus == install::Status::Cancelled ? history::Outcome::Cancelled
                                                                       : history::Outcome::Failed;
    const auto error = outcome == history::Outcome::Failed ? failureText(installer_.progress().error) : installer_.progress().error;
    // How it ended, with what the player was told: a report shows it.
    logLine("install " + std::string(outcome == history::Outcome::Done ? "done" : outcome == history::Outcome::Cancelled ? "cancelled" : "failed") +
            ": " + installer_.progress().name + (installer_.progress().error.empty() ? "" : " · " + installer_.progress().error));
    if (history::finish(history_, outcome, error, int64_t(std::time(nullptr)))) saveHistory();
  }
  pumpPatchQueue();
  // Done or cancelled on purpose: nothing left to resume. A failure keeps it.
  if (observedInstall_ == install::Status::Running &&
      (installStatus == install::Status::Succeeded || installStatus == install::Status::Cancelled))
    forgetInstall();
  const bool ended = observedInstall_ == install::Status::Running && installStatus != install::Status::Running;
  observedInstall_ = installStatus;
  if (ended && !stopped) {
    // A base game that did not install takes the rest of its pick with it.
    if (installStatus != install::Status::Succeeded) {
      const auto before = installQueue_.size();
      const std::string why = tr("It did not install, so the rest picked with it was taken off the queue.");
      dropGroup(runningGroup_, why);
      if (installQueue_.size() != before && installStatus == install::Status::Failed)
        message(why);
    }
    runningGroup_ = -1;
    // A title app whose file changed on the server after the list was made
    // is refused (503); the list is read again, so a next try takes the new
    // file.
    if (installStatus == install::Status::Failed) {
      const auto parts = installer_.progress().parts;
      const bool titleApp = std::any_of(homebrew_.begin(), homebrew_.end(), [&](const homebrew::App &app) {
        return app.title && std::any_of(parts.begin(), parts.end(), [&](const Item &f) { return f.url == app.downloadUrl; });
      });
      if (titleApp) {
        homebrewEtags_.clear();
        loadHomebrew();
      }
      for (const auto &part : parts)
        if (forwarder::shortcutId(forwarder::parseHexId(part.titleId))) shortcutFailed(part.titleId);
    }
    // The card leaves 진행 중 at once; how it ended is said here and kept
    // in 완료 or 실패.
    if (installStatus == install::Status::Succeeded) {
      // A HOME Menu shortcut follows what it belongs to, which already
      // played the done: its own, smaller sound.
      const auto &done = installer_.progress().parts;
      const bool shortcut = !done.empty() && std::all_of(done.begin(), done.end(), [](const Item &f) {
        return forwarder::shortcutId(forwarder::parseHexId(f.titleId));
      });
      sounds_.play(shortcut ? Cue::Shortcut : Cue::Done);
      // Only what puts a new entry on the HOME Menu (a base game, an app
      // installed as a title) points there; an update or DLC just finished.
      const auto &parts = installer_.progress().parts;
      const bool addon = !parts.empty() && std::all_of(parts.begin(), parts.end(), [](const Item &f) {
        return f.type == "dlc" || f.type == "update";
      });
      message(installer_.progress().name + " · " +
              (addon ? tr("Install finished.") : tr("Installed. You can launch it from the HOME Menu.")));
    } else if (installStatus == install::Status::Failed) {
      sounds_.play(Cue::Error);
      message(installer_.progress().name + " · " + installer_.progress().error);
    }
  }
  // A patch job whose patch the worker finished (or skipped) ends its row.
  if (!patchWorker_.joinable() && patchQueue_.empty())
    settlePatchJob(false, "");
  // The next one starts once nothing downloads. At launch the queue waits
  // until the shop answers (offerResume), so what was left resumes in order.
  if (resumeOffered_ || ended)
    if (auto error = launchQueued())
      alert_ = error.message;
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
  takeNetwork();
}
// What the network thread brought: a detail, the approval state or the catalog.
void Application::takeNetwork() {
  network_.join();
  // An enroll token the session spent is gone from the copy here too, so a
  // later save of the servers does not write it back.
  if (session_ && session_->server().enrollToken.empty())
    for (auto &server : config_.servers)
      if (server.url == session_->server().url) server.enrollToken.clear();
  if (detailRequest_) {
    detailLoading_ = false;
    if (!incomingError_) {
      details_[detailServer_ + requestedId_] = {incomingDetail_,
                                                         SDL_GetTicks()};
      if (screen_ == Screen::Detail && detailGroup_ < groups_.size() &&
          groups_[detailGroup_].id == requestedId_)
      {
        detail_ = std::move(incomingDetail_);
        syncDetailPatches();
      }
    } else if (!net::serverRestarting()) // a restart says so in its own notice
      message(incomingError_.message);
    detailRequest_ = false;
    if (screen_ == Screen::Detail && detailGroup_ < groups_.size() &&
        groups_[detailGroup_].id != requestedId_)
      fetchDetail();
    return;
  }
  pairingPending_ = session_->pairingPending();
  pairingInfo_ = session_->pairingInfo();
  // A password taken out of config.json after approval leaves this copy
  // too, so saving the server form never writes it back.
  if (session_->server().password.empty())
    for (auto &server : config_.servers)
      if (server.url == session_->server().url) server.password.clear();
  menuKnown_ = true; // the server answered (or did not): the menu is what it is
  qr_.encode(pairingInfo_.url);
  pairPollAt_ = pairingPending_ ? SDL_GetTicks() : 0;
  if (!pairingPending_) pairSince_ = 0;
  else if (!pairSince_) pairSince_ = SDL_GetTicks();
  // Asked while the server restarts (- pressed, or a timed check): not a
  // failure. What is on screen stays, the notice already says why, and the
  // list is asked again once the server is back.
  if (incomingError_ && net::serverRestarting()) {
    refreshing_ = false;
    refreshAfterRestart_ = true;
    return;
  }
  if (incomingError_) {
    error_ = incomingError_;
    refreshing_ = false;
    if (!pairingPending_)
      message(error_.message);
    return;
  }
  if (refreshing_) {
    refreshing_ = false;
    message(tr("The list is up to date."));
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
    state_.makeShortcuts = mine.makeShortcuts;
    state_.sounds = mine.sounds;
    state_.installTarget = mine.installTarget;
    state_.pairMessages = mine.pairMessages;
    // Each other server's keys come from its own session.
    for (const auto &other : others_) {
      const auto &url = other->server().url;
      const auto &theirs = other->state();
      if (const auto key = theirs.accessKeys.find(url); key != theirs.accessKeys.end()) state_.accessKeys[url] = key->second;
      else state_.accessKeys.erase(url);
      if (const auto secret = theirs.pairSecrets.find(url); secret != theirs.pairSecrets.end()) state_.pairSecrets[url] = secret->second;
      else state_.pairSecrets.erase(url);
    }
  }
  // Another server that newly fails or waits is said once, by name; the
  // shop goes on with the rest.
  for (const auto &[url, status] : incomingStatus_) {
    const auto before = serverStatus_.find(url);
    if (status.empty() || status == "same" || (before != serverStatus_.end() && before->second == status)) continue;
    const auto title = serverFor(url).title;
    message(status == "pairing" ? trf("{0} is waiting for approval. Its list shows once approved.", {title})
                                : trf("Could not connect to {0}. Its list is left out for now.", {title}));
  }
  serverStatus_ = std::move(incomingStatus_);
  incomingStatus_.clear();
  features_ = session_->features();
  serverFeatures_.clear();
  serverFeatures_[session_->server().url] = features_;
  for (const auto &other : others_)
    if (const auto status = serverStatus_.find(other->server().url); status != serverStatus_.end() && status->second.empty())
      serverFeatures_[other->server().url] = other->features();
  // Same server: keep decoded covers; only the access keys may have changed.
  images_->updateState(state_);
  if (screen_ == Screen::Detail)
    screen_ = previous_;
  // A 304 keeps the catalog already on screen.
  retries_ = 0;
  if (!incomingCatalog_ && !groups_.empty()) {
    connected_ = true;
    serverAnsweredAt_ = SDL_GetTicks();
    error_ = {};
    return;
  }
  featured_ = session_->featured();
  genreNames_ = session_->genres();
  scoreLabels_ = session_->scoreLabels();
  labelBook_ = serverLabels();
  setGroups(std::move(incomingGroups_));
  incomingGroups_ = {};
  error_ = {};
  offerResume();
  // 홈브류 is read ahead once the home is up, so it opens with its list.
  needHomebrewList();
  if (!homebrewScanned_ && !serversWith("homebrew").empty()) scanHomebrewCard();
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
InstalledMask Application::installedMaskNow() const {
  InstalledMask mask(groups_.size(), false);
  if (installed_.installed.complete)
    for (const auto &t : installed_.installed.titles)
      if (const auto found = groupIndex_.find(t.titleId); found != groupIndex_.end())
        mask[found->second] = true;
  return mask;
}
void Application::rebuildHome() {
  installedMask_ = installedMaskNow();
  // Admin picks (feature: featured) lead the home, mapped onto loaded groups.
  std::vector<HomeTheme> pinned;
  heroBanner_.clear();
  for (const auto &shelf : featured_) {
    HomeTheme row{{shelf.title, SortMode::Default, Filter::All, "", 0, 0, 0, 0, false, false, "", 0}, {}, shelf.icon};
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
    heroes_.push_back({homeHero(groups_, installedMask_), tr("Featured")});
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
  draftChips_ = activeChips_;
  draftSort_ = librarySort_;
  if (retro) {
    draftPicks_ = retro::keptPicks(retroFilterBook(), retroPicks_);
    draftSortKey_ = retroSortKey_;
    filterFocus_ = 0;
    filterScroll_ = 0;
    const auto sorts = retroFilterBook().sorts;
    const auto at = std::find_if(sorts.begin(), sorts.end(), [&](const auto &s) { return s.key == retroSortKey_; });
    popupRow_ = at == sorts.end() ? 0 : int(at - sorts.begin());
  }
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
      retroSystem_ = retroSystemAt(retroRow_);
    }
    retroGames_.clear();
    retroListSystem_.clear();
    retroTotal_ = 0;
    selected_ = 0;
    sidebar_ = false;
    if (!retroListWorker_.joinable()) loadRetroPage(false);
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
  if (a == Action::Up || a == Action::Down || a == Action::Left || a == Action::Right || a == Action::Previous ||
      a == Action::Next)
    sounds_.play(Cue::Move);
  // A button brings the view back to the cursor. Text read by finger stays
  // where it was read to: its scroll steps take the glide's place.
  if (glide_.active && glide_.id == "description")
    descriptionScroll_ = std::clamp(int(std::lround(glide_.pos / ScrollStep)), 0,
                                    (descriptionHeight_ + ScrollStep - 1) / ScrollStep);
  unglide();
  if (galleryOpen_ && (fullPage() || (screen_ == Screen::Patches && managingPatch_))) {
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
  if (alert_.empty() && clockModal()) {
    clockAction(a);
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
  if (adminEdit_.open) {
    adminEditAction(a);
    return;
  }
  if (resumeAsked()) {
    if (a == Action::Accept && settled()) {
      confirmResume_ = false;
      if (auto error = launchQueued())
        alert_ = error.message;
    } else if (a == Action::Back) {
      confirmResume_ = false;
      queueHeld_ = true;
    }
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
  if (viewPopup_ && screen_ == Screen::Retro) {
    retroFilterAction(a);
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
      {
        activeChips_ = draftChips_;
        librarySort_ = draftSort_;
      }
    }
    if (a == Action::Back) viewPopup_ = false;
    return;
  }
  if (confirmInstall_) {
    if (a == Action::Left || a == Action::Right) stepInstallServer(a == Action::Left ? -1 : 1);
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
  if (a == Action::Up && headerFocus_.empty() && !sidebar_ && !popupOpen() && !fullPage() &&
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
  if (screen_ == Screen::Retro && (retroArchAsking_ || raModal() || (!sidebar_ && (a != Action::Back || retroView_ != RetroView::Home)) || a == Action::Queue ||
                                   a == Action::Previous || a == Action::Next)) {
    retroAction(a);
    return;
  }

  if (languagePopup_) {
    // Two columns, filled top to bottom.
    if (a == Action::Up && languageRow_ % LanguageRows > 0) --languageRow_;
    if (a == Action::Down && languageRow_ % LanguageRows < LanguageRows - 1 && languageRow_ + 1 < LanguageCount) ++languageRow_;
    if (a == Action::Left && languageRow_ >= LanguageRows) languageRow_ -= LanguageRows;
    if (a == Action::Right && languageRow_ + LanguageRows < LanguageCount) languageRow_ += LanguageRows;
    if (a == Action::Accept) {
      languagePopup_ = false;
      if (Language(languageRow_) != language()) chooseLanguage(Language(languageRow_));
    }
    if (a == Action::Back) languagePopup_ = false;
    return;
  }
  if (tidyOpen_) {
    fileCleanupAction(a);
    return;
  }
  if (homebrewSortAsk_) {
    homebrewSortAction(a);
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
  if (bugAsk_) {
    // A sends (the log goes with it), Y writes what happened, B closes.
    if (a == Action::Accept && settled()) {
      bugAsk_ = false;
      sendBugReport(bugMessage_);
    }
    if (a == Action::Queue) {
      TextRequest request;
      request.guide = tr("What went wrong?");
      request.initial = bugMessage_;
      request.maxLength = 300;
      textRequest_ = request;
      textField_ = -1;
      raField_ = -1; // the keyboard is this field's now
      bugMessageField_ = true;
    }
    if (a == Action::Back) bugAsk_ = false;
    return;
  }
  if (unqueueAsk_ >= 0) {
    // A takes it off (once the popup has settled), B keeps it.
    if (a == Action::Accept && settled() && unqueueAsk_ < int(installQueue_.size())) {
      const int group = installQueue_[size_t(unqueueAsk_)].group;
      unqueue(size_t(unqueueAsk_));
      if (unqueuePick_ && group > 0)
        dropGroup(group, ""); // from 상세: the whole pick
      unqueueAsk_ = -1;
    }
    if (a == Action::Back || unqueueAsk_ >= int(installQueue_.size()))
      unqueueAsk_ = -1;
    if (unqueueAsk_ < 0)
      unqueuePick_ = false;
    return;
  }
  if (confirmCancel_) {
    // B keeps installing; A cancels, but not one pressed as the popup opened.
    if (a == Action::Accept && settled()) {
      if (cancelRetro_) retroCancel_ = true;
      else if (confirmExit_) {
        installer_.stop(); // leaving: kept to go on next time
        exitAfterCancel_ = true;
      } else
        installer_.cancel();
      confirmCancel_ = false;
    }
    if (a == Action::Back)
      confirmCancel_ = false;
    return;
  }
  // A retro game's page, under its cancel questions.
  if (screen_ == Screen::RetroDetail && a != Action::Reload) {
    retroDetailAction(a);
    return;
  }
  if (screen_ == Screen::RetroArch && a != Action::Reload) {
    retroArchPageAction(a);
    return;
  }
  // 진행 중: the card, then the queue, picked with up and down.
  if (screen_ == Screen::Downloads && downloadsTab_ == 0 && !sidebar_ &&
      (a == Action::Up || a == Action::Down || a == Action::Accept || a == Action::Queue || a == Action::Search)) {
    runningAction(a);
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
    menuKnown_ = false; // the menu waits for the answer, as at start
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
    // While the menu is placeholders there is nothing to move to yet.
    if (menuPending()) return;
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
      menuPending_ = menuOpen(nav_) && !showing(MenuScreens[nav_]);
      menuAt_ = SDL_GetTicks();
    }
    if ((a == Action::Accept || a == Action::Right) && menuOpen(nav_)) {
      menuPending_ = false;
      const auto target = MenuScreens[nav_];
      // Into the screen already shown: the cursor moves in, nothing reloads
      // (설치된 게임 would read the console again).
      if (!showing(target)) openMenuScreen(target);
      // Its content first; with nothing there yet, its header buttons.
      sidebar_ = autoSidebar_ = !hasFocusable();
      if (sidebar_ && enterHeader()) autoSidebar_ = false;
    }
    return;
  }
  if (screen_ == Screen::Home) {
    // The notice's button: 설정 with no server yet, 다시 시도 after a failure.
    if (homeOffersSettings() || homeOffersRetry()) {
      if (a == Action::Accept) {
        if (homeOffersSettings()) screen(Screen::Settings);
        else refreshAll();
      }
      if (a == Action::Left) sidebar_ = true; // to the menu, as from any page's first item
      return;
    }
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
    if (adminChipAction(a, part_ == 0)) return;
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
    if (!rows.empty()) updatesFocus_ = rows[std::min(selected_, int(rows.size()) - 1)].item.url;
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
      // What each leftover takes, in a popup; the picked ones go.
      case 3: openFileCleanup(); break;
      // The switches, together.
      case 4: toggleInstallCheck(state_.skipSignatureCheck); break;
      case 5: toggleInstallCheck(state_.skipFirmwareCheck); break;
      case 6: toggleInstallCheck(state_.makeShortcuts); break;
      case 7:
        toggleInstallCheck(state_.sounds);
        sounds_.setOn(state_.sounds);
        sounds_.play(Cue::Add); // turned on: heard at once
        break;
      case 8:
        bugAsk_ = true;
        bugMessage_.clear();
        break;
      default:
        // A server opens its form; "+ 서버 추가" an empty one.
        openServerForm(selected_ < int(config_.servers.size()) ? std::max(0, selected_) : -1);
      }
    }
    if (a == Action::Left)
      sidebar_ = true;
  } else if (!fullPage() && a == Action::Left)
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
  } else if (thumb.empty() && name == "RetroArch") {
    // RetroArch has no art of its own: the retro menu's mark on a dark tile.
    paint_.rect(x, y, size, size, {38, 38, 44, 255}, 14);
    const int mark = size / 2;
    paint_.icon(35 /* the retro menu icon */, x + (size - mark) / 2.f, y + (size - mark) / 2.f, Butter, mark);
    return;
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
SDL_Color scoreColor(int score) {
  return score >= 90   ? SDL_Color{13, 110, 64, 255}
         : score >= 80 ? SDL_Color{34, 150, 90, 255}
         : score >= 70 ? SDL_Color{118, 140, 28, 255}
         : score >= 50 ? SDL_Color{196, 124, 0, 255}
                       : SDL_Color{204, 62, 52, 255};
}
void Application::scoreBadge(int score, float right, float y) {
  if (score <= 0) return;
  const auto label = std::to_string(score);
  const float w = paint_.fonts.measure(label, 13, true) + 10, h = 19;
  const SDL_Color band = scoreColor(score);
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
  // With several servers, a dot per server holding it on the bottom right.
  if (manyServers()) {
    constexpr float Dot = 12, Step = 10;
    const auto servers = groupServers(g);
    const size_t shown = std::min<size_t>(servers.size(), 4);
    float dx = x + 150 - 8 - Dot - Step * float(shown - 1);
    for (size_t i = 0; i < shown; ++i, dx += Step) {
      paint_.rect(dx - 2, y + 150 - 8 - Dot - 2, Dot + 4, Dot + 4, Surface, int(Dot / 2 + 2));
      paint_.rect(dx, y + 150 - 8 - Dot, Dot, Dot, serverDot(servers[i]), int(Dot / 2));
    }
  }
  if (badges) {
    if (selected) marquee(g.representative.name, x, y + 158, 18, 150, false);
    else paint_.fonts.text(g.representative.name, x, y + 158, 18, Ink, false, 150);
    // Most wanted first; one that would spill past the card is skipped and
    // the next, maybe shorter, still gets its chance. The server's own
    // labels when it words them, else the client's.
    float at = x;
    const Item &labelled = g.base ? *g.base : g.representative;
    // A server that words labels leaves the field out of a game without any.
    if (labelled.hasLabels || !labelBook_.empty()) {
      for (const auto &ref : labelled.labels) {
        const float w = labelChip(ref, labelBook_, at, y + CardLabelTop, x + 150);
        if (w > 0) at += w + 6;
      }
      return;
    }
    for (const auto kind : cardLabels(g)) {
      using L = CardLabel;
      const std::string label = kind == L::KoreanPatch     ? tr("Korean patch|badge")
                                : kind == L::RequiredPatch ? tr("Required patch")
                                : kind == L::Damaged       ? tr("May be damaged")
                                : kind == L::Modified      ? tr("Modified|file")
                                : kind == L::Latest        ? tr("Latest version")
                                : kind == L::Update        ? tr("Update|item")
                                : kind == L::Sale          ? tr("eShop sale")
                                                           : "DLC " + std::to_string(g.dlc.size());
      if (at + paint_.fonts.measure(label, 15, true) + 18 > x + 150) continue;
      at += paint_.pill(label, at, y + CardLabelTop) + 6;
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
  // Until the server first answers the buttons mean little: placeholders,
  // as the menu shows.
  if (menuPending()) {
    (void)left;
    float x = 1248;
    for (const float w : {96.f, 110.f, 96.f, 110.f}) {
      x -= w;
      paint_.skeleton(x, 676, w, 30, 15);
      x -= 24;
    }
    return;
  }
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
  all.push_back({"-", tr("Refresh")});
  all.push_back({"+", tr("Exit")});
  if (1248 - width(all) > left) hints = std::move(all);
  float x = 1248;
  for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
    const auto label = paint_.fonts.measure(it->second, 19), key = paint_.keyWidth(it->first);
    x -= label;
    paint_.fonts.middle(it->second, x, 691, 19, Ink);
    x -= key + 10;
    // The whole bar's height answers; pressed, only the key and its label
    // darken, as a pill around them.
    const float w = key + label + 26;
    hit(x - 8, 660, w, 60, [this, k = it->first] { hintAction(k); }, {x - 8, 691 - 22, w, 44}, 22);
    paint_.key(it->first, x, 691);
    x -= 30;
  }
}
void Application::shell() {
  paint_.rect(0, 0, 232, 660, Surface);
  paint_.rect(231, 0, 1, 660, Line);
  // Centered in the 232px menu column: equal room left and right of the
  // mark and the wordmark's period.
  paint_.logo(19, 24);
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
  // Until the server first answers, the menu is placeholders: 레트로 게임
  // and 홈브류 appear only when it has them, and the rows would jump.
  if (menuPending()) {
    for (int i = 0; i < MenuItems; ++i)
      paint_.skeleton(16, 92 + i * MenuRowPitch, 200, MenuRowHeight, 12);
    return;
  }
  // A screen left open when approval began reads as 홈 in the menu.
  if (!menuOpen(nav_)) nav_ = 0;
  for (int i = 0; i < MenuItems; ++i) {
    if (!menuShown(i)) continue;
    const int y = menuRowTop(i);
    const int height = MenuRowHeight;
    const bool active = i == nav_, open = menuOpen(i);
    if (open)
      // A touched item opens its screen and keeps the focus: what was
      // touched is what is selected, as on every other screen.
      hit(16, y, 200, height, [this, i] {
        if (!showing(MenuScreens[i]))
          openMenuScreen(MenuScreens[i]);
        nav_ = i;
        menuPending_ = false;
        sidebar_ = true;
        autoSidebar_ = false;
      }, {16, float(y), 200, float(height)}, 12); // pressed, the row itself darkens, not a card over its neighbours
    if (sidebar_ && active)
      paint_.ring(16, y, 200, height, 12);
    if (active)
      paint_.rect(16, y, 200, height, Ink, 12);
    const SDL_Color ink = active ? Surface : open ? Ink : SDL_Color{184, 182, 176, 255};
    paint_.icon(iconOf(MenuScreens[i]), 32, y + (height - 26) / 2, ink);
    // 내 게임 counts its updates, so they are not missed without a menu row.
    const bool updates = MenuScreens[i] == Screen::Installed, downloads = MenuScreens[i] == Screen::Downloads;
    const auto count = open && (updates || downloads) ? (updates ? pendingUpdates().size() : downloadsRunning()) : 0;
    // A newer CookingFoil on the server: the same badge on 설정, where it is.
    const bool newer = open && MenuScreens[i] == Screen::Settings && updateNewer_;
    // Longer languages step the size down to fit the menu instead of
    // running past it, or under the badge (at 171) when there is one.
    const float labelRoom = count || newer ? 93 : 136;
    int labelSize = 21;
    while (labelSize > 16 && paint_.fonts.measure(labels[i], labelSize, active) > labelRoom)
      --labelSize;
    paint_.fonts.middle(labels[i], 72, y + height / 2.f, labelSize, ink, active, labelRoom);
    if (newer) {
      paint_.rect(171, y + height / 2 - 14, 28, 28, Butter, 14);
      paint_.fonts.text("1", 185 - paint_.fonts.measure("1", 16, true) / 2, paint_.fonts.centerTop("1", 16, true, y + height / 2),
                        16, Ink, true);
    }
    if (count) {
      paint_.rect(171, y + height / 2 - 14, 28, 28, Butter, 14);
      const auto text = count > 99 ? "99+" : std::to_string(count);
      paint_.fonts.text(text, 185 - paint_.fonts.measure(text, 16, true) / 2,
                        paint_.fonts.centerTop(text, 16, true, y + height / 2), 16, Ink, true);
    }
  }
}
// Shop connection (its state only) and free space, compact on the
// footer's left.
// Returns where it ends so the key hints can stay clear of it.
void Application::noteSignal(int strength) {
  if (strength >= 0) {
    linkSeenUp_ = true;
    linkDropAt_ = 0;
  } else if (linkSeenUp_ && signal_ >= 0) {
    linkDropAt_ = std::max<uint32_t>(1, SDL_GetTicks());
  }
  signal_ = strength;
}
const char *Application::connectionStatus() const {
  const bool offline = error_.message == net::offlineMessage();
  // The Korean status is a key: statusColor() reads it, tr() draws it.
  return linkDropped()                     ? "No internet" // i18n: keep
         : connected_                      ? "Connected" // i18n: keep
         : pairingPending_                 ? "Awaiting approval" // i18n: keep
         : session_ && network_.joinable() ? "Connecting" // i18n: keep
         : offline                         ? "No internet" // i18n: keep
         : session_ && error_              ? "Failed|connection" // i18n: keep
         : session_                        ? "Connecting" // i18n: keep
                                           : "No server"; // i18n: keep
}
float Application::statusBar() {
  const bool offline = error_.message == net::offlineMessage();
  // The console's own link, read once a second (the placeholders' time
  // too, so a drop is told from a link never up): a cut Wi-Fi shows at a
  // glance, before any server error does.
  if (SDL_GetTicks() - signalAt_ >= 1000 || !signalAt_) {
    signalAt_ = SDL_GetTicks();
    noteSignal(net::signalStrength());
  }
  const char *status = connectionStatus();
  constexpr float Mid = 690;
  float x = 28;
  // Until the server first answers: placeholders, as the menu and the
  // buttons show (the link, then the two storages).
  if (menuPending()) {
    for (const float w : {120.f, 210.f, 210.f}) {
      paint_.skeleton(x, Mid - 15, w, 30, 15);
      x += w + 24;
    }
    return x;
  }
  // The link's icon, then the state in its color. A server that answers
  // outranks the console's own check, which can read offline on a working
  // network; a link that dropped after that answer does not.
  const bool cut = (!connected_ || linkDropped()) && (signal_ < 0 || offline);
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
    const auto room = d.free ? trf("{0} free|left", {formatBytes(*d.free)}) : std::string(tr("Unknown"));
    x = paint_.fonts.middle(room, x, Mid, 15, Secondary) + 26;
  }
#ifdef __SWITCH__
  if (!platform::applicationMode()) {
    const auto mode = tr("Applet mode");
    const float w = paint_.fonts.measure(mode, 15, true) + 18;
    paint_.rect(x - 8, Mid - 13, w, 26, {255, 236, 196, 255}, 8);
    paint_.fonts.middle(mode, x + 1, Mid, 15, Pending, true);
    x += w + 10;
  }
#endif
  // On 다운로드 while something comes down: the screen does not dim or
  // sleep (platform::keepAwake), said as the applet mode is.
  if (screen_ == Screen::Downloads && awake_) {
    const auto awake = tr("Screen stays on");
    const float w = paint_.fonts.measure(awake, 15, true) + 18;
    paint_.rect(x - 8, Mid - 13, w, 26, {214, 241, 223, 255}, 8);
    paint_.fonts.middle(awake, x + 1, Mid, 15, Good, true);
    x += w + 10;
  }
  return x;
}
bool Application::refreshSkeleton() const {
  return menuPending() && !fullPage() && screen_ != Screen::Home;
}
// A menu screen's shape while the server is asked: its title, then cards
// for the screens of games, rows for the others.
void Application::screenSkeleton() {
  paint_.skeleton(260, 30, 180, 34, 10);
  const bool cards = screen_ == Screen::Library || screen_ == Screen::Installed || screen_ == Screen::Homebrew ||
                     screen_ == Screen::Retro;
  if (cards) {
    paint_.skeletonCards(260, 104, 12);
    return;
  }
  constexpr float Top = 96, Row = 64, Gap = 10;
  for (int n = 0; n < 8; ++n) paint_.skeleton(260, Top + n * (Row + Gap), 992, Row, 16);
}
void Application::render() {
  poll();
  // An alert that comes up says so once with the error cue.
  if (alert_.empty()) alertHeard_ = false;
  else if (!std::exchange(alertHeard_, true)) sounds_.play(Cue::Error);
  hits_.clear();
  glideAreas_.clear();
  // A screen that returned between clipTo() and unclip() left its clip
  // behind: every frame starts with none, or the menu's hits fall outside.
  clips_.clear();
  glideStep();
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
    if (sidebar_ && menuOpen(nav_) && !showing(MenuScreens[nav_])) {
      openMenuScreen(MenuScreens[nav_]);
      sidebar_ = true;
      autoSidebar_ = false;
    }
  }
  // While the server is asked again every menu screen is placeholders: the
  // cursor waits on the menu (which does not move meanwhile) and comes back
  // in once the answer is there.
  if (refreshSkeleton() && !sidebar_) sidebar_ = autoSidebar_ = true;
  // With nothing to select the cursor waits on the menu; once the screen
  // has something (a list finished loading) it moves in by itself.
  if (!sidebar_ && !hasFocusable() && !managedGame_ && headerFocus_.empty()) {
    // A search that found nothing keeps the cursor on its box, so B clears it.
    if (const auto *own = listQuery(); own && !own->empty()) headerFocus_ = "search";
    else sidebar_ = autoSidebar_ = true;
  }
  else if (sidebar_ && autoSidebar_ && !fullPage() && hasFocusable()) {
    sidebar_ = autoSidebar_ = false;
  }
  pollStorage();
  images_->decode();
  paint_.rect(0, 0, 1280, 720, Ground);
  if (!fullPage())
    shell();
  SDL_Rect clip{fullPage() ? 0 : 232, 0, fullPage() ? 1280 : 1048, 660};
  SDL_RenderSetClipRect(paint_.renderer, &clip);
  if (refreshSkeleton())
    screenSkeleton();
  else if (pairingPending_ && screen_ != Screen::Settings)
    pairing();
  else if (screen_ == Screen::Home)
    home();
  else if (screen_ == Screen::Library)
    library();
  else if (screen_ == Screen::Detail)
    detail();
  else if (screen_ == Screen::RetroDetail)
    retroDetail();
  else if (screen_ == Screen::RetroArch)
    retroArchPage();
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
    footer({{"Y", tr("Write message")}, {"B", tr("Back")}});
  else if (adminFocused())
    footer({{"A", tr("Admin edit")}, {"B", tr("Back")}});
  else if (voteFocus_ && voteTarget()) {
    const auto mine = voteTarget()->votes.mine;
    const bool up = voteFocus_ == 1;
    footer({{"A", mine == (up ? "up" : "down") ? tr("Take back the vote") : up ? tr("Works") : tr("Doesn't work")},
            {"B", tr("Back")}});
  }
  else if (!headerFocus_.empty())
    footer({{"A", headerFocus_.rfind("tab", 0) == 0 ? tr("Next tab") : tr("Select")}, {"B", tr("Back")}}); // on a header button
  else if (screen_ == Screen::Library)
    footer({{"A", tr("Select")}, {"L R", tr("Sort")}, {"B", query_.empty() ? tr("Back") : tr("Clear search")}});
  else if (screen_ == Screen::Detail) {
    if (galleryOpen_) footer({{"L R", tr("Prev / next photo")}, {"B", tr("Close")}});
    else {
      const int items = selection_ ? int(selection_->rows()) : 0;
      const bool onButton = selection_ && part_ > items, onStorage = selection_ && part_ == items;
      double percent = 0;
      const bool onTheWay = onButton && pickedOnTheWay(percent);
      using Hints = std::vector<std::pair<std::string, std::string>>;
      auto hints = descriptionFocus_ ? Hints{{"A", tr("Back to items")}, {"B", tr("Back")}}
                                       : Hints{{"A", onTheWay   ? tr("Cancel download")
                                                     : onButton ? tr("Install")
                                                     : onStorage ? tr("Choose this")
                                                     : part_ == 0 && selection_ && selection_->baseChoices() > 1 &&
                                                               selection_->parts().front().type == "base"
                                                         ? tr("Other file")
                                                         : tr("Toggle")},
                                               {"B", tr("Back")}};
      if (!detail_.screenshots.empty()) hints.insert(hints.end() - 1, {"ZR", tr("Photos")});
      footer(hints);
    }
  }
  else if (screen_ == Screen::RetroDetail) {
    if (galleryOpen_) footer({{"L R", tr("Prev / next photo")}, {"B", tr("Close")}});
    else {
      using Hints = std::vector<std::pair<std::string, std::string>>;
      double percent = 0;
      int queueAt = -1;
      const bool onButton = retroPart_ >= int(retroVersionList().size());
      auto hints = descriptionFocus_ ? Hints{{"A", tr("Back to items")}, {"B", tr("Back")}}
                   : Hints{{"A", !onButton ? std::string(tr("Toggle"))
                                 : retroPageOnTheWay(percent, queueAt) ? std::string(tr("Cancel download"))
                                                                      : retroGetLabel()},
                           {"B", tr("Back")}};
      if (!detail_.screenshots.empty()) hints.insert(hints.end() - 1, {"ZR", tr("Photos")});
      footer(hints);
    }
  }
  else if (screen_ == Screen::RetroArch) {
    const bool onTheWay = retroBusy_ == "pack" || retroArchQueuedAt() >= 0;
    const bool full = retroArchPart_ == 0 && retroArchPresent();
    footer({{"A", onTheWay ? tr("Cancel download") : full ? tr("Get all again")
                  : retroArchPresent() ? tr("Check for updates|retroarch") : tr("Install RetroArch")},
            {"B", tr("Back")}});
  }
  else if (screen_ == Screen::Downloads) {
    using Hints = std::vector<std::pair<std::string, std::string>>;
    if (downloadsTab_ == 0 && !sidebar_ && downloadsRow_ > 0 && downloadsRow_ <= int(installQueue_.size()))
    {
      Hints hints;
      if (downloadsRow_ > 1) hints.push_back({"A", tr("Move up")});
      if (downloadsRow_ < int(installQueue_.size())) hints.push_back({"X", tr("Move down")});
      hints.push_back({"Y", tr("Cancel")});
      hints.push_back({"B", tr("Back")});
      footer(hints);
    }
    else if (downloadsTab_ == 0 && !sidebar_ && (installer_.running() || retroDownloading())) {
      Hints hints{{"A", installer_.running() && installer_.paused() ? tr("Resume") : tr("Pause")}};
      if (runningDetails()) hints.push_back({"X", tr("Details")});
      hints.push_back({"Y", tr("Cancel")});
      hints.push_back({"B", tr("Back")});
      footer(hints);
    }
    else if (downloadsTab_ == 0 && !sidebar_ && queueHeld_ && !installQueue_.empty())
      footer(Hints{{"A", tr("Resume")}, {"B", tr("Back")}});
    else if (downloadsTab_ == 0 || sidebar_ || downloadsList().empty())
      footer({{"B", tr("Back")}});
    else
      footer(Hints{{"A", downloadsTab_ == 3 ? tr("Details") : tr("Download again")}, {"X", tr("Remove")},
                   {"Y", tr("Clear all")}, {"B", tr("Back")}});
  }
  else if (screen_ == Screen::Settings)
    footer({{"A", tr("Select")}, {"B", tr("Back")}}); // Y still rereads config.json; - shows as 새로고침
  else if (screen_ == Screen::Installed)
    // L R and Y are header buttons on this screen; the footer keeps the rest.
    footer(managedGame_ ? std::vector<std::pair<std::string, std::string>>{{"A", tr("Select")}, {"B", tr("Back")}}
           : installed_.games.empty() ? std::vector<std::pair<std::string, std::string>>{{"B", tr("Back")}}
                                      : std::vector<std::pair<std::string, std::string>>{{"A", tr("Manage")}, {"B", tr("Back")}});
  else if (screen_ == Screen::Patches && managingPatch_ && galleryOpen_)
    footer({{"L R", tr("Prev / next photo")}, {"B", tr("Close")}});
  else if (screen_ == Screen::Patches)
    // L R and Y are header buttons, as on 설치된 게임.
    footer(patchRows().empty() || sidebar_
               ? std::vector<std::pair<std::string, std::string>>{{"B", tr("Back")}}
               : std::vector<std::pair<std::string, std::string>>{{"A", tr("Manage patch")}, {"B", tr("Back")}});
  else if (screen_ == Screen::Homebrew && homebrewAsk_ && !homebrewExtras_) {
    const auto *app = homebrewPicked();
    double percent = 0;
    const auto button = !app ? std::string() : app->cardOnly ? std::string()
                        : homebrewOnTheWay(*app, percent) ? std::string(tr("Cancel download")) : homebrewButtonLabel(*app);
    using Hints = std::vector<std::pair<std::string, std::string>>;
    auto hints = homebrewAskRow_ >= 0 ? Hints{{"A", tr("Select")}, {"B", tr("Back")}}
                 : button.empty()      ? Hints{{"B", tr("Back")}}
                                       : Hints{{"A", button}, {"B", tr("Back")}};
    if (app && !app->screenshots.empty()) hints.insert(hints.end() - 1, {"ZR", tr("Photos")});
    if (galleryOpen_) hints = {{"L R", tr("Prev / next photo")}, {"B", tr("Close")}};
    footer(hints);
  }
  else if (screen_ == Screen::Homebrew) {
    // A opens the app's page: what it gets or updates, its facts, patches
    // and saves with 삭제.
    const auto rows = homebrewRows();
    if (rows.empty() || sidebar_)
      footer({{"B", tr("Back")}});
    else {
      const auto state = homebrewStatus(homebrewAt(rows[std::clamp(selected_, 0, int(rows.size()) - 1)]));
      footer({{"A", state == homebrew::Status::Installed ? tr("Details")
                    : state == homebrew::Status::Update  ? tr("Update|action")
                                                         : tr("Get")},
              {"B", tr("Back")}});
    }
  }
  else if (screen_ == Screen::Retro) {
    using Hints = std::vector<std::pair<std::string, std::string>>;
    if (sidebar_)
      footer({{"B", tr("Back")}});
    else if (retroView_ == RetroView::Achievements)
      footer(raPageHints());
    else if (retroView_ == RetroView::List)
      // X is on the search box; the footer keeps A and B.
      footer(retroGames_.empty() ? Hints{{"B", tr("Back")}} : Hints{{"A", tr("Details")}, {"B", tr("Back")}});
    else
      footer(retroFocused() ? Hints{{"A", tr("Details")}, {"B", tr("Back")}}
                            : Hints{{"A", tr("Select")}, {"B", tr("Back")}});
  }
  else if (screen_ == Screen::Saves && savesTab_ == 2 && !managedGame_) {
    using Hints = std::vector<std::pair<std::string, std::string>>;
    if (dropboxSession_) footer({{"B", tr("Cancel")}});
    else if (sidebar_ || !dropboxFocusable()) footer({{"B", tr("Back")}});
    else if (!dropboxLinked_) footer(Hints{{"A", tr("Connect")}, {"B", tr("Back")}});
    else footer(Hints{{"A", dropboxOnAccount_ ? tr("Disconnect") : tr("Manage backups")},
                      {"B", savesQuery_.empty() ? tr("Back") : tr("Clear search")}});
  }
  else if (screen_ == Screen::Saves)
    footer(managedGame_ ? std::vector<std::pair<std::string, std::string>>{{"A", tr("Select")}, {"B", tr("Back")}}
           : sidebar_ || savesTab_ == 2 || (savesTab_ == 1 ? shopSaves_.empty() : installed_.games.empty())
               ? std::vector<std::pair<std::string, std::string>>{{"B", tr("Back")}}
               : std::vector<std::pair<std::string, std::string>>{{"A", savesTab_ != 1 ? tr("Manage saves") : tr("Choose a save")},
                                                                  {"B", tr("Back")}});
  else if (screen_ == Screen::Updates) {
    // Only offer what can happen now.
    // Y is the header button; the footer keeps A and B.
    if (!updateRows().empty())
      footer({{"A", updatesTab_ == 2 ? tr("Install this DLC") : tr("Update this game")}, {"B", tr("Back")}});
    else
      footer({{"B", tr("Back")}});
  }
  else
    footer({{"A", tr("Select")}, {"B", tr("Back")}});
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
    // It comes up a little as it appears, eased, instead of popping in.
    constexpr float RiseMs = 180, Rise = 14;
    const float t = std::min(1.f, float(SDL_GetTicks() - noticeShownAt_) / RiseMs);
    const float x = (fullPage() ? 640 : 756) - w / 2, y = 646 - h + Rise * (1 - t) * (1 - t);
    paint_.rect(x, y, w, h, {22, 22, 26, 235}, int(h / 2 < 25 ? h / 2 : 25));
    paint_.icon(33, x + 22, y + (h - Icon) / 2, Butter, int(Icon));
    if (twoLines)
      paint_.fonts.wrapped(notice_, x + 22 + Icon + 12, y + 12, 18, Surface, text + 4, 2);
    else
      paint_.fonts.middle(notice_, x + 22 + Icon + 12, y + h / 2, 18, Surface);
  }
  dialogs();
  // What the finger is on, darker, as a pressed button on the web.
  if (pressing_) {
    // A popup the tap opened (or closed) is on top now: the pressed card of
    // what was under it would show over the popup, as if it were broken.
    if ((pressedUntil_ && SDL_GetTicks() >= pressedUntil_) || pressedScreen_ != int(screen_) || pressedPopup_ != popupOpen())
      pressing_ = false;
    else {
      // A card a little larger than what was touched, as on the web, not a
      // box drawn tight around it; a button darkens as itself.
      constexpr float Pad = 8;
      SDL_RenderSetClipRect(paint_.renderer, nullptr);
      if (pressedLook_.w > 0)
        paint_.rect(pressedLook_.x, pressedLook_.y, pressedLook_.w, pressedLook_.h, {0, 0, 0, 36}, pressedRadius_);
      else
        paint_.rect(pressed_.x - Pad, pressed_.y - Pad, pressed_.w + 2 * Pad, pressed_.h + 2 * Pad, {0, 0, 0, 36}, 18);
    }
  }
  SDL_RenderPresent(paint_.renderer);
}

void Application::requestInstall() {
  if (!selection_ || detailGroup_ >= groups_.size())
    return;
  // Already on its way: asked whether to cancel it, not to install it again.
  if (double percent = 0; const int onTheWay = pickedOnTheWay(percent)) {
    askCancelPick(onTheWay == 2);
    return;
  }
  if (!selection_->count()) {
    alert_ = tr("Turn on at least one item to install.");
    return;
  }
  confirmInstall_ = true;
  // From several servers it comes from the game's own unless picked.
  const auto servers = selection_->servers();
  if (std::find(servers.begin(), servers.end(), selection_->server) == servers.end())
    selection_->server = servers.empty() ? std::string() : servers.front();
}
} // namespace foil::ui

namespace foil::ui {
void Application::refreshAll() {
  // Reconnects the shop (and rereads config.json), then the console's
  // games and free space; a screen with its own list reloads with it.
  cardChanged();
  images_->forgetFailures();
#ifdef __SWITCH__
  if (!deviceBusy_ && !managedGame_) refreshInstalled();
#endif
  if (screen_ == Screen::Patches) patchError_ = {};
  homebrewLoaded_ = false; // 홈브류 asks the server again when it shows
  raRead_ = false;         // the achievements id and sign-in too, on 레트로 게임
  raGamesAsked_ = false;   // and its games with achievements
  // 레트로 게임 starts over from its first screen and loads again; 업적 stays.
  if (retroView_ != RetroView::Achievements) retroView_ = retroHomeMissing_ ? RetroView::List : RetroView::Home;
  retroShelves_.clear();
  retroGames_.clear();
  retroListSystem_.clear();
  retroQuery_.clear();
  clearRetroFilters();
  retroSortKey_ = "rating";
  retroOnCard_.clear();
  retroRow_ = retroCol_ = 0;
  retroTotal_ = 0;
  retroLoaded_ = false;
  if (screen_ == Screen::Retro) selected_ = 0;
  if (network_.joinable() && !done_) {
    message(tr("Connecting to the server. The lists update when it finishes."));
    return;
  }
  message(tr("Refreshing."));
  refreshing_ = !configPath_.empty();
  // The menu shows its placeholders until the server answers, as at start:
  // what it lists may change with the answer.
  if (!configPath_.empty()) menuKnown_ = false;
  if (!configPath_.empty()) reload();
  else if (screen_ == Screen::Patches) fetchPatches();
}
bool Application::requestQuit() {
  // A console query in flight finishes first; the app then closes by itself.
  if (deviceBusy_) {
    exitAfterDevice_ = true;
    message(tr("Closing as soon as the console task finishes."));
    return false;
  }
  // Paused, its place is already kept: nothing to stop, nothing to ask.
  if (!installer_.running() || installer_.paused())
    return true;
  cancelRetro_ = false;
  confirmCancel_ = true;
  confirmExit_ = true;
  return false;
}
int Application::pickedOnTheWay(double &percent) const {
  percent = 0;
  if (!selection_)
    return 0;
  const auto files = selection_->files();
  if (files.empty())
    return 0;
  const auto row = [&](const Item &f, history::Outcome outcome) {
    return std::any_of(history_.begin(), history_.end(), [&](const history::Entry &e) {
      return e.outcome == outcome && e.kind.empty() && e.install.files.size() == 1 && e.install.files[0].url == f.url;
    });
  };
  // Every picked file waits, runs or came in already (the base before its
  // update), and at least one has yet to finish.
  bool running = false, waiting = false;
  for (const auto &f : files) {
    const bool queued = std::any_of(installQueue_.begin(), installQueue_.end(),
                                    [&](const QueuedInstall &q) { return q.file.url == f.url; });
    const bool now = installer_.running() && row(f, history::Outcome::Running);
    if (!queued && !now && !row(f, history::Outcome::Done))
      return 0;
    running = running || now;
    waiting = waiting || queued;
  }
  if (running)
    percent = installer_.progress().percent;
  return running ? 2 : waiting ? 1 : 0;
}
void Application::installSelected() {
  // Already on its way: A asks whether to cancel it, as 다운로드 does.
  if (double percent = 0; const int onTheWay = pickedOnTheWay(percent)) {
    askCancelPick(onTheWay == 2);
    return;
  }
  if (deviceBusy_) {
    alert_ = tr("Install after the console scan finishes.");
    return;
  }
  if (!selection_ || detailGroup_ >= groups_.size())
    return;
  const auto &g = groups_[detailGroup_];
  // Patches alone (every file off) go in as one pick; with files they come
  // right after the game's files and go with them if the game fails.
  const auto files = selection_->files();
  if (!files.empty()) {
    if (auto error = startInstall(files, g.representative.name, g.representative.icon, selection_->target,
                                  selection_->patchesOn())) {
      alert_ = error.message;
      return;
    }
  } else {
    queuePatchJobs(selection_->patchesOn(), ++installGroups_);
    const bool waits = downloadRunning() || installQueue_.size() > 1;
    if (auto error = launchQueued())
      alert_ = error.message;
    else
      message(waits ? tr("Added to the download queue.") : tr("Download started."));
  }
  // The page stays: more can be picked right after; 다운로드 shows the rest.
  notice_.clear();
}
void Application::saveStateQuietly() { saveOwnState(); }
Error Application::startInstall(const std::vector<Item> &files, const std::string &name, const std::string &icon,
                                const std::string &target, std::vector<UserPatch> patches, bool first) {
  bool added = false;
  if (auto error = enqueueInstall(files, name, icon, target, std::move(patches), first, added))
    return error;
  if (!added) {
    if (!files.empty())
      message(tr("Already in the download queue."));
    return {};
  }
  return launchAdded(1);
}
// Saved once and started once for however many picks went in together.
Error Application::launchAdded(int picks) {
  saveHistory();
  rebuildQueue();
  const bool waits = downloadRunning();
  if (auto error = launchQueued())
    return error;
  sounds_.play(Cue::Add);
  message(picks > 1 ? trf("Added {0} games to the download queue.", {std::to_string(picks)})
          : waits   ? tr("Added to the download queue.")
                    : tr("Download started."));
  return {};
}
// Puts a pick's files in the queue; nothing is saved or started yet.
Error Application::enqueueInstall(const std::vector<Item> &files, const std::string &name, const std::string &icon,
                                  const std::string &target, std::vector<UserPatch> patches, bool first, bool &added) {
  added = false;
  // A file already queued or installing is not added twice; one that failed
  // or was canceled before leaves 실패 and 취소 as it goes in again.
  for (const auto &f : files)
    if (!catalogTitleAllowed(f.titleId) || !catalogTitleAllowed(f.baseId)) {
      logLine("install refused: system title " + f.titleId);
      return {tr("This is a system title, which cannot be installed.")};
    }
  std::vector<Item> fresh;
  for (const auto &f : files) {
    const bool waiting = std::any_of(installQueue_.begin(), installQueue_.end(),
                                     [&](const QueuedInstall &q) { return q.file.url == f.url; }) ||
                         (installer_.running() && std::any_of(history_.begin(), history_.end(), [&](const history::Entry &e) {
                            return e.outcome == history::Outcome::Running && e.kind.empty() && e.install.files.size() == 1 &&
                                   e.install.files[0].url == f.url;
                          }));
    if (!waiting)
      fresh.push_back(f);
  }
  if (fresh.empty())
    return {};
  history_.erase(std::remove_if(history_.begin(), history_.end(),
                                [&](const history::Entry &e) {
                                  return e.kind.empty() &&
                                         (e.outcome == history::Outcome::Failed || e.outcome == history::Outcome::Cancelled) &&
                                         std::any_of(e.install.files.begin(), e.install.files.end(), [&](const Item &old) {
                                           return std::any_of(fresh.begin(), fresh.end(),
                                                              [&](const Item &f) { return f.url == old.url; });
                                         });
                                }),
                 history_.end());
  const int group = ++installGroups_;
  std::vector<uint64_t> ids;
  for (const auto &file : fresh) {
    history::Entry row;
    row.install = {name, icon, target, {file}, file.server.empty() ? serverInUse() : file.server};
    row.outcome = history::Outcome::Queued;
    row.group = group;
    row.at = int64_t(std::time(nullptr));
    ids.push_back(history::add(history_, std::move(row), SIZE_MAX, store_.topId()));
  }
  // Its patches come right after its files, in the same pick: a game that
  // does not install takes them off the queue with the rest.
  queuePatchJobs(patches, group);
  // Ahead of what waits: from the last file to the first each takes the
  // first turn, so they end up in front in the order picked.
  if (first)
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) history::takeFirstTurn(history_, *it);
  added = true;
  return {};
}
// The rows of 다운로드 that wait, in their turn, as the queue the screens and
// the launcher use.
void Application::rebuildQueue() {
  installQueue_.clear();
  for (const auto id : history::queueOrder(history_)) {
    const auto e = std::find_if(history_.begin(), history_.end(), [&](const history::Entry &r) { return r.id == id; });
    if (e == history_.end() || e->install.files.empty())
      continue;
    // An install row is one file; a row of more is from an older build.
    if (e->kind.empty() && e->install.files.size() != 1)
      continue;
    installQueue_.push_back(
        {e->install.files[0], e->install.name, e->install.icon, e->install.target, e->group, e->id, e->kind, e->payload, e->install.server});
    installGroups_ = std::max(installGroups_, e->group);
  }
}
bool Application::queueJob(const std::string &kind, const std::string &name, const std::string &icon,
                           const std::string &url, uint64_t size, const std::string &payload, int group, bool first,
                           const std::string &server) {
  Item file;
  file.url = url;
  file.name = name;
  file.icon = icon;
  file.size = size;
  history::Entry row;
  row.install = {name, icon, "sd", {file}, server.empty() ? serverInUse() : server};
  row.kind = kind;
  row.payload = payload;
  row.outcome = history::Outcome::Queued;
  row.group = group ? group : ++installGroups_;
  row.at = int64_t(std::time(nullptr));
  // RetroArch is one job whatever was asked (all files or what changed).
  const bool waiting = kind == "retroarch" ? std::any_of(history_.begin(), history_.end(), [](const history::Entry &e) {
    return e.kind == "retroarch" && (e.outcome == history::Outcome::Queued || e.outcome == history::Outcome::Running);
  }) : history::pending(history_, row);
  if (waiting) {
    message(tr("Already in the download queue."));
    return false;
  }
  // An older failed or cancelled row of it gives way to this one.
  history_.erase(std::remove_if(history_.begin(), history_.end(),
                                [&](const history::Entry &e) {
                                  return e.kind == kind && e.payload == payload &&
                                         (e.outcome == history::Outcome::Failed || e.outcome == history::Outcome::Cancelled);
                                }),
                 history_.end());
  const auto id = history::add(history_, std::move(row), SIZE_MAX, store_.topId());
  if (first)
    history::takeFirstTurn(history_, id);
  saveHistory();
  rebuildQueue();
  // Patches picked with a game are queued as part of it, before it runs.
  if (group)
    return true;
  const bool waits = downloadRunning() || installQueue_.size() > 1;
  const auto launched = launchQueued();
  if (auto error = launched)
    alert_ = error.message;
  else {
    sounds_.play(Cue::Add);
    // A shortcut made at once says so itself when it is in.
    if (waits || kind != "shortcut") message(waits ? tr("Added to the download queue.") : tr("Download started."));
  }
  return true;
}
void Application::queuePatchJobs(const std::vector<UserPatch> &patches, int group) {
  for (const auto &patch : patches) {
    // "Game · patch name" and the game's cover, as the 패치 screen shows them.
    std::string game, icon;
    patchGame(patch, game, icon);
    queueJob("patch", game + " · " + patch.name, icon, "", patch.size, net::serializePatch(patch), group, false,
             patch.server);
  }
}
bool Application::downloadRunning() const {
  return installer_.running() || retroDownloading() || patchDownloading();
}
void Application::dropGroup(int group, const std::string &why) {
  bool dropped = false;
  std::vector<uint64_t> ids;
  for (const auto &e : history_)
    if (e.outcome == history::Outcome::Queued && e.group == group)
      ids.push_back(e.id);
  for (const auto id : ids)
    dropped |= history::end(history_, id, history::Outcome::Cancelled, why, int64_t(std::time(nullptr)));
  installQueue_.erase(std::remove_if(installQueue_.begin(), installQueue_.end(),
                                     [&](const QueuedInstall &q) { return q.group == group; }),
                      installQueue_.end());
  if (dropped)
    saveHistory();
}
// On the card A pauses or resumes and Y cancels (after asking); on a queued
// row Y takes it off the queue.
void Application::runningAction(Action a) {
  const auto shown = shownQueue(); // the rows below the card
  const int rows = 1 + int(shown.size());
  if (a == Action::Up) downloadsRow_ = std::max(0, downloadsRow_ - 1);
  if (a == Action::Down) downloadsRow_ = std::min(rows - 1, downloadsRow_ + 1);
  downloadsRow_ = std::clamp(downloadsRow_, 0, rows - 1);
  if (downloadsRow_ > 0) {
    const int row = downloadsRow_ - 1;
    if (a == Action::Queue) {
      unqueueAsk_ = int(shown[size_t(row)]);
    }
    if (a == Action::Accept) moveShown(row, true);
    if (a == Action::Search) moveShown(row, false);
    return;
  }
  // X opens its page, when it has one.
  if (a == Action::Search) {
    if (const auto open = runningDetails()) open();
    return;
  }
  if (a == Action::Accept && installer_.running())
    installer_.setPaused(!installer_.paused());
  else if (a == Action::Accept && !(retroDownloading() && retroCancel_)) // stopping: one press is enough
    pauseSmallDownload(!queueHeld_);
  // Paused before it began (held at start): Y takes it off, after asking.
  if (a == Action::Queue && queueHeld_ && !installer_.running() && !retroDownloading() && !installQueue_.empty()) {
    unqueueAsk_ = 0;
    unqueuePick_ = false;
    return;
  }
  if (a == Action::Queue && retroDownloading() && retroCancel_) return; // already stopping
  if (a == Action::Queue && (installer_.running() || retroDownloading())) {
    cancelRetro_ = !installer_.running();
    confirmCancel_ = true;
    confirmExit_ = false;
  }
}
// One place sooner. The rows trade turns (ids), which keep the order on the card.
void Application::moveUp(size_t index) {
  if (index == 0 || index >= installQueue_.size())
    return;
  if (history::swapTurns(history_, installQueue_[index - 1].entry, installQueue_[index].entry)) {
    saveHistory();
    rebuildQueue();
  } else
    std::swap(installQueue_[index - 1], installQueue_[index]); // a preview row with no history
}
// The running one asks as its card on 진행 중 does; a waiting one asks for
// its first file in the queue, and takes the rest of its pick along.
void Application::askCancelPick(bool running) {
  if (running) {
    cancelRetro_ = false;
    confirmCancel_ = true;
    confirmExit_ = false;
    return;
  }
  const auto files = selection_ ? selection_->files() : std::vector<Item>{};
  for (size_t i = 0; i < installQueue_.size(); ++i)
    if (std::any_of(files.begin(), files.end(), [&](const Item &f) { return f.url == installQueue_[i].file.url; })) {
      unqueueAsk_ = int(i);
      unqueuePick_ = true;
      return;
    }
}
void Application::unqueue(size_t index) {
  if (index >= installQueue_.size())
    return;
  const auto gone = installQueue_[index];
  installQueue_.erase(installQueue_.begin() + long(index));
  history::end(history_, gone.entry, history::Outcome::Cancelled, "", int64_t(std::time(nullptr)));
  saveHistory();
  downloadsRow_ = std::min(downloadsRow_, int(installQueue_.size()));
  message(trf("Took {0} off the queue.", {gone.name}));
}
std::string Application::serverInUse() const { return session_ ? session_->server().url : std::string(); }
Server Application::serverFor(const std::string &url) const {
  if (!url.empty())
    for (const auto &server : config_.servers)
      if (server.url == url) return server;
  return session_ ? session_->server() : Server{};
}
SDL_Color Application::serverDot(const std::string &url) const {
  for (size_t i = 0; i < config_.servers.size(); ++i)
    if (config_.servers[i].url == url) return serverColor(config_.servers[i].color, i);
  return serverColor(-1, 0);
}
bool Application::manyServers() const {
  return std::count_if(config_.servers.begin(), config_.servers.end(), [](const Server &s) { return s.enabled; }) > 1;
}
std::vector<std::string> Application::groupServers(const Group &g) const {
  const auto &item = g.base ? *g.base : g.representative;
  std::vector<std::string> servers{item.server.empty() ? serverInUse() : item.server};
  for (const auto &copy : item.alsoOn) servers.push_back(copy.server);
  return servers;
}
void Application::stepInstallServer(int by) {
  const auto servers = selection_->servers();
  if (servers.size() < 2) return;
  const auto at = std::find(servers.begin(), servers.end(), selection_->server);
  const int now = at == servers.end() ? 0 : int(at - servers.begin()), count = int(servers.size());
  selection_->server = servers[size_t((now + by + count) % count)];
  sounds_.play(Cue::Move);
}
std::vector<Server> Application::serversWith(const std::string &feature) const {
  std::vector<Server> servers;
  if (!session_) return servers;
  // No feature: every server read now.
  const auto offers = [&](const std::string &url) {
    const auto found = serverFeatures_.find(url);
    return found != serverFeatures_.end() && (feature.empty() || found->second.count(feature));
  };
  if (offers(session_->server().url)) servers.push_back(session_->server());
  for (const auto &other : others_)
    if (offers(other->server().url)) servers.push_back(other->server());
  return servers;
}
net::Session *Application::sessionFor(const std::string &url) const {
  for (const auto &other : others_)
    if (other->server().url == url) return other.get();
  return session_.get();
}
bool Application::readOthers(bool primaryChanged) {
  incomingStatus_.clear();
  if (others_.empty()) return false;
  if (primaryChanged) serverItems_[session_->server().url] = session_->takeItems();
  bool changed = primaryChanged;
  std::set<std::string> ids;
  if (!session_->serverId().empty()) ids.insert(session_->serverId());
  for (const auto &other : others_) {
    const auto &url = other->server().url;
    Error error;
    const bool read = other->refresh(error);
    // Two addresses of one server (its own and a tunnel's): listed once.
    if (read && !other->serverId().empty() && !ids.insert(other->serverId()).second) {
      incomingStatus_[url] = "same";
      changed = serverItems_.erase(url) > 0 || changed;
      continue;
    }
    if (!read) {
      incomingStatus_[url] = other->pairingPending() ? "pairing" : "failed";
      if (!other->pairingPending()) logLine("server " + url + " not read: " + error.message);
      continue; // its games from before stay until it answers again
    }
    incomingStatus_[url] = "";
    if (!other->unchanged()) {
      serverItems_[url] = other->takeItems();
      changed = true;
    }
  }
  return changed;
}
Error Application::launchQueued() {
  // One download at a time, whatever it is; a paused one holds the rest, and
  // so does the question at start until it is answered.
  if (installQueue_.empty() || downloadRunning() || queueHeld_ || resumeAsked())
    return {};
  auto next = std::move(installQueue_.front());
  installQueue_.pop_front();
  if (!next.kind.empty())
    return launchJob(next);
  std::vector<Item> files{next.file};
  // A server shortcut or a homebrew title queued by an earlier build has no
  // format, and its address ends in the server's route (…/download): it is
  // an NSP all the same.
  if (files[0].format.empty()) {
    const auto path = files[0].url.substr(0, files[0].url.find_first_of("?#"));
    const bool route = path.size() >= 9 && path.compare(path.size() - 9, 9, "/download") == 0;
    if (route || forwarder::shortcutId(forwarder::parseHexId(files[0].titleId))) files[0].format = "nsp";
  }
  const auto row = std::find_if(history_.begin(), history_.end(),
                               [&](const history::Entry &e) { return e.id == next.entry; });
  // Room as last read off the screen; unknown, it is not asked here (a full
  // card's query takes seconds): the console refuses a write that does not fit.
  const auto known = next.target == "nand" ? nandFree_ : sdFree_;
  if (auto error = installer_.start(files, next.name, next.icon, next.target,
                                    serverFor(next.server), state_,
                                    known ? known : std::optional<uint64_t>(std::numeric_limits<uint64_t>::max()))) {
    // It cannot start (no room, firmware): the rest of its pick goes too.
    history::end(history_, next.entry, history::Outcome::Failed, error.message, int64_t(std::time(nullptr)));
    dropGroup(next.group, error.message);
    return error;
  }
  runningGroup_ = next.group;
  jobStartMs_ = lastByteMs_ = SDL_GetTicks();
  jobStartBytes_ = lastBytes_ = net::receivedBytes();
  jobStartedAt_ = int64_t(std::time(nullptr));
  if (row != history_.end()) {
    row->outcome = history::Outcome::Running;
    row->at = int64_t(std::time(nullptr));
  } else {
    history::Entry entry;
    entry.install = {next.name, next.icon, next.target, files, next.server};
    entry.at = int64_t(std::time(nullptr));
    history::add(history_, std::move(entry), SIZE_MAX, store_.topId());
  }
  saveHistory();
  observedInstall_ = install::Status::Running;
  return {};
}
// Its turn came: a homebrew app, a retro game, RetroArch, a patch or a
// shortcut starts, and its row reads as running until it ends.
Error Application::launchJob(const QueuedInstall &next) {
  const auto row = [&] {
    return std::find_if(history_.begin(), history_.end(), [&](const history::Entry &e) { return e.id == next.entry; });
  };
  const auto failed = [&](const std::string &why) {
    history::end(history_, next.entry, history::Outcome::Failed, why, int64_t(std::time(nullptr)));
    saveHistory();
    return Error{why};
  };
  if (row() == history_.end())
    return {};
  // Running first: the worker finds its row by kind when it ends. Its clock
  // starts here, for 완료's average speed and time.
  jobStartMs_ = SDL_GetTicks();
  jobStartedAt_ = int64_t(std::time(nullptr));
  row()->outcome = history::Outcome::Running;
  row()->at = int64_t(std::time(nullptr));
  bool started = false;
  if (next.kind == "homebrew") {
    auto apps = homebrew::parse("{\"homebrew\":[" + next.payload + "]}", "");
    // Queued before the server rebuilt it: today's entry for it (its id and
    // addresses may have changed), so it does not fetch a file now gone.
    if (!apps.empty() && homebrewLoaded_)
      if (const auto now = homebrewLike(apps.front()); now != homebrew_.end() && now->id != apps.front().id) {
        logLine("homebrew: " + apps.front().name + " is listed under a new id; fetched from there");
        apps.front() = *now;
      }
    started = !apps.empty() && runHomebrew(apps.front(), serverFor(next.server));
  } else if (next.kind == "retro") {
    started = runRetroGame(retro::parseGameJson(next.payload), serverFor(next.server));
  } else if (next.kind == "retroarch") {
    started = runRetroArch(next.payload == "full", serverFor(next.server));
  } else if (next.kind == "reapply") {
    // Puts back what went missing, downloading it again when the server
    // still has the patch: a download like any other, so it waits its turn.
    auto patches = net::parsePatches(next.payload);
    for (auto &patch : patches) patch.server = next.server;
    if (!patches.empty() && !patchWorker_.joinable() && !installer_.running() && !retroDownloading()) {
      const bool onServer = std::any_of(serverPatches_.begin(), serverPatches_.end(),
                                        [&](const UserPatch &p) { return p.id == patches.front().id; });
      started = startPatchReapply(patches.front(), onServer);
    }
  } else if (next.kind == "patch") {
    auto patches = net::parsePatches(next.payload);
    for (auto &patch : patches) patch.server = next.server;
    if (!patches.empty() && patchStore_ && session_) {
      patchQueue_.push_back(patches.front());
      started = true;
      pumpPatchQueue();
    }
  } else if (next.kind == "shortcut") {
    // Made here from the app's file (or this app's own): no download, only
    // NCM, which is free now.
    if (next.payload == "self") {
      makeShortcutFromNro(forwarder::loaderPath(selfPath_).substr(1), "", "CookingFoil", "", "", "", false);
      history::end(history_, next.entry, history::Outcome::Done, "", int64_t(std::time(nullptr)));
      started = true;
    } else
      started = startShortcut(next); // its row ends when it is made

  }
  if (!started)
    return failed(tr("Could not start the download."));
  saveHistory();
  return {};
}
void Application::settlePatchJob(bool failed, const std::string &error) {
  const auto running = std::find_if(history_.begin(), history_.end(), [](const history::Entry &e) {
    return (e.kind == "patch" || e.kind == "reapply") && e.outcome == history::Outcome::Running;
  });
  if (running == history_.end())
    return;
  if (!failed && !running->install.files.empty()) {
    running->bytes = running->install.files.front().size;
    running->startedAt = jobStartedAt_;
    running->downloadSeconds = (SDL_GetTicks() - jobStartMs_) / 1000.0;
  }
  history::end(history_, running->id, failed ? history::Outcome::Failed : history::Outcome::Done,
               failed ? failureText(error) : error, int64_t(std::time(nullptr)));
  saveHistory();
}
// Why a download failed, for its row: a login the server refused when a
// fresh download token was asked for says so (and once on screen), since
// trying again would not help.
std::string Application::failureText(const std::string &error) {
  if (!session_ || !net::downloadSignInRefused(session_->server().url)) return error;
  const std::string text = tr("The server refused the login. Check the server's username and password in Settings.");
  message(text);
  return text;
}
// 다운로드's records live in cookingfoil.db; the downloads.json of earlier
// versions moves in once and goes.
void Application::openDownloads(const std::string &sd) {
  makeDirectories(std::filesystem::path(sd + "/switch/cookingfoil").string());
  std::string error;
  const bool opened = store_.open(sd + "/switch/cookingfoil/cookingfoil.db", error);
  if (!opened) {
    logLine("downloads: " + error);
    history_.clear();
    return;
  }
  history_ = store_.load(HistoryPage);
  installer_.loadResumePoints(store_.resumePoints());
  checkResumePoints();
  const auto old = sd + "/switch/cookingfoil/downloads.json";
  if (history_.empty() && std::filesystem::exists(old)) {
    history_ = history::parse(readFile(old));
    if (store_.save(history_, error))
      std::filesystem::remove(old);
    else
      logLine("downloads: " + error);
  }
  // Written back only when settling changed a row: a write rewrites the
  // whole file, seconds on a slow card, and the records read right after
  // (the server settings) would wait on it. Settling only changes running
  // rows.
  const bool running = std::any_of(history_.begin(), history_.end(),
                                   [](const history::Entry &e) { return e.outcome == history::Outcome::Running; });
  history::settle(history_);
  if (running) saveHistory();
}
// The points were written beside the console's placeholders, which can go
// without them (a system cleanup, another installer); a point with nothing
// to resume into is dropped, and the window files no point owns go. Windows
// written in this run are never touched.
void Application::checkResumePoints() {
  if (resumeCheckWorker_.joinable()) return;
  resumeChecked_ = store_.resumePoints();
  std::vector<std::string> keys;
  for (const auto &[key, point] : resumeChecked_) keys.push_back(key);
  const auto started = std::filesystem::file_time_type::clock::now();
  resumeCheckWorker_ = std::thread([this, keys, started] {
    const platform::Behind behind; // runs while the splash is up
    ResumeLeftovers leftovers;
    guarded("resume check", [&] {
      const auto there = platform::placeholdersThere(keys);
      const std::string dir = sdRoot_ + "/switch/cookingfoil/resume";
      std::vector<std::string> names;
      std::error_code ec;
      for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.last_write_time(ec) < started && !ec) names.push_back(entry.path().filename().string());
      leftovers = resumeLeftovers(keys, [&](const std::string &key) -> std::optional<bool> {
        const auto found = there.find(key);
        return found == there.end() ? std::nullopt : std::optional<bool>(found->second);
      }, names);
      for (const auto &name : leftovers.windows) std::filesystem::remove(dir + "/" + name, ec);
      if (!leftovers.windows.empty())
        logLine("resume: removed " + std::to_string(leftovers.windows.size()) + " window file(s) no point owns");
      // Placeholders this app made whose point is gone (its file deleted, a
      // cut before the first one was kept) go, so nothing is left on the
      // console for good; one with a point stays to be gone on from. Not
      // while an install runs: it may be making one just now.
      std::vector<std::string> orphans;
      for (const auto &key : store_.madePlaceholders())
        if (std::find(keys.begin(), keys.end(), key) == keys.end() && !installer_.resumePoint(key)) orphans.push_back(key);
      if (!orphans.empty() && !installer_.running()) {
        const auto dropped = platform::dropPlaceholders(orphans);
        for (const auto &key : dropped.gone) store_.dropMadePlaceholder(key);
        if (!dropped.gone.empty())
          logLine("resume: cleared " + std::to_string(dropped.gone.size()) + " placeholder(s) no point owns, " +
                  std::to_string(dropped.bytes >> 20) + " MB");
        resumeCleared_ = dropped.bytes;
      }
    });
    resumeStale_ = std::move(leftovers.points);
    resumeCheckDone_ = true;
  });
}
void Application::pollResumeCheck() {
  if (!resumeCheckDone_.exchange(false)) return;
  resumeCheckWorker_.join();
  // Said only when something was there to clear.
  if (const uint64_t freed = std::exchange(resumeCleared_, 0))
    message(trf("Cleared {0} left by downloads that did not finish.", {formatBytes(freed)}));
  // An install that began meanwhile may have made the point again.
  for (const auto &key : std::exchange(resumeStale_, {})) {
    const auto now = installer_.resumePoint(key);
    const auto then = resumeChecked_.find(key);
    if (!now || then == resumeChecked_.end() || now->plan != then->second.plan || now->url != then->second.url) continue;
    installer_.forgetResumePoint(key);
    logLine("resume: " + key + " has no placeholder on the console; it starts over");
  }
  resumeChecked_.clear();
}
// Every five seconds (or right away once something changed what is on the
// card), on a worker; the frame only picks up what it found.
void Application::pollStorage() {
  if (storageDone_.exchange(false)) {
    storageWorker_.join();
    auto found = std::exchange(storageFound_, {});
#ifdef __SWITCH__
    sdFree_ = found.sdFree;
    nandFree_ = found.nandFree;
    if (found.sdTotal) sdTotal_ = found.sdTotal;
    if (found.nandTotal) nandTotal_ = found.nandTotal;
#endif
    // Read before the card last changed: stale.
    if (found.generation == cardGeneration_)
      for (const auto &[path, there] : found.files) cardSeen_[path] = there;
    // After the first query (or on a PC, where there is nothing to ask) a
    // missing number means unknown, not "still loading".
    storageChecked_ = true;
  }
  if (storageWorker_.joinable() || (cardAsked_.empty() && storageAt_ && SDL_GetTicks() - storageAt_ <= 5000)) return;
  storageAt_ = std::max<uint32_t>(1, SDL_GetTicks());
  // RetroArch from the first run on, so the retro screens open knowing it.
  std::set<std::string> asked = std::exchange(cardAsked_, {});
  asked.insert(retro::RetroArchApp);
  for (const auto &[path, there] : cardSeen_) asked.insert(path);
  std::vector<std::string> paths(asked.begin(), asked.end());
  const bool totals = !sdTotal_ || !nandTotal_;
  storageWorker_ = std::thread([this, paths = std::move(paths), totals, sd = sdRoot_, generation = cardGeneration_] {
    StorageFound found;
    found.generation = generation;
#ifdef __SWITCH__
    found.sdFree = platform::freeSpace("sd");
    found.nandFree = platform::freeSpace("nand");
    if (totals) {
      found.sdTotal = platform::totalSpace("sd");
      found.nandTotal = platform::totalSpace("nand");
    }
#else
    (void)totals;
#endif
    for (const auto &path : paths) {
      std::error_code ec;
      found.files[path] = std::filesystem::exists(sd + "/" + path, ec);
    }
    storageFound_ = std::move(found);
    storageDone_ = true;
  });
}
void Application::cardChanged() {
  ++cardGeneration_;
  storageAt_ = 0;
}
std::optional<bool> Application::cardKnows(const std::string &path) const {
  const auto seen = cardSeen_.find(path);
  if (seen != cardSeen_.end()) return seen->second;
  cardAsked_.insert(path);
  return std::nullopt;
}
void Application::saveHistory() {
  if (!store_.isOpen())
    return;
  {
    std::lock_guard<std::mutex> held(historyMutex_);
    if (historyClosing_)
      return;
    historyPending_ = history_;
  }
  wakeHistoryWriter();
}
void Application::wakeHistoryWriter() {
  historyWake_.notify_one();
  if (historyWriter_.joinable())
    return;
  historyWriter_ = std::thread([this] {
    // The whole records file is built and written here: never on the
    // drawing core, where a finished download would stall the screen.
    const platform::OnCore off(platform::WriterCore);
    for (;;) {
      std::optional<std::vector<history::Entry>> list;
      std::map<std::string, std::optional<install::Installer::ResumePoint>> points;
      {
        std::unique_lock<std::mutex> held(historyMutex_);
        historyWake_.wait(held, [this] {
          return historyPending_ || !resumePending_.empty() || statePending_ || ledgerPending_ || historyClosing_;
        });
        if (!historyPending_ && resumePending_.empty() && !statePending_ && !ledgerPending_)
          return; // closing, nothing left
        list = std::exchange(historyPending_, std::nullopt);
        points = std::exchange(resumePending_, {});
      }
      writePendingState();
      writePendingLedger();
      std::string error;
      if (list && !store_.save(*list, error))
        logLine("downloads: " + error);
      for (const auto &[key, point] : points)
        point ? store_.saveResumePoint(key, *point) : store_.forgetResumePoint(key);
      if (!points.empty())
        ++resumeWritten_;
    }
  });
}
void Application::flushHistory() {
  {
    std::lock_guard<std::mutex> held(historyMutex_);
    historyClosing_ = true;
  }
  historyWake_.notify_one();
  if (historyWriter_.joinable())
    historyWriter_.join();
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
  // A record from before the queue was kept in 다운로드: into the queue.
  if (const auto pending = parsePending(state_.queueJson)) {
    forgetInstall();
    if (installQueue_.empty())
      startInstall(pending->files, pending->name, pending->icon, pending->target);
  }
  if (installQueue_.empty())
    return;
  // Only HOME Menu shortcuts left: the last step of downloads that finished
  // (the app closed right after their message). They go on by themselves;
  // nothing the player left part way is waiting.
  if (std::all_of(installQueue_.begin(), installQueue_.end(), [](const QueuedInstall &q) {
        return q.kind == "shortcut" || forwarder::shortcutId(forwarder::parseHexId(q.file.titleId));
      })) {
    if (auto error = launchQueued()) logLine("resume: shortcuts wait, " + error.message);
    return;
  }
  // Asked, now that it can go on: A picks the queue up where it stopped;
  // B leaves it paused in 다운로드, where A goes on with it.
  confirmResume_ = true;
}
} // namespace foil::ui
namespace foil::ui {
void Application::languageDialog() {
  constexpr float Row = 60, Gap = 8;
  const auto body = modal(820, 16 + LanguageRows * (Row + Gap) + 8,
                          // " · Language" stays in English, as on 설정's row.
                          std::string(tr("Language")) + (language() == Language::English ? "" : " · Language"),
                          tr("Close"), "", "", 17);
  const float w = (body.w - 48 - Gap * 2) / 2;
  for (int i = 0; i < LanguageCount; ++i) {
    const float x = body.x + 24 + (i / LanguageRows) * (w + Gap * 2), y = body.y + 16 + (i % LanguageRows) * (Row + Gap);
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
} // namespace foil::ui
