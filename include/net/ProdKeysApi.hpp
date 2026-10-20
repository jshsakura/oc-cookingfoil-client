#pragma once
// prod.keys on the shop (feature keys-upload, admin consoles only): what it
// holds, and the console's file sent to it sealed for that server alone.
#include "core/ProdKeys.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
Parsed<prodkeys::Status> keysStatus(const Server &server, const State &state, const std::atomic<bool> *cancel);
// Asks for a one-time key, seals `keysText` to it and sends it; the server's
// holding after it.
Parsed<prodkeys::Status> sendKeys(const Server &server, const State &state, const std::string &keysText,
                                  const std::atomic<bool> *cancel);
} // namespace foil::net
