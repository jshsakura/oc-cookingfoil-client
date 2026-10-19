#include "net/Tuner.hpp"
#include "net/Socket.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>
namespace foil::net {
ConnectionTuner::ConnectionTuner(int ceiling, int remembered)
    : ceiling_(std::max(1, ceiling)), now_(std::min(2, ceiling_)) {
  if (remembered > 0) {
    now_ = std::min(remembered, ceiling_);
    settle(RetryAfter, "remembered");
  }
}
int ConnectionTuner::ceiling(int downloading) const {
  return downloading > 1 ? std::max(1, ceiling_ / downloading) : ceiling_;
}
void ConnectionTuner::settle(int wait, const char *reason) {
  settled_ = speed_;
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
  // The line changed under a decided count: look again now.
  if (settled_ == 0)
    settled_ = speed_;
  else if (here_ >= 2 && (speed_ < settled_ * (1 - Shift) || speed_ > settled_ * (1 + Shift))) {
    settled_ = speed_;
    wait_ = here_;
    reason_ = "re-tuned";
  }
  if (here_ >= wait_ && now_ < most && troubled_ == 0) {
    trying_ = true;
    before_ = speed_;
    tried_ = 0;
    trySum_ = 0;
    ++now_;
  }
  return now_;
}
void ConnectionTuner::carryOn(int ceiling) {
  ceiling_ = std::max(1, ceiling);
  if (now_ > ceiling_) {
    now_ = ceiling_;
    speed_ = 0;
    settle(RetryAfter, "shared");
  }
  if (reason_ == "start") reason_ = "kept";
}
std::string ConnectionTuner::summary(int downloading) const {
  auto text = "connections " + std::to_string(now_) + " (" + reason_ + ")";
  if (downloading > 1) text += ", " + std::to_string(downloading - 1) + " other" + (downloading > 2 ? "s" : "") + " downloading";
  return text;
}
namespace {
using Clock = std::chrono::steady_clock;
std::mutex lock;
// What each place taught, kept for the run: a small download from elsewhere
// in between (it ends before a span, learning nothing) leaves the rest as
// they were.
std::map<std::string, ConnectionTuner> learned;
struct Tuning {
  int id, ceiling;
  std::string place;
};
std::vector<Tuning> runs; // oldest first; the first leads
int lastId = 0;
ConnectionTuner *tuner = nullptr;
ConnectionTuner idle;
Clock::time_point spanStart;
uint64_t spanBytes = 0;
std::atomic<int> active{0};
std::string keptIn;                // where the found counts live on the card
bool recalled = false;             // read from there once
std::map<std::string, int> kept;   // as the card holds them
// A fresh span from now (the bytes before it are not this one's).
void restartSpan() {
  spanStart = Clock::now();
  spanBytes = receivedBytes();
}
// The leading run's tuner: what that place found, under its ceiling.
void lead(const Tuning &run) {
  const auto found = learned.find(run.place);
  if (found != learned.end()) {
    found->second.carryOn(run.ceiling);
    tuner = &found->second;
  } else {
    if (!recalled && !keptIn.empty()) {
      kept = parseFound(readFile(keptIn));
      recalled = true;
    }
    const auto before = kept.find(run.place);
    tuner = &learned.emplace(run.place, ConnectionTuner(run.ceiling, before == kept.end() ? 0 : before->second))
                 .first->second;
  }
  restartSpan();
}
// The counts to keep on the card now, or "" when they are as kept.
std::string foundToKeep() {
  if (keptIn.empty()) return "";
  auto now = kept;
  for (const auto &[place, learnt] : learned)
    if (learnt.found()) now[place] = learnt.connections();
  if (now == kept) return "";
  kept = std::move(now);
  return serializeFound(kept);
}
} // namespace
int startTuning(int ceiling, const std::string &place) {
  std::lock_guard<std::mutex> held(lock);
  runs.push_back({++lastId, ceiling, place});
  if (runs.size() == 1)
    lead(runs.front());
  else
    restartSpan(); // shared from here: nothing is judged
  return lastId;
}
void stopTuning(int id) {
  std::string keep, path;
  {
    std::lock_guard<std::mutex> held(lock);
    const auto at = std::find_if(runs.begin(), runs.end(), [id](const Tuning &run) { return run.id == id; });
    if (at == runs.end()) return;
    const bool leading = at == runs.begin();
    runs.erase(at);
    keep = foundToKeep();
    path = keptIn;
    if (!runs.empty()) {
      if (leading)
        lead(runs.front()); // the one left goes on with its own place
      else
        restartSpan(); // alone again: a span of its own bytes only
    }
  }
  std::string error;
  if (!keep.empty()) atomicWrite(path, keep, error); // a count not kept is found again next run
}
void rememberTuningIn(const std::string &path) {
  std::lock_guard<std::mutex> held(lock);
  if (path == keptIn) return;
  keptIn = path;
  recalled = false;
  kept.clear();
}
std::string serializeFound(const std::map<std::string, int> &found) {
  std::string text;
  for (const auto &[place, count] : found)
    if (count > 0 && !place.empty() && place.find_first_of(" \r\n") == std::string::npos)
      text += place + " " + std::to_string(count) + "\n";
  return text;
}
std::map<std::string, int> parseFound(const std::string &text) {
  std::map<std::string, int> found;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream fields(line);
    std::string place;
    int count = 0;
    if (fields >> place >> count && count > 0 && count <= 64) found[place] = count;
  }
  return found;
}
int tunedConnections() {
  std::lock_guard<std::mutex> held(lock);
  if (runs.empty() || !tuner) return 0;
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - spanStart).count();
  if (seconds >= ConnectionTuner::SpanSeconds) {
    const uint64_t bytes = receivedBytes();
    if (runs.size() == 1) tuner->span(bytes - spanBytes, seconds, active.load());
    spanStart = now;
    spanBytes = bytes;
  }
  return tuner->connections();
}
void tuneTrouble() {
  std::lock_guard<std::mutex> held(lock);
  if (runs.size() == 1 && tuner) tuner->trouble(); // shared, whose it was is not known
}
bool tuningFound() {
  std::lock_guard<std::mutex> held(lock);
  return runs.empty() || !tuner || tuner->found();
}
std::string tuningSummary() {
  std::lock_guard<std::mutex> held(lock);
  return (tuner ? *tuner : idle).summary(active.load());
}
void heardActiveDownloads(int count) { active = std::clamp(count, 0, 64); }
int activeDownloads() { return active.load(); }
} // namespace foil::net
