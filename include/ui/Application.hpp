#pragma once
#include <ctime>
#include "core/Catalog.hpp"
#include "core/Selection.hpp"
#include "install/Installer.hpp"
#include "net/Api.hpp"
#include "net/Dropbox.hpp"
#include "net/HomebrewApi.hpp"
#include "ui/DropboxRows.hpp"
#include "ui/Images.hpp"
#include "ui/Painter.hpp"
#include "ui/Job.hpp"
#include "ui/Qr.hpp"
#include "platform/Seal.hpp"
#include "ui/Sounds.hpp"
#include "core/Installed.hpp"
#include "core/SaveBackups.hpp"
#include "core/Retro.hpp"
#include "core/DownloadHistory.hpp"
#include "core/DownloadStore.hpp"
#include "core/ShopSaves.hpp"
#include "core/ClientUpdate.hpp"
#include "core/Patches.hpp"
#include "core/ProdKeys.hpp"
#include "core/Votes.hpp"
#include "core/CardPatches.hpp"
#include "core/I18n.hpp"
#include "core/AdminEdit.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <cmath>
#include <unordered_map>
#include "core/HomebrewCard.hpp"
#include <utility>
namespace foil::ui {
// A review score's colour, the cards' badge and the banner's tag alike.
SDL_Color scoreColor(int score);
// Description scroll unit for d-pad and right stick, in pixels.
constexpr int ScrollStep = 40;
constexpr int MenuItems = 9;
constexpr int PopupFilterColumns = 2; // filter grid width in the view popup
constexpr SortMode SortOrders[] = {SortMode::Recent,  SortMode::Popular, SortMode::Score,
                                   SortMode::Release, SortMode::Updated, SortMode::Name,
                                   SortMode::Largest, SortMode::Default};
enum class Screen { Home, Library, Detail, Downloads, Updates, Settings, Installed, Patches, Retro, Saves, Homebrew, RetroDetail, RetroArch };
// The menu, top to bottom; 설정 stays last (menuOpen() relies on it).
// Browsing first (the shop, retro games), then this console's own (내 게임
// with its updates and DLC as tabs, saves, patches), then downloads and
// settings. 업데이트 is a tab of 내 게임 and shares its row; 홈브류 sits
// right above 유저 패치.
constexpr Screen MenuScreens[MenuItems] = {Screen::Home,      Screen::Library,  Screen::Retro,
                                           Screen::Installed, Screen::Saves,    Screen::Homebrew,
                                           Screen::Patches,   Screen::Downloads, Screen::Settings};
// The menu's rows, one after another; rows a server does not offer are left out.
constexpr int MenuRowHeight = 52, MenuRowPitch = 58;
// The menu row of a screen; screens outside the menu (detail) read as 홈.
int menuIndex(Screen screen);
// The menu's name for a screen, translated (the detail page's back link).
const char *screenLabel(Screen screen);
enum class Action {
  Up,
  Down,
  Left,
  Right,
  Accept,
  Back,
  Search,
  Queue,
  Previous,
  Next,
  Reload,
  Preview,
  ScrollUp,
  ScrollDown,
  Extra // ZL: a page's extra (a retro game's achievements)
};
int themeIcon(const Theme &theme);
class Application {
public:
  // The approval message kept for a server address (tests and the form).
  std::string pairMessageFor(const std::string &url) const {
    const auto found = state_.pairMessages.find(url);
    return found == state_.pairMessages.end() ? "" : found->second;
  }
  Application(SDL_Renderer *renderer, std::string configPath = "",
              std::string statePath = "", std::string fixture = "");
  ~Application();
  bool open();
  void setItems(std::vector<Item> items);
  void setGroups(std::vector<Group> groups); // already grouped (groupCatalog)
  void setSession(std::unique_ptr<net::Session> session);
  void refresh();
  // Downloads that find their token run out get a fresh one from this server.
  void renewTokensWith(const Server &server);
  bool pairingPending() const { return pairingPending_; }
  const net::PairingInfo &pairingInfo() const { return pairingInfo_; }
  bool loading() const { return detailLoading_; }
  void screen(Screen screen);
  void action(Action action);
  void search(std::string query);
  void render();
  void poll();
  bool imagesReady() const { return images_->ready(); }
  Screen current() const { return screen_; }
  size_t groupCount() const { return groups_.size(); }
  std::string selectedName() const;
  void setDetail(net::Detail detail);
  bool confirmingInstall() const { return confirmInstall_; }
  std::string detailId() const { return detailGroup_ < groups_.size() ? groups_[detailGroup_].id : ""; }
  // Patches picked with a game install that have not started yet.
  size_t queuedPatches() const { return patchQueue_.size(); }
  // The light check of the card: whether its thread runs, and what it found.
  bool patchChecking() const { return patchCheckWorker_.joinable(); }
  const std::map<std::string, PatchHealth> &patchHealth() const { return patchHealth_; }
  // Whether a patch job shows as busy anywhere (its row, 다운로드).
  bool patchJobShown() const { return !patchBusy_.empty() || patchDownloading(); }
  uint64_t selectedBytes() const {
    return selection_ ? selection_->total() : 0;
  }
  std::string selectedTarget() const {
    return selection_ ? selection_->target : "sd";
  }
  std::string query() const {
    return screen_ == Screen::Retro ? retroQuery_ : screen_ == Screen::Saves ? savesQuery_
           : screen_ == Screen::Patches ? patchQuery_ : screen_ == Screen::Installed ? installedQuery_
           : screen_ == Screen::Updates ? updatesQuery_ : screen_ == Screen::Homebrew ? homebrewQuery_ : query_;
  }
  // Set when the sidebar search item is chosen; the platform opens a keyboard.
  bool takeSearchRequest() { return std::exchange(searchRequested_, false); }
  void requestSearch() { searchRequested_ = true; }
  // A line of text the app asks for (a field of the server form): the
  // platform opens its keyboard and hands what was typed to enterText().
  struct TextRequest {
    std::string guide, initial;
    bool password = false; // typed hidden
    int maxLength = 256;
  };
  std::optional<TextRequest> takeTextRequest() { return std::exchange(textRequest_, std::nullopt); }
  // Goes to the field that asked last (a server form field or the approval
  // note); nothing happens once its form closed.
  void enterText(const std::string &text);
  bool requestQuit();
  // Where this .nro runs from (argv[0]); 설정 > 앱 업데이트 replaces it.
  // Opened from the album without a shortcut, it offers to make one.
  void setSelfPath(std::string path) {
    selfPath_ = std::move(path);
    noticeRollBack();
    considerSelfShortcut();
  }
  // - on any screen: the shop, the console's games and free space again.
  void refreshAll();
  // The right stick pressed in: the list's first row, the view following.
  void scrollToTop();
  bool shouldQuit() const {
    return exitRequested_ || (exitAfterCancel_ && !installer_.running()) ||
           (exitAfterDevice_ && !deviceBusy_);
  }
  bool managingGame() const { return managedGame_.has_value(); }
  bool viewingScreenshots() const { return galleryOpen_; }
  bool confirmingRemoval() const { return removal_.has_value(); }
  const std::string &alert() const { return alert_; }
  size_t installedCount() const { return installed_.games.size(); }
#ifndef __SWITCH__
  void previewInstall(install::Progress p) { installer_.preview(std::move(p)); }
  void previewQueued(Item file, std::string name, std::string icon) {
    installQueue_.push_back({std::move(file), std::move(name), std::move(icon), "sd", 0, 0, "", ""});
  }
  // The queue as tests drive it: picks go in as from a game's page, and the
  // installer's work on the host is whatever setHostInstall() says.
  Error previewPick(const std::vector<Item> &files, const std::string &name, std::vector<UserPatch> patches = {}) {
    return startInstall(files, name, "", "sd", std::move(patches));
  }
  // A game paused before it began, its history row `entry`, on the top card.
  void previewHeld(Item file, std::string name, uint64_t entry) {
    installQueue_.push_back({std::move(file), std::move(name), "", "sd", 0, entry, "", ""});
    queueHeld_ = true;
  }
  // The top card's 상세 정보, pressed: false when the card has none.
  bool previewTopDetails() {
    TopCard card;
    if (!topCardNow(installer_.progress(), card) || !card.onDetails) return false;
    card.onDetails();
    return true;
  }
  void previewQueueOpen() {
    resumeOffered_ = true;
    connected_ = true;
  }
  // The start once the server answers: what was cut off is asked about.
  void previewOfferResume() {
    connected_ = true;
    offerResume();
  }
  bool previewResumeAsked() const { return resumeAsked(); }
  bool previewPaused() const { return installer_.paused(); }
  void previewPause(bool paused) { installer_.setPaused(paused); }
  bool previewGetHomebrew(const homebrew::App &app) { return getHomebrew(app); }
  homebrew::Status previewHomebrewStatus(const homebrew::App &app) const { return homebrewStatus(app); }
  // The queue's rows in their turn, by name; what runs now; how a row ended.
  std::vector<std::string> queuedNames() const {
    std::vector<std::string> out;
    for (const auto &q : installQueue_) out.push_back(q.name);
    return out;
  }
  std::string installingName() const { return installer_.running() ? installer_.progress().name : ""; }
  bool otherDownloadRunning() const { return retroDownloading() || (patchWorker_.joinable() && !patchBusy_.empty()); }
  std::optional<history::Outcome> outcomeOf(const std::string &name) const {
    for (const auto &e : history_)
      if (e.install.name == name) return e.outcome;
    return std::nullopt;
  }
  void previewHistory(std::vector<history::Entry> entries) { history_ = std::move(entries); }
  void previewRecord(history::Entry entry) { history_.push_back(std::move(entry)); }
  int downloadsTab() const { return downloadsTab_; }
  void previewStorage(uint64_t sd, uint64_t nand, uint64_t sdTotal = 476ULL << 30,
                      uint64_t nandTotal = 26ULL << 30) {
    sdFree_ = sd;
    nandFree_ = nand;
    sdTotal_ = sdTotal;
    nandTotal_ = nandTotal;
  }
  void previewPairing(net::PairingInfo info) {
    pairingInfo_ = std::move(info);
    pairingPending_ = true;
    qr_.encode(pairingInfo_.url);
  }
  void previewDeviceBusy(bool busy) {
    deviceBusy_ = busy;
    deviceFixture_ = true;
  }
  void previewFeatures(std::set<std::string> features) { features_ = std::move(features); }
  // A retro game's page as the server would fill it, without a server.
  void previewRetroDetail(retro::Detail detail) {
    openRetroDetail(detail.game);
    retroMeta_ = std::move(detail.meta);
    retroVariants_ = std::move(detail.variants);
    retroDetailLoading_ = false;
  }
  // An admin console without a server: these fields, changed in place.
  void previewAdminEdit(std::vector<admin::Field> fields) {
    features_.insert("admin-edit");
    adminPreview_ = std::move(fields);
  }
  // prod.keys as an admin console sees it, without a server: this holding.
  void previewKeys(prodkeys::Status status) {
    features_.insert("keys-upload");
    keysPreview_ = std::move(status);
  }
  bool askingSelfShortcut() const { return selfShortcutAsk_; }
  bool askingShortcutRemoval() const { return shortcutRemoveAsk_; }
  bool appPopupOpen() const { return homebrewAsk_; }
  int appPopupRow() const { return homebrewAskRow_; }
  bool hasShortcut(uint64_t id) const { return shortcuts_.count(id) > 0; }
  // 홈브류 with its card scan finished (the worker joined).
  bool homebrewCardScanned() {
    poll();
    return homebrewScanned_ && !homebrewCardWorker_.joinable();
  }
  // 홈브류 without a server: the menu item and this list.
  // 패치's shop list in the preview, as the server would send it.
  void previewPatches(std::vector<UserPatch> patches) {
    serverPatches_ = std::move(patches);
    patchesLoaded_ = true;
    patchesFixed_ = true;
  }
  void previewHomebrew(std::vector<homebrew::App> apps) {
    features_.insert("homebrew");
    homebrew_ = std::move(apps);
    homebrewLoaded_ = true;
  }
  void previewInstalled(InstalledLibrary library, std::vector<SaveEntry> saves = {}) {
    installed_ = std::move(library);
    installed_.games = sortInstalled(std::move(installed_.games), installedSort_);
    saves_ = std::move(saves);
    deviceFixture_ = true;
    deviceError_ = {};
  }
  // 세이브's backups on the card without a card (a read running is dropped).
  void previewBackups(std::map<uint64_t, saves::BackupSummary> summary) {
    if (backupSummaryWorker_.joinable()) backupSummaryWorker_.join();
    backupSummaryDone_ = false;
    backupSummary_ = std::move(summary);
  }
  // Which games have a save on the console (the card's read leaves it unknown).
  void previewSavedGames(std::set<uint64_t> ids) { savedGames_ = std::move(ids); }
  std::string savesOrder() const { return savesSortLabel(); }
  // The console's link read as `strength` (-1 offline), as the footer reads it.
  void previewSignal(int strength) {
    noteSignal(strength);
    signalAt_ = SDL_GetTicks();
  }
  std::string connectionLabel() const { return connectionStatus(); }
  std::string saveRowName(size_t i) const {
    const auto rows = saveGameRows();
    return i < rows.size() ? installed_.games[size_t(rows[i])].name : "";
  }
  std::string managedName() const { return managedGame_ ? managedGame_->name : ""; }
  // 세이브 > 드롭박스 without Dropbox: linked with `remote`, or not linked;
  // a `signIn` address shows the QR code waiting for the phone.
  void previewDropbox(bool linked, net::DropboxRevisions remote = {}, const std::string &signIn = "") {
    dropbox_ = std::make_unique<net::Dropbox>("", &stop_);
    dropboxRead_ = true;
    dropboxLinked_ = dropboxListed_ = linked;
    dropboxAccount_ = linked ? "player@example.com" : "";
    dropboxRemote_ = std::move(remote);
    connected_ = true;
    dropboxSession_.reset();
    if (!signIn.empty()) {
      dropboxSession_ = net::dropbox::BridgeSession{"preview", "preview", signIn, "", 0};
      dropboxQr_.encode(signIn);
      dropboxPollAt_ = dropboxExpiresAt_ = SDL_GetTicks() + 15 * 60 * 1000;
    }
  }
  bool dropboxPopupOpen() const { return dropboxOpen_ || dropboxUnlinkAsk_; }
  bool dropboxOnAccount() const { return dropboxOnAccount_; }
  bool dropboxChoosingUpload() const { return dropboxChoosing_; }
  std::string dropboxAcceptLabel() const { return dropboxPopupAccept(); }
  std::vector<DropboxRow> dropboxListed() const { return dropboxGameRows(); }
  uint64_t dropboxFocused() const { return dropboxCursor_; }
  bool confirmingRestore() const { return saveConfirm_ == SaveConfirm::Restore; }
  bool askingApply() const { return applyAsking_; }
  void previewApplying(shopsaves::ShopSave save) { applying_ = std::move(save); }
  int savesTab() const { return savesTab_; }
  const std::string &headerFocus() const { return headerFocus_; }
  bool onMenu() const { return sidebar_; }
  bool serverFormOpen() const { return serverForm_.has_value(); }
  const Config &config() const { return config_; }
  size_t activeServer() const { return serverIndex_; }
#endif
  // Where installs go: 설정's choice, else config.json's.
  std::string installTarget() const {
    return state_.installTarget.empty() ? config_.installTarget : state_.installTarget;
  }
  // Where a menu row starts; hidden rows take no room.
  int menuRowTop(int item) const {
    int y = 92;
    for (int i = 0; i < item; ++i)
      if (menuShown(i)) y += MenuRowPitch;
    return y;
  }
  // 내 게임's tab: 0 설치됨, 1 업데이트, 2 DLC.
  int myGamesTab() const { return screen_ == Screen::Updates ? updatesTab_ : 0; }
  void decode() { images_->decode(); }
  // Switches the interface language and redraws what was built from text.
  // Flips one of the install checks in 설정 and saves state.json.
  void toggleInstallCheck(bool &skip);
  void saveOwnState();
  // 설정's options card: 언어, 기본 저장 위치, 앱 업데이트, 남은 파일 정리,
  // then the switches: 서명 검사, 펌웨어 검사, 홈 메뉴 바로가기, 오류 로그 전송.
  static constexpr int SettingsOptions = 9;
  float settingsScroll_ = 0; // how far 설정 is scrolled to keep the cursor in view
  void chooseLanguage(Language language);
  // Genre names without a session (the PC preview's fixtures).
  void setGenreTable(net::NameTable table) { genreTable_ = std::move(table); genreNames_ = net::namesIn(genreTable_, languageCode()); }
  // Where the PC preview keeps its fake SD card for user patches.
  void usePatchFolder(const std::string &sd);
  void openCardRecords(const std::string &sd);
  void openPatchStore(const std::string &sd);
  // The preview's picture cache, where the console keeps it beside its state.
  void useCacheFolder(std::string folder) { cacheFolder_ = std::move(folder); }
  // Touch: a tap runs the topmost area drawn last frame; a drag scrolls.
  void touch(float x, float y);
  void drag(float dx, float dy);
  void fingerDown(); // a finger lands: a coasting list stops, and that touch opens nothing
  // Where it lands: only a touch on the coasting list itself is spent on
  // stopping it, and what is under the finger shows pressed until it lifts.
  void fingerDown(float x, float y);
  // A finger starts moving (past the tap slop) from x, y; dx, dy is its first
  // move, which picks sideways (a shelf, the banner) or up and down.
  void dragBegin(float x, float y, float dx = 0, float dy = 1);
  // A finger lifts. `speedX`, `speedY` (px/s) when the caller measured the
  // flick itself (the web preview); otherwise from the moves.
  void dragEnd(float speedX = NAN, float speedY = NAN);
  bool gliding() const { return glide_.active; }
  // The right stick: moves the screen's main scrolling area (a popup's list
  // when one is open) without the cursor. `amount` -1 (up) to 1 (down).
  void stick(float amount, float seconds);
  float glidePos() const { return glide_.pos; } // tests
  // User patches: the server's list joined with what the SD store holds.
  struct PatchRow {
    UserPatch patch;
    std::optional<PatchRecord> record;
    bool onServer = false;
    std::string game, icon, installedVersion;
    int64_t installedNumber = -1; // the installed update's version number
    bool installed = false;
    int shopGroup = -1; // the game in the shop's catalog, if it is there
    // Files on the card no record of this app accounts for (scanCardPatches):
    // a patch of some kind is there, which one cannot be told. Shown only.
    bool onCard = false;
  };
  // Tabs over the list; L R or a tap switches.
  // Starts on the games this console has; L R widens to every patch.
  // Tabs: 설치됨 (on the SD card), 전체, 새 버전.
  enum class PatchFilter { Stored, All, Updates };
  static constexpr int PatchFilters = 3;

private:
  void takeNetwork();
  std::vector<homebrew::App>::iterator homebrewLike(const homebrew::App &old); // homebrew_.end(): none
  std::optional<homebrew::App> homebrewOpenLike_; // a page asked for by an app known from before
  // Until the server first answers (or while the records are still being
  // read), the menu and the buttons are placeholders.
  bool refreshSkeleton() const;
  void screenSkeleton();
  bool menuPending() const { return !menuKnown_ && (network_.joinable() || stateWaiting_); }
  void onTheWayButton(float x, float w, bool downloading, double percent, double skipped = 0);
  // False when there was nothing new to write.
  bool keepResumeChanges();
  // Leaving the screen (HOME, sleep): a running download's place goes to
  // the card now rather than at its next save; waits a moment for it.
  void keepDownloadPlace();
  void requestRetroDetail(const std::string &id);
  void startRetroDetail();
  void moveRetroVariant(int step);
  void serverRestartNotice();
  void askCancelPick(bool running);
  // A retro game's other versions: a stack and how many. Dark and small on
  // a cover; light, as a label pill, in a list row. Returns its width.
  float versionsBadge(int count, float x, float y, bool dark);
  // A trophy and 업적 40, butter when the ROM itself earns them, grey when
  // only its title matched; nothing (0) for a game without any.
  float achievementLabel(const achievements::Summary &a, float x, float top, float height, int size);
  void shell();
  float statusBar();
  // Before the shop approves this device only 홈 (the approval page) and
  // 설정 open; the rest need the shop.
  // 레트로 게임 and 홈브류 show only when the server has some.
  bool menuShown(int item) const {
    switch (MenuScreens[item]) {
    case Screen::Retro: return features_.count("retro");
    case Screen::Homebrew: return features_.count("homebrew");
    default: return true;
    }
  }
  bool menuOpen(int item) const {
    return menuShown(item) && (!pairingPending_ || item == 0 || item == MenuItems - 1);
  }
  std::set<std::string> features_; // the server's /api/shop/info features
  bool menuKnown_ = false;          // the server has answered once: menuShown() holds
  std::string thisDevice_; // oc-save-keeper's id for this console, read when a save panel opens
  // 홈브류: the server's apps beside what this console holds (the ledger).
  std::vector<homebrew::App> homebrew_;
  homebrew::Ledger homebrewLedger_;
  std::map<std::string, bool> homebrewOnCard_; // ledger file -> on the SD card
  // The .nro files under switch/ as the last card scan found them, which
  // of them each listed app owns (app id -> homebrewCard_ index), and the
  // ones no listed app owns as rows of their own (after homebrew_ in the
  // row indexes).
  std::vector<homebrew::CardNro> homebrewCard_;
  std::map<std::string, size_t> homebrewCardOf_;
  std::vector<homebrew::App> homebrewLocal_;
  bool homebrewScanned_ = false;
  std::thread homebrewCardWorker_;
  std::atomic<bool> homebrewCardDone_{false};
  // A removed app's own folder, deleted off the drawing thread: it can hold
  // thousands of files (a game's data). Its name, and the error when it failed.
  std::thread homebrewFolderWorker_;
  std::atomic<bool> homebrewFolderDone_{false};
  struct HomebrewRemoved {
    bool file = false;              // the .nro went
    std::vector<uint64_t> shortcuts; // the shortcuts that went
    std::string shortcutError, folderError;
  };
  HomebrewRemoved homebrewRemoved_;
  homebrew::App homebrewRemovedApp_;
  void settleHomebrewRemoval();
  // The folder the app's file has to itself ("switch/<folder>"), offered for
  // removal with it; empty when there is none.
  std::string homebrewOwnFolder(const homebrew::App &app) const;
  std::vector<homebrew::CardNro> homebrewCardFound_; // the worker's result, taken on the main thread
  homebrew::Ledger homebrewLedgerLooked_;             // the ledger the worker checked
  homebrew::LedgerCheck homebrewLedgerFound_;          // and what it found
  void takeHomebrewLedgerCheck();
  void scanHomebrewCard();
  void matchHomebrewCard();
  const homebrew::App &homebrewAt(int index) const; // a homebrewRows() index
  std::string homebrewIcon(const homebrew::App &app); // the URL or the card icon's key
  std::string homebrewFile(const homebrew::App &app) const; // its file on the card, from the SD root
  std::string homebrewEtag_, homebrewQuery_;
  // 홈브류's order: the server's (most recently updated) unless one is picked.
  SortMode homebrewSort_ = SortMode::Updated;
  bool homebrewSortAsk_ = false;
  int homebrewSortRow_ = 0;
  Error homebrewError_;
  bool homebrewLoaded_ = false, homebrewRemoveAsk_ = false, homebrewAsk_ = false;
  int homebrewTab_ = 0; // 0 설치됨, 1 도구, 2 포트
  // The app popup's rows under its facts (-1 the app itself) and the
  // patch (1) or save (2) picker opened from one; 0 when none is open.
  int homebrewAskRow_ = -1, homebrewExtras_ = 0, homebrewExtraRow_ = 0;
  bool homebrewSaveAsk_ = false; // applying one of the app's saves, asked first
  const homebrew::App *homebrewPicked() const;
  std::vector<int> homebrewExtraKinds(const homebrew::App &app) const;
  std::vector<PatchRow> homebrewPatchRows(const homebrew::App &app) const;
  std::vector<int> homebrewSaveRows(const homebrew::App &app) const; // indexes into homebrewSaves_
  void homebrewExtrasAction(Action a);
  void homebrewExtrasDialog();
  void homebrewSaveDialog();
  void applyHomebrewSave(const shopsaves::ShopSave &save);
  // 패치 and 세이브 > 공유 name apps' patches and saves from 홈브류's list:
  // asks for it once when the server has 홈브류 and it is not here yet.
  void needHomebrewList();
  // The popup before an app's save from 공유 goes into switch/<app>/.
  bool shopAppSaveAsk_ = false;
  void appSaveConfirm(const shopsaves::ShopSave &save);
  std::thread homebrewWorker_;
  std::atomic<bool> homebrewDone_{false};
  std::function<void()> homebrewFinish_;
  void loadHomebrew();
  void pollHomebrew();
  void homebrewScreen();
  void homebrewAction(Action a);
  void openHomebrewSort();
  void launchManagedGame();
  void rememberShortcutKey(const std::string &appId, const std::string &key);
  bool homebrewShortcutStale(const homebrew::App &app) const;
  void homebrewSortAction(Action a);
  void homebrewSortDialog();
  void homebrewRemoveDialog();
  // 관리자 편집 (feature admin-edit): the page's item, its chip on the dark
  // band and the editor it opens (AdminEditor.cpp).
  struct AdminTarget {
    std::string kind, id, name; // kind: homebrew, retro or game
  };
  struct AdminEdit {
    bool open = false, busy = false, changed = false;
    AdminTarget target;
    std::vector<admin::Field> fields;
    int row = 0;
    Error error;
    std::string confirm; // a change waiting on 확인: hiding the item from every list
  };
  std::optional<AdminTarget> adminTarget() const;
  bool adminFocused() const;
  bool adminChipAction(Action a, bool atTop);
  float adminChip(); // its left edge, or the band's right edge when not drawn
  void openAdminEdit();
  void sendAdminChange(const std::string &body);
  void adminEditAction(Action a);
  void adminEditDialog();
  AdminEdit adminEdit_;
  bool adminFocus_ = false;
  // 잘 돼요 / 안 돼요 on a homebrew or retro page: chips on the band beside
  // the admin one, reached the same way (up from the page's top). A vote
  // sent shows its answer until the lists are read again.
  struct VoteTarget {
    std::string kind, id;
    votes::Votes votes;
  };
  std::optional<VoteTarget> voteTarget() const;
  bool voteAction(Action a, bool atTop);
  float voteChips(float right); // their left edge, or `right` when not drawn
  void castVote(const std::string &vote);
  // "잘 돼요 5 · 안 돼요 1" for a row; "" when nobody voted yet.
  static std::string voteSummary(const votes::Votes &votes);
  int voteFocus_ = 0; // 0 none, 1 잘 돼요, 2 안 돼요
  std::map<std::string, votes::Votes> votesCast_; // "kind/id"
  Job voteJob_;
  Job adminJob_;
  std::optional<std::vector<admin::Field>> adminPreview_;
  // HOME Menu shortcuts (platform/Shortcut): the ids the console has, as the
  // last card scan found them and as made or removed since.
  std::set<uint64_t> shortcuts_;
  std::set<uint64_t> homebrewShortcutsFound_; // the card worker's result
  std::vector<uint64_t> homebrewShortcutsAsked_; // the ids it looked for
  bool shortcutsUnasked() const;
  std::atomic<bool> signaturePatchesFound_{true};
  bool signaturePatches_ = true; // seen on the card at the last scan; a shortcut needs them to open
  bool shortcutRemoveAsk_ = false; // X on the app popup's shortcut row, asked first
  bool selfShortcutAsk_ = false;   // this app's own, offered with the applet mode notice
  bool selfShortcutClosed_ = false; // closed this run
  // The app popup's shortcut row: only for an app on the card, after its
  // 패치 and 세이브 rows.
  bool homebrewShortcutRow(const homebrew::App &app) const;
  // The ids an app's shortcut can have: the one made here for its file, and
  // the server's (0 when it has none).
  uint64_t homebrewShortcutId(const homebrew::App &app) const;
  uint64_t homebrewServerShortcut(const homebrew::App &app) const;
  bool homebrewHasShortcut(const homebrew::App &app) const;
  // A shortcut made here, in its queue turn: the server's picture as its
  // icon (the .nro's when that cannot be had). payload: a homebrew app's
  // JSON, "retro:" and a game's, or "self".
  void queueShortcut(const std::string &name, const std::string &icon, const std::string &payload);
  bool makeHomebrewShortcutHere(const homebrew::App &app); // queued; false where none can be made
  bool makeRetroShortcutHere(const retro::Game &game);
  std::string shortcutApp(const history::Entry &entry) const; // the app id, "" when none
  bool shortcutFailed(const std::string &titleId); // the server's would not come: made here (false: cannot)
  void removeHomebrewShortcut(const homebrew::App &app);
  void removeShortcuts(const std::vector<uint64_t> &ids, const std::string &name); // those the console has
  // After a download, when 설정 asks for shortcuts: made here when the
  // console can, else the server's.
  bool autoHomebrewShortcut(const homebrew::App &app);
  // The shortcut with the name and icon of the .nro at `file` (SD-root
  // relative) that starts `launch` (empty: `file` itself) with `extra` after
  // its path in argv. Says how it went; false on failure.
  bool makeShortcutFromNro(const std::string &file, const std::string &launch, const std::string &name,
                           const std::string &extra, const std::string &nameOverride,
                           const std::string &authorOverride, bool justDownloaded);
  void shortcutRemoveDialog();
  void selfShortcutDialog();
  void selfShortcutAction(Action a);
  // At start in the album: offers this app's shortcut once (replaces the
  // applet mode notice, which it repeats).
  void considerSelfShortcut();
  void checkHomebrewCard();
  homebrew::Status homebrewStatus(const homebrew::App &app) const;
  // The installed title a title app (homebrew::App::title) is, or nullptr.
  const InstalledGame *homebrewInstalledTitle(const homebrew::App &app) const;
  std::vector<int> homebrewRows() const; // homebrewAt() indexes in this tab and search
  bool getHomebrew(const homebrew::App &app);
  bool runHomebrew(const homebrew::App &app);
  // The app's HOME Menu shortcut: made now, or queued behind a download
  // (NCM is the installer's while it runs); false when none can be made.
  bool homebrewShortcutPossible(const homebrew::App &app) const;
  void requestHomebrewShortcut(const homebrew::App &app);
  void removeHomebrew(const homebrew::App &app, bool folder = false);
  void settleHomebrew(const homebrew::App &app, const std::optional<homebrew::CardFile> &seen, const std::string &fileSha,
                      const std::vector<std::string> &removed);
  // Older copies of the app the download replaces, for its worker to delete.
  std::vector<std::string> homebrewStale(const homebrew::App &app) const;
  void saveHomebrewLedger();
  // The app's file list (homebrew::fileListPath) goes with the next ledger
  // write; an empty one removes it.
  void queueFileList(const std::string &id, const std::vector<std::string> &files);
  void writePendingLedger();
  void home();
  // The home's first view asked for ahead of it: the banner's pictures and
  // the first two rows' covers (while the start's last word waits).
  void warmHome();
  void library();
  // Every list's search box sits at the header's right end, one size on all
  // of them; filters and tabs go left of it.
  static constexpr float SearchW = 220, SearchX = 1252 - SearchW;
  // A header item: X, a tap, or the d-pad. 세이브 and 유저 패치 draw it
  // narrower, left of their tabs.
  void searchBar(const std::string &query, float x = SearchX, float w = SearchW);
  // Each list screen keeps its own search: 세이브 (내 기기, 공유), 유저 패치,
  // 설치된 게임 and 업데이트.
  std::string savesQuery_, patchQuery_, installedQuery_, updatesQuery_;
  std::string *listQuery(); // this screen's, or nullptr (모든 게임 and 레트로 have their own)
  std::vector<int> gameRows(const std::string &query) const; // installed_.games matching
  // 이 기기's games in the order Y picks (savesSort_[0]).
  std::vector<int> saveGameRows() const;
  std::vector<int> saveGameRows(const std::string &query) const;
  // Y on each tab: the order (and on 서버 "installed only"), each tab its
  // own. 이 기기: latest backup, name, updated, most backups. 서버:
  // installed first, installed only, name, recently added. 드롭박스: latest
  // backup, name, this console's games added (DropboxView).
  int savesSort_[3] = {0, 0, 0};
  static constexpr int SavesSorts = 4, DropboxSorts = 3;
  std::string savesSortLabel() const;
  void cycleSavesSort();
  // The labels shrink (down to 14pt) when the strip would start left of
  // leftLimit. `counts`, when given, puts a small grey label after a
  // label whose count is above 0.
  float headerTabs(const std::vector<std::string> &labels, int current, const std::function<void(int)> &pick,
                   float right = 1252, float leftLimit = 0, const std::vector<int> &counts = {});
  std::string updatesFocus_; // the file the cursor is on in 업데이트/DLC, kept as rows leave
  int updatesTab_ = 1; // 1 업데이트, 2 DLC (내 게임's tabs after 설치됨)
  void showMyGamesTab(int tab);
  float myGamesTabs(float right, float reserve);
  float myGamesButtonWidth(const std::string &label);
  std::vector<UpdateCandidate> updateRows() const;
  void settings();
  void installedScreen();
  void management();
  void removalConfirm(); // the delete question over 게임 관리
  void installedAction(Action action);
  // first runs on the worker before the read (console calls a frame must
  // not wait for), or here when no read can start.
  void refreshInstalled(std::function<void()> first = nullptr);
  void cycleInstalledSort(int step);
  void pollDevice();
  void openManagement();
  void performRemoval();
  // 설치된 게임 > 게임 관리 > a save: back up, restore and trash backups in
  // oc-save-keeper's folders (SavesPanel.cpp).
  void openSave(int index);
  void savePanel();
  // Whose save a backup holds, and where and why it was made, for its row.
  std::string backupDetail(const saves::LocalBackup &backup) const;
  // 레트로 게임: a home of shelves, one per system, best rated first
  // (RetroHome.cpp); R or 모두 보기 opens a system's whole list, where L R
  // moves between systems and X searches (RetroScreen.cpp).
  enum class RetroView { Home, List, Achievements };
  void retroScreen();
  void retroHome();
  void retroHeader();
  void retroList();
  void retroBanner(float y);
  void retroAction(Action action);
  void retroHomeAction(Action action);
  void loadRetroHome();
  void loadRetroPage(bool next);
  void openRetroList(int system);
  // A shelf near its end takes the system's next page (best rated first),
  // the games it already shows left out; it stops once all have come.
  void loadShelfMore(int row);
  std::map<std::string, int> shelfPage_;   // per system: the list pages taken so far
  std::set<std::string> shelfDone_;        // systems whose shelf holds all it can
  std::string shelfWant_;                  // asked while the list worker was busy
  // A retro game's page (RetroDetail.cpp), as a shop game has one.
  void openRetroDetail(const retro::Game &game);
  // RetroArch's own page (RetroArchPage.cpp): its state, and the button that
  // gets it or checks it for new files.
  void openRetroArchPage();
  void retroArchPage();
  void retroArchPageAction(Action action);
  int retroArchQueuedAt() const;
  int retroArchPart_ = 1;      // 0 the 전체 다시 받기 row, 1 the button
  size_t retroArchFiles_ = 0;  // files the base pack record lists
  std::future<size_t> retroArchCount_; // that count, read off the main thread
  void retroDetail();
  void retroDetailPanel();
  void retroDetailAction(Action action);
  int retroCoreCard(int y);
  float retroInfoCard(float y);
  std::vector<retro::Variant> retroVersionList() const;
  std::string retroGetLabel() const;
  void getRetroPick(); // the button: get it, or ask to cancel it on its way
  int retroOnTheWay(const retro::Game &game, double &percent) const;
  // The page's button: 2 while this game or a checked version downloads, 1
  // while one waits (`queueAt` its place in the queue), else 0.
  int retroPageOnTheWay(double &percent, int &queueAt) const;
  // A page the menu gives way to: a game's, shop or retro.
  bool fullPage() const {
    return screen_ == Screen::Detail || screen_ == Screen::RetroDetail || screen_ == Screen::RetroArch ||
           (screen_ == Screen::Homebrew && homebrewPageOpen());
  }
  // An app's page in 홈브류 (HomebrewDetail.cpp): open while its card is, and
  // behind the questions it asks (delete it, delete its shortcut).
  bool homebrewPageOpen() const { return homebrewAsk_ || homebrewRemoveAsk_ || shortcutRemoveAsk_; }
  void homebrewDetail();
  void homebrewDetailPanel(const homebrew::App &app);
  std::string homebrewCardVersion(const homebrew::App &app) const;
  int homebrewOnTheWay(const homebrew::App &app, double &percent) const;
  int homebrewQueuedAt(const homebrew::App &app) const;
  void homebrewButton(const homebrew::App &app);
  std::string homebrewButtonLabel(const homebrew::App &app) const;
  Screen homebrewReturn_ = Screen::Homebrew; // where B on the page goes
  std::string failureText(const std::string &error);
  // false when it did not start (another retro job runs, no room, already there).
  bool getRetroGame(const retro::Game &game, bool again = false);
  // Its turn in the queue came: the download itself, on the retro worker.
  bool runRetroGame(const retro::Game &game);
  bool finishRetroEntry(const std::string &kind, history::Outcome outcome, const std::string &error, uint64_t bytes, uint64_t kept = 0);
  // A retro game or RetroArch downloading (not a list loading).
  bool retroDownloading() const { return !retroBusy_.empty(); }
  int signal_ = 3;          // net::signalStrength(), refreshed by statusBar
  uint32_t signalAt_ = 0;
  // The console's link going down after it was up (out of sleep, out of
  // range), and the server's last answer: a drop newer than the answer
  // turns 연결됨 off until either comes back. A console that never reads
  // online (its connection test blocked) never drops.
  bool linkSeenUp_ = false;
  uint32_t linkDropAt_ = 0, serverAnsweredAt_ = 0;
  void noteSignal(int strength);
  bool linkDropped() const { return connected_ && linkDropAt_ && linkDropAt_ > serverAnsweredAt_; }
  const char *connectionStatus() const; // the footer's state, an untranslated key
  bool installRetroShortcut(const retro::Game &game);
  bool installShortcut(const std::string &name, const std::string &icon, const std::string &url,
                       const std::string &titleId, uint64_t size, bool justDownloaded = true);
  const retro::Game *retroFocused() const;
  void scoreBadge(int score, float right, float y);
  // Into the queue; `first` puts it ahead of what waits (a game's core).
  bool syncRetroArch(bool full = false, bool first = false); // full: every file again, settings kept
  bool runRetroArch(bool full);
  bool retroCoreMissing(const retro::Game &game) const;
  bool retroReady(const retro::Game &game) const; // RetroArch and its core on the card
  bool retroArchPending() const;                   // a RetroArch job waits or runs
  void pollRetro();
  // Runs `job` on the retro worker; what it returns runs on the UI thread.
  bool runRetroJob(const std::string &busy, std::function<std::function<void()>()> job);
  bool retroInstalled(const retro::Game &game) const;
  // detail: the get popup, which also names the version a flag stands for.
  std::vector<std::string> retroLabels(const retro::Game &game, bool detail = false) const;
  bool retroArchAsking_ = false;
  // RetroAchievements (레트로 게임's X): the id the server keeps, read once a
  // run (again after 새로고침) with the card's sign-in in retroarch.cfg; the
  // popup that signs in on the console; a game's achievements from its page.
  achievements::Account raAccount_;
  bool raRead_ = false;        // asked this run
  std::string raSignedIn_;     // retroarch.cfg's signed-in user, "" when none
  bool raOpen_ = false;        // the sign-in popup
  int raRow_ = 0;              // its focus: 0 id, 1 password, 2 sign in (signed in: 0 the button)
  int raField_ = -1;           // the field the keyboard types into
  std::string raUser_, raPassword_; // the password lives only until the sign-in runs
  Error raError_;
  // The account's jobs (read, sign in and out, the phone link) one at a time;
  // a game's list on its own, so neither waits on the other.
  Job raJob_, raListJob_;
  bool raListOpen_ = false;    // a game's achievements
  int raListRow_ = 0;
  std::string raListGame_;     // whose list is shown or coming
  std::string raListAsked_;    // whose list was asked for (a fetch for another game finishes first)
  achievements::Game raList_;
  Error raListError_;
  // The phone sign-in: a key made for one QR and kept only in memory; the
  // console waits for the phone's sealed envelope one long poll at a time.
  struct RaLink {
    seal::Key key;
    achievements::LinkStart start;
    std::string url;
    std::string note;   // why the phone is asked again, "" while it is first awaited
    uint32_t until = 0; // SDL ticks when the QR expires
  };
  std::optional<RaLink> raLink_; // the page shows its QR
  std::atomic<bool> raLinkStop_{false};
  Qr raQr_;
  bool raRunning() const { return raJob_.busy(); }
  void readRaAccount();
  void openRaPopup();
  void raAction(Action a);
  void raDialog();
  void raSignIn();
  void raSignOut();
  // Both sign-ins end here: the console is signed in as `user`; `serverError`
  // says the server did not keep the id.
  void raSignedIn(const std::string &user, const Error &serverError);
  void raPhone();          // opens the QR
  void raLinkWait();       // one long poll for the phone's envelope
  void raLinkAgain(const std::string &code, const std::string &note); // the phone asks again
  void raLinkClose();      // drops the QR and its key
  // 업적 (X on the retro home): how to sign in, then the account and the
  // server's retro games with achievements and the player's progress.
  achievements::Games raGames_;
  Error raGamesError_;
  bool raGamesAsked_ = false; // asked this run (새로고침 and a sign-in ask again)
  Job raGamesJob_;
  int raPageRow_ = 0;         // 0 the account or the connect card, then the games
  int raConnectButton_ = 0;   // on the connect card: 0 the phone, 1 the keyboard
  void openRaPage();
  void leaveRaPage();
  void raPage();
  void raPageAction(Action a);
  void raGamesFetch();
  void raConnectCard(float y);             // not signed in: the steps and the buttons
  void raLinkCard();                       // the QR while the phone is awaited
  void raAccountRow(float y, bool loading); // signed in: the account, its points and sign-out
  void raGamesPage();                      // either of those, then the games, scrolling as one
  // What to do when there is nothing to show yet, set out as a manual
  // page: an icon, what is missing, why, and numbered steps.
  struct RaGuide {
    int icon = 33;
    std::string title, body;
    std::vector<std::string> steps;
    bool bad = false;
  };
  std::optional<RaGuide> raGuide() const;
  float raGuideHeight(const RaGuide &guide);
  void raGuideCard(float y, const RaGuide &guide);
  std::vector<std::pair<std::string, std::string>> raPageHints() const;
  float raHeaderButton(float right); // the header's X 업적; returns its left edge
  float raChip(float right);         // the game page's 업적 chip, left of `right`; its left edge
  void openRaList();
  void raListFetch(); // asks for raListGame_'s list once nothing else is being fetched
  void raListAction(Action a);
  void raListDialog();
  bool raModal() const { return raOpen_ || raListOpen_; }
  void retroArchConfirm(); // Y with RetroArch already there
  // On the card from before the shop had its bezel: getting it again adds it.
  bool retroBezelMissing(const retro::Game &game) const;
  bool retroArchPresent() const;
  // 세이브: the console's games, each opening its saves (SavesScreen.cpp).
  void savesScreen();
  void savesAction(Action action);
  // 세이브's second tab: saves the shop offers, applied to an account.
  void shopSavesList();
  void loadShopSaves();
  void pollShopSaves();
  void chooseShopSave();
  void applyShopSave();
  void applyConfirm();
  void saveNeedsGameDialog();
  void marquee(const std::string &text, float x, float y, int size, float width, bool bold = true);
  // Per name: when it started running and when it was last drawn.
  std::map<std::string, std::pair<uint32_t, uint32_t>> marquees_;
  static constexpr int SavesTabs = 3;
  int savesTab_ = 0; // 0 the console's games, 1 the server's saves, 2 Dropbox
  std::vector<shopsaves::ShopSave> shopSaves_, incomingShopSaves_;
  std::vector<shopsaves::ShopSave> homebrewSaves_; // apps' saves, for the app's popup in 홈브류
  // A server save's game or app: the name and icon 공유 shows, and whether
  // it is on this console (an app 홈브류 does not list counts as here).
  std::string shopSaveTitle(const shopsaves::ShopSave &save) const;
  std::string shopSaveIcon(const shopsaves::ShopSave &save) const;
  bool shopSaveTargetHere(const shopsaves::ShopSave &save) const;
  void sortShopSaves(); // games and apps here first, then by name, each one's saves together
  bool shopSavesLoaded_ = false, shopSavesMissing_ = false, incomingShopSavesMissing_ = false;
  Error shopSavesError_, incomingShopSavesError_;
  // The app's records held by a write still going at start (a slow card):
  // the servers show at once and a worker reads them again until it can,
  // then reload goes on (stateWaiting_ meanwhile, from stateWaitAt_).
  std::thread stateWorker_;
  std::atomic<bool> stateReady_{false};
  bool stateWaiting_ = false;
  uint32_t stateWaitAt_ = 0;
  int statePrefer_ = -1;
  void waitForState(int prefer);
  std::thread shopSavesWorker_;
  std::atomic<bool> shopSavesDone_{false};
  int shopSaveRow_ = 0;
  bool savePicking_ = false; // the picker for a game with several server saves
  int savePickRow_ = 0, shopSavePick_ = 0; // picker row; the save chosen (index into shopSaves_)
  std::vector<std::pair<int, int>> shopSaveGroups() const;
  void chooseShopGame();
  void savePickDialog();
  bool saveNeedsGame_ = false; // a server save picked whose game is not installed
  int saveGameGroup_ = -1;     // that game in the catalog, or -1
  std::optional<shopsaves::ShopSave> applying_; // waiting for its account
  bool applyAsking_ = false, applyRunning_ = false;
  int applySave_ = -1;
  // 세이브's third tab: link Dropbox through the shop's bridge (a QR code
  // for the phone), then each game's backups go up to Dropbox or come down
  // into the console's backups, in oc-save-keeper's layout (DropboxScreen.cpp).
  void dropboxTab();
  std::vector<DropboxRow> dropboxGameRows() const; // the search's games, in order
  int dropboxFocus(const std::vector<DropboxRow> &rows) const; // the cursor's place in `rows`
  const std::vector<net::dropbox::Revision> &dropboxRevisions(uint64_t id) const;
  void dropboxAction(Action action);
  void dropboxDialog(); // a game's backups, or the unlink question
  void dropboxEnter();  // the tab opened: read the link, list once
  void pollDropbox();
  void startDropboxLink();
  void listDropbox();
  void openDropboxGame();
  void dropboxTransfer();
  void uploadDropbox(const saves::LocalBackup &backup);
  void dropboxUploadList(); // the console's backups, one to pick for the upload
  // What A does on the open game's highlighted row: 올리기, 받기 or nothing.
  std::string dropboxPopupAccept() const;
  bool dropboxFocusable() const;
  bool dropboxModal() const { return dropboxOpen_ || dropboxUnlinkAsk_; }
  // Runs `job` on the Dropbox worker; what it returns runs on the UI thread.
  bool runDropboxJob(const std::string &busy, std::function<std::function<void()>()> job);
  std::unique_ptr<net::Dropbox> dropbox_; // the worker's while one runs
  bool dropboxLinked_ = false, dropboxMissing_ = false, dropboxListed_ = false;
  // The link read from cookingfoil.db (dropboxEnter): until then neither
  // 연결하기 nor the account shows, only the rows loading.
  bool dropboxRead_ = false;
  // Reading it now (a read that failed shows 연결하기 with its error).
  bool dropboxReading() const { return !dropboxRead_ && dropboxWorker_.joinable() && dropboxBusy_ == "list"; }
  std::string dropboxAccount_, dropboxBusy_; // busy: "start", "status", "finish", "list", "upload", "download"
  std::optional<net::dropbox::BridgeSession> dropboxSession_; // a sign-in waiting for the phone
  uint32_t dropboxPollAt_ = 0, dropboxExpiresAt_ = 0;
  net::DropboxRevisions dropboxRemote_;
  Error dropboxError_;
  std::thread dropboxWorker_;
  std::atomic<bool> dropboxDone_{false};
  std::function<void()> dropboxFinish_;
  uint64_t dropboxCursor_ = 0;      // the focused game's application id (0: the first)
  bool dropboxOnAccount_ = false;  // the cursor on the account row above the games
  std::function<void()> dropboxAfter_; // a press made while a game's backup details were read
  DropboxRow dropboxGame_;          // the game whose popup is open
  bool dropboxOpen_ = false, dropboxUnlinkAsk_ = false;
  int dropboxPick_ = 0;             // in the game popup: 0 upload (installed only), then Dropbox backups
  bool dropboxChoosing_ = false;    // the upload row opened the console's backups
  int dropboxUploadPick_ = 0;       // which of dropboxLocal_ goes up
  std::vector<saves::LocalBackup> dropboxLocal_; // the open game's backups on the console
  std::map<std::string, saves::BackupMeta> dropboxMetas_; // its Dropbox backups' .meta, by name
  std::string dropboxDevice_; // this console's save-keeper device id
  Qr dropboxQr_;
  void saveAction(Action action);
  void runSaveJob(bool restore);
  void pollSaveJob();
  void detail();
  int requiredPatchesCard(int y); // returns the y below it
  int notesCard(int y);           // the operator's notes; returns the y below it
  // The detail's patches for its game, as rows of the selection.
  void syncDetailPatches();
  // The patch whose turn came in the queue goes to the patch worker.
  void pumpPatchQueue();
  // A patch job or patches still queued: 다운로드 shows them while they run.
  bool patchDownloading() const { return !patchQueue_.empty() || (patchWorker_.joinable() && !patchBusy_.empty()); }
  std::deque<UserPatch> patchQueue_;
  UserPatch patchQueueNow_; // the queued patch the worker has
  void gallery();
  int screenshotStrip(int y);
  std::string shopAddress(const std::string &url) const;
  float chipRow(const std::string &title, const std::vector<std::string> &labels, float y);
  std::string homebrewNotesOf_, homebrewNotes_; // the app whose notes homebrewNotes_ holds, as plain text
  std::string galleryName() const;
  void downloads();
  void updates();
  void installUpdates(bool all);
  void pairing();
  // The note for the admin on the approval screen: Y asks for it, and a
  // changed one is saved per server and sent with a new request.
  std::string pairMessage() const;
  void askPairMessage();
  void setPairMessage(const std::string &text);
  static bool patchFilterMatches(PatchFilter filter, const PatchRow &row);
  std::vector<PatchRow> allPatchRows() const;
  // One entry per game, its patches as indices into the rows given, in
  // the rows' order. The list shows games; a game with several patches
  // opens a picker.
  struct PatchGame {
    std::string titleId, game, icon; // titleId: homebrew::ownerKey, "app:<folder>" for an app
    bool installed = false;
    std::vector<int> rows;
    bool app = false; // an app's patches (switch/<app>/), not a game's
  };
  static std::vector<PatchGame> patchGames(const std::vector<PatchRow> &rows);
  // The patch the popups act on, by id so a list that changes under it
  // (a filter, a finished job) never moves it to another patch.
  const PatchRow *pickedPatch(const std::vector<PatchRow> &rows) const;
  void patchPicker(const std::vector<PatchRow> &rows, const PatchGame &game);
  bool pickingPatch_ = false;
  int patchPickRow_ = 0;
  std::string patchPickId_;
  std::vector<PatchRow> patchRows() const { return patchRows(allPatchRows()); } // allPatchRows() under the current tab
  std::vector<PatchRow> patchRows(std::vector<PatchRow> all) const;          // `all` under the current tab
  PatchFilter patchFilter_ = PatchFilter::Stored; // 설치됨 first, as on 내 게임 and 홈브류
  void patchesScreen();
  void patchAction(Action action);
  void fetchPatches();
  void pollPatches();
  // `failTitle` heads the alert when the job fails (patch failure when empty).
  bool runPatchJob(const std::string &id, const std::string &label,
                   std::function<void(net::Session &, PatchStore &, Error &, std::string &)> job,
                   const std::string &failTitle = "");
  // Shared by 유저 패치 and an app's patches in 홈브류.
  void startPatchGet(const UserPatch &patch, bool fresh);
  void startPatchEnable(const UserPatch &patch);
  void startPatchDisable(const UserPatch &patch);
  // Puts back an enabled patch's missing files, downloading it again when
  // its store lacks them too.
  bool startPatchReapply(const UserPatch &patch, bool onServer);
  void pauseSmallDownload(bool pause);
  // 진행 중's one card at the top, for whatever downloads now.
  struct TopCard {
    std::string key, name, icon, phase;
    double percent = 0;
    double skipped = 0; // of percent, what was already on the card (drawn lighter)
    uint64_t bytes = 0, left = 0; // a running byte count (for the speed) and what is left
    bool paused = false, measure = true;
    bool extra = false; // a shortcut that follows what just came down: it takes the card's place quietly
    std::vector<std::pair<std::string, int>> chips; // label, 0 waits / 1 now / 2 done
    std::function<void()> onPause, onCancel, onDetails; // onDetails: none when it has no page
  };
  // What downloads now, as its 다운로드 entry; nullptr when none is running.
  const history::Entry *runningEntry() const;
  // Whether an entry has a page to open (a game's, an app's, a retro game's):
  // the same pages a row of 실패, 취소 or 완료 opens.
  bool hasPage(const history::Entry &entry) const;
  // Opens the page of what downloads now; nothing when it has none (then
  // 진행 중 shows no 상세 at all).
  std::function<void()> runningDetails();
  bool topCardNow(const install::Progress &p, TopCard &card);
  void topCard(const TopCard &card, float dy = 0);
  std::string topCardKey_;
  uint64_t topCardBytes_ = 0, topCardStartBytes_ = 0;
  double topCardMovedAt_ = 0; // when its byte count last moved
  double cardChangedAt_ = -10, cardShownAt_ = -10; // a new card's rise; the last frame with one
  TopCard lastCard_; // drawn on in the moment between one job and the next
  net::CheckProgress cardCheck_; // an app's or RetroArch's files on the card checked before it downloads
  double resumedNoteUntil_ = 0; // its bar went back: "중단되었던 블록부터…" until then
  // A paused app or retro game sits on the card, not again in the list.
  size_t queueSkip() const { return queueHeld_ && !installQueue_.empty() && !installer_.running() && !retroDownloading() ? 1 : 0; }
  void patchGame(const UserPatch &patch, std::string &name, std::string &icon) const;
  void patchDialog();
  // 설정 > 파일 정리 (FileCleanup.cpp): what each kind of leftover takes,
  // and the picked ones deleted, on a worker thread.
  struct TidyFound {
    Parsed<CleanupReport> leftovers;
    std::vector<std::pair<std::string, uint64_t>> pieces; // half-downloaded files, by full path
    uint64_t pieceBytes = 0, cacheBytes = 0;
  };
  void openFileCleanup();
  void deletePickedFiles();
  void pollFileCleanup();
  void fileCleanupAction(Action a);
  void fileCleanupDialog();
  bool tidyLeftoversAllowed() const;
  bool tidyPiecesAllowed() const;
  uint64_t tidyBytes(int row) const;
  bool tidyPicked(int row) const;
  bool tidyOpen_ = false, tidyBusy_ = false, tidyDeleting_ = false, tidyKnown_ = false;
  int tidyRow_ = 0;
  std::array<bool, 3> tidyOn_{true, true, false}, tidyRemoving_{};
  TidyFound tidyFound_, tidyIncoming_; // what the screen shows; what the worker brings
  std::thread tidyWorker_;
  std::atomic<bool> tidyDone_{false};
  std::string imageCacheFolder() const;
  CleanupProgress cleanupProgress_;
  // 설정 > 앱 업데이트: the server's newest build, checked when 설정 opens;
  // A gets it, swaps the .nro and starts the app again.
  enum class AppUpdate { Unknown, Checking, Latest, Available, Missing, Failed, Downloading };
  AppUpdate appUpdate_ = AppUpdate::Unknown;
  update::Release appRelease_;
  std::string appUpdateError_, selfPath_;
  void noticeRollBack(); // an update undone at start says so (AppUpdate.cpp)
  // A secure connection failed and the console clock is off from internet
  // time: asked once per run, then set to it (ClockFix.cpp). Failing, the
  // log keeps why and the popup leads to a bug report.
  enum class ClockFix { None, Checking, Ask, Setting, Failed };
  ClockFix clockFix_ = ClockFix::None;
  bool clockAsked_ = false;
  int64_t clockInternet_ = 0;         // internet time when asked, Unix seconds
  uint32_t clockAskedAt_ = 0;         // SDL ticks then, to carry it forward
  uint32_t clockCheckedAt_ = 0;       // the last comparison, at most one a minute
  std::thread clockWorker_;
  // Resume points checked against the console's placeholders once a run.
  uint64_t resumeCleared_ = 0; // bytes the check freed, said once it is back
  std::thread resumeCheckWorker_;
  std::atomic<bool> resumeCheckDone_{false};
  std::map<std::string, DownloadStore::ResumePoint> resumeChecked_;
  std::vector<std::string> resumeStale_;
  void checkResumePoints();
  void pollResumeCheck();
  std::atomic<bool> clockDone_{false};
  std::function<void()> clockFinish_;
  void pollClock();
  void clockDialog();
  void clockAction(Action action);
  bool clockModal() const { return clockFix_ == ClockFix::Ask || clockFix_ == ClockFix::Setting || clockFix_ == ClockFix::Failed; }
  bool appUpdateAsk_ = false;
  std::thread updateWorker_;
  std::atomic<bool> updateDone_{false}, updateCancel_{false};
  std::atomic<uint64_t> updateReceived_{0};
  std::function<void()> updateFinish_;
  // 설정 > 오류 기록 보내기, and on its own whenever an error popup shows:
  // the log's tail goes to the server (data/client/logs) to be read there.
  void pollLog();
  // 설정 > 버그 리포트 보내기: the log's tail to the server, on request.
  std::string recentCrashReport(size_t room) const;
  void sendBugReport(const std::string &said);
  bool bugAsk_ = false, bugMessageField_ = false; // the report's popup, and its note being typed
  std::string bugMessage_;
  std::thread bugWorker_;
  std::atomic<bool> bugDone_{false};
  std::string incomingBugStatus_;
  std::string lastAlert_; // the alert already written to the log
  void checkAppUpdate();
  void installAppUpdate();
  void pollAppUpdate();
  void appUpdateDialog();
  void appUpdateAction(Action action);
  std::string appUpdateStatus() const;
  // 설정 > 언어: every language at once, with its flag.
  void languageDialog();
  bool languagePopup_ = false;
  int languageRow_ = 0;
  void patchMenu(const PatchRow &row);
  void runPatchOp(int op);
  void installSelected();
  // The picked files already on their way: 0 not all of them, 1 every one
  // queued, 2 one of them downloading now (`percent` its progress).
  int pickedOnTheWay(double &percent) const;
  void requestInstall();
  void dialogs();
  float infoCard(const Group &g, const std::string &publisher, float y);
  // Centered notice card for empty, error and waiting states.
  // `button`, when given, sits under the text: A (on the content) or a tap
  // presses it. `other` sits beside it, pressed by its own `otherKey` or a tap.
  void notice(const std::string &title, const std::string &body = "", int icon = -1, const std::string &button = "",
              std::function<void()> press = {}, const std::string &other = "", const std::string &otherKey = "",
              std::function<void()> pressOther = {}, int buttonIcon = -1);
  // A screen that needs the server, before it answers: still reading it,
  // or not connected at all.
  void offlineNotice(const std::string &body);
  // 홈 with nothing to show and a button on its notice: 설정 when no server
  // is set up, 다시 시도 when the one set up did not answer.
  bool homeOffersSettings() const { return groups_.empty() && !session_ && !stateWaiting_; }
public:
  // The banner's tags, as the website's (#218): score, sale (else the
  // regular price), Korean, size,
  // two genres, players (2 or more), release, update, DLC, in that order;
  // `score` is set on the score's own tag (its card colour).
  struct HeroTag {
    std::string text;
    int score = 0;
  };
  std::vector<HeroTag> heroTags(const Group &g) const;
private:
  bool homeOffersRetry() const { return groups_.empty() && session_ && error_ && !pairingPending_; }
  // Page title and its meta text, centered on the header's button line.
  // Returns the x after the meta text.
  // `metaRight`: where header buttons start, so long meta text stops short.
  float pageTitle(const std::string &title, const std::string &meta = "", float metaRight = 0);
  // The count beside a title: while searching it says what it counts (the
  // words themselves stay in the search box).
  std::string countLabel(size_t count, const std::string &query) const;
  void connect(size_t server);
  void clearCatalog();
  // `prefer`: the server to connect to when it is on, else the first one on.
  void reload(int prefer = -1);
  // 설정's server form (ServerForm.cpp): + 서버 추가 opens it empty, A on a
  // server opens that one. Rows: the four fields, then 저장 and 삭제.
  struct ServerForm {
    int index = -1; // the server edited; -1 a new one
    Server server;  // the edited copy
    std::string note; // the approval message, kept in state.json by address
    int row = 0;    // 0 주소, 1 아이디, 2 비밀번호, 3 이름, 4 메시지
    std::string error;
    bool confirmDelete = false;
    bool confirmKeys = false; // asked before prod.keys goes to the server
  };
  static constexpr int ServerFields = 5; // then the colour row, then prod.keys on an admin console
  static constexpr int KeysRow = ServerFields + 1;
  // prod.keys for the server being edited: the row shows on the connected
  // server when it offers keys-upload to this (admin) console.
  bool keysRowShown() const;
  int serverFormRows() const { return keysRowShown() ? KeysRow + 1 : KeysRow; }
  std::optional<prodkeys::Status> keysStatus_;
  std::string keysNote_; // what the last read or send said
  Job keysJob_;
  bool keysSending_ = false;
  std::optional<prodkeys::Status> keysPreview_;
  void readKeysStatus();
  void sendProdKeys();
  void keysRow(float x, float y, float w, float h, float valueX, float valueW);
  int serverFormColor() const;
  void stepServerColor(int step);
  std::optional<ServerForm> serverForm_;
  std::optional<TextRequest> textRequest_;
  int textField_ = -1; // the form field the keyboard types into
  bool pairMessageField_ = false; // the keyboard types the approval note
  void openServerForm(int index);
  void serverFormAction(Action a);
  void serverFormDialog();
  void askServerField(int field);
  void saveServerForm();
  void deleteServerForm();
  // Writes `next` to config.json and reads it back, connecting to `prefer`;
  // `done` is the message once it is in. An alert says why when it fails.
  bool writeServers(const Config &next, int prefer, const std::string &done);
  void openDetail(size_t group);
  void fetchDetail();
  void card(const Group &g, float x, float y, bool selected,
            bool badges = true);
  // `exact`: the URL as given (no thumbnail size asked) and the picture
  // fitted whole, for art the server already made square (retro tiles).
  // `dark`: the cover sits on a dark backdrop (a banner), so its waiting
  // skeleton breathes in dark shades.
  void cover(const std::string &url, float x, float y, int size,
             bool selected = false, const std::string &name = "", bool exact = false, bool dark = false);
  void footer(const std::vector<std::pair<std::string, std::string>> &hints);
  // `look`, when given, is how it shows pressed (corner `radius`); else as a
  // card a little larger than the area.
  void hit(float x, float y, float w, float h, std::function<void()> run, SDL_FRect look = {}, int radius = 0);
  // A small button drawn at `look` (corner `radius`): it answers a little
  // above and below it, and shows pressed as itself, not as a larger card.
  void buttonHit(SDL_FRect look, int radius, std::function<void()> run);
  // Under a popup nothing answers: no taps, and no list glides behind it.
  void blockTouch() {
    hit(0, 0, 1280, 720, [] {});
    glideAreas_.clear();
  }
  void hintAction(const std::string &key);
  // Whether the current screen has anything the cursor can land on.
  bool hasFocusable() const;
  struct Hit {
    SDL_FRect area;
    std::function<void()> run;
    SDL_FRect look{}; // shown pressed as this when set; else a card around area
    int radius = 0;
  };
  std::vector<Hit> hits_;
  // The header's buttons (tabs, filter, search, Y buttons), recorded each
  // frame like hits_ so the d-pad can reach them: Up from the top of the
  // content, or Right from the menu when the screen has nothing else.
  struct HeaderItem {
    std::string id;
    SDL_FRect area;
    std::function<void()> run;
    bool live = false; // a tab: landing on it switches to it
  };
  std::vector<HeaderItem> header_;
  std::string headerFocus_, headerHome_; // the item with the cursor ("" none); where it lands first
  // Touch area and d-pad stop at once; draws the focus ring when it has the cursor.
  // Pressed, it darkens as `look` (the drawn button), or the area itself.
  void headerHit(const std::string &id, float x, float y, float w, float h, const std::function<void()> &run,
                 SDL_FRect look, int radius);
  void headerItem(const std::string &id, float x, float y, float w, float h, std::function<void()> run,
                  bool home = false, SDL_FRect look = {}, int radius = 12);
  bool enterHeader();
  bool headerAction(Action a); // false: the key is not the header's, handle it as usual
  bool contentAtTop() const;   // Up from here goes to the header
  bool popupOpen() const;
  // The cursor is on the screen's own content, not the menu or the header.
  bool inContent() const { return !sidebar_ && headerFocus_.empty(); }
  // Moving in the menu shows that screen once the cursor rests a moment;
  // A or Right goes into it.
  bool menuPending_ = false;
  uint32_t menuAt_ = 0;
  void openMenuScreen(Screen target);
  // Touch scrolling that glides like a phone's. A screen registers each
  // scrolling area as it draws (glide()); a finger in one moves it freely
  // and a flick keeps it going. The cursor stays put: any button brings
  // the view back to it (unglide()).
  struct GlideArea {
    std::string id;
    SDL_FRect area;
    float shown = 0, max = 0, unit = 1; // what was drawn, its end, pixels per step
    bool horizontal = false;            // a shelf moves sideways
    bool snap = false;                  // it comes to rest on whole steps (a shelf's cards)
  };
  std::vector<GlideArea> glideAreas_; // as the last frame drew them
  struct Glide {
    std::string id;
    float pos = 0, velocity = 0, max = 0, unit = 1; // in steps (rows, or pixels when unit is 1)
    bool active = false, held = false, horizontal = false;
    uint32_t at = 0;    // the last finger move or glide step
    float travel = 0;   // the finger's whole way, in pixels (the banner flips by it)
    bool snap = false, settling = false; // springing onto `target`, a whole step
    float target = 0;
  } glide_;
  bool swallowTap_ = false; // the touch that stopped a coasting list
  SDL_FRect pressed_{};      // what the finger is on, shown pressed
  SDL_FRect pressedLook_{};  // a button's own shape, when it gave one
  int pressedRadius_ = 0;
  uint32_t pressedUntil_ = 0; // 0 while held; else when the pressed look goes
  bool pressing_ = false;
  int pressedScreen_ = -1; // the screen it was pressed on: another one drops the look
  bool pressedPopup_ = false; // pressed in a popup or under none: a popup opening or closing drops the look
  // Where other areas were left by the finger (a shelf swiped, then the
  // page scrolled): each keeps its place until a button is pressed.
  std::map<std::string, float> glideRest_;
  // Where the area `id` (x, y, w, h on screen) starts: `focus` (what the
  // cursor wants) unless the finger moved it; 0..max, in steps of `unit` px.
  float glide(const std::string &id, float x, float y, float w, float h, float focus, float max, float unit,
              bool horizontal = false, bool snap = false);
  void glideStep(); // the flick's coast and the spring back from an end, once a frame
  void heroFlick(float speed); // the banner's turn when a finger lets go of it
  float glideFollow(float steps) const; // where `steps` more takes it, stretching past an end
  // A gliding list draws rows cut at its edges: drawing and touch are both
  // kept inside the area until unclip().
  std::vector<SDL_Rect> clips_;
  void clipTo(float x, float y, float w, float h);
  void unclip();
  void unglide() {
    glide_.active = glide_.held = glide_.settling = false;
    glideRest_.clear();
  }
  std::vector<size_t> homeRow(int row) const;
  void openTheme(size_t row);
  const std::vector<size_t> &libraryList() const; // cached until its inputs change
  void message(const std::string &text);
  Painter paint_;
  std::unique_ptr<Images> images_;
  float outputScale_ = 1; // physical pixels per 1280 wide, read on the screen's thread
  std::atomic<bool> loading_{false}; // open() is reading on its loading thread
  Sounds sounds_;
  bool alertHeard_ = false; // the error cue played for the alert on screen
  Config config_;
  State state_;
  Error error_;
  std::string configPath_, statePath_, fixture_, query_, notice_;
  std::vector<Group> groups_;
  // groups_ by id, rebuilt with it: a title ID finds its game without a scan.
  std::unordered_map<std::string, size_t> groupIndex_;
  const Group *findGroup(const std::string &id) const;
  // An installed game's cover: the console's own icon, else the shop's.
  std::string installedIcon(const InstalledGame &game);
  Screen screen_ = Screen::Home, previous_ = Screen::Home;
  bool sidebar_ = true, connected_ = false; // the cursor starts on the menu, on 홈
  // The cursor went to the menu only because the screen had nothing yet.
  bool autoSidebar_ = false;
  int nav_ = 0, row_ = 1, selected_ = 0;
  // Filters that are on (indices into chips_, never 0 which is "전체").
  std::vector<int> activeChips_;
  // The filtered list is rebuilt only when its inputs change.
  mutable std::string listKey_;
  mutable std::vector<size_t> listCache_;
  int catalogVersion_ = 0;
  InstalledMask installedMask_;
  // Picked once per launch: rotating home rows change between launches but
  // stay put while the app is open.
  unsigned homeSeed_ = unsigned(std::time(nullptr)) | 1u;
  std::vector<net::Featured> featured_;
  GenreNames genreNames_, scoreLabels_; // key -> name on screen
  net::NameTable genreTable_;            // fixtures only; a session has its own
  std::string heroBanner_; // admin banner for the hero, when featured has one
  void rebuildHome();
  // Shows each game under its name in the interface language when the
  // server sent one (feature item-names), else the store's own name.
  void localizeNames();
  // Startup language: 설정's choice, else the console's, else config.json's.
  void applyStartupLanguage();

  size_t serverIndex_ = 0, detailGroup_ = 0;
  std::unique_ptr<net::Session> session_;
  std::thread network_;
  std::atomic<bool> done_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> retroCancel_{false}; // Y while RetroArch downloads; also set on exit
  std::vector<Item> incoming_;
  std::vector<Group> incomingGroups_; // incoming_ grouped on the network thread
  Error incomingError_;
  net::Detail detail_, incomingDetail_;
  std::unique_ptr<Selection> selection_;
  int part_ = 1, descriptionScroll_ = 0;
  int storageCursor_ = 0; // detail storage row: 0 SD card, 1 console
  bool descriptionFocus_ = false; // detail: the cursor is on the description column
  int descriptionHeight_ = 0;
  bool galleryOpen_ = false;
  size_t screenshot_ = 0;
  std::string requestedId_;
  bool detailRequest_ = false, detailLoading_ = false;
  struct DetailCache {
    net::Detail detail;
    uint32_t at;
  };
  std::map<std::string, DetailCache> details_;
  install::Installer installer_;
  // The download screen's speed and time left, timed by its own frames.
  install::SpeedWindow downloadSpeed_;
  install::TimeLeft timeLeft_;
  bool timeLeftShown_ = false;  // once the connections are found (or a minute passed)
  double timeLeftFrom_ = -1;    // when this download's speed was first measured
  double downloadSeen_ = -1;
  double shownPercent_ = -1, shownPercentAt_ = 0; // the bar as drawn, gliding
  std::optional<uint64_t> sdFree_, nandFree_, sdTotal_, nandTotal_;
  bool storageChecked_ = false;
  uint32_t storageAt_ = 0, noticeAt_ = 0;
  uint32_t noticeShownAt_ = 0; // when the toast came up; a new text on it does not move it
  // Free space and the card files the screens ask about (RetroArch, cores),
  // read on a worker: while a download writes, the card answers slowly, and
  // a frame that waits for it stops the screen.
  struct StorageFound {
    std::optional<uint64_t> sdFree, nandFree, sdTotal, nandTotal;
    std::map<std::string, bool> files;
    uint64_t generation = 0;
  };
  std::thread storageWorker_;
  std::atomic<bool> storageDone_{false};
  StorageFound storageFound_;
  void pollStorage();
  // Whether a file (relative to the card's root) is there, as last read; a
  // path never asked about is read once on the spot.
  bool cardHas(const std::string &path) const;
  mutable std::map<std::string, bool> cardSeen_;
  uint64_t cardGeneration_ = 0;
  // Something was written to or removed from the card: what was read goes,
  // and free space is asked again.
  void cardChanged();
  void followTakeBack();
  bool takenBack_ = false; // heard while a refresh ran: the next poll refreshes
  bool exitAfterDevice_ = false; // + pressed while the console was being read
  bool cancelRetro_ = false; // the cancel popup is about the retro download
  bool confirmCancel_ = false, confirmExit_ = false, exitAfterCancel_ = false,
       exitRequested_ = false;
  bool searchRequested_ = false, confirmInstall_ = false;
  int dialogFocus_ = 0;
  void dialogHints(float right, float y, const std::string &close,
                   const std::string &apply = "", const std::string &accept = "", const std::string &remove = "");
  static inline const std::string NoAccept = "\x1f"; // `accept` for a popup A does nothing in
  SDL_FRect modal(float width, float bodyHeight, const std::string &title,
                  const std::string &close, const std::string &apply = "", const std::string &accept = "",
                  int icon = -1, const std::string &remove = "");
  std::string alert_, alertTitle_;
  bool refreshing_ = false; // - pressed: say so when the list is back
  bool keepCatalog_ = false; // - pressed on the same server: the list stays until the new one is in
  std::unique_ptr<PatchStore> patchStore_;
  std::map<std::string, PatchRecord> patchRecords_; // drawn; the worker owns the store
  std::vector<UserPatch> serverPatches_, incomingPatches_;
  bool patchesLoaded_ = false, patchFetching_ = false, confirmPatchRemove_ = false;
  bool patchesFixed_ = false; // the preview's list: never asked for
  std::atomic<int> incomingPatchRetry_{0};
  uint32_t patchRetryAt_ = 0; // ticks when a 503 list asks again
  bool managingPatch_ = false; // the patch management popup is open
  std::atomic<bool> cacheBusy_{false}; // the image cache is being emptied
  std::string cacheFolder_; // set by the preview; else beside the state file
  uint64_t restartSeen_ = 0; // the server restarts already said
  bool restartShowing_ = false, refreshAfterRestart_ = false;
  uint32_t restartProbeAt_ = 0; // when the list was last asked during a restart
  int patchMenuRow_ = 0;
  Error patchError_, incomingPatchError_;
  std::string patchBusy_, patchBusyLabel_, patchMessage_, patchFailTitle_;
  std::thread patchWorker_;
  std::atomic<bool> patchDone_{false};
  std::atomic<uint64_t> patchReceived_{0};
  // Files an enable or disable moves; patchReceived_ counts the ones done.
  std::atomic<uint64_t> patchMoving_{0};
  // The light check of enabled patches' files on the card. It runs on its
  // own thread from a copy of the ledger, within a budget, and gives way to
  // any patch job; the list draws from the ledger meanwhile.
  void startPatchCheck();
  void pollPatchCheck();
  std::unique_ptr<PatchChecker> patchChecker_; // the check's thread has it while it runs
  std::thread patchCheckWorker_;
  std::atomic<bool> patchCheckDone_{false}, patchCheckStop_{false};
  bool patchCheckWanted_ = false;
  // Bumped whenever the ledger copy changes; a check of an older copy is dropped.
  uint64_t patchLedgerGen_ = 0, patchCheckGen_ = 0;
  std::map<std::string, PatchHealth> patchHealth_; // per patch id, what the check found
  // Patches on the card put there some other way, found by the check's
  // thread (incoming) and taken on the drawing thread.
  std::vector<CardPatch> cardPatches_, incomingCardPatches_;
  Qr qr_;
  bool pairingPending_ = false;
  net::PairingInfo pairingInfo_;
  uint32_t pairPollAt_ = 0, offlineCheckAt_ = 0;
  int retries_ = 0; // automatic reconnects since the last success or -
  // Updates and DLC the console lacks, worked out once per catalog or
  // installed list; the last answer stays while the list is read again, so
  // the menu's count does not blink.
  mutable std::vector<UpdateCandidate> updates_;
  mutable bool updatesStale_ = true;
  // Updates and DLC not yet in 다운로드 (queued or downloading ones hide).
  std::vector<UpdateCandidate> pendingUpdates() const;
  uint32_t installedAt_ = 0; // when the installed list was last read
  bool updateNewer_ = false; // a newer build was found; kept while checking again
  // Every automatic ask is bounded; - or Y starts them again.
  uint32_t pairSince_ = 0;    // when approval became pending; polling stops after a while
  int patchRetries_ = 0, retroRetries_ = 0;
  InstalledLibrary installed_, incomingInstalled_;
  std::vector<SaveEntry> saves_, incomingSaves_;
  Error deviceError_, incomingDeviceError_;
  std::thread deviceWorker_;
  std::atomic<bool> deviceDone_{false};
  bool deviceBusy_ = false, deviceFixture_ = false, readingSaves_ = false;
  bool removalOperation_ = false;
  // Popups with one decision take A only once they have been up a moment,
  // so the A that opened them (a double press) does not also answer them.
  // A quarter second: longer (it was 0.7 s) swallowed a deliberate press.
  int popupKind_ = 0;
  uint32_t popupAt_ = 0;
  bool settled() const { return SDL_GetTicks() - popupAt_ >= 250; }
  uint32_t removalAt_ = 0; // when the delete popup opened
  std::optional<InstalledGame> managedGame_;
  std::optional<RemovalRequest> removal_;
  int manageRow_ = 0;
  std::string sdRoot_; // "sdmc:" on the console, the preview's fake SD on the host
  RetroView retroView_ = RetroView::Home;
  std::vector<retro::Shelf> retroShelves_;
  bool retroHomeMissing_ = false; // an older server: straight to the list
  int retroRow_ = 0, retroCol_ = 0; // home focus: shelf and card
  std::string retroQuery_;          // the list's search, kept across L R
  std::vector<int> retroFilters_;   // the filter popup's choices (indices, never 0 "전체")
  SortMode retroSort_ = SortMode::Score;
  retro::Filter retroFilter() const;
  // The filter popup serves 모든 게임 and 레트로 게임: what it lists depends on the screen.
  std::vector<std::string> popupFilterLabels() const;
  std::vector<SortMode> popupSorts() const;
  void openViewPopup();
  retro::Game retroPick_;           // the game the confirm popup is about
  std::vector<retro::Variant> retroVariants_; // its versions, when it has more than one
  int retroVariant_ = 0;            // the one the cursor is on
  std::string retroDetailWant_;     // the version to load in full next
  // The versions picked to get (by id), and the full games loaded so far:
  // several versions sit side by side on the card under their own names.
  std::set<std::string> retroChecked_;
  std::map<std::string, retro::Game> retroLoaded_games_;
  void toggleRetroVersion(int row);
  void getRetroChecked();
  uint64_t retroCheckedBytes() const;
  retro::Meta retroMeta_;           // what the server knows of the game on its page
  int retroPart_ = 0;               // its page's cursor: a version row, or the button past them
  bool retroDetailLoading_ = false;
  std::vector<retro::System> retroSystems_;
  std::vector<retro::Game> retroGames_;
  int retroSystem_ = 0, retroTotal_ = 0, retroPage_ = 0;
  std::string retroListSystem_; // the system retroGames_ belongs to
  bool retroMissing_ = false, retroLoaded_ = false;
  Error retroError_;
  uint32_t retroRetryAt_ = 0;
  std::string retroBusy_;              // a game id, "pack", "homebrew:<id>" or "" when idle
  std::atomic<uint64_t> retroReceived_{0};
  std::atomic<uint64_t> retroKept_{0}; // of retroReceived_, files already on the card (not the network)
  std::atomic<uint64_t> retroBytes_{0}; // what the running download expects
  std::thread retroWorker_;
  std::atomic<bool> retroDone_{false};
  // Paused: a homebrew or retro download stops with its part kept and goes
  // back to the front of the queue, which holds until it resumes (from the
  // byte it reached).
  bool retroPausing_ = false, queueHeld_ = false;
  int unqueueAsk_ = -1; // the queue row whose 취소 asks first
  bool unqueuePick_ = false; // asked from 상세: its whole pick goes
  // Lists load on a thread of their own, so switching systems never waits
  // for a download.
  std::thread retroListWorker_;
  std::atomic<bool> retroListDone_{false};
  std::function<void()> retroListFinish_;
  std::function<void()> retroFinish_;  // set by the worker, run by pollRetro
  std::map<std::string, bool> retroOnCard_; // rom paths checked this visit
  // Every game's backups on the card (count, newest), read on a worker when
  // 세이브 opens or a backup comes or goes; the rows and their order use it.
  std::map<uint64_t, saves::BackupSummary> backupSummary_, incomingSummary_;
  std::thread backupSummaryWorker_;
  std::atomic<bool> backupSummaryDone_{false};
  bool backupSummaryAgain_ = false; // asked again while a read ran
  void readBackupSummary();
  void pollBackupSummary();
  int backupsOf(uint64_t id) const {
    const auto found = backupSummary_.find(id);
    return found == backupSummary_.end() ? 0 : found->second.count;
  }
  // The games with a save on the console, read with the summary; unknown
  // (the host, or a failed read) keeps every game.
  std::optional<std::set<uint64_t>> savedGames_, incomingSaved_;
  // A game worth listing in 세이브: a save on the console, a backup on the
  // card or one on Dropbox.
  bool hasSaveWork(uint64_t id) const {
    return backupsOf(id) > 0 || !savedGames_ || savedGames_->count(id) || dropboxHas(id);
  }
  bool dropboxHas(uint64_t id) const;
  // The console's saves are being read for the first time: the list waits
  // (placeholders) rather than show every game and then drop most.
  bool savesUnread() const { return !savedGames_ && backupSummaryWorker_.joinable(); }
  std::optional<int> saveOpen_;          // index into saves_
  std::vector<saves::LocalBackup> backups_;
  int saveRow_ = 0;                      // 0 back up, 1..n backups, n+1 delete
  enum class SaveConfirm { None, Restore, Trash };
  SaveConfirm saveConfirm_ = SaveConfirm::None;
  bool saveRestoring_ = false;
  std::thread saveWorker_;
  std::atomic<bool> saveDone_{false};
  std::atomic<uint64_t> saveCopied_{0};
  Error saveResult_;
  InstalledSort installedSort_ = InstalledSort::Name;
  install::Status observedInstall_ = install::Status::Idle;
  // Console calls (a HOME Menu icon refreshed, a shortcut removed) that
  // would wait on the drawing thread: run in order on a thread of their own.
  std::thread consoleWorker_;
  std::atomic<bool> consoleDone_{false};
  std::vector<std::function<void()>> consoleTasks_;
  void runOffScreen(std::function<void()> task);
  void pumpConsoleTasks();
  bool awake_ = false; // the console is kept from dimming while downloading
  // The applet calls behind it (the CPU boost above all) take up to a second:
  // made on a thread of their own, the latest wanted state last.
  std::thread awakeWorker_;
  std::atomic<bool> awakeDone_{false};
  bool awakeApplied_ = false;
  void applyAwake();
  // Unfinished installs are kept in state.json so they can be resumed.
  // Every install starts here: the installer, the resume record and 다운로드's history.
  // Each file goes in the queue as its own entry (base game, update, DLC,
  // in the order picked), behind whatever is already there; patches start
  // with the game's last file.
  // `first`: ahead of what waits (a shortcut for what just came down).
  Error startInstall(const std::vector<Item> &files, const std::string &name, const std::string &icon,
                     const std::string &target, std::vector<UserPatch> patches = {}, bool first = false);
  Error enqueueInstall(const std::vector<Item> &files, const std::string &name, const std::string &icon,
                       const std::string &target, std::vector<UserPatch> patches, bool first, bool &added);
  Error launchAdded(int picks);
  // Everything that downloads waits its turn in one queue and runs alone:
  // a game's files, its patches, homebrew, retro games, RetroArch and
  // shortcuts. `kind` is "" for an install, else the history row's kind.
  struct QueuedInstall {
    Item file;
    std::string name, icon, target;
    int group = 0; // one pick: a base game that fails takes the rest of it (update, DLC, patches) along
    uint64_t entry = 0; // its row in 다운로드's history, which keeps the queue across launches
    std::string kind, payload;
  };
  // A job other than an install goes in behind the rest (or `first`); false
  // when the same one already waits or runs.
  bool queueJob(const std::string &kind, const std::string &name, const std::string &icon, const std::string &url,
                uint64_t size, const std::string &payload, int group = 0, bool first = false);
  void queuePatchJobs(const std::vector<UserPatch> &patches, int group);
  Error launchJob(const QueuedInstall &next);
  bool startShortcut(const QueuedInstall &next); // false when it cannot start
  std::function<void()> queuedDetails(const QueuedInstall &item); // its page, or null when it has none
  // Whether a download (of any kind) runs now; the next one waits for it.
  bool downloadRunning() const;
  // The installer, the queue and the screens see the order the rows run in.
  void rebuildQueue();
  // A patch job's row ends once the patch worker is done with it.
  void settlePatchJob(bool failed, const std::string &error);
  // 다운로드's history rows open what they were: the game's page, the app,
  // the retro game; or say the server has it no longer.
  void openHistoryRow(const history::Entry &entry);
  // A 실패 or 취소 row in again, first in line, and the queue goes at once.
  void retryHistoryRow(history::Entry entry);
  // An app's page by its id; false when the list does not have it. Before
  // the list has come, it opens once it does.
  bool openHomebrewPage(const std::string &id);
  std::string homebrewOpenId_;
  // The rest of a pick leaves the queue (its rows read as cancelled).
  void dropGroup(int group, const std::string &why);
  std::deque<QueuedInstall> installQueue_;
  int installGroups_ = 0, runningGroup_ = -1;
  // The running install's clock and byte count, for what 완료 keeps.
  uint32_t jobStartMs_ = 0, lastByteMs_ = 0;
  uint64_t jobStartBytes_ = 0, lastBytes_ = 0;
  int64_t jobStartedAt_ = 0;
  // Starts the next entry while the installer is free.
  Error launchQueued();
  void runningAction(Action a);
  void unqueue(size_t index);
  void moveUp(size_t index);
  // 대기 중's rows from `y`; rows above `viewTop` are scrolled away.
  void queueList(float y, float viewTop);
  float queueHeight() const;
  std::string queueRowDetail(const QueuedInstall &q) const;
  // A HOME Menu shortcut comes with what was downloaded: it waits its turn in
  // the queue but has no row in 다운로드's lists or counts.
  static bool extraJob(const std::string &kind, const Item &file) { return kind == "shortcut" || file.type == "shortcut"; }
  static bool extraJob(const history::Entry &e) {
    return extraJob(e.kind, e.install.files.empty() ? Item{} : e.install.files.front());
  }
  // The queue's rows as 진행 중 lists them (their places in installQueue_):
  // no extras, and not a paused item that is on the card at the top.
  std::vector<size_t> shownQueue() const;
  size_t downloadsRunning() const;
  void moveShown(int row, bool up);
  static constexpr float QueueTitle = 0, QueueStep = 88; // the queue has no title: its cards follow the one above
  // 진행 중 below the installer's card: the retro and patch cards and the
  // queue, gliding as one under a finger.
  void runningList(float y, const TopCard *card = nullptr, float cardDy = 0, float shift = 0);
  uint64_t queuedBytes() const;
  std::vector<history::Entry> history_;
  // Hands the list to the history writer and returns at once: a write on the
  // SD card (a lock file, a transaction) takes long enough to stall a frame,
  // and a button press can save two or three times. The writer keeps only
  // the newest list, so a burst of saves is one write.
  void saveHistory();
  // Writes what is still waiting and stops the writer (closing).
  void flushHistory();
  void openDownloads(const std::string &sd);
  DownloadStore store_;
  std::thread historyWriter_;
  std::mutex historyMutex_;
  std::condition_variable historyWake_;
  std::optional<std::vector<history::Entry>> historyPending_;
  // Resume points from the install thread, newest per file, for the writer.
  std::map<std::string, std::optional<install::Installer::ResumePoint>> resumePending_;
  // The app's state (settings, keys) for cookingfoil.db, newest only: the
  // database is read and written whole, which a button must not wait for.
  std::optional<State> statePending_;
  // homebrew.json's newest text, for the writer: an atomic write on the SD
  // card is a stall the screen must not wait for.
  std::optional<std::string> ledgerPending_;
  // Folder installs' file lists to write with it, by app id (none: removed).
  std::map<std::string, std::optional<std::string>> fileListsPending_;
  std::mutex stateWriteLock_; // one state write at a time, the newest last
  // Writes the waiting settings now; reload() does, before it reads them.
  void writePendingState();
  bool historyClosing_ = false;
  std::atomic<uint64_t> resumeWritten_{0}; // resume points the writer put on the card, in rounds
  void wakeHistoryWriter();
  // 다운로드's tabs: 0 진행 중, 1 완료, 2 실패 (and cancelled).
  int downloadsTab_ = 0, downloadsRow_ = 0;
  std::vector<int> downloadsList() const;
  // 다운로드's ended rows are read a page at a time; the next page comes
  // as the cursor nears the end of what is read.
  static constexpr size_t HistoryPage = 60;
  Job historyPageJob_;
  void readOlderHistory();
  void clearHistoryTab(history::Outcome outcome); // history_ indices in the tab
  void downloadsAction(Action action);
  void downloadsHistory();
  void forgetInstall();
  void saveStateQuietly();
  void offerResume();
  bool configReadOnce_ = false; // the first read at start reads again when it finds no servers
  bool resumeOffered_ = false, confirmResume_ = false; // asked at start: go on with what was cut off
  bool resumeAsked() const { return confirmResume_ && !installQueue_.empty(); }
  // The menu's screen is the one shown: 내 게임 is its 업데이트 and DLC tabs too,
  // so passing the cursor through the menu leaves the tab as it was.
  bool showing(Screen menu) const {
    return menu == screen_ || (menu == Screen::Installed && screen_ == Screen::Updates);
  }
  // Home rows are built once per catalog load, not every frame.
  size_t hero_ = 0;
  // Home banner slides; the first is the admin's pick when featured has one.
  std::vector<HeroSlide> heroes_;
  int heroSlide_ = 0;
  bool heroFeatured_ = false;
  uint32_t heroAt_ = 0;
  void showHero(int slide);
  std::vector<HomeTheme> themes_;
  std::vector<Theme> chips_; // the full list's filters
  // Filter and sort popup: column 0 lists filters, column 1 sort orders.
  bool viewPopup_ = false;
  // The popup edits a draft; Y applies it, B drops it.
  std::vector<int> draftChips_;
  SortMode draftSort_ = SortMode::Default;
  bool draftOn(int index) const;
  int popupColumn_ = 0, popupRow_ = 0;
  void viewPopup();
  SortMode librarySort_ = SortMode::Recent; // newest first, like a shop
};
} // namespace foil::ui
