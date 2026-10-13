#include "core/Log.hpp"
#include "install/Installer.hpp"
#include <cstdio>
#include <chrono>
#include "install/EngineHooks.hpp"
#include "core/I18n.hpp"
#include "net/Api.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <stdexcept>
namespace foil::install {
int compareVersions(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0, y = 0;
    while (i < a.size() && std::isdigit(static_cast<unsigned char>(a[i]))) x = x * 10 + unsigned(a[i++] - '0');
    while (j < b.size() && std::isdigit(static_cast<unsigned char>(b[j]))) y = y * 10 + unsigned(b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    while (i < a.size() && !std::isdigit(static_cast<unsigned char>(a[i]))) ++i;
    while (j < b.size() && !std::isdigit(static_cast<unsigned char>(b[j]))) ++j;
  }
  return 0;
}
std::string missingFirmware(const std::vector<Item> &files, const std::string &console) {
  std::string need;
  for (const auto &file : files)
    if (!file.requiredFirmware.empty() && (need.empty() || compareVersions(need, file.requiredFirmware) < 0))
      need = file.requiredFirmware;
  if (need.empty() || console.empty() || compareVersions(console, need) >= 0) return "";
  return need;
}
Error preflight(const std::vector<Item> &files,
                std::optional<uint64_t> space) {
  Error error;
  if (files.empty()) {
    error.message = tr("Select items to install.");
    return error;
  }
  uint64_t total = 0;
  for (const auto &file : files) {
    auto url = file.url.substr(0, file.url.find_first_of("?#"));
    auto dot = url.find_last_of('.');
    auto ext = dot == std::string::npos ? "" : url.substr(dot);
    for (auto &c : ext)
      c = std::tolower(static_cast<unsigned char>(c));
    if (ext != ".nsp" && ext != ".nsz" && ext != ".xci" && ext != ".xcz") {
      error.message = tr("This file type cannot be installed.");
      return error;
    }
    // .nsz and .xcz unpack as they install: the room needed is the
    // installed size when the server knows it.
    const uint64_t need = std::max(file.size, file.installSize);
    if (UINT64_MAX - total < need) {
      error.message = tr("Check the file size.");
      return error;
    }
    total += need;
  }
  if (!space) {
    error.message = tr("Could not check the free space on the storage.");
    return error;
  }
  if (*space < total || *space - total < 256 * 1024 * 1024) {
    error.message = tr("Not enough free space on the selected storage.");
    return error;
  }
  return error;
}
Installer::~Installer() {
  cancel_ = true;
  if (thread_.joinable())
    thread_.join();
}
bool Installer::running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_.status == Status::Running;
}
Progress Installer::progress() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_;
}
Error Installer::start(std::vector<Item> files, std::string name,
                       std::string icon, std::string target, Server server,
                       State state) {
  Error error;
  if (running()) {
    error.message = tr("Select again after the current install finishes.");
    return error;
  }
  error = preflight(files, platform::freeSpace(target));
  if (error)
    return error;
  if (!state.skipFirmwareCheck) {
    const auto console = platform::firmwareVersion();
    const auto need = missingFirmware(files, console);
    if (!need.empty()) {
      error.message = trf("This needs firmware {1} or later; the console has {0}. Turn on Ignore firmware check in Settings to install it anyway.",
                          {console, need});
      return error;
    }
  }
  if (thread_.joinable())
    thread_.join();
  cancel_ = false;
  pause_ = false;
  skipSignature_ = state.skipSignatureCheck;
  skipFirmware_ = state.skipFirmwareCheck;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_ = {};
    progress_.status = Status::Running;
    progress_.name = std::move(name);
    progress_.icon = std::move(icon);
    progress_.target = std::move(target);
    progress_.count = files.size();
    progress_.parts = files;
    for (const auto &f : files)
      progress_.total += f.size;
    completed_ = 0;
    contents_.clear();
  }
  thread_ = std::thread(&Installer::run, this, std::move(files),
                        std::move(server), std::move(state));
  return {};
}
void SpeedWindow::advance(double seconds) {
  const auto steps = int64_t((seconds - tick_) / SlotSeconds);
  if (steps <= 0)
    return;
  for (int64_t n = 0; n < std::min<int64_t>(steps, Slots); ++n) {
    slot_ = (slot_ + 1) % Slots;
    slots_[slot_] = 0;
  }
  tick_ += steps * SlotSeconds;
}
void SpeedWindow::add(double seconds, uint64_t total) {
  if (tick_ < 0) {
    tick_ = first_ = seconds;
    last_ = total;
    return;
  }
  advance(seconds);
  if (total > last_)
    slots_[slot_] += total - last_;
  last_ = total;
}
double SpeedWindow::speed(double seconds) {
  constexpr double Least = 2;
  if (tick_ < 0 || seconds - first_ < Least)
    return -1;
  advance(seconds);
  uint64_t sum = 0;
  for (const auto bytes : slots_)
    sum += bytes;
  const double span = std::min(seconds - first_, Slots * SlotSeconds);
  return double(sum) / span / (1024 * 1024);
}
double TimeLeft::update(double seconds, uint64_t left, double speed) {
  if (at_ >= 0 && seconds - at_ < 1)
    return left_;
  speed_ = speed_ > 0 ? (speed_ * 4 + std::max(0., speed)) / 5 : std::max(0., speed);
  const double estimate = speed_ > 0 ? double(left) / (speed_ * 1024 * 1024) : -1;
  if (estimate < 0 || left_ < 0)
    left_ = estimate;
  else
    left_ = std::max(0., (left_ - (seconds - at_)) * 0.8 + estimate * 0.2);
  at_ = seconds;
  return left_;
}
std::string timeLeftText(double seconds) {
  if (seconds < 0)
    return tr("Estimating time left");
  const auto s = std::max<int64_t>(1, std::llround(seconds));
  if (s < 60)
    return trf("About {0} s left", {std::to_string(s)});
  if (s < 600)
    return trf("About {0} min {1} s left", {std::to_string(s / 60), std::to_string(s % 60)});
  if (s < 3600)
    return trf("About {0} min left", {std::to_string((s + 30) / 60)});
  const auto minutes = (s + 30) / 60;
  return trf("About {0} h {1} min left", {std::to_string(minutes / 60), std::to_string(minutes % 60)});
}
void Installer::setPaused(bool paused) {
  pause_ = paused;
  std::lock_guard<std::mutex> lock(mutex_);
  if (progress_.status != Status::Running)
    return;
  progress_.speed = 0;
  progress_.phase = paused ? tr("Paused.") : tr("Downloading.");
}
void Installer::phase(const std::string &text) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pause_)
    return; // 일시정지했습니다 stays until it resumes
  progress_.phase = text;
}
void Installer::stagePercent(double value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (progress_.total == 0)
    progress_.percent = std::clamp(value, 0., 99.);
}
void Installer::bytes(const std::string &content, uint64_t received,
                      double speed) {
  std::lock_guard<std::mutex> lock(mutex_);
  contents_[content] = received;
  uint64_t total = completed_;
  for (const auto &entry : contents_)
    total += entry.second;
  progress_.received = std::min(progress_.total, total);
  progress_.speed = speed;
  progress_.percent =
      progress_.total
          ? std::min(99., 100. * progress_.received / progress_.total)
          : 0;
}
void Installer::run(std::vector<Item> files, Server server, State state) {
  engineSink(this);
  struct Sink {
    ~Sink() { engineSink(nullptr); }
  } clear;
  try {
    for (size_t i = 0; i < files.size(); ++i) {
      if (cancel_)
        throw std::runtime_error(tr("Install canceled."));
      std::string target;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.index = i;
        progress_.kind = files[i].type;
        progress_.phase = tr("Checking files.");
        contents_.clear();
        target = progress_.target;
      }
      // A failed item is tried again from the start: the engine skips the
      // content files it already finished, so this picks up where it broke.
      constexpr int Attempts = 3;
      for (int attempt = 1;; ++attempt) {
        try {
          nativeInstall(files[i], target, net::authHeaders(server, state, files[i].url));
          break;
        } catch (const std::exception &) {
          if (cancel_ || attempt >= Attempts)
            throw;
          phase(trf("Retrying. Attempt {0} of {1}.", {std::to_string(attempt + 1), std::to_string(Attempts)}));
          for (int tenth = 0; tenth < 100 && !cancel_; ++tenth)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (cancel_)
            throw;
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        completed_ += files[i].size;
        progress_.received = completed_;
        progress_.percent =
            progress_.total ? 100. * completed_ / progress_.total : 0;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_.status = Status::Succeeded;
      progress_.percent = 100;
      progress_.phase = tr("Installed.");
    }
  } catch (const std::exception &e) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.status = cancel_ ? Status::Cancelled : Status::Failed;
    progress_.error = e.what();
    progress_.phase = cancel_ ? tr("Install canceled.") : tr("Install failed.");
  }
}
} // namespace foil::install
