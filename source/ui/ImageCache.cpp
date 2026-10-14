// 설정's 이미지 캐시 비우기 row: A opens a popup with how much of its room the
// pictures on the card take, and empties it. The size is a running count
// (see imageCacheTally); only emptying it walks the folder, off the drawing thread.
#include "ui/Application.hpp"
#include "ui/Images.hpp"
#include <filesystem>
namespace foil::ui {
std::string Application::imageCacheFolder() const {
  if (!cacheFolder_.empty()) return cacheFolder_;
  return statePath_.empty() ? "" : std::filesystem::path(statePath_).parent_path().string() + "/cache";
}
void Application::measureImageCache(bool clear) {
  const auto folder = imageCacheFolder();
  if (cacheBusy_) return;
  if (folder.empty()) { // no cache folder (a preview without state): nothing kept
    cacheBytes_ = 0;
    return;
  }
  // Counted as the pictures come and go: nothing to walk to show it.
  if (!clear) {
    cacheBytes_ = imageCacheTally();
    return;
  }
  if (cacheWorker_.joinable()) cacheWorker_.join();
  cacheBusy_ = true;
  cacheWorker_ = std::thread([this, folder] {
    clearImageCache(folder);
    cacheBytes_ = imageCacheTally();
    cacheCleared_ = true;
    cacheBusy_ = false;
  });
}
void Application::imageCacheAction(Action a) {
  if (a == Action::Back) cacheClearAsk_ = false;
  else if (a == Action::Accept && settled()) {
    cacheClearAsk_ = false;
    measureImageCache(true);
  }
}
void Application::imageCacheDialog() {
  const auto body = modal(700, 24 + 30 + 14 + 10 + 18 + 48 + 16, tr("Delete the image cache?"), tr("Cancel"), "",
                          tr("Delete"), 41);
  const float x = body.x + 32, y = body.y + 24, w = body.w - 64;
  if (!cacheBusy_) cacheBytes_ = imageCacheTally(); // it grows as pictures come
  const uint64_t bytes = uint64_t(std::max<int64_t>(0, cacheBytes_)), room = imageCacheLimit(sdFree_);
  // The size large, then a bar of the room it may take, then what stays.
  const auto size = cacheBytes_ < 0 ? std::string(tr("Checking|size")) : formatBytes(bytes);
  paint_.fonts.middle(size, x, y + 15, 26, Ink, true);
  paint_.fonts.middle("/ " + formatBytes(room), x + paint_.fonts.measure(size, 26, true) + 10, y + 17, 18, Secondary);
  paint_.rect(x, y + 44, w, 10, Line, 5);
  if (bytes > 0)
    paint_.rect(x, y + 44, std::max(10.f, w * std::min(1.f, float(bytes) / float(std::max<uint64_t>(room, 1)))), 10,
                Butter, 5);
  paint_.fonts.wrapped(tr("Games and download records stay. Pictures on screen are fetched again."), x, y + 72, 18,
                       Secondary, w, 2);
}
} // namespace foil::ui
