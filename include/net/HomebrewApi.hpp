#pragma once
#include "core/Homebrew.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
// GET /api/homebrew. With `etag` from the last reply a 304 sets `unchanged`
// and leaves `apps` alone; `etag` takes the new one. `titles` (feature
// homebrew-titles) asks for the apps installed as titles too, which the
// server leaves out otherwise.
bool homebrewList(const Server &server, const State &state, std::string &etag, std::vector<homebrew::App> &apps,
                  bool &unchanged, Error &error, const std::atomic<bool> *cancel, bool titles = false);
// Saves the app at sd/<app.installPath> (a folder install: each of its files
// at sd/<path>), each through a .part file that replaces the old one only
// once complete and matching its checksum.
// `received` counts files already on the card as they are passed, for the
// progress; `kept`, when given, counts those alone, so a speed can leave
// them out (they came from the card, not the network).
bool homebrewDownload(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel,
                      std::atomic<uint64_t> *kept = nullptr);
} // namespace foil::net
