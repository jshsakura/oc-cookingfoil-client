#pragma once
// How one file is split across connections, aria2 style: one long part per
// connection, each read to its end; a connection that finishes takes the back
// half of the part with the most left. Pure bookkeeping, no network.
#include <cstdint>
#include <string>
#include <vector>
namespace foil::net {
// A part of the file: [start, end), received up to `next`.
struct Segment {
  uint64_t start = 0, end = 0, next = 0;
  uint64_t left() const { return end > next ? end - next : 0; }
};
// Below this a part is never split further; a part is split only when more
// than twice this is left (the two halves stay at least this big).
constexpr uint64_t MinSplit = 32ull << 20;
// Files smaller than this always come on one connection.
constexpr uint64_t ParallelFloor = 64ull << 20;
class SegmentPlan {
public:
  // `total` bytes from `from` (0 for a whole file) in `connections` parts of
  // near equal size, each a multiple of 4 KB but the last.
  SegmentPlan(uint64_t from, uint64_t total, int connections);
  // A plan saved earlier (resume); parts as they were.
  explicit SegmentPlan(std::vector<Segment> segments) : segments_(std::move(segments)) {}
  const std::vector<Segment> &segments() const { return segments_; }
  std::vector<Segment> &segments() { return segments_; }
  // Splits the part with the most left when it can: its back half becomes a
  // new part (the returned index), and the part now ends where that begins.
  // -1 when no part has more than 2 x MinSplit left. `busy` parts are being
  // read; an idle part with anything left is handed out whole first.
  int take(const std::vector<bool> &busy);
  bool done() const;
  uint64_t received() const; // bytes in place across all parts
  // "start-end-next;..." for the resume file, and back.
  std::string save() const;
  static bool load(const std::string &text, std::vector<Segment> &out);

private:
  std::vector<Segment> segments_;
};
// How many connections a download from `serverUrl` gets: `setting` 1/2/4/8,
// or 0 for automatic (2 for a private address, 4 otherwise), never more than
// the server recommends.
int connectionsFor(const std::string &serverUrl, int setting);
// The server's recommendation (/api/shop/info download.maxConnections); 0
// when it gives none.
void recommendConnections(int most);
bool privateHost(const std::string &url);
} // namespace foil::net
