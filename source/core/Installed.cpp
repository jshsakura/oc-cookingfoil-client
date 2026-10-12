#include "core/Installed.hpp"
#include <set>
#include "core/I18n.hpp"
#include <algorithm>
namespace foil {
const char *installedSortLabel(InstalledSort sort) {
  return sort == InstalledSort::Size     ? tr("용량순")
         : sort == InstalledSort::Recent ? tr("최근 설치순")
                                         : tr("이름순");
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
                      uint64_t runningApplication, bool applicationMode,
                      bool installing) {
  if (!applicationMode)
    return {tr("게임을 R 버튼을 누른 채 실행한 애플리케이션 모드에서 삭제합니다.")};
  if (installing)
    return {tr("설치가 끝난 뒤 게임을 관리합니다.")};
  if (!runningApplication || request.applicationId == runningApplication)
    return {tr("현재 실행 중인 게임은 삭제할 수 없습니다. 다른 게임으로 앱을 실행합니다.")};
  if (!library.installed.complete)
    return {tr("설치 목록을 새로고침한 뒤 삭제합니다.")};
  const auto found = std::find_if(library.games.begin(), library.games.end(),
                                [&](const auto &g) { return g.applicationId == request.applicationId; });
  if (found == library.games.end() || request.applicationId < 0x0100000000000000ULL ||
      request.applicationId >= 0x0200000000000000ULL || (request.applicationId & 0xFFF))
    return {tr("설치된 본편을 찾지 못했습니다. 목록을 새로고침합니다.")};
  if (request.kind == RemovalKind::Game)
    return {};
  const auto &s = request.save;
  const bool account = s.type == 1 && (s.user[0] || s.user[1]);
  const bool shared = s.type == 3 && !s.user[0] && !s.user[1];
  if ((!account && !shared) || s.space != 1 || s.rank != 0 || !s.saveId ||
      s.applicationId != request.applicationId)
    return {tr("선택한 계정 또는 공용 게임 세이브만 삭제할 수 있습니다.")};
  const auto current = std::find_if(saves.begin(), saves.end(), [&](const auto &v) {
    return v.saveId == s.saveId && v.applicationId == s.applicationId &&
           v.user == s.user && v.type == s.type && v.space == s.space && v.rank == s.rank;
  });
  if (current == saves.end())
    return {tr("세이브 정보가 바뀌었습니다. 새로고침한 뒤 선택합니다.")};
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
