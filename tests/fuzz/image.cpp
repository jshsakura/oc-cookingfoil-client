// The picture header reader on damaged input, under ASan and UBSan (make
// fuzz): headers of each kind, cut and changed byte by byte.
#include "core/ImageSize.hpp"
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>
int main() {
  uint64_t state = 0x2545F4914F6CDD1Dull;
  const auto next = [&] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  const auto bytes = [](std::initializer_list<int> list) {
    std::string out;
    for (const int b : list) out += char(b);
    return out;
  };
  const std::string png = bytes({0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0x10, 0, 0, 0, 0x10, 0});
  const std::string jpeg = bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
                                  0xFF, 0xC0, 0, 17, 8, 0x10, 0, 0x10, 0, 3});
  std::string webp = std::string("RIFF\0\0\0\0WEBPVP8X", 16) + std::string(14, '\xFF');
  const std::string gif = bytes({'G', 'I', 'F', '8', '9', 'a', 0, 0x10, 0, 0x10});
  size_t runs = 0;
  uint32_t w, h;
  for (const auto &seed : {png, jpeg, webp, gif}) {
    for (size_t cut = 0; cut <= seed.size(); ++cut, ++runs) foil::imageSize(seed.substr(0, cut), w, h);
    for (int i = 0; i < 200000; ++i, ++runs) {
      auto changed = seed;
      for (int c = 1 + int(next() % 6); c > 0; --c) changed[next() % changed.size()] = char(next());
      if (next() % 4 == 0) changed.resize(next() % (changed.size() + 1));
      foil::imageSize(changed, w, h);
    }
  }
  std::printf("image header: %zu inputs, no fault\n", runs);
  return 0;
}
