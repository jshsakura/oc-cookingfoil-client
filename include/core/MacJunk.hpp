#pragma once
// What a Mac leaves on a card it writes to: AppleDouble "._" twins beside
// each file, .DS_Store in every folder it opened, and at the top its index,
// trash and event folders. None of them means anything to the console, and
// "._" files inside atmosphere/ can stop a mod or a sysmodule from loading.
#include <cstdint>
#include <string>
#include <vector>
namespace foil {
// Whether `name` (a file or folder name, no path) is one of them. The folders
// count only at the card's top, where macOS makes them.
bool macJunk(const std::string &name, bool directory, bool atTop);
struct JunkSweep {
  int removed = 0;
  uint64_t bytes = 0;
  int failed = 0;
};
// Deletes them from the card's top and from each of `folders` (paths under
// `sd`, not walked into): the folders a PC wrote to, so the sweep takes as
// long as what was copied, not the whole card.
JunkSweep sweepMacJunk(const std::string &sd, const std::vector<std::string> &folders);
} // namespace foil
