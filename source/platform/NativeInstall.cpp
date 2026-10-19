#include "install/Installer.hpp"
#include "core/I18n.hpp"
#ifndef __SWITCH__
#include "core/Log.hpp"
#include "install/EngineHooks.hpp"
#include "net/Segmented.hpp"
#include "net/Direct.hpp"
#include "net/Tuner.hpp"
#include "core/Config.hpp"
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
namespace foil::install {
namespace {
std::mutex hostLock;
std::function<void(const Item &)> hostInstall;
// The preview has nothing to install to: it downloads the file on the
// console's own engine (its connections, shared pool and retries) and
// throws the bytes away. The download card shows what this code reaches
// from here.
void benchmark(const Item &item, const std::vector<std::string> &headers) {
  auto *sink = engineSink();
  const auto &from = item.url;
  net::SegmentedOptions options;
  options.connections = net::connectionsFor(from, 0);
  options.headers = headers;
  struct Tuning {
    int id;
    ~Tuning() { net::stopTuning(id); }
  } tuning{net::startTuning(options.connections, originOf(net::routeDownload(from)))};
  options.paused = [sink] { return sink && sink->paused(); };
  options.cancelled = [sink] { return sink && sink->cancellationRequested(); };
  const auto started = std::chrono::steady_clock::now();
  const auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };
  options.progress = [&](uint64_t received, uint64_t) {
    const double span = seconds();
    reportBytes(item.url, received, span > 0 ? double(received) / (1024 * 1024) / span : 0);
  };
  options.log = [](const std::string &line) { logLine(line); };
  const auto result = net::segmentedFetch(
      from, [](uint64_t) { return true; }, [](uint64_t, const char *, size_t) { return true; }, options);
  if (result.cancelled)
    throw std::runtime_error(tr("Install canceled."));
  if (!result.ok)
    throw std::runtime_error(result.error.empty() ? "status " + std::to_string(result.status) : result.error);
  const double span = seconds(), mb = double(result.total) / (1024 * 1024);
  reportBytes(item.url, result.total, span > 0 ? mb / span : 0);
}
} // namespace
// Off the console an install does what a test says (wait, then succeed or
// throw); with nothing set it downloads the file for the preview's speed.
void setHostInstall(std::function<void(const Item &)> run) {
  std::lock_guard<std::mutex> held(hostLock);
  hostInstall = std::move(run);
}
void nativeInstall(const Item &item, const std::string &,
                   const std::vector<std::string> &headers) {
  std::function<void(const Item &)> run;
  {
    std::lock_guard<std::mutex> held(hostLock);
    run = hostInstall;
  }
  if (!run)
    return benchmark(item, headers);
  run(item);
}
} // namespace foil::install
#endif
