#include <cerrno>
#include "core/Log.hpp"
#include "core/Patches.hpp"
#include "core/Homebrew.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
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
} // namespace

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
bool isCheatPath(const std::string &path) {
  const auto parts = split(path);
  return parts.size() == 5 && same(parts[0], "atmosphere") && same(parts[1], "contents") &&
         same(parts[3], "cheats") && parts[4].size() > 4 &&
         same(parts[4].substr(parts[4].size() - 4), ".txt");
}
const char *patchKindLabel(const std::string &kind) {
  if (kind == "romfs")
    return tr("데이터 패치");
  if (kind == "exefs")
    return tr("실행 파일 패치");
  if (kind == "cheats")
    return tr("치트");
  if (kind == "exefs_patches")
    return tr("IPS 패치");
  if (kind == "fpslocker")
    return "FPSLocker";
  if (kind == "homebrew")
    return tr("홈브류");
  return tr("기타");
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
bool TarWriter::openFile(const std::string &path) {
  const fs::path full = root_ + "/" + path;
  std::error_code ec;
  makeDirectories(std::filesystem::path(full.parent_path()).string());
  file_ = std::fopen(full.string().c_str(), "wb");
  writeBuffer(file_);
  if (!file_) {
    // The system's reason goes on screen too: on the console log.txt is out
    // of reach while the app runs.
    const std::string reason = std::strerror(errno);
    const bool folder = !std::filesystem::is_directory(full.parent_path(), ec);
    logLine("tar: cannot create " + full.string() + ": " + reason + (folder ? " (no folder)" : ""));
    return fail(trf("파일을 만들지 못했습니다: {0}", {path}) + " (" + reason + (folder ? ", no folder" : "") + ")");
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
    return fail(tr("패치 파일 형식이 아닙니다."));
  unsigned sum = 0;
  for (size_t i = 0; i < Block; ++i)
    sum += (i >= 148 && i < 156) ? unsigned(' ') : unsigned(static_cast<unsigned char>(b[i]));
  if (octal(b + 148, 8) != sum)
    return fail(tr("받은 패치 파일이 손상되었습니다."));
  const uint64_t size = octal(b + 124, 12);
  if (size == UINT64_MAX)
    return fail(tr("받은 패치 파일이 손상되었습니다."));
  const char type = b[156];
  auto name = text(b, 100);
  const auto prefix = text(b + 345, 155);
  if (!prefix.empty())
    name = prefix + "/" + name;
  if (!paxPath_.empty() && type != 'x' && type != 'g')
    name = std::exchange(paxPath_, "");
  remaining_ = size;
  padding_ = padding(size);
  if (type == 'x' || type == 'g') {
    if (size > MaxPaxBytes)
      return fail(tr("패치 파일의 항목 정보가 너무 깁니다."));
    pax_ = true;
    paxGlobal_ = type == 'g';
    paxData_.clear();
    return true;
  }
  if (type == '5') { // folders are created with their files
    if (size)
      return fail(tr("받은 패치 파일이 손상되었습니다."));
    return true;
  }
  if (type != '0' && type != '\0' && type != '7')
    return fail(trf("패치에 일반 파일이 아닌 항목이 있습니다: {0}", {name}));
  while (name.rfind("./", 0) == 0)
    name.erase(0, 2);
  if (allow_ ? !allow_(name) : !patchPathAllowed(titleId_, name))
    return fail(trf("패치가 허용되지 않은 위치에 쓰려고 합니다: {0}", {name}));
  if (skip_ && skip_(name))
    return true; // its bytes pass by with no file open
  if (!openFile(name))
    return false;
  if (size == 0) {
    std::fclose(file_);
    file_ = nullptr;
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
    at += length;
  }
}
bool TarWriter::feed(const char *data, size_t size) {
  if (!error_.empty())
    return false;
  while (size > 0) {
    if (remaining_ > 0) {
      const auto take = size_t(std::min<uint64_t>(remaining_, size));
      if (pax_)
        paxData_.append(data, take);
      else if (file_ && std::fwrite(data, 1, take, file_) != take)
        return fail(tr("SD 카드에 쓰지 못했습니다. 남은 공간을 확인합니다."));
      remaining_ -= take;
      data += take;
      size -= take;
      if (remaining_ == 0) {
        if (pax_) {
          readPax();
          pax_ = false;
        } else if (file_) {
          const bool closed = std::fclose(file_) == 0;
          file_ = nullptr;
          if (!closed)
            return fail(tr("SD 카드에 쓰지 못했습니다. 남은 공간을 확인합니다."));
        }
      }
      continue;
    }
    if (padding_ > 0) {
      const auto take = size_t(std::min<uint64_t>(padding_, size));
      padding_ -= take;
      data += take;
      size -= take;
      continue;
    }
    if (ended_)
      return true; // trailing zero blocks after the end marker
    const auto take = std::min(Block - block_.size(), size);
    block_.append(data, take);
    data += take;
    size -= take;
    if (block_.size() == Block) {
      if (!header())
        return false;
      block_.clear();
    }
  }
  return true;
}
bool TarWriter::finish() {
  if (!error_.empty())
    return false;
  if (!ended_ || remaining_ > 0)
    return fail(tr("패치를 받다가 끊겼습니다. 다시 받습니다."));
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
    error.message = tr("패치 파일을 옮기지 못했습니다. SD 카드를 확인합니다.");
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
        r.cheats = strings(field(p, "cheats"));
        r.bytes = uint64_t(std::max<int64_t>(0, integerField(p, "bytes")));
        r.order = integerField(p, "order");
        records_[r.id] = std::move(r);
      }
    }
  }
  // A cheat file we set aside but never recorded goes back where it was;
  // an app's file set aside for a patch that is on, or half way, stays.
  const auto baseRoot = storeRoot_ + "/_base";
  if (fs::is_directory(baseRoot, ec)) {
    std::vector<std::string> orphans;
    for (auto it = fs::recursive_directory_iterator(baseRoot, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_regular_file(ec)) {
        const auto rel = it->path().string().substr(baseRoot.size() + 1);
        if (!cheatPaths_.count(rel) && !setAside(rel))
          orphans.push_back(rel);
      }
    for (const auto &rel : orphans) {
      fs::remove(sd(rel), ec);
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
  const std::vector<std::string> taken(cheatPaths_.begin(), cheatPaths_.end());
  for (const auto &path : taken)
    if (!rebuildCheat(path, error))
      return false;
  // Downloads cut off before commit start over.
  if (fs::is_directory(storeRoot_, ec))
    for (const auto &entry : fs::directory_iterator(storeRoot_, ec)) {
      const auto name = entry.path().filename().string();
      if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0)
        fs::remove_all(entry.path(), ec);
    }
  return true;
}
bool PatchStore::setAside(const std::string &path) const {
  for (const auto &[id, r] : records_)
    if (!r.homebrewApp.empty() && (r.on || !r.pending.empty()) &&
        std::find(r.files.begin(), r.files.end(), path) != r.files.end())
      return true;
  return false;
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
  if (!record || record->on)
    return out;
  std::error_code ec;
  for (const auto &file : record->files) {
    if (!fs::exists(sd(file), ec))
      continue;
    std::string owner;
    for (const auto &[otherId, other] : records_)
      if (other.on && std::find(other.files.begin(), other.files.end(), file) != other.files.end())
        owner = otherId;
    // An app's own file is set aside on enable, so only another patch clashes.
    if (owner.empty() && !record->homebrewApp.empty())
      continue;
    out.push_back({file, owner});
  }
  return out;
}
bool PatchStore::finishEnable(PatchRecord &record, Error &error) {
  std::error_code ec;
  for (const auto &file : record.files) {
    if (!fs::exists(stored(record.id, file), ec))
      continue;
    // An app patch sets the app's own file aside first.
    if (!record.homebrewApp.empty() && fs::exists(sd(file), ec) && !fs::exists(base(file), ec) &&
        !move(sd(file), base(file), error))
      return false;
    if (!fs::exists(sd(file), ec) && !move(stored(record.id, file), sd(file), error))
      return false;
  }
  record.on = true;
  record.order = ++order_;
  record.pending.clear();
  if (!save(error))
    return false;
  for (const auto &cheat : record.cheats)
    if (!rebuildCheat(cheat, error))
      return false;
  return true;
}
bool PatchStore::finishDisable(PatchRecord &record, Error &error) {
  std::error_code ec;
  for (const auto &file : record.files)
    if (fs::exists(sd(file), ec) && !fs::exists(stored(record.id, file), ec)) {
      if (!move(sd(file), stored(record.id, file), error))
        return false;
      prune(sd(file));
    }
  // What an app patch set aside goes back.
  for (const auto &file : record.files)
    if (!record.homebrewApp.empty() && fs::exists(base(file), ec) && !fs::exists(sd(file), ec) &&
        !move(base(file), sd(file), error))
      return false;
  record.on = false;
  record.pending.clear();
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
    error.message = tr("받아 둔 패치가 아닙니다.");
    return false;
  }
  if (it->second.on)
    return true;
  if (!conflicts(id).empty()) {
    error.message = tr("다른 패치나 직접 넣은 파일과 겹칩니다.");
    return false;
  }
  it->second.pending = "enable";
  return save(error) && finishEnable(it->second, error);
}
bool PatchStore::disable(const std::string &id, Error &error) {
  const auto it = records_.find(id);
  if (it == records_.end() || !it->second.on)
    return true;
  it->second.pending = "disable";
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
bool PatchStore::commit(const UserPatch &patch, Error &error) {
  const bool app = !patch.homebrewApp.empty();
  if (!validId(patch.id) || (app ? !homebrew::appFolderAllowed(patch.homebrewApp) : patch.titleId.size() != 16)) {
    error.message = tr("패치 정보가 잘못되었습니다.");
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
      error.message = trf("패치가 허용되지 않은 위치에 쓰려고 합니다: {0}", {rel});
      return false;
    }
    (isCheatPath(rel) ? record.cheats : record.files).push_back(rel);
    record.bytes += it->file_size(ec);
  }
  if (ec || (record.files.empty() && record.cheats.empty())) {
    error.message = tr("패치에 들어 있는 파일이 없습니다.");
    return false;
  }
  std::sort(record.files.begin(), record.files.end());
  std::sort(record.cheats.begin(), record.cheats.end());
  bool wasOn = false;
  if (const auto *old = find(patch.id)) {
    wasOn = old->on;
    if (!remove(patch.id, error))
      return false;
  }
  fs::remove_all(storeRoot_ + "/" + patch.id, ec);
  if (!move(from, storeRoot_ + "/" + patch.id, error))
    return false;
  records_[patch.id] = std::move(record);
  if (!save(error))
    return false;
  if (wasOn && conflicts(patch.id).empty())
    return enable(patch.id, error);
  return true;
}
} // namespace foil
