#pragma once
// A game's NACP as the console hands it over (0x4000 bytes).
#include <cstddef>
#include <cstdint>
namespace foil::nacp {
// Since HOS 21.0.0 a NACP may keep its titles compressed: the byte at 0x3215
// is 1, the first two bytes are the size of a raw deflate stream that
// follows, and it holds 32 title entries in place of the 16. libnx reads the
// 16 plain ones, so this puts them back: the 16 decompressed, or when those
// are all empty the first named one of the rest in slot 0, and the format
// byte set to 0. A NACP already plain, or one that does not decompress,
// stays as it was. Returns whether it changed.
bool plainTitles(uint8_t *nacp, size_t size);
} // namespace foil::nacp
