#pragma once
#include <functional>
#include "install/Installer.hpp"
namespace foil::install {
void engineSink(Installer *sink);
Installer *engineSink();
void reportBytes(const std::string &content, uint64_t received, double speed);
// An NCA that can be written anywhere in its placeholder (not NCZ) comes
// down on the segmented engine: [from, from + size) of the install's file,
// `prepare` making the placeholder and `write` putting bytes at an offset
// from `from`. NotUsed: take the engine's own path (a split JBOD file).
enum class Placed { NotUsed, Done, Failed, Cancelled };
Placed streamToPlace(const std::string &content, uint64_t from, uint64_t size, const std::function<bool()> &prepare,
                     const std::function<bool(uint64_t offset, const char *data, size_t size)> &write);
} // namespace foil::install
