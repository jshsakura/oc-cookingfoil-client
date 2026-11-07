#pragma once
// A zip read as it downloads (core/ZipStream.hpp): its directory first (from
// the zip's last bytes), then the whole zip in one request, each chosen entry
// written as it arrives and checked by its CRC. A cut goes on from the last
// whole file.
#include "core/Types.hpp"
#include "core/ZipStream.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
namespace foil::net {
struct ZipSource {
  std::string url;
  uint64_t size = 0;
  std::string tag;    // what makes a kept resume point the same zip (its sha256)
  std::string record; // the resume point's file
};
// From the zip's entries, the ones to write and where (under the root);
// false, with why, when the zip will not do.
using ZipPick = std::function<bool(const std::vector<ZipEntry> &inZip,
                                   std::vector<std::pair<ZipEntry, std::string>> &wanted, std::string &why)>;
// `unusable`: the zip itself will not do (no such address, not a zip, the
// pick refused it); `retryAfter`: the server is busy with the file (503),
// ask again then; `refused`: the HTTP status the server turned the zip
// down with (410, 507...), 0 when it did not.
bool fetchZip(const std::vector<std::string> &headers, const ZipSource &zip, const std::string &root,
              const ZipPick &pick, std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel,
              bool &unusable, int &retryAfter, long *refused = nullptr);
} // namespace foil::net
