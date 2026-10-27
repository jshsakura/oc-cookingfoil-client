#include "ui/Application.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <limits>
#include <map>
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
  updatesFocus_.clear();
  if (onTabs) headerFocus_ = "tab" + std::to_string(tab);
  else if (!sidebar_ && !hasFocusable()) headerFocus_ = "tab" + std::to_string(tab);
}
std::vector<UpdateCandidate> Application::pendingUpdates() const {
  if (updatesStale_ && installed_.installed.complete) {
    updates_ = findUpdates(groups_, installed_.installed);
    updatesStale_ = false;
  }
  // What waits in 다운로드 or comes down now cannot be asked for again.
  std::set<std::string> taken;
  for (const auto &e : history_)
    if ((e.outcome == history::Outcome::Queued || e.outcome == history::Outcome::Running) && e.kind.empty())
      for (const auto &file : e.install.files) taken.insert(file.url);
  if (taken.empty()) return updates_;
  std::vector<UpdateCandidate> out;
  for (const auto &row : updates_)
    if (!taken.count(row.item.url)) out.push_back(row);
  return out;
}
float Application::myGamesTabs(float right, float reserve) {
  const auto &all = pendingUpdates();
  const auto count = [&](const char *type) {
    return int(std::count_if(all.begin(), all.end(), [&](const auto &row) { return row.item.type == type; }));
  };
  // Short labels (Japanese runs long), and the strip ends where the title
  // and the Y button left of it (`reserve`) do.
  return headerTabs({tr("Installed|tab"), tr("Updates"), "DLC"}, myGamesTab(), [this](int n) { showMyGamesTab(n); }, right,
                    260 + paint_.fonts.measure(tr("My games"), 30, true) + 24 + reserve, {0, count("update"), count("dlc")});
}
// The Y button between 내 게임's tabs and search (the order on 설치됨,
// 모두 업데이트 or 모두 설치 on the others).
float Application::myGamesButtonWidth(const std::string &) {
  // One width on all three tabs, the longest label's, so switching tabs
  // never moves the tabs or the title.
  float widest = std::max(paint_.fonts.measure(tr("Update all"), 18, true), paint_.fonts.measure(tr("Install all"), 18, true));
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
                              const auto *group = findGroup(row.baseId);
                              return !textMatches({row.item.name, row.baseId,
                                                   group ? std::string_view(group->representative.name) : std::string_view()},
                                                  updatesQuery_);
                            }),
             rows.end());
  return rows;
}
void Application::updates() {
  const auto rows = updateRows();
  // Rows leave as they are queued and installed: the cursor stays on the
  // game it was on, and only when that one left does it take the row there.
  if (!rows.empty()) {
    const auto at = std::find_if(rows.begin(), rows.end(), [&](const UpdateCandidate &r) { return r.item.url == updatesFocus_; });
    if (at != rows.end()) selected_ = int(at - rows.begin());
    selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
    updatesFocus_ = rows[selected_].item.url;
  }
  uint64_t bytes = 0;
  for (const auto &row : rows) bytes += std::min(row.item.size, std::numeric_limits<uint64_t>::max() - bytes);

  // Y updates everything in the tab; its button only shows when there is
  // something to update.
  const bool canUpdate = installed_.installed.complete && !deviceBusy_ && !rows.empty();
  // As on 홈브류: search at the right end, the tabs left of it, then
  // 모두 업데이트 (Y) in the order's place.
  const auto yLabel = updatesTab_ == 2 ? tr("Install all") : tr("Update all");
  const float searchX = SearchX, w = myGamesButtonWidth(yLabel);
  searchBar(updatesQuery_, searchX);
  const float tabsLeft = myGamesTabs(searchX - 16, w + 12), buttonX = tabsLeft - 12 - w;
  // Always there, dimmed with nothing to update, so the tabs never move.
  if (canUpdate) headerItem("all", buttonX, 24, w, 48, [this] { action(Action::Queue); });
  paint_.rect(buttonX, 24, w, 48, Surface, 12);
  paint_.key("Y", buttonX + 18, 48);
  const auto ink = canUpdate ? Ink : Muted;
  paint_.icon(3, buttonX + 60, 38, ink, 20);
  paint_.fonts.middle(yLabel, buttonX + 88, 48, 18, ink, true, w - 106);
  pageTitle(tr("My games"),
            !updatesQuery_.empty() ? countLabel(rows.size(), updatesQuery_) : bytes ? std::to_string(rows.size()) + " · " + formatBytes(bytes) : std::to_string(rows.size()),
            buttonX);
  if (!connected_) {
    offlineNotice(tr("Connect to a server to check for updates."));
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
    notice(deviceError_ ? tr("Couldn't read installed games.") : tr("Installed list needed."),
           deviceError_ ? deviceError_.message : tr("Press - to read the console's installed games."), 25);
    return;
  }
  if (rows.empty() && !updatesQuery_.empty()) {
    notice(trf("No games match '{0}'.", {updatesQuery_}), tr("Press B to clear the search."), 23);
    return;
  }
  if (rows.empty()) {
    notice(tr("All up to date."), tr("No new updates or uninstalled DLC."), 24);
    return;
  }
  selected_ = std::clamp(selected_, 0, int(rows.size()) - 1);
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("updates", 232, 90, 1048, 660 - (90), float(std::max(0, selected_ - 2)),
                          float((int(rows.size())) - (4)), 124);
  const int first = std::max(0, int(top));
  paint_.scrollbar(1266, 90, 480, top, 4, float(rows.size()));
  clipTo(232, 90 - 8, 1048, 660 - 90 + 8);
  for (int i = first; i < int(rows.size()) && i <= first + 5; ++i) {
    const auto &row = rows[i];
    const auto *group = findGroup(row.baseId);
    const float y = 90 + (i - top) * 124;
    rowHit(260, y, 992, 108, 18, [this, i] {
      sidebar_ = false;
      selected_ = i;
    });
    if (inContent() && i == selected_) paint_.ring(260, y, 992, 108, 18);
    paint_.rect(260, y, 992, 108, Surface, 18);
    const auto name = group ? group->representative.name : row.item.name;
    cover(group ? group->representative.icon : row.item.icon, 282, y + 14, 80, false, name);
    if (inContent() && i == selected_) marquee(name, 382, y + 24, 22, 620);
    else paint_.fonts.text(name, 382, y + 24, 22, Ink, true, 620);
    std::string installed = row.installedVersion ? "v" + std::to_string(*row.installedVersion) : tr("None");
    const auto local = std::find_if(installed_.games.begin(), installed_.games.end(), [&](const auto &g) { return g.titleId == row.baseId; });
    if (row.item.type == "update" && local != installed_.games.end() && !local->displayVersion.empty())
      installed = local->displayVersion;
    // An update says which version it brings; a DLC says what it is.
    const auto detail = row.item.type == "dlc" ? row.item.name : installed + " → " + versionLabel(row.item);
    paint_.fonts.text(detail, 382, y + 65, 19, Secondary, false, 600);
    // Its kind, at the right end above the size.
    const std::string kind = row.item.type == "update" ? tr("Update|item") : row.installedVersion ? tr("DLC update") : tr("New DLC");
    paint_.pill(kind, 1230 - (paint_.fonts.measure(kind, 15, true) + 18), y + 22);
    const auto size = formatBytes(row.item.size);
    paint_.fonts.text(size, 1230 - paint_.fonts.measure(size, 19, true), y + 65, 19, Ink, true);
  }
  unclip();
}
void Application::installUpdates(bool all) {
  if (noInternet()) { alert_ = tr("Downloads start once the console is online."); return; }
  if (deviceBusy_ || !installed_.installed.complete) { message(tr("Updates start after installed versions are read.")); return; }
  const auto rows = updateRows();
  if (rows.empty()) return;
  const auto &selected = rows[std::clamp(selected_, 0, int(rows.size()) - 1)];
  // One pick a game, under that game's name and cover, in the list's
  // order: 다운로드 then shows which game each row is, and one that fails
  // takes only its own game's rows off the queue.
  std::vector<std::string> order;
  std::map<std::string, std::vector<Item>> files;
  for (const auto &row : rows) {
    if (!all && row.baseId != selected.baseId) continue;
    if (!files.count(row.baseId)) order.push_back(row.baseId);
    files[row.baseId].push_back(row.item);
  }
  // All in at once: queued together, saved and started once, one word.
  int picks = 0;
  for (const auto &base : order) {
    const auto *group = findGroup(base);
    const auto &first = files[base].front();
    const auto name = group ? group->representative.name : first.name;
    const auto icon = group ? group->representative.icon : first.icon;
    bool added = false;
    if (const auto refused = enqueueInstall(files[base], name, icon, installTarget(), {}, false, added))
      logLine("update all: " + name + ": " + refused.message); // that one stays out; the rest go in
    picks += added;
  }
  if (!picks) {
    message(tr("Already in the download queue."));
    return;
  }
  if (const auto result = launchAdded(picks))
    message(result.message);
  // The list stays (what was queued leaves it); 다운로드 shows the queue.
  selected_ = 0;
  updatesFocus_.clear();
}
} // namespace foil::ui
