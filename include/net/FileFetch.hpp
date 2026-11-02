#pragma once
#include "core/State.hpp"
#include "core/Types.hpp"
#include <atomic>
#include <cstdint>
#include <string>
// One file of an app's or of the retro pack's onto the card: the part file
// name, the folders, resuming, and the size and hash checks live here only,
// so the two never drift apart again.
namespace foil::net {
// Where sd/<path> waits while it downloads: a short ".cf-<hash>.part" in its
// folder, whatever characters the listed name has.
std::string filePart(const std::string &sd, const std::string &path);
// One file to sd/<path> through filePart, resumed when an earlier try left
// one. `sha256` (64 hex) is checked; empty, the download's ETag is taken as
// the checksum when it carries one. `size` 0 skips that check. `busy` gets
// a 503's wait, `status` the HTTP status (a 429 to wait out). `refused` is
// set when the whole file came but the card would not take it under its name.
bool fetchFile(const Server &server, const State &state, const std::string &url, const std::string &sd,
               const std::string &path, std::string sha256, uint64_t size, std::atomic<uint64_t> &received,
               Error &error, const std::atomic<bool> *cancel, int *busy = nullptr, long *status = nullptr,
               bool *refused = nullptr);
} // namespace foil::net
