#include "core/Homebrew.hpp"
#include "core/Forwarder.hpp"
#include "core/HomebrewCard.hpp"
#include "core/Json.hpp"
#include "core/TitleGuard.hpp"
#include <algorithm>
#include <set>
#include <sys/stat.h>
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
// Every file inside the app's folder, checked; false when one is not.
bool readFiles(json_object *files, const std::string &installPath, const std::string &base, std::vector<File> &out) {
  const auto folder = appFolder(installPath);
  for (size_t f = 0; f < json_object_array_length(files); ++f) {
    auto *item = json_object_array_get_idx(files, f);
    File file{stringField(item, "path"), stringField(item, "sha256"), urlField(item, "url", base),
              uint64_t(std::max<int64_t>(0, integerField(item, "size")))};
    std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(), ::tolower);
    if (!json_object_is_type(item, json_type_object) || !pathAllowed(file.path) ||
        file.path.rfind(folder + "/", 0) != 0 || file.url.empty() || file.sha256.size() != 64 ||
        file.sha256.find_first_not_of("0123456789abcdef") != std::string::npos)
      return false;
    out.push_back(std::move(file));
  }
  return true;
}
} // namespace
bool parseFiles(const std::string &json, App &app, const std::string &base) {
  Error error;
  auto root = parseJson(json, error);
  auto *files = root ? field(root.get(), "files") : nullptr;
  std::vector<File> read;
  if (!json_object_is_type(files, json_type_array) || !readFiles(files, app.installPath, base, read) || read.empty())
    return false;
  app.files = std::move(read);
  return true;
}
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
    a.bundleUrl = urlField(o, "bundle_url", base);
    a.source = stringField(o, "source");
    a.releasedAt = stringField(o, "released_at");
    a.repoUrl = stringField(o, "repo_url");
    a.size = uint64_t(std::max<int64_t>(0, integerField(o, "size")));
    a.forwarderUrl = urlField(o, "forwarder_url", base);
    a.forwarderTitleId = stringField(o, "forwarder_title_id");
    a.forwarderSize = uint64_t(std::max<int64_t>(0, integerField(o, "forwarder_size")));
    a.forwarderCores = integerField(o, "forwarder_cores", 3) == 4 ? 4 : 3;
    a.forwarderKey = stringField(o, "forwarder_key");
    if (!forwarder::serverShortcutId(a.forwarderTitleId))
      a.forwarderUrl.clear(), a.forwarderTitleId.clear();
    a.kind = stringField(o, "kind") == "port" ? Kind::Port : Kind::Tool;
    a.bannerUrl = urlField(o, "banner_url", base);
    a.screenshots = urlList(o, "screenshots", base, 8);
    if (auto *codes = field(o, "languages"); json_object_is_type(codes, json_type_array))
      for (size_t k = 0; k < json_object_array_length(codes) && a.languages.size() < 32; ++k)
        if (auto *code = json_object_array_get_idx(codes, k); json_object_is_type(code, json_type_string))
          a.languages.emplace_back(json_object_get_string(code));
    a.requiredFirmware = stringField(o, "required_firmware");
    a.releaseNotes = stringField(o, "release_notes").substr(0, 6000);
    a.license = stringField(o, "license");
    a.updatedAt = stringField(o, "updated_at");
    if (auto *stars = field(o, "stars"); json_object_is_type(stars, json_type_int))
      a.stars = int(std::clamp<int64_t>(json_object_get_int64(stars), 0, INT32_MAX));
    if (stringField(o, "install_type") == "title") {
      // Installed as a game is: nothing on the SD card but the title. One
      // naming a system title is never offered.
      a.title = true;
      a.titleId = stringField(o, "title_id");
      std::transform(a.titleId.begin(), a.titleId.end(), a.titleId.begin(), ::toupper);
      a.forwarderUrl.clear(), a.forwarderTitleId.clear(), a.forwarderSize = 0;
      if (!idAllowed(a.id) || a.name.empty() || a.downloadUrl.empty() || a.titleId.size() != 16 ||
          a.titleId.find_first_not_of("0123456789ABCDEF") != std::string::npos || !catalogTitleAllowed(a.titleId))
        continue;
      out.push_back(std::move(a));
      continue;
    }
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
    // A folder install: every file inside the app's folder, checked; or,
    // left out of the list, how many there are and where they are listed.
    if (auto *files = field(o, "files"); json_object_is_type(files, json_type_array)) {
      if (!readFiles(files, a.installPath, base, a.files)) continue;
    } else if (const auto count = integerField(o, "files_count"); count > 0) {
      a.filesCount = size_t(count);
      a.filesSize = uint64_t(std::max<int64_t>(0, integerField(o, "files_size")));
      a.filesUrl = urlField(o, "files_url", base);
      if (a.filesUrl.empty() && !base.empty()) a.filesUrl = base + "/api/homebrew/" + a.id + "/files";
    }
    // A zip taken whole: every entry goes somewhere under switch/ by a path
    // that cannot leave it; one that does not leaves the zip out (the .nro
    // alone still comes from download_url).
    if (auto *archive = field(o, "archive"); json_object_is_type(archive, json_type_object)) {
      Archive zip;
      zip.format = stringField(archive, "format");
      zip.url = urlField(archive, "url", base);
      zip.sha256 = stringField(archive, "sha256");
      zip.size = uint64_t(std::max<int64_t>(0, integerField(archive, "size")));
      bool entriesOk = true;
      auto *entries = field(archive, "entries");
      if (json_object_is_type(entries, json_type_array))
        for (size_t e = 0; e < json_object_array_length(entries) && entriesOk; ++e) {
          auto *item = json_object_array_get_idx(entries, e);
          ArchiveEntry entry{stringField(item, "entry"), stringField(item, "path"),
                             uint64_t(std::max<int64_t>(0, integerField(item, "size")))};
          entriesOk = !entry.entry.empty() && pathAllowed(entry.path) && entry.path.rfind("switch/", 0) == 0;
          zip.entries.push_back(std::move(entry));
        }
      if (entriesOk && zip.usable()) a.archive = std::move(zip);
    }
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
  add("icon_url", a.iconUrl); add("download_url", a.downloadUrl); add("bundle_url", a.bundleUrl); add("source", a.source);
  add("released_at", a.releasedAt); add("repo_url", a.repoUrl);
  json_object_object_add(o.get(), "size", json_object_new_int64(int64_t(a.size)));
  add("forwarder_url", a.forwarderUrl);
  add("forwarder_title_id", a.forwarderTitleId);
  json_object_object_add(o.get(), "forwarder_size", json_object_new_int64(int64_t(a.forwarderSize)));
  json_object_object_add(o.get(), "forwarder_cores", json_object_new_int(a.forwarderCores));
  add("forwarder_key", a.forwarderKey);
  add("install_path", a.installPath);
  add("kind", a.kind == Kind::Port ? "port" : "tool");
  add("app", a.app);
  if (!a.bannerUrl.empty()) add("banner_url", a.bannerUrl);
  if (!a.screenshots.empty()) {
    auto *shots = json_object_new_array();
    for (const auto &url : a.screenshots) json_object_array_add(shots, json_object_new_string(url.c_str()));
    json_object_object_add(o.get(), "screenshots", shots);
  }
  if (a.title) {
    add("install_type", "title");
    add("title_id", a.titleId);
  }
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
  } else if (a.filesCount) {
    json_object_object_add(o.get(), "files_count", json_object_new_int64(int64_t(a.filesCount)));
    json_object_object_add(o.get(), "files_size", json_object_new_int64(int64_t(a.filesSize)));
    add("files_url", a.filesUrl);
  }
  if (a.archive.usable()) {
    auto *archive = json_object_new_object();
    json_object_object_add(archive, "format", json_object_new_string(a.archive.format.c_str()));
    json_object_object_add(archive, "url", json_object_new_string(a.archive.url.c_str()));
    json_object_object_add(archive, "sha256", json_object_new_string(a.archive.sha256.c_str()));
    json_object_object_add(archive, "size", json_object_new_int64(int64_t(a.archive.size)));
    auto *entries = json_object_new_array();
    for (const auto &e : a.archive.entries) {
      auto *item = json_object_new_object();
      json_object_object_add(item, "entry", json_object_new_string(e.entry.c_str()));
      json_object_object_add(item, "path", json_object_new_string(e.path.c_str()));
      json_object_object_add(item, "size", json_object_new_int64(int64_t(e.size)));
      json_object_array_add(entries, item);
    }
    json_object_object_add(archive, "entries", entries);
    json_object_object_add(o.get(), "archive", archive);
  }
  return json_object_to_json_string_ext(o.get(), JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
}
std::string plainNotes(const std::string &markdown) {
  std::string out;
  size_t start = 0;
  int blanks = 0;
  while (start <= markdown.size()) {
    const auto end = std::min(markdown.find('\n', start), markdown.size());
    std::string line = markdown.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // HTML tags (<br>, <img ...>, <details>) are not text.
    for (size_t open; (open = line.find('<')) != std::string::npos;) {
      const auto close = line.find('>', open);
      if (close == std::string::npos) break;
      line.erase(open, close - open + 1);
    }
    // ![alt](src) goes; [words](url) keeps its words.
    for (size_t open; (open = line.find('[')) != std::string::npos;) {
      const auto close = line.find("](", open);
      const auto stop = close == std::string::npos ? std::string::npos : line.find(')', close);
      if (stop == std::string::npos) break;
      const bool image = open > 0 && line[open - 1] == '!';
      const auto words = image ? std::string() : line.substr(open + 1, close - open - 1);
      line.replace(image ? open - 1 : open, stop - open + 1 + (image ? 1 : 0), words);
    }
    const auto indent = line.find_first_not_of(" \t");
    auto first = indent;
    if (first == std::string::npos) {
      if (!out.empty() && ++blanks == 1) out += "\n";
      continue;
    }
    std::string body = line.substr(first);
    const bool heading = body[0] == '#';
    while (!body.empty() && (body[0] == '#' || body[0] == '>')) body.erase(0, 1);
    if (body.size() > 1 && (body[0] == '-' || body[0] == '*' || body[0] == '+') && body[1] == ' ')
      body = "• " + body.substr(2);
    if (body.find_first_not_of("-*_= ") == std::string::npos) continue; // a rule
    for (const char *mark : {"**", "__", "`"})
      for (size_t at; (at = body.find(mark)) != std::string::npos;) body.erase(at, std::string(mark).size());
    first = body.find_first_not_of(' ');
    if (first == std::string::npos) continue;
    if (heading && !out.empty() && blanks == 0) out += "\n";
    const bool nested = indent >= 2 && body.compare(first, 4, "• ") == 0; // a list inside a list
    out += (nested ? "  " : "") + body.substr(first) + "\n";
    blanks = 0;
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();
  return out;
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
    Record r{stringField(value, "hash"), stringField(value, "version"), stringField(value, "file"), {}, {}, {}};
    r.seen = {uint64_t(std::max<int64_t>(0, integerField(value, "size"))), integerField(value, "mtime")};
    r.fileSha = stringField(value, "fileSha");
    r.shortcutKey = stringField(value, "shortcutKey");
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
    if (r.seen.size) {
      json_object_object_add(o, "size", json_object_new_int64(int64_t(r.seen.size)));
      json_object_object_add(o, "mtime", json_object_new_int64(r.seen.mtime));
    }
    if (!r.fileSha.empty()) json_object_object_add(o, "fileSha", json_object_new_string(r.fileSha.c_str()));
    if (!r.shortcutKey.empty()) json_object_object_add(o, "shortcutKey", json_object_new_string(r.shortcutKey.c_str()));
    json_object_object_add(root.get(), id.c_str(), o);
  }
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE);
}
std::string fileListPath(const std::string &id) { return "switch/cookingfoil/homebrew-files/" + id + ".json"; }
std::string serializeFileList(const std::vector<std::string> &files) {
  Json root(json_object_new_array());
  for (const auto &f : files) json_object_array_add(root.get(), json_object_new_string(f.c_str()));
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
}
std::vector<std::string> parseFileList(const std::string &json) {
  std::vector<std::string> out;
  Error error;
  auto root = parseJson(json, error);
  if (!root || !json_object_is_type(root.get(), json_type_array)) return out;
  for (size_t f = 0; f < json_object_array_length(root.get()); ++f) {
    auto *path = json_object_array_get_idx(root.get(), f);
    if (json_object_is_type(path, json_type_string) && pathAllowed(json_object_get_string(path)))
      out.push_back(json_object_get_string(path));
  }
  return out;
}
std::vector<std::string> movedAway(const std::string &oldFile, const std::vector<std::string> &oldFiles, const App &app) {
  const auto folder = [](const std::string &path) { return path.substr(0, path.rfind('/') + 1); };
  if (oldFile.empty() || folder(oldFile) == folder(app.installPath)) return {};
  std::set<std::string> used{app.installPath};
  for (const auto &file : app.files) used.insert(file.path);
  for (const auto &entry : app.archive.entries) used.insert(entry.path);
  std::vector<std::string> out;
  for (const auto &file : oldFiles)
    if (!used.count(file)) out.push_back(file);
  return out;
}
std::string verifiedPath(const std::string &id) { return "switch/cookingfoil/homebrew-files/" + id + ".seen"; }
// One line a file: hash, size, time, then the path (which may hold spaces).
std::string serializeVerified(const VerifiedFiles &files) {
  std::string out;
  for (const auto &[path, file] : files)
    out += file.sha256 + " " + std::to_string(file.seen.size) + " " + std::to_string(file.seen.mtime) + " " + path + "\n";
  return out;
}
VerifiedFiles parseVerified(const std::string &text) {
  VerifiedFiles out;
  size_t at = 0;
  while (at < text.size()) {
    const auto end = std::min(text.find('\n', at), text.size());
    const auto line = text.substr(at, end - at);
    at = end + 1;
    const auto a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1),
               c = b == std::string::npos ? b : line.find(' ', b + 1);
    if (c == std::string::npos || a != 64) continue;
    const auto path = line.substr(c + 1);
    if (!pathAllowed(path)) continue;
    try {
      out[path] = {line.substr(0, a), {std::stoull(line.substr(a + 1, b - a - 1)), std::stoll(line.substr(b + 1, c - b - 1))}};
    } catch (...) {
    }
  }
  return out;
}
bool stillVerified(const VerifiedFiles &files, const std::string &path, const std::string &sha256,
                   const std::optional<CardFile> &now) {
  const auto found = files.find(path);
  return now && !sha256.empty() && found != files.end() && found->second.sha256 == sha256 && found->second.seen == *now;
}
std::optional<CardFile> cardFile(const std::string &fullPath) {
  struct stat st {};
  if (::stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
  return CardFile{uint64_t(st.st_size), int64_t(st.st_mtime)};
}
LedgerCheck checkLedger(const Ledger &ledger, const std::function<std::optional<CardFile>(const std::string &)> &look,
                        const std::function<std::string(const std::string &)> &sha256,
                        const std::function<std::string(const std::string &)> &cardVersion) {
  LedgerCheck out;
  for (const auto &[id, record] : ledger) {
    const auto now = look(record.file);
    out.onCard[record.file] = now.has_value();
    if (!now || (record.seen.size && record.seen == *now && !record.fileSha.empty())) continue;
    const auto sha = sha256(record.file);
    ++out.hashed;
    if (sha.empty()) continue; // unreadable now: left for the next look
    auto fixed = record;
    fixed.seen = *now;
    fixed.fileSha = sha;
    if (!record.fileSha.empty() && sha != record.fileSha) {
      fixed.hash.clear();
      fixed.version = cardVersion(record.file);
      out.followed.push_back(id);
    }
    out.fixed[id] = std::move(fixed);
  }
  return out;
}
void sortRows(const std::vector<App> &apps, std::vector<int> &rows, SortMode sort) {
  const auto lowerAscii = [](std::string text) {
    for (auto &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  const auto by = [&](auto less) {
    std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return less(apps[a], apps[b]); });
  };
  if (sort == SortMode::Name)
    by([&](const App &a, const App &b) { return lowerAscii(a.name) < lowerAscii(b.name); });
  else if (sort == SortMode::Popular) // GitHub stars; an app without them after
    by([](const App &a, const App &b) { return a.stars > b.stars; });
  else if (sort == SortMode::Largest)
    by([](const App &a, const App &b) { return a.size > b.size; });
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
uint64_t bundleSize(const App &app) {
  uint64_t total = 1024;
  std::set<std::string> folders;
  for (const auto &file : app.files) {
    total += 512 + (file.size + 511) / 512 * 512;
    for (auto slash = file.path.find('/'); slash != std::string::npos; slash = file.path.find('/', slash + 1))
      folders.insert(file.path.substr(0, slash));
  }
  return total + 512 * folders.size();
}
} // namespace foil::homebrew
