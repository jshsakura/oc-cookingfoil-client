#pragma once
#include "core/State.hpp"
#include <optional>
#include <SDL2/SDL.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
namespace foil::ui {
// Keeps a picture cache folder under `limit` bytes, oldest files first.
// Each file is looked at once; nothing is sorted when the folder is under
// the limit. Stops early when `stop` turns true.
// Over `limit`, the oldest pictures go until three quarters of it are left.
void trimImageCache(const std::string &folder, uint64_t limit, const std::atomic<bool> *stop = nullptr);
// How much the picture cache may keep, from the card's free space (none known: the most).
uint64_t imageCacheLimit(std::optional<uint64_t> free);
// The pictures in the cache folder: their size, or all of them gone (설정).
uint64_t imageCacheBytes(const std::string &folder);
void clearImageCache(const std::string &folder);
class Images {
public:
  explicit Images(SDL_Renderer *renderer);
  ~Images();
  void source(Server server, State state, std::string cache,
              std::string fixture = "");
  // New access keys after pairing; decoded images and textures are kept.
  void updateState(State state);
  void request(const std::string &url);
  void provide(const std::string &key, const std::string &bytes);
  void decode(int budget = 2);
  bool ready() const;
  bool failed(const std::string &url) const;
  // Pictures that failed are asked for again the next time they are drawn
  // (the - refresh).
  void forgetFailures();
  bool loaded(const std::string &url) const;
  void draw(const std::string &url, float x, float y, int w, int h,
            int radius = 14, bool blurred = false, bool contain = false);

private:
  struct Entry {
    std::string bytes;
    SDL_Surface *surface = nullptr;
    bool finished = false, failed = false;
    uint64_t used = 0;
    uint64_t frame = 0; // the last frame that drew it
    uint32_t failedAt = 0;
    int attempts = 0;
  };
  void worker();
  SDL_Renderer *renderer_;
  float scale_ = 1;
  Server server_;
  State state_;
  std::string cache_, fixture_;
  std::atomic<bool> stop_{false};
  std::vector<std::thread> threads_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  uint32_t heldUntil_ = 0; // after a 429, no picture is asked for until then (SDL ticks)
  void waitOutLimit();
  std::deque<std::string> jobs_;
  std::map<std::string, Entry> entries_;
  struct Texture {
    SDL_Texture *texture = nullptr;
    uint64_t frame = 0;
  };
  std::map<std::string, Texture> textures_;
  uint64_t tick_ = 0;
  // Counts decode() calls, one per drawn frame. Whatever was drawn in the
  // last two frames is on screen and is never evicted: evicting it only
  // makes it load again, and with more pictures on screen than the cache
  // holds they took turns loading forever (covers that kept shimmering).
  uint64_t frame_ = 0;
};
} // namespace foil::ui
