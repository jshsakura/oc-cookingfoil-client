#pragma once
// Downloads that continue where they stopped: a dropped connection asks
// again from the bytes already kept (a bounded number of times), and a part
// kept on the card resumes on the next try or the next launch.
#include "core/Resume.hpp"
#include "net/Api.hpp"
#include <memory>
namespace foil {
class ArchiveWriter;
}
namespace foil::net {
// Asks in all for one download, the first included; never more.
constexpr int RangedTries = 3;
// Where a ranged download's bytes go.
struct RangedSink {
  // Bytes kept so far: each request asks from here.
  std::function<uint64_t()> kept;
  // A reply was taken; its bytes start at `from` (0: everything kept so far
  // is dropped). False stops the download.
  std::function<bool(uint64_t from, const Response &reply)> begin;
  std::function<bool(const char *, size_t)> data;
};
// A GET that continues from sink.kept(), sending `validator` as If-Range.
// A drop mid-transfer asks again after a short wait. Success is a reply of
// 200 or 206 with no error.
Response rangedStream(const std::string &url, const std::vector<std::string> &headers, std::string validator,
                      const RangedSink &sink, const std::atomic<bool> *cancel);
inline bool rangedOk(const Response &response) {
  return response.error.empty() && (response.status == 200 || response.status == 206);
}

// A file fetched into `part`, resuming the part kept there when its sidecar
// says it is the same download (`wanted`).
struct PartFetch {
  Response response;
  uint64_t size = 0;  // bytes in the part file
  std::string sha256; // its hash, when asked for
  std::string failure; // could not create or write the file (with the system's reason)
  bool tooLong = false; // more bytes than wanted.size: the server's file changed
  bool keptPlan = false; // fetched in parts and stopped short: the part stays with its .plan
};
PartFetch fetchPart(const std::string &url, const std::vector<std::string> &headers, const std::string &part,
                    const PartInfo &wanted, bool hash, std::atomic<uint64_t> &received,
                    const std::atomic<bool> *cancel);
// A dropped or cancelled fetch keeps its part for the next try; anything
// else (a bad file, an HTTP error) leaves nothing worth keeping.
inline bool keepPart(const PartFetch &fetch) {
  return !fetch.response.error.empty() && fetch.failure.empty() && !fetch.tooLong;
}
// Removes a part and its sidecar.
void dropPart(const std::string &part);

// An archive (a tar or a zip) unpacked as it arrives, resumed at
// whole-entry boundaries.
struct TarFetch {
  Response response;
  std::string tarError; // the archive's own failure (a bad entry, a full card)
  bool complete = false;
};
// `open(at)` makes the writer for bytes from `at` (at 0 the caller clears
// what earlier tries wrote). `checkpoint(boundary, etag)`, when given, hears
// boundaries worth saving. Starts at `from`, a boundary saved earlier.
TarFetch rangedTar(const std::string &url, const std::vector<std::string> &headers, uint64_t from,
                   const std::string &validator, const std::function<std::unique_ptr<ArchiveWriter>(uint64_t)> &open,
                   const std::function<void(uint64_t, const std::string &)> &checkpoint,
                   std::atomic<uint64_t> &received, const std::atomic<bool> *cancel);
} // namespace foil::net
