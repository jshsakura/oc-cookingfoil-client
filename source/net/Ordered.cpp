#include "net/Ordered.hpp"
#include "net/Direct.hpp"
#include "net/Socket.hpp"
#include "net/Tuner.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
namespace foil::net {
namespace {
using Clock = std::chrono::steady_clock;
using std::chrono::seconds;
// While bytes came within this long, a drop is not a failed try.
constexpr auto ResumeWindow = seconds(60);
// A turn this long after the last one means the console slept.
constexpr auto SleepGap = seconds(10);
constexpr int Tries = 3, Renewals = 3;
// What a request carries where it goes: on the direct address the token
// alone, on the server the download's headers with the token as it is now,
// anywhere else nothing.
curl_slist *requestHeaders(const std::string &url, const std::string &sent, const OrderedOptions &options) {
  std::vector<std::string> headers;
  if (!isDirect(sent) && originOf(sent) == originOf(url)) {
    headers = options.headers;
    freshenBearer(sent, headers);
  }
  curl_slist *list = nullptr;
  for (const auto &header : headersFor(sent, headers))
    if (header.find_first_of("\r\n") == std::string::npos) list = curl_slist_append(list, header.c_str());
  return list;
}
bool stopping(const OrderedOptions &o) { return o.cancelled && o.cancelled(); }
bool paused(const OrderedOptions &o) { return o.paused && o.paused(); }
void waitWhilePaused(const OrderedOptions &o) {
  while (paused(o) && !stopping(o)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
int abortTransfer(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto &o = *static_cast<const OrderedOptions *>(user);
  return stopping(o) || paused(o) ? 1 : 0;
}
// The options every request carries: TLS, timeouts, cancel, no redirects,
// large reads, the last download's connections.
void setUp(CURL *curl, const std::string &sent, const std::string &range, curl_slist *headers,
           const OrderedOptions &options) {
  curl_easy_setopt(curl, CURLOPT_URL, sent.c_str());
#ifdef __SWITCH__
  curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortTransfer);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &options);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent().c_str());
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
  curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  shareConnections(curl);
  watchRestart(curl);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
}
std::string rangeOf(uint64_t from, uint64_t end) { return std::to_string(from) + "-" + std::to_string(end - 1); }

// One piece of the line: its bytes are kept until it reaches the front.
struct Piece {
  CURL *curl = nullptr;
  curl_slist *headers = nullptr;
  std::string range, token, sent;
  uint64_t start = 0, length = 0;
  std::vector<uint8_t> data;
  int attempts = 0, renewals = 0;
  bool done = false, checked = false;
  Clock::time_point *lastData = nullptr, retryAt{};
  const OrderedOptions *options = nullptr;
};
size_t collect(char *bytes, size_t size, size_t count, void *user) {
  auto &piece = *static_cast<Piece *>(user);
  const size_t n = size * count;
  if (stopping(*piece.options) || piece.data.size() + n > piece.length) return 0;
  // An error page is not the piece's bytes: fail, and ask again.
  if (!piece.checked) {
    long code = 0;
    curl_easy_getinfo(piece.curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 206) return 0;
    piece.checked = true;
  }
  piece.data.insert(piece.data.end(), bytes, bytes + n);
  countReceived(n);
  *piece.lastData = Clock::now();
  return n;
}
// Consecutive pieces on several connections, handed to the sink strictly in
// order; a piece that fails is asked for again from where it stopped. A slow
// piece at the front does not hold the others: they go on to the next ones,
// up to `ahead` pieces a connection held in all. Returns the bytes handed on.
uint64_t streamPieces(const std::string &url, uint64_t from, uint64_t length, const OrderedSink &sink,
                      const OrderedOptions &options) {
  CURLM *multi = curl_multi_init();
  if (!multi) return 0;
  std::deque<std::unique_ptr<Piece>> line;
  const auto drop = [&](Piece &piece) {
    if (piece.curl) {
      curl_multi_remove_handle(multi, piece.curl);
      curl_easy_cleanup(piece.curl);
      piece.curl = nullptr;
    }
    if (piece.headers) curl_slist_free_all(piece.headers);
    piece.headers = nullptr;
  };
  const auto issue = [&](Piece &piece) {
    drop(piece);
    piece.curl = curl_easy_init();
    if (!piece.curl) return false;
    piece.checked = false;
    piece.range = rangeOf(piece.start + piece.data.size(), piece.start + piece.length);
    piece.token = downloadToken(url);
    piece.sent = routeDownload(url);
    piece.headers = requestHeaders(url, piece.sent, options);
    setUp(piece.curl, piece.sent, piece.range, piece.headers, options);
    curl_easy_setopt(piece.curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(piece.curl, CURLOPT_WRITEDATA, &piece);
    curl_easy_setopt(piece.curl, CURLOPT_PRIVATE, &piece);
    return curl_multi_add_handle(multi, piece.curl) == CURLM_OK;
  };
  const uint64_t end = from + length;
  uint64_t next = from, delivered = 0;
  bool failed = false, offlineSaid = false;
  auto lastData = Clock::now(), lastTurn = lastData;
  while (delivered < length && !failed) {
    const auto turn = Clock::now();
    if (turn - lastTurn > SleepGap) {
      logLine("download went on after " +
              std::to_string(std::chrono::duration_cast<seconds>(turn - lastTurn).count()) + " s away");
      lastData = turn;
    }
    lastTurn = turn;
    // No network: nothing is asked for, and the outage does not count.
    const bool offline = linkDown(url);
    if (offline) {
      lastData = Clock::now();
      if (!offlineSaid) logDetail("download waits for the network");
      offlineSaid = true;
    } else if (offlineSaid) {
      logDetail("network back; download goes on");
      offlineSaid = false;
    }
    const auto reading = [&] {
      int count = 0;
      for (const auto &piece : line) count += !piece->done;
      return count;
    };
    const int tuned = tunedConnections();
    const int connections = tuned > 0 ? std::min(tuned, options.connections) : options.connections;
    while (!offline && reading() < connections && int(line.size()) < connections * options.ahead && next < end) {
      auto piece = std::make_unique<Piece>();
      piece->start = next;
      piece->length = std::min<uint64_t>(options.chunk, end - next);
      piece->lastData = &lastData;
      piece->options = &options;
      piece->data.reserve(piece->length);
      next += piece->length;
      if (!issue(*piece)) {
        failed = true;
        break;
      }
      line.push_back(std::move(piece));
    }
    // Paused: every connection goes; the pieces keep what they got and ask
    // for the rest once it resumes.
    if (paused(options)) {
      for (auto &piece : line)
        if (!piece->done) drop(*piece);
      waitWhilePaused(options);
      if (stopping(options)) break;
      lastData = lastTurn = Clock::now(); // the pause is no silence
    }
    const auto now = Clock::now();
    for (auto &piece : line)
      if (!offline && !piece->done && !piece->curl && piece->retryAt <= now && !issue(*piece)) failed = true;
    int running = 0, left = 0;
    curl_multi_perform(multi, &running);
    while (CURLMsg *msg = curl_multi_info_read(multi, &left)) {
      if (msg->msg != CURLMSG_DONE) continue;
      Piece *piece = nullptr;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &piece);
      long code = 0;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &code);
      if (linkDown(url)) lastData = Clock::now();
      if (msg->data.result == CURLE_OK && code == 206 && piece->data.size() == piece->length) {
        piece->done = true;
        drop(*piece);
      } else if (paused(options) && !stopping(options)) {
        drop(*piece); // asked again after the pause
      } else if (!stopping(options) && afterDirectFailure(piece->sent, code, piece->token) == DirectStep::Again) {
        // A 401 on the direct address a fresh token answers: at once, from
        // the same byte, not as a failure.
        drop(*piece);
        piece->retryAt = {};
      } else if (code == 401 && !stopping(options) && piece->renewals < Renewals &&
                 renewDownloadToken(piece->sent, piece->token)) {
        ++piece->renewals;
        drop(*piece);
        piece->retryAt = {};
      } else if (code == 401 || code == 403 || code == 404 || code == 416 || stopping(options) ||
                 (Clock::now() - lastData > ResumeWindow && ++piece->attempts > Tries)) {
        failed = true;
      } else {
        // Again after a wait: as long as Retry-After says when the server is
        // busy (503), rate limited (429) or the tunnel timed out (524), else
        // 5 s; 2 s for a dropped connection, longer the longer nothing came.
        curl_off_t serverWait = 0;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_RETRY_AFTER, &serverWait);
        drop(*piece);
        if (!linkDown(piece->sent)) tuneTrouble(); // one fewer may do better
        const bool busy = code == 503 || code == 429 || code == 524;
        const auto quiet = std::chrono::duration_cast<seconds>(Clock::now() - lastData).count();
        const auto wait = busy ? (serverWait > 0 ? std::min<long long>(serverWait, 60) : 5)
                               : std::clamp<long long>(quiet / 4, 2, 15);
        piece->retryAt = Clock::now() + seconds(wait);
      }
    }
    // Whatever is complete at the front goes on, in order.
    while (!line.empty() && line.front()->done) {
      auto &piece = *line.front();
      size_t sent = 0;
      try {
        sent = sink(piece.data.data(), piece.data.size());
      } catch (...) {
      }
      if (sent != piece.data.size()) {
        failed = true;
        break;
      }
      delivered += sent;
      line.pop_front();
    }
    if (stopping(options)) failed = true;
    if (!failed && delivered < length) curl_multi_poll(multi, nullptr, 0, 100, nullptr);
  }
  for (auto &piece : line) drop(*piece);
  curl_multi_cleanup(multi);
  return delivered;
}

// One request for the range, its bytes straight to the sink. 0 when all of
// it came, else the HTTP status, 1000 + curl's code, or SinkFailed.
constexpr int SinkFailed = 1999;
thread_local std::string lastFailure;
std::string describe(int rc) {
  if (rc == SinkFailed) return "the install stopped taking data";
  if (rc >= 1000) return curl_easy_strerror(CURLcode(rc - 1000));
  return "HTTP " + std::to_string(rc);
}
struct Single {
  const OrderedSink *sink;
  const OrderedOptions *options;
  CURL *curl;
  bool checked = false, threw = false;
};
size_t pass(char *bytes, size_t size, size_t count, void *user) {
  auto &s = *static_cast<Single *>(user);
  if (stopping(*s.options)) return 0;
  // Only a 206 carries the bytes asked for: an error page (503, 429, 524, a
  // proxy's 5xx) fails the request, asked again from the same byte.
  if (!s.checked) {
    long code = 0;
    curl_easy_getinfo(s.curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 206) return 0;
    s.checked = true;
  }
  try {
    return (*s.sink)(reinterpret_cast<const uint8_t *>(bytes), size * count);
  } catch (...) {
    s.threw = true;
    return 0;
  }
}
int requestOnce(const std::string &url, const std::string &sent, uint64_t from, uint64_t length,
                const OrderedSink &sink, const OrderedOptions &options) {
  CURL *curl = curl_easy_init();
  if (!curl) return 1000 + CURLE_FAILED_INIT;
  const auto range = rangeOf(from, from + length);
  curl_slist *headers = requestHeaders(url, sent, options);
  setUp(curl, sent, range, headers, options);
  Single single{&sink, &options, curl};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, pass);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &single);
  const CURLcode rc = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (single.threw) return SinkFailed;
  if (rc == CURLE_OK && code == 206) return 0;
  if (code != 0 && code != 206) return int(code);
  return 1000 + int(rc);
}
void sleepUnlessStopping(seconds wait, const OrderedOptions &options) {
  for (auto left = std::chrono::duration_cast<std::chrono::milliseconds>(wait); left.count() > 0 && !stopping(options);
       left -= std::chrono::milliseconds(100))
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
// The range on one connection, going on from the byte it stopped at.
bool streamOne(const std::string &url, uint64_t from, uint64_t length, const OrderedSink &sink,
               const OrderedOptions &options) {
  uint64_t received = 0;
  const OrderedSink counted = [&](const uint8_t *data, size_t size) {
    const size_t taken = sink(data, size);
    received += taken;
    return taken;
  };
  auto lastData = Clock::now();
  std::string lastWhere;
  for (;;) {
    // Before the next ask: 2 s unless the last one said otherwise (0: at once).
    seconds wait = seconds(2);
    int renewals = 0;
    for (int attempt = 0; attempt <= Tries; ++attempt) {
      if (attempt > 0) sleepUnlessStopping(wait, options);
      waitWhilePaused(options);
      if (stopping(options)) return false;
      if (received == length) return true;
      const uint64_t before = received;
      const auto token = downloadToken(url);
      const auto sent = routeDownload(url);
      const int rc = requestOnce(url, sent, from + received, length - received, counted, options);
      if (rc == 0) return true;
      lastFailure = describe(rc);
      const auto where = std::string(sent == url ? "server" : "direct") + " at " + std::to_string(from + received) + ", " +
                         std::to_string(received) + " of " + std::to_string(length);
      lastWhere = where;
      wait = seconds(0);
      // Not a try, again at once from the same byte: a 401 on the direct
      // address a fresh token answers, a pause, a token that ran out.
      if (rc == 401 && !stopping(options) && afterDirectFailure(sent, rc, token) == DirectStep::Again) {
        --attempt;
        continue;
      }
      if (paused(options) && !stopping(options)) {
        --attempt;
        continue;
      }
      // The server restarting (or no internet): waited out, not a try.
      if (linkDown(url) && !stopping(options)) {
        --attempt;
        wait = seconds(2);
        continue;
      }
      if (rc == 401 && !stopping(options) && renewals < Renewals && renewDownloadToken(url, token)) {
        ++renewals;
        --attempt;
        continue;
      }
      if (rc == SinkFailed || rc == 401 || rc == 403 || rc == 404 || rc == 416 || rc == 1000 + CURLE_WRITE_ERROR) {
        logLine("download stopped: " + lastFailure + " (" + where + ")");
        return false;
      }
      // Busy (503), rate limited (429) or a slow first byte through the
      // tunnel (524): a longer wait.
      wait = rc == 503 || rc == 429 || rc == 524 ? seconds(5) : seconds(2);
      // Within the window, not a try: again after a wait that grows the
      // longer nothing has come (2 s to 15 s).
      if (received > before) lastData = Clock::now();
      if (Clock::now() - lastData < ResumeWindow) {
        const auto quiet = std::chrono::duration_cast<seconds>(Clock::now() - lastData).count();
        sleepUnlessStopping(seconds(std::clamp<long long>(quiet / 4, 2, 15)), options);
        wait = seconds(0);
        --attempt;
        continue;
      }
    }
    logLine("download gave up after its tries: " + lastFailure + " (" + lastWhere + ")");
    if (!(options.tryAgain && options.tryAgain())) return false;
  }
}
} // namespace
std::string orderedFailure() { return lastFailure; }
bool orderedStream(const std::string &url, uint64_t from, uint64_t length, const OrderedSink &sink,
                   const OrderedOptions &options) {
  lastFailure.clear();
  if (length == 0) return true;
  // Large ranges first on several connections; what is left after a
  // failure goes on one, with its tries.
  if (options.connections > 1 && length >= 4 * uint64_t(options.chunk)) {
    const uint64_t done = streamPieces(url, from, length, sink, options);
    if (done == length) return true;
    if (stopping(options)) return false;
    from += done;
    length -= done;
  }
  return streamOne(url, from, length, sink, options);
}
bool orderedRead(const std::string &url, uint64_t from, void *out, size_t length, const OrderedOptions &options) {
  size_t read = 0;
  auto *bytes = static_cast<uint8_t *>(out);
  const OrderedSink fill = [&](const uint8_t *data, size_t size) -> size_t {
    if (read + size > length) return 0;
    std::memcpy(bytes + read, data, size);
    read += size;
    return size;
  };
  OrderedOptions one = options;
  one.connections = 1;
  return orderedStream(url, from, length, fill, one) && read == length;
}
} // namespace foil::net
