#pragma once
#include "core/Catalog.hpp"
namespace foil {
class Selection {
public:
  explicit Selection(const Group &group, std::string target = "sd");
  bool toggle(size_t part);
  bool enabled(size_t part) const;
  uint64_t total() const;
  std::vector<Item> files() const;
  const std::vector<Item> &parts() const { return parts_; }
  std::string target;

private:
  std::vector<Item> parts_;
  std::vector<bool> selected_;
};
} // namespace foil
