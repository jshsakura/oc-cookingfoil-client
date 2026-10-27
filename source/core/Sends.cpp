#include "core/Sends.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace foil::sends {
Feed parse(const std::string &json) {
  Feed feed;
  Error ignored;
  const auto root = parseJson(json, ignored);
  if (!root) return feed;
  feed.serverId = stringField(root.get(), "serverId");
  auto *items = field(root.get(), "items");
  if (!json_object_is_type(items, json_type_array)) return feed;
  const size_t count = std::min<size_t>(json_object_array_length(items), MaxItems);
  for (size_t i = 0; i < count; ++i) {
    auto *o = json_object_array_get_idx(items, i);
    if (!json_object_is_type(o, json_type_object)) continue;
    Sent s;
    s.id = stringField(o, "id");
    s.url = urlField(o, "url", ""); // absolute only: the console's own address
    if (s.id.empty() || s.url.empty()) continue;
    s.path = stringField(o, "path");
    s.name = stringField(o, "name");
    s.titleId = stringField(o, "titleId");
    s.sentAt = stringField(o, "sentAt");
    s.sentBy = stringField(o, "sentBy");
    s.size = uint64_t(std::max<int64_t>(integerField(o, "size"), 0));
    feed.items.push_back(std::move(s));
  }
  return feed;
}
std::vector<Pick> merge(const std::vector<Source> &sources) {
  std::vector<Pick> picks;
  std::map<std::string, size_t> byGame;
  std::set<std::string> serverIds;
  for (const auto &source : sources) {
    // Two entries for one server: its items are the same items.
    if (!source.feed.serverId.empty() && !serverIds.insert(source.feed.serverId).second) continue;
    for (const auto &item : source.feed.items) {
      const auto key = (item.titleId.empty() ? item.path : item.titleId) + "|" + std::to_string(item.size);
      const auto [at, fresh] = byGame.emplace(key, picks.size());
      if (fresh) {
        picks.push_back({source.server, item, {}});
      } else if (item.sentAt < picks[at->second].item.sentAt) {
        // The earliest send sets the order.
        picks[at->second].item.sentAt = item.sentAt;
      }
      picks[at->second].acks.push_back({source.server, item.id});
    }
  }
  std::stable_sort(picks.begin(), picks.end(), [](const Pick &a, const Pick &b) { return a.item.sentAt < b.item.sentAt; });
  return picks;
}
Admission admit(const std::vector<Candidate> &candidates, const Room &room) {
  Admission a;
  if (!room.free) {
    a.waiting = !candidates.empty();
    return a;
  }
  const uint64_t free = *room.free;
  uint64_t used = room.queued;
  size_t count = room.queueCount;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const auto &c = candidates[i];
    const bool fits = c.runs && count < MaxQueue && used <= free && c.need <= free - used &&
                      free - used - c.need >= Reserve;
    if (!fits) {
      a.held.push_back(i);
      continue;
    }
    used += c.need;
    ++count;
    a.take.push_back(i);
  }
  return a;
}
std::string ackBody(const std::vector<std::string> &ids, bool accepted) {
  Json root(json_object_new_object());
  auto *list = json_object_new_array();
  for (const auto &id : ids)
    json_object_array_add(list, json_object_new_string(id.c_str()));
  json_object_object_add(root.get(), "ids", list);
  json_object_object_add(root.get(), "result", json_object_new_string(accepted ? "accepted" : "declined"));
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
} // namespace foil::sends
