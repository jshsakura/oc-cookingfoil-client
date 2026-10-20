#pragma once
// The phone sign-in's crypto: a P-256 key the console makes for one link,
// ECDH with the phone's one-time key, and AES-256-GCM to open what the phone
// sealed. mbedTLS on the console, OpenSSL's libcrypto on the host.
#include <cstddef>
#include <string>
namespace foil::seal {
constexpr size_t ScalarSize = 32, PointSize = 65, TagSize = 16;
// Raw bytes: the secret scalar, and the uncompressed point (0x04, X, Y).
struct Key {
  std::string secret, point;
};
bool generate(Key &key);
// ECDH: the shared point's X coordinate, 32 bytes; "" when `peerPoint` is
// not a point on the curve.
std::string sharedX(const std::string &secret, const std::string &peerPoint);
// AES-256-GCM: `sealed` is the ciphertext with its 16-byte tag after it.
// False when the tag does not match (wrong key, iv, aad or altered bytes).
bool open(const std::string &key, const std::string &iv, const std::string &aad, const std::string &sealed,
          std::string &plain);
// AES-256-GCM the other way: `sealed` gets the ciphertext with the tag after it.
bool close(const std::string &key, const std::string &iv, const std::string &aad, const std::string &plain,
           std::string &sealed);
// `size` bytes from the system's secure random source; "" when it fails.
std::string random(size_t size);
} // namespace foil::seal
