// The engine's downloads (util/network_util.hpp in engine-shim) on the app's
// own ordered stream.
#ifdef __SWITCH__
#include "install/EngineDownload.hpp"
#include "install/EngineHooks.hpp"
#include "core/I18n.hpp"
#include "net/Ordered.hpp"
#include "util/network_util.hpp"
#include <stdexcept>
namespace foil::install {
namespace {
net::OrderedOptions current;
}
void setEngineDownload(net::OrderedOptions options) {
  options.paused = [] {
    auto *sink = engineSink();
    return sink && sink->paused();
  };
  options.cancelled = [] {
    auto *sink = engineSink();
    return sink && sink->cancellationRequested();
  };
  current = std::move(options);
}
void clearEngineDownload() { current = {}; }
} // namespace foil::install
namespace tin::network {
HTTPDownload::HTTPDownload(std::string url) : m_url(std::move(url)) {
  const auto first = m_url.find_first_not_of(" \t\r\n"), last = m_url.find_last_not_of(" \t\r\n");
  m_url = first == std::string::npos ? std::string() : m_url.substr(first, last - first + 1);
  m_url.erase(std::min(m_url.find('#'), m_url.size()));
}
constexpr int HeaderRounds = 5;
void HTTPDownload::BufferDataRange(void *buffer, size_t offset, size_t size, std::function<void(size_t)>) {
  // A header read has no retry prompt of its own: it keeps going on its own
  // through a dropped connection for a few rounds of tries (minutes) before
  // the install says why it stopped. Cancel still ends it at once.
  auto options = foil::install::current;
  int rounds = 0;
  options.tryAgain = [&] { return ++rounds < HeaderRounds && !(options.cancelled && options.cancelled()); };
  if (!foil::net::orderedRead(m_url, offset, buffer, size, options)) {
    const auto why = foil::net::orderedFailure();
    throw std::runtime_error(why.empty() ? std::string(foil::tr("Cancelled")) : foil::trf("Lost the connection to the server while reading the file: {0}", {why}));
  }
}
int HTTPDownload::StreamDataRange(size_t offset, size_t size, std::function<size_t(u8 *, size_t)> streamFunc,
                                  std::function<bool()> retryConfirmFunc) {
  auto options = foil::install::current;
  options.tryAgain = retryConfirmFunc;
  const foil::net::OrderedSink sink = [&](const uint8_t *data, size_t length) {
    return streamFunc(const_cast<u8 *>(data), length);
  };
  return foil::net::orderedStream(m_url, offset, size, sink, options) ? 0 : 1;
}
} // namespace tin::network
#endif
