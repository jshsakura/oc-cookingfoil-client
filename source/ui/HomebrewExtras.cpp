// An app's patches and saves in 홈브류: the server lists them with the
// game ones, naming the app's folder (switch/<app>) instead of a Title ID.
#include "ui/Application.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "net/ShopSavesApi.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
namespace foil::ui {
namespace {
constexpr int Patches = 1, Saves = 2, Remove = 3, Shortcut = 4;
constexpr const char *Getting = "Downloading"; // i18n: keep
bool updateAvailable(const Application::PatchRow &row) {
  return row.record && row.onServer && !row.patch.hash.empty() && row.record->hash != row.patch.hash;
}
// What A does to a patch row, as on 유저 패치's menu.
const char *patchVerb(const Application::PatchRow &row) {
  if (!row.record) return tr("Download and apply the patch");
  if (updateAvailable(row)) return tr("Update|action");
  return row.record->on ? tr("Turn off") : tr("Turn on");
}
} // namespace
std::vector<Application::PatchRow> Application::homebrewPatchRows(const homebrew::App &app) const {
  std::vector<PatchRow> rows;
  if (app.app.empty()) return rows;
  for (const auto &patch : serverPatches_) {
    if (patch.homebrewApp != app.app) continue;
    PatchRow row{patch, std::nullopt, true, app.name, app.iconUrl, "", true};
    if (const auto found = patchRecords_.find(patch.id); found != patchRecords_.end()) row.record = found->second;
    rows.push_back(std::move(row));
  }
  // Kept on the card but gone from the server: it can still go on and off.
  for (const auto &[id, record] : patchRecords_) {
    if (record.homebrewApp != app.app ||
        std::any_of(rows.begin(), rows.end(), [&](const PatchRow &row) { return row.patch.id == id; }))
      continue;
    PatchRow row;
    row.patch.id = id;
    row.patch.homebrewApp = record.homebrewApp;
    row.patch.name = record.name;
    row.patch.version = record.version;
    row.patch.size = record.bytes;
    row.record = record;
    row.game = app.name;
    rows.push_back(std::move(row));
  }
  return rows;
}
void Application::needHomebrewList() {
  if (!homebrewLoaded_ && !homebrewWorker_.joinable() && connected_ && features_.count("homebrew")) loadHomebrew();
}
std::vector<int> Application::homebrewSaveRows(const homebrew::App &app) const {
  std::vector<int> rows;
  for (int i = 0; i < int(homebrewSaves_.size()) && !app.app.empty(); ++i)
    if (homebrewSaves_[i].homebrewApp == app.app) rows.push_back(i);
  return rows;
}
// The rows the app's popup shows: 패치 and 세이브, each only when there are some.
std::vector<int> Application::homebrewExtraKinds(const homebrew::App &app) const {
  std::vector<int> kinds;
  if (!homebrewPatchRows(app).empty()) kinds.push_back(Patches);
  if (!homebrewSaveRows(app).empty()) kinds.push_back(Saves);
  if (homebrewShortcutRow(app)) kinds.push_back(Shortcut);
  // An app on the card ends its rows with 지우기, as a patch's menu does.
  if (homebrewStatus(app) != homebrew::Status::Available) kinds.push_back(Remove);
  return kinds;
}
void Application::homebrewExtrasAction(Action a) {
  const auto *app = homebrewPicked();
  if (!app) {
    homebrewExtras_ = 0;
    homebrewSaveAsk_ = false;
    return;
  }
  const auto patches = homebrewPatchRows(*app);
  const auto saves = homebrewSaveRows(*app);
  const int count = homebrewExtras_ == Patches ? int(patches.size()) : int(saves.size());
  homebrewExtraRow_ = std::clamp(homebrewExtraRow_, 0, std::max(0, count - 1));
  if (homebrewSaveAsk_) {
    if (a == Action::Back) homebrewSaveAsk_ = false;
    else if (a == Action::Accept && settled()) {
      homebrewSaveAsk_ = false;
      if (homebrewExtraRow_ < int(saves.size())) applyHomebrewSave(homebrewSaves_[saves[homebrewExtraRow_]]);
    }
    return;
  }
  if (a == Action::Back || count == 0) {
    homebrewExtras_ = 0;
    return;
  }
  if (a == Action::Up) homebrewExtraRow_ = std::max(0, homebrewExtraRow_ - 1);
  if (a == Action::Down) homebrewExtraRow_ = std::min(count - 1, homebrewExtraRow_ + 1);
  if (a != Action::Accept || !settled()) return;
  // A download waits its turn in 다운로드's queue, as everything does.
  if (homebrewExtras_ != Saves) {
    const auto &row = patches[homebrewExtraRow_];
    if (!row.record || updateAvailable(row)) {
      queuePatchJobs({row.patch}, 0);
      return;
    }
  }
  if (patchWorker_.joinable()) {
    message(tr("Try again when the patch task is done."));
    return;
  }
  if (homebrewExtras_ == Saves) {
    homebrewSaveAsk_ = true;
    return;
  }
  const auto &row = patches[homebrewExtraRow_];
  if (row.record->on) startPatchDisable(row.patch);
  else startPatchEnable(row.patch);
}
// The app's patches or saves, one row each; A does what the row allows.
void Application::homebrewExtrasDialog() {
  const auto *app = homebrewPicked();
  if (!app) return;
  const auto patches = homebrewPatchRows(*app);
  const auto saves = homebrewSaveRows(*app);
  const bool patchList = homebrewExtras_ == Patches;
  const int count = patchList ? int(patches.size()) : int(saves.size());
  if (count == 0) {
    homebrewExtras_ = 0;
    return;
  }
  homebrewExtraRow_ = std::clamp(homebrewExtraRow_, 0, count - 1);
  constexpr float Width = 760, Row = 76, Gap = 8;
  constexpr int Visible = 5;
  const int shown = std::min(count, Visible);
  const auto accept = patchList ? patchVerb(patches[homebrewExtraRow_]) : tr("Apply");
  const auto body = modal(Width, 16 + shown * (Row + Gap) + 8,
                          app->name + " · " + (patchList ? tr("Patches") : tr("Saves")), tr("Close"), "", accept,
                          patchList ? 32 : 36);
  const float x = body.x + 24, w = body.w - 48;
  const int first = std::clamp(homebrewExtraRow_ - Visible / 2, 0, std::max(0, count - Visible));
  for (int n = first; n < count && n < first + Visible; ++n) {
    const float y = body.y + 16 + (n - first) * (Row + Gap);
    hit(x, y, w, Row, [this, n] {
      homebrewExtraRow_ = n;
      action(Action::Accept);
    });
    if (n == homebrewExtraRow_ && !homebrewSaveAsk_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    std::string name, meta, state;
    uint64_t size = 0;
    bool lit = false, busy = false, update = false;
    if (patchList) {
      const auto &row = patches[n];
      name = row.patch.name;
      if (!row.patch.version.empty()) meta = "v" + row.patch.version;
      if (!row.patch.author.empty()) meta += (meta.empty() ? "" : " · ") + row.patch.author;
      if (!row.onServer) meta += (meta.empty() ? "" : " · ") + std::string(tr("Gone from server"));
      busy = patchBusy_ == row.patch.id;
      update = updateAvailable(row);
      lit = row.record && row.record->on && !busy && !update;
      state = busy ? tr(patchBusyLabel_.c_str())
            : !row.record ? tr("Not installed")
            : update ? tr("Update|item")
            : row.record->on ? tr("On") : tr("Off");
      size = row.record ? row.record->bytes : row.patch.size;
    } else {
      const auto &save = homebrewSaves_[saves[n]];
      name = save.name;
      meta = save.description.substr(0, save.description.find('\n'));
      busy = patchBusy_ == save.id;
      if (busy) state = tr(patchBusyLabel_.c_str());
      size = save.size;
    }
    paint_.fonts.text(name, x + 20, y + 10, 21, Ink, true, w - 220);
    paint_.fonts.text(meta, x + 20, y + 42, 17, Secondary, false, w - 220);
    if (!state.empty()) {
      const float sw = paint_.fonts.measure(state, 16, true) + 20;
      paint_.rect(x + w - 16 - sw, y + 10, sw, 28, busy || update ? Butter : lit ? Good : Surface, 8);
      paint_.fonts.middle(state, x + w - 16 - sw + 10, y + 24, 16, lit ? Surface : Ink, true);
    }
    if (size) {
      const auto bytes = formatBytes(size);
      paint_.fonts.text(bytes, x + w - 16 - paint_.fonts.measure(bytes, 17, true), y + 44, 17, Ink, true);
    }
  }
}
// The files go into the app's folder; what they replace is kept first.
void Application::homebrewSaveDialog() {
  const auto *app = homebrewPicked();
  const auto rows = app ? homebrewSaveRows(*app) : std::vector<int>{};
  if (rows.empty()) {
    homebrewSaveAsk_ = false;
    return;
  }
  appSaveConfirm(homebrewSaves_[rows[std::clamp(homebrewExtraRow_, 0, int(rows.size()) - 1)]]);
}
// The same question from 세이브 > 공유 and from the app's popup.
void Application::appSaveConfirm(const shopsaves::ShopSave &save) {
  const auto body = modal(700, 20 + 30 + 84 + 16, tr("Apply this save?"), tr("Cancel"), "", tr("Apply|confirm"), 36);
  paint_.fonts.text(save.name, body.x + 32, body.y + 20, 21, Ink, true, body.w - 64);
  paint_.fonts.wrapped(trf("Puts the save files in {0}. Files with the same name are moved to {1} first.",
                           {save.installRoot, "switch/cookingfoil/homebrew-saves/" + save.homebrewApp}),
                       body.x + 32, body.y + 56, 19, Secondary, body.w - 64, 3);
}
// Through the patch worker: unpacked beside the card's own files first, so
// a cut download leaves the app's folder as it was.
void Application::applyHomebrewSave(const shopsaves::ShopSave &save) {
  if (sdFree_ && *sdFree_ < save.size * 2 + (16u << 20)) {
    alertTitle_ = tr("Cannot apply the save");
    alert_ = trf("Not enough space on the SD card. {0} is needed.", {formatBytes(save.size * 2)});
    return;
  }
  if (!session_) return;
  const auto server = session_->server();
  const auto state = state_;
  const auto sd = sdRoot_;
  runPatchJob(save.id, Getting,
              [this, save, server, state, sd](net::Session &, PatchStore &, Error &error, std::string &done) {
                const auto root = sd + "/switch/cookingfoil/homebrew-saves";
                const auto staging = root + "/" + save.id + ".part";
                std::error_code ec;
                std::filesystem::remove_all(staging, ec);
                makeDirectories(staging);
                std::string failed;
                if (!net::downloadShopSave(server, state, save, staging, patchReceived_, error, &stop_) ||
                    !shopsaves::placeFiles(staging, sd + "/" + save.installRoot,
                                           root + "/" + save.homebrewApp + "/" + std::to_string(std::time(nullptr)),
                                           failed)) {
                  if (!failed.empty()) error.message = failed;
                  std::filesystem::remove_all(staging, ec);
                  return;
                }
                done = tr("Save applied. The app starts with it next time.");
              },
              tr("Could not apply the save"));
}
} // namespace foil::ui
