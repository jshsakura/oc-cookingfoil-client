#include "core/Selection.hpp"
#include <limits>
namespace foil {
Selection::Selection(const Group &group, std::string storage)
    : target(std::move(storage)) {
  if (group.base) {
    parts_.push_back(*group.base);
    bases_.push_back(*group.base);
    bases_.insert(bases_.end(), group.alternatives.begin(), group.alternatives.end());
  }
  if (group.update)
    parts_.push_back(*group.update);
  for (const auto &dlc : group.dlc)
    parts_.push_back(dlc);
  selected_.assign(parts_.size(), true);
}
void Selection::setPatches(std::vector<UserPatch> patches) {
  std::map<std::string, bool> on;
  for (const auto &patch : patches) {
    const auto kept = patchOn_.find(patch.id);
    on[patch.id] = kept != patchOn_.end() ? kept->second : patch.required;
  }
  offered_ = std::move(patches);
  patchOn_ = std::move(on);
  showPatches();
}
std::string Selection::installedVersion() const {
  std::string base;
  for (size_t i = 0; i < parts_.size(); ++i) {
    if (parts_[i].type == "update" && selected_[i] && !parts_[i].versionName.empty()) return parts_[i].versionName;
    if (parts_[i].type == "base") base = parts_[i].versionName;
  }
  return base;
}
void Selection::showPatches() {
  const auto version = installedVersion();
  patches_.clear();
  for (const auto &patch : offered_)
    if (patchFitsVersion(patch, version)) patches_.push_back(patch);
}
bool Selection::toggle(size_t i) {
  if (i >= parts_.size()) {
    if (i - parts_.size() >= patches_.size())
      return false;
    auto &on = patchOn_[patches_[i - parts_.size()].id];
    on = !on;
    return true;
  }
  if (parts_[i].type == "base") {
    if (i != 0 || bases_.size() < 2)
      return false;
    baseAt_ = (baseAt_ + 1) % bases_.size();
    parts_[0] = bases_[baseAt_];
    showPatches(); // its version may differ
    return true;
  }
  selected_[i] = !selected_[i];
  // The update on or off changes which game version the patches meet.
  if (parts_[i].type == "update") showPatches();
  return true;
}
bool Selection::enabled(size_t i) const {
  if (i >= parts_.size()) {
    if (i - parts_.size() >= patches_.size()) return false;
    const auto on = patchOn_.find(patches_[i - parts_.size()].id);
    return on != patchOn_.end() && on->second;
  }
  return selected_[i];
}
const UserPatch *Selection::patch(size_t row) const {
  return row >= parts_.size() && row - parts_.size() < patches_.size() ? &patches_[row - parts_.size()] : nullptr;
}
uint64_t Selection::total() const {
  uint64_t sum = 0;
  const auto add = [&sum](uint64_t size) { sum = UINT64_MAX - sum < size ? UINT64_MAX : sum + size; };
  for (size_t i = 0; i < parts_.size(); ++i)
    if (selected_[i]) add(parts_[i].size);
  for (const auto &patch : patchesOn())
    add(patch.size);
  return sum;
}
std::vector<Item> Selection::files() const {
  std::vector<Item> files;
  for (size_t i = 0; i < parts_.size(); ++i)
    if (selected_[i])
      files.push_back(parts_[i]);
  return files;
}
std::vector<UserPatch> Selection::patchesOn() const {
  std::vector<UserPatch> on;
  for (size_t row = parts_.size(); row < rows(); ++row)
    if (enabled(row)) on.push_back(patches_[row - parts_.size()]);
  return on;
}
size_t Selection::count() const { return files().size() + patchesOn().size(); }
} // namespace foil
