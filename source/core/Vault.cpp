#include "core/Vault.hpp"
#include "core/Sha256.hpp"
namespace foil::vault {
namespace {
constexpr const char *Prefix = "enc:v1:";
uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
void quarter(uint32_t *s, int a, int b, int c, int d) {
  s[a] += s[b]; s[d] = rotl(s[d] ^ s[a], 16);
  s[c] += s[d]; s[b] = rotl(s[b] ^ s[c], 12);
  s[a] += s[b]; s[d] = rotl(s[d] ^ s[a], 8);
  s[c] += s[d]; s[b] = rotl(s[b] ^ s[c], 7);
}
uint32_t le32(const std::string &bytes, size_t at) {
  return uint32_t(uint8_t(bytes[at])) | uint32_t(uint8_t(bytes[at + 1])) << 8 | uint32_t(uint8_t(bytes[at + 2])) << 16 |
         uint32_t(uint8_t(bytes[at + 3])) << 24;
}
std::string hex(const std::string &bytes) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}
// The encryption key and the tag key, each its own.
std::string subkey(const std::string &key, const char *purpose) { return hexBytes(hmacSha256(key, purpose)); }
std::string tagOf(const std::string &key, const std::string &body) { return hmacSha256(subkey(key, "cookingfoil vault tag v1"), body); }
// Compared in time that does not depend on where they differ.
bool same(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= uint8_t(a[i] ^ b[i]);
  return diff == 0;
}
} // namespace
std::string chacha20(const std::string &key, const std::string &nonce, uint32_t counter, const std::string &data) {
  if (key.size() != 32 || nonce.size() != 12) return "";
  uint32_t input[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; ++i) input[4 + i] = le32(key, size_t(i) * 4);
  input[12] = counter;
  for (int i = 0; i < 3; ++i) input[13 + i] = le32(nonce, size_t(i) * 4);
  std::string out = data;
  for (size_t at = 0; at < out.size(); at += 64, ++input[12]) {
    uint32_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = input[i];
    for (int round = 0; round < 10; ++round) {
      quarter(s, 0, 4, 8, 12); quarter(s, 1, 5, 9, 13); quarter(s, 2, 6, 10, 14); quarter(s, 3, 7, 11, 15);
      quarter(s, 0, 5, 10, 15); quarter(s, 1, 6, 11, 12); quarter(s, 2, 7, 8, 13); quarter(s, 3, 4, 9, 14);
    }
    for (size_t i = 0; i < 64 && at + i < out.size(); ++i) {
      const uint32_t word = s[i / 4] + input[i / 4];
      out[at + i] = char(uint8_t(out[at + i]) ^ uint8_t(word >> (8 * (i % 4))));
    }
  }
  return out;
}
std::string seal(const std::string &key, const std::string &plain, const std::string &nonce) {
  if (key.size() != 32 || nonce.size() != 12) return "";
  const auto body = Prefix + hex(nonce) + ":" + hex(chacha20(subkey(key, "cookingfoil vault cipher v1"), nonce, 1, plain));
  return body + ":" + tagOf(key, body);
}
std::optional<std::string> open(const std::string &key, const std::string &text) {
  if (key.size() != 32 || !sealed(text)) return std::nullopt;
  const auto tagAt = text.rfind(':');
  const auto cipherAt = text.find(':', 7);
  if (tagAt == std::string::npos || cipherAt == std::string::npos || cipherAt >= tagAt) return std::nullopt;
  const auto body = text.substr(0, tagAt);
  if (!same(text.substr(tagAt + 1), tagOf(key, body))) return std::nullopt;
  const auto nonce = hexBytes(text.substr(7, cipherAt - 7));
  const auto cipherHex = text.substr(cipherAt + 1, tagAt - cipherAt - 1);
  const auto cipher = hexBytes(cipherHex);
  if (nonce.size() != 12 || cipher.size() * 2 != cipherHex.size()) return std::nullopt;
  return chacha20(subkey(key, "cookingfoil vault cipher v1"), nonce, 1, cipher);
}
bool sealed(const std::string &text) { return text.rfind(Prefix, 0) == 0; }
} // namespace foil::vault
