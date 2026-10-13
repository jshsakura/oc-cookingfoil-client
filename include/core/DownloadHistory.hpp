#pragma once
// What 다운로드 remembers: each install's files and how it ended, newest
// first, kept on the SD card so a failed one can be tried again later.
#include "core/State.hpp"
#include <cstdint>
namespace foil::history {
// Queued: waiting its turn in 다운로드, kept across launches.
enum class Outcome { Running, Done, Failed, Cancelled, Queued };
struct Entry {
  uint64_t id = 0;
  PendingInstall install; // name, icon, target and the files, as the resume record keeps them
  Outcome outcome = Outcome::Running;
  std::string error;
  int64_t at = 0; // when it started, then when it ended (seconds)
  // "" an install; "retro" a retro game (payload: the game, retro::serializeGame);
  // "retroarch" RetroArch's files.
  std::string kind, payload;
  int group = 0; // one pick of files (base game, update, DLC) shares it
  // What a finished install took: bytes received, when it started, the
  // download and the install after it (seconds).
  uint64_t bytes = 0;
  int64_t startedAt = 0;
  double downloadSeconds = 0, installSeconds = 0;
};
std::vector<Entry> parse(const std::string &json);
std::string serialize(const std::vector<Entry> &entries);
// Puts `entry` first with a fresh id. With a `cap`, the oldest finished
// ones past it go (queued and running ones always stay); by default the
// list keeps everything, one row a file (see end()).
uint64_t add(std::vector<Entry> &entries, Entry entry, size_t cap = SIZE_MAX);
// The row `id` ends with `outcome` and moves to the top; older finished rows
// of the same download go, so a file never shows twice. False when no such row.
bool end(std::vector<Entry> &entries, uint64_t id, Outcome outcome, const std::string &error, int64_t at);
// The newest running entry of `kind` ends with `outcome`; false when none runs.
bool finish(std::vector<Entry> &entries, Outcome outcome, const std::string &error, int64_t at,
            const std::string &kind = "");
// An app that closed mid-download leaves a running entry: an install goes
// back in the queue, a retro download reads as failed.
void settle(std::vector<Entry> &entries);
} // namespace foil::history
