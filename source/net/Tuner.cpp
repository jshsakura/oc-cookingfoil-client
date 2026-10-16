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
int ConnectionTuner::span(uint64_t bytes, double seconds, int downloading) {
  const int most = ceiling(downloading);
  const bool hadTrouble = std::exchange(trouble_, false);
  if (now_ > most) {
    now_ = most;
    probing_ = false;
    before_ = 0;
    reason_ = "shared";
    return now_;
  }
  if (hadTrouble) {
    if (now_ > 1) --now_;
    probing_ = false;
    before_ = 0;
    reason_ = "backed off";
    return now_;
  }
  if (bytes == 0 || seconds <= 0)
    return now_; // paused or offline: nothing to tell
  const double speed = double(bytes) / seconds;
  if (probing_) {
    if (speed > before_ * Gain) {
      before_ = speed;
      reason_ = "tuned";
      if (now_ < most) ++now_;
      else probing_ = false;
    } else if (speed < before_) {
      --now_; // the step up made it slower
      probing_ = false;
      reason_ = "tuned";
    } else {
      probing_ = false; // no better: stays
      reason_ = "tuned";
    }
    return now_;
  }
  // The first span (or the first after a change): a step up is tried while
  // there is room.
  if (before_ == 0 && now_ < most) {
    before_ = speed;
    ++now_;
    probing_ = true;
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
