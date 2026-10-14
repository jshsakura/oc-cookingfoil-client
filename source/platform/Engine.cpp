#ifdef __SWITCH__
#include "core/Config.hpp"
#include "core/I18n.hpp"
#include "install/EngineHooks.hpp"
#include "install/Installer.hpp"
#include "install/http_nsp.hpp"
#include "install/http_xci.hpp"
#include "install/install_nsp.hpp"
#include "install/install_xci.hpp"
#include "nx/ipc/es.h"
#include "nx/ipc/ns_ext.h"
#include "ui/MainApplication.hpp"
#include "util/config.hpp"
#include "util/network_util.hpp"
#include "net/Segmented.hpp"
#include <cctype>
#include "net/Segments.hpp"
#include "core/DownloadStore.hpp"
#include "core/Json.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <map>
#include <filesystem>
#include "core/ZstdBlockDecoder.hpp"
#include "core/NczPoint.hpp"
#include "net/Socket.hpp"
#include <cstdio>
#include "core/Log.hpp"
#include "data/buffered_placeholder_writer.hpp"
#include <curl/curl.h>
#include <stdexcept>
namespace {
// 256MB and 32MB of download buffer.
// 64 MB as an application is plenty between the network and the card;
// 256 MB was a large block taken and freed for every NCA.
constexpr int ApplicationSegments = 8, AppletSegments = 4;
// The file being installed, for streamToPlace: its address, the headers
// every request carries, and how many connections it gets.
struct Current {
  std::string url;
  std::vector<std::string> headers;
  int connections = 1;
  bool application = false;
} current;
} // namespace
namespace tin::network {
void SetExtraHeaders(const std::vector<std::string> &headers,
                     const std::string &origin);
}
namespace inst::ui {
static MainApplication adapter;
MainApplication *mainApp = &adapter;
} // namespace inst::ui
namespace inst::util {
void playAudio(std::string) {}
std::string formatUrlString(std::string text) {
  auto query = text.find_first_of("?#");
  if (query != std::string::npos)
    text.erase(query);
  auto slash = text.find_last_of('/');
  if (slash != std::string::npos)
    text.erase(0, slash + 1);
  int size = 0;
  char *decoded = curl_easy_unescape(nullptr, text.c_str(), text.size(), &size);
  if (!decoded)
    return text;
  std::string result(decoded, size);
  curl_free(decoded);
  return result;
}
} // namespace inst::util
namespace foil::install {
class Services {
public:
  Services() {
    check(ncmInitialize(), 0);
    check(nsextInitialize(), 1);
    check(esInitialize(), 2);
    check(splCryptoInitialize(), 3);
    check(splInitialize(), 4);
  }
  ~Services() { close(); }

private:
  int count_ = 0;
  void check(Result rc, int stage) {
    if (R_FAILED(rc)) {
      close();
      throw std::runtime_error(foil::trf("Could not open the install service. Code {0} / {1}", {std::to_string(rc), std::to_string(stage)}));
    }
    ++count_;
  }
  void close() {
    if (count_ > 4)
      splExit();
    if (count_ > 3)
      splCryptoExit();
    if (count_ > 2)
      esExit();
    if (count_ > 1)
      nsextExit();
    if (count_ > 0)
      ncmExit();
    count_ = 0;
  }
};
void nativeInstall(const Item &item, const std::string &target,
                   const std::vector<std::string> &headers) {
  Services services;
  inst::ui::mainApp->ResetRetries();
  // With the check on, an NCA whose signature fails stops the install; with
  // it off (설정), the engine installs it as is and sigpatches let it run.
  auto *sink = foil::install::engineSink();
  inst::config::validateNCAs = !(sink && sink->skipSignatureCheck());
  // The download ring between curl and the placeholder writer, in 8MB
  // segments. CyberFoil sets this from its own menu, which is not built here;
  // left at 0 no data could ever be buffered and every download stalled.
  // Applet mode has a few hundred MB in all, so it gets a small ring.
  tin::data::NUM_BUFFER_SEGMENTS =
      appletGetAppletType() == AppletType_Application ? ApplicationSegments : AppletSegments;
  // Large files come down on several connections at once (each a 4 MB or
  // 2 MB piece held until its turn): 4 x 4 MB as an application, 4 x 2 MB
  // from the album, whose heap is small. A single connection rarely fills a
  // path with real latency, such as a player outside the server's network.
  // Connections by the server's address (2 on a private network, 4
  // outside). NCZ, which must arrive in order, uses them on a window in
  // front: 32 MB pieces as an application, 4 MB from the album.
  current.url = item.url;
  current.headers = headers;
  current.application = appletGetAppletType() == AppletType_Application;
  current.connections = foil::net::connectionsFor(item.url, 0);
  // Pieces of 8 MB as an application: the next byte in order is never far
  // off, so the bar and the card keep moving. A slow piece at the front
  // does not stop the others: an application holds up to two pieces a
  // connection (4 x 2 x 8 MB = 64 MB), the album one (16 MB).
  tin::network::SetParallelDownload(current.connections, current.application ? 0x800000 : 0x400000,
                                    current.application ? 2 : 1);
  // Off in 설정: the required system version is cleared so the console
  // launches the game anyway (it may still fail on firmware that is too old).
  const bool ignoreFirmware = sink && sink->skipFirmwareCheck();
  tin::network::ClearBasicAuth();
  tin::network::SetExtraHeaders(headers, originOf(item.url));
  struct Headers {
    ~Headers() {
      tin::network::SetExtraHeaders({}, "");
      tin::data::ReleaseSpareRing(); // kept between this file's NCAs only
    }
  } clear;
  const auto storage =
      target == "nand" ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
  auto path = item.url.substr(0, item.url.find_first_of("?#"));
  for (auto &c : path)
    c = std::tolower(static_cast<unsigned char>(c));
  // One line in the log per file once it is in: size, time, average speed,
  // connections, compressed or not.
  struct Summary {
    std::string name;
    int connections;
    bool compressed;
    uint64_t bytes = foil::net::receivedBytes(), tick = svcGetSystemTick();
    ~Summary() {
      const double seconds = double(svcGetSystemTick() - tick) / 19200000.0;
      const double mb = double(foil::net::receivedBytes() - bytes) / (1024 * 1024);
      char line[160];
      std::snprintf(line, sizeof(line), "download %.0f MB in %.0f s = %.1f MB/s, %d connections, %s: ", mb, seconds,
                    seconds > 0 ? mb / seconds : 0, connections, compressed ? "compressed" : "plain");
      logLine(line + name);
    }
  } summary{item.name, current.connections,
            path.size() >= 4 && (path.substr(path.size() - 4) == ".nsz" || path.substr(path.size() - 4) == ".xcz")};
  if (path.size() >= 4 && (path.substr(path.size() - 4) == ".xci" ||
                           path.substr(path.size() - 4) == ".xcz")) {
    auto file = std::make_shared<tin::install::xci::HTTPXCI>(item.url);
    tin::install::xci::XCIInstallTask task(storage, ignoreFirmware, file);
    task.Prepare();
    task.Begin();
  } else {
    auto file = std::make_shared<tin::install::nsp::HTTPNSP>(item.url);
    tin::install::nsp::NSPInstall task(storage, ignoreFirmware, file);
    task.Prepare();
    task.Begin();
  }
}
namespace {
// "sd:0123abcd…": the install's storage and the NCA's id, from its id or its
// file name, so the engine and the download agree on it.
std::string resumeKey(const std::string &nca) {
  auto *sink = engineSink();
  auto id = nca.substr(0, nca.find('.'));
  for (auto &c : id)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  return (sink ? sink->target() : std::string("sd")) + ":" + id;
}
} // namespace
bool resumable(const std::string &nca) {
  auto *sink = engineSink();
  return sink && sink->resumePoint(resumeKey(nca)).has_value();
}
namespace {
const std::string ResumeDir = "sdmc:/switch/cookingfoil/resume";
// The window files kept for `key`, by the decoded position each ends at.
std::map<uint64_t, std::string> windowFiles(const std::string &key) {
  std::map<uint64_t, std::string> out;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(ResumeDir, ec)) {
    std::string owner;
    uint64_t produced = 0;
    if (foil::windowFileOf(entry.path().filename().string(), owner, produced) && owner == key)
      out[produced] = entry.path().string();
  }
  return out;
}
} // namespace
void forgetResume(const std::string &nca) {
  if (auto *sink = engineSink())
    sink->forgetResumePoint(resumeKey(nca));
  std::error_code ec;
  for (const auto &[at, path] : windowFiles(resumeKey(nca)))
    std::filesystem::remove(path, ec);
}
bool nczPoint(const std::string &nca, uint64_t size, std::string &point, std::vector<uint8_t> &window) {
  auto *sink = engineSink();
  if (!sink || current.url.empty())
    return false;
  const auto key = resumeKey(nca);
  const auto kept = sink->resumePoint(key);
  foil::NczPoint at;
  if (!kept || kept->url != current.url || kept->total != size || !foil::NczPoint::isNcz(kept->plan) ||
      !foil::NczPoint::decode(kept->plan, at))
    return false;
  foil::ZstdBlockDecoder::Point state;
  if (!foil::ZstdBlockDecoder::pointOf(at.decoder, state))
    return false;
  const auto files = windowFiles(key);
  const auto file = files.find(at.produced);
  if (file == files.end())
    return false;
  const auto bytes = readFile(file->second);
  if (bytes.size() != state.window)
    return false;
  window.assign(bytes.begin(), bytes.end());
  point = kept->plan;
  logLine("download resumes at " + std::to_string((at.zstdStart + at.consumed) >> 20) + " MB (compressed): " + nca);
  return true;
}
void keepNczPoint(const std::string &nca, uint64_t size, const std::string &point, const std::vector<uint8_t> &window,
                  uint64_t produced) {
  auto *sink = engineSink();
  if (!sink || current.url.empty())
    return;
  const auto key = resumeKey(nca);
  makeDirectories(ResumeDir);
  const auto path = ResumeDir + "/" + foil::windowFileName(key, produced);
  // The window on the card before the point is kept: a point without its
  // window could not be gone on from.
  const int out = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0)
    return;
  size_t done = 0;
  while (done < window.size()) {
    const auto n = ::write(out, window.data() + done, window.size() - done);
    if (n <= 0)
      break;
    done += size_t(n);
  }
  const bool written = done == window.size() && ::fsync(out) == 0;
  if (::close(out) != 0 || !written) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return;
  }
  sink->keepResumePoint(key, {current.url, "", size, point});
  // The newest two stay: the database may still hold the one before.
  auto files = windowFiles(key);
  std::error_code ec;
  while (files.size() > 2) {
    std::filesystem::remove(files.begin()->second, ec);
    files.erase(files.begin());
  }
}
uint64_t nczMaxWindow() { return current.application ? 32ull << 20 : 8ull << 20; }
Placed streamToPlace(const std::string &content, uint64_t from, uint64_t size, const std::function<bool()> &prepare,
                     const std::function<bool(uint64_t, const char *, size_t)> &write,
                     const std::function<void()> &flush) {
  if (current.url.empty() || size == 0)
    return Placed::NotUsed;
  auto *sink = engineSink();
  foil::net::SegmentedOptions options;
  options.connections = current.connections;
  options.from = from;
  options.length = size;
  options.headers = current.headers;
  // Each connection gathers this much before a write; the writer holds at
  // most bufferBytes. Small from the album.
  options.batchBytes = current.application ? 4u << 20 : 2u << 20;
  options.bufferBytes = current.application ? 64ull << 20 : 8ull << 20;
  options.paused = [sink] { return sink && sink->paused(); };
  options.cancelled = [sink] { return sink && sink->cancellationRequested(); };
  const auto started = svcGetSystemTick();
  double speed = 0;
  uint64_t lastBytes = 0, lastTick = started;
  options.progress = [&](uint64_t received, uint64_t) {
    const uint64_t got = received > 0 ? received : 0;
    const uint64_t now = svcGetSystemTick();
    const double seconds = double(now - lastTick) / 19200000.0;
    if (seconds > 0.4) {
      const double recent = double(got - std::min(got, lastBytes)) / (1024.0 * 1024.0) / seconds;
      speed = speed > 0 ? speed * 0.7 + recent * 0.3 : recent;
      lastBytes = got;
      lastTick = now;
    }
    reportBytes(content, got, speed);
  };
  options.log = [](const std::string &line) { logLine(line); };
  // Cut off before: on from where each part stood, if the file is the same
  // one (the server's validator says so, or it starts over).
  const auto key = resumeKey(content);
  if (sink)
    if (const auto point = sink->resumePoint(key);
        point && point->url == current.url && point->total == from + size &&
        foil::net::SegmentPlan::load(point->plan, options.resume)) {
      options.resumeTotal = point->total;
      options.etag = point->etag;
      logLine("download resumes at " + std::to_string(foil::net::SegmentPlan(options.resume).received() >> 20) +
              " MB: " + content);
    }
  // Where it stands, every 10 s at most, once the bytes are on the card: each
  // save rewrites the database file, so not more often than that. The last
  // one heard is saved too when the download stops short.
  uint64_t savedTick = 0;
  Installer::ResumePoint latest;
  bool unsaved = false;
  const auto save = [&] {
    if (flush)
      flush();
    sink->keepResumePoint(key, latest);
    savedTick = svcGetSystemTick();
    unsaved = false;
  };
  if (sink)
    options.checkpoint = [&](const std::string &etag, uint64_t total, const std::string &plan) {
      if (latest.etag == etag && latest.total == total && latest.plan == plan)
        return; // nothing moved
      latest = {current.url, etag, total, plan};
      unsaved = true;
      if (!savedTick || svcGetSystemTick() - savedTick >= 10ull * 19200000)
        save();
    };
  const auto result = foil::net::segmentedFetch(
      current.url, [&](uint64_t) { return prepare(); },
      [&](uint64_t offset, const char *data, size_t length) { return offset >= from && write(offset - from, data, length); },
      options);
  if (result.cancelled) {
    if (sink && !sink->stopping())
      sink->forgetResumePoint(key);
    else if (sink && unsaved)
      save(); // stopped: goes on from here next time
    return Placed::Cancelled;
  }
  if (!result.ok) {
    // The point stays: the next try goes on from it.
    if (sink && unsaved)
      save();
    logLine("segmented download failed: " + result.error + " status=" + std::to_string(result.status));
    return Placed::Failed;
  }
  if (sink)
    sink->forgetResumePoint(key);
  reportBytes(content, size, speed);
  return Placed::Done;
}
} // namespace foil::install
#endif
