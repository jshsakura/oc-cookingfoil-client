// Where the pictures are kept on the card; 파일 정리 empties it (FileCleanup.cpp).
#include "ui/Application.hpp"
#include <filesystem>
namespace foil::ui {
std::string Application::imageCacheFolder() const {
  if (!cacheFolder_.empty()) return cacheFolder_;
  return statePath_.empty() ? "" : std::filesystem::path(statePath_).parent_path().string() + "/cache";
}
} // namespace foil::ui
