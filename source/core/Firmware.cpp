#include "core/Firmware.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Zip.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
namespace foil::firmware {
namespace {
// The three numbers of a version; false past three digits each.
bool numbers(const std::string &text, int (&out)[3]) {
  int part = 0, digits = 0;
  out[0] = out[1] = out[2] = 0;
  for (const char c : text) {
    if (c == '.') {
      if (!digits || ++part > 2) return false;
      digits = 0;
    } else if (std::isdigit(static_cast<unsigned char>(c)) && digits < 3) {
      out[part] = out[part] * 10 + (c - '0');
      ++digits;
    } else
      return false;
  }
  return part == 2 && digits;
}
bool endsWith(const std::string &text, const std::string &end) {
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}
json_object *versionJson(const Version &v) {
  auto *o = json_object_new_object();
  json_object_object_add(o, "version", json_object_new_string(v.version.c_str()));
  json_object_object_add(o, "released", json_object_new_string(v.released.c_str()));
  json_object_object_add(o, "available", json_object_new_boolean(v.available));
  json_object_object_add(o, "reason", json_object_new_string(v.reason.c_str()));
  auto *zip = json_object_new_object();
  json_object_object_add(zip, "url", json_object_new_string(v.archive.url.c_str()));
  json_object_object_add(zip, "size", json_object_new_int64(int64_t(v.archive.size)));
  json_object_object_add(zip, "sha256", json_object_new_string(v.archive.sha256.c_str()));
  json_object_object_add(zip, "files", json_object_new_int64(int64_t(v.archive.files)));
  json_object_object_add(zip, "unpacked", json_object_new_int64(int64_t(v.archive.unpacked)));
  json_object_object_add(o, "archive", zip);
  return o;
}
bool readVersion(json_object *o, const std::string &base, Version &v) {
  if (!json_object_is_type(o, json_type_object)) return false;
  v.version = stringField(o, "version");
  if (!isVersion(v.version)) return false;
  v.released = stringField(o, "released");
  auto *available = field(o, "available");
  v.available = json_object_is_type(available, json_type_boolean) && json_object_get_boolean(available);
  v.reason = stringField(o, "reason");
  auto *zip = field(o, "archive");
  if (json_object_is_type(zip, json_type_object)) {
    v.archive.url = urlField(zip, "url", base);
    v.archive.sha256 = stringField(zip, "sha256");
    v.archive.size = uint64_t(std::max<int64_t>(0, integerField(zip, "size")));
    v.archive.files = uint64_t(std::max<int64_t>(0, integerField(zip, "files")));
    v.archive.unpacked = uint64_t(std::max<int64_t>(0, integerField(zip, "unpacked")));
  }
  // Nothing to fetch is nothing to offer, whatever the flag says.
  if (!v.archive.usable()) v.available = false;
  return true;
}
} // namespace
bool isVersion(const std::string &text) {
  int parts[3];
  return numbers(text, parts);
}
int compare(const std::string &a, const std::string &b) {
  int x[3], y[3];
  const bool okA = numbers(a, x), okB = numbers(b, y);
  if (!okA || !okB) return int(okA) - int(okB);
  for (int i = 0; i < 3; ++i)
    if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}
bool parse(const std::string &body, const std::string &serverUrl, List &out) {
  Error error;
  auto root = parseJson(body, error);
  auto *versions = root ? field(root.get(), "versions") : nullptr;
  if (!json_object_is_type(versions, json_type_array)) return false;
  List read;
  if (auto *source = field(root.get(), "source"); json_object_is_type(source, json_type_object)) {
    auto *reachable = field(source, "reachable");
    read.reachable = !json_object_is_type(reachable, json_type_boolean) || json_object_get_boolean(reachable);
    read.checkedAt = stringField(source, "checked_at");
  }
  for (size_t i = 0; i < json_object_array_length(versions); ++i) {
    Version v;
    if (!readVersion(json_object_array_get_idx(versions, i), serverUrl, v)) continue;
    if (std::any_of(read.versions.begin(), read.versions.end(), [&](const Version &o) { return o.version == v.version; }))
      continue;
    read.versions.push_back(std::move(v));
  }
  std::stable_sort(read.versions.begin(), read.versions.end(),
                   [](const Version &a, const Version &b) { return compare(a.version, b.version) > 0; });
  out = std::move(read);
  return true;
}
std::string serialize(const Version &version) {
  Json o(versionJson(version));
  return json_object_to_json_string_ext(o.get(), JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
}
bool parseOne(const std::string &text, Version &out) {
  Error error;
  auto root = parseJson(text, error);
  Version v;
  if (!root || !readVersion(root.get(), "", v)) return false;
  out = std::move(v);
  return true;
}
std::string folder(const std::string &version) { return "firmware/" + version; }
std::string staging(const std::string &version) { return "switch/cookingfoil/staging/firmware-" + version; }
bool isFirmwareEntry(const std::string &name) {
  return isSafeArchivePath(name) && name.find('/') == std::string::npos && endsWith(name, ".nca") &&
         name.size() > 4;
}
bool pickEntries(const Version &version, const std::vector<ZipEntry> &inZip,
                 std::vector<std::pair<ZipEntry, std::string>> &wanted, std::string &why) {
  wanted.clear();
  const auto into = staging(version.version) + "/";
  for (const auto &entry : inZip) {
    if (entry.name.empty() || entry.name.back() == '/') continue; // a folder record
    if (!isFirmwareEntry(entry.name) || entry.encrypted) {
      why = trf("The archive does not match the server's list: {0}", {entry.name});
      wanted.clear();
      return false;
    }
    wanted.emplace_back(entry, into + entry.name);
  }
  if (wanted.empty() || (version.archive.files && wanted.size() != version.archive.files)) {
    why = trf("The archive does not match the server's list: {0}", {version.version});
    wanted.clear();
    return false;
  }
  return true;
}
std::set<std::string> onCard(const std::string &sd) {
  std::set<std::string> found;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(sd + "/firmware", ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (!isVersion(name) || !it->is_directory(ec)) continue;
    std::error_code inner;
    for (std::filesystem::directory_iterator file(it->path(), inner), stop; !inner && file != stop; file.increment(inner))
      if (endsWith(file->path().filename().string(), ".nca")) {
        found.insert(name);
        break;
      }
  }
  return found;
}
} // namespace foil::firmware
