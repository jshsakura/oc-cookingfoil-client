#pragma once
// /api/saves on the server: the list, and one save's folder as a tar.
#include "core/ServerSaves.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
struct ServerSavesReply {
  Error error;
  bool missing = false; // 404: this server offers no saves
};
Parsed<std::vector<serversaves::ServerSave>> serverSaves(const Server &server, const State &state, ServerSavesReply &reply,
                                                    const std::atomic<bool> *cancel);
// Unpacks the save into dir (relative paths only, see serversaves::pathAllowed).
bool downloadServerSave(const Server &server, const State &state, const serversaves::ServerSave &save,
                      const std::string &dir, std::atomic<uint64_t> &received, Error &error,
                      const std::atomic<bool> *cancel);
} // namespace foil::net
