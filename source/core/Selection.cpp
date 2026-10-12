#include "core/Selection.hpp"
#include <limits>
namespace foil {
Selection::Selection(const Group &group, std::string storage)
    : target(std::move(storage)) {
  if (group.base)
    parts_.push_back(*group.base);
  if (group.update)
    parts_.push_back(*group.update);
  for (const auto &dlc : group.dlc)
    parts_.push_back(dlc);
  selected_.assign(parts_.size(), true);
}
bool Selection::toggle(size_t i) {
  if (i >= parts_.size() || parts_[i].type == "base")
    return false;
  selected_[i] = !selected_[i];
  return true;
}
bool Selection::enabled(size_t i) const {
  return i < selected_.size() && selected_[i];
}
uint64_t Selection::total() const {
  uint64_t sum = 0;
  for (size_t i = 0; i < parts_.size(); ++i)
    if (selected_[i]) {
      if (UINT64_MAX - sum < parts_[i].size)
        return UINT64_MAX;
      sum += parts_[i].size;
    }
  return sum;
}
std::vector<Item> Selection::files() const {
  std::vector<Item> files;
  for (size_t i = 0; i < parts_.size(); ++i)
    if (selected_[i])
      files.push_back(parts_[i]);
  return files;
}
} // namespace foil
