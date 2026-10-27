#pragma once
// 내 기기로 보내기: the downloads waiting for this console on one server, and
// the answer once the queue took them or would not.
#include "core/Sends.hpp"
#include "core/State.hpp"
#include <atomic>
#include <optional>
namespace foil::net {
struct SendsReply {
  std::optional<sends::Feed> feed; // none: unchanged since `etag`, or not read
  std::string etag;
  bool failed = false;
};
// GET /api/device/sends, with If-None-Match `etag`.
SendsReply deviceSends(const Server &server, const State &state, const std::string &etag,
                       const std::atomic<bool> *cancel);
// POST /api/device/sends/ack; false when the server did not take it.
bool ackSends(const Server &server, const State &state, const std::vector<std::string> &ids, bool accepted,
              const std::atomic<bool> *cancel);
} // namespace foil::net
