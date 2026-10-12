#pragma once
// Save files the shop offers (/api/saves, feature shop-saves), such as an
// all-unlocked save. One entry is a whole save folder sent as a tar.
#include "core/Types.hpp"
namespace foil::shopsaves {
struct ShopSave {
  std::string id, titleId, name, description, gameVersion, hash, url;
  uint64_t size = 0;
  int files = 0;
  int64_t updatedAt = 0;
  std::string gameName, gameIcon;
  std::vector<std::string> gameNames; // ko, en, ja, zh; any may be empty
};
// `base` resolves "/api/..." URLs; entries without an id, Title ID or URL
// are dropped.
std::vector<ShopSave> parse(const std::string &json, const std::string &base);
// Inside a save folder: relative, no "..", no drive or device names.
bool pathAllowed(const std::string &path);
// The name shown for the save's game in the interface language.
std::string gameName(const ShopSave &save, const std::string &languageCode);
} // namespace foil::shopsaves
