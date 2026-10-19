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
// How far the check of the files already on the card has come, in bytes
// (total 0 once it is done), for the screen. Bytes, not files: one big file
// takes as long as thousands of small ones.
struct CheckProgress {
  std::atomic<uint64_t> done{0}, total{0};
};
// A folder install's files the list left out (App::filesCount), read from
// App::filesUrl into app.files; true at once when it has them already.
bool homebrewFilesOf(const Server &server, const State &state, homebrew::App &app, Error &error,
                     const std::atomic<bool> *cancel);
bool homebrewDownload(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel,
                      std::atomic<uint64_t> *kept = nullptr, CheckProgress *check = nullptr);
} // namespace foil::net
