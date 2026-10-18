#include "core/Nacp.hpp"
#include <cstring>
#include <vector>
#include <zlib.h>
namespace foil::nacp {
namespace {
// The title table: 16 entries of 0x300 (name 0x200, author 0x100) when
// plain, 32 once decompressed; the format byte sits past the display
// version, where older NACPs left it zero.
constexpr size_t NacpSize = 0x4000, Entry = 0x300, Titles = 0x3000, PlainEntries = 16, AllEntries = 32,
                 FormatAt = 0x3215, StreamAt = 2;
} // namespace
bool plainTitles(uint8_t *nacp, size_t size) {
  if (!nacp || size < NacpSize || nacp[FormatAt] != 1) return false;
  const size_t streamSize = size_t(nacp[0]) | size_t(nacp[1]) << 8;
  if (!streamSize || streamSize > Titles - StreamAt) return false;
  std::vector<uint8_t> titles(Entry * AllEntries, 0);
  z_stream z{};
  if (inflateInit2(&z, -15) != Z_OK) return false;
  z.next_in = nacp + StreamAt;
  z.avail_in = uInt(streamSize);
  z.next_out = titles.data();
  z.avail_out = uInt(titles.size());
  const int result = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  if (result != Z_STREAM_END) return false;
  bool plainNamed = false;
  for (size_t i = 0; i < PlainEntries && !plainNamed; ++i) plainNamed = titles[i * Entry] != 0;
  std::memset(nacp, 0, Titles);
  if (plainNamed) {
    std::memcpy(nacp, titles.data(), Titles);
  } else {
    for (size_t i = PlainEntries; i < AllEntries; ++i)
      if (titles[i * Entry]) {
        std::memcpy(nacp, titles.data() + i * Entry, Entry);
        break;
      }
  }
  nacp[FormatAt] = 0;
  return true;
}
} // namespace foil::nacp
