#include "Test.hpp"
#include "core/Nacp.hpp"
#include <string>
#include <vector>
#include <zlib.h>
using namespace foil::nacp;
namespace {
constexpr size_t Size = 0x4000, Entry = 0x300, Titles = 0x3000, FormatAt = 0x3215;
// 32 title entries with names at the given slots, raw-deflated into a NACP
// the way HOS 21 keeps them.
std::vector<uint8_t> compressed(const std::vector<std::pair<int, std::string>> &names) {
  std::vector<uint8_t> titles(Entry * 32, 0);
  for (const auto &[slot, name] : names) {
    std::copy(name.begin(), name.end(), titles.begin() + slot * Entry);
    const std::string author = "maker";
    std::copy(author.begin(), author.end(), titles.begin() + slot * Entry + 0x200);
  }
  std::vector<uint8_t> out(Titles - 2);
  z_stream z{};
  deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
  z.next_in = titles.data();
  z.avail_in = uInt(titles.size());
  z.next_out = out.data();
  z.avail_out = uInt(out.size());
  deflate(&z, Z_FINISH);
  out.resize(z.total_out);
  deflateEnd(&z);
  std::vector<uint8_t> nacp(Size, 0);
  nacp[0] = uint8_t(out.size());
  nacp[1] = uint8_t(out.size() >> 8);
  std::copy(out.begin(), out.end(), nacp.begin() + 2);
  nacp[FormatAt] = 1;
  return nacp;
}
std::string name(const std::vector<uint8_t> &nacp, int slot) {
  return std::string(reinterpret_cast<const char *>(nacp.data()) + slot * Entry);
}
} // namespace
int runNacpTests() {
  int failures = 0;
  {
    // Korean (slot 12) and English (slot 0) come back where libnx reads them.
    auto n = compressed({{0, "Game"}, {12, "게임"}});
    CHECK(plainTitles(n.data(), n.size()));
    CHECK(name(n, 0) == "Game");
    CHECK(name(n, 12) == "게임");
    CHECK(std::string(reinterpret_cast<const char *>(n.data()) + 12 * Entry + 0x200) == "maker");
    CHECK(n[FormatAt] == 0);
  }
  {
    // Only a newer language slot named: slot 0 shows it.
    auto n = compressed({{20, "Later"}});
    CHECK(plainTitles(n.data(), n.size()));
    CHECK(name(n, 0) == "Later");
    CHECK(n[FormatAt] == 0);
  }
  {
    // A plain NACP is left alone.
    std::vector<uint8_t> n(Size, 0);
    n[0] = 'A';
    CHECK(!plainTitles(n.data(), n.size()));
    CHECK(name(n, 0) == "A");
  }
  {
    // A stream that does not decompress, a size past the titles, or a
    // short buffer: nothing changes.
    auto bad = compressed({{0, "Game"}});
    bad[2] = 0xFF; // a block of the reserved type: deflate refuses it
    const auto before = bad;
    CHECK(!plainTitles(bad.data(), bad.size()));
    CHECK(bad == before);
    auto huge = compressed({{0, "Game"}});
    huge[0] = 0xFF;
    huge[1] = 0xFF;
    const auto hugeBefore = huge;
    CHECK(!plainTitles(huge.data(), huge.size()));
    CHECK(huge == hugeBefore);
    auto shortOne = compressed({{0, "Game"}});
    CHECK(!plainTitles(shortOne.data(), 0x3000));
  }
  return failures;
}
