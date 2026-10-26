#include "core/Version.hpp"
namespace foil {
const std::string &userAgent() {
  static const std::string agent = "CookingFoil/" COOKINGFOIL_VERSION;
  return agent;
}
const char *appVersion() { return COOKINGFOIL_VERSION; }
} // namespace foil
