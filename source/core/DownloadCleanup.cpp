#include "core/DownloadCleanup.hpp"
#include "core/Resume.hpp"
#include <filesystem>
namespace foil {
namespace {
bool endsWith(const std::string &text, const std::string &end) {
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}
}
std::vector<std::pair<std::string, uint64_t>> downloadPieces(const std::string &sd) {
  namespace fs = std::filesystem;
  std::vector<std::pair<std::string, uint64_t>> out;
  const auto patches = fs::path(sd) / "switch/cookingfoil/patches";
  for (const char *root : {"switch", "retroarch"}) {
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(sd) / root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (it->is_symlink(ec)) continue;
      if (it->is_directory(ec)) {
        if (endsWith(name, ".part")) {
          it.disable_recursion_pending();
          if (it->path().parent_path() != patches) continue;
          uint64_t bytes = 0;
          std::error_code scan;
          for (fs::recursive_directory_iterator file(it->path(), scan), stop; !scan && file != stop; file.increment(scan))
            if (!file->is_symlink(scan) && file->is_regular_file(scan)) {
              const auto size = file->file_size(scan);
              if (!scan) bytes += size;
            }
          if (!scan) out.push_back({it->path().string(), bytes});
        }
        continue;
      }
      const bool record = it->path().parent_path().filename() == "resume" && name.rfind("homebrew-", 0) == 0 &&
                          endsWith(name, ".json");
      if (!endsWith(name, ".part") && !endsWith(name, std::string(".part") + ResumeFile) &&
          !endsWith(name, std::string(".part") + PlanFile) && !record) continue;
      std::error_code size;
      const auto bytes = it->file_size(size);
      out.push_back({it->path().string(), size ? 0 : uint64_t(bytes)});
    }
  }
  return out;
}
}
