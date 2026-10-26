#pragma once
// 관리자 편집 on the server (feature admin-edit): an item's fields, and one
// field changed. `kind` is homebrew, retro or game.
#include "core/AdminEdit.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
Parsed<std::vector<admin::Field>> adminFields(const Server &server, const State &state, const std::string &kind,
                                              const std::string &id, const std::atomic<bool> *cancel);
// Sends `body` (admin::toggled or admin::stepped); the item's fields as they
// are after it.
Parsed<std::vector<admin::Field>> setAdminField(const Server &server, const State &state, const std::string &kind,
                                                const std::string &id, const std::string &body,
                                                const std::atomic<bool> *cancel);
} // namespace foil::net
