#pragma once
#include "core/Version.hpp"
#include <string>
namespace inst::curl {
// The app's own User-Agent, with its real version (core/Version.hpp).
inline const std::string &getUserAgent() { return foil::userAgent(); }
} // namespace inst::curl
