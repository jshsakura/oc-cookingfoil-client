#pragma once
// What 다운로드 remembers: each install's files and how it ended, newest
// first, kept on the SD card so a failed one can be tried again later.
#include "core/State.hpp"
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
};
std::vector<Entry> parse(const std::string &json);
std::string serialize(const std::vector<Entry> &entries);
// Puts `entry` first with a fresh id and drops the oldest finished ones
// past `cap` (queued and running ones always stay).
uint64_t add(std::vector<Entry> &entries, Entry entry, size_t cap = 50);
// The newest running entry of `kind` ends with `outcome`; false when none runs.
bool finish(std::vector<Entry> &entries, Outcome outcome, const std::string &error, int64_t at,
            const std::string &kind = "");
// An app that closed mid-download leaves a running entry: an install goes
// back in the queue, a retro download reads as failed.
void settle(std::vector<Entry> &entries);
} // namespace foil::history
