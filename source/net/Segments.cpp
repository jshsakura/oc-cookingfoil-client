#include "net/Segments.hpp"
#include <algorithm>
#include <cstdio>
#include <sstream>
namespace foil::net {
namespace {
constexpr uint64_t Align = 4096;
uint64_t alignDown(uint64_t value) { return value / Align * Align; }
} // namespace
SegmentPlan::SegmentPlan(uint64_t from, uint64_t total, int connections) {
  const uint64_t size = total > from ? total - from : 0;
  const int parts = size < ParallelFloor ? 1 : std::max(1, connections);
  uint64_t at = from;
  for (int n = 0; n < parts; ++n) {
    const uint64_t end = n + 1 == parts ? total : std::max(at, alignDown(from + size * (n + 1) / parts));
    if (end > at)
      segments_.push_back({at, end, at});
    at = end;
  }
  if (segments_.empty())
    segments_.push_back({from, total, from});
}
int SegmentPlan::take(const std::vector<bool> &busy) {
  // A part nobody reads (its connection gave up, or a resumed plan) first.
  for (size_t i = 0; i < segments_.size(); ++i)
    if ((i >= busy.size() || !busy[i]) && segments_[i].left() > 0)
      return int(i);
  size_t most = segments_.size();
  for (size_t i = 0; i < segments_.size(); ++i)
    if (most == segments_.size() || segments_[i].left() > segments_[most].left())
      most = i;
  if (most == segments_.size() || segments_[most].left() <= 2 * MinSplit)
    return -1;
  auto &part = segments_[most];
  const uint64_t middle = alignDown(part.next + part.left() / 2);
  segments_.push_back({middle, part.end, middle});
  segments_[most].end = middle; // reference may move with push_back: index again
  return int(segments_.size() - 1);
}
bool SegmentPlan::done() const {
  return std::all_of(segments_.begin(), segments_.end(), [](const Segment &s) { return s.left() == 0; });
}
uint64_t SegmentPlan::received() const {
  uint64_t sum = 0;
  for (const auto &s : segments_)
    sum += std::min(s.next, s.end) - s.start;
  return sum;
}
std::string SegmentPlan::save() const {
  std::string out;
  for (const auto &s : segments_)
    out += std::to_string(s.start) + "-" + std::to_string(s.end) + "-" + std::to_string(s.next) + ";";
  return out;
}
bool SegmentPlan::load(const std::string &text, std::vector<Segment> &out) {
  out.clear();
  std::istringstream in(text);
  std::string item;
  while (std::getline(in, item, ';')) {
    if (item.empty())
      continue;
    unsigned long long a = 0, b = 0, c = 0;
    if (std::sscanf(item.c_str(), "%llu-%llu-%llu", &a, &b, &c) != 3 || a > b || c < a || c > b)
      return false;
    out.push_back({a, b, c});
  }
  return !out.empty();
}
bool privateHost(const std::string &url) {
  auto host = url.substr(url.find("://") == std::string::npos ? 0 : url.find("://") + 3);
  host = host.substr(0, host.find_first_of("/:?#"));
  unsigned a = 0, b = 0, c = 0, d = 0;
  char extra = 0;
  if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 ||
      d > 255)
    return false; // a name, or not IPv4: outside
  return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
         (a == 100 && b >= 64 && b <= 127);
}
int connectionsFor(const std::string &serverUrl, int setting) {
  if (setting == 1 || setting == 2 || setting == 4 || setting == 8)
    return setting;
  return privateHost(serverUrl) ? 2 : 8;
}
} // namespace foil::net
