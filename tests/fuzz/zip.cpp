// The zip directory parsers on damaged input, built with ASan and UBSan
// (make fuzz): a valid zip and a zip64 tail, cut and changed byte by byte in
// a fixed sequence. Each must fail or succeed inside its buffer, never read
// past it.
#include "core/ZipStream.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
namespace {
void put(std::string &s, size_t at, uint64_t value, size_t n) {
  for (size_t i = 0; i < n; ++i) s[at + i] = char(value >> (8 * i));
}
std::string zip64Tail() {
  std::string t(98, '\0');
  put(t, 0, 0x06064b50, 4);
  put(t, 40, 46, 8);
  put(t, 48, 0, 8);
  put(t, 56, 0x07064b50, 4);
  put(t, 64, 0, 8);
  put(t, 76, 0x06054b50, 4);
  put(t, 86, 0xffff, 2);
  put(t, 88, 0xffffffff, 4);
  put(t, 92, 0xffffffff, 4);
  return t;
}
std::string directory() {
  std::string d(46 + 5 + 20, '\0');
  put(d, 0, 0x02014b50, 4);
  put(d, 20, 0xffffffff, 4);
  put(d, 24, 0xffffffff, 4);
  put(d, 28, 5, 2);
  put(d, 30, 20, 2);
  put(d, 42, 0xffffffff, 4);
  d.replace(46, 5, "a.nro");
  put(d, 51, 1, 2);
  put(d, 53, 16, 2);
  put(d, 55, 1000, 8);
  put(d, 63, 900, 8);
  return d;
}
uint64_t state = 0x9e3779b97f4a7c15ull;
uint64_t next() {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}
void probe(const std::string &bytes) {
  uint64_t at = 0, size = 0;
  foil::zipDirectory(bytes, 0, bytes.size(), at, size);
  foil::zipDirectory(bytes, next() % 4096, next(), at, size);
  std::vector<foil::ZipEntry> entries;
  foil::zipEntries(bytes, entries);
}
} // namespace
int main() {
  const std::vector<std::string> seeds = {zip64Tail(), directory(), directory() + directory()};
  size_t runs = 0;
  for (const auto &seed : seeds) {
    for (size_t cut = 0; cut <= seed.size(); ++cut, ++runs) probe(seed.substr(0, cut));
    for (int i = 0; i < 200000; ++i, ++runs) {
      auto bytes = seed;
      const int changes = 1 + int(next() % 8);
      for (int c = 0; c < changes; ++c) {
        const size_t at = next() % bytes.size();
        switch (next() % 3) {
        case 0: bytes[at] = char(next()); break;
        case 1: bytes[at] = char(0xff); break;
        default: if (at + 8 <= bytes.size()) put(bytes, at, next() % 2 ? UINT64_MAX - next() % 128 : next(), 8);
        }
      }
      probe(bytes);
    }
  }
  std::printf("zip parsers: %zu inputs, no fault\n", runs);
  return 0;
}
