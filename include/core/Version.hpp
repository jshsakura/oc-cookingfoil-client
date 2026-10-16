#pragma once
// Set by the build from APP_VERSION in makefiles/switch.mk.
#ifndef COOKINGFOIL_VERSION
#define COOKINGFOIL_VERSION "0.0.0-dev"
#endif
#include <string>
namespace foil {
// "CookingFoil/<version>", the User-Agent of every request this app makes
// (its own and the engine's): the server tells the client's version by it.
// Defined in core/Version.cpp, which a new version always rebuilds.
const std::string &userAgent();
} // namespace foil
