#include "core/Log.hpp"
#include "install/Installer.hpp"
#include <utility>
#include <cstdio>
#include <chrono>
#include "install/EngineHooks.hpp"
#include "core/I18n.hpp"
#include "net/Api.hpp"
#include "net/Socket.hpp"
#include "net/Segments.hpp"
#include "core/NczPoint.hpp"
#include "platform/Cores.hpp"
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
  stop(); // closing: kept to go on next time
  if (thread_.joinable())
    thread_.join();
}
bool Installer::running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_.status == Status::Running;
}
void Installer::loadResumePoints(std::map<std::string, ResumePoint> points) {
  std::lock_guard<std::mutex> held(resumeLock_);
  resume_ = std::move(points);
  resumeChanges_.clear();
}
std::optional<Installer::ResumePoint> Installer::resumePoint(const std::string &key) const {
  std::lock_guard<std::mutex> held(resumeLock_);
  const auto found = resume_.find(key);
  if (found == resume_.end())
    return std::nullopt;
  return found->second;
}
void Installer::keepResumePoint(const std::string &key, const ResumePoint &point) {
  std::lock_guard<std::mutex> held(resumeLock_);
  resume_[key] = point;
  resumeChanges_[key] = point;
}
void Installer::forgetResumePoint(const std::string &key) {
  std::lock_guard<std::mutex> held(resumeLock_);
  if (resume_.erase(key))
    resumeChanges_[key] = std::nullopt;
}
std::map<std::string, std::optional<Installer::ResumePoint>> Installer::takeResumeChanges() {
  std::lock_guard<std::mutex> held(resumeLock_);
  return std::exchange(resumeChanges_, {});
}
std::string Installer::target() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_.target;
}
Progress Installer::progress() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_;
}
Error Installer::start(std::vector<Item> files, std::string name,
                       std::string icon, std::string target, Server server,
                       State state, std::optional<uint64_t> free) {
  Error error;
  if (running()) {
    error.message = tr("Select again after the current install finishes.");
    return error;
  }
  error = preflight(files, free ? free : platform::freeSpace(target));
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
  stop_ = false;
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
  if (total > last_) {
    slots_[slot_] += total - last_;
    gathered_ += total - last_;
  }
  last_ = total;
}
uint64_t SpeedWindow::lastSeconds(int slots) const {
  uint64_t sum = 0;
  for (int n = 0; n < std::min(slots, Slots); ++n)
    sum += slots_[(slot_ - n + Slots) % Slots];
  return sum;
}
double SpeedWindow::speed(double seconds) {
  // As other downloaders show it: the last few seconds, refreshed every
  // second and eased so it does not flicker, from the first second on.
  constexpr double Least = 1, Every = 1, Recent = 5, Window = Slots * SlotSeconds;
  if (tick_ < 0)
    return -1;
  const double since = seconds - first_;
  if (since < Least)
    return -1;
  advance(seconds);
  if (since >= 10 && lastSeconds(int(10 / SlotSeconds)) == 0) {
    shown_ = 0;
    return 0; // nothing for ten seconds: stalled
  }
  if (shown_ < 0 || seconds - shownAt_ >= Every) {
    average_ = double(lastSeconds(Slots)) / std::min(since, Window) / (1024 * 1024);
    // What came in over the last seconds, pauses included: when the card or
    // the install holds the download back, that is the speed it really goes
    // at (leaving the pauses out showed the network's bursts, too high).
    const int count = std::max(1, int(std::min(since, Recent) / SlotSeconds));
    const double now = double(lastSeconds(count)) / (count * SlotSeconds) / (1024 * 1024);
    shown_ = shown_ <= 0 ? now : (shown_ + now) / 2;
    shownAt_ = seconds;
  }
  return shown_;
}
double TimeLeft::update(double seconds, uint64_t left, double speed) {
  constexpr double Every = 10, Stall = 15;
  if (speed <= 0) {
    if (anchor_ < 0)
      return -1;
    if (stalled_ < 0) {
      stalled_ = seconds;
      anchor_ = count(seconds); // held where it stood
    }
    if (seconds - stalled_ >= Stall) {
      reset();
      return -1;
    }
    // The count goes on once bytes come again, and the first of them works
    // it out afresh.
    at_ = seconds;
    estimatedAt_ = -1;
    return anchor_;
  }
  stalled_ = -1;
  if (estimatedAt_ < 0 || seconds - estimatedAt_ >= Every) {
    // On the safe side: a tenth slower than measured, so it rather ends early.
    const double estimate = double(left) / (speed * 0.9 * 1024 * 1024);
    const double now = anchor_ < 0 ? estimate : count(seconds);
    anchor_ = std::abs(estimate - now) > std::max(5.0, estimate * 0.15) ? estimate : now;
    at_ = estimatedAt_ = seconds;
  }
  return count(seconds);
}
std::string timeLeftText(double seconds) {
  if (seconds < 0)
    return tr("Estimating time left");
  const auto s = std::max<int64_t>(1, std::llround(seconds));
  // Minutes rounded up, never seconds: a calm number that rather ends early.
  if (s < 60)
    return tr("Less than a minute left");
  const auto minutes = (s + 59) / 60;
  if (minutes < 60)
    return trf("About {0} min left", {std::to_string(minutes)});
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
namespace {
// A content file by its id ("0123abcd" of "0123ABCD.nca" or of the resume
// key "sd:0123abcd"), so the engine's reports and the saved points meet.
std::string contentId(std::string name) {
  name = name.substr(name.rfind(':') == std::string::npos ? 0 : name.rfind(':') + 1);
  name = name.substr(0, name.find('.'));
  for (auto &c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  return name;
}
// Bytes an earlier try left in place by its saved point: the parts of a
// plain file, or how far into its compressed stream an NCZ got.
uint64_t keptBytes(const Installer::ResumePoint &point) {
  if (NczPoint::isNcz(point.plan)) {
    NczPoint at;
    return NczPoint::decode(point.plan, at) ? at.zstdStart + at.consumed : 0;
  }
  std::vector<net::Segment> parts;
  return net::SegmentPlan::load(point.plan, parts) ? net::SegmentPlan(parts).received() : 0;
}
} // namespace
void Installer::bytes(const std::string &content, uint64_t received,
                      double speed) {
  std::lock_guard<std::mutex> lock(mutex_);
  contents_[contentId(content)] = received;
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
  const platform::OnCore off(platform::WriterCore); // the drawing core keeps the screen
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
      // Going on from an earlier try: the bar starts where it stopped, not
      // at 0 until the first new byte.
      {
        std::lock_guard<std::mutex> held(resumeLock_);
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &[key, point] : resume_)
          if (point.url == files[i].url && key.rfind(target + ":", 0) == 0) contents_[contentId(key)] = keptBytes(point);
        uint64_t total = completed_;
        for (const auto &entry : contents_) total += entry.second;
        progress_.received = std::min(progress_.total, total);
        if (progress_.total) progress_.percent = std::min(99., 100. * progress_.received / progress_.total);
      }
      // A failed item is tried again from the start: the engine skips the
      // content files it already finished, so this picks up where it broke.
      constexpr int Attempts = 3;
      int renewed = 0;
      for (int attempt = 1;; ++attempt) {
        const auto headers = net::downloadHeaders(server, state, files[i].url);
        const auto bearer = net::bearerOf(headers);
        const bool session = net::isSessionToken(bearer);
        const auto sent = session ? bearer : net::downloadToken();
        const auto refusals = net::sessionRefusals();
        try {
          nativeInstall(files[i], target, headers);
          break;
        } catch (const std::exception &) {
          // The download token (or the session token) ran out: a fresh one,
          // and the same file goes on from the same byte, not counted as a
          // failed try.
          const bool refused = session ? net::sessionRefusals() != refusals : net::downloadTokenRejected();
          if (refused && !cancel_ && renewed++ < 2 && net::renewDownloadToken(sent)) {
            --attempt;
            continue;
          }
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
    progress_.status = stop_ ? Status::Stopped : cancel_ ? Status::Cancelled : Status::Failed;
    progress_.error = e.what();
    progress_.phase = cancel_ ? tr("Install canceled.") : tr("Install failed.");
  }
}
} // namespace foil::install
