// 설정's 이미지 캐시 row: how much of its room the pictures on the card take,
// and a way to empty it (asked first). Measured off the drawing thread each
// time 설정 opens, since a full folder takes a moment to walk.
#include "ui/Application.hpp"
#include "ui/Images.hpp"
#include <filesystem>
namespace foil::ui {
std::string Application::imageCacheFolder() const {
  return statePath_.empty() ? "" : std::filesystem::path(statePath_).parent_path().string() + "/cache";
}
void Application::measureImageCache(bool clear) {
  const auto folder = imageCacheFolder();
  if (cacheBusy_) return;
  if (folder.empty()) { // no cache folder (a preview without state): nothing kept
    cacheBytes_ = 0;
    return;
  }
  if (cacheWorker_.joinable()) cacheWorker_.join();
  cacheBusy_ = true;
  cacheWorker_ = std::thread([this, folder, clear] {
    if (clear) clearImageCache(folder);
    cacheBytes_ = int64_t(imageCacheBytes(folder));
    cacheCleared_ = clear;
    cacheBusy_ = false;
  });
}
// The value side of the row: what the cache is for, a bar of the room it may
// take filled as far as it is, and the size in a pill ("42 MB / 128 MB").
void Application::imageCacheValue(float right, float mid) {
  const int64_t bytes = cacheBytes_;
  const uint64_t room = imageCacheLimit(sdFree_);
  const std::string label = bytes < 0 ? std::string(tr("Checking|size"))
                                      : formatBytes(uint64_t(bytes)) + " / " + formatBytes(room);
  const float textW = paint_.fonts.measure(label, 17, true), pillW = textW + 24, pillX = right - pillW;
  paint_.rect(pillX, mid - 15, pillW, 30, Ground, 15);
  paint_.fonts.middle(label, pillX + 12, mid, 17, bytes < 0 ? Secondary : Ink, true);
  constexpr float Bar = 150, Track = 8;
  const float barX = pillX - 16 - Bar;
  paint_.rect(barX, mid - Track / 2, Bar, Track, Line, Track / 2);
  if (bytes > 0) {
    const float used = std::min(1.f, float(bytes) / float(std::max<uint64_t>(room, 1)));
    paint_.rect(barX, mid - Track / 2, std::max(Track, Bar * used), Track, Butter, Track / 2);
  }
  const auto why = tr("Saves covers and screenshots for next time");
  paint_.fonts.middle(why, barX - 20 - paint_.fonts.measure(why, 17), mid, 17, Secondary);
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
  const uint64_t bytes = uint64_t(std::max<int64_t>(0, cacheBytes_)), room = imageCacheLimit(sdFree_);
  // The size large, then the same bar as the row, then what stays.
  const auto size = formatBytes(bytes);
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
