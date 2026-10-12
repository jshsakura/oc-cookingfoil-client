#pragma once
#include "core/Types.hpp"
namespace foil {
std::string normalizeUrl(const std::string& input);
std::string originOf(const std::string& url);
Parsed<Config> parseConfig(const std::string& json);
Parsed<Config> loadConfig(const std::string& path);
std::string serializeConfig(const Config& config, bool redact = false);
}
