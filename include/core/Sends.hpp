#pragma once
// 내 기기로 보내기: the downloads someone sent this console from the server's
// website, gathered from every server, and which of them the queue can take.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace foil::sends {
// More than anyone sends in a week; a reply listing more is cut here so a
// broken server cannot fill the memory.
constexpr size_t MaxItems = 100;
// The queue takes no more than this many downloads in all.
constexpr size_t MaxQueue = 100;
// Kept free after every queued download, as the installer keeps it.
constexpr uint64_t Reserve = 256ull * 1024 * 1024;
struct Sent {
  std::string id, path, name, titleId, url, sentAt, sentBy;
  uint64_t size = 0;
};
struct Feed {
  std::string serverId;
  std::vector<Sent> items;
};
// GET /api/device/sends; an item without an id or a url is left out.
Feed parse(const std::string &json);
// One server's answer, with the address it came from.
struct Source {
  std::string server;
  Feed feed;
};
struct Ack {
  std::string server, id;
};
// A download once, with every server's copy of it to answer for.
struct Pick {
  std::string server; // the first server that sent it
  Sent item;
  std::vector<Ack> acks;
};
// Every server's items in the order they were sent; the same game (title and
// size) sent from several servers comes once, and the same server listed
// twice (one serverId) counts once.
std::vector<Pick> merge(const std::vector<Source> &sources);
// What the queue takes. need: each pick's room (max of size and installed
// size); runs: false when the console's firmware is too old for it.
struct Candidate {
  uint64_t need = 0;
  bool runs = true;
};
struct Room {
  std::optional<uint64_t> free; // unknown until the worker has read it
  uint64_t queued = 0;          // room the queue already needs
  size_t queueCount = 0;
};
struct Admission {
  std::vector<size_t> take, held; // indexes into the candidates
  bool waiting = false;           // free space not known yet: nothing decided
};
// In order: a download that does not fit is held and the ones after it still
// get their chance, so one large game does not stop the small ones.
Admission admit(const std::vector<Candidate> &candidates, const Room &room);
// POST /api/device/sends/ack.
std::string ackBody(const std::vector<std::string> &ids, bool accepted);
} // namespace foil::sends
