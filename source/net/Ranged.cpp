#include "net/Ranged.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/Patches.hpp"
#include "core/Sha256.hpp"
#include "net/Direct.hpp"
#include "net/Tuner.hpp"
#include "core/Config.hpp"
#include "net/Segmented.hpp"
#include "net/Segments.hpp"
#include "net/Socket.hpp"
#include "platform/Cores.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>
namespace foil::net {
namespace fs = std::filesystem;
namespace {
constexpr int RetryWaitMs = 1000; // times the try number: 1 s, then 2 s
constexpr int WaitStepMs = 100;
// As every download: while bytes came in this recently, a drop is not a
// failed try (a console rejoining Wi-Fi, or waking from sleep mid-transfer).
constexpr auto ResumeWindow = std::chrono::seconds(60);
// A tar's boundary is saved this often, so a power cut costs little.
constexpr uint64_t CheckpointBytes = 16ull << 20;
bool cancelled(const std::atomic<bool> *cancel) { return cancel && cancel->load(); }
// Feeds bytes [from, size) of a kept part to `sha`.
bool hashFile(const std::string &path, uint64_t size, Sha256 &sha, uint64_t from = 0) {
  std::unique_ptr<FILE, int (*)(FILE *)> in(std::fopen(path.c_str(), "rb"), std::fclose);
  if (!in || from > size || (from && std::fseek(in.get(), long(from), SEEK_SET) != 0))
    return false;
  size -= from;
  std::vector<char> buffer(1 << 20);
  while (size > 0) {
    const auto want = size_t(std::min<uint64_t>(size, buffer.size()));
    if (std::fread(buffer.data(), 1, want, in.get()) != want)
      return false;
    sha.update(buffer.data(), want);
    size -= want;
  }
  return true;
}
// In both directions without passing through zero, which a progress bar
// reading at that moment would show as a jump.
void moveCount(std::atomic<uint64_t> &received, uint64_t from, uint64_t to) {
  if (to >= from)
    received += to - from;
  else
    received -= from - to;
}
} // namespace
Response rangedStream(const std::string &url, const std::vector<std::string> &headers, std::string validator,
                      const RangedSink &sink, const std::atomic<bool> *cancel) {
  Response last;
  bool fromZero = false;
  auto sendHeaders = headers; // the download token in them is swapped when it ran out
  int renewals = 0;
  bool now = false; // asked again at once (a fresh token for the direct address)
  auto lastData = std::chrono::steady_clock::now();
  const auto take = [&](const char *data, size_t size) {
    lastData = std::chrono::steady_clock::now();
    return sink.data(data, size);
  };
  for (int tries = 0; tries < RangedTries; ++tries) {
    for (int waited = 0; tries && !now && waited < RetryWaitMs * tries && !cancelled(cancel); waited += WaitStepMs)
      std::this_thread::sleep_for(std::chrono::milliseconds(WaitStepMs));
    now = false;
    if (tries && cancelled(cancel))
      return last;
    const uint64_t from = fromZero ? 0 : sink.kept();
    // The direct address when the server named one; its keys with it.
    const auto requestUrl = routeDownload(url);
    auto ask = headersFor(requestUrl, sendHeaders);
    const auto sent = bearerOf(ask);
    for (auto &header : rangeHeaders(from, validator))
      ask.push_back(std::move(header));
    auto verdict = RangeReply::Fail;
    bool started = false, refused = false;
    const auto start = [&](const Response &reply) {
      started = true;
      verdict = judgeRange(from, reply.status, reply.contentRange);
      if (verdict == RangeReply::Fail)
        return Start::Drop;
      if (verdict == RangeReply::Reject)
        return Start::Stop;
      if (!sink.begin(verdict == RangeReply::Restart ? 0 : from, reply)) {
        refused = true;
        return Start::Stop;
      }
      if (!reply.etag.empty())
        validator = reply.etag;
      return Start::Take;
    };
    last = stream(requestUrl, ask, start, take, cancel);
    if (!started && last.error.empty())
      start(last); // a reply without a body
    if (refused)
      return last;
    // A 401 on the direct address a fresh token answers: at once.
    if ((!last.error.empty() || verdict == RangeReply::Fail) && !cancelled(cancel) &&
        afterDirectFailure(requestUrl, last.status, sent) == DirectStep::Again) {
      --tries;
      now = true;
      continue;
    }
    // The download token ran out: a fresh one, and on from the same place.
    if (last.status == 401 && renewals < 3 && !cancelled(cancel) && renewDownloadToken(url, bearerOf(sendHeaders))) {
      ++renewals;
      freshenBearer(url, sendHeaders);
      --tries;
      continue;
    }
    // The server restarting (or no internet): not a try. Wait as long as it
    // asks and go on from the same place.
    if (linkDown(url) && !cancelled(cancel)) {
      const int waitMs = std::max(2, last.retryAfter) * 1000;
      for (int waited = 0; waited < waitMs && !cancelled(cancel); waited += WaitStepMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(WaitStepMs));
      --tries;
      continue;
    }
    if (verdict == RangeReply::Reject) {
      // The server cannot continue there: once more from the start.
      logLine("ranged: " + std::to_string(last.status) + " at " + std::to_string(from) + ", starting over");
      fromZero = true;
      validator.clear();
      last.error = tr("Could not start the connection.");
      continue;
    }
    fromZero = false;
    if (last.error.empty() || cancelled(cancel))
      return last;
    // Within the window: not a try; again after a wait that grows the longer
    // nothing has come (2 s to 15 s), from where it stopped. A drop that came
    // with bytes (the console slept mid-transfer) starts the window again.
    if (sink.kept() > from)
      lastData = std::chrono::steady_clock::now();
    if (const auto quiet = std::chrono::steady_clock::now() - lastData; quiet < ResumeWindow) {
      const auto wait = std::clamp<long>(long(std::chrono::duration_cast<std::chrono::seconds>(quiet).count() / 4), 2, 15);
      for (long waited = 0; waited < wait * 1000 && !cancelled(cancel); waited += WaitStepMs)
        std::this_thread::sleep_for(std::chrono::milliseconds(WaitStepMs));
      --tries;
      now = true;
      continue;
    }
    logLine("ranged: dropped at " + std::to_string(sink.kept()) + ", try " + std::to_string(tries + 2) + " of " +
            std::to_string(RangedTries));
  }
  return last;
}

// A file this big or more, fetched from its start, comes over several
// connections at once as a game does: one connection is slow on a link with a
// long round trip (a phone's hotspot). Its parts land out of order, so a
// download that stops is not resumed from the card; it starts over.
constexpr uint64_t ParallelFrom = 4ull << 20;
namespace {
// The whole file over several connections into `part`. False when it did
// not finish; the part is gone then, and the bytes it counted given back.
// `sha`, when given, takes the bytes that land in order from the start as
// they come (a file under ParallelFloor comes whole on one connection);
// `hashed` says how far it got.
bool fetchParallel(const std::string &url, const std::vector<std::string> &headers, const std::string &part,
                   const PartInfo &wanted, std::atomic<uint64_t> &received, const std::atomic<bool> *cancel,
                   PartFetch &result, std::string &etag, Sha256 *sha, uint64_t &hashed) {
  const auto name = fs::path(part).filename().string();
  std::unique_ptr<FILE, int (*)(FILE *)> out(std::fopen(part.c_str(), "wb"), std::fclose);
  if (!out)
    return false;
  // The whole size up front: the parts land out of order, and a write past
  // the end must not depend on how the card's file system fills the gap.
  if (wanted.size && (std::fseek(out.get(), long(wanted.size - 1), SEEK_SET) != 0 || std::fputc(0, out.get()) == EOF ||
                      std::fflush(out.get()) != 0)) {
    out.reset();
    dropPart(part);
    return false;
  }
  uint64_t counted = 0;
  bool tooLong = false, writeFailed = false, prepared = false;
  SegmentedOptions options;
  options.connections = connectionsFor(url, 0);
  options.headers = headers;
  options.batchBytes = 1u << 20;
  // Found as it goes under the server's ceiling, for this file only.
  struct Tuning {
    int id;
    ~Tuning() { stopTuning(id); }
  } tuning{startTuning(options.connections, originOf(routeDownload(url)))};
  options.bufferBytes = 16ull << 20;
  options.cancelled = [cancel] { return cancel && cancel->load(); };
  options.log = [](const std::string &line) { logLine(line); };
  const auto got = segmentedFetch(
      url,
      [&](uint64_t total) {
        if (total != wanted.size) {
          tooLong = total > wanted.size;
          return false; // not the file asked for; the one-connection path says why
        }
        // Asked again: the server's file changed and comes from the start.
        if (sha && std::exchange(prepared, true)) {
          *sha = Sha256{};
          hashed = 0;
        }
        return true;
      },
      [&](uint64_t offset, const char *data, size_t size) {
        if (offset + size > wanted.size) {
          tooLong = true;
          return false;
        }
        if (std::fseek(out.get(), long(offset), SEEK_SET) != 0 || std::fwrite(data, 1, size, out.get()) != size) {
          writeFailed = true;
          return false;
        }
        if (sha && offset == hashed) {
          sha->update(data, size);
          hashed += size;
        }
        counted += size;
        received += size;
        return true;
      },
      options);
  const bool closed = std::fclose(out.release()) == 0;
  if (got.ok && closed && !tooLong && !writeFailed) {
    etag = got.etag;
    result.response.status = 200;
    result.response.etag = got.etag;
    return true;
  }
  received -= std::min<uint64_t>(counted, received.load());
  dropPart(part);
  if (writeFailed || !closed)
    result.failure = trf("Could not write the file: {0}", {name}) + " (" + std::strerror(errno) + ")";
  result.tooLong = tooLong;
  if (got.cancelled)
    result.response.error = tr("Cancelled");
  logLine("parallel " + name + " did not finish: " + (got.cancelled ? std::string("cancelled") : got.error));
  return false;
}
} // namespace
void dropPart(const std::string &part) {
  std::error_code ec;
  fs::remove(part, ec);
  fs::remove(partInfoPath(part), ec);
}
PartFetch fetchPart(const std::string &url, const std::vector<std::string> &headers, const std::string &part,
                    const PartInfo &wanted, bool hash, std::atomic<uint64_t> &received,
                    const std::atomic<bool> *cancel) {
  PartFetch result;
  const auto name = fs::path(part).filename().string();
  const auto infoPath = partInfoPath(part);
  const auto kept = parsePartInfo(readFile(infoPath));
  std::error_code ec;
  const auto onCard = fs::file_size(part, ec);
  uint64_t written = partResumeAt(kept, wanted, ec ? 0 : uint64_t(onCard));
  Sha256 sha;
  if (written && hash && !hashFile(part, written, sha)) {
    written = 0;
    sha = Sha256{};
  }
  if (!written)
    dropPart(part);
  else
    logLine("resume " + name + " at " + std::to_string(written));
  PartInfo info = wanted;
  info.etag = written ? kept.etag : "";
  received += written;
  const auto finish = [&] {
    result.size = written;
    if (hash)
      result.sha256 = sha.hex();
    if (result.response.etag.empty())
      result.response.etag = info.etag;
    return result;
  };
  if (wanted.size && written == wanted.size) {
    result.response.status = 206; // all of it was kept: nothing left to ask
    return finish();
  }
  if (!written && wanted.size >= ParallelFrom && !platform::needsBigFile(wanted.size) &&
      connectionsFor(url, 0) > 1) {
    std::string etag;
    uint64_t hashed = 0;
    if (fetchParallel(url, headers, part, wanted, received, cancel, result, etag, hash ? &sha : nullptr, hashed)) {
      written = wanted.size;
      // Only what did not land in order is read back.
      if (hash && hashed < written && !hashFile(part, written, sha, hashed))
        result.failure = trf("Could not write the file: {0}", {name});
      info.etag = etag;
      std::string ignored;
      atomicWrite(infoPath, serializePartInfo(info), ignored);
      return finish();
    }
    // Stopped on purpose, or the card refused it: nothing more to try.
    if (!result.response.error.empty() || !result.failure.empty() || result.tooLong)
      return finish();
    result = PartFetch{}; // anything else: once more on one connection, as before
    sha = Sha256{};
  }
  std::unique_ptr<FILE, int (*)(FILE *)> out(nullptr, std::fclose);
  const auto open = [&](uint64_t at, uint64_t whole) {
    out.reset();
    std::string reason;
    if (at == 0 && platform::needsBigFile(whole) && !platform::createBigFile(part, reason)) {
      result.failure = trf("Could not create a file over 4 GB on the SD card: {0}", {name}) + " (" + reason + ")";
      return false;
    }
    out.reset(std::fopen(part.c_str(), at ? "ab" : "wb"));
    if (!out) {
      result.failure = trf("Could not create file: {0}", {name}) + " (" + std::strerror(errno) + ")";
      return false;
    }
    writeBuffer(out.get());
    return true;
  };
  RangedSink sink;
  sink.kept = [&] { return written; };
  sink.begin = [&](uint64_t at, const Response &reply) {
    if (at == 0 && written) {
      moveCount(received, written, 0);
      written = 0;
      sha = Sha256{};
      out.reset();
    }
    if (!out) {
      const auto whole = wanted.size ? wanted.size : declaredSize(reply.status, reply.contentRange, reply.contentLength);
      if (!open(at, whole))
        return false;
    }
    if (!reply.etag.empty())
      info.etag = reply.etag;
    std::string ignored;
    atomicWrite(infoPath, serializePartInfo(info), ignored);
    return true;
  };
  sink.data = [&](const char *data, size_t size) {
    if (wanted.size && written + size > wanted.size) {
      result.tooLong = true;
      return false;
    }
    if (std::fwrite(data, 1, size, out.get()) != size) {
      result.failure = trf("Could not write the file: {0}", {name}) + " (" + std::strerror(errno) + ")";
      return false;
    }
    if (hash)
      sha.update(data, size);
    written += size;
    received += size;
    return true;
  };
  result.response = rangedStream(url, headers, info.etag, sink, cancel);
  if (out && std::fclose(out.release()) != 0 && result.failure.empty())
    result.failure = trf("Could not write the file: {0}", {name}) + " (" + std::strerror(errno) + ")";
  return finish();
}

namespace {
// What came over the network goes to the card on a thread of its own, as a
// game's parts do (Segmented's writer): the connection keeps reading while
// the card makes a file or catches up, up to `Held` bytes ahead of it.
class TarPipe {
public:
  explicit TarPipe(std::function<bool(const char *, size_t)> feed) : feed_(std::move(feed)), thread_([this] { run(); }) {}
  ~TarPipe() {
    {
      std::lock_guard<std::mutex> held(lock_);
      stop_ = true;
    }
    changed_.notify_all();
    thread_.join();
  }
  // Gathered into 1 MB pieces; false once a write failed.
  bool put(const char *data, size_t size) {
    batch_.insert(batch_.end(), data, data + size);
    return batch_.size() < Piece || send();
  }
  // Everything handed over is on the card; false if a write failed.
  bool drain() {
    if (!send()) return false;
    std::unique_lock<std::mutex> held(lock_);
    changed_.wait(held, [&] { return (queue_.empty() && !busy_) || failed_; });
    return !failed_;
  }

private:
  static constexpr size_t Piece = 1u << 20;
  static constexpr size_t Held = 16u << 20;
  bool send() {
    if (batch_.empty()) return !failed();
    std::unique_lock<std::mutex> held(lock_);
    changed_.wait(held, [&] { return held_ + batch_.size() <= Held || queue_.empty() || failed_; });
    if (failed_) return false;
    held_ += batch_.size();
    queue_.push_back(std::move(batch_));
    batch_ = {};
    changed_.notify_all();
    return true;
  }
  bool failed() {
    std::lock_guard<std::mutex> held(lock_);
    return failed_;
  }
  void run() {
    const platform::OnCore off(platform::WriterCore);
    std::unique_lock<std::mutex> held(lock_);
    for (;;) {
      changed_.wait(held, [&] { return !queue_.empty() || stop_; });
      if (queue_.empty()) return;
      auto piece = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
      held.unlock();
      const bool ok = !failed_ && feed_(piece.data(), piece.size());
      held.lock();
      busy_ = false;
      held_ -= piece.size();
      if (!ok) failed_ = true;
      changed_.notify_all();
    }
  }
  std::function<bool(const char *, size_t)> feed_;
  std::vector<char> batch_;
  std::mutex lock_;
  std::condition_variable changed_;
  std::deque<std::vector<char>> queue_;
  size_t held_ = 0;
  bool busy_ = false, failed_ = false, stop_ = false;
  std::thread thread_;
};
} // namespace
TarFetch rangedTar(const std::string &url, const std::vector<std::string> &headers, uint64_t from,
                   const std::string &validator, const std::function<std::unique_ptr<ArchiveWriter>(uint64_t)> &open,
                   const std::function<void(uint64_t, const std::string &)> &checkpoint,
                   std::atomic<uint64_t> &received, const std::atomic<bool> *cancel) {
  TarFetch result;
  std::unique_ptr<ArchiveWriter> writer;
  uint64_t shown = from, saved = from;
  std::string etag = validator;
  received += from;
  const auto save = [&](uint64_t at) {
    saved = at;
    if (checkpoint)
      checkpoint(at, etag);
  };
  // The writer is fed on the pipe's thread; whatever reads it (where to ask
  // from, a fresh start) waits for the pipe to empty first.
  TarPipe pipe([&](const char *data, size_t size) {
    if (!writer->feed(data, size))
      return false;
    if (writer->boundary() >= saved + CheckpointBytes)
      save(writer->boundary());
    return true;
  });
  RangedSink sink;
  sink.kept = [&] {
    pipe.drain(); // a failed write is the writer's error, read below
    return writer ? writer->boundary() : from;
  };
  sink.begin = [&](uint64_t at, const Response &reply) {
    if (!pipe.drain())
      return false;
    // A fresh writer at the boundary; the entry cut off part way goes.
    if (writer) {
      writer->abandon();
      writer.reset();
    }
    writer = open(at);
    if (!writer)
      return false;
    writer->startAt(at);
    moveCount(received, shown, at);
    shown = at;
    if (!reply.etag.empty())
      etag = reply.etag;
    save(at);
    return true;
  };
  sink.data = [&](const char *data, size_t size) {
    received += size;
    shown += size;
    return pipe.put(data, size);
  };
  result.response = rangedStream(url, headers, validator, sink, cancel);
  pipe.drain();
  if (writer && !writer->error().empty())
    result.tarError = writer->error();
  else if (rangedOk(result.response)) {
    result.complete = writer && writer->finish();
    if (!result.complete)
      result.tarError = writer ? writer->error() : tr("The patch download was interrupted. Download it again.");
  }
  if (!result.complete && writer) {
    if (result.tarError.empty())
      save(writer->boundary());
    writer->abandon();
  }
  return result;
}
} // namespace foil::net
