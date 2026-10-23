#include "ui/Application.hpp"
#include "platform/Installed.hpp"
#include "platform/Cores.hpp"
#include "core/Log.hpp"
#include <algorithm>
namespace foil::ui {
void Application::refreshInstalled(std::function<void()> first) {
  if (deviceBusy_ || deviceFixture_ || installer_.running()) {
    // No read now, but what had to come first still runs, off the screen.
    if (first) runOffScreen(std::move(first));
    if (installer_.running()) message(tr("Refresh the list after the install finishes."));
    return;
  }
  if (deviceWorker_.joinable()) deviceWorker_.join();
  // The list on screen stays until the new one arrives: no blank flash.
  deviceBusy_ = true;
  readingSaves_ = removalOperation_ = false;
  deviceError_ = {};
  deviceWorker_ = std::thread([this, first = std::move(first)] {
    const platform::OnCore off(platform::WriterCore); // the drawing core keeps the screen
    if (first && !guarded("installed first", first)) logLine("installed: the step before the read failed");
    if (!guarded("installed", [this] {
          auto result = platform::readInstalled();
          incomingInstalled_ = std::move(result.value);
          incomingDeviceError_ = result.error;
        }))
      incomingDeviceError_.message = tr("Error reading installed games.");
    deviceDone_ = true;
  });
}
void Application::pollDevice() {
  if (!deviceDone_.exchange(false)) return;
  deviceWorker_.join();
  deviceBusy_ = false;
  deviceError_ = incomingDeviceError_;
  if (readingSaves_) {
    saves_ = std::move(incomingSaves_);
    // Accounts without a save yet come after the saves there are.
    std::stable_partition(saves_.begin(), saves_.end(), [](const SaveEntry &e) { return !e.missing; });
    readingSaves_ = false;
    return;
  }
  installed_ = std::move(incomingInstalled_);
  installedAt_ = SDL_GetTicks();
  updatesStale_ = true;
  installed_.games = sortInstalled(std::move(installed_.games), installedSort_);
  // The home is built from the whole shop: only when what is on the console
  // changed, not after every finished download (an update or a DLC does not).
  if (installedMaskNow() != installedMask_) rebuildHome();
  if (screen_ == Screen::Installed)
    selected_ = std::clamp(selected_, 0, std::max(0, int(installed_.games.size()) - 1));
  if (removalOperation_) {
    removalOperation_ = false;
    managedGame_.reset(); removal_.reset(); saves_.clear(); saveOpen_.reset();
    message(deviceError_ ? deviceError_.message : tr("Deleted. The installed list was refreshed."));
  }
}
void Application::cycleInstalledSort(int step) {
  const auto &games = installed_.games;
  const auto current = games.empty() ? 0 : games[std::clamp(selected_, 0, int(games.size()) - 1)].applicationId;
  installedSort_ = InstalledSort((int(installedSort_) + step + InstalledSortCount) % InstalledSortCount);
  installed_.games = sortInstalled(std::move(installed_.games), installedSort_);
  const auto found = std::find_if(games.begin(), games.end(), [&](const auto &g) { return g.applicationId == current; });
  selected_ = found == games.end() ? 0 : int(found - games.begin());
}
void Application::openManagement() {
  if (deviceBusy_ || !installed_.installed.complete || installed_.games.empty()) return;
  if (installer_.running()) { message(tr("Manage games after the install finishes.")); return; }
  // The list may be a search, and 세이브's has its own order: its rows
  // point into the games as they are drawn.
  const auto rows = screen_ == Screen::Saves ? saveGameRows() : gameRows(installedQuery_);
  if (rows.empty()) return;
  managedGame_ = installed_.games[rows[std::clamp(selected_, 0, int(rows.size()) - 1)]];
  manageRow_ = screen_ == Screen::Saves ? 1 : 0; // 세이브 has no game deletion row
  deviceError_ = {};
  if (deviceFixture_) return;
  saves_.clear();
  readingSaves_ = deviceBusy_ = true;
  const auto id = managedGame_->applicationId;
  deviceWorker_ = std::thread([this, id] {
    const platform::OnCore off(platform::WriterCore);
    if (!guarded("saves", [this, id] {
          auto result = platform::readSaves(id);
          incomingSaves_ = std::move(result.value);
          incomingDeviceError_ = result.error;
        }))
      incomingDeviceError_.message = tr("Error reading saves.");
    deviceDone_ = true;
  });
}
void Application::performRemoval() {
  if (!removal_ || deviceBusy_) return;
  if (installer_.running()) { message(tr("Delete after the install finishes.")); return; }
  const auto request = *removal_;
  deviceBusy_ = removalOperation_ = true;
  deviceWorker_ = std::thread([this, request] {
    const platform::OnCore off(platform::WriterCore);
    if (!guarded("removal", [this, &request] {
          incomingDeviceError_ = platform::removeInstalled(request);
          if (!incomingDeviceError_) {
            auto refreshed = platform::readInstalled();
            incomingInstalled_ = std::move(refreshed.value);
            incomingDeviceError_ = refreshed.error;
          } else incomingInstalled_ = installed_;
        })) {
      incomingDeviceError_.message = tr("Error while deleting. Refreshing the installed list.");
      incomingInstalled_ = installed_;
    }
    deviceDone_ = true;
  });
}
void Application::installedAction(Action a) {
  if (removal_) {
    if (deviceBusy_) return;
    // One decision in the footer: A deletes, B backs out. An A pressed
    // right as the popup opens (a double press) is not taken as a yes.
    constexpr uint32_t SettleMs = 700;
    if (a == Action::Back) removal_.reset();
    if (a == Action::Accept && SDL_GetTicks() - removalAt_ >= SettleMs) performRemoval();
    return;
  }
  if (managedGame_ && saveOpen_) {
    saveAction(a);
    return;
  }
  // A shop save picked in 세이브: the rows choose its account.
  if (managedGame_ && applying_) {
    if (applyAsking_) {
      if (a == Action::Back) applyAsking_ = false;
      if (a == Action::Accept) {
        applyAsking_ = false;
        applyShopSave();
      }
      return;
    }
    if (deviceBusy_) return;
    if (a == Action::Back) { managedGame_.reset(); applying_.reset(); saves_.clear(); return; }
    if (a == Action::Up) manageRow_ = std::max(1, manageRow_ - 1);
    if (a == Action::Down) manageRow_ = std::min(int(saves_.size()), manageRow_ + 1);
    if (a == Action::Accept && manageRow_ > 0 && manageRow_ <= int(saves_.size())) {
      // A save the server marks shared goes only into the shared save, and so on.
      if (auto error = shopsaves::targetMismatch(*applying_, saves_[manageRow_ - 1].device())) {
        message(error.message);
        return;
      }
      applySave_ = manageRow_ - 1;
      applyAsking_ = true;
    }
    return;
  }
  if (managedGame_) {
    if (deviceBusy_) return;
    if (a == Action::Back) { managedGame_.reset(); saves_.clear(); return; }
    if (a == Action::Queue && screen_ != Screen::Saves) {
      launchManagedGame();
      return;
    }
    if (a == Action::Up) manageRow_ = std::max(screen_ == Screen::Saves ? 1 : 0, manageRow_ - 1);
    if (a == Action::Down) manageRow_ = std::min(int(saves_.size()), manageRow_ + 1);
    if (a == Action::Accept && manageRow_ > 0) {
      openSave(manageRow_ - 1);
    } else if (a == Action::Accept) {
      RemovalRequest request;
      request.applicationId = managedGame_->applicationId;
      removal_ = request;
    removalAt_ = SDL_GetTicks();
    }
    return;
  }
  if (a == Action::Queue) { cycleInstalledSort(1); return; }
  if (a == Action::Reload) { refreshInstalled(); return; }
  if (a == Action::Search) { requestSearch(); return; }
  const int count = int(gameRows(installedQuery_).size());
  if (a == Action::Up) selected_ = std::max(0, selected_ - 6);
  if (a == Action::Down) selected_ = std::min(count - 1, selected_ + 6);
  if (a == Action::Left) {
    if (selected_ % 6 == 0) sidebar_ = true;
    else --selected_;
  }
  if (a == Action::Right) selected_ = std::min(count - 1, selected_ + 1);
  selected_ = std::max(0, selected_);
  if (a == Action::Accept) openManagement();
}
void Application::installedScreen() {
  const auto rows = gameRows(installedQuery_);
  // A first read pulses; a reread keeps the list it already has.
  const bool firstRead = deviceBusy_ && !installed_.installed.complete;
  // As on 홈브류: search at the right end, 내 게임's tabs left of it, then
  // the order (Y); - reloads the list.
  const auto sort = installedSortLabel(installedSort_);
  const float searchX = SearchX, sortW = myGamesButtonWidth(sort);
  searchBar(installedQuery_, searchX);
  const float tabsLeft = myGamesTabs(searchX - 16, sortW + 12), sortX = tabsLeft - 12 - sortW;
  headerItem("sort", sortX, 24, sortW, 48, [this] { cycleInstalledSort(1); });
  paint_.rect(sortX, 24, sortW, 48, Surface, 12);
  paint_.key("Y", sortX + 18, 48);
  paint_.icon(10, sortX + 60, 38, Ink, 20);
  paint_.fonts.middle(sort, sortX + 88, 48, 18, Ink, true, sortW - 106);
  pageTitle(tr("My games"), !firstRead || managedGame_ ? countLabel(rows.size(), installedQuery_) : std::string(),
            sortX);
  if (firstRead && !managedGame_) {
    paint_.skeletonCards(260, 104, 12);
    return;
  }
  if (deviceError_ && !managedGame_) {
    notice(tr("Couldn't read installed games."), deviceError_.message, 25);
    return;
  }
  if (installed_.games.empty()) {
    notice(tr("No games installed."), tr("Games you install from the server appear here."), 22);
    return;
  }
  if (rows.empty()) {
    notice(trf("No games match '{0}'.", {installedQuery_}), tr("Press B to clear the search."), 23);
    return;
  }
  // Games with a newer update on the server say so on their card.
  const auto &pending = pendingUpdates();
  const auto updatable = [&](const std::string &titleId) {
    return std::any_of(pending.begin(), pending.end(),
                       [&](const auto &row) { return row.baseId == titleId && row.item.type == "update"; });
  };
  // The row at the top follows the cursor, or the finger while it glides.
  const int rowCount = int((rows.size() + 5) / 6);
  const float top = glide("installed", 232, 104 - 8, 1048, 660 - 104 + 8, float(std::max(0, selected_ / 6 - 1)),
                          float(rowCount - 2), CardRowStep);
  const int first = std::max(0, int(top)) * 6;
  paint_.scrollbar(1266, 104, 500, top, 2.1f, float(rowCount));
  clipTo(232, 104 - 8, 1048, 660 - 104 + 8);
  for (int n = 0; n < 24 && first + n < int(rows.size()); ++n) {
    const auto &g = installed_.games[rows[first + n]];
    const float x = 260 + n % 6 * 170, y = 104 + (n / 6 + first / 6 - top) * CardRowStep;
    const auto icon = installedIcon(g);
    hit(x, y, 150, 220, [this, index = first + n] {
      sidebar_ = false;
      selected_ = index;
      openManagement();
    });
    cover(icon, x, y, 150, inContent() && selected_ == first + n && !managedGame_, g.name);
    if (inContent() && selected_ == first + n && !managedGame_) marquee(g.name, x, y + 158, 18, 150, false);
    else paint_.fonts.text(g.name, x, y + 158, 18, Ink, false, 150);
    if (updatable(g.titleId)) paint_.pill(tr("Update|item"), x + 8, y + 8);
    const float pill = paint_.pill(g.storage.size() > 1 ? tr("SD · Console") : g.storage.empty() ? tr("Installed") : g.storage.front(), x, y + CardLabelTop);
    if (g.bytes)
      paint_.fonts.text(formatBytes(g.bytes), x + pill + 8, y + CardLabelTop + 2, 15, Secondary, false, 142 - pill);
  }
  unclip();
  if (managedGame_) management();
}
void Application::management() {
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  if (saveOpen_) {
    savePanel();
    removalConfirm();
    return;
  }
  // From 세이브 the popup is about saves only: no game deletion row.
  const bool savesOnly = screen_ == Screen::Saves;
  constexpr float Row = 68, Gap = 10, Label = 34;
  const int shown = savesOnly ? 4 : 3;
  const int listed = std::max(1, std::min(shown, int(saves_.size())));
  const float height = 20 + 28 + (savesOnly ? 0 : Label + Row + 16) + Label + listed * (Row + Gap) + 6;
  // Y starts the game, as the HOME Menu would (내 게임 only).
  const auto body = modal(860, height, managedGame_->name, tr("Back"), savesOnly ? "" : tr("Launch|game"), "", savesOnly ? 36 : 6);
  const float x = body.x + 32, w = body.w - 64;
  float y = body.y + 20;
  paint_.fonts.text(applying_ ? trf("Apply {0} · pick where it goes", {applying_->name}) : managedGame_->titleId, x, y, 17,
                    applying_ ? Ink : Secondary, bool(applying_), w);
  y += 28;
  if (!savesOnly) {
    paint_.fonts.text(tr("Delete game"), x, y + 4, 19, Secondary, true);
    y += Label;
    rowHit(x, y, w, Row, 12, [this] {
      manageRow_ = 0;
      installedAction(Action::Accept);
    });
    if (manageRow_ == 0 && !removal_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.icon(37, x + 20, y + Row / 2 - 13, Bad, 26);
    paint_.fonts.text(tr("Delete game files"), x + 62, y + 9, 21, Ink, true);
    paint_.fonts.text(tr("Deletes the game, updates and DLC. Saves are kept."), x + 62, y + 39, 16, Secondary);
    y += Row + 16;
  }
  paint_.fonts.text(tr("Save backup and restore"), x, y + 4, 19, Secondary, true);
  y += Label;
  if (applyRunning_) {
    paint_.fonts.text(trf("Getting and applying the save · {0}", {formatBytes(saveCopied_.load())}), x, y + 6, 19, Ink, true);
    paint_.fonts.text(tr("Keep the console on until it finishes."), x, y + 36, 17, Secondary);
  } else if (deviceBusy_ && !removalOperation_) {
    // The save rows' shape while they are read.
    for (int n = 0; n < listed; ++n) paint_.skeleton(x, y + n * (Row + Gap), w, Row, 14);
  } else if (deviceError_) {
    paint_.fonts.wrapped(deviceError_.message, x, y + 4, 19, Secondary, w, 2);
  } else if (saves_.empty()) {
    paint_.fonts.wrapped(applying_ ? tr("This game has no save yet. Play it once to make one, then apply.")
                                   : tr("This game has no saves."),
                         x, y + 6, 19, Secondary, w, 2);
  } else {
    // One row per save: each account's, and the device save every account shares.
    const int first = std::clamp(manageRow_ - shown, 0, std::max(0, int(saves_.size()) - shown));
    for (int i = first; i < int(saves_.size()) && i < first + shown; ++i, y += Row + Gap) {
      const auto &save = saves_[i];
      const bool device = save.device();
      rowHit(x, y, w, Row, 12, [this, i] {
        manageRow_ = i + 1;
        installedAction(Action::Accept);
      });
      if (manageRow_ == i + 1 && !removal_) paint_.focusRow(x, y, w, Row, Ground, 12);
      else paint_.rect(x, y, w, Row, Ground, 12);
      paint_.icon(device ? 14 : 13, x + 20, y + Row / 2 - 13, Ink, 26);
      paint_.fonts.text(save.userName, x + 62, y + 9, 21, Ink, true, w - 220);
      paint_.fonts.text(applying_ && device && save.missing ? tr("Creates the shared save and applies it there.")
                        : applying_ && device             ? tr("Applies to the shared save. The current save is backed up first.")
                        : applying_ && save.missing       ? tr("Creates a save for this account and applies it.")
                        : applying_                       ? tr("Apply to this account. The current save is backed up first.")
                        : save.missing          ? tr("No save yet. Restoring a backup creates one.")
                        : device                ? tr("Back up, restore or delete the save all accounts share.")
                                                : tr("Back up, restore or delete this account's save."),
                        x + 62, y + 39, 16, Secondary, false, w - 220);
      // A save's size; an account without one says so in a grey label.
      if (save.missing) {
        const auto label = tr("No save");
        paint_.pill(label, x + w - 20 - paint_.fonts.measure(label, 15, true) - 18, y + Row / 2 - 13);
      } else {
        const auto size = formatBytes(save.bytes);
        paint_.fonts.middle(size, x + w - 20 - paint_.fonts.measure(size, 18, true), y + Row / 2, 18, Ink, true);
      }
    }
  }
  removalConfirm();
  applyConfirm();
}
// The game picked in 내 게임, started now. Starting it closes this app, so
// not while something downloads.
void Application::launchManagedGame() {
  if (!managedGame_) return;
  if (downloadRunning()) {
    message(tr("Start the game after the downloads finish."));
    return;
  }
  // The system asks this app to close; it winds down as on any exit.
  if (auto error = platform::launchApplication(managedGame_->applicationId)) {
    alertTitle_ = tr("Could not start the game.");
    alert_ = error.message;
  }
}
void Application::removalConfirm() {
  if (!removal_) return;
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  const bool save = removal_->kind == RemovalKind::Save;
  const auto warning = removal_->save.device() ? std::string(tr("Deletes the save shared by all users. This can't be undone."))
                       : trf("Deletes {0}'s save. This can't be undone.", {removal_->save.userName});
  // While it runs there is nothing to press: no footer keys.
  const auto body = modal(700, 20 + 30 + 84 + 16, save ? tr("Delete save?") : tr("Delete game?"),
                          deviceBusy_ ? "" : tr("Cancel"), "", deviceBusy_ ? "" : tr("Delete|confirm"), 37);
  paint_.fonts.text(managedGame_->name, body.x + 32, body.y + 20, 21, Ink, true, body.w - 64);
  paint_.fonts.wrapped(deviceBusy_ ? std::string(tr("Checking the result."))
                       : save      ? warning
                                   : std::string(tr("Deletes the game, updates and DLC. User saves are kept.")),
                       body.x + 32, body.y + 56, 19, Secondary, body.w - 64, 3);
}
} // namespace foil::ui
