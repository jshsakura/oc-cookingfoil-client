#include "core/Sha256.hpp"
#include <cstdio>
#include <memory>
#include <vector>
namespace foil {
namespace {
constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }
} // namespace
void Sha256::block(const unsigned char *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i)
    w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}
void Sha256::update(const char *data, size_t size) {
#ifdef __SWITCH__
  sha256ContextUpdate(&context_, data, size);
  return;
#endif
  const auto *p = reinterpret_cast<const unsigned char *>(data);
  total_ += size;
  // Whole blocks straight from the input, the rest through the buffer.
  if (!used_)
    for (; size >= 64; p += 64, size -= 64) block(p);
  while (size) {
    const size_t take = size < 64 - used_ ? size : 64 - used_;
    for (size_t i = 0; i < take; ++i) buffer_[used_ + i] = p[i];
    used_ += take;
    p += take;
    size -= take;
    if (used_ == 64) {
      block(buffer_);
      used_ = 0;
      for (; size >= 64; p += 64, size -= 64) block(p);
    }
  }
}
std::string Sha256::hex() {
  static const char *digits = "0123456789abcdef";
#ifdef __SWITCH__
  unsigned char hash[SHA256_HASH_SIZE];
  sha256ContextGetHash(&context_, hash);
  std::string hashed;
  for (const auto byte : hash) {
    hashed += digits[byte >> 4];
    hashed += digits[byte & 15];
  }
  return hashed;
#endif
  const uint64_t bits = total_ * 8;
  const char pad = char(0x80);
  update(&pad, 1);
  const char zero = 0;
  while (used_ != 56) update(&zero, 1);
  char length[8];
  for (int i = 0; i < 8; ++i) length[i] = char(bits >> ((7 - i) * 8));
  update(length, 8);
  std::string out;
  for (const auto word : h_)
    for (int shift = 28; shift >= 0; shift -= 4) out += digits[(word >> shift) & 15];
  return out;
}
std::string hexBytes(const std::string &hex) {
  if (hex.size() % 2) return "";
  const auto nibble = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  std::string out;
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
    if (high < 0 || low < 0) return "";
    out += char(high << 4 | low);
  }
  return out;
}
std::string hmacSha256(const std::string &key, const std::string &message) {
  constexpr size_t Block = 64;
  std::string k = key;
  if (k.size() > Block) {
    Sha256 shorter;
    shorter.update(k.data(), k.size());
    k = hexBytes(shorter.hex());
  }
  k.resize(Block, '\0');
  std::string inner(Block, '\0'), outer(Block, '\0');
  for (size_t i = 0; i < Block; ++i) {
    inner[i] = char(k[i] ^ 0x36);
    outer[i] = char(k[i] ^ 0x5c);
  }
  Sha256 first;
  first.update(inner.data(), inner.size());
  first.update(message.data(), message.size());
  const auto innerHash = hexBytes(first.hex());
  Sha256 second;
  second.update(outer.data(), outer.size());
  second.update(innerHash.data(), innerHash.size());
  return second.hex();
}
std::string sha256File(const std::string &path, const std::atomic<bool> *stop) {
  std::unique_ptr<FILE, int (*)(FILE *)> in(std::fopen(path.c_str(), "rb"), std::fclose);
  if (!in) return "";
  Sha256 sha;
  std::vector<char> buffer(1 << 20);
  for (size_t n; (n = std::fread(buffer.data(), 1, buffer.size(), in.get())) > 0;) {
    if (stop && *stop) return "";
    sha.update(buffer.data(), n);
  }
  return std::ferror(in.get()) ? "" : sha.hex();
}
} // namespace foil
