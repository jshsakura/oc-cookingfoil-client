#include "core/Json.hpp"
#include "core/HomebrewCard.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
namespace foil::homebrew {
namespace {
// The .nro layout: a 0x10 byte start, then the "NRO0" header whose size
// field says where the asset section begins.
constexpr size_t NroHeaderEnd = 0x80, NroMagicAt = 0x10, NroSizeAt = 0x18;
// The asset header: "ASET", a version, then offset and size pairs (icon,
// NACP, RomFS) counted from the asset header itself.
constexpr size_t AssetHeaderSize = 0x38, AssetIconAt = 0x08, AssetNacpAt = 0x18;
// The NACP: 16 title entries (name 0x200, author 0x100), the display
// version at 0x3060.
constexpr size_t NacpSize = 0x4000, NacpEntry = 0x300, NacpName = 0x200, NacpAuthor = 0x100,
                 NacpVersionAt = 0x3060, NacpVersion = 0x10, NacpEntries = 16;
// Icons are 256x256 JPEGs, far below this.
constexpr uint64_t MaxIcon = 512 * 1024;
// Directory entries looked at in switch/ and in each folder under it.
constexpr size_t MaxEntries = 4096;
uint32_t u32(const std::string &b, size_t at) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = v << 8 | uint8_t(b[at + i]);
  return v;
}
uint64_t u64(const std::string &b, size_t at) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = v << 8 | uint8_t(b[at + i]);
  return v;
}
// A NUL padded field, cut at the first NUL.
std::string text(const std::string &b, size_t at, size_t size) {
  const auto field = b.substr(at, size);
  return field.substr(0, field.find('\0'));
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
// `offset` + `size` inside `limit`, with no overflow.
bool fits(uint64_t offset, uint64_t size, uint64_t limit) { return offset <= limit && size <= limit - offset; }
std::string bare(const std::string &version) {
  auto v = version;
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
  if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) v.erase(v.begin());
  return v;
}
} // namespace
std::vector<int> nacpLanguages(const std::string &code) {
  // Entry indexes: 0 American English, 1 British English, 2 Japanese,
  // 12 Korean, 13 Traditional Chinese, 14 Simplified Chinese.
  std::vector<int> order;
  if (code == "ko") order = {12};
  else if (code == "ja") order = {2};
  else if (code == "zh") order = {14, 13};
  order.push_back(0);
  order.push_back(1);
  return order;
}
bool readNro(uint64_t fileSize, const NroReader &read, const std::vector<int> &languages, NroInfo &out) {
  out = {};
  std::string header;
  if (fileSize < NroHeaderEnd || !read(0, NroHeaderEnd, header) || header.size() != NroHeaderEnd ||
      header.compare(NroMagicAt, 4, "NRO0") != 0)
    return false;
  // From here on a broken asset section only means nothing more to show.
  const uint64_t assets = u32(header, NroSizeAt);
  std::string asset;
  if (assets < NroHeaderEnd || !fits(assets, AssetHeaderSize, fileSize) || !read(assets, AssetHeaderSize, asset) ||
      asset.size() != AssetHeaderSize || asset.compare(0, 4, "ASET") != 0) {
    out.iconProblem = "no asset section";
    return true;
  }
  const uint64_t room = fileSize - assets;
  const uint64_t nacpAt = u64(asset, AssetNacpAt), nacpSize = u64(asset, AssetNacpAt + 8);
  std::string nacp;
  if (nacpSize >= NacpVersionAt + NacpVersion && nacpSize <= NacpSize && fits(nacpAt, nacpSize, room) &&
      read(assets + nacpAt, size_t(nacpSize), nacp) && nacp.size() == nacpSize) {
    const auto entry = [&](int i) { return text(nacp, size_t(i) * NacpEntry, NacpName); };
    for (const int i : languages)
      if (i >= 0 && i < int(NacpEntries) && !(out.name = entry(i)).empty()) {
        out.author = text(nacp, size_t(i) * NacpEntry + NacpName, NacpAuthor);
        break;
      }
    for (int i = 0; i < int(NacpEntries) && out.name.empty(); ++i)
      if (!(out.name = entry(i)).empty()) out.author = text(nacp, size_t(i) * NacpEntry + NacpName, NacpAuthor);
    out.version = text(nacp, NacpVersionAt, NacpVersion);
    if (nacp.size() == NacpSize) out.nacp = std::move(nacp);
  }
  const uint64_t iconAt = u64(asset, AssetIconAt), iconSize = u64(asset, AssetIconAt + 8);
  std::string icon;
  const auto jpeg = [&] { return uint8_t(icon[0]) == 0xFF && uint8_t(icon[1]) == 0xD8; };
  const auto png = [&] { return icon.compare(0, 4, "\x89PNG") == 0; };
  if (iconSize == 0)
    out.iconProblem = "no icon in the asset section";
  else if (iconSize < 4 || iconSize > MaxIcon || !fits(iconAt, iconSize, room))
    out.iconProblem = "icon at " + std::to_string(iconAt) + " size " + std::to_string(iconSize) + " outside the file";
  else if (!read(assets + iconAt, size_t(iconSize), icon) || icon.size() != iconSize)
    out.iconProblem = "icon unreadable";
  else if (!jpeg() && !png())
    out.iconProblem = "icon is no JPEG or PNG";
  else
    out.icon = std::move(icon);
  return true;
}
bool parseNro(const std::string &bytes, const std::vector<int> &languages, NroInfo &out) {
  return readNro(bytes.size(), [&](uint64_t offset, size_t size, std::string &part) {
    if (!fits(offset, size, bytes.size())) return false;
    part = bytes.substr(size_t(offset), size);
    return true;
  }, languages, out);
}
bool readNroFile(const std::string &path, const std::vector<int> &languages, NroInfo &out, uint64_t *size) {
  std::ifstream file(path, std::ios::binary);
  std::error_code sizeError;
  const auto bytes = std::filesystem::file_size(path, sizeError);
  if (!file || sizeError) return false;
  if (size) *size = bytes;
  return readNro(bytes, [&](uint64_t offset, size_t length, std::string &part) {
    part.assign(length, '\0');
    file.clear();
    file.seekg(std::streamoff(offset));
    file.read(part.data(), std::streamsize(length));
    return bool(file) && size_t(file.gcount()) == length;
  }, languages, out);
}
bool cardNroPath(const std::string &path) {
  if (path.rfind("switch/", 0) != 0 || path.find_first_of("\\:") != std::string::npos) return false;
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= path.size()) {
    const auto end = std::min(path.find('/', start), path.size());
    parts.push_back(path.substr(start, end - start));
    start = end + 1;
  }
  if (parts.size() != 2 && parts.size() != 3) return false;
  for (const auto &part : parts)
    if (part.empty() || part == "." || part == "..") return false;
  const auto file = lower(parts.back());
  if (file.size() <= 4 || file.compare(file.size() - 4, 4, ".nro") != 0 || file == "hbmenu.nro") return false;
  return parts.size() == 2 || lower(parts[1]) != "cookingfoil";
}
std::vector<std::string> cardNroPaths(std::vector<std::string> paths) {
  paths.erase(std::remove_if(paths.begin(), paths.end(), [](const std::string &p) { return !cardNroPath(p); }),
              paths.end());
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  if (paths.size() > MaxCardNros) paths.resize(MaxCardNros);
  return paths;
}
std::vector<CardNro> scanCard(const std::string &sdRoot, const std::vector<int> &languages,
                              const std::vector<CardNro> &known) {
  namespace fs = std::filesystem;
  std::vector<std::string> paths;
  std::error_code ec;
  const auto list = [&](const fs::path &folder, const std::string &prefix, bool descend) {
    size_t seen = 0;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end && seen < MaxEntries; it.increment(ec), ++seen) {
      std::error_code typeError;
      const auto name = it->path().filename().string();
      if (systemJunk(name)) continue; // a Mac's "._app.nro" is no app
      if (it->is_regular_file(typeError)) paths.push_back(prefix + name);
      else if (descend && it->is_directory(typeError)) {
        std::error_code inner;
        size_t count = 0;
        for (fs::directory_iterator sub(it->path(), inner), subEnd; !inner && sub != subEnd && count < MaxEntries;
             sub.increment(inner), ++count) {
          std::error_code subType;
          const auto file = sub->path().filename().string();
          if (!systemJunk(file) && sub->is_regular_file(subType)) paths.push_back(prefix + name + "/" + file);
        }
      }
    }
  };
  list(fs::path(sdRoot) / "switch", "switch/", true);
  std::vector<CardNro> out;
  for (const auto &path : cardNroPaths(paths)) {
    const auto full = sdRoot + "/" + path;
    struct stat st {};
    const int64_t mtime = ::stat(full.c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0;
    const auto before = std::find_if(known.begin(), known.end(), [&](const CardNro &n) { return n.path == path; });
    if (before != known.end() && mtime && before->mtime == mtime && before->size == uint64_t(st.st_size)) {
      out.push_back(*before);
      continue;
    }
    NroInfo info;
    uint64_t size = 0;
    if (!readNroFile(full, languages, info, &size)) continue;
    if (info.icon.empty())
      logLine("homebrew card: " + path + " shows no icon: " + (info.iconProblem.empty() ? "unknown" : info.iconProblem));
    out.push_back({path, std::move(info.name), std::move(info.author), std::move(info.version), std::move(info.icon), size, mtime});
  }
  return out;
}
const App *matchCard(const std::vector<App> &apps, const std::string &path) {
  const auto at = lower(path);
  const auto folder = at.substr(0, at.rfind('/'));
  for (const auto &app : apps)
    if (!app.title && lower(app.installPath) == at) return &app;
  if (folder != "switch")
    for (const auto &app : apps) {
      if (app.title) continue; // a title has nothing on the card
      const auto installFolder = lower(app.installPath.substr(0, app.installPath.rfind('/')));
      if ((!app.app.empty() && folder == "switch/" + lower(app.app)) || folder == installFolder) return &app;
    }
  for (const auto &app : apps)
    if (!app.title && !app.fileName.empty() && at == "switch/" + lower(app.fileName)) return &app;
  return nullptr;
}
std::string tileIcon(const App &app, bool onCard, const std::string &cardKey) {
  return onCard || app.cardOnly ? cardKey : app.iconUrl;
}
Status cardStatus(const std::string &appVersion, const std::string &cardVersion) {
  const auto a = bare(appVersion), c = bare(cardVersion);
  return a.empty() || c.empty() || a == c ? Status::Installed : Status::Update;
}
void removeEmptyFolders(const std::string &sdRoot, const std::string &path) {
  std::error_code ec;
  for (auto folder = std::filesystem::path(path).parent_path(); folder.has_parent_path() && folder != "switch";
       folder = folder.parent_path()) {
    const auto full = sdRoot + "/" + folder.string();
    if (!std::filesystem::is_empty(full, ec) || ec) break;
    std::filesystem::remove(full, ec);
  }
}
bool removeCardFile(const std::string &sdRoot, const std::string &path, std::string &error) {
  if (!cardNroPath(path)) {
    error = path;
    return false;
  }
  const auto full = sdRoot + "/" + path;
  std::error_code ec;
  std::filesystem::remove(full, ec);
  if (ec && std::filesystem::exists(full)) {
    error = ec.message();
    return false;
  }
  removeEmptyFolders(sdRoot, path);
  return true;
}
} // namespace foil::homebrew
