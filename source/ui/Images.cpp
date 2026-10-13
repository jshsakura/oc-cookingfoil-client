#include "ui/Images.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include "core/Log.hpp"
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
namespace foil::ui {
namespace {
constexpr int MaxImageSide = 1280;
constexpr int Workers = 4;
constexpr size_t MaxQueuedJobs = 64;
constexpr uint64_t MaxCacheBytes = 64ull << 20; // pictures kept on the SD card
constexpr int MaxAttempts = 3; // then it stays blank until the list is refreshed
constexpr uint32_t RetryAfterMs = 10000;
// Large art is kept at screen size; a 4K banner would otherwise hold 64MB.
SDL_Surface *shrink(SDL_Surface *surface) {
  if (!surface || std::max(surface->w, surface->h) <= MaxImageSide)
    return surface;
  const float scale = float(MaxImageSide) / std::max(surface->w, surface->h);
  auto *small = SDL_CreateRGBSurfaceWithFormat(
      0, std::max(1, int(surface->w * scale)), std::max(1, int(surface->h * scale)), 32,
      SDL_PIXELFORMAT_RGBA32);
  if (!small)
    return surface;
  SDL_BlitScaled(surface, nullptr, small, nullptr);
  SDL_FreeSurface(surface);
  return small;
}
std::string hash(const std::string &text) {
  uint64_t value = 14695981039346656037ULL;
  for (unsigned char c : text)
    value = (value ^ c) * 1099511628211ULL;
  std::ostringstream out;
  out << std::hex << value;
  return out.str();
}
} // namespace
namespace {
// The cache folder's cleanup thread; stopped and joined when the app ends.
struct CacheCleanup {
  std::thread thread;
  std::atomic<bool> stop{false};
  ~CacheCleanup() {
    stop = true;
    if (thread.joinable())
      thread.join();
  }
};
CacheCleanup &cacheCleanup() {
  static CacheCleanup cleanup;
  return cleanup;
}
} // namespace
void trimImageCache(const std::string &folder, uint64_t limit, const std::atomic<bool> *stop) {
  struct File {
    std::filesystem::file_time_type at;
    uint64_t size;
    std::filesystem::path path;
  };
  std::vector<File> files;
  uint64_t bytes = 0;
  std::error_code error;
  for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
    if (stop && *stop)
      return;
    if (!it->is_regular_file(error) || it->path().extension() != ".image")
      continue;
    const uint64_t size = it->file_size(error);
    if (error)
      continue;
    files.push_back({it->last_write_time(error), size, it->path()});
    bytes += size;
  }
  if (bytes <= limit)
    return;
  std::sort(files.begin(), files.end(), [](const File &a, const File &b) { return a.at < b.at; });
  for (const auto &file : files) {
    if (bytes <= limit || (stop && *stop))
      break;
    if (std::filesystem::remove(file.path, error))
      bytes -= std::min(bytes, file.size);
  }
}
Images::Images(SDL_Renderer *renderer) : renderer_(renderer) {
  int w, h;
  SDL_GetRendererOutputSize(renderer, &w, &h);
  scale_ = w / 1280.f;
}
Images::~Images() {
  stop_ = true;
  wake_.notify_all();
  for (auto &thread : threads_)
    if (thread.joinable())
      thread.join();
  for (const auto &t : textures_)
    SDL_DestroyTexture(t.second.texture);
  for (auto &e : entries_)
    if (e.second.surface)
      SDL_FreeSurface(e.second.surface);
}
void Images::source(Server server, State state, std::string cache,
                    std::string fixture) {
  // Source is set once per instance; a server change creates a fresh instance.
  server_ = std::move(server);
  state_ = std::move(state);
  cache_ = std::move(cache);
  fixture_ = std::move(fixture);
  if (!cache_.empty()) {
    makeDirectories(std::filesystem::path(cache_).string());
    // Once per run, in the background: on a full SD card the scan took up
    // to six seconds and ran again with every server read at startup. It
    // belongs to the run, not to this instance, which a server read replaces.
    auto &cleanup = cacheCleanup();
    if (!cleanup.thread.joinable())
      cleanup.thread = std::thread([folder = cache_] { trimImageCache(folder, MaxCacheBytes, &cacheCleanup().stop); });
  }
  for (int i = 0; i < Workers; ++i)
    threads_.emplace_back(&Images::worker, this);
}
void Images::updateState(State state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = std::move(state);
}
void Images::request(const std::string &url) {
  if (url.empty())
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(url);
  if (found != entries_.end()) {
    auto &e = found->second;
    // Asking again counts as use, so a picture loaded ahead of time (the
    // next banner slide) is kept like one on screen. Otherwise it was
    // dropped as soon as it was decoded and fetched again every frame,
    // and the pictures on screen waited behind it.
    e.used = ++tick_;
    e.frame = frame_;
    // Still waiting: drawn again means on screen now, so it goes first.
    if (!e.finished) {
      if (const auto queued = std::find(jobs_.begin(), jobs_.end(), url); queued != jobs_.end() && queued != jobs_.begin()) {
        jobs_.erase(queued);
        jobs_.push_front(url);
      }
      return;
    }
    // Failed: a few more tries, each waiting longer (10s, 20s), then none.
    if (!e.failed || e.attempts >= MaxAttempts ||
        SDL_GetTicks() - e.failedAt < RetryAfterMs << std::clamp(e.attempts - 1, 0, MaxAttempts))
      return;
    e.failed = e.finished = false;
  } else {
    entries_[url].used = ++tick_;
    entries_[url].frame = frame_;
  }
  // Newest first: what was just drawn is what the player is looking at.
  jobs_.push_front(url);
  while (jobs_.size() > MaxQueuedJobs) {
    // Off-screen leftovers; they are requested again if drawn again.
    entries_.erase(jobs_.back());
    jobs_.pop_back();
  }
  wake_.notify_one();
}
void Images::provide(const std::string &key, const std::string &bytes) {
  if (key.empty() || bytes.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (entries_.count(key)) return;
  entries_[key] = {bytes, nullptr, true, false, 0};
}
void Images::worker() {
  while (!stop_) {
    std::string url;
    State state;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
      if (stop_)
        break;
      url = jobs_.front();
      jobs_.pop_front();
      state = state_;
    }
    std::string bytes;
    guarded("image", [&] {
    if (!fixture_.empty()) {
      auto filename = url.substr(0, url.find('?'));
      filename = filename.substr(filename.find_last_of('/') + 1);
      if (filename.find_first_not_of("0123456789ABCDEF.jpg") ==
          std::string::npos)
        bytes = readFile(fixture_ + "/" + filename);
    }
    const auto path = cache_.empty() ? "" : cache_ + "/" + hash(url) + ".image";
    if (bytes.empty() && !path.empty())
      bytes = readFile(path);
    if (bytes.empty() && fixture_.empty()) {
      // WebP when the server has it (a third smaller), JPEG otherwise.
      auto headers = net::authHeaders(server_, state, url);
      headers.push_back("Accept: image/webp,image/*;q=0.8");
      const auto result = net::get(url, headers, "", &stop_);
      if (result.status == 200 && result.error.empty())
        bytes = result.body;
      if (!bytes.empty() && !path.empty()) {
        std::string ignored;
        atomicWrite(path, bytes, ignored);
      }
    }
    });
    // Decoded here, off the drawing thread: unpacking a picture as a row
    // scrolled in held up that frame.
    SDL_Surface *decoded = nullptr;
    if (!bytes.empty()) {
      decoded = shrink(IMG_Load_RW(SDL_RWFromConstMem(bytes.data(), int(bytes.size())), 1));
      if (!decoded) {
        logLine("image decode failed " + url.substr(0, url.find('?')) + ": " + IMG_GetError());
        // A broken cache file (an HTML error page, a cut download) would fail
        // the same way on every retry, so drop it and fetch again next time.
        std::error_code ignored;
        if (!cache_.empty())
          std::filesystem::remove(cache_ + "/" + hash(url) + ".image", ignored);
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto &entry = entries_[url];
      entry.finished = true;
      entry.failed = !decoded;
      if (entry.failed) {
        entry.failedAt = SDL_GetTicks();
        ++entry.attempts;
      }
      if (entry.surface)
        SDL_FreeSurface(entry.surface);
      entry.surface = decoded;
    }
  }
}
bool Images::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!jobs_.empty())
    return false;
  for (const auto &pair : entries_)
    if (!pair.second.finished || (!pair.second.failed && !pair.second.surface))
      return false;
  return true;
}
void Images::forgetFailures() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = entries_.begin(); it != entries_.end();)
    it = it->second.failed ? entries_.erase(it) : std::next(it);
}
bool Images::failed(const std::string &url) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto entry = entries_.find(url);
  return entry != entries_.end() && entry->second.failed;
}
bool Images::loaded(const std::string &url) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto entry = entries_.find(url);
  return entry != entries_.end() && entry->second.surface;
}
void Images::decode(int budget) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++frame_;
  for (auto &pair : entries_) {
    auto &e = pair.second;
    if (budget <= 0)
      break;
    if (!e.finished || e.failed || e.surface)
      continue;
    auto *rw = SDL_RWFromConstMem(e.bytes.data(), e.bytes.size());
    e.surface = shrink(IMG_Load_RW(rw, 1));
    if (!e.surface) {
      logLine("image decode failed " + pair.first.substr(0, pair.first.find('?')) + ": " + IMG_GetError());
      // A broken cache file (an HTML error page, a cut download) would fail the
      // same way on every retry, so drop it and fetch again next time.
      std::error_code ignored;
      if (!cache_.empty())
        std::filesystem::remove(cache_ + "/" + hash(pair.first) + ".image", ignored);
      e.failedAt = SDL_GetTicks();
      ++e.attempts;
    }
    e.bytes.clear();
    e.bytes.shrink_to_fit();
    e.failed = !e.surface;
    --budget;
  }
  size_t count = 0;
  for (const auto &pair : entries_)
    if (pair.second.surface)
      ++count;
  while (count > 24) { // decoded pictures kept for quick redraw, beyond those on screen
    auto oldest = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
      if (it->second.surface && it->second.frame + 2 < frame_ &&
          (oldest == entries_.end() || it->second.used < oldest->second.used))
        oldest = it;
    if (oldest == entries_.end())
      break;
    SDL_FreeSurface(oldest->second.surface);
    entries_.erase(oldest);
    --count;
  }
}
void Images::draw(const std::string &url, float x, float y, int w, int h,
                  int radius, bool blurred, bool contain) {
  request(url);
  SDL_Surface *source = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = entries_.find(url);
    if (entry != entries_.end()) {
      source = entry->second.surface;
      entry->second.used = ++tick_;
      entry->second.frame = frame_;
    }
  }
  if (!source)
    return;
  const auto key = url + ":" + std::to_string(w) + ":" + std::to_string(h) +
                   ":" + std::to_string(radius) + ":" + std::to_string(blurred) + ":" + std::to_string(contain);
  auto found = textures_.find(key);
  if (found == textures_.end()) {
    if (textures_.size() > 48) { // about two screens of covers; on-screen ones stay
      for (auto it = textures_.begin(); it != textures_.end();)
        if (it->second.frame + 2 < frame_) {
          SDL_DestroyTexture(it->second.texture);
          it = textures_.erase(it);
        } else
          ++it;
    }
    const int pw = std::lround(w * scale_), ph = std::lround(h * scale_);
    auto *surface =
        SDL_CreateRGBSurfaceWithFormat(0, pw, ph, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface)
      return;
    if (blurred) {
      auto *small =
          SDL_CreateRGBSurfaceWithFormat(0, 16, 8, 32, SDL_PIXELFORMAT_RGBA32);
      SDL_BlitScaled(source, nullptr, small, nullptr);
      auto *data = static_cast<Uint32 *>(small->pixels);
      auto *pixels = static_cast<Uint32 *>(surface->pixels);
      float colors[8][16][3]{};
      for (int by = 0; by < 8; ++by)
        for (int bx = 0; bx < 16; ++bx)
          for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
              Uint8 r, g, b, a;
              SDL_GetRGBA(data[std::clamp(by + dy, 0, 7) * 16 +
                               std::clamp(bx + dx, 0, 15)],
                          small->format, &r, &g, &b, &a);
              colors[by][bx][0] += r / 25.f;
              colors[by][bx][1] += g / 25.f;
              colors[by][bx][2] += b / 25.f;
            }
      for (int yy = 0; yy < ph; ++yy)
        for (int xx = 0; xx < pw; ++xx) {
          const float sx = xx * 15.f / pw, sy = yy * 7.f / ph;
          const int bx = sx, by = sy;
          const float fx = sx - bx, fy = sy - by;
          float rgb[3]{};
          for (int c = 0; c < 3; ++c)
            rgb[c] =
                (1 - fy) * ((1 - fx) * colors[by][bx][c] +
                            fx * colors[by][std::min(15, bx + 1)][c]) +
                fy *
                    ((1 - fx) * colors[std::min(7, by + 1)][bx][c] +
                     fx * colors[std::min(7, by + 1)][std::min(15, bx + 1)][c]);
          const float opacity = .34f - .22f * xx / pw;
          pixels[yy * pw + xx] =
              SDL_MapRGBA(surface->format, 10 + rgb[0] * opacity,
                          10 + rgb[1] * opacity, 14 + rgb[2] * opacity, 255);
        }
      SDL_FreeSurface(small);
    } else if (contain) {
      SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 10, 10, 14, 255));
      const float scale = std::min(float(pw) / source->w, float(ph) / source->h);
      SDL_Rect fit{0, 0, int(source->w * scale), int(source->h * scale)};
      fit.x = (pw - fit.w) / 2; fit.y = (ph - fit.h) / 2;
      SDL_BlitScaled(source, nullptr, surface, &fit);
    } else {
      SDL_Rect crop{0, 0, source->w, source->h};
      const float ratio = float(w) / h;
      if (float(source->w) / source->h > ratio) {
        crop.w = std::lround(source->h * ratio);
        crop.x = (source->w - crop.w) / 2;
      } else {
        crop.h = std::lround(source->w / ratio);
        crop.y = (source->h - crop.h) / 2;
      }
      SDL_BlitScaled(source, &crop, surface, nullptr);
    }
    // Rounded corners: only the four corner squares are touched, and the
    // alpha byte directly (RGBA32 keeps it last in memory). Going over every
    // pixel of every new cover was most of a scrolled-in row's cost.
    const float rr = radius * scale_;
    const int corner = std::min({int(std::ceil(rr)), pw / 2, ph / 2});
    auto *bytes = static_cast<Uint8 *>(surface->pixels);
    for (int yy = 0; yy < corner; ++yy)
      for (int xx = 0; xx < corner; ++xx) {
        const float dx = rr - xx - .5f, dy = rr - yy - .5f;
        if (dx <= 0 || dy <= 0)
          continue;
        const float coverage = std::clamp(rr + .5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f);
        if (coverage >= 1)
          continue;
        for (const int y : {yy, ph - 1 - yy})
          for (const int x : {xx, pw - 1 - xx}) {
            auto &alpha = bytes[y * surface->pitch + x * 4 + 3];
            alpha = Uint8(alpha * coverage);
          }
      }
    auto *texture = SDL_CreateTextureFromSurface(renderer_, surface);
    SDL_FreeSurface(surface);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    found = textures_.emplace(key, Texture{texture, frame_}).first;
  }
  found->second.frame = frame_;
  SDL_FRect dest{x, y, float(w), float(h)};
  SDL_RenderCopyF(renderer_, found->second.texture, nullptr, &dest);
}
} // namespace foil::ui
