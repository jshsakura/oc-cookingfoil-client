#pragma once
#include "core/Homebrew.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
// GET /api/homebrew. With `etag` from the last reply a 304 sets `unchanged`
// and leaves `apps` alone; `etag` takes the new one.
bool homebrewList(const Server &server, const State &state, std::string &etag, std::vector<homebrew::App> &apps,
                  bool &unchanged, Error &error, const std::atomic<bool> *cancel);
// Saves the app at sd/<homebrew::sdPath>, through a .part file that replaces
// it only once complete and matching the download's ETag.
bool homebrewDownload(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel);
} // namespace foil::net
