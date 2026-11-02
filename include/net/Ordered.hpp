#pragma once
// A range of one file handed on strictly in order, for what must be read
// front to back as it arrives (an NCZ is decompressed as it comes). Large
// ranges come down like aria2's: consecutive pieces on several connections at
// once, each held until its turn; the rest on one connection.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
namespace foil::net {
struct OrderedOptions {
  // At most this many at once (the tuner may say fewer); 1 keeps one stream.
  int connections = 1;
  size_t chunk = 4u << 20; // bytes a piece
  // Pieces a connection may hold past a slow one at the front of the line.
  int ahead = 1;
  // Sent to the server's own address; the direct address gets its token alone.
  std::vector<std::string> headers;
  std::function<bool()> paused, cancelled;
  // Asked once the tries run out on one connection: true tries again.
  std::function<bool()> tryAgain;
};
// Takes bytes in order; fewer than given (or a throw) stops the download.
using OrderedSink = std::function<size_t(const uint8_t *data, size_t size)>;
// [from, from + length) of `url` to `sink`. True when all of it went.
bool orderedStream(const std::string &url, uint64_t from, uint64_t length, const OrderedSink &sink,
                   const OrderedOptions &options);
// Why the last range on this thread stopped ("HTTP 200", curl's words), or empty.
std::string orderedFailure();
// The same range read into `out` (its size is the length); false on failure.
bool orderedRead(const std::string &url, uint64_t from, void *out, size_t length, const OrderedOptions &options);
} // namespace foil::net
