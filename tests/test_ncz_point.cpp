#include "Test.hpp"
#include "core/NczPoint.hpp"
#include <string>
using foil::NczPoint;
namespace {
NczPoint sample() {
  NczPoint p;
  p.zstdStart = 0x4000 + 0x30;
  p.consumed = (5ull << 30) + 17; // past 4 GiB: every field is 64-bit
  p.produced = (9ull << 30) + 3;
  p.windowSize = 8u << 20;
  for (int i = 0; i < 3000; ++i) p.decoder.push_back(char(i * 7));
  for (int i = 0; i < 112; ++i) p.sha.push_back(char(255 - i));
  return p;
}
bool same(const NczPoint &a, const NczPoint &b) {
  return a.version == b.version && a.zstdStart == b.zstdStart && a.consumed == b.consumed && a.produced == b.produced &&
         a.windowSize == b.windowSize && a.decoder == b.decoder && a.sha == b.sha;
}
// The base64 body of an encoded point, and one put back together around a
// changed body (for the damaged cases).
std::string body(const std::string &text) { return text.substr(5); }
} // namespace
int runNczPointTests() {
  int failures = 0;
  // Round trip, including empty opaque parts and every base64 tail length.
  const auto p = sample();
  const auto text = p.encode();
  CHECK(text.rfind("ncz1:", 0) == 0);
  CHECK(text.find_first_of(" \r\n\t") == std::string::npos);
  NczPoint back;
  CHECK(NczPoint::decode(text, back) && same(p, back));
  for (int extra = 0; extra < 3; ++extra) {
    auto q = p;
    q.sha.resize(q.sha.size() + size_t(extra), 'x');
    NczPoint r;
    CHECK(NczPoint::decode(q.encode(), r) && same(q, r));
  }
  auto empty = p;
  empty.decoder.clear();
  empty.sha.clear();
  NczPoint r;
  CHECK(NczPoint::decode(empty.encode(), r) && same(empty, r));

  // isNcz tells it from the segmented plan strings.
  CHECK(NczPoint::isNcz(text));
  CHECK(!NczPoint::isNcz(""));
  CHECK(!NczPoint::isNcz("0:1048576,1048576:2097152"));
  CHECK(!NczPoint::isNcz("ncz2:AAAA"));

  // Rejections leave nothing half-filled that a caller would trust.
  CHECK(!NczPoint::decode("", r));
  CHECK(!NczPoint::decode("ncz1:", r));
  CHECK(!NczPoint::decode("ncz2:" + body(text), r));
  CHECK(!NczPoint::decode("NCZ1:" + body(text), r));
  CHECK(!NczPoint::decode(body(text), r));
  auto v2 = p;
  v2.version = 2;
  CHECK(!NczPoint::decode(v2.encode(), r));
  // Truncated or with bytes after the end (re-encoded so base64 stays valid).
  auto shortSha = p;
  shortSha.sha.clear();
  const auto full = p.encode(), cut = shortSha.encode();
  CHECK(!NczPoint::decode(full.substr(0, full.size() - 4), r));
  CHECK(!NczPoint::decode(full.substr(0, 40), r));
  CHECK(!NczPoint::decode(full + "AAAA", r));
  // Bad base64: a stray character, a bad length, padding in the middle.
  auto stray = full;
  stray[10] = '*';
  CHECK(!NczPoint::decode(stray, r));
  CHECK(!NczPoint::decode(full.substr(0, full.size() - 1), r));
  auto padded = full;
  padded[12] = '=';
  CHECK(!NczPoint::decode(padded, r));
  CHECK(!NczPoint::decode(full + " ", r));
  // Absurd sizes.
  auto bigSha = p;
  bigSha.sha.assign(257, 'x');
  CHECK(!NczPoint::decode(bigSha.encode(), r));
  auto bigDecoder = p;
  bigDecoder.decoder.assign(1u << 20, 'x');
  CHECK(!NczPoint::decode(bigDecoder.encode(), r));
  auto noWindow = p;
  noWindow.windowSize = 0;
  CHECK(!NczPoint::decode(noWindow.encode(), r));
  auto hugeWindow = p;
  hugeWindow.windowSize = 1ull << 40;
  CHECK(!NczPoint::decode(hugeWindow.encode(), r));
  CHECK(NczPoint::decode(cut, r) && r.sha.empty());

  // Window file names: safe on FAT, and read back to the same key and point.
  for (const std::string key : {"sd:0123abcdef", "nand:ABCD-ef_01", "x", "a:b/c\\d e%f.g-h_i"}) {
    for (const uint64_t produced : {uint64_t(0), uint64_t(123456789), uint64_t(1) << 40, ~uint64_t(0)}) {
      const auto name = foil::windowFileName(key, produced);
      CHECK(name.size() > 4 && name.compare(name.size() - 4, 4, ".win") == 0);
      CHECK(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") ==
            std::string::npos);
      std::string k;
      uint64_t n = 1;
      CHECK(foil::windowFileOf(name, k, n) && k == key && n == produced);
    }
  }
  CHECK(foil::windowFileName("sd:1", 5) != foil::windowFileName("sd_1", 5));
  std::string k;
  uint64_t n = 0;
  CHECK(!foil::windowFileOf("", k, n));
  CHECK(!foil::windowFileOf(".win", k, n));
  CHECK(!foil::windowFileOf("sd_3a01-12.part", k, n));
  CHECK(!foil::windowFileOf("sd_3a01.win", k, n));
  CHECK(!foil::windowFileOf("sd_3a01-.win", k, n));
  CHECK(!foil::windowFileOf("sd_3a01-1x2.win", k, n));
  CHECK(!foil::windowFileOf("sd_3a01-99999999999999999999.win", k, n));
  CHECK(!foil::windowFileOf("sd_3-1.win", k, n));
  CHECK(!foil::windowFileOf("sd_zz01-1.win", k, n));
  CHECK(!foil::windowFileOf("-1.win", k, n));
    {
    // A point with no placeholder on the console goes; one the console could
    // not be asked about stays; window files go with their point, and files
    // that are not windows are never touched.
    const std::vector<std::string> points{"sd:aa", "sd:bb", "nand:cc"};
    const auto there = [](const std::string &key) -> std::optional<bool> {
      if (key == "sd:aa") return true;
      if (key == "sd:bb") return false;
      return std::nullopt;
    };
    const std::vector<std::string> names{foil::windowFileName("sd:aa", 10), foil::windowFileName("sd:bb", 20),
                                         foil::windowFileName("sd:dd", 30), foil::windowFileName("nand:cc", 5),
                                         "homebrew-x.json"};
    const auto left = foil::resumeLeftovers(points, there, names);
    CHECK(left.points == std::vector<std::string>{"sd:bb"});
    CHECK((left.windows == std::vector<std::string>{foil::windowFileName("sd:bb", 20), foil::windowFileName("sd:dd", 30)}));
    // Everything in place: nothing to do.
    const auto all = [](const std::string &) -> std::optional<bool> { return true; };
    CHECK(foil::resumeLeftovers({"sd:aa"}, all, {foil::windowFileName("sd:aa", 1)}).windows.empty());
  }
  return failures;
}
