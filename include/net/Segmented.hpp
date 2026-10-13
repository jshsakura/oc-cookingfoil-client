#pragma once
// One file on several connections, aria2 style (net/Segments.hpp), its bytes
// written where they belong as they arrive. Games' plain NCAs, retro ROMs and
// homebrew all come down through here.
#include "net/Segments.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
typedef void CURL;
namespace foil::net {
struct SegmentedOptions {
  int connections = 1;
  // A part that keeps failing goes on alone on one connection; that pass
  // itself (false) fails instead of falling back again.
  bool fallBack = true;
  // Only [from, from + length) of the file (an NCA inside an NSP); length 0
  // is the whole file. With a length the size is known up front: every
  // request names its end and no first probe is needed.
  uint64_t from = 0, length = 0;
  // Bytes gathered on a connection before they go to the writer: large
  // writes suit the card (FAT32 slows down on small ones).
  size_t batchBytes = 4u << 20;
  std::vector<std::string> headers;      // sent with every request (auth, device keys)
  std::function<void(CURL *)> configure; // more per-handle options (the engine's basic auth)
  // Bytes held for the writer thread at most; the network waits beyond it.
  uint64_t bufferBytes = 32ull << 20;
  std::function<bool()> paused, cancelled;
  std::function<void(uint64_t received, uint64_t total)> progress;
  // Resume: the validator and parts saved earlier ({} / "" for a fresh start).
  std::string etag;
  std::vector<Segment> resume;
  uint64_t resumeTotal = 0;
  // Hears the parts written so far, now and then, to save for a resume.
  std::function<void(const std::string &etag, uint64_t total, const std::string &plan)> checkpoint;
  std::function<void(const std::string &)> log;
};
struct SegmentedResult {
  bool ok = false;
  long status = 0;    // the last HTTP status that ended it
  std::string error;  // why it stopped ("" when ok or cancelled)
  bool cancelled = false;
  std::string etag;
  uint64_t total = 0;
};
// Fetches `url` whole. `prepare(total)` runs once the size is known (and
// again with 0 written when the server's file changed and it starts over);
// `write(offset, data, size)` puts bytes in place, from a writer thread, in
// any order. Either returning false stops the download.
SegmentedResult segmentedFetch(const std::string &url, const std::function<bool(uint64_t total)> &prepare,
                               const std::function<bool(uint64_t offset, const char *data, size_t size)> &write,
                               const SegmentedOptions &options);
} // namespace foil::net
