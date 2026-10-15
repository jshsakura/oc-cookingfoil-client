#pragma once
// Values kept on the card that only this console can read back: ChaCha20
// (RFC 8439) to hide them, then HMAC-SHA256 over the result to tell any
// change (encrypt-then-MAC). Two keys are drawn from the one given, one for
// each. The form stored:
//
//   enc:v1:<nonce, 24 hex>:<ciphertext, hex>:<tag, 64 hex>
//
// Keys and nonces are raw bytes in std::string.
#include <cstdint>
#include <optional>
#include <string>
namespace foil::vault {
// ChaCha20's keystream XORed into `data` (RFC 8439 section 2.4): a 32-byte
// key, a 12-byte nonce, the block counter to start at.
std::string chacha20(const std::string &key, const std::string &nonce, uint32_t counter, const std::string &data);
// `plain` sealed under `key` (32 bytes) with `nonce` (12 bytes, never used
// twice with one key); empty for a key or nonce of another size.
std::string seal(const std::string &key, const std::string &plain, const std::string &nonce);
// What seal() hid, or nothing when `text` is not one, its tag does not
// match (changed, or sealed under another key), or the key is not 32 bytes.
std::optional<std::string> open(const std::string &key, const std::string &text);
bool sealed(const std::string &text); // starts "enc:v1:"
} // namespace foil::vault
