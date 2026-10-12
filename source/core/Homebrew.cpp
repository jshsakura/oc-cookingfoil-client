#include "core/Homebrew.hpp"
#include "core/HomebrewCard.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <cctype>
namespace foil::homebrew {
namespace {
bool idAllowed(const std::string &id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.';
  });
}
bool fileNameAllowed(const std::string &name) {
  constexpr size_t Ext = 4; // ".nro"
  if (name.size() <= Ext || name.size() > 80 || name.front() == '.' || name.front() == ' ') return false;
  std::string ext = name.substr(name.size() - Ext);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == ".nro" && name.find("..") == std::string::npos &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) {
           return std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.';
         });
}
// A path the server gave: relative, inside switch/, no "..", no backslash.
bool pathAllowed(const std::string &path) {
  if (path.rfind("switch/", 0) != 0 || path.find('\\') != std::string::npos || path.back() == '/') return false;
  size_t start = 0;
  while (start <= path.size()) {
    const auto end = std::min(path.find('/', start), path.size());
    const auto part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = end + 1;
  }
  return true;
}
std::string folderOf(const std::string &path) { return path.substr(0, path.rfind('/')); }
// switch/<app>/...: the app's own folder, two levels deep.
std::string appFolder(const std::string &path) {
  const auto second = path.find('/', 7);
  return second == std::string::npos ? "" : path.substr(0, second);
}
} // namespace
std::vector<App> parse(const std::string &json, const std::string &base) {
  std::vector<App> out;
  Error error;
  auto root = parseJson(json, error);
  auto *list = root ? field(root.get(), "homebrew") : nullptr;
  if (!json_object_is_type(list, json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(list); ++i) {
    auto *o = json_object_array_get_idx(list, i);
    if (!json_object_is_type(o, json_type_object)) continue;
    App a;
    a.id = stringField(o, "id");
    a.name = stringField(o, "name");
    a.version = stringField(o, "version");
    a.author = stringField(o, "author");
    a.description = stringField(o, "description");
    a.hash = stringField(o, "hash");
    a.fileName = stringField(o, "file_name");
    a.iconUrl = urlField(o, "icon_url", base);
    a.downloadUrl = urlField(o, "download_url", base);
    a.source = stringField(o, "source");
    a.releasedAt = stringField(o, "released_at");
    a.repoUrl = stringField(o, "repo_url");
    a.size = uint64_t(std::max<int64_t>(0, integerField(o, "size")));
    a.forwarderUrl = urlField(o, "forwarder_url", base);
    a.forwarderTitleId = stringField(o, "forwarder_title_id");
    a.forwarderSize = uint64_t(std::max<int64_t>(0, integerField(o, "forwarder_size")));
    if (a.forwarderTitleId.size() != 16 || a.forwarderTitleId.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos)
      a.forwarderUrl.clear(), a.forwarderTitleId.clear();
    if (!idAllowed(a.id) || a.name.empty() || a.hash.empty() || !fileNameAllowed(a.fileName) ||
        a.downloadUrl.empty())
      continue;
    // Where it goes: the server's install_path, else <install_dir>/<file>,
    // else switch/<stem>/<file>.
    auto installPath = stringField(o, "install_path");
    const auto installDir = stringField(o, "install_dir");
    if (installPath.empty() && !installDir.empty()) installPath = installDir + "/" + a.fileName;
    if (installPath.empty()) a.installPath = sdPath(a.fileName);
    else if (pathAllowed(installPath) && appFolder(installPath) == folderOf(installPath) &&
             installPath.size() > 4 && installPath.compare(installPath.size() - 4, 4, ".nro") == 0)
      a.installPath = installPath;
    else
      continue;
    // A folder install: every file inside the app's folder, checked.
    auto *files = field(o, "files");
    bool filesOk = true;
    if (json_object_is_type(files, json_type_array)) {
      const auto folder = appFolder(a.installPath);
      for (size_t f = 0; f < json_object_array_length(files) && filesOk; ++f) {
        auto *item = json_object_array_get_idx(files, f);
        File file{stringField(item, "path"), stringField(item, "sha256"), urlField(item, "url", base),
                  uint64_t(std::max<int64_t>(0, integerField(item, "size")))};
        std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(), ::tolower);
        filesOk = json_object_is_type(item, json_type_object) && pathAllowed(file.path) &&
                  file.path.rfind(folder + "/", 0) == 0 && !file.url.empty() && file.sha256.size() == 64 &&
                  file.sha256.find_first_not_of("0123456789abcdef") == std::string::npos;
        a.files.push_back(std::move(file));
      }
    }
    if (!filesOk) continue;
    a.kind = stringField(o, "kind") == "port" ? Kind::Port : Kind::Tool;
    a.app = stringField(o, "app");
    if (!appFolderAllowed(a.app)) a.app = appFolder(a.installPath).substr(7);
    out.push_back(std::move(a));
  }
  return out;
}
std::string serialize(const App &a) {
  Json o(json_object_new_object());
  const auto add = [&](const char *key, const std::string &value) {
    json_object_object_add(o.get(), key, json_object_new_string(value.c_str()));
  };
  add("id", a.id); add("name", a.name); add("version", a.version); add("author", a.author);
  add("description", a.description); add("hash", a.hash); add("file_name", a.fileName);
  add("icon_url", a.iconUrl); add("download_url", a.downloadUrl); add("source", a.source);
  add("released_at", a.releasedAt); add("repo_url", a.repoUrl);
  json_object_object_add(o.get(), "size", json_object_new_int64(int64_t(a.size)));
  add("forwarder_url", a.forwarderUrl);
  add("forwarder_title_id", a.forwarderTitleId);
  json_object_object_add(o.get(), "forwarder_size", json_object_new_int64(int64_t(a.forwarderSize)));
  add("install_path", a.installPath);
  add("kind", a.kind == Kind::Port ? "port" : "tool");
  add("app", a.app);
  if (!a.files.empty()) {
    auto *files = json_object_new_array();
    for (const auto &f : a.files) {
      auto *item = json_object_new_object();
      json_object_object_add(item, "path", json_object_new_string(f.path.c_str()));
      json_object_object_add(item, "sha256", json_object_new_string(f.sha256.c_str()));
      json_object_object_add(item, "url", json_object_new_string(f.url.c_str()));
      json_object_object_add(item, "size", json_object_new_int64(int64_t(f.size)));
      json_object_array_add(files, item);
    }
    json_object_object_add(o.get(), "files", files);
  }
  return json_object_to_json_string_ext(o.get(), JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
}
bool appFolderAllowed(const std::string &app) {
  if (app.empty() || app.size() > 64 || app.find("..") != std::string::npos) return false;
  const auto edge = [](char c) { return c == ' ' || c == '.'; };
  if (edge(app.front()) || edge(app.back())) return false;
  return std::all_of(app.begin(), app.end(), [](unsigned char c) {
    return std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.';
  });
}
bool insideApp(const std::string &app, const std::string &path) {
  const auto folder = "switch/" + app + "/";
  return appFolderAllowed(app) && path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 &&
         path.find(':') == std::string::npos && pathAllowed(path);
}
std::string sdPath(const std::string &fileName) {
  return "switch/" + fileName.substr(0, fileName.size() - 4) + "/" + fileName;
}
std::string etagSha256(const std::string &etag) {
  auto value = etag;
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
  if (value.rfind("sha256:", 0) == 0) value = value.substr(7);
  if (value.size() != 64) return "";
  for (auto &c : value) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!std::isxdigit(static_cast<unsigned char>(c))) return "";
  }
  return value;
}
Ledger parseLedger(const std::string &json) {
  Ledger out;
  Error error;
  auto root = parseJson(json, error);
  if (!root || !json_object_is_type(root.get(), json_type_object)) return out;
  json_object_object_foreach(root.get(), key, value) {
    if (!json_object_is_type(value, json_type_object) || !idAllowed(key)) continue;
    Record r{stringField(value, "hash"), stringField(value, "version"), stringField(value, "file"), {}};
    auto *files = field(value, "files");
    if (json_object_is_type(files, json_type_array))
      for (size_t f = 0; f < json_object_array_length(files); ++f) {
        auto *path = json_object_array_get_idx(files, f);
        if (json_object_is_type(path, json_type_string) && pathAllowed(json_object_get_string(path)))
          r.files.push_back(json_object_get_string(path));
      }
    if (!r.file.empty()) out[key] = r;
  }
  return out;
}
std::string serializeLedger(const Ledger &ledger) {
  Json root(json_object_new_object());
  for (const auto &[id, r] : ledger) {
    auto *o = json_object_new_object();
    json_object_object_add(o, "hash", json_object_new_string(r.hash.c_str()));
    json_object_object_add(o, "version", json_object_new_string(r.version.c_str()));
    json_object_object_add(o, "file", json_object_new_string(r.file.c_str()));
    if (!r.files.empty()) {
      auto *files = json_object_new_array();
      for (const auto &f : r.files) json_object_array_add(files, json_object_new_string(f.c_str()));
      json_object_object_add(o, "files", files);
    }
    json_object_object_add(root.get(), id.c_str(), o);
  }
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
}
Status status(const App &app, const Ledger &ledger, const std::function<bool(const std::string &)> &exists,
              const std::string *cardVersion) {
  const auto found = ledger.find(app.id);
  if (found == ledger.end() || !exists(found->second.file))
    return cardVersion ? cardStatus(app.version, *cardVersion) : Status::Available;
  // Same build in another place (the server moved it) also reads as an update.
  return found->second.hash == app.hash && found->second.file == app.installPath ? Status::Installed : Status::Update;
}
const App *findApp(const std::vector<App> &apps, const std::string &folder) {
  if (folder.empty()) return nullptr;
  const auto found = std::find_if(apps.begin(), apps.end(), [&](const App &app) { return app.app == folder; });
  return found == apps.end() ? nullptr : &*found;
}
std::string appTitle(const std::vector<App> &apps, const std::string &folder) {
  const auto *app = findApp(apps, folder);
  return app && !app->name.empty() ? app->name : folder;
}
std::string ownerKey(const std::string &titleId, const std::string &folder) {
  return titleId.empty() && !folder.empty() ? "app:" + folder : titleId;
}
} // namespace foil::homebrew
