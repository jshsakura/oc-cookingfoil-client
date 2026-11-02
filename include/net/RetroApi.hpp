#pragma once
// /api/retro on the server: systems, game pages, bundles and the base pack.
#include "core/Retro.hpp"
#include "core/State.hpp"
#include <atomic>
#include <functional>
namespace foil::net {
struct RetroReply {
  Error error;
  bool missing = false; // 404: this server has no retro games
  int retryAfter = 0;   // 503: the server is still hashing; ask again then
  labels::Book labels;  // the reply's label dictionary, when it sends one
};
Parsed<std::vector<retro::System>> retroSystems(const Server &server, const State &state, RetroReply &reply,
                                                 const std::atomic<bool> *cancel);
// One page of a system's games, searched, filtered and ordered by `filter`.
Parsed<retro::Page> retroGames(const Server &server, const State &state, const std::string &system, int page,
                               const retro::Filter &filter, RetroReply &reply, const std::atomic<bool> *cancel);
// A system's filters and sorts as the server words them; 404 on a server without them.
Parsed<retro::FilterBook> retroFilters(const Server &server, const State &state, const std::string &system,
                                       RetroReply &reply, const std::atomic<bool> *cancel);
// One game in full with its other versions (/api/retro/games/:id).
Parsed<retro::Detail> retroDetail(const Server &server, const State &state, const std::string &id, RetroReply &reply,
                                  const std::atomic<bool> *cancel);
// Every system with its best rated games, for the shelves.
Parsed<std::vector<retro::Shelf>> retroHome(const Server &server, const State &state, RetroReply &reply,
                                             const std::atomic<bool> *cancel);
// The theme rows across systems (genres, decades, two players…), picked by
// the server once a day; 404 on a server without them.
Parsed<std::vector<retro::Theme>> retroThemes(const Server &server, const State &state, RetroReply &reply,
                                              const std::atomic<bool> *cancel);
Parsed<retro::Pack> retroPack(const Server &server, const State &state, RetroReply &reply,
                              const std::atomic<bool> *cancel);
// Unpacks a game's bundle onto the SD card (only retro::bundlePathAllowed paths).
bool retroBundle(const Server &server, const State &state, const retro::Game &game, const std::string &sd,
                 std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel);
// One base pack file to sd/path, through a .part file swapped in when complete.
// The whole base pack in one tar (pack.bundleUrl); `skip` names files left
// as they are on the card.
bool retroPackBundle(const Server &server, const State &state, const retro::Pack &pack, const std::string &sd,
                     std::function<bool(const std::string &)> skip, std::atomic<uint64_t> &received, Error &error,
                     const std::atomic<bool> *cancel);
// `status` gets the HTTP status, so a caller can wait out a 429; `refused`
// says the card would not take the file under its name (see fetchFile).
bool retroFile(const Server &server, const State &state, const retro::PackFile &file, const std::string &sd,
               std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel, long *status = nullptr,
               bool *refused = nullptr);
} // namespace foil::net
