#include "net/Ranged.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/Patches.hpp"
#include "core/Sha256.hpp"
#include "net/Socket.hpp"
#include "platform/Storage.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
namespace foil::net {
namespace fs = std::filesystem;
namespace {
constexpr int RetryWaitMs = 1000; // times the try number: 1 s, then 2 s
constexpr int WaitStepMs = 100;
// A tar's boundary is saved this often, so a power cut costs little.
constexpr uint64_t CheckpointBytes = 16ull << 20;
bool cancelled(const std::atomic<bool> *cancel) { return cancel && cancel->load(); }
// Feeds the first `size` bytes of a kept part to `sha`.
bool hashFile(const std::string &path, uint64_t size, Sha256 &sha) {
  std::unique_ptr<FILE, int (*)(FILE *)> in(std::fopen(path.c_str(), "rb"), std::fclose);
  if (!in)
    return false;
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
  for (int tries = 0; tries < RangedTries; ++tries) {
    for (int waited = 0; tries && waited < RetryWaitMs * tries && !cancelled(cancel); waited += WaitStepMs)
      std::this_thread::sleep_for(std::chrono::milliseconds(WaitStepMs));
    if (tries && cancelled(cancel))
      return last;
    const uint64_t from = fromZero ? 0 : sink.kept();
    auto ask = sendHeaders;
    for (auto &header : rangeHeaders(from, validator))
      ask.push_back(std::move(header));
    auto verdict = RangeReply::Fail;
    bool started = false, refused = false;
    const auto take = [&](const Response &reply) {
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
    last = stream(url, ask, take, sink.data, cancel);
    if (!started && last.error.empty())
      take(last); // a reply without a body
    if (refused)
      return last;
    // The download token ran out: a fresh one, and on from the same place.
    if (last.status == 401 && renewals < 3 && !cancelled(cancel) && renewDownloadToken(bearerOf(sendHeaders))) {
      ++renewals;
      freshenBearer(sendHeaders);
      --tries;
      continue;
    }
    // The server restarting (or no internet): not a try. Wait as long as it
    // asks and go on from the same place.
    if (linkDown() && !cancelled(cancel)) {
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
    logLine("ranged: dropped at " + std::to_string(sink.kept()) + ", try " + std::to_string(tries + 2) + " of " +
            std::to_string(RangedTries));
  }
  return last;
}

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
  RangedSink sink;
  sink.kept = [&] { return writer ? writer->boundary() : from; };
  sink.begin = [&](uint64_t at, const Response &reply) {
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
    if (!writer->feed(data, size))
      return false;
    if (writer->boundary() >= saved + CheckpointBytes)
      save(writer->boundary());
    return true;
  };
  result.response = rangedStream(url, headers, validator, sink, cancel);
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
