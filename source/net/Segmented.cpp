#include "net/Segmented.hpp"
#include "net/Direct.hpp"
#include "net/Tuner.hpp"
#include "net/Socket.hpp"
#include "platform/Cores.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
namespace foil::net {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int TokenRetries = 32; // parts refused for a stale download token, all told
constexpr int MaxFails = 3;          // a part failing this often ends the parallel phase
// While bytes arrived this recently, a dropped connection or a server error
// is the network coming back (a console rejoining Wi-Fi takes about this
// long): asked again from the same byte, not counted as a failure.
constexpr auto ResumeWindow = std::chrono::seconds(60);
constexpr auto RetryGap = std::chrono::seconds(2);
// After a 429 the connections come back one at a time, each after this long
// without another: a limit that has passed does not keep the speed down.
constexpr auto LimitCalm = std::chrono::seconds(60);
constexpr auto ProgressEvery = std::chrono::milliseconds(250);
constexpr auto CheckpointEvery = std::chrono::seconds(2);
// A turn of the loop this long after the last one: the console slept (or
// the app was in the background), and no part had a chance to try.
constexpr auto SleepGap = std::chrono::seconds(10);
std::atomic<uint64_t> checkpointAsked{0}, checkpointGiven{0};

// How long the download stood still because the card (or the install
// behind it) had not taken what came: the log tells a slow card from a slow
// line with it.
std::atomic<uint64_t> heldMicros{0};
// The writer thread: bytes in place, in the order they were handed over,
// at most `limit` bytes held. A failed write stops everything.
class Writer {
public:
  Writer(const std::function<bool(uint64_t, const char *, size_t)> &write, uint64_t limit)
      : write_(write), limit_(limit), thread_([this] { run(); }) {}
  ~Writer() { finish(); }
  // False once a write failed.
  bool put(uint64_t offset, std::vector<char> &&data, std::function<void()> done) {
    std::unique_lock<std::mutex> held(lock_);
    const auto room = [&] { return held_ + data.size() <= limit_ || queue_.empty() || failed_; };
    if (!room()) {
      const auto from = std::chrono::steady_clock::now();
      changed_.wait(held, room);
      heldMicros += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count());
    }
    if (failed_)
      return false;
    held_ += data.size();
    queue_.push_back({offset, std::move(data), std::move(done)});
    changed_.notify_all();
    return true;
  }
  // Waits until everything handed over is written; false if a write failed.
  bool drain() {
    std::unique_lock<std::mutex> held(lock_);
    changed_.wait(held, [&] { return (queue_.empty() && !busy_) || failed_; });
    return !failed_;
  }
  bool failed() {
    std::lock_guard<std::mutex> held(lock_);
    return failed_;
  }
  void finish() {
    {
      std::lock_guard<std::mutex> held(lock_);
      stop_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable())
      thread_.join();
  }

private:
  struct Job {
    uint64_t offset;
    std::vector<char> data;
    std::function<void()> done;
  };
  void run() {
    const platform::OnCore off(platform::WriterCore);
    std::unique_lock<std::mutex> held(lock_);
    for (;;) {
      changed_.wait(held, [&] { return !queue_.empty() || stop_; });
      if (queue_.empty())
        return;
      Job job = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
      held.unlock();
      const bool ok = write_(job.offset, job.data.data(), job.data.size());
      if (ok && job.done)
        job.done();
      held.lock();
      busy_ = false;
      held_ -= job.data.size();
      if (!ok)
        failed_ = true;
      changed_.notify_all();
    }
  }
  const std::function<bool(uint64_t, const char *, size_t)> &write_;
  const uint64_t limit_;
  std::mutex lock_;
  std::condition_variable changed_;
  std::deque<Job> queue_;
  uint64_t held_ = 0;
  bool busy_ = false, failed_ = false, stop_ = false;
  std::thread thread_;
};

// One connection reading one part.
struct Conn {
  CURL *curl = nullptr;
  curl_slist *headers = nullptr;
  int seg = -1;
  uint64_t pos = 0, stop = 0; // next byte expected, and where this part now ends
  bool openEnded = false;     // the first request (bytes=0-): its reply tells the size
  bool checked = false;       // the reply's status and range were looked at
  bool cut = false;           // stopped on purpose at the part's end
  bool bad = false;           // the reply was not the bytes asked for
  bool changed = false;       // 200 to an If-Range request: the server's file changed
  long status = 0;
  std::string contentRange, etag, retryAfter, range;
  std::string token; // the download token it was sent with
  std::string url;   // where it was asked for: the direct address or the server's
  std::vector<char> pending;
};
struct Session;
size_t onHeader(char *data, size_t size, size_t count, void *user);
size_t onBody(char *data, size_t size, size_t count, void *user);

struct Session {
  const std::string &url;
  size_t batch;
  const SegmentedOptions &options;
  Writer &writer;
  std::mutex &planLock;
  SegmentPlan &plan;
  std::vector<uint64_t> &written; // per part: bytes in place from its start
  uint64_t total = 0;
  bool known = false;    // the size is known
  bool prepared = false; // the place for the bytes is ready: they may go to the writer
  bool ranged = true;   // the server answers ranges
  std::string etag;
  std::chrono::steady_clock::time_point lastData = std::chrono::steady_clock::now();
};

bool hand(Session &s, Conn &c) {
  // Before the place is ready the first reply's bytes wait here.
  if (c.pending.empty() || !s.prepared)
    return true;
  const uint64_t offset = c.pos - c.pending.size();
  const int seg = c.seg;
  const uint64_t end = c.pos;
  std::vector<char> data;
  data.swap(c.pending);
  c.pending.reserve(s.batch);
  // What runs once the bytes are on the card holds the plan's lock and list,
  // which outlive the session (the writer thread may finish after it).
  auto &planLock = s.planLock;
  auto &written = s.written;
  return s.writer.put(offset, std::move(data), [&planLock, &written, seg, end] {
    std::lock_guard<std::mutex> held(planLock);
    if (seg >= 0 && size_t(seg) < written.size())
      written[seg] = std::max(written[seg], end);
  });
}

void parseRange(const std::string &value, uint64_t &start, uint64_t &total) {
  // "bytes a-b/total"
  unsigned long long a = 0, b = 0, t = 0;
  if (std::sscanf(value.c_str(), "bytes %llu-%llu/%llu", &a, &b, &t) == 3) {
    start = a;
    total = t;
  }
}

size_t onHeaderUnsafe(char *data, size_t size, size_t count, void *user);
// curl is C: an exception thrown through it ends the app. A callback that
// fails (out of memory) fails the transfer instead.
size_t onHeader(char *data, size_t size, size_t count, void *user) {
  try {
    return onHeaderUnsafe(data, size, count, user);
  } catch (...) {
    return 0;
  }
}
size_t onHeaderUnsafe(char *data, size_t size, size_t count, void *user) {
  heardHeader(data, size * count);
  auto &c = *static_cast<Conn *>(user);
  std::string line(data, size * count);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  if (line.rfind("HTTP/", 0) == 0) { // a new reply (after a 100 or a redirect): start over
    c.contentRange.clear();
    c.etag.clear();
    c.retryAfter.clear();
  }
  const auto colon = line.find(':');
  if (colon == std::string::npos)
    return size * count;
  std::string key = line.substr(0, colon), value = line.substr(colon + 1);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
  value.erase(0, value.find_first_not_of(' '));
  if (key == "content-range")
    c.contentRange = value;
  else if (key == "etag")
    c.etag = value;
  else if (key == "retry-after")
    c.retryAfter = value;
  return size * count;
}

struct BodyContext {
  Session *session;
  Conn *conn;
};

size_t onBodyUnsafe(char *data, size_t size, size_t count, void *user);
size_t onBody(char *data, size_t size, size_t count, void *user) {
  try {
    return onBodyUnsafe(data, size, count, user);
  } catch (...) {
    return 0;
  }
}
size_t onBodyUnsafe(char *data, size_t size, size_t count, void *user) {
  auto &ctx = *static_cast<BodyContext *>(user);
  auto &s = *ctx.session;
  auto &c = *ctx.conn;
  const size_t n = size * count;
  if (!c.checked) {
    c.checked = true;
    curl_easy_getinfo(c.curl, CURLINFO_RESPONSE_CODE, &c.status);
    uint64_t start = 0, total = 0;
    parseRange(c.contentRange, start, total);
    if (c.openEnded && !s.known) {
      if (c.status == 200) { // no ranges: one connection to the end
        curl_off_t length = -1;
        curl_easy_getinfo(c.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        s.ranged = false;
        s.total = length > 0 ? uint64_t(length) : 0;
      } else if (c.status == 206 && total > 0 && start == c.pos) {
        s.total = total;
      } else {
        c.bad = true;
        return 0;
      }
      s.etag = c.etag;
      s.known = true; // the caller plans the parts; these bytes are part 0's
    }
    else if (c.status == 200 && !c.openEnded) { // If-Range failed: the file changed
      c.changed = true;
      return 0;
    } else if (c.status != 206 || start != c.pos) {
      c.bad = true;
      return 0;
    } else if (s.etag.empty() && !c.etag.empty()) {
      // A known stretch has no first probe: its first reply gives the
      // validator a resume point keeps.
      s.etag = c.etag;
    }
  }
  const uint64_t room = c.stop > c.pos ? c.stop - c.pos : 0;
  const size_t take = size_t(std::min<uint64_t>(room, n));
  c.pending.insert(c.pending.end(), data, data + take);
  countReceived(take);
  if (take)
    s.lastData = std::chrono::steady_clock::now();
  c.pos += take;
  {
    std::lock_guard<std::mutex> held(s.planLock);
    auto &part = s.plan.segments()[size_t(c.seg)];
    part.next = std::max(part.next, c.pos);
  }
  if (c.pending.size() >= s.batch && !hand(s, c))
    return 0;
  if (c.pos >= c.stop && s.ranged) {
    c.cut = true; // its part is done; the rest of an open-ended reply is not wanted
    return take == n ? n : 0;
  }
  return n;
}

std::string rangeOf(uint64_t from, uint64_t end, bool openEnded) {
  return std::to_string(from) + "-" + (openEnded ? std::string() : std::to_string(end - 1));
}
} // namespace

double cardHeldSeconds() { return double(heldMicros.load()) / 1e6; }
void checkpointSoon() { ++checkpointAsked; }
uint64_t checkpointsAsked() { return checkpointAsked.load(); }
uint64_t checkpointsServed() { return checkpointGiven.load(); }
SegmentedResult segmentedFetch(const std::string &url, const std::function<bool(uint64_t total)> &prepare,
                               const std::function<bool(uint64_t offset, const char *data, size_t size)> &write,
                               const SegmentedOptions &options) {
  // Every connection's TLS runs on this thread.
  const platform::OnCore off(platform::NetworkCore);
  SegmentedResult result;
  const auto log = [&](const std::string &line) {
    if (options.log)
      options.log(line);
  };
  const auto cancelled = [&] { return options.cancelled && options.cancelled(); };
  const auto paused = [&] { return options.paused && options.paused(); };
  std::mutex planLock;
  SegmentPlan plan(std::vector<Segment>{});
  std::vector<uint64_t> written;
  bool restarted = false;
  // The headers sent; the download token in them is swapped when it ran out.
  std::vector<std::string> sendHeaders = options.headers;
  int renewals = 0; // every part refused may come back a few times

  for (;;) { // a second round only when the server's file changed
    const size_t batch = std::max<size_t>(options.batchBytes, 256u << 10);
    Writer writer(write, std::max<uint64_t>(options.bufferBytes, 2 * batch));
    Session s{url, batch, options, writer, planLock, plan, written};
    // options.connections is the most; a download being tuned starts at
    // what the tuner says and follows it (net/Tuner.hpp).
    int limit = std::max(1, options.connections);
    if (const int tuned = tunedConnections(); tuned > 0) limit = std::min(limit, tuned);
    int wanted = limit; // what the limit returns to once 429s stop
    Clock::time_point limitedAt;
    const bool resuming = !restarted && !options.resume.empty() && options.resumeTotal > 0;
    std::vector<std::unique_ptr<Conn>> conns;
    std::vector<std::unique_ptr<BodyContext>> contexts;
    std::vector<int> fails;
    std::vector<Clock::time_point> waitUntil;
    CURLM *multi = curl_multi_init();
    if (!multi) {
      result.error = "curl";
      return result;
    }
    const auto open = [&](int seg, uint64_t from, uint64_t end, bool openEnded) -> bool {
      auto c = std::make_unique<Conn>();
      c->curl = curl_easy_init();
      if (!c->curl)
        return false;
      c->seg = seg;
      c->pos = from;
      c->stop = end;
      c->openEnded = openEnded;
      c->pending.reserve(s.batch);
      c->range = rangeOf(from, end, openEnded);
      // The direct address when the server named one; its keys with it.
      c->url = routeDownload(url);
      const auto ask = headersFor(c->url, sendHeaders);
      c->token = bearerOf(ask);
      for (const auto &h : ask)
        if (h.find_first_of("\r\n") == std::string::npos)
          c->headers = curl_slist_append(c->headers, h.c_str());
      if (!openEnded && !s.etag.empty())
        c->headers = curl_slist_append(c->headers, ("If-Range: " + s.etag).c_str());
      CURL *curl = c->curl;
      curl_easy_setopt(curl, CURLOPT_URL, c->url.c_str());
#ifdef __SWITCH__
      curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, c->headers);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "CookingFoil/" COOKINGFOIL_VERSION);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
      curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024);
      curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, &tuneSocket);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
      shareConnections(curl); // the last file's connections, still warm
      curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
      curl_easy_setopt(curl, CURLOPT_RANGE, c->range.c_str());
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
      curl_easy_setopt(curl, CURLOPT_HEADERDATA, c.get());
      auto ctx = std::make_unique<BodyContext>(BodyContext{&s, c.get()});
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, ctx.get());
      curl_easy_setopt(curl, CURLOPT_PRIVATE, c.get());
      if (options.configure)
        options.configure(curl);
      if (curl_multi_add_handle(multi, curl) != CURLM_OK)
        return false;
      contexts.push_back(std::move(ctx));
      conns.push_back(std::move(c));
      return true;
    };
    const auto close = [&](Conn &c) {
      if (c.curl) {
        curl_multi_remove_handle(multi, c.curl);
        curl_easy_cleanup(c.curl);
        c.curl = nullptr;
      }
      if (c.headers)
        curl_slist_free_all(c.headers);
      c.headers = nullptr;
      // Its bytes went to the writer already; the room it reserved goes too
      // (a long file opens many parts, each kept in the list).
      std::vector<char>().swap(c.pending);
    };
    // Parts being read, or waiting out a 503/524/429 before they are asked again.
    const auto busyParts = [&] {
      std::vector<bool> busy(plan.segments().size(), false);
      for (auto &c : conns)
        if (c->curl && c->seg >= 0 && size_t(c->seg) < busy.size())
          busy[size_t(c->seg)] = true;
      const auto now = Clock::now();
      for (size_t i = 0; i < busy.size() && i < waitUntil.size(); ++i)
        if (waitUntil[i] > now)
          busy[i] = true;
      return busy;
    };
    const auto active = [&] {
      return int(std::count_if(conns.begin(), conns.end(), [](const auto &c) { return c->curl != nullptr; }));
    };
    // A part to read for a free connection: an idle one, else the back half of
    // the one with the most left (its reader now stops where that begins).
    const auto assign = [&]() -> bool {
      std::vector<uint64_t> ends;
      int seg;
      {
        std::lock_guard<std::mutex> held(planLock);
        for (const auto &p : plan.segments())
          ends.push_back(p.end);
        seg = plan.take(busyParts());
        if (seg < 0)
          return false;
        written.resize(plan.segments().size(), 0);
        if (size_t(seg) >= ends.size())
          written[size_t(seg)] = plan.segments()[size_t(seg)].start;
        for (size_t i = 0; i < ends.size(); ++i)
          if (plan.segments()[i].end != ends[i])
            for (auto &c : conns)
              if (c->seg == int(i))
                c->stop = plan.segments()[i].end;
      }
      const auto &part = plan.segments()[size_t(seg)];
      fails.resize(plan.segments().size(), 0);
      waitUntil.resize(plan.segments().size(), Clock::time_point());
      return open(seg, part.next, part.end, false);
    };

    bool started = false;
    if (resuming) {
      plan = SegmentPlan(options.resume);
      s.total = options.resumeTotal;
      s.etag = options.etag;
      s.known = true;
      written.assign(plan.segments().size(), 0);
      for (size_t i = 0; i < plan.segments().size(); ++i)
        written[i] = plan.segments()[i].next;
      fails.assign(plan.segments().size(), 0);
      waitUntil.assign(plan.segments().size(), Clock::time_point());
      bool ready = false;
      try {
        ready = prepare(s.total);
      } catch (...) {
      }
      if (!ready) {
        result.error = "prepare";
        curl_multi_cleanup(multi);
        return result;
      }
      s.prepared = true;
      started = true;
      while (active() < limit && assign()) {
      }
    } else if (options.length > 0) {
      // A known stretch of the file: parts planned now, every request closed.
      s.total = options.from + options.length;
      s.known = true;
      bool ready = false;
      try {
        ready = prepare(s.total);
      } catch (...) {
      }
      if (!ready) {
        result.error = "prepare";
        curl_multi_cleanup(multi);
        return result;
      }
      s.prepared = true;
      started = true;
      plan = SegmentPlan(options.from, s.total, limit);
      written.assign(plan.segments().size(), 0);
      for (size_t i = 0; i < plan.segments().size(); ++i)
        written[i] = plan.segments()[i].start;
      fails.assign(plan.segments().size(), 0);
      waitUntil.assign(plan.segments().size(), Clock::time_point());
      while (active() < limit && assign()) {
      }
    } else {
      // The first request tells the size and validator, and keeps reading.
      plan = SegmentPlan(std::vector<Segment>{{0, UINT64_MAX, 0}});
      written.assign(1, 0);
      fails.assign(1, 0);
      waitUntil.assign(1, Clock::time_point());
      open(0, 0, UINT64_MAX, true);
    }

    std::string failure;
    long lastStatus = 0;
    bool fileChanged = false, fallBack = false;
    auto lastProgress = Clock::now(), lastCheckpoint = Clock::now(), lastTurn = Clock::now();
    uint64_t checkpointSeen = checkpointAsked.load();
    bool offlineSaid = false;
    // Where every part stands, by the bytes actually written, to the caller.
    const auto checkpointNow = [&] {
      if (!options.checkpoint || !s.known || !s.ranged || !s.prepared) return;
      std::lock_guard<std::mutex> held(planLock);
      auto saved = plan.segments();
      for (size_t i = 0; i < saved.size() && i < written.size(); ++i)
        saved[i].next = std::clamp(written[i], saved[i].start, saved[i].end);
      options.checkpoint(s.etag, s.total, SegmentPlan(saved).save());
    };
    while (true) {
      if (cancelled()) {
        result.cancelled = true;
        break;
      }
      if (paused()) {
        // Every connection goes; each part keeps what it got, and where they
        // stand is handed over before the wait.
        for (auto &c : conns)
          if (c->curl) {
            hand(s, *c);
            close(*c);
          }
        if (writer.drain()) checkpointNow();
        while (paused() && !cancelled())
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s.lastData = lastTurn = Clock::now(); // the pause is no silence
        continue;
      }
      // Back from sleep (or the background): the parts get their full window
      // again rather than failing on the first try after the gap.
      if (const auto now = Clock::now(); now - lastTurn > SleepGap) {
        log("download went on after " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - lastTurn).count()) +
            " s away");
        s.lastData = now;
      }
      lastTurn = Clock::now();
      // No network on the console: nothing is asked for and nothing counts
      // against the parts; they go on from where they stood once it is back.
      const bool offline = linkDown();
      if (offline) {
        s.lastData = Clock::now();
        if (!offlineSaid) log("download waits for the network");
        offlineSaid = true;
      } else if (offlineSaid) {
        log("network back; download goes on");
        offlineSaid = false;
      }
      if (const int tuned = tunedConnections(); tuned > 0) {
        const int target = std::min(tuned, std::max(1, options.connections));
        if (target != wanted) {
          wanted = target;
          // Down at once; up at once too unless a 429 is still being eased off.
          if (limit > wanted || Clock::now() - limitedAt >= LimitCalm) limit = wanted;
        }
      }
      if (const auto asked = checkpointAsked.load(); asked != checkpointSeen) {
        checkpointSeen = asked;
        checkpointNow();
        ++checkpointGiven;
      }
      // The size arrived on the first reply: plan the parts and open the rest.
      if (s.known && !started) {
        started = true;
        bool ready = false;
        try {
          ready = prepare(s.total);
        } catch (...) {
        }
        if (!ready) {
          failure = "prepare";
          break;
        }
        s.prepared = true;
        std::lock_guard<std::mutex> held(planLock);
        auto &first = *conns.front();
        if (!s.ranged || s.total == 0) {
          plan = SegmentPlan(std::vector<Segment>{{0, s.total ? s.total : UINT64_MAX, first.pos}});
          first.stop = s.total ? s.total : UINT64_MAX;
          limit = wanted = 1;
        } else {
          plan = SegmentPlan(0, s.total, limit);
          plan.segments()[0].next = first.pos;
          first.stop = plan.segments()[0].end;
        }
        written.assign(plan.segments().size(), 0);
        for (size_t i = 0; i < plan.segments().size(); ++i)
          written[i] = plan.segments()[i].start;
        fails.assign(plan.segments().size(), 0);
        waitUntil.assign(plan.segments().size(), Clock::time_point());
      }
      if (started && !conns.empty() && conns.front()->curl && conns.front()->pending.size() >= s.batch &&
          !hand(s, *conns.front())) {
        failure = "write";
        break;
      }
      if (started && !offline) {
        // Parts whose wait is over, then more parts while there is room.
        const auto now = Clock::now();
        if (limit < wanted && now - limitedAt >= LimitCalm) {
          ++limit;
          limitedAt = now;
          log("download limit eased: " + std::to_string(limit) + " connections");
        }
        for (size_t i = 0; i < plan.segments().size() && active() < limit; ++i) {
          const auto busy = busyParts();
          if (!busy[i] && plan.segments()[i].left() > 0 && waitUntil[i] <= now)
            open(int(i), plan.segments()[i].next, plan.segments()[i].end, false);
        }
        while (active() < limit && assign()) {
        }
      }
      int running = 0;
      curl_multi_perform(multi, &running);
      int left = 0;
      while (CURLMsg *msg = curl_multi_info_read(multi, &left)) {
        if (msg->msg != CURLMSG_DONE)
          continue;
        Conn *c = nullptr;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &c);
        if (!c)
          continue;
        if (c->status == 0)
          curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &c->status);
        lastStatus = c->status;
        const bool reachedEnd = c->pos >= c->stop || (!s.ranged && msg->data.result == CURLE_OK);
        if (!hand(s, *c)) {
          failure = "write";
        }
        const int seg = c->seg;
        close(*c);
        if (!failure.empty())
          break;
        if (c->changed) {
          fileChanged = true;
          break;
        }
        if (reachedEnd && (msg->data.result == CURLE_OK || c->cut)) {
          if (!s.ranged) { // the whole file came on the first reply
            std::lock_guard<std::mutex> held(planLock);
            plan.segments()[0].end = plan.segments()[0].next = c->pos;
            s.total = c->pos;
          }
          continue;
        }
        if (paused() && !cancelled())
          continue; // read again after the pause
        const long code = c->status;
        // A 401 on the direct address a fresh token answers: asked again at
        // once, from the same byte, without counting.
        if (!cancelled() && afterDirectFailure(c->url, code, c->token) == DirectStep::Again) {
          if (!started) { // the first reply never came: the first request again
            conns.clear();
            contexts.clear();
            open(0, 0, UINT64_MAX, true);
            break; // the list just read is gone
          }
          waitUntil[size_t(seg)] = Clock::time_point();
          continue;
        }
        if (code == 503 || code == 524) {
          tuneTrouble();
          long wait = 5;
          if (!c->retryAfter.empty())
            wait = std::clamp(std::atol(c->retryAfter.c_str()), 1L, 60L);
          waitUntil[size_t(seg)] = Clock::now() + std::chrono::seconds(wait);
          continue; // not counted
        }
        if (code == 429) { // Cloudflare's limit (1015 included): half the connections
          tuneTrouble();
          limit = std::max(1, limit / 2);
          limitedAt = Clock::now();
          log("download rate limited (429): " + std::to_string(limit) + " connections");
          long wait = 5;
          if (!c->retryAfter.empty())
            wait = std::clamp(std::atol(c->retryAfter.c_str()), 1L, 60L);
          waitUntil[size_t(seg)] = Clock::now() + std::chrono::seconds(wait);
          continue;
        }
        // The download token ran out: a fresh one, and the part asks again at
        // once from the byte it stood at, not as a failure.
        if (code == 401 && renewals < TokenRetries && renewDownloadToken(c->token)) {
          ++renewals;
          freshenBearer(sendHeaders);
          waitUntil[size_t(seg)] = Clock::time_point();
          continue;
        }
        const bool fatal = code == 401 || code == 403 || code == 404 || code == 416;
        // No internet on the console: the outage does not count against the
        // window; the parts are asked again once it is back.
        if (linkDown())
          s.lastData = Clock::now();
        if (!linkDown()) tuneTrouble(); // a drop, a timeout, an error: one fewer may do better
        if (!fatal && !c->changed && (code >= 500 || !c->bad) && Clock::now() - s.lastData < ResumeWindow) {
          // The longer nothing has come, the longer between asks: 2 s at
          // first, up to 15 s, so a server that is down is not hammered.
          const auto quiet = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - s.lastData);
          waitUntil[size_t(seg)] = Clock::now() + std::clamp(quiet / 4, std::chrono::seconds(RetryGap), std::chrono::seconds(15));
          continue;
        }
        if (fatal || c->bad || ++fails[size_t(seg)] >= MaxFails || !s.known) {
          if (!s.known || code == 401 || code == 403 || code == 404 || !options.fallBack) {
            failure = "http " + std::to_string(code);
            break;
          }
          fallBack = true;
          break;
        }
        waitUntil[size_t(seg)] = Clock::now() + std::chrono::seconds(1);
      }
      if (!failure.empty() || fileChanged || fallBack)
        break;
      if (writer.failed()) {
        failure = "write";
        break;
      }
      const auto now = Clock::now();
      if (options.progress && now - lastProgress >= ProgressEvery) {
        lastProgress = now;
        std::lock_guard<std::mutex> held(planLock);
        options.progress(plan.received(), s.total);
      }
      if (now - lastCheckpoint >= CheckpointEvery) {
        lastCheckpoint = now;
        checkpointNow();
      }
      if (started && s.known) {
        std::lock_guard<std::mutex> held(planLock);
        if (plan.done() && active() == 0)
          break;
      }
      curl_multi_poll(multi, nullptr, 0, 100, nullptr);
    }
    for (auto &c : conns)
      close(*c);
    curl_multi_cleanup(multi);
    const bool drained = writer.drain();
    writer.finish();
    // Where every part stands as it stops (cancelled, failed or done), so a
    // later try resumes from the last byte actually written.
    if (options.checkpoint && s.known && s.ranged && s.prepared) {
      std::lock_guard<std::mutex> held(planLock);
      auto saved = plan.segments();
      for (size_t i = 0; i < saved.size() && i < written.size(); ++i)
        saved[i].next = std::clamp(written[i], saved[i].start, saved[i].end);
      options.checkpoint(s.etag, s.total, SegmentPlan(saved).save());
    }
    result.etag = s.etag;
    result.total = s.total;
    result.status = lastStatus;
    if (result.cancelled)
      return result;
    if (!drained) {
      result.error = "write";
      return result;
    }
    if (fileChanged && !restarted) {
      restarted = true;
      log("download changed on the server; starting over");
      continue;
    }
    if (fallBack) {
      // The rest on one connection, part by part, with its own retries.
      log("download falls back to one connection");
      SegmentedOptions single = options;
      single.connections = 1;
      single.fallBack = false;
      for (auto &part : plan.segments()) {
        for (int attempt = 0; part.left() > 0 && attempt < MaxFails; ++attempt) {
          if (cancelled()) {
            result.cancelled = true;
            return result;
          }
          if (attempt)
            std::this_thread::sleep_for(std::chrono::seconds(2));
          // A plan of just this part, resumed where it stands.
          SegmentedOptions one = single;
          one.resume = {part};
          one.resumeTotal = s.total;
          one.etag = s.etag;
          one.checkpoint = nullptr;
          const auto r = segmentedFetch(
              url, [](uint64_t) { return true; }, write, one);
          if (r.cancelled) {
            result.cancelled = true;
            return result;
          }
          if (r.ok)
            part.next = part.end;
        }
        if (part.left() > 0) {
          result.error = "http " + std::to_string(lastStatus);
          return result;
        }
      }
    }
    if (!failure.empty()) {
      result.error = failure;
      return result;
    }
    std::lock_guard<std::mutex> held(planLock);
    result.ok = s.known && plan.done();
    if (!result.ok)
      result.error = "incomplete";
    return result;
  }
}
} // namespace foil::net
