#pragma once
// /api/client on the server: the newest CookingFoil build, and getting it
// in place of the running .nro.
#include "core/ClientUpdate.hpp"
#include "core/Log.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
struct ClientUpdateReply {
  Error error;
  bool missing = false; // 404: the server has no build to offer
};
std::optional<update::Release> latestClient(const Server &server, const State &state, ClientUpdateReply &reply,
                                            const std::atomic<bool> *cancel);
// Downloads next to `target` (the running .nro), checks size and SHA-256,
// then swaps it in; the old file comes back if the swap fails part way.
bool installClient(const Server &server, const State &state, const update::Release &release,
                   const std::string &target, std::atomic<uint64_t> &received, Error &error,
                   const std::atomic<bool> *cancel);
// What this console's report must not carry: the server's host, the
// account, and every key, secret and note it keeps. sendBugReport sends the
// log through forReport with these.
ReportPrivate reportPrivate(const Server &server, const State &state);
// 설정 > 버그 리포트 보내기: the log's tail goes to the server
// (POST /api/client/log, data/client/logs/<device>), only when asked.
bool sendBugReport(const Server &server, const State &state, const std::string &log, Error &error,
                   const std::atomic<bool> *cancel);
} // namespace foil::net
