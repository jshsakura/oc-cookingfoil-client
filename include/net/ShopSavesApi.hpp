#pragma once
// /api/saves on the shop: the list, and one save's folder as a tar.
#include "core/ShopSaves.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
struct ShopSavesReply {
  Error error;
  bool missing = false; // 404: this shop offers no saves
};
Parsed<std::vector<shopsaves::ShopSave>> shopSaves(const Server &server, const State &state, ShopSavesReply &reply,
                                                    const std::atomic<bool> *cancel);
// Unpacks the save into dir (relative paths only, see shopsaves::pathAllowed).
bool downloadShopSave(const Server &server, const State &state, const shopsaves::ShopSave &save,
                      const std::string &dir, std::atomic<uint64_t> &received, Error &error,
                      const std::atomic<bool> *cancel);
} // namespace foil::net
