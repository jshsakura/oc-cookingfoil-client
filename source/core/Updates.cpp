#include "core/Updates.hpp"
#include <algorithm>
#include <map>
namespace foil {
namespace {
std::string titleKey(std::string id) {
  if (id.size() != 16)
    return {};
  for (auto &c : id) {
    if (c >= 'a' && c <= 'f')
      c -= 'a' - 'A';
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
      return {};
  }
  return id;
}
} // namespace
std::vector<UpdateCandidate> findUpdates(const std::vector<Group> &groups,
                                       const InstalledSnapshot &installed) {
  std::vector<UpdateCandidate> result;
  if (!installed.complete)
    return result;
  std::map<std::string, uint32_t> versions;
  for (const auto &title : installed.titles) {
    const auto key = titleKey(title.titleId);
    if (!key.empty())
      versions[key] = std::max(versions[key], title.version);
  }
  for (const auto &group : groups) {
    const auto base = versions.find(titleKey(group.id));
    if (base == versions.end())
      continue;
    if (group.update) {
      const auto key = titleKey(group.update->titleId);
      const auto patch = versions.find(key);
      const auto version = patch == versions.end()
                               ? base->second
                               : std::max(base->second, patch->second);
      if (!key.empty() && group.update->version > version)
        result.push_back({group.id, *group.update, version});
    }
    for (const auto &dlc : group.dlc) {
      const auto key = titleKey(dlc.titleId);
      if (key.empty())
        continue;
      const auto found = versions.find(key);
      if (found == versions.end())
        result.push_back({group.id, dlc, std::nullopt});
      else if (dlc.version > found->second)
        result.push_back({group.id, dlc, found->second});
    }
  }
  return result;
}
} // namespace foil
