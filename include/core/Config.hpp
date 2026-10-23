#pragma once
#include "core/Types.hpp"
namespace foil {
std::string normalizeUrl(const std::string& input);
std::string originOf(const std::string& url);
// The host and port, what parseConfig names a server without a title.
std::string hostOf(const std::string& url);
// An http:// address off the home network: what the console sends there (the
// password, keys) crosses the internet unencrypted. A LAN, loopback or
// Tailscale address, a .local or one-word host name is not.
bool plainHttpOverInternet(const std::string& url);
Parsed<Config> parseConfig(const std::string& json);
Parsed<Config> loadConfig(const std::string& path);
std::string serializeConfig(const Config& config, bool redact = false);
// Writes config.json in the form parseConfig reads, replacing it whole.
bool saveConfig(const std::string& path, const Config& config, std::string& error);
// The enroll token of the server at `serverUrl` taken out of the config.json
// at `path`, once it was spent. True when nothing needed it.
bool dropEnrollToken(const std::string& path, const std::string& serverUrl, std::string& error);
// The server's password out of the file (the username stays): once the
// console holds a key from a server that takes passwords for approval only.
bool dropPassword(const std::string& path, const std::string& serverUrl, std::string& error);
}
