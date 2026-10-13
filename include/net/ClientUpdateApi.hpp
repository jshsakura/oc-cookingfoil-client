#pragma once
// /api/client on the server: the newest CookingFoil build, and getting it
// in place of the running .nro.
#include "core/ClientUpdate.hpp"
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
} // namespace foil::net
