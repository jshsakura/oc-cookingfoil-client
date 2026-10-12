#pragma once
#include <ctime>
#include "core/Catalog.hpp"
#include "core/Selection.hpp"
#include "install/Installer.hpp"
#include "net/Api.hpp"
#include "net/Dropbox.hpp"
#include "ui/Images.hpp"
#include "ui/Painter.hpp"
#include "ui/Qr.hpp"
#include "core/Installed.hpp"
#include "core/SaveBackups.hpp"
#include "core/Retro.hpp"
#include "core/DownloadHistory.hpp"
#include "core/ShopSaves.hpp"
#include "core/ClientUpdate.hpp"
#include "core/Patches.hpp"
#include "core/I18n.hpp"
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include "core/HomebrewCard.hpp"
#include <utility>
namespace foil::ui {
// Description scroll unit for d-pad and right stick, in pixels.
constexpr int ScrollStep = 40;
constexpr int MenuItems = 9;
constexpr int PopupFilterColumns = 2; // filter grid width in the view popup
constexpr SortMode SortOrders[] = {SortMode::Recent,  SortMode::Popular, SortMode::Score,
                                   SortMode::Release, SortMode::Updated, SortMode::Name,
                                   SortMode::Largest, SortMode::Default};
enum class Screen { Home, Library, Detail, Downloads, Updates, Settings, Installed, Patches, Retro, Saves, Homebrew };
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
  ScrollDown
};
int themeIcon(const Theme &theme);
class Application {
public:
  Application(SDL_Renderer *renderer, std::string configPath = "",
              std::string statePath = "", std::string fixture = "");
  ~Application();
  bool open();
  void setItems(std::vector<Item> items);
  void setSession(std::unique_ptr<net::Session> session);
  void refresh();
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
  // The game or app 패치 shows alone, opened from a detail page.
  const std::string &patchOwnerFilter() const { return patchOwner_; }
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
  // Goes to the field that asked last; nothing happens once its form closed.
  void enterText(const std::string &text);
  bool requestQuit();
  // Where this .nro runs from (argv[0]); 설정 > 앱 업데이트 replaces it.
  void setSelfPath(std::string path) { selfPath_ = std::move(path); }
  // - on any screen: the shop, the console's games and free space again.
  void refreshAll();
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
  // 홈브류 without a server: the menu item and this list.
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
  // 세이브 > 드롭박스 without Dropbox: linked with `remote`, or not linked;
  // a `signIn` address shows the QR code waiting for the phone.
  void previewDropbox(bool linked, net::DropboxRevisions remote = {}, const std::string &signIn = "") {
    dropbox_ = std::make_unique<net::Dropbox>("", &stop_);
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
  bool dropboxChoosingUpload() const { return dropboxChoosing_; }
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
  static constexpr int SettingsOptions = 8;
  float settingsScroll_ = 0; // how far 설정 is scrolled to keep the cursor in view
  void chooseLanguage(Language language);
  // Genre names without a session (the PC preview's fixtures).
  void setGenreTable(net::NameTable table) { genreTable_ = std::move(table); genreNames_ = net::namesIn(genreTable_, languageCode()); }
  // Where the PC preview keeps its fake SD card for user patches.
  void usePatchFolder(const std::string &sd);
  // Touch: a tap runs the topmost area drawn last frame; a drag scrolls.
  void touch(float x, float y);
  void drag(float dy);
  // User patches: the server's list joined with what the SD store holds.
  struct PatchRow {
    UserPatch patch;
    std::optional<PatchRecord> record;
    bool onServer = false;
    std::string game, icon, installedVersion;
    bool installed = false;
    int shopGroup = -1; // the game in the shop's catalog, if it is there
  };
  // Tabs over the list; L R or a tap switches.
  // Starts on the games this console has; L R widens to every patch.
  // Tabs: 설치됨 (on the SD card), 전체, 새 버전.
  enum class PatchFilter { Stored, All, Updates };
  static constexpr int PatchFilters = 3;

private:
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
  std::vector<homebrew::CardNro> homebrewCardFound_; // the worker's result, taken on the main thread
  void scanHomebrewCard();
  void matchHomebrewCard();
  const homebrew::App &homebrewAt(int index) const; // a homebrewRows() index
  std::string homebrewIcon(const homebrew::App &app); // the URL or the card icon's key
  std::string homebrewFile(const homebrew::App &app) const; // its file on the card, from the SD root
  std::string homebrewEtag_, homebrewQuery_;
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
  void homebrewRemoveDialog();
  void homebrewAskDialog(); // what the app is, before it comes down
  void checkHomebrewCard();
  homebrew::Status homebrewStatus(const homebrew::App &app) const;
  std::vector<int> homebrewRows() const; // homebrewAt() indexes in this tab and search
  bool getHomebrew(const homebrew::App &app);
  void removeHomebrew(const homebrew::App &app);
  void settleHomebrew(const homebrew::App &app);
  void saveHomebrewLedger();
  void home();
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
  std::vector<int> saveGameRows() const { return gameRows(savesQuery_); }
  // The labels shrink (down to 14pt) when the strip would start left of
  // leftLimit. `counts`, when given, puts a small grey label after a
  // label whose count is above 0.
  float headerTabs(const std::vector<std::string> &labels, int current, const std::function<void(int)> &pick,
                   float right = 1252, float leftLimit = 0, const std::vector<int> &counts = {});
  int updatesTab_ = 1; // 1 업데이트, 2 DLC (내 게임's tabs after 설치됨)
  void showMyGamesTab(int tab);
  float myGamesTabs(float right = 1252);
  float myGamesButtonWidth(const std::string &label);
  std::vector<UpdateCandidate> updateRows() const;
  void settings();
  void installedScreen();
  void management();
  void removalConfirm(); // the delete question over 게임 관리
  void installedAction(Action action);
  void refreshInstalled();
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
  enum class RetroView { Home, List };
  void retroScreen();
  void retroHome();
  void retroList();
  void retroBanner(float y);
  void retroAction(Action action);
  void retroHomeAction(Action action);
  void loadRetroHome();
  void loadRetroPage(bool next);
  void openRetroList(int system);
  void askRetroGame(const retro::Game &game);
  // false when it did not start (another retro job runs, no room, already there).
  bool getRetroGame(const retro::Game &game, bool again = false);
  void startRetroEntry(const std::string &kind, const std::string &name, const std::string &icon,
                       const std::string &url, uint64_t size, const std::string &payload);
  void finishRetroEntry(const std::string &kind, history::Outcome outcome, const std::string &error, uint64_t bytes);
  // A retro game or RetroArch downloading (not a list loading).
  bool retroDownloading() const { return !retroBusy_.empty() && retroBusy_ != "list"; }
  float retroDownloadCard(float y); // 다운로드 > 진행 중; returns the height drawn
  int signal_ = 3;          // net::signalStrength(), refreshed by statusBar
  uint32_t signalAt_ = 0;
  bool installRetroShortcut(const retro::Game &game);
  bool installShortcut(const std::string &name, const std::string &icon, const std::string &url,
                       const std::string &titleId, uint64_t size, bool justDownloaded = true);
  const retro::Game *retroFocused() const;
  void scoreBadge(int score, float right, float y);
  bool syncRetroArch(bool full = false); // full: every file again, settings kept
  bool retroCoreMissing(const retro::Game &game) const;
  void pollRetro();
  // Runs `job` on the retro worker; what it returns runs on the UI thread.
  bool runRetroJob(const std::string &busy, std::function<std::function<void()>()> job);
  bool retroInstalled(const retro::Game &game) const;
  void retroConfirm(); // the game's info, with one button to get it
  // detail: the get popup, which also names the version a flag stands for.
  std::vector<std::string> retroLabels(const retro::Game &game, bool detail = false) const;
  bool retroAsking_ = false, retroArchAsking_ = false;
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
  void marquee(const std::string &text, float x, float y, int size, float width);
  std::string marqueeText_;
  uint32_t marqueeAt_ = 0;
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
  std::vector<int> dropboxGameRows() const;
  void dropboxAction(Action action);
  void dropboxDialog(); // a game's backups, or the unlink question
  void dropboxEnter();  // the tab opened: read dropbox.json, list once
  void pollDropbox();
  void startDropboxLink();
  void listDropbox();
  void openDropboxGame();
  void dropboxTransfer();
  void uploadDropbox(const saves::LocalBackup &backup);
  void dropboxUploadList(); // the console's backups, one to pick for the upload
  bool dropboxFocusable() const;
  bool dropboxModal() const { return dropboxOpen_ || dropboxUnlinkAsk_; }
  // Runs `job` on the Dropbox worker; what it returns runs on the UI thread.
  bool runDropboxJob(const std::string &busy, std::function<std::function<void()>()> job);
  std::unique_ptr<net::Dropbox> dropbox_; // the worker's while one runs
  bool dropboxLinked_ = false, dropboxMissing_ = false, dropboxListed_ = false;
  std::string dropboxAccount_, dropboxBusy_; // busy: "start", "status", "finish", "list", "upload", "download"
  std::optional<net::dropbox::BridgeSession> dropboxSession_; // a sign-in waiting for the phone
  uint32_t dropboxPollAt_ = 0, dropboxExpiresAt_ = 0;
  net::DropboxRevisions dropboxRemote_;
  Error dropboxError_;
  std::thread dropboxWorker_;
  std::atomic<bool> dropboxDone_{false};
  std::function<void()> dropboxFinish_;
  int dropboxRow_ = 0;              // -1 the account card, 0.. installed games
  bool dropboxOpen_ = false, dropboxUnlinkAsk_ = false;
  int dropboxPick_ = 0;             // in the game popup: 0 upload, 1.. Dropbox backups
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
  // The detail's patches for its game, as rows of the selection.
  void syncDetailPatches();
  // 패치 on its 전체 tab with only the detail's game; B comes back here.
  void openGamePatches();
  void returnToDetail();
  bool patchLinkFocus_ = false; // detail: the cursor is on the patch link
  std::string patchOwner_;      // 패치 shows only this owner's patches
  bool patchFromDetail_ = false;
  int detailReturnSelected_ = 0, detailReturnRow_ = 1;
  // Patches picked with an install; they start once the install ends well.
  void queuePatches(std::vector<UserPatch> patches, bool afterInstall);
  void pumpPatchQueue();
  float patchDownloadCard(float y);
  // A patch job or patches still queued: 다운로드 shows them while they run.
  bool patchDownloading() const { return !patchQueue_.empty() || (patchWorker_.joinable() && !patchBusy_.empty()); }
  std::deque<UserPatch> patchQueue_;
  bool patchQueueWaits_ = false;
  UserPatch patchQueueNow_; // the queued patch the worker has
  void gallery();
  void downloads();
  void updates();
  void installUpdates(bool all);
  void pairing();
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
  void patchDialog();
  // 설정 > 남은 파일 정리: scan, confirm, then delete on a worker thread.
  void startCleanup(bool remove);
  void pollCleanup();
  void cleanupDialog();
  void cleanupProgressDialog(); // while a scan or delete runs
  CleanupProgress cleanupProgress_;
  float cleanupShown_ = 0; // the bar's drawn fill, eased toward the real one
  // 설정 > 앱 업데이트: the server's newest build, checked when 설정 opens;
  // A gets it, swaps the .nro and starts the app again.
  enum class AppUpdate { Unknown, Checking, Latest, Available, Missing, Failed, Downloading };
  AppUpdate appUpdate_ = AppUpdate::Unknown;
  update::Release appRelease_;
  std::string appUpdateError_, selfPath_;
  bool appUpdateAsk_ = false;
  std::thread updateWorker_;
  std::atomic<bool> updateDone_{false}, updateCancel_{false};
  std::atomic<uint64_t> updateReceived_{0};
  std::function<void()> updateFinish_;
  // 설정 > 오류 기록 보내기, and on its own whenever an error popup shows:
  // the log's tail goes to the server (data/client/logs) to be read there.
  void sendLog(const std::string &reason);
  void pollLog();
  std::thread logWorker_;
  std::atomic<bool> logDone_{false};
  std::function<void()> logFinish_;
  std::string lastAlert_, logStatus_; // the alert already sent; 설정's line
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
  void requestInstall();
  void dialogs();
  float infoCard(const Group &g, const std::string &publisher, float y);
  // Centered notice card for empty, error and waiting states.
  void notice(const std::string &title, const std::string &body = "", int icon = -1);
  // Page title and its meta text, centered on the header's button line.
  // Returns the x after the meta text.
  // `metaRight`: where header buttons start, so long meta text stops short.
  float pageTitle(const std::string &title, const std::string &meta = "", float metaRight = 0);
  void connect(size_t server);
  void clearCatalog();
  // `prefer`: the server to connect to when it is on, else the first one on.
  void reload(int prefer = -1);
  // 설정's server form (ServerForm.cpp): + 서버 추가 opens it empty, A on a
  // server opens that one. Rows: the four fields, then 저장 and 삭제.
  struct ServerForm {
    int index = -1; // the server edited; -1 a new one
    Server server;  // the edited copy
    int row = 0;    // 0 주소, 1 아이디, 2 비밀번호, 3 이름, 4 저장, 5 삭제
    std::string error;
    bool confirmDelete = false;
  };
  static constexpr int ServerFields = 4;
  std::optional<ServerForm> serverForm_;
  std::optional<TextRequest> textRequest_;
  int textField_ = -1; // the form field the keyboard types into
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
  void cover(const std::string &url, float x, float y, int size,
             bool selected = false, const std::string &name = "", bool exact = false);
  void footer(const std::vector<std::pair<std::string, std::string>> &hints);
  void hit(float x, float y, float w, float h, std::function<void()> run);
  void blockTouch() { hit(0, 0, 1280, 720, [] {}); }
  void hintAction(const std::string &key);
  // Whether the current screen has anything the cursor can land on.
  bool hasFocusable() const;
  struct Hit {
    SDL_FRect area;
    std::function<void()> run;
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
  void headerItem(const std::string &id, float x, float y, float w, float h, std::function<void()> run,
                  bool home = false);
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
  float dragRest_ = 0;
  std::vector<size_t> homeRow(int row) const;
  void openTheme(size_t row);
  const std::vector<size_t> &libraryList() const; // cached until its inputs change
  void message(const std::string &text);
  Painter paint_;
  std::unique_ptr<Images> images_;
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
  std::optional<uint64_t> sdFree_, nandFree_, sdTotal_, nandTotal_;
  bool storageChecked_ = false;
  uint32_t storageAt_ = 0, noticeAt_ = 0;
  bool exitAfterDevice_ = false; // + pressed while the console was being read
  bool cancelRetro_ = false; // the cancel popup is about the retro download
  bool confirmCancel_ = false, confirmExit_ = false, exitAfterCancel_ = false,
       exitRequested_ = false;
  bool searchRequested_ = false, confirmInstall_ = false;
  int dialogFocus_ = 0;
  void dialogHints(float right, float y, const std::string &close,
                   const std::string &apply = "", const std::string &accept = "", const std::string &remove = "");
  SDL_FRect modal(float width, float bodyHeight, const std::string &title,
                  const std::string &close, const std::string &apply = "", const std::string &accept = "",
                  int icon = -1, const std::string &remove = "");
  std::string alert_, alertTitle_;
  bool refreshing_ = false; // - pressed: say so when the list is back
  std::unique_ptr<PatchStore> patchStore_;
  std::map<std::string, PatchRecord> patchRecords_; // drawn; the worker owns the store
  std::vector<UserPatch> serverPatches_, incomingPatches_;
  bool patchesLoaded_ = false, patchFetching_ = false, confirmPatchRemove_ = false;
  std::atomic<int> incomingPatchRetry_{0};
  uint32_t patchRetryAt_ = 0; // ticks when a 503 list asks again
  bool managingPatch_ = false; // the patch management popup is open
  std::thread cleanupWorker_;
  std::atomic<bool> cleanupDone_{false};
  bool cleanupBusy_ = false, cleanupRemoving_ = false, confirmCleanup_ = false;
  Parsed<CleanupReport> cleanupResult_;
  int patchMenuRow_ = 0;
  Error patchError_, incomingPatchError_;
  std::string patchBusy_, patchBusyLabel_, patchMessage_, patchFailTitle_;
  std::thread patchWorker_;
  std::atomic<bool> patchDone_{false};
  std::atomic<uint64_t> patchReceived_{0};
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
  const std::vector<UpdateCandidate> &pendingUpdates() const;
  uint32_t installedAt_ = 0; // when the installed list was last read
  bool updateNewer_ = false; // a newer build was found; kept while checking again
  // Every automatic ask is bounded; - or Y starts them again.
  uint32_t pairSince_ = 0;    // when approval became pending; polling stops after a while
  int patchRetries_ = 0, retroRetries_ = 0;
  int logsSent_ = 0;
  uint32_t logSentAt_ = 0;
  InstalledLibrary installed_, incomingInstalled_;
  std::vector<SaveEntry> saves_, incomingSaves_;
  Error deviceError_, incomingDeviceError_;
  std::thread deviceWorker_;
  std::atomic<bool> deviceDone_{false};
  bool deviceBusy_ = false, deviceFixture_ = false, readingSaves_ = false;
  bool removalOperation_ = false;
  // Popups with one decision take A only once they have been up a moment,
  // so the A that opened them (a double press) does not also answer them.
  int popupKind_ = 0;
  uint32_t popupAt_ = 0;
  bool settled() const { return SDL_GetTicks() - popupAt_ >= 700; }
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
  std::vector<retro::System> retroSystems_;
  std::vector<retro::Game> retroGames_;
  int retroSystem_ = 0, retroTotal_ = 0, retroPage_ = 0;
  std::string retroListSystem_; // the system retroGames_ belongs to
  bool retroMissing_ = false, retroLoaded_ = false;
  Error retroError_;
  uint32_t retroRetryAt_ = 0;
  std::string retroBusy_;              // a game id, "pack", "list" or "" when idle
  std::atomic<uint64_t> retroReceived_{0};
  std::atomic<uint64_t> retroBytes_{0}; // what the running download expects
  std::thread retroWorker_;
  std::atomic<bool> retroDone_{false};
  std::function<void()> retroFinish_;  // set by the worker, run by pollRetro
  std::map<std::string, bool> retroOnCard_; // rom paths checked this visit
  std::map<uint64_t, int> backupCounts_;    // 세이브: backups per game, this visit
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
  bool awake_ = false; // the console is kept from dimming while downloading
  // Unfinished installs are kept in state.json so they can be resumed.
  // Every install starts here: the installer, the resume record and 다운로드's history.
  Error startInstall(const std::vector<Item> &files, const std::string &name, const std::string &icon,
                     const std::string &target);
  std::vector<history::Entry> history_;
  void saveHistory();
  // 다운로드's tabs: 0 진행 중, 1 완료, 2 실패 (and cancelled).
  int downloadsTab_ = 0, downloadsRow_ = 0;
  std::vector<int> downloadsList() const; // history_ indices in the tab
  void downloadsAction(Action action);
  void downloadsHistory();
  void rememberInstall(const std::vector<Item> &files, const std::string &name,
                       const std::string &icon, const std::string &target);
  void forgetInstall();
  void saveStateQuietly();
  void offerResume();
  void resumeDialog();
  void resumeInstall();
  bool resumeOffered_ = false, confirmResume_ = false;
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
