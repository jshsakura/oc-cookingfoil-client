#include "net/Tuner.hpp"
#include "net/Socket.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>
namespace foil::net {
ConnectionTuner::ConnectionTuner(int ceiling) : ceiling_(std::max(1, ceiling)), now_(std::min(2, ceiling_)) {}
int ConnectionTuner::ceiling(int downloading) const {
  return downloading > 1 ? std::max(1, ceiling_ / downloading) : ceiling_;
}
void ConnectionTuner::settle(int wait, const char *reason) {
  trying_ = false;
  here_ = tried_ = 0;
  trySum_ = 0;
  wait_ = wait;
  reason_ = reason;
}
int ConnectionTuner::span(uint64_t bytes, double seconds, int downloading) {
  const int most = ceiling(downloading);
  troubled_ = std::exchange(trouble_, false) ? troubled_ + 1 : 0;
  if (now_ > most) {
    now_ = most;
    speed_ = 0;
    settle(RetryAfter, "shared");
    return now_;
  }
  if (bytes == 0 || seconds <= 0)
    return now_; // paused or offline: nothing to tell
  const double speed = double(bytes) / seconds;
  if (troubled_ >= 2 && now_ > 1) {
    --now_;
    troubled_ = 0;
    speed_ = 0;
    settle(std::min(MostWait, std::max(RetryAfter, wait_ * 2)), "backed off");
    return now_;
  }
  if (trying_) {
    trySum_ += speed;
    if (++tried_ < TrySpans)
      return now_;
    const double tried = trySum_ / TrySpans;
    if (tried > before_ * Gain) {
      speed_ = tried;
      settle(1, "tuned"); // worth it: the next one soon
    } else {
      --now_; // no better (or worse): the count before, for fewer connections
      speed_ = before_;
      settle(std::min(MostWait, std::max(RetryAfter, wait_ * 2)), "tuned");
    }
    return now_;
  }
  speed_ = speed_ > 0 ? speed_ * 0.5 + speed * 0.5 : speed;
  ++here_;
  if (here_ >= wait_ && now_ < most && troubled_ == 0) {
    trying_ = true;
    before_ = speed_;
    tried_ = 0;
    trySum_ = 0;
    ++now_;
  }
  return now_;
}
std::string ConnectionTuner::summary(int downloading) const {
  auto text = "connections " + std::to_string(now_) + " (" + reason_ + ")";
  if (downloading > 1) text += ", " + std::to_string(downloading - 1) + " other" + (downloading > 2 ? "s" : "") + " downloading";
  return text;
}
namespace {
using Clock = std::chrono::steady_clock;
std::mutex lock;
bool running = false;
ConnectionTuner tuner;
Clock::time_point spanStart;
uint64_t spanBytes = 0;
std::atomic<int> active{0};
} // namespace
void startTuning(int ceiling) {
  std::lock_guard<std::mutex> held(lock);
  tuner = ConnectionTuner(ceiling);
  running = true;
  spanStart = Clock::now();
  spanBytes = receivedBytes();
}
void stopTuning() {
  std::lock_guard<std::mutex> held(lock);
  running = false;
}
int tunedConnections() {
  std::lock_guard<std::mutex> held(lock);
  if (!running) return 0;
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - spanStart).count();
  if (seconds >= ConnectionTuner::SpanSeconds) {
    const uint64_t bytes = receivedBytes();
    tuner.span(bytes - spanBytes, seconds, active.load());
    spanStart = now;
    spanBytes = bytes;
  }
  return tuner.connections();
}
void tuneTrouble() {
  std::lock_guard<std::mutex> held(lock);
  if (running) tuner.trouble();
}
std::string tuningSummary() {
  std::lock_guard<std::mutex> held(lock);
  return tuner.summary(active.load());
}
void heardActiveDownloads(int count) { active = std::clamp(count, 0, 64); }
int activeDownloads() { return active.load(); }
} // namespace foil::net
