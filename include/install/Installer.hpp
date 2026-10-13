#pragma once
#include "core/State.hpp"
#include "core/I18n.hpp"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
namespace foil::install {
enum class Status { Idle, Running, Succeeded, Failed, Cancelled };
struct Progress {
  Status status = Status::Idle;
  std::string name, icon, target = "sd", phase = tr("준비하고 있습니다."), error,
                          kind;
  size_t index = 0, count = 0;
  uint64_t total = 0, received = 0;
  double speed = 0, percent = 0;
  bool steady = false; // speed is the four-second average
  std::vector<Item> parts, waiting, completed;
};
Error preflight(const std::vector<Item> &files,
                std::optional<uint64_t> freeBytes);
// Compares dotted versions ("16.0.3" < "17.0.0"); negative, zero or positive.
int compareVersions(const std::string &a, const std::string &b);
// The highest firmware the files need when the console is older, else "".
// Unknown versions on either side never block.
std::string missingFirmware(const std::vector<Item> &files, const std::string &console);
// The speed shown: the bytes of the last four seconds, in eight half-second
// slots. The card writes in pauses (a burst, then nothing while a block
// goes down), which a four-second sum flattens; when nothing arrives the
// slots empty and it falls to zero.
class SpeedWindow {
public:
  // The bytes so far at `seconds`; a smaller total is the next file.
  void add(double seconds, uint64_t total);
  // MB/s, or negative until two seconds have been seen.
  double speed(double seconds);
  void reset() { *this = SpeedWindow(); }

private:
  static constexpr int Slots = 8;
  static constexpr double SlotSeconds = 0.5;
  void advance(double seconds);
  uint64_t slots_[Slots] = {};
  int slot_ = 0;
  double tick_ = -1, first_ = -1;
  uint64_t last_ = 0;
};
// The time left as a clock shows it: from a speed that follows slowly
// (four parts the old, one the new, once a second), counting down between.
class TimeLeft {
public:
  // Seconds, or negative while there is no speed to tell it by.
  double update(double seconds, uint64_t left, double speed);
  void reset() { *this = TimeLeft(); }

private:
  double speed_ = 0, left_ = -1, at_ = -1;
};
// "약 42초 남음", "약 3분 20초 남음", "약 25분 남음", "약 1시간 25분 남음".
std::string timeLeftText(double seconds);
class Installer {
public:
  ~Installer();
  Error start(std::vector<Item> files, std::string name, std::string icon,
              std::string target, Server server, State state);
  void cancel() { cancel_ = true; }
  // Pause at any moment: the download stops and resumes at the same byte.
  // What is already downloaded keeps being written meanwhile.
  void setPaused(bool paused);
  bool paused() const { return pause_; }
  bool cancellationRequested() const { return cancel_; }
  // Set from 설정 when the install starts; the engine reads it per file.
  bool skipSignatureCheck() const { return skipSignature_; }
  bool skipFirmwareCheck() const { return skipFirmware_; }
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
  std::atomic<bool> cancel_{false}, pause_{false}, skipSignature_{false}, skipFirmware_{false};
  Progress progress_;
  uint64_t completed_ = 0;
  std::map<std::string, uint64_t> contents_;
  mutable SpeedWindow speed_;
};
void nativeInstall(const Item &item, const std::string &target,
                   const std::vector<std::string> &headers);
} // namespace foil::install
