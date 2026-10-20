#pragma once
// 잘 돼요 / 안 돼요: this console's vote on an item. `kind` is homebrew,
// retro, patch or save.
#include "core/State.hpp"
#include "core/Votes.hpp"
#include <atomic>
namespace foil::net {
// Sends `body` (votes::body); the counts after it.
Parsed<votes::Votes> castVote(const Server &server, const State &state, const std::string &kind, const std::string &id,
                              const std::string &body, const std::atomic<bool> *cancel);
} // namespace foil::net
