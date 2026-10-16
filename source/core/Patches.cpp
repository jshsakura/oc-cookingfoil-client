#include <cerrno>
#include "core/Log.hpp"
#include "core/Patches.hpp"
#include "core/Resume.hpp"
#include "core/Homebrew.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <utility>
namespace foil {
namespace fs = std::filesystem;
namespace {
constexpr size_t Block = 512;
constexpr uint64_t MaxPaxBytes = 64 * 1024;
std::vector<std::string> split(const std::string &path) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const auto slash = path.find('/', start);
    parts.push_back(path.substr(start, slash - start));
    if (slash == std::string::npos)
      return parts;
    start = slash + 1;
  }
}
std::string upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return text;
}
bool same(const std::string &a, const char *b) { return upper(a) == upper(b); }
bool validId(const std::string &id) {
  return !id.empty() && id.size() <= 64 &&
         id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
             std::string::npos;
}
// Folders the layout depends on; pruning empty folders stops here.
bool keepFolder(const std::string &rel) {
  static const char *keep[] = {"atmosphere", "atmosphere/contents", "atmosphere/exefs_patches",
                               "SaltySD", "SaltySD/plugins", "SaltySD/plugins/FPSLocker",
                               "SaltySD/plugins/FPSLocker/patches"};
  for (const auto *k : keep)
    if (upper(rel) == upper(k))
      return true;
  // switch/ and an app's own folder stay too.
  const auto parts = split(rel);
  if (same(parts[0], "switch") && parts.size() <= 2)
    return true;
  return rel.empty() || rel == ".";
}
uint64_t octal(const char *field, size_t size) {
  uint64_t value = 0;
  for (size_t i = 0; i < size && field[i]; ++i) {
    if (field[i] == ' ')
      continue;
    if (field[i] < '0' || field[i] > '7')
      return UINT64_MAX;
    value = value * 8 + uint64_t(field[i] - '0');
  }
  return value;
}
std::string text(const char *field, size_t size) {
  const auto *end = static_cast<const char *>(std::memchr(field, 0, size));
  return std::string(field, end ? size_t(end - field) : size);
}
uint64_t padding(uint64_t size) { return (Block - size % Block) % Block; }
// A PAX number: digits only.
std::optional<uint64_t> paxNumber(const std::string &text) {
  if (text.empty() || text.size() > 19 || text.find_first_not_of("0123456789") != std::string::npos)
    return std::nullopt;
  return uint64_t(std::strtoull(text.c_str(), nullptr, 10));
}
std::vector<std::string> strings(json_object *array) {
  std::vector<std::string> out;
  if (!json_object_is_type(array, json_type_array))
    return out;
  for (size_t i = 0; i < json_object_array_length(array); ++i) {
    auto *value = json_object_array_get_idx(array, i);
    if (json_object_is_type(value, json_type_string))
      out.push_back(json_object_get_string(value));
  }
  return out;
}
json_object *array(const std::vector<std::string> &values) {
  auto *out = json_object_new_array();
  for (const auto &value : values)
    json_object_array_add(out, json_object_new_string(value.c_str()));
  return out;
}
// Whether the patch has a file at path (its files are kept sorted).
bool lists(const PatchRecord &record, const std::string &path) {
  return std::binary_search(record.files.begin(), record.files.end(), path);
}
// The folder right under a path's fixed prefix, which a patch may move whole:
// atmosphere/contents/<TID>/<folder>, atmosphere/exefs_patches/<name>,
// SaltySD/plugins/FPSLocker/patches/<TID> and switch/<app>/<folder>. Empty
// for a file sitting directly in the prefix, for cheats (merged, never moved)
// and for paths outside these layouts.
std::string wholeRoot(const std::string &path) {
  const auto parts = split(path);
  size_t depth = 0;
  if (parts.size() >= 5 && same(parts[0], "atmosphere") && same(parts[1], "contents") && !same(parts[3], "cheats"))
    depth = 4;
  else if (parts.size() >= 4 && same(parts[0], "atmosphere") && same(parts[1], "exefs_patches"))
    depth = 3;
  else if (parts.size() >= 6 && same(parts[0], "SaltySD") && same(parts[1], "plugins") &&
           same(parts[2], "FPSLocker") && same(parts[3], "patches"))
    depth = 5;
  else if (parts.size() >= 4 && same(parts[0], "switch"))
    depth = 3;
  std::string root;
  for (size_t i = 0; i < depth; ++i)
    root += (i ? "/" : "") + parts[i];
  return root;
}
// Whether path lies below root; case does not matter on the card.
bool under(const std::string &path, const std::string &root) {
  return path.size() > root.size() && path[root.size()] == '/' && upper(path.substr(0, root.size())) == upper(root);
}
bool anyUnder(const std::vector<std::string> &paths, const std::string &root) {
  return std::any_of(paths.begin(), paths.end(), [&](const std::string &p) { return under(p, root); });
}
bool inWhole(const std::string &path, const std::vector<std::string> &whole) {
  return std::any_of(whole.begin(), whole.end(), [&](const std::string &root) { return under(path, root); });
}
} // namespace

std::optional<FileMark> fileMark(const std::string &fullPath) {
  struct stat st {};
  if (::stat(fullPath.c_str(), &st) != 0)
    return std::nullopt;
  return FileMark{uint64_t(st.st_size), int64_t(st.st_mtime)};
}
bool sameFile(const FileMark &then, const FileMark &now) {
  return !then.foreign() && then.size == now.size && (then.mtime == 0 || now.mtime == 0 || then.mtime == now.mtime);
}
bool patchPathAllowed(const std::string &titleId, const std::string &path) {
  if (titleId.size() != 16 || path.empty() || path.find_first_of("\\:") != std::string::npos)
    return false;
  const auto parts = split(path);
  for (const auto &part : parts)
    if (part.empty() || part == "." || part == "..")
      return false;
  const auto tid = upper(titleId);
  if (parts.size() >= 4 && same(parts[0], "atmosphere") && same(parts[1], "contents"))
    return upper(parts[2]) == tid;
  if (parts.size() >= 4 && same(parts[0], "atmosphere") && same(parts[1], "exefs_patches"))
    return true;
  if (parts.size() >= 6 && same(parts[0], "SaltySD") && same(parts[1], "plugins") &&
      same(parts[2], "FPSLocker") && same(parts[3], "patches"))
    return upper(parts[4]) == tid;
  return false;
}
bool patchPathAllowed(const UserPatch &patch, const std::string &path) {
  if (!patch.homebrewApp.empty())
    return homebrew::insideApp(patch.homebrewApp, path);
  return patchPathAllowed(patch.titleId, path);
}
std::string baseTitleId(const std::string &titleId) {
  auto id = upper(titleId);
  if (id.size() != 16 || id.find_first_not_of("0123456789ABCDEF") != std::string::npos)
    return id;
  char text[17];
  std::snprintf(text, sizeof(text), "%016llX",
                static_cast<unsigned long long>(std::stoull(id, nullptr, 16) & ~0x1FFFULL));
  return text;
}
std::string patchOwner(const UserPatch &patch) {
  return homebrew::ownerKey(patch.titleId.empty() ? std::string() : baseTitleId(patch.titleId), patch.homebrewApp);
}
bool patchBelongs(const UserPatch &patch, const std::string &owner) {
  if (owner.empty()) return false;
  const auto key = owner.rfind("app:", 0) == 0 ? owner : baseTitleId(owner);
  return patchOwner(patch) == key;
}
std::vector<UserPatch> patchesFor(const std::vector<UserPatch> &patches, const std::string &owner) {
  std::vector<UserPatch> out;
  for (const auto &patch : patches)
    if (patchBelongs(patch, owner)) out.push_back(patch);
  return out;
}
std::string normalizeVersion(const std::string &version) {
  const auto first = version.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  auto text = version.substr(first, version.find_last_not_of(" \t\r\n") - first + 1);
  if (text[0] == 'v' || text[0] == 'V') text.erase(0, 1);
  return text;
}
bool patchFitsVersion(const UserPatch &patch, const std::string &version) {
  const auto wanted = normalizeVersion(patch.gameVersion), have = normalizeVersion(version);
  return wanted.empty() || upper(wanted) == "ANY" || have.empty() || wanted == have;
}
PatchStep patchStep(const UserPatch &patch, const PatchRecord *record) {
  if (!record) return PatchStep::Get;
  if (!patch.hash.empty() && record->hash != patch.hash) return PatchStep::Update;
  return record->on ? PatchStep::Skip : PatchStep::Enable;
}
bool isCheatPath(const std::string &path) {
  const auto parts = split(path);
  return parts.size() == 5 && same(parts[0], "atmosphere") && same(parts[1], "contents") &&
         same(parts[3], "cheats") && parts[4].size() > 4 &&
         same(parts[4].substr(parts[4].size() - 4), ".txt");
}
const char *patchKindLabel(const std::string &kind) {
  if (kind == "romfs")
    return tr("Data patch");
  if (kind == "exefs")
    return tr("Executable patch");
  if (kind == "cheats")
    return tr("Cheats");
  if (kind == "exefs_patches")
    return tr("IPS patch");
  if (kind == "fpslocker")
    return "FPSLocker";
  if (kind == "homebrew")
    return tr("Homebrew");
  return tr("Other");
}

uint64_t tarSize(const char *field) {
  const auto first = static_cast<unsigned char>(field[0]);
  if (!(first & 0x80))
    return octal(field, 12);
  if (first == 0xFF)
    return UINT64_MAX; // negative
  uint64_t value = first & 0x7F;
  for (size_t i = 1; i < 12; ++i) {
    if (value >> 56)
      return UINT64_MAX; // more than 64 bits
    value = (value << 8) | static_cast<unsigned char>(field[i]);
  }
  return value;
}
TarWriter::TarWriter(std::string root, std::string titleId)
    : root_(std::move(root)), titleId_(std::move(titleId)) {}
TarWriter::TarWriter(std::string root, std::function<bool(const std::string &)> allow)
    : root_(std::move(root)), allow_(std::move(allow)) {}
TarWriter::~TarWriter() {
  if (file_)
    std::fclose(file_);
}
bool TarWriter::fail(const std::string &message) {
  if (error_.empty())
    error_ = message;
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  return false;
}
void TarWriter::abandon() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  if (!current_.empty() && remaining_ > 0) {
    std::error_code ec;
    fs::remove(root_ + "/" + current_, ec);
  }
  current_.clear();
}
bool TarWriter::openFile(const std::string &path, uint64_t size) {
  const fs::path full = root_ + "/" + path;
  std::error_code ec;
  makeDirectories(std::filesystem::path(full.parent_path()).string());
  // 4 GiB or more: made as a split file first, or FAT32 would cut it short.
  if (std::string reason; platform::needsBigFile(size) && !platform::createBigFile(full.string(), reason)) {
    logLine("tar: cannot create big file " + full.string() + ": " + reason);
    return fail(trf("Could not create a file over 4 GB on the SD card: {0}", {path}) + " (" + reason + ")");
  }
  file_ = std::fopen(full.string().c_str(), "wb");
  current_ = path;
  writeBuffer(file_);
  if (!file_) {
    // The system's reason goes on screen too: on the console log.txt is out
    // of reach while the app runs.
    const std::string reason = std::strerror(errno);
    const bool folder = !std::filesystem::is_directory(full.parent_path(), ec);
    logLine("tar: cannot create " + full.string() + ": " + reason + (folder ? " (no folder)" : ""));
    return fail(trf("Could not create file: {0}", {path}) + " (" + reason + (folder ? ", no folder" : "") + ")");
  }
  return true;
}
bool TarWriter::header() {
  const char *b = block_.data();
  if (std::all_of(block_.begin(), block_.end(), [](char c) { return c == 0; })) {
    if (++zeroBlocks_ == 2)
      ended_ = true;
    return true;
  }
  zeroBlocks_ = 0;
  if (std::memcmp(b + 257, "ustar", 5) != 0)
    return fail(tr("Not a patch file."));
  unsigned sum = 0;
  for (size_t i = 0; i < Block; ++i)
    sum += (i >= 148 && i < 156) ? unsigned(' ') : unsigned(static_cast<unsigned char>(b[i]));
  if (octal(b + 148, 8) != sum)
    return fail(tr("The downloaded patch file is damaged."));
  uint64_t size = tarSize(b + 124);
  if (size == UINT64_MAX)
    return fail(tr("The downloaded patch file is damaged."));
  const char type = b[156];
  auto name = text(b, 100);
  const auto prefix = text(b + 345, 155);
  if (!prefix.empty())
    name = prefix + "/" + name;
  if (type != 'x' && type != 'g') {
    if (!paxPath_.empty())
      name = std::exchange(paxPath_, "");
    if (paxSize_)
      size = *std::exchange(paxSize_, std::nullopt);
  }
  remaining_ = size;
  padding_ = padding(size);
  if (type == 'x' || type == 'g') {
    if (size > MaxPaxBytes)
      return fail(tr("A patch file entry header is too long."));
    pax_ = true;
    paxGlobal_ = type == 'g';
    paxData_.clear();
    return true;
  }
  if (type == '5') { // folders are created with their files
    if (size)
      return fail(tr("The downloaded patch file is damaged."));
    return true;
  }
  if (type != '0' && type != '\0' && type != '7')
    return fail(trf("The patch has an entry that is not a regular file: {0}", {name}));
  while (name.rfind("./", 0) == 0)
    name.erase(0, 2);
  if (allow_ ? !allow_(name) : !patchPathAllowed(titleId_, name))
    return fail(trf("The patch tries to write to a disallowed location: {0}", {name}));
  if (skip_ && skip_(name))
    return true; // its bytes pass by with no file open
  if (!openFile(name, size))
    return false;
  if (size == 0) {
    std::fclose(file_);
    file_ = nullptr;
    current_.clear();
  }
  return true;
}
void TarWriter::readPax() {
  size_t at = 0;
  while (at < paxData_.size()) {
    const auto space = paxData_.find(' ', at);
    if (space == std::string::npos)
      return;
    const auto length = std::strtoull(paxData_.c_str() + at, nullptr, 10);
    if (length == 0 || at + length > paxData_.size())
      return;
    const auto record = paxData_.substr(space + 1, at + length - space - 2);
    if (!paxGlobal_ && record.rfind("path=", 0) == 0)
      paxPath_ = record.substr(5);
    // Sizes past what the header's field holds come here.
    if (!paxGlobal_ && record.rfind("size=", 0) == 0)
      paxSize_ = paxNumber(record.substr(5));
    at += length;
  }
}
bool TarWriter::feed(const char *data, size_t size) {
  if (!error_.empty())
    return false;
  const auto consume = [&](size_t take) {
    data += take;
    size -= take;
    fed_ += take;
  };
  while (size > 0) {
    if (remaining_ > 0) {
      const auto take = size_t(std::min<uint64_t>(remaining_, size));
      if (pax_)
        paxData_.append(data, take);
      else if (file_ && std::fwrite(data, 1, take, file_) != take)
        return fail(tr("Could not write to the SD card. Check the free space."));
      remaining_ -= take;
      consume(take);
      if (remaining_ == 0) {
        if (pax_) {
          readPax();
          pax_ = false;
        } else {
          current_.clear();
          if (file_) {
            const bool closed = std::fclose(file_) == 0;
            file_ = nullptr;
            if (!closed)
              return fail(tr("Could not write to the SD card. Check the free space."));
          }
        }
      }
    } else if (padding_ > 0) {
      const auto take = size_t(std::min<uint64_t>(padding_, size));
      padding_ -= take;
      consume(take);
    } else {
      if (ended_)
        return true; // trailing zero blocks after the end marker
      const auto take = std::min(Block - block_.size(), size);
      block_.append(data, take);
      consume(take);
      if (block_.size() == Block) {
        if (!header())
          return false;
        block_.clear();
      }
    }
    // Between entries with nothing carried over: a place to resume from.
    if (!remaining_ && !padding_ && block_.empty() && !pax_ && paxPath_.empty() && !paxSize_ &&
        !zeroBlocks_ && !file_)
      boundary_ = fed_;
  }
  return true;
}
bool TarWriter::finish() {
  if (!error_.empty())
    return false;
  if (!ended_ || remaining_ > 0)
    return fail(tr("The patch download was interrupted. Download it again."));
  return true;
}

PatchStore::PatchStore(std::string sdRoot, std::string storeRoot)
    : sdRoot_(std::move(sdRoot)), storeRoot_(std::move(storeRoot)) {
  while (sdRoot_.size() > 1 && sdRoot_.back() == '/')
    sdRoot_.pop_back();
}
std::string PatchStore::sd(const std::string &path) const { return sdRoot_ + "/" + path; }
std::string PatchStore::stored(const std::string &id, const std::string &path) const {
  return storeRoot_ + "/" + id + "/" + path;
}
std::string PatchStore::base(const std::string &path) const {
  return storeRoot_ + "/_base/" + path;
}
std::string PatchStore::staging(const std::string &id) const {
  return storeRoot_ + "/" + id + ".part";
}
const PatchRecord *PatchStore::find(const std::string &id) const {
  const auto it = records_.find(id);
  return it == records_.end() ? nullptr : &it->second;
}
bool PatchStore::save(Error &error) {
  markShadows();
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "order", json_object_new_int64(order_));
  json_object_object_add(root.get(), "cheatPaths",
                         array({cheatPaths_.begin(), cheatPaths_.end()}));
  auto *patches = json_object_new_object();
  for (const auto &[id, r] : records_) {
    auto *p = json_object_new_object();
    json_object_object_add(p, "titleId", json_object_new_string(r.titleId.c_str()));
    if (!r.homebrewApp.empty())
      json_object_object_add(p, "homebrew", json_object_new_string(r.homebrewApp.c_str()));
    json_object_object_add(p, "name", json_object_new_string(r.name.c_str()));
    json_object_object_add(p, "version", json_object_new_string(r.version.c_str()));
    json_object_object_add(p, "hash", json_object_new_string(r.hash.c_str()));
    json_object_object_add(p, "on", json_object_new_boolean(r.on));
    json_object_object_add(p, "pending", json_object_new_string(r.pending.c_str()));
    json_object_object_add(p, "files", array(r.files));
    json_object_object_add(p, "cheats", array(r.cheats));
    json_object_object_add(p, "bytes", json_object_new_int64(int64_t(r.bytes)));
    json_object_object_add(p, "order", json_object_new_int64(r.order));
    if (!r.roots.empty())
      json_object_object_add(p, "roots", array(r.roots));
    if (!r.marks.empty()) {
      auto *marks = json_object_new_object();
      for (const auto &[path, m] : r.marks) {
        auto *pair = json_object_new_array();
        json_object_array_add(pair, json_object_new_int64(int64_t(m.size)));
        json_object_array_add(pair, json_object_new_int64(m.mtime));
        json_object_object_add(marks, path.c_str(), pair);
      }
      json_object_object_add(p, "marks", marks);
    }
    if (!r.shadowed.empty()) {
      auto *shadowed = json_object_new_object();
      for (const auto &[path, by] : r.shadowed)
        json_object_object_add(shadowed, path.c_str(), json_object_new_string(by.c_str()));
      json_object_object_add(p, "shadowed", shadowed);
    }
    json_object_object_add(patches, id.c_str(), p);
  }
  json_object_object_add(root.get(), "patches", patches);
  return atomicWrite(storeRoot_ + "/ledger.json",
                     json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PRETTY),
                     error.message);
}
bool PatchStore::move(const std::string &from, const std::string &to, Error &error) {
  std::error_code ec;
  makeDirectories(std::filesystem::path(fs::path(to).parent_path()).string());
  fs::rename(from, to, ec);
  if (ec)
    error.message = tr("Could not move the patch files. Check the SD card.");
  return !ec;
}
void PatchStore::prune(const std::string &path) const {
  std::error_code ec;
  auto folder = fs::path(path).parent_path();
  const auto rootLength = sdRoot_.size() + 1;
  while (folder.string().size() > rootLength) {
    const auto rel = folder.string().substr(rootLength);
    if (keepFolder(rel) || !fs::is_empty(folder, ec) || ec || !fs::remove(folder, ec))
      return;
    folder = folder.parent_path();
  }
}
bool PatchStore::load(Error &error) {
  records_.clear();
  cheatPaths_.clear();
  std::error_code ec;
  const auto ledger = storeRoot_ + "/ledger.json";
  if (fs::exists(ledger, ec) || fs::exists(ledger + ".old", ec)) {
    auto root = parseJson(readFile(ledger), error);
    if (error)
      return false;
    order_ = integerField(root.get(), "order");
    for (const auto &path : strings(field(root.get(), "cheatPaths")))
      cheatPaths_.insert(path);
    auto *patches = field(root.get(), "patches");
    if (json_object_is_type(patches, json_type_object)) {
      json_object_object_foreach(patches, id, p) {
        if (!validId(id))
          continue;
        PatchRecord r;
        r.id = id;
        r.titleId = stringField(p, "titleId");
        r.homebrewApp = stringField(p, "homebrew");
        if (!r.homebrewApp.empty() && !homebrew::appFolderAllowed(r.homebrewApp))
          continue;
        r.name = stringField(p, "name");
        r.version = stringField(p, "version");
        r.hash = stringField(p, "hash");
        r.on = json_object_get_boolean(field(p, "on"));
        r.pending = stringField(p, "pending");
        r.files = strings(field(p, "files"));
        std::sort(r.files.begin(), r.files.end()); // lists() searches it
        r.cheats = strings(field(p, "cheats"));
        r.bytes = uint64_t(std::max<int64_t>(0, integerField(p, "bytes")));
        r.order = integerField(p, "order");
        // Only folders of the known layouts; anything else moves file by file.
        for (const auto &root : strings(field(p, "roots")))
          if (wholeRoot(root + "/x/x") == root)
            r.roots.push_back(root);
        // [size, mtime] per path; one not listed or not readable is dropped.
        if (auto *marks = field(p, "marks"); json_object_is_type(marks, json_type_object)) {
          json_object_object_foreach(marks, path, pair) {
            if (!lists(r, path) || !json_object_is_type(pair, json_type_array) ||
                json_object_array_length(pair) != 2)
              continue;
            const auto size = json_object_get_int64(json_object_array_get_idx(pair, 0));
            if (size == -1)
              r.marks[path] = FileMark::foreignFile();
            else if (size >= 0)
              r.marks[path] = {uint64_t(size), json_object_get_int64(json_object_array_get_idx(pair, 1))};
          }
        }
        records_[r.id] = std::move(r);
      }
    }
  }
  // A cheat file we set aside but never recorded goes back where it was.
  // The user's other files set aside are looked at once every move is done.
  const auto baseRoot = storeRoot_ + "/_base";
  std::vector<std::string> userFiles;
  if (fs::is_directory(baseRoot, ec)) {
    std::vector<std::string> orphans;
    for (auto it = fs::recursive_directory_iterator(baseRoot, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      if (!systemJunk(it->path().filename().string()) && it->is_regular_file(ec)) {
        const auto rel = it->path().string().substr(baseRoot.size() + 1);
        if (!isCheatPath(rel))
          userFiles.push_back(rel);
        else if (!cheatPaths_.count(rel))
          orphans.push_back(rel);
      }
    for (const auto &rel : orphans) {
      fs::remove(sd(rel), ec); // our merged file, rebuilt from the patches
      if (!move(base(rel), sd(rel), error))
        return false;
    }
  }
  for (auto &[id, r] : records_) {
    if (r.pending == "enable" && !finishEnable(r, error))
      return false;
    if (r.pending == "disable" && !finishDisable(r, error))
      return false;
  }
  // One no patch covers any more goes back; one a patch covers stays aside.
  for (const auto &rel : userFiles)
    if (!place(rel, error))
      return false;
  if (!userFiles.empty() && !save(error)) // marks of any patch file placed back
    return false;
  markShadows();
  const std::vector<std::string> taken(cheatPaths_.begin(), cheatPaths_.end());
  for (const auto &path : taken)
    if (!rebuildCheat(path, error))
      return false;
  // A download cut off before commit resumes from its record; one without a
  // record cannot, and goes.
  if (fs::is_directory(storeRoot_, ec))
    for (const auto &entry : fs::directory_iterator(storeRoot_, ec)) {
      const auto name = entry.path().filename().string();
      if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0 &&
          !fs::is_regular_file(entry.path() / ResumeFile, ec))
        fs::remove_all(entry.path(), ec);
    }
  return true;
}
const PatchRecord *PatchStore::provider(const std::string &path) const {
  const PatchRecord *top = nullptr;
  for (const auto &[id, r] : records_)
    if (r.on && lists(r, path) && (!top || r.order > top->order))
      top = &r;
  return top;
}
const PatchRecord *PatchStore::holder(const std::string &path) const {
  std::error_code ec;
  if (!fs::exists(sd(path), ec))
    return nullptr;
  std::optional<FileMark> now;
  bool looked = false;
  for (const auto &[id, r] : records_) {
    if (!(r.on || !r.pending.empty()) || !lists(r, path) || fs::exists(stored(r.id, path), ec))
      continue;
    const auto marked = r.marks.find(path);
    if (marked == r.marks.end())
      return &r; // a ledger from before marks: the patch's, as it always was
    if (!looked) {
      now = fileMark(sd(path));
      looked = true;
    }
    if (now && sameFile(marked->second, *now))
      return &r;
  }
  return nullptr; // the user's: put there by hand, or changed since
}
void PatchStore::mark(const std::string &id, const std::string &path) {
  const auto it = records_.find(id);
  if (it == records_.end())
    return;
  if (const auto now = fileMark(sd(path)))
    it->second.marks[path] = *now;
  else
    it->second.marks.erase(path);
}
// A file of the user's stays where the user put it unless a patch has a copy
// to put there instead; then it goes aside under _base and comes back once no
// patch's copy needs the place.
bool PatchStore::place(const std::string &path, Error &error) {
  std::error_code ec;
  const auto *want = provider(path);
  const bool wantHas = want && fs::exists(stored(want->id, path), ec);
  if (fs::exists(sd(path), ec)) {
    const auto *have = holder(path);
    if (have == want)
      return true; // the right patch's copy, or the user's file with no patch over it
    if (!have && !wantHas) {
      // The user's file, and no patch's copy to put on top of it. Marked so
      // it never reads as the patch's.
      if (want)
        records_[want->id].marks[path] = FileMark::foreignFile();
      return true;
    }
    if (have) {
      const auto id = have->id;
      if (!move(sd(path), stored(id, path), error))
        return false;
      records_[id].marks.erase(path);
    } else {
      // The user's file goes aside. One is already there only when the user
      // put another file in place since; that one is kept apart, never lost.
      auto aside = base(path);
      for (int n = 1; fs::exists(aside, ec); ++n)
        aside = storeRoot_ + "/_kept/" + path + "." + std::to_string(n);
      if (aside != base(path))
        logLine("patches: kept a second file of the user's at " + aside);
      if (!move(sd(path), aside, error))
        return false;
    }
  }
  if (wantHas) {
    if (!move(stored(want->id, path), sd(path), error))
      return false;
    mark(want->id, path);
    return true;
  }
  if (want)
    logLine("patches: " + want->id + " is missing " + path);
  // No patch's copy to put here: the user's own file comes back, if any.
  if (fs::exists(base(path), ec)) {
    if (want)
      records_[want->id].marks[path] = FileMark::foreignFile();
    return move(base(path), sd(path), error);
  }
  prune(sd(path));
  return true;
}
void PatchStore::markShadows() {
  std::map<std::string, const PatchRecord *> top;
  for (const auto &[id, r] : records_)
    if (r.on)
      for (const auto &file : r.files) {
        auto &slot = top[file];
        if (!slot || r.order > slot->order)
          slot = &r;
      }
  for (auto &[id, r] : records_) {
    r.shadowed.clear();
    if (r.on)
      for (const auto &file : r.files)
        if (const auto *over = top[file]; over != &r)
          r.shadowed[file] = over->id;
  }
}
bool PatchStore::rebuildCheat(const std::string &path, Error &error) {
  std::vector<const PatchRecord *> owners;
  for (const auto &[id, r] : records_)
    if (r.on && std::find(r.cheats.begin(), r.cheats.end(), path) != r.cheats.end())
      owners.push_back(&r);
  std::sort(owners.begin(), owners.end(),
            [](const PatchRecord *a, const PatchRecord *b) { return a->order < b->order; });
  std::error_code ec;
  if (owners.empty()) {
    if (!cheatPaths_.count(path))
      return true;
    if (fs::exists(base(path), ec)) {
      // Forget the path first: a leftover base file is restored on load.
      cheatPaths_.erase(path);
      if (!save(error))
        return false;
      fs::remove(sd(path), ec);
      return move(base(path), sd(path), error);
    }
    fs::remove(sd(path), ec);
    prune(sd(path));
    cheatPaths_.erase(path);
    return save(error);
  }
  if (!cheatPaths_.count(path)) {
    // The user's own cheats for this build are kept and merged first.
    if (fs::exists(sd(path), ec) && !move(sd(path), base(path), error))
      return false;
    cheatPaths_.insert(path);
    if (!save(error))
      return false;
  }
  std::string merged = readFile(base(path));
  for (const auto *owner : owners) {
    if (!merged.empty() && merged.back() != '\n')
      merged += '\n';
    if (!merged.empty())
      merged += '\n';
    merged += readFile(stored(owner->id, path));
  }
  return atomicWrite(sd(path), merged, error.message);
}
std::vector<PatchConflict> PatchStore::conflicts(const std::string &id) const {
  std::vector<PatchConflict> out;
  const auto *record = find(id);
  if (!record)
    return out;
  std::error_code ec;
  for (const auto &file : record->files) {
    bool shared = false;
    for (const auto &[otherId, other] : records_)
      if (otherId != id && other.on && lists(other, file)) {
        out.push_back({file, otherId});
        shared = true;
      }
    // The user's own file: still in place, or set aside while this is on.
    if (!shared && (record->on ? fs::exists(base(file), ec) : fs::exists(sd(file), ec)))
      out.push_back({file, ""});
  }
  return out;
}
std::vector<std::string> PatchStore::wholeRoots(const PatchRecord &record, bool enabling) const {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto &file : record.files) {
    const auto root = wholeRoot(file);
    if (root.empty() || std::find(out.begin(), out.end(), root) != out.end())
      continue;
    // Never the store itself, nor a folder holding it.
    if (upper(storeRoot_ + "/").rfind(upper(sd(root) + "/"), 0) == 0)
      continue;
    if (fs::exists(enabling ? sd(root) : stored(record.id, root), ec) || fs::exists(base(root), ec))
      continue;
    bool shared = false;
    for (const auto &[id, r] : records_)
      if (id != record.id && (r.on || !r.pending.empty()) && (anyUnder(r.files, root) || anyUnder(r.cheats, root)))
        shared = true;
    if (!shared)
      out.push_back(root);
  }
  return out;
}
bool PatchStore::holdsOnly(const PatchRecord &record, const std::string &root, const std::string &dir) const {
  std::error_code ec;
  if (!fs::is_directory(dir, ec) || fs::is_symlink(dir, ec))
    return false;
  std::vector<std::string> found;
  for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec)
      return false;
    if (it->is_symlink(ec))
      return false; // a link: leave it to the file by file path
    if (systemJunk(it->path().filename().string()))
      continue; // a Mac's "._" file: neither ours nor the user's
    if (it->is_regular_file(ec))
      found.push_back(root + "/" + it->path().string().substr(dir.size() + 1));
    else if (!it->is_directory(ec))
      return false;
  }
  if (ec)
    return false;
  std::vector<std::string> mine;
  for (const auto &file : record.files)
    if (under(file, root))
      mine.push_back(file);
  std::sort(found.begin(), found.end());
  return found == mine;
}
bool PatchStore::unchanged(const PatchRecord &record, const std::string &root) const {
  for (const auto &[file, then] : record.marks)
    if (under(file, root)) {
      const auto now = fileMark(sd(file));
      if (!now || !sameFile(then, *now))
        return false;
    }
  return true;
}
bool PatchStore::placeRest(const PatchRecord &record, const std::vector<std::string> &whole, Error &error) {
  const size_t total = record.files.size();
  size_t done = 0;
  for (const auto &file : record.files)
    if (inWhole(file, whole))
      ++done;
  if (progress_)
    progress_(done, total);
  for (const auto &file : record.files) {
    if (inWhole(file, whole))
      continue;
    if (!place(file, error))
      return false;
    if (progress_)
      progress_(++done, total);
  }
  return true;
}
// Both finish steps only move files towards what the ledger says, so running
// one again after a power loss completes it. Ledgers from before layering
// left `on` as it was until the end; that is caught up here first.
//
// A folder in `roots` moves with one rename when its files are all on one
// side and nothing else is in it. A rename either happened or did not, so
// after a power loss the folder is found on one side and the check repeats.
// Otherwise its files go one by one like the rest.
bool PatchStore::finishEnable(PatchRecord &record, Error &error) {
  if (!record.on) {
    record.on = true;
    record.order = ++order_;
    if (!save(error))
      return false;
  }
  std::error_code ec;
  std::vector<std::string> whole;
  for (const auto &root : record.roots) {
    const auto from = stored(record.id, root), to = sd(root);
    if (!fs::exists(to, ec) && holdsOnly(record, root, from) && !move(from, to, error))
      return false;
    if (!fs::exists(from, ec) && fs::is_directory(to, ec))
      whole.push_back(root);
  }
  // A rename keeps each file as it was: marked here, one lookup each.
  for (const auto &file : record.files)
    if (inWhole(file, whole))
      mark(record.id, file);
  if (!placeRest(record, whole, error))
    return false;
  record.pending.clear();
  record.roots.clear();
  if (!save(error))
    return false;
  for (const auto &cheat : record.cheats)
    if (!rebuildCheat(cheat, error))
      return false;
  return true;
}
bool PatchStore::finishDisable(PatchRecord &record, Error &error) {
  if (record.on) {
    record.on = false;
    if (!save(error))
      return false;
  }
  std::error_code ec;
  std::vector<std::string> whole;
  for (const auto &root : record.roots) {
    const auto from = sd(root), to = stored(record.id, root);
    // A file the user added or replaced below the folder keeps it on the
    // card; then the patch's files leave one by one and the user's stays.
    if (!fs::exists(to, ec) && holdsOnly(record, root, from) && unchanged(record, root)) {
      if (!move(from, to, error))
        return false;
      prune(from);
    }
    if (!fs::exists(from, ec) && fs::is_directory(to, ec))
      whole.push_back(root);
  }
  if (!placeRest(record, whole, error))
    return false;
  record.pending.clear();
  record.roots.clear();
  // Off, it has nothing on the card. A path whose copy it no longer has (the
  // user wrote over it) is marked foreign, so whatever is there later never
  // reads as its own, not even when it goes on again.
  record.marks.clear();
  for (const auto &file : record.files)
    if (!fs::exists(stored(record.id, file), ec))
      record.marks[file] = FileMark::foreignFile();
  if (!save(error))
    return false;
  for (const auto &cheat : record.cheats)
    if (!rebuildCheat(cheat, error))
      return false;
  return true;
}
bool PatchStore::enable(const std::string &id, Error &error) {
  const auto it = records_.find(id);
  if (it == records_.end()) {
    error.message = tr("This patch has not been downloaded.");
    return false;
  }
  if (it->second.on)
    return true;
  return turnOn(it->second, ++order_, error);
}
// On and its place in the order are written with the pending mark, so a move
// cut short finishes the same way on the next load.
bool PatchStore::turnOn(PatchRecord &record, int64_t order, Error &error) {
  record.on = true;
  record.order = order;
  record.pending = "enable";
  record.roots = wholeRoots(record, true);
  return save(error) && finishEnable(record, error);
}
bool PatchStore::disable(const std::string &id, Error &error) {
  const auto it = records_.find(id);
  if (it == records_.end() || !it->second.on)
    return true;
  it->second.on = false;
  it->second.pending = "disable";
  it->second.roots = wholeRoots(it->second, false);
  return save(error) && finishDisable(it->second, error);
}
bool PatchStore::remove(const std::string &id, Error &error) {
  if (!validId(id) || !disable(id, error))
    return false;
  std::error_code ec;
  fs::remove_all(storeRoot_ + "/" + id, ec);
  records_.erase(id);
  return save(error);
}
bool PatchStore::reapply(const std::string &id, size_t &left, Error &error) {
  left = 0;
  const auto it = records_.find(id);
  if (it == records_.end() || !it->second.on)
    return true;
  auto &record = it->second;
  std::error_code ec;
  for (const auto &file : record.files) {
    if (provider(file) != &record || fs::exists(sd(file), ec))
      continue;
    if (!fs::exists(stored(id, file), ec)) {
      ++left;
      continue;
    }
    if (!place(file, error))
      return false;
  }
  if (!save(error))
    return false;
  for (const auto &cheat : record.cheats)
    if (!rebuildCheat(cheat, error))
      return false;
  return true;
}
bool PatchStore::commit(const UserPatch &patch, Error &error) {
  const bool app = !patch.homebrewApp.empty();
  if (!validId(patch.id) || (app ? !homebrew::appFolderAllowed(patch.homebrewApp) : patch.titleId.size() != 16)) {
    error.message = tr("The patch information is invalid.");
    return false;
  }
  PatchRecord record;
  record.id = patch.id;
  record.titleId = patch.titleId;
  record.homebrewApp = patch.homebrewApp;
  record.name = patch.name;
  record.version = patch.version;
  record.hash = patch.hash;
  const auto from = staging(patch.id);
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(from, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec))
      continue;
    const auto rel = it->path().string().substr(from.size() + 1);
    if (!patchPathAllowed(patch, rel)) {
      error.message = trf("The patch tries to write to a disallowed location: {0}", {rel});
      return false;
    }
    (isCheatPath(rel) ? record.cheats : record.files).push_back(rel);
    record.bytes += it->file_size(ec);
  }
  if (ec || (record.files.empty() && record.cheats.empty())) {
    error.message = tr("The patch contains no files.");
    return false;
  }
  std::sort(record.files.begin(), record.files.end());
  std::sort(record.cheats.begin(), record.cheats.end());
  bool wasOn = false;
  int64_t order = 0;
  if (const auto *old = find(patch.id)) {
    wasOn = old->on;
    order = old->order;
    if (!remove(patch.id, error))
      return false;
  }
  fs::remove_all(storeRoot_ + "/" + patch.id, ec);
  if (!move(from, storeRoot_ + "/" + patch.id, error))
    return false;
  auto &fresh = records_[patch.id] = std::move(record);
  if (!save(error))
    return false;
  // Back on where it was in the order: patches over it stay over it.
  return !wasOn || turnOn(fresh, order, error);
}

PatchChecker::PatchChecker(std::string sdRoot, std::string storeRoot, const std::map<std::string, PatchRecord> &records)
    : sdRoot_(std::move(sdRoot)), storeRoot_(std::move(storeRoot)) {
  // Only what the ledger says is on the card: each enabled patch's files
  // that no patch turned on later covers. Cheats are rebuilt, never checked.
  for (const auto &[id, r] : records) {
    if (!r.on || !r.pending.empty())
      continue;
    for (const auto &file : r.files) {
      if (r.shadowed.count(file))
        continue;
      Item item{id, file, std::nullopt};
      if (const auto marked = r.marks.find(file); marked != r.marks.end())
        item.mark = marked->second;
      work_.push_back(std::move(item));
    }
  }
}
bool PatchChecker::run(size_t maxLooks, std::chrono::milliseconds budget, const std::atomic<bool> *stop) {
  const auto until = std::chrono::steady_clock::now() + budget;
  const size_t limit = looks_ + maxLooks;
  std::error_code ec;
  while (next_ < work_.size()) {
    if ((stop && stop->load()) || looks_ >= limit || std::chrono::steady_clock::now() >= until)
      return false;
    const auto &item = work_[next_];
    auto &health = health_[item.id];
    // A folder that moves whole is looked at once; with it gone, so are all
    // its files.
    const auto root = wholeRoot(item.path);
    if (!root.empty()) {
      auto card = cardRoots_.find(root);
      if (card == cardRoots_.end()) {
        ++looks_;
        card = cardRoots_.emplace(root, fs::is_directory(sdRoot_ + "/" + root, ec)).first;
      }
      if (!card->second) {
        ++health.missing;
        ++next_;
        continue;
      }
    }
    ++looks_;
    if (const auto now = fileMark(sdRoot_ + "/" + item.path)) {
      if (item.mark && !sameFile(*item.mark, *now))
        ++health.changed;
    } else
      ++health.missing;
    ++next_;
  }
  return true;
}
} // namespace foil
