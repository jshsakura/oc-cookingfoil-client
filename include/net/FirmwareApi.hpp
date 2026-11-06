#pragma once
#include "core/Firmware.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
// GET /api/firmware. With `etag` from the last reply a 304 sets `unchanged`
// and leaves `list` alone; `etag` takes the new one.
bool firmwareList(const Server &server, const State &state, std::string &etag, firmware::List &list, bool &unchanged,
                  Error &error, const std::atomic<bool> *cancel);
// The version's zip, unpacked into sd/<staging> as it downloads, then moved
// to sd/firmware/<version> whole: a cut or a bad zip never leaves a folder
// Daybreak could take for a complete firmware. A cut goes on from the last
// whole file; while the server still fetches the zip (503) it waits, as
// long as `waitSeconds`.
bool firmwareDownload(const Server &server, const State &state, const firmware::Version &version, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel,
                      int waitSeconds = 600);
} // namespace foil::net
