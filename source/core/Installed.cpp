#include "core/Installed.hpp"
#include <set>
#include "core/I18n.hpp"
#include <algorithm>
namespace foil {
const char *installedSortLabel(InstalledSort sort) {
  return sort == InstalledSort::Size     ? tr("Size")
         : sort == InstalledSort::Recent ? tr("Recent")
                                         : tr("Name|sort");
}
std::vector<InstalledGame> sortInstalled(std::vector<InstalledGame> games,
                                         InstalledSort sort) {
  std::stable_sort(games.begin(), games.end(), [sort](const auto &a, const auto &b) {
    if (sort == InstalledSort::Size && a.bytes != b.bytes)
      return a.bytes > b.bytes;
    if (sort == InstalledSort::Recent && a.lastUpdated != b.lastUpdated)
      return a.lastUpdated > b.lastUpdated;
    return a.name < b.name;
  });
  return games;
}
Error validateRemoval(const RemovalRequest &request,
                      const InstalledLibrary &library,
                      const std::vector<SaveEntry> &saves,
                      uint64_t runningApplication, bool installing) {
  if (installing)
    return {tr("Manage games after the install finishes.")};
  // 0 means no game runs (opened from the album); otherwise that game stays.
  if (runningApplication && request.applicationId == runningApplication)
    return {tr("The running game cannot be deleted. Close it or launch the app through another game.")};
  if (!library.installed.complete)
    return {tr("Refresh the installed list, then delete.")};
  const auto found = std::find_if(library.games.begin(), library.games.end(),
                                [&](const auto &g) { return g.applicationId == request.applicationId; });
  if (found == library.games.end() || request.applicationId < 0x0100000000000000ULL ||
      request.applicationId >= 0x0200000000000000ULL || (request.applicationId & 0xFFF))
    return {tr("The installed base game was not found. Refresh the list.")};
  if (request.kind == RemovalKind::Game)
    return {};
  const auto &s = request.save;
  const bool account = s.type == 1 && (s.user[0] || s.user[1]);
  const bool shared = s.type == 3 && !s.user[0] && !s.user[1];
  if ((!account && !shared) || s.space != 1 || s.rank != 0 || !s.saveId ||
      s.applicationId != request.applicationId)
    return {tr("Only a selected account's save or a shared game save can be deleted.")};
  const auto current = std::find_if(saves.begin(), saves.end(), [&](const auto &v) {
    return v.saveId == s.saveId && v.applicationId == s.applicationId &&
           v.user == s.user && v.type == s.type && v.space == s.space && v.rank == s.rank;
  });
  if (current == saves.end())
    return {tr("The save data changed. Refresh and select again.")};
  return {};
}
std::optional<std::vector<std::string>> orphanContents(const std::vector<std::string> &stored,
                                                       const std::vector<std::string> &referenced) {
  if (!stored.empty() && referenced.empty())
    return std::nullopt;
  std::set<std::string> used(referenced.begin(), referenced.end());
  std::vector<std::string> out;
  for (const auto &id : stored)
    if (!used.count(id))
      out.push_back(id);
  return out;
}
} // namespace foil
