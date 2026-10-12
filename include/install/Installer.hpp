#pragma once
#include "core/State.hpp"
#include "core/I18n.hpp"
#include <atomic>
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
  std::vector<Item> parts, waiting, completed;
};
Error preflight(const std::vector<Item> &files,
                std::optional<uint64_t> freeBytes);
// Compares dotted versions ("16.0.3" < "17.0.0"); negative, zero or positive.
int compareVersions(const std::string &a, const std::string &b);
// The highest firmware the files need when the console is older, else "".
// Unknown versions on either side never block.
std::string missingFirmware(const std::vector<Item> &files, const std::string &console);
class Installer {
public:
  ~Installer();
  Error start(std::vector<Item> files, std::string name, std::string icon,
              std::string target, Server server, State state);
  void cancel() { cancel_ = true; }
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
  std::atomic<bool> cancel_{false}, skipSignature_{false}, skipFirmware_{false};
  Progress progress_;
  uint64_t completed_ = 0;
  std::map<std::string, uint64_t> contents_;
};
void nativeInstall(const Item &item, const std::string &target,
                   const std::vector<std::string> &headers);
} // namespace foil::install
