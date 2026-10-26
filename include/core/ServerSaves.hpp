#pragma once
#include "core/Votes.hpp"
// Save files the server offers (/api/saves, feature library-saves), such as an
// all-unlocked save. One entry is a whole save folder sent as a tar.
#include "core/Types.hpp"
namespace foil::serversaves {
struct ServerSave {
  std::string id, titleId, name, description, gameVersion, hash, url;
  uint64_t size = 0;
  int files = 0;
  int64_t updatedAt = 0;
  bool verified = false; // an admin checked it
  votes::Votes votes;
  std::string gameName, gameIcon;
  LanguageTexts gameNames;
  // An app's save has no Title ID: its files go as files into installRoot
  // (switch/<app>) on the SD card, not into system save data.
  std::string homebrewApp, installRoot;
  // "device" for a shared save every account plays on, "account" for one
  // account's; empty when the server does not say, and then either fits.
  std::string saveType;
  std::string server; // the configured server it came from (its url)
};
// Several servers' save lists as one: each names its server, and the same
// save (hash) on several is listed once, from the first. `lists` in the
// servers' order, as {server url, its saves}.
std::vector<ServerSave> mergeServers(std::vector<std::pair<std::string, std::vector<ServerSave>>> lists);
// `base` resolves "/api/..." URLs; entries without an id, Title ID (or an
// app with its switch/<app> root) or URL are dropped.
std::vector<ServerSave> parse(const std::string &json, const std::string &base);
// Inside a save folder: relative, no "..", no drive or device names.
bool pathAllowed(const std::string &path);
// A path of an app's save, relative to its installRoot, that stays below
// switch/<app>/.
bool homebrewPathAllowed(const ServerSave &save, const std::string &path);
// Moves every file under `from` to the same place under `to`, a file
// already there moved to the same place under `keep` first; `from` goes.
bool placeFiles(const std::string &from, const std::string &to, const std::string &keep, std::string &error);
// Why `save` may not go into the chosen save, the shared one when
// `deviceTarget`; nothing when it fits.
Error targetMismatch(const ServerSave &save, bool deviceTarget);
// The name shown for the save's game in the interface language.
std::string gameName(const ServerSave &save, const std::string &languageCode);
} // namespace foil::serversaves
