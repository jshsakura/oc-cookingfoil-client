#pragma once
#include <string>
namespace inst::curl {
inline const std::string &getUserAgent() {
  static const std::string userAgent = "CookingFoil/0.1.0";
  return userAgent;
}
} // namespace inst::curl
