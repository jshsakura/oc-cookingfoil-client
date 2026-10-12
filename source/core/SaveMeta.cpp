// Ported from oc-save-keeper (MIT), include/core/MetadataLogic.hpp and
// include/core/SyncLogic.hpp. Copyright (c) jshsakura.
#include "core/SaveMeta.hpp"
#include "core/Json.hpp"
#include <cstdlib>
#include <fstream>
#include <iterator>
namespace foil::saves {
namespace {
// Metadata is a few hundred bytes; anything this large is not ours.
constexpr std::streamoff MaxMetaBytes = 64 * 1024;
bool flag(const std::string &value) { return value == "1" || value == "true"; }
void assign(BackupMeta &meta, const std::string &key, const std::string &raw) {
  if (key == "title_id") {
    meta.titleId = std::strtoull(raw.c_str(), nullptr, 10);
    return;
  }
  if (key == "created_at") {
    meta.createdAt = static_cast<std::time_t>(std::atoll(raw.c_str()));
    return;
  }
  if (key == "device_priority") {
    meta.devicePriority = std::atoi(raw.c_str());
    return;
  }
  if (key == "size") {
    meta.size = std::atoll(raw.c_str());
    return;
  }
  if (key == "is_auto_backup" || key == "isAutoBackup") {
    meta.isAutoBackup = flag(raw);
    return;
  }
  if (key == "is_favorite" || key == "isFavorite") {
    meta.isFavorite = flag(raw);
    return;
  }
  std::string BackupMeta::*text = nullptr;
  if (key == "title_name") text = &BackupMeta::titleName;
  else if (key == "backup_name") text = &BackupMeta::backupName;
  else if (key == "revision_id") text = &BackupMeta::revisionId;
  else if (key == "device_id") text = &BackupMeta::deviceId;
  else if (key == "device_label") text = &BackupMeta::deviceLabel;
  else if (key == "user_id") text = &BackupMeta::userId;
  else if (key == "user_name") text = &BackupMeta::userName;
  else if (key == "source") text = &BackupMeta::source;
  if (text)
    meta.*text = unescapeMetaValue(raw);
}
} // namespace

std::string escapeMetaValue(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\\') {
      const char next = i + 1 < value.size() ? value[i + 1] : '\0';
      out += (next == 'n' || next == 'r' || next == '\\') ? "\\\\" : "\\";
    } else {
      out += c;
    }
  }
  return out;
}

std::string unescapeMetaValue(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    const char next = i + 1 < value.size() ? value[i + 1] : '\0';
    if (c == '\\' && (next == 'n' || next == 'r' || next == '\\')) {
      out += next == 'n' ? '\n' : next == 'r' ? '\r' : '\\';
      ++i;
    } else {
      out += c;
    }
  }
  return out;
}

std::string serializeMeta(const BackupMeta &meta) {
  std::string text;
  text.reserve(512);
  auto line = [&](const char *key, const std::string &value) {
    text += key;
    text += '=';
    text += value;
    text += '\n';
  };
  line("title_id", std::to_string(static_cast<unsigned long long>(meta.titleId)));
  line("title_name", escapeMetaValue(meta.titleName));
  line("backup_name", escapeMetaValue(meta.backupName));
  line("revision_id", escapeMetaValue(meta.revisionId));
  line("device_id", escapeMetaValue(meta.deviceId));
  line("device_label", escapeMetaValue(meta.deviceLabel));
  line("user_id", escapeMetaValue(meta.userId));
  line("user_name", escapeMetaValue(meta.userName));
  line("source", escapeMetaValue(meta.source));
  line("created_at", std::to_string(static_cast<long long>(meta.createdAt)));
  line("device_priority", std::to_string(meta.devicePriority));
  line("size", std::to_string(static_cast<long long>(meta.size)));
  line("is_auto_backup", meta.isAutoBackup ? "1" : "0");
  line("is_favorite", meta.isFavorite ? "1" : "0");
  return text;
}

Parsed<BackupMeta> parseMeta(const std::string &text) {
  Parsed<BackupMeta> result;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find('\n', start);
    std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t equals = line.find('=');
    if (equals != std::string::npos)
      assign(result.value, line.substr(0, equals), line.substr(equals + 1));
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  if (result.value.titleId == 0 && result.value.backupName.empty())
    result.error.message = "metadata has neither title_id nor backup_name";
  return result;
}

Parsed<BackupMeta> readMetaFile(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    Parsed<BackupMeta> missing;
    missing.error.message = "cannot open " + path;
    return missing;
  }
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0 || length > MaxMetaBytes) {
    Parsed<BackupMeta> large;
    large.error.message = "metadata too large: " + path;
    return large;
  }
  input.seekg(0);
  return parseMeta(std::string(std::istreambuf_iterator<char>(input), {}));
}

Error writeMetaFile(const std::string &path, const BackupMeta &meta) {
  Error error;
  if (!atomicWrite(path, serializeMeta(meta), error.message) && error.message.empty())
    error.message = "cannot write " + path;
  return error;
}

Error copyMetaFile(const std::string &source, const std::string &destination) {
  std::ifstream input(source, std::ios::binary);
  if (!input)
    return {"cannot open " + source};
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0 || length > MaxMetaBytes)
    return {"metadata too large: " + source};
  input.seekg(0);
  const std::string text(std::istreambuf_iterator<char>(input), {});
  Error error;
  if (!atomicWrite(destination, text, error.message) && error.message.empty())
    error.message = "cannot write " + destination;
  return error;
}

SyncDecision decideSync(SyncPolicy policy, const BackupMeta *local, const BackupMeta &incoming) {
  if (!local)
    return {true, "No local backup metadata found"};
  if (!incoming.userId.empty() && !local->userId.empty() && incoming.userId != local->userId)
    return {false, "Different user save detected; manual restore only"};
  const bool byPriority = policy == SyncPolicy::PreferPriority;
  if (byPriority && !incoming.deviceId.empty() && !local->deviceId.empty() &&
      incoming.deviceId != local->deviceId) {
    if (incoming.devicePriority > local->devicePriority)
      return {true, "Different device save accepted by higher priority"};
    if (incoming.devicePriority < local->devicePriority)
      return {false, "Different device save kept out by local priority"};
  }
  if (byPriority) {
    if (incoming.devicePriority > local->devicePriority)
      return {true, "Incoming backup has higher device priority"};
    if (incoming.devicePriority < local->devicePriority)
      return {false, "Local backup has higher device priority"};
  }
  if (incoming.createdAt > local->createdAt)
    return {true, "Incoming backup is newer"};
  if (incoming.createdAt < local->createdAt)
    return {false, "Local backup is newer"};
  if (policy == SyncPolicy::PreferNewest)
    return {false, "Timestamps match; keeping local backup"};
  return {false, "Priority and timestamp are tied; keeping local backup"};
}
} // namespace foil::saves
