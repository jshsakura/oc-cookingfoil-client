#include "core/DownloadHistory.hpp"
#include "core/Json.hpp"
#include <algorithm>
namespace foil::history {
namespace {
const char *name(Outcome outcome) {
  switch (outcome) {
  case Outcome::Done: return "done";
  case Outcome::Failed: return "failed";
  case Outcome::Cancelled: return "cancelled";
  case Outcome::Queued: return "queued";
  default: return "running";
  }
}
Outcome outcomeOf(const std::string &text) {
  return text == "done" ? Outcome::Done : text == "failed" ? Outcome::Failed
         : text == "cancelled"                             ? Outcome::Cancelled
         : text == "queued"                                ? Outcome::Queued
                                                           : Outcome::Running;
}
} // namespace
std::vector<Entry> parse(const std::string &json) {
  std::vector<Entry> out;
  Error error;
  auto root = parseJson(json, error);
  if (!root || !json_object_is_type(root.get(), json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(root.get()); ++i) {
    auto *object = json_object_array_get_idx(root.get(), i);
    auto install = parsePending(stringField(object, "install"));
    if (!install) continue;
    Entry e;
    e.id = uint64_t(std::max<int64_t>(integerField(object, "id"), 0));
    e.install = std::move(*install);
    e.outcome = outcomeOf(stringField(object, "outcome"));
    e.error = stringField(object, "error");
    e.at = integerField(object, "at");
    e.kind = stringField(object, "kind");
    e.payload = stringField(object, "payload");
    e.group = int(integerField(object, "group"));
    out.push_back(std::move(e));
  }
  return out;
}
std::string serialize(const std::vector<Entry> &entries) {
  Json list(json_object_new_array());
  for (const auto &e : entries) {
    auto *object = json_object_new_object();
    json_object_object_add(object, "id", json_object_new_int64(int64_t(e.id)));
    json_object_object_add(object, "install", json_object_new_string(serializePending(e.install).c_str()));
    json_object_object_add(object, "outcome", json_object_new_string(name(e.outcome)));
    json_object_object_add(object, "error", json_object_new_string(e.error.c_str()));
    json_object_object_add(object, "at", json_object_new_int64(e.at));
    if (e.group)
      json_object_object_add(object, "group", json_object_new_int(e.group));
    if (!e.kind.empty()) {
      json_object_object_add(object, "kind", json_object_new_string(e.kind.c_str()));
      json_object_object_add(object, "payload", json_object_new_string(e.payload.c_str()));
    }
    json_object_array_add(list.get(), object);
  }
  return json_object_to_json_string_ext(list.get(), JSON_C_TO_STRING_PLAIN);
}
uint64_t add(std::vector<Entry> &entries, Entry entry, size_t cap, uint64_t floor) {
  uint64_t top = floor;
  for (const auto &e : entries) top = std::max(top, e.id);
  entry.id = top + 1;
  entries.insert(entries.begin(), std::move(entry));
  size_t finished = 0;
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const Entry &e) {
                                 const bool ended = e.outcome != Outcome::Running && e.outcome != Outcome::Queued;
                                 return ended && ++finished > cap;
                               }),
                entries.end());
  return top + 1;
}
namespace {
bool sameDownload(const Entry &a, const Entry &b) {
  if (a.kind != b.kind)
    return false;
  if (!a.kind.empty())
    return a.payload == b.payload;
  if (a.install.files.size() != b.install.files.size())
    return false;
  for (size_t i = 0; i < a.install.files.size(); ++i)
    if (a.install.files[i].url != b.install.files[i].url)
      return false;
  return true;
}
} // namespace
bool end(std::vector<Entry> &entries, uint64_t id, Outcome outcome, const std::string &error, int64_t at) {
  auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry &e) { return e.id == id; });
  if (found == entries.end())
    return false;
  Entry ended = std::move(*found);
  entries.erase(found);
  ended.outcome = outcome;
  ended.error = error;
  ended.at = at;
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const Entry &e) {
                                 return e.outcome != Outcome::Running && e.outcome != Outcome::Queued &&
                                        sameDownload(e, ended);
                               }),
                entries.end());
  entries.insert(entries.begin(), std::move(ended));
  return true;
}
bool finish(std::vector<Entry> &entries, Outcome outcome, const std::string &error, int64_t at,
            const std::string &kind) {
  for (const auto &e : entries)
    if (e.outcome == Outcome::Running && e.kind == kind)
      return end(entries, e.id, outcome, error, at);
  return false;
}
void settle(std::vector<Entry> &entries) {
  for (auto &e : entries)
    if (e.outcome == Outcome::Running) e.outcome = Outcome::Queued;
}
std::vector<uint64_t> queueOrder(const std::vector<Entry> &entries) {
  std::vector<uint64_t> out;
  for (const auto &e : entries)
    if (e.outcome == Outcome::Queued) out.push_back(e.id);
  std::sort(out.begin(), out.end());
  return out;
}
bool swapTurns(std::vector<Entry> &entries, uint64_t a, uint64_t b) {
  const auto find = [&](uint64_t id) {
    return std::find_if(entries.begin(), entries.end(),
                        [&](const Entry &e) { return e.id == id && e.outcome == Outcome::Queued; });
  };
  auto rowA = find(a), rowB = find(b);
  if (a == b || rowA == entries.end() || rowB == entries.end())
    return false;
  // Each keeps the other's turn and the other's place in the list.
  std::swap(rowA->id, rowB->id);
  std::iter_swap(rowA, rowB);
  return true;
}
uint64_t takeFirstTurn(std::vector<Entry> &entries, uint64_t id) {
  const auto order = queueOrder(entries);
  const auto at = std::find(order.begin(), order.end(), id);
  if (at == order.end())
    return 0;
  // One turn sooner at a time (it holds order[i] before each step): the
  // ones it passes keep their order.
  for (auto i = size_t(at - order.begin()); i > 0; --i)
    swapTurns(entries, order[i - 1], order[i]);
  return order.front();
}
bool pending(const std::vector<Entry> &entries, const Entry &job) {
  return std::any_of(entries.begin(), entries.end(), [&](const Entry &e) {
    return (e.outcome == Outcome::Queued || e.outcome == Outcome::Running) && sameDownload(e, job);
  });
}
std::string catalogId(const Entry &entry) {
  if (!entry.kind.empty() || entry.install.files.empty())
    return "";
  const auto &file = entry.install.files.front();
  return !file.baseId.empty() ? file.baseId : !file.titleId.empty() ? file.titleId : file.url;
}
} // namespace foil::history
