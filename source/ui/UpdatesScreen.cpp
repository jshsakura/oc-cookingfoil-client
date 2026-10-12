#include "ui/Application.hpp"
#include <algorithm>
#include <limits>
namespace foil::ui {
// 내 게임: 설치됨 is the installed screen, 업데이트 and DLC this one. Moving
// between them keeps the console's list (no rereading) and the cursor's place:
// on the tabs it stays there, and a tab with nothing to pick takes it.
void Application::showMyGamesTab(int tab) {
  const bool onTabs = headerFocus_.rfind("tab", 0) == 0;
  notice_.clear();
  header_.clear();
  screen_ = tab == 0 ? Screen::Installed : Screen::Updates;
  if (tab) updatesTab_ = tab;
  selected_ = 0;
  if (onTabs) headerFocus_ = "tab" + std::to_string(tab);
  else if (!sidebar_ && !hasFocusable()) headerFocus_ = "tab" + std::to_string(tab);
}
const std::vector<UpdateCandidate> &Application::pendingUpdates() const {
  if (updatesStale_ && installed_.installed.complete) {
    updates_ = findUpdates(groups_, installed_.installed);
    updatesStale_ = false;
  }
  return updates_;
}
float Application::myGamesTabs(float right) {
  const auto &all = pendingUpdates();
  const auto count = [&](const char *type) {
    return int(std::count_if(all.begin(), all.end(), [&](const auto &row) { return row.item.type == type; }));
  };
  // Short labels (Japanese runs long), and the strip ends where the title does.
  return headerTabs({tr("설치됨|탭"), tr("업데이트|탭"), "DLC"}, myGamesTab(), [this](int n) { showMyGamesTab(n); }, right,
                    260 + paint_.fonts.measure(tr("내 게임"), 30, true) + 24, {0, count("update"), count("dlc")});
}
// The Y button between 내 게임's tabs and search (the order on 설치됨,
// 모두 업데이트 or 모두 설치 on the others).
float Application::myGamesButtonWidth(const std::string &) {
  // One width on all three tabs, the longest label's, so switching tabs
  // never moves the tabs or the title.
  float widest = std::max(paint_.fonts.measure(tr("모두 업데이트"), 18, true), paint_.fonts.measure(tr("모두 설치"), 18, true));
  for (int n = 0; n < InstalledSortCount; ++n)
    widest = std::max(widest, paint_.fonts.measure(installedSortLabel(InstalledSort(n)), 18, true));
  return widest + 106;
}
std::vector<UpdateCandidate> Application::updateRows() const {
  auto rows = pendingUpdates();
  const std::string type = updatesTab_ == 1 ? "update" : updatesTab_ == 2 ? "dlc" : "";
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [&](const auto &row) {
                              if (!type.empty() && row.item.type != type) return true;
                              const auto group = std::find_if(groups_.begin(), groups_.end(),
                                                              [&](const auto &g) { return g.id == row.baseId; });
                              return !textMatches({row.item.name, row.baseId,
                                                   group == groups_.end() ? std::string() : group->representative.name},
                                                  updatesQuery_);
                            }),
             rows.end());
  return rows;
}
void Application::updates() {
  const auto rows = updateRows();
  uint64_t bytes = 0;
  for (const auto &row : rows) bytes += std::min(row.item.size, std::numeric_limits<uint64_t>::max() - bytes);

  // Y updates everything in the tab; its button only shows when there is
  // something to update.
  const bool canUpdate = installed_.installed.complete && !deviceBusy_ && !rows.empty();
  // As on 설치됨: search at the right end, 모두 업데이트 (Y) in the order's
  // place, then the tabs.
  const auto yLabel = updatesTab_ == 2 ? tr("모두 설치") : tr("모두 업데이트");
  const float searchX = SearchX, w = myGamesButtonWidth(yLabel), buttonX = searchX - 16 - w;
  searchBar(updatesQuery_, searchX);
  // Always there, dimmed with nothing to update, so the tabs never move.
  if (canUpdate) headerItem("all", buttonX, 24, w, 48, [this] { action(Action::Queue); });
  paint_.rect(buttonX, 24, w, 48, Surface, 12);
  paint_.key("Y", buttonX + 18, 48);
  const auto ink = canUpdate ? Ink : SDL_Color{184, 182, 176, 255};
  paint_.icon(3, buttonX + 60, 38, ink, 20);
  paint_.fonts.middle(yLabel, buttonX + 88, 48, 18, ink, true, w - 106);
  const float tabsLeft = myGamesTabs(buttonX - 16);
  pageTitle(tr("내 게임"),
            bytes ? std::to_string(rows.size()) + " · " + formatBytes(bytes) : std::to_string(rows.size()),
            tabsLeft);
  if (!connected_) {
    notice(tr("서버에 연결되어 있지 않습니다."), tr("서버에 연결한 뒤 업데이트를 확인합니다."), 21);
    return;
  }
  // Pulsing rows only for a first read; a reread keeps the rows on screen.
  if (deviceBusy_ && !installed_.installed.complete) {
    // Same shape as the update rows, pulsing.
    for (int n = 0; n < 4; ++n) {
      const float y = 90 + n * 124;
      paint_.skeleton(260, y, 992, 108, 18);
    }
    return;
  }
  if (!installed_.installed.complete) {
    notice(deviceError_ ? tr("설치 목록을 읽지 못했습니다.") : tr("설치 목록이 필요합니다."),
           deviceError_ ? deviceError_.message : tr("- 버튼으로 본체의 설치 목록을 새로고침합니다."), 25);
    return;
  }
  if (rows.empty() && !updatesQuery_.empty()) {
    notice(trf("'{0}'에 맞는 게임이 없습니다.", {updatesQuery_}), tr("B로 검색어를 지웁니다."), 23);
    return;
  }
  if (rows.empty()) {
    notice(tr("모두 최신입니다."), tr("새 업데이트나 설치하지 않은 DLC가 없습니다."), 24);
    return;
  }
  selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
  const int first = std::max(0, selected_ - 2);
  paint_.scrollbar(1266, 90, 480, float(first), 4, float(rows.size()));
  for (int i = first; i < int(rows.size()) && i < first + 4; ++i) {
    const auto &row = rows[i];
    const auto group = std::find_if(groups_.begin(), groups_.end(), [&](const auto &g) { return g.id == row.baseId; });
    const int y = 90 + (i - first) * 124;
    hit(260, y, 992, 108, [this, i] {
      sidebar_ = false;
      selected_ = i;
    });
    if (inContent() && i == selected_) paint_.ring(260, y, 992, 108, 18);
    paint_.rect(260, y, 992, 108, Surface, 18);
    const auto name = group == groups_.end() ? row.item.name : group->representative.name;
    cover(group == groups_.end() ? row.item.icon : group->representative.icon, 282, y + 14, 80, false, name);
    paint_.fonts.text(name, 382, y + 24, 22, Ink, true, 620);
    std::string installed = row.installedVersion ? "v" + std::to_string(*row.installedVersion) : tr("없음");
    const auto local = std::find_if(installed_.games.begin(), installed_.games.end(), [&](const auto &g) { return g.titleId == row.baseId; });
    if (row.item.type == "update" && local != installed_.games.end() && !local->displayVersion.empty())
      installed = local->displayVersion;
    // An update says which version it brings; a DLC says what it is.
    const auto detail = row.item.type == "dlc" ? row.item.name : installed + " → " + versionLabel(row.item);
    paint_.fonts.text(detail, 382, y + 65, 19, Secondary, false, 600);
    // Its kind, at the right end above the size.
    const std::string kind = row.item.type == "update" ? tr("업데이트|항목") : row.installedVersion ? tr("DLC 업데이트") : tr("새 DLC");
    paint_.pill(kind, 1230 - (paint_.fonts.measure(kind, 15, true) + 18), y + 22);
    const auto size = formatBytes(row.item.size);
    paint_.fonts.text(size, 1230 - paint_.fonts.measure(size, 19, true), y + 65, 19, Ink, true);
  }
  paint_.fonts.text(tr("설치된 본편의 최신 업데이트와 미설치·최신 DLC만 표시합니다."), 260, 606, 18, Secondary);
}
void Application::installUpdates(bool all) {
  if (deviceBusy_ || !installed_.installed.complete) { message(tr("설치된 버전 조회가 끝난 뒤 업데이트합니다.")); return; }
  const auto rows = updateRows();
  if (rows.empty()) return;
  const auto &selected = rows[std::clamp(selected_, 0, int(rows.size()) - 1)];
  std::vector<Item> files;
  for (const auto &row : rows) if (all || row.baseId == selected.baseId) files.push_back(row.item);
  const auto name = all ? std::string(updatesTab_ == 2 ? tr("모두 설치") : tr("모두 업데이트")) : selected.item.name;
  if (const auto result = startInstall(files, name, selected.item.icon, installTarget())) {
    message(result.message);
    return;
  }
  downloadsTab_ = 0;
  screen_ = Screen::Downloads; nav_ = menuIndex(Screen::Downloads);
}
} // namespace foil::ui
