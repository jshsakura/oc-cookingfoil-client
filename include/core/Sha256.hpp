#pragma once
// SHA-256 fed piece by piece, so a download is hashed as it arrives instead
// of being held in memory.
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <string>
#ifdef __SWITCH__
extern "C" { // the header alone is not wrapped for C++ (switch.h is)
#include <switch/crypto/sha256.h>
}
#endif
namespace foil {
class Sha256 {
public:
  void update(const char *data, size_t size);
  // Lowercase hex of everything fed so far; finishes the hash.
  std::string hex();
private:
#ifdef __SWITCH__
  // The CPU's SHA-256 instructions (libnx): the records file is hashed at
  // each read, and every download as it arrives.
  Sha256Context context_ = [] {
    Sha256Context c;
    sha256ContextCreate(&c);
    return c;
  }();
#endif
  void block(const unsigned char *p);
  uint32_t h_[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  unsigned char buffer_[64]{};
  size_t used_ = 0;
  uint64_t total_ = 0;
};
// A file's SHA-256 as 64 lowercase hex digits; "" when it cannot be read.
// Empty when it cannot be read, or when `stop` turns true part way (checked
// between 1 MB reads, so a big file does not hold up a cancel).
std::string sha256File(const std::string &path, const std::atomic<bool> *stop = nullptr);
// The bytes a lowercase or uppercase hex string spells; empty when it is not
// hex of whole bytes.
std::string hexBytes(const std::string &hex);
// HMAC-SHA256 (RFC 2104) of `message` under `key` (raw bytes), as lowercase
// hex.
std::string hmacSha256(const std::string &key, const std::string &message);
} // namespace foil
