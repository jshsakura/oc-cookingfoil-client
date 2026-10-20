// prod.keys to a server: the sealed body opens on the server's side with
// the same key, and only with the nonce it was sealed for.
#include "Test.hpp"
#include "core/Achievements.hpp"
#include "core/Json.hpp"
#include "core/ProdKeys.hpp"
#include "platform/Seal.hpp"
namespace pk = foil::prodkeys;
namespace ach = foil::achievements;
int runProdKeysTests() {
  int failures = 0;
  const std::string keys = "header_key = " + std::string(64, 'A') + "\nkey_area_key_application_00 = " +
                           std::string(32, 'B') + "\n";
  CHECK(pk::looksLikeKeys(keys));
  CHECK(!pk::looksLikeKeys("not keys"));
  CHECK(!pk::looksLikeKeys("master_key_00 = " + std::string(32, 'C') + "\n"));
  // The server's side: its one-time key, and the offer it sends.
  foil::seal::Key server;
  CHECK(foil::seal::generate(server));
  std::string point64;
  {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint32_t bits = 0;
    int held = 0;
    for (const unsigned char c : server.point) {
      bits = (bits << 8) | c;
      held += 8;
      while (held >= 6) point64 += A[(bits >> (held -= 6)) & 63];
    }
    if (held) point64 += A[(bits << (6 - held)) & 63];
  }
  const auto offer = pk::parseOffer(R"({"publicKey":")" + point64 + R"(","nonce":"n-1","expiresAt":99})");
  CHECK(offer.nonce == "n-1" && offer.expiresAt == 99);
  const auto body = pk::sealBody(offer, keys);
  CHECK(!body.error);
  foil::Error ignored;
  const auto root = foil::parseJson(body.value, ignored);
  CHECK(root && foil::stringField(root.get(), "nonce") == "n-1");
  if (root) {
    const auto epk = ach::decodeBase64Url(foil::stringField(root.get(), "epk"));
    const auto iv = ach::decodeBase64Url(foil::stringField(root.get(), "iv"));
    const auto ct = ach::decodeBase64Url(foil::stringField(root.get(), "ct"));
    CHECK(epk.size() == 65 && iv.size() == 12);
    const auto key = ach::hkdf(foil::seal::sharedX(server.secret, epk), "cookingfoil-prod-keys");
    std::string plain;
    CHECK(foil::seal::open(key, iv, "n-1", ct, plain) && plain == keys);
    CHECK(!foil::seal::open(key, iv, "n-2", ct, plain)); // another nonce does not open it
  }
  // A point that is not on the curve gives an error, not a body.
  CHECK(pk::sealBody(pk::parseOffer(R"({"publicKey":"AAAA","nonce":"n"})"), keys).error);
  const auto status = pk::parseStatus(
      R"({"present":true,"source":"upload","uploadedAt":1700000000,"keyCount":40,"hasHeaderKey":true,"titleKekCount":12})");
  CHECK(status.present && status.source == "upload" && status.uploadedAt == 1700000000 && status.keyCount == 40 &&
        status.hasHeaderKey && status.titleKekCount == 12);
  CHECK(!pk::parseStatus(R"({"present":false,"source":null})").present);
  return failures;
}
