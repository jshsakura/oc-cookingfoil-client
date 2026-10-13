#pragma once
#include <functional>
#include <vector>
#include "install/Installer.hpp"
namespace foil::install {
void engineSink(Installer *sink);
Installer *engineSink();
void reportBytes(const std::string &content, uint64_t received, double speed);
// An NCA that can be written anywhere in its placeholder (not NCZ) comes
// down on the segmented engine: [from, from + size) of the install's file,
// `prepare` making the placeholder and `write` putting bytes at an offset
// from `from`. NotUsed: take the engine's own path (a split JBOD file).
// It picks up where an earlier try stopped when one was cut off (see
// resumable()), and while it runs `flush` makes what is written stay on
// the card before the place it reached is saved.
enum class Placed { NotUsed, Done, Failed, Cancelled };
Placed streamToPlace(const std::string &content, uint64_t from, uint64_t size, const std::function<bool()> &prepare,
                     const std::function<bool(uint64_t offset, const char *data, size_t size)> &write,
                     const std::function<void()> &flush);
// An NCA cut off mid-download keeps its placeholder and where it stands, in
// 다운로드's database, so the next try goes on from that byte. `nca` is its id
// or file name ("0123….nca"). True when such a point is kept for it.
bool resumable(const std::string &nca);
void forgetResume(const std::string &nca);
// A solid NCZ (`nca`, `size` bytes in the file): the point kept for it by this
// download (the same file at the same place), with its decoded window read
// back from the card. False when there is none to go on from.
bool nczPoint(const std::string &nca, uint64_t size, std::string &point, std::vector<uint8_t> &window);
// Keeps one: the window to the card first (the newest two stay), then the
// point, which 다운로드's database takes.
void keepNczPoint(const std::string &nca, uint64_t size, const std::string &point, const std::vector<uint8_t> &window,
                  uint64_t produced);
// The largest window a point is kept for (memory: more as an application).
uint64_t nczMaxWindow();
} // namespace foil::install
