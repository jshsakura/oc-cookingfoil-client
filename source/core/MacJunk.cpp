#include "core/MacJunk.hpp"
#include "platform/FileSystem.hpp"
#include <filesystem>
#include <set>
namespace foil {
bool macJunk(const std::string &name, bool directory, bool atTop) {
  if (directory)
    return atTop && (name == ".Spotlight-V100" || name == ".Trashes" || name == ".fseventsd" ||
                     name == ".TemporaryItems" || name == ".DocumentRevisions-V100");
  return name.rfind("._", 0) == 0 || name == ".DS_Store" || (atTop && (name == ".apdisk" || name == ".VolumeIcon.icns"));
}
JunkSweep sweepMacJunk(const std::string &sd, const std::vector<std::string> &folders) {
  namespace fs = std::filesystem;
  JunkSweep out;
  if (sd.empty()) return out;
  const fs::path top(sd);
  std::set<fs::path> seen;
  const auto sweep = [&](const fs::path &folder) {
    const auto clean = folder.lexically_normal();
    // Only folders on the card: a path from the PC that climbs out is skipped.
    const auto inside = clean.lexically_relative(top.lexically_normal());
    if (inside.empty() || *inside.begin() == ".." || !seen.insert(clean).second) return;
    const bool atTop = inside == ".";
    std::error_code ec;
    std::vector<fs::directory_entry> found;
    for (fs::directory_iterator it(clean, ec), end; !ec && it != end; it.increment(ec)) found.push_back(*it);
    for (const auto &entry : found) {
      std::error_code kind;
      if (entry.is_symlink(kind)) continue;
      const bool directory = entry.is_directory(kind);
      if (kind || !macJunk(entry.path().filename().string(), directory, atTop)) continue;
      std::error_code size;
      const uint64_t bytes = directory ? 0 : entry.file_size(size);
      std::error_code removed;
      if (platform::removePath(entry.path().string(), removed)) {
        ++out.removed;
        if (!size) out.bytes += bytes;
      } else {
        ++out.failed;
      }
    }
  };
  sweep(top);
  for (const auto &folder : folders) sweep(top / fs::path(folder).relative_path());
  return out;
}
} // namespace foil
