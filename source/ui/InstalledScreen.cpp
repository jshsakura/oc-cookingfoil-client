#include "ui/Application.hpp"
#include <set>
#include "platform/Installed.hpp"
#include "core/Log.hpp"
#include <algorithm>
namespace foil::ui {
void Application::refreshInstalled() {
  if (deviceBusy_ || deviceFixture_) return;
  if (installer_.running()) { message(tr("설치가 끝난 뒤 설치 목록을 새로고침합니다.")); return; }
  if (deviceWorker_.joinable()) deviceWorker_.join();
  installed_.installed.complete = false;
  deviceBusy_ = true;
  readingSaves_ = removalOperation_ = false;
  deviceError_ = {};
  deviceWorker_ = std::thread([this] {
    if (!guarded("installed", [this] {
          auto result = platform::readInstalled();
          incomingInstalled_ = std::move(result.value);
          incomingDeviceError_ = result.error;
        }))
      incomingDeviceError_.message = tr("설치 목록을 읽다 오류가 났습니다.");
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
  installed_.games = sortInstalled(std::move(installed_.games), installedSort_);
  rebuildHome();
  if (screen_ == Screen::Installed)
    selected_ = std::clamp(selected_, 0, std::max(0, int(installed_.games.size()) - 1));
  if (removalOperation_) {
    removalOperation_ = false;
    managedGame_.reset(); removal_.reset(); saves_.clear(); saveOpen_.reset();
    message(deviceError_ ? deviceError_.message : tr("삭제했습니다. 설치 목록을 새로고침했습니다."));
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
  if (installer_.running()) { message(tr("설치가 끝난 뒤 게임을 관리합니다.")); return; }
  // The list may be a search: its rows point into the games.
  const auto rows = gameRows(screen_ == Screen::Saves ? savesQuery_ : installedQuery_);
  if (rows.empty()) return;
  managedGame_ = installed_.games[rows[std::clamp(selected_, 0, int(rows.size()) - 1)]];
  manageRow_ = screen_ == Screen::Saves ? 1 : 0; // 세이브 has no game deletion row
  deviceError_ = {};
  if (deviceFixture_) return;
  saves_.clear();
  readingSaves_ = deviceBusy_ = true;
  const auto id = managedGame_->applicationId;
  deviceWorker_ = std::thread([this, id] {
    if (!guarded("saves", [this, id] {
          auto result = platform::readSaves(id);
          incomingSaves_ = std::move(result.value);
          incomingDeviceError_ = result.error;
        }))
      incomingDeviceError_.message = tr("세이브 목록을 읽다 오류가 났습니다.");
    deviceDone_ = true;
  });
}
void Application::performRemoval() {
  if (!removal_ || deviceBusy_) return;
  if (installer_.running()) { message(tr("설치가 끝난 뒤 삭제합니다.")); return; }
  const auto request = *removal_;
  deviceBusy_ = removalOperation_ = true;
  deviceWorker_ = std::thread([this, request] {
    if (!guarded("removal", [this, &request] {
          incomingDeviceError_ = platform::removeInstalled(request);
          if (!incomingDeviceError_) {
            auto refreshed = platform::readInstalled();
            incomingInstalled_ = std::move(refreshed.value);
            incomingDeviceError_ = refreshed.error;
          } else incomingInstalled_ = installed_;
        })) {
      incomingDeviceError_.message = tr("삭제 중 오류가 났습니다. 설치 목록을 새로고침합니다.");
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
      applySave_ = manageRow_ - 1;
      applyAsking_ = true;
    }
    return;
  }
  if (managedGame_) {
    if (deviceBusy_) return;
    if (a == Action::Back) { managedGame_.reset(); saves_.clear(); return; }
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
  paint_.fonts.text(deviceBusy_ && !managedGame_ ? tr("설치 목록을 읽고 있습니다.") : tr("본체에 설치된 게임을 관리합니다."), 260, 88, 19, Secondary);
  // Search at the right end, the order (Y, as 모든 게임's filter) left of
  // it, then 내 게임's tabs; - reloads the list.
  const auto sort = installedSortLabel(installedSort_);
  const float searchX = SearchX, sortW = myGamesButtonWidth(sort), sortX = searchX - 16 - sortW;
  searchBar(installedQuery_, searchX);
  headerItem("sort", sortX, 24, sortW, 48, [this] { cycleInstalledSort(1); });
  paint_.rect(sortX, 24, sortW, 48, Surface, 12);
  paint_.key("Y", sortX + 18, 48);
  paint_.icon(10, sortX + 60, 38, Ink, 20);
  paint_.fonts.middle(sort, sortX + 88, 48, 18, Ink, true, sortW - 106);
  const float tabsLeft = myGamesTabs(sortX - 16);
  pageTitle(tr("내 게임"), !deviceBusy_ || managedGame_ ? std::to_string(rows.size()) : std::string(),
            tabsLeft);
  if (deviceBusy_ && !managedGame_) {
    paint_.skeletonCards(260, 146, 12);
    return;
  }
  if (deviceError_ && !managedGame_) {
    notice(tr("설치 목록을 읽지 못했습니다."), deviceError_.message, 25);
    return;
  }
  if (installed_.games.empty()) {
    notice(tr("설치된 게임이 없습니다."), tr("서버에서 게임을 설치하면 여기에 보입니다."), 22);
    return;
  }
  if (rows.empty()) {
    notice(trf("'{0}'에 맞는 게임이 없습니다.", {installedQuery_}), tr("B로 검색어를 지웁니다."), 23);
    return;
  }
  // Games with a newer update on the server say so on their card.
  std::set<std::string> updatable;
  for (const auto &row : findUpdates(groups_, installed_.installed))
    if (row.item.type == "update") updatable.insert(row.baseId);
  const int first = std::max(0, selected_ / 6 - 1) * 6;
  paint_.scrollbar(1266, 146, 500, float(first / 6), 2.1f, float((rows.size() + 5) / 6));
  for (int n = 0; n < 18 && first + n < int(rows.size()); ++n) {
    const auto &g = installed_.games[rows[first + n]];
    const int x = 260 + n % 6 * 170, y = 146 + n / 6 * 240;
    std::string icon = "installed:" + g.titleId;
    if (!g.iconBytes.empty()) images_->provide(icon, g.iconBytes);
    else {
      const auto match = std::find_if(groups_.begin(), groups_.end(), [&](const auto &group) { return group.id == g.titleId; });
      icon = match == groups_.end() ? "" : match->representative.icon;
    }
    hit(x, y, 150, 220, [this, index = first + n] {
      sidebar_ = false;
      selected_ = index;
      openManagement();
    });
    cover(icon, x, y, 150, inContent() && selected_ == first + n && !managedGame_, g.name);
    paint_.fonts.text(g.name, x, y + 158, 18, Ink, false, 150);
    if (updatable.count(g.titleId)) paint_.pill(tr("업데이트|항목"), x + 8, y + 8);
    const float pill = paint_.pill(g.storage.size() > 1 ? tr("SD · 본체") : g.storage.empty() ? tr("설치됨") : g.storage.front(), x, y + 194);
    if (g.bytes)
      paint_.fonts.text(formatBytes(g.bytes), x + pill + 8, y + 196, 15, Secondary, false, 142 - pill);
  }
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
  const auto body = modal(860, height, managedGame_->name, tr("뒤로"), "", "", savesOnly ? 36 : 6);
  const float x = body.x + 32, w = body.w - 64;
  float y = body.y + 20;
  paint_.fonts.text(applying_ ? trf("{0} 적용 · 계정을 고릅니다", {applying_->name}) : managedGame_->titleId, x, y, 17,
                    applying_ ? Ink : Secondary, bool(applying_), w);
  y += 28;
  if (!savesOnly) {
    paint_.fonts.text(tr("게임 삭제"), x, y + 4, 19, Secondary, true);
    y += Label;
    hit(x, y, w, Row, [this] {
      manageRow_ = 0;
      installedAction(Action::Accept);
    });
    if (manageRow_ == 0 && !removal_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.icon(37, x + 20, y + Row / 2 - 13, Bad, 26);
    paint_.fonts.text(tr("게임 파일을 삭제합니다"), x + 62, y + 9, 21, Ink, true);
    paint_.fonts.text(tr("본편·업데이트·DLC를 삭제합니다. 세이브는 유지합니다."), x + 62, y + 39, 16, Secondary);
    y += Row + 16;
  }
  paint_.fonts.text(tr("세이브 백업·복원"), x, y + 4, 19, Secondary, true);
  y += Label;
  if (applyRunning_) {
    paint_.fonts.text(trf("세이브를 받아 적용하고 있습니다 · {0}", {formatBytes(saveCopied_.load())}), x, y + 6, 19, Ink, true);
    paint_.fonts.text(tr("끝날 때까지 본체를 끄지 않습니다."), x, y + 36, 17, Secondary);
  } else if (deviceBusy_ && !removalOperation_) {
    paint_.fonts.text(tr("세이브 목록을 읽고 있습니다."), x, y + 6, 19, Secondary);
  } else if (deviceError_) {
    paint_.fonts.wrapped(deviceError_.message, x, y + 4, 19, Secondary, w, 2);
  } else if (saves_.empty()) {
    paint_.fonts.wrapped(applying_ ? tr("이 게임의 세이브가 없습니다. 게임을 한 번 실행해 세이브를 만든 뒤 적용합니다.")
                                   : tr("이 게임의 세이브가 없습니다."),
                         x, y + 6, 19, Secondary, w, 2);
  } else {
    // One row per save: each account's, and the device save every account shares.
    const int first = std::clamp(manageRow_ - shown, 0, std::max(0, int(saves_.size()) - shown));
    for (int i = first; i < int(saves_.size()) && i < first + shown; ++i, y += Row + Gap) {
      const auto &save = saves_[i];
      const bool device = save.type == 3;
      hit(x, y, w, Row, [this, i] {
        manageRow_ = i + 1;
        installedAction(Action::Accept);
      });
      if (manageRow_ == i + 1 && !removal_) paint_.focusRow(x, y, w, Row, Ground, 12);
      else paint_.rect(x, y, w, Row, Ground, 12);
      paint_.icon(device ? 14 : 13, x + 20, y + Row / 2 - 13, Ink, 26);
      paint_.fonts.text(save.userName, x + 62, y + 9, 21, Ink, true, w - 220);
      paint_.fonts.text(applying_ && save.missing ? tr("이 계정에 세이브를 새로 만들어 적용합니다.")
                        : applying_             ? tr("이 계정에 적용합니다. 지금 세이브는 먼저 백업합니다.")
                        : save.missing          ? tr("아직 세이브가 없습니다. 백업을 복원하면 새로 만듭니다.")
                        : device                ? tr("모든 계정이 함께 쓰는 세이브를 백업·복원·삭제합니다.")
                                                : tr("이 계정의 세이브를 백업·복원·삭제합니다."),
                        x + 62, y + 39, 16, Secondary, false, w - 220);
      // A save's size; an account without one says so in a grey label.
      if (save.missing) {
        const auto label = tr("세이브 없음");
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
void Application::removalConfirm() {
  if (!removal_) return;
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 150});
  blockTouch();
  const bool save = removal_->kind == RemovalKind::Save;
  const auto warning = removal_->save.type == 3 ? std::string(tr("모든 계정이 함께 쓰는 공용 세이브를 삭제합니다. 되돌릴 수 없습니다."))
                       : trf("{0} 계정의 세이브를 삭제합니다. 되돌릴 수 없습니다.", {removal_->save.userName});
  // While it runs there is nothing to press: no footer keys.
  const auto body = modal(700, 20 + 30 + 84 + 16, save ? tr("세이브를 삭제합니까?") : tr("게임을 삭제합니까?"),
                          deviceBusy_ ? "" : tr("취소"), "", deviceBusy_ ? "" : tr("삭제합니다"), 37);
  paint_.fonts.text(managedGame_->name, body.x + 32, body.y + 20, 21, Ink, true, body.w - 64);
  paint_.fonts.wrapped(deviceBusy_ ? std::string(tr("삭제 결과를 확인하고 있습니다."))
                       : save      ? warning
                                   : std::string(tr("본편·업데이트·DLC를 삭제합니다. 계정의 세이브는 유지합니다.")),
                       body.x + 32, body.y + 56, 19, Secondary, body.w - 64, 3);
}
} // namespace foil::ui
