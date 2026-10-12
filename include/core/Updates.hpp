#pragma once
#include "core/Catalog.hpp"
namespace foil {
struct InstalledTitle {
  std::string titleId;
  uint32_t version = 0;
};
struct InstalledSnapshot {
  // Set only after the entire installed-title query succeeds.
  bool complete = false;
  std::vector<InstalledTitle> titles;
};
struct UpdateCandidate {
  std::string baseId;
  Item item;
  // Missing DLC has no installed version, including DLC with version zero.
  std::optional<uint32_t> installedVersion;
};
std::vector<UpdateCandidate> findUpdates(const std::vector<Group> &groups,
                                       const InstalledSnapshot &installed);
} // namespace foil
