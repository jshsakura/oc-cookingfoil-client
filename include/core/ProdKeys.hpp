#pragma once
// prod.keys sent to a server from an admin console (feature keys-upload):
// what the server holds, the one-time key it offers, and the sealed body.
#include "core/Types.hpp"
#include <cstdint>
#include <string>
namespace foil::prodkeys {
// GET /api/admin/keys. The keys themselves never come back.
struct Status {
  bool present = false;
  std::string source; // "upload", "file" or "" (none)
  int64_t uploadedAt = 0;
  int keyCount = 0, titleKekCount = 0;
  bool hasHeaderKey = false;
};
Status parseStatus(const std::string &json);
// POST /api/admin/keys/seal: the server's P-256 point for one upload.
struct Offer {
  std::string publicKey, nonce; // base64url, and the nonce as text
  int64_t expiresAt = 0;
};
Offer parseOffer(const std::string &json);
// The PUT body {nonce, epk, iv, ct}: a fresh P-256 key, ECDH with the
// offer's point, HKDF-SHA256 ("cookingfoil-prod-keys"), AES-256-GCM with
// the nonce as additional data. An error when the offer's point is bad.
Parsed<std::string> sealBody(const Offer &offer, const std::string &keysText);
// A prod.keys file worth sending: key = hex lines, header_key among them.
bool looksLikeKeys(const std::string &text);
} // namespace foil::prodkeys
