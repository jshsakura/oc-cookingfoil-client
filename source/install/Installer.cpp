#include "core/Log.hpp"
#include "install/Installer.hpp"
#include <cstdio>
#include <chrono>
#include "install/EngineHooks.hpp"
#include "core/I18n.hpp"
#include "net/Api.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
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
    error.message = tr("설치할 항목을 선택해 주세요.");
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
      error.message = tr("이 파일 형식은 설치할 수 없습니다.");
      return error;
    }
    // .nsz and .xcz unpack as they install: the room needed is the
    // installed size when the server knows it.
    const uint64_t need = std::max(file.size, file.installSize);
    if (UINT64_MAX - total < need) {
      error.message = tr("파일 용량을 확인해 주세요.");
      return error;
    }
    total += need;
  }
  if (!space) {
    error.message = tr("저장 위치의 여유 공간을 확인하지 못했습니다.");
    return error;
  }
  if (*space < total || *space - total < 256 * 1024 * 1024) {
    error.message = tr("선택한 저장 위치의 여유 공간이 부족합니다.");
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
    error.message = tr("현재 설치가 끝난 뒤 다시 선택합니다.");
    return error;
  }
  error = preflight(files, platform::freeSpace(target));
  if (error)
    return error;
  if (!state.skipFirmwareCheck) {
    const auto console = platform::firmwareVersion();
    const auto need = missingFirmware(files, console);
    if (!need.empty()) {
      error.message = trf("본체 펌웨어 {0}에서는 실행되지 않습니다. {1} 이상이 필요합니다. 설정에서 펌웨어 검사 무시를 켜면 설치합니다.",
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
    speed_.reset();
  }
  thread_ = std::thread(&Installer::run, this, std::move(files),
                        std::move(server), std::move(state));
  return {};
}
double SpeedWindow::add(double seconds, uint64_t bytes) {
  constexpr double Span = 10, Least = 2, Step = 0.25;
  if (!samples_.empty() && bytes < samples_.back().second)
    samples_.clear();
  if (samples_.empty() || seconds - samples_.back().first >= Step)
    samples_.push_back({seconds, bytes});
  while (samples_.size() > 2 && seconds - samples_[1].first >= Span)
    samples_.pop_front();
  const double span = seconds - samples_.front().first;
  if (span < Least)
    return 0;
  return double(bytes - samples_.front().second) / span / (1024 * 1024);
}
void Installer::setPaused(bool paused) {
  pause_ = paused;
  std::lock_guard<std::mutex> lock(mutex_);
  if (progress_.status != Status::Running)
    return;
  progress_.speed = 0;
  speed_.reset();
  progress_.phase = paused ? tr("일시정지했습니다.") : tr("다운로드하고 있습니다.");
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
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const double steady = speed_.add(seconds, progress_.received);
  progress_.speed = steady > 0 ? steady : speed;
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
        throw std::runtime_error(tr("설치를 취소했습니다."));
      std::string target;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_.index = i;
        progress_.kind = files[i].type;
        progress_.phase = tr("파일을 확인하고 있습니다.");
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
          phase(trf("다시 받고 있습니다. {0}/{1}번째 시도", {std::to_string(attempt + 1), std::to_string(Attempts)}));
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
      progress_.phase = tr("설치했습니다.");
    }
  } catch (const std::exception &e) {
    std::lock_guard<std::mutex> lock(mutex_);
    progress_.status = cancel_ ? Status::Cancelled : Status::Failed;
    progress_.error = e.what();
    progress_.phase = cancel_ ? tr("설치를 취소했습니다.") : tr("설치하지 못했습니다.");
  }
}
} // namespace foil::install
