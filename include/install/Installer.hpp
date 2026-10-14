#pragma once
#include <algorithm>
#include "core/State.hpp"
#include "core/I18n.hpp"
#include "core/DownloadStore.hpp"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
namespace foil::install {
// Stopped: cut short to go on later (the app closing), kept as it was.
enum class Status { Idle, Running, Succeeded, Failed, Cancelled, Stopped };
struct Progress {
  Status status = Status::Idle;
  std::string name, icon, target = "sd", phase = tr("Preparing."), error,
                          kind;
  size_t index = 0, count = 0;
  uint64_t total = 0, received = 0;
  double speed = 0, percent = 0;
  std::vector<Item> parts, waiting, completed;
};
Error preflight(const std::vector<Item> &files,
                std::optional<uint64_t> freeBytes);
// Compares dotted versions ("16.0.3" < "17.0.0"); negative, zero or positive.
int compareVersions(const std::string &a, const std::string &b);
// The highest firmware the files need when the console is older, else "".
// Unknown versions on either side never block.
std::string missingFirmware(const std::vector<Item> &files, const std::string &console);
// The speed shown, calm enough to read: everything received since this
// download started over the time it has run, refreshed every three
// seconds. Nothing is shown for the first five seconds and the first 4 MB
// (or fifteen seconds at most), while it is still measuring. Ten seconds
// with nothing arriving (the last twenty half-second slots empty) shows 0.
class SpeedWindow {
public:
  // The bytes so far at `seconds`; a smaller total is the next file.
  void add(double seconds, uint64_t total);
  // MB/s, or negative in the first second: the last five seconds, refreshed
  // every second and eased, as other downloaders show it. Pauses of two
  // seconds or more (between files, install steps) are left out of it.
  double speed(double seconds);
  // The last thirty seconds over all of their time, pauses included: what
  // the time left goes by, so it does not jump. Set by speed().
  double average() const { return average_; }
  void reset() { *this = SpeedWindow(); }

private:
  static constexpr int Slots = 60;
  static constexpr double SlotSeconds = 0.5;
  void advance(double seconds);
  uint64_t lastSeconds(int slots) const;
  uint64_t slots_[Slots] = {};
  int slot_ = 0;
  double tick_ = -1, first_ = -1, shownAt_ = -1, shown_ = -1, average_ = -1;
  uint64_t last_ = 0, gathered_ = 0;
};
// The time left, counting down second by second like a clock. Every ten
// seconds it is worked out again from what is left over the speed; a result
// close to the count keeps the count (no jump), one far off replaces it.
// While nothing arrives it holds; fifteen seconds of that make it unknown.
class TimeLeft {
public:
  // Seconds, or negative while there is no speed to tell it by.
  double update(double seconds, uint64_t left, double speed);
  void reset() { *this = TimeLeft(); }

private:
  double count(double seconds) const { return std::max(1.0, anchor_ - (seconds - at_)); }
  double anchor_ = -1, at_ = -1, estimatedAt_ = -1, stalled_ = -1;
};
// "1분 미만 남음", "약 25분 남음", "약 1시간 25분 남음" (minutes rounded up).
std::string timeLeftText(double seconds);
class Installer {
public:
  ~Installer();
  Error start(std::vector<Item> files, std::string name, std::string icon,
              std::string target, Server server, State state);
  void cancel() { cancel_ = true; }
  // Ends the download now but keeps where it stands, for the next start.
  void stop() {
    stop_ = true;
    cancel_ = true;
  }
  bool stopping() const { return stop_; }
  // Until the install thread has ended (after stop or cancel).
  void wait() {
    if (thread_.joinable())
      thread_.join();
  }
  // Pause at any moment: the download stops and resumes at the same byte.
  // What is already downloaded keeps being written meanwhile.
  void setPaused(bool paused);
  bool paused() const { return pause_; }
  bool cancellationRequested() const { return cancel_; }
  // Set from 설정 when the install starts; the engine reads it per file.
  bool skipSignatureCheck() const { return skipSignature_; }
  bool skipFirmwareCheck() const { return skipFirmware_; }
  // Where cut-off files stand (EngineHooks), held here so the install
  // thread never waits on the card: the UI thread loads them from
  // 다운로드's database and writes back what changed (takeResumeChanges).
  using ResumePoint = DownloadStore::ResumePoint;
  void loadResumePoints(std::map<std::string, ResumePoint> points);
  std::optional<ResumePoint> resumePoint(const std::string &key) const;
  void keepResumePoint(const std::string &key, const ResumePoint &point);
  void forgetResumePoint(const std::string &key);
  // What changed since the last call: a point to keep, or none to forget.
  std::map<std::string, std::optional<ResumePoint>> takeResumeChanges();
  // "sd" or "nand", as the install was started.
  std::string target() const;
  bool running() const;
  Progress progress() const;
  void phase(const std::string &text);
  void bytes(const std::string &content, uint64_t received, double speed);
  void stagePercent(double percent);
#ifndef __SWITCH__
  void preview(Progress progress) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_ = std::move(progress);
  }
#endif
private:
  void run(std::vector<Item> files, Server server, State state);
  mutable std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> cancel_{false}, stop_{false}, pause_{false}, skipSignature_{false}, skipFirmware_{false};
  Progress progress_;
  uint64_t completed_ = 0;
  std::map<std::string, uint64_t> contents_;
  mutable std::mutex resumeLock_;
  std::map<std::string, ResumePoint> resume_;
  std::map<std::string, std::optional<ResumePoint>> resumeChanges_;
};
void nativeInstall(const Item &item, const std::string &target,
                   const std::vector<std::string> &headers);
} // namespace foil::install
