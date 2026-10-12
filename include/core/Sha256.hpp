#pragma once
// SHA-256 fed piece by piece, so a download is hashed as it arrives instead
// of being held in memory.
#include <cstddef>
#include <cstdint>
#include <string>
namespace foil {
class Sha256 {
public:
  void update(const char *data, size_t size);
  // Lowercase hex of everything fed so far; finishes the hash.
  std::string hex();
private:
  void block(const unsigned char *p);
  uint32_t h_[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  unsigned char buffer_[64]{};
  size_t used_ = 0;
  uint64_t total_ = 0;
};
} // namespace foil
