#include "mtp/Mtp.hpp"
#include <algorithm>
#include <strings.h>
#include <unordered_map>
namespace foil::mtp {
void Objects::clear() {
  // swap, not clear(): the memory itself goes back.
  std::vector<Object>().swap(table_);
  table_.emplace_back();
  table_[Root].folder = true;
}
const Objects::Object *Objects::find(uint32_t handle) const {
  if (handle == Root || handle >= table_.size() || table_[handle].name.empty()) return nullptr;
  return &table_[handle];
}
std::string Objects::path(uint32_t handle) const {
  std::vector<const std::string *> names;
  for (uint32_t at = handle; at != Root && at < table_.size(); at = table_[at].parent) names.push_back(&table_[at].name);
  std::string out;
  for (auto it = names.rbegin(); it != names.rend(); ++it) out += "/" + **it;
  return out.empty() ? "/" : out;
}
const std::vector<uint32_t> &Objects::sync(uint32_t folder, std::vector<Entry> listing) {
  std::unordered_map<std::string, uint32_t> had;
  for (const auto h : table_[folder].children) had.emplace(table_[h].name, h);
  std::vector<uint32_t> kept;
  kept.reserve(listing.size());
  for (auto &entry : listing) {
    const auto at = had.find(entry.name);
    if (at != had.end() && table_[at->second].folder == entry.folder) {
      table_[at->second].size = entry.size;
      kept.push_back(at->second);
      had.erase(at);
      continue;
    }
    const auto handle = uint32_t(table_.size());
    auto &object = table_.emplace_back();
    object.name = std::move(entry.name);
    object.parent = folder;
    object.size = entry.size;
    object.folder = entry.folder;
    kept.push_back(handle);
  }
  table_[folder].children.clear();
  for (const auto &[name, h] : had) remove(h);
  table_[folder].children = std::move(kept);
  table_[folder].listed = true;
  return table_[folder].children;
}
uint32_t Objects::add(uint32_t folder, Entry entry) {
  const auto handle = uint32_t(table_.size());
  auto &object = table_.emplace_back();
  object.name = std::move(entry.name);
  object.parent = folder;
  object.size = entry.size;
  object.folder = entry.folder;
  table_[folder].children.push_back(handle);
  return handle;
}
void Objects::unlink(uint32_t handle) {
  auto &siblings = table_[table_[handle].parent].children;
  siblings.erase(std::remove(siblings.begin(), siblings.end(), handle), siblings.end());
}
void Objects::remove(uint32_t handle) {
  if (!find(handle)) return;
  unlink(handle);
  // Handles are never given out again in a session, so what goes keeps only
  // its slot.
  std::vector<uint32_t> gone{handle};
  while (!gone.empty()) {
    const auto h = gone.back();
    gone.pop_back();
    auto &object = table_[h];
    gone.insert(gone.end(), object.children.begin(), object.children.end());
    std::string().swap(object.name);
    std::vector<uint32_t>().swap(object.children);
  }
}
void Objects::rename(uint32_t handle, std::string name) {
  if (find(handle)) table_[handle].name = std::move(name);
}
void Objects::resize(uint32_t handle, uint64_t size) {
  if (find(handle)) table_[handle].size = size;
}
uint32_t Objects::child(uint32_t folder, const std::string &name) const {
  for (const auto h : table_[folder].children)
    if (!strcasecmp(table_[h].name.c_str(), name.c_str())) return h;
  return 0;
}
} // namespace foil::mtp
