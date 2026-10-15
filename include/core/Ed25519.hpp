#pragma once
// Ed25519 signatures (RFC 8032), from TweetNaCl (source/core/Ed25519.cpp).
// Keys and signatures are raw bytes in std::string.
#include <string>
namespace foil::ed25519 {
// Whether `signature` (64 bytes) is `publicKey`'s (32 bytes) over `message`.
bool verify(const std::string &message, const std::string &signature, const std::string &publicKey);
// The signature over `message` with a 64-byte secret key (seed, then the
// public key); empty for a key of another size. For tests and tools.
std::string sign(const std::string &message, const std::string &secretKey);
} // namespace foil::ed25519
